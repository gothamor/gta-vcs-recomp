#!/usr/bin/env python3
"""PC VC car (.dff) -> VCS vehicle model (`ldm`), FORK.md T9 step 3 - the encoder first.

    python tools/dff2mdl.py --self-test [--img DIR]   every PS2 mesh of the extracted vehicles (tools/img.py extract)
                                                     decoded to its raw vertex values and encoded again: the bytes
                                                     must come out identical to the game's

A mesh's DMA data (docs/game-internals.md 9d, tools/mdlinfo.py): a DMA tag qword (0x60000000 | qwords - 1), then
batches of at most 70 vertices - the next batch repeats the last two (a strip) - each {UNPACK V4-32 x1 at 0x8000: 0, 0,
n, n | 0x8000 (the program's header); STMASK 0x40404040, STROW 0, UNPACK V3-16 positions at 1; STMASK 0x50505050,
STROW (0, 0, 0xFF, 0), UNPACK V2-8 unsigned UVs at 0x8D; UNPACK V3-8 normals at 0x47; MSCAL 0x06} padded with zeros to
a qword.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdlinfo  # noqa: E402

BATCH = 70


def decode_raw(d, at):
    """A mesh's vertices as stored: (x, y, z) int16, (u, v) uint8, (nx, ny, nz) int8 or None; the strip in order."""
    qwords = (struct.unpack_from("<I", d, at)[0] & 0xFFFF) + 1
    end, w = at + qwords * 16, at + 16
    out, first = [], True
    while w < end and struct.unpack_from("<I", d, w)[0] == 0x6C018000:
        n = struct.unpack_from("<I", d, w + 16)[0] & 0x7FFF
        w += 20
        arrays = {}
        while True:
            code = struct.unpack_from("<I", d, w)[0]
            cmd = code >> 24
            if cmd == 0x14:
                w += 4
                while w < end and struct.unpack_from("<I", d, w)[0] == 0:
                    w += 4
                break
            if cmd == 0x20:
                w += 8
            elif cmd == 0x30:
                w += 20
            else:
                size = {0x9: 6, 0x6: 2, 0xA: 3}[cmd & 0xF]
                arrays[cmd & 0xF] = w + 4
                w += 4 + (((code >> 16) & 0xFF) * size + 3) // 4 * 4
        for i in range(0 if first else 2, n):
            p = struct.unpack_from("<3h", d, arrays[0x9] + i * 6)
            t = struct.unpack_from("<2B", d, arrays[0x6] + i * 2)
            nrm = struct.unpack_from("<3b", d, arrays[0xA] + i * 3) if 0xA in arrays else None
            out.append((p, t, nrm))
        first = False
    return out, end - at


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def encode(verts):
    """The DMA data for a strip of raw vertices (the inverse of decode_raw)."""
    body = b""
    start = 0
    while True:
        batch = verts[start:start + BATCH]
        n = len(batch)
        body += struct.pack("<5I", 0x6C018000, 0, 0, n, n | 0x8000)
        body += struct.pack("<2I", 0x20000000, 0x40404040) + struct.pack("<5I", 0x30000000, 0, 0, 0, 0)
        body += struct.pack("<I", 0x79008001 | n << 16) + pad4(b"".join(struct.pack("<3h", *v[0]) for v in batch))
        body += struct.pack("<2I", 0x20000000, 0x50505050) + struct.pack("<5I", 0x30000000, 0, 0, 0xFF, 0)
        body += struct.pack("<I", 0x7600C08D | n << 16) + pad4(b"".join(struct.pack("<2B", *v[1]) for v in batch))
        if batch[0][2] is not None:
            body += struct.pack("<I", 0x6A008047 | n << 16) + pad4(b"".join(struct.pack("<3b", *v[2]) for v in batch))
        body += struct.pack("<I", 0x14000006)
        body += b"\0" * (-(16 + len(body)) % 16)
        if start + BATCH >= len(verts):
            break
        start += BATCH - 2
    return struct.pack("<4I", 0x60000000 | (len(body) // 16), 0, 0, 0) + body


def self_test(folder):
    meshes = same = 0
    bad = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".mdl"):
            continue
        m = mdlinfo.Mdl(open(os.path.join(folder, name), "rb").read())
        group = m.ptr(0x20)
        for e in m.elements(group):
            g = m.ptr(e + 0x14)
            if not g:
                continue
            h = g + 32
            n = struct.unpack_from("<I", m.d, h + 16)[0] >> 20
            base = h + 0x40 + n * 0x30
            for i in range(n):
                at = base + struct.unpack_from("<I", m.d, h + 0x40 + i * 0x30 + 28)[0]
                verts, length = decode_raw(m.d, at)
                meshes += 1
                if encode(verts) == m.d[at:at + length]:
                    same += 1
                elif len(bad) < 5:
                    bad.append("%s mesh at 0x%x" % (name, at))
    print("dff2mdl self-test: %d / %d meshes encoded byte for byte%s" % (same, meshes, ("; differ: " + ", ".join(bad)) if bad else ""))
    return same == meshes and meshes > 0


def main():
    args = sys.argv[1:]
    if args[:1] == ["--self-test"]:
        import toolchain as tc
        folder = args[args.index("--img") + 1] if "--img" in args else os.path.join(tc.ROOT, "build", "img", "GTA3PS2")
        return 0 if self_test(folder) else 1
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
