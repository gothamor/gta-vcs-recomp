// VCS world geometry pipeline (VU1 program 0x0030 with the prelit-world stage table), reimplemented in C++ - the
// reference for a native (GPU) version. See docs/ENGINE.md "VU1 renderer pipeline" for what each routine does.
//
// PS2X_HLE_VERIFY=1: for every world packet, predict the VU1's decision (culled / clipper / fast path) and the fast
// path's output (ST, RGBA, XYZF per vertex), let the VU1 run, and compare. Reports every 10 s on stderr.
#include "runtime/ps2_hle_survey.h"

#include <algorithm>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
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

    Vec4 loadF(const uint8_t *vu, uint32_t q)
    {
        Vec4 r;
        std::memcpy(r.v, vu + (q & (kQwords - 1u)) * 16u, 16);
        return r;
    }

    IVec4 loadI(const uint8_t *vu, uint32_t q)
    {
        IVec4 r;
        std::memcpy(r.v, vu + (q & (kQwords - 1u)) * 16u, 16);
        return r;
    }

    Vec4 itof(const IVec4 &a)
    {
        return {{static_cast<float>(a.v[0]), static_cast<float>(a.v[1]), static_cast<float>(a.v[2]), static_cast<float>(a.v[3])}};
    }

    int32_t ftoi(float x, float scale)
    {
        const double d = static_cast<double>(x * scale); // VU FTOIn: truncate, saturate
        if (!(d == d))
            return 0;
        if (d >= 2147483647.0)
            return 2147483647;
        if (d <= -2147483648.0)
            return static_cast<int32_t>(0x80000000u);
        return static_cast<int32_t>(d);
    }

    // rows r0..r3 folded with scale s / offset o exactly as the VU does (MULAx/MADDAy/MADDAz/MADDw, then MULx/y/z)
    void foldMatrix(const uint8_t *vu, uint32_t firstRow, Vec4 m[4])
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

    Vec4 transform(const Vec4 m[4], const Vec4 &p) // ACC = r0*x + r1*y + r2*z; r3*1 + ACC
    {
        Vec4 r;
        for (int c = 0; c < 4; ++c)
        {
            float acc = m[0].v[c] * p.v[0];
            acc = acc + m[1].v[c] * p.v[1];
            acc = acc + m[2].v[c] * p.v[2];
            r.v[c] = acc + m[3].v[c] * 1.0f;
        }
        return r;
    }

    uint32_t clipFlags(const Vec4 &p, float w) // CLIP: +x -x +y -y +z -z
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

    uint64_t fnv(const uint8_t *p, uint32_t from, uint32_t to, uint64_t h)
    {
        for (uint32_t i = from; i < to; ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return h;
    }

    // the code of every routine the reference reproduces (prelit world variant at 0x2700)
    bool isWorldPipeline(const uint8_t *code, const uint8_t *vu)
    {
        const IVec4 t38 = loadI(vu, 38), t40 = loadI(vu, 40), t41 = loadI(vu, 41), t52 = loadI(vu, 52);
        if ((t38.v[0] & 0xFFFF) != 0x19 || (t40.v[2] & 0xFFFF) != 0xDA || (t41.v[0] & 0xFFFF) != 0x4E0 ||
            (t41.v[1] & 0xFFFF) != 0x9C || (t41.v[2] & 0xFFFF) != 0x2B6 || (t52.v[0] & 0xFFFF) != 0xE9)
            return false;
        static const uint32_t ranges[][2] = {{0x30, 0x118}, {0x6D0, 0x748}, {0x748, 0x7E0}, {0x7F0, 0xC20}, {0xC38, 0x1030},
                                             {0x1048, 0x11F8}, {0x1360, 0x13D8}, {0x15B0, 0x1638}, {0x2700, 0x28A8}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        return h == 0x100E28E3AB20E014ull;
    }

    enum class Decision
    {
        None,
        Culled,
        Clipper,
        Fast,
    };

    struct Prediction
    {
        Decision decision = Decision::None;
        uint32_t outBase = 0, count = 0, nloop = 0;
        uint32_t expected[256][12]; // per vertex: STQ (float bits), RGBA, XYZF
        uint64_t vertsBefore = 0;
    } g_pred;

    struct Stats
    {
        uint64_t packets = 0, culled = 0, clipper = 0, fast = 0;
        uint64_t culledOk = 0, culledBad = 0, fastExact = 0, fastClose = 0, fastBad = 0, vertsExact = 0, vertsBad = 0;
        uint64_t field[3] = {}; // mismatching STQ / RGBA / XYZF qwords
        uint32_t logged = 0;
    } g_stats;

    auto g_reportAt = std::chrono::steady_clock::now();

    void report()
    {
        const Stats &s = g_stats;
        std::fprintf(stderr,
                     "[hle-verify] world packets %llu: culled %llu (%llu ok / %llu drew), clipper %llu, fast %llu "
                     "(%llu exact, %llu within 1, %llu off; verts %llu exact / %llu off; off qwords ST %llu RGBA %llu XYZF %llu)\n",
                     (unsigned long long)s.packets, (unsigned long long)s.culled, (unsigned long long)s.culledOk,
                     (unsigned long long)s.culledBad, (unsigned long long)s.clipper, (unsigned long long)s.fast,
                     (unsigned long long)s.fastExact, (unsigned long long)s.fastClose, (unsigned long long)s.fastBad,
                     (unsigned long long)s.vertsExact, (unsigned long long)s.vertsBad, (unsigned long long)s.field[0],
                     (unsigned long long)s.field[1], (unsigned long long)s.field[2]);
    }

    bool predict(const ps2hle::MscalContext &c);

    // the VU rounds toward zero (the runtime's VU1 core runs under FE_TOWARDZERO too)
    bool before(const ps2hle::MscalContext &c)
    {
        const int rounding = std::fegetround();
        std::fesetround(FE_TOWARDZERO);
        const bool handled = predict(c);
        std::fesetround(rounding);
        return handled;
    }

    bool predict(const ps2hle::MscalContext &c)
    {
        g_pred.decision = Decision::None;
        if (c.startPc != 0x30u || !isWorldPipeline(c.vuCode, c.vuData))
            return false;
        const uint8_t *vu = c.vuData;
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        const uint32_t top = c.top & 0x3FFu, mask = static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + (top & mask);
        const uint32_t uvBase = static_cast<uint32_t>(i47.v[2]) + (top & mask);
        const uint32_t colBase = static_cast<uint32_t>(i47.v[3]) + (top & mask);
        const uint32_t outBase = static_cast<uint32_t>(i48.v[3]) + top;
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        if (count == 0u || count > 256u)
            return false;
        ++g_stats.packets;

        // clip test (0x0c38): clip matrix 24-27, guard band scale 65; at least 6 vertices are tested
        Vec4 cm[4];
        foldMatrix(vu, 24, cm);
        const Vec4 guard = loadF(vu, 65);
        uint32_t any = 0, all = 0xFFFu;
        for (uint32_t i = 0; i < std::max(count, 6u); ++i)
        {
            const Vec4 p = transform(cm, itof(loadI(vu, posBase + i)));
            Vec4 g;
            for (int k = 0; k < 4; ++k)
                g.v[k] = p.v[k] * guard.v[k];
            const uint32_t f = (clipFlags(g, p.v[3]) << 6) | clipFlags(p, p.v[3]);
            any |= f;
            all &= f;
        }
        g_pred.vertsBefore = ps2hle::g_gsVerticesRecorded;
        if (all != 0u)
        {
            g_pred.decision = Decision::Culled;
            ++g_stats.culled;
            return false;
        }
        if (any & 0xFC0u)
        {
            g_pred.decision = Decision::Clipper;
            ++g_stats.clipper;
            return false;
        }

        // fast path: transform (0x07f0), fog (0x1048), colour (stage 0x2700 with vi01 = 0x9c)
        Vec4 m[4];
        foldMatrix(vu, 28, m);
        const float inv255 = []() { float f; const uint32_t b = 0x3B808081u; std::memcpy(&f, &b, 4); return f; }();
        Vec4 uvm[3] = {loadF(vu, 44), loadF(vu, 45), loadF(vu, 46)};
        for (auto &r : uvm)
            for (int k = 0; k < 3; ++k)
                r.v[k] *= inv255;
        const bool fog = (loadI(vu, 52).v[1] & 2) != 0;
        const Vec4 fogp = loadF(vu, 53);
        const Vec4 cs = loadF(vu, 0x9C), co = loadF(vu, 0x9D), cmax = loadF(vu, 0x9E);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Vec4 p = transform(m, itof(loadI(vu, posBase + i)));
            const Vec4 uv = itof(loadI(vu, uvBase + i));
            float st[3];
            for (int k = 0; k < 3; ++k)
            {
                float acc = uvm[0].v[k] * uv.v[0];
                acc = acc + uvm[1].v[k] * uv.v[1];
                st[k] = acc + uvm[2].v[k] * uv.v[2];
            }
            const float q = 1.0f / p.v[3];
            uint32_t *e = g_pred.expected[i];
            float stq[4] = {st[0] * q, st[1] * q, st[2] * q, p.v[3]};
            std::memcpy(e, stq, 16);
            const Vec4 col = itof(loadI(vu, colBase + i));
            for (int k = 0; k < 4; ++k)
            {
                const float r = co.v[k] + cs.v[k] * col.v[k];
                e[4 + k] = static_cast<uint32_t>(ftoi(std::min(r, cmax.v[k]), 1.0f));
            }
            for (int k = 0; k < 3; ++k)
                e[8 + k] = static_cast<uint32_t>(ftoi(p.v[k] * q, 16.0f));
            float f = 0.0f;
            if (fog)
                f = std::max(std::min(fogp.v[3] * p.v[3] + 1.0f * fogp.v[2], 255.0f), 0.0f);
            e[11] = static_cast<uint32_t>(ftoi(f, 16.0f));
        }
        g_pred.decision = Decision::Fast;
        g_pred.outBase = outBase;
        g_pred.count = count;
        g_pred.nloop = static_cast<uint32_t>(header.v[3]);
        ++g_stats.fast;
        return false;
    }

    void after(const ps2hle::MscalContext &c)
    {
        if (g_pred.decision == Decision::None)
            return;
        Stats &s = g_stats;
        if (g_pred.decision == Decision::Culled)
        {
            const bool drew = ps2hle::g_gsVerticesRecorded != g_pred.vertsBefore;
            (drew ? s.culledBad : s.culledOk) += 1;
        }
        else if (g_pred.decision == Decision::Fast)
        {
            bool exact = true, close = true;
            for (uint32_t i = 0; i < g_pred.count; ++i)
            {
                uint32_t got[12];
                for (int k = 0; k < 3; ++k)
                    std::memcpy(got + k * 4, c.vuData + ((g_pred.outBase + 1u + i * 3u + k) & (kQwords - 1u)) * 16u, 16);
                const uint32_t *e = g_pred.expected[i];
                bool vertExact = true;
                for (int k = 0; k < 3; ++k)
                {
                    bool fieldExact = true, fieldClose = true;
                    for (int l = 0; l < 4; ++l)
                    {
                        const uint32_t a = got[k * 4 + l], b = e[k * 4 + l];
                        if (a == b)
                            continue;
                        fieldExact = false;
                        // float lanes: 2 ulp; integer lanes: 1
                        const int64_t d = static_cast<int64_t>(static_cast<int32_t>(a)) - static_cast<int32_t>(b);
                        if (std::llabs(d) > (k == 0 ? 2 : 1))
                            fieldClose = false;
                    }
                    if (!fieldExact)
                    {
                        ++s.field[k];
                        vertExact = false;
                    }
                    close = close && fieldClose;
                }
                exact = exact && vertExact;
                (vertExact ? s.vertsExact : s.vertsBad) += 1;
                if (!vertExact && s.logged < 12)
                {
                    ++s.logged;
                    std::fprintf(stderr, "[hle-verify] bits vu %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x\n"
                                         "[hle-verify] bits ref %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                                 got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7], got[8], got[9], got[10], got[11],
                                 e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11]);
                    float gf[4], ef[4];
                    std::memcpy(gf, got, 16);
                    std::memcpy(ef, e, 16);
                    std::fprintf(stderr,
                                 "[hle-verify] vertex %u/%u: STQ vu (%g %g %g %g) ref (%g %g %g %g); RGBA vu (%d %d %d %d) ref (%d %d %d %d); "
                                 "XYZF vu (%d %d %d %d) ref (%d %d %d %d)\n",
                                 i, g_pred.count, gf[0], gf[1], gf[2], gf[3], ef[0], ef[1], ef[2], ef[3], (int)got[4], (int)got[5],
                                 (int)got[6], (int)got[7], (int)e[4], (int)e[5], (int)e[6], (int)e[7], (int)got[8], (int)got[9],
                                 (int)got[10], (int)got[11], (int)e[8], (int)e[9], (int)e[10], (int)e[11]);
                }
            }
            if (exact)
                ++s.fastExact;
            else if (close)
                ++s.fastClose;
            else
                ++s.fastBad;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - g_reportAt >= std::chrono::seconds(10))
        {
            g_reportAt = now;
            report();
        }
    }

    struct Registrar
    {
        Registrar()
        {
            if (std::getenv("PS2X_HLE_VERIFY"))
                ps2hle::setMscalHooks(&before, &after);
        }
    } registrar;
}
