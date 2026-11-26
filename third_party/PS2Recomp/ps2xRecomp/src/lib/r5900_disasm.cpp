// [gta-recomp] ps2recomp::r5900 (r5900_disasm.h): the R5900 decode tree, operand text and instruction facts.
// The tables (instruction names, operands, flags, the case tables of each opcode group, register names) are in
// r5900_tables.inc; the walk through the opcode groups follows the EE's encoding: opcode, then SPECIAL / REGIMM /
// COP0 / COP1 / COP2 / MMI by their sub-fields. Each group checked sets the bits it looked at as mandatory; an
// instruction is valid when every set bit of the word is mandatory or belongs to one of its operands.
#include "ps2recomp/r5900_disasm.h"

#include <cstdio>

namespace ps2recomp::r5900
{
    namespace
    {
#include "r5900_tables.inc"

        constexpr uint32_t kOpcode = 0xFC000000u;
        constexpr uint32_t kRs = 0x03E00000u;
        constexpr uint32_t kRt = 0x001F0000u;
        constexpr uint32_t kRd = 0x0000F800u;
        constexpr uint32_t kSa = 0x000007C0u;
        constexpr uint32_t kFunction = 0x0000003Fu;
        constexpr uint32_t kXyzw = 0x01E00000u;

        struct Decoded
        {
            uint32_t word = 0;
            uint16_t id = 0;
            const char *idType = "ALL_INVALID";
            uint32_t mandatory = 0; // fields the decoding looked at
        };

        uint32_t field(uint32_t word, int shift, int width)
        {
            return (word >> shift) & ((1u << width) - 1u);
        }

        // a group's case table: the id it gives for key, or none (the id stays)
        bool pick(Decoded &d, const uint16_t *table, uint32_t key)
        {
            const uint16_t id = table[key];
            if (id == kNoId)
                return false;
            d.id = id;
            return true;
        }

        uint32_t rs(uint32_t w) { return field(w, 21, 5); }
        uint32_t rt(uint32_t w) { return field(w, 16, 5); }
        uint32_t fn(uint32_t w) { return field(w, 0, 6); }

        void cpuNormal(Decoded &d)
        {
            d.idType = "CPU_NORMAL";
            d.mandatory |= kOpcode;
            const uint32_t op = field(d.word, 26, 6);
            if (pick(d, kCpuNormal, op))
            {
                if (d.id == ID_cpu_beq && rt(d.word) == 0)
                    d.id = rs(d.word) == 0 ? ID_cpu_b : ID_cpu_beqz;
                else if (d.id == ID_cpu_bne && rt(d.word) == 0)
                    d.id = ID_cpu_bnez;
            }
        }

        void cpuSpecial(Decoded &d)
        {
            d.idType = "CPU_SPECIAL";
            d.mandatory |= kFunction;
            pick(d, kCpuSpecial, fn(d.word));
            if (d.word == 0)
                d.id = ID_cpu_nop;
            else if (d.id == ID_cpu_or && rt(d.word) == 0)
                d.id = ID_cpu_move;
            else if (d.id == ID_cpu_nor && rt(d.word) == 0)
                d.id = ID_cpu_not;
            else if (d.id == ID_cpu_sub && rs(d.word) == 0)
                d.id = ID_cpu_neg;
            else if (d.id == ID_cpu_subu && rs(d.word) == 0)
                d.id = ID_cpu_negu;
        }

        void cpuRegimm(Decoded &d)
        {
            d.idType = "CPU_REGIMM";
            d.mandatory |= kRt;
            if (pick(d, kCpuRegimm, rt(d.word)) && d.id == ID_cpu_bgezal && rs(d.word) == 0)
                d.id = ID_cpu_bal;
        }

        void r5900Normal(Decoded &d)
        {
            d.idType = "R5900_NORMAL";
            d.mandatory |= kOpcode;
            if (!pick(d, kR5900Normal, field(d.word, 26, 6)))
                cpuNormal(d);
        }

        void r5900Special(Decoded &d)
        {
            d.idType = "R5900_SPECIAL";
            d.mandatory |= kFunction;
            if (!pick(d, kR5900Special, fn(d.word)))
                cpuSpecial(d);
            if (d.id == ID_cpu_sync)
            {
                d.mandatory |= kSa; // stype
                if (field(d.word, 6, 5) & 0x10)
                    d.id = ID_r5900_sync_p;
            }
        }

        void r5900Regimm(Decoded &d)
        {
            d.idType = "R5900_REGIMM";
            d.mandatory |= kRt;
            if (!pick(d, kR5900Regimm, rt(d.word)))
                cpuRegimm(d);
        }

        void r5900Cop0(Decoded &d)
        {
            d.idType = "R5900_COP0";
            d.mandatory |= kRs; // fmt
            const uint32_t fmt = rs(d.word);
            if (fmt == 0x08)
            {
                d.idType = "CPU_COP0_BC0";
                d.mandatory |= kRt; // bc_fmt
                pick(d, kCpuCop0Bc0, rt(d.word));
            }
            else if (fmt == 0x10)
            {
                d.idType = "R5900_COP0_TLB";
                d.mandatory |= kFunction;
                if (!pick(d, kR5900Cop0Tlb, fn(d.word)))
                {
                    d.idType = "CPU_COP0_TLB";
                    pick(d, kCpuCop0Tlb, fn(d.word));
                }
            }
            else
                pick(d, kCpuCop0, fmt);
        }

        void r5900Cop1(Decoded &d)
        {
            d.idType = "R5900_COP1";
            d.mandatory |= kRs; // fmt
            const uint32_t fmt = rs(d.word);
            if (fmt == 0x08)
            {
                d.idType = "CPU_COP1_BC1";
                d.mandatory |= kRt; // bc_fmt
                pick(d, kCpuCop1Bc1, rt(d.word));
            }
            else if (fmt == 0x10)
            {
                d.idType = "R5900_COP1_FPUS";
                d.mandatory |= kFunction;
                if (!pick(d, kR5900Cop1FpuS, fn(d.word)))
                {
                    d.idType = "CPU_COP1_FPUS";
                    pick(d, kCpuCop1FpuS, fn(d.word));
                }
            }
            else if (fmt == 0x14)
            {
                d.idType = "CPU_COP1_FPUW";
                d.mandatory |= kFunction;
                pick(d, kCpuCop1FpuW, fn(d.word));
            }
            else
                pick(d, kCpuCop1, fmt);
        }

        void r5900Cop2(Decoded &d)
        {
            d.idType = "R5900_COP2";
            d.mandatory |= (1u << 25) | 1u; // the high bit and the low bit
            const uint32_t highLow = (field(d.word, 25, 1) << 1) | field(d.word, 0, 1);
            if (highLow <= 1)
            {
                // no high bit: the transfers (qmfc2 / cfc2 / qmtc2 / ctc2, .ni or .i by the low bit) and bc2
                d.idType = "R5900_COP2_NOHIGHBIT";
                d.mandatory |= 0x01E00000u; // fmt without its high bit
                const uint32_t fmt = field(d.word, 21, 4);
                if (fmt == 0x08)
                {
                    d.idType = "R5900_COP2_BC2";
                    d.mandatory |= kRt; // bc_fmt
                    pick(d, kR5900Cop2Bc2, rt(d.word));
                }
                else
                    pick(d, highLow == 0 ? kR5900Cop2Ni : kR5900Cop2I, fmt);
                return;
            }
            // the VU0 macro instructions: SPECIAL1 by the function, SPECIAL2 by fhi / flo, VIWR by fhi (21..24) / flo
            d.idType = "R5900_COP2_SPECIAL1";
            d.mandatory |= kFunction;
            const uint32_t function = fn(d.word);
            if (function < 0x3C)
            {
                pick(d, kR5900Cop2Special1, function);
                return;
            }
            d.idType = "R5900_COP2_SPECIAL2";
            d.mandatory |= kSa | 0x3u;
            const uint32_t fhiFlo = (field(d.word, 6, 5) << 2) | field(d.word, 0, 2);
            if (fhiFlo != 0x3E && fhiFlo != 0x3F)
            {
                pick(d, kR5900Cop2Special2, fhiFlo);
                return;
            }
            d.idType = "R5900_COP2_VIWR";
            d.mandatory |= 0x01E00000u | 0x3u;
            pick(d, kR5900Cop2Viwr, (field(d.word, 21, 4) << 2) | field(d.word, 0, 2));
        }

        void r5900Mmi(Decoded &d)
        {
            d.idType = "R5900_MMI";
            d.mandatory |= kFunction;
            const uint32_t function = fn(d.word);
            const uint16_t *sub = nullptr;
            const char *type = nullptr;
            switch (function)
            {
            case 0x08: sub = kR5900Mmi0; type = "R5900_MMI_0"; break;
            case 0x09: sub = kR5900Mmi2; type = "R5900_MMI_2"; break;
            case 0x28: sub = kR5900Mmi1; type = "R5900_MMI_1"; break;
            case 0x29: sub = kR5900Mmi3; type = "R5900_MMI_3"; break;
            case 0x30: sub = kR5900MmiPmfhl; type = "R5900_MMI_PMFHL"; break;
            case 0x31: sub = kR5900MmiPmthl; type = "R5900_MMI_PMTHL"; break;
            default: break;
            }
            if (!sub)
            {
                pick(d, kR5900Mmi, function);
                return;
            }
            d.idType = type;
            d.mandatory |= kSa; // the MMI sub-function
            pick(d, sub, field(d.word, 6, 5));
        }

        Decoded decode(uint32_t word)
        {
            Decoded d;
            d.word = word;
            d.id = ID_r5900_INVALID;
            d.idType = "R5900_INVALID";
            d.mandatory = kOpcode;
            switch (field(word, 26, 6))
            {
            case 0x00: r5900Special(d); break;
            case 0x01: r5900Regimm(d); break;
            case 0x10: r5900Cop0(d); break;
            case 0x11: r5900Cop1(d); break;
            case 0x12: r5900Cop2(d); break;
            case 0x1C: r5900Mmi(d); break;
            default: r5900Normal(d); break;
            }
            return d;
        }

        // the word's bits an operand takes
        uint32_t operandBits(Operand op)
        {
            switch (op)
            {
            case Operand::None: return 0;
            case Operand::cpu_rs: return kRs;
            case Operand::cpu_rt: return kRt;
            case Operand::cpu_rd: return kRd;
            case Operand::cpu_sa: return kSa;
            case Operand::cpu_zero: return 0;
            case Operand::cpu_cop0d: return kRd;
            case Operand::cpu_fs: return kRd;
            case Operand::cpu_ft: return kRt;
            case Operand::cpu_fd: return kSa;
            case Operand::cpu_cop1cs: return kRd;
            case Operand::cpu_cop2t: return kRt;
            case Operand::cpu_cop2cd: return kRd;
            case Operand::cpu_op: return kRt;
            case Operand::cpu_hint: return kRt;
            case Operand::cpu_code: return 0x03FFFFC0u;
            case Operand::cpu_code_lower: return 0x0000FFC0u;
            case Operand::cpu_copraw: return 0x01FFFFFFu;
            case Operand::cpu_label: return 0x03FFFFFFu;
            case Operand::cpu_immediate: return 0xFFFFu;
            case Operand::cpu_branch_target_label: return 0xFFFFu;
            case Operand::cpu_immediate_base: return 0xFFFFu | kRs;
            case Operand::cpu_maybe_rd_rs: return kRd | kRs;
            case Operand::r5900_I:
            case Operand::r5900_Q:
            case Operand::r5900_R: return 0;
            case Operand::r5900_ACCxyzw: return kXyzw;
            case Operand::r5900_vfs: return kRd;
            case Operand::r5900_vft: return kRt;
            case Operand::r5900_vfsxyzw: return kRd | kXyzw;
            case Operand::r5900_vftxyzw: return kRt | kXyzw;
            case Operand::r5900_vfdxyzw: return kSa | kXyzw;
            case Operand::r5900_vftn: return kRt | 0x3u;
            case Operand::r5900_vfsl: return kRd | 0x00600000u;
            case Operand::r5900_vftm: return kRt | 0x01800000u;
            case Operand::r5900_vis:
            case Operand::r5900_vis_postincr:
            case Operand::r5900_vis_predecr:
            case Operand::r5900_vis_parenthesis: return kRd;
            case Operand::r5900_vit:
            case Operand::r5900_vit_postincr:
            case Operand::r5900_vit_predecr: return kRt;
            case Operand::r5900_vid: return kSa;
            case Operand::r5900_immediate5: return kSa;
            case Operand::r5900_immediate15: return 0x001FFFC0u;
            }
            return 0;
        }

        uint32_t validBits(const Decoded &d)
        {
            // the mandatory fields hold the word's own bits there
            uint32_t bits = d.mandatory;
            for (Operand op : kDescriptors[d.id].operands)
            {
                if (op == Operand::None)
                    break;
                bits |= operandBits(op);
            }
            return bits;
        }

        bool isValid(const Decoded &d)
        {
            return kDescriptors[d.id].valid && (d.word & ~validBits(d)) == 0;
        }

        // GNU as would not assemble these back to the same word (cvt.w.s is trunc.w.s on the EE; vclipw / vsqrt
        // have forms it does not take): rabbitizer writes them as data
        bool asData(const Decoded &d)
        {
            if (d.id == ID_cpu_trunc_w_s || d.id == ID_cpu_cvt_w_s || d.id == ID_r5900_vclipw || d.id == ID_r5900_vsqrt)
                return true;
            return !isValid(d);
        }

        void hexImmediate(std::string &out, int32_t value)
        {
            char buffer[24];
            if (value < 0)
                std::snprintf(buffer, sizeof(buffer), "-0x%X", static_cast<unsigned>(-static_cast<int64_t>(value)));
            else
                std::snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned>(value));
            out += buffer;
        }

        int32_t immediate(const Decoded &d)
        {
            const uint32_t raw = d.word & 0xFFFFu;
            if (kDescriptors[d.id].flags & F_isUnsigned)
                return static_cast<int32_t>(raw);
            return static_cast<int32_t>(static_cast<int16_t>(raw));
        }

        void operandText(std::string &out, const Decoded &d, Operand op, uint32_t address)
        {
            const uint32_t w = d.word;
            char buffer[32];
            switch (op)
            {
            case Operand::None: return;
            case Operand::cpu_rs: out += kGprNames[rs(w)]; return;
            case Operand::cpu_rt: out += kGprNames[rt(w)]; return;
            case Operand::cpu_rd: out += kGprNames[field(w, 11, 5)]; return;
            case Operand::cpu_zero: out += kGprNames[0]; return;
            case Operand::cpu_cop0d: out += kCop0Names[field(w, 11, 5)]; return;
            case Operand::cpu_fs: out += kCop1Names[field(w, 11, 5)]; return;
            case Operand::cpu_ft: out += kCop1Names[field(w, 16, 5)]; return;
            case Operand::cpu_fd: out += kCop1Names[field(w, 6, 5)]; return;
            case Operand::cpu_cop1cs: out += kCop1ControlNames[field(w, 11, 5)]; return;
            case Operand::cpu_cop2t: out += kCop2Names[field(w, 16, 5)]; return;
            case Operand::cpu_cop2cd: out += kCop2Names[field(w, 11, 5)]; return;
            case Operand::cpu_sa:
                std::snprintf(buffer, sizeof(buffer), "%u", field(w, 6, 5));
                out += buffer;
                return;
            case Operand::cpu_op:
            case Operand::cpu_hint:
                std::snprintf(buffer, sizeof(buffer), "0x%02X", field(w, 16, 5));
                out += buffer;
                return;
            case Operand::cpu_code:
            {
                const uint32_t upper = field(w, 16, 10), lower = field(w, 6, 10);
                std::snprintf(buffer, sizeof(buffer), "%u", upper);
                out += buffer;
                if (lower)
                {
                    std::snprintf(buffer, sizeof(buffer), ", %u", lower);
                    out += buffer;
                }
                return;
            }
            case Operand::cpu_code_lower:
                std::snprintf(buffer, sizeof(buffer), "%u", field(w, 6, 10));
                out += buffer;
                return;
            case Operand::cpu_copraw:
                std::snprintf(buffer, sizeof(buffer), "0x%X", field(w, 0, 25));
                out += buffer;
                return;
            case Operand::cpu_label:
            {
                uint32_t target = field(w, 0, 26) << 2;
                target |= address == 0 ? 0x80000000u : ((address + 4u) & 0xF0000000u);
                std::snprintf(buffer, sizeof(buffer), "func_%06X", target);
                out += buffer;
                return;
            }
            case Operand::cpu_immediate: hexImmediate(out, immediate(d)); return;
            case Operand::cpu_branch_target_label:
                out += ". + 4 + (";
                hexImmediate(out, immediate(d));
                out += " << 2)";
                return;
            case Operand::cpu_immediate_base:
                hexImmediate(out, immediate(d));
                out += '(';
                out += kGprNames[rs(w)];
                out += ')';
                return;
            case Operand::cpu_maybe_rd_rs:
                if (field(w, 11, 5) != 31) // jalr's rd is shown unless it is $ra
                {
                    out += kGprNames[field(w, 11, 5)];
                    out += ", ";
                }
                out += kGprNames[rs(w)];
                return;
            case Operand::r5900_I: out += "$I"; return;
            case Operand::r5900_Q: out += "$Q"; return;
            case Operand::r5900_R: out += "$R"; return;
            case Operand::r5900_ACCxyzw: out += "$ACC"; return;
            case Operand::r5900_vfs:
            case Operand::r5900_vfsxyzw: out += kVfNames[field(w, 11, 5)]; return;
            case Operand::r5900_vft:
            case Operand::r5900_vftxyzw: out += kVfNames[field(w, 16, 5)]; return;
            case Operand::r5900_vfdxyzw: out += kVfNames[field(w, 6, 5)]; return;
            case Operand::r5900_vftn:
                out += kVfNames[field(w, 16, 5)];
                out += "xyzw"[field(w, 0, 2)];
                return;
            case Operand::r5900_vfsl:
                out += kVfNames[field(w, 11, 5)];
                out += "xyzw"[field(w, 21, 2)];
                return;
            case Operand::r5900_vftm:
                out += kVfNames[field(w, 16, 5)];
                out += "xyzw"[field(w, 23, 2)];
                return;
            case Operand::r5900_vis: out += kViNames[field(w, 11, 5)]; return;
            case Operand::r5900_vit: out += kViNames[field(w, 16, 5)]; return;
            case Operand::r5900_vid: out += kViNames[field(w, 6, 5)]; return;
            case Operand::r5900_vis_predecr:
                out += "(--";
                out += kViNames[field(w, 11, 5)];
                out += ')';
                return;
            case Operand::r5900_vit_predecr:
                out += "(--";
                out += kViNames[field(w, 16, 5)];
                out += ')';
                return;
            case Operand::r5900_vis_postincr:
                out += '(';
                out += kViNames[field(w, 11, 5)];
                out += "++)";
                return;
            case Operand::r5900_vit_postincr:
                out += '(';
                out += kViNames[field(w, 16, 5)];
                out += "++)";
                return;
            case Operand::r5900_vis_parenthesis:
                out += '(';
                out += kViNames[field(w, 11, 5)];
                out += ')';
                return;
            case Operand::r5900_immediate5:
            {
                const uint32_t raw = field(w, 6, 5);
                hexImmediate(out, (raw & 0x10u) ? static_cast<int32_t>(raw) - 32 : static_cast<int32_t>(raw));
                return;
            }
            case Operand::r5900_immediate15: hexImmediate(out, static_cast<int32_t>(field(w, 6, 15) * 8u)); return;
            }
        }

        // the instruction as text: name, xyzw suffix, operands after the name padded to 11 columns
        void instructionText(std::string &out, const Decoded &d, uint32_t address)
        {
            const InstrDescriptor &desc = kDescriptors[d.id];
            const size_t start = out.size();
            out += desc.name;
            if (desc.xyzwSuffix && (d.word & kXyzw))
            {
                out += '.';
                if (d.word & (1u << 24)) out += 'x';
                if (d.word & (1u << 23)) out += 'y';
                if (d.word & (1u << 22)) out += 'z';
                if (d.word & (1u << 21)) out += 'w';
            }
            if (desc.operands[0] == Operand::None)
                return;
            const size_t length = out.size() - start;
            if (length < 11)
                out.append(11 - length, ' ');
            out += ' ';
            for (size_t i = 0; i < 5 && desc.operands[i] != Operand::None; ++i)
            {
                if (i)
                    out += ", ";
                operandText(out, d, desc.operands[i], address);
            }
        }
    }

    Facts examine(uint32_t word)
    {
        const Decoded d = decode(word);
        const uint32_t flags = kDescriptors[d.id].flags;
        Facts f;
        f.isBranch = flags & F_isBranch;
        f.isJump = flags & F_isJump;
        f.isTrap = flags & F_isTrap;
        f.doesLoad = flags & F_doesLoad;
        f.doesStore = flags & F_doesStore;
        f.modifiesRs = flags & F_modifiesRs;
        f.modifiesRt = flags & F_modifiesRt;
        f.modifiesRd = flags & F_modifiesRd;
        f.doesLink = flags & F_doesLink;
        f.modifiesFs = flags & F_modifiesFs;
        f.modifiesFt = flags & F_modifiesFt;
        f.modifiesFd = flags & F_modifiesFd;
        f.modifiesHI = flags & F_modifiesHI;
        f.modifiesLO = flags & F_modifiesLO;
        f.isFunctionCall = f.doesLink;
        f.isReturn = d.id == ID_cpu_jr && rs(word) == 31;
        f.hasDelaySlot = f.isBranch || f.isJump;
        f.isEret = d.id == ID_cpu_eret;
        return f;
    }

    std::string disassemble(uint32_t word, uint32_t address)
    {
        const Decoded d = decode(word);
        std::string out;
        out.reserve(96);
        if (!asData(d))
        {
            instructionText(out, d, address);
            return out;
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), ".word       0x%08X", word);
        out += buffer;
        out.append(40 - out.size(), ' ');
        out += " # ";
        instructionText(out, d, address);
        std::snprintf(buffer, sizeof(buffer), " # %08X <InstrIdType: ", word & ~validBits(d));
        out += buffer;
        out += d.idType;
        out += '>';
        return out;
    }
}
