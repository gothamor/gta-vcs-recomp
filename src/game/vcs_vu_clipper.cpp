// VCS world pipeline, clipper path (VU1 program 0x0030: clip test 0x0c38, fog 0x11f8, 0x13d8, prelit colour 0x2700,
// kick 0x15b0 -> 0x1638 per-triangle clipper with 0x1b10 / 0x1f68 and the emitters 0x23c0 / 0x2520 / 0x2608),
// reproduced from the disassembly (build/analysis/vu1_code_full.txt; docs/game-internals.md 6h). Checked against the
// VU1's XGKICKs with PS2X_HLE_VERIFY=1.
#include "vcs_vu_clipper.h"
#include "vcs_vu_math.h"

#include <algorithm>
#include <cstring>

namespace vcsclip
{
    namespace
    {
        using namespace vcsvu;

        // a vertex in the clipper's buffers (66+1 / 111+1, four qwords): ST + clip w, colour (float), clip position +
        // fog, clip flags (plain in bits 0-5, guard band in 6-11)
        struct ClipVertex
        {
            Vec4 st, col, pos;
            uint32_t flags;
        };

        uint32_t flagsOf(const Vec4 &pos, const Vec4 &guard, float w)
        {
            Vec4 g;
            for (int k = 0; k < 4; ++k)
                g.v[k] = pos.v[k] * guard.v[k];
            return (clipFlags(g, w) << 6) | clipFlags(pos, w);
        }

        Vec4 lerp(const Vec4 &a, float fa, const Vec4 &b, float fb) // ACC = a * fa; ACC + b * fb
        {
            Vec4 r;
            for (int k = 0; k < 4; ++k)
            {
                const float acc = a.v[k] * fa;
                r.v[k] = acc + b.v[k] * fb;
            }
            return r;
        }

        // 0x1b10 / 0x1f68: Sutherland-Hodgman against one plane (a x + b y + c w + d, outside = the plane's flag bit),
        // in[0..n] with in[n] = in[0]. Returns the output count; andFlags / orFlags over the output vertices.
        uint32_t clipPlane(const ClipVertex *in, uint32_t n, ClipVertex *out, const Vec4 &plane, uint32_t bit, const Vec4 &guard,
                           uint32_t &andFlags, uint32_t &orFlags)
        {
            const auto dist = [&](const ClipVertex &v) {
                float acc = plane.v[0] * v.pos.v[0];
                acc = acc + plane.v[1] * v.pos.v[1];
                acc = acc + plane.v[2] * v.st.v[3];
                return acc + plane.v[3] * 1.0f;
            };
            uint32_t count = 0;
            andFlags = 4095u;
            orFlags = 0u;
            const auto push = [&](const ClipVertex &v) {
                out[count++] = v;
                andFlags &= v.flags;
                orFlags |= v.flags;
            };
            const auto intersect = [&](const ClipVertex &a, const ClipVertex &b, float q) { // a * (1 - q) + b * q
                const float fa = -q + 1.0f;
                ClipVertex v;
                v.st = lerp(a.st, fa, b.st, q);
                v.col = lerp(a.col, fa, b.col, q);
                v.pos = lerp(a.pos, fa, b.pos, q);
                v.flags = flagsOf(v.pos, guard, v.st.v[3]);
                return v;
            };
            const ClipVertex *prev = &in[0];
            float dPrev = dist(*prev);
            for (uint32_t j = 1; j <= n; ++j)
            {
                const ClipVertex &cur = in[j];
                const float dCur = dist(cur);
                const bool prevIn = (prev->flags & bit) == 0u, curIn = (cur.flags & bit) == 0u;
                if (prevIn)
                {
                    push(*prev);
                    if (!curIn)
                        push(intersect(*prev, cur, dPrev / (dPrev - dCur)));
                }
                else if (curIn)
                    push(intersect(cur, *prev, dCur / (dCur - dPrev)));
                prev = &cur;
                dPrev = dCur;
            }
            return count;
        }

        // 0x23c0 / 0x2520 / 0x2608: tag (fan PRIM) + per vertex STQ, RGBA, XYZF2
        struct Emitter
        {
            Vec4 m[4];      // viewport matrix 12-15
            uint32_t tag[4]; // 49 with the upper lane in y
            float bias;      // I = 0.001 for FTOI0 of interpolated colours

            void begin(std::vector<uint8_t> &p, uint32_t n) const
            {
                p.resize(16u + n * 48u);
                uint32_t t[4] = {0x8000u | n, tag[1], tag[2], tag[3]};
                std::memcpy(p.data(), t, 16);
            }

            // position / STQ of a vertex: g = M * (pos.xyz, st.w); XYZF = FTOI4(g.xyz / w, fog); STQ = st.xyz / w
            void put(uint8_t *q, const Vec4 &st, const Vec4 &pos, const int32_t rgba[4]) const
            {
                const float qv = 1.0f / st.v[3];
                const Vec4 g = transform(m, pos, st.v[3]);
                const float stq[4] = {st.v[0] * qv, st.v[1] * qv, st.v[2] * qv, st.v[3]};
                const int32_t xyzf[4] = {ftoi(g.v[0] * qv, 16.0f), ftoi(g.v[1] * qv, 16.0f), ftoi(g.v[2] * qv, 16.0f),
                                         ftoi(pos.v[3], 16.0f)};
                std::memcpy(q, stq, 16);
                std::memcpy(q + 16, rgba, 16);
                std::memcpy(q + 32, xyzf, 16);
            }

            void polygon(std::vector<std::vector<uint8_t>> &packets, const ClipVertex *v, uint32_t n, bool divideColour) const
            {
                std::vector<uint8_t> &p = packets.emplace_back();
                begin(p, n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    Vec4 c = v[i].col;
                    if (divideColour) // 0x2608: colours were multiplied by w before clipping
                    {
                        const float qv = 1.0f / v[i].st.v[3];
                        for (float &x : c.v)
                            x = x * qv;
                    }
                    int32_t rgba[4];
                    for (int k = 0; k < 4; ++k)
                        rgba[k] = ftoi(c.v[k] + bias, 1.0f);
                    put(p.data() + 16u + i * 48u, v[i].st, v[i].pos, rgba);
                }
            }
        };
    }

    bool clipWorld(const uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets, bool defaultColour)
    {
        const IVec4 i47 = loadI(vu, 47), i48 = loadI(vu, 48), i52 = loadI(vu, 52);
        if (i52.v[1] & 1)
            return false; // backface pass (0x1440) not reproduced
        top &= 0x3FFu;
        const uint32_t mask = static_cast<uint32_t>(i48.v[2]);
        const uint32_t posBase = static_cast<uint32_t>(i47.v[0]) + (top & mask);
        const uint32_t uvBase = static_cast<uint32_t>(i47.v[2]) + (top & mask);
        const uint32_t colBase = static_cast<uint32_t>(i47.v[3]) + (top & mask);
        const uint32_t count = static_cast<uint32_t>(loadI(vu, posBase - 1u).v[2]);
        if (count < 3u || count > 256u)
            return false;

        // clip test 0x0c38: clip-space position, ST (not divided), flags; fog 0x11f8; colour 0x2700
        Vec4 cm[4];
        foldMatrix(vu, 24, cm);
        const Vec4 guard = loadF(vu, 65);
        const float inv255 = bitsToFloat(0x3B808081u);
        Vec4 uvm[3] = {loadF(vu, 44), loadF(vu, 45), loadF(vu, 46)};
        for (auto &r : uvm)
            for (int k = 0; k < 4; ++k)
                r.v[k] = r.v[k] * inv255;
        const bool fog = (i52.v[1] & 2) != 0;
        const Vec4 fogp = loadF(vu, 53), cs = loadF(vu, 0x9C), co = loadF(vu, 0x9D), cmax = loadF(vu, 0x9E);
        static thread_local std::vector<ClipVertex> in;
        static thread_local std::vector<IVec4> rgbaIn;
        in.resize(count);
        rgbaIn.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            ClipVertex &v = in[i];
            const Vec4 p = transform(cm, itof(loadI(vu, posBase + i)));
            const Vec4 uv = itof(loadI(vu, uvBase + i));
            for (int k = 0; k < 3; ++k)
            {
                float acc = uvm[0].v[k] * uv.v[0];
                acc = acc + uvm[1].v[k] * uv.v[1];
                v.st.v[k] = acc + uvm[2].v[k] * uv.v[2];
            }
            v.st.v[3] = p.v[3];
            v.pos = p;
            v.flags = flagsOf(p, guard, p.v[3]);
            if (fog)
            {
                const float acc = fogp.v[3] * p.v[3];
                v.pos.v[3] = std::max(std::min(acc + 1.0f * fogp.v[2], 255.0f), 0.0f);
            }
            if (defaultColour)
                rgbaIn[i] = loadI(vu, 51);
            else
            {
                const Vec4 col = itof(loadI(vu, colBase + i));
                for (int k = 0; k < 4; ++k)
                {
                    const float r = co.v[k] + cs.v[k] * col.v[k];
                    rgbaIn[i].v[k] = ftoi(std::min(r, cmax.v[k]), 1.0f);
                }
            }
            v.col = itof(rgbaIn[i]);
        }

        // kick 0x1638: per triangle of the strip
        Emitter e;
        for (int r = 0; r < 4; ++r)
            e.m[r] = loadF(vu, 12 + r);
        const IVec4 t49 = loadI(vu, 49);
        e.tag[0] = static_cast<uint32_t>(t49.v[0]);
        e.tag[1] = static_cast<uint32_t>(t49.v[3]);
        e.tag[2] = static_cast<uint32_t>(t49.v[2]);
        e.tag[3] = static_cast<uint32_t>(t49.v[3]);
        e.bias = bitsToFloat(0x3A83126Fu);
        static const struct
        {
            uint32_t qword, bit, remaining;
        } kPlanes[] = {{64, 32, 1984}, {63, 16, 960}, {62, 8, 448}, {61, 4, 192}, {60, 2, 64}, {59, 1, 0}};
        ClipVertex bufA[16], bufB[16];
        for (uint32_t t = 0; t + 2u < count; ++t)
        {
            const ClipVertex *v = &in[t];
            const uint32_t f0 = v[0].flags, f1 = v[1].flags, f2 = v[2].flags;
            if (f0 & f1 & f2 & 63u)
                continue;
            if (((f0 | f1 | f2) & 0xFC0u) == 0u)
            {
                std::vector<uint8_t> &p = packets.emplace_back(); // 0x23c0: inside the guard band, colours as computed
                e.begin(p, 3);
                for (uint32_t i = 0; i < 3; ++i)
                    e.put(p.data() + 16u + i * 48u, v[i].st, v[i].pos, rgbaIn[t + i].v);
                continue;
            }
            ClipVertex *cur = bufA, *next = bufB;
            cur[0] = v[0];
            cur[1] = v[1];
            cur[2] = v[2];
            cur[3] = v[0];
            uint32_t n = 3, orFlags = f0 | f1 | f2, andFlags = 0;
            bool done = false;
            for (uint32_t p = 0; p < 6u && !done; ++p)
            {
                if (p == 1u) // 0x1918: after the near plane, colours times w (divided again when emitted, 0x2608)
                    for (uint32_t i = 0; i <= n; ++i)
                        for (float &x : cur[i].col.v)
                            x = x * cur[i].st.v[3];
                if ((orFlags & kPlanes[p].bit) == 0u)
                    continue;
                n = clipPlane(cur, n, next, loadF(vu, kPlanes[p].qword), kPlanes[p].bit, guard, andFlags, orFlags);
                std::swap(cur, next);
                if (andFlags != 0u)
                    done = true; // rejected
                else if ((orFlags & kPlanes[p].remaining) == 0u)
                {
                    e.polygon(packets, cur, n, p != 0u);
                    done = true;
                }
                else
                    cur[n] = cur[0];
            }
        }
        return true;
    }
}
