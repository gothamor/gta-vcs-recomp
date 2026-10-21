// Debug aid: with VCS_WATCHDOG=<seconds> set, prints the EE thread states
// (pc/ra/status/wait) periodically so hangs show where every thread is.
#pragma once

class PS2Runtime;

namespace vcs
{
    void startWatchdog(PS2Runtime &runtime);
}
