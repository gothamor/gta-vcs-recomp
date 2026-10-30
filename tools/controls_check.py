#!/usr/bin/env python3
"""Controls and player settings check (docs/CONTROLS.md, src/game/vcs_input.cpp): the bindings, checked in the pad
report they make and in the game, plus the settings registry's presets and reset, with menu screenshots.

    python tools/controls_check.py [OUT] [--port 47673]

Loads the Fort Baxter rig save (a copy, hidden), then: every default key binding (both slots, either Shift / Ctrl,
Keypad Enter, the mouse buttons) gives its PS2 button or stick direction, Alt+Enter is not Start; a rebound key moves
Vic and the old one no longer does; the walk key holds the stick at walking speed; the mouse turns the on-foot camera
by the amount asked (heading and pitch, inverted, off), right click locks on to a ped, the Driving keys (W / S / Space
accelerate, brake, handbrake in a car, and nothing else there), the mouse on the vehicle camera, on foot again after
leaving the car; a mouse wheel binding; a menu key
refuses a key a control has; the presets and Reset all settings; screenshots of the settings menu's pages; Audio >
Music / Sound effects are the game's own levels (set here, shown in its pause menu, changed there, kept by a save
load) (src/game/vcs_volume.cpp). Results: OUT/summary.txt; screenshots OUT/*.png. Default
OUT: build/controls/<date-time>.
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

    def keys(self, *seq):
        """Key presses, KEY[@seconds to wait after] (the game's menus take their time)"""
        for k in seq:
            key, _, wait = k.partition("@")
            self.g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))

    def sound(self):
        """soundstate: the game's music / effects levels (0..127)."""
        self.g.wait_frames(3)
        return dict(re.findall(r"(\w+)=([0-9/]+)", self.g.cmd("soundstate")))

    def sound_levels(self):
        """Audio > Music / Sound effects are the game's own levels (src/game/vcs_volume.cpp)."""
        g = self.g
        s = self.sound()
        self.ok("sound levels at the defaults", s.get("music") == "80" and s.get("effects") == "112", str(s))
        self.setting("audio.music_volume", 25)
        self.setting("audio.sfx_volume", 50)
        s = self.sound()
        prefs = g.peek(g.u32(0x48F3D0) + 0x38, 8).hex()  # the menu's prefs: effects, music (u32)
        self.ok("music / effects settings reach the game", s.get("music") == "32" and s.get("effects") == "64" and
                prefs == "4000000020000000", "%s prefs %s" % (s, prefs))
        # loading the card's save (it holds the defaults) keeps the player's levels: pause menu > Game > Load, slot 1
        self.keys("Return@3", "Right", "Right", "Shift@2", "Shift@3", "Down", "Shift@3", "Up", "Shift@25")
        s = self.sound()
        with open(g.log_path, encoding="utf-8", errors="replace") as f:
            loaded = "the game sets the music level 80 (ra=0x3add90): the player's 32" in f.read()
        self.ok("a save load keeps the player's levels", loaded and s.get("music") == "32" and s.get("effects") == "64",
                "%s, load %s" % (s, "seen" if loaded else "not seen"))
        # the pause menu's Audio page shows them (2 and 4 bars) and its slider changes the setting
        self.keys("Return@3", "Down@2", "Right@2")
        self.shot("game_audio_page")
        self.keys("Shift@2", "Left@2")
        s = self.sound()
        music = self.setting("audio.music_volume")
        self.shot("game_audio_music_down")
        self.ok("the pause menu's music slider changes the setting", s.get("music") == "16" and music == "13",
                "%s, setting %s" % (s, music))
        self.keys("f", "f@2")  # Triangle: back, close
        self.setting("audio.music_volume", 63)
        self.setting("audio.sfx_volume", 88)

    CAM = 0x6F44D0 + 0x70  # TheCamera's active CCam: +0 mode (4 on foot, 11 locked on, 18 vehicle), +0x78 pitch, +0x7C heading

    def cam(self):
        """(mode, pitch, heading) of the active camera."""
        pitch, heading = self.g.peekf(self.CAM + 0x78, 2)
        return self.g.u32(self.CAM), pitch, heading

    def mouse_moves(self, n, dx, dy):
        """Mouse motion in n small moves (as a real mouse sends it); (heading, pitch) turned once the camera settled."""
        _, p0, h0 = self.cam()
        for _ in range(n):
            self.g.cmd("mouse %d %d" % (dx, dy))
        self.g.wait_frames(20)
        _, p1, h1 = self.cam()
        return (h1 - h0 + math.pi) % (2 * math.pi) - math.pi, p1 - p0

    def mouse_and_driving(self):
        """The mouse camera on foot (heading written in the game, pitch through the stick), right-click lock-on, and the
        Driving keys (W / S / Space as Cross / Square / R1 while driving only), checked in the camera and the car."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)  # a straight street (the camera starts at its lowest here)
        time.sleep(4.0)
        g.cmd("mouse 0 -150")
        g.wait_frames(30)
        mode = self.cam()[0]
        turn, up = self.mouse_moves(30, 10, -3)  # 400 counts a radian at 100%: -0.75 heading (right), +0.225 pitch (up)
        self.ok("mouse turns the camera on foot", mode == 4 and abs(turn + 0.75) < 0.03 and abs(up - 0.225) < 0.02,
                "mode %d heading %+.3f pitch %+.3f" % (mode, turn, up))
        self.setting("mouse.invert_y", 1)
        _, up = self.mouse_moves(10, 0, -4)
        self.setting("mouse.invert_y", 0)
        self.ok("invert mouse up / down", abs(up + 0.1) < 0.01, "pitch %+.3f" % up)
        self.setting("mouse.camera", 0)
        turn, up = self.mouse_moves(10, 20, 5)
        self.setting("mouse.camera", 1)
        self.ok("mouse camera off", abs(turn) < 0.005 and abs(up) < 0.005, "heading %+.3f pitch %+.3f" % (turn, up))
        self.keys("Return@3")  # the game's pause menu: the camera stays
        _, _, h0 = self.cam()
        g.cmd("mouse 400 0")
        time.sleep(1.0)
        _, _, h1 = self.cam()
        self.keys("Return@3")
        self.ok("the mouse leaves the camera in the pause menu", abs(h1 - h0) < 0.001, "heading %.3f -> %.3f" % (h0, h1))

        # right click locks on to a ped next to Vic (the game's lock-on camera, mode 11), and the mouse does not break it
        _, (x, y, _), _ = g.player()
        g.scm(S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x + 2.0), S.f(y + 2.0), S.f(-100.0), S.LOCAL0))
        time.sleep(1.5)
        g.cmd("key mouseright down")
        time.sleep(1.0)
        locked = self.cam()[0]
        g.cmd("mouse 400 0")
        time.sleep(0.5)
        still = self.cam()[0]
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.ok("right click locks on", locked == 11 and still == 11 and self.cam()[0] == 4,
                "held %d, mouse moved %d, released %d" % (locked, still, self.cam()[0]))

        # driving: the Driving keys, and nothing else on those keys
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        g.spawn_car(ahead=0.0)
        time.sleep(3.0)
        ped = g.player()[0]
        car = g.u32(ped + 0x480)
        self.ok("in a car", car != 0 and g.u32(ped + 0x8C4) == 0x37 and self.cam()[0] == 18, "car 0x%x mode %d" % (car, self.cam()[0]))
        got = {k: self.held(k) for k in ("w", "s", "space", "a", "shift", "mouseright")}
        want = {"w": CROSS, "s": SQUARE, "space": R1, "a": 0, "shift": CROSS, "mouseright": R1}
        bad = ["%s -> %s" % (k, p) for k, p in got.items() if p["buttons"] != want[k] or p["ly"] != 128 or
               p["lx"] != (0 if k == "a" else 128)]
        self.ok("driving keys", not bad, "; ".join(bad))
        self.setting("keys.drive_lean_forward", KEY_G)
        p = self.held("g")
        self.setting("keys.drive_lean_forward", 0)
        self.ok("lean forward rebound", p["ly"] == 0 and p["buttons"] == 0, str(p))

        def forward():
            v, f = g.peekf(car + 0x140, 3), g.peekf(car + 0x10, 3)
            return sum(a * b for a, b in zip(v, f))
        g.cmd("key w down")
        time.sleep(2.0)
        fast = forward()
        g.cmd("key w up")
        g.cmd("key space down")
        time.sleep(1.6)
        stopped = forward()
        g.cmd("key space up")
        self.ok("W drives, Space is the handbrake", fast > 0.15 and abs(stopped) < 0.05, "%.3f then %.3f" % (fast, stopped))
        _, _, h0 = self.cam()
        for _ in range(20):  # the vehicle camera turns at the stick's speed: a steady movement over 0.6 s
            g.cmd("mouse 20 0")
            time.sleep(0.03)
        g.wait_frames(20)
        _, _, h1 = self.cam()
        self.ok("mouse turns the vehicle camera", abs(h1 - h0) > 0.1, "heading %.3f -> %.3f" % (h0, h1))

        # out of the car: the on-foot keys and the mouse look again
        g.cmd("key space down")
        time.sleep(1.0)
        g.cmd("key space up")
        g.cmd("press f 300")
        t0 = time.time()
        while time.time() - t0 < 8 and (g.u32(ped + 0x8C4) == 0x37 or self.cam()[0] != 4):
            time.sleep(0.25)
        time.sleep(3.0)  # the camera's move back behind Vic sets its heading itself meanwhile
        turn, _ = self.mouse_moves(30, 10, 0)
        p = self.held("w")
        self.ok("on foot again", self.cam()[0] == 4 and abs(turn + 0.75) < 0.03 and p["ly"] == 0 and p["buttons"] == 0,
                "mode %d heading %+.3f, W %s" % (self.cam()[0], turn, p))

    def run(self):
        g = self.g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="controls_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "controls", "data"),
                                    env={"VCS_FPS": "30", "VCS_TIMING": ""})  # timing unlocked: the presets set it
        try:
            g.wait_ingame()
            # the player's own vcs.ini is read by hidden runs too (never written): start from the defaults
            self.ok("start from the defaults", self.setting("advanced.reset_all") == "pressed")
            g.prime()
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

            self.mouse_and_driving()

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
            # the sound levels with their save load here, after play in the city with god mode on: the load used to hang
            # on "Loading..." (the trainer cancelled a script the load had started in its slot, game-internals.md 11d)
            self.sound_levels()

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
