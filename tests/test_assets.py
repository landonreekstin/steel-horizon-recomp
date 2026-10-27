#!/usr/bin/env python3
"""Regression checks for the asset tools against the user's extracted game data.

Usage: python3 -I tests/test_assets.py [FILES_DIR]   (default assets/files)
Needs `tools/imp_extract.py` to have been run. Exits non-zero on any failure.
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from bog import Bog, BPP  # noqa: E402
from pingu import Pingu, Points  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def main(files):
    allfiles = [p for p in glob.glob(os.path.join(files, "**", "*"), recursive=True)
                if os.path.isfile(p)]
    check(len(allfiles) == 2797, f"expected 2797 files, got {len(allfiles)}")

    bogs = glob.glob(os.path.join(files, "**", "*.bog"), recursive=True)
    check(len(bogs) == 1574, f"expected 1574 .bog, got {len(bogs)}")
    for p in bogs:
        with open(p, "rb") as f:
            t = Bog(f.read())
        need = sum(t.row_bytes(max(1, t.width >> i)) * max(1, t.height >> i)
                   for i in range(t.mips))
        check(need == t.data_size == len(t.pixels), f"{p}: size {t.data_size} != {need}")
        w, h, rgba = t.level(0)
        check(len(rgba) == w * h * 4, f"{p}: decoded size")

    pingus = glob.glob(os.path.join(files, "**", "*.pingu"), recursive=True)
    check(len(pingus) == 258, f"expected 258 .pingu, got {len(pingus)}")
    for p in pingus:
        with open(p, "rb") as f:
            m = Pingu(f.read())          # raises on any layout mismatch
        used = {x.material for x in m.meshes} - {None}
        check(used == set(range(len(m.materials))), f"{p}: materials used {used}")
        check(sum(x.material is None for x in m.meshes) <= 1, f"{p}: >1 untextured mesh")
        for x in m.meshes:
            for k in range(3):
                lo = min(q[k] for q in x.positions)
                hi = max(q[k] for q in x.positions)
                check(m.bbox_min[k] - 1 <= lo and hi <= m.bbox_max[k] + 1,
                      f"{p}: mesh outside bbox on axis {k}")

    upright = 0
    total = 0
    for p in glob.glob(os.path.join(files, "**", "*.pts"), recursive=True):
        with open(p, "rb") as f:
            for name, pos, r in Points(f.read()).points:
                total += 1
                upright += r[4] > 0.999      # model +Y maps to +Y
    check(total == 832 and upright >= 716, f"mounts: {upright}/{total} upright")

    # Known values from PSP_AL_BB01 (checked by rendering).
    with open(os.path.join(files, "pingus/ships/allied/PSP_AL_BB01.pingu"), "rb") as f:
        bb = Pingu(f.read())
    check([x.vertex_count for x in bb.meshes] == [2581, 592, 56, 99], "BB01 vertex counts")
    check(bb.materials[0] == "PSP_AL_BB01_4", "BB01 material 0")

    for msg in failures[:30]:
        print("FAIL", msg)
    print(f"{len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets/files")))
