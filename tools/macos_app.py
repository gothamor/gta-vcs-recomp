#!/usr/bin/env python3
"""macOS: the game as an app - "Vice City Stories.app", started from the Dock, Launchpad, Spotlight or Finder like any
other (docs/MACOS.md).

    python3 tools/macos_app.py              makes build/Vice City Stories.app and puts a copy in ~/Applications
    python3 tools/macos_app.py --no-install only build/Vice City Stories.app
    python3 tools/macos_app.py --check      says whether the app is there and points at this build (exit 1 if not)

The app holds no game: its Contents/MacOS/vcs is a link to the build's game (build/vcs), which it starts with the log
in ~/Library/Logs/VCSRecomp/play.log (the previous one kept as play.prev.log). Started through the link, macOS takes
the process for the app (its name in the menu bar, its icon in the Dock), and the game still finds vcs.ini and the disc
next to build/vcs (src/main.cpp resolves the link). So a rebuild needs no new app; a build folder that moves does
(tools/build.py makes the app again after every macOS build).

The icon is the launcher's (launcher/make_icon.py), drawn at 824 px on a 1024 px canvas as macOS icons are, then made
into an .icns with sips and iconutil (both part of macOS). It is kept in build/macos-icon and drawn once.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile

import toolchain as tc

APP_NAME = "Vice City Stories"
BUNDLE_ID = "local.vcs-recomp.game"  # a local build's own id (no store, no signing)
APP = os.path.join(tc.BUILD, APP_NAME + ".app")
INSTALLED = os.path.join(os.path.expanduser("~/Applications"), APP_NAME + ".app")
ICON_DIR = os.path.join(tc.BUILD, "macos-icon")
ICNS = os.path.join(ICON_DIR, "AppIcon.icns")
LOG_DIR = os.path.expanduser("~/Library/Logs/VCSRecomp")

# Contents/MacOS/vcs-launch: what macOS runs. Shell, because it only sets up the log and replaces itself with the game
# (exec: one process, the app's). A missing build says so in a dialog instead of failing silently.
LAUNCH_SCRIPT = r'''#!/bin/sh
# Made by tools/macos_app.py - starts the game this app was made for (Contents/MacOS/vcs links to it).
here="$(cd "$(dirname "$0")" && pwd)"
game="$here/vcs"
if [ ! -x "$game" ]; then
    /usr/bin/osascript -e 'display alert "Vice City Stories" message "The game is not built (or its folder moved). In the source folder run: python3 tools/build.py" as critical' >/dev/null 2>&1
    exit 1
fi
logs="$HOME/Library/Logs/VCSRecomp"
mkdir -p "$logs"
[ -f "$logs/play.log" ] && mv -f "$logs/play.log" "$logs/play.prev.log"
exec "$game" "$@" >"$logs/play.log" 2>&1
'''


def make_icon():
    """build/macos-icon/AppIcon.icns from launcher/make_icon.py's drawing (once: it takes a minute in pure Python)."""
    if os.path.isfile(ICNS):
        return ICNS
    sys.path.insert(0, os.path.join(tc.ROOT, "launcher"))
    import make_icon
    os.makedirs(ICON_DIR, exist_ok=True)
    print("[app] drawing the icon (once, about a minute)")
    art = os.path.join(ICON_DIR, "art_824.png")
    with open(art, "wb") as f:
        f.write(make_icon.render(824))
    master = os.path.join(ICON_DIR, "icon_1024.png")
    # macOS's icon grid: the shape 824 px inside a 1024 px canvas, the rest transparent
    subprocess.run(["sips", "--padToHeightWidth", "1024", "1024", art, "--out", master], check=True,
                   capture_output=True)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = "icon_%dx%d%s.png" % (size, size, "@2x" if scale == 2 else "")
                subprocess.run(["sips", "-z", str(px), str(px), master, "--out", os.path.join(iconset, name)],
                               check=True, capture_output=True)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", ICNS], check=True)
    return ICNS


def info_plist():
    with open(os.path.join(tc.ROOT, "VERSION"), encoding="utf-8") as f:
        version = f.read().strip()
    return {
        "CFBundleName": APP_NAME,
        "CFBundleDisplayName": APP_NAME,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleExecutable": "vcs-launch",
        "CFBundleIconFile": "AppIcon",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "CFBundleInfoDictionaryVersion": "6.0",
        "LSMinimumSystemVersion": "13.0",
        "LSApplicationCategoryType": "public.app-category.games",  # Game Mode in full screen
        "LSArchitecturePriority": ["arm64"],
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": True,
    }


def make_app(game):
    """build/Vice City Stories.app for `game` (made again from scratch: nothing in it is the player's)."""
    contents = os.path.join(APP, "Contents")
    if os.path.isdir(APP):
        shutil.rmtree(APP)
    os.makedirs(os.path.join(contents, "MacOS"))
    os.makedirs(os.path.join(contents, "Resources"))
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(info_plist(), f)
    with open(os.path.join(contents, "PkgInfo"), "w", encoding="ascii") as f:
        f.write("APPL????")
    launch = os.path.join(contents, "MacOS", "vcs-launch")
    with open(launch, "w", encoding="utf-8", newline="\n") as f:
        f.write(LAUNCH_SCRIPT)
    os.chmod(launch, 0o755)
    os.symlink(game, os.path.join(contents, "MacOS", "vcs"))
    shutil.copyfile(make_icon(), os.path.join(contents, "Resources", "AppIcon.icns"))
    return APP


def install():
    """A copy in ~/Applications (the user's own: no password), replacing an older one; Launchpad and Spotlight
    pick it up from there."""
    os.makedirs(os.path.dirname(INSTALLED), exist_ok=True)
    if os.path.lexists(INSTALLED):
        if not os.path.isfile(os.path.join(INSTALLED, "Contents", "MacOS", "vcs-launch")):
            tc.fail("%s is there and isn't this tool's app: not replacing it" % INSTALLED, "app_exists")
        shutil.rmtree(INSTALLED)
    shutil.copytree(APP, INSTALLED, symlinks=True)
    # Finder and the Dock read the icon from LaunchServices: tell it about the new bundle
    lsregister = ("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/"
                  "lsregister")
    if os.path.isfile(lsregister):
        subprocess.run([lsregister, "-f", INSTALLED], capture_output=True)
    return INSTALLED


def check():
    link = os.path.join(INSTALLED, "Contents", "MacOS", "vcs")
    target = os.path.realpath(link) if os.path.islink(link) else None
    ok = target is not None and os.path.realpath(tc.VCS_EXE) == target and os.path.isfile(target)
    print("[app] %s: %s" % (tc.show(INSTALLED), "starts %s" % tc.show(target) if ok else
                            "missing or not this build - python3 tools/macos_app.py"))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--no-install", action="store_true", help="only build/%s.app" % APP_NAME)
    ap.add_argument("--check", action="store_true", help="is the installed app there and this build's")
    a = ap.parse_args()
    if not tc.IS_MACOS:
        tc.fail("the app is for macOS (Windows / Linux: the launcher, python tools/build.py --launcher)", "unsupported")
    if a.check:
        sys.exit(0 if check() else 1)
    game = os.path.realpath(tc.VCS_EXE)
    if not os.path.isfile(game):
        tc.fail("the game isn't built yet: python3 tools/build.py --recompile", "not_built")
    make_app(game)
    print("[app] made " + tc.show(APP))
    if not a.no_install:
        print("[app] installed " + install() + " (Launchpad, Spotlight, Finder > Applications)")
    print("[app] the game's log: " + os.path.join(LOG_DIR, "play.log"))
    tc.event(result="ok", app=INSTALLED if not a.no_install else APP)


if __name__ == "__main__":
    main()
