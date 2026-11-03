#!/usr/bin/env python3
"""Make the release zip: the PS5 build kit - the source with PS5-README.txt (the quick start of docs/PS5.md). The game
itself is built from the player's own disc on their build machine.

    python3 tools/release.py [--ref TAG | --worktree] [--version X] [--out DIR]

    --ref TAG       the source at a tag or commit (git archive; default HEAD)
    --worktree      the working tree as it is, uncommitted files included (tracked + untracked, not ignored) - tests
    --version X     the release's number (default: VERSION in the repository; a version tag must match it)

Writes build/dist/VCS-Recomp-<version>-ps5.zip and its .sha256: one folder with the source, PS5-README.txt, VERSION and
RELEASE.json (version, repository, file list). Never game code or data (DECISIONS.md, source-only): before zipping,
every file is checked - no disc images, no PS2 executables (MIPS ELF), nothing from generated/, build/ or the Ghidra
project - and the release stops if one is. Every file in the zip is dated a day before the commit (zip_time): never in
the future, whatever the extractor's zone.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

DIST = os.path.join(tc.BUILD, "dist")
FORBIDDEN_DIRS = ("generated/", "build/", "ghidra/project/", "ghidra/export/", "mc0/", "mc1/")
FORBIDDEN_NAME = re.compile(r"(?i)(\.iso|\.gzf|\.chd|\.cso|\.cue)$|(^|/)(SLUS|SLES|SCUS|SLPM|SLPS)_\d{3}\.\d{2}")


def git(*args, **kw):
    return subprocess.run(["git", "-C", tc.ROOT] + list(args), check=True, capture_output=True, **kw).stdout


def repository():
    """owner/name of origin on GitHub ("" when not)."""
    try:
        url = git("remote", "get-url", "origin", text=True).strip()
    except (subprocess.CalledProcessError, OSError):
        return ""
    m = re.search(r"github\.com[:/]([^/]+/[^/.]+?)(\.git)?$", url)
    return m.group(1) if m else ""


def copy_source(stage, ref, worktree):
    if worktree:
        names = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").split(b"\0")
        for name in filter(None, (n.decode("utf-8") for n in names)):
            src = os.path.join(tc.ROOT, name)
            if not os.path.isfile(src):
                continue  # deleted in the working tree
            dst = os.path.join(stage, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        return
    data = git("archive", "--format=tar", ref)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(stage, filter="data")


def guard(stage):
    """Every file checked for game code or data; the release stops at the first."""
    problems = []
    for d, _, files in os.walk(stage):
        for f in files:
            path = os.path.join(d, f)
            rel = os.path.relpath(path, stage).replace(os.sep, "/")
            if any(rel.startswith(p) for p in FORBIDDEN_DIRS) or FORBIDDEN_NAME.search(rel):
                problems.append(rel + ": a game file's name or folder")
                continue
            size = os.path.getsize(path)
            if size > 200 << 20:
                problems.append(rel + ": %d MB, larger than anything a release carries" % (size >> 20))
                continue
            with open(path, "rb") as fh:
                head = fh.read(20)
            if head[:4] == b"\x7fELF" and len(head) >= 20 and int.from_bytes(head[18:20], "little") == 8:
                problems.append(rel + ": a MIPS ELF (a PS2 executable)")
            elif head[:6] == b"\x01CD001" or (size > 32768 + 6 and _iso_at(path)):
                problems.append(rel + ": a disc image")
    if problems:
        for p in problems:
            print("[release] REFUSED " + p)
        tc.fail("game files in the release (above): nothing was written", "release_refused")


PS5_README = """{title}
{underline}

For jailbroken PS5 consoles that run homebrew from /data/homebrew/. This zip is the port's source: no game code or
data is in it, and none ever is. You build the game from your own disc (USA, SLUS-21590) on a build machine with
Ubuntu 24.04, WSL 2 or Fedora 41+, and the result - an app folder - goes to your PS5.

0. The packages, once (Ubuntu / WSL 2, or Fedora):
       sudo apt install python3 git make wget unzip xz-utils rsync meson ninja-build glslang-tools bison flex gcc g++ curl clang-20 lld-20 llvm-20 cmake python3-mako python3-ply python3-yaml
       sudo dnf install python3 git make wget unzip xz tar rsync meson ninja-build glslang bison flex gcc gcc-c++ curl clang lld llvm llvm-devel cmake python3-mako python3-ply python3-pyyaml
1. The game's code from your disc:
       python3 tools/recompile.py "Grand Theft Auto - Vice City Stories (USA).iso"
2. The PS5 toolchain, once (ps5-plume-triangle: the PS5 payload SDK, its Vulkan driver RADV, the eboot tools):
       python3 tools/ps5.py setup
3. Check, then build:
       python3 tools/ps5.py check
       python3 tools/ps5.py build --zip          -> build/ps5/app/PPSA94590/ and build/ps5/PPSA94590.zip
4. To the console, over an FTP payload (etaHEN: --port 1337):
       python3 tools/ps5.py deploy --host <PS5 IP> --iso "<your>.iso"
   or unzip build/ps5/PPSA94590.zip into /data/homebrew/ (USB), with your .iso in PPSA94590/disc/ or on a USB drive.
5. Start it from the PS5's home screen.

Controls (the DualSense is the DualShock 2, button for button):
   Cross / Circle / Square / Triangle, L1 / R1, L2 / R2 (the triggers), L3 / R3, both sticks, the d-pad: as on the PS2
   Options = Start          touchpad click = Select (the Create button belongs to the system)
   touchpad + Options = the settings menu      touchpad + R1 = the trainer      touchpad + L1 = the free camera
   In the menus: Cross selects, Circle backs out, L1 / R1 change pages.

The app folder you build holds the game's code made from your disc: copy it to your own console, never publish it.
Everything else - what is tested, the first-run checklist, how it works - is in docs/PS5.md.
"""


def zip_time(ref, worktree):
    """The one date every file in the zip carries. Zip dates are a wall-clock time with no time zone: the extractor's
    zone reads them, so a file packed at 23:44 in Adelaide came out 13.5 h in the future in Brazil, and Ninja re-ran
    CMake until it gave up (2026-10-31). The commit's time (SOURCE_DATE_EPOCH when set; now for --worktree) a day
    earlier, written as UTC, is in the past in every zone (UTC-12 to UTC+14) - and the same for every file and run."""
    if os.environ.get("SOURCE_DATE_EPOCH"):
        when = int(os.environ["SOURCE_DATE_EPOCH"])
    elif worktree:
        when = int(time.time())
    else:
        when = int(git("log", "-1", "--format=%ct", ref, text=True).strip())
    return max(time.gmtime(when - 86400)[:6], (1980, 1, 1, 0, 0, 0))


def _iso_at(path):
    with open(path, "rb") as fh:
        fh.seek(16 * 2048)
        return fh.read(6) == b"\x01CD001"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--ref", default=None)
    src.add_argument("--worktree", action="store_true")
    ap.add_argument("--version")
    ap.add_argument("--allow-dirty", action="store_true", help="package --ref even with uncommitted changes")
    ap.add_argument("--out", default=DIST)
    ap.add_argument("--ps5", action="store_true", help="(the only kind of release: accepted for older instructions)")
    a = ap.parse_args()

    ref = a.ref or "HEAD"
    if not a.worktree and not a.allow_dirty:
        # the source is taken from a commit: uncommitted changes would not be in it
        changed = git("status", "--porcelain", "--untracked-files=normal", text=True).splitlines()
        if changed:
            for line in changed[:15]:
                print("[release]   " + line)
            if len(changed) > 15:
                print("[release]   ... and %d more" % (len(changed) - 15))
            tc.fail("uncommitted changes (above): the release packages %s's files without them. Commit them, or use "
                    "--worktree to package the working tree as it is (--allow-dirty to go ahead anyway)" % ref,
                    "release_dirty")
    # VERSION in the repository is the number; a version tag must agree with it
    if a.worktree:
        repo_version = tc.source_version()["version"]
    else:
        try:
            repo_version = git("show", ref + ":VERSION", text=True).strip()
        except subprocess.CalledProcessError:
            repo_version = ""
    tag_version = re.sub(r"^v", "", a.ref) if a.ref and re.match(r"v?\d+\.\d+", a.ref) else ""
    if tag_version and repo_version and tag_version != repo_version and not a.version:
        tc.fail("the tag says %s, VERSION says %s: bump VERSION (and commit) before tagging" % (tag_version, repo_version),
                "release_version")
    version = a.version or tag_version or repo_version or "0.0.0-dev"
    name = "VCS-Recomp-%s-ps5" % version
    stage = os.path.join(DIST, name)
    if os.path.isdir(stage):
        shutil.rmtree(stage, onerror=lambda f, p, _: (os.chmod(p, 0o666), f(p)))
    os.makedirs(stage)

    print("[release] %s: source %s" % (name, "working tree" if a.worktree else ref))
    copy_source(stage, ref, a.worktree)
    with open(os.path.join(stage, "VERSION"), "w", encoding="utf-8") as f:
        f.write(version + "\n")
    # the build machine has its own Python, and needs the internet anyway (ps5-plume-triangle's setup downloads the SDK
    # and Mesa): nothing bundled
    with open(os.path.join(stage, "PS5-README.txt"), "w", encoding="utf-8") as f:
        title = "GTA: Vice City Stories - native PS5 port, build kit %s" % version
        f.write(PS5_README.format(title=title, underline="=" * len(title)))

    files = sorted(os.path.relpath(os.path.join(d, f), stage).replace(os.sep, "/")
                   for d, _, fs in os.walk(stage) for f in fs)
    commit = "" if a.worktree else git("rev-parse", ref, text=True).strip()
    with open(os.path.join(stage, "RELEASE.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version, "repository": repository(), "commit": commit, "files": files + ["RELEASE.json"]},
                  f, indent=0)
    guard(stage)
    # its tools answer from the kit itself (Python 3, standard library only)
    for tool in ("ps5.py", "recompile.py", "release.py"):
        if subprocess.run([sys.executable, os.path.join(stage, "tools", tool), "--help"], capture_output=True).returncode:
            tc.fail("the kit's tools/%s does not start: nothing was written" % tool, "release_selftest")

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, name + ".zip")
    when = zip_time(ref, a.worktree)
    print("[release] zipping %d files, dated %04d-%02d-%02d %02d:%02d:%02d" % ((len(files) + 1,) + when))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in files + ["RELEASE.json"]:
            info = zipfile.ZipInfo.from_file(os.path.join(stage, rel), name + "/" + rel)
            info.date_time = when
            with open(os.path.join(stage, rel), "rb") as f:
                z.writestr(info, f.read(), zipfile.ZIP_DEFLATED, 9)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(out)))
    print("[release] ok: %s (%.0f MB, sha256 %s)" % (out, os.path.getsize(out) / (1 << 20), digest[:16]))


if __name__ == "__main__":
    main()
