#!/usr/bin/env python3
"""Check of Enhancements > Saving: "Quick save" (docs/mods/QUICKSAVE.md), in hidden games.

    python tools/quicksave_check.py [OUT] [--port N] [--save DIR]

From a copy of the rig's card (build/rig/saves/fortbaxter or user: a save in the Fort Baxter barracks - never the live
card), with VCS_QUICKSAVE=1. One session:
  interior  at boot the player is in the barracks (safehouse 0, area 2): the key (F12) -> the Save menu -> slot 2
  foot      on foot in Vice Point -> slot 3; the clock does not move on (the safehouse save's 6 hours skipped)
  vehicle   in a car rolling at 15 m/s -> slot 4
  near      on foot 12 m from the car he got out of -> slot 5
  mission   the key while a mission runs: refused, no menu
  load-*    each slot through Load Game (pause menu > Game): back at its place, area, heading, with its vehicle (in it /
            next to it: the model; its colours do not fit in the save), the clock as saved
  normal    then a normal safehouse save (the barracks' save point code, main script 0x12213) -> slot 6: loads at the
            safehouse as before (no quick-save restore)
Then two fresh starts: the card without slot 6 (the newest is the "near" quick save: restored at boot) and with it
(slot 6, a normal save: no restore). Screenshots and OUT/results.json in OUT (default build/quicksave_check). Exit code 1
when a check fails. Menu keys (CONTROLS.md): Shift = Cross, Escape = Start, arrows = D-pad.
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import tour  # noqa: E402  (picture_stats)

ROOT = rig.ROOT
FOLDER = ("mc0", "BASLUS-21590GTA40001")
S = rig.Scm
WARP_CHAR_FROM_CAR_TO_COORD = 0x0216
STORE_CAR_CHAR_IS_IN_NO_SAVE = 0x024B
SET_CAR_FORWARD_SPEED = 0x02E9


class Session:
    def __init__(self, out, port):
        self.out, self.port, self.shots, self.g = out, port, 0, None

    def start(self, card, data):
        self.g = rig.Game.start(save=card, name="quicksave", data_dir=data, port=self.port,
                                env={"VCS_QUICKSAVE": "1", "PS2X_AUDIO": "0"})
        return self.g

    def shot(self, tag):
        self.shots += 1
        path = os.path.join(self.out, "%02d_%s.png" % (self.shots, tag))
        self.g.cmd("screenshot " + path)
        return path

    def visible(self, tag):
        stats = tour.picture_stats(self.shot(tag))
        return stats is None or stats[1] < 0.9

    def keys(self, tag, *seq):
        for k in seq:
            key, _, wait = k.partition("@")
            self.g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))
        self.shot(tag)

    def status(self):
        return dict(w.split("=", 1) for w in self.g.cmd("quicksave").split() if "=" in w)

    def glob(self, n, f="<i"):
        """script global n (ScriptSpace moves when a save is loaded from the menu: read its pointer)"""
        return struct.unpack(f, self.g.peek(self.g.u32(0x48741C) + 4 * n, 4))[0]

    def clock(self):
        h, m = self.g.peek(0x4CD148, 2)
        return h * 60 + m

    def ms(self):
        return self.g.u32(0x4CD104)

    def running(self, seconds=1.0):
        t = self.ms()
        time.sleep(seconds)
        return self.ms() != t

    def wait_menu(self, timeout=20.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.glob(4) == 1 and not self.running(1.0):
                time.sleep(1.5)
                return True
            time.sleep(0.25)
        return False

    def wait_saved(self, n, timeout=30.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            st = self.status()
            if int(st["saves"]) >= n and st["saving"] == "0":
                return st
            time.sleep(0.25)
        return self.status()

    def wait_game(self, timeout=120.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                f0 = int(self.g.cmd("frames"))
                if self.g.player()[0]:
                    time.sleep(1)
                    if int(self.g.cmd("frames")) > f0 + 10:
                        return True
            except Exception:
                pass
            time.sleep(1)
        return False

    def state(self):
        """where the player is: position, heading, area, the vehicle he sits in (model, colours) or None"""
        g = self.g
        ped = g.player()[0]
        veh = g.u32(ped + 0x480)
        at = veh or ped
        pos = struct.unpack("<3f", g.peek(at + 0x30, 12))
        fx, fy = struct.unpack("<2f", g.peek(at + 0x10, 8))
        heading = math.degrees(math.atan2(-fx, fy)) % 360
        car = None
        if veh:
            car = {"model": struct.unpack("<H", g.peek(veh + 0x56, 2))[0], "colours": g.peek(veh + 0x224, 8).hex()}
        return {"pos": [round(v, 2) for v in pos], "heading": round(heading, 1), "area": g.area(), "car": car,
                "g171": self.glob(171), "g172": self.glob(172)}

    def cars_near(self, pos, radius=20.0):
        """vehicles of the pool within `radius` of pos: (model, colours, distance)"""
        g = self.g
        pool = g.u32(0x487A98)
        base, flags, size = g.u32(pool), g.u32(pool + 4), g.u32(pool + 8)
        fl = g.peek(flags, size)
        out = []
        for i in range(size):
            if fl[i] & 0x80:
                continue
            v = base + i * 0x8C0
            p = struct.unpack("<3f", g.peek(v + 0x30, 12))
            d = math.hypot(p[0] - pos[0], p[1] - pos[1])
            if d <= radius:
                out.append({"model": struct.unpack("<H", g.peek(v + 0x56, 2))[0], "colours": g.peek(v + 0x224, 8).hex(),
                            "dist": round(d, 1), "z": round(p[2], 2)})
        return out

    def vehicle_logs(self):
        """the game's '[quicksave] the vehicle at z ...' lines so far: the height it was given on a load"""
        try:
            log = os.path.join(os.environ.get("VCS_BUILD_DIR") or os.path.join(ROOT, "build"), "run", "quicksave.log")
            return [l.strip() for l in open(log, errors="replace") if "[quicksave] the vehicle at z" in l]
        except OSError:
            return []

    def quick_save(self, tag, slot):
        """the key -> the Save menu -> slot `slot` (an empty one) -> Yes -> OK"""
        n = int(self.status()["saves"])
        self.g.cmd("press F12 150")
        menu = self.wait_menu()
        self.shot(tag + "_menu")
        self.keys(tag + "_saved", "Shift@3", *(["Down"] * slot), "Shift@3", "Up", "Shift@12", "Shift@4")
        st = self.wait_saved(n + 1)
        time.sleep(2)
        return menu, st

    def load_slot(self, tag, slot):
        """pause menu > Game > Load Game > slot > Yes; then until INITSAV and the restore are done"""
        self.keys(tag, "Escape@3", "Right", "Right", "Shift@2", "Shift@3", *(["Down"] * slot), "Shift@3", "Up", "Shift@15")
        self.wait_game()
        time.sleep(12)


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "quicksave_check"))
    ap.add_argument("--port", type=int, default=47701)
    ap.add_argument("--save", default=rig.default_save())
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    data = os.path.join(out, "data")
    folder = os.path.join(data, *FOLDER)
    s = Session(out, a.port)
    checks = {}
    saved = {}

    def slot_files():
        return sorted(f for f in os.listdir(folder) if f[:1].isdigit())

    try:
        g = s.start(a.save, data)
        g.wait_ingame()
        st = s.status()
        checks["setting"] = {"ok": st["hooked"] == "1" and st["on"] == "1", "status": st}

        # 1. in the barracks (safehouse 0's interior)
        before = s.state()
        menu, st = s.quick_save("interior", 2)
        saved[2] = before
        checks["save_interior"] = {"ok": menu and before["area"] != 0 and before["g171"] == 0 and
                                   any(f.startswith("2 ") for f in slot_files()) and s.visible("interior_after"),
                                   "state": before, "status": st}

        # 2. on foot in Vice Point: the clock
        g.teleport(470.0, 900.0, -100.0)
        time.sleep(4)
        before = s.state()
        c0 = s.clock()
        menu, st = s.quick_save("foot", 3)
        c1 = s.clock()
        saved[3] = dict(before, clock=c0)
        checks["save_foot"] = {"ok": menu and 0 <= c1 - c0 <= 10 and st["clock_skips"] != "0" and
                               any(f.startswith("3 ") for f in slot_files()) and s.visible("foot_after"),
                               "state": before, "clock_before": c0, "clock_after": c1, "status": st}

        # 3. in a car rolling at 15 m/s
        g.teleport(300.0, 200.0, -100.0)
        time.sleep(3)
        g.spawn_car()
        time.sleep(3)
        g.scm(S.op(STORE_CAR_CHAR_IS_IN_NO_SAVE, S.PLAYER, S.LOCAL0) + S.op(SET_CAR_FORWARD_SPEED, S.LOCAL0, S.f(15.0)))
        time.sleep(0.5)
        menu, st = s.quick_save("vehicle", 4)
        after = s.state()  # the car stopped where the save was made: that is the saved place
        saved[4] = after
        checks["save_vehicle"] = {"ok": menu and after["car"] is not None and any(f.startswith("4 ") for f in slot_files()),
                                  "state": after, "status": st}

        # 4. on foot next to the car he got out of
        car_pos = after["pos"]
        g.scm(S.op(WARP_CHAR_FROM_CAR_TO_COORD, S.PLAYER, S.f(car_pos[0] + 12.0), S.f(car_pos[1]), S.f(-100.0)))
        time.sleep(3)
        before = s.state()
        near = s.cars_near(car_pos, 3.0)
        menu, st = s.quick_save("near", 5)
        saved[5] = dict(before, near_car=near, car_pos=car_pos)
        checks["save_near"] = {"ok": menu and before["car"] is None and bool(near) and
                               any(f.startswith("5 ") for f in slot_files()), "state": before, "near": near, "status": st}

        # 6. each quick save through Load Game
        for slot, tag in ((3, "foot"), (4, "vehicle"), (5, "near"), (2, "interior")):
            logs = len(s.vehicle_logs())
            s.load_slot("load_" + tag, slot)
            now = s.state()
            want = saved[slot]
            st = s.status()
            # the place at the key press: a rolling car still moves a few metres until it stops
            tol = 10.0 if tag in ("vehicle", "near") else 1.5
            ok = close(now["pos"][:2], want["pos"][:2], tol) and abs(now["pos"][2] - want["pos"][2]) < 1.5 and \
                now["area"] == want["area"] and not g.player()[2]
            extra = {}
            if tag == "foot":
                extra = {"clock_saved": want["clock"], "clock_loaded": s.clock()}
                # minutes of game time since the key press (a game minute is a second), not the safehouse's 6 hours
                ok = ok and 0 <= s.clock() - want["clock"] <= 60 and now["car"] is None
            elif tag == "vehicle":
                # the model (the colours do not fit in the save: the game picks them)
                # and at its height: given it at once (no drop from 1 m up), still there once settled
                given = s.vehicle_logs()[logs:]
                extra = {"given": given}
                ok = ok and now["car"] is not None and now["car"]["model"] == want["car"]["model"] and                     abs(now["pos"][2] - want["pos"][2]) < 0.3 and len(given) == 1 and                     abs(float(given[0].split()[5]) - want["pos"][2]) < 0.3
            elif tag == "near":
                cars = s.cars_near(want["car_pos"], 3.0)
                given = s.vehicle_logs()[logs:]
                extra = {"cars_at_saved_car_pos": cars, "given": given}
                ok = ok and now["car"] is None and len(given) == 1 and                     abs(float(given[0].split()[5]) - want["near_car"][0]["z"]) < 0.3 and                     any(c["model"] == want["near_car"][0]["model"] and abs(c["z"] - want["near_car"][0]["z"]) < 0.3
                        for c in cars)
            elif tag == "interior":
                ok = ok and now["g171"] == 0
            checks["load_" + tag] = {"ok": ok and s.visible("load_%s_screen" % tag), "now": now, "saved": want,
                                     "status": st, **extra}

        # 7. a normal safehouse save afterwards (the barracks' save point code) -> slot 6
        g.cmd("quicksave savepoint")  # the save point's code as a script of its own (test command)
        menu = s.wait_menu()
        s.keys("normal_saved", "Shift@3", *(["Down"] * 6), "Shift@3", "Up", "Shift@12", "Shift@4")
        time.sleep(4)
        new6 = [f for f in slot_files() if f.startswith("6 ")]
        checks["normal_save"] = {"ok": menu and bool(new6) and s.glob(4) == 0, "new": new6}
        # 5. refused during a mission (after the loads: the mission is left running)
        g.mission(44)
        time.sleep(8)
        n = int(s.status()["refused"])
        g.cmd("press F12 150")
        time.sleep(3)
        st = s.status()
        checks["mission_refused"] = {"ok": int(st["refused"]) == n + 1 and st["last_refusal"] == "mission" and
                                     s.glob(4) == 0 and s.running(), "status": st}

        g.quit()

        # 8. fresh starts: without slot 6 (the newest is the "near" quick save) and with it
        for tag, drop6 in (("boot_quick", True), ("boot_normal", False)):
            card = os.path.join(out, "card_" + tag)
            rig.copy_card(os.path.join(data, "mc0"), os.path.join(card, "mc0"))
            if drop6:
                cf = os.path.join(card, *FOLDER)
                for f in os.listdir(cf):
                    if f.startswith("6 "):
                        os.remove(os.path.join(cf, f))
                order = os.path.join(cf, ".ps2x-order")
                lines = [l for l in open(order).read().split("\n") if not l.startswith("6 ")]
                open(order, "w", newline="\n").write("\n".join(lines))
            g = s.start(card, os.path.join(out, "data_" + tag))
            s.wait_game()
            time.sleep(14)
            now = s.state()
            st = s.status()
            if drop6:
                want = saved[5]
                ok = close(now["pos"][:2], want["pos"][:2], 6.0) and abs(now["pos"][2] - want["pos"][2]) < 1.5 and                     st["restores"] == "1"
            else:
                ok = st["restores"] == "0" and now["area"] != 0 and now["g171"] == 0  # the barracks, as a normal save
            checks[tag] = {"ok": ok and s.visible(tag), "now": now, "status": st}
            g.quit()
    except Exception as e:
        checks["error"] = {"ok": False, "what": repr(e)}
    finally:
        if s.g:
            s.g.quit()
    json.dump(checks, open(os.path.join(out, "results.json"), "w"), indent=1)
    for k, v in checks.items():
        print("%-16s %s %s" % (k, "ok" if v["ok"] else "FAIL", {x: y for x, y in v.items() if x != "ok"}))
    return 0 if checks and all(v["ok"] for v in checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
