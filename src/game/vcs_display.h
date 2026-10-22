// VCS display settings applied to the game: Hor+ widescreen through the camera's view window (see vcs_display.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // r = display aspect / (4:3): 1 = the original view, 4/3 = 16:9 Hor+. Any thread; applies at the next view-window set.
    void setAspectWidening(float r);
    float aspectWidening();

    // At start-up (applyVcs): wraps the view-window setter.
    void installDisplayHooks(PS2Runtime &runtime);

    // Game thread, once per game frame (testing: VCS_WIDESCREEN forces the game's own preference).
    void applyDisplay(uint8_t *rdram);
}
