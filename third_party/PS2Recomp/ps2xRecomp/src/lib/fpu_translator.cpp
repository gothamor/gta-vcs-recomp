#include "ps2recomp/Translators/fpu_translator.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include "ps2recomp/format.h"
#include <sstream>
#include <cmath>

namespace ps2recomp
{
    FpuTranslator::FpuTranslator(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FpuTranslator::translate(const Instruction &inst)
    {
        uint8_t format = inst.rs; // Format field
        uint32_t ft = inst.rt;    // FPU source register
        uint32_t fs = inst.rd;    // FPU source register
        uint32_t fd = inst.sa;    // FPU destination register
        uint32_t function = inst.function;

        switch (format)
        {
        case COP1_MF:
            return ps2recomp::format("{{ uint32_t bits; std::memcpy(&bits, &ctx->f[{}], sizeof(bits)); SET_GPR_U32(ctx, {}, bits); }}", fs, ft);
        case COP1_MT:
            return ps2recomp::format("{{ uint32_t bits = GPR_U32(ctx, {}); std::memcpy(&ctx->f[{}], &bits, sizeof(bits)); }}", ft, fs);
        case COP1_CF:
            if (fs == 31)
                return ps2recomp::format("SET_GPR_U32(ctx, {}, ctx->fcr31);", ft); // FCR31 contains status/control
            if (fs == 0)
                return ps2recomp::format("SET_GPR_U32(ctx, {}, 0x00000000);", ft); // FCR0 is the FPU implementation register
            return ps2recomp::format("SET_GPR_U32(ctx, {}, 0); // Unimplemented FCR{}", ft, fs);
        case COP1_CT:
            if (fs == 31)
                return ps2recomp::format("ctx->fcr31 = GPR_U32(ctx, {}) & 0x0183FFFF;", ft);
            else
                return ps2recomp::format("// CTC1 to FCR{} ignored", fs);
            return "";
        case COP1_BC:
            return "// FPU branch instruction - handled elsewhere";
        case COP1_S:
            switch (function)
            {
            case COP1_S_ADD:
                return ps2recomp::format("ctx->f[{}] = FPU_ADD_S(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_SUB:
                return ps2recomp::format("ctx->f[{}] = FPU_SUB_S(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_MUL:
                return ps2recomp::format("ctx->f[{}] = FPU_MUL_S(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_DIV:
                // The EE has no Inf/NaN: x/0 gives +-max (sign of fs xor ft), see ps2_fdiv.
                return ps2recomp::format("{{ if (ctx->f[{}] == 0.0f) ctx->fcr31 |= 0x100000; /* DZ flag */ "
                                   "ctx->f[{}] = FPU_DIV_S(ctx->f[{}], ctx->f[{}]); }}",
                                   ft, fd, fs, ft);
            case COP1_S_SQRT:
                // [gta-recomp] R5900 SQRT.S fd, ft: the operand is in the ft field (fs is 0).
                // Upstream read fs, so every sqrtf computed sqrt($f0).
                return ps2recomp::format("ctx->f[{}] = FPU_SQRT_S(ctx->f[{}]);", fd, ft);
            case COP1_S_ABS:
                return ps2recomp::format("ctx->f[{}] = FPU_ABS_S(ctx->f[{}]);", fd, fs);
            case COP1_S_MOV:
                return ps2recomp::format("ctx->f[{}] = FPU_MOV_S(ctx->f[{}]);", fd, fs);
            case COP1_S_NEG:
                return ps2recomp::format("ctx->f[{}] = FPU_NEG_S(ctx->f[{}]);", fd, fs);
            case COP1_S_ROUND_W:
                return ps2recomp::format("{{ int32_t tmp = FPU_ROUND_W_S(ctx->f[{}]); std::memcpy(&ctx->f[{}], &tmp, sizeof(tmp)); }}", fs, fd);
            case COP1_S_TRUNC_W:
                return ps2recomp::format("{{ int32_t tmp = FPU_TRUNC_W_S(ctx->f[{}]); std::memcpy(&ctx->f[{}], &tmp, sizeof(tmp)); }}", fs, fd);
            case COP1_S_CEIL_W:
                return ps2recomp::format("{{ int32_t tmp = FPU_CEIL_W_S(ctx->f[{}]); std::memcpy(&ctx->f[{}], &tmp, sizeof(tmp)); }}", fs, fd);
            case COP1_S_FLOOR_W:
                return ps2recomp::format("{{ int32_t tmp = FPU_FLOOR_W_S(ctx->f[{}]); std::memcpy(&ctx->f[{}], &tmp, sizeof(tmp)); }}", fs, fd);
            case COP1_S_CVT_W:
                return ps2recomp::format("{{ int32_t tmp = FPU_CVT_W_S(ctx->f[{}]); std::memcpy(&ctx->f[{}], &tmp, sizeof(tmp)); }}", fs, fd);
            case COP1_S_RSQRT:
                // RSQRT.S fd, fs, ft computes fs / sqrt(ft), not 1 / sqrt(fs).
                return ps2recomp::format("ctx->f[{}] = FPU_RSQRT_S(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_ADDA:
                return ps2recomp::format("FPU_SET_ACC(ctx, FPU_ADD_S(ctx->f[{}], ctx->f[{}]));", fs, ft);
            case COP1_S_SUBA:
                return ps2recomp::format("FPU_SET_ACC(ctx, FPU_SUB_S(ctx->f[{}], ctx->f[{}]));", fs, ft);
            case COP1_S_MULA:
                return ps2recomp::format("FPU_SET_ACC(ctx, FPU_MUL_S(ctx->f[{}], ctx->f[{}]));", fs, ft);
            case COP1_S_MADD:
                return ps2recomp::format("ctx->f[{}] = FPU_ADD_S(ctx->f_acc, FPU_MUL_S(ctx->f[{}], ctx->f[{}]));", fd, fs, ft);
            case COP1_S_MSUB:
                return ps2recomp::format("ctx->f[{}] = FPU_SUB_S(ctx->f_acc, FPU_MUL_S(ctx->f[{}], ctx->f[{}]));", fd, fs, ft);
            case COP1_S_MADDA:
                return ps2recomp::format("FPU_SET_ACC(ctx, FPU_ADD_S(ctx->f_acc, FPU_MUL_S(ctx->f[{}], ctx->f[{}])));", fs, ft);
            case COP1_S_MSUBA:
                return ps2recomp::format("FPU_SET_ACC(ctx, FPU_SUB_S(ctx->f_acc, FPU_MUL_S(ctx->f[{}], ctx->f[{}])));", fs, ft);
            case COP1_S_MAX:
                return ps2recomp::format("ctx->f[{}] = std::max(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_MIN:
                return ps2recomp::format("ctx->f[{}] = std::min(ctx->f[{}], ctx->f[{}]);", fd, fs, ft);
            case COP1_S_C_F:
                return ps2recomp::format("ctx->fcr31 &= ~0x800000;");
            case COP1_S_C_UN:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_UN_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_EQ:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_EQ_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_UEQ:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_UEQ_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_OLT:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_OLT_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_ULT:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_ULT_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_OLE:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_OLE_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_ULE:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_ULE_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_SF:
                return ps2recomp::format("ctx->fcr31 &= ~0x800000;");
            case COP1_S_C_NGLE:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_NGLE_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_SEQ:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_SEQ_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_NGL:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_NGL_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_LT:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_LT_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_NGE:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_NGE_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_LE:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_LE_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            case COP1_S_C_NGT:
                return ps2recomp::format("ctx->fcr31 = (FPU_C_NGT_S(ctx->f[{}], ctx->f[{}])) ? (ctx->fcr31 | 0x800000) : (ctx->fcr31 & ~0x800000);", fs, ft);
            default:
                return m_codeGenerator.emitUnhandledInstruction(inst, ps2recomp::format("Unhandled FPU.S instruction: function 0x{:X}", function));
            }
        case COP1_W:
            switch (function)
            {
            case COP1_W_CVT_S:
                return ps2recomp::format("{{ int32_t tmp; std::memcpy(&tmp, &ctx->f[{}], sizeof(tmp)); ctx->f[{}] = FPU_CVT_S_W(tmp); }}", fs, fd);
            default:
                return m_codeGenerator.emitUnhandledInstruction(inst, ps2recomp::format("Unhandled FPU.W instruction: function 0x{:X}", function));
            }
        default:
            return m_codeGenerator.emitUnhandledInstruction(inst, ps2recomp::format("Unhandled FPU instruction: format 0x{:X}, function 0x{:X}", format, function));
        }
    }

}
