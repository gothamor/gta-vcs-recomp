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
    std::vector<uint8_t> takeBuffer();
    void recycleBuffer(std::vector<uint8_t> &&buffer);

    // Runtime shutdown: runs what is queued, then ends the thread.
    void shutdown();
}
