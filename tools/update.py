#!/usr/bin/env python3
"""Update a release folder to the newest release on GitHub. Only when asked; nothing is sent but the request itself.

    python tools/update.py check               the newest release vs this one (RELEASE.json's version)
    python tools/update.py apply [--zip FILE]  download the newest release's zip (or use FILE) and put it in place

apply replaces the release's files with the new release's, removes the files the old release had and the new one
doesn't (RELEASE.json lists them), and keeps everything else: build/ (the built game, vcs.ini, the launcher's
settings), generated/, your disc, anything you added. The game then needs building again (the launcher says so;
it is incremental). A git clone isn't a release: update it with git pull.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import urllib.error
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

RELEASE_JSON = os.path.join(tc.ROOT, "RELEASE.json")
UPDATE_DIR = os.path.join(tc.BUILD, "update")
KEEP = ("build/", "generated/")  # never replaced or removed


def this_release():
    try:
        with open(RELEASE_JSON, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def version_key(v):
    return [int(x) for x in re.findall(r"\d+", v)] or [0]


def latest(repo):
    req = urllib.request.Request("https://api.github.com/repos/%s/releases/latest" % repo,
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "vcs-recomp-update"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None  # no release published yet
        tc.fail("GitHub answered %d" % e.code, "network")
    except (OSError, ValueError) as e:
        tc.fail("could not reach GitHub: %s" % e, "network")
    assets = {a["name"]: a["browser_download_url"] for a in data.get("assets", [])}
    zips = [n for n in assets if n.endswith("-windows.zip")]
    return {"version": re.sub(r"^v", "", data.get("tag_name", "")), "url": data.get("html_url", ""),
            "zip": assets[zips[0]] if zips else "", "sha256": assets.get(zips[0] + ".sha256", "") if zips else ""}


def check():
    mine = this_release()
    if not mine:
        message = ("This is a git clone, not a release: update it with git pull, then build again."
                   if os.path.exists(os.path.join(tc.ROOT, ".git")) else "No RELEASE.json: not a release folder.")
        print("[update] " + message)
        tc.event(result="ok", newer=False, message=message)
        return
    repo = mine.get("repository") or ""
    if not repo:
        tc.fail("RELEASE.json names no repository", "error")
    found = latest(repo)
    if not found or not found["version"]:
        message = "No release published yet on github.com/%s." % repo
        print("[update] " + message)
        tc.event(result="ok", newer=False, message=message)
        return
    newer = version_key(found["version"]) > version_key(mine["version"])
    print("[update] this: %s, newest: %s%s" % (mine["version"], found["version"], " - an update is available" if newer else ""))
    tc.event(result="ok", newer=newer, current=mine["version"], latest=found["version"], url=found["url"],
             zip=found["zip"], message="" if newer else "You have the latest release (%s)." % mine["version"])


def set_aside(path):
    """A file in use (a running exe or DLL can be renamed, not replaced): moved to <name>.old, removed next time."""
    old = path + ".old"
    if os.path.exists(old):
        os.remove(old)
    os.replace(path, old)


def remove_set_aside():
    """The .old files a previous update couldn't remove (they were running then)."""
    for d in (tc.ROOT, os.path.join(tc.ROOT, "python")):
        if os.path.isdir(d):
            for f in os.listdir(d):
                if f.endswith(".old"):
                    try:
                        os.remove(os.path.join(d, f))
                    except OSError:
                        pass


def download(url, out):
    tc.stage("download", "Downloading the new release")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with urllib.request.urlopen(url, timeout=60) as r, open(out + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            tc.event(progress=done >> 20, total=total >> 20, eta=None, note="MB")
    os.replace(out + ".part", out)


def apply(zip_path=None):
    mine = this_release()
    if not mine:
        tc.fail("not a release folder (no RELEASE.json): a git clone updates with git pull", "git_checkout")
    if not zip_path:
        found = latest(mine.get("repository", ""))
        if not found or not found["zip"]:
            tc.fail("no release zip found on GitHub", "network")
        if version_key(found["version"]) <= version_key(mine["version"]):
            print("[update] already the newest (%s)" % mine["version"])
            tc.event(result="ok", version=mine["version"])
            return
        zip_path = os.path.join(UPDATE_DIR, found["zip"].rsplit("/", 1)[1])
        download(found["zip"], zip_path)
        if found["sha256"]:
            with urllib.request.urlopen(found["sha256"], timeout=30) as r:
                want = r.read().decode().split()[0].lower()
            got = hashlib.sha256(open(zip_path, "rb").read()).hexdigest()
            if got != want:
                os.remove(zip_path)
                tc.fail("the download's checksum doesn't match: try again", "network")
    tc.stage("apply", "Putting the new release in place")
    with zipfile.ZipFile(zip_path) as z:
        top = {n.split("/", 1)[0] for n in z.namelist()}
        if len(top) != 1:
            tc.fail("not a release zip (one folder expected): " + zip_path, "error")
        prefix = top.pop() + "/"
        new = json.loads(z.read(prefix + "RELEASE.json"))
        names = [n for n in z.namelist() if not n.endswith("/")]
        for i, n in enumerate(names):
            rel = n[len(prefix):]
            if not rel or rel.startswith(KEEP):
                continue
            target = os.path.join(tc.ROOT, *rel.split("/"))
            if not os.path.abspath(target).startswith(os.path.abspath(tc.ROOT) + os.sep):
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            try:
                with z.open(n) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
            except PermissionError:
                set_aside(target)  # running: the launcher, this Python - renamed, then written fresh
                with z.open(n) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
            if i % 200 == 0:
                tc.event(progress=i, total=len(names), eta=None, note="files")
    gone = sorted(set(mine.get("files", [])) - set(new.get("files", [])))
    for rel in gone:
        if rel.startswith(KEEP):
            continue
        path = os.path.join(tc.ROOT, *rel.split("/"))
        if os.path.isfile(path):
            try:
                os.remove(path)
            except PermissionError:
                set_aside(path)
    print("[update] %s -> %s (%d files, %d removed). Build the game again (the launcher, or python tools/build.py "
          "--recompile-if-needed)." % (mine["version"], new["version"], len(names), len(gone)))
    tc.event(result="ok", version=new["version"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    p = sub.add_parser("apply")
    p.add_argument("--zip", help="a release zip to apply instead of downloading the newest")
    a = ap.parse_args()
    remove_set_aside()
    if a.cmd == "check":
        check()
    else:
        apply(a.zip)


if __name__ == "__main__":
    main()
