// World draws from the RSL world draw calls (docs/RSL_SUBMISSION.md phase 2, world sectors).
//
// FUN_002F7EB0(geometry, qwc) - the RSL world draw (game-internals.md 6i) - ends by writing a REF to a sector's prebuilt
// geometry in the streamed cells. As the frame's chain is copied, that REF's payload is replaced by a VIF MARK (the
// runtime's chain observer, runtime/ps2_chain_observer.h); at the MARK, in stream order on the thread that processes
// the chain (so the draw's state flush and GS register writes before it have run), the block is drawn from a
// recording: the first time a block is seen it is processed as the packet path would (the VIF interpreter over its
// bytes in guest memory) while its UNPACK results, MSCALs and meshes are recorded; later frames copy the recorded
// UNPACK results into VU memory and run the MSCALs through the runtime's own MSCAL (the native world handler draws
// them with the recorded meshes). No VIF parsing, unpacking, mesh hashing or vertex conversion for a recorded block.
// A recording is used only while the block's bytes hash the same and the VIF mode / cycle match.
//
// VCS_RSL_WORLD=0: the packet path (the REF's payload copied and walked as before). VCS_RSL_WORLD_VERIFY=1: every hit is
// processed by the packet path instead and recorded again, and the new recording compared with the cached one.
// VCS_RSL_WORLD_LOG=1: counts every 10 s.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::rslworld
{
    void install(PS2Runtime &runtime); // main thread, before runtime.run()
    void onFrameKick();                // game thread, at the frame kick: the frame's world draws are complete
}
