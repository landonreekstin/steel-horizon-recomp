#!/usr/bin/env python3
"""Export ship-class and weapon data to JSON for the engine.

Usage: python3 -I tools/data2json.py FILES_DIR OUTDIR

- weapons.json: maps/gunValues.xml (the table BOOT.BIN loads from
  Data/GunSmith/gunValues.xml), in the game's gun-index order.
- ships.json: per ship type, the most common stat set across all mission XMLs
  (missions tweak individual ships; the mode is the class baseline), how many
  mission entries agree with it, the model files, and -- from
  evidence/ship_loadouts.json (decompiled BOOT.BIN) -- weapons, hull type,
  submerged flag and constructor defaults. Flagship classes, which no mission
  places, come from their constructors alone.

Stats are the original game's units (e.g. speed 33 for a battleship); how they
map to knots or metres is not known yet (notes/unknowns.md).
"""
import collections
import glob
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

# Ship type suffix -> hull code used in model names (PSP_<side>_<code>NN.pingu).
HULL = {
    "AircraftCarrier": "CV", "Battlecruiser": "CB", "Battleship": "BB", "CargoShip": "AK",
    "Cruiser": "CA", "Destroyer": "DD", "DestroyerEscort": "DE", "Gerste": "GTN",
    "HospitalShip": "AH", "LandingCraft": "LCI", "MineLayer": "CM", "RepairShip": "AR",
    "Submarine": "SS", "TorpedoBoat": "PT", "ShadowSubmarine": "SSF",
}
# Nation prefix -> model side. Allied = American/British, Axis = German/Japanese.
SIDE = {"American": "AL", "British": "AL", "German": "AX", "Japanese": "AX", "Shadow": "SH"}
# Types whose model doesn't follow <nation side>_<hull code> (from the model list).
MODEL_OVERRIDE = {"GermanBossShip": ("AX", "GBS"), "JapaneseBossShip": ("AX", "JBS"),
                  "ShadowBossShip": ("SH", "MS"), "AmericanGerste": ("AX", "GTN")}
NUMERIC = ("max_health", "speed", "turn_rate", "aa_damage", "attack_mod", "defense_mod",
           "size_mod", "exposure_mod", "toughness", "special_recharge", "blocking")


def split_type(t):
    for nation in SIDE:
        if t.startswith(nation):
            rest = t[len(nation):]
            # longest suffix match first (DestroyerEscort before Destroyer)
            for suffix in sorted(HULL, key=len, reverse=True):
                if rest == suffix or (nation == "Japanese" and rest == "Shadow" + suffix):
                    return nation, suffix
            return nation, rest
    return None, t


def models_for(files_dir, side, code):
    out = {}
    for stage, n in (("intact", 1), ("damaged", 2), ("wrecked", 3)):
        rel = f"pingus/ships/{dict(AL='allied', AX='axis', SH='shadow')[side]}/PSP_{side}_{code}{n:02d}.pingu"
        if os.path.exists(os.path.join(files_dir, rel)):
            out[stage] = rel
    gun = f"pingus/guns/{dict(AL='allied', AX='axis', SH='shadow')[side]}/PSP_{side}_{code}01_G.pingu"
    if os.path.exists(os.path.join(files_dir, gun)):
        out["gun"] = gun
    return out


def main(files_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    guns = []
    for i, g in enumerate(ET.parse(os.path.join(files_dir, "maps/gunValues.xml")).getroot()):
        guns.append({"index": i, "name": g.get("name"), "base_damage": int(g.get("baseDamage")),
                     "min_range": int(g.get("minRange")), "max_range": int(g.get("maxRange")),
                     "fire_rate": float(g.get("fireRate"))})
    with open(os.path.join(out_dir, "weapons.json"), "w") as f:
        json.dump({"source": "maps/gunValues.xml", "guns": guns}, f, indent=1)

    seen = collections.defaultdict(collections.Counter)
    names = collections.defaultdict(collections.Counter)
    for p in sorted(glob.glob(os.path.join(files_dir, "maps", "*.xml"))):
        for s in ET.parse(p).getroot().iter("ship"):
            stats = tuple(int(s.get(k)) for k in NUMERIC if s.get(k) is not None)
            seen[s.get("type")][stats] += 1
            if s.get("name"):
                names[s.get("type")][s.get("name")] += 1
    ships = {}
    for t in sorted(seen):
        (stats, agree), = seen[t].most_common(1)
        nation, hull = split_type(t)
        side, code = MODEL_OVERRIDE.get(t, (SIDE.get(nation), HULL.get(hull)))
        entry = {"nation": nation, "hull": hull, "code": code,
                 "stats": dict(zip(NUMERIC, stats)),
                 "mission_entries": sum(seen[t].values()), "agree_with_baseline": agree}
        if names[t]:
            entry["label"] = names[t].most_common(1)[0][0]
        if side and code:
            entry["models"] = models_for(files_dir, side, code)
        ships[t] = entry
    loadouts = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "evidence",
                            "ship_loadouts.json")
    if os.path.exists(loadouts):
        with open(loadouts) as f:
            classes = json.load(f)["classes"]
        for t, c in classes.items():          # classes no mission places (flagships)
            if t not in ships:
                d = c.get("ctor_defaults", {})
                nation, hull = split_type(t)
                ships[t] = {"nation": nation, "hull": hull,
                            "stats": {k: d[k] for k in NUMERIC if k in d},
                            "mission_entries": 0, "source": "constructor only"}
        for t, entry in ships.items():
            c = classes.get(t)
            if c and c.get("ds_model"):
                # The constructor names its DS model, e.g. "AL_BBF01.nsbmd" -> PSP_AL_BBF01.
                m = re.match(r"([A-Z]{2})_([A-Z]+)\d\d\.nsbmd$", c["ds_model"])
                if m:
                    entry["code"] = m.group(2)
                    entry["models"] = models_for(files_dir, m.group(1), m.group(2))
            entry.setdefault("weapons", [])
            if c:
                entry["weapons"] = c["weapons"]
                entry["hull_type"] = c.get("ctor_defaults", {}).get("hull_type")
                entry["submerged"] = bool(c.get("ctor_defaults", {}).get("submerged", 0))
                entry["ctor_defaults"] = c.get("ctor_defaults", {})
    with open(os.path.join(out_dir, "ships.json"), "w") as f:
        json.dump({"source": "maps/*.xml <ship> entries (mode per type)", "ships": ships}, f,
                  indent=1)
    missing = [t for t, e in ships.items() if not e.get("models", {}).get("intact")]
    print(f"{len(guns)} guns, {len(ships)} ship types; no intact model for: {missing}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
