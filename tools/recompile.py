#!/usr/bin/env python3
"""Recompile the game: your ISO -> generated C++ (generated/ee, generated/vu1).

    python tools/recompile.py ["path/to/Grand Theft Auto - Vice City Stories (USA).iso"]

Without an argument the disc is taken from VCS_ISO, iso=... in build/vcs.ini or vcs.ini, or a '*Vice City
Stories*.iso' in the repository root. `--check` says whether generated/ is current (generated/recompile_stamp.json: a
hash of the recompiler, its config and the function map). Uses the committed function map (game/vcs); Ghidra is only needed to redo the analysis
(tools/ghidra_analyze.py). To recompile and build in one go: python tools/build.py --recompile
"""
import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

ELF = os.path.join(tc.BUILD, "elf", "SLUS_215.90")
GENERATED = os.path.join(tc.ROOT, "generated")
STAMP = os.path.join(GENERATED, "recompile_stamp.json")

# What the generated code depends on besides the disc: the recompiler (and the runtime headers it compiles in: the
# stub list), the config and function map, and the scripts that translate and sync. A change to any of them means a
# recompile; nothing else does (the runtime and src/ are compiled, not generated).
STAMP_INPUTS = [
    "game/vcs",
    "third_party/PS2Recomp/ps2xRecomp",
    "third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h",
    "third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_calls.h",
    "third_party/PS2Recomp/ps2xRuntime/include/runtime/ps2_address.h",
    "third_party/PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h",
    "tools/recompile.py",
    "tools/vu1_recomp.py",
    "tools/sync_generated.py",
    "tools/make_config.py",
]


def step(title, stage_id=None):
    print(f"[recompile] {title}")
    if stage_id:
        tc.stage(stage_id, title[0].upper() + title[1:])


def inputs_hash():
    """SHA-1 over the contents of STAMP_INPUTS (line endings normalised: a checkout with CRLF is the same source)."""
    h = hashlib.sha1()
    for rel in STAMP_INPUTS:
        path = os.path.join(tc.ROOT, rel)
        files = [path] if os.path.isfile(path) else sorted(
            os.path.join(d, f) for d, _, names in os.walk(path) for f in names if "__pycache__" not in d)
        for f in files:
            h.update(os.path.relpath(f, tc.ROOT).replace(os.sep, "/").encode())
            with open(f, "rb") as fh:
                h.update(fh.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def read_stamp():
    try:
        with open(STAMP, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def why_recompile():
    """None when generated/ came from this source's recompiler and config, else the reason (for people and the
    launcher). The disc doesn't count: only one build of it is supported, and recompile checks that."""
    if not os.path.isfile(os.path.join(GENERATED, "ee", "register_functions.cpp")):
        return "no generated code yet"
    if not os.path.isfile(os.path.join(GENERATED, "vu1", "vu1_compiled.cpp")):
        return "no compiled VU1 microcode yet"
    stamp = read_stamp()
    if not stamp:
        return "the generated code has no stamp (made before stamps existed)"
    if stamp.get("inputs") != inputs_hash():
        return "the recompiler, its config or the function map changed"
    return None


def write_stamp():
    os.makedirs(GENERATED, exist_ok=True)
    with open(STAMP, "w", encoding="utf-8") as f:
        json.dump({"inputs": inputs_hash(), "source": tc.source_version(), "time": int(time.time())}, f, indent=1)


def run_logged(cmd, log, what):
    code = tc.run_with_progress(cmd, log, "[recompile] " + what)
    if code != 0:
        print(tc.tail(log))
        tc.fail(f"{what} failed (full log: {tc.show(log)})", "recompiler_failed", log)


def recompile(iso, env=None, verbose=False):
    env = env or tc.build_env()
    iso = tc.find_iso(iso)
    os.makedirs(os.path.dirname(ELF), exist_ok=True)

    step("extracting the executable from " + os.path.basename(iso), "disc")
    if subprocess.run(tc.PY + [tc.tool("vcs_disc.py"), "elf", iso, ELF], cwd=tc.ROOT).returncode != 0:
        tc.fail("could not extract SLUS_215.90 from " + iso, "wrong_disc")

    # Always (re)build the recompiler: it compiles in the runtime's stub list (ps2_call_list.h), so a stale
    # binary silently ignores new stubs.
    tc.configure(env)  # its own stage when it runs (the first time)
    step("building the recompiler", "recompiler")
    if not tc.cmake_build(env, "ps2_recomp", "build_recomp.log", verbose):
        tc.fail("recompiler build failed", "recompiler_failed", os.path.join(tc.BUILD, "build_recomp.log"))

    export = os.path.join(tc.ROOT, "ghidra", "export", "ghidra_export.toml")
    analyzer = os.path.join(tc.BUILD, "analyzer", "analyzer.toml")
    if os.path.isfile(export) and os.path.isfile(analyzer):
        step("merging the analysis exports into game/vcs/vcs.toml")
        subprocess.run(tc.PY + [tc.tool("make_config.py"), export, analyzer], cwd=tc.ROOT)
    else:
        step("analysis exports not present: using the committed game/vcs/vcs.toml")

    # Generate into a scratch dir, then copy only changed files so rebuilds stay incremental.
    step("translating MIPS to C++", "translate")
    fresh = os.path.join(tc.BUILD, "generated_fresh")
    shutil.rmtree(fresh, ignore_errors=True)
    with open(os.path.join(tc.ROOT, "game", "vcs", "vcs.toml"), encoding="utf-8") as f:
        config = f.read()
    config = re.sub(r'(?m)^output = .*$', lambda _: 'output = "%s"' % fresh.replace(os.sep, "/"), config)
    config = re.sub(r'(?m)^input = .*$', lambda _: 'input = "%s"' % ELF.replace(os.sep, "/"), config)
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
    step("compiling VU1 microcode", "vu1")
    out = os.path.join(tc.BUILD, "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    result = subprocess.run(tc.PY + [tc.tool("vu1_recomp.py"), ELF, out, os.path.join(tc.ROOT, "game", "vcs", "vu1_uploads.txt")],
                            cwd=tc.ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:] + result.stderr[-2000:])
        tc.fail("vu1_recomp failed", "recompiler_failed")
    lines = result.stdout.strip().splitlines()
    if lines:
        print("  " + lines[-1])
    dest = os.path.join(tc.ROOT, "generated", "vu1", "vu1_compiled.cpp")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if not os.path.isfile(dest) or not filecmp.cmp(out, dest, shallow=False):
        shutil.copyfile(out, dest)
    write_stamp()
    step("done")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso", nargs="?", help="your Vice City Stories (USA) disc image")
    ap.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    ap.add_argument("--check", action="store_true", help="only say whether the generated code is current (exit 0) or not (1)")
    a = ap.parse_args()
    if a.check:
        why = why_recompile()
        print("[recompile] " + ("needed: " + why if why else "generated code is current"))
        sys.exit(1 if why else 0)
    recompile(a.iso, verbose=a.verbose)


if __name__ == "__main__":
    main()
