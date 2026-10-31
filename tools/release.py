#!/usr/bin/env python3
"""Make the Windows release zip: the source, the launcher, Python and the libraries - everything but the game.

    python tools/release.py [--ref TAG | --worktree] [--version X] [--launcher EXE] [--out DIR]

    --ref TAG       the source at a tag or commit (git archive; default HEAD)
    --worktree      the working tree as it is, uncommitted files included (tracked + untracked, not ignored) - tests
    --version X     the release's number (default: VERSION in the repository; a version tag must match it)
    --launcher EXE  a VCSLauncher.exe built elsewhere (CI); default: python tools/build.py --launcher
    --python X.Y.Z  the embeddable Python to bundle (default: the version running this)

Writes build/dist/VCS-Recomp-<version>-windows.zip and its .sha256. Inside, one folder:
    VCSLauncher.exe, VERSION, RELEASE.json (version, repository, file list for tools/update.py),
    python/ (python.org's embeddable CPython), deps/ (tools/deps.py fetch), THIRD_PARTY_LICENSES.txt, the source.
Never game code or data (DECISIONS.md, source-only): before zipping, every file is checked - no disc images, no
PS2 executables (MIPS ELF), nothing from generated/, build/ or the Ghidra project - and the release stops if one is.
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
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deps  # noqa: E402
import toolchain as tc  # noqa: E402

DIST = os.path.join(tc.BUILD, "dist")
CACHE = os.path.join(DIST, "cache")
FORBIDDEN_DIRS = ("generated/", "build/", "ghidra/project/", "ghidra/export/", "mc0/", "mc1/", "SLUS-21590/", "textures/")
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


def build_launcher():
    exe = os.path.join(tc.ROOT, "VCSLauncher.exe")
    if subprocess.run([sys.executable, tc.tool("build.py"), "--launcher"]).returncode != 0:
        tc.fail("the launcher didn't build")
    return exe


def bundle_python(stage, version):
    name = "python-%s-embed-amd64.zip" % version
    cached = os.path.join(CACHE, name)
    if not os.path.isfile(cached):
        url = "https://www.python.org/ftp/python/%s/%s" % (version, name)
        print("[release] downloading " + url)
        os.makedirs(CACHE, exist_ok=True)
        deps.download(url, cached)
    with zipfile.ZipFile(cached) as z:
        z.extractall(os.path.join(stage, "python"))


def bundle_deps(stage):
    cached = os.path.join(CACHE, "deps")
    if deps.check(cached):
        if os.path.isdir(cached):
            shutil.rmtree(cached, onerror=lambda f, p, _: (os.chmod(p, 0o666), f(p)))
        deps.fetch(cached)
    shutil.copytree(cached, os.path.join(stage, "deps"))


def licenses(stage):
    """THIRD_PARTY_LICENSES.txt from the bundled libraries' own licence files (and Python's, FFmpeg's)."""
    parts = []
    for base in sorted(os.listdir(os.path.join(stage, "deps"))):
        folder = os.path.join(stage, "deps", base)
        if not os.path.isdir(folder):
            continue
        for f in sorted(os.listdir(folder)):
            if re.match(r"(?i)(license|licence|copying)(\.(txt|md|rst))?$", f):
                with open(os.path.join(folder, f), encoding="utf-8", errors="replace") as fh:
                    parts.append("=" * 78 + "\n%s (deps/%s/%s)\n" % (base, base, f) + "=" * 78 + "\n" + fh.read())
    for z in (f for f in os.listdir(os.path.join(stage, "deps")) if f.endswith(".zip")):
        with zipfile.ZipFile(os.path.join(stage, "deps", z)) as zf:
            for n in zf.namelist():
                if re.search(r"(?i)(^|/)(license|copying)[^/]*$", n) and not n.endswith("/"):
                    parts.append("=" * 78 + "\nFFmpeg (deps/%s: %s)\n" % (z, n) + "=" * 78 + "\n"
                                 + zf.read(n).decode("utf-8", "replace"))
    py = os.path.join(stage, "python", "LICENSE.txt")
    if os.path.isfile(py):
        with open(py, encoding="utf-8", errors="replace") as fh:
            parts.append("=" * 78 + "\nPython (python/LICENSE.txt)\n" + "=" * 78 + "\n" + fh.read())
    with open(os.path.join(stage, "THIRD_PARTY_LICENSES.txt"), "w", encoding="utf-8") as f:
        f.write("The libraries and tools bundled with this release, under their own licences. The project itself is "
                "GPL-3.0 (LICENSE).\n\n" + "\n\n".join(parts))


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


def self_test(stage):
    """The release's own launcher, with its own Python and scripts, checks itself (VCSLauncher.exe --self-test: the
    scripts it runs are there, the bundled Python runs them, doctor.py answers). A release that fails isn't zipped."""
    out = os.path.join(DIST, "selftest.txt")
    code = subprocess.run([os.path.join(stage, "VCSLauncher.exe"), "--self-test", "--out", out], cwd=stage).returncode
    report = "(no output)"
    if os.path.isfile(out):
        with open(out, encoding="utf-8", errors="replace") as f:
            report = f.read()
        os.remove(out)
    for line in report.splitlines():
        print("[release]   " + line)
    if code != 0:
        tc.fail("the release's launcher failed its self-test (above): nothing was written", "release_selftest")
    # the self-test ran doctor.py, which may leave build/ (the Visual Studio environment cache): not part of a release
    shutil.rmtree(os.path.join(stage, "build"), ignore_errors=True)


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
    ap.add_argument("--launcher")
    ap.add_argument("--python", default="%d.%d.%d" % sys.version_info[:3])
    ap.add_argument("--allow-dirty", action="store_true", help="package --ref even with uncommitted changes")
    ap.add_argument("--out", default=DIST)
    a = ap.parse_args()

    ref = a.ref or "HEAD"
    if not a.worktree and not a.allow_dirty:
        # the launcher is built from the working tree, the source taken from a commit: with uncommitted changes the
        # two don't match (a new launcher next to old scripts - "build tools missing" in the window, 2026-10-30)
        changed = git("status", "--porcelain", "--untracked-files=normal", text=True).splitlines()
        if changed:
            for line in changed[:15]:
                print("[release]   " + line)
            if len(changed) > 15:
                print("[release]   ... and %d more" % (len(changed) - 15))
            tc.fail("uncommitted changes (above): the release would package %s's files next to a launcher built from "
                    "the working tree. Commit them, or use --worktree to package the working tree as it is "
                    "(--allow-dirty to go ahead anyway)" % ref, "release_dirty")
    # VERSION in the repository is the number (the launcher's exe carries it too: launcher/CMakeLists.txt); a version tag
    # must agree with it, or the release would say one number and its launcher another
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
    name = "VCS-Recomp-%s-windows" % version
    stage = os.path.join(DIST, name)
    if os.path.isdir(stage):
        shutil.rmtree(stage, onerror=lambda f, p, _: (os.chmod(p, 0o666), f(p)))
    os.makedirs(stage)

    print("[release] %s: source %s" % (name, "working tree" if a.worktree else ref))
    copy_source(stage, ref, a.worktree)
    with open(os.path.join(stage, "VERSION"), "w", encoding="utf-8") as f:
        f.write(version + "\n")
    launcher = a.launcher or build_launcher()
    shutil.copy2(launcher, os.path.join(stage, "VCSLauncher.exe"))
    print("[release] python %s (embeddable)" % a.python)
    bundle_python(stage, a.python)
    print("[release] libraries (deps/)")
    bundle_deps(stage)
    licenses(stage)

    files = sorted(os.path.relpath(os.path.join(d, f), stage).replace(os.sep, "/")
                   for d, _, fs in os.walk(stage) for f in fs)
    commit = "" if a.worktree else git("rev-parse", ref, text=True).strip()
    with open(os.path.join(stage, "RELEASE.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version, "repository": repository(), "commit": commit, "files": files + ["RELEASE.json"]},
                  f, indent=0)
    guard(stage)
    self_test(stage)

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, name + ".zip")
    print("[release] zipping %d files" % (len(files) + 1))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in files + ["RELEASE.json"]:
            z.write(os.path.join(stage, rel), name + "/" + rel)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    with open(out + ".sha256", "w", encoding="utf-8") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(out)))
    print("[release] ok: %s (%.0f MB, sha256 %s)" % (out, os.path.getsize(out) / (1 << 20), digest[:16]))
    tc.event(result="ok", path=out, version=version)


if __name__ == "__main__":
    main()
