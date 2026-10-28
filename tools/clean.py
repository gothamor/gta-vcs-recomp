#!/usr/bin/env python3
"""Delete test output in build/ (screenshots, logs, card copies, training data) - everything but what is listed below.

    python tools/clean.py                      list what would go, by folder, with sizes (nothing is deleted)
    python tools/clean.py --apply              delete it
    python tools/clean.py --apply --keep-days 7    ... but keep anything changed in the last 7 days

Kept always (PROTECTED): the game at the top of build/ (vcs.exe, its DLLs, vcs.pdb, vcs.ini), the builds (release,
tools, analyzer, iop_tests), the ELF / VU1 extracts, analysis dumps,
the rig's card copies (build/rig/saves), the regression baseline, the reference scenes capture
(build/scenes/reference), the PGO profile and the cached compiler environment. Outside build/ nothing is touched:
not the disc images, mc0 / mc1, generated/ or ghidra/. New test folders under build/ need no entry here - they are
test output unless protected.
"""
import argparse
import os
import shutil
import stat
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

PROTECTED = [
    "release", "tools", "analyzer", "iop_tests", "elf", "vu1", "analysis",
    "rig/saves", "regress/baseline", "scenes/reference", "pgo/vcs.profdata", "msvc_env.json",
]


def norm(path):
    return os.path.normcase(os.path.abspath(path))


def size_of(path):
    if os.path.isfile(path):
        return os.path.getsize(path)
    total = 0
    for base, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(base, name))
            except OSError:
                pass
    return total


def newest(path):
    """Latest modification time in `path` (a file, or anything inside a folder)."""
    latest = os.path.getmtime(path)
    if os.path.isdir(path):
        for base, _, files in os.walk(path):
            for name in files:
                try:
                    latest = max(latest, os.path.getmtime(os.path.join(base, name)))
                except OSError:
                    pass
    return latest


def writable_retry(func, path, _):
    """rmtree's error handler: read-only files (git packs of fetched dependencies) are made writable and retried."""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def is_game_file(name):
    """The game at the top of build/: vcs.exe, its symbols, its DLLs and the player's vcs.ini (build.py)."""
    lower = name.lower()
    return lower in ("vcs", "vcs.exe", "vcs.pdb", "vcs.ini") or lower.endswith(".dll")


def candidates(build, protected, keep_after):
    """(path, bytes) to delete: build/'s entries, descending into folders that hold something protected."""
    out = []

    def visit(folder):
        for name in sorted(os.listdir(folder)):
            path = os.path.join(folder, name)
            p = norm(path)
            if p in protected:
                continue
            if any(q.startswith(p + os.sep) for q in protected):
                visit(path)  # keep the protected part, look at the rest
                continue
            if folder == build and is_game_file(name):
                continue
            if keep_after and newest(path) >= keep_after:
                continue
            out.append((path, size_of(path)))

    visit(build)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="delete (default: only list)")
    ap.add_argument("--keep-days", type=float, default=0.0, help="keep anything changed in the last N days")
    a = ap.parse_args()

    build = tc.BUILD
    if not os.path.isdir(build):
        print("nothing to clean: no build/")
        return
    protected = {norm(os.path.join(build, p)) for p in PROTECTED}
    keep_after = time.time() - a.keep_days * 86400.0 if a.keep_days > 0 else 0.0
    found = candidates(build, protected, keep_after)

    by_top = {}
    for path, size in found:
        top = os.path.relpath(path, build).split(os.sep)[0]
        count, total = by_top.get(top, (0, 0))
        by_top[top] = (count + 1, total + size)
    small_count, small_total = 0, 0
    for top, (count, total) in sorted(by_top.items(), key=lambda kv: -kv[1][1]):
        if total < 2**20 and not os.path.isdir(os.path.join(build, top)):
            small_count, small_total = small_count + 1, small_total + total
            continue
        print("%10.1f MB  build/%s%s" % (total / 2**20, top, "  (%d entries)" % count if count > 1 else ""))
    if small_count:
        print("%10.1f MB  %d small files in build/ (logs, notes)" % (small_total / 2**20, small_count))
    grand = sum(size for _, size in found)
    print("%10.1f MB  total, %s" % (grand / 2**20, "deleting" if a.apply else "nothing deleted (--apply to delete)"))
    if not a.apply:
        return
    failed = 0
    for path, _ in found:
        try:
            if os.path.isdir(path) and not os.path.islink(path):
                if sys.version_info >= (3, 12):
                    shutil.rmtree(path, onexc=writable_retry)
                else:
                    shutil.rmtree(path, onerror=writable_retry)
            else:
                os.remove(path)
        except OSError as e:
            failed += 1
            print("could not delete %s: %s" % (os.path.relpath(path, tc.ROOT), e))
    print("done%s" % (", %d not deleted" % failed if failed else ""))


if __name__ == "__main__":
    main()
