#!/usr/bin/env python3
"""Checks on assets/data/*.json produced by tools/data2json.py.

Usage: python3 -I tests/test_data.py [FILES_DIR] [DATA_DIR]
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGULAR_STATS = ("max_health", "speed", "turn_rate", "aa_damage", "exposure_mod", "size_mod")


def main(files, data):
    fails = []
    ships = json.load(open(os.path.join(data, "ships.json")))["ships"]
    guns = json.load(open(os.path.join(data, "weapons.json")))["guns"]
    if len(guns) != 16:
        fails.append(f"{len(guns)} guns")
    if len(ships) != 64:
        fails.append(f"{len(ships)} ship types")
    for t, s in ships.items():
        model = s.get("models", {}).get("intact")
        if not model or not os.path.exists(os.path.join(files, model)):
            fails.append(f"{t}: no intact model")
        d = s.get("ctor_defaults", {})
        # Two independent sources: mission XML mode vs. decompiled constructor.
        if "Boss" not in t and s.get("mission_entries"):
            for k in REGULAR_STATS:
                if s["stats"].get(k) != d.get(k):
                    fails.append(f"{t}.{k}: missions {s['stats'].get(k)} vs ctor {d.get(k)}")
        if not s["weapons"] and t != "AmericanHospitalShip":
            fails.append(f"{t}: no weapons")
        if s.get("submerged") != ("Submarine" in t or t in ("AmericanFlagshipSub",
                                                            "GermanBossShip")):
            fails.append(f"{t}: submerged flag {s.get('submerged')}")
    bb = [w["weapon"] for w in ships["AmericanBattleship"]["weapons"]]
    if bb[:3] != ["16 Inch"] * 3:
        fails.append(f"AmericanBattleship weapons {bb}")
    for f in fails[:30]:
        print("FAIL", f)
    print(f"{len(fails)} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    a = sys.argv[1:] + [None, None]
    sys.exit(main(a[0] or os.path.join(ROOT, "assets/files"), a[1] or os.path.join(ROOT, "assets/data")))
