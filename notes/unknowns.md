# Open questions

- **.pingu VADDR values** (e.g. 0xb6c, 0x52a4 in BB01) are not file offsets; vertices
  are found by order instead. Probably offsets the loader relocates against some
  base. Confirm in BOOT.BIN's pingu loader.
- **.bog swizzling for formats 3/5** is decided by eye, not from code; also the
  meaning of flag 0x2.
- **.pingu render-state fields**: the three RGBA colours, the u32 after the hash
  (0 or 2), u16 at +0x1c (0 or 3), flag 0x40 at +0x1f on damaged models.
- **Untextured mesh** in every model (render state `2099 24c9`): shadow? waterline?
- **Mount matrix convention**: Q·M is derived from the data, not the code.
- ~~Scale~~ **answered in the emulator** (`notes/runtime.md`): models are drawn 1:1 in
  world units (object scale vector = 1,1,1), gunValues ranges are world units. Still
  open: ship speed in units/second (the tutorial battle ended before a sample).
- **attack_mod / defense_mod** (+0xc4 / +0xcc): no reader found by pattern search;
  may be read through getters.
- **Special abilities**: `special_id` (+0xb4) values and what each does.
- **Hit-chance matrix** in FUN_088e54fc: pseudocode control flow is tangled; check the
  exact bonuses against the assembly.
- **Submerged type 6** (destroyer escorts only): its gun-table key has no name, and
  the live table has no entry with key 6, so its stat lookups fail (return -1).
  Probably the hedgehog (`CHedgehog`, `PSP_Hedgehog01`), with stats set elsewhere.
- Air weapon type 1 (cruisers/battlecruisers only): anti-recon? (in-game text says
  cruisers auto-destroy recon planes).
- Mount-group numbers 1..9 vs .pts names (F/R/P/S) — inferred, not traced.
- `.ker`, `.bed` (fonts), `.bnk` (sound banks), `.scc`, `.rom`, `.mvl` not looked at.
- Music: which of the music manager's states 1 (tracks 1–5) and 2 (6–10) is the battle
  and which the campaign map, and what the global at 0x8830000-0x7150 (thresholds 90 /
  180 / 200) is: a campaign turn or mission number? The state comes through a virtual
  call (vtable 0x89d8708 +0x24); callers not traced. The engine assumes 6–10 = battle.
