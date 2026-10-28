// Vehicle physics that acts a fixed amount every frame, put on the game's time step (docs/game-internals.md 4).
//
// The PS2 game ran at 30 fps, and some vehicle physics is applied once a frame without CTimer::ms_fTimeStep: at 144 fps
// it acted 4.8 times as often per second. Both cases here are scaled by the frame's step over the PS2's (50/30) when
// the frame is shorter, so they act as much per second as at 30 fps; longer frames keep the game's amount, as the PS2
// did when it dropped below 30. Only while the frame isn't the PS2's (independent timing, or original timing at 60).
//
// 1. The push from a body under a wheel. FUN_003F34F0 runs after each wheel's ProcessWheel, for cars (FUN_00152E18,
//    from CAutomobile::ProcessControl) and bikes (CBike::ProcessControl, FUN_00337208). When the player drives, the
//    vehicle moves and a ped near the driver is dead or knocked down within 1 m (horizontally) of the wheel's contact
//    point, it gives the vehicle an upward push at that wheel: ApplyTurnForce((0, 0, mass * 0.01 (* 1.5 for cars)),
//    contact point). A car's front wheels over a body pitched it up so fast at 144 fps that its rear hit the ground and
//    threw it up (3.3 m/s against 1.4 at 30 fps); a bike rolling past the rider it was stolen from stood on its nose.
//    The hook on ApplyTurnForce scales that one call's force (f14, return address 0x3F36D4).
// 2. Rolling resistance. CVehicle::ProcessWheel (FUN_003E6C18 cars, FUN_003E7618 bikes) brakes a wheel with neither
//    gas nor brake by 0.9 / mass (other constants for light vehicles, the RC Bandit and bikes) per frame: a car coasted
//    down at 16 m/s^2 at 144 fps instead of 3.4. Patched in game/vcs/overrides.toml to read those constants from guest
//    RAM 0x7FF00.. (kBrakeSlots); the ProcessWheel hooks write them, scaled, before every call - always, also with this
//    off, since the patched code has no constant of its own any more.
//
// VCS_PHYSICS_TIMESTEP=0 keeps both at the game's per-frame amounts.
#include "game/vcs_physics_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kApplyTurnForce = 0x00347E78u;  // CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
        constexpr uint32_t kBodyPushReturn = 0x003F36D4u;  // FUN_003F34F0's call: the push from a body under a wheel
        constexpr uint32_t kProcessWheelCar = 0x003E6C18u; // CVehicle::ProcessWheel
        constexpr uint32_t kProcessWheelBike = 0x003E7618u;
        constexpr uint32_t kBrakeSlots = 0x0007FF00u;      // the patched ProcessWheels' rolling-resistance constants
        constexpr float kBrakeConstants[] = {0.54f, 0.09f, 0.9f, 0.18f}; // as the game loaded them (0x3F0A3D71 ...)
        constexpr uint32_t kTimeStep = 0x004CD110u;        // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr float kPs2Step = 50.0f / 30.0f;          // a 30 fps frame

        PS2Runtime::RecompiledFunction g_origTurnForce = nullptr, g_origWheelCar = nullptr, g_origWheelBike = nullptr;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_PHYSICS_TIMESTEP");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        // the frame isn't the PS2's 1/30 s: independent timing, or original timing at 60 (one VBlank a frame)
        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

        // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            float step;
            std::memcpy(&step, rdram + kTimeStep, 4);
            return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
        }

        PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kBodyPushReturn)
                ctx->f[14] *= frameScale(rdram);
            g_origTurnForce(rdram, ctx, runtime);
        }

        void writeBrakeConstants(uint8_t *rdram)
        {
            const float scale = frameScale(rdram);
            for (size_t i = 0; i < sizeof(kBrakeConstants) / sizeof(kBrakeConstants[0]); ++i)
            {
                const float v = kBrakeConstants[i] * scale; // exact at 1
                std::memcpy(rdram + kBrakeSlots + 4u * i, &v, 4);
            }
        }

        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void processWheel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            writeBrakeConstants(rdram);
            (*Original)(rdram, ctx, runtime);
        }
    }

    void installPhysicsTimingHooks(PS2Runtime &runtime)
    {
        g_origTurnForce = runtime.lookupFunction(kApplyTurnForce);
        g_origWheelCar = runtime.lookupFunction(kProcessWheelCar);
        g_origWheelBike = runtime.lookupFunction(kProcessWheelBike);
        if (g_origTurnForce)
            runtime.replaceFunction(kApplyTurnForce, &applyTurnForce);
        else
            std::fprintf(stderr, "[physics-timing] ApplyTurnForce 0x%x not found: the push from bodies under wheels stays on the frame rate\n",
                         kApplyTurnForce);
        // The patched ProcessWheels read their constants from kBrakeSlots, so these hooks are not optional.
        if (g_origWheelCar)
            runtime.replaceFunction(kProcessWheelCar, &processWheel<&g_origWheelCar>);
        if (g_origWheelBike)
            runtime.replaceFunction(kProcessWheelBike, &processWheel<&g_origWheelBike>);
        if (!g_origWheelCar || !g_origWheelBike)
            std::fprintf(stderr, "[physics-timing] ProcessWheel 0x%x / 0x%x not found: vehicles roll without resistance\n",
                         kProcessWheelCar, kProcessWheelBike);
    }
}
