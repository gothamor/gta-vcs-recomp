// World and model draws from the RSL draw calls (docs/RSL_SUBMISSION.md phase 2: world sectors, model instances).
//
// FUN_002F7EB0(geometry, qwc) - the RSL world draw (game-internals.md 6i) - ends by writing a REF to a sector's prebuilt
// geometry in the streamed cells; FUN_002F7D58(packet) / FUN_002F80A0(packet) - model instances - end by writing a CALL
// to the model's prebuilt packet (a sub-chain ending in RET). As the frame's chain is copied, a VIF MARK qword is put
// before such a REF's payload / CALLed packet (the runtime's chain observer, runtime/ps2_chain_observer.h; every CALL
// in the packet buffer is marked). At the MARK, in stream order on the thread that processes the chain (so the draw's
// state flush and GS register writes before it have run), the marked bytes - the chain's copy of them - are drawn from
// a recording and skipped: the first time a block is seen it is processed as the packet path would (the VIF
// interpreter) while its UNPACK results (and the fields each wrote), MSCALs, DIRECT GS data and meshes are recorded;
// later frames copy the recorded fields into VU memory, resend the GS data and run the MSCALs through the runtime's
// own MSCAL (the native world handler draws world programs with the recorded meshes). No VIF parsing, unpacking, mesh
// hashing or vertex conversion for a recorded block. A recording is used only while the block's bytes hash the same
// and the VIF mode / cycle match; a block that keeps changing goes through the packet path for a while.
//
// Step 4 (docs/FULLY_NATIVE.md): a world / model block whose guest pages are unchanged (runtime/ps2_write_watch.h) since
// a copy the worker confirmed its recording for is not copied at all - the walker writes the MARK alone and the worker
// replays the recording by key (or rebuilds the block from guest RAM, unchanged by construction). Textures are named by
// raster here too (onDirect: where each upload's pixels are in the chain's copy). VCS_RSL_SKIP=0: copy every block;
// VCS_RSL_SKIP_VERIFY=1: decide, copy anyway, compare; VCS_RSL_RASTERS=0: no raster names.
//
// VCS_RSL_WORLD=0 / VCS_RSL_MODELS=0: world blocks / CALLed packets through the packet path. VCS_RSL_WORLD_VERIFY=1:
// every hit is processed by the packet path instead and recorded again, and the new recording compared with the
// cached one. VCS_RSL_WORLD_LOG=1: counts every 10 s.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::rslworld
{
    void install(PS2Runtime &runtime); // main thread, before runtime.run()
    void onFrameKick();                // game thread, at the frame kick: the frame's world draws are complete
}
