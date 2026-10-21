// Write watchpoint backend for ps2TraceGuestWrite (third_party ps2_runtime.h).
//   VCS_WATCH=<lo>,<hi>[,<value>]   hex guest addresses, optional value to match
//   VCS_WATCH_MAX=<n>               stop logging after n hits (default 50)
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

void ps2WriteWatchHit(uint32_t guestAddr, uint32_t size, uint64_t value, const char *op, const R5900Context *ctx)
{
    static std::atomic<int> hits{0};
    static const int maxHits = std::getenv("VCS_WATCH_MAX") ? std::atoi(std::getenv("VCS_WATCH_MAX")) : 50;
    const int n = hits.fetch_add(1);
    if (n >= maxHits)
        return;
    std::fprintf(stderr, "[watch] %s addr=0x%08x size=%u value=0x%llx pc=0x%08x ra=0x%08x sp=0x%08x\n", op, guestAddr, size,
                 static_cast<unsigned long long>(value), ctx ? ctx->pc : 0u, ctx ? GPR_U32(ctx, 31) : 0u,
                 ctx ? GPR_U32(ctx, 29) : 0u);
}

namespace vcs
{
    void configureWatch()
    {
        const char *env = std::getenv("VCS_WATCH");
        if (!env)
            return;
        unsigned long long lo = 0, hi = 0, value = 0;
        const int n = std::sscanf(env, "%llx,%llx,%llx", &lo, &hi, &value);
        if (n < 2 || hi <= lo)
            return;
        g_ps2WriteWatch.lo = static_cast<uint32_t>(lo);
        g_ps2WriteWatch.span = static_cast<uint32_t>(hi - lo);
        g_ps2WriteWatch.matchValue = n >= 3;
        g_ps2WriteWatch.value = value;
        std::fprintf(stderr, "[watch] armed 0x%08llx..0x%08llx%s\n", lo, hi, n >= 3 ? " (value match)" : "");
    }
}
