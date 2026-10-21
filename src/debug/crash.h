#pragma once

namespace vcs
{
    // Print a reason and symbolized stack on uncaught exceptions, abort() and crashes.
    void installCrashHandlers();
}
