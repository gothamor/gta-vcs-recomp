// VCS_SELFTEST=1: before booting, run a few recompiled libgcc helpers on known
// inputs and compare with native results. Catches recompiler/instruction bugs in
// code every other function depends on.
#include "selftest.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        using GuestFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

        uint64_t call2(PS2Runtime &runtime, GuestFn fn, uint32_t pc, uint64_t a, uint64_t b)
        {
            uint8_t *rdram = runtime.memory().getRDRAM();
            R5900Context storage{};
            R5900Context *ctx = &storage;
            SET_GPR_U64(ctx, 4, a);
            SET_GPR_U64(ctx, 5, b);
            SET_GPR_U32(ctx, 29, 0x01F00000u); // scratch stack, below the game's
            SET_GPR_U32(ctx, 31, 0u);
            ctx->pc = pc;
            fn(rdram, ctx, &runtime);
            return GPR_U64(ctx, 2);
        }
    }

    bool runSelfTest(PS2Runtime &runtime)
    {
        if (!std::getenv("VCS_SELFTEST"))
            return true;
        struct Case
        {
            uint64_t a, b;
        };
        const Case cases[] = {
            {100, 7}, {65521ull * 3 + 5, 65521}, {0xFFFFFFFFull, 65521}, {0x123456789ABCDEFull, 65521},
            {0xFFFFFFFFFFFFFFFFull, 3}, {0x100000000ull, 0x10001}, {0x8000000000000000ull, 0xFFFFFFFFull},
            {12345678901234ull, 987654321ull},
        };
        int failures = 0;
        for (const Case &c : cases)
        {
            const uint64_t div = call2(runtime, FUN_00443c30_0x443c30, 0x00443C30u, c.a, c.b);
            const uint64_t mod = call2(runtime, FUN_00444200_0x444200, 0x00444200u, c.a, c.b);
            const uint64_t mul = call2(runtime, FUN_00443bc8_0x443bc8, 0x00443BC8u, c.a, c.b);
            const bool ok = div == c.a / c.b && mod == c.a % c.b && mul == c.a * c.b;
            failures += ok ? 0 : 1;
            std::fprintf(stderr, "[selftest] %s %llx,%llx: udiv %llx (%llx) umod %llx (%llx) mul %llx (%llx)\n",
                         ok ? "ok  " : "FAIL", (unsigned long long)c.a, (unsigned long long)c.b,
                         (unsigned long long)div, (unsigned long long)(c.a / c.b),
                         (unsigned long long)mod, (unsigned long long)(c.a % c.b),
                         (unsigned long long)mul, (unsigned long long)(c.a * c.b));
        }
        std::fprintf(stderr, "[selftest] %d failure(s)\n", failures);
        return failures == 0;
    }
}
