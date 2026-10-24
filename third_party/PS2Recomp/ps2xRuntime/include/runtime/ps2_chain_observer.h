// [gta-recomp] Observers of the VIF1 DMA chains (game code: docs/RSL_SUBMISSION.md, src/debug/rsl_shadow.cpp,
// src/game/vcs_rsl_world.cpp).
// - Tags: called on the thread that kicks a chain (it is copied then), once per tag, in chain order; chainStart on a
//   chain's first tag. dataAddr / qwc: the payload (0 when none); vifCodes: the tag's upper 64 bits (the two VIFcodes
//   sent with it); flatOffset: where this tag's bytes start in the copied chain - the stream processVIF1Data walks.
//   The return value may replace a REF's payload: 0 keeps it, kReplaceWithMark | imm copies one qword - VIF MARK imm
//   and three NOPs - in its place (the MARK handler then does the payload's work, in stream order).
// - Queued: the copied chain was queued for processing; key = its buffer's data pointer, the same pointer the
//   processing sees (the buffer is moved, not copied, to the render worker).
// - Processing: a copied chain's processing begins / ends on the calling thread (key as above).
// - Mark: the VIF1 interpreter ran a MARK on the processing thread (after setting the MARK register).
// - streamOffset(): on the processing thread, the offset in that stream of the VIFcode being run (set at MSCAL, so the
//   MSCAL hooks can tell which part of the chain started the program).
// Kept out of ps2_memory.h: the generated code includes that one.
#pragma once

#include <cstdint>

class PS2Memory;

namespace ps2chain
{
    constexpr uint32_t kReplaceWithMark = 0x80000000u;

    using TagObserver = uint32_t (*)(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                                     uint32_t flatOffset, bool chainStart);
    using QueuedObserver = void (*)(const void *key, uint32_t sizeBytes);
    using ProcessingObserver = void (*)(const void *key, bool begin);
    using MarkHandler = void (*)(PS2Memory &memory, uint32_t imm);

    void addTagObserver(TagObserver observer);
    void addChainObservers(QueuedObserver queued, ProcessingObserver processing);
    void setMarkHandler(MarkHandler handler); // nullptr = off

    uint32_t streamOffset();
    void setStreamOffset(uint32_t offset); // the VIF1 interpreter

    // Recording what a VIF stream does (a REF's block recorded once, then replayed: copy the UNPACKs' results, run the
    // MSCALs with PS2Memory::vu1Mscal): while set on a thread, the VIF1 interpreter reports each UNPACK after it wrote VU
    // memory (qwords VU addresses vuAddr .. + qwords, wrapping at 1024; topRelative: the address had TOPS added), each
    // MSCAL before it runs, and every other VIFcode (the recorder decides which ones a replay can do without).
    class VifRecorder
    {
    public:
        virtual ~VifRecorder() = default;
        virtual void unpack(uint32_t vuAddr, uint32_t qwords, bool topRelative, uint32_t tops, uint32_t cycle, bool masked,
                            uint32_t mode, const uint8_t *vuData) = 0;
        virtual void mscal(uint32_t startPc) = 0;
        virtual void other(uint32_t vifcode) = 0;
    };
    void setVifRecorder(VifRecorder *recorder); // this thread
    VifRecorder *vifRecorder();

    // runtime side
    bool tagObserved();
    uint32_t notifyTag(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                       uint32_t flatOffset, bool chainStart);
    void notifyQueued(const void *key, uint32_t sizeBytes);
    void notifyProcessing(const void *key, bool begin);
    void notifyMark(PS2Memory &memory, uint32_t imm);
}
