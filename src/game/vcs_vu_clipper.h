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
    // The caller sets FE_TOWARDZERO. defaultColour: no colour stage runs (0x02c0's re-draws): every vertex has the clip
    // test's colour, qword 51.
    bool clipWorld(const uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets, bool defaultColour = false);

    // The clipper (0x1638) alone, from what the clip test, fog (0x11f8) and colour stage left in VU memory - the output
    // buffer (ST + w, RGBA, clip position + fog) and the flags in the positions' .w. For code images whose clip test
    // or colour stage differ from the prelit world's (the float images, game-internals.md 6d). tagQword: the GIF tag
    // the kick routine loaded into vf01 before jumping to the clipper - 49 (0x2b6) or 50 (0x2b2, a second pass).
    bool clipFromOutput(const uint8_t *vu, uint32_t top, std::vector<std::vector<uint8_t>> &packets, uint32_t tagQword = 49);
}
