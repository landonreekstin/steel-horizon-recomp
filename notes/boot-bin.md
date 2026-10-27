# BOOT.BIN code map (USA)

Analysed as a static image: `python3 -I tools/prx2exec.py BOOT.BIN boot_exec.elf`
(applies all 72,316 PRX relocations at base **0x08804000**, where PPSSPP loads it, so
every address below is a real runtime address). Checked against emulator RAM after
load: all 495,628 code words and 3,078 data words match, except 230 syscall stubs and
2 function-replacement hooks that PPSSPP patches itself. (An earlier build ignored
the relocation `addrbase` byte; that only misplaced references into the second
segment — `.bss` globals — not the code/rodata/data the findings rest on.) Ghidra 12.1.4, `MIPS:LE:32:default` — no Allegrex module, so VFPU
instructions decode as bad data; integer code (constructors, tables) is unaffected.
REA's Ghidra lane refuses the image (it requires a MIPS ABI-flags record), so Ghidra
headless is used directly.

C++ objects keep their vtable pointer at **+0xc**; vtable entries are 8 bytes
`{s16 this-adjust, pad, fn*}`, called as `fn(this + adjust, ...)`.

| Address | What | Evidence |
|---|---|---|
| 0x088b1f28 | class registry: `Register(name, factory)` for 99 classes (`CShip`, `CAmericanBattleship`, …, `CShipWeapon_Surface`) | decompiled |
| 0x088aece4 | `CreateByName("C…")` | call sites pass class-name strings |
| e.g. 0x088af360 → 0x08955f84 | factory → constructor (`CAmericanBattleship`, 0x1e4 bytes) | decompiled |
| ship ctor fields | 0xb0 hull type · 0xb4 special id (inferred) · 0xb8 submerged · 0xc0 max hp · 0xc2 current hp · 0xc4 attack · 0xc6 size · 0xc8 blocking · 0xca exposure · 0xcc defense · 0xce damage stage 1–3 (XML `toughness`) · 0xd0 aa_damage · 0xd2 special recharge · 0xd4 turn rate · 0xd6 speed | values equal the mission-XML baseline for every regular class |
| vtable +0x104 | per-class weapon setup | `evidence/ship_loadouts.json` |
| 0x088c9324 / 0x088c91f0 / 0x088c9458 | AddSurface / AddAir / AddSubmerged weapon (ship, owner, type, mount group); 10 slots at ship+0x11c | decompiled |
| 0x088ecc58 | surface `SetGun(type)`: 0..9 = 18"…3" (one init fn each) | switch |
| 0x088eabf8 | submerged `SetWeapon(type)`: 0 24"T, 2 18"T, 3 24"T variant, 4 300lb DC, 5 600lb DC, 6 unnamed (hedgehog?) | switch |
| 0x088ee698 / 0x088ee6e0 | gun-table lookups: 10 shell entries at +0x104, 7 torpedo/DC/air at +0x74 (stride 0x14) | decompiled |
| 0x089d2fa8 | static gun table `{u32, char* name, shell idx, torp idx, ?}` × 16, names at 0x089bf118 | data |
| "Data/GunSmith/gunValues.xml" | path the gun values load from (the IMP copy is `maps/gunValues.xml`) | string |

| 0x088e94fc | gunnery hit chance | `notes/battle-logic.md` |
| 0x088eb708 | torpedo / depth-charge hit chance | 〃 |
| 0x088cb818 | apply weapon damage | 〃 |
| 0x088c9c98 | change HP, update damage stage | 〃 |

Full pseudocode dump (6,389 functions, 0 failures) is regenerated with the
`DumpAll.java` Ghidra script; it is not committed (derived from the game code).

Leftovers from the DS version: constructors set `"AL_BB01.nsbmd"`-style model names
(Nitro format); the PSP models are the matching `PSP_AL_BB01.pingu`.
