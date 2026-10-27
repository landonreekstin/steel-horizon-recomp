#!/usr/bin/env python3
"""Convert .pingu models (+ .pts mount points) to glTF 2.0.

Usage: python3 -I tools/pingu2gltf.py PINGU_DIR PNG_DIR OUTDIR

PINGU_DIR is the extracted /pingus tree, PNG_DIR the output of bog2png.py
(textures are found by basename, case-insensitively). Each model becomes
OUTDIR/<relpath>.gltf + .bin; images are referenced by relative URI, so the
PNG tree must stay where it is. Mount points from a sibling .pts become empty
child nodes named after the point (F1_..., R1_..., smoke01, ...).
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pingu import Pingu, Points  # noqa: E402

FLOAT, UINT32 = 5126, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
REPEAT, LINEAR, LINEAR_MIPMAP_LINEAR = 10497, 9729, 9987


def index_pngs(png_dir):
    found = {}
    for root, _, files in os.walk(png_dir):
        for name in files:
            if name.lower().endswith(".png"):
                found.setdefault(name[:-4].lower(), os.path.join(root, name))
    return found


def matrix_from_rows(r):
    # glTF wants a column-major 4x4; the .pts rows are a 3x3 rotation.
    return [r[0], r[3], r[6], 0, r[1], r[4], r[7], 0, r[2], r[5], r[8], 0]


def convert(model, points, pngs, out_path):
    blob = bytearray()
    views, accessors, meshes_json = [], [], []

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data),
                      "target": target})
        blob.extend(data)
        return len(views) - 1

    def add_accessor(view, ctype, count, kind, lo=None, hi=None):
        acc = {"bufferView": view, "componentType": ctype, "count": count, "type": kind}
        if lo is not None:
            acc["min"], acc["max"] = lo, hi
        accessors.append(acc)
        return len(accessors) - 1

    images, textures, materials = [], [], []
    mat_of = {}
    out_dir = os.path.dirname(out_path)
    for i, name in enumerate(model.materials):
        png = pngs.get(name.lower())
        mat = {"name": name, "pbrMetallicRoughness": {"metallicFactor": 0.0,
                                                      "roughnessFactor": 1.0}}
        if png:
            images.append({"uri": os.path.relpath(png, out_dir).replace(os.sep, "/")})
            textures.append({"source": len(images) - 1, "sampler": 0})
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(textures) - 1}
        else:
            print(f"  missing texture {name}", file=sys.stderr)
        materials.append(mat)
        mat_of[i] = len(materials) - 1
    materials.append({"name": "untextured", "pbrMetallicRoughness": {
        "baseColorFactor": [0.5, 0.5, 0.5, 1.0], "metallicFactor": 0.0}})
    untextured = len(materials) - 1

    prims = []
    for mesh in model.meshes:
        tris = mesh.triangles()
        if not tris:
            continue
        pos = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
        nrm = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
        uv = b"".join(struct.pack("<2f", *t) for t in mesh.uvs)
        idx = b"".join(struct.pack("<3I", *t) for t in tris)
        n = mesh.vertex_count
        lo = [min(p[k] for p in mesh.positions) for k in range(3)]
        hi = [max(p[k] for p in mesh.positions) for k in range(3)]
        prims.append({
            "attributes": {
                "POSITION": add_accessor(add_view(pos, ARRAY_BUFFER), FLOAT, n, "VEC3", lo, hi),
                "NORMAL": add_accessor(add_view(nrm, ARRAY_BUFFER), FLOAT, n, "VEC3"),
                "TEXCOORD_0": add_accessor(add_view(uv, ARRAY_BUFFER), FLOAT, n, "VEC2"),
            },
            "indices": add_accessor(add_view(idx, ELEMENT_ARRAY_BUFFER), UINT32,
                                    len(tris) * 3, "SCALAR"),
            "material": mat_of.get(mesh.material, untextured),
        })
    meshes_json.append({"name": os.path.basename(out_path)[:-5], "primitives": prims})

    nodes = [{"name": meshes_json[0]["name"], "mesh": 0, "children": []}]
    for name, p, rows in (points.points if points else []):
        m = matrix_from_rows(rows) + [p[0], p[1], p[2], 1]
        nodes.append({"name": name, "matrix": m})
        nodes[0]["children"].append(len(nodes) - 1)
    if not nodes[0]["children"]:
        del nodes[0]["children"]

    bin_name = os.path.basename(out_path)[:-5] + ".bin"
    gltf = {
        "asset": {"version": "2.0", "generator": "steel-horizon-recomp pingu2gltf"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": nodes, "meshes": meshes_json,
        "materials": materials, "buffers": [{"uri": bin_name, "byteLength": len(blob)}],
        "bufferViews": views, "accessors": accessors,
        "samplers": [{"magFilter": LINEAR, "minFilter": LINEAR_MIPMAP_LINEAR,
                      "wrapS": REPEAT, "wrapT": REPEAT}],
    }
    if images:
        gltf["images"], gltf["textures"] = images, textures
    with open(out_path, "w") as f:
        json.dump(gltf, f, indent=1)
    with open(os.path.join(out_dir, bin_name), "wb") as f:
        f.write(blob)


def main(pingu_dir, png_dir, out_dir):
    pngs = index_pngs(png_dir)
    ok = bad = 0
    for root, _, files in os.walk(pingu_dir):
        for name in sorted(files):
            if not name.endswith(".pingu"):
                continue
            src = os.path.join(root, name)
            out = os.path.join(out_dir, os.path.relpath(src, pingu_dir))[:-6] + ".gltf"
            os.makedirs(os.path.dirname(out), exist_ok=True)
            pts_path = src[:-6] + ".pts"
            try:
                with open(src, "rb") as f:
                    model = Pingu(f.read())
                points = None
                if os.path.exists(pts_path):
                    with open(pts_path, "rb") as f:
                        points = Points(f.read())
                convert(model, points, pngs, out)
                ok += 1
            except ValueError as e:
                print(f"{src}: {e}", file=sys.stderr)
                bad += 1
    print(f"converted {ok}, failed {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
