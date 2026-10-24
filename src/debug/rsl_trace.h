// The RSL submission trace (docs/RSL_SUBMISSION.md phase 0, VCS_TRACE_RSL=1): which game functions build the frame's
// VIF1 DMA packets, how often and how many bytes. Every function that moves the packet buffer's write cursor (the
// object at 0x520F90, cursor at +0x18; game-internals.md 6i) gets a before-hook that reads the cursor; the bytes
// written between one hooked entry and the next are charged to the first (its own writes and those of callees not
// in the list), the rest of the frame at the frame kick. Report every 5 s on stderr: "[rsl] ..." per function, per
// game frame.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::rsltrace
{
    // Main thread, before runtime.run(): hooks the functions when VCS_TRACE_RSL is set.
    void install(PS2Runtime &runtime);

    // Game thread, at the frame kick (before the frame's chain goes out).
    void onFrameKick(const uint8_t *rdram);
}
