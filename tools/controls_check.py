#!/usr/bin/env python3
"""Controls and player settings check (docs/CONTROLS.md, src/game/vcs_input.cpp): the bindings, checked in the pad
report they make and in the game, plus the settings registry's presets and reset, with menu screenshots.

    python tools/controls_check.py [OUT] [--port 47673]

Loads the Fort Baxter rig save (a copy, hidden), then: every default key binding (both slots, either Shift / Ctrl,
Keypad Enter, the mouse buttons) gives its PS2 button or stick direction, Alt+Enter is not Start; a rebound key moves
Vic and the old one no longer does; the walk key holds the stick at walking speed; a mouse wheel binding; a menu key
refuses a key a control has; the presets and Reset all settings; screenshots of the settings menu's pages. Results:
OUT/summary.txt; screenshots OUT/*.png. Default OUT: build/controls/<date-time>.
"""
import argparse
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# DualShock 2 bits (pressed), as "padstate" reports them
SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT = 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080
L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE = 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
KEY_G, KEY_SHIFT, KEY_WHEEL_UP, KEY_F2 = 71, 340, 405, 291

# control-channel key name -> what the default bindings make of it: a button bit, or (axis, byte)
DEFAULTS = [
    ("w", ("ly", 0)), ("s", ("ly", 255)), ("a", ("lx", 0)), ("d", ("lx", 255)),
    ("i", ("ry", 0)), ("k", ("ry", 255)), ("j", ("rx", 0)), ("l", ("rx", 255)),
    ("shift", CROSS), ("rshift", CROSS), ("space", SQUARE), ("f", TRIANGLE), ("ctrl", CIRCLE), ("rctrl", CIRCLE),
    ("mouseleft", CIRCLE), ("q", L1), ("e", R1), ("mouseright", R1), ("1", L2), ("z", L2), ("3", R2), ("x", R2),
    ("c", L3), ("v", R3), ("backspace", SELECT),
    ("up", UP), ("down", DOWN), ("left", LEFT), ("right", RIGHT),
]
# Start opens the game's pause menu: checked last
START_KEYS = [("return", START), ("kpenter", START)]


class Check:
    def __init__(self, out, port):
        self.out = out
        self.port = port
        self.results = []
        self.g = None

    def ok(self, name, cond, detail=""):
        self.results.append((name, bool(cond), detail))
        print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail), flush=True)
        return cond

    def pad(self):
        """The last pad report: {'buttons': int, 'lx': .., 'ly': .., 'rx': .., 'ry': ..}, after a few game frames."""
        self.g.wait_frames(3)
        line = self.g.cmd("padstate")
        d = dict(re.findall(r"(\w+)=([0-9a-f]+)", line))
        return {k: int(v, 16) if k == "buttons" else int(v) for k, v in d.items()}

    def held(self, *keys):
        for k in keys:
            self.g.cmd("key %s down" % k)
        try:
            return self.pad()
        finally:
            for k in keys:
                self.g.cmd("key %s up" % k)

    def setting(self, key, value=None):
        r = self.g.cmd("setting %s%s" % (key, "" if value is None else " %s" % value))
        return r.split(" ", 1)[1] if r.startswith("ok ") else r

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace("\\", "/"))

    def moved(self, key, seconds=1.5):
        """How far Vic moves while `key` is held."""
        before = self.g.player()[1]
        self.g.cmd("key %s down" % key)
        time.sleep(seconds)
        self.g.cmd("key %s up" % key)
        time.sleep(0.5)
        after = self.g.player()[1]
        return math.hypot(after[0] - before[0], after[1] - before[1])

    def run(self):
        g = self.g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="controls_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "controls", "data"),
                                    env={"VCS_FPS": "30"})
        try:
            g.wait_ingame()
            g.prime()
            # the player's own vcs.ini is read by hidden runs too (never written): start from the defaults
            self.ok("start from the defaults", self.setting("advanced.reset_all") == "pressed")
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(2)
            idle = self.pad()
            self.ok("idle report", idle.get("buttons") == 0 and all(idle.get(a) in (127, 128) for a in ("lx", "ly", "rx", "ry")), str(idle))

            # every default binding
            bad = []
            for key, want in DEFAULTS:
                p = self.held(key)
                if isinstance(want, int):
                    good = p["buttons"] == want
                else:
                    good = p[want[0]] == want[1] and p["buttons"] == 0
                if not good:
                    bad.append("%s -> %s" % (key, p))
            self.ok("default bindings (%d keys)" % len(DEFAULTS), not bad, "; ".join(bad))
            p = self.held("w", "d", "shift", "e")
            self.ok("keys together", p["ly"] == 0 and p["lx"] == 255 and p["buttons"] == CROSS | R1, str(p))

            # the game sees them: Vic runs forward
            far = self.moved("w")
            self.ok("W moves Vic", far > 1.0, "%.2f m" % far)

            # rebinding: forward on G, W does nothing then
            self.setting("keys.move_up", KEY_G)
            far_g = self.moved("g")
            far_w = self.moved("w")
            self.ok("rebound key moves Vic, the old one doesn't", far_g > 1.0 and far_w < 0.2, "G %.2f m, W %.2f m" % (far_g, far_w))
            self.setting("keys.move_up", 87)
            self.setting("keys.cross", KEY_G)
            p_g, p_shift = self.held("g"), self.held("shift")
            self.ok("Cross rebound", p_g["buttons"] == CROSS and p_shift["buttons"] == 0, "G %s, Shift %s" % (p_g, p_shift))
            self.setting("keys.cross", KEY_SHIFT)

            # walk: the stick at the walking speed while held
            self.setting("keys.walk", KEY_G)
            p = self.held("w", "g")
            want = round(128 - 0.45 * 128)
            self.ok("walk key", abs(p["ly"] - want) <= 1, "ly %d (want %d)" % (p["ly"], want))
            walk = self.setting("keys.walk", 0)
            self.ok("walk unbound", walk == "0", walk)

            # a wheel notch as L1 (second slot); the menu key keeps F2 (the menu refuses keys a control has)
            self.setting("keys.l12", KEY_WHEEL_UP)
            p = self.held("wheelup")
            self.ok("wheel binding", p["buttons"] == L1, str(p))
            self.setting("keys.l12", 0)

            # settings: presets and reset
            self.setting("presets.enhanced")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing")}
            self.ok("Enhanced preset", got == {"display.aspect": "0", "display.hud": "2", "display.msaa": "4", "display.timing": "1"}, str(got))
            self.setting("presets.authentic")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing", "display.fps")}
            self.ok("Authentic preset", got == {"display.aspect": "43", "display.hud": "0", "display.msaa": "1", "display.timing": "0",
                                                "display.fps": "30"}, str(got))
            self.setting("keys.cross", KEY_G)
            self.setting("audio.volume", 40)
            self.setting("pad.vibration", 0)
            self.setting("advanced.reset_all")
            got = {k: self.setting(k) for k in ("keys.cross", "audio.volume", "pad.vibration", "controls.menu_key")}
            self.ok("Reset all settings", got == {"keys.cross": str(KEY_SHIFT), "audio.volume": "100", "pad.vibration": "100",
                                                  "controls.menu_key": str(KEY_F2)}, str(got))
            p = self.held("shift")
            self.ok("bindings after the reset", p["buttons"] == CROSS, str(p))

            # the settings menu's pages
            for page in ("Display", "Graphics", "Audio", "Game", "Keyboard", "Gamepad", "Advanced"):
                g.cmd("menu " + page)
                g.wait_frames(4)
                self.shot("menu_" + page.lower())
                if page == "Keyboard":  # its later sections
                    g.cmd("menu scroll 0.6")
                    g.wait_frames(4)
                    self.shot("menu_keyboard_lower")
            g.cmd("menu close")
            p = self.held("shift")
            self.ok("input back after the menu", p["buttons"] == CROSS, str(p))

            # vibration: an explosion next to Vic (the game shakes the pad; god mode so he lives)
            g.cmd("trainer toggle god 1")
            x, y, z = g.player()[1]
            g.scm(rig.Scm.op(0x013C, rig.Scm.f(x + 2.0), rig.Scm.f(y), rig.Scm.f(z), rig.Scm.i(0)))
            time.sleep(2.0)
            with open(g.log_path, encoding="utf-8", errors="replace") as f:
                log = f.read()
            m = re.search(r"\[input\] the game vibrates the pad \(small (\d+), large (\d+)\)", log)
            self.ok("the game's vibration reaches the host", m is not None, m.group(0) if m else "no vibration logged")
            self.ok("player bindings in use", "[input] player bindings" in log)

            # Start last (it opens the pause menu); Alt+Enter is full screen, not Start
            p = self.held("alt", "return")
            self.ok("Alt+Enter is not Start", not p["buttons"] & START, str(p))
            for key, want in START_KEYS:
                p = self.held(key)
                self.ok("%s is Start" % key, p["buttons"] == want, str(p))
                time.sleep(1.0)
            self.ok("still running", g.alive())
        finally:
            g.quit()
        fails = [r for r in self.results if not r[1]]
        with open(os.path.join(self.out, "summary.txt"), "w", encoding="utf-8") as f:
            for name, good, detail in self.results:
                f.write("%-4s %s %s\n" % ("ok" if good else "FAIL", name, detail))
            f.write("%d checks, %d failed\n" % (len(self.results), len(fails)))
        print("%d checks, %d failed -> %s" % (len(self.results), len(fails), self.out))
        return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int, default=47673)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "controls", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    sys.exit(0 if Check(out, a.port).run() else 1)


if __name__ == "__main__":
    main()
