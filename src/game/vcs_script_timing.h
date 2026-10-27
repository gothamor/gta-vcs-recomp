// Mission scripts that count per frame, put on the game's time step (docs/game-internals.md 11f).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Hooks the script VM's add / subtract / multiply / set handlers: at the instructions listed in
    // vcs_script_timing.cpp (meters a mission script changes by a fixed amount every frame, e.g. PHI_A2's heat) the
    // amount is scaled by the frame's time step over the PS2's 1/30 s, so the meter moves as fast per second at any
    // frame rate. Only while the frame isn't the PS2's (independent timing, or original timing at 60); original
    // timing at 30 runs the game's own handlers. VCS_SCRIPT_TIMESTEP=0 turns it off.
    void installScriptTimingHooks(PS2Runtime &runtime);
}
