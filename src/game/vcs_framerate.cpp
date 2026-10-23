// VCS frame rate (docs/NATIVE_PORT_ROADMAP.md milestone 4; docs/game-internals.md 4).
//
// The game paces itself on the VBlank: a VBlank-start handler (FUN_00370300) runs the registered VBlank callbacks,
// one of which (FUN_00317aa8 -> FUN_00317ac8) counts VBlanks in the graphics object at 0x520F90 (+0x4). At the end of
// a frame FUN_003179a0 waits `do { WaitVBlank(); } while (vblanks < 2);` and resets the count: 30 fps. CTimer::Update
// (FUN_002f4960) measures the frame in bus-clock ticks, so the time step follows the real frame time.
//
// 60 fps (setFrameRate60): the counting callback adds 2 per VBlank, so the limiter lets a frame through after one.
// The time step then halves by itself; what does not follow it is fixed in the game's code, each fix behind the same
// switch (docs/game-internals.md 4).
#include "game/vcs_framerate.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kCountVBlank = 0x00317AC8u; // (graphics object): +0x4 += 1, from a VBlank callback
        std::atomic<bool> g_60{false};

        // replaces FUN_00317ac8 (a leaf: lw / addiu / sw / jr)
        void countVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t at = (GPR_U32(ctx, 4) + 4u) & 0x01FFFFFFu;
            uint32_t n;
            std::memcpy(&n, rdram + at, 4);
            n += g_60.load(std::memory_order_relaxed) ? 2u : 1u;
            std::memcpy(rdram + at, &n, 4);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void setFrameRate60(bool on) { g_60.store(on, std::memory_order_relaxed); }
    bool frameRate60() { return g_60.load(std::memory_order_relaxed); }

    void installFrameRateHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(kCountVBlank, &countVBlank);
    }
}
