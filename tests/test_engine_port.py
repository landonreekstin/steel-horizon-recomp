#!/usr/bin/env python3
"""Cross-check the engine's C asset readers against the Python decoders.

Usage: python3 -I tests/test_engine_port.py [ASSETS_DIR]
Needs engine/build/test_engine (cmake -S engine -B engine/build -G Ninja && ninja -C engine/build).
For every .bog and .pingu under ASSETS_DIR/files, the decoded RGBA / mesh data must
hash the same in C and Python (same FNV-1a over the same byte layout).
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from bog import Bog  # noqa: E402
from pingu import Pingu  # noqa: E402

FNV_OFFSET, FNV_PRIME, MASK = 1469598103934665603, 1099511628211, (1 << 64) - 1


def fnv(h, data):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def lround(v):
    return int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)


def pingu_hash(data):
    """Mirror engine/src/asset.c: float32 arithmetic in the same order."""
    m = Pingu(data)
    (scale,) = struct.unpack_from("<f", data, 0x34)
    s = f32(f32(scale) / 128.0)
    h = FNV_OFFSET
    verts = tris = 0
    # Re-read raw vertex bytes so the arithmetic matches C exactly.
    o = len(data) - 8 * sum(x.vertex_count for x in m.meshes)
    for x in m.meshes:
        h = fnv(h, struct.pack("<i", -1 if x.material is None else x.material))
        ts = [f32(v) for v in x.tex_scale]
        to = [f32(v) for v in x.tex_offset]
        out = bytearray()
        for _ in range(x.vertex_count):
            tu, tv, _nx, _ny, _nz, px, py, pz = struct.unpack_from("<BBbbbbbb", data, o)
            o += 8
            pos = [f32(c * s) for c in (px, py, pz)]
            uv = [f32(f32(f32(t / 128.0) * ts[k]) + to[k]) for k, t in enumerate((tu, tv))]
            q = [lround(f32(v * 10000.0)) for v in pos + uv]
            out += struct.pack("<5i", *q)
        h = fnv(h, out)
        idx = bytearray()
        base = 0
        for n in x.strips:
            for t in range(n - 2):
                a, b, c = base + t, base + t + 1, base + t + 2
                if t & 1:
                    a, b = b, a
                idx += struct.pack("<3I", a, b, c)
            base += n
        h = fnv(h, idx)
        verts += x.vertex_count
        tris += len(idx) // 12
    return verts, tris, h


def main(assets):
    exe = os.path.join(ROOT, "engine", "build", "test_engine")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "hashes.txt")
        subprocess.run([exe, assets, "--hashes", out], check=True)
        c_hashes = {}
        for line in open(out):
            rel, *rest = line.split()
            c_hashes[rel] = rest
    files = os.path.join(assets, "files")
    fails, checked = [], 0
    for rel, rest in sorted(c_hashes.items()):
        with open(os.path.join(files, rel), "rb") as f:
            data = f.read()
        if rest == ["ERROR"]:
            fails.append(f"{rel}: C reader failed")
            continue
        if rel.endswith(".bog"):
            w, h, rgba = Bog(data).level(0)
            want = [str(w), str(h), f"{fnv(FNV_OFFSET, rgba):016x}"]
        else:
            v, t, hh = pingu_hash(data)
            want = [str(v), str(t), f"{hh:016x}"]
        checked += 1
        if rest != want:
            fails.append(f"{rel}: C {rest} vs Python {want}")
    for f in fails[:20]:
        print("FAIL", f)
    print(f"{checked} files compared, {len(fails)} failures")
    return 1 if fails or checked < 1800 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets")))
