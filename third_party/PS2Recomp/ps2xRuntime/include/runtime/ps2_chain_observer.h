// [gta-recomp] An observer of the VIF1 DMA chains as the runtime walks them (game code: docs/RSL_SUBMISSION.md,
// src/debug/rsl_shadow.cpp). Called on the thread that kicks the chain (it is copied then), once per tag, in chain
// order; chainStart on a chain's first tag. dataAddr / qwc: the payload (0 when none); vifCodes: the tag's upper 64
// bits (the two VIFcodes sent with it). Kept out of ps2_memory.h: the generated code includes that one.
#pragma once

#include <cstdint>

namespace ps2chain
{
    using TagObserver = void (*)(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                                 bool chainStart);

    void setTagObserver(TagObserver observer); // nullptr = off
}
