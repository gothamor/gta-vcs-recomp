#!/usr/bin/env python3
"""Read the user's own GTA: Vice City Stories (PS2) disc image.

Plain ISO 9660 reader with no third-party dependencies. Used by the build
pipeline to pull the game executable out of the user's ISO and check that
it is the exact build this project was made for.

    python tools/vcs_disc.py info    <iso>
    python tools/vcs_disc.py ls      <iso>
    python tools/vcs_disc.py extract <iso> <ISO path> <out file>
    python tools/vcs_disc.py elf     <iso> <out file>
"""
import hashlib
import struct
import sys

SECTOR = 2048

# The one supported build. The recompiled code only matches this executable.
KNOWN_BUILDS = {
    "d1832508caf0990c2291c0f255c96a889cce9f88": "SLUS-21590 (USA) v1.00",
}
ELF_NAME = "SLUS_215.90"


class IsoImage:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read_sectors(16, 1)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image (no primary volume descriptor)")
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.root = self._parse_record(pvd[156:190])

    def read_sectors(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    @staticmethod
    def _parse_record(rec):
        length = rec[0]
        lba = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len].decode("ascii", "replace")
        return {"len": length, "lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def list_dir(self, rec):
        data = self.read_sectors(rec["lba"], (rec["size"] + SECTOR - 1) // SECTOR)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if length == 0:  # records never straddle sectors; skip to next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            r = self._parse_record(data[pos:pos + length])
            pos += length
            if r["name"] in ("\x00", "\x01"):
                continue
            r["name"] = r["name"].split(";")[0]
            yield r

    def walk(self, rec=None, prefix=""):
        rec = rec or self.root
        for r in self.list_dir(rec):
            path = prefix + r["name"]
            if r["dir"]:
                yield from self.walk(r, path + "/")
            else:
                yield path, r

    def find(self, path):
        cur = self.root
        for part in path.replace("\\", "/").strip("/").upper().split("/"):
            for r in self.list_dir(cur):
                if r["name"].upper() == part:
                    cur = r
                    break
            else:
                return None
        return cur

    def read_file(self, rec):
        return self.read_sectors(rec["lba"], (rec["size"] + SECTOR - 1) // SECTOR)[:rec["size"]]


def cmd_info(iso):
    img = IsoImage(iso)
    print("volume:", img.volume_id)
    rec = img.find(ELF_NAME)
    if not rec:
        sys.exit("error: %s not found on disc - is this GTA: Vice City Stories (USA)?" % ELF_NAME)
    sha1 = hashlib.sha1(img.read_file(rec)).hexdigest()
    print("elf:", ELF_NAME, "lba", rec["lba"], "size", rec["size"])
    print("sha1:", sha1, "->", KNOWN_BUILDS.get(sha1, "UNKNOWN BUILD (unsupported)"))


def cmd_ls(iso):
    for path, r in IsoImage(iso).walk():
        print("%8d %10d %s" % (r["lba"], r["size"], path))


def cmd_extract(iso, src, out):
    img = IsoImage(iso)
    rec = img.find(src)
    if not rec or rec["dir"]:
        sys.exit("error: %s not found" % src)
    with open(out, "wb") as f:
        f.write(img.read_file(rec))


def cmd_elf(iso, out):
    img = IsoImage(iso)
    rec = img.find(ELF_NAME)
    if not rec:
        sys.exit("error: %s not found on disc - is this GTA: Vice City Stories (USA)?" % ELF_NAME)
    data = img.read_file(rec)
    sha1 = hashlib.sha1(data).hexdigest()
    if sha1 not in KNOWN_BUILDS:
        sys.exit("error: unsupported executable build (sha1 %s)" % sha1)
    with open(out, "wb") as f:
        f.write(data)
    print("extracted %s (%s)" % (ELF_NAME, KNOWN_BUILDS[sha1]))


def main(argv):
    cmds = {"info": (cmd_info, 1), "ls": (cmd_ls, 1), "extract": (cmd_extract, 3), "elf": (cmd_elf, 2)}
    if len(argv) < 2 or argv[1] not in cmds or len(argv) - 2 != cmds[argv[1]][1]:
        sys.exit(__doc__)
    fn, _ = cmds[argv[1]]
    fn(*argv[2:])


if __name__ == "__main__":
    main(sys.argv)
