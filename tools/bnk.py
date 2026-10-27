#!/usr/bin/env python3
"""Steel Horizon sound banks (.bnk, "SBlk"): named PS-ADPCM (VAG) samples.

Layout (from game.bnk): u32 header words, "SBlk" block at 0x18; a table of
24-byte entries (f32 85.21, u32 0, u32 0x80ff0000, u32 flags, u32 offset, u32 size)
whose offsets are relative to the sample data (the last 4 MB of the file, at
header word [4]); then the names, one per entry in the same order, each in a
40-byte record. Samples are standard PSP ADPCM: 16-byte frames (predictor/shift,
flags, 14 bytes = 28 nibbles), each sample followed by one end-flagged pad frame.
The sample rate isn't in the table; see notes/formats.md for how it was chosen.

Usage: python3 -I tools/bnk.py BANK.bnk OUTDIR    (writes NAME.wav per sample)
"""
import re
import struct
import sys
import wave

RATE = 44100
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
        names = [n.decode() for n in re.findall(rb"[A-Z0-9_]{4,}", data[:self.data_off])]
        # Entries: walk the 24-byte records that fit the layout.
        o = data.find(struct.pack("<f", 85.21) if False else b"\x00\x6e\xaa\x42")
        ents = []
        while o >= 0 and o + 24 <= self.data_off:
            f, z, a, fl, off, size = struct.unpack_from("<fIIIII", data, o)
            if not (1 < f < 200) or self.data_off + off + size > len(data):
                break
            ents.append((off, size))
            o += 24
        names = names[-len(ents):] if len(names) >= len(ents) else names
        self.samples = {}
        for (off, size), name in zip(ents, names):
            a = self.data_off + off
            self.samples[name] = data[a:a + size]


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
