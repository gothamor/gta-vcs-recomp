// VCS 2D at the RenderWare level (docs/NATIVE_PORT_ROADMAP.md milestone 3 step 2): the HUD, text, radar and menus as
// the game hands them to RenderWare's Im2D, before they become GS packets. See docs/game-internals.md 6f.
//
// Im2D (FUN_002fbe08 / FUN_002fbe68) takes 16-byte vertices: u, v (u16 fractions), RGBA (bytes), x, y (s16 pixels in
// 640x448), z (u32; the HUD's is 0xfffffe, the far plane). The game's 2D pass (FUN_0021f348) draws the sky and clouds,
// renders the radar map off screen, then calls CHud::Draw (FUN_0031c628), inside which every HUD element is drawn, then
// the message / text functions (FUN_001676c8, FUN_00396670 ...).
//
// HUD context: from CHud::Draw's entry to the end of the frame (the display-list kick). Inside it, primitives drawn with
// RenderWare's depth test off (RwRenderStateSet ZTESTENABLE = 0: the GS's ZTST ALWAYS) and not at the far plane get its
// z - the weapon icon comes through a sprite routine (0x3a07a8) at z 0x4dcc3, so the renderer's widescreen placement
// (far plane = HUD) left it out, at the 4:3 position on a 16:9 picture. Without a depth test the z changes nothing
// else; depth-tested HUD draws keep theirs (the radar map is masked to its circle by depth). VCS_HUD_RW=0 turns it off.
//
// VCS_TRACE_IM2D=<n>: log n Im2D calls (caller, primitive, vertex count, the first vertices' raw words) and markers at
// the 2D pass's calls (VCS_TRACE_IM2D_MARKERS=<1..6>), from 30 s on.
#include "game/vcs_hud.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kIm2DRender = 0x002FBE08u;        // (prim, vertices, count): RwIm2DRenderPrimitive
        constexpr uint32_t kIm2DRenderIndexed = 0x002FBE68u; // (prim, vertices, count, indices, indexCount)
        constexpr uint32_t kHudDraw = 0x0031C628u;           // CHud::Draw
        constexpr uint32_t kHudZ = 0x00FFFFFEu;              // the HUD's Im2D z (the far plane)
        constexpr uint32_t kRenderStateSet = 0x00175198u;    // RwRenderStateSet(state, value); 6 = rwRENDERSTATEZTESTENABLE

        PS2Runtime::RecompiledFunction g_origRender = nullptr, g_origRenderIndexed = nullptr, g_origHudDraw = nullptr,
                                       g_origRenderStateSet = nullptr;
        bool g_inHud = false;    // game thread
        bool g_zTest = true;     // RenderWare's ZTESTENABLE as the game last set it (off: the GS's ZTST ALWAYS)

        // one window for all trace lines: from 30 s after the hooks went in, VCS_TRACE_IM2D lines in total
        auto g_traceStart = std::chrono::steady_clock::now();
        int g_traceLeft = 0;
        bool traceNow()
        {
            if (g_traceLeft <= 0 || std::chrono::steady_clock::now() - g_traceStart < std::chrono::seconds(30))
                return false;
            --g_traceLeft;
            return true;
        }

        void trace(uint8_t *rdram, R5900Context *ctx, bool indexed)
        {
            if (!traceNow())
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & 0x01FFFFFFu;
            std::fprintf(stderr, "[im2d] %s ra %08x prim %u n %u%s%s", indexed ? "idx" : "prim", GPR_U32(ctx, 31), GPR_U32(ctx, 4),
                         GPR_U32(ctx, 6), g_inHud ? " hud" : "", indexed ? "" : "\n");
            if (indexed)
                std::fprintf(stderr, " indices %u\n", GPR_U32(ctx, 8));
            for (uint32_t q = 0; q < 4u; ++q)
            {
                uint32_t w[4];
                float f[4];
                std::memcpy(w, rdram + verts + q * 16u, 16);
                std::memcpy(f, w, 16);
                std::fprintf(stderr, "[im2d]   +%02x %08x %08x %08x %08x  (%g %g %g %g)\n", q * 16u, w[0], w[1], w[2], w[3], f[0], f[1],
                             f[2], f[3]);
            }
        }

        struct Marker
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original;
        };
        Marker g_markers[] = {{0x001676C8u, "after1", nullptr}, {0x00396670u, "after2", nullptr}, {0x003AEE80u, "after3", nullptr},
                              {0x002A9708u, "after4", nullptr}, {0x0025CD98u, "after5", nullptr}};

        template <int I>
        void marker(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- %s (%08x)\n", g_markers[I].name, g_markers[I].address);
            g_markers[I].original(rdram, ctx, runtime);
        }

        // HUD context: everything but the far plane moved onto it (see the top)
        void toHudPlane(uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_inHud || g_zTest) // depth-tested HUD draws keep their z (the radar is masked to its circle by depth)
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & 0x01FFFFFFu;
            const uint32_t n = GPR_U32(ctx, 6);
            if (n == 0u || n > 1024u || verts + n * 16u > 0x02000000u)
                return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint8_t *z = rdram + verts + i * 16u + 12u;
                uint32_t v;
                std::memcpy(&v, z, 4);
                if (v != kHudZ)
                    std::memcpy(z, &kHudZ, 4);
            }
        }

        void im2dRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trace(rdram, ctx, false);
            toHudPlane(rdram, ctx);
            g_origRender(rdram, ctx, runtime);
        }

        void im2dRenderIndexed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trace(rdram, ctx, true);
            toHudPlane(rdram, ctx);
            g_origRenderIndexed(rdram, ctx, runtime);
        }

        void renderStateSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 4) == 6u)
                g_zTest = GPR_U32(ctx, 5) != 0u;
            g_origRenderStateSet(rdram, ctx, runtime);
        }

        void hudDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- hud (%08x)\n", kHudDraw);
            g_inHud = true;
            g_origHudDraw(rdram, ctx, runtime);
        }
    }

    void onFrameKick()
    {
        if (traceNow())
            std::fprintf(stderr, "[im2d] --- frame kick\n");
        g_inHud = false;
    }

    void installHudHooks(PS2Runtime &runtime)
    {
        const char *rw = std::getenv("VCS_HUD_RW");
        const bool on = !(rw && rw[0] == '0');
        const char *traceEnv = std::getenv("VCS_TRACE_IM2D");
        if (!on && !traceEnv)
            return;
        if (traceEnv)
        {
            g_traceLeft = std::atoi(traceEnv);
            g_traceStart = std::chrono::steady_clock::now();
        }
        g_origRender = runtime.lookupFunction(kIm2DRender);
        g_origRenderIndexed = runtime.lookupFunction(kIm2DRenderIndexed);
        g_origHudDraw = runtime.lookupFunction(kHudDraw);
        g_origRenderStateSet = runtime.lookupFunction(kRenderStateSet);
        if (!g_origRender || !g_origRenderIndexed || !g_origHudDraw || !g_origRenderStateSet)
            return;
        runtime.replaceFunction(kRenderStateSet, &renderStateSet);
        runtime.replaceFunction(kIm2DRender, &im2dRender);
        runtime.replaceFunction(kIm2DRenderIndexed, &im2dRenderIndexed);
        if (on)
            runtime.replaceFunction(kHudDraw, &hudDraw);
        const PS2Runtime::RecompiledFunction markers[] = {&marker<0>, &marker<1>, &marker<2>, &marker<3>, &marker<4>};
        const int markerCount = std::getenv("VCS_TRACE_IM2D_MARKERS") ? std::atoi(std::getenv("VCS_TRACE_IM2D_MARKERS")) : 0;
        for (int i = 0; i < markerCount && i < 5; ++i)
        {
            g_markers[i].original = runtime.lookupFunction(g_markers[i].address);
            if (g_markers[i].original)
                runtime.replaceFunction(g_markers[i].address, markers[i]);
        }
    }
}
