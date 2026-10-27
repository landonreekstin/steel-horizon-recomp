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

## Next

### 7. Cross-section damage model *(requested, War Thunder style)* — remaining
Done in the first pass (above); still to do: labels and tooltips on the panel (needs
text rendering, shared with the toolbar), assigning teams to a compartment by hand,
per-turret magazines and ammunition counts, decks above/below the waterline (shell
hit height), and compartments laid out from real ship plans once ships are generated.
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

### 8. Battle layer
- **Controls toolbar** *(requested, War Thunder style)*: an on-screen bar listing
  every control and hotkey for the current selection (move, focus/split/hold fire,
  launch strikes, damage control), with tooltips; clickable as an alternative to keys.
- **Carrier attack types** *(requested)*: separate **dive bombers** (steep dive,
  accurate, bomb hits deck and magazines) and **torpedo bombers** (low, slow run-in,
  drop torpedoes that flood below the waterline, vulnerable to AA), alongside
  fighters; per-carrier air group mix, squadron selection and direct orders.
- **Bases / shipyards** *(requested)*: a shore base or shipyard on the map that can
  be shelled, bombed and destroyed (its own compartments and defences: coastal guns,
  AA, a harbour boom). Ships inside its radius repair and rearm; it builds new units
  mid-battle from a budget (production queue, build times per class), and launches
  its own aircraft if it has an airfield.
- **Game modes** *(requested)*:
  - *Shipyard assault*: each side has a shipyard; first to destroy the other's wins.
    Shipyards are where you build reinforcements and repair, so losing ships early
    can be recovered, losing the yard cannot.
  - *Attack vs defend*: only the defender has a shipyard (repairs, reinforcements,
    coastal batteries); the attacker brings a larger fixed fleet and must destroy it
    within a time limit, the defender wins by holding out.
- Recon planes and the original's special abilities (`special_id`, recharge times).
- Scenario setup, fleet picker, maps; victory conditions and reinforcements.
- Fleet-level AI commander (targeting, formations, strike timing).
- UI: ship cards, damage panels, minimap.
- Audio: `.at3` music, sound banks.
