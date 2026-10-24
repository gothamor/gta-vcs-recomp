// Native versions of RSL packet emitters (docs/RSL_SUBMISSION.md, game-internals.md 6i "The state flush").
//
// FUN_002F8710 - the state flush every RSL draw runs first - writes the dirty render state into the packet buffer as
// VIF UNPACKs / CALLs (matrices, viewport, lights, fog, depth range) and queues dirty GS registers. Recompiled it was
// ~2% of the game thread while driving; the native version writes the same bytes. Falls back to the original when the
// flush would call other guest code (dirty bits 0x1 / 0x4 / 0x10: render-context callbacks, FUN_001CFAE0), when the
// buffer is nearly full (the original then writes through a null pointer) or when the cursor is not in main RAM.
//
// VCS_RSL_NATIVE=0: the original. VCS_RSL_NATIVE_VERIFY=1: the original runs; the native bytes are computed alongside
// and compared with what it wrote (at the next flush / the frame kick). VCS_RSL_NATIVE_LOG=1: counts every 10 s.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::rslnative
{
    void install(PS2Runtime &runtime); // main thread, before runtime.run()
    void onFrameKick(const uint8_t *rdram); // game thread, at the frame kick
}
