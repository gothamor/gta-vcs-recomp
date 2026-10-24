// Native RSL packet emitters - see game/vcs_rsl_native.h.
#include "game/vcs_rsl_native.h"

#include "ps2_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs::rslnative
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kFlush = 0x002F8710u;
        constexpr uint32_t kPacketBuffer = 0x520F90u; // +0x14 half, +0x18 cursor, +0x1C down pointer (game-internals.md 6i)
        constexpr uint32_t kHalfBytes = 0xE6670u;
        constexpr uint32_t kDirty = 0x4882A0u;      // dirty bits
        constexpr uint32_t kGsQueue = 0x729510u;    // queued GS registers: {value u64, register u64}
        constexpr uint32_t kGsQueueCount = 0x488308u;

        bool g_native = false, g_verify = false, g_log = false;
        PS2Runtime::RecompiledFunction g_original = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }
        uint64_t rd64(const uint8_t *rdram, uint32_t addr)
        {
            uint64_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 8);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }
        void wr64(uint8_t *rdram, uint32_t addr, uint64_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 8); }

        // what one flush writes: packet bytes at the cursor, GS registers queued, the dirty word after
        struct Output
        {
            uint8_t bytes[1024];
            uint32_t size = 0;
            uint64_t queue[4][2];
            uint32_t queued = 0;
            uint32_t dirtyAfter = 0;

            void put64(uint64_t v)
            {
                std::memcpy(bytes + size, &v, 8);
                size += 8;
            }
            void put32x4(uint32_t a)
            {
                const uint32_t w[4] = {a, 0u, 0u, 0u};
                std::memcpy(bytes + size, w, 16);
                size += 16;
            }
            void copy(const uint8_t *rdram, uint32_t addr, uint32_t n)
            {
                std::memcpy(bytes + size, rdram + (addr & kRamMask), n);
                size += n;
            }
        };

        uint64_t g_fallbackCfae0 = 0; // flushes the original ran for bit 0x4 (the rest: plug-in callbacks)

        // FUN_002F8710 without its callback branches (game-internals.md 6i); false: the original must run
        bool build(const uint8_t *rdram, Output &out)
        {
            const uint32_t dirty = rd32(rdram, kDirty);
            if (dirty & 0x4u)
            {
                ++g_fallbackCfae0;
                return false; // FUN_001CFAE0
            }
            const uint32_t context = rd32(rdram, 0x4882A4u);
            if (dirty & 0x11u)
            {
                // the render context's plug-in list (+0x114, ends at +0xB0, next at +0x64): any callback the flush would
                // call (+0x48 and +0x4C on a context change, +0x50 on bit 0x10) means the original runs
                const uint32_t end = context + 0xB0u;
                uint32_t node = rd32(rdram, context + 0x114u);
                for (int n = 0; node != end; ++n, node = rd32(rdram, node + 0x64u))
                {
                    if (n >= 64 || (node & 0xF0000000u) != 0u)
                        return false;
                    if ((dirty & 0x1u) ? (rd32(rdram, node + 0x48u) | rd32(rdram, node + 0x4Cu)) != 0u : rd32(rdram, node + 0x50u) != 0u)
                        return false;
                }
            }
            if (dirty & 0x1u) // CALL the render context's own packet
            {
                out.put64((uint64_t(context + 0x10u) << 32) | 0x50000000u);
                out.put64(0x1000000000000000ull);
            }
            if (dirty & 0x2u) // CALL the render context's packet
            {
                out.put64((uint64_t(rd32(rdram, 0x4882A8u)) << 32) | 0x50000000u);
                out.put64(0x1000000000000000ull);
            }
            if ((dirty & 0x11u) && (dirty & 0x600u) != 0x600u) // MSCALs the context change needs (0x75 / 0x4A) unless done below
            {
                out.put64(0x10000000u);
                out.put64((uint64_t((dirty & 0x200u) ? 0u : 0x1400004Au) << 32) | ((dirty & 0x400u) ? 0u : 0x14000075u));
            }
            if (dirty & 0x2000u) // three CALLs from the current table entry
            {
                const uint32_t entry = rd32(rdram, 0x742AB0u + rd32(rdram, 0x4CD57Cu) * 4u);
                for (uint32_t k = 0; k < 3u; ++k)
                {
                    out.put64((uint64_t(rd32(rdram, rd32(rdram, entry + k * 4u))) << 32) | 0x50000000u);
                    out.put64(k == 0u ? 0x10000000u : 0u);
                }
            }
            if (dirty & 0x8u) // UNPACK V4-32 x5 -> 0x31 (0x742AE0)
            {
                out.put64(0x10000005u);
                out.put64(0x6C05003110000000ull);
                out.copy(rdram, 0x742AE0u, 0x50u);
            }
            if (dirty & 0x1000u) // viewport -> 0x0C, 0x520AC0 -> 0x41
            {
                out.put64(0x10000000u);
                out.put64(0x10000000u);
                out.put64(0x10000005u);
                out.put64(0x6C04000C01000101ull);
                out.copy(rdram, 0x520920u, 0x40u);
                out.put32x4(0x1400008Fu);
                out.put64(0x10000001u);
                out.put64(0x6C01004110000000ull);
                out.copy(rdram, 0x520AC0u, 0x10u);
            }
            if (dirty & 0x800u) // 0x5208E0 -> 0x08, lights 0x5209A0 -> 0x3B
            {
                out.put64(0x10000000u);
                out.put64(0x10000000u);
                out.put64(0x10000005u);
                out.put64(0x6C04000801000101ull);
                out.copy(rdram, 0x5208E0u, 0x40u);
                out.put32x4(0x1400008Fu);
                out.put64(0x10000006u);
                out.put64(0x6C06003B00000000ull);
                out.copy(rdram, 0x5209A0u, 0x60u);
            }
            if (dirty & 0x400u) // 0x520990 -> 0x20, 0x5208A0 -> 0x04
            {
                out.put64(0x10000001u);
                out.put64(0x6C01002010000000ull);
                out.copy(rdram, 0x520990u, 0x10u);
                out.put64(0x10000000u);
                out.put64(0x10000000u);
                out.put64(0x10000005u);
                out.put64(0x6C04000401000101ull);
                out.copy(rdram, 0x5208A0u, 0x40u);
                out.put32x4(0x14000083u);
            }
            if ((dirty & 0x200u) && rd32(rdram, 0x4882B0u) == 0u) // the instance matrix 0x520860 -> 0x00 (or a REF to it)
            {
                const uint32_t ref = rd32(rdram, 0x4882B8u);
                if (ref == 0u)
                {
                    out.put64(0x10000005u);
                    out.put64(0x6C04800001000101ull);
                    out.copy(rdram, 0x520860u, 0x40u);
                    out.put32x4(0x1400003Cu);
                }
                else
                {
                    out.put64((uint64_t(ref) << 32) | 0x30000004u);
                    out.put64(0x6C04800000000000ull);
                    out.put64(0x10000000u);
                    out.put64(0x1400003Cu);
                }
            }
            // GS registers: FOG (0x80), FOGCOL (0x100), TEST (0x20), ... (0x40)
            const struct
            {
                uint32_t bit, value, reg;
            } regs[4] = {{0x80u, 0x4882E0u, 6u}, {0x100u, 0x4882E8u, 7u}, {0x20u, 0x4882F0u, 0x14u}, {0x40u, 0x4882F8u, 0x15u}};
            for (const auto &g : regs)
                if (dirty & g.bit)
                {
                    out.queue[out.queued][0] = rd64(rdram, g.value);
                    out.queue[out.queued][1] = g.reg;
                    ++out.queued;
                }
            out.dirtyAfter = dirty & 0x80000000u;
            return true;
        }

        // the cursor, when the packet buffer has room for `bytes` (with a margin) and the cursor is in main RAM
        bool cursorWithRoom(const uint8_t *rdram, uint32_t bytes, uint32_t &cursor)
        {
            cursor = rd32(rdram, kPacketBuffer + 0x18u);
            if ((cursor & 0xF0000000u) != 0u || (cursor & 0xFu) != 0u)
                return false;
            // FUN_00476770: bytes used in the current half
            const int32_t base = static_cast<int32_t>(kPacketBuffer + rd32(rdram, kPacketBuffer + 0x14u) * kHalfBytes + 0x20u);
            const int32_t used = ((static_cast<int32_t>(cursor) - (base - 0x10)) >> 4) * 0x10 +
                                 ((base + static_cast<int32_t>(kHalfBytes)) - static_cast<int32_t>(rd32(rdram, kPacketBuffer + 0x1Cu)));
            const int32_t space = static_cast<int32_t>(kHalfBytes) - used;
            return space >= static_cast<int32_t>(bytes + 0x400u);
        }

        void commit(uint8_t *rdram, uint32_t cursor, const Output &out)
        {
            std::memcpy(rdram + (cursor & kRamMask), out.bytes, out.size);
            wr32(rdram, kPacketBuffer + 0x18u, cursor + out.size);
            uint32_t count = rd32(rdram, kGsQueueCount);
            for (uint32_t i = 0; i < out.queued; ++i, ++count)
            {
                wr64(rdram, kGsQueue + count * 16u, out.queue[i][0]);
                wr64(rdram, kGsQueue + count * 16u + 8u, out.queue[i][1]);
            }
            wr32(rdram, kGsQueueCount, count);
            wr32(rdram, kDirty, out.dirtyAfter);
        }

        struct Stats
        {
            uint64_t native = 0, fallback = 0, noRoom = 0, verifySame = 0, verifyDiffer = 0, queueMovedOn = 0;
        } g_stats;
        auto g_reportAt = std::chrono::steady_clock::now();

        // verify: the last flush's native bytes, compared with what the original left once it has run
        struct Pending
        {
            bool armed = false;
            uint32_t cursor = 0, queueAt = 0;
            Output out;
        } g_pending;

        void checkPending(const uint8_t *rdram)
        {
            if (!g_pending.armed)
                return;
            g_pending.armed = false;
            const Output &o = g_pending.out;
            const char *what = nullptr;
            if (std::memcmp(rdram + (g_pending.cursor & kRamMask), o.bytes, o.size) != 0)
                what = "packet bytes";
            // the queue is checked while its count is still the flush's (the draw consumes it, later setters refill it)
            // - or where the draw wrote it: right after the flush's bytes, CNT (n + 1) + DIRECTHL (n + 1), a GIFtag, then the
            // n entries (FUN_002F7D58 / FUN_002F80A0 / FUN_002F7EB0, game-internals.md 6i)
            const uint32_t total = g_pending.queueAt + o.queued, after = g_pending.cursor + o.size;
            // (the packet first: a count equal to the flush's may be a refilled queue)
            const bool inPacket = rd32(rdram, after) == ((total + 1u) | 0x10000000u) && rd32(rdram, after + 12u) == ((total + 1u) | 0x51000000u);
            const bool inQueue = !inPacket && rd32(rdram, kGsQueueCount) == total;
            if (o.queued && !inQueue && !inPacket)
                ++g_stats.queueMovedOn;
            for (uint32_t i = 0; !what && (inQueue || inPacket) && i < o.queued; ++i)
            {
                const uint32_t at = inQueue ? kGsQueue + (g_pending.queueAt + i) * 16u : after + 32u + (g_pending.queueAt + i) * 16u;
                if (rd64(rdram, at) != o.queue[i][0] || rd64(rdram, at + 8u) != o.queue[i][1])
                    what = "GS register queue";
            }
            if (!what)
            {
                ++g_stats.verifySame;
                return;
            }
            ++g_stats.verifyDiffer;
            static int logged = 0;
            if (logged++ < 10)
            {
                std::fprintf(stderr, "[rsl-native] verify: flush differs in %s (%u bytes at 0x%08x; queue %u + %u, %s)", what, o.size,
                             g_pending.cursor, g_pending.queueAt, o.queued, inQueue ? "in the queue" : "in the packet");
                for (uint32_t i = 0; std::strcmp(what, "GS register queue") == 0 && i < o.queued; ++i)
                {
                    const uint32_t at = inQueue ? kGsQueue + (g_pending.queueAt + i) * 16u : after + 32u + (g_pending.queueAt + i) * 16u;
                    std::fprintf(stderr, " [native %llx = %016llx, original %llx = %016llx]", (unsigned long long)o.queue[i][1],
                                 (unsigned long long)o.queue[i][0], (unsigned long long)rd64(rdram, at + 8u),
                                 (unsigned long long)rd64(rdram, at));
                }
                for (uint32_t i = 0; i < o.size; i += 8u)
                {
                    const uint64_t want = *reinterpret_cast<const uint64_t *>(o.bytes + i), got = rd64(rdram, g_pending.cursor + i);
                    if (want != got)
                    {
                        std::fprintf(stderr, ": first at +0x%x native %016llx original %016llx", i, (unsigned long long)want,
                                     (unsigned long long)got);
                        break;
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }

        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            if (!g_log || now - g_reportAt < std::chrono::seconds(10))
                return;
            g_reportAt = now;
            std::fprintf(stderr, "[rsl-native] flush: %llu native, %llu original (%llu of them FUN_001CFAE0, the rest plug-in callbacks), %llu original (no room)\n",
                         (unsigned long long)g_stats.native, (unsigned long long)g_stats.fallback, (unsigned long long)g_fallbackCfae0,
                         (unsigned long long)g_stats.noRoom);
            g_fallbackCfae0 = 0;
            if (g_verify)
                std::fprintf(stderr, "[rsl-native] verify: %llu flushes the same as the original, %llu differ (GS queue not checked: %llu, consumed first)\n",
                             (unsigned long long)g_stats.verifySame, (unsigned long long)g_stats.verifyDiffer,
                             (unsigned long long)g_stats.queueMovedOn);
            g_stats = Stats{};
        }

        void flushHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            checkPending(rdram);
            report();
            thread_local Output out;
            out.size = 0;
            out.queued = 0;
            uint32_t cursor = 0;
            if (!build(rdram, out))
            {
                ++g_stats.fallback;
                g_original(rdram, ctx, runtime);
                return;
            }
            if (!cursorWithRoom(rdram, out.size, cursor))
            {
                ++g_stats.noRoom;
                g_original(rdram, ctx, runtime);
                return;
            }
            if (g_verify)
            {
                // the original writes; its bytes are compared with these before the next flush (code after the original
                // returns is not relied on: DECISIONS.md "Rule for hooks on recompiled functions")
                g_pending.armed = true;
                g_pending.cursor = cursor;
                g_pending.queueAt = rd32(rdram, kGsQueueCount);
                g_pending.out = out;
                ++g_stats.native;
                g_original(rdram, ctx, runtime);
                return;
            }
            commit(rdram, cursor, out);
            ++g_stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_RSL_NATIVE");
        if (e && *e == '0')
            return;
        g_original = runtime.lookupFunction(kFlush);
        if (!g_original)
            return;
        g_native = true;
        g_verify = std::getenv("VCS_RSL_NATIVE_VERIFY") != nullptr;
        g_log = g_verify || std::getenv("VCS_RSL_NATIVE_LOG") != nullptr;
        runtime.replaceFunction(kFlush, &flushHook);
    }

    void onFrameKick(const uint8_t *rdram)
    {
        if (g_native && g_verify)
            checkPending(rdram);
    }
}
