# Steel Horizon (PSP) asset formats

All little-endian. Status of each claim: **checked** = holds for every file in the
archive (by `tests/test_assets.py`) or confirmed by rendering; **hypothesis** = fits the
data but not yet confirmed in BOOT.BIN code.

## GAMEDATA.IMP / GAMEDATA.XML (archive) — checked
- IMP starts with `iMp0`; the XML lists every file with `Offset`, `CompressedSize`,
  `FullSize`, `Compression`. All 2,797 entries are `Compression="none"` and
  `CompressedSize == FullSize`, so a file is `imp[Offset : Offset+Size]`.
- Tool: `tools/imp_extract.py`.

## .bog — texture (magic `" GOB PSP"`) — checked by rendering
| off | type | field |
|---|---|---|
| 0x00 | char[8] | `" GOB PSP"` |
| 0x08 | f32 | version 1.05 |
| 0x10 | u32 | format: 3 = 16bpp ABGR1555, 5 = RGBA8888, 6 = 8bpp CLUT, 7 = 4bpp CLUT |
| 0x14/0x18 | u32 | width / height |
| 0x1c | u32 | mip count (1, or 4 on map tiles) |
| 0x24 | u32 | pixel bytes, all levels |
| 0x28 | u32 | flags: 0x4 = CLUT data swizzled; 0x2 unknown |
| 0x34 | — | palette (RGBA8888 ×16 or ×256, CLUT formats), then pixels |

- Each level's width is padded to a multiple of 16 **pixels** (checked: all 1,574
  sizes match).
- Swizzle = PSP 16-byte × 8-row blocks. CLUT textures: swizzled iff flag 0x4
  (decoding both ways: 842/845 and 435/469 agree). Formats 3 and 5 are always
  swizzled (by eye; *hypothesis* until the loader is read).
- 1555 over 4444/5650 picked by eye; every fmt-3 pixel has bit 15 set.
- Tools: `tools/bog.py`, `tools/bog2png.py`.

## .pingu — model (magic `"gniP PSP"`) — checked (all 258 parse; renders correct)
| off | type | field |
|---|---|---|
| 0x08 | f32 | version 2.01 |
| 0x10 / 0x20 | f32[4] | bbox min / max |
| 0x30 | f32 | bounding radius |
| 0x34 | f32 | position scale S |
| 0x38 | u32 | mesh count |
| 0x3c | | `"MAT "`, u32 1, u32 material count M (0x44) |
| 0x6c | M × 0x100 | material records; texture name (= .bog basename) first |
| 0x70+M·0x100 | n × 0x24 | render states: u32 hash, u32, 3 × RGBA (amb/diff/spec?), RGBA, …, **u8 +0x1e texture index (0xff none)**, u8 +0x1f flags |
| … | 0x28 each | mesh records: u32 display-list offset in words, u16, u16 nverts, u16 0, u16 nprims, **u8 render-state index** |
| … | | PSP GE display lists, one per mesh (BASE, OFFSET, VTYPE, TEXSCALE/OFFSET U/V, VADDR, PRIM×n, RET), then one more RET |
| RET+12 | | vertex data for all meshes, back to back in list order (ends at EOF) |

- Every mesh: VTYPE 0xa1 (u8 uv, s8 normal, s8 position, 8 bytes/vertex), only
  triangle strips. Position = s8 / 128 × S. UV = u8 / 128 × TEXSCALE + TEXOFFSET
  (GE 24-bit floats). Y up, +Z = bow.
- One untextured mesh per model (render state hash `2099 24c9`), probably the
  shadow/blob — *hypothesis*.
- Model families: `PSP_<side>_<class><nn>` with `01` intact, `02` damaged, `03` wrecked
  (their textures use `_D2_` / `_D3_`). Guns: `pingus/guns/<side>/…_G.pingu`.
- Tools: `tools/pingu.py`, `tools/pingu2gltf.py`, `tools/preview.py`.

## .pts — mount points (magic `"POINT 1.00"`) — checked by rendering
- u32 count at 0x10; records of 0x50 from 0x14: char[32] name, f32[3] position,
  f32[9] row-major matrix M.
- Rotation in model space = Q·M, Q = rows (−1,0,0)(0,0,1)(0,1,0). That gives fore
  mounts at yaw 0, aft 180, P*/S* ±90; 716 of 832 are exactly upright and the
  tilted ones are in the wrecked `*03` models.
- Names: `F1..` fore, `R1..` aft, `P1/S1` port/starboard, `smoke01..` funnels;
  gun models have `firepoint` (muzzle).

## .pfn — text list of the texture names a model uses (one per line).

## Sound banks (`sound/banks/*.bnk`, "SBlk") — `tools/bnk.py`, `engine/src/asset.c`
- Header u32 words: `3, 2, 0x18, …, data_offset (word [4]), data_size`; "SBlk" block at
  0x18. game.bnk: 415 samples, data at 0x6b30 (4.0 MB); frontend.bnk the menu sounds.
- At 0x58: one 12-byte record per sound, `s8 volume (127), s8 pan, s16, u8 present, u8,
  u16 flags, u32 offset` into a table of 8-byte entries (`u24 offset into the sample
  table, u8 count`) that follows them.
- Table of 24-byte sample entries from `00 6e aa 42` (not a float):
  `u8, u8, s8 fine-tune (0xaa = -86), u8 root note (0x42), u32 0, u16 0, u16 ADSR1 0x80ff,
  u16 ADSR2 0x9fc0, u16 flags (0 / 0x40), u32 offset, u32 size`; offsets relative to the
  data, sizes include one end-flagged pad frame.
- Names follow in **20-byte slots, `char name[16]` + u32 entry index, in hash order**
  (not entry order; some slots empty), cut at 15 characters ("ENORMCANNSHT1_W"). Until
  2026-10-27 the readers took the names in file order, which gave most sounds the wrong
  sample.
- Data: standard PSP/PS2 ADPCM (16-byte frames: predictor<<4|shift, flags, 28 nibbles;
  the usual five filter pairs). All 262,252 frames parse; flag bit 0 ends a sample.
- **Sample rate: 11.025 kHz.** BOOT.BIN's sound request (0x0898b6c8) starts every sound
  at note 60 (0x3c, no bend unless the caller asks); the note-to-pitch routine
  (0x08988054, run under unicorn) with this bank's root bytes gives SAS pitch 0x400 for
  note 60 (0x1000 = 44.1 kHz at note 84). Cross-check: with the names mapped by index,
  the US voice lines' F0 is 160–300 Hz at 11.025 kHz (800–1,100 Hz at 44.1 kHz). The
  engine mixes at 44.1 kHz and resamples (audio.c).
- Music (`sound/music/*.at3`) is ATRAC3 in RIFF; not decoded (no decoder on hand).
