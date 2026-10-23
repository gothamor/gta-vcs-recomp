// VCS frame rate and game timing (docs/NATIVE_PORT_ROADMAP.md milestone 4; docs/game-internals.md 4).
//
// The game paces itself on the VBlank: a VBlank-start handler (FUN_00370300) runs the registered VBlank callbacks,
// one of which (FUN_00317aa8 -> FUN_00317ac8) counts VBlanks in the graphics object at 0x520F90 (+0x4). At the end of
// a frame FUN_003179a0 kicks the display list, waits `do { WaitVBlank(); } while (vblanks < 2);` and flips
// (FUN_003706c8): 30 fps. CTimer::Update (FUN_002f4960) measures the frame in bus-clock ticks read from a kernel timer
// (FUN_00451c00, its only caller), so the time step follows the measured frame time - in emulated time.
//
// Original timing: as on the PS2; a cap of 60 or more makes the counting callback add 2 per VBlank, so the limiter
// lets a frame through after one (the 60 fps mode).
//
// Independent timing:
// - the tick count CTimer reads is the host clock (147.456 MHz units) - or exactly 1/N s per frame with
//   VCS_TIMESTEP=<N> (tests: behaviour per simulated second at any step, game unpaced);
// - CTimer's minimum step of 0.5 (1/100 s) is lifted to 0.05: above 100 fps the clamped step ran the game fast;
// - the limiter's VBlank wait paces on the host clock to the frame-rate cap instead, brings the emulated clock up to
//   real time (VBlank events, timers and the IOP follow it) and presents the frame (runtime/ps2_present.h).
#include "game/vcs_framerate.h"

#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kCountVBlank = 0x00317AC8u; // (graphics object): +0x4 += 1, from a VBlank callback
        constexpr uint32_t kTimerTicks = 0x00451C00u;  // kernel timer count (64-bit), read by CTimer::Update only
        constexpr uint32_t kTimerEnsure = 0x002F47C0u; // CTimer: create the kernel timer if needed (2 calls in Update)
        constexpr uint32_t kWaitVBlank = 0x003703B8u;
        constexpr uint32_t kAfterMinClamp = 0x002F4C04u; // return address of Update's 2nd kTimerEnsure call
        constexpr uint32_t kLimiterWait = 0x00317A50u;   // return address of the limiter's VBlank wait

        constexpr uint32_t kPrevTicks = 0x00488298u;  // CTimer: tick count of the last Update (64-bit)
        constexpr uint32_t kFrameTicks = 0x004CD564u; // CTimer: this frame's ticks
        constexpr uint32_t kTimeScale = 0x004CD168u;  // CTimer::ms_fTimeScale
        constexpr uint32_t kTimeStep = 0x004CD110u;   // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kTimeStepNonClipped = 0x004CD300u;

        constexpr uint64_t kBusHz = 147456000ull;     // the tick rate (CTimer: ms = ticks / 147456)
        constexpr double kEeCyclesPerNs = 0.294912;   // EE clock 294.912 MHz
        constexpr float kMinStep = 0.05f;             // independent timing's floor: 1000 fps

        std::atomic<int> g_timing{static_cast<int>(Timing::Original)};
        std::atomic<int> g_cap{30};
        PS2Runtime::RecompiledFunction g_origTicks = nullptr, g_origEnsure = nullptr, g_origWait = nullptr;
        uint64_t g_lastSleepNs = 0; // the limiter's last host wait (VCS_TIMING_LOG)

        bool independent() { return g_timing.load(std::memory_order_relaxed) == static_cast<int>(Timing::Independent); }

        uint64_t hostNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        uint32_t forcedStepFps()
        {
            static const uint32_t fps = [] {
                const char *v = std::getenv("VCS_TIMESTEP");
                const int n = v ? std::atoi(v) : 0;
                if (n > 0)
                    std::fprintf(stderr, "[framerate] forced time step: 1/%d s per frame, unpaced\n", n);
                return static_cast<uint32_t>(n > 0 ? n : 0);
            }();
            return fps;
        }

        uint32_t read32(uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        void write32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & 0x01FFFFFFu), &v, 4); }
        float readF(uint8_t *rdram, uint32_t address)
        {
            float f;
            std::memcpy(&f, rdram + (address & 0x01FFFFFFu), 4);
            return f;
        }
        void writeF(uint8_t *rdram, uint32_t address, float f) { std::memcpy(rdram + (address & 0x01FFFFFFu), &f, 4); }

        // replaces FUN_00317ac8 (a leaf: lw / addiu / sw / jr)
        void countVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t at = GPR_U32(ctx, 4) + 4u;
            const bool sixty = !independent() && g_cap.load(std::memory_order_relaxed) != 30;
            write32(rdram, at, read32(rdram, at) + (sixty ? 2u : 1u));
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_00451c00 (independent timing): the tick count from the host clock, continuing CTimer's last count
        void timerTicks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!independent())
            {
                g_origTicks(rdram, ctx, runtime);
                return;
            }
            static bool started = false;
            static uint64_t base = 0, originNs = 0, calls = 0;
            if (!started)
            {
                std::memcpy(&base, rdram + kPrevTicks, 8);
                originNs = hostNs();
                started = true;
            }
            uint64_t elapsed; // ticks since the start of independent timing
            if (const uint32_t fps = forcedStepFps())
                elapsed = ++calls * kBusHz / fps;
            else
            {
                const uint64_t ns = hostNs() - originNs;
                elapsed = (ns / 1000000000ull) * kBusHz + (ns % 1000000000ull) * kBusHz / 1000000000ull;
            }
            // Whole milliseconds, the rest carried to the next frame: CTimer adds each frame's milliseconds truncated
            // to its clock (m_snTimeInMilliseconds), which lost up to 1 ms a frame - 1.5% slow at 30 fps, ~5% at 100
            // (game-internals.md 4). Whole-ms deltas convert back exactly (checked for 1-399 ms).
            constexpr uint64_t kTicksPerMs = kBusHz / 1000u;
            SET_GPR_U64(ctx, 2, base + elapsed / kTicksPerMs * kTicksPerMs);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_002f47c0; on Update's second call (independent timing) its minimum step is lifted first: the step was
        // just computed and raised to 0.5 when below; recompute it from the frame's ticks as Update does
        // (ticks * scale * 50/147456000) and put it back, floored at kMinStep. The maximum clamp follows the call.
        void timerEnsure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (independent() && GPR_U32(ctx, 31) == kAfterMinClamp && readF(rdram, kTimeStep) == 0.5f)
            {
                const int32_t ticks = static_cast<int32_t>(read32(rdram, kFrameTicks));
                const float t = ticks >= 0 ? static_cast<float>(ticks) : static_cast<float>(static_cast<uint32_t>(ticks)); // Update's cvt / unsigned path
                uint32_t kBits = 0x34B60B61u; // 50 / 147456000 as the game has it
                float k;
                std::memcpy(&k, &kBits, 4);
                const float step = t * readF(rdram, kTimeScale) * k;
                if (step < 0.5f)
                {
                    const float lifted = std::max(step, kMinStep);
                    writeF(rdram, kTimeStep, lifted);
                    writeF(rdram, kTimeStepNonClipped, lifted);
                }
            }
            g_origEnsure(rdram, ctx, runtime);
        }

        // The end of a game frame in independent timing (the limiter's VBlank wait): present it, wait for the cap's
        // deadline on the host clock, bring the emulated clock up to real time, and end the limiter's wait.
        void limiterWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2PresentGameFrame(); // the frame's display list was kicked just before

            static uint64_t deadline = 0;
            const int cap = g_cap.load(std::memory_order_relaxed);
            const double fps = cap == kCapRefresh ? platform::displayRefreshHz() : cap;
            if (fps > 0.0 && !forcedStepFps())
            {
                const uint64_t period = static_cast<uint64_t>(1e9 / fps);
                const uint64_t now = hostNs();
                deadline = deadline && deadline + period > now ? deadline + period : now; // behind: no catching up
                g_lastSleepNs = deadline > now ? deadline - now : 0u;
                if (deadline > now)
                {
                    ps2perf::PhaseScope waitTime(ps2perf::Phase::Wait); // the perf line's wait_ms, not game_ms
                    platform::delayPrecise(deadline - now);
                }
            }

            // emulated time follows real time (it only advances as guest code runs otherwise): up to where real time is
            // on the VBlank chain - never past it, or the EE is held at every VBlank (runtime/ee_scheduler.h)
            static uint64_t lastNs = 0, lastCycle = 0;
            EeScheduler &scheduler = runtime->eeScheduler();
            const uint64_t now = hostNs();
            scheduler.syncToRealTime(static_cast<uint64_t>(200e6 * kEeCyclesPerNs)); // a longer hitch is forgiven
            { // VCS_TIMING_LOG=1: frames, sleep, emulated-clock top-up, every 2 s
                static const bool log = std::getenv("VCS_TIMING_LOG") != nullptr;
                static uint64_t logStart = 0, frames = 0, toppedCycles = 0;
                if (log)
                {
                    ++frames;
                    if (!logStart)
                        logStart = now;
                    if (now - logStart > 2000000000ull)
                    {
                        std::fprintf(stderr, "[timing] %.1f fps, ee clock %.3f of real time, last frame slept %.1f ms\n",
                                     frames / ((now - logStart) / 1e9), toppedCycles / ((now - logStart) * kEeCyclesPerNs),
                                     g_lastSleepNs / 1e6);
                        logStart = now;
                        frames = 0;
                        toppedCycles = 0;
                    }
                    toppedCycles += lastNs ? scheduler.fastClock().cycle - lastCycle : 0u;
                }
            }
            lastNs = now;
            lastCycle = scheduler.fastClock().cycle;

            write32(rdram, GPR_U32(ctx, 18) + 4u, 2u); // the limiter's VBlank count ($s2 = the graphics object): done
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_003703b8 = WaitVBlank
        void waitVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            { // VCS_TIMING_LOG=1: WaitVBlank calls by return address, every 2 s
                static const bool log = std::getenv("VCS_TIMING_LOG") != nullptr;
                static std::unordered_map<uint32_t, uint32_t> byCaller;
                static uint64_t since = 0;
                if (log)
                {
                    ++byCaller[GPR_U32(ctx, 31)];
                    const uint64_t now = hostNs();
                    if (!since)
                        since = now;
                    if (now - since > 2000000000ull)
                    {
                        std::fprintf(stderr, "[timing] WaitVBlank callers:");
                        for (const auto &[ra, n] : byCaller)
                            std::fprintf(stderr, " %08x x%u", ra, n);
                        std::fprintf(stderr, "\n");
                        byCaller.clear();
                        since = now;
                    }
                }
            }
            if (independent() && GPR_U32(ctx, 31) == kLimiterWait)
            {
                limiterWait(rdram, ctx, runtime);
                return;
            }
            g_origWait(rdram, ctx, runtime);
        }

        void applyPresentRate()
        {
            const int cap = g_cap.load(std::memory_order_relaxed);
            double hz = 60.0;
            if (independent())
                hz = cap == kCapRefresh || cap == kCapUnlimited ? platform::displayRefreshHz() : std::max(60.0, double(cap));
            platform::setFrameRate(hz);
        }
    }

    void setTiming(Timing timing)
    {
        g_timing.store(static_cast<int>(timing), std::memory_order_relaxed);
        ps2SetPresentOnGameFrames(timing == Timing::Independent);
        applyPresentRate();
    }

    Timing timing() { return static_cast<Timing>(g_timing.load(std::memory_order_relaxed)); }

    void setFrameCap(int fps)
    {
        g_cap.store(fps, std::memory_order_relaxed);
        applyPresentRate();
    }

    int frameCap() { return g_cap.load(std::memory_order_relaxed); }

    void setFrameRate60(bool on) { setFrameCap(on ? 60 : 30); }
    bool frameRate60() { return frameCap() != 30; }

    void installFrameRateHooks(PS2Runtime &runtime)
    {
        g_origTicks = runtime.lookupFunction(kTimerTicks);
        g_origEnsure = runtime.lookupFunction(kTimerEnsure);
        g_origWait = runtime.lookupFunction(kWaitVBlank);
        runtime.replaceFunction(kCountVBlank, &countVBlank);
        if (!g_origTicks || !g_origEnsure || !g_origWait)
        {
            std::fprintf(stderr, "[framerate] CTimer / WaitVBlank not found: independent timing unavailable\n");
            g_timing.store(static_cast<int>(Timing::Original), std::memory_order_relaxed);
            return;
        }
        runtime.replaceFunction(kTimerTicks, &timerTicks);
        runtime.replaceFunction(kTimerEnsure, &timerEnsure);
        runtime.replaceFunction(kWaitVBlank, &waitVBlank);
    }
}
