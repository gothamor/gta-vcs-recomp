#!/usr/bin/env python3
"""Make VCS.app (macOS) from the built game.

    python3 tools/package_mac.py [--out VCS.app]

Copies build/vcs and build/vcs.ini (the disc path) into a .app bundle, writes Info.plist, and ad-hoc signs it.
The game keeps its settings and memory cards in ~/Library/Application Support/VCSRecomp, so the bundle itself is
replaceable. docs/MACOS.md has the build steps.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

INFO = {
    "CFBundleDisplayName": "GTA: Vice City Stories",
    "CFBundleName": "VCS",
    "CFBundleExecutable": "vcs",
    "CFBundleIdentifier": "org.vcsrecomp.macos",
    "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "0.0.2",
    "CFBundleVersion": "1",
    "LSMinimumSystemVersion": "12.0",
    "NSHighResolutionCapable": True,
    "LSApplicationCategoryType": "public.app-category.action-games",
}

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(tc.BUILD, "VCS.app"))
    ap.add_argument("--diagnostics", action="store_true",
                    help="also make <name>-diagnostics.app: console log + control channel, for bug reports")
    a = ap.parse_args()
    if not sys.platform == "darwin":
        tc.fail("package_mac.py makes a macOS bundle; build the game first (docs/MACOS.md)")
    exe = tc.VCS_EXE
    if not os.path.isfile(exe):
        tc.fail("not built yet: python3 tools/build.py --recompile")
    out = os.path.abspath(a.out)
    macos = os.path.join(out, "Contents", "MacOS")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(macos)
    shutil.copy2(exe, os.path.join(macos, "vcs"))
    ini = os.path.join(os.path.dirname(exe), "vcs.ini")
    if os.path.isfile(ini):
        shutil.copy2(ini, os.path.join(macos, "vcs.ini"))
    with open(os.path.join(out, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(INFO, f)
    # ad-hoc signature: Gatekeeper on another Mac still warns, but the local one runs it
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", out], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if a.diagnostics:
        diag = out[:-4] + "-diagnostics.app" if out.endswith(".app") else out + "-diagnostics.app"
        shutil.rmtree(diag, ignore_errors=True)
        shutil.copytree(out, diag)
        mac = os.path.join(diag, "Contents", "MacOS")
        os.replace(os.path.join(mac, "vcs"), os.path.join(mac, "vcs-bin"))
        wrapper = os.path.join(mac, "vcs")
        with open(wrapper, "w", encoding="utf-8") as f:
            f.write("#!/bin/sh\n"
                    "# Diagnostics launcher: the game's console output to ~/Library/Logs/VCSRecomp.log, the control\n"
                    "# channel on (tools/gamectl.py can drive it). Send the log with a bug report.\n"
                    "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                    "export PS2X_CONTROL=1\n"
                    "export PS2X_PERF_LOG=1\n"
                    "exec \"$DIR/vcs-bin\" >\"$HOME/Library/Logs/VCSRecomp.log\" 2>&1\n")
        os.chmod(wrapper, 0o755)
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", diag], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("[package] " + tc.show(diag) + " (log: ~/Library/Logs/VCSRecomp.log, control channel on)")
    print("[package] " + tc.show(out))
    print("[package] run it with: open \"%s\", or the binary %s" % (out, tc.show(os.path.join(macos, "vcs"))))

if __name__ == "__main__":
    main()
