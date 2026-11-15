// Enhancements > Saving: "Quick save" (docs/mods/QUICKSAVE.md has the addresses and what was checked in the game).
//
// THE SAVE. The quick save key (Keyboard > Menus, F12) runs the game's own safehouse save: the main script's save
// subroutine (IP 0xC646; each save point CALLs it, 0x1222B) as a new script of the game's VM (vcs::rig::startScriptAt,
// returning onto a TERMINATE_THIS_SCRIPT at 0x16E90), named "quicksav", with L0 = 1 (it then fades the screen back in
// itself; the save points pass 0 and fade in after it). Around ACTIVATE_SAVE_MENU the subroutine takes the player's
// control, sets G4 = G684 = 1 (the "saved by the save subroutine" flags the after-load script INITSAV checks), stores the
// spawn point (G678..680 -> G174..176), the weapons and money, fades out, waits for HAS_SAVE_GAME_FINISHED, then puts the
// camera back, fades in and clears the flags. A bare ACTIVATE_SAVE_MENU (the first auto save) left G4 = 0: such a save
// loads as a new game.
// - Only outside missions (u8 0x4CD3E7: the story missions and the side jobs, which the game launches as missions), with
//   the camera on the player (TheCamera +0x888: no cutscene), the player alive and not wanted (ped +0x920), and the game
//   not saving already. Refused with a notice (the trainer's notices).
// - In a vehicle: the game's ACTIVATE_SAVE_MENU handler (FUN_002D38C8) does nothing there (FUN_00219D50); for our script
//   it is replaced by what it does otherwise (0x48A65C = 1, the stat 0x4CD480 + 1). The vehicle is stopped first
//   (SET_CAR_FORWARD_SPEED 0: the subroutine takes the control, the car would roll on).
// - The clock: the card manager's save (0x275EC4) calls FUN_003ADF68, which moves the clock on by 360 minutes
//   (FUN_0024D5B8(0x168)) before writing - the safehouse's "sleep". For a quick save that call is skipped (the hook on
//   FUN_0024D5B8 while our save runs); 720 minutes (after wasted / busted) are untouched.
//
// THE QUICK-SAVE DATA lives in the save itself, in script globals - the only script state a save keeps: the scripts
// block's load (FUN_0025A348) restores the globals and nothing of the running scripts, which start again from their
// beginnings (hence INITSAV and the G4 flag).
// - Where the player stands: the game's own spawn point G678..680 and heading G177. INITSAV (main 0x2ADF7, after a load
//   with G4 = 1) puts the player there - inside the safehouse's interior when G171 != -1 (0x20C69), inside the Empire
//   building's when G172 != -1 (0x14750, 0x126D0), else outdoors (LOAD_SCENE first). For the quick save they hold the
//   player's place while the save is written (G171 / G172 to -1 when he is outdoors: they can be left over); right after
//   it the live game gets its own values back.
// - The rest in G1106, one word: the load (FUN_0025A348) restores only the globals listed in MAIN.SCM's segment 2 (1092
//   byte offsets, the table FUN_00255188 searches - read before as a model-id table) and zeroes the others; of those
//   1092, G1106 is the only one no script uses (and 0 in every save looked at). Bits 31..28 0xA (a quick save), 27..26
//   the vehicle, 25..17 its model, 16..9 / 8..1 the parked vehicle's place from the player (signed, 0.8 m units: +-100 m),
//   bit 0 free. The vehicle's colours and health do not fit: it comes back as the same model, the game picks a colour.
// Vehicle: 0 none, 1 the player sits in it, 2 near: the last one he drove, still there, within 100 m.
//
// THE LOAD (Load Game, or the newest save at boot). INITSAV puts the player back as above. Once it is done (G4 = G684
// = 0) and G1106 marks a quick save: G1106 to 0, and with a vehicle REQUEST_MODEL, CREATE_CAR (in it: at the player, his
// heading; parked: at its place, on the ground), the player WARPed into it when he was in it, MARK_CAR_AS_NO_LONGER_NEEDED.
// G678..680 / G177 keep the quick save's place until the next save sets them (only a save and INITSAV read them).
// Quick saves in other interiors (shops...) are refused: nothing would put the game back in them.
//
// An older save made by the auto save (this mod's first form) outside its safehouse: INITSAV put the player at the
// safehouse's spawn point but outside; when G678..680 is one of the 4 safehouses with an interior (GA3305 / 3309 [i]),
// 0x20C69 is run with L0 = i, then the player placed at its spawn point (legacyFix).
//
// VCS_QUICKSAVE=0/1 overrides the setting; VCS_QUICKSAVE_HOOKS=0 installs nothing. `quicksave` -> ok state;
// `quicksave now` = the key, `quicksave savepoint` = a normal safehouse save (tests).
#include "game/vcs_quicksave.h"

#include "debug/rig.h"
#include "game/vcs_trainer.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vcs::quicksave
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kScriptSpacePtr = 0x0048741Cu; // 0 until the script engine runs; global n at +4n
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kTimeMs = 0x004CD104u;         // CTimer ms: stands still while the game is paused
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8
        constexpr uint32_t kClock = 0x004CD148u;          // hours, minutes (bytes)
        constexpr uint32_t kArea = 0x00489F7Cu;           // CGame::currArea: 0 outdoors
        constexpr uint32_t kLookingAtPlayer = 0x006F44D0u + 0x888u; // TheCamera: 1 unless a cutscene / script camera
        constexpr uint32_t kVehiclePool = 0x00487A98u, kVehicleSize = 0x8C0u; // CPool {entries, flags, size}
        constexpr uint32_t kStreaming = 0x00489FF8u;      // -> CStreaming; +200 -> the models' info (0x18 each, +0xC: 1 loaded)
        constexpr uint32_t kPedVehicle = 0x480u, kPedHealth = 0x4E4u, kPedState = 0x8C4u, kPedWanted = 0x920u;
        constexpr uint32_t kPos = 0x30u, kForward = 0x10u, kModel = 0x56u;
        constexpr uint32_t kSaveRoutine = 0x0000C646u;    // main script: the safehouse save subroutine (CALL, L0)
        constexpr uint32_t kTerminateIp = 0x00016E90u;    // main script: a TERMINATE_THIS_SCRIPT to return onto
        constexpr uint32_t kEnterSafehouse = 0x00020C69u; // main script: enter safehouse L0's interior (INITSAV's)
        constexpr uint32_t kActivateSaveMenu = 0x002D38C8u; // ACTIVATE_SAVE_MENU's handler (opcode 0x0260)
        constexpr uint32_t kAdvanceClock = 0x0024D5B8u;   // FUN_0024D5B8(minutes): the clock (and the day) on
        constexpr uint32_t kSaveRequest = 0x0048A65Cu;    // set to 1 by that handler: the frontend opens the Save menu
        constexpr uint32_t kSavesMade = 0x004CD480u;      // counted up by that handler (a stat)
        constexpr uint32_t kSaveSleepMinutes = 360u;      // FUN_003ADF68's step, from the card manager's save
        constexpr uint32_t G_SAVING = 4, G_SAVING2 = 684, G_SPAWN = 678, G_IN_SAFEHOUSE = 171, G_IN_EMPIRE = 172;
        constexpr uint32_t GA_SAFEHOUSE_X = 3305, GA_SAFEHOUSE_Y = 3309, GA_SAFEHOUSE_Z = 3313, GA_SAFEHOUSE_HEADING = 3317;
        constexpr uint32_t kSafehouses = 4;
        constexpr uint32_t kLocal0 = 0x54u;               // CRunningScript: local 0 (frame 0)
        constexpr char kScriptName[] = "quicksav";
        constexpr uint32_t G_HEADING = 177;
        constexpr uint32_t G_DATA = 1106; // saved, used by no script (see above)
        constexpr uint32_t kMark = 0xAu;  // bits 31..28
        constexpr float kNearVehicle = 100.0f;
        constexpr uint32_t kStartTimeoutMs = 3000u, kModelTimeoutMs = 10000u;

        enum : uint16_t
        {
            TERMINATE_THIS_SCRIPT = 0x0023,
            SET_CHAR_COORDINATES = 0x0044,
            CREATE_CAR = 0x0048,
            SET_CHAR_HEADING = 0x00CF,
            SET_CAR_HEADING = 0x00D1,
            MARK_CAR_AS_NO_LONGER_NEEDED = 0x0113,
            REQUEST_MODEL = 0x0160,
            MARK_MODEL_AS_NO_LONGER_NEEDED = 0x0162,
            RESTORE_CAMERA_JUMPCUT = 0x01CF,
            WARP_CHAR_INTO_CAR = 0x021C,
            SET_CAMERA_BEHIND_PLAYER = 0x0221,
            STORE_CAR_CHAR_IS_IN_NO_SAVE = 0x024B,
            LOAD_SCENE = 0x0253,
            SET_CAR_FORWARD_SPEED = 0x02E9,
            SET_AREA_VISIBLE = 0x02EA,
        };

        // Bytecode for one script (as the trainer's Scm): arguments carry a type byte.
        struct Code
        {
            std::vector<uint8_t> b;
            Code &op(uint16_t c) { return put(c & 0xFF).put(c >> 8); }
            Code &i(int32_t v)
            {
                put(0x06);
                for (int k = 0; k < 4; ++k)
                    put((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF);
                return *this;
            }
            Code &f(float v)
            {
                uint32_t u;
                std::memcpy(&u, &v, 4);
                put(0x09);
                for (int k = 0; k < 4; ++k)
                    put((u >> (8 * k)) & 0xFF);
                return *this;
            }
            Code &player() { return put(0xCF).put(0xA5); } // global 0x2A5: the player and his char
            Code &local(int n) { return put(static_cast<uint8_t>(0x0D + n)); }
            Code &put(uint32_t v)
            {
                b.push_back(static_cast<uint8_t>(v));
                return *this;
            }
            std::vector<uint8_t> done()
            {
                op(TERMINATE_THIS_SCRIPT);
                return b;
            }
        };

        struct Data // what a quick save keeps (G1106, the player's place in G678..680 and G177)
        {
            float x = 0, y = 0, z = 0, heading = 0;
            uint32_t area = 0, vehicle = 0, model = 0;
            float vx = 0, vy = 0, vz = 0, vheading = 0;
            uint32_t spawn[4] = {}; // the game's G678..680, G177 (raw words)
        };

        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_keyRequest{false};
        std::atomic<bool> g_savePointRequest{false}; // tests: the save point's own code (a normal safehouse save)
        constexpr uint32_t kSavePointCode = 0x00012213u; // main script: G678.. = G2293.., CALL 0xC646 (L0 0), fade in
        PS2Runtime::RecompiledFunction g_origActivate = nullptr, g_origClock = nullptr;
        bool g_installed = false;

        // game thread
        enum class State
        {
            Idle,
            Saving, // the save subroutine runs for us
        };
        State g_state = State::Idle;
        uint32_t g_saveScript = 0, g_startedAt = 0, g_lastTime = 0, g_lastLoadCheck = 0;
        bool g_sawSaving = false, g_wasInGame = false;
        uint32_t g_lastVehicle = 0, g_lastVehicleModel = 0;
        uint8_t g_clockAtSave[2] = {};
        // the live values the quick save borrowed, given back once it is written
        uint32_t g_liveData = 0, g_liveSpawn[4] = {}, g_liveIn[2] = {};

        enum class Restore
        {
            Idle,
            Armed,   // a game was loaded: find our data, wait for INITSAV
            Model,   // the vehicle's model requested
            Car,     // the car script started: its handle -> colours, then released
            Legacy,  // an old auto save: 0x20C69 started, until G171 = the safehouse, then the player to its spawn point
        };
        Restore g_restore = Restore::Idle;
        bool g_haveData = false;
        Data g_data;
        uint32_t g_restoreAt = 0, g_carScript = 0, g_legacySafehouse = 0;

        // status (any thread)
        std::atomic<int> g_saves{0}, g_refused{0}, g_restores{0}, g_legacyFixes{0}, g_clockSkips{0};
        std::atomic<const char *> g_lastRefusal{"-"};
        std::atomic<uint32_t> g_lastSavedClock{0};

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        uint32_t global(const uint8_t *ram, uint32_t n) { return rd32(ram, rd32(ram, kScriptSpacePtr) + 4u * n); }
        void setGlobal(uint8_t *ram, uint32_t n, uint32_t v) { wr32(ram, rd32(ram, kScriptSpacePtr) + 4u * n, v); }
        uint32_t bitsOf(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        float floatOf(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        constexpr uint32_t kSpawnGlobals[4] = {678, 679, 680, 177}; // G678..680, G177
        float globalf(const uint8_t *ram, uint32_t n) { return rdf(ram, rd32(ram, kScriptSpacePtr) + 4u * n); }
        float headingOf(const uint8_t *ram, uint32_t entity) // degrees, as GET_CHAR_HEADING / GET_CAR_HEADING
        {
            const float fx = rdf(ram, entity + kForward), fy = rdf(ram, entity + kForward + 4);
            float h = std::atan2(-fx, fy) * 57.2957795f;
            return h < 0 ? h + 360.0f : h;
        }

        void notice(const char *text) { vcs::trainer::notify(text); }
        void refuse(const char *why, const char *text)
        {
            g_lastRefusal.store(why, std::memory_order_relaxed);
            g_refused.fetch_add(1, std::memory_order_relaxed);
            notice(text);
        }

        // the vehicle at `ptr` still in the pool (not freed, not reused for another model)
        bool vehicleAlive(const uint8_t *ram, uint32_t ptr, uint32_t model)
        {
            const uint32_t pool = rd32(ram, kVehiclePool);
            if (!pool || !ptr)
                return false;
            const uint32_t base = rd32(ram, pool), flags = rd32(ram, pool + 4), size = rd32(ram, pool + 8);
            if (ptr < base || (ptr - base) % kVehicleSize)
                return false;
            const uint32_t index = (ptr - base) / kVehicleSize;
            return index < size && !(ram[(flags + index) & kRamMask] & 0x80u) && rd32(ram, ptr + kModel) % 0x10000u == model;
        }
        uint32_t vehicleAt(const uint8_t *ram, uint32_t handle) // CPool::GetAt for a script handle
        {
            const uint32_t pool = rd32(ram, kVehiclePool);
            const int32_t index = static_cast<int32_t>(handle) >> 8;
            if (!pool || index < 0 || index >= static_cast<int32_t>(rd32(ram, pool + 8)))
                return 0;
            if (ram[(rd32(ram, pool + 4) + index) & kRamMask] != (handle & 0xFFu))
                return 0;
            return rd32(ram, pool) + static_cast<uint32_t>(index) * kVehicleSize;
        }
        bool modelLoaded(const uint8_t *ram, uint32_t model)
        {
            const uint32_t streaming = rd32(ram, kStreaming);
            return streaming && ram[(rd32(ram, streaming + 200) + model * 0x18u + 0xCu) & kRamMask] == 1;
        }

        // ACTIVATE_SAVE_MENU in a vehicle, for our script only: what the handler does on foot (the ped part - his move /
        // turn speed to 0 - left out; the vehicle was stopped by our SET_CAR_FORWARD_SPEED 0). Return 0 (go on).
        PS2_GUEST_CC void activateSaveMenu(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = rd32(rdram, kPlayerPed);
            if (g_saveScript && GPR_U32(ctx, 4) == g_saveScript && ped && rd32(rdram, ped + kPedVehicle))
            {
                wr32(rdram, kSaveRequest, 1);
                wr32(rdram, kSavesMade, rd32(rdram, kSavesMade) + 1);
                SET_GPR_U32(ctx, 2, 0u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origActivate(rdram, ctx, runtime);
        }

        // FUN_0024D5B8(minutes): the save's 6 hours skipped while our quick save runs
        PS2_GUEST_CC void advanceClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Saving && GPR_U32(ctx, 4) == kSaveSleepMinutes && global(rdram, G_SAVING))
            {
                g_clockSkips.fetch_add(1, std::memory_order_relaxed);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origClock(rdram, ctx, runtime);
        }

        void startSave(uint8_t *ram, uint32_t time, uint32_t ped)
        {
            if (!g_enabled.load(std::memory_order_relaxed))
                return refuse("off", "Quick save is off (Settings > Enhancements > Saving)");
            if (ram[kMissionRunning])
                return refuse("mission", "Quick save: not during a mission");
            if (global(ram, G_SAVING) || global(ram, G_SAVING2) || g_restore != Restore::Idle)
                return refuse("busy", "Quick save: not now");
            if (rd32(ram, kLookingAtPlayer) != 1)
                return refuse("cutscene", "Quick save: not now");
            const uint32_t state = rd32(ram, ped + kPedState);
            if (state == 0x39u || state == 0x3Au || !(rdf(ram, ped + kPedHealth) > 0.0f))
                return refuse("dead", "Quick save: not now");
            if (rd32(ram, ped + kPedWanted) != 0)
                return refuse("wanted", "Quick save: lose the police first");
            const int32_t inSafehouse = static_cast<int32_t>(global(ram, G_IN_SAFEHOUSE));
            const int32_t inEmpire = static_cast<int32_t>(global(ram, G_IN_EMPIRE));
            const uint32_t area = rd32(ram, kArea);
            if (area != 0 && inSafehouse == -1 && inEmpire == -1)
                return refuse("interior", "Quick save: not in this building");

            Data d;
            const uint32_t vehicle = rd32(ram, ped + kPedVehicle);
            const uint32_t at = vehicle ? vehicle : ped;
            d.x = rdf(ram, ped + kPos), d.y = rdf(ram, ped + kPos + 4), d.z = rdf(ram, ped + kPos + 8);
            if (vehicle) // the ped's own matrix is not his place while he sits in a vehicle
                d.x = rdf(ram, at + kPos), d.y = rdf(ram, at + kPos + 4), d.z = rdf(ram, at + kPos + 8) + 1.0f;
            d.heading = headingOf(ram, at);
            d.area = area;
            uint32_t car = vehicle;
            if (vehicle)
                d.vehicle = 1;
            else if (g_lastVehicle && vehicleAlive(ram, g_lastVehicle, g_lastVehicleModel) &&
                     std::hypot(rdf(ram, g_lastVehicle + kPos) - d.x, rdf(ram, g_lastVehicle + kPos + 4) - d.y) <= kNearVehicle)
                d.vehicle = 2, car = g_lastVehicle;
            if (car)
            {
                d.model = rd32(ram, car + kModel) & 0xFFFFu;
                d.vx = rdf(ram, car + kPos), d.vy = rdf(ram, car + kPos + 4), d.vz = rdf(ram, car + kPos + 8);
                d.vheading = headingOf(ram, car);
            }

            if (vehicle) // stop it: the subroutine takes the player's control
            {
                Code stop;
                stop.op(STORE_CAR_CHAR_IS_IN_NO_SAVE).player().local(0).op(SET_CAR_FORWARD_SPEED).local(0).put(0x02);
                const std::vector<uint8_t> bytes = stop.done();
                if (!vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), kScriptName))
                    return refuse("slots", "Quick save: not now");
            }
            const uint32_t s = vcs::rig::startScriptAt(ram, kSaveRoutine, kScriptName, kTerminateIp);
            if (!s)
                return refuse("slots", "Quick save: not now");
            wr32(ram, s + kLocal0, 1); // L0 = 1: the subroutine fades back in itself
            // the data into the globals the save keeps; the live values kept to be given back
            g_liveData = global(ram, G_DATA);
            for (uint32_t k = 0; k < 4; ++k)
                g_liveSpawn[k] = global(ram, kSpawnGlobals[k]);
            g_liveIn[0] = global(ram, G_IN_SAFEHOUSE), g_liveIn[1] = global(ram, G_IN_EMPIRE);
            const auto unit = [](float metres) {
                const int v = static_cast<int>(std::lround(metres / 0.8f));
                return static_cast<uint32_t>(std::clamp(v, -127, 127)) & 0xFFu;
            };
            const uint32_t word = kMark << 28 | (d.vehicle & 3u) << 26 | (d.model & 0x1FFu) << 17 |
                                  (d.vehicle == 2 ? unit(d.vx - d.x) << 9 | unit(d.vy - d.y) << 1 : 0u);
            setGlobal(ram, G_DATA, word);
            const float place[4] = {d.x, d.y, d.z, d.heading};
            for (uint32_t k = 0; k < 4; ++k)
                setGlobal(ram, kSpawnGlobals[k], bitsOf(place[k]));
            if (area == 0) // outdoors: no interior to put him in (the two can be left over from one)
                setGlobal(ram, G_IN_SAFEHOUSE, 0xFFFFFFFFu), setGlobal(ram, G_IN_EMPIRE, 0xFFFFFFFFu);
            g_saveScript = s;
            g_startedAt = time;
            g_sawSaving = false;
            std::memcpy(g_clockAtSave, ram + kClock, 2);
            g_state = State::Saving;
            std::fprintf(stderr, "[quicksave] at %.1f %.1f %.1f area %u vehicle %u (model %u), clock %02u:%02u\n", d.x, d.y,
                         d.z, d.area, d.vehicle, d.model, g_clockAtSave[0], g_clockAtSave[1]);
        }

        // the live game's own values back after the quick save was written
        void giveBack(uint8_t *ram)
        {
            setGlobal(ram, G_DATA, g_liveData);
            for (uint32_t k = 0; k < 4; ++k)
                setGlobal(ram, kSpawnGlobals[k], g_liveSpawn[k]);
            setGlobal(ram, G_IN_SAFEHOUSE, g_liveIn[0]), setGlobal(ram, G_IN_EMPIRE, g_liveIn[1]);
        }

        // a loaded quick save's data (G1106); then G1106 as a normal save leaves it
        bool takeData(uint8_t *ram, Data &d)
        {
            const uint32_t w = global(ram, G_DATA);
            if ((w >> 28) != kMark)
                return false;
            const auto metres = [](uint32_t u) { return static_cast<int8_t>(u & 0xFFu) * 0.8f; };
            d = {};
            d.x = globalf(ram, 678), d.y = globalf(ram, 679), d.z = globalf(ram, 680), d.heading = globalf(ram, G_HEADING);
            d.vehicle = (w >> 26) & 3u, d.model = (w >> 17) & 0x1FFu;
            d.vx = d.x, d.vy = d.y, d.vz = d.z - 1.0f, d.vheading = d.heading;
            if (d.vehicle == 2)
                d.vx = d.x + metres(w >> 9), d.vy = d.y + metres(w >> 1), d.vz = -100.0f; // on the ground there
            setGlobal(ram, G_DATA, 0);
            return true;
        }

        // the safehouse (with an interior) whose spawn point is G678..680; -1: none
        int safehouseOfSpawn(const uint8_t *ram)
        {
            const float x = globalf(ram, G_SPAWN), y = globalf(ram, G_SPAWN + 1);
            for (uint32_t i = 0; i < kSafehouses; ++i)
            {
                const float sx = globalf(ram, GA_SAFEHOUSE_X + i), sy = globalf(ram, GA_SAFEHOUSE_Y + i);
                if ((sx != 0.0f || sy != 0.0f) && std::fabs(sx - x) < 2.0f && std::fabs(sy - y) < 2.0f)
                    return static_cast<int>(i);
            }
            return -1;
        }

        void place(uint8_t *ram, float x, float y, float z, float heading, bool outdoors)
        {
            Code c;
            if (outdoors)
                c.op(SET_AREA_VISIBLE).i(0);
            c.op(LOAD_SCENE).f(x).f(y).f(z);
            c.op(SET_CHAR_COORDINATES).player().f(x).f(y).f(z);
            c.op(SET_CHAR_HEADING).player().f(heading);
            c.op(SET_CAMERA_BEHIND_PLAYER).op(RESTORE_CAMERA_JUMPCUT);
            const std::vector<uint8_t> bytes = c.done();
            vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), kScriptName);
        }

        void restore(uint8_t *ram, uint32_t time, bool running, bool inGame)
        {
            switch (g_restore)
            {
            case Restore::Idle:
                return;
            case Restore::Armed:
            {
                if (!inGame)
                    return;
                if (!running || global(ram, G_SAVING) || global(ram, G_SAVING2))
                    return; // INITSAV still at it (it puts the player at G678..680)
                g_restore = Restore::Idle;
                g_haveData = takeData(ram, g_data);
                if (!g_haveData)
                {
                    // an older auto save made outside its safehouse: into the safehouse of its spawn point
                    if (static_cast<int32_t>(global(ram, G_IN_SAFEHOUSE)) != -1 ||
                        static_cast<int32_t>(global(ram, G_IN_EMPIRE)) != -1)
                        return;
                    const int i = safehouseOfSpawn(ram);
                    if (i < 0)
                        return;
                    const uint32_t s = vcs::rig::startScriptAt(ram, kEnterSafehouse, kScriptName, kTerminateIp);
                    if (!s)
                        return;
                    wr32(ram, s + kLocal0, static_cast<uint32_t>(i));
                    g_legacySafehouse = static_cast<uint32_t>(i);
                    g_restoreAt = time;
                    g_restore = Restore::Legacy;
                    std::fprintf(stderr, "[quicksave] an auto save outside safehouse %d: into it\n", i);
                    return;
                }
                const Data &d = g_data;
                std::fprintf(stderr, "[quicksave] loaded a quick save at %.1f %.1f %.1f area %u vehicle %u\n", d.x, d.y, d.z,
                             d.area, d.vehicle);
                g_restores.fetch_add(1, std::memory_order_relaxed);
                if (d.vehicle && d.model)
                {
                    Code c;
                    c.op(REQUEST_MODEL).i(static_cast<int32_t>(d.model));
                    const std::vector<uint8_t> bytes = c.done();
                    vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), kScriptName);
                    g_restore = Restore::Model;
                    g_restoreAt = time;
                }
                g_haveData = false;
                return;
            }
            case Restore::Model:
            {
                if (!inGame)
                    return;
                const Data &d = g_data;
                if (!modelLoaded(ram, d.model))
                {
                    if (running && time - g_restoreAt > kModelTimeoutMs)
                    {
                        std::fprintf(stderr, "[quicksave] the vehicle's model %u did not load\n", d.model);
                        g_restore = Restore::Idle;
                    }
                    return;
                }
                Code c;
                c.op(CREATE_CAR).i(static_cast<int32_t>(d.model)).f(d.vx).f(d.vy).f(d.vz).local(0);
                c.op(SET_CAR_HEADING).local(0).f(d.vheading);
                if (d.vehicle == 1)
                    c.op(WARP_CHAR_INTO_CAR).player().local(0);
                c.op(MARK_MODEL_AS_NO_LONGER_NEEDED).i(static_cast<int32_t>(d.model));
                const std::vector<uint8_t> bytes = c.done();
                g_carScript = vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), kScriptName);
                g_restore = g_carScript ? Restore::Car : Restore::Idle;
                return;
            }
            case Restore::Legacy:
            {
                if (!inGame)
                    return;
                if (global(ram, G_IN_SAFEHOUSE) == g_legacySafehouse)
                {
                    // the safehouse's own spawn point; LOAD_SCENE first: its collision (else through the floor)
                    const uint32_t space = rd32(ram, kScriptSpacePtr), i = g_legacySafehouse;
                    const auto at = [&](uint32_t a) { return rdf(ram, space + 4u * (a + i)); };
                    place(ram, at(GA_SAFEHOUSE_X), at(GA_SAFEHOUSE_Y), at(GA_SAFEHOUSE_Z), at(GA_SAFEHOUSE_HEADING), false);
                    g_legacyFixes.fetch_add(1, std::memory_order_relaxed);
                    g_restore = Restore::Idle;
                }
                else if (running && time - g_restoreAt > kModelTimeoutMs)
                    g_restore = Restore::Idle;
                return;
            }
            case Restore::Car:
            {
                if (vcs::rig::scriptActive(ram, g_carScript))
                    return;
                // the car script's output: its handle in L0 (the slot keeps its locals until the next script there)
                const uint32_t handle = rd32(ram, g_carScript + kLocal0);
                const uint32_t car = vehicleAt(ram, handle);
                if (car)
                {
                    Code c;
                    c.op(MARK_CAR_AS_NO_LONGER_NEEDED).i(static_cast<int32_t>(handle));
                    const std::vector<uint8_t> bytes = c.done();
                    vcs::rig::startCodeScript(ram, bytes.data(), bytes.size(), kScriptName);
                }
                else
                    std::fprintf(stderr, "[quicksave] the vehicle was not created\n");
                g_restore = Restore::Idle;
                return;
            }
            }
        }
    }

    void onHostFrame(bool menus)
    {
        const int key = settings::value("controls.quicksave_key");
        if (!menus && key > 0 && (platform::keyPressed(key) || ps2control::keyPressed(key)))
            g_keyRequest.store(true, std::memory_order_relaxed);
    }

    void onGameFrame(uint8_t *ram)
    {
        const uint32_t time = rd32(ram, kTimeMs);
        const bool running = time != g_lastTime;
        g_lastTime = time;
        const uint32_t ped = rd32(ram, kPlayerPed);
        const bool inGame = rd32(ram, kScriptSpacePtr) != 0 && ped != 0;

        // a load (at boot or from the menu): the game comes up again, or its clock goes back
        if ((inGame && !g_wasInGame) || (running && time < g_lastLoadCheck))
        {
            g_restore = Restore::Armed;
            g_haveData = false;
            g_lastVehicle = 0;
            if (g_state == State::Saving)
                g_state = State::Idle, g_saveScript = 0;
        }
        g_wasInGame = inGame;
        if (running)
            g_lastLoadCheck = time;
        restore(ram, time, running, inGame);

        if (inGame)
            if (const uint32_t v = rd32(ram, ped + kPedVehicle))
                g_lastVehicle = v, g_lastVehicleModel = rd32(ram, v + kModel) & 0xFFFFu;

        if (g_savePointRequest.exchange(false, std::memory_order_relaxed) && inGame)
            vcs::rig::startScriptAt(ram, kSavePointCode, "savepnt", kTerminateIp);
        const bool key = g_keyRequest.exchange(false, std::memory_order_relaxed);
        switch (g_state)
        {
        case State::Idle:
            if (key && inGame && running)
                startSave(ram, time, ped);
            return;
        case State::Saving:
        {
            const bool saving = global(ram, G_SAVING) || global(ram, G_SAVING2);
            if (saving)
                g_sawSaving = true;
            else if (g_sawSaving)
            {
                std::fprintf(stderr, "[quicksave] done, clock %02u:%02u (was %02u:%02u)\n", ram[kClock], ram[kClock + 1],
                             g_clockAtSave[0], g_clockAtSave[1]);
                g_lastSavedClock.store(ram[kClock] | (ram[kClock + 1] << 8), std::memory_order_relaxed);
                giveBack(ram);
                g_saves.fetch_add(1, std::memory_order_relaxed);
                g_state = State::Idle;
                g_saveScript = 0;
            }
            else if (running && time - g_startedAt > kStartTimeoutMs)
            {
                std::fprintf(stderr, "[quicksave] the save did not start\n");
                giveBack(ram);
                g_state = State::Idle;
                g_saveScript = 0;
            }
            return;
        }
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.quicksave";
        s.page = "Enhancements";
        s.section = "Saving";
        s.label = "Quick save";
        s.help = "The quick save key (Keyboard > Menus, F12) opens the game's Save menu anywhere outside a mission, on foot "
                 "or in a vehicle: choose a slot, or Cancel. The save keeps where you are (in a safehouse or an Empire "
                 "building too), your vehicle - or the last one you drove, if it is near - and the time of day; Load Game "
                 "brings you back there. Not during a mission, a cutscene or with the police after you. Off: as the "
                 "original game, saving only at safehouses.";
        s.type = settings::Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_QUICKSAVE";
        s.onChange = [](int v) { g_enabled.store(v != 0, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "controls.quicksave_key";
        s.page = "Keyboard";
        s.section = "Menus";
        s.label = "Quick save";
        s.help = "Opens the game's Save menu where you are (Enhancements > Saving > Quick save turns it on). F12 is also "
                 "Steam's screenshot key: pick another one when the game runs through Steam.";
        s.type = settings::Type::Key;
        s.group = "keys";
        s.defaultValue = platform::KEY_F12;
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("VCS_QUICKSAVE_HOOKS"); env && env[0] == '0')
            std::fprintf(stderr, "[quicksave] off (VCS_QUICKSAVE_HOOKS=0)\n");
        else if ((g_origActivate = runtime.lookupFunction(kActivateSaveMenu)) &&
                 (g_origClock = runtime.lookupFunction(kAdvanceClock)))
        {
            runtime.replaceFunction(kActivateSaveMenu, &activateSaveMenu);
            runtime.replaceFunction(kAdvanceClock, &advanceClock);
            g_installed = true;
        }
        else
            std::fprintf(stderr, "[quicksave] the game's functions not found: no quick save\n");

        // quicksave -> ok state; quicksave now = the key; quicksave savepoint = a normal safehouse save, the barracks'
        // save point's own code (main script 0x12213), as walking onto its disk does (tests)
        ps2control::addCommand("quicksave", [](const std::string &args, ps2control::Reply reply) {
            if (args.find("now") != std::string::npos)
            {
                g_keyRequest.store(true, std::memory_order_relaxed);
                return reply("ok");
            }
            if (args.find("savepoint") != std::string::npos)
            {
                g_savePointRequest.store(true, std::memory_order_relaxed);
                return reply("ok");
            }
            const uint32_t clock = g_lastSavedClock.load();
            char text[256];
            std::snprintf(text, sizeof(text),
                          "ok hooked=%d on=%d saving=%d saves=%d refused=%d last_refusal=%s restores=%d legacy_fixes=%d "
                          "clock_skips=%d last_clock=%02u:%02u",
                          g_installed ? 1 : 0, g_enabled.load() ? 1 : 0, g_state == State::Saving ? 1 : 0, g_saves.load(),
                          g_refused.load(), g_lastRefusal.load(), g_restores.load(), g_legacyFixes.load(),
                          g_clockSkips.load(), clock & 0xFFu, (clock >> 8) & 0xFFu);
            reply(text);
        });
    }
}
