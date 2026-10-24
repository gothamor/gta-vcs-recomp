// The RSL shadow draw list - see debug/rsl_shadow.h.
#include "debug/rsl_shadow.h"

#include "ps2_runtime.h"
#include "runtime/ps2_chain_observer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace vcs::rslshadow
{
    namespace
    {
        constexpr uint32_t kCursor = 0x520FA8u; // the packet buffer's DMA cursor (game-internals.md 6i)
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPktLo = 0x520F90u, kPktHi = 0x6EDC70u; // both halves of the packet buffer
        constexpr uint32_t kCellsLo = 0xCB7A80u, kCellsHi = 0x112A660u, kModelsLo = 0x1200000u, kModelsHi = 0x1800000u;

        enum Kind : uint8_t
        {
            Boundary,
            Model,     // FUN_002F7D58 / FUN_002F80A0 (a0 = packet): CALL a0
            World,     // FUN_002F7EB0 (a0 = geometry, a1 = qwc): REF a0, qwc
            Immediate, // FUN_002F8250: its own REF / UNPACK / MSCAL (counted, not checked)
            KindCount,
        };
        constexpr const char *kKindNames[KindCount] = {"boundary", "model", "world", "immediate"};

        struct Fn
        {
            uint32_t address;
            Kind kind;
        };
        // the draws, then every other function that writes the buffer (the scan of game-internals.md 6i) except the
        // state flush FUN_002F8710, which belongs to the draw that calls it
        constexpr Fn kFns[] = {
            {0x002F7D58, Model},    {0x002F80A0, Model},    {0x002F7EB0, World},    {0x002F8250, Immediate},
            {0x002409C8, Boundary}, {0x00240ED8, Boundary}, {0x001CEF18, Boundary}, {0x00266868, Boundary},
            {0x002A92C0, Boundary}, {0x002ABF68, Boundary}, {0x002F0360, Boundary}, {0x002F39D8, Boundary},
            {0x002F3DD0, Boundary}, {0x002F53D8, Boundary}, {0x002F5520, Boundary}, {0x002F62B8, Boundary},
            {0x002F6A78, Boundary}, {0x002F7090, Boundary}, {0x002F7908, Boundary}, {0x002F7B30, Boundary},
            {0x002F9388, Boundary}, {0x002F9AA0, Boundary}, {0x002F9CE8, Boundary}, {0x002F9FA0, Boundary},
            {0x002FB690, Boundary}, {0x002FB3C0, Boundary}, {0x0032CD60, Boundary}, {0x0032D480, Boundary},
            {0x0032D7C8, Boundary}, {0x0032D880, Boundary}, {0x0040C430, Boundary}, {0x0040F6C8, Boundary},
        };
        constexpr int kCount = static_cast<int>(sizeof(kFns) / sizeof(kFns[0]));

        PS2Runtime::RecompiledFunction g_originals[kCount] = {};

        struct Record
        {
            int fn;          // index into kFns
            uint32_t a0, a1; // arguments
            uint32_t start;  // cursor at entry
            bool matched = false;
        };
        std::vector<Record> g_current, g_kicked; // this frame's / the frame whose chain is being copied
        uint32_t g_kickedEnd = 0;                // the main stream's end (the cursor at the kick)
        std::vector<size_t> g_draws;             // indices of g_kicked's draws, by start

        // per report window
        struct Stats
        {
            uint64_t frames = 0;
            uint64_t draws[KindCount] = {}, matched[KindCount] = {};
            uint64_t tags3d = 0, own3d = 0, state3d = 0, unexplained3d = 0; // 3D tags in the packet buffer: the draws'
                                                                               // own, their state, not from a draw
            std::map<std::pair<int, uint32_t>, uint64_t> tagsByFn; // (fn or -1, tag id) -> tags in the packet buffer
            std::map<int, uint64_t> unexplainedByFn;
            uint64_t topTags[8] = {}, top3d = 0; // tags in blocks allocated from the top of the half
        } g_stats;
        auto g_lastReport = std::chrono::steady_clock::now();

        uint32_t cursor(const uint8_t *rdram)
        {
            uint32_t c;
            std::memcpy(&c, rdram + (kCursor & kRamMask), 4);
            return c & kRamMask;
        }

        template <int I>
        void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
            g_current.push_back({I, getRegU32(ctx, 4), getRegU32(ctx, 5), cursor(rdram)});
            g_originals[I](rdram, ctx, runtime);
        }

        template <int... Is>
        PS2Runtime::RecompiledFunction hookFor(int i, std::integer_sequence<int, Is...>)
        {
            static const PS2Runtime::RecompiledFunction table[] = {&hook<Is>...};
            return table[i];
        }

        bool in(uint32_t a, uint32_t lo, uint32_t hi)
        {
            a &= kRamMask;
            return a >= lo && a < hi;
        }

        // the record (draw or boundary) a packet-buffer address was written by: the last one entered before it
        int recordAt(uint32_t tagAddr)
        {
            const auto it = std::upper_bound(g_kicked.begin(), g_kicked.end(), tagAddr,
                                             [](uint32_t a, const Record &r) { return a < r.start; });
            return it == g_kicked.begin() ? -1 : static_cast<int>(it - g_kicked.begin()) - 1;
        }

        // VIFcode DIRECT / DIRECTHL (0x50 / 0x51): the payload goes to the GS (textures, 2D), not to the VU1
        bool direct(uint64_t vifCodes)
        {
            for (int half = 0; half < 2; ++half)
            {
                const uint32_t cmd = static_cast<uint32_t>(vifCodes >> (half * 32 + 24)) & 0x7Fu;
                if (cmd == 0x50u || cmd == 0x51u)
                    return true;
            }
            return false;
        }

        void observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes, bool chainStart)
        {
            (void)chainStart;
            if (g_kicked.empty() || !in(tagAddr, kPktLo, kPktHi))
                return;
            tagAddr &= kRamMask;
            if (tagAddr >= g_kickedEnd && tagAddr < g_kickedEnd + 0xE6670u)
            {
                // a block allocated from the top of the half (the +0x1C pointer), CALLed from the main stream
                ++g_stats.topTags[id & 7u];
                const bool call3d = id == 5u && in(tagTarget, kModelsLo, kModelsHi);
                const bool ref3d = id == 3u && qwc && !direct(vifCodes) && (in(dataAddr, kCellsLo, kCellsHi) || in(dataAddr, kModelsLo, kModelsHi));
                g_stats.top3d += (call3d || ref3d) ? 1u : 0u;
                return;
            }
            const int r = recordAt(tagAddr);
            ++g_stats.tagsByFn[{r >= 0 ? g_kicked[r].fn : -1, id}];
            // 3D: a packet CALLed in the model heap, or VU1 data REFed from the cells / the model heap
            const bool call3d = id == 5u && in(tagTarget, kModelsLo, kModelsHi);
            const bool ref3d = id == 3u && qwc && !direct(vifCodes) && (in(dataAddr, kCellsLo, kCellsHi) || in(dataAddr, kModelsLo, kModelsHi));
            // the draw whose range [start, next draw's start) holds the tag
            const auto it = std::upper_bound(g_draws.begin(), g_draws.end(), tagAddr,
                                             [](uint32_t a, size_t i) { return a < g_kicked[i].start; });
            // the draw's own tag anywhere in its range (a setter the state flush calls may have been entered before it);
            // other tags: the draw's state when the draw itself is the record they fall in, else the function's
            const bool inDraw = r >= 0 && kFns[g_kicked[r].fn].kind != Boundary;
            bool own = false;
            if (it != g_draws.begin())
            {
                Record &d = g_kicked[*(it - 1)];
                const Kind k = kFns[d.fn].kind;
                if (!d.matched && k == Model && id == 5u && (tagTarget & kRamMask) == (d.a0 & kRamMask))
                    d.matched = own = true;
                else if (!d.matched && k == World && id == 3u && (dataAddr & kRamMask) == (d.a0 & kRamMask) &&
                         qwc == (d.a1 & 0xFFFFu))
                    d.matched = own = true;
            }
            if (call3d || ref3d)
            {
                ++g_stats.tags3d;
                if (own)
                    ++g_stats.own3d;
                else if (inDraw)
                    ++g_stats.state3d; // the draw's state flush (FUN_002F8710: the object's packet, plug-ins) or immediate data
                else
                {
                    ++g_stats.unexplained3d;
                    ++g_stats.unexplainedByFn[r >= 0 ? g_kicked[r].fn : -1];
                    static int logged = 0;
                    if (logged++ < 12)
                        std::fprintf(stderr, "[rsl-shadow] not from a draw: tag@%08x id %u target %08x data %08x qwc %u vif %016llx; in %06x "
                                             "(a0 %08x, entered at %08x, +%u), main stream ends %08x\n",
                                     tagAddr, id, tagTarget, dataAddr, qwc, static_cast<unsigned long long>(vifCodes),
                                     r >= 0 ? kFns[g_kicked[r].fn].address : 0u, r >= 0 ? g_kicked[r].a0 : 0u,
                                     r >= 0 ? g_kicked[r].start : 0u, r >= 0 ? tagAddr - g_kicked[r].start : 0u, g_kickedEnd);
                }
            }
        }

        // the frame whose chain was copied since the last kick
        void evaluate()
        {
            if (g_kicked.empty())
                return;
            ++g_stats.frames;
            for (const Record &d : g_kicked)
            {
                const Kind k = kFns[d.fn].kind;
                if (k == Boundary)
                    continue;
                ++g_stats.draws[k];
                g_stats.matched[k] += (d.matched || k == Immediate) ? 1u : 0u;
            }
        }

        const char *fnName(int fn)
        {
            static char buf[16];
            if (fn < 0)
                return "(none)";
            std::snprintf(buf, sizeof(buf), "%06x", kFns[fn].address);
            return buf;
        }

        void report()
        {
            Stats &s = g_stats;
            const double n = std::max<double>(1.0, static_cast<double>(s.frames));
            std::fprintf(stderr, "[rsl-shadow] %llu frames, per frame:", static_cast<unsigned long long>(s.frames));
            for (int k = Model; k < KindCount; ++k)
                std::fprintf(stderr, " %s %.1f (%.1f matched)", kKindNames[k], s.draws[k] / n, s.matched[k] / n);
            std::fprintf(stderr, "; 3D tags in the packet buffer %.1f: the draws' own %.1f, their state %.1f, not from a draw %.2f\n",
                         s.tags3d / n, s.own3d / n, s.state3d / n, s.unexplained3d / n);
            std::fprintf(stderr, "[rsl-shadow]   top blocks: cnt %.1f ref %.1f call %.1f ret %.1f tags, 3D %.1f\n", s.topTags[1] / n,
                         s.topTags[3] / n, s.topTags[5] / n, s.topTags[6] / n, s.top3d / n);
            for (const auto &[fn, count] : s.unexplainedByFn)
                std::fprintf(stderr, "[rsl-shadow]   3D tag not from a draw, written in %s: %.2f per frame\n", fnName(fn), count / n);
            static const char *const kIds[] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
            std::map<int, std::string> byFn;
            for (const auto &[key, count] : s.tagsByFn)
            {
                char t[48];
                std::snprintf(t, sizeof(t), " %s %.1f", kIds[key.second & 7u], count / n);
                byFn[key.first] += t;
            }
            for (const auto &[fn, text] : byFn)
                std::fprintf(stderr, "[rsl-shadow]   tags written in %s:%s\n", fnName(fn), text.c_str());
            s = Stats{};
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!std::getenv("VCS_RSL_SHADOW"))
            return;
        for (int i = 0; i < kCount; ++i)
        {
            g_originals[i] = runtime.lookupFunction(kFns[i].address);
            if (g_originals[i])
                runtime.replaceFunction(kFns[i].address, hookFor(i, std::make_integer_sequence<int, kCount>{}));
        }
        ps2chain::setTagObserver(&observe);
    }

    void onFrameKick(const uint8_t *rdram)
    {
        if (!g_originals[0])
            return; // not installed
        evaluate();
        g_kickedEnd = cursor(rdram);
        // the frame's records in buffer order (entries are in call order; the cursor only grows within a frame)
        g_kicked.swap(g_current);
        g_current.clear();
        std::stable_sort(g_kicked.begin(), g_kicked.end(), [](const Record &a, const Record &b) { return a.start < b.start; });
        g_draws.clear();
        for (size_t i = 0; i < g_kicked.size(); ++i)
            if (kFns[g_kicked[i].fn].kind != Boundary)
                g_draws.push_back(i);
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastReport >= std::chrono::seconds(5))
        {
            g_lastReport = now;
            report();
        }
    }
}
