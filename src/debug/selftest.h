#pragma once

class PS2Runtime;

namespace vcs
{
    // VCS_SELFTEST=1: checks recompiled helper functions against native results.
    bool runSelfTest(PS2Runtime &runtime);
}
