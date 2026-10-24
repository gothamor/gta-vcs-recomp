// [gta-recomp] The render submission worker (PS2X_RENDER_THREAD).
//
// The VIF1 / GIF DMA chains a game kicks - VIF1 interpretation, VU1 microprograms, the GS front end and the hardware
// renderer's frame building - run on a thread of their own, in the order they were kicked, while the game thread
// carries on. The game sees the DMA complete at once (as before: the chain is copied when kicked). Whatever the game
// can observe of that work - GS CSR / SIGLBLID, GIF and VIF1 registers and FIFOs, VU1 memory - waits for the worker to
// finish what was posted first (fence), so it reads what the synchronous path would have shown.
//
// PS2X_RENDER_THREAD=0 / 1 overrides the default (on with the hardware renderer); PS2X_RENDER_THREAD_LOG=1 reports
// items, frames and fences every 5 s.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace ps2_render_worker
{
    enum class FenceReason : uint8_t
    {
        GsPrivileged, // GS CSR / SIGLBLID (privileged registers)
        GifRegister,  // GIF registers
        Vif1Register, // VIF1 registers
        Fifo,         // VIF1 / GIF FIFO
        Vu1Memory,    // VU1 code / data memory
        GsStub,       // an HLE GS library call touching the GS directly
        Other,
        Count
    };

    // Called once while the runtime starts: whether the worker is used when PS2X_RENDER_THREAD doesn't say.
    void configure(bool defaultOn);
    bool enabled();

    // Game thread: `work` runs on the worker after everything posted before it; at once when the worker is off.
    // frameEnd marks the end of a displayed frame: the game thread waits rather than get more than one such frame
    // ahead of the worker.
    void post(std::function<void()> work, bool frameEnd = false);

    // Until everything posted so far has run. Nothing to do when the worker is off or on the worker itself.
    void fence(FenceReason reason);

    bool onWorker();

    // Chain gather buffers, kept with their capacity, shared by the game thread (filling) and the worker (done with).
    // [gta-recomp] takeBuffer(want): the smallest pooled buffer holding `want` bytes, else the largest - the pool holds
    // both small GIF copies and multi-MB VIF1 chains, and a chain started in a small one reallocated its way up every
    // frame (~4% of the game thread while driving, docs/PS2RECOMP_PATCHES.md).
    std::vector<uint8_t> takeBuffer(size_t want = 0);
    void recycleBuffer(std::vector<uint8_t> &&buffer);

    // Runtime shutdown: runs what is queued, then ends the thread.
    void shutdown();

    // [gta-recomp] The GS stage (PS2X_GS_THREAD=0 / 1; on by default with the worker and the hardware renderer): the GS
    // front end and the hardware renderer's frame building run on a second thread behind the worker. The worker
    // interprets the VIF1 chains and runs the VU1 programs (and their native reproductions); what they hand the GS goes
    // to the GS stage as commands, data copied, in order (runtime/gs/gs_frontend.cpp). Fences wait for both stages.
    struct GsCommand
    {
        uint32_t type = 0;  // the GS front end's command (gs_frontend.cpp)
        uint32_t bytes = 0; // payload after the header
        uint64_t a = 0, b = 0;
        uint32_t gifPath = 0, producer = 0, uploadAddress = 0, uploadBytes = 0; // the worker's thread-locals when sent
        uint32_t csr = 0;                                                    // the worker's MXCSR
    };
    using GsHandler = void (*)(const GsCommand &command, const uint8_t *payload);

    void configureGsStage(bool defaultOn); // at start, after configure(); off when the worker is
    void setGsHandler(GsHandler handler);  // the GS front end, once
    // GS calls made on this thread go to the GS stage (the worker, with the stage on)
    bool gsStage();
    bool onGsStage();
    // Room for a command and `bytes` of payload (the worker only); fill it, then gsSend(). nullptr: too large for the
    // queue - the GS stage has been drained, call the GS directly.
    uint8_t *gsReserve(const GsCommand &command);
    void gsSend();
    // `work` on the GS stage in order with the commands, or at once without it; frameEnd: a displayed frame's end (the
    // worker waits rather than get more than one frame ahead of the GS stage)
    void runOnGsStage(std::function<void()> work, bool frameEnd = false);
    // Until the GS stage has run everything sent so far (any thread but the GS stage)
    void gsDrain();
}
