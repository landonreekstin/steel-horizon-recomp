#!/usr/bin/env python3
"""Bake a TrueType font into a C header holding an ASCII glyph atlas.

Usage: python3 -I tools/bakefont.py FONT.ttf PIXELS OUT.h

A small self-contained TrueType reader (cmap format 4, loca/glyf with simple and
composite glyphs, hmtx) and a 4x4-supersampled non-zero-winding rasterizer, so the
engine needs no font library. Glyphs 32..126 go in a 16-column grid of equal cells.
Used with DejaVu Sans Mono, whose licence allows redistributing the glyphs
(the notice is copied into the header).
"""
import os
import struct
import sys

SS = 4  # supersampling per axis


class Font:
    def __init__(self, data):
        self.d = data
        n = struct.unpack_from(">H", data, 4)[0]
        self.tab = {}
        for i in range(n):
            tag, _, off, ln = struct.unpack_from(">4sIII", data, 12 + 16 * i)
            self.tab[tag.decode("latin-1")] = (off, ln)
        head = self.tab["head"][0]
        self.upem = struct.unpack_from(">H", data, head + 18)[0]
        self.loca_long = struct.unpack_from(">h", data, head + 50)[0]
        self.nglyphs = struct.unpack_from(">H", data, self.tab["maxp"][0] + 4)[0]
        hhea = self.tab["hhea"][0]
        self.ascent, self.descent = struct.unpack_from(">hh", data, hhea + 4)
        self.nhmetrics = struct.unpack_from(">H", data, hhea + 34)[0]
        self.cmap = self._cmap()

    def _cmap(self):
        base = self.tab["cmap"][0]
        n = struct.unpack_from(">H", self.d, base + 2)[0]
        for i in range(n):
            pid, eid, off = struct.unpack_from(">HHI", self.d, base + 4 + 8 * i)
            sub = base + off
            if struct.unpack_from(">H", self.d, sub)[0] == 4 and pid in (0, 3):
                return self._fmt4(sub)
        raise ValueError("no cmap format 4")

    def _fmt4(self, sub):
        segx2 = struct.unpack_from(">H", self.d, sub + 6)[0]
        seg = segx2 // 2
        ends = struct.unpack_from(">%dH" % seg, self.d, sub + 14)
        starts = struct.unpack_from(">%dH" % seg, self.d, sub + 16 + segx2)
        deltas = struct.unpack_from(">%dh" % seg, self.d, sub + 16 + 2 * segx2)
        roff_at = sub + 16 + 3 * segx2
        roffs = struct.unpack_from(">%dH" % seg, self.d, roff_at)
        m = {}
        for i in range(seg):
            for c in range(starts[i], ends[i] + 1):
                if c > 0xFFFF or c == 0xFFFF:
                    continue
                if roffs[i] == 0:
                    g = (c + deltas[i]) & 0xFFFF
                else:
                    a = roff_at + 2 * i + roffs[i] + 2 * (c - starts[i])
                    g = struct.unpack_from(">H", self.d, a)[0]
                    if g:
                        g = (g + deltas[i]) & 0xFFFF
                m[c] = g
        return m

    def advance(self, g):
        h = self.tab["hmtx"][0]
        i = min(g, self.nhmetrics - 1)
        return struct.unpack_from(">H", self.d, h + 4 * i)[0]

    def _glyph_range(self, g):
        loca = self.tab["loca"][0]
        if self.loca_long:
            a, b = struct.unpack_from(">II", self.d, loca + 4 * g)
        else:
            a, b = (2 * v for v in struct.unpack_from(">HH", self.d, loca + 2 * g))
        return self.tab["glyf"][0] + a, b - a

    def contours(self, g):
        """List of contours; each a list of (x, y, on_curve)."""
        off, ln = self._glyph_range(g)
        if ln == 0:
            return []
        nc = struct.unpack_from(">h", self.d, off)[0]
        if nc < 0:
            return self._composite(off)
        ends = struct.unpack_from(">%dH" % nc, self.d, off + 10)
        npts = ends[-1] + 1
        p = off + 10 + 2 * nc
        ilen = struct.unpack_from(">H", self.d, p)[0]
        p += 2 + ilen
        flags = []
        while len(flags) < npts:
            f = self.d[p]
            p += 1
            flags.append(f)
            if f & 8:
                r = self.d[p]
                p += 1
                flags.extend([f] * r)
        flags = flags[:npts]
        xs, ys = [], []
        for coords, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    dv = self.d[p]
                    p += 1
                    v += dv if f & same else -dv
                elif not f & same:
                    v += struct.unpack_from(">h", self.d, p)[0]
                    p += 2
                coords.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], flags[i] & 1) for i in range(s, e + 1)])
            s = e + 1
        return out

    def _composite(self, off):
        p = off + 10
        out = []
        while True:
            fl, gi = struct.unpack_from(">HH", self.d, p)
            p += 4
            if fl & 1:
                dx, dy = struct.unpack_from(">hh", self.d, p)
                p += 4
            else:
                dx, dy = struct.unpack_from(">bb", self.d, p)
                p += 2
            if fl & 8:
                p += 2
            elif fl & 0x40:
                p += 4
            elif fl & 0x80:
                p += 8
            for c in self.contours(gi):
                out.append([(x + dx, y + dy, on) for x, y, on in c])
            if not fl & 0x20:
                return out


def flatten(contour):
    """Quadratic outline -> closed polyline: insert the implied on-curve midpoints
    between consecutive off-curve points, then 8 line steps per curve."""
    pts = []
    n = len(contour)
    for i, p in enumerate(contour):
        q = contour[(i + 1) % n]
        pts.append(p)
        if not p[2] and not q[2]:
            pts.append(((p[0] + q[0]) / 2, (p[1] + q[1]) / 2, 1))
    if not pts:
        return []
    k = next(i for i, p in enumerate(pts) if p[2])        # start on an on-curve point
    pts = pts[k:] + pts[:k]
    out = [(pts[0][0], pts[0][1])]
    i, n = 0, len(pts)
    while i < n:
        cur, nxt = pts[i], pts[(i + 1) % n]
        if nxt[2]:
            out.append((nxt[0], nxt[1]))
            i += 1
        else:
            end = pts[(i + 2) % n]
            for step in range(1, 9):
                t = step / 8
                a, b, c = (1 - t) * (1 - t), 2 * (1 - t) * t, t * t
                out.append((a * cur[0] + b * nxt[0] + c * end[0], a * cur[1] + b * nxt[1] + c * end[1]))
            i += 2
    return out[:-1]


def raster(polys, w, h):
    """Coverage 0..255 for polygons in pixel space (y down), non-zero winding."""
    W, H = w * SS, h * SS
    acc = [0] * (w * h)
    edges = []
    for poly in polys:
        for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
            if y0 != y1:
                edges.append((x0 * SS, y0 * SS, x1 * SS, y1 * SS))
    for sy in range(H):
        yc = sy + 0.5
        xs = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                xs.append((x0 + (yc - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
        if not xs:
            continue
        xs.sort()
        wind = 0
        for (xa, da), (xb, _) in zip(xs, xs[1:]):
            wind += da
            if wind == 0:
                continue
            a, b = max(0, int(xa + 0.5)), min(W, int(xb + 0.5))
            for sx in range(a, b):
                acc[(sy // SS) * w + sx // SS] += 1
    return [min(255, v * 255 // (SS * SS)) for v in acc]


def main(path, pixels, out):
    f = Font(open(path, "rb").read())
    scale = pixels / (f.ascent - f.descent)
    cw = int(f.advance(f.cmap[ord("M")]) * scale + 0.999)
    ch = pixels
    base = f.ascent * scale
    cols, rows = 16, 6
    W, H = cols * cw, rows * ch
    atlas = bytearray(W * H)
    for c in range(32, 127):
        g = f.cmap.get(c, 0)
        polys = [[(x * scale, base - y * scale) for x, y in flatten(con)] for con in f.contours(g)]
        cov = raster(polys, cw, ch)
        cx, cy = (c - 32) % cols * cw, (c - 32) // cols * ch
        for y in range(ch):
            atlas[(cy + y) * W + cx:(cy + y) * W + cx + cw] = bytes(cov[y * cw:(y + 1) * cw])
    with open(out, "w") as o:
        o.write("/* Generated by tools/bakefont.py from %s at %dpx: ASCII 32..126,\n" % (os.path.basename(path), pixels))
        o.write(" * %d columns of %dx%d cells. Do not edit.\n *\n" % (cols, cw, ch))
        o.write(" * DejaVu fonts: Bitstream Vera copyright (c) 2003 Bitstream, Inc.; DejaVu changes\n"
                " * are in the public domain. The Vera licence permits redistribution of the font\n"
                " * software, including modified forms, provided this notice is kept and the result\n"
                " * is not sold by itself or named \"Bitstream\" or \"Vera\". */\n")
        o.write("#define FONT_COLS %d\n#define FONT_CELL_W %d\n#define FONT_CELL_H %d\n" % (cols, cw, ch))
        o.write("#define FONT_ATLAS_W %d\n#define FONT_ATLAS_H %d\n" % (W, H))
        o.write("static const unsigned char FONT_ATLAS[%d] = {\n" % len(atlas))
        for i in range(0, len(atlas), 24):
            o.write("    " + ",".join(str(b) for b in atlas[i:i + 24]) + ",\n")
        o.write("};\n")
    print(f"{out}: {cw}x{ch} cells, atlas {W}x{H}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), sys.argv[3])
