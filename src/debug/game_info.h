// The game-state panel of the F3 overlay (under the perf numbers): the player's position, heading, state, health,
// the vehicle, the camera, the world clock / area / pauses, the pools and the running scripts - read from the game's
// memory at the frame kick (addresses: docs/game-internals.md 4, 11, 12). The same lines answer the control channel's
// `gameinfo` command (`python tools/gamectl.py gameinfo`), for scripted runs and bug hunts.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::gameinfo
{
    // Main thread, before runtime.run(): the overlay panel and the `gameinfo` command.
    void install(PS2Runtime &runtime);
    // Game thread, at the frame kick: takes the snapshot the panel shows (only while the panel is on screen).
    void onGameFrame(const uint8_t *rdram);
}
