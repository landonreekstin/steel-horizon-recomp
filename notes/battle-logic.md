# Original battle logic (reference for the RTS redesign)

Read from Ghidra pseudocode of the relocated BOOT.BIN (`notes/boot-bin.md`).
**Status:** decompiled reading, not checked instruction-by-instruction against the
assembly, and not observed at runtime. Good enough to seed an "inspired by" design;
re-verify any number you want to copy exactly.

Units: positions are floats in world units. Range checks compare squared distances.
How world units relate to `gunValues.xml` `maxRange` (3200 for big guns) is unknown —
the gunnery bands below reach 6000 (notes/unknowns.md).

## Ship state (object offsets)
| off | field |
|---|---|
| 0xb0 | hull type (1 CV, 2 CB, 3 BB, 4 AK, 5 CA, 6 DD, 7 DE, 8 AH, 9 LCI, 10 CM, 11 AR, 12 SS, 13 PT, 14 BBF, 15 DDF, 16 SSF, 17 Shadow mothership, 18 GTN, 19 Shadow sub, 20 JBS, 21 GBS) |
| 0xb4 | special-ability id (inferred) |
| 0xb8 | submerged |
| 0xc0 / 0xc2 | max HP / current HP |
| 0xc4 / 0xcc | attack / defense mod (no reader found yet) |
| 0xc6 / 0xca | size mod / exposure mod (target profile, below) |
| 0xce | **damage stage 1–3** (XML calls it `toughness`); drives the 01/02/03 model swap |
| 0xd0 | AA damage (copied into the ship's air weapon) |
| 0x11c | 10 weapon slots |

## Gunnery hit chance — `FUN_088e94fc(weapon, target)` → percent
1. Range band (firer→target distance d): d < 2400 → 75; < 4800 → 55; < 6000 → 45; else 15.
2. Firer hull vs target hull matrix: e.g. a carrier firing at CB/BB +15, at CV/CA −15;
   DD/DE (6/7) firing at BB/CB/CV −40 and at subs (12/13) +40; subs firing at big
   ships +40, at DD/DE −40; most other pairs +10. (Control flow in the pseudocode is
   tangled — verify exact values against the assembly before copying.)
3. **Aspect profile:** c = |dot(shot direction, target heading)|;
   profile = exposure + (size − exposure)·(1 − c); chance = chance × profile / 100.
   Broadside targets present `size_mod`, bow/stern-on targets `exposure_mod`.
   Halved for a target with special 9 active (torpedo boat evasion?).
4. Bonuses gated by tech/upgrade bit flags (+10, +25, or forced 100).

## Torpedo / depth-charge hit chance — `FUN_088eb708`
- Depth charges (submerged types 4/5): always 100.
- Torpedoes: d < 1500 → 100; < 2000 → 80; < 2500 → 70; else 60.
  +40 vs CV/CB/BB/CA, −30 vs DD/DE. Same aspect profile rule.

## Damage — `FUN_088cb818(target, weapon, hit_quality, …)`
1. dmg = weapon damage (gunValues `baseDamage` via the weapon).
2. dmg ×= 1 − 0.25·max(0, weapon tier (+0xb4, 1 for guns) − target damage stage).
3. Near misses: quality 2 → ×0.5, 3 → ×0.25 (0 if a target flag is set).
4. Upgrade/ability multipliers: ×0.5 (target flag, vs weapon kinds 3/4),
   ×1.5 (torpedo with attacker flag), ×0.8 (two defensive flags), ×0.75 (vs torpedo).
5. Surface/air weapons (+0x98 < 3) against a submerged target: ×0.1.
6. Apply via `FUN_088c9c98`: HP −= dmg, clamped [0, max]; crossing 66% / 33% moves
   the damage stage (1→2→3, and back on repair); HP ≤ 0 → `FUN_088ca6f8` (sink).

## Loadouts
Per class in `evidence/ship_loadouts.json` (exported into `assets/data/ships.json`).
Surface battery entries are per *mount group*, not per turret: the US battleship has
three 16" entries (groups 1,1,2) but four turret mounts (F1, F2, R1, R2).

## Takeaways for the RTS
- Keep: aspect-based profile (rewards manoeuvring to cross the T), range bands,
  hull-vs-hull counters, damage stages with visible model swaps, depth charges vs subs.
- Replace: percent-roll hits with simulated shells (dispersion scaled by these
  chances), single HP pool with sectioned damage (stage swaps per section).

## What the RTS does with it (phase 6, `engine/src/combat.c`, `layout.c`, `ai.c`)
Design choices, tuned with `shrts --headless N --ai-both --scenario m01|fleet`.
- **Mounts:** model turrets (`F1..`, `R1..`, `P1..`, `S1..` in the .pts) take the next
  weapon entry of their mount group; leftover entries get virtual mounts (group 3/4
  side batteries, beam torpedo tubes or sub bow tubes, stern depth-charge racks).
  Arcs: fore/aft ±150° around bow/stern, sides ±80° around the beam, deck tubes ±60°,
  bow tubes ±25°. Traverse 5°/s (≥80 dmg), 8°/s (≥55), 15°/s smaller.
- **Reload:** `fire_rate` × 2.5 s (16": 17.5 s, 5": 7.5 s); torpedoes × 10 (60 s).
- **Shells:** analytic ballistic arc (g = 60 u/s², 260–330 u/s horizontal, 12 s to
  3200) at an exact lead point, plus Gaussian aim error σ = 0.010·d·85/p with
  p = `hit_chance` (the original's range bands and hull counters, without the aspect
  term) and a 15% target-speed misjudgement. The hit is whatever hull the shell lands
  in (bbox footprint, beam tapering to bow/stern); within 12 units: a quarter damage.
  Damage = `baseDamage`, no other modifiers yet (phase 7 adds sections).
- **Torpedoes:** spread of three ±3° at 30 u/s to `maxRange`; arm against friends
  after 3 s. **Depth charges:** lobbed on the boat, detonate 2 s later, 45-unit radius;
  the only weapon that reaches a submerged hull (guns and torpedoes ignore subs).
- **Stages:** model 01/02/03 at >2/3, >1/3, else; HP 0 sinks (wreck settles ~80 s).
- Measured (seed 1–3): m01 4 BB vs 4 DD → BBs win in 165–200 s, ~20% of shells hit;
  `fleet` 8 v 8 → 460–600 s, 26–28% hit, 30+ torpedoes, ~2–9 torpedo hits.

## Aircraft (phase 6b, `engine/src/air.c`)
From the original: `CFighterPlane` / `CBomberPlane` constructors (FUN_089559b0 /
FUN_08955a50 for US; every nation identical): type 0/1 at +0x98, HP 600 / 800 at
+0x9c/+0x9e, 400 / 300 at +0xa8 (taken as speed), 70 / 60 at +0xa0 (unknown), model
scale 2. Models: US/British `AL_AS0n`, German/Japanese `AX_AS0n`, Shadow `SH_AS0n`
(`pingus/props`; AL has 01–02, AX/SH 01–03). Weapons: `Aircraft Guns1` 30 dmg every
0.2 s, `Aircraft Bomb1` 100 dmg. Ship AA = `aa_damage` (CA 100, CB 70, CV 50, BB 40,
DD 20). How the original launches or resolves strikes was not traced.
Design: 6 fighters + 6 bombers per carrier; speed = original × 0.15 (60 / 45 u/s);
launch every 2 s; bombers dive to 130 and release at the fall-time lead point
(σ = 10 + 0.05·alt), then land and rearm in 40 s; fighters patrol or escort, pick
planes within 2,500 of what they guard, match speed on the tail, 25 % hit per shot
within 300 units; AA twice a second within 1,200, 30 % falling to 15 % at range.
Measured `carriers` scenario, 4 seeds: 18–25 min, ~50 % of bombs hit, AA and fighters
each kill planes (fighters more); Japan won all 4 after the fighter fix.
