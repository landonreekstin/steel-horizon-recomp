*Created by `re new`. The rules for every RE project are in `../CLAUDE.md`.*

## Target

`target/Steel Horizon (USA).iso` — Steel Horizon, **PSP** (Konami / Climax Group, 2007),
the user's own copy. SHA-256
`6e00ac4ba1f91385b8065c5be142fa73d45a2d58b45aae2cc18634c02d7b3e9c`.

- `PSP_GAME/SYSDIR/BOOT.BIN` — unencrypted relocatable MIPS Allegrex ELF (type 0xFFA0),
  ~1.7 MB `.text`, stripped, C++ (class-name strings survive).
- `PSP_GAME/USRDIR/GAMEDATA.IMP` + `GAMEDATA.XML` — uncompressed asset archive with an
  XML index (2,797 files: `.pingu` models, `.bog` textures, csv/xml data, `.at3` audio).

Question being answered: can the 3D battle layer's assets and mechanics be lifted into a
**new native Linux real-time naval battle game** (Total War style, no turn-based map)?
Approach chosen: a new engine (`engine/`, C11 + SDL2 + GL 3.3) that loads assets from the
user's ISO at runtime; the original code is decompiled for reference only — mechanics are
*inspired by*, not bound to, the original. No full static recomp of BOOT.BIN.
Plan: `~/.claude/plans/i-want-to-recompile-snoopy-pearl.md`.

Never commit game data: `target/` and `assets/` are gitignored.

## Status

Asset pipeline (milestone 1) done; formats in `notes/formats.md`, open questions in
`notes/unknowns.md`. Regenerate everything from the ISO:

```
7z x -oassets/iso "target/Steel Horizon (USA).iso" PSP_GAME/USRDIR
python3 -I tools/imp_extract.py assets/iso/PSP_GAME/USRDIR/GAMEDATA.{XML,IMP} assets/files
python3 -I tools/bog2png.py assets/files assets/png
python3 -I tools/pingu2gltf.py assets/files/pingus assets/png assets/gltf
python3 -I tests/test_assets.py        # 0 failures expected
python3 -I tools/data2json.py assets/files assets/data && python3 -I tests/test_data.py
python3 -I tools/prx2exec.py BOOT.BIN boot_exec.elf   # BOOT.BIN from PSP_GAME/SYSDIR
python3 -I tools/preview.py assets/files MODEL.pingu out.png [GUN.pingu]
```

- Archive: 2,797/2,797 files extracted, sizes match the index.
- Textures (.bog): 1,574/1,574 decode; checked by eye (ships, logo, fonts).
- Models (.pingu): 258/258 parse into meshes + materials; ships checked by software
  render with turrets placed at the .pts mounts (allied BB, axis CV, shadow DDF).
- Formats are derived from the data, not yet confirmed against BOOT.BIN code.
- Game data (phase 3): `tools/data2json.py` → `assets/data/{ships,weapons}.json`;
  64 ship classes with stats, weapons (decompiled, `evidence/ship_loadouts.json`),
  hull type, submerged flag and models. Constructor defaults equal the mission-XML
  baselines for every regular class (`tests/test_data.py`, 0 failures).
- Code: `tools/prx2exec.py` makes BOOT.BIN loadable (Ghidra headless; REA refuses it).
  Map in `notes/boot-bin.md`; hit-chance and damage rules in `notes/battle-logic.md`.
- Emulator (PPSSPP 1.20.4, `tools/ppsspp_dbg.py`, setup in `notes/runtime.md`): load
  base 0x08804000, relocated image == RAM; live gun table == gunValues.xml; ship
  objects read mid-battle; models drawn 1:1 in world units (scale question closed).
- Engine MVP (phase 5) in `engine/`: C11 + SDL2 + GL 3.3, loads the original
  .pingu/.bog/.pts directly (C port matches the Python decoders on all 1,832 files,
  `tests/test_engine_port.py`), Gerstner ocean, RTS camera, select/box-select,
  formation moves with no crossing paths, 20 Hz deterministic sim
  (`engine/build/test_engine assets`, 0 failures).
- Combat (phase 6): turrets on the model mounts with arcs/traverse/reload, ballistic
  shells with lead and dispersion, torpedoes, depth charges vs submarines, damage-stage
  model swaps and sinking, focus/split/hold fire orders, minimal enemy AI; scenarios
  `m01` and `fleet`. Rules in `notes/battle-logic.md` (last section); tests cover arcs,
  traverse, range, hit rates (lead checked by a mutant), stages, DC, torpedoes,
  determinism.
- Carriers and aircraft (6b): the original's fighter/bomber stats and models, strikes,
  patrols, escorts, dogfights, shipboard AA, land/rearm; scenario `carriers`; tests for
  launch, bombing, AA, interception, recall, determinism. Roadmap (incl. War Thunder-
  style damage model, controls toolbar, dive vs torpedo bombers): `notes/roadmap.md`.
- Damage model (phase 7, first pass): compartments, armour/penetration, flooding, list
  and counter-flooding, fires, magazines, destroyable modules, damage-control teams,
  cross-section panel. Rules in `notes/battle-logic.md`; tests incl. two mutants.
- Command and crew (7b): accuracy model + HUD readout (calibrated against fired
  shells), macro targeting, crew casualties, repair limits, hangar lifts, evasive
  manoeuvres, Captain's Command bar, clickable controls toolbar, baked font.
- Shipyards and modes (8): scenarios `assault`, `attack`, `defend`; build panel,
  drydock repairs, reinforcement waves, AI waves; `--trace N` prints the battle state.
- Fleet command (8b): command stream (replayable), flagships (L), officers, fleet bar
  (F2) with damage thumbnails, real-profile cross-section, dive/torpedo bombers.
- Submarines: surface / periscope / deep with a battery, deck guns; depth charges only
  on destroyers and escorts, short throw (notes/battle-logic.md, last section).

Build/run the engine:

```
cmake -S engine -B engine/build -G Ninja && ninja -C engine/build
engine/build/test_engine assets && python3 -I tests/test_engine_port.py
engine/build/shrts --assets assets      # front end: skirmish, custom battle, host/join online
engine/build/shrts --standalone         # no game files needed: own ship data, generated models
engine/build/shrts --assets assets [--scenario m01|fleet|carriers|assault|attack|defend|custom]
#   LMB select, RMB move (Shift queues), RMB enemy focus fire (Ctrl: secondaries only),
#   H hold fire, F fire at will, S stop, WASD/QE/wheel camera, Space pause, +/- speed
#   carrier selected: B air strike (focus target or nearest), G fighter patrol, R recall
#   damage control: P crew priority, X counter-flood, M flood magazines
#   T aim (ship/engines/magazines/steering/bridge/guns), Z evasive, Enter top report,
#   C Captain's Command bar, F1 controls toolbar (both clickable), F2 fleet bar, L flagship
#   submarines: J dive, K surface (surface / periscope / deep; battery charges on the surface)
# online (LAN/forwarded port): one host, the others join; teams default 0,1,0,1...
engine/build/shrts --assets assets --host 47800 --players 3 --teams 0,1,0 --scenario fleet --name Me
engine/build/shrts --assets assets --join HOSTADDR:47800 --name Friend
python3 -I tests/test_multiplayer.py     # lockstep check: host + 2 clients, same checksum
engine/build/shrts --assets assets --scenario fleet --ai-both --headless 900   # balance run
# record a battle to video with sound (auto camera; needs ffmpeg, e.g. `nix shell nixpkgs#ffmpeg-headless -c ...`)
engine/build/shrts --assets assets --scenario fleet --ai-both --no-fog --record out.mp4 --frames 32400
```

## Game-dev skills

`.claude/skills/` holds 14 engine-agnostic discipline skills vendored from
awesome-gamedev-agent-skills (game-ai, game-feel, game-ui-ux, camera-systems,
shader-programming, audio-design, performance-optimization, ...; see its README). Load
the matching one before design work in `engine/`; their engine examples are concepts to
port to C, not APIs to use.
