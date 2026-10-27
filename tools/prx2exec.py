#!/usr/bin/env python3
"""Turn a relocatable PSP PRX (ELF type 0xFFA0) into a static ET_EXEC for Ghidra/REA.

Usage: python3 -I tools/prx2exec.py BOOT.BIN OUT.elf [BASE]   (BASE default 0x08804000,
       where PPSSPP loads it, so addresses match the emulator)

Applies the SHT_PRXRELOC (0x700000a0) section relocations: each Elf32_Rel has
r_offset relative to program header `ofsbase` = (r_info >> 8) & 0xff, the
stored value relative to program header `addrbase` = (r_info >> 16) & 0xff, and
type = r_info & 0xff, one of R_MIPS_32 (2), R_MIPS_26 (4), R_MIPS_HI16 (5),
R_MIPS_LO16 (6). HI16 entries are paired with the next LO16 (standard MIPS
ABI rule): the full address (hi << 16) + sext(lo) is relocated and the carry from
the low half is folded into each pending HI16. Then segment/section addresses and the entry
point are moved by BASE, e_type becomes ET_EXEC and the reloc sections become
SHT_NULL so nothing applies them twice.
"""
import struct
import sys

SHT_PRXRELOC = 0x700000a0
SHF_ALLOC = 2
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def main(src, dst, base="0x08804000"):
    base = int(base, 0)
    d = bytearray(open(src, "rb").read())
    if d[:4] != b"\x7fELF" or struct.unpack_from("<H", d, 0x10)[0] != 0xffa0:
        sys.exit("not a PSP relocatable PRX (ELF type 0xFFA0)")
    entry, phoff, shoff = struct.unpack_from("<III", d, 0x18)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", d, 0x2a)

    phdrs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        _, off, vaddr, _, filesz, _ = struct.unpack_from("<6I", d, o)
        phdrs.append((o, off, vaddr, filesz))

    def file_offset(vaddr):
        for _, off, va, filesz in phdrs:
            if va <= vaddr < va + filesz:
                return off + vaddr - va
        raise ValueError(f"vaddr {vaddr:#x} not in any file-backed segment")

    counts = {}
    pending_hi = []                  # file offsets of HI16 words awaiting their LO16
    for i in range(shnum):
        o = shoff + i * shentsize
        _, sh_type, flags, addr, off, size = struct.unpack_from("<6I", d, o)
        if sh_type != SHT_PRXRELOC:
            continue
        for r in range(off, off + size, 8):
            r_offset, info = struct.unpack_from("<II", d, r)
            rtype, ofsbase, addrbase = info & 0xff, (info >> 8) & 0xff, (info >> 16) & 0xff
            at = file_offset(phdrs[ofsbase][2] + r_offset)
            delta = base + phdrs[addrbase][2]   # value is relative to that segment
            (word,) = struct.unpack_from("<I", d, at)
            if rtype == R_MIPS_32:
                word = (word + delta) & 0xffffffff
            elif rtype == R_MIPS_26:
                target = ((word & 0x3ffffff) << 2) + delta
                word = (word & 0xfc000000) | ((target >> 2) & 0x3ffffff)
            elif rtype == R_MIPS_HI16:
                pending_hi.append((at, delta))
                counts[rtype] = counts.get(rtype, 0) + 1
                continue                 # patched when its LO16 arrives
            elif rtype == R_MIPS_LO16:
                lo = sext16(word & 0xffff)
                for hat, hdelta in pending_hi:
                    (hw,) = struct.unpack_from("<I", d, hat)
                    full = ((hw & 0xffff) << 16) + lo + hdelta
                    hw = (hw & 0xffff0000) | (((full + 0x8000) >> 16) & 0xffff)
                    struct.pack_into("<I", d, hat, hw)
                pending_hi = []
                word = (word & 0xffff0000) | ((word + delta) & 0xffff)
            else:
                sys.exit(f"unsupported relocation type {rtype} at {r:#x}")
            struct.pack_into("<I", d, at, word)
            counts[rtype] = counts.get(rtype, 0) + 1
        if pending_hi:
            sys.exit(f"{len(pending_hi)} HI16 relocations without a LO16")
        struct.pack_into("<I", d, o + 4, 0)          # SHT_NULL

    for i in range(shnum):
        o = shoff + i * shentsize
        flags, addr = struct.unpack_from("<II", d, o + 8)
        if flags & SHF_ALLOC:
            struct.pack_into("<I", d, o + 12, addr + base)
    for o, _, vaddr, _ in phdrs:
        struct.pack_into("<II", d, o + 8, vaddr + base, vaddr + base)
    struct.pack_into("<H", d, 0x10, 2)                # ET_EXEC
    struct.pack_into("<I", d, 0x18, entry + base)
    open(dst, "wb").write(d)
    print(f"relocated at {base:#x}: " + ", ".join(f"type {k}: {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
