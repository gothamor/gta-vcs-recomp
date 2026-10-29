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

    // Game thread, between frames: run `code` (script bytecode ending in TERMINATE_THIS_SCRIPT, at most 176 bytes; its
    // outputs go to locals 0..0x2F) as a new script of the game's script VM (game-internals.md 11d). Returns the
    // script's address, 0 when the engine is not running or no slot is free.
    uint32_t startCodeScript(uint8_t *ram, const uint8_t *code, size_t bytes, const char *name);
    // Game thread, between frames: a new script at `ip` in the game's own code (an IP into ScriptSpace). With
    // `returnIp`, as if GOSUBed from there: a subroutine's RETURN goes on at `returnIp` (e.g. a TERMINATE_THIS_SCRIPT).
    // Our code cannot be the return: a CALL keeps its return IP packed with the frame, and an IP outside ScriptSpace
    // (startCodeScript's) comes back as garbage (game-internals.md 11b).
    uint32_t startScriptAt(uint8_t *ram, uint32_t ip, const char *name, uint32_t returnIp = 0);
    // the script started by startCodeScript has not terminated yet: active, and its IP still in the slot's code (the
    // game reuses the slot once it has, and a save load fills the slots with the save's scripts)
    bool scriptActive(const uint8_t *ram, uint32_t script);
    // Game thread, between frames: take a script started by startCodeScript out of the active list unrun (back to the
    // idle list, as TERMINATE_THIS_SCRIPT would) - so it is not written into a save made while the game is paused.
    void cancelCodeScript(uint8_t *ram, uint32_t script);
}
