#!/usr/bin/env python3
"""Automation rig client (docs/NATIVE_PORT_ROADMAP.md milestone 1; src/debug/rig.cpp is the game side).

    python tools/rig.py start [--save DIR] [--seconds N] [--name NAME]   launch with the control channel
    python tools/rig.py peek ADDR [BYTES] | peekf ADDR [COUNT] | poke ADDR HEX | pokef ADDR F...
    python tools/rig.py findvec X Y Z TOL [LO HI] | findu32 VALUE [LO HI] | pause on|off | frames
    python tools/rig.py <any control command ...>                        sent as it is

`start` runs the game from a memory card folder (default: an empty card in build/rig/data). With --save DIR the
card is copied from DIR (a folder holding mc0/) so the game auto-loads its newest save - the quickest way into
gameplay. The user's own card is never used directly.

As a library: `import rig; g = rig.Game.start(save=...); g.cmd('peekf 0x6f4500 3'); g.quit()`.
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = tc.ROOT if hasattr(tc, "ROOT") else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def copy_card(src, dst):
    """Copy a card folder keeping file times (the card's directory order falls back to creation times)."""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if os.name == "nt":
        # robocopy keeps creation times; exit codes < 8 mean success
        r = subprocess.run(["robocopy", src, dst, "/E", "/COPY:DAT", "/DCOPY:T", "/NFL", "/NDL", "/NJH", "/NJS"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode >= 8:
            raise RuntimeError("robocopy failed (%d) copying %s" % (r.returncode, src))
    else:
        shutil.copytree(src, dst)


class Scm:
    """Bytecode for the game's script VM (docs/game-internals.md section 11). Opcodes are the VCS numbers
    (Sanny Builder Library vcs.json, checked against the handler table); arguments carry a type byte."""
    PLAYER = b"\xcf\xa5"  # global 0x2A5: the player (VCS uses one handle for the player and the player's char)
    LOCAL0 = b"\x0d"      # local variable 0 (outputs)

    # opcodes
    TERMINATE = 0x0023
    SET_CHAR_COORDINATES = 0x0044
    CREATE_CAR = 0x0048
    SET_TIME_OF_DAY = 0x005B
    CLEAR_WANTED_LEVEL = 0x009B
    DO_FADE = 0x00C9
    SET_CHAR_HEADING = 0x00CF
    GIVE_WEAPON_TO_CHAR = 0x0106
    SET_PLAYER_CONTROL = 0x0107
    FORCE_WEATHER_NOW = 0x0109
    RELEASE_WEATHER = 0x010A
    SET_CHAR_HEALTH = 0x014B
    REQUEST_MODEL = 0x0160
    MARK_MODEL_AS_NO_LONGER_NEEDED = 0x0162
    SET_CHAR_PROOFS = 0x01B1
    WARP_CHAR_INTO_CAR = 0x021C
    LOAD_ALL_MODELS_NOW = 0x0228
    LOAD_SCENE = 0x0253
    LAUNCH_MISSION = 0x0289
    REQUEST_COLLISION = 0x0303

    @staticmethod
    def op(code, *args):
        return code.to_bytes(2, "little") + b"".join(args)

    @staticmethod
    def i(v):
        v = int(v)
        if v == 0:
            return b"\x01"
        if -128 <= v < 128:
            return b"\x07" + (v & 0xFF).to_bytes(1, "little")
        if -32768 <= v < 32768:
            return b"\x08" + (v & 0xFFFF).to_bytes(2, "little")
        return b"\x06" + (v & 0xFFFFFFFF).to_bytes(4, "little")

    @staticmethod
    def f(v):
        import struct
        return b"\x09" + struct.pack("<f", float(v))

    @classmethod
    def end(cls):
        return cls.op(cls.TERMINATE)


class Game:
    def __init__(self, proc, log_path):
        self.proc = proc
        self.log_path = log_path

    @classmethod
    def start(cls, save=None, name="rig", seconds=3600, env=None, data_dir=None):
        data = data_dir or os.path.join(ROOT, "build", "rig", "data")
        if save:
            os.makedirs(data, exist_ok=True)
            copy_card(os.path.join(save, "mc0"), os.path.join(data, "mc0"))
        else:
            if os.path.isdir(data):
                shutil.rmtree(data)
            os.makedirs(data)
        e = dict(os.environ)
        e["VCS_DATA_DIR"] = os.path.abspath(data)
        e.update(env or {})
        # run.py logs to build/run/<name>.log; no inherited pipes, so a caller's shell does not wait for the game
        proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "run.py"), str(int(seconds)), name, "--control"],
                                cwd=ROOT, env=e, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        gamectl.wait(timeout=90)
        return cls(proc, os.path.join(ROOT, "build", "run", name + ".log"))

    def cmd(self, line, timeout=70.0):
        reply = gamectl.send(line, timeout=timeout)
        if not reply.startswith("ok"):
            raise RuntimeError("%s -> %s" % (line, reply))
        return reply[3:] if len(reply) > 3 else ""

    def peekf(self, addr, n=4):
        return [float(v) for v in self.cmd("peekf 0x%x %d" % (addr, n)).split()]

    def peek(self, addr, n):
        return bytes.fromhex(self.cmd("peek 0x%x %d" % (addr, n)))

    def u32(self, addr):
        return int.from_bytes(self.peek(addr, 4), "little")

    # ---- script commands (run through the game's own script VM) ----
    def scm(self, code):
        """Run bytecode as a new script; returns when it terminated (Scm.end() is appended)."""
        return self.cmd("scm " + (code + Scm.end()).hex(), timeout=90)

    def player(self):
        """(ped address, (x, y, z), mission running)"""
        w = self.cmd("player").split()
        return int(w[0], 16), (float(w[1]), float(w[2]), float(w[3])), w[4] == "1"

    def scripts(self):
        out = []
        for item in self.cmd("scripts").split():
            sid, name, ip, mission = item.split(":")
            out.append((int(sid), name, int(ip, 16), mission == "1"))
        return out

    def teleport(self, x, y, z, heading=None):
        S = Scm
        code = S.op(S.LOAD_SCENE, S.f(x), S.f(y), S.f(z)) + S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(x), S.f(y), S.f(z))
        if heading is not None:
            code += S.op(S.SET_CHAR_HEADING, S.PLAYER, S.f(heading))
        return self.scm(code)

    def set_time(self, hours, minutes=0):
        return self.scm(Scm.op(Scm.SET_TIME_OF_DAY, Scm.i(hours), Scm.i(minutes)))

    def weather(self, kind=None):
        """Force a weather type (0 sunny ...), or None to release it."""
        if kind is None:
            return self.scm(Scm.op(Scm.RELEASE_WEATHER))
        return self.scm(Scm.op(Scm.FORCE_WEATHER_NOW, Scm.i(kind)))

    def invincible(self, on=True):
        b = Scm.i(1 if on else 0)
        return self.scm(Scm.op(Scm.SET_CHAR_PROOFS, Scm.PLAYER, b, b, b, b, b) + Scm.op(Scm.CLEAR_WANTED_LEVEL, Scm.PLAYER))

    def health(self, value=100):
        return self.scm(Scm.op(Scm.SET_CHAR_HEALTH, Scm.PLAYER, Scm.i(value)))

    def mission(self, index):
        """LOAD_AND_LAUNCH_MISSION_INTERNAL: the game's own mission start (only when no mission is running)."""
        if self.player()[2]:
            raise RuntimeError("a mission is already running")
        return self.scm(Scm.op(Scm.LAUNCH_MISSION, Scm.i(index)))

    def wait_frames(self, n, timeout=60.0):
        start = int(self.cmd("frames"))
        t0 = time.time()
        while int(self.cmd("frames")) < start + n:
            if time.time() - t0 > timeout:
                raise RuntimeError("game frames stalled")
            time.sleep(0.05)

    def quit(self):
        try:
            gamectl.send("quit", timeout=10)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=30)
        except Exception:
            self.proc.kill()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "start":
        import argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("start")
        ap.add_argument("--save")
        ap.add_argument("--seconds", type=int, default=3600)
        ap.add_argument("--name", default="rig")
        a = ap.parse_args()
        g = Game.start(save=a.save, name=a.name, seconds=a.seconds)
        print("started; control channel up (log: %s)" % g.log_path)
        return 0
    print(gamectl.send(" ".join(sys.argv[1:])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
