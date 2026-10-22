#!/usr/bin/env python3
"""Press a key in the game (testing menus without a person at the keyboard).

Through the game's control channel when it is open (no focus needed, works hidden; see tools/gamectl.py),
otherwise by focusing the window and sending the key (Windows).

    python tools/sendkey.py Return [--hold 150] [--times 1] [--title "GTA: Vice City Stories"]

Key names: Return, ShiftKey, Space, Escape, Up / Down / Left / Right, letters, digits, F1-F24 (see docs/CONTROLS.md
for what they do in game: Return = Start, ShiftKey = Cross, F = Triangle, WASD = left stick, IJKL = right stick).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import winwindow  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("key", nargs="?", default="Return")
    ap.add_argument("--hold", type=int, default=150, help="milliseconds")
    ap.add_argument("--times", type=int, default=1)
    ap.add_argument("--title", default=winwindow.DEFAULT_TITLE)
    a = ap.parse_args()
    if gamectl.available():
        for _ in range(a.times):
            reply = gamectl.send(f"press {a.key} {a.hold}")
            if not reply.startswith("ok"):
                sys.exit(reply)
        return
    winwindow.press(winwindow.find_window(a.title), a.key, a.hold, a.times)


if __name__ == "__main__":
    main()
