#!/usr/bin/env python3
"""Back up and restore the memory card (your saves). Standard library only.

    python tools/backup.py create [--name NAME]   the card (mc0, mc1) into <data>/backups/saves-<date>.zip
    python tools/backup.py list                   the backups, newest first
    python tools/backup.py restore NAME           the card replaced by a backup (the card as it is goes into a backup
                                                  first); refused while the game runs unless --force

<data> is VCS_DATA_DIR or the game's own folder (%LOCALAPPDATA%\\VCSRecomp, ~/.local/share/VCSRecomp). The card's
order matters to the game (docs/game-internals.md 10c): each save folder's `.ps2x-order` file is backed up with it,
and a restore puts back every file's modification and (Windows) creation time, in the original creation order.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

CARDS = ("mc0", "mc1")
MANIFEST = "backup.json"


def data_dir():
    if os.environ.get("VCS_DATA_DIR"):
        return os.environ["VCS_DATA_DIR"]
    if tc.IS_WINDOWS:
        return os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "VCSRecomp")
    xdg = os.environ.get("XDG_DATA_HOME", "")
    return os.path.join(xdg if xdg.startswith("/") else os.path.expanduser("~/.local/share"), "VCSRecomp")


def backups_dir():
    return os.path.join(data_dir(), "backups")


def creation_time(path):
    st = os.stat(path)
    return getattr(st, "st_birthtime", st.st_ctime if tc.IS_WINDOWS else st.st_mtime)


def card_entries():
    """(relative path, is_dir, creation time, modification time) of everything on the cards, in creation order."""
    entries = []
    root = data_dir()
    for card in CARDS:
        base = os.path.join(root, card)
        if not os.path.isdir(base):
            continue
        for d, dirs, files in os.walk(base):
            for name in dirs + files:
                path = os.path.join(d, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                entries.append((rel, os.path.isdir(path), creation_time(path), os.path.getmtime(path)))
        entries.append((card, True, creation_time(base), os.path.getmtime(base)))
    return sorted(entries, key=lambda e: (e[2], e[0].count("/"), e[0]))


def count_saves(names):
    """Save slot files ("1 In the beginning") in a list of card paths."""
    return sum(1 for n in names if "/" in n and n.rsplit("/", 1)[1][:1].isdigit() and n.rsplit("/", 1)[1][1:2] == " ")


def create(name=None, quiet=False):
    entries = card_entries()
    if not entries:
        tc.fail("no memory card in " + data_dir() + " (the game hasn't saved yet)", "no_card")
    os.makedirs(backups_dir(), exist_ok=True)
    name = name or "saves-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.join(backups_dir(), name + ".zip")
    manifest = {"created": int(time.time()), "source": data_dir(),
                "entries": [{"path": p, "dir": d, "ctime": c, "mtime": m} for p, d, c, m in entries]}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(MANIFEST, json.dumps(manifest, indent=1))
        for path, is_dir, _, _ in entries:
            full = os.path.join(data_dir(), path)
            if is_dir:
                z.writestr(path + "/", b"")
            else:
                z.write(full, path)
    saves = count_saves([e[0] for e in entries if not e[1]])
    if not quiet:
        print("[backup] %s: %d files, %d saves" % (out, sum(1 for e in entries if not e[1]), saves))
    tc.event(result="ok", path=out, name=name, saves=saves)
    return out


def listing():
    found = []
    if os.path.isdir(backups_dir()):
        for f in os.listdir(backups_dir()):
            if not f.endswith(".zip"):
                continue
            path = os.path.join(backups_dir(), f)
            try:
                with zipfile.ZipFile(path) as z:
                    manifest = json.loads(z.read(MANIFEST))
                    saves = count_saves([e["path"] for e in manifest["entries"] if not e["dir"]])
            except (OSError, KeyError, ValueError, zipfile.BadZipFile):
                continue
            found.append({"name": f[:-4], "path": path, "time": manifest.get("created", 0), "saves": str(saves) + " saves",
                          "size": os.path.getsize(path)})
    found.sort(key=lambda b: b["time"], reverse=True)
    for b in found:
        print("%-28s %s  %s" % (b["name"], datetime.datetime.fromtimestamp(b["time"]).strftime("%Y-%m-%d %H:%M"), b["saves"]))
    if not found:
        print("[backup] no backups in " + backups_dir())
    tc.event(result="ok", backups=found)


def game_running():
    if not tc.IS_WINDOWS:
        return subprocess.run(["pgrep", "-x", "vcs"], capture_output=True).returncode == 0
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vcs.exe", "/NH"], capture_output=True, text=True).stdout
    return "vcs.exe" in out.lower()


def set_times(path, ctime, mtime, is_dir):
    os.utime(path, (mtime, mtime))
    if not tc.IS_WINDOWS:
        return  # Linux has no way to set a birth time; the order files carry the order
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.DWORD, wintypes.HANDLE]
    handle = k32.CreateFileW(path, 0x100, 0x7, None, 3, 0x02000000 if is_dir else 0x80, None)  # FILE_WRITE_ATTRIBUTES
    if handle in (None, wintypes.HANDLE(-1).value):
        return

    def filetime(t):
        v = int(t * 10_000_000) + 116444736000000000
        return wintypes.FILETIME(v & 0xFFFFFFFF, v >> 32)

    c, m = filetime(ctime), filetime(mtime)
    k32.SetFileTime(handle, ctypes.byref(c), None, ctypes.byref(m))
    k32.CloseHandle(handle)


def restore(name, force=False):
    path = os.path.join(backups_dir(), name if name.endswith(".zip") else name + ".zip")
    if not os.path.isfile(path):
        tc.fail("no backup " + path, "no_backup")
    if game_running() and not force:
        tc.fail("the game is running: quit it first (it keeps the card open)", "game_running")
    with zipfile.ZipFile(path) as z:
        manifest = json.loads(z.read(MANIFEST))
        if card_entries():
            safety = create("before-restore-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"), quiet=True)
            print("[backup] the card as it was: " + safety)
        for card in CARDS:
            shutil.rmtree(os.path.join(data_dir(), card), ignore_errors=True)
        entries = manifest["entries"]
        for e in entries:  # creation order: folders and files come back in the order they were made
            target = os.path.join(data_dir(), *e["path"].split("/"))
            if not os.path.abspath(target).startswith(os.path.abspath(data_dir()) + os.sep):
                continue  # never outside the data folder
            if e["dir"]:
                os.makedirs(target, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(z.read(e["path"]))
        for e in reversed(entries):  # times last, deepest first, so writing a file doesn't touch its folder's again
            target = os.path.join(data_dir(), *e["path"].split("/"))
            if os.path.exists(target):
                set_times(target, e["ctime"], e["mtime"], e["dir"])
    saves = count_saves([e["path"] for e in entries if not e["dir"]])
    print("[backup] restored %s: %d saves" % (os.path.basename(path), saves))
    tc.event(result="ok", name=name, saves=saves)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create")
    c.add_argument("--name")
    sub.add_parser("list")
    r = sub.add_parser("restore")
    r.add_argument("name")
    r.add_argument("--force", action="store_true")
    a = ap.parse_args()
    if a.cmd == "create":
        create(a.name)
    elif a.cmd == "list":
        listing()
    else:
        restore(a.name, a.force)


if __name__ == "__main__":
    main()
