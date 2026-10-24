// [gta-recomp] The EE scheduler's clock, shared with the inline fast paths of PS2Runtime::dispatchGuestBranch /
// eeCheckpointDue (ps2_runtime.h), so that a guest call or a loop's back edge with nothing due costs a few
// instructions instead of out-of-line calls into the scheduler (~15% of the game thread). EE executor thread only.
#pragma once

#include <atomic>
#include <cstdint>

struct EeFastClock
{
    static constexpr uint32_t kDispatchCycles = 8u; // EeScheduler::kGuestDispatchCycles
    static constexpr uint64_t kFeedCycles = 512u;   // timers and the IOP get their cycles in batches of at least this

    uint64_t cycle = 0u;    // EE cycles accounted (the scheduler's clock)
    uint64_t sliceEnd = 0u; // the running thread's time slice ends here
    uint64_t fedCycle = 0u; // timers / IOP have been given the cycles up to here
    const std::atomic<bool> *pending = nullptr;      // a checkpoint is pending (events, stop, timer interrupts)
    const std::atomic<uint64_t> *deadline = nullptr; // next scheduled event's cycle (0: none)
    // [gta-recomp] Guest work is charged cycles >> chargeShift (dispatches, loop back-edges; not idle time or the
    // top-up to real time). 0 = as estimated (8 per call, 32 per loop iteration). EeScheduler::setGuestChargeShift.
    uint32_t chargeShift = 0u;

    // True: `cycles` passed with nothing due - counted, as EeScheduler::checkpointDue would. False: nothing changed;
    // the scheduler's checkpointDue decides (time slice end, an event, timers / IOP to feed).
    bool tryAdvance(uint32_t cycles) noexcept
    {
        const uint64_t next = cycle + ((cycles ? cycles : 1u) >> chargeShift);
        if (next >= sliceEnd || next - fedCycle >= kFeedCycles || pending->load(std::memory_order_acquire))
            return false;
        const uint64_t due = deadline->load(std::memory_order_acquire);
        if (due != 0u && next >= due)
            return false;
        cycle = next;
        return true;
    }
};
