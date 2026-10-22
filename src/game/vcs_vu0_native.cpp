// Native versions of VCS's two hottest VU0 microprograms (about a quarter of the EE thread when interpreted:
// they read MAC flags, so the interpreter runs them on its exact path). Transcribed pair by pair from the
// microcode; the maths uses the same per-lane rules as the compiled VU1 code (runtime/ps2_vu1_compiled.h), so
// results are bit-identical to the interpreter. Registered by code hash: they only run while VU0 code memory
// holds exactly the game's programs. PS2X_VU0_VERIFY=1 checks them against the interpreter on live data.
#include "ps2_runtime.h"
#include "runtime/ps2_vu0_native.h"
#include "runtime/ps2_vu1_compiled.h"

#include <cstdint>
#include <cstring>

namespace
{
    using vu1v::fused;
    using vu1v::load;
    using vu1v::operand;
    using vu1v::splat;
    using vu1v::store;

    struct Vu0Regs
    {
        float vf[32][4];
        float acc[4];
        int16_t vi[16];
    };

    void loadRegs(const R5900Context *ctx, Vu0Regs &r)
    {
        for (int i = 0; i < 32; ++i)
            _mm_storeu_ps(r.vf[i], ctx->vu0_vf[i]);
        r.vf[0][0] = r.vf[0][1] = r.vf[0][2] = 0.0f; // as copyVu0ContextToState
        r.vf[0][3] = 1.0f;
        _mm_storeu_ps(r.acc, ctx->vu0_acc);
        for (int i = 0; i < 16; ++i)
            r.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        r.vi[0] = 0;
    }

    void storeRegs(const Vu0Regs &r, R5900Context *ctx)
    {
        for (int i = 1; i < 32; ++i)
            ctx->vu0_vf[i] = _mm_loadu_ps(r.vf[i]);
        ctx->vu0_acc = _mm_loadu_ps(r.acc);
        for (int i = 1; i < 16; ++i)
            ctx->vi[i] = static_cast<uint16_t>(r.vi[i]);
    }

    // VF = a * b.bc  (MUL / MULA with a broadcast)
    vu1v::V mulBc(const float *a, const float *b, int bc) { return operand(vu1v::mul(operand(load(a)), splat(vu1fast::operand(b[bc])))); }
    // ACC + a * b.bc  (MADD / MADDA)
    vu1v::V maddBc(const float *acc, const float *a, const float *b, int bc)
    {
        return fused<0xFu, false>(operand(load(acc)), operand(load(a)), splat(vu1fast::operand(b[bc])));
    }

    // MAC sign flags of a full-dest result, as FMAND sees them: bit 7 = x ... bit 4 = w
    int16_t signFlags(const float *v)
    {
        int16_t flags = 0;
        for (int c = 0; c < 4; ++c)
        {
            uint32_t bits;
            std::memcpy(&bits, &v[c], 4);
            if (bits & 0x80000000u)
                flags |= static_cast<int16_t>(0x80 >> c);
        }
        return flags;
    }

    void lqi(Vu0Regs &r, int vf, int vi, const uint8_t *data, uint32_t size)
    {
        const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[vi])) * 16u) & (size - 1u);
        std::memcpy(r.vf[vf], data + addr, 16);
        r.vi[vi] = static_cast<int16_t>(r.vi[vi] + 1);
    }

    void sqi(Vu0Regs &r, int vf, int vi, uint8_t *data, uint32_t size)
    {
        const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[vi])) * 16u) & (size - 1u);
        std::memcpy(data + addr, r.vf[vf], 16);
        r.vi[vi] = static_cast<int16_t>(r.vi[vi] + 1);
    }

    // VU0 0x0CF8: sector box vs three plane sets (FUN_002F7528 via FUN_0030C8D4). In: vf1 = box min, vf2 = box max,
    // vi15 = VU address of the matrices. Out: vi1 / vi2 = per set the sign flags of the box's most negative /
    // most positive transformed corner (4 bits each, shifted in by 4 per set).
    void boxClip(R5900Context *ctx, uint8_t *data, uint32_t size)
    {
        Vu0Regs r;
        loadRegs(ctx, r);
        r.vi[4] = static_cast<int16_t>(r.vi[15] + 24);                     // 0cf8 IADDIU vi4, vi15, 24
        store<0xFu>(r.vf[31], mulBc(r.vf[0], r.vf[0], 0));                 // 0d00 MULx vf31, vf0, vf0x | IADD vi1 = 0
        r.vi[1] = 0;
        store<0x7u>(r.vf[2], operand(vu1v::sub(operand(load(r.vf[2])), operand(load(r.vf[1]))))); // 0d08 SUB.xyz | vi2 = 0
        r.vi[2] = 0;
        r.vi[3] = 3;                                                        // 0d10
        r.vi[5] = 240;                                                      // 0d18
        lqi(r, 3, 4, data, size);                                           // 0d20..0d38
        lqi(r, 4, 4, data, size);
        lqi(r, 5, 4, data, size);
        lqi(r, 6, 4, data, size);
        do
        {
            r.vi[1] = static_cast<int16_t>(r.vi[1] << 4);                   // 0d40..0d58 IADD vi1, vi1, vi1 (x4)
            store<0xFu>(r.acc, mulBc(r.vf[3], r.vf[1], 0));                 // 0d40 MULAx  acc = vf3 * vf1x
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[4], r.vf[1], 1));         // 0d48 MADDAy
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[5], r.vf[1], 2));         // 0d50 MADDAz
            store<0xFu>(r.vf[7], maddBc(r.acc, r.vf[6], r.vf[0], 3));       // 0d58 MADDw vf7 = acc + vf6 * 1
            store<0xFu>(r.vf[8], mulBc(r.vf[3], r.vf[2], 0));               // 0d60 MULx vf8
            store<0xFu>(r.vf[9], mulBc(r.vf[4], r.vf[2], 1));               // 0d68 MULy vf9
            store<0xFu>(r.vf[10], mulBc(r.vf[5], r.vf[2], 2));              // 0d70 MULz vf10
            r.vi[2] = static_cast<int16_t>(r.vi[2] << 4);                   // 0d60..0d78 IADD vi2, vi2, vi2 (x4)
            for (int i = 0; i < 3; ++i)                                     // 0d80..0da8 MINI / MAX against vf31 (0)
            {
                store<0xFu>(r.vf[11 + i], vu1v::min(operand(load(r.vf[8 + i])), operand(load(r.vf[31]))));
            }
            for (int i = 0; i < 3; ++i)
            {
                store<0xFu>(r.vf[14 + i], vu1v::max(operand(load(r.vf[8 + i])), operand(load(r.vf[31]))));
            }
            store<0xFu>(r.acc, mulBc(r.vf[7], r.vf[0], 3));                 // 0db0 MULAw acc = vf7 | LQI vf3 (next set)
            lqi(r, 3, 4, data, size);
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[11], r.vf[0], 3));        // 0db8 MADDAw | LQI vf4
            lqi(r, 4, 4, data, size);
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[12], r.vf[0], 3));        // 0dc0 MADDAw | LQI vf5
            lqi(r, 5, 4, data, size);
            store<0xFu>(r.vf[17], maddBc(r.acc, r.vf[13], r.vf[0], 3));     // 0dc8 MADDw vf17 (most negative corner) | LQI vf6
            lqi(r, 6, 4, data, size);
            store<0xFu>(r.acc, mulBc(r.vf[7], r.vf[0], 3));                 // 0dd0 MULAw acc = vf7
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[14], r.vf[0], 3));        // 0dd8 MADDAw
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[15], r.vf[0], 3));        // 0de0 MADDAw
            store<0xFu>(r.vf[18], maddBc(r.acc, r.vf[16], r.vf[0], 3));     // 0de8 MADDw vf18 (most positive corner)
            r.vi[6] = static_cast<int16_t>(r.vi[5] & signFlags(r.vf[17]));  // 0de8 FMAND: flags of 0dc8 (4 pairs earlier)
            r.vi[1] = static_cast<int16_t>(r.vi[1] | r.vi[6]);              // 0df0 IOR
            r.vi[3] = static_cast<int16_t>(r.vi[3] - 1);                    // 0df8 ISUBIU
            r.vi[7] = static_cast<int16_t>(r.vi[5] & signFlags(r.vf[18]));  // 0e08 FMAND: flags of 0de8
            r.vi[2] = static_cast<int16_t>(r.vi[2] | r.vi[7]);              // 0e18 IOR (delay slot of 0e10 IBNE)
        } while (r.vi[3] != 0);
        storeRegs(r, ctx);
    }

    // VU0 0x0EB8: three 4x4 matrix products. In: vf1..vf4 = vectors, vi15 = VU address; matrices at vi15+12,
    // results to vi15+24 (12 quadwords).
    void matrixTimes3(R5900Context *ctx, uint8_t *data, uint32_t size)
    {
        Vu0Regs r;
        loadRegs(ctx, r);
        r.vi[1] = static_cast<int16_t>(r.vi[15] + 12);                      // 0eb8
        r.vi[2] = static_cast<int16_t>(r.vi[15] + 24);                      // 0ec0
        r.vi[3] = 3;                                                        // 0ec8
        do
        {
            lqi(r, 5, 1, data, size);                                       // 0ed0..0ee8
            lqi(r, 6, 1, data, size);
            lqi(r, 7, 1, data, size);
            lqi(r, 8, 1, data, size);
            for (int v = 0; v < 4; ++v)                                     // 0ef0..0f68: vf13+v = M * vf(1+v)
            {
                const float *in = r.vf[1 + v];
                store<0xFu>(r.acc, mulBc(r.vf[5], in, 0));
                store<0xFu>(r.acc, maddBc(r.acc, r.vf[6], in, 1));
                store<0xFu>(r.acc, maddBc(r.acc, r.vf[7], in, 2));
                store<0xFu>(r.vf[13 + v], maddBc(r.acc, r.vf[8], in, 3));
            }
            r.vi[3] = static_cast<int16_t>(r.vi[3] - 1);                    // 0f68 ISUBIU
            sqi(r, 13, 2, data, size);                                      // 0f70..0f80
            sqi(r, 14, 2, data, size);
            sqi(r, 15, 2, data, size);
            sqi(r, 16, 2, data, size);                                      // 0f90 (delay slot of 0f88 IBNE)
        } while (r.vi[3] != 0);
        storeRegs(r, ctx);
    }

    struct Registrar
    {
        Registrar()
        {
            ps2RegisterVu0Native(0x0CF8u, 0x138u, 0x3679405981306074ull, &boxClip, "VCS VU0 sector box clip (0x0CF8)");
            ps2RegisterVu0Native(0x0EB8u, 0x0F0u, 0xEA4DCB2F62C1EC30ull, &matrixTimes3, "VCS VU0 3x matrix product (0x0EB8)");
        }
    } registrar;
}
