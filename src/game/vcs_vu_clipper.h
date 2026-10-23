// The clipper path of VCS's prelit world pipeline (VU1 program 0x0030, stage table 0x19) in C++: the GIF packets the
// VU1 would XGKICK for a packet whose triangles cross the guard band. See docs/game-internals.md 6h.
#pragma once

#include <cstdint>
#include <vector>

namespace vcsclip
{
    // vu: VU1 data memory before the program runs; top: TOP. Appends one GIF packet per drawn polygon (tag + n x
    // STQ / RGBA / XYZF2, the fan PRIM from tag 49's upper lane) - not the empty sync kicks of qword 58. Returns
    // false when the packet needs something not reproduced (the backface pass, 52.y bit 0): run the VU1 then.
    // The caller sets FE_TOWARDZERO.
    bool clipWorld(const uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets);
}
