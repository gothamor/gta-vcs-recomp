// VCS frame rate (docs/NATIVE_PORT_ROADMAP.md milestone 4): the 30 fps limiter and the 60 fps option.
#pragma once

class PS2Runtime;

namespace vcs
{
    void installFrameRateHooks(PS2Runtime &runtime);
    // 60 fps: the game's limiter lets a frame through after one VBlank instead of two (any thread)
    void setFrameRate60(bool on);
    bool frameRate60();
}
