// [gta-recomp] Test runs and remote control.
//
// PS2X_HIDDEN=1 ignores the physical controller and mutes the sound output unless PS2X_AUDIO=1, so automated tests
// neither disturb the person playing nor pick up their input. PS2X_INPUT=injected only ignores the controller.
//
// PS2X_CONTROL=1 (implied by PS2X_HIDDEN) listens on 127.0.0.1:PS2X_CONTROL_PORT (default 47615) for one-line text
// commands, answered with one line ("ok ..." / "error ..."):
//   ping                      -> ok
//   screenshot <path.png>     -> ok <w>x<h>     the presented picture incl. the perf overlay, rendered off-screen
//   button <name> down|up     -> ok             hold / release a DualShock 2 button (cross, circle, square, triangle,
//                                               l1, r1, l2, r2, l3, r3, start, select, up, down, left, right)
//   press <name> [ms=150]     -> ok             press and release; answers after the release
//   perf                      -> ok <summary>   the perf overlay's numbers for the last second
//   quit                      -> ok             stop the game
// tools/gamectl.py is the client. (On the PS5 the environment is not settable yet: docs/PS5.md's work list.)
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps2control
{
    bool hiddenMode();        // PS2X_HIDDEN
    bool physicalInputOn();   // false when hidden, PS2X_INPUT=injected or the game's input is blocked
    // Main thread: an in-game UI (the settings menu, the trainer) has the controller; the game sees none of it.
    void setGameInputBlocked(bool blocked);
    bool gameInputBlocked();  // [gta-recomp] the pad then reads no buttons at all, injected ones included

    // The DualShock 2 buttons held through the control channel, as the pad report's 16-bit word with set bits for
    // buttons down (any thread).
    uint16_t injectedButtons();

    // Main thread. start() after the platform layer; pump() once per frame after drawing - it runs the queued
    // commands; `capture` renders the current picture into a PNG and returns "<w>x<h>" or "" on failure.
    void start();

    // [gta-recomp] More commands, from the game layer (src/debug/rig.cpp). Register before start(). The handler
    // runs on the main thread with the rest of the line; it answers through `reply` ("ok ..." / "error ..."),
    // at once or later from any thread (e.g. after the game thread did the work). One answer per command.
    using Reply = std::function<void(const std::string &answer)>;
    using CommandHandler = std::function<void(const std::string &args, Reply reply)>;
    void addCommand(const std::string &verb, CommandHandler handler);
    void pump(const std::function<std::string(const std::string &path)> &capture, const std::function<void()> &quit);
    void stop();
}
