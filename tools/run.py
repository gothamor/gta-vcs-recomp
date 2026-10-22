#!/usr/bin/env python3
"""Run the built game for a while and keep its output (testing / scripted runs).

    python tools/run.py [seconds=60] [logname=run] [--iso ISO]

Runs in build/run/ (so files the game or the debug switches write land there), logs stdout+stderr to
build/run/<logname>.log and stops the game after <seconds> (0 = until it exits). Environment variables
(PS2X_*, VCS_*) pass through. Exit code 124 means the time ran out, as with `timeout`.
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
    a = ap.parse_args()

    if not os.path.isfile(tc.VCS_EXE):
        tc.fail("not built yet: python tools/build.py --recompile")
    iso = tc.find_iso(a.iso)
    rundir = os.path.join(tc.BUILD, "run")
    os.makedirs(rundir, exist_ok=True)
    log = os.path.join(rundir, a.logname + ".log")
    with open(log, "w", encoding="utf-8") as f:
        proc = subprocess.Popen([tc.VCS_EXE, iso], cwd=rundir, stdout=f, stderr=subprocess.STDOUT)
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
