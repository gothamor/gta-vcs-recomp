#!/usr/bin/env python3
"""Headless Ghidra analysis of your SLUS_215.90, exporting the PS2Recomp function map
(ghidra/export/ghidra_export.toml + functions.csv). Only needed to redo the analysis.

    python3 tools/ghidra_analyze.py [build/elf/SLUS_215.90]

Needs Ghidra 12.1.4 with the ghidra-emotionengine-reloaded extension and JDK 21: GHIDRA_HOME (default: Ghidra's
ghidra_12.1.4_PUBLIC folder in the home folder or /opt) and JAVA_HOME (default: the java on the PATH).
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?", default=os.path.join(tc.BUILD, "elf", "SLUS_215.90"))
    ap.add_argument("--max-cpu", type=int, default=6)
    a = ap.parse_args()

    ghidra = tc.ghidra_home()
    env = dict(os.environ)  # JAVA_HOME, else the java on the PATH
    headless = os.path.join(ghidra, "support", "analyzeHeadless")
    if not os.path.isfile(headless):
        tc.fail(f"{headless} not found: set GHIDRA_HOME")
    if not os.path.isfile(a.elf):
        tc.fail(f"{a.elf} not found: python3 tools/recompile.py extracts it from your disc")
    project = os.path.join(tc.ROOT, "ghidra", "project")
    out = os.path.join(tc.ROOT, "ghidra", "export")
    os.makedirs(project, exist_ok=True)
    os.makedirs(out, exist_ok=True)
    scripts = os.path.join(tc.ROOT, "third_party", "PS2Recomp", "ps2xRecomp", "tools", "ghidra")
    cmd = [headless, project, "vcs", "-import", os.path.abspath(a.elf), "-overwrite", "-scriptPath", scripts,
           "-postScript", "ExportPS2Functions.java", os.path.join(out, "ghidra_export.toml"), os.path.join(out, "functions.csv"),
           "-max-cpu", str(a.max_cpu)]
    sys.exit(subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL).returncode)


if __name__ == "__main__":
    main()
