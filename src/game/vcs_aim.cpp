// See vcs_aim.h and game-internals.md 12c.
#include "game/vcs_aim.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::aim
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        // the ped (game-internals.md 12, 12c)
        constexpr uint32_t kPedPos = 0x30u, kPedVehicle = 0x480u, kPedState = 0x8C4u, kPedSlot = 0x789u, kPedWeapons = 0x578u;
        constexpr uint32_t kPedTarget = 0x820u;     // what the player is locked on to (the dummy object in free aim)
        constexpr uint32_t kPedAimPoint = 0xC90u;   // the free-aim point (the game keeps the dummy there)
        constexpr uint32_t kPedFreeAimWord = 0xCCCu; // the high word of the flags at +0xCC8: bit 51 = free aim
        constexpr uint32_t kFreeAimBit = 0x00080000u;
        constexpr uint32_t kEntityFlags = 0x48u;    // & 0xE: 6 ped, 8 object
        // TheCamera and its CCams (game-internals.md 12b)
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u;
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kCamSource = 0x20u, kCamAlpha = 0x78u, kCamBeta = 0x7Cu;
        constexpr uint32_t kCamModeLockOn = 11;
        constexpr uint32_t kTimeStep = 0x004CD110u; // CTimer::ms_fTimeStep (1.0 = 1/50 s)

        // the hooked functions
        constexpr uint32_t kGameProcess = 0x003BA8D8u;   // CGame::Process: scripts, the world (peds move, guns fire)
        constexpr uint32_t kCameraProcess = 0x00378DD8u; // CCamera::Process: after the world, before the HUD
        constexpr uint32_t kShotLine = 0x002D0870u;      // the weapons' line test (CWorld::ProcessLineOfSight, 8 args)
        constexpr uint32_t kShotLineReturn = 0x002C47A4u; // its call in FireInstantHit (0x2c3e48) for a locked-on shooter
        // Guest memory for the shot's start point: the EE kernel's area below the game (0x00000-0x7FFFF), unused by the
        // recompiled runtime and the game (all zero in play, 2026-10-31).
        constexpr uint32_t kScratch = 0x00060000u;

        constexpr float kDummyDistance = 12.0f; // metres from Vic: the lock drops past ~30 (the weapons' lock-on range)
        constexpr float kPivotHeight = 0.6f;    // the dummy turns about Vic's shoulders
        constexpr float kPitchLimit = 1.2f;
        constexpr float kStartPitchLimit = 0.35f; // aiming starts about level, not where a steep camera looked
        constexpr uint32_t kCamModeFollowPed = 4;
        constexpr float kStickRate = 2.0f;      // radians a second at full tilt (gamepad)

        PS2Runtime::RecompiledFunction g_origGame = nullptr, g_origCamera = nullptr, g_origShotLine = nullptr;
        uint8_t *g_rdram = nullptr;

        enum class State
        {
            Idle,     // not aiming
            Entering, // R1 + L3 sent: waiting for the game's free-aim lock
            Active,   // the dummy is ours
            Released, // one report with R1 up (a lock that went wrong), then Idle again
        };
        // game thread
        State g_state = State::Idle;
        int g_wait = 0;
        uint32_t g_dummy = 0;
        float g_yaw = 0, g_pitch = 0; // the aim from Vic's shoulders: yaw from +x toward +y, pitch up
        int g_handBack = 0;           // frames left to give the follow camera the aim's heading when it is back
        float g_handBeta = 0;
        std::atomic<bool> g_active{false};
        std::atomic<uint32_t> g_shots{0}; // shots sent down the camera ray ("aimstate")

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        void wrVec(uint32_t a, const float v[3])
        {
            for (int i = 0; i < 3; ++i)
                wrf(a + i * 4, v[i]);
        }

        uint32_t activeCam() { return kCams + g_rdram[kActiveCam] * kCamSize; }
        bool isObject(uint32_t e) { return e && (rd32(e + kEntityFlags) & 0xEu) == 8u; }
        bool freeAimBit(uint32_t ped) { return rd32(ped + kPedFreeAimWord) & kFreeAimBit; }
        void setFreeAimBit(uint32_t ped, bool on)
        {
            const uint32_t w = rd32(ped + kPedFreeAimWord);
            wr32(ped + kPedFreeAimWord, on ? w | kFreeAimBit : w & ~kFreeAimBit);
        }

        // Weapon types (docs/TRAINER.md): 19..29 pistols, shotguns, SMGs, rifles; 33..35 flame-thrower, M249, minigun.
        // Not 30..32 (the snipers and the rocket launcher aim through their scopes), 38 / 39 (camera, binoculars),
        // melee (1..11) or thrown (12..17).
        bool isGun(uint32_t type) { return (type >= 19 && type <= 29) || (type >= 33 && type <= 35); }

        bool onFoot(uint32_t ped) { return ped && !rd32(ped + kPedVehicle) && rd32(ped + kPedState) != 0x37; }

        void dummyPlace(uint32_t ped)
        {
            const float cp = std::cos(g_pitch);
            const float at[3] = {rdf(ped + kPedPos) + kDummyDistance * cp * std::cos(g_yaw),
                                 rdf(ped + kPedPos + 4) + kDummyDistance * cp * std::sin(g_yaw),
                                 rdf(ped + kPedPos + 8) + kPivotHeight + kDummyDistance * std::sin(g_pitch)};
            wrVec(g_dummy + kPedPos, at);
            wrVec(ped + kPedAimPoint, at);
        }

        void stop(const char *why)
        {
            if (g_state == State::Active)
            {
                std::fprintf(stderr, "[aim] free aim off (%s)\n", why);
                // the follow camera comes back looking where the aim did (game-internals.md 12c)
                g_handBeta = rdf(activeCam() + kCamBeta);
                g_handBack = 10;
            }
            g_state = State::Idle;
            g_active = false;
        }

        // The follow camera, back after free aim, starts from the lock-on camera's heading but eases toward its source
        // points (+0x220 / +0x230: where it stood before the aim, game-internals.md 12b) - a swing back to the old view.
        // Its first frame back, both are put where the aim camera looked: the heading, and the source points turned
        // about Vic to it (their distance and height kept).
        void handBack(uint32_t ped)
        {
            const uint32_t cam = activeCam();
            if (rd32(cam) != kCamModeFollowPed)
                return;
            g_handBack = 0;
            wrf(cam + kCamBeta, g_handBeta);
            const float px = rdf(ped + kPedPos), py = rdf(ped + kPedPos + 4);
            for (uint32_t at : {cam + 0x220u, cam + 0x230u})
            {
                const float ox = rdf(at) - px, oy = rdf(at + 4) - py, r = std::sqrt(ox * ox + oy * oy);
                wrf(at, px + r * std::cos(g_handBeta));
                wrf(at + 4, py + r * std::sin(g_handBeta));
            }
        }

        // CGame::Process: the world runs with the free-aim bit cleared - to the game a lock-on to an object, so Vic
        // walks / strafes / stands as he does locked on with this weapon, and the left stick moves him (in the game's
        // free aim it moves the dummy, and Vic stands still).
        PS2_GUEST_CC void gameProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Active)
                if (const uint32_t ped = rd32(kPlayerPed))
                    setFreeAimBit(ped, false);
            g_origGame(rdram, ctx, runtime);
        }

        // CCamera::Process: the camera and then the HUD see free aim again (its camera centres the dummy, its circle is
        // drawn on it).
        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Active)
                if (const uint32_t ped = rd32(kPlayerPed); ped && rd32(ped + kPedTarget) == g_dummy)
                    setFreeAimBit(ped, true);
            g_origCamera(rdram, ctx, runtime);
        }

        // The weapons' line test (from a0 to a1; t1 the shooter). For the player's free-aim shot (FireInstantHit's call
        // for a locked-on shooter) the line is the camera's through the circle: from the point on it level with the gun
        // to as far as the gun reaches. The gun's own start (a0) is left for the tracer and the impact's effects; the
        // end (a1, FireInstantHit's) gets the camera line's end, so the tracer goes where the shot went.
        PS2_GUEST_CC void shotLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = rd32(kPlayerPed);
            if (g_state == State::Active && GPR_U32(ctx, 31) == kShotLineReturn && ped && GPR_U32(ctx, 9) == ped &&
                rd32(ped + kPedTarget) == g_dummy)
            {
                const uint32_t from = GPR_U32(ctx, 4), to = GPR_U32(ctx, 5), cam = activeCam();
                const float src[3] = {rdf(cam + kCamSource), rdf(cam + kCamSource + 4), rdf(cam + kCamSource + 8)};
                const float gun[3] = {rdf(from), rdf(from + 4), rdf(from + 8)};
                const float end[3] = {rdf(to), rdf(to + 4), rdf(to + 8)};
                float dir[3] = {rdf(g_dummy + kPedPos) - src[0], rdf(g_dummy + kPedPos + 4) - src[1],
                                rdf(g_dummy + kPedPos + 8) - src[2]};
                const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const float range = std::sqrt((end[0] - gun[0]) * (end[0] - gun[0]) + (end[1] - gun[1]) * (end[1] - gun[1]) +
                                              (end[2] - gun[2]) * (end[2] - gun[2]));
                if (len > 0.01f && range > 0.01f)
                {
                    for (float &d : dir)
                        d /= len;
                    const float along = std::max(0.0f, (gun[0] - src[0]) * dir[0] + (gun[1] - src[1]) * dir[1] +
                                                           (gun[2] - src[2]) * dir[2]);
                    const float start[3] = {src[0] + dir[0] * along, src[1] + dir[1] * along, src[2] + dir[2] * along};
                    const float stop[3] = {start[0] + dir[0] * range, start[1] + dir[1] * range, start[2] + dir[2] * range};
                    wrVec(kScratch, start);
                    wr32(kScratch + 12, 0);
                    wrVec(to, stop);
                    SET_GPR_U32(ctx, 4, kScratch);
                    g_shots.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_origShotLine(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_FREEAIM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[aim] free aim off (VCS_FREEAIM=0)\n");
            return;
        }
        g_origGame = runtime.lookupFunction(kGameProcess);
        g_origCamera = runtime.lookupFunction(kCameraProcess);
        g_origShotLine = runtime.lookupFunction(kShotLine);
        if (!g_origGame || !g_origCamera || !g_origShotLine)
            return;
        runtime.replaceFunction(kGameProcess, &gameProcess);
        runtime.replaceFunction(kCameraProcess, &cameraProcess);
        runtime.replaceFunction(kShotLine, &shotLine);
        // aimstate -> ok active=<0|1> shots=<shots down the camera ray so far> (tools/controls_check.py)
        ps2control::addCommand("aimstate", [](const std::string &, ps2control::Reply reply) {
            char text[96];
            std::snprintf(text, sizeof(text), "ok active=%d shots=%u", g_active.load() ? 1 : 0, g_shots.load());
            reply(text);
        });
    }

    bool gunInHand()
    {
        const uint32_t ped = g_rdram ? rd32(kPlayerPed) : 0;
        if (!ped)
            return false;
        const int8_t slot = static_cast<int8_t>(g_rdram[(ped + kPedSlot) & kRamMask]);
        return slot >= 0 && slot < 10 && isGun(rd32(ped + kPedWeapons + slot * 0x1Cu));
    }

    bool aiming() { return g_active.load(std::memory_order_relaxed); }

    Output update(const Input &in)
    {
        Output out;
        const uint32_t ped = g_rdram && g_origGame ? rd32(kPlayerPed) : 0;
        if (!ped || !in.want || !onFoot(ped) || !gunInHand())
        {
            stop(!in.want ? "released" : "no gun / not on foot");
            if (ped && g_handBack > 0 && --g_handBack >= 0)
                handBack(ped);
            return out;
        }
        g_handBack = 0;
        out.own = true;
        const uint32_t cam = activeCam();
        switch (g_state)
        {
        case State::Released:
            g_state = State::Idle; // R1 up for this report; the next one enters again
            return out;
        case State::Idle:
        {
            // aim where the camera looks
            const float fx = rdf(cam + 0x10), fy = rdf(cam + 0x14), fz = rdf(cam + 0x18);
            g_yaw = std::atan2(fy, fx);
            g_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kStartPitchLimit, kStartPitchLimit);
            g_state = State::Entering;
            g_wait = 0;
            out.r1 = out.l3 = true;
            return out;
        }
        case State::Entering:
        {
            const uint32_t target = rd32(ped + kPedTarget);
            if (!(isObject(target) && freeAimBit(ped)))
            {
                out.r1 = ++g_wait < 20; // not yet (Vic may still be busy); after ~20 frames let go and try again
                if (!out.r1)
                    g_state = State::Released;
                return out;
            }
            g_dummy = target;
            g_state = State::Active;
            g_active = true;
            std::fprintf(stderr, "[aim] free aim on (dummy 0x%x)\n", target);
            break;
        }
        case State::Active:
            if (rd32(ped + kPedTarget) != g_dummy)
            {
                stop("lock lost");
                g_state = State::Released;
                return out;
            }
            break;
        }

        // turn the aim, and the camera by the same amount before its update (as the mouse turns the follow camera:
        // the camera then has nothing left to catch up, game-internals.md 12c)
        const float dt = std::clamp(rdf(kTimeStep), 0.05f, 3.4f) / 50.0f;
        const float turn = in.turn + in.turnRate * kStickRate * dt;
        const float pitch = std::clamp(g_pitch + in.tilt + in.tiltRate * kStickRate * dt, -kPitchLimit, kPitchLimit);
        const float tilt = pitch - g_pitch;
        g_yaw = std::remainder(g_yaw + turn, 6.2831855f);
        g_pitch = pitch;
        if (rd32(cam) == kCamModeLockOn && (turn != 0.0f || tilt != 0.0f))
        {
            const float beta = std::fmod(rdf(cam + kCamBeta) + turn, 6.2831855f);
            wrf(cam + kCamBeta, beta < 0.0f ? beta + 6.2831855f : beta);
            wrf(cam + kCamAlpha, rdf(cam + kCamAlpha) + tilt);
        }
        dummyPlace(ped);
        out.r1 = true;
        return out;
    }
}
