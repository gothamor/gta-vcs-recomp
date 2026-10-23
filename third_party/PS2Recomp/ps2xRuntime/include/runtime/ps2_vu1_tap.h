// [gta-recomp] A copy of every VU1 XGKICK packet for checkers (game thread). Set g_kicks to a vector to collect the
// packets the VU1 sends to the GS (they still go to the GS); null (the default) costs one branch per kick.
// Used by src/game/vcs_hle_world.cpp (PS2X_HLE_VERIFY) to compare its clipper with the VU1's.
#pragma once

#include <cstdint>
#include <vector>

namespace ps2vu1tap
{
    extern std::vector<std::vector<uint8_t>> *g_kicks;
}
