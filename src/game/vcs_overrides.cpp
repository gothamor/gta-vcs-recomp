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

    void applyVcs(PS2Runtime &runtime)
    {
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
