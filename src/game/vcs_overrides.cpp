// Game-specific runtime bindings for SLUS_215.90, applied when the ELF loads.
//
// Use this (not game/vcs/overrides.toml) for quick experiments: changing a stub
// in the recompiler config rewrites the generated headers and rebuilds every
// unity batch (~9 min), while this file only needs a relink.
// Once a binding is settled it can move to overrides.toml.
#include "game_overrides.h"
#include "debug/rig.h"
#include "game/vcs_display.h"
#include "game/vcs_framerate.h"
#include "game/vcs_hud.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_perf.h"
#include <ps2_recompiled_functions.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <map>
#include <iostream>

namespace
{
    uint32_t g_lastInflateDst = 0;

    struct Binding
    {
        uint32_t address;
        const char *handler; // runtime syscall/stub name
        const char *why;
    };

    // Functions replaced by a runtime handler.
    constexpr Binding kBindings[] = {
        {0, nullptr, nullptr},
    };

    // Functions that just return 0 (skipped).
    struct Skip
    {
        uint32_t address;
        const char *why;
    };
    constexpr Skip kSkips[] = {
        // Movie player (STINGS.PSS / INTRO.PSS). Its IPU/sceMpeg path stalls on the
        // runtime's MPEG stubs; skipped until native playback (Phase 4).
        {0x0040D8B8, "CMoviePlayer::Play(path)"},
    };

    // ---- VCS_TRACE_FILES=1: log the game's own file API (CFileMgr-like, 0x220xxx) ----
    const char *guestString(uint8_t *rdram, uint32_t address)
    {
        return reinterpret_cast<const char *>(rdram + (address & 0x01FFFFFFu));
    }

    void traceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t nameAddr = GPR_U32(ctx, 4);
        if (std::strstr(guestString(rdram, nameAddr), "LVZ"))
        {
            // The disc index built at boot by FUN_002209F0: count at 0x486E8C, 16-byte entries at 0x5038A8
            uint32_t count = 0;
            std::memcpy(&count, rdram + 0x00486E8Cu, 4);
            std::fprintf(stderr, "[file] index has %u entries; LVZ/IMG ones:\n", count);
            for (uint32_t i = 0; i < count && i < 4096u; ++i)
            {
                uint32_t e[4];
                std::memcpy(e, rdram + 0x005038A8u + i * 16u, sizeof(e));
                const char *n = guestString(rdram, e[0]);
                if (std::strstr(n, "LVZ") || std::strstr(n, "IMG"))
                    std::fprintf(stderr, "[file]   #%u \"%s\" lsn=0x%x size=0x%x\n", i, n, e[1], e[2]);
            }
        }
        const uint32_t ra = GPR_U32(ctx, 31);
        FUN_00220c68_0x220c68(rdram, ctx, runtime);
        std::fprintf(stderr, "[file] open \"%s\" -> handle %d (ra=0x%08x)\n", guestString(rdram, nameAddr),
                     static_cast<int32_t>(GPR_U32(ctx, 2)), ra);
    }

    void traceRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[file] read handle %d buf=0x%08x size=0x%x (ra=0x%08x)\n",
                     static_cast<int32_t>(GPR_U32(ctx, 4)), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 31));
        const uint32_t buf = GPR_U32(ctx, 5) & 0x01FFFFFFu;
        const uint32_t size = GPR_U32(ctx, 6);
        FUN_00220778_0x220778(rdram, ctx, runtime);
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < size && buf + i < 0x02000000u; ++i)
            hash = (hash ^ rdram[buf + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file]   -> %d, fnv64=%016llx first=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                     static_cast<int32_t>(GPR_U32(ctx, 2)), static_cast<unsigned long long>(hash),
                     rdram[buf], rdram[buf + 1], rdram[buf + 2], rdram[buf + 3], rdram[buf + 4], rdram[buf + 5],
                     rdram[buf + 6], rdram[buf + 7]);
    }

    void traceInflate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // FUN_0021FA38(dst, src, dstCapacity, ?) - zlib inflate of e.g. GAME.DTZ
        const uint32_t src = GPR_U32(ctx, 5) & 0x01FFFFFFu;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < 0x217E70u && src + i < 0x02000000u; ++i)
            hash = (hash ^ rdram[src + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] inflate dst=0x%08x src=0x%08x cap=0x%x first=%02x%02x fnv64(0x217e70)=%016llx\n",
                     GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), rdram[src], rdram[src + 1],
                     static_cast<unsigned long long>(hash));
        g_lastInflateDst = GPR_U32(ctx, 4);
        FUN_0021fa38_0x21fa38(rdram, ctx, runtime);
    }

    void traceRelocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // FUN_00324A50(header{'GTAG',1,..}, image, flag): relocates a loaded GTAG memory image.
        const uint32_t image = GPR_U32(ctx, 5) & 0x01FFFFFFu;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < 0x6495D0u && image + i < 0x02000000u; ++i)
            hash = (hash ^ rdram[image + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] relocate image=0x%08x fnv64(0x6495d0)=%016llx\n", image,
                     static_cast<unsigned long long>(hash));
        if (std::memcmp(rdram + image, "DLRW", 4) == 0) // WRLD level image (from .LVZ): dump before relocation
        {
            if (FILE *f = std::fopen("wrld_image.bin", "wb"))
            {
                std::fwrite(rdram + image, 1, 0x00600000u, f);
                std::fclose(f);
            }
        }
        FUN_00324a50_0x324a50(rdram, ctx, runtime);
    }

    void traceInflateEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // inflateEnd(z_stream *strm): strm->next_out (+0x0C) / total_out (+0x14)
        const uint32_t strm = GPR_U32(ctx, 4) & 0x01FFFFFFu;
        uint32_t totalOut = 0;
        std::memcpy(&totalOut, rdram + strm + 0x14, 4);
        const uint32_t dst = g_lastInflateDst & 0x01FFFFFFu;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < totalOut && dst + i < 0x02000000u; ++i)
            hash = (hash ^ rdram[dst + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] inflateEnd total_out=0x%x fnv64=%016llx first=%02x%02x%02x%02x\n", totalOut,
                     static_cast<unsigned long long>(hash), rdram[dst], rdram[dst + 1], rdram[dst + 2], rdram[dst + 3]);
        uint32_t f[8];
        std::memcpy(f, rdram + strm, sizeof(f));
        const char *msg = f[6] ? reinterpret_cast<const char *>(rdram + (f[6] & 0x01FFFFFFu)) : "(null)";
        uint32_t state = f[7] & 0x01FFFFFFu, mode = 0;
        if (state)
            std::memcpy(&mode, rdram + state, 4);
        std::fprintf(stderr, "[file]   next_in=0x%08x avail_in=0x%x total_in=0x%x next_out=0x%08x avail_out=0x%x msg=\"%s\" state.mode=%u\n",
                     f[0], f[1], f[2], f[3], f[4], msg, mode);
        FUN_00265208_0x265208(rdram, ctx, runtime);
    }

    void returnZero(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        SET_GPR_U64(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // abort() (0x4604F0 -> exit(1)): the game hit a fatal check. Log a heuristic guest
    // backtrace: stack words that point just past a jal/jalr in .text.
    void traceAbort(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[vcs] abort()/_Exit pc=0x%08x called from ra=0x%08x sp=0x%08x a0=0x%08x\n[vcs] backtrace:",
                     ctx->pc, GPR_U32(ctx, 31), GPR_U32(ctx, 29), GPR_U32(ctx, 4));
        const uint32_t sp = GPR_U32(ctx, 29) & 0x01FFFFFFu;
        int shown = 0;
        for (uint32_t a = sp; a < sp + 0x2000u && a + 4u <= 0x02000000u && shown < 24; a += 4u)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + a, 4);
            if (v < 0x00100008u || v >= 0x00480000u || (v & 3u))
                continue;
            uint32_t call = 0;
            std::memcpy(&call, rdram + v - 8u, 4);
            if ((call >> 26) == 3u || (call & 0xFC1F07FFu) == 0x00000009u)
            {
                std::fprintf(stderr, " 0x%08x", v - 8u);
                ++shown;
            }
        }
        std::fprintf(stderr, "\n");
        if (ctx->pc == 0x004672C0u)
            ps2__Exit_0x4672c0(rdram, ctx, runtime);
        else
            FUN_004604f0_0x4604f0(rdram, ctx, runtime);
    }

    // VCS_TRACE_DELAY=1: DelayThread(usec) (0x468318, SetTimerAlarm + WaitSema) with host time between calls.
    void traceDelay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static auto last = std::chrono::steady_clock::now();
        static int count = 0;
        const auto now = std::chrono::steady_clock::now();
        if (count++ < 100000)
            std::fprintf(stderr, "[delay] DelayThread(%u us) ra=0x%08x  %.2f ms since previous call\n", GPR_U32(ctx, 4),
                         GPR_U32(ctx, 31), std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        FUN_00468318_0x468318(rdram, ctx, runtime);
    }

    // VCS_TRACE_SOUND=1: the EE sound driver's RPC commands (FUN_0010E970(cmd, arg) -> client 0x4D3D40, sid 0x65686579)
    void traceSoundCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int count = 0;
        const uint32_t cmd = GPR_U32(ctx, 4);
        if ((cmd & 0xFF0000u) != 0x0A0000u && count++ < 300)
        {
            uint32_t server = 0, reply = 0;
            std::memcpy(&server, rdram + 0x004D3D40u + 0x24u, 4);
            std::memcpy(&reply, rdram + 0x004CE300u, 4);
            std::fprintf(stderr, "[sound] cmd=0x%06x arg=0x%x ra=0x%08x client.server=0x%08x lastReply=0x%x\n", GPR_U32(ctx, 4),
                         GPR_U32(ctx, 5), GPR_U32(ctx, 31), server, reply);
        }
        FUN_0010e970_0x10e970(rdram, ctx, runtime);
    }

    void traceSoundInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t flag = 0;
        std::memcpy(&flag, rdram + 0x00481900u, 4);
        std::fprintf(stderr, "[sound] init (0x10D868) skipFlag@0x481900=0x%x ra=0x%08x\n", flag, GPR_U32(ctx, 31));
        FUN_0010d868_0x10d868(rdram, ctx, runtime);
    }

    // Level loader FUN_0023BAF0(level, ...): reads <level>.LVZ, stores the WRLD image at level+0x1C
    void traceLevelLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[level] load level=0x%08x a1=0x%08x a2=0x%08x ra=0x%08x\n", GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                     GPR_U32(ctx, 6), GPR_U32(ctx, 31));
        FUN_0023baf0_0x23baf0(rdram, ctx, runtime);
    }

    // VCS_COUNT_CALLS=addr,addr,...: count calls of up to 32 functions, print every 5 s.
    struct CountedFn
    {
        uint32_t address = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        uint64_t calls = 0;
    };
    CountedFn g_counted[32];
    uint32_t g_countedN = 0;

    template <int I>
    void countCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_counted[I].calls;
        static auto last = std::chrono::steady_clock::now();
        if (I == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(5))
            {
                last = now;
                std::fprintf(stderr, "[calls]");
                for (uint32_t i = 0; i < g_countedN; ++i)
                {
                    std::fprintf(stderr, " %06x:%llu", g_counted[i].address, static_cast<unsigned long long>(g_counted[i].calls));
                    g_counted[i].calls = 0;
                }
                std::fprintf(stderr, "\n");
            }
        }
        g_counted[I].original(rdram, ctx, runtime);
    }

    template <int... Is>
    constexpr PS2Runtime::RecompiledFunction counterFor(int i, std::integer_sequence<int, Is...>)
    {
        PS2Runtime::RecompiledFunction table[] = {&countCall<Is>...};
        return table[i];
    }

    void installCallCounters(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_COUNT_CALLS");
        if (!env)
            return;
        for (const char *p = env; *p && g_countedN < 32u;)
        {
            char *end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            CountedFn &c = g_counted[g_countedN];
            c.address = static_cast<uint32_t>(a);
            c.original = runtime.lookupFunction(c.address);
            if (c.original)
                runtime.replaceFunction(c.address, counterFor(static_cast<int>(g_countedN), std::make_integer_sequence<int, 32>{}));
            ++g_countedN;
            p = *end ? end + 1 : end;
        }
    }

    // VCS_TRACE_DMASEND=1: sceDmaSend(channel, tag) callers (ra) with counts, every 5 s.
    PS2Runtime::RecompiledFunction g_origDmaSend = nullptr;
    void traceDmaSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<std::pair<uint32_t, uint32_t>, uint64_t> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[{GPR_U32(ctx, 31), GPR_U32(ctx, 4)}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5))
        {
            last = now;
            for (const auto &[k, n] : counts)
                std::fprintf(stderr, "[dmasend] ra=0x%08x ch=0x%08x x%llu\n", k.first, k.second, static_cast<unsigned long long>(n));
            counts.clear();
        }
        g_origDmaSend(rdram, ctx, runtime);
    }

    // VCS_TRACE_VISIBLE=1: CCamera::IsSphereVisible(cam, center, matrix, radius) at 0x3816F8 (a leaf, safe to
    // inspect after the call): visible ratio every 5 s and a few samples with the camera-space point.
    float guestFloat(uint8_t *rdram, uint32_t address)
    {
        float f;
        std::memcpy(&f, rdram + (address & 0x01FFFFFFu), 4);
        return f;
    }
    void traceSphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = GPR_U32(ctx, 4), center = GPR_U32(ctx, 5), mat = GPR_U32(ctx, 6);
        const float radius = ctx->f[12];
        const uint32_t sp = GPR_U32(ctx, 29) - 16u;
        FUN_003816f8_0x3816f8(rdram, ctx, runtime);
        static uint64_t calls = 0, visible = 0;
        static int samples = 0;
        static auto last = std::chrono::steady_clock::now();
        ++calls;
        visible += GPR_U32(ctx, 2) ? 1u : 0u;
        if (samples < 12 && (calls % 997u) == 0u)
        {
            ++samples;
            std::fprintf(stderr,
                         "[vis] r=%u center=(%.1f,%.1f,%.1f) radius=%.1f camSpace=(%.1f,%.1f,%.1f) near=%.3f far=%.1f "
                         "planes=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) mat.row3=(%.1f,%.1f,%.1f)\n",
                         GPR_U32(ctx, 2), guestFloat(rdram, center), guestFloat(rdram, center + 4), guestFloat(rdram, center + 8), radius,
                         guestFloat(rdram, sp), guestFloat(rdram, sp + 4), guestFloat(rdram, sp + 8), guestFloat(rdram, 0x4CD130u),
                         guestFloat(rdram, 0x4CD12Cu), guestFloat(rdram, cam + 0xB60), guestFloat(rdram, cam + 0xB64),
                         guestFloat(rdram, cam + 0xB70), guestFloat(rdram, cam + 0xB74), guestFloat(rdram, cam + 0xB84),
                         guestFloat(rdram, cam + 0xB88), guestFloat(rdram, cam + 0xB94), guestFloat(rdram, cam + 0xB98),
                         guestFloat(rdram, mat + 48), guestFloat(rdram, mat + 52), guestFloat(rdram, mat + 56));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5))
        {
            last = now;
            std::fprintf(stderr, "[vis] %llu calls, %llu visible\n", static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(visible));
            calls = visible = 0;
        }
    }

    // VCS_TRACE_BOXCLIP=1: FUN_0030C8D4(prog, box*, out0*, out1*) = CTC2 CMSAR0; LQC2 vf1,vf2; VCALLMSR; CFC2 vi1,vi2.
    // The sector visibility test (FUN_002F7528) culls unless the low 6 bits come back 0x3F.
    void traceBoxClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t prog = GPR_U32(ctx, 4), box = GPR_U32(ctx, 5), out0 = GPR_U32(ctx, 6), out1 = GPR_U32(ctx, 7);
        static bool dumped = false;
        if (!dumped)
        {
            dumped = true;
            if (FILE *f = std::fopen("vu0_code.bin", "wb"))
            {
                std::fwrite(runtime->memory().getVU0Code(), 1, PS2_VU0_CODE_SIZE, f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen("vu0_data.bin", "wb"))
            {
                std::fwrite(runtime->memory().getVU0Data(), 1, PS2_VU0_DATA_SIZE, f);
                std::fclose(f);
            }
        }
        FUN_0030c8d4_0x30c8d4(rdram, ctx, runtime);
        static int samples = 0;
        static uint64_t calls = 0, allIn = 0;
        uint32_t r0 = 0, r1 = 0;
        std::memcpy(&r0, rdram + (out0 & 0x01FFFFFFu), 4);
        std::memcpy(&r1, rdram + (out1 & 0x01FFFFFFu), 4);
        ++calls;
        allIn += ((r1 & 0x3Fu) == 0x3Fu || (r0 & 0x3Fu) == 0x3Fu) ? 1u : 0u;
        if (samples < 10 && (calls % 211u) == 0u)
        {
            ++samples;
            std::fprintf(stderr, "[boxclip] prog=0x%x box=(%.2f,%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f,%.2f) -> vi1>>4=0x%x vi2>>4=0x%x vi15=%u\n", prog,
                         guestFloat(rdram, box), guestFloat(rdram, box + 4), guestFloat(rdram, box + 8), guestFloat(rdram, box + 12),
                         guestFloat(rdram, box + 16), guestFloat(rdram, box + 20), guestFloat(rdram, box + 24), guestFloat(rdram, box + 28), r0, r1,
                         static_cast<uint32_t>(ctx->vi[15]));
        }
        if ((calls % 20000u) == 0u)
            std::fprintf(stderr, "[boxclip] %llu calls, %llu with 0x3F\n", static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(allIn));
    }

    // VCS_TEST_SAVE=<s>[:<slot>]: <s> seconds after boot, save to slot 1 (or 1..8) through the game's own memory card code, in the
    // order FUN_00108128 (the safehouse save menu's action) uses: FUN_00276bf0(card manager 0x510480, 0) scans
    // the card, the "saving" flag at 0x48A664 goes up, FUN_002778a8(manager, slot) saves, the flag goes down.
    // Each call is made as a guest tail call from scePadRead (pc = function, ra = scePadRead's return address),
    // so scheduler checkpoints inside the save unwind and resume normally; scePadRead's own result is lost
    // for those frames. Lets the save path be tested without playing to a safehouse.
    //
    // VCS_DUMP_RAM=<s>[,<s>...]: write all 32 MB of EE RAM to ram_<s>.bin at those seconds after boot
    // (finding game structures by diffing snapshots).
    PS2Runtime::RecompiledFunction g_origPadRead = nullptr;
    void dumpRamOnSchedule(const uint8_t *rdram)
    {
        static const char *env = std::getenv("VCS_DUMP_RAM");
        static const auto start = std::chrono::steady_clock::now();
        static size_t next = 0;
        if (!env)
            return;
        const char *p = env;
        for (size_t i = 0; i < next && p; ++i)
            p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : nullptr;
        if (!p || !*p)
            return;
        const int at = std::atoi(p);
        if (std::chrono::steady_clock::now() - start < std::chrono::seconds(at))
            return;
        ++next;
        char name[64];
        std::snprintf(name, sizeof(name), "ram_%d.bin", at);
        if (FILE *f = std::fopen(name, "wb"))
        {
            std::fwrite(rdram, 1, 32u * 1024u * 1024u, f);
            std::fclose(f);
            std::fprintf(stderr, "[ram] wrote %s\n", name);
        }
    }

    void testSaveOnPadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const auto start = std::chrono::steady_clock::now();
        static const char *saveEnv = std::getenv("VCS_TEST_SAVE");
        static const int delay = saveEnv ? std::atoi(saveEnv) : 0;
        static const uint32_t slot = [] {
            const char *colon = saveEnv ? std::strchr(saveEnv, ':') : nullptr;
            const int s = colon ? std::atoi(colon + 1) : 1;
            return static_cast<uint32_t>(std::clamp(s, 1, 8) - 1); // the game counts slots from 0
        }();
        static int step = 0;
        constexpr uint32_t kCardManager = 0x00510480u, kSavingFlag = 0x0048A664u;
        g_origPadRead(rdram, ctx, runtime);
        dumpRamOnSchedule(rdram);
        if (!saveEnv || step > 3 || std::chrono::steady_clock::now() - start < std::chrono::seconds(delay))
            return;
        uint32_t status = 0;
        std::memcpy(&status, rdram + kCardManager + 0x54u, 4);
        const auto tailCall = [&](uint32_t function, uint32_t a1 = 0u)
        {
            SET_GPR_U32(ctx, 4, kCardManager);
            SET_GPR_U32(ctx, 5, a1);
            ctx->pc = function;
        };
        switch (step++)
        {
        case 0:
            std::fprintf(stderr, "[mc-test] scanning the card (status %u)\n", status);
            tailCall(0x00276BF0u);
            break;
        case 1:
        {
            std::fprintf(stderr, "[mc-test] scan done (status %u); saving to slot %u\n", status, slot + 1u);
            const uint32_t one = 1u;
            std::memcpy(rdram + kSavingFlag, &one, 4);
            tailCall(0x002778A8u, slot);
            break;
        }
        case 2:
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + kSavingFlag, &zero, 4);
            std::fprintf(stderr, "[mc-test] save done, manager status %u\n", status);
            break;
        }
        }
    }

    // VCS_TRACE_SECTORS=1: why FUN_00240ED8(level, sector) skips sectors, every 5 s. It draws a sector only when
    // the sector has a model index (+2) whose slot in the level's model table (*(level+0x1C))[idx*12] holds a
    // loaded model; the culling result decides the rest.
    // VCS_TRACE_TELEPORT: FUN_0020b3d8(ped, CVector *pos) is the ped teleport (remove, set position, reset, add);
    // log who calls it (it is reached through a vtable) and where to.
    uint32_t g_currentScript = 0; // CRunningScript being interpreted (FUN_002560d8 ProcessOneCommand), game thread
    void traceScriptCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_currentScript = GPR_U32(ctx, 4);
        FUN_002560d8_0x2560d8(rdram, ctx, runtime);
    }

    void traceTeleport(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float pos[3];
        std::memcpy(pos, rdram + (GPR_U32(ctx, 5) & 0x01FFFFFFu), sizeof(pos));
        char name[9] = {};
        uint32_t ip = 0;
        if (g_currentScript)
        {
            std::memcpy(name, rdram + ((g_currentScript + 0x20F) & 0x01FFFFFFu), 8);
            std::memcpy(&ip, rdram + ((g_currentScript + 0x10) & 0x01FFFFFFu), 4);
        }
        std::fprintf(stderr, "[teleport] ped %08x -> (%.2f, %.2f, %.2f) ra=%08x last script %s ip %x\n", GPR_U32(ctx, 4), pos[0],
                     pos[1], pos[2], GPR_U32(ctx, 31), name, ip);
        FUN_0020b3d8_0x20b3d8(rdram, ctx, runtime);
    }

    uint64_t g_sectorCalls = 0; // FUN_00240ED8 calls, for VCS_TRACE_CELL
    void traceSector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_sectorCalls;
        static uint64_t calls = 0, noSector = 0, noIndex = 0, notLoaded = 0, loaded = 0;
        static auto last = std::chrono::steady_clock::now();
        static std::map<uint32_t, uint32_t> missing; // model index -> count
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; };
        const uint32_t level = GPR_U32(ctx, 4), sector = GPR_U32(ctx, 5);
        ++calls;
        if (!sector)
            ++noSector;
        else
        {
            uint16_t idx = 0;
            std::memcpy(&idx, rdram + ((sector + 2u) & 0x01FFFFFFu), 2);
            if (!idx)
                ++noIndex;
            else if (!rd32(rd32(rd32(level + 0x1Cu)) + idx * 12u))
            {
                ++notLoaded;
                ++missing[idx];
            }
            else
                ++loaded;
        }
        FUN_00240ed8_0x240ed8(rdram, ctx, runtime);
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5))
            return;
        last = now;
        std::fprintf(stderr, "[sector] %llu calls: no sector %llu, no model %llu, model not loaded %llu, loaded %llu; missing idx:",
                     (unsigned long long)calls, (unsigned long long)noSector, (unsigned long long)noIndex,
                     (unsigned long long)notLoaded, (unsigned long long)loaded);
        int shown = 0;
        for (const auto &[idx, n] : missing)
            if (shown++ < 12)
                std::fprintf(stderr, " %u(x%u)", idx, n);
        std::fprintf(stderr, " [%zu distinct]\n", missing.size());
        calls = noSector = noIndex = notLoaded = loaded = 0;
        missing.clear();
    }

    // VCS_TRACE_CELL=1: the world is drawn relative to the "current" hex cell (*(level+0x248): grid x at +8, y at
    // +0xC; FUN_00241F00 turns it into a world origin). Logs that cell every 2 s from the sector walk
    // FUN_0023E748, and the focus position each time FUN_0023C188(level, pos) re-evaluates it.
    void traceCellWalk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static auto last = std::chrono::steady_clock::now();
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; };
        const uint32_t level = GPR_U32(ctx, 4);
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(2))
        {
            last = now;
            const uint32_t cur = rd32(level + 0x248u), next = rd32(level + 0x250u);
            const int32_t gx = static_cast<int32_t>(rd32(cur + 8u)), gy = static_cast<int32_t>(rd32(cur + 0xCu));
            const float ox = (gx + 0.5f - (gy & 1) * 0.5f) * 125.0f - 2400.0f, oy = (gy + 0.5f) * 108.25f - 2000.0f;
            std::fprintf(stderr, "[cell] level=0x%x current=0x%x grid=(%d,%d) origin=(%.1f,%.1f) loaded=%u next=0x%x grid=(%d,%d) loaded=%u camera=(%.1f,%.1f,%.1f)\n",
                         level, cur, gx, gy, ox, oy, rd32(cur + 0x18u), next, static_cast<int32_t>(rd32(next + 8u)),
                         static_cast<int32_t>(rd32(next + 0xCu)), rd32(next + 0x18u), guestFloat(rdram, 0x006F4500u),
                         guestFloat(rdram, 0x006F4504u), guestFloat(rdram, 0x006F4508u));
        }
        static uint64_t walks = 0, emptyWalks = 0, skippedGrid = 0;
        static auto lastWalk = std::chrono::steady_clock::now();
        const uint64_t before = g_sectorCalls;
        if (static_cast<int32_t>(rd32(rd32(level + 0x248u) + 8u)) <= 0)
            ++skippedGrid;
        FUN_0023e748_0x23e748(rdram, ctx, runtime);
        ++walks;
        emptyWalks += (g_sectorCalls == before) ? 1u : 0u;
        if (now - lastWalk > std::chrono::seconds(1))
        {
            lastWalk = now;
            std::fprintf(stderr, "[cell] walks %llu, without sectors %llu, grid<=0 %llu\n", (unsigned long long)walks,
                         (unsigned long long)emptyWalks, (unsigned long long)skippedGrid);
            walks = emptyWalks = skippedGrid = 0;
        }
    }
    void traceCellFocus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pos = GPR_U32(ctx, 5);
        const float x = guestFloat(rdram, pos), y = guestFloat(rdram, pos + 4), z = guestFloat(rdram, pos + 8);
        const int row = static_cast<int>((y + 2000.0f) * (1.0f / 108.25f));
        const int col = static_cast<int>((x + 2400.0f) * 0.008f + (row & 1) * 0.5f);
        std::fprintf(stderr, "[cell] focus (%.1f,%.1f,%.1f) -> grid (%d,%d)\n", x, y, z, col, row);
        FUN_0023c188_0x23c188(rdram, ctx, runtime);
    }

    // VCS_TRACE_PACKETS=1: FUN_00476770(buffer) returns the bytes used in the current half of a double-buffered DMA
    // packet buffer (halves of 0xE6670). Draw code skips work once fewer than ~0x12C00 bytes are left; log the
    // peak use and how often that limit was reached, every 2 s.
    void tracePacketUse(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<uint32_t, uint32_t> peak;
        static uint64_t calls = 0, nearFull = 0;
        static auto last = std::chrono::steady_clock::now();
        const uint32_t buffer = GPR_U32(ctx, 4);
        FUN_00476770_0x476770(rdram, ctx, runtime);
        const uint32_t used = GPR_U32(ctx, 2);
        ++calls;
        nearFull += (0xE6670u - used <= 0x12BFFu) ? 1u : 0u;
        uint32_t &p = peak[buffer];
        p = std::max(p, used);
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(2))
            return;
        last = now;
        std::fprintf(stderr, "[packets] %llu checks, %llu near full; peak use:", (unsigned long long)calls, (unsigned long long)nearFull);
        for (const auto &[b, u] : peak)
            std::fprintf(stderr, " 0x%x=%u KB", b, u >> 10);
        std::fprintf(stderr, " (of %u KB)\n", 0xE6670u >> 10);
        peak.clear();
        calls = nearFull = 0;
    }

    // Game frame boundary for the perf overlay (runtime/ps2_perf.h): FUN_002723E8 kicks the frame's VIF1 display
    // list (D1_CHCR = 0x145 at 0x27245C), once per rendered frame.
    void markFrameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2perf::markGameFrame();
        vcs::applyDisplay(rdram); // widescreen preference / aspect (settings menu)
        vcs::rig::onGameFrame(rdram, ctx, runtime); // automation rig jobs (debug/rig.h)
        vcs::onFrameKick();                         // the HUD context ends (game/vcs_hud.h)
        FUN_002723e8_0x2723e8(rdram, ctx, runtime);
    }

    void applyVcs(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x002723E8u, &markFrameKick);
        vcs::installDisplayHooks(runtime); // widescreen (settings menu)
        vcs::installHudHooks(runtime);     // 2D at the RenderWare level
        vcs::installFrameRateHooks(runtime); // 30 / 60 fps (game/vcs_framerate.h)
        if (std::getenv("VCS_TRACE_PACKETS"))
            runtime.replaceFunction(0x00476770u, &tracePacketUse);
        if (std::getenv("VCS_TRACE_TELEPORT"))
        {
            runtime.replaceFunction(0x0020B3D8u, &traceTeleport);
            runtime.replaceFunction(0x002560D8u, &traceScriptCommand);
        }
        if (std::getenv("VCS_TRACE_SECTORS"))
            runtime.replaceFunction(0x00240ED8u, &traceSector);
        if (std::getenv("VCS_TRACE_CELL"))
        {
            runtime.replaceFunction(0x00240ED8u, &traceSector);
            runtime.replaceFunction(0x0023E748u, &traceCellWalk);
            runtime.replaceFunction(0x0023C188u, &traceCellFocus);
        }
        installCallCounters(runtime);
        if (std::getenv("VCS_TEST_SAVE") || std::getenv("VCS_DUMP_RAM"))
        {
            g_origPadRead = runtime.lookupFunction(0x00449488u); // scePadRead: polled every frame
            if (g_origPadRead)
                runtime.replaceFunction(0x00449488u, &testSaveOnPadRead);
        }
        if (std::getenv("VCS_TRACE_BOXCLIP"))
            runtime.replaceFunction(0x0030C8D4u, &traceBoxClip);
        if (std::getenv("VCS_EXPERIMENT_NOCLIP")) // timing experiment only: every sector box "fully inside"
            runtime.replaceFunction(0x0030C8D4u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
                                    {
                                        const uint32_t v = 0x3Fu;
                                        std::memcpy(rdram + (GPR_U32(ctx, 6) & 0x01FFFFFFu), &v, 4);
                                        std::memcpy(rdram + (GPR_U32(ctx, 7) & 0x01FFFFFFu), &v, 4);
                                        ctx->pc = GPR_U32(ctx, 31);
                                    });
        if (std::getenv("VCS_TRACE_VISIBLE"))
            runtime.replaceFunction(0x003816F8u, &traceSphereVisible);
        if (std::getenv("VCS_TRACE_DMASEND"))
        {
            g_origDmaSend = runtime.lookupFunction(0x00448970u);
            if (g_origDmaSend)
                runtime.replaceFunction(0x00448970u, &traceDmaSend);
        }
        if (std::getenv("VCS_TRACE_FILES"))
            runtime.replaceFunction(0x0023BAF0u, &traceLevelLoad);
        if (std::getenv("VCS_TRACE_SOUND"))
        {
            runtime.replaceFunction(0x0010E970u, &traceSoundCmd);
            runtime.replaceFunction(0x0010D868u, &traceSoundInit);
        }
        if (std::getenv("VCS_TRACE_DELAY"))
            runtime.replaceFunction(0x00468318u, &traceDelay);
        runtime.replaceFunction(0x004604F0u, &traceAbort);
        runtime.replaceFunction(0x004672C0u, &traceAbort); // _Exit
        for (const Binding &b : kBindings)
        {
            if (b.handler && !ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
                std::cerr << "[vcs] failed to bind 0x" << std::hex << b.address << std::dec << " -> " << b.handler << std::endl;
        }
        for (const Skip &s : kSkips)
        {
            if (!runtime.replaceFunction(s.address, &returnZero))
                std::cerr << "[vcs] failed to skip 0x" << std::hex << s.address << std::dec << " (" << s.why << ")" << std::endl;
        }
        if (std::getenv("VCS_TRACE_FILES"))
        {
            runtime.replaceFunction(0x00220C68u, &traceOpen);
            runtime.replaceFunction(0x00220778u, &traceRead);
            runtime.replaceFunction(0x0021FA38u, &traceInflate);
            runtime.replaceFunction(0x00324A50u, &traceRelocate);
            runtime.replaceFunction(0x00265208u, &traceInflateEnd);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS-21590)", "SLUS_215.90", 0x001DD6A0u, 0x079B8BE6u, &applyVcs)
