#!/usr/bin/env python3
"""Build the game (configures CMake the first time).

    python tools/build.py                       build vcs (build/release/.../ps2xRuntime/vcs[.exe])
    python tools/build.py --recompile [ISO]     recompile from your disc first, then build - the whole pipeline
    python tools/build.py --quick               no link-time optimisation: fast rebuilds while changing code
    python tools/build.py --iop-tests           build and run the IOP / SPU2 unit tests (build/iop_tests, no disc)
    python tools/build.py --opt none|lto|pgo    optimisation of the game, kept until changed: lto (default; ThinLTO,
                                                the game to play), none (= --quick), pgo (ThinLTO + profile-guided:
                                                trains first when build/pgo/vcs.profdata is missing or with --train;
                                                training needs the rig's card copies, build/rig/saves)

The optimisation is kept in the CMake cache: after --quick every build stays quick until `--opt lto`. Switching
recompiles the whole game (~6-10 min). The last line of each build says which one was built.

Progress: every build step shows `[build] <target> done/total %, elapsed, ~time left` (Ninja's step count; redrawn in
place on a terminal, a line per 5% / minute otherwise); CMake configure and the recompiler show their elapsed time.

PGO training: an instrumented build in build/pgo-gen plays tools/bench.py (city spots, an AI drive) and the
reference scenes' boot / flyover / cutscene (tools/scenes.py); the profiles go to build/pgo/vcs.profdata. A profile
from older code still works: the functions that changed are optimised as without one.

ISO defaults to VCS_ISO, iso=... in vcs.ini, or a '*Vice City Stories*.iso' in the repository root.
Windows: needs Visual Studio 2022+ (or its Build Tools) with "Desktop development with C++" and "C++ Clang tools for
Windows"; its CMake and Ninja are used when no others are on PATH. The Visual Studio environment is set up
automatically.
"""
import argparse
import os
import subprocess
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
    ap.add_argument("--iop-tests", action="store_true", help="build and run the ps2xIOP unit tests (SPU2) instead")
    ap.add_argument("--opt", choices=("none", "lto", "pgo"), help="optimisation of the game (kept until changed)")
    ap.add_argument("--quick", action="store_true", help="same as --opt none: fast rebuilds, slower game")
    ap.add_argument("--train", action="store_true", help="with --opt pgo: train a new profile first")
    a = ap.parse_args()
    if a.quick:
        if a.opt not in (None, "none"):
            ap.error("--quick is --opt none; it can't be combined with --opt " + a.opt)
        a.opt = "none"

    env = tc.build_env()
    if a.iop_tests:
        sys.exit(iop_tests(env))
    if a.recompile is not None:
        import recompile
        recompile.recompile(a.recompile or None, env, a.verbose)
    elif not os.path.isdir(os.path.join(tc.ROOT, "generated", "ee")):
        print("[build] no generated code yet: run with --recompile (only the tools will be built)")

    defines = None
    if a.opt == "none":
        defines = {"PS2X_LTO": "OFF", "PS2X_PGO": ""}
    elif a.opt == "lto":
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": ""}
    elif a.opt == "pgo":
        if a.train or not os.path.isfile(PROFDATA):
            train(env, a.verbose)
        defines = {"PS2X_LTO": "THIN", "PS2X_PGO": PROFDATA.replace(os.sep, "/")}
    tc.configure(env, force=a.reconfigure, defines=defines)
    print(f"[build] building {a.target}")
    if not tc.cmake_build(env, a.target, "build.log", a.verbose):
        sys.exit(1)
    if a.target == "ps2EntryRunner":
        if not os.path.isfile(tc.VCS_EXE):
            tc.fail("build finished but " + os.path.relpath(tc.VCS_EXE, tc.ROOT) + " is missing")
        remember_iso(a.recompile or None)
        print("[build] ok: " + os.path.relpath(tc.VCS_EXE, tc.ROOT) + " (" + built_opt() + ")")


def remember_iso(given):
    """Writes iso=<disc> into the vcs.ini next to vcs.exe when it has none, so the exe started on its own (a double
    click) finds the disc the build used. An iso= already there is the player's and stays."""
    try:
        iso = tc.find_iso(given)
    except SystemExit:
        return  # no disc known here: vcs.exe looks next to itself, or is given one
    ini = os.path.join(os.path.dirname(tc.VCS_EXE), "vcs.ini")
    lines = []
    if os.path.isfile(ini):
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    if any(line.strip().lower().startswith("iso=") for line in lines):
        return
    with open(ini, "w", encoding="utf-8") as f:
        f.write("\n".join(lines + ["iso=" + iso]) + "\n")
    print("[build] disc for vcs.exe: " + iso + " (iso= in " + os.path.relpath(ini, tc.ROOT) + ")")


def built_opt():
    """The optimisation the CMake cache of build/release holds, for the last line of a build."""
    cache = {}
    try:
        with open(os.path.join(tc.BUILD_DIR, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for line in f:
                key, sep, value = line.partition("=")
                if sep and key.split(":")[0] in ("PS2X_LTO", "PS2X_PGO"):
                    cache[key.split(":")[0]] = value.strip()
    except OSError:
        return "optimisation unknown"
    if cache.get("PS2X_PGO") and cache["PS2X_PGO"] != "GEN":
        return "optimised: ThinLTO + profile-guided"
    if cache.get("PS2X_LTO") == "THIN":
        return "optimised: ThinLTO"
    if cache.get("PS2X_LTO") == "FULL":
        return "optimised: full LTO"
    return "quick build, not optimised - python tools/build.py --opt lto for the game to play"


PGO_DIR = os.path.join(tc.BUILD, "pgo")
PROFDATA = os.path.join(PGO_DIR, "vcs.profdata")
PGO_BUILD = os.path.join(tc.BUILD, "pgo-gen")


def train(env, verbose):
    """Instrumented build (build/pgo-gen), training runs, build/pgo/vcs.profdata."""
    import glob
    import shutil
    tc.configure(env, build_dir=PGO_BUILD, defines={"PS2X_LTO": "OFF", "PS2X_PGO": "GEN"})
    print("[build] building the instrumented game (build/pgo-gen)")
    if not tc.cmake_build(env, "ps2EntryRunner", "build_pgo_gen.log", verbose, build_dir=PGO_BUILD):
        sys.exit(1)
    raw = os.path.join(PGO_DIR, "raw")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    run_env = dict(os.environ)
    run_env["VCS_EXE"] = tc.game_exe(PGO_BUILD)
    run_env["LLVM_PROFILE_FILE"] = os.path.join(raw, "vcs-%p.profraw")
    for cmd in ([tool("bench.py"), "--name", "pgo-train"], [tool("scenes.py"), "capture", os.path.join(PGO_DIR, "scenes")]):
        print("[build] training: " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
        subprocess.call([sys.executable] + cmd, env=run_env)
    profiles = glob.glob(os.path.join(raw, "*.profraw"))
    if not profiles:
        tc.fail("training wrote no profiles (build/pgo/raw)")
    profdata = shutil.which("llvm-profdata", path=env.get("PATH")) or "llvm-profdata"
    subprocess.check_call([profdata, "merge", "-o", PROFDATA] + profiles, env=env)
    print("[build] profile: " + os.path.relpath(PROFDATA, tc.ROOT) + " from %d runs" % len(profiles))


def tool(name):
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), name)


def iop_tests(env):
    """ps2xIOP on its own with PS2X_IOP_BUILD_TESTS=ON in build/iop_tests; runs the SPU2 checks."""
    out = os.path.join(tc.BUILD, "iop_tests")
    if not os.path.isfile(os.path.join(out, "build.ninja")):
        subprocess.check_call(["cmake", "-S", os.path.join(tc.ROOT, "third_party", "PS2Recomp", "ps2xIOP"), "-B", out,
                               "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_IOP_BUILD_TESTS=ON",
                               "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"], env=env)
    if not tc.cmake_build(env, "ps2_iop_spu2_tests", "build_iop_tests.log", build_dir=out):
        return 1
    return subprocess.call([tc.exe(os.path.join(out, "ps2_iop_spu2_tests"))])


if __name__ == "__main__":
    main()
