// VCS 2D at the RenderWare level (vcs_hud.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    void installHudHooks(PS2Runtime &runtime);
    void onFrameKick(); // the frame's display list goes out: the HUD context ends
    // Widescreen, HUD "Screen edges": each HUD element squeezed towards the screen edge it sits near (any thread)
    void setHudAnchoring(bool on);
    uint32_t hudDrawCount(); // CHud::Draw calls drawn so far (the trainer's Hide HUD skips them)
}
