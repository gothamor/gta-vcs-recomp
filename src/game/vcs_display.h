// VCS display settings applied to the game: Hor+ widescreen through the camera's view window (see vcs_display.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // r = display aspect / (4:3): 1 = the original view, 4/3 = 16:9 Hor+. Any thread; applies at the next view-window set.
    void setAspectWidening(float r);
    float aspectWidening();

    // Game thread, before CCamera::IsSphereVisible (FUN_003816F8) runs on this camera: widens its side planes to the
    // Hor+ view. Called by the display hooks' wrapper, and by any other wrapper of that function.
    void prepareSphereTest(uint8_t *rdram, uint32_t camera);

    // At start-up (applyVcs): wraps the view-window setter and the sphere test.
    void installDisplayHooks(PS2Runtime &runtime);

    // Game thread, once per game frame (testing: VCS_WIDESCREEN forces the game's own preference).
    void applyDisplay(uint8_t *rdram);
}
