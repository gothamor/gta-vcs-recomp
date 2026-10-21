// Game-specific runtime bindings for SLUS_215.90, applied when the ELF loads.
//
// Use this (not game/vcs/overrides.toml) for quick experiments: changing a stub
// in the recompiler config rewrites the generated headers and rebuilds every
// unity batch (~9 min), while this file only needs a relink.
// Once a binding is settled it can move to overrides.toml.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    void applyVcs(PS2Runtime &runtime)
    {
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
