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
// Step 4c, draw items: a world / model draw call whose state flush is native (game/vcs_rsl_native.h) replaces the draw
// function and writes no packets for the draw: its flush (when all CNTs), its GS registers and its block become an item,
// and the packet buffer gets one 16-byte CNT whose VIFcodes - MARK (0x8000 + index), NOP (0xD4 | frame) - keep its place
// in the chain. At the kick the item's block is skipped or copied from guest RAM as the walker would; at the MARK the
// worker runs the item. Draws stay packets while FUN_00240ED8 caches a sector's packets (it copies them and sends the
// copy again in later frames), when the flush calls plug-in callbacks, and for a model block not yet seen copied by the
// walker. VCS_RSL_ITEMS=0: the draws' packets as before; VCS_RSL_ITEMS_VERIFY=1: the original writes the packets, which
// are compared with what each item stands for, and each block the walker copies with what an item would have copied.
//
// Step 5a, state ops: the GS queue senders (FUN_002F7908 / FUN_002F7B30), the texture matrix (FUN_002F7090),
// FUN_001CEF18, FUN_002F9CE8 and all-CNT native flushes (flushAsOp) put their VIF bytes into the item stream instead of
// the packet buffer - several in a row share one item and tag. Not while a sector or a material's setup is being cached
// (FUN_002409C8 copies the latter too). VCS_RSL_OPS=0: their packets.
//
// Step 5b, texture uploads: FUN_003298D8's two REFs (the destination's transfer header, the pixels) become reference
// ops - their payloads copied from guest RAM at the kick into the item's bytes, raster names noted - when the header
// exists already. VCS_RSL_UPLOADS=0: their REFs. VCS_RSL_SLOW_LOG=1: slow kicks / chains.
//
// Step 5c, Im2D: FUN_002FB690's packet, as the game writes it, becomes an op at the next hook point (settlePending) when
// nothing else was written after it - the cursor rewound to its start. VCS_RSL_IM2D=0: the packets as written.
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
    // step 5a: a native state flush's bytes (DMA CNTs with their data) as a state op in chain order; false: the caller
    // writes them into the packet buffer (ops off, a cache being filled, no room)
    bool flushAsOp(uint8_t *rdram, const uint8_t *tags, uint32_t bytes);
    // step 5c: an Im2D packet written since the last hook point becomes an op now (before the caller writes packets)
    void settlePending(uint8_t *rdram);
}
