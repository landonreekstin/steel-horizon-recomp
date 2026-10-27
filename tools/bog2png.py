#!/usr/bin/env python3
"""Convert .bog textures to PNG (mip level 0).

Usage: python3 -I tools/bog2png.py INDIR OUTDIR    (recurses, mirrors paths)
       python3 -I tools/bog2png.py FILE.bog OUT.png
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bog import Bog, write_png  # noqa: E402


def convert(src, dst):
    with open(src, "rb") as f:
        tex = Bog(f.read())
    w, h, rgba = tex.level(0)
    write_png(dst, w, h, rgba)


def main(src, dst):
    if os.path.isfile(src):
        convert(src, dst)
        return 0
    ok = bad = 0
    for root, _, files in os.walk(src):
        for name in files:
            if not name.lower().endswith(".bog"):
                continue
            path = os.path.join(root, name)
            out = os.path.join(dst, os.path.relpath(path, src))[:-4] + ".png"
            os.makedirs(os.path.dirname(out), exist_ok=True)
            try:
                convert(path, out)
                ok += 1
            except ValueError as e:
                print(f"{path}: {e}", file=sys.stderr)
                bad += 1
    print(f"converted {ok}, failed {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
