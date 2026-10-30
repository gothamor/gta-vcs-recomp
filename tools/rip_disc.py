#!/usr/bin/env python3
"""Copy your PS2 disc from a PC DVD drive to an .iso - for when you own the disc and have no image. Standard library.

    python tools/rip_disc.py list               the DVD drives and the disc in each
    python tools/rip_disc.py rip E: OUT.iso     copy the disc in E: to OUT.iso, then check it (tools/vcs_disc.py check)

A PS2 DVD reads as a plain data disc on a PC drive: the copy is the disc's sectors as they are, the size its ISO 9660
volume says (sector 16). Reads that fail are retried, then the copy stops with the sector it failed at (a scratched
disc: clean it and try again). Windows only. A file path in place of the drive copies that file the same way (tests).
"""
import argparse
import ctypes
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402
import vcs_disc  # noqa: E402

SECTOR = 2048
CHUNK = 512 * SECTOR  # 1 MB per read
DRIVE_CDROM = 5


def drives():
    """Optical drives with what's in them: [{drive, ok, boot, label, message}]."""
    if not tc.IS_WINDOWS:
        return []
    k32 = ctypes.windll.kernel32
    mask = k32.GetLogicalDrives()
    found = []
    old_mode = k32.SetErrorMode(1)  # SEM_FAILCRITICALERRORS: no "insert a disc" dialog for an empty drive
    try:
        for i in range(26):
            if not mask & (1 << i):
                continue
            letter = chr(65 + i) + ":"
            if k32.GetDriveTypeW(letter + "\\") != DRIVE_CDROM:
                continue
            label = ctypes.create_unicode_buffer(261)
            has_media = bool(k32.GetVolumeInformationW(letter + "\\", label, 261, None, None, None, None, 0))
            entry = {"drive": letter, "ok": False, "boot": "", "label": label.value if has_media else "", "message": ""}
            if not has_media:
                entry["message"] = "no disc"
            else:
                try:
                    with open(letter + "\\SYSTEM.CNF", encoding="ascii", errors="replace") as f:
                        m = re.search(r"BOOT2\s*=\s*cdrom0:\\?([^;\s]+)", f.read())
                    entry["boot"] = m.group(1) if m else ""
                except OSError:
                    pass
                if entry["boot"].upper() == vcs_disc.ELF_NAME:
                    entry.update(ok=True, message="GTA: Vice City Stories (USA)")
                elif entry["boot"]:
                    other = vcs_disc.OTHER_RELEASES.get(entry["boot"].upper())
                    entry["message"] = "%s, not the supported disc" % (other or "another PS2 game")
                else:
                    entry["message"] = "not a PS2 disc"
            found.append(entry)
    finally:
        k32.SetErrorMode(old_mode)
    return found


def source_path(source):
    if re.fullmatch(r"[A-Za-z]:?", source):
        return "\\\\.\\" + source[0].upper() + ":"  # the raw volume
    return source


def rip(source, out):
    path = source_path(source)
    tc.stage("copy", "Copying the disc")
    try:
        src = open(path, "rb", buffering=0)
    except OSError as e:
        tc.fail("cannot read %s: %s" % (source, e), "rip_failed")
    with src:
        pvd = src.read(17 * SECTOR)[16 * SECTOR:]
        if len(pvd) < SECTOR or pvd[1:6] != b"CD001":
            tc.fail("%s holds no ISO 9660 volume (not a PS2 DVD?)" % source, "rip_failed")
        sectors = struct.unpack_from("<I", pvd, 80)[0]
        total = sectors * SECTOR
        print("[rip] %s: %d sectors (%.2f GB) -> %s" % (source, sectors, total / (1 << 30), out))
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        part = out + ".part"
        start = time.time()
        with open(part, "wb") as dst:
            src.seek(0)
            done = 0
            while done < total:
                want = min(CHUNK, total - done)
                for attempt in range(4):
                    try:
                        src.seek(done)
                        data = src.read(want)
                        if data and len(data) % SECTOR == 0:
                            break
                    except OSError:
                        data = b""
                    time.sleep(1 + attempt)
                else:
                    tc.fail("the drive could not read sector %d: clean the disc and try again" % (done // SECTOR),
                            "rip_failed")
                dst.write(data)
                done += len(data)
                elapsed = time.time() - start
                eta = int(elapsed * (total - done) / done) if done and elapsed > 5 else None
                tc.event(progress=done >> 20, total=total >> 20, eta=eta, note="MB")
    tc.stage("check", "Checking the copy")
    result = vcs_disc.check(part)
    if not result["ok"]:
        os.remove(part)
        tc.fail("the copy is not the supported disc: " + result["message"], "wrong_disc")
    os.replace(part, out)
    print("[rip] ok: %s (%s, %.0f s)" % (out, result["message"], time.time() - start))
    tc.event(result="ok", iso=os.path.abspath(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("rip")
    r.add_argument("drive", help="the drive letter (E:), or a file")
    r.add_argument("out", help="the .iso to write")
    a = ap.parse_args()
    if a.cmd == "list":
        found = drives()
        for d in found:
            print("%s  %s%s" % (d["drive"], d["message"], " (%s)" % d["boot"] if d["boot"] else ""))
        if not found:
            print("[rip] no DVD drive found")
        tc.event(result="ok", drives=found)
    else:
        rip(a.drive, a.out)


if __name__ == "__main__":
    main()
