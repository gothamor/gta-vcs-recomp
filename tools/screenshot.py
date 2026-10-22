#!/usr/bin/env python3
"""Save a screenshot of the running game as a PNG.

    python tools/screenshot.py [build/run/shot.png] [--title "GTA: Vice City Stories"]

Uses the game's control channel when it is open (works with the window hidden or covered, see tools/gamectl.py);
otherwise captures the window (Windows).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import winwindow  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join("build", "run", "shot.png"))
    ap.add_argument("--title", default=winwindow.DEFAULT_TITLE)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if gamectl.available():
        reply = gamectl.send("screenshot " + out)
        if not reply.startswith("ok"):
            sys.exit(reply)
        print(f"saved {a.out} {reply[3:]}")
        return
    w, h = winwindow.capture_png(winwindow.find_window(a.title), out)
    print(f"saved {a.out} {w}x{h}")


if __name__ == "__main__":
    main()
