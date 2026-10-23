// VCS world geometry pipeline (VU1 program 0x0030 with the prelit-world stage table), reimplemented in C++ - the
// reference for a native (GPU) version. See docs/game-internals.md "VU1 renderer pipeline" for what each routine does.
//
// PS2X_HLE_VERIFY=1: for every world packet, predict the VU1's decision (culled / clipper / fast path) and the fast
// path's output (ST, RGBA, XYZF per vertex) - or for clipper packets the C++ clipper's GIF packets
// (vcs_vu_clipper.cpp) - let the VU1 run, and compare. Reports every 10 s on stderr.
// Default (PS2X_HLE=0 turns it off): draw world packets natively instead (GS::submitNative, vertex program vs_world in
// src/render/shaders/gs_draw.hlsl): the VU1 program is skipped; culled packets draw nothing; packets the VU would
// clip go through the C++ clipper, whose GIF packets the GS gets as the VU's kicks (PS2X_HLE_CLIPPER=0: the VU1;
// PS2X_HLE_CLIPPED=1: GPU clipping at the clip test's near / far planes - wrong in places, see drawNative).
#include "runtime/ps2_hle_survey.h"
#include "runtime/ps2_vu1_tap.h"
#include "vcs_vu_clipper.h"
#include "vcs_vu_math.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

    // the code of every routine the reference reproduces (prelit world variant at 0x2700); hashed again only after
    // VU1 code memory changed
    bool worldCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        static const uint32_t ranges[][2] = {{0x30, 0x118}, {0x6D0, 0x748}, {0x748, 0x7E0}, {0x7F0, 0xC20}, {0xC38, 0x1030},
                                             {0x1048, 0x11F8}, {0x1360, 0x13D8}, {0x15B0, 0x1638}, {0x2700, 0x28A8}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        checkedGeneration = generation;
        resident = h == 0x100E28E3AB20E014ull;
        return resident;
    }

    // the clipper's code (0x11f8 fog, 0x13d8 / 0x1440, 0x1638-0x26ff clipper and emitters), which vcs_vu_clipper.cpp
    // reproduces
    bool clipperCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        static const uint32_t ranges[][2] = {{0x11F8, 0x1360}, {0x13D8, 0x15B0}, {0x1638, 0x2700}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        checkedGeneration = generation;
        resident = h == 0xBE56D05105F3611Full;
        static int logged = 0;
        if (!resident && logged++ < 4)
            std::fprintf(stderr, "[hle] clipper code hash %016llx (not the reproduced one): clipped packets run on the VU1\n",
                         (unsigned long long)h);
        return resident;
    }

    bool isWorldPipeline(const uint8_t *code, const uint8_t *vu, uint64_t generation)
    {
        const IVec4 t38 = loadI(vu, 38), t40 = loadI(vu, 40), t41 = loadI(vu, 41), t52 = loadI(vu, 52);
        if ((t38.v[0] & 0xFFFF) != 0x19 || (t40.v[2] & 0xFFFF) != 0xDA || (t41.v[0] & 0xFFFF) != 0x4E0 ||
            (t41.v[1] & 0xFFFF) != 0x9C || (t41.v[2] & 0xFFFF) != 0x2B6)
            return false;
        // 52.x: 0x0e9 = clip test first (0x0748); 0x0de = straight to the fast path (0x06f0) - the EE already found the
        // sector inside the view (VU0 box clip)
        const uint32_t entry = t52.v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        return worldCodeResident(code, generation);
    }

    // ---- lit / skinned models: stage table 0x16 (0x5a5 skin, 0x0da, 0x4ff light, 0x2b6 kick), lit-pass code --------
    bool litCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        static const uint32_t ranges[][2] = {{0x30, 0x118}, {0x6D0, 0x748}, {0x748, 0x7E0}, {0x7F0, 0xC20}, {0xC38, 0x1030},
                                             {0x1048, 0x11F8}, {0x1360, 0x13D8}, {0x15B0, 0x1638}, {0x2700, 0x3400}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        checkedGeneration = generation;
        resident = h == 0x1B498A6A0D9BD196ull;
        return resident;
    }

    bool isLitPipeline(const uint8_t *code, const uint8_t *vu, uint64_t generation)
    {
        const IVec4 t38 = loadI(vu, 38), t40 = loadI(vu, 40), t41 = loadI(vu, 41), t52 = loadI(vu, 52);
        if ((t38.v[0] & 0xFFFF) != 0x16 || (t40.v[0] & 0xFFFF) != 0x5A5 || (t40.v[1] & 0xFFFF) != 0xA8 || (t40.v[2] & 0xFFFF) != 0xDA ||
            (t41.v[0] & 0xFFFF) != 0x4FF || (t41.v[1] & 0xFFFF) != 0x9C || (t41.v[2] & 0xFFFF) != 0x2B6)
            return false;
        const uint32_t entry = t52.v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        return litCodeResident(code, generation);
    }

    float bitsToFloat(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    struct LitPacket
    {
        uint32_t posBase = 0, nrmBase = 0, uvBase = 0, wBase = 0, outBase = 0, count = 0, nloop = 0;
        float pos[256][3];
        float nrm[256][3];
    };

    bool litPacket(const ps2hle::MscalContext &c, LitPacket &lp)
    {
        const uint8_t *vu = c.vuData;
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        const uint32_t top = c.top & 0x3FFu, base = top & static_cast<uint32_t>(i48.v[2]);
        lp.posBase = static_cast<uint32_t>(i47.v[0]) + base;
        lp.nrmBase = static_cast<uint32_t>(i47.v[1]) + base;
        lp.uvBase = static_cast<uint32_t>(i47.v[2]) + base;
        lp.wBase = static_cast<uint32_t>(i48.v[0]) + base;
        lp.outBase = static_cast<uint32_t>(i48.v[3]) + top;
        const IVec4 header = loadI(vu, lp.posBase - 1u);
        lp.count = static_cast<uint32_t>(header.v[2]);
        lp.nloop = std::min(static_cast<uint32_t>(header.v[3]), lp.count);
        return lp.count != 0u && lp.count <= 256u;
    }

    // stage 0x5a5 (0x2d28): 4-bone skinning, in the VU's operation order
    void skin(const uint8_t *vu, LitPacket &lp)
    {
        const uint32_t mask = static_cast<uint32_t>(loadI(vu, 0xA8).v[1]);
        const Vec4 scale = loadF(vu, 42), offset = loadF(vu, 43);
        const float inv128 = bitsToFloat(0x3C000000u);
        for (uint32_t i = 0; i < lp.count; ++i)
        {
            const Vec4 pi = itof(loadI(vu, lp.posBase + i)), ni = itof(loadI(vu, lp.nrmBase + i));
            const IVec4 wi = loadI(vu, lp.wBase + i);
            const Vec4 w = loadF(vu, lp.wBase + i);
            float p[3], n[3];
            for (int k = 0; k < 3; ++k)
            {
                const float acc = pi.v[k] * scale.v[k];
                p[k] = acc + offset.v[k] * 1.0f;
                n[k] = ni.v[k] * inv128;
            }
            float t[4][3];
            Vec4 b0[4];
            for (int bone = 0; bone < 4; ++bone)
            {
                const uint32_t at = 0xA9u + (static_cast<uint32_t>(wi.v[bone]) & mask);
                Vec4 r[4] = {loadF(vu, at), loadF(vu, at + 1), loadF(vu, at + 2), loadF(vu, at + 3)};
                if (bone == 0)
                    for (int k = 0; k < 4; ++k)
                        b0[k] = r[k];
                for (int k = 0; k < 3; ++k)
                {
                    float acc = r[0].v[k] * p[0];
                    acc = acc + r[1].v[k] * p[1];
                    acc = acc + r[2].v[k] * p[2];
                    t[bone][k] = acc + r[3].v[k] * 1.0f;
                }
            }
            for (int k = 0; k < 3; ++k)
            {
                float acc = b0[0].v[k] * n[0];
                acc = acc + b0[1].v[k] * n[1];
                lp.nrm[i][k] = acc + b0[2].v[k] * n[2];
                acc = t[0][k] * w.v[0];
                acc = acc + t[1][k] * w.v[1];
                acc = acc + t[2][k] * w.v[2];
                lp.pos[i][k] = acc + t[3][k] * w.v[3];
            }
        }
    }

    // clip test on the skinned positions (lit pass 0x0c38: clip matrix 24-27 as is); returns (any, all) flags
    void litClip(const uint8_t *vu, const LitPacket &lp, uint32_t &any, uint32_t &all)
    {
        Vec4 cm[4] = {loadF(vu, 24), loadF(vu, 25), loadF(vu, 26), loadF(vu, 27)};
        const Vec4 guard = loadF(vu, 65);
        any = 0;
        all = 0xFFFu;
        for (uint32_t i = 0; i < std::max(lp.count, 6u); ++i)
        {
            Vec4 pos;
            if (i < lp.count)
                pos = {{lp.pos[i][0], lp.pos[i][1], lp.pos[i][2], 1.0f}};
            else
                pos = loadF(vu, lp.posBase + i); // padding vertices: whatever the buffer holds
            const Vec4 pc = transform(cm, pos);
            Vec4 g;
            for (int k = 0; k < 4; ++k)
                g.v[k] = pc.v[k] * guard.v[k];
            const uint32_t f = (clipFlags(g, pc.v[3]) << 6) | clipFlags(pc, pc.v[3]);
            any |= f;
            all &= f;
        }
    }

    // stage 0x4ff (0x27f8, vi01 = 0x9c): lighting constants as the VU prepares them
    struct LitLights
    {
        Vec4 dir[3], col[4], ambient, clamp;
        int32_t alpha = 0;
    };

    LitLights litLights(const uint8_t *vu)
    {
        LitLights L;
        const float inv510 = bitsToFloat(0x3B008081u);
        const IVec4 mi = loadI(vu, 51);
        Vec4 mat = itof(mi);
        for (int k = 0; k < 4; ++k)
            mat.v[k] *= inv510;
        for (int r = 0; r < 3; ++r)
            L.dir[r] = loadF(vu, 0x9Cu + 9u + r);
        for (int r = 0; r < 4; ++r)
        {
            L.col[r] = loadF(vu, 0x9Cu + 3u + r);
            for (int k = 0; k < 4; ++k)
                L.col[r].v[k] *= mat.v[k];
        }
        L.ambient = loadF(vu, 0x9Cu + 7u);
        for (int k = 0; k < 4; ++k)
            L.ambient.v[k] *= mat.v[k];
        L.clamp = loadF(vu, 0x9Cu + 8u);
        L.alpha = mi.v[3];
        return L;
    }

    LitLights rigidLights(const uint8_t *vu)
    {
        LitLights L = litLights(vu);
        const float inv128 = bitsToFloat(0x3C000000u);
        for (auto &row : L.dir)
            for (int k = 0; k < 4; ++k)
                row.v[k] *= inv128;
        return L;
    }

    void litColour(const LitLights &L, const float n[3], int32_t out[3])
    {
        float d[4];
        for (int k = 0; k < 4; ++k)
        {
            float acc = L.dir[0].v[k] * n[0];
            acc = acc + L.dir[1].v[k] * n[1];
            d[k] = std::max(0.0f, acc + L.dir[2].v[k] * n[2]);
        }
        for (int k = 0; k < 3; ++k)
        {
            float acc = L.col[0].v[k] * d[0];
            acc = acc + L.col[1].v[k] * d[1];
            acc = acc + L.col[2].v[k] * d[2];
            acc = acc + L.col[3].v[k] * d[3];
            const float c = acc + L.ambient.v[k] * 1.0f;
            out[k] = ftoi(std::min(L.clamp.v[k], c), 1.0f);
        }
    }

    // ---- rigid lit models (vehicles, props): the world pipeline with lighting stage 0x4ff (rigid-lit upload) --------
    bool rigidCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        static const uint32_t ranges[][2] = {{0x30, 0x118}, {0x6D0, 0x748}, {0x748, 0x7E0}, {0x7F0, 0xC20}, {0xC38, 0x1030},
                                             {0x1048, 0x11F8}, {0x1360, 0x13D8}, {0x15B0, 0x1638}, {0x2700, 0x3400}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        checkedGeneration = generation;
        resident = h == 0x0061E55208AD3296ull;
        return resident;
    }

    bool isRigidPipeline(const uint8_t *code, const uint8_t *vu, uint64_t generation)
    {
        const IVec4 t38 = loadI(vu, 38), t40 = loadI(vu, 40), t41 = loadI(vu, 41), t52 = loadI(vu, 52);
        if ((t38.v[0] & 0xFFFF) != 0x19 || (t40.v[2] & 0xFFFF) != 0xDA || (t41.v[0] & 0xFFFF) != 0x4FF ||
            (t41.v[1] & 0xFFFF) != 0x9C || (t41.v[2] & 0xFFFF) != 0x2B6)
            return false;
        const uint32_t entry = t52.v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        return rigidCodeResident(code, generation);
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
        bool litLanes = false;      // lit packets: STQ.w and fog not compared
        uint64_t vertsBefore = 0;
        bool clipChecked = false;   // prelit clipper packets: the C++ clipper's GIF packets, compared with the VU's kicks
        bool envKicks = false;      // ... or a table-0x13 packet's three kicks (envKicks)
        std::vector<std::vector<uint8_t>> clipPackets, vuKicks;
    } g_pred;

    struct Stats
    {
        uint64_t packets = 0, culled = 0, clipper = 0, fast = 0;
        uint64_t culledOk = 0, culledBad = 0, fastExact = 0, fastClose = 0, fastBad = 0, vertsExact = 0, vertsBad = 0;
        uint64_t field[3] = {}; // mismatching STQ / RGBA / XYZF qwords
        uint32_t logged = 0;
        uint64_t clipExact = 0, clipBad = 0, clipUnsupported = 0, clipPacketsOk = 0, clipPacketsBad = 0;
        uint64_t envExact = 0, envBad = 0; // stage table 0x13 packets: the three kicks
    } g_stats;

    auto g_reportAt = std::chrono::steady_clock::now();

    void report()
    {
        const Stats &s = g_stats;
        std::fprintf(stderr,
                     "[hle-verify] world packets %llu: culled %llu (%llu ok / %llu drew), clipper %llu, fast %llu "
                     "(%llu exact, %llu within 1, %llu off; verts %llu exact / %llu off; off qwords ST %llu RGBA %llu XYZF %llu); "
                     "C++ clipper %llu exact / %llu off / %llu not reproduced (GIF packets %llu ok / %llu off); env pass %llu exact / %llu off\n",
                     (unsigned long long)s.packets, (unsigned long long)s.culled, (unsigned long long)s.culledOk,
                     (unsigned long long)s.culledBad, (unsigned long long)s.clipper, (unsigned long long)s.fast,
                     (unsigned long long)s.fastExact, (unsigned long long)s.fastClose, (unsigned long long)s.fastBad,
                     (unsigned long long)s.vertsExact, (unsigned long long)s.vertsBad, (unsigned long long)s.field[0],
                     (unsigned long long)s.field[1], (unsigned long long)s.field[2], (unsigned long long)s.clipExact,
                     (unsigned long long)s.clipBad, (unsigned long long)s.clipUnsupported, (unsigned long long)s.clipPacketsOk,
                     (unsigned long long)s.clipPacketsBad, (unsigned long long)s.envExact, (unsigned long long)s.envBad);
    }

    // PS2X_CLIP_DUMP=<dir>[,<n>]: the first n (default 300) world packets the VU sends through its clipper, as
    // <dir>/clip_NNNN.bin - u32 magic 'CLP1', u32 top, u32 rigid, VU1 data memory before the program (16 KB), u32
    // kick count, then per XGKICK u32 size + the packet. For developing / checking vcs_vu_clipper.cpp offline: load
    // a file, run vcsclip::clipWorld under FE_TOWARDZERO, compare with the non-empty kicks.
    struct ClipDump
    {
        std::vector<uint8_t> before;
        std::vector<std::vector<uint8_t>> kicks;
        uint32_t top = 0, rigid = 0, written = 0;
        bool pending = false;
    } g_clipDump;

    void clipDumpBegin(const ps2hle::MscalContext &c, uint32_t kind) // kind: 0 prelit world, 1 rigid lit, 2 skinned lit
    {
        static const char *env = std::getenv("PS2X_CLIP_DUMP");
        static const uint32_t limit = [] {
            const char *comma = env ? std::strchr(env, ',') : nullptr;
            return comma ? static_cast<uint32_t>(std::atoi(comma + 1)) : 300u;
        }();
        g_pred.vuKicks.clear();
        g_pred.clipPackets.clear();
        g_pred.clipChecked = false;
        g_pred.envKicks = false;
        if (kind == 0u)
        {
            g_pred.clipChecked = true;
            if (!vcsclip::clipWorld(c.vuData, c.top, g_pred.clipPackets))
            {
                g_pred.clipChecked = false;
                ++g_stats.clipUnsupported;
            }
        }
        ps2vu1tap::g_kicks = &g_pred.vuKicks;
        if (!env || g_clipDump.written >= limit)
            return;
        g_clipDump.before.assign(c.vuData, c.vuData + kQwords * 16u);
        g_clipDump.top = c.top;
        g_clipDump.rigid = kind;
        g_clipDump.pending = true;
    }

    void clipCompare();

    void clipDumpEnd()
    {
        if (ps2vu1tap::g_kicks)
        {
            ps2vu1tap::g_kicks = nullptr;
            clipCompare();
        }
        if (!g_clipDump.pending)
            return;
        g_clipDump.pending = false;
        g_clipDump.kicks = g_pred.vuKicks;
        static const std::string dir = [] {
            std::string d = std::getenv("PS2X_CLIP_DUMP");
            const size_t comma = d.find(',');
            return comma == std::string::npos ? d : d.substr(0, comma);
        }();
        char name[64];
        std::snprintf(name, sizeof(name), "/clip_%04u.bin", g_clipDump.written++);
        if (FILE *f = std::fopen((dir + name).c_str(), "wb"))
        {
            const uint32_t head[3] = {0x31504C43u, g_clipDump.top, g_clipDump.rigid};
            std::fwrite(head, 4, 3, f);
            std::fwrite(g_clipDump.before.data(), 1, g_clipDump.before.size(), f);
            const uint32_t n = static_cast<uint32_t>(g_clipDump.kicks.size());
            std::fwrite(&n, 4, 1, f);
            for (const auto &k : g_clipDump.kicks)
            {
                const uint32_t size = static_cast<uint32_t>(k.size());
                std::fwrite(&size, 4, 1, f);
                std::fwrite(k.data(), 1, k.size(), f);
            }
            std::fclose(f);
        }
    }

    // the C++ clipper (vcs_vu_clipper.cpp) against the VU's kicks of the same packet: tags exact, per vertex ST / Q,
    // RGBA and XYZF2 exact (STQ.w is not read by the GS); the empty sync kicks (qword 58) are skipped
    void clipCompare()
    {
        if (!g_pred.clipChecked)
            return;
        std::vector<const std::vector<uint8_t> *> vu;
        for (const auto &k : g_pred.vuKicks)
        {
            uint32_t t0 = 0;
            std::memcpy(&t0, k.data(), 4);
            if (!(k.size() == 16u && (t0 & 0x7FFFu) == 0u))
                vu.push_back(&k);
        }
        const auto &mine = g_pred.clipPackets;
        bool exact = vu.size() == mine.size();
        for (size_t i = 0; i < std::min(vu.size(), mine.size()); ++i)
        {
            const auto &a = mine[i], &b = *vu[i];
            bool same = a.size() == b.size() && std::memcmp(a.data(), b.data(), 16) == 0;
            size_t q = 16;
            for (; same && q < a.size(); q += 16)
                same = std::memcmp(a.data() + q, b.data() + q, ((q - 16u) / 16u) % 3u == 0u ? 12u : 16u) == 0;
            if (!same && g_stats.logged < 12)
            {
                ++g_stats.logged;
                const size_t at = a.size() == b.size() ? (std::memcmp(a.data(), b.data(), 16) == 0 ? q - 16u : 0u) : 0u;
                uint32_t wa[4] = {}, wb[4] = {};
                if (at + 16u <= a.size())
                    std::memcpy(wa, a.data() + at, 16);
                if (at + 16u <= b.size())
                    std::memcpy(wb, b.data() + at, 16);
                std::fprintf(stderr, "[hle-verify] kick %zu (%zu / %zu bytes) qword %zu: mine %08x %08x %08x %08x vu %08x %08x %08x %08x\n", i,
                             a.size(), b.size(), at / 16u, wa[0], wa[1], wa[2], wa[3], wb[0], wb[1], wb[2], wb[3]);
            }
            (same ? g_stats.clipPacketsOk : g_stats.clipPacketsBad) += 1;
            exact = exact && same;
        }
        if (g_pred.envKicks)
            (exact ? g_stats.envExact : g_stats.envBad) += 1;
        else
            (exact ? g_stats.clipExact : g_stats.clipBad) += 1;
        if (!exact && g_stats.logged < 12)
        {
            ++g_stats.logged;
            std::fprintf(stderr, "[hle-verify] clipper packet differs: %zu GIF packets, VU %zu\n", mine.size(), vu.size());
        }
    }

    // The fast path's per-vertex output (STQ float bits, RGBA, XYZF2): transform 0x07f0 (matrix 28-31), fog 0x1048,
    // colour = prelit stage 0x2700 (vi01 = 0x9c) or, for rigid lit models, the lighting stage 0x27f8.
    void fastOutputs(const uint8_t *vu, uint32_t posBase, uint32_t uvBase, uint32_t colBase, uint32_t nrmBase, bool rigid,
                     uint32_t count, uint32_t (*out)[12])
    {
        Vec4 m[4];
        foldMatrix(vu, 28, m);
        const float inv255 = bitsToFloat(0x3B808081u);
        Vec4 uvm[3] = {loadF(vu, 44), loadF(vu, 45), loadF(vu, 46)};
        for (auto &r : uvm)
            for (int k = 0; k < 3; ++k)
                r.v[k] *= inv255;
        const bool fog = (loadI(vu, 52).v[1] & 2) != 0;
        const Vec4 fogp = loadF(vu, 53);
        const Vec4 cs = loadF(vu, 0x9C), co = loadF(vu, 0x9D), cmax = loadF(vu, 0x9E);
        LitLights lights;
        if (rigid)
            lights = rigidLights(vu);
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
            uint32_t *e = out[i];
            float stq[4] = {st[0] * q, st[1] * q, st[2] * q, p.v[3]};
            std::memcpy(e, stq, 16);
            if (rigid)
            {
                const Vec4 n = itof(loadI(vu, nrmBase + i));
                int32_t rgb[3];
                litColour(lights, n.v, rgb);
                e[4] = static_cast<uint32_t>(rgb[0]);
                e[5] = static_cast<uint32_t>(rgb[1]);
                e[6] = static_cast<uint32_t>(rgb[2]);
                e[7] = static_cast<uint32_t>(lights.alpha);
            }
            else
            {
                const Vec4 col = itof(loadI(vu, colBase + i));
                for (int k = 0; k < 4; ++k)
                {
                    const float r = co.v[k] + cs.v[k] * col.v[k];
                    e[4 + k] = static_cast<uint32_t>(ftoi(std::min(r, cmax.v[k]), 1.0f));
                }
            }
            for (int k = 0; k < 3; ++k)
                e[8 + k] = static_cast<uint32_t>(ftoi(p.v[k] * q, 16.0f));
            float f = 0.0f;
            if (fog)
                f = std::max(std::min(fogp.v[3] * p.v[3] + 1.0f * fogp.v[2], 255.0f), 0.0f);
            e[11] = static_cast<uint32_t>(ftoi(f, 16.0f));
        }
    }

    // ---- stage table 0x13: rigid lit models + environment map --------------------------------------------------------
    // 0x0da transform, 0x4ff lighting (0x9c), kick 0x2b6 (tag 49), then 0x5b1 (vi01 = 41.y, 0xa8): XGKICK the GIF packet
    // at vi01 (a TEXFLUSH), ST = (rows vi01+5..7 . xy / 256) * normal + 0.5, times the vertex's Q, over the output's ST;
    // kick 0x2b2 sends the same output again with tag 50 (context 2, the environment texture). game-internals.md 6d.
    bool envCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        static const uint32_t ranges[][2] = {{0x1590, 0x15B0}, {0x2D88, 0x2F70}};
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto &r : ranges)
            h = fnv(code, r[0], r[1], h);
        checkedGeneration = generation;
        resident = h == 0xA6FF2E08D4CC67F3ull && rigidCodeResident(code, generation);
        static int logged = 0;
        if (h != 0xA6FF2E08D4CC67F3ull && logged++ < 4)
            std::fprintf(stderr, "[hle] env pass code hash %016llx (not the reproduced one): table 0x13 runs on the VU1\n",
                         (unsigned long long)h);
        return resident;
    }

    bool isEnvPipeline(const uint8_t *code, const uint8_t *vu, uint64_t generation)
    {
        const IVec4 t38 = loadI(vu, 38), t39 = loadI(vu, 39), t40 = loadI(vu, 40), t41 = loadI(vu, 41), t52 = loadI(vu, 52);
        if ((t38.v[0] & 0xFFFF) != 0x13 || (t39.v[2] & 0xFFFF) != 0xDA || (t40.v[0] & 0xFFFF) != 0x4FF ||
            (t40.v[1] & 0xFFFF) != 0x9C || (t40.v[2] & 0xFFFF) != 0x2B6 || (t41.v[0] & 0xFFFF) != 0x5B1 ||
            (t41.v[2] & 0xFFFF) != 0x2B2)
            return false;
        const uint32_t entry = t52.v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        return envCodeResident(code, generation);
    }

    // the GIF packet at qword q (tags until EOP); empty when it does not fit
    void gifPacketAt(const uint8_t *vu, uint32_t q, std::vector<uint8_t> &out)
    {
        out.clear();
        for (uint32_t guard = 0; guard < 64u; ++guard)
        {
            uint64_t tag;
            std::memcpy(&tag, vu + (q & (kQwords - 1u)) * 16u, 8);
            const uint32_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u, nreg = ((tag >> 60) & 15u) ? ((tag >> 60) & 15u) : 16u;
            const uint32_t qwords = 1u + (flg == 0u ? nloop * nreg : flg == 1u ? (nloop * nreg + 1u) / 2u : nloop);
            if (out.size() / 16u + qwords > 256u)
            {
                out.clear();
                return;
            }
            for (uint32_t i = 0; i < qwords; ++i)
                out.insert(out.end(), vu + ((q + i) & (kQwords - 1u)) * 16u, vu + ((q + i) & (kQwords - 1u)) * 16u + 16u);
            q += qwords;
            if ((tag >> 15) & 1u)
                return;
        }
        out.clear();
    }

    // the kick of 0x15d0 for tag qword `tagQ`: tag (x = the header's .w) + n x (STQ, RGBA, XYZF2) from `outputs`
    void kickPacket(const uint8_t *vu, uint32_t tagQ, uint32_t headerW, const uint32_t (*outputs)[12], uint32_t n,
                    std::vector<uint8_t> &out)
    {
        const IVec4 t = loadI(vu, tagQ);
        // ILW.w + MFIR.x: the header's low 16 bits, sign-extended
        const uint32_t x = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(headerW & 0xFFFFu)));
        const uint32_t tag[4] = {x, static_cast<uint32_t>(t.v[1]), static_cast<uint32_t>(t.v[2]), static_cast<uint32_t>(t.v[3])};
        out.resize(16u + n * 48u);
        std::memcpy(out.data(), tag, 16);
        for (uint32_t i = 0; i < n; ++i)
            std::memcpy(out.data() + 16u + i * 48u, outputs[i], 48);
    }

    // 0x5b1's ST over the fast path's outputs (in place)
    void envST(const uint8_t *vu, uint32_t param, uint32_t nrmBase, uint32_t count, uint32_t (*outputs)[12])
    {
        const float scale = bitsToFloat(0x3B800000u); // I = 1/256 on the rows' x / y
        Vec4 m[3] = {loadF(vu, param + 5u), loadF(vu, param + 6u), loadF(vu, param + 7u)};
        for (auto &r : m)
        {
            r.v[0] = r.v[0] * scale;
            r.v[1] = r.v[1] * scale;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const Vec4 n = itof(loadI(vu, nrmBase + i));
            float q, st[2];
            std::memcpy(&q, &outputs[i][2], 4);
            for (int k = 0; k < 2; ++k)
            {
                float acc = m[0].v[k] * n.v[0];
                acc = acc + m[1].v[k] * n.v[1];
                const float e = acc + m[2].v[k] * n.v[2];
                st[k] = (0.5f + e) * q;
            }
            std::memcpy(&outputs[i][0], st, 8);
        }
    }

    // the three kicks of a fast-path table-0x13 packet: pass 1 (tag 49), the packet at 41.y, pass 2 (tag 50)
    bool envKicks(const uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &kicks)
    {
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        top &= 0x3FFu;
        const uint32_t base = top & static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + base, nrmBase = static_cast<uint32_t>(i47.v[1]) + base,
                       uvBase = static_cast<uint32_t>(i47.v[2]) + base;
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        const uint32_t n = static_cast<uint32_t>(header.v[3]) & 0x7FFFu;
        if (count == 0u || count > 256u || n > count)
            return false;
        static thread_local uint32_t outputs[256][12];
        fastOutputs(vu, posBase, uvBase, 0, nrmBase, true, count, outputs);
        const uint32_t param = static_cast<uint32_t>(loadI(vu, 41).v[1]) & 0x3FFu;
        kicks.resize(3);
        kickPacket(vu, 49, static_cast<uint32_t>(header.v[3]), outputs, n, kicks[0]);
        gifPacketAt(vu, param, kicks[1]);
        if (kicks[1].empty())
            return false;
        envST(vu, param, nrmBase, count, outputs);
        kickPacket(vu, 50, static_cast<uint32_t>(header.v[3]), outputs, n, kicks[2]);
        return true;
    }

    // ---- program 0x02c0, setup slot 0xc8 + entry 0x1c: instanced re-draws -------------------------------------------
    // 0x02c0 copies the packet's object matrix (4 qwords at TOP) to 0-3, 0x04d8 builds the clip matrix 24-27 (view 16-19
    // x object) and the window matrix 28-31 (viewport 12-15 x clip), setup slot 0xc8 (byte 0x640) the camera in object
    // space (33 = R^T (32 - T)); then the dispatcher's entry 0x1c runs only the transform stage 0x0da and the kick 0x2b6
    // (tag 49) - no colour stage: the RGBA the output buffer already holds is drawn (qword 51's colour when the clip test
    // ran, 52.x = 0x0e9). Meshes stay resident at absolute addresses (48.z mask 0). game-internals.md 6d.
    bool redrawCodeResident(const uint8_t *code, uint64_t generation)
    {
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (generation == checkedGeneration)
            return resident;
        const uint64_t h = fnv(code, 0x30, 0x2700, 0xCBF29CE484222325ull);
        checkedGeneration = generation;
        resident = h == 0x11DD6EA54A3E572Aull;
        static int logged = 0;
        if (!resident && logged++ < 4)
            std::fprintf(stderr, "[hle] re-draw code hash %016llx (not the reproduced one): 0x02c0 runs on the VU1\n",
                         (unsigned long long)h);
        return resident;
    }

    bool isRedraw(const ps2hle::MscalContext &c)
    {
        if (c.startPc != 0x2C0u)
            return false;
        const uint8_t *vu = c.vuData;
        const IVec4 t34 = loadI(vu, 34), t38 = loadI(vu, 38), t41 = loadI(vu, 41);
        if ((t34.v[0] & 0xFFFF) != 0xC8 || (loadI(vu, 35).v[0] & 0xFFFF) != 0x3A || (loadI(vu, 36).v[0] & 0xFFFF) != 0x3A ||
            (loadI(vu, 37).v[0] & 0xFFFF) != 0x3A || (t38.v[0] & 0xFFFF) != 0x1C || (t41.v[0] & 0xFFFF) != 0xDA ||
            (t41.v[2] & 0xFFFF) != 0x2B6)
            return false;
        const uint32_t entry = loadI(vu, 52).v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        return redrawCodeResident(c.vuCode, c.codeGeneration);
    }

    void storeF(uint8_t *vu, uint32_t q, const Vec4 &v, uint32_t lanes = 0xFu)
    {
        for (int k = 0; k < 4; ++k)
            if (lanes & (1u << k))
                std::memcpy(vu + (q & (kQwords - 1u)) * 16u + k * 4u, &v.v[k], 4);
    }

    // 0x02c0's setup: matrices 0-3, 24-27, 28-31 and qword 33 written into vu
    void redrawSetup(uint8_t *vu, uint32_t top)
    {
        Vec4 a[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            a[i] = loadF(vu, top + i);
            storeF(vu, i, a[i]);
        }
        const auto mul = [](const Vec4 b[4], const Vec4 &r) { // 0x04d8: ACC = b0 r.x + b1 r.y + b2 r.z; + b3 r.w
            Vec4 o;
            for (int c = 0; c < 4; ++c)
            {
                float acc = b[0].v[c] * r.v[0];
                acc = acc + b[1].v[c] * r.v[1];
                acc = acc + b[2].v[c] * r.v[2];
                o.v[c] = acc + b[3].v[c] * r.v[3];
            }
            return o;
        };
        const Vec4 view[4] = {loadF(vu, 16), loadF(vu, 17), loadF(vu, 18), loadF(vu, 19)};
        const Vec4 port[4] = {loadF(vu, 12), loadF(vu, 13), loadF(vu, 14), loadF(vu, 15)};
        Vec4 clip[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            clip[i] = mul(view, a[i]);
            storeF(vu, 24 + i, clip[i]);
        }
        for (uint32_t i = 0; i < 4; ++i)
            storeF(vu, 28 + i, mul(port, clip[i]));
        // 0x0640: the rotation transposed (ADDx from vf00: 0 + a), camera (32) minus the translation, rotated
        Vec4 rt[3];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                rt[r].v[k] = 0.0f + a[k].v[r];
        const Vec4 cam = loadF(vu, 32);
        float d[3];
        for (int k = 0; k < 3; ++k)
            d[k] = cam.v[k] - a[3].v[k];
        Vec4 o = loadF(vu, 33);
        for (int k = 0; k < 3; ++k)
        {
            float acc = rt[0].v[k] * d[0];
            acc = acc + rt[1].v[k] * d[1];
            o.v[k] = acc + rt[2].v[k] * d[2];
        }
        storeF(vu, 33, o, 0x7u);
    }

    // after redrawSetup: the program's VU memory writes and the GIF packets it kicks, for the code image 0x02c0 runs
    // with - its transform / clip test / fog (0x07f0 / 0x0c38 / 0x1048) are a float variant of the world's: positions
    // and UVs are floats (no ITOF0, no 42/43 fold, no texture matrix), RGBA is always qword 51, fog FTOI4'd into
    // XYZF.w. false: not reproduced (fog off, backface pass, the clipper) - run the VU1 on the untouched memory.
    // The float code images differ in how ST and RGBA are made: 0x02c0's re-draws use the UV qword as is and qword 51;
    // the prelit float image (variant C: world table 0x19 with float positions) runs the UVs through the texture
    // matrix (ITOF0, 44-46 / 255) and its colour stage 0x2700 copies the colour qwords over RGBA.
    struct FloatVariant
    {
        bool uvMatrix;
        bool rawColours;
        uint32_t clipMin, fastMin; // vertices the clip test / transform process at least
        bool fogOffZero;           // fog off: F = 0 (vf08 cleared by the transform); else F is a stale register: VU1
        bool epilogueSkew;         // the transform's epilogue stores the *next* vertex's w in STQ.w for vertices N-4..N-2
                                   // (N processed): its MOVE.w waits for the MULw before it (the fog reads w there)
    };
    constexpr FloatVariant kRedrawVariant{false, false, 5u, 6u, false, false};
    constexpr FloatVariant kPrelitFloatVariant{true, true, 6u, 7u, true, true};

    bool floatKicks(uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &kicks, const FloatVariant &fv)
    {
        kicks.clear();
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48), i52 = loadI(vu, 52);
        const bool fog = (i52.v[1] & 2) != 0;
        if ((i52.v[1] & 1) || (!fog && !fv.fogOffZero))
            return false;
        top &= 0x3FFu;
        const uint32_t base = top & static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + base, uvBase = static_cast<uint32_t>(i47.v[2]) + base;
        const uint32_t outBase = static_cast<uint32_t>(i48.v[3]) + top + 1u;
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        const uint32_t n = static_cast<uint32_t>(header.v[3]) & 0x7FFFu;
        if (count == 0u || count > 256u || n > count)
            return false;
        const Vec4 def = loadF(vu, 51);
        const auto xform = [](const Vec4 m[4], const Vec4 &p) { // ACC = r0 x + r1 y + r2 z; + r3 * 1
            Vec4 r;
            for (int c = 0; c < 4; ++c)
            {
                float acc = m[0].v[c] * p.v[0];
                acc = acc + m[1].v[c] * p.v[1];
                acc = acc + m[2].v[c] * p.v[2];
                r.v[c] = acc + m[3].v[c] * 1.0f;
            }
            return r;
        };
        Vec4 uvm[3] = {loadF(vu, 44), loadF(vu, 45), loadF(vu, 46)};
        const float inv255 = bitsToFloat(0x3B808081u);
        for (auto &r : uvm)
            for (int k = 0; k < 3; ++k)
                r.v[k] *= inv255;
        const auto uvTimesOne = [&](uint32_t q) { // ST: the UV as is, or ITOF0 through the texture matrix; then * 1
            Vec4 r, uv = loadF(vu, q);
            if (fv.uvMatrix)
            {
                const Vec4 u = itof(loadI(vu, q));
                for (int k = 0; k < 3; ++k)
                {
                    float acc = uvm[0].v[k] * u.v[0];
                    acc = acc + uvm[1].v[k] * u.v[1];
                    uv.v[k] = acc + uvm[2].v[k] * u.v[2];
                }
            }
            for (int k = 0; k < 3; ++k)
                r.v[k] = uv.v[k] * 1.0f;
            r.v[3] = 0.0f;
            return r;
        };
        if ((i52.v[0] & 0xFFFF) == 0xE9)
        {
            // clip test 0x0c38 (at least 5 vertices): ST, w | qword 51 | clip position; flags into the position's .w
            const Vec4 cm[4] = {loadF(vu, 24), loadF(vu, 25), loadF(vu, 26), loadF(vu, 27)};
            const Vec4 guard = loadF(vu, 65);
            const auto vuXform = [](const Vec4 m[4], const Vec4 &v) { // xform with the VU's edge cases (vertices past
                Vec4 r;                                               // the count may hold anything)
                for (int c = 0; c < 4; ++c)
                {
                    float acc = vcsvu::vuMul(m[0].v[c], v.v[0]);
                    acc = vcsvu::vuMadd(acc, m[1].v[c], v.v[1]);
                    acc = vcsvu::vuMadd(acc, m[2].v[c], v.v[2]);
                    r.v[c] = vcsvu::vuMadd(acc, m[3].v[c], 1.0f);
                }
                return r;
            };
            uint32_t any = 0, all = 0xFFFu;
            for (uint32_t i = 0; i < std::max(count, fv.clipMin); ++i)
            {
                const Vec4 p = vuXform(cm, loadF(vu, posBase + i));
                Vec4 st = uvTimesOne(uvBase + i);
                st.v[3] = p.v[3] * 1.0f;
                Vec4 g;
                for (int k = 0; k < 4; ++k)
                    g.v[k] = vcsvu::vuMul(p.v[k], guard.v[k]);
                const uint32_t f = (clipFlags(g, p.v[3]) << 6) | clipFlags(p, p.v[3]);
                any |= f;
                all &= f;
                storeF(vu, outBase + i * 3u, st);
                storeF(vu, outBase + i * 3u + 1u, def);
                storeF(vu, outBase + i * 3u + 2u, p);
                std::memcpy(vu + ((posBase + i) & (kQwords - 1u)) * 16u + 12u, &f, 4);
            }
            if (all != 0u)
                return true; // culled: only the empty kick of qword 58
            if (any & 0xFC0u)
            {
                // 0x11f8 fog into the position's .w (at least 5 vertices), the colour stage, then the clipper from memory
                const uint32_t colBase = static_cast<uint32_t>(i47.v[3]) + base;
                if (fog)
                {
                    const Vec4 fogp = loadF(vu, 53);
                    for (uint32_t i = 0; i < std::max(count, 5u); ++i)
                    {
                        float w;
                        std::memcpy(&w, vu + ((outBase + i * 3u) & (kQwords - 1u)) * 16u + 12u, 4);
                        const float acc = fogp.v[3] * w;
                        const float f = std::max(std::min(acc + 1.0f * fogp.v[2], 255.0f), 0.0f);
                        std::memcpy(vu + ((outBase + i * 3u + 2u) & (kQwords - 1u)) * 16u + 12u, &f, 4);
                    }
                }
                if (fv.rawColours)
                    for (uint32_t i = 0; i < count; ++i)
                        std::memcpy(vu + ((outBase + i * 3u + 1u) & (kQwords - 1u)) * 16u, vu + ((colBase + i) & (kQwords - 1u)) * 16u, 16);
                if (!vcsclip::clipFromOutput(vu, top, kicks))
                    return false;
                const int32_t variant = 711;
                std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
                return true;
            }
        }
        // fast path 0x06f0 (transform 0x07f0, fog 0x1048; at least 6 vertices): STQ, qword 51, XYZF2 with the fog
        const Vec4 m[4] = {loadF(vu, 28), loadF(vu, 29), loadF(vu, 30), loadF(vu, 31)};
        const Vec4 fogp = loadF(vu, 53);
        static thread_local uint32_t outputs[256][12];
        const uint32_t colBase = static_cast<uint32_t>(i47.v[3]) + base;
        const auto vuXformW = [&](uint32_t i) { // the w of vertex i with the VU's edge cases (it may be past the count)
            const Vec4 v = loadF(vu, posBase + i);
            float acc = vcsvu::vuMul(m[0].v[3], v.v[0]);
            acc = vcsvu::vuMadd(acc, m[1].v[3], v.v[1]);
            acc = vcsvu::vuMadd(acc, m[2].v[3], v.v[2]);
            return vcsvu::vuMadd(acc, m[3].v[3], 1.0f);
        };
        const uint32_t processed = std::max(count, fv.fastMin);
        for (uint32_t i = 0; i < processed; ++i)
        {
            const Vec4 p = xform(m, loadF(vu, posBase + i));
            const float q = 1.0f / p.v[3];
            const Vec4 st = uvTimesOne(uvBase + i);
            const float w = fv.epilogueSkew && i + 4u >= processed && i + 2u <= processed ? vuXformW(i + 1u) : p.v[3];
            const float stq[4] = {st.v[0] * q, st.v[1] * q, st.v[2] * q, w};
            float f = 0.0f;
            if (fog)
            {
                const float acc = fogp.v[3] * w;
                f = std::max(std::min(acc + 1.0f * fogp.v[2], 255.0f), 0.0f);
            }
            const int32_t xyzf[4] = {ftoi(p.v[0] * q, 16.0f), ftoi(p.v[1] * q, 16.0f), ftoi(p.v[2] * q, 16.0f), ftoi(f, 16.0f)};
            const uint32_t qo = outBase + i * 3u;
            std::memcpy(vu + (qo & (kQwords - 1u)) * 16u, stq, 16);
            storeF(vu, qo + 1u, def);
            if (fv.rawColours && i < count) // colour stage 0x2700 (this image): the colour qword copied
                std::memcpy(vu + ((qo + 1u) & (kQwords - 1u)) * 16u, vu + ((colBase + i) & (kQwords - 1u)) * 16u, 16);
            std::memcpy(vu + ((qo + 2u) & (kQwords - 1u)) * 16u, xyzf, 16);
            if (i < count)
            {
                std::memcpy(outputs[i], stq, 16);
                std::memcpy(outputs[i] + 4, vu + ((qo + 1u) & (kQwords - 1u)) * 16u, 16);
                std::memcpy(outputs[i] + 8, xyzf, 16);
            }
        }
        const int32_t variant = 698;
        std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
        kicks.emplace_back();
        kickPacket(vu, 49, static_cast<uint32_t>(header.v[3]), outputs, n, kicks.back());
        return true;
    }

    // the prelit float image: world stage table 0x19 (0x0da, 0x4e0 / 0x9c, 0x2b6), its own code below 0x2790
    bool isPrelitFloat(const ps2hle::MscalContext &c)
    {
        if (c.startPc != 0x30u)
            return false;
        const uint8_t *vu = c.vuData;
        const IVec4 t38 = loadI(vu, 38), t40 = loadI(vu, 40), t41 = loadI(vu, 41);
        if ((t38.v[0] & 0xFFFF) != 0x19 || (t40.v[2] & 0xFFFF) != 0xDA || (t41.v[0] & 0xFFFF) != 0x4E0 ||
            (t41.v[2] & 0xFFFF) != 0x2B6)
            return false;
        const uint32_t entry = loadI(vu, 52).v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (c.codeGeneration != checkedGeneration)
        {
            const uint64_t h = fnv(c.vuCode, 0x30, 0x2790, 0xCBF29CE484222325ull);
            checkedGeneration = c.codeGeneration;
            resident = h == 0x301ED1C437E36FF8ull || h == 0x9CF596AA7BF61264ull; // the second differs only in padding after a JR (0x1030)
        }
        return resident;
    }

    // ---- 0x02c0 entry 0x0d: two passes over a resident mesh - texture, then a shine (environment map) ----------------
    // Setup slot 35 = 0x4e0 (vi01 = P = 0x9c): the object's rotation rows normalised (ERLENG) and the 3x2 env matrix
    // rows P+2..4 . row -> P+5..7 (x / y). Then 0x4ff: qword P+1 = A+D TEX0_1 from P+2, kick P; the float transform +
    // kick 0x2b6 (tag 49); 0x509: P+1 = TEX0_2 from P+3, kick P; 0x546 (vi01 = Q = 0xa4): RGBA = Q.xyz, alpha =
    // FTOI0(clamp(Q+1.x * w + Q+1.y, Q+1.z, Q+1.w)); 0x513: kick P again, ST = ((P+5..7 . xy * 0.5) . normal + 0.5) * Q
    // over the output; kick 0x2b2 (tag 50). game-internals.md 6d.
    bool isShine(const ps2hle::MscalContext &c, uint32_t &p, uint32_t &q)
    {
        if (c.startPc != 0x2C0u)
            return false;
        const uint8_t *vu = c.vuData;
        const IVec4 t34 = loadI(vu, 34), t35 = loadI(vu, 35), t38 = loadI(vu, 38), t39 = loadI(vu, 39), t40 = loadI(vu, 40),
                    t41 = loadI(vu, 41);
        if ((t34.v[0] & 0xFFFF) != 0xC8 || (t35.v[0] & 0xFFFF) != 0x4E0 || (loadI(vu, 36).v[0] & 0xFFFF) != 0x3A ||
            (loadI(vu, 37).v[0] & 0xFFFF) != 0x3A || (t38.v[0] & 0xFFFF) != 0x0D || (t38.v[2] & 0xFFFF) != 0x4FF ||
            (t39.v[0] & 0xFFFF) != 0xDA || (t39.v[2] & 0xFFFF) != 0x2B6 || (t40.v[0] & 0xFFFF) != 0x509 ||
            (t40.v[2] & 0xFFFF) != 0x546 || (t41.v[0] & 0xFFFF) != 0x513 || (t41.v[2] & 0xFFFF) != 0x2B2)
            return false;
        p = t35.v[1] & 0x3FF;
        if ((t38.v[3] & 0x3FF) != static_cast<int32_t>(p) || (t40.v[1] & 0x3FF) != static_cast<int32_t>(p) ||
            (t41.v[1] & 0x3FF) != static_cast<int32_t>(p))
            return false;
        q = t40.v[3] & 0x3FF;
        const uint32_t entry = loadI(vu, 52).v[0] & 0xFFFF;
        if (entry != 0xE9 && entry != 0xDE)
            return false;
        static uint64_t checkedGeneration = ~0ull;
        static bool resident = false;
        if (c.codeGeneration != checkedGeneration)
        {
            checkedGeneration = c.codeGeneration;
            resident = fnv(c.vuCode, 0x30, 0x2B20, 0xCBF29CE484222325ull) == 0x1A8EDFFDDC5C7665ull; // through 0x546's end
        }
        return resident;
    }

    // 0x4e0 (after redrawSetup)
    void shineSetup(uint8_t *vu, uint32_t p)
    {
        Vec4 r[3];
        for (uint32_t i = 0; i < 3; ++i)
        {
            r[i] = loadF(vu, i);
            const float x = vcsvu::vuOperand(r[i].v[0]), y = vcsvu::vuOperand(r[i].v[1]), z = vcsvu::vuOperand(r[i].v[2]);
            const float len = std::sqrt(x * x + y * y + z * z); // ERLENG
            const float rl = len != 0.0f ? 1.0f / len : len;
            for (int k = 0; k < 3; ++k)
                r[i].v[k] = r[i].v[k] * rl;
        }
        const Vec4 m[3] = {loadF(vu, p + 2u), loadF(vu, p + 3u), loadF(vu, p + 4u)};
        for (uint32_t i = 0; i < 3; ++i)
        {
            Vec4 o = loadF(vu, p + 5u + i); // only x / y are written that matter (z / w: stale registers)
            for (int k = 0; k < 2; ++k)
            {
                float acc = m[0].v[k] * r[i].v[0];
                acc = acc + m[1].v[k] * r[i].v[1];
                o.v[k] = acc + m[2].v[k] * r[i].v[2];
            }
            storeF(vu, p + 5u + i, o, 0x3u);
        }
    }

    // 0x4ff / 0x509: qword P+1 = (src.z, src.w, reg, src.y) - an A+D write - then the packet at P
    void shineRegisterKick(uint8_t *vu, uint32_t p, uint32_t src, int32_t reg, std::vector<std::vector<uint8_t>> &kicks)
    {
        const IVec4 a = loadI(vu, src);
        const int32_t d[4] = {a.v[2], a.v[3], reg, a.v[1]};
        std::memcpy(vu + ((p + 1u) & (kQwords - 1u)) * 16u, d, 16);
        kicks.emplace_back();
        gifPacketAt(vu, p, kicks.back());
    }

    bool shineKicks(uint8_t *vu, uint32_t top, uint32_t p, uint32_t q, std::vector<std::vector<uint8_t>> &kicks)
    {
        kicks.clear();
        shineRegisterKick(vu, p, p + 2u, 6, kicks);
        if (kicks.back().empty())
            return false;
        std::vector<std::vector<uint8_t>> pass1;
        if (!floatKicks(vu, top, pass1, kRedrawVariant) || pass1.size() != 1u)
            return false; // culled / clipped / unsupported: the VU1
        kicks.push_back(std::move(pass1[0]));
        shineRegisterKick(vu, p, p + 3u, 7, kicks);
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        top &= 0x3FFu;
        const uint32_t base = top & static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + base, nrmBase = static_cast<uint32_t>(i47.v[1]) + base;
        const uint32_t outBase = static_cast<uint32_t>(i48.v[3]) + top + 1u;
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        const uint32_t n = static_cast<uint32_t>(header.v[3]) & 0x7FFFu;
        // 0x546: colours
        const IVec4 rgb = loadI(vu, q);
        const Vec4 a = loadF(vu, q + 1u);
        const float ka = 1.0f * a.v[0], kb = 1.0f * a.v[1], lo = 1.0f * a.v[2];
        for (uint32_t i = 0; i < count; ++i)
        {
            float w;
            std::memcpy(&w, vu + ((outBase + i * 3u) & (kQwords - 1u)) * 16u + 12u, 4);
            const float acc = ka * w;
            const float v = std::min(std::max(acc + kb * 1.0f, lo), a.v[3]);
            const int32_t c[4] = {rgb.v[0], rgb.v[1], rgb.v[2], ftoi(v, 1.0f)};
            std::memcpy(vu + ((outBase + i * 3u + 1u) & (kQwords - 1u)) * 16u, c, 16);
        }
        // 0x513: the packet at P again, then the environment ST (at least 5 vertices)
        kicks.emplace_back();
        gifPacketAt(vu, p, kicks.back());
        Vec4 m[3] = {loadF(vu, p + 5u), loadF(vu, p + 6u), loadF(vu, p + 7u)};
        for (auto &r : m)
        {
            r.v[0] = r.v[0] * 0.5f;
            r.v[1] = r.v[1] * 0.5f;
        }
        const float half = 0.0f + 0.5f;
        for (uint32_t i = 0; i < std::max(count, 5u); ++i)
        {
            const Vec4 nv = loadF(vu, nrmBase + i);
            uint8_t *o = vu + ((outBase + i * 3u) & (kQwords - 1u)) * 16u;
            float qz;
            std::memcpy(&qz, o + 8, 4);
            float st[2];
            for (int k = 0; k < 2; ++k)
            {
                float acc = m[0].v[k] * nv.v[0];
                acc = acc + m[1].v[k] * nv.v[1];
                const float e = acc + m[2].v[k] * nv.v[2];
                st[k] = (half + e) * qz;
            }
            std::memcpy(o, st, 8);
        }
        // kick 0x2b2: tag 50 over the output
        static thread_local uint32_t outputs[256][12];
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < 3; ++k)
                std::memcpy(outputs[i] + k * 4u, vu + ((outBase + i * 3u + k) & (kQwords - 1u)) * 16u, 16);
        kicks.emplace_back();
        kickPacket(vu, 50, static_cast<uint32_t>(header.v[3]), outputs, n, kicks.back());
        return true;
    }

    // verify: the memory the re-draw reproduces, compared after the VU's run (matrices, 33, output buffer, flags, 52.z)
    struct RedrawCheck
    {
        std::vector<uint8_t> expected, before;
        bool active = false;
        uint32_t shineP = 0; // entry 0x0d: also compare P+1 and the env matrix P+5..7 (x / y)
        uint64_t exact = 0, memBad = 0, kicksBad = 0;
    } g_redraw;

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

    bool predictLit(const ps2hle::MscalContext &c);

    // the clip test's decision for a packet (0x0c38 with matrix 24-27 when 52.x = 0x0e9): any / all of the flags
    void clipTest(const uint8_t *vu, uint32_t posBase, uint32_t count, uint32_t &any, uint32_t &all)
    {
        any = 0;
        all = 0;
        if ((loadI(vu, 52).v[0] & 0xFFFF) != 0xE9)
            return;
        Vec4 cm[4];
        foldMatrix(vu, 24, cm);
        const Vec4 guard = loadF(vu, 65);
        all = 0xFFFu;
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
    }

    uint32_t packetPosBase(const uint8_t *vu, uint32_t top)
    {
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        return static_cast<uint32_t>(i47.v[0]) + ((top & 0x3FFu) & static_cast<uint32_t>(i48.v[2]));
    }

    // table 0x13: culled / clipped decided as the VU does; fast packets: the three kicks compared with the VU's
    bool predictEnv(const ps2hle::MscalContext &c)
    {
        const uint8_t *vu = c.vuData;
        const uint32_t posBase = packetPosBase(vu, c.top);
        const uint32_t count = static_cast<uint32_t>(loadI(vu, posBase - 1u).v[2]);
        if (count == 0u || count > 256u)
            return false;
        ++g_stats.packets;
        uint32_t any, all;
        clipTest(vu, posBase, count, any, all);
        g_pred.vertsBefore = ps2hle::g_gsVerticesRecorded;
        if (all != 0u)
        {
            g_pred.decision = Decision::Culled;
            ++g_stats.culled;
            return false;
        }
        if (any & 0xFC0u)
        {
            ++g_stats.clipper; // not compared (the VU's clipper on the lit pass)
            return false;
        }
        g_pred.vuKicks.clear();
        g_pred.clipPackets.clear();
        g_pred.clipChecked = envKicks(vu, c.top, g_pred.clipPackets);
        g_pred.envKicks = true;
        if (g_pred.clipChecked)
            ps2vu1tap::g_kicks = &g_pred.vuKicks;
        return false;
    }

    bool predict(const ps2hle::MscalContext &c)
    {
        g_pred.decision = Decision::None;
        g_pred.litLanes = false;
        if (c.startPc == 0x30u && isLitPipeline(c.vuCode, c.vuData, c.codeGeneration))
            return predictLit(c);
        if (c.startPc == 0x30u && isEnvPipeline(c.vuCode, c.vuData, c.codeGeneration))
            return predictEnv(c);
        uint32_t shineP = 0, shineQ = 0;
        if (isShine(c, shineP, shineQ))
        {
            g_redraw.expected.assign(c.vuData, c.vuData + kQwords * 16u);
            g_redraw.before = g_redraw.expected;
            g_pred.vuKicks.clear();
            g_pred.clipPackets.clear();
            redrawSetup(g_redraw.expected.data(), c.top & 0x3FFu);
            shineSetup(g_redraw.expected.data(), shineP);
            g_redraw.active = shineKicks(g_redraw.expected.data(), c.top, shineP, shineQ, g_pred.clipPackets);
            g_redraw.shineP = shineP;
            if (g_redraw.active)
                ps2vu1tap::g_kicks = &g_pred.vuKicks;
            return false;
        }
        g_redraw.shineP = 0;
        if (isRedraw(c) || isPrelitFloat(c))
        {
            g_redraw.expected.assign(c.vuData, c.vuData + kQwords * 16u);
            g_redraw.before = g_redraw.expected;
            g_pred.vuKicks.clear();
            g_pred.clipPackets.clear();
            if (c.startPc == 0x2C0u)
                redrawSetup(g_redraw.expected.data(), c.top & 0x3FFu);
            g_redraw.active = floatKicks(g_redraw.expected.data(), c.top, g_pred.clipPackets, isRedraw(c) ? kRedrawVariant : kPrelitFloatVariant);
            if (g_redraw.active)
                ps2vu1tap::g_kicks = &g_pred.vuKicks;
            return false;
        }
        const bool rigid = c.startPc == 0x30u && isRigidPipeline(c.vuCode, c.vuData, c.codeGeneration);
        if (!rigid && (c.startPc != 0x30u || !isWorldPipeline(c.vuCode, c.vuData, c.codeGeneration)))
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
        const bool clipTest = (loadI(vu, 52).v[0] & 0xFFFF) == 0xE9;
        if (!clipTest)
            all = 0;
        for (uint32_t i = 0; clipTest && i < std::max(count, 6u); ++i)
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
            clipDumpBegin(c, rigid ? 1u : 0u);
            return false;
        }

        // fast path: transform (0x07f0), fog (0x1048), colour (stage 0x2700 with vi01 = 0x9c)
        fastOutputs(vu, posBase, uvBase, colBase, static_cast<uint32_t>(i47.v[1]) + (top & mask), rigid, count, g_pred.expected);
        g_pred.decision = Decision::Fast;
        g_pred.outBase = outBase;
        g_pred.count = count;
        g_pred.nloop = static_cast<uint32_t>(header.v[3]);
        ++g_stats.fast;
        return false;
    }

    bool predictLit(const ps2hle::MscalContext &c)
    {
        static LitPacket lp;
        const uint8_t *vu = c.vuData;
        if (!litPacket(c, lp))
            return false;
        ++g_stats.packets;
        skin(vu, lp);
        uint32_t any = 0, all = 0;
        if ((loadI(vu, 52).v[0] & 0xFFFF) == 0xE9)
            litClip(vu, lp, any, all);
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
            clipDumpBegin(c, 2u);
            return false;
        }
        // lit-pass transform (0x07f0: matrix 28-31 as is), fog (0x1048), lighting (0x27f8)
        Vec4 m[4] = {loadF(vu, 28), loadF(vu, 29), loadF(vu, 30), loadF(vu, 31)};
        const float inv255 = bitsToFloat(0x3B808081u);
        Vec4 uvm[3] = {loadF(vu, 44), loadF(vu, 45), loadF(vu, 46)};
        for (auto &r : uvm)
            for (int k = 0; k < 3; ++k)
                r.v[k] *= inv255;
        const bool fog = (loadI(vu, 52).v[1] & 2) != 0;
        const Vec4 fogp = loadF(vu, 53);
        const LitLights L = litLights(vu);
        // The STQ qword's w lane (ignored by the GS, read by the fog stage) does not follow one vertex in the lit
        // transform's software pipeline as the runtime's VU1 runs it (it repeats / lags); that lane and the fog are
        // not compared for lit packets (g_pred.litLanes)
        for (uint32_t i = 0; i < lp.count; ++i)
        {
            const Vec4 p = transform(m, {{lp.pos[i][0], lp.pos[i][1], lp.pos[i][2], 1.0f}});
            const float wNext = p.v[3];
            const Vec4 uv = itof(loadI(vu, lp.uvBase + i));
            float st[3];
            for (int k = 0; k < 3; ++k)
            {
                float acc = uvm[0].v[k] * uv.v[0];
                acc = acc + uvm[1].v[k] * uv.v[1];
                st[k] = acc + uvm[2].v[k] * uv.v[2];
            }
            const float q = 1.0f / p.v[3];
            uint32_t *e = g_pred.expected[i];
            float stq[4] = {st[0] * q, st[1] * q, st[2] * q, wNext};
            std::memcpy(e, stq, 16);
            int32_t rgb[3];
            litColour(L, lp.nrm[i], rgb);
            e[4] = static_cast<uint32_t>(rgb[0]);
            e[5] = static_cast<uint32_t>(rgb[1]);
            e[6] = static_cast<uint32_t>(rgb[2]);
            e[7] = static_cast<uint32_t>(L.alpha);
            for (int k = 0; k < 3; ++k)
                e[8 + k] = static_cast<uint32_t>(ftoi(p.v[k] * q, 16.0f));
            float f = 0.0f;
            if (fog)
                f = std::max(std::min(fogp.v[3] * wNext + 1.0f * fogp.v[2], 255.0f), 0.0f);
            e[11] = static_cast<uint32_t>(ftoi(f, 16.0f));
        }
        g_pred.decision = Decision::Fast;
        g_pred.litLanes = true;
        g_pred.outBase = lp.outBase;
        g_pred.count = lp.count;
        g_pred.nloop = lp.nloop;
        ++g_stats.fast;
        return false;
    }

    void redrawCompare(const ps2hle::MscalContext &c)
    {
        g_redraw.active = false;
        ps2vu1tap::g_kicks = nullptr;
        // kicks: non-empty ones exact (STQ.w not compared)
        std::vector<const std::vector<uint8_t> *> vu;
        for (const auto &k : g_pred.vuKicks)
        {
            uint32_t t0 = 0;
            std::memcpy(&t0, k.data(), 4);
            if (!(k.size() == 16u && (t0 & 0x7FFFu) == 0u))
                vu.push_back(&k);
        }
        bool kicksOk = vu.size() == g_pred.clipPackets.size();
        for (size_t i = 0; kicksOk && i < vu.size(); ++i)
        {
            const auto &a = g_pred.clipPackets[i], &b = *vu[i];
            kicksOk = a.size() == b.size() && std::memcmp(a.data(), b.data(), 16) == 0;
            for (size_t q = 16; kicksOk && q < a.size(); q += 16)
                kicksOk = std::memcmp(a.data() + q, b.data() + q, ((q - 16u) / 16u) % 3u == 0u ? 12u : 16u) == 0;
        }
        // memory: 0-3, 24-33 except 32 (the stack at 54 and the clipper's buffers 66-130 are not reproduced), the output
        // buffer and the positions' .w for the vertices drawn, 52.z
        const uint8_t *e = g_redraw.expected.data(), *g = c.vuData;
        bool memOk = std::memcmp(e, g, 4u * 16u) == 0 && std::memcmp(e + 24u * 16u, g + 24u * 16u, 8u * 16u) == 0 &&
                     std::memcmp(e + 33u * 16u, g + 33u * 16u, 12u) == 0 && std::memcmp(e + 52u * 16u + 8u, g + 52u * 16u + 8u, 4u) == 0;
        const IVec4 i47 = loadI(e, 47), i48 = loadI(e, 48);
        const uint32_t top = c.top & 0x3FFu;
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + (top & static_cast<uint32_t>(i48.v[2]));
        const uint32_t outBase = static_cast<uint32_t>(i48.v[3]) + top + 1u;
        const uint32_t count = std::min(static_cast<uint32_t>(loadI(e, posBase - 1u).v[2]), 256u);
        for (uint32_t i = 0; memOk && i < count * 3u; ++i)
        {
            const uint32_t q = (outBase + i) & (kQwords - 1u);
            memOk = std::memcmp(e + q * 16u, g + q * 16u, (i % 3u) == 0u ? 12u : 16u) == 0;
        }
        for (uint32_t i = 0; memOk && i < count; ++i)
        {
            const uint32_t q = (posBase + i) & (kQwords - 1u);
            memOk = std::memcmp(e + q * 16u + 12u, g + q * 16u + 12u, 4) == 0;
        }
        if (memOk && g_redraw.shineP)
        {
            const uint32_t sp = g_redraw.shineP;
            memOk = std::memcmp(e + (sp + 1u) * 16u, g + (sp + 1u) * 16u, 16) == 0;
            for (uint32_t i = 5; memOk && i < 8; ++i)
                memOk = std::memcmp(e + (sp + i) * 16u, g + (sp + i) * 16u, 8) == 0;
        }
        if (kicksOk && memOk)
            ++g_redraw.exact;
        else
        {
            (kicksOk ? g_redraw.memBad : g_redraw.kicksBad) += 1;
            static int dumped = 0;
            if (dumped < 3 && std::getenv("PS2X_HLE_STATS")) // redraw_fail_N: before / expected / VU memory, kicks
            {
                const auto write = [](const std::string &name, const void *data, size_t size) {
                    if (FILE *f = std::fopen(name.c_str(), "wb"))
                    {
                        std::fwrite(data, 1, size, f);
                        std::fclose(f);
                    }
                };
                const std::string base = "redraw_fail_" + std::to_string(dumped++);
                write(base + "_before.bin", g_redraw.before.data(), g_redraw.before.size());
                write(base + "_expected.bin", g_redraw.expected.data(), g_redraw.expected.size());
                write(base + "_vu.bin", c.vuData, kQwords * 16u);
                std::vector<uint8_t> k;
                for (const auto &x : g_pred.clipPackets)
                    k.insert(k.end(), x.begin(), x.end());
                write(base + "_mykicks.bin", k.data(), k.size());
                k.clear();
                for (const auto *x : vu)
                    k.insert(k.end(), x->begin(), x->end());
                write(base + "_vukicks.bin", k.data(), k.size());
                const uint32_t topWord = c.top;
                write(base + "_top.bin", &topWord, 4);
            }
            if (g_stats.logged < 12)
            {
                ++g_stats.logged;
                std::fprintf(stderr, "[hle-verify] re-draw differs: kicks %s (%zu / VU %zu), memory %s\n", kicksOk ? "ok" : "off",
                             g_pred.clipPackets.size(), vu.size(), memOk ? "ok" : "off");
            }
        }
        static auto reportAt = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - reportAt >= std::chrono::seconds(10))
        {
            reportAt = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[hle-verify] 0x02c0 re-draws: %llu exact, %llu memory off, %llu kicks off\n",
                         (unsigned long long)g_redraw.exact, (unsigned long long)g_redraw.memBad, (unsigned long long)g_redraw.kicksBad);
        }
    }

    void after(const ps2hle::MscalContext &c)
    {
        if (g_redraw.active)
        {
            redrawCompare(c);
            return;
        }
        clipDumpEnd();
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
                        if (g_pred.litLanes && l == 3 && k != 1)
                            continue;
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

    // ---- native drawing -------------------------------------------------------------------------------------------
    struct NativeStats
    {
        uint64_t packets = 0, culled = 0, clipped = 0, drawn = 0, fallback = 0, vertices = 0, clippedCpu = 0;
    } g_native;

    int g_callerRounding = FE_TONEAREST; // the rounding mode beforeNative found (GS work runs in it)

    uint64_t hashQwords(const uint8_t *vu, uint32_t base, uint32_t count, uint64_t h)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t w[2];
            std::memcpy(w, vu + ((base + i) & (kQwords - 1u)) * 16u, 16);
            h = (h ^ w[0]) * 0x100000001B3ull;
            h = (h ^ w[1]) * 0x100000001B3ull;
        }
        return h;
    }

    void putRow(float *out, const Vec4 &r)
    {
        std::memcpy(out, r.v, 16);
    }

    bool drawNative(const ps2hle::MscalContext &c)
    {
        if (c.startPc != 0x30u || !ps2hle::gs())
            return false;
        if (!isWorldPipeline(c.vuCode, c.vuData, c.codeGeneration))
        {
            // PS2X_HLE_STATS=1: which stage tables / code the handler passes over
            static const bool stats = std::getenv("PS2X_HLE_STATS") != nullptr;
            if (stats)
            {
                static std::map<std::string, uint64_t> missed;
                static auto at = std::chrono::steady_clock::now();
                const IVec4 t38 = loadI(c.vuData, 38), t39 = loadI(c.vuData, 39), t40 = loadI(c.vuData, 40), t41 = loadI(c.vuData, 41),
                            t52 = loadI(c.vuData, 52);
                char key[160];
                std::snprintf(key, sizeof(key), "entry %03x | %03x %03x %03x %03x | %03x %03x %03x %03x | %03x %03x %03x %03x | 52.x %03x code %d",
                              t38.v[0] & 0xFFFF, t39.v[0] & 0xFFFF, t39.v[1] & 0xFFFF, t39.v[2] & 0xFFFF, t39.v[3] & 0xFFFF,
                              t40.v[0] & 0xFFFF, t40.v[1] & 0xFFFF, t40.v[2] & 0xFFFF, t40.v[3] & 0xFFFF, t41.v[0] & 0xFFFF,
                              t41.v[1] & 0xFFFF, t41.v[2] & 0xFFFF, t41.v[3] & 0xFFFF, t52.v[0] & 0xFFFF,
                              worldCodeResident(c.vuCode, c.codeGeneration) ? 1 : 0);
                ++missed[key];
                if (std::chrono::steady_clock::now() - at > std::chrono::seconds(10))
                {
                    at = std::chrono::steady_clock::now();
                    for (const auto &[k, n] : missed)
                        if (n > 100)
                            std::fprintf(stderr, "[hle-stats] passed over %8llu x %s\n", (unsigned long long)n, k.c_str());
                }
            }
            return false;
        }
        uint8_t *vu = c.vuData;
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        const uint32_t top = c.top & 0x3FFu, mask = static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + (top & mask);
        const uint32_t uvBase = static_cast<uint32_t>(i47.v[2]) + (top & mask);
        const uint32_t colBase = static_cast<uint32_t>(i47.v[3]) + (top & mask);
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        const uint32_t nloop = std::min(static_cast<uint32_t>(header.v[3]), count);
        if (count == 0u || count > 256u)
            return false;
        ++g_native.packets;

        // the VU's clip test (0x0c38) decides culled / clipper / fast, exactly (see predict); entry 0x0de skips it
        Vec4 cm[4];
        foldMatrix(vu, 24, cm);
        const Vec4 guard = loadF(vu, 65);
        uint32_t any = 0, all = 0xFFFu;
        const bool clipTest = (loadI(vu, 52).v[0] & 0xFFFF) == 0xE9;
        if (!clipTest)
            all = 0;
        for (uint32_t i = 0; clipTest && i < std::max(count, 6u); ++i)
        {
            const Vec4 p = transform(cm, itof(loadI(vu, posBase + i)));
            Vec4 g;
            for (int k = 0; k < 4; ++k)
                g.v[k] = p.v[k] * guard.v[k];
            const uint32_t f = (clipFlags(g, p.v[3]) << 6) | clipFlags(p, p.v[3]);
            any |= f;
            all &= f;
        }
        if (all != 0u)
        {
            ++g_native.culled;
            return true; // the VU program ends without drawing or touching VU memory the game reads
        }
        const bool clipped = (any & 0xFC0u) != 0u;
        // Packets the VU would send through its clipper stay on the VU1 by default: GPU clipping drew some of them
        // black (the poolside ground in GON_C3's cutscene, game-internals.md 6h); PS2X_HLE_CLIPPED=1 draws them
        // natively (GPU-clipped at the clip test's near / far planes) for working on that.
        static const bool clippedNative = [] {
            const char *e = std::getenv("PS2X_HLE_CLIPPED");
            return e && e[0] == '1';
        }();
        if (clipped && !clippedNative)
        {
            // the VU's clipper in C++ (vcs_vu_clipper.cpp, checked bit-exact with PS2X_HLE_VERIFY): its GIF packets go to
            // the GS as the VU's XGKICKs would (PATH1); PS2X_HLE_CLIPPER=0 runs the VU1 instead
            static const bool cppClipper = !(std::getenv("PS2X_HLE_CLIPPER") && *std::getenv("PS2X_HLE_CLIPPER") == '0');
            static thread_local std::vector<std::vector<uint8_t>> packets;
            packets.clear();
            if (!cppClipper || !clipperCodeResident(c.vuCode, c.codeGeneration) || !vcsclip::clipWorld(vu, c.top, packets))
            {
                ++g_native.fallback;
                return false;
            }
            const int rounding = std::fegetround();
            std::fesetround(g_callerRounding); // the GS runs in the caller's mode, as for the VU's kicks
            gsSetCurrentGifPath(1u);
            for (const auto &p : packets)
                ps2hle::gs()->processGIFPacket(p.data(), static_cast<uint32_t>(p.size()));
            gsSetCurrentGifPath(3u);
            std::fesetround(rounding);
            const int32_t variant = 711; // 52.z, as the VU leaves it after its clipper
            std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
            ++g_native.clippedCpu;
            return true;
        }

        GSNativeDraw d;
        d.kind = GSNativeDraw::VcsWorld;
        d.vertexStride = 32;
        d.vertexCount = count;
        d.drawCount = nloop;
        uint64_t key = 0xCBF29CE484222325ull ^ count;
        key = hashQwords(vu, posBase, count, key);
        key = hashQwords(vu, uvBase, count, key);
        key = hashQwords(vu, colBase, count, key);
        d.meshKey = key;
        static thread_local std::vector<uint8_t> verts;
        verts.resize(size_t(count) * 32u);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Vec4 pos = itof(loadI(vu, posBase + i)), uv = itof(loadI(vu, uvBase + i));
            const IVec4 col = loadI(vu, colBase + i);
            uint8_t *v = verts.data() + size_t(i) * 32u;
            std::memcpy(v, pos.v, 12);
            std::memcpy(v + 12, uv.v, 12);
            const uint32_t rgba = (uint32_t(col.v[0]) & 0xFF) | ((uint32_t(col.v[1]) & 0xFF) << 8) | ((uint32_t(col.v[2]) & 0xFF) << 16) |
                                  ((uint32_t(col.v[3]) & 0xFF) << 24);
            std::memcpy(v + 24, &rgba, 4);
            std::memset(v + 28, 0, 4);
        }
        d.vertices = verts.data();

        // constants (render/shaders/gs_draw.hlsl, cbuffer World), folded as the VU folds them
        Vec4 m[4];
        foldMatrix(vu, 28, m);
        float inv255;
        const uint32_t inv255Bits = 0x3B808081u;
        std::memcpy(&inv255, &inv255Bits, 4);
        float *k = d.constants;
        for (int r = 0; r < 4; ++r)
            putRow(k + r * 4, m[r]);
        for (int r = 0; r < 3; ++r)
        {
            Vec4 row = loadF(vu, 44 + r);
            for (int l = 0; l < 3; ++l)
                row.v[l] *= inv255;
            row.v[3] = 0.0f;
            putRow(k + 16 + r * 4, row);
        }
        putRow(k + 28, loadF(vu, 0x9C));
        putRow(k + 32, loadF(vu, 0x9D));
        putRow(k + 36, loadF(vu, 0x9E));
        const Vec4 fog = loadF(vu, 53);
        k[40] = (loadI(vu, 52).v[1] & 2) ? 1.0f : 0.0f;
        k[41] = fog.v[2];
        k[42] = fog.v[3];
        k[43] = 0.0f;
        for (int r = 0; r < 4; ++r)
            putRow(k + 44 + r * 4, cm[r]);
        d.constantCount = 60;

        // the kick's GIFtag (49, the unclipped path's) carries PRIM
        const IVec4 tag = loadI(vu, 49);
        const uint64_t tagLo = uint64_t(uint32_t(tag.v[0])) | (uint64_t(uint32_t(tag.v[1])) << 32);
        const uint64_t prim = (tagLo >> 47) & 0x7FFu;
        if (!ps2hle::gs()->submitNative(prim, d))
        {
            ++g_native.fallback;
            return false;
        }
        // VU memory the program leaves for later ones: the kick variant (52.z: 698 fast path, 711 after clipping)
        const int32_t variant = clipped ? 711 : 698;
        std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
        (clipped ? g_native.clipped : g_native.drawn) += 1;
        g_native.vertices += nloop;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_reportAt >= std::chrono::seconds(10))
        {
            g_reportAt = now;
            std::fprintf(stderr, "[hle] world packets %llu: culled %llu, drawn %llu + %llu GPU-clipped (%llu vertices), %llu C++-clipped, "
                                 "VU fallback %llu\n",
                         (unsigned long long)g_native.packets, (unsigned long long)g_native.culled, (unsigned long long)g_native.drawn,
                         (unsigned long long)g_native.clipped, (unsigned long long)g_native.vertices,
                         (unsigned long long)g_native.clippedCpu, (unsigned long long)g_native.fallback);
        }
        return true;
    }

    NativeStats g_nativeLit, g_nativeRigid;

    bool drawNativeRigid(const ps2hle::MscalContext &c)
    {
        uint8_t *vu = c.vuData;
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48);
        const uint32_t top = c.top & 0x3FFu, base = top & static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + base, nrmBase = static_cast<uint32_t>(i47.v[1]) + base,
                       uvBase = static_cast<uint32_t>(i47.v[2]) + base;
        const IVec4 header = loadI(vu, posBase - 1u);
        const uint32_t count = static_cast<uint32_t>(header.v[2]);
        if (count == 0u || count > 256u)
            return false;
        ++g_nativeRigid.packets;
        Vec4 cm[4];
        foldMatrix(vu, 24, cm);
        uint32_t any = 0, all = 0;
        if ((loadI(vu, 52).v[0] & 0xFFFF) == 0xE9)
        {
            const Vec4 guard = loadF(vu, 65);
            all = 0xFFFu;
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
        }
        if (all != 0u)
        {
            ++g_nativeRigid.culled;
            return true;
        }
        const bool clipped = (any & 0xFC0u) != 0u;
        GSNativeDraw d;
        d.kind = GSNativeDraw::VcsLit;
        d.vertexStride = 48;
        d.vertexCount = count;
        d.drawCount = std::min(static_cast<uint32_t>(header.v[3]), count);
        uint64_t key = 0x52494749ull ^ count;
        key = hashQwords(vu, posBase, count, key);
        key = hashQwords(vu, uvBase, count, key);
        key = hashQwords(vu, nrmBase, count, key);
        d.meshKey = key;
        static thread_local std::vector<uint8_t> verts;
        verts.assign(size_t(count) * 48u, 0);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t *v = verts.data() + size_t(i) * 48u;
            std::memcpy(v, itof(loadI(vu, posBase + i)).v, 12);
            std::memcpy(v + 12, itof(loadI(vu, uvBase + i)).v, 12);
            std::memcpy(v + 24, itof(loadI(vu, nrmBase + i)).v, 12);
        }
        d.vertices = verts.data();
        float *k = d.constants;
        Vec4 m[4];
        foldMatrix(vu, 28, m);
        for (int r = 0; r < 4; ++r)
            putRow(k + r * 4, m[r]);
        const float inv255 = bitsToFloat(0x3B808081u);
        for (int r = 0; r < 3; ++r)
        {
            Vec4 row = loadF(vu, 44 + r);
            for (int l = 0; l < 3; ++l)
                row.v[l] *= inv255;
            row.v[3] = 0.0f;
            putRow(k + 16 + r * 4, row);
        }
        const Vec4 fog = loadF(vu, 53);
        k[28] = (loadI(vu, 52).v[1] & 2) ? 1.0f : 0.0f;
        k[29] = fog.v[2];
        k[30] = fog.v[3];
        for (int r = 0; r < 4; ++r)
            putRow(k + 32 + r * 4, cm[r]);
        const LitLights L = rigidLights(vu);
        for (int r = 0; r < 3; ++r)
            putRow(k + 48 + r * 4, L.dir[r]);
        for (int r = 0; r < 4; ++r)
            putRow(k + 60 + r * 4, L.col[r]);
        putRow(k + 76, L.ambient);
        putRow(k + 80, L.clamp);
        k[84] = static_cast<float>(L.alpha);
        d.constantCount = 88;
        const IVec4 tag = loadI(vu, 49);
        const uint64_t tagLo = uint64_t(uint32_t(tag.v[0])) | (uint64_t(uint32_t(tag.v[1])) << 32);
        if (!ps2hle::gs()->submitNative((tagLo >> 47) & 0x7FFu, d))
        {
            ++g_nativeRigid.fallback;
            return false;
        }
        const int32_t variant = clipped ? 711 : 698;
        std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
        (clipped ? g_nativeRigid.clipped : g_nativeRigid.drawn) += 1;
        return true;
    }

    bool drawNativeLit(const ps2hle::MscalContext &c)
    {
        static LitPacket lp;
        uint8_t *vu = c.vuData;
        if (!litPacket(c, lp))
            return false;
        ++g_nativeLit.packets;
        skin(vu, lp);
        // the skinning stage writes the skinned position (xyz) and normal (xyz) back over its inputs; a later pass
        // over the same buffer (stage table 0x13) reads them
        for (uint32_t i = 0; i < lp.count; ++i)
        {
            std::memcpy(vu + ((lp.posBase + i) & (kQwords - 1u)) * 16u, lp.pos[i], 12);
            std::memcpy(vu + ((lp.nrmBase + i) & (kQwords - 1u)) * 16u, lp.nrm[i], 12);
        }
        uint32_t any = 0, all = 0;
        if ((loadI(vu, 52).v[0] & 0xFFFF) == 0xE9)
            litClip(vu, lp, any, all);
        if (all != 0u)
        {
            ++g_nativeLit.culled;
            return true;
        }
        const bool clipped = (any & 0xFC0u) != 0u;

        GSNativeDraw d;
        d.kind = GSNativeDraw::VcsLit;
        d.transient = true;
        d.vertexStride = 48;
        d.vertexCount = lp.count;
        d.drawCount = lp.nloop;
        static thread_local std::vector<uint8_t> verts;
        verts.assign(size_t(lp.count) * 48u, 0);
        for (uint32_t i = 0; i < lp.count; ++i)
        {
            uint8_t *v = verts.data() + size_t(i) * 48u;
            const Vec4 uv = itof(loadI(vu, lp.uvBase + i));
            std::memcpy(v, lp.pos[i], 12);
            std::memcpy(v + 12, uv.v, 12);
            std::memcpy(v + 24, lp.nrm[i], 12);
        }
        d.vertices = verts.data();
        float *k = d.constants;
        for (int r = 0; r < 4; ++r)
            putRow(k + r * 4, loadF(vu, 28 + r));
        const float inv255 = bitsToFloat(0x3B808081u);
        for (int r = 0; r < 3; ++r)
        {
            Vec4 row = loadF(vu, 44 + r);
            for (int l = 0; l < 3; ++l)
                row.v[l] *= inv255;
            row.v[3] = 0.0f;
            putRow(k + 16 + r * 4, row);
        }
        const Vec4 fog = loadF(vu, 53);
        k[28] = (loadI(vu, 52).v[1] & 2) ? 1.0f : 0.0f;
        k[29] = fog.v[2];
        k[30] = fog.v[3];
        for (int r = 0; r < 4; ++r)
            putRow(k + 32 + r * 4, loadF(vu, 24 + r));
        const LitLights L = litLights(vu);
        for (int r = 0; r < 3; ++r)
            putRow(k + 48 + r * 4, L.dir[r]);
        for (int r = 0; r < 4; ++r)
            putRow(k + 60 + r * 4, L.col[r]);
        putRow(k + 76, L.ambient);
        putRow(k + 80, L.clamp);
        k[84] = static_cast<float>(L.alpha);
        d.constantCount = 88;
        const IVec4 tag = loadI(vu, 49);
        const uint64_t tagLo = uint64_t(uint32_t(tag.v[0])) | (uint64_t(uint32_t(tag.v[1])) << 32);
        if (!ps2hle::gs()->submitNative((tagLo >> 47) & 0x7FFu, d))
        {
            ++g_nativeLit.fallback;
            return false; // (the skinned write-back above is what the VU's own run does first anyway)
        }
        const int32_t variant = clipped ? 711 : 698;
        std::memcpy(vu + 52u * 16u + 8u, &variant, 4);
        (clipped ? g_nativeLit.clipped : g_nativeLit.drawn) += 1;
        g_nativeLit.vertices += lp.nloop;
        return true;
    }

    // table 0x13: pass 1 natively as a rigid lit model, the TEXFLUSH packet and the environment pass as the VU's kicks
    // (PATH1); packets that need the VU's clipper run on the VU1
    struct EnvStats
    {
        uint64_t packets = 0, culled = 0, drawn = 0, fallback = 0;
    } g_nativeEnv;

    bool drawNativeEnv(const ps2hle::MscalContext &c)
    {
        uint8_t *vu = c.vuData;
        const uint32_t posBase = packetPosBase(vu, c.top);
        const uint32_t count = static_cast<uint32_t>(loadI(vu, posBase - 1u).v[2]);
        if (count == 0u || count > 256u)
            return false;
        ++g_nativeEnv.packets;
        uint32_t any, all;
        clipTest(vu, posBase, count, any, all);
        if (all != 0u)
        {
            ++g_nativeEnv.culled;
            return true;
        }
        static thread_local std::vector<std::vector<uint8_t>> kicks;
        if ((any & 0xFC0u) != 0u || !envKicks(vu, c.top, kicks) || !drawNativeRigid(c))
        {
            ++g_nativeEnv.fallback;
            return false;
        }
        const int rounding = std::fegetround();
        std::fesetround(g_callerRounding);
        gsSetCurrentGifPath(1u);
        for (size_t i = 1; i < kicks.size(); ++i)
            ps2hle::gs()->processGIFPacket(kicks[i].data(), static_cast<uint32_t>(kicks[i].size()));
        gsSetCurrentGifPath(3u);
        std::fesetround(rounding);
        ++g_nativeEnv.drawn;
        return true;
    }

    // 0x02c0 re-draws natively: the program's VU memory writes, its GIF packets to the GS (PATH1)
    struct RedrawStats
    {
        uint64_t drawn = 0, fallback = 0;
    } g_nativeRedraw;

    bool drawNativeRedraw(const ps2hle::MscalContext &c)
    {
        static thread_local std::vector<uint8_t> saved;
        static thread_local std::vector<std::vector<uint8_t>> kicks;
        saved.assign(c.vuData, c.vuData + kQwords * 16u);
        if (c.startPc == 0x2C0u)
            redrawSetup(c.vuData, c.top & 0x3FFu);
        if (!floatKicks(c.vuData, c.top, kicks, c.startPc == 0x2C0u ? kRedrawVariant : kPrelitFloatVariant))
        {
            std::memcpy(c.vuData, saved.data(), saved.size()); // the VU1 runs on what it would have seen
            ++g_nativeRedraw.fallback;
            return false;
        }
        const int rounding = std::fegetround();
        std::fesetround(g_callerRounding);
        gsSetCurrentGifPath(1u);
        for (const auto &k : kicks)
            ps2hle::gs()->processGIFPacket(k.data(), static_cast<uint32_t>(k.size()));
        gsSetCurrentGifPath(3u);
        std::fesetround(rounding);
        ++g_nativeRedraw.drawn;
        return true;
    }

    bool drawNativeShine(const ps2hle::MscalContext &c, uint32_t p, uint32_t q)
    {
        static thread_local std::vector<uint8_t> saved;
        static thread_local std::vector<std::vector<uint8_t>> kicks;
        saved.assign(c.vuData, c.vuData + kQwords * 16u);
        redrawSetup(c.vuData, c.top & 0x3FFu);
        shineSetup(c.vuData, p);
        if (!shineKicks(c.vuData, c.top, p, q, kicks))
        {
            std::memcpy(c.vuData, saved.data(), saved.size());
            ++g_nativeRedraw.fallback;
            return false;
        }
        const int rounding = std::fegetround();
        std::fesetround(g_callerRounding);
        gsSetCurrentGifPath(1u);
        for (const auto &k : kicks)
            ps2hle::gs()->processGIFPacket(k.data(), static_cast<uint32_t>(k.size()));
        gsSetCurrentGifPath(3u);
        std::fesetround(rounding);
        ++g_nativeRedraw.drawn;
        return true;
    }

    bool beforeNative(const ps2hle::MscalContext &c)
    {
        const int rounding = std::fegetround();
        g_callerRounding = rounding;
        std::fesetround(FE_TOWARDZERO);
        static const bool lit =!(std::getenv("PS2X_HLE_LIT") && *std::getenv("PS2X_HLE_LIT") == '0');
        bool handled = false;
        static const bool env = !(std::getenv("PS2X_HLE_ENV") && *std::getenv("PS2X_HLE_ENV") == '0');
        static const bool redraw = !(std::getenv("PS2X_HLE_REDRAW") && *std::getenv("PS2X_HLE_REDRAW") == '0');
        uint32_t shineP = 0, shineQ = 0;
        if (redraw && c.startPc == 0x2C0u && ps2hle::gs() && isShine(c, shineP, shineQ))
            handled = drawNativeShine(c, shineP, shineQ);
        else if (redraw && c.startPc == 0x2C0u && ps2hle::gs() && isRedraw(c))
            handled = drawNativeRedraw(c);
        else if (redraw && c.startPc == 0x30u && ps2hle::gs() && isPrelitFloat(c))
            handled = drawNativeRedraw(c); // the same float path, without the setup
        else if (lit && c.startPc == 0x30u && ps2hle::gs() && isLitPipeline(c.vuCode, c.vuData, c.codeGeneration))
            handled = drawNativeLit(c);
        else if (lit && env && c.startPc == 0x30u && ps2hle::gs() && isEnvPipeline(c.vuCode, c.vuData, c.codeGeneration))
            handled = drawNativeEnv(c);
        else if (lit && c.startPc == 0x30u && ps2hle::gs() && isRigidPipeline(c.vuCode, c.vuData, c.codeGeneration))
            handled = drawNativeRigid(c);
        else
            handled = drawNative(c);
        std::fesetround(rounding);
        // PS2X_HLE_STATS=1: which stage tables (38-41, entry 52.x) of 0x0030 still run on the VU1, every 10 s
        static const bool stats = std::getenv("PS2X_HLE_STATS") != nullptr;
        if (stats && !handled && (c.startPc == 0x30u || c.startPc == 0x2C0u))
        {
            static std::map<std::string, uint64_t> unhandled;
            static auto reportAt = std::chrono::steady_clock::now();
            char key[256];
            const IVec4 t38 = loadI(c.vuData, 38), t39 = loadI(c.vuData, 39), t40 = loadI(c.vuData, 40), t41 = loadI(c.vuData, 41);
            int len = 0;
            if (c.startPc == 0x2C0u) // setup program: its own table at 34-37 first
            {
                len = std::snprintf(key, sizeof(key), "pc 2c0 setup");
                for (uint32_t q = 34; q <= 37; ++q)
                {
                    const IVec4 t = loadI(c.vuData, q);
                    len += std::snprintf(key + len, sizeof(key) - len, " %x %x %x %x |", t.v[0] & 0xFFFF, t.v[1] & 0xFFFF,
                                         t.v[2] & 0xFFFF, t.v[3] & 0xFFFF);
                }
            }
            std::snprintf(key + len, sizeof(key) - len, "entry %x | %x %x %x %x | %x %x %x %x | %x %x %x %x | 52.x %x", t38.v[0] & 0xFFFF,
                          t39.v[0] & 0xFFFF, t39.v[1] & 0xFFFF, t39.v[2] & 0xFFFF, t39.v[3] & 0xFFFF, t40.v[0] & 0xFFFF,
                          t40.v[1] & 0xFFFF, t40.v[2] & 0xFFFF, t40.v[3] & 0xFFFF, t41.v[0] & 0xFFFF, t41.v[1] & 0xFFFF,
                          t41.v[2] & 0xFFFF, t41.v[3] & 0xFFFF, loadI(c.vuData, 52).v[0] & 0xFFFF);
            if (++unhandled[key] == 200u) // a sample of each (after the start-up frames): stage_<entry>_<n>_code / _data.bin
            {
                static int samples = 0;
                char name[64];
                std::snprintf(name, sizeof(name), "stage_%03x_%02x_%d_code.bin", c.startPc, t38.v[0] & 0xFFFF, samples);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(c.vuCode, 1, 16384, f);
                    std::fclose(f);
                }
                std::snprintf(name, sizeof(name), "stage_%03x_%02x_%d_data.bin", c.startPc, t38.v[0] & 0xFFFF, samples++);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(c.vuData, 1, 16384, f);
                    std::fclose(f);
                }
            }
            if (std::chrono::steady_clock::now() - reportAt >= std::chrono::seconds(10))
            {
                reportAt = std::chrono::steady_clock::now();
                for (const auto &[k, n] : unhandled)
                    std::fprintf(stderr, "[hle] on the VU1: %8llu x %s\n", (unsigned long long)n, k.c_str());
            }
        }
        return handled;
    }

    struct Registrar
    {
        Registrar()
        {
            // native drawing is the default; PS2X_HLE=0 keeps the VU1 path, PS2X_HLE_VERIFY=1 checks it instead
            const char *hle = std::getenv("PS2X_HLE");
            if (std::getenv("PS2X_HLE_VERIFY"))
                ps2hle::setMscalHooks(&before, &after);
            else if (!(hle && *hle == '0'))
                ps2hle::setMscalHooks(&beforeNative, nullptr);
        }
    } registrar;
}
