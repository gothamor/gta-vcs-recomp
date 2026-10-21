// Debug aid: sampling profiler for the recompiled game.
//
// VCS_PROFILE=<seconds between reports> samples the "GameThread" host thread's
// instruction pointer ~2000x per second and maps it back to the guest function
// whose recompiled body contains it (nearest lower host function pointer in the
// generated function table). Prints the hottest guest functions every report.
// Works without debug symbols. Windows only.
#pragma once

class PS2Runtime;

namespace vcs
{
    void startProfiler(PS2Runtime &runtime);
}
