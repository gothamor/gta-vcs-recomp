#!/usr/bin/env python3
"""Control a running game over its control channel (runtime/ps2_control.h).

The game listens when started with PS2X_CONTROL=1 or PS2X_HIDDEN=1, on 127.0.0.1 (the console's own loopback: the
channel on the LAN is on docs/PS5.md's work list, as is a way to set those variables on the console).

    python3 tools/gamectl.py wait                       wait until the game answers (up to --timeout seconds)
    python3 tools/gamectl.py screenshot out.png         picture incl. the perf overlay (relative paths: from here)
    python3 tools/gamectl.py press cross [150]          press and release a DualShock 2 button (ms held)
    python3 tools/gamectl.py button start down | up     hold / release
    python3 tools/gamectl.py perf                       the perf overlay's numbers for the last second
    python3 tools/gamectl.py quit

Button names: cross, circle, square, triangle, l1, r1, l2, r2, l3, r3, start, select, up, down, left, right. The
game layer adds more commands (setting, menu, padstate, the rig's: src/debug/rig.cpp).
"""
import argparse
import os
import socket
import struct
import sys
import time

DEFAULT_PORT = int(os.environ.get("PS2X_CONTROL_PORT", "47615"))


def send(command, port=DEFAULT_PORT, timeout=70.0):
    """Sends one command line; returns the reply line. Raises OSError when no game is listening."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        # closed without TIME_WAIT: a tool polling every frame would otherwise run out of ephemeral ports
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.sendall((command + "\n").encode("utf-8"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    return data.decode("utf-8", errors="replace").strip()


def available(port=DEFAULT_PORT):
    try:
        return send("ping", port, timeout=2.0) == "ok"
    except OSError:
        return False


def wait(port=DEFAULT_PORT, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if available(port):
            return True
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="+", help="wait | screenshot PATH | press BUTTON [MS] | button BUTTON down|up | perf | quit | ping")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=60.0, help="for wait (s)")
    a = ap.parse_args()
    words = a.command
    if words[0] == "wait":
        ok = wait(a.port, a.timeout)
        print("ok" if ok else "error no game answered")
        sys.exit(0 if ok else 1)
    if words[0] == "screenshot" and len(words) > 1:
        path = os.path.abspath(" ".join(words[1:]))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        words = ["screenshot", path]
    try:
        reply = send(" ".join(words), a.port)
    except OSError as e:
        sys.exit(f"error: no game on 127.0.0.1:{a.port} ({e}); start it with PS2X_CONTROL=1")
    print(reply)
    sys.exit(0 if reply.startswith("ok") else 1)


if __name__ == "__main__":
    main()
