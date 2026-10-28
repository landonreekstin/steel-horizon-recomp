#!/usr/bin/env python3
"""Bake the HUD icons into a C header holding a coverage atlas.

Usage: python3 -I tools/bakeicons.py engine/src/icon_atlas.h [preview.png]

Each icon is drawn from polygons on a 32x32 grid (y down) and rasterized with the
font baker's anti-aliased non-zero-winding rasterizer, so the icons are our own
and need no image files. Fills wind one way, holes the other (`hole()`); shapes that
overlap with the same winding merge. The atlas goes under the font atlas in the
same texture, so icons are drawn by the UI text shader in any colour.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bakefont import raster  # noqa: E402

CELL = 32
COLS = 4


def area(p):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(p, p[1:] + p[:1])) / 2


def fill(p):
    """Positive-area winding: a filled shape."""
    return p if area(p) > 0 else p[::-1]


def hole(p):
    return p[::-1] if area(p) > 0 else p


def rect(x0, y0, x1, y1):
    return fill([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def circle(cx, cy, r, n=32):
    return fill([(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)])


def ring(cx, cy, r, w):
    return [circle(cx, cy, r), hole(circle(cx, cy, r - w))]


def seg(a, b, w):
    """A thick line from a to b with square ends extended by half the width."""
    (x0, y0), (x1, y1) = a, b
    dx, dy = x1 - x0, y1 - y0
    l = math.hypot(dx, dy)
    ux, uy = dx / l * w / 2, dy / l * w / 2
    return fill([(x0 - ux - uy, y0 - uy + ux), (x1 + ux - uy, y1 + uy + ux),
                 (x1 + ux + uy, y1 + uy - ux), (x0 - ux + uy, y0 - uy - ux)])


def polyline(pts, w):
    return [seg(a, b, w) for a, b in zip(pts, pts[1:])]


def arc(cx, cy, r, a0, a1, w, n=16):
    """A thick arc between angles a0..a1 (degrees, y down so positive = clockwise)."""
    pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
            cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    return [fill([pts[i], pts[i + 1], (cx + (r - w) * math.cos(math.radians(a0 + (a1 - a0) * (i + 1) / n)),
                                       cy + (r - w) * math.sin(math.radians(a0 + (a1 - a0) * (i + 1) / n))),
                  (cx + (r - w) * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                   cy + (r - w) * math.sin(math.radians(a0 + (a1 - a0) * i / n)))]) for i in range(n)]


def head(tip, direction, size):
    """An arrowhead with its tip at `tip`, pointing along `direction` (degrees)."""
    a = math.radians(direction)
    dx, dy = math.cos(a), math.sin(a)
    bx, by = tip[0] - dx * size, tip[1] - dy * size
    return fill([tip, (bx - dy * size * 0.7, by + dx * size * 0.7), (bx + dy * size * 0.7, by - dx * size * 0.7)])


def rot(polys, deg, cx=16, cy=16):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return [[(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in p] for p in polys]


def move(polys, dx, dy, k=1.0):
    return [[(x * k + dx, y * k + dy) for x, y in p] for p in polys]


def shell(cx, top, w, h):
    """An artillery shell standing upright: pointed nose, straight body."""
    r = w / 2
    nose = [(cx + r * math.cos(math.radians(a)), top + r * 1.6 - r * 1.6 * math.sin(math.radians(a)))
            for a in range(0, 181, 15)]
    return [fill(nose + [(cx - r, top + h), (cx + r, top + h)])]


def plane(cx, cy, k=1.0):
    """A top-down aircraft pointing up."""
    p = [rect(-1.6, -9, 1.6, 9),                       # fuselage
         fill([(-11, -1), (11, -1), (11, 2.5), (-11, 2.5)]),   # wings
         fill([(-5, 6), (5, 6), (5, 8.5), (-5, 8.5)]),         # tailplane
         circle(0, -9, 1.6, 12)]
    return move(p, cx, cy, k)


def droplet(cx, cy, r):
    c = circle(cx, cy, r)
    return [c, fill([(cx - r * 0.86, cy - r * 0.5), (cx, cy - r * 2.3), (cx + r * 0.86, cy - r * 0.5)])]


def hull(y=0.0):
    """A ship side profile, bow to the right."""
    return [fill([(3, 18 + y), (29, 18 + y), (25, 24 + y), (7, 24 + y)]),
            rect(11, 12 + y, 20, 18 + y), rect(14, 8 + y, 17, 12 + y)]


def sub(y):
    """A submarine side profile, bow to the right, hull centred on y."""
    hull = fill([(3, y), (7, y - 3), (25, y - 3), (29.5, y), (25, y + 3), (7, y + 3)])
    return [hull, rect(12, y - 8, 18, y - 3), rect(14.5, y - 11, 15.8, y - 8)]


def star(cx, cy, r0, r1, n=8):
    return fill([(cx + (r1 if i % 2 == 0 else r0) * math.cos(math.pi * i / n - math.pi / 2),
                  cy + (r1 if i % 2 == 0 else r0) * math.sin(math.pi * i / n - math.pi / 2)) for i in range(2 * n)])


# Order = icon id = enum in hud.c (ICON_*).
ICONS = [
    ("hold_fire", shell(16, 7, 7, 18) + ring(16, 16, 14, 3) + [seg((7, 7), (25, 25), 3)]),
    ("fire_at_will", [fill([(1, 26), (3, 13), (13, 13), (15, 26)]), rect(5, 9, 11, 13), rect(13, 16, 21, 20),
                      star(25.5, 18, 2.6, 6.5)]),
    ("aim", ring(16, 16, 10, 2.5) + [rect(15, 1, 17, 10), rect(15, 22, 17, 31), rect(1, 15, 10, 17),
                                      rect(22, 15, 31, 17), circle(16, 16, 2.2, 12)]),
    ("evasive", polyline([(5, 28), (11, 20), (6, 13), (14, 6)], 3.2) + [head((22, 2), -30, 7)]
     + polyline([(14, 6), (18, 4)], 3.2)),
    ("stop", [rect(7, 7, 25, 25)]),
    ("crew_priority", [seg((7, 25), (20, 12), 4.5)] + ring(22, 10, 7.5, 3.5) + [hole(rect(21, 2, 26, 9))]),
    ("counter_flood", move(rot(hull(0), 16, 16, 21), -2, 4, 1.0) + arc(16, 14, 13, 190, 290, 2.6)
     + [head((25, 3), 30, 6.5)]),
    ("flood_magazines", shell(9, 4, 8, 22) + droplet(23, 21, 6)),
    ("strike", plane(16, 11, 0.95) + [fill([(14, 22), (18, 22), (18, 27), (16, 30), (14, 27)])]),
    ("patrol", plane(16, 16, 0.6) + arc(16, 16, 14, -60, 230, 2.5) + [head((7, 6), -40, 6)]),
    ("recall", plane(10, 11, 0.55) + arc(18, 18, 10, -90, 90, 2.6) + polyline([(18, 8.7), (25, 8.7)], 2.6)
     + [seg((3, 27), (29, 27), 2.5), head((11, 27.5), 180, 6)]),
    # Captain's report kinds.
    ("fire", rot(droplet(10, 22, 5), -25, 10, 22) + rot(droplet(22, 22, 5), 25, 22, 22) + droplet(16, 20, 7.5)
     + [rect(5, 27, 27, 30)]),
    ("torpedo", [rect(4, 14, 24, 18), fill([(24, 14), (28, 16), (24, 18)]), rect(2, 11, 4, 21)]
     + polyline([(6, 24), (10, 26), (14, 24), (18, 26), (22, 24), (26, 26)], 1.8)),
    ("water", droplet(16, 19, 8)),
    ("withdraw", polyline([(27, 16), (9, 16)], 4) + [head((3, 16), 180, 9)]),
    ("bridge_lost", [rect(6, 14, 26, 26), rect(10, 8, 22, 14)] + [hole(rect(9, 17, 23, 21))]
     + [seg((4, 4), (28, 28), 3.2), seg((28, 4), (4, 28), 3.2)]),
    # Submarines.
    ("dive", sub(12) + [rect(14.5, 17, 17.5, 24), head((16, 31), 90, 7)]),
    ("surface", sub(25) + [rect(14.5, 8, 17.5, 15), head((16, 1), -90, 7)]),
    ("battery", [rect(3, 9, 27, 25), hole(rect(6, 12, 24, 22)), rect(27, 13, 30, 21), rect(8, 14, 12, 20)]),
]


def main(out, preview=None):
    rows = (len(ICONS) + COLS - 1) // COLS
    W, H = COLS * CELL, rows * CELL
    atlas = bytearray(W * H)
    for i, (_, polys) in enumerate(ICONS):
        cov = raster(polys, CELL, CELL)
        cx, cy = i % COLS * CELL, i // COLS * CELL
        for y in range(CELL):
            atlas[(cy + y) * W + cx:(cy + y) * W + cx + CELL] = bytes(cov[y * CELL:(y + 1) * CELL])
    with open(out, "w") as o:
        o.write("/* Generated by tools/bakeicons.py: %d HUD icons, %d columns of %dx%d cells.\n"
                " * Do not edit; change the shapes in the script. */\n" % (len(ICONS), COLS, CELL, CELL))
        o.write("#define ICON_COLS %d\n#define ICON_CELL %d\n#define ICON_COUNT %d\n" % (COLS, CELL, len(ICONS)))
        o.write("#define ICON_ATLAS_W %d\n#define ICON_ATLAS_H %d\n" % (W, H))
        o.write("/* " + ", ".join("%d %s" % (i, n) for i, (n, _) in enumerate(ICONS)) + " */\n")
        o.write("static const unsigned char ICON_ATLAS[%d] = {\n" % len(atlas))
        for i in range(0, len(atlas), 32):
            o.write("    " + ",".join(str(b) for b in atlas[i:i + 32]) + ",\n")
        o.write("};\n")
    if preview:
        import struct
        import zlib
        k = 4                                          # 4x zoom, white on dark grey
        rows_out = []
        for y in range(H * k):
            row = bytearray([0])
            for x in range(W * k):
                v = atlas[(y // k) * W + x // k]
                g = 40 + v * 215 // 255
                row += bytes([g, g, g])
            rows_out.append(bytes(row))

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * k, H * k, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(b"".join(rows_out))) + chunk(b"IEND", b""))
        open(preview, "wb").write(png)
    print(f"{out}: {len(ICONS)} icons, atlas {W}x{H}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
