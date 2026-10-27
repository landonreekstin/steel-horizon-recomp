#!/usr/bin/env python3
"""Steel Horizon sound banks (.bnk, "SBlk"): named PS-ADPCM (VAG) samples.

Layout (from game.bnk): u32 header words, "SBlk" block at 0x18; a table of
24-byte entries (f32 85.21, u32 0, u32 0x80ff0000, u32 flags, u32 offset, u32 size)
whose offsets are relative to the sample data (the last 4 MB of the file, at
header word [4]); then a name table of 20-byte slots, `char name[16]` + u32 entry
index, in hash order (not entry order; some slots empty). Samples are standard PSP ADPCM: 16-byte frames (predictor/shift,
flags, 14 bytes = 28 nibbles), each sample followed by one end-flagged pad frame.
Played at 11.025 kHz (note 60 against the root in each entry; notes/formats.md).

Usage: python3 -I tools/bnk.py BANK.bnk OUTDIR    (writes NAME.wav per sample)
"""
import re
import struct
import sys
import wave

RATE = 11025   # the game plays every sample at note 60 = 11.025 kHz (notes/formats.md)
COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode(frames):
    out, s1, s2 = [], 0, 0
    for o in range(0, len(frames) - 15, 16):
        pred, shift = frames[o] >> 4, frames[o] & 15
        k1, k2 = COEF[pred] if pred < 5 else (0, 0)
        for b in frames[o + 2:o + 16]:
            for nib in (b & 15, b >> 4):
                v = nib - 16 if nib >= 8 else nib
                v = (v << 12) >> shift
                v += (s1 * k1 + s2 * k2) >> 6
                v = max(-32768, min(32767, v))
                out.append(v)
                s2, s1 = s1, v
        if frames[o + 1] & 1:
            break
    return out


class Bank:
    def __init__(self, data):
        if data[0x18:0x1c] != b"SBlk":
            raise ValueError("not an SBlk bank")
        self.data_off = struct.unpack_from("<I", data, 16)[0] + 0x18 - 0x18
        # The sample data starts right after the name list; header word [4] + 0x18 gives it
        # in game.bnk (0x6b30); fall back to the first frame-aligned run if not.
        self.data_off = struct.unpack_from("<I", data, 16)[0]
        # Entries: walk the 24-byte records that fit the layout.
        o = data.find(struct.pack("<f", 85.21) if False else b"\x00\x6e\xaa\x42")
        ents = []
        while o >= 0 and o + 24 <= self.data_off:
            f, z, a, fl, off, size = struct.unpack_from("<fIIIII", data, o)
            if not (1 < f < 200) or self.data_off + off + size > len(data):
                break
            ents.append((off, size))
            o += 24
        # Name slots after the entry table: name (NUL-padded to 16) + u32 entry index.
        names = {}
        for m in re.finditer(rb"[A-Za-z0-9_]{3,16}\x00", data[o:self.data_off]):
            p = o + m.start()
            if p + 20 <= self.data_off:
                idx = struct.unpack_from("<I", data, p + 16)[0]
                if idx < len(ents):
                    names[idx] = m.group()[:-1].decode()
        self.samples = {}
        for i, (off, size) in enumerate(ents):
            a = self.data_off + off
            self.samples[names.get(i, "SAMPLE%03d" % i)] = data[a:a + size]


def main(path, outdir):
    import os
    b = Bank(open(path, "rb").read())
    os.makedirs(outdir, exist_ok=True)
    for name, frames in b.samples.items():
        pcm = decode(frames)
        with wave.open(os.path.join(outdir, name + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
    print(f"{len(b.samples)} samples -> {outdir}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
