// Free aim (docs/CONTROLS.md "Free aim", game-internals.md 12c): with Keyboard > Free aim (or Gamepad > Free aim) on,
// holding the target button with a gun aims where the mouse / right stick points instead of locking on to a ped.
//
// It is the game's own free aim (R1, then L3: a lock-on to an invisible dummy object that the left stick moves, the
// white circle drawn on it), driven by the host: the pad read enters it (R1 + L3 in one report), the mouse turns the
// dummy around Vic and the camera with it, the world update runs with the free-aim bit cleared so Vic moves as he does
// locked on with that weapon (the game's free aim makes him stand still), the camera and HUD run with it set (the free-aim
// camera and its circle), and the player's shots test their line from the camera through the circle (so they hit what
// is under it, not what the gun's own line would). VCS_FREEAIM=0 installs none of it (the reference).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::aim
{
    // Before runtime.run(): the hooks (applyVcs).
    void install(PS2Runtime &runtime);

    // What the pad read hands over, once per game frame (game thread).
    struct Input
    {
        bool want = false;        // the target button is held (or hip fire) and free aim is on for that device
        float turn = 0, tilt = 0; // radians this frame: left +, up + (the mouse)
        float turnRate = 0, tiltRate = 0; // -1..1: left +, up + (the right stick)
    };
    // What the pad report gets.
    struct Output
    {
        bool own = false;   // free aim has the target button: R1 is `r1` (not the bindings'), the right stick is centred
        bool r1 = false, l3 = false;
    };
    Output update(const Input &in);

    // The player's current weapon is one free aim works with (the guns: not melee, thrown, scopes, the camera).
    bool gunInHand();
    // Free aim is on (the game's free-aim lock is held), as of the last pad read.
    bool aiming();
}
