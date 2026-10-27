#!/usr/bin/env python3
"""Frame-rate dependence audit: the same scripted scenarios at forced time steps, compared per simulated second.

    python tools/framerate_audit.py [OUT] [--steps 15,30,60,100] [--only fall,run,...]

Each step N is its own game run in frame-rate independent timing with VCS_TIMESTEP=N: CTimer is given exactly 1/N s
per frame (in whole milliseconds, carried) and the game runs unpaced, so behaviour can be compared per simulated
second at the equivalent of any frame rate on this machine. Simulated time is the game's own clock
(CTimer::m_snTimeInMilliseconds, 0x4CD104). Input is injected over the control channel (PS2X_INPUT=injected: the
keyboard / pad are ignored). From a copy of the Fort Baxter card; noon, sunny, invincible.

Scenarios (each from a fresh teleport, so earlier ones don't leak into later ones):
  fall    the player dropped from 30 m above the ground: time to fall 15 m, where he lands
  run     W held for 4 s: speed over the last 3 s
  sprint  W + Cross held for 4 s: speed over the last 3 s
  jump    Square on foot: rise and airtime
  car     in a car (model 211), Cross held for 4 s: speed at 1 / 2 / 3 / 4 s (the car's move speed), distance
  clock   game clock ms per frame vs the step; frames per simulated second
  heat    PHI_A2's heat bar: its floor and the heat at +1 a frame, per simulated second (starts the mission: last)

    python tools/framerate_audit.py --sites

checks instead that every per-frame mission meter src/game/vcs_script_timing.cpp puts on the time step is found where
its table says (a game per mission, VCS_SCRIPT_TIMESTEP_CHECK=1).

Prints, per scenario and metric, each step's value and its difference from the 30 fps result (the original frame
rate), flagging differences over --tolerance (default 5%). Writes OUT/audit.json (OUT: build/audit/<date-time>).
"""
import argparse
import datetime
import json
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
CLOCK_MS = 0x4CD104      # CTimer::m_snTimeInMilliseconds
PED_VEHICLE = 0x450      # ped -> its vehicle (found by scanning the ped for the car's address, game-internals.md 12)
MOVE_SPEED = 0x140       # CPhysical move speed (x, y, z), units per 1/50 s (z found mid-fall at +0x148)
FRAME_COUNTER = 0x4CD124  # CTimer::m_FrameCounter
# open ground for the on-foot and car runs (spots from the world tour, checked by screenshot): x, y, heading - a long
# straight pavement beside the open tarmac near the airport (the Fort Baxter door faces a fence; -675,-75 is sea)
FOOT_SPOT = (-1425.0, -575.0, 90.0)
CAR_SPOT = (-1425.0, -640.0, 0.0)  # a new car faces north; open for 4 s of full throttle here (scratch probe)


class Session:
    def __init__(self, step, name, port):
        env = {"VCS_ASPECT": "43", "VCS_TIMING": "1", "VCS_TIMESTEP": str(step), "VCS_FPS": "0", "PS2X_INPUT": "injected"}
        self.step = step
        self.in_car = False
        self.g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name=name, port=port,
                                data_dir=os.path.join(ROOT, "build", "audit", "data_%d" % step), env=env)
        g = self.g
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        g.prime()

    def clock(self):
        return self.g.u32(CLOCK_MS) / 1000.0

    def pos(self):
        """The player's position - the vehicle's while in one (the ped's own matrix isn't its place then)."""
        ped, p, _ = self.g.player()
        vehicle = self.g.u32(ped + PED_VEHICLE) if self.in_car else 0
        if vehicle:
            return tuple(self.g.peekf(vehicle + 0x30, 3))
        return p

    def sample(self, seconds, every=0.0):
        """[(sim time, (x, y, z))] for `seconds` of simulated time from now."""
        out = []
        t0 = self.clock()
        while True:
            t = self.clock()
            out.append((t - t0, self.pos()))
            if t - t0 >= seconds:
                return out
            if every:
                time.sleep(every)

    def wait_sim(self, seconds):
        t0 = self.clock()
        while self.clock() - t0 < seconds:
            time.sleep(0.01)

    def key(self, name, down):
        self.g.cmd("key %s %s" % (name, "down" if down else "up"))

    def place(self, x, y, z=-100.0, heading=0.0):
        self.g.teleport(x, y, z, heading=heading)
        self.wait_sim(1.5)

    def quit(self):
        self.g.quit()


def horizontal_speed(samples, t_from, t_to):
    """Average horizontal speed (m/s) between two simulated times (nearest samples)."""
    a = min(samples, key=lambda s: abs(s[0] - t_from))
    b = min(samples, key=lambda s: abs(s[0] - t_to))
    if b[0] <= a[0]:
        return 0.0
    return math.hypot(b[1][0] - a[1][0], b[1][1] - a[1][1]) / (b[0] - a[0])


def scenario_fall(s):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    ground = s.pos()[2]
    s.g.teleport(x, y, ground + 30.0, heading=h)
    samples = s.sample(5.0)
    z0 = samples[0][1][2]
    t15 = None
    for (ta, pa), (tb, pb) in zip(samples, samples[1:]):
        if pa[2] > z0 - 15.0 >= pb[2]:
            t15 = ta + (tb - ta) * (pa[2] - (z0 - 15.0)) / max(pa[2] - pb[2], 1e-6)
            break
    return {"fall_15m_s": t15, "landed_z_above_ground": samples[-1][1][2] - ground}


def scenario_foot(s, keys):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    for k in keys:
        s.key(k, True)
    try:
        if os.environ.get("AUDIT_SHOTS"):
            s.wait_sim(1.5)
            for i in range(3):
                s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "foot_%d_%d.png" % (s.step, i)))
        samples = s.sample(4.0)
    finally:
        for k in keys:
            s.key(k, False)
    return {"speed_m_s": horizontal_speed(samples, 1.0, 4.0)}


def scenario_jump(s):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    z0 = s.pos()[2]
    s.key("Space", True)
    samples = s.sample(2.5)
    s.key("Space", False)
    top = max(p[2] for _, p in samples)
    airborne = [t for t, p in samples if p[2] > z0 + 0.05]
    return {"rise_m": top - z0, "airtime_s": (airborne[-1] - airborne[0]) if len(airborne) > 1 else 0.0}


def scenario_car(s):
    x, y, h = CAR_SPOT
    s.place(x, y, heading=h)
    S = rig.Scm
    _, (px, py, pz), _ = s.g.player()
    s.g.scm(S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
            S.op(S.CREATE_CAR, S.i(211), S.f(px), S.f(py), S.f(pz + 1.0), S.LOCAL0) +
            S.op(S.SET_CAR_PROOFS, S.LOCAL0, S.i(1), S.i(1), S.i(1), S.i(1), S.i(1)) +
            S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.LOCAL0) + S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)))
    s.wait_sim(1.0)
    vehicle = s.g.u32(s.g.player()[0] + PED_VEHICLE)
    if not vehicle:
        raise RuntimeError("not in the car")
    s.in_car = True
    s.wait_sim(2.0)
    if os.environ.get("AUDIT_SHOTS"):
        s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "car_%d_a.png" % s.step))
    s.key("Shift", True)
    speeds = []  # (sim time, m/s from the car's own move speed - positions read apart from the clock are too noisy)
    try:
        t0 = s.clock()
        while True:
            t = s.clock() - t0
            v = s.g.peekf(vehicle + MOVE_SPEED, 3)
            speeds.append((t, math.hypot(v[0], v[1]) * 50.0))
            if t >= 4.0:
                break
    finally:
        if os.environ.get("AUDIT_SHOTS"):
            s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "car_%d_b.png" % s.step))
        s.key("Shift", False)
    s.in_car = False

    def at(t):
        return min(speeds, key=lambda x: abs(x[0] - t))[1]

    # distance by integrating the speed
    distance = sum((b[0] - a[0]) * (a[1] + b[1]) / 2 for a, b in zip(speeds, speeds[1:]))
    return {"speed_1s": at(1.0), "speed_2s": at(2.0), "speed_3s": at(3.0), "speed_4s": at(4.0), "distance_4s": distance}


def scenario_heat(s):
    """PHI_A2 "Boomshine Blowout": the heat's floor (+0.015 a frame in the script) and the heat at a rate of +1 a frame,
    per simulated second (0.45 / 30 at the PS2's 30 fps). The mission is walked to its forklift state: into Phil's
    truck, then to the warehouse's back (the state-2 marker); in state 4 the heat routine runs every frame and the
    rate $G15AC is only recomputed once the player is on the forklift, so it can be set (game-internals.md 11f).
    Leaves the mission running: keep it the last scenario."""
    g, S = s.g, rig.Scm
    space = g.u32(0x48741C)

    def gv(n):
        return space + 4 * n

    state = gv(0x159E)
    g.mission(47)
    for i in range(240):  # the opening cutscene (skipped), then state 0's "get in the truck" loop at mission +0x3BEE..
        base = g.u32(0x4CD3D8)
        if any(0x3BEE <= sc[2] - base <= 0x3C40 for sc in g.scripts()):
            break
        if i % 6 == 3:
            g.cmd("press Shift 150")
        time.sleep(0.5)
    else:
        raise RuntimeError("PHI_A2 never reached its truck loop")
    g.scm(S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, bytes([0xCD + 0x14, 0xEE])))  # $G14EE: Phil's truck
    for i in range(120):  # state 1 (drive Phil to the warehouse) skipped: to state 2 once the mission is in it
        if g.u32(state) == 2:
            break
        if i == 20 and g.u32(state) == 1:
            g.cmd("poke 0x%x 02000000" % state)
        time.sleep(0.25)
    g.scm(S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(-1235.45), S.f(-278.9), S.f(11.6)))  # the warehouse's back
    for i in range(120):
        if g.u32(state) == 4:
            break
        time.sleep(0.25)
    else:
        raise RuntimeError("PHI_A2 never reached the forklift state (state %d)" % g.u32(state))
    s.wait_sim(1.0)

    def sample():
        return s.clock(), g.peekf(gv(0x15AB), 1)[0], g.peekf(gv(0x15AA), 1)[0]

    a = sample()
    s.wait_sim(5.0)
    b = sample()
    g.cmd("pokef 0x%x 1.0" % gv(0x15AC))
    s.wait_sim(0.2)
    c = sample()
    s.wait_sim(1.5)  # +45 at 30 a second: the heat stays under 100 (where the boomshine blows)
    d = sample()
    return {"floor_per_s": (b[1] - a[1]) / (b[0] - a[0]), "heat_per_s": (d[2] - c[2]) / (d[0] - c[0])}


def scenario_clock(s):
    c0, f0 = s.g.u32(CLOCK_MS), s.g.u32(FRAME_COUNTER)
    s.wait_sim(5.0)
    c1, f1 = s.g.u32(CLOCK_MS), s.g.u32(FRAME_COUNTER)
    frames = max(f1 - f0, 1)
    return {"ms_per_frame": (c1 - c0) / frames, "expected_ms": 1000.0 / s.step, "frames_per_sim_s": frames / ((c1 - c0) / 1000.0)}


SCENARIOS = {
    "clock": scenario_clock,
    "fall": scenario_fall,
    "run": lambda s: scenario_foot(s, ["W"]),
    "sprint": lambda s: scenario_foot(s, ["W", "Shift"]),
    "jump": scenario_jump,
    "car": scenario_car,
    "heat": scenario_heat,  # starts a mission: last
}


def check_sites(port):
    """--sites: every per-frame meter instruction src/game/vcs_script_timing.cpp scales (its kSites) is where the table
    says, as the game decodes it: a game per listed mission with VCS_SCRIPT_TIMESTEP_CHECK=1, the mission launched, the
    log's "check: mission N +0x.. matches" lines counted against the table."""
    src = open(os.path.join(ROOT, "src", "game", "vcs_script_timing.cpp"), encoding="utf-8").read()
    table = {}
    for m, off in re.findall(r"^\s*\{(\d+), 0x([0-9A-Fa-f]+), 0x[0-9A-Fa-f]{4}, ", src, re.M):
        table.setdefault(int(m), set()).add(int(off, 16))
    bad = 0
    for mission in sorted(table):
        g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="sites_%d" % mission,
                           port=port, data_dir=os.path.join(ROOT, "build", "audit", "sites_data"),
                           env={"VCS_SCRIPT_TIMESTEP_CHECK": "1"})
        try:
            g.wait_ingame()
            g.prime()
            g.mission(mission)
            found = set()
            for _ in range(40):
                time.sleep(0.5)
                log = open(g.log_path, encoding="utf-8", errors="replace").read()
                found = {int(o, 16) for o in re.findall(r"check: mission %d \+0x([0-9A-F]+) matches" % mission, log)}
                if found >= table[mission]:
                    break
        finally:
            g.quit()
        missing = table[mission] - found
        bad += len(missing)
        print("[sites] mission %d: %d of %d found%s" % (mission, len(found & table[mission]), len(table[mission]),
              ", missing " + " ".join("+0x%X" % o for o in sorted(missing)) if missing else ""), flush=True)
    print("[sites] %s" % ("all found" if not bad else "%d missing" % bad))
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--steps", default="15,30,60,100")
    ap.add_argument("--only", default="", help="comma-separated scenarios (default: all)")
    ap.add_argument("--tolerance", type=float, default=0.05)
    ap.add_argument("--sites", action="store_true", help="only check the per-frame meter sites (check_sites)")
    a = ap.parse_args()
    if a.sites:
        sys.exit(0 if check_sites(47670) else 1)
    out = a.out or os.path.join(ROOT, "build", "audit", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    steps = [int(x) for x in a.steps.split(",") if x]
    names = [n for n in (a.only.split(",") if a.only else SCENARIOS) if n]
    results = {}
    for step in steps:
        print("[audit] step 1/%d s" % step, flush=True)
        s = Session(step, "audit_%d" % step, 47670)
        try:
            results[step] = {}
            for name in names:
                try:
                    results[step][name] = SCENARIOS[name](s)
                except Exception as e:  # one scenario failing doesn't end the run
                    results[step][name] = {"error": str(e)}
                print("  %-7s %s" % (name, results[step][name]), flush=True)
        finally:
            s.quit()
    with open(os.path.join(out, "audit.json"), "w", encoding="utf-8") as f:
        json.dump({"steps": steps, "results": results}, f, indent=1)

    ref = results.get(30) or results[steps[0]]
    print("\n%-8s %-22s" % ("scenario", "metric") + "".join("%12s" % ("1/%d" % st) for st in steps))
    flagged = 0
    for name in names:
        metrics = [k for k, v in ref.get(name, {}).items() if isinstance(v, (int, float))]
        for m in metrics:
            row = "%-8s %-22s" % (name, m)
            for st in steps:
                v = results[st].get(name, {}).get(m)
                r = ref[name][m]
                if not isinstance(v, (int, float)):
                    row += "%12s" % "-"
                    continue
                off = (v - r) / abs(r) if r else 0.0
                bad = abs(off) > a.tolerance and m not in ("expected_ms", "frames_per_sim_s")
                flagged += bad
                row += "%12s" % ("%.3g%s" % (v, " !" if bad else ""))
            print(row)
    print("\n%d values differ from 1/30 by more than %.0f%% (marked !); results in %s" % (flagged, a.tolerance * 100, out))


if __name__ == "__main__":
    main()
