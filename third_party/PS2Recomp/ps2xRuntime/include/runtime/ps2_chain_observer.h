// [gta-recomp] Observers of the VIF1 DMA chains (game code: docs/RSL_SUBMISSION.md, src/debug/rsl_shadow.cpp).
// - Tags: called on the thread that kicks a chain (it is copied then), once per tag, in chain order; chainStart on a
//   chain's first tag. dataAddr / qwc: the payload (0 when none); vifCodes: the tag's upper 64 bits (the two VIFcodes
//   sent with it); flatOffset: where this tag's bytes start in the copied chain - the stream processVIF1Data walks.
// - Queued: the copied chain was queued for processing; key = its buffer's data pointer, the same pointer the
//   processing sees (the buffer is moved, not copied, to the render worker).
// - Processing: a copied chain's processing begins / ends on the calling thread (key as above).
// - streamOffset(): on the processing thread, the offset in that stream of the VIFcode being run (set at MSCAL, so the
//   MSCAL hooks can tell which part of the chain started the program).
// Kept out of ps2_memory.h: the generated code includes that one.
#pragma once

#include <cstdint>

namespace ps2chain
{
    using TagObserver = void (*)(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                                 uint32_t flatOffset, bool chainStart);
    using QueuedObserver = void (*)(const void *key, uint32_t sizeBytes);
    using ProcessingObserver = void (*)(const void *key, bool begin);

    void setTagObserver(TagObserver observer); // nullptr = off
    void setChainObservers(QueuedObserver queued, ProcessingObserver processing);

    uint32_t streamOffset();
    void setStreamOffset(uint32_t offset); // the VIF1 interpreter

    // runtime side
    void notifyQueued(const void *key, uint32_t sizeBytes);
    void notifyProcessing(const void *key, bool begin);
}
