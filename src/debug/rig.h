// The automation rig (docs/NATIVE_PORT_ROADMAP.md milestone 1): control-channel commands that look into and drive
// the running game - memory peek / poke / search, and (built on those) teleport, world state and missions.
// Needs the control channel (PS2X_CONTROL=1, tools/run.py --control); tools/rig.py is the client.
//
// Commands that change the game run on the game thread at the next frame kick (a safe point between frames) and
// answer when done; reads answer at once from the main thread.
#pragma once

#include <cstdint>
#include <functional>

#include "ps2_guest_cc.h"

class PS2Runtime;
struct R5900Context;

namespace vcs::rig
{
    // Main thread, before runtime.run(): registers the commands.
    void install(PS2Runtime &runtime);

    // Game thread, once per game frame (the frame-kick hook): runs queued jobs, holds the game while paused.
    PS2_GUEST_CC void onGameFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // Queue work for the game thread's next frame kick.
    void runOnGameThread(std::function<void(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)> job);

    uint64_t gameFrames();
}
