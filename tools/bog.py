"""Decoder for Steel Horizon PSP textures (.bog, magic " GOB PSP").

Layout (see notes/formats.md):
  0x00  char[8]  " GOB PSP"
  0x08  f32      version (1.05)
  0x0c  u32      0
  0x10  u32      format: 3 = 16bpp direct, 5 = RGBA8888, 6 = 8bpp CLUT, 7 = 4bpp CLUT
  0x14  u32      width
  0x18  u32      height
  0x1c  u32      mip level count
  0x20  u32      0
  0x24  u32      pixel data size, all mip levels
  0x28  u32      flags: 0x4 = CLUT data PSP-swizzled (16-byte x 8-row blocks);
                 0x2 unknown. Formats 3/5 are swizzled whatever the flags say
                 (found by decoding both ways; not yet confirmed in BOOT.BIN).
  0x2c  u32[2]   0
  0x34  palette  (RGBA8888, 16 or 256 entries; CLUT formats only)
        pixels   mip 0 first; each level's width padded to a multiple of 16 pixels
"""
import struct
import zlib

MAGIC = b" GOB PSP"
FMT_16, FMT_32, FMT_T8, FMT_T4 = 3, 5, 6, 7
BPP = {FMT_16: 16, FMT_32: 32, FMT_T8: 8, FMT_T4: 4}
PAL_ENTRIES = {FMT_T8: 256, FMT_T4: 16}
FLAG_SWIZZLED = 0x4


class Bog:
    def __init__(self, data):
        if data[:8] != MAGIC:
            raise ValueError("not a .bog file")
        (self.version,) = struct.unpack_from("<f", data, 8)
        (_, self.fmt, self.width, self.height, self.mips, _, self.data_size,
         self.flags) = struct.unpack_from("<8I", data, 0xc)
        if self.fmt not in BPP:
            raise ValueError(f"unknown format {self.fmt}")
        off = 0x34
        n = PAL_ENTRIES.get(self.fmt, 0)
        self.palette = [data[off + i * 4: off + i * 4 + 4] for i in range(n)]
        off += n * 4
        self.pixels = data[off: off + self.data_size]

    @property
    def swizzled(self):
        return self.fmt in (FMT_16, FMT_32) or bool(self.flags & FLAG_SWIZZLED)

    def row_bytes(self, w):
        return ((w + 15) & ~15) * BPP[self.fmt] // 8

    def level(self, n):
        """Return (width, height, rgba bytes) for mip level n."""
        off = 0
        w, h = self.width, self.height
        for _ in range(n):
            off += self.row_bytes(w) * h
            w, h = max(1, w // 2), max(1, h // 2)
        rb = self.row_bytes(w)
        raw = self.pixels[off: off + rb * h]
        if self.swizzled:
            raw = unswizzle(raw, rb, h)
        return w, h, self._to_rgba(raw, rb, w, h)

    def _to_rgba(self, raw, rb, w, h):
        out = bytearray()
        for y in range(h):
            row = raw[y * rb: (y + 1) * rb]
            if self.fmt == FMT_32:
                out += row[: w * 4]
            elif self.fmt == FMT_T8:
                for x in range(w):
                    out += self.palette[row[x]]
            elif self.fmt == FMT_T4:
                for x in range(w):
                    out += self.palette[(row[x >> 1] >> ((x & 1) * 4)) & 15]
            else:
                for x in range(w):
                    out += rgba5551(row[x * 2] | row[x * 2 + 1] << 8)
        return bytes(out)


def rgba5551(v):
    # GE pixel format 1 (ABGR1555): R in bits 0-4, G 5-9, B 10-14, A bit 15.
    # Chosen over 4444/5650 by eye; every fmt-3 pixel has bit 15 set.
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2),
                  255 if v & 0x8000 else 0))


def unswizzle(buf, rb, h):
    """Undo PSP texture swizzling: data is stored as 16-byte x 8-row blocks."""
    out = bytearray(rb * h)
    i = 0
    for by in range(0, h, 8):
        for bx in range(0, rb, 16):
            for y in range(by, min(by + 8, h)):
                o = y * rb + bx
                out[o: o + 16] = buf[i: i + 16]
                i += 16
            i += 16 * max(0, by + 8 - h)
    return bytes(out)


def write_png(path, w, h, rgba):
    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))
    rows = b"".join(b"\0" + rgba[y * w * 4: (y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))
