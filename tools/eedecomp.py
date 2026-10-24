#!/usr/bin/env python3
"""Decompile game functions to C with headless Ghidra, from the analysed EE project (tools/ghidra_analyze.py).
Analysis only: the output is derived from game code and stays under build/analysis/.

    python tools/eedecomp.py ADDR [ADDR ...] [--out build/analysis/decomp/NAME.c]

ADDR: hex addresses (any address inside a function names that function). Needs Ghidra 12.1.4 + the EE extension and
JDK 21 (GHIDRA_HOME / JAVA_HOME, defaults as in ghidra_analyze.py). The project is opened read-only (-readOnly).
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addrs", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()

    home = os.path.expanduser("~")
    ghidra = os.environ.get("GHIDRA_HOME", os.path.join(home, "scoop", "apps", "ghidra", "current"))
    env = dict(os.environ)
    env.setdefault("JAVA_HOME", os.path.join(home, "scoop", "apps", "temurin21-jdk", "current"))
    headless = os.path.join(ghidra, "support", "analyzeHeadless" + (".bat" if tc.IS_WINDOWS else ""))
    if not os.path.isfile(headless):
        tc.fail(f"{headless} not found: set GHIDRA_HOME")
    project = os.path.join(tc.ROOT, "ghidra", "project")
    if not os.path.isfile(os.path.join(project, "vcs.gpr")):
        tc.fail("no EE project yet: python tools/ghidra_analyze.py")
    out = os.path.abspath(a.out or os.path.join(tc.BUILD, "analysis", "decomp", "_".join(x.lower().replace("0x", "") for x in a.addrs[:4]) + ".c"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    scripts = os.path.join(tc.ROOT, "tools", "ghidra")
    cmd = [headless, project, "vcs", "-process", "-noanalysis", "-readOnly", "-scriptPath", scripts,
           "-postScript", "DecompileList.java", out] + a.addrs
    code = subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if code != 0 or not os.path.isfile(out):
        tc.fail(f"Ghidra failed ({code}); run the command by hand to see why: {' '.join(cmd)}")
    print(out)


if __name__ == "__main__":
    main()
