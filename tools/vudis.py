#!/usr/bin/env python3
"""VU (VU0 / VU1) microcode disassembler.

    python tools/vudis.py CODE.bin [--start 0x30] [--end 0x800] [--to-ebit]

CODE.bin: VU code memory as raw bytes (e.g. hle_samples/*_vucode.bin from PS2X_HLE_SURVEY_DUMP, or vu1_code_*.bin
from PS2X_VU1_DUMP). Addresses are byte addresses in code memory (8 bytes per instruction pair, as MSCAL*8).
--to-ebit stops after the pair following the first E bit (the end of the program started at --start).

Each line: address, upper instruction | lower instruction, flags (E end, I lower is a float for I, M D T).
Branch targets are shown as byte addresses. Decoder tables come from tools/vu1_recomp.py.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_recomp as vr  # noqa: E402

BC = 'xyzw'


def dest(i):
    d = (i >> 21) & 0xF
    return ''.join(c for c, bit in zip('xyzw', (8, 4, 2, 1)) if d & bit)


def vf(r, d=''):
    return f'vf{r:02d}' + ('.' + d if d else '')


def vi(r):
    return f'vi{r:02d}'


def fsf(i):
    return 'xyzw'[(i >> 21) & 3]


def ftf(i):
    return 'xyzw'[(i >> 23) & 3]


def upper(i):
    name = vr.upper_name(i)
    op, d, fs, ft, fd = i & 0x3F, dest(i), vr.FS(i), vr.FT(i), vr.FD(i)
    if name in ('NOP', 'reserved'):
        return name
    if op <= 0x1B:
        return f'{name}.{d} {vf(fd, d)}, {vf(fs, d)}, {vf(ft, BC[op & 3])}'
    if op < 0x3C:
        tail = {'q': 'Q', 'i': 'I'}.get(name[-1], None) if name[-1] in 'qi' else None
        if tail:
            return f'{name}.{d} {vf(fd, d)}, {vf(fs, d)}, {tail}'
        return f'{name}.{d} {vf(fd, d)}, {vf(fs, d)}, {vf(ft, d)}'
    sp = vr.upper_special(i)
    if sp <= 0x0F or 0x18 <= sp <= 0x1B:
        return f'{name}.{d} ACC, {vf(fs, d)}, {vf(ft, BC[sp & 3])}'
    if 0x10 <= sp <= 0x17 or sp == 0x1D:
        return f'{name}.{d} {vf(ft, d)}, {vf(fs, d)}'
    if sp == 0x1F:
        return f'CLIPw.xyz {vf(fs, "xyz")}, {vf(ft, "w")}'
    if name.endswith('q') or name.endswith('i'):
        return f'{name}.{d} ACC, {vf(fs, d)}, {name[-1].upper()}'
    if name in ('ADDA', 'MADDA', 'MULA', 'SUBA', 'MSUBA', 'OPMULA'):
        return f'{name}.{d} ACC, {vf(fs, d)}, {vf(ft, d)}'
    return f'{name}.{d} {vf(fs, d)}, {vf(ft, d)}'


def lower(i, pc):
    if i in (0, 0x8000033C):
        return 'NOP'
    name = vr.lower_name(i)
    op = (i >> 25) & 0x7F
    d, ft, fs, it, is_, id_ = dest(i), vr.FT(i), vr.FS(i), vr.VIT(i), vr.VIS(i), vr.VID(i)
    imm11 = vr.IMM11(i)
    target = lambda: f'0x{(pc // 8 + 1 + imm11) * 8:04x}'  # noqa: E731
    if op == 0x00:
        return f'LQ.{d} {vf(ft, d)}, {imm11}({vi(is_)})'
    if op == 0x01:
        return f'SQ.{d} {vf(fs, d)}, {imm11}({vi(it)})'
    if op == 0x04:
        return f'ILW.{d} {vi(it)}, {imm11}({vi(is_)})'
    if op == 0x05:
        return f'ISW.{d} {vi(it)}, {imm11}({vi(is_)})'
    if op in (0x08, 0x09):
        imm15 = (i & 0x7FF) | (((i >> 21) & 0xF) << 11)
        return f'{name} {vi(it)}, {vi(is_)}, {imm15}'
    if op in (0x10, 0x11, 0x12, 0x13):
        return f'{name} vi01, 0x{i & 0xFFFFFF:06x}'
    if op in (0x14, 0x15, 0x16, 0x17):
        imm12 = (i & 0x7FF) | (((i >> 21) & 1) << 11)
        return f'{name} {vi(it)}, 0x{imm12:03x}'
    if op in (0x18, 0x1A, 0x1B):
        return f'{name} {vi(it)}, {vi(is_)}'
    if op == 0x1C:
        return f'FCGET {vi(it)}'
    if op == 0x20:
        return f'B {target()}'
    if op == 0x21:
        return f'BAL {vi(it)}, {target()}'
    if op == 0x24:
        return f'JR {vi(is_)}'
    if op == 0x25:
        return f'JALR {vi(it)}, {vi(is_)}'
    if op in (0x28, 0x29):
        return f'{name} {vi(it)}, {vi(is_)}, {target()}'
    if op in (0x2C, 0x2D, 0x2E, 0x2F):
        return f'{name} {vi(is_)}, {target()}'
    if op != 0x40:
        return f'reserved {i:08x}'
    direct = i & 0x3F
    if direct < 0x3C:
        if direct == 0x32:
            imm5 = (i >> 6) & 0x1F
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            return f'IADDI {vi(it)}, {vi(is_)}, {imm5}'
        return f'{name} {vi(id_)}, {vi(is_)}, {vi(it)}'
    sp = vr.lower_special(i)
    if sp in (0x30, 0x31):
        return f'{name}.{d} {vf(ft, d)}, {vf(fs, d)}'
    if sp == 0x34:
        return f'LQI.{d} {vf(ft, d)}, ({vi(is_)}++)'
    if sp == 0x35:
        return f'SQI.{d} {vf(fs, d)}, ({vi(it)}++)'
    if sp == 0x36:
        return f'LQD.{d} {vf(ft, d)}, (--{vi(is_)})'
    if sp == 0x37:
        return f'SQD.{d} {vf(fs, d)}, (--{vi(it)})'
    if sp in (0x38, 0x3A):
        return f'{name} Q, {vf(fs, fsf(i))}, {vf(ft, ftf(i))}'
    if sp == 0x39:
        return f'SQRT Q, {vf(ft, ftf(i))}'
    if sp == 0x3B:
        return 'WAITQ'
    if sp == 0x3C:
        return f'MTIR {vi(it)}, {vf(fs, fsf(i))}'
    if sp == 0x3D:
        return f'MFIR.{d} {vf(ft, d)}, {vi(is_)}'
    if sp in (0x3E, 0x3F):
        return f'{name}.{d} {vi(it)}, ({vi(is_)})'
    if sp in (0x40, 0x41):
        return f'{name}.{d} {vf(ft, d)}, R'
    if sp in (0x42, 0x43):
        return f'{name} R, {vf(fs, fsf(i))}'
    if sp == 0x64:
        return f'MFP.{d} {vf(ft, d)}, P'
    if sp in (0x68, 0x69):
        return f'{name} {vi(it)}'
    if sp == 0x6C:
        return f'XGKICK {vi(is_)}'
    if sp in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77):
        return f'{name} P, {vf(fs)}'
    if 0x78 <= sp <= 0x7F:
        return f'{name} P, {vf(fs, fsf(i))}'
    return name


def disassemble(code, start, end, to_ebit):
    out = []
    ebit_left = None
    for pc in range(start, end, 8):
        lo, up = struct.unpack_from('<II', code, pc)
        flags = ''.join(f for f, bit in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if up & (1 << bit))
        low = f'loi 0x{lo:08x} ({struct.unpack("<f", struct.pack("<I", lo))[0]:g})' if up & (1 << 31) else lower(lo, pc)
        out.append(f'{pc:04x}  {upper(up):<40} | {low:<44} {flags}')
        if to_ebit:
            if ebit_left is not None:
                ebit_left -= 1
                if ebit_left < 0:
                    break
            elif up & (1 << 30):
                ebit_left = 0
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('code')
    ap.add_argument('--start', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--end', type=lambda s: int(s, 0), default=None)
    ap.add_argument('--to-ebit', action='store_true')
    a = ap.parse_args()
    code = open(a.code, 'rb').read()
    end = min(a.end if a.end is not None else len(code), len(code))
    print('\n'.join(disassemble(code, a.start, end, a.to_ebit)))


if __name__ == '__main__':
    main()
