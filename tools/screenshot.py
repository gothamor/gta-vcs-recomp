#!/usr/bin/env python3
"""Capture the game window to a PNG (Windows).

    python tools/screenshot.py [build/run/shot.png] [--title "GTA: Vice City Stories"]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winwindow  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join("build", "run", "shot.png"))
    ap.add_argument("--title", default=winwindow.DEFAULT_TITLE)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    w, h = winwindow.capture_png(winwindow.find_window(a.title), a.out)
    print(f"saved {a.out} {w}x{h}")


if __name__ == "__main__":
    main()
