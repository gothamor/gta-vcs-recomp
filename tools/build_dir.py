#!/usr/bin/env python3
"""Where the game is built: build/ in this folder, or a folder of your choice (another drive with more room...).

    python tools/build_dir.py               where it is now
    python tools/build_dir.py check PATH    what choosing PATH would mean (the folder used, free space, warnings, what
                                            stops it) - changes nothing
    python tools/build_dir.py set PATH      build in PATH from now on
    python tools/build_dir.py reset         back to build/ in this folder

PATH can be an empty or new folder (used as it is), a build folder made before (used again), or any other folder - a
"VCS-build" folder is made inside it, so the build's files never mix with others. Not inside this source folder
(other than build/ itself, which is `reset`).

The choice is kept in .vcs-build-dir in this folder (VCS_BUILD_DIR overrides it for one run); every tool follows it
(toolchain.BUILD), and vcs.exe is built at the top of the folder. `set` brings the game's settings (vcs.ini, with the
disc) and the launcher's (launcher.ini) along; the old build folder stays as it was - the game is built again in the
new one, and the old one can be deleted. The recompiled code (generated/, 0.2 GB) stays in the source folder.
"""
import argparse
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import doctor  # noqa: E402
import toolchain as tc  # noqa: E402

SUBFOLDER = "VCS-build"
CARRIED = ("vcs.ini", "launcher.ini")  # settings that follow the build folder


def norm(path):
    return os.path.normcase(os.path.abspath(path))


def is_build_folder(path):
    """A folder a build was made in (by this or another source folder): its markers."""
    return any(os.path.exists(os.path.join(path, m)) for m in ("build_info.json", "vcs.ini", os.path.join("release", "CMakeCache.txt")))


def resolve(path):
    """The folder `path` would mean, and why it can't be used ("" when it can)."""
    path = os.path.abspath(os.path.expandvars(os.path.expanduser(path.strip().strip('"'))))
    if norm(path) == norm(tc.DEFAULT_BUILD):
        return path, ""
    inside = norm(path) == norm(tc.ROOT) or norm(path).startswith(norm(tc.ROOT) + os.sep)
    if inside:
        return path, ("it is inside the game's source folder; choose a folder elsewhere (or build/, the default)")
    if os.path.isfile(path):
        return path, "it is a file, not a folder"
    if os.path.isdir(path) and os.listdir(path) and not is_build_folder(path):
        path = os.path.join(path, SUBFOLDER)  # a folder with other things in it: ours goes inside
    if os.path.isfile(path):
        return path, "%s is a file" % path
    if os.path.splitdrive(path)[1] in ("\\", "/", ""):
        return path, "the top of a drive is not a good place; choose or make a folder on it"
    return path, ""


def writable(path):
    """Can the build folder be made there and written in? Tried for real - the folder made (and removed again when it
    was new) with a file in it: some places allow folders but not files (the top of C:)."""
    made = []
    probe = path
    while not os.path.exists(probe):
        made.append(probe)
        probe = os.path.dirname(probe)
    try:
        os.makedirs(path, exist_ok=True)
        test = os.path.join(path, ".vcs-write-test")
        with open(test, "w", encoding="utf-8") as f:
            f.write("x")
        os.remove(test)
        return True
    except OSError:
        return False
    finally:
        for d in made:  # deepest first: only the folders this made
            try:
                os.rmdir(d)
            except OSError:
                pass


def check(path):
    target, problem = resolve(path)
    if not problem and not writable(target):
        problem = "it can't be written to (a protected or read-only place)"
    free = shutil.disk_usage(doctor.existing(target)).free
    warnings = [w["message"] for w in doctor.place_warnings(target, "the folder")
                if w["code"] not in ("read_only",)]
    if free < doctor.NEED_FREE:
        warnings.append("only %.1f GB free on that drive; a build needs about %d GB" % (free / doctor.GB, doctor.NEED_FREE // doctor.GB))
    return {"path": target, "default": norm(target) == norm(tc.DEFAULT_BUILD), "current": norm(target) == norm(tc.BUILD),
            "reused": os.path.isdir(target) and is_build_folder(target), "free_bytes": free, "warnings": warnings,
            "problem": problem, "ok": not problem}


def set_dir(path):
    result = check(path)
    if not result["ok"]:
        tc.fail("can't build in %s: %s" % (result["path"], result["problem"]), "build_dir_refused")
    target, old = result["path"], tc.BUILD
    os.makedirs(target, exist_ok=True)
    carried = []
    if norm(target) != norm(old):
        for name in CARRIED:
            src, dst = os.path.join(old, name), os.path.join(target, name)
            if os.path.isfile(src) and not os.path.exists(dst):
                shutil.copy2(src, dst)
                carried.append(name)
    if result["default"]:
        if os.path.exists(tc.BUILD_POINTER):
            os.remove(tc.BUILD_POINTER)
    else:
        with open(tc.BUILD_POINTER, "w", encoding="utf-8") as f:
            f.write(target + "\n")
    old_built = norm(target) != norm(old) and os.path.isfile(os.path.join(old, tc.exe("vcs")))
    print("[build_dir] the game is built in %s from now on%s" % (target, " (the default)" if result["default"] else ""))
    if carried:
        print("[build_dir] brought along: " + ", ".join(carried))
    if old_built:
        print("[build_dir] the game built in %s stays there; build it again here, then that folder can be deleted" % old)
    tc.event(result="ok", path=target, previous=old, default=result["default"], carried=carried, old_built=old_built,
             reused=result["reused"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("check")
    c.add_argument("path")
    c.add_argument("--json", action="store_true")
    s = sub.add_parser("set")
    s.add_argument("path")
    sub.add_parser("reset")
    a = ap.parse_args()
    if a.cmd == "check":
        result = check(a.path)
        tc.event(result="ok", **result)
        if a.json:
            print(json.dumps(result))
        else:
            print(("ok: " if result["ok"] else "no: ") + result["path"] + ("" if result["ok"] else " - " + result["problem"]))
            for w in result["warnings"]:
                print("  warning: " + w)
        sys.exit(0 if result["ok"] else 1)
    elif a.cmd == "set":
        set_dir(a.path)
    elif a.cmd == "reset":
        set_dir(tc.DEFAULT_BUILD)
    else:
        custom = norm(tc.BUILD) != norm(tc.DEFAULT_BUILD)
        source = "VCS_BUILD_DIR" if os.environ.get("VCS_BUILD_DIR") else ".vcs-build-dir" if custom else "the default"
        print("%s (%s)" % (tc.BUILD, source))


if __name__ == "__main__":
    main()
