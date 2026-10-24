// VU arithmetic the C++ references of VCS's VU1 programs reproduce bit-exactly (docs/NATIVE_RENDERER.md): every
// MULA / MADD step rounded on its own (no FP contraction), under FE_TOWARDZERO set by the caller - the runtime's VU1
// core runs the same way. Same semantics as the helpers in vcs_hle_world.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vcsvu
{
    constexpr uint32_t kQwords = 1024u; // VU1 data memory

    struct Vec4
    {
        float v[4];
    };

    struct IVec4
    {
        int32_t v[4];
    };

    inline Vec4 loadF(const uint8_t *vu, uint32_t q)
    {
        Vec4 r;
        std::memcpy(r.v, vu + (q & (kQwords - 1u)) * 16u, 16);
        return r;
    }

    inline IVec4 loadI(const uint8_t *vu, uint32_t q)
    {
        IVec4 r;
        std::memcpy(r.v, vu + (q & (kQwords - 1u)) * 16u, 16);
        return r;
    }

    inline Vec4 itof(const IVec4 &a)
    {
        return {{static_cast<float>(a.v[0]), static_cast<float>(a.v[1]), static_cast<float>(a.v[2]), static_cast<float>(a.v[3])}};
    }

    inline int32_t ftoi(float x, float scale) // FTOIn: truncate, saturate
    {
        const double d = static_cast<double>(x * scale);
        if (!(d == d))
            return 0;
        if (d >= 2147483647.0)
            return 2147483647;
        if (d <= -2147483648.0)
            return static_cast<int32_t>(0x80000000u);
        return static_cast<int32_t>(d);
    }

    // rows firstRow..+3 folded with the position scale (42) / offset (43) as the VU does
    inline void foldMatrix(const uint8_t *vu, uint32_t firstRow, Vec4 m[4])
    {
        for (int i = 0; i < 4; ++i)
            m[i] = loadF(vu, firstRow + i);
        const Vec4 s = loadF(vu, 42), o = loadF(vu, 43);
        Vec4 r3;
        for (int c = 0; c < 4; ++c)
        {
            float acc = m[0].v[c] * o.v[0];
            acc = acc + m[1].v[c] * o.v[1];
            acc = acc + m[2].v[c] * o.v[2];
            r3.v[c] = acc + m[3].v[c] * o.v[3];
        }
        m[3] = r3;
        for (int c = 0; c < 4; ++c)
        {
            m[0].v[c] *= s.v[0];
            m[1].v[c] *= s.v[1];
            m[2].v[c] *= s.v[2];
        }
    }

    // ACC = r0*x + r1*y + r2*z; r3*w + ACC
    inline Vec4 transform(const Vec4 m[4], const Vec4 &p, float w = 1.0f)
    {
        Vec4 r;
        for (int c = 0; c < 4; ++c)
        {
            float acc = m[0].v[c] * p.v[0];
            acc = acc + m[1].v[c] * p.v[1];
            acc = acc + m[2].v[c] * p.v[2];
            r.v[c] = acc + m[3].v[c] * w;
        }
        return r;
    }

    inline uint32_t clipFlags(const Vec4 &p, float w) // CLIP: +x -x +y -y +z -z
    {
        const float aw = std::fabs(w);
        uint32_t f = 0;
        for (int c = 0; c < 3; ++c)
        {
            if (p.v[c] > aw)
                f |= 1u << (c * 2);
            if (p.v[c] < -aw)
                f |= 2u << (c * 2);
        }
        return f;
    }

    // VU float edge cases as the runtime's VU1 core has them (runtime/ps2_vu1_compiled.h, vu1fast): operands with
    // exponent 255 are the largest finite value (the VU has no Inf / NaN), denormals zero; results clamp the same way,
    // multiply-adds decided on the exact value near zero / the top. Needed where garbage can reach the arithmetic
    // (vertices past a packet's count); identical to plain float maths for ordinary values.
    inline float vuOperand(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0x7F800000u)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    inline float vuExact(float value, double exact)
    {
        const double magnitude = exact < 0.0 ? -exact : exact;
        const uint32_t sign = std::signbit(exact) ? 0x80000000u : 0u;
        uint32_t bits;
        if (magnitude == 0.0 || magnitude < 1.17549435e-38)
            bits = sign;
        else if (magnitude > 3.40282347e+38)
            bits = sign | 0x7F7FFFFFu;
        else
            return value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    inline float vuMul(float a, float b) // MUL / MULA
    {
        a = vuOperand(a);
        b = vuOperand(b);
        return vuOperand(a * b);
    }

    inline float vuAdd(float a, float b) // ADD (vu1fast::plainResult)
    {
        return vuOperand(vuOperand(a) + vuOperand(b));
    }

    inline float vuSub(float a, float b) // SUB
    {
        return vuOperand(vuOperand(a) - vuOperand(b));
    }

    inline float vuMadd(float acc, float a, float b) // MADD / MADDA: acc + a * b
    {
        a = vuOperand(a);
        b = vuOperand(b);
        return vuExact(acc + a * b, static_cast<double>(acc) + static_cast<double>(a) * static_cast<double>(b));
    }

    inline float bitsToFloat(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
}
