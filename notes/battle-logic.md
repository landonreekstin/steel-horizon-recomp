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

## Cross-section damage model (phase 7, `engine/src/damage.c`) — design, not the original
- **Layout per class** (`layout.c`): 10 compartments for capital ships and carriers, 8
  cruisers/auxiliaries, 6 destroyers/escorts/subs, 4 PT boats, equal lengths stern→bow,
  each split port/starboard for water. Modules: every weapon mount, a magazine per
  main-battery/torpedo compartment, two engine rooms amidships, steering aft, bridge and
  radar forward, two AA groups, flight deck on carriers. Armour (shell_pen units):
  BB belt 2.9 / deck 2.2, CB 2.2/1.6, CA 1.8/1.2, CV 1.4/1.2, DD-class 0.3/0.2.
- **Hits:** `shell_pen = damage/25 × (1 − 0.35·range/max)`; flat fire meets the belt,
  shells beyond 55% of max range and all bombs (pen 2.4) meet the deck; ±15% scatter.
  Non-penetrating: 12% structure, splinters on exposed modules, 5% fire. Penetrating:
  structure −dmg/(maxHP/n·1.2), modules there 50% each, fire 25% (bombs 50%), flat
  hits 35% leak, magazine detonation 2% per 90 dmg. Torpedoes always flood (bulges cut
  it 40% on capital ships), depth charges flood subs. Damage beyond a wrecked
  compartment carries outward; a wrecked compartment that keeps being hit is holed.
- **Over time:** leaks fill the dry space, power pumps and teams drain and plug; fires
  grow, spread, burn structure and modules, cook off a dry magazine (0.4%/s at full
  fire); list = port/starboard water difference; the crew counter-floods past ~10°.
- **She goes down when** water ≥ reserve buoyancy (BB 32%, CV 30%, CA 27%, DD 24%),
  list > ~22° (capsize), a magazine goes up, mean structure < 20%, or 55% of the
  compartments are wrecked (broken back). Health bar and 01/02/03 models = the worst of
  those margins.
- **Effects:** engines → top speed; steering out → rudder jammed; bridge out → helm at
  half rate, +20% spread; radar out → +40% spread; turret out → silent, damaged →
  slower reload/training; flooded magazine → its turrets silent; AA hp scales AA;
  flight deck < 40% → no launches or landings.
- **Damage control:** teams per class (BB/CV 4, CA 3, DD 2, PT 1) reassigned every second
  by score (fire ×6 next to a magazine, leaks, module repairs) under a policy: auto,
  fires, flooding, weapons. Repairs cap at 70%, wrecked modules repair at a third.
- Measured (seeds 1–3, both sides AI): m01 3–3.5 min; fleet 9–11 min, sinkings by
  flooding, structure, magazines and the odd capsize; carriers 23–51 min. Mirrored
  identical fleets over 40 seeds: 19–21 and 22–18, so no side bias.

## Command and crew (phase 7b) — design
- **Accuracy:** each shell's aim error per axis is √(σ² + lead² + turn²): σ = the gun's
  dispersion at range from the original's accuracy table, × fire control (radar/bridge),
  × crew (up to +60% short-handed), × 1.25 when aiming at a part of the ship, × 1.3 while
  evading, × the firer's own turning; lead = 15% of the *relative* speed of the two
  ships × time of flight; turn = the target's turn over the flight. `hit_probability`
  integrates that over the tapered hull outline and is what the HUD shows; the test
  checks it against fired shells (60% vs 59% still, 50% vs 50% crossing at 2,100).
- **Macro targets (T):** the aim point moves to the compartment of the chosen engines,
  magazines, steering, bridge or main guns; aiming at the steering puts >2× the share
  of damage in the two stern compartments.
- **Crew:** complement per class (BB 2,500, CV 2,600, CA 1,100, DD 330, PT 17, SS 80 …);
  penetrations, splinters, fires and rising water kill men. Fewer men → fewer teams
  (⌈teams × min(1, 1.4·crew)⌉), slower teams, slower reloads; below 8% she's abandoned.
- **Repair limit:** a module knocked below 10% is lost for the battle; others repair to 70%.
- **Hangar lifts:** two per carrier; with them out, landed planes stay on deck unrearmed.
- **Evasive (Z):** for 60 s, comb any torpedo whose closest approach is within her
  length + 60 in the next 45 s (turn parallel to the track, smaller turn), else
  zig-zag ±35° every 12 s; costs 30% accuracy.
- **Captain's reports** (`captain.c`): fire by a dry magazine → flood it; torpedoes
  inbound → evade; list > 6° → counter-flood; water > 40% of reserve → pumps first;
  2+ fires → fight fires; condition < 30% with an enemy within 3,500 → withdraw;
  carrier with bombers and a target in range → strike. Acting quiets that report for
  45 s, dismissing for 60 s. A ship with her bridge destroyed reports nothing and is
  listed as such.

## Shipyards and game modes (phase 8, `engine/src/base.c`) — design
- **Shipyard:** a shore "ship" (hull type 30): 8 waterfront sections, 6,000 HP,
  concrete armour (belt 2.6 / deck 2.0), two coastal batteries of the nation's
  battleship gun plus two of its secondary, AA, headquarters (bridge) and radar, an
  ammunition dump (magazine), a **drydock** and a **slipway**. It can't flood, list or
  sink by water; it falls to structure failure or its dump exploding. Ships are kept
  off its ground. The model is generated in code (placeholder; there is no original).
- **Economy:** 1,200 points to start, +4/s while the yard stands. Costs: BB 1,500,
  CV 1,400, CB 1,100, CA 800, DD 400, SS 350, DE 300, PT 120; build time = cost/10 s
  (scaled by slipway health; nothing below 30%). New ships launch seaward of the yard.
- **Drydock repairs** for friendly ships moored (< 4 u/s) within half-length + 900:
  structure, fires, leaks and water, modules back to 100% (even ones lost at sea,
  slowly), crew replacements; scaled by drydock health, none below 30%.
- **Modes:** *assault*: a yard each, first to destroy the other's wins (a side with a
  standing yard isn't beaten by losing its fleet). *attack / defend*: one yard; the
  attacker gets a cruiser + destroyer wave every 5 min, the defender 70% income, 30 min
  limit; the attacker wins by destroying the yard, the defender on time or when the
  attacker has nothing left afloat.
- **AI:** yards keep two ships queued, cycling their options; ships below 35% go home
  to the drydock until 85%; attackers make for the enemy yard and fight only ships
  within 3,500; in assault every fourth ship guards home and the rest muster off the
  yard, sailing in waves of 4, then 6, 8, … so stalled assaults escalate.
- Measured, both sides AI: assault 40–63 min, 2 of 3 decided within 80 min; attack and
  defend split 2–2 over four seeds each (decided in 14–20 min or on time).
