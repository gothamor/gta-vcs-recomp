// VCS 2D at the RenderWare level (vcs_hud.cpp).
#pragma once

class PS2Runtime;

namespace vcs
{
    void installHudHooks(PS2Runtime &runtime);
    void onFrameKick(); // the frame's display list goes out: the HUD context ends
}
