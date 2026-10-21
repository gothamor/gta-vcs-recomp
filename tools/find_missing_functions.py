#!/usr/bin/env python3
"""Find code Ghidra left outside any function that the game reaches indirectly.

Scans .data/.rodata/.sdata-like sections for 32-bit words pointing into .text
that fall in a gap between known functions (vtables, callback tables). Writes
them to game/vcs/extra_functions.csv (merged by make_config.py); each one ends
at the next known or new function start.

    python -I tools/find_missing_functions.py build/elf/SLUS_215.90 ghidra/export/functions.csv
"""
import bisect
import csv
import struct
import sys

OUT = "game/vcs/extra_functions.csv"
LABEL_PREFIXES = ("entry_", "caseD_", "default", "LAB_", "switchD_")


def sections(d):
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    base = secs[shstrndx][4]
    for s in secs:
        yield d[base + s[0]:d.index(b"\0", base + s[0])].decode(), s


def main(elf, csv_path):
    d = open(elf, "rb").read()
    secs = dict(sections(d))
    text = secs[".text"]
    t_lo, t_hi = text[3], text[3] + text[5]
    funcs = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in csv.DictReader(open(csv_path))
                   if not r["Name"].startswith(LABEL_PREFIXES) and not r["Name"].startswith("fn_"))
    starts = [f[0] for f in funcs]

    def covered(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and funcs[i][0] <= a < funcs[i][1]

    found = set()
    for name in (".data", ".rodata", ".sdata", ".vudata"):
        if name not in secs:
            continue
        s = secs[name]
        blob = d[s[4]:s[4] + s[5]]
        for i in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, i)[0]
            if t_lo <= v < t_hi and v % 4 == 0 and not covered(v):
                if struct.unpack_from("<I", d, text[4] + (v - t_lo))[0] != 0:
                    found.add(v)

    # Function pointers built in code: lui rX, hi ; ... addiu/ori rY, rX, lo (within 8 insns).
    code = d[text[4]:text[4] + text[5]]
    n = len(code) // 4
    words = struct.unpack_from("<%dI" % n, code)
    for i, w in enumerate(words):
        if (w >> 26) != 0x0F:  # lui
            continue
        rt = (w >> 16) & 31
        hi = (w & 0xFFFF) << 16
        for j in range(i + 1, min(n, i + 9)):
            x = words[j]
            op, rs, rd = x >> 26, (x >> 21) & 31, (x >> 16) & 31
            if rs == rt and op in (0x09, 0x0D):  # addiu / ori
                lo = x & 0xFFFF
                v = (hi + (lo - 0x10000 if (op == 0x09 and lo & 0x8000) else lo)) & 0xFFFFFFFF
                if t_lo <= v < t_hi and v % 4 == 0 and not covered(v):
                    if struct.unpack_from("<I", d, text[4] + (v - t_lo))[0] != 0:
                        found.add(v)
                break
            if ((x >> 16) & 31) == rt and op not in (0x2B, 0x29, 0x28, 0x3F, 0x1F):  # rt overwritten (not a store)
                break

    new = sorted(found)
    all_starts = sorted(set(starts) | found)
    with open(OUT, "w", newline="\n") as out:
        out.write("Name,Start,End,Size\n")
        for v in new:
            j = bisect.bisect_right(all_starts, v)
            end = all_starts[j] if j < len(all_starts) else t_hi
            out.write("fn_%08x,0x%08X,0x%08X,%d\n" % (v, v, end, end - v))
    print("wrote %s: %d indirectly-referenced functions Ghidra missed" % (OUT, len(new)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
