#!/usr/bin/env python3
"""Recompile the game: your ISO -> generated C++ (generated/ee, generated/vu1).

    python tools/recompile.py ["path/to/Grand Theft Auto - Vice City Stories (USA).iso"]

Without an argument the disc is taken from VCS_ISO, iso=... in vcs.ini, or a '*Vice City Stories*.iso' in the
repository root. Uses the committed function map (game/vcs); Ghidra is only needed to redo the analysis
(tools/ghidra_analyze.py). To recompile and build in one go: python tools/build.py --recompile
"""
import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

ELF = os.path.join(tc.BUILD, "elf", "SLUS_215.90")


def step(title):
    print(f"[recompile] {title}")


def run_logged(cmd, log, what):
    code = tc.run_with_progress(cmd, log, "[recompile] " + what)
    if code != 0:
        print(tc.tail(log))
        tc.fail(f"{what} failed (full log: {os.path.relpath(log, tc.ROOT)})")


def recompile(iso, env=None, verbose=False):
    env = env or tc.build_env()
    iso = tc.find_iso(iso)
    os.makedirs(os.path.dirname(ELF), exist_ok=True)

    step("extracting the executable from " + os.path.basename(iso))
    if subprocess.run(tc.PY + [tc.tool("vcs_disc.py"), "elf", iso, ELF], cwd=tc.ROOT).returncode != 0:
        tc.fail("could not extract SLUS_215.90")

    # Always (re)build the recompiler: it compiles in the runtime's stub list (ps2_call_list.h), so a stale
    # binary silently ignores new stubs.
    step("building the recompiler")
    tc.configure(env)
    if not tc.cmake_build(env, "ps2_recomp", "build_recomp.log", verbose):
        tc.fail("recompiler build failed")

    export = os.path.join(tc.ROOT, "ghidra", "export", "ghidra_export.toml")
    analyzer = os.path.join(tc.BUILD, "analyzer", "analyzer.toml")
    if os.path.isfile(export) and os.path.isfile(analyzer):
        step("merging the analysis exports into game/vcs/vcs.toml")
        subprocess.run(tc.PY + [tc.tool("make_config.py"), export, analyzer], cwd=tc.ROOT)
    else:
        step("analysis exports not present: using the committed game/vcs/vcs.toml")

    # Generate into a scratch dir, then copy only changed files so rebuilds stay incremental.
    step("translating MIPS to C++")
    fresh = os.path.join(tc.BUILD, "generated_fresh")
    shutil.rmtree(fresh, ignore_errors=True)
    with open(os.path.join(tc.ROOT, "game", "vcs", "vcs.toml"), encoding="utf-8") as f:
        config = f.read()
    config = re.sub(r'(?m)^output = .*$', 'output = "build/generated_fresh"', config)
    fresh_toml = os.path.join(tc.BUILD, "vcs_fresh.toml")
    with open(fresh_toml, "w", encoding="utf-8") as f:
        f.write(config)
    log = os.path.join(tc.BUILD, "recomp.log")
    run_logged([tc.RECOMP_EXE, fresh_toml], log, "recompiler")
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Warnings:" in line:
                print("  " + line.strip())
    subprocess.run(tc.PY + [tc.tool("sync_generated.py"), fresh, os.path.join(tc.ROOT, "generated", "ee")], cwd=tc.ROOT, check=True)

    # VU1 microcode -> C++ (runs instead of the VU1 interpreter while VU1 memory holds exactly that code)
    step("compiling VU1 microcode")
    out = os.path.join(tc.BUILD, "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    result = subprocess.run(tc.PY + [tc.tool("vu1_recomp.py"), ELF, out, os.path.join(tc.ROOT, "game", "vcs", "vu1_uploads.txt")],
                            cwd=tc.ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:] + result.stderr[-2000:])
        tc.fail("vu1_recomp failed")
    lines = result.stdout.strip().splitlines()
    if lines:
        print("  " + lines[-1])
    dest = os.path.join(tc.ROOT, "generated", "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.isfile(dest) or not filecmp.cmp(out, dest, shallow=False):
        shutil.copyfile(out, dest)
    step("done")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", nargs="?", help="your Vice City Stories (USA) disc image")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    a = ap.parse_args()
    recompile(a.iso, verbose=a.verbose)


if __name__ == "__main__":
    main()
