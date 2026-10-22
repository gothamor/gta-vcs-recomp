// [gta-recomp] Packet-level HLE groundwork (docs/NATIVE_RENDERER.md, stage 4): what the VIF1 stream does, per
// VU1 program and per model packet.
//
// The VIF1 DMA chain is flattened before the VIF interprets it, so the chain walker records each CALLed packet
// (a model in VCS's model heap) as a segment of the flattened stream. The VIF interpreter reports UNPACKs and
// MSCALs with the segment they belong to; the MSCAL callback is timed and the GS vertices it produced counted.
//
// PS2X_HLE_SURVEY=1 prints a report every 10 s to stderr and writes it to hle_survey.txt (working directory).
// PS2X_HLE_SURVEY_DUMP=n also writes up to n samples per (program, layout) to hle_samples/: the packet's VIF bytes,
// VU1 data memory before and after the program, and VU1 code memory. PS2X_HLE_SURVEY_DUMP_EVERY=n samples every
// n-th call of a layout instead of the first ones (which are boot screens).
#pragma once

#include <cstdint>
#include <vector>

class GS;

namespace ps2hle
{
    // The GS frontend, for handlers that draw natively (GS::submitNative). Set by the runtime.
    void setGs(GS *gs);
    GS *gs();

    // A CALLed packet in a flattened VIF1 chain: bytes [begin, end) of the stream, entered at guest `target`.
    struct ChainSegment
    {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t target = 0;
    };

    bool surveyEnabled();

    // GS vertices recorded by the HW renderer so far (game thread).
    extern uint64_t g_gsVerticesRecorded;

    // VIF1 interpreter (game thread). `segment` is null outside CALLed packets.
    void onUnpack(const ChainSegment *segment, uint8_t opcode, uint8_t num, uint16_t imm);
    void beforeMscal(const ChainSegment *segment, uint32_t startPc, const uint8_t *vuCode, const uint8_t *vuData,
                     const uint8_t *stream, uint32_t streamSize);
    void afterMscal(const uint8_t *vuData);
    void onChainEnd(); // end of one flattened chain (one frame's worth for VCS)

    // A game's hooks around VU1 programs started by MSCAL (game thread): `before` may handle the program itself and
    // return true (the VU1 run is then skipped); `after` runs after a VU1 run that was not skipped. Checkers use the
    // pair to compare their own result with the VU1's.
    struct MscalContext
    {
        uint32_t startPc = 0;
        uint32_t top = 0;  // TOP / ITOP the program sees
        uint32_t itop = 0;
        const uint8_t *vuCode = nullptr;
        uint8_t *vuData = nullptr;
        uint64_t codeGeneration = 0;          // changes whenever VU1 code memory does (cache code checks by it)
        const ChainSegment *segment = nullptr; // only with PS2X_HLE_SURVEY
    };
    using MscalBefore = bool (*)(const MscalContext &);
    using MscalAfter = void (*)(const MscalContext &);
    void setMscalHooks(MscalBefore before, MscalAfter after);
    MscalBefore mscalBefore();
    MscalAfter mscalAfter();
}
