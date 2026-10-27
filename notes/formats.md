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
- Table of 24-byte entries from the first f32 85.21 (`00 6e aa 42`):
  `f32 85.21, u32 0, u32 0x80ff0000, u32 flags (0x9fc0 / 0x409fc0), u32 offset, u32 size`;
  offsets relative to the data, sizes include one end-flagged pad frame.
- Names follow, one per entry in order, in 40-byte records, cut at 15 characters
  ("ENORMCANNSHT1_W").
- Data: standard PSP/PS2 ADPCM (16-byte frames: predictor<<4|shift, flags, 28 nibbles;
  the usual five filter pairs). All 262,252 frames parse; flag bit 0 ends a sample.
- **Sample rate is not in the table.** Taken as 44.1 kHz: the voice lines' pitch comes
  out at 133–164 Hz (male) at 44.1 kHz, 67 Hz at 22.05 kHz. Unconfirmed by ear yet
  (samples on the NAS under `steel-horizon/sound-check/`).
- Music (`sound/music/*.at3`) is ATRAC3 in RIFF; not decoded (no decoder on hand).
