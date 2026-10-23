"""MIPS I disassembly (the IOP's R3000A instruction set), standard library only - used by tools/irxdis.py.

    decode(word, address) -> (mnemonic, operands, branch_target_or_None)

Covers every MIPS I opcode (ALU, shifts, multiply / divide, loads / stores incl. lwl / lwr / swl / swr, branches,
jumps, syscall / break, COP0 mfc0 / mtc0 / rfe, COP2 moves). Anything else comes out as `.word`.
"""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

_SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr",
            0x0C: "syscall", 0x0D: "break", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x18: "mult",
            0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and",
            0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu"}
_IMM = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori"}
_MEM = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x28: "sb", 0x29: "sh",
        0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x32: "lwc2", 0x3A: "swc2"}


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(w, addr):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    R = REG
    if w == 0:
        return "nop", "", None
    if op == 0:
        m = _SPECIAL.get(fn)
        if m is None:
            return ".word", "0x%08x" % w, None
        if m in ("sll", "srl", "sra"):
            return m, "%s, %s, %d" % (R[rd], R[rt], sa), None
        if m in ("sllv", "srlv", "srav"):
            return m, "%s, %s, %s" % (R[rd], R[rt], R[rs]), None
        if m == "jr":
            return m, R[rs], None
        if m == "jalr":
            return m, (R[rs] if rd == 31 else "%s, %s" % (R[rd], R[rs])), None
        if m in ("syscall", "break"):
            return m, "0x%x" % ((w >> 6) & 0xFFFFF), None
        if m in ("mfhi", "mflo"):
            return m, R[rd], None
        if m in ("mthi", "mtlo"):
            return m, R[rs], None
        if m in ("mult", "multu", "div", "divu"):
            return m, "%s, %s" % (R[rs], R[rt]), None
        if m == "addu" and rt == 0:
            return "move", "%s, %s" % (R[rd], R[rs]), None
        return m, "%s, %s, %s" % (R[rd], R[rs], R[rt]), None
    if op == 1:
        names = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}
        if rt not in names:
            return ".word", "0x%08x" % w, None
        t = (addr + 4 + (_s16(imm) << 2)) & 0xFFFFFFFF
        return names[rt], "%s, 0x%x" % (R[rs], t), t
    if op in (2, 3):
        t = ((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        return ("j" if op == 2 else "jal"), "0x%x" % t, t
    if op in (4, 5):
        t = (addr + 4 + (_s16(imm) << 2)) & 0xFFFFFFFF
        m = "beq" if op == 4 else "bne"
        if op == 4 and rs == 0 and rt == 0:
            return "b", "0x%x" % t, t
        return m, "%s, %s, 0x%x" % (R[rs], R[rt], t), t
    if op in (6, 7):
        t = (addr + 4 + (_s16(imm) << 2)) & 0xFFFFFFFF
        return ("blez" if op == 6 else "bgtz"), "%s, 0x%x" % (R[rs], t), t
    if op in _IMM:
        m = _IMM[op]
        if m == "addiu" and rs == 0:
            return "li", "%s, %d" % (R[rt], _s16(imm)), None
        if m in ("andi", "ori", "xori"):
            return m, "%s, %s, 0x%x" % (R[rt], R[rs], imm), None
        return m, "%s, %s, %d" % (R[rt], R[rs], _s16(imm)), None
    if op == 0x0F:
        return "lui", "%s, 0x%x" % (R[rt], imm), None
    if op == 0x10:  # COP0
        if rs == 0:
            return "mfc0", "%s, $%d" % (R[rt], rd), None
        if rs == 4:
            return "mtc0", "%s, $%d" % (R[rt], rd), None
        if rs == 16 and fn == 0x10:
            return "rfe", "", None
    if op == 0x12:  # COP2 moves (GTE: unused by sound code, decoded for completeness)
        names = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}
        if rs in names:
            return names[rs], "%s, $%d" % (R[rt], rd), None
        return "cop2", "0x%x" % (w & 0x1FFFFFF), None
    if op in _MEM:
        return _MEM[op], "%s, %d(%s)" % (R[rt] if op < 0x30 else "$%d" % rt, _s16(imm), R[rs]), None
    return ".word", "0x%08x" % w, None
