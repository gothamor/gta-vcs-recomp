#!/usr/bin/env python3
"""Copy freshly generated C++ into generated/ee, touching only changed files.

ps2_recomp rewrites every file on each run, which would make ninja rebuild
all ~300 unity batches (~20 min). Syncing by content keeps rebuilds incremental.

    python tools/sync_generated.py <fresh dir> <dest dir>
"""
import os
import shutil
import sys


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    fresh = set(os.listdir(src))
    changed = added = removed = 0
    for name in fresh:
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        with open(s, "rb") as f:
            data = f.read()
        if os.path.exists(d):
            with open(d, "rb") as f:
                if f.read() == data:
                    continue
            changed += 1
        else:
            added += 1
        with open(d, "wb") as f:
            f.write(data)
    for name in os.listdir(dst):
        if name not in fresh:
            os.remove(os.path.join(dst, name))
            removed += 1
    shutil.rmtree(src)
    print("generated: %d changed, %d added, %d removed, %d total" % (changed, added, removed, len(fresh)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
