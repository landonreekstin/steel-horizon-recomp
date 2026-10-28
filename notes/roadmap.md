# Roadmap

Full plan: `~/.claude/plans/i-want-to-recompile-snoopy-pearl.md`. Status per phase in
`CLAUDE.md`. Items marked *(requested)* came from the user.

## Done
- 0–4: bookkeeping, archive extraction, formats, game data, battle-logic notes.
- 5: engine MVP (renderer, camera, selection, formation moves, deterministic sim).
- 6: combat: turrets, ballistics, torpedoes, depth charges, stages and sinking, fire orders.
- 6b: carriers and aircraft: fighters (CAP/escort, dogfights), bombers (strikes),
  shipboard AA, launch/land/rearm cycle.
- 7 (first pass): compartments, armour and penetration, flooding with list and
  counter-flooding, fires, magazine detonation, engine/steering/bridge/radar/turret/
  AA/flight-deck modules, damage-control teams with priority policies, flooding
  magazines, a cross-section panel for the selected ship (notes/battle-logic.md).
- 7b (first pass): accuracy model with relative speed and turning, shown in the HUD;
  macro targeting (T); crew and casualties; repair limit; carrier hangar lifts;
  evasive manoeuvres (Z); Captain's Command bar (reports, one-click actions, dismiss,
  collapsed alert, bridge-out ships listed); clickable controls toolbar (F1); text
  rendering (DejaVu Sans Mono baked by tools/bakefont.py).
- 8 (shipyards): shore shipyards with coastal batteries, drydock repairs, slipway
  production on a budget, the assault and attack/defend modes with reinforcement
  waves, AI that builds, repairs, guards and attacks in escalating waves, a shipyard
  build panel and a mode banner. Placeholder generated yard model.
- Submarine depth (surface / periscope / deep, battery, deck gun) and historical ASW: depth
  charges only on destroyers and escorts, short throw *(requested)*.
- 8b: command stream, flagships, distinguished officers, fleet toolbar with live
  damage thumbnails, ship-profile cross-section, dive and torpedo bombers.
- 8c (first pass): online lockstep multiplayer, teams of several players each with a
  fleet, desync detection, chat (Y) (`tests/test_multiplayer.py`). Rejoin after a drop
  (host keeps the full turn history and replays it to the returning player); tested by
  `tests/test_multiplayer.py` (host paced with `--pace 40` so the drop lands mid-battle).
- 8 (front end): main menu, skirmish list, custom-battle fleet picker (nations, hull
  counts, optional shipyards), host/join screens and lobby; minimap; hover accuracy
  readout; original sound effects and voice lines.
- Generated ship models (`--models generated`, or the main menu's toggle): the first
  step toward not needing the ISO.
- Standalone mode (`--standalone`, or automatic without game data): our own ship table
  of 26 historical classes, generated models and aircraft; the game runs with no game
  files at all. Each standalone class now has a "plan" read off published profile drawings (turret, tower, funnel, mast, aft-superstructure and island positions; approximate), and genship.c builds her from it. Sounds without the game's bank: `synth.c` synthesises every
  effect the battle uses (first pass, not tuned by ear); no crew voices.

## Next

### 7. Cross-section damage model *(requested, War Thunder style)* — remaining
Done: first pass (above), plus panel tooltips, sending teams to a compartment by hand
(click it), per-mount ammunition (restocked at the drydock), and hit height (upperworks
/ belt / below the waterline). Standalone classes place their engine rooms under the
plan's funnels and the bridge in the tower or island (the original's models carry no
funnel points: their `smoke01-03` are bow / midships / stern damage-smoke emitters).
Original plan:
Replace the single HP pool with a ship laid out as compartments along the hull, each
with its own state, and shells/bombs/torpedoes resolved against that layout:
- **Compartments** bow → stern, below and above the waterline, built per class from
  the model (hull length, turret mounts, funnel `smoke*` points).
- **Flooding** per compartment from below-waterline hits and torpedoes; water spreads
  to neighbours, adds list and trim, slows the ship; counter-flooding and pumping.
- **Fires** that spread, cut systems and threaten magazines; fought by crews.
- **Engine / boiler rooms**: damage cuts top speed and acceleration; steering-gear
  damage jams the rudder.
- **Magazine detonation**: a hit or a fire reaching a turret's magazine can blow the
  ship up outright; flooding the magazine is the counter-measure.
- **Destroyable modules**: each gun turret, the bridge (helm / command delay), radar
  (fire-control accuracy), AA mounts, catapults/flight deck on carriers.
- **Damage-control crews** assigned per compartment with priorities and repair rates.
- **Cross-section view** of the selected ship: compartments coloured by fire,
  flooding and module state (the in-game damage panel).
- Model swaps (01/02/03) follow overall condition; a ship sinks from lost buoyancy
  or capsizes from list, not from an HP counter.

### 7b. Command and crew *(requested)* — first pass done (above), hover accuracy readout
done; per-turret aim done (right-click a gun on the cross-section, then click an enemy;
a battery order or Fire at will puts it back); evasion tuned (notes/battle-logic.md:
helps most at range, costs some of her own fire). Original list:
- **Accuracy metric** per firing ship and target: gun, crew quality, range, the speed
  difference between the two ships (and own turning), fire-control damage; shown when
  targeting, since you don't aim shells yourself.
- **Targeting**: general (fire at the ship) or **macro targets**: aim for the engines,
  magazines, steering, bridge or guns, at an accuracy cost (fewer hits, but they land
  in the chosen compartment).
- **Captain's Command toolbar**: each ship's captain reports the recommended immediate
  action (fire near a magazine → flood it, list → counter-flood, torpedoes inbound →
  evade, steering jammed, losing a duel → withdraw); clicking a report carries it out
  without selecting that ship. The bar can be dismissed and alerts when reports are
  pending. A ship whose bridge is destroyed sends no reports; the bar shows her as
  out of contact.
- **Carrier hangar lifts**: destroyable; with the lifts out, planes can land but can't
  be struck below to rearm or repair.
- **Repair limits**: modules can be repaired until damaged past a critical point;
  beyond it they're lost for the battle.
- **Crew**: a crew count per ship; casualties from hits, fires and flooding slow damage
  control, reloading and every other operation.
- **Evasive manoeuvres**: one key (or a captain's report) to comb incoming torpedoes or
  zig-zag against gunfire, at the cost of your own accuracy.

### 8. Battle layer
- **Final UI art** *(requested)*: icon buttons in the controls toolbar (War Thunder
  style) instead of text, and Captain's reports that show a thumbnail of the ship
  reflecting her current damage and afflictions (fires, flooding, list, lost turrets).
  Done: icon buttons with key letters and hover tooltips in the controls toolbar, and an
  icon per report kind on the Captain's cards (16 vector icons baked by
  `tools/bakeicons.py` into `engine/src/icon_atlas.h`, drawn from the UI font texture).
  Captain's cards also carry a live damage thumbnail of the ship.
- **Everything clickable** *(requested)*: done. Every order has a toolbar button (split fire
  and recon as click-a-target modes, a Queue toggle for waypoints, torpedo/dive strikes,
  flagship, special), plus pause/speed, control groups, chat, quit and fleet-bar reopen.
- **Controls toolbar** *(requested, War Thunder style)*, alongside the Captain's Command bar: an on-screen bar listing
  every control and hotkey for the current selection (move, focus/split/hold fire,
  launch strikes, damage control), with tooltips; clickable as an alternative to keys.
- **Carrier attack types** *(requested)*: separate **dive bombers** (steep dive,
  accurate, bomb hits deck and magazines) and **torpedo bombers** (low, slow run-in,
  drop torpedoes that flood below the waterline, vulnerable to AA), alongside
  fighters; per-carrier air group mix, squadron selection and direct orders.
  Done: dive and torpedo bombers; air group mix per side in custom battles (balanced
  6/4/4, fighters 10/2/2, strike 2/6/6); strike buttons (all / torpedo / dive) then click
  the target, or B over an enemy.
- **Bases / shipyards** *(requested)*: a shore base or shipyard on the map that can
  be shelled, bombed and destroyed (its own compartments and defences: coastal guns,
  AA, a harbour boom). Ships inside its radius repair and rearm; it builds new units
  mid-battle from a budget (production queue, build times per class), and launches
  its own aircraft if it has an airfield.
  Done: coastal batteries, AA, drydock, slipway, harbour boom (torpedoes netted in the
  anchorage), airfield (fighter-heavy group; AI flies it).
- **Game modes** *(requested)*:
  - *Shipyard assault*: each side has a shipyard; first to destroy the other's wins.
    Shipyards are where you build reinforcements and repair, so losing ships early
    can be recovered, losing the yard cannot.
  - *Attack vs defend*: only the defender has a shipyard (repairs, reinforcements,
    coastal batteries); the attacker brings a larger fixed fleet and must destroy it
    within a time limit, the defender wins by holding out.
- Recon planes *(done: fog of war, radar, recon floatplanes and scouts)* and the original's special abilities *(done: all eight, paired by the game's own
  training text; U)*.
- Scenario setup, fleet picker, maps *(done: front end, custom battles, islands /
  archipelago; reinforcement waves; victory on yards, time or points)*.
- Fleet-level AI commander (targeting, formations, strike timing): done for skirmish
  (`commander.c`; A/B in notes/battle-logic.md); in the yard modes it commands waves at sea.
- UI: ship cards, damage panels, minimap.
- Audio: sound banks done; `.at3` music done (title track in the menu, battle playlist
  in battle, ducked under the effects; `tools/at3wav.py`). Still: battle vs map lists unconfirmed.

### 8b. Fleet command *(requested)*
- **Flagship**: one ship per fleet designated flagship; she gets boosts (and the
  fleet suffers if she's lost).
- **Distinguished officers**: n per team, assigned to ships (several may serve on one
  ship); each boosts one aspect of that ship: repairs, AA, main-turret operation,
  gunnery, engineering, damage control...
- **Fleet toolbar**: one card per ship in your fleet, with a thumbnail showing her
  current status; click to select (double-click to centre the camera).
- **Cross-section that looks like the ship**: the damage panel drawn from the ship's
  real side profile instead of blocks.

### 8c. Online multiplayer *(requested)*
- Teams with several players per team, each commanding a fleet; deterministic
  lockstep over the network (the sim is already fixed-tick and seeded), lobby,
  team and fleet assignment, chat, reconnect.

### 9. Tutorial *(requested; last, once everything else is in place)* — done
Built to this design (main menu → Tutorial; `engine/src/tutorial.c`, `tests/test_tutorial.c`): lessons as scenarios `tutorial1..8` in
main.c's setup_scenario (fog off), a `tutorial.c` step machine fed every issued command
(hook in main.c `issue()`) plus world/HUD state; a panel top right (the Captain's bar
moves below it via a hud y offset) showing the step text, its icon and key, Skip step /
Next lesson (reinit the world in place: setup_scenario, renderer_load_class, reset hud).
Lessons and completion conditions: 1 camera moved, select, move (CMD_MOVE), queued move
(flag), stop; 2 hover an enemy 1 s (accuracy), focus fire, aim (CMD_AIM), hold/fire at
will, sink her; 3 pre-damaged cruiser: act on a captain's report, DC priority click, crew
policy, counter-flood, fires out; 4 PT boats' torpedoes: evade (CMD_EVADE), sink them;
5 carrier: fighter patrol, strike pick, target sunk; 6 submarine: surface, periscope
depth, sink a cargo ship, go deep from the escort; 7 flagship, officer, save a group,
special, recon, win; 8 shipyard: select it, build, take a damaged ship to the drydock,
airfield strike, beat the raid.
- An in-game tutorial campaign that teaches the controls toolbar, fleet movement,
  focus and macro targeting, the accuracy readout, damage control and the Captain's
  Command bar, carrier strikes, and the shipyard modes, step by step.

### 10. Icon-first HUD *(requested)* — done
Built: 11 new icons (condition, crew, list, ammo, focus, six aim points), every icon
also baked at 16 px for inline use (`ui_icon_small`); `stat()` in hud.c draws icon +
value in green/amber/red and leaves a hover tooltip. Readout rows, fleet cards and the
compartment box use them; fire state (`hud_fire_state`) shows over each own ship
(main.c `ship_markers`), on her card and in the readout; the toolbar lights H or F and
Z for the selection and draws Aim as the current aim point. Not done: the battery
target as a class silhouette (the name is shown).


Read the state at a glance: icons wherever text is used now, with a number beside
them where the value matters.
- **Ship readout** (`hud.c`, the selected-ship lines near `condition %.0f%%`): an icon
  each for condition, water/flooding, list, crew and DC mode, each with its number and
  a colour (green, amber, red) for how bad it is. This replaces the `condition 80%  water 3%/40%  crew …`
  string.
- **Battery line**: the target as a class silhouette, hit chance with a crosshair icon,
  bearing guns as pips (filled = on bearing), ammo with a shell icon.
- **Aim**: an icon per aim point (ship, engines, magazines, steering, bridge, guns),
  not the `aim: magazines [T]` text, here and on the controls toolbar button.
- **Fire state**: show whether a ship is on hold fire, fire at will, focus fire or
  secondaries only (Ctrl): an icon by the ship in the world (above her, like
  the selection marker), on her fleet-bar (F2) card and in the readout. The same for
  evasive, flagship and submarine depth.
- Cross-section panel: icons for the compartment `structure / water P S / fire / teams`
  line.
- Keep a tooltip with the full text on hover, so the meaning can still be read.
- Icons go into `icon_atlas.h` alongside the toolbar ones, in the same style.

### 10b. Enemy damage, as observed *(requested)*

Show what the lookouts could see of an enemy, never her exact numbers (those stay for
own ships). Today the hover tooltip (`hud.c` `hover_tooltip`) prints her exact
condition %: replace that.
- **Condition band**: undamaged / light / heavy / critical as a coloured bar, from
  her hp with the band edges blurred (no exact %).
- **Visible damage icons** (the section 10 icon set): on fire, listing / low in the
  water, slowed (engines), guns silenced, steering jammed (circling), magazine fire.
  Each one shows only once it is outwardly visible (a fire's smoke, a list past a few
  degrees, speed well under her class top speed, turrets that stopped firing). These
  line up with the aim points, so the player can see whether "aim: engines" is working.
- **Where**: the hover tooltip; a small card for the selection's focus target; a thin
  band bar over enemy ships while own ships are firing at them (optional, toggle).
- **Better information as a reward**: close range (well inside main-gun range) or a
  spotter / recon plane overhead narrows the band to a closer estimate (e.g. tens of
  percent); a ship in fog shows her last-seen state, greyed, with its age.
- Same rules for the AI and online play: computed from what that side has spotted (the
  vision system), so the readout doesn't leak hidden information.
