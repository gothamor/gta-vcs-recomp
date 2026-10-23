// [gta-recomp] ps2_vu_code_log for the VU1 core ported from BigDaddyJRich/PS2Recomp (hctp).
// A ring of the last kEntries writes, each stored under the generation it brought. writtenSince() needs every
// generation in (since, now] to be in the ring: a write that bumped the generation without recording (or one not yet
// recorded - the bump comes first) makes it answer "unknown", and the VU1 decode cache re-decodes everything.
#include "runtime/ps2_vu_code_log.h"

#include <algorithm>
#include <mutex>

namespace ps2_vu_code_log
{
    namespace
    {
        constexpr uint64_t kEntries = 64u;
        struct Entry
        {
            uint64_t generation = 0u; // 0: empty (generation 0 is the state before any write)
            uint32_t begin = 0u;
            uint32_t end = 0u;
        };
        std::mutex g_mutex;
        const PS2Memory *g_memory = nullptr;
        Entry g_ring[kEntries];
    }

    void record(const PS2Memory *memory, uint64_t generation, uint32_t begin, uint32_t end)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (memory != g_memory)
        {
            g_memory = memory;
            std::fill(std::begin(g_ring), std::end(g_ring), Entry{});
        }
        g_ring[generation % kEntries] = {generation, begin, end};
    }

    bool writtenSince(const PS2Memory *memory, uint64_t since, uint64_t now, uint32_t &begin, uint32_t &end)
    {
        begin = end = 0u;
        if (now < since || now - since > kEntries)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (memory != g_memory)
            return now == since;
        bool any = false;
        for (uint64_t generation = since + 1u; generation <= now; ++generation)
        {
            const Entry &entry = g_ring[generation % kEntries];
            if (entry.generation != generation)
                return false;
            if (entry.begin >= entry.end)
                continue;
            begin = any ? std::min(begin, entry.begin) : entry.begin;
            end = any ? std::max(end, entry.end) : entry.end;
            any = true;
        }
        return true;
    }
}

// hctp's VIF trace dump hook (debug only).
void hctpDumpVifTrace(const char *)
{
}
