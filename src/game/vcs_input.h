// The player's controls (docs/CONTROLS.md): the DualSense's button map, stick options, free aim and vibration - all
// turned into the one DualShock 2 the game reads.
#pragma once

#include <cstdint>

namespace vcs::input
{
    // The settings of the Controller page (settings/settings.h).
    void registerSettings();

    // Feeds the game's pad reads from the bindings (PSPadBackend::setReader) and its motor levels to the DualSense's
    // vibration (setActuatorSink). VCS_INPUT_MAP=0 keeps the runtime's built-in fixed mapping (the reference). `rdram`:
    // the game's memory (its frame counter: free aim, game/vcs_aim.h, is worked out once per game frame); null in the
    // controller test.
    void install(uint8_t *rdram);

    // Stops any vibration now.
    void stopVibration();

    // The free camera's controls (game/vcs_freecam.h; fixed buttons, docs/CONTROLS.md), taken once per camera update on
    // the game thread. While the free camera flies the game's pad reads give it nothing.
    struct FreeCamInput
    {
        float strafe = 0, forward = 0, rise = 0; // -1..1: right, ahead, up
        float turnRate = 0, tiltRate = 0;        // the right stick: -1..1 (left +, up +)
        float zoom = 0;                          // -1..1: narrower (+) or wider
        bool fast = false, slow = false;
    };
    FreeCamInput takeFreeCameraInput();
}
