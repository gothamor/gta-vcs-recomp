// [gta-recomp] Minimal ps2_vu_code_log for the VU1 core ported from BigDaddyJRich/PS2Recomp (hctp).
// No write log is kept yet: writtenSince() answers "unknown", so the VU1 decode cache re-decodes the
// whole code memory after any upload (correct, just slower when a game re-uploads microcode often).
#include "runtime/ps2_vu_code_log.h"

namespace ps2_vu_code_log
{
    void record(const PS2Memory *, uint64_t, uint32_t, uint32_t)
    {
    }

    bool writtenSince(const PS2Memory *, uint64_t, uint64_t, uint32_t &begin, uint32_t &end)
    {
        begin = end = 0u;
        return false;
    }
}

// hctp's VIF trace dump hook (debug only).
void hctpDumpVifTrace(const char *)
{
}
