// VCS display settings applied to the game (docs/SETTINGS_MENU_WIDESCREEN.md).
//
// Widescreen is Hor+: FUN_001ab9e0 (sets the camera's view window: x = tan(FOV / 2), y = x / aspect, then
// RwCameraSetViewWindow) is wrapped so the horizontal extent grows by r = display aspect / (4:3) while the vertical
// one stays (both arguments times r). RenderWare derives the frustum (EE culling) and the VU matrices from the view
// window, so the 3D, the culling and both renderers follow; the picture (still 640 wide) is presented at the wider
// aspect and the renderer keeps the HUD at 4:3 (gsHwSetHudSqueeze).
//
// The game's own widescreen preference is Vert- (same horizontal view, vertical view cut) and stays as the game has
// it; VCS_WIDESCREEN=0|1 forces it (testing only).
#include "game/vcs_display.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_hw.h"
#include <ps2_recompiled_functions.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kPrefsPointer = 0x0048F3D0u; // -> the game's preferences / menu object
        constexpr uint32_t kWidescreenField = 0x30u;    // int: 1 = the game's (Vert-) widescreen; FUN_002af658 reads it
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        std::atomic<float> g_widening{1.0f};
        std::atomic<int> g_gamePreference{-1}; // -1: leave the game's preference alone

        uint32_t load32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }

        void store32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // FUN_001ab9e0(camera a0, raster rect a1 (null: 640x448), tan(FOV / 2) f12, aspect f13)
        void setViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float r = g_widening.load(std::memory_order_relaxed);
            if (r > 1.0001f || r < 0.9999f)
            {
                ctx->f[12] *= r; // horizontal extent x r
                ctx->f[13] *= r; // vertical extent (x / aspect) unchanged
            }
            FUN_001ab9e0_0x1ab9e0(rdram, ctx, runtime);
        }
    }

    void setAspectWidening(float r) { g_widening.store(r, std::memory_order_relaxed); }

    float aspectWidening() { return g_widening.load(std::memory_order_relaxed); }

    void installDisplayHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x001AB9E0u, &setViewWindow);
        if (const char *e = std::getenv("VCS_WIDESCREEN"); e && *e) // testing: force the game's own (Vert-) preference
            g_gamePreference.store(*e == '0' ? 0 : 1, std::memory_order_relaxed);
        // VCS's render targets by what they are (game-internals.md 6g; docs/FULLY_NATIVE.md step 3d). The reflection map
        // is only ever sampled (vehicles' paint), so it follows "Internal resolution: Match window" like the picture.
        gsHwSetTargetRole(140u, 10u, "frame", false);
        gsHwSetTargetRole(0u, 10u, "shown picture (the previous frame)", false);
        gsHwSetTargetRole(280u, 10u, "Z buffer", false);
        gsHwSetTargetRole(420u, 2u, "reflection map (128x128 sample of the frame + sun glow)", true);
        gsHwSetTargetRole(420u, 4u, "bloom", false);
        gsHwSetTargetRole(452u, 4u, "bloom (second buffer)", false);
        gsHwSetTargetRole(384u, 8u, "trails", false);
        // The flip (FUN_003706c8) reads GS CSR at 0x370714 for the FIELD bit alone ($s4 = (CSR >> 13) & 1, the rest of
        // the value is overwritten in the next delay slot). FIELD is set at VBlank on the game thread: no need to wait
        // for the render worker to finish the frame's GS work (that fence was ~25% of the game thread uncapped,
        // game-internals.md 6). VCS_CSR_FENCE=1: fence as before.
        if (!std::getenv("VCS_CSR_FENCE"))
            runtime.addCsrReadWithoutFence(0x00370714u);
    }

    void applyDisplay(uint8_t *rdram)
    {
        const int wanted = g_gamePreference.load(std::memory_order_relaxed);
        if (wanted < 0)
            return;
        const uint32_t prefs = load32(rdram, kPrefsPointer);
        if (prefs == 0u || (prefs & kRamMask) + kWidescreenField + 4u > 0x02000000u)
            return; // the game has not created its preferences yet
        if (static_cast<int>(load32(rdram, prefs + kWidescreenField)) != wanted)
            store32(rdram, prefs + kWidescreenField, static_cast<uint32_t>(wanted)); // FUN_002653f0 picks the aspect up
    }
}
