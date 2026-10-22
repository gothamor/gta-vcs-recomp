#!/usr/bin/env python3
"""Build the game (configures CMake the first time).

    python tools/build.py                       build vcs (build/release/.../ps2xRuntime/vcs[.exe])
    python tools/build.py --recompile [ISO]     recompile from your disc first, then build - the whole pipeline

ISO defaults to VCS_ISO, iso=... in vcs.ini, or a '*Vice City Stories*.iso' in the repository root.
Windows: needs Visual Studio 2022+ with the C++ workload (clang-cl), CMake and Ninja; the Visual Studio environment
is set up automatically.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recompile", nargs="?", const="", metavar="ISO",
                    help="run tools/recompile.py first (disc image optional, see above)")
    ap.add_argument("--target", default="ps2EntryRunner", help="CMake target (default: the game, ps2EntryRunner)")
    ap.add_argument("--reconfigure", action="store_true", help="run CMake configure again")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    a = ap.parse_args()

    env = tc.build_env()
    if a.recompile is not None:
        import recompile
        recompile.recompile(a.recompile or None, env, a.verbose)
    elif not os.path.isdir(os.path.join(tc.ROOT, "generated", "ee")):
        print("[build] no generated code yet: run with --recompile (only the tools will be built)")

    tc.configure(env, force=a.reconfigure)
    print(f"[build] building {a.target}")
    if not tc.cmake_build(env, a.target, "build.log", a.verbose):
        sys.exit(1)
    if a.target == "ps2EntryRunner":
        if not os.path.isfile(tc.VCS_EXE):
            tc.fail("build finished but " + os.path.relpath(tc.VCS_EXE, tc.ROOT) + " is missing")
        print("[build] ok: " + os.path.relpath(tc.VCS_EXE, tc.ROOT))


if __name__ == "__main__":
    main()
