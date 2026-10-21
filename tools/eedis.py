#!/usr/bin/env python3
"""Disassemble part of the EE ELF, with call targets named from game/vcs/functions.csv.

    python -I tools/eedis.py <elf> <start_hex> [end_hex | +len_hex]
    python -I tools/eedis.py <elf> <name_or_addr_inside_function>     (whole function)

The ELF is staged by vcs.exe at %LOCALAPPDATA%/VCSRecomp/cache/SLUS_215.90.
Output is derived from game code: keep listings under build/analysis/.
R5900-only opcodes (MMI, LQ/SQ, VU0 macro) show as '.word'. Needs capstone.
"""
import bisect
import csv
import os
import struct
import sys

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_elf(path):
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phnum, = struct.unpack_from('<H', d, 0x2C)
    segs = []
    for i in range(phnum):
        typ, off, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', d, phoff + i * 32)
        if typ == 1:
            segs.append((vaddr, d[off:off + filesz]))
    return segs


def read(segs, addr, n):
    for base, blob in segs:
        if base <= addr < base + len(blob):
            return blob[addr - base:addr - base + n]
    return b''


def load_functions():
    rows = []
    for r in csv.reader(open(os.path.join(ROOT, 'game', 'vcs', 'functions.csv'))):
        if len(r) >= 3 and r[1].startswith('0x'):
            rows.append((int(r[1], 16), int(r[2], 16), r[0]))
    rows.sort()
    return rows


def main():
    segs = load_elf(sys.argv[1])
    funcs = load_functions()
    starts = [f[0] for f in funcs]
    names = {f[0]: f[2] for f in funcs}
    by_name = {f[2]: f for f in funcs}

    arg = sys.argv[2]
    if arg in by_name:
        start, end = by_name[arg][0], by_name[arg][1]
    else:
        start = int(arg, 16)
        if len(sys.argv) > 3:
            e = sys.argv[3]
            end = start + int(e[1:], 16) if e.startswith('+') else int(e, 16)
        else:
            i = bisect.bisect_right(starts, start) - 1
            start, end = funcs[i][0], funcs[i][1]

    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
    code = read(segs, start, end - start)
    for off in range(0, len(code) - 3, 4):
        a = start + off
        if a in names:
            print('\n%s:' % names[a])
        word, = struct.unpack_from('<I', code, off)
        ins = list(md.disasm(code[off:off + 4], a))
        text = '%s %s' % (ins[0].mnemonic, ins[0].op_str) if ins else '.word 0x%08x' % word
        op = word >> 26
        if op in (2, 3):  # j / jal
            t = ((a + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            text += '    ; %s' % names.get(t, hex(t))
        print('  %08x: %08x  %s' % (a, word, text))


if __name__ == '__main__':
    main()
