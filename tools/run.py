#!/usr/bin/env python3
"""Run the built game for a while and keep its output (testing / scripted runs).

    python tools/run.py [seconds=60] [logname=run] [--iso ISO] [--visible] [--control]

Runs in build/run/ (so files the game or the debug switches write land there), logs stdout+stderr to
build/run/<logname>.log and stops the game after <seconds> (0 = until it exits). Environment variables
(PS2X_*, VCS_*) pass through; PS2X_GS defaults to hw (the D3D11 hardware renderer). Exit code 124 means the time ran out, as with `timeout`.

Test runs are hidden by default: no window on the desktop, the physical keyboard / mouse / gamepad ignored, sound on;
drive them with tools/gamectl.py (screenshots, key presses, perf numbers). --visible (or VCS_TEST_VISIBLE=1) shows
the window; --control then also opens the control channel. --hidden is accepted and is the default.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seconds", nargs="?", type=float, default=60.0)
    ap.add_argument("logname", nargs="?", default="run")
    ap.add_argument("--iso", help="disc image (default: see tools/toolchain.py find_iso)")
    ap.add_argument("--hidden", action="store_true", help="the default: no window, no physical input; control channel on")
    ap.add_argument("--visible", action="store_true", help="show the window (VCS_TEST_VISIBLE=1 does too)")
    ap.add_argument("--control", action="store_true", help="control channel on (tools/gamectl.py) with the window shown")
    a = ap.parse_args()

    if not os.path.isfile(tc.VCS_EXE):
        tc.fail("not built yet: python tools/build.py --recompile")
    iso = tc.find_iso(a.iso)
    rundir = os.path.join(tc.BUILD, "run")
    os.makedirs(rundir, exist_ok=True)
    log = os.path.join(rundir, a.logname + ".log")
    with open(log, "w", encoding="utf-8") as f:
        env = dict(os.environ)
        env.setdefault("PS2X_GS", "hw")  # the hardware (D3D11) renderer; the runtime's own default is the slow software one
        env.setdefault("PS2X_CAPTURE_SIZE", "960x672")  # control-channel screenshots independent of the window size
        env.setdefault("VCS_SKIP_MOVIES", "1")  # test runs boot straight on (VCS_SKIP_MOVIES=0 plays the intro movies)
        tc.test_window(env, visible=a.visible and not a.hidden)
        if a.control:
            env["PS2X_CONTROL"] = "1"
        proc = subprocess.Popen([tc.VCS_EXE, iso], cwd=rundir, stdout=f, stderr=subprocess.STDOUT, env=env)
        try:
            code = proc.wait(timeout=a.seconds if a.seconds > 0 else None)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            code = 124
    with open(log, encoding="utf-8", errors="replace") as f:
        lines = sum(1 for _ in f)
    print(f"exit={code} lines={lines}")
    sys.exit(0 if code in (0, 124) else code)


if __name__ == "__main__":
    main()
