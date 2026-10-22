#!/usr/bin/env python3
"""Memory card check through the game's own menus (milestone 2, docs/game-internals.md 10e).

    python tools/saves.py [OUT] [--port N]

One session from a copy of build/rig/saves/fortbaxter (slot 1 = "1 Soldier"):
  save    ACTIVATE_SAVE_MENU (script opcode 0x0260) -> Save Game -> slot 2 -> Yes: a "2 ..." slot file appears
  load    pause menu -> Game -> Load Game -> slot 1 -> Yes: slot 1 is read in full, the player is back in free roam
  delete  pause menu -> Game -> Delete Game -> slot 2 -> Yes: the slot file and its card-order entry are gone
Screenshots after every key press go to OUT (default build/saves); OUT/results.json holds the checks. Exit code 1
when one fails. Menu keys (CONTROLS.md): Shift = Cross, Return = Start, arrows = D-pad.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
FOLDER = ("mc0", "BASLUS-21590GTA40001")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "saves"))
    ap.add_argument("--port", type=int, default=47640)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    data = os.path.join(out, "data")
    folder = os.path.join(data, *FOLDER)
    g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="saves", data_dir=data,
                       port=a.port)
    shots = [0]

    def keys(tag, *seq):
        """KEY[@seconds to wait after], a screenshot after each"""
        for k in seq:
            key, _, wait = k.partition("@")
            g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))
            shots[0] += 1
            g.cmd("screenshot " + os.path.join(out, "%02d_%s_%s.png" % (shots[0], tag, key)))

    def slot_files():
        return sorted(f for f in os.listdir(folder) if f[:1].isdigit())

    def log_text():
        return open(g.log_path, errors="replace").read()

    checks = {}
    try:
        g.wait_ingame()
        before = slot_files()
        # save: the menu opens on "Save Game"; the slot list starts on Cancel, two down is slot 2
        g.cmd("scm " + (rig.Scm.op(0x0260) + rig.Scm.end()).hex())
        time.sleep(3)
        keys("save", "Shift@3", "Down", "Down", "Shift@3", "Up", "Shift@12", "Shift@4")
        new = [f for f in slot_files() if f not in before]
        checks["save"] = {"ok": len(new) == 1 and new[0].startswith("2 ") and
                          os.path.getsize(os.path.join(folder, new[0])) == 98304, "new_files": new}
        # load slot 1 (Map -> Brief -> Game tab)
        mark = len(log_text())
        keys("load", "Return@3", "Right", "Right", "Shift@2", "Shift@3", "Down", "Shift@3", "Up", "Shift@15")
        tail = log_text()[mark:]
        read_full = "'/BASLUS-21590GTA40001/1 Soldier' flags=0x1" in tail and "result=98300" in tail
        _, pos, mission = g.player()
        checks["load"] = {"ok": read_full and not mission, "slot1_read": read_full, "mission_running": mission,
                          "pos": pos}
        # delete slot 2 (the Game tab again: Load, New, Delete)
        keys("delete", "Return@3", "Right", "Right", "Shift@2", "Down", "Down", "Shift@3", "Down", "Down", "Shift@3",
             "Up", "Shift@8", "Shift@3", "F@2", "F@2", "F@2")
        order = open(os.path.join(folder, ".ps2x-order")).read().split("\n")
        left = [f for f in slot_files() if f.startswith("2 ")]
        checks["delete"] = {"ok": not left and not any(o.startswith("2 ") for o in order), "left": left}
    finally:
        g.quit()
    json.dump(checks, open(os.path.join(out, "results.json"), "w"), indent=1)
    for k, v in checks.items():
        print("%-7s %s %s" % (k, "ok" if v["ok"] else "FAIL", {x: y for x, y in v.items() if x != "ok"}))
    return 0 if checks and all(v["ok"] for v in checks.values()) and len(checks) == 3 else 1


if __name__ == "__main__":
    sys.exit(main())
