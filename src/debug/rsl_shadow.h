// The RSL shadow draw list (docs/RSL_SUBMISSION.md phase 1, VCS_RSL_SHADOW=1): the RSL draw calls of a frame, recorded
// from before-hooks on the draw functions (game-internals.md 6i), checked against the frame's VIF1 chain as the runtime
// copies it (runtime/ps2_chain_observer.h):
// - every draw must have written its own tag - a model instance (FUN_002F7D58 / FUN_002F80A0) a CALL to its packet, a
//   world draw (FUN_002F7EB0) a REF to its geometry - between its entry and the next draw's;
// - every 3D tag the packet buffer holds (CALL into the model heap, REF into the streamed cells) must belong to a draw.
// The other functions that write the buffer are hooked as boundaries, so each packet-buffer tag is also charged to
// the function that wrote it. Report every 5 s on stderr ("[rsl-shadow] ...").
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::rslshadow
{
    // Main thread, before runtime.run(): hooks and the chain observer when VCS_RSL_SHADOW is set.
    void install(PS2Runtime &runtime);

    // Game thread, at the frame kick (before the frame's chain goes out): the frame's draw list is complete.
    void onFrameKick(const uint8_t *rdram);

    // 1b, on the thread processing the chain (the MSCAL hook, before the program): the VU memory a draw program of
    // 0x0030 / 0x02C0 starts with, checked against the RSL state of the draw whose stream started it - the draw is
    // found by the MSCAL's offset in the copied chain (runtime/ps2_chain_observer.h).
    void onMscal(uint32_t startPc, const uint8_t *vu);
}
