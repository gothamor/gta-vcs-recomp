// [gta-recomp] Which 4 KB pages of guest RAM were written, without reading them (docs/FULLY_NATIVE.md step 4): fold()
// takes the pages written since the last fold and bumps their generation; a page whose generation is unchanged holds the
// bytes it held then. The PS5 has no OS write watch (ps2_write_watch.cpp): enabled() is false and every generation 0,
// so nothing is told clean and the blocks are copied as before. Kept out of ps2_memory.h: the generated code includes
// that one.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2writewatch
{
    constexpr uint32_t kPageShift = 12u;
    constexpr uint32_t kPageSize = 1u << kPageShift;

    uint8_t *allocate(size_t bytes); // guest RAM (zeroed); new[] when write watch is off or unavailable
    void release(uint8_t *ram);

    bool enabled();
    // the pages written since the last fold get a new generation; returns how many. One thread at a time (the one that
    // kicks the frame's chain).
    uint32_t fold();
    uint32_t generation(uint32_t page);                       // page = guest RAM offset >> kPageShift
    bool unchangedSince(uint32_t page, uint32_t generation);  // false when write watch is off
}
