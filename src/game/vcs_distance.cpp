// Draw distances and the population as settings (docs/game-internals.md 9h) - Project2DFX's DrawDistance and the
// limit adjuster's pools, done the game's own way. Five sliders on Graphics > Distance and population, 0 (the game's
// own) to 100 (the most), as GTA IV's: view distance, detail distance, population distance, vehicle density,
// pedestrian density.
//
// View distance. VCS draws its entities (objects, the ped crowds, anything with a model info and LOD distances) as VC
// does: CRenderer's SetupEntityVisibility (FUN_0035EFD8) picks a model's atomic by `distance < lodDistance[i] *
// LODDistMultiplier` (GetAtomicFromDistance FUN_003A52F8, GetLargestLodDistance FUN_003A5380; the model info's LOD
// distances at +0x2C, its atomics at +0x28), fades it in past the last one and has it streamed 50 m before. The camera's
// LODDistMultiplier (0x6F4CAC, camera +0x7DC) is rebuilt every frame in CCamera::Process (FUN_00378DD8): 70 / FOV, more
// above 55 m, at most 2, copied to GenerationDistMultiplier (+0x7D0: the population's distances), then times
// CRenderer::ms_lodDistScale (0x4887F8) - a constant 1.0 that nothing writes and only CCamera::Process reads. Writing the
// view factor there before each CCamera::Process scales every entity's draw (and stream) distance and nothing else; the
// game's 1.0 at 0. The city itself (the streamed sectors, game/vcs_world.cpp) has no distance LOD.
//
// Detail distance. The last of a model's LOD distances is where it stops being drawn (the view distance), the ones
// before it where it changes to a less detailed atomic. GetAtomicFromDistance (a leaf) is done here when the two
// settings differ: the earlier distances times detail / view, at most the last one. The game's own code when they match.
//
// Population distance. GenerationDistMultiplier is read by seven functions: cars made 100-160 x and removed past 160 x
// (FUN_00430290, FUN_00433378), peds made 35-60 x and removed past 51-65 x (FUN_002AFE80, FUN_002B0200), the vehicles'
// LOD and fade distances 60-150 x (FUN_0036EBF8, VC's CVisibilityPlugins), FUN_003DA070 and FUN_003F29D8. The game
// thread runs them in CGame::Process and the render's setup, never inside CCamera::Process, which writes the value: the
// first of them after a CCamera::Process takes the fresh value as the frame's base (-1 written before CCamera::Process:
// still -1 means it wrote none and the previous base stays), and each of them, at its entry, writes the base times its
// own factor - the setting's for the traffic, at most 2 for the peds (small, gone in the fog, the costliest to simulate).
// Nothing is written while both factors are 1 from the start.
//
// Density. The numbers of cars and peds are the zone's density times the scripts' multipliers (0x48A968 cars,
// 0x487FF8 peds: missions set them) times two constants the PS2 version keeps low - 0x48A680 cars 0.7, 0x48A67C peds
// 0.4 (as VC's CarNumberMultiplier / PedNumberMultiplier) - capped by MaxNumberOfCarsInUse 0x48A9A8 (16) and the peds'
// 0x488004 / 0x488008 (25 / 30); GenerateRandomCars also stops at 30 random cars (a constant in the code, patched to 127
// in game/vcs/overrides.toml). The settings scale the constants and the caps by the density times the population
// distance squared (the same density over the larger area); the scripts' multipliers stay theirs. The caps share the
// pools: VCS has room for 70 peds and 40 vehicles (CPools::Initialise FUN_00288100); with the streaming heap in the upper
// memory (game/vcs_memory.h: the main heap keeps the 5.4 MB) the pools are built for 280 / 200 instead (their
// constructors FUN_00478F28 / FUN_00478FC0, called only from there, given the larger size).
//
// Traffic far away (game-internals.md 9h, random traffic): offscreen cars are kept within 60 m x the population factor
// (patched constants read from 0x7FF40..), the spot finder's node list moves ahead along its direction, and the road
// classes' spawn chances scale with the number of cars wanted.
#include "game/vcs_distance.h"

#include "game/vcs_memory.h"
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
#include <utility>

namespace vcs::distance
{
    namespace
    {
        constexpr uint32_t kCameraProcess = 0x00378DD8u; // CCamera::Process
        constexpr uint32_t kLodDistScale = 0x004887F8u;  // CRenderer::ms_lodDistScale (1.0)
        constexpr uint32_t kLodMultiplier = 0x006F4CACu; // TheCamera.LODDistMultiplier (camera 0x6F44D0 + 0x7DC)
        constexpr uint32_t kGenMultiplier = 0x006F4CA0u; // TheCamera.GenerationDistMultiplier (+0x7D0)
        constexpr uint32_t kAtomicFromDistance = 0x003A52F8u; // CBaseModelInfo::GetAtomicFromDistance (leaf)

        // the functions that read GenerationDistMultiplier (not FUN_00219898: its +0x7D0 is a ped's); the first two are
        // the peds'
        constexpr uint32_t kGenReaders[] = {0x002AFE80u, 0x002B0200u, 0x0036EBF8u, 0x003DA070u, 0x003F29D8u, 0x00430290u, 0x00433378u};
        constexpr size_t kReaderCount = sizeof(kGenReaders) / sizeof(kGenReaders[0]);
        constexpr size_t kPedReaders = 2;
        constexpr float kMaxPedDistance = 2.0f; // the peds' population distance factor at most

        constexpr uint32_t kCarNumber = 0x0048A680u; // float, 0.7
        constexpr uint32_t kPedNumber = 0x0048A67Cu; // float, 0.4
        constexpr uint32_t kMaxCars = 0x0048A9A8u;   // int, 16
        constexpr uint32_t kMaxPeds = 0x00488004u;   // int, 25
        constexpr uint32_t kMaxPedsB = 0x00488008u;  // int, 30
        constexpr uint32_t kRandomCars = 0x0048A96Cu, kPeds = 0x004CD550u; // the counts the caps are compared with
        // guest RAM the patched GenerateOneRandomCar / vehicle removal read their offscreen distances from (60, 90, 70 m;
        // game/vcs/overrides.toml) - the EE kernel's area below the game, next to vcs_physics_timing's 0x7FF00..0x7FF0C
        constexpr uint32_t kOffscreenSlots = 0x0007FF40u;
        constexpr uint32_t kRoadDensity = 0x00487498u; // bytes: a road density class's spawn chance (0, 4, 7, 15)

        constexpr uint32_t kPedPoolCtor = 0x00478F28u, kCarPoolCtor = 0x00478FC0u;   // (memory, size, name)
        constexpr uint32_t kPedPoolReturn = 0x00288144u, kCarPoolReturn = 0x00288168u; // their calls in FUN_00288100
        constexpr uint32_t kPedPool = 0x00487A94u, kCarPool = 0x00487A98u;           // CPool *: +8 size
        // 280 peds (VC's limit adjusters' usual) and 200 vehicles: +1.1 MB of the main heap, which has the 5.4 MB the PS2's
        // streaming heap took. Above 128 peds a load needs the world clear's fix below (pedGetAt).
        constexpr uint32_t kLargePeds = 280u, kLargeCars = 200u;
        constexpr uint32_t kCarsKept = 25u; // vehicle slots left for missions, parked cars, the player's
        constexpr uint32_t kPedsKept = 30u; // ped slots left for missions and the player, besides a driver a car

        PS2Runtime::RecompiledFunction g_original = nullptr, g_pedPoolCtor = nullptr, g_carPoolCtor = nullptr;
        PS2Runtime::RecompiledFunction g_atomicFromDistance = nullptr;
        PS2Runtime::RecompiledFunction g_readers[kReaderCount] = {};
        // percent of the game's: object draw distance (view), detail distance, population distance, densities
        std::atomic<int> g_view{100}, g_detail{100}, g_population{100}, g_traffic{100}, g_pedestrians{100};
        uint8_t *g_rdram = nullptr;
        // the population distance on GenerationDistMultiplier (game thread only): CCamera::Process has run since the last
        // reader; the value is ours to keep (a factor other than 1 since); the game's value this frame (unscaled)
        bool g_genFresh = false, g_genTouched = false;
        float g_genBase = 1.0f;
        constexpr float kGenUnwritten = -1.0f; // written before CCamera::Process: still there = it wrote none this frame

        // the game's constants, read once
        bool g_haveDefaults = false;
        float g_carNumber = 0.7f, g_pedNumber = 0.4f;
        int32_t g_maxCars = 16, g_maxPeds = 25, g_maxPedsB = 30;
        uint8_t g_roadDensity[4] = {0, 4, 7, 15};

        float rdf(uint32_t addr)
        {
            float f;
            std::memcpy(&f, g_rdram + (addr & PS2_RAM_MASK), 4);
            return f;
        }
        int32_t rd32(uint32_t addr)
        {
            int32_t v;
            std::memcpy(&v, g_rdram + (addr & PS2_RAM_MASK), 4);
            return v;
        }
        void wrf(uint32_t addr, float f) { std::memcpy(g_rdram + (addr & PS2_RAM_MASK), &f, 4); }
        void wr32(uint32_t addr, int32_t v) { std::memcpy(g_rdram + (addr & PS2_RAM_MASK), &v, 4); }

        uint32_t poolSize(uint32_t global)
        {
            const uint32_t pool = static_cast<uint32_t>(rd32(global));
            return pool ? static_cast<uint32_t>(rd32(pool + 8u)) : 0u;
        }

        float percent(const std::atomic<int> &v) { return static_cast<float>(v.load(std::memory_order_relaxed)) / 100.0f; }
        float carDistance() { return percent(g_population); }
        float pedDistance() { return std::min(percent(g_population), kMaxPedDistance); }

        // GenerateOneRandomCar gives up on a found spot unless rand() & 15 <= the road's density class value (the lower
        // of the link's two nodes): 1, 5, 8 and 16 chances in 16. More cars over a larger area need more spots taken -
        // the chance times the traffic factor, at most every time.
        void applyRoadDensity(float factor)
        {
            for (uint32_t i = 0; i < 4; ++i)
            {
                const long v = std::lround((g_roadDensity[i] + 1) * factor) - 1;
                g_rdram[(kRoadDensity + i) & PS2_RAM_MASK] = static_cast<uint8_t>(std::clamp<long>(v, 0, 15));
            }
        }

        // the numbers of cars and peds: the game's constants times the density and the population distance squared,
        // the caps within the pools
        void applyDensity()
        {
            if (!g_haveDefaults)
            {
                g_carNumber = rdf(kCarNumber);
                g_pedNumber = rdf(kPedNumber);
                g_maxCars = rd32(kMaxCars);
                g_maxPeds = rd32(kMaxPeds);
                g_maxPedsB = rd32(kMaxPedsB);
                std::memcpy(g_roadDensity, g_rdram + (kRoadDensity & PS2_RAM_MASK), 4);
                g_haveDefaults = true;
            }
            const float cars = percent(g_traffic) * carDistance() * carDistance();
            const float peds = percent(g_pedestrians) * pedDistance() * pedDistance();
            if (cars == 1.0f && peds == 1.0f)
            {
                wrf(kCarNumber, g_carNumber); // the game's own (a setting back at 0)
                wrf(kPedNumber, g_pedNumber);
                wr32(kMaxCars, g_maxCars);
                wr32(kMaxPeds, g_maxPeds);
                wr32(kMaxPedsB, g_maxPedsB);
                applyRoadDensity(1.0f);
                return;
            }
            // the caps share the ped pool: every random car takes a driver from it, the ambient peds the rest (less the
            // slots kept for missions and the player). Each keeps up to the game's own cap; what the settings ask beyond
            // that shares the room left, in proportion.
            const int32_t carPool = static_cast<int32_t>(poolSize(kCarPool)), pedPool = static_cast<int32_t>(poolSize(kPedPool));
            const int32_t carRoom = std::max<int32_t>(g_maxCars, carPool - static_cast<int32_t>(kCarsKept));
            int32_t maxCars = std::clamp(static_cast<int32_t>(std::lround(g_maxCars * cars)), 0, carRoom);
            int32_t maxPeds = std::max<int32_t>(0, static_cast<int32_t>(std::lround(g_maxPedsB * peds)));
            const int32_t pedRoom = std::max<int32_t>(g_maxCars + g_maxPedsB, pedPool - static_cast<int32_t>(kPedsKept));
            const int32_t baseCars = std::min(maxCars, g_maxCars), basePeds = std::min(maxPeds, g_maxPedsB);
            const int32_t extraCars = maxCars - baseCars, extraPeds = maxPeds - basePeds;
            const int32_t rest = pedRoom - baseCars - basePeds;
            if (extraCars + extraPeds > rest)
            {
                const double f = static_cast<double>(rest) / static_cast<double>(extraCars + extraPeds);
                maxCars = baseCars + static_cast<int32_t>(extraCars * f);
                maxPeds = basePeds + static_cast<int32_t>(extraPeds * f);
            }
            wrf(kCarNumber, g_carNumber * cars);
            wrf(kPedNumber, g_pedNumber * peds);
            wr32(kMaxCars, maxCars);
            wr32(kMaxPeds, static_cast<int32_t>(std::lround(maxPeds * static_cast<double>(g_maxPeds) / g_maxPedsB)));
            wr32(kMaxPedsB, maxPeds);
            applyRoadDensity(cars);
        }

        // before the original (DECISIONS.md, hooks): it reads the scale once, near its end, and writes the multipliers
        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            wrf(kLodDistScale, percent(g_view));
            applyDensity();
            g_genFresh = true;
            if (carDistance() != 1.0f || g_genTouched)
            {
                g_genTouched = true;
                wrf(kGenMultiplier, kGenUnwritten);
            }
            g_original(rdram, ctx, runtime);
        }

        // a reader of GenerationDistMultiplier: the frame's base (the game's value) times this reader's factor
        template <size_t I>
        PS2_GUEST_CC void genReader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // the offscreen vehicle distances the patched code reads (game/vcs/overrides.toml): always written, the RAM
            // there starts at zero
            g_rdram = rdram;
            const float cars = carDistance();
            wrf(kOffscreenSlots, 60.0f * cars);
            wrf(kOffscreenSlots + 4u, 90.0f * cars);
            wrf(kOffscreenSlots + 8u, 70.0f * cars);
            if (g_genTouched)
            {
                if (g_genFresh)
                {
                    const float v = rdf(kGenMultiplier);
                    g_genBase = v == kGenUnwritten ? g_genBase : v;
                }
                wrf(kGenMultiplier, g_genBase * (I < kPedReaders ? pedDistance() : cars));
                if (cars == 1.0f) // back at the game's own: this frame keeps it, the next is left alone
                    g_genTouched = false;
            }
            g_genFresh = false;
            g_readers[I](rdram, ctx, runtime);
        }

        template <size_t... I>
        void hookReaders(PS2Runtime &runtime, std::index_sequence<I...>)
        {
            const PS2Runtime::RecompiledFunction hooks[] = {&genReader<I>...};
            for (size_t i = 0; i < kReaderCount; ++i)
                if ((g_readers[i] = runtime.lookupFunction(kGenReaders[i])))
                    runtime.replaceFunction(kGenReaders[i], hooks[i]);
        }

        // GetAtomicFromDistance(model info, distance): the first atomic from the start index (flags +0x3A: & 3 when bit 3
        // is set, else 0) to the count (+0x38) whose LOD distance (+0x2C) times LODDistMultiplier is beyond the distance,
        // 0 past the last. With a detail distance other than the view's, the distances before the last are scaled by
        // detail / view (at most the last); the original otherwise.
        PS2_GUEST_CC void atomicFromDistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float view = percent(g_view), detail = percent(g_detail);
            if (detail == view)
                return g_atomicFromDistance(rdram, ctx, runtime);
            g_rdram = rdram;
            const uint32_t mi = GPR_U32(ctx, 4);
            const float distance = ctx->f[12];
            const uint32_t atomics = static_cast<uint32_t>(rd32(mi + 0x28u));
            uint32_t result = 0;
            if (atomics)
            {
                uint16_t flags;
                std::memcpy(&flags, rdram + ((mi + 0x3Au) & PS2_RAM_MASK), 2);
                const int count = rdram[(mi + 0x38u) & PS2_RAM_MASK];
                const float mult = rdf(kLodMultiplier);
                const float last = count > 0 ? rdf(mi + 0x2Cu + 4u * static_cast<uint32_t>(count - 1)) * mult : 0.0f;
                for (int i = (flags & 8u) ? (flags & 3u) : 0; i < count; ++i)
                {
                    float limit = rdf(mi + 0x2Cu + 4u * static_cast<uint32_t>(i)) * mult;
                    if (i < count - 1)
                        limit = std::min(limit * detail / view, last);
                    if (distance < limit)
                    {
                        result = static_cast<uint32_t>(rd32(atomics + 4u * static_cast<uint32_t>(i)));
                        break;
                    }
                }
            }
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // The spot finder FUN_00267928(x, y, dirx, diry, distance, cos, nodes, ...) looks for a road node `distance` (110 x
        // GenerationDistMultiplier for traffic) ahead along dir, among a list it rebuilds every 64 frames with FUN_00269228:
        // up to 250 nodes in a square of +-310 m around the player, in grid order. With the population distance the ring
        // in view a new car has to stand in (100-160 x gen) moves past that square, and nearly every new car was deleted.
        // The list's centre moves ahead instead, distance x (1 - 1 / factor) along the finder's direction: the same square,
        // so no more nodes than the game's, over the ring. The game's own at 0.
        constexpr uint32_t kSpotFinder = 0x00267928u, kSpotFinderEnd = 0x00267E98u, kNodeList = 0x00269228u;
        PS2Runtime::RecompiledFunction g_spotFinder = nullptr, g_nodeList = nullptr;
        float g_spotDir[2] = {0.0f, 0.0f}, g_spotDistance = 0.0f;
        PS2_GUEST_CC void spotFinder(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_spotDir[0] = ctx->f[14];
            g_spotDir[1] = ctx->f[15];
            g_spotDistance = ctx->f[16];
            g_spotFinder(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void nodeList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float k = carDistance();
            const uint32_t ra = GPR_U32(ctx, 31);
            if (k > 1.0f && ra >= kSpotFinder && ra < kSpotFinderEnd)
            {
                g_rdram = rdram;
                const uint32_t pos = GPR_U32(ctx, 5); // the centre (x, y) on the finder's stack
                const float shift = g_spotDistance * (1.0f - 1.0f / k);
                wrf(pos, rdf(pos) + g_spotDir[0] * shift);
                wrf(pos + 4u, rdf(pos + 4u) + g_spotDir[1] * shift);
            }
            g_nodeList(rdram, ctx, runtime);
        }

        // FUN_0018F5D0 (the world's clear before a load / restart) deletes the peds with `for (i = 0; i < pool size; i++)
        // if ((ped = GetAt(pool, i))) delete ped;` - GetAt FUN_00471788 takes a handle (slot << 8 | the slot's flag byte),
        // so i is slot 0 with flag byte i: below 128 that matches slot 0's 7-bit id when it is in use (the PS2 deletes
        // that one ped; the pool's flags are reset after). From 128 up it matches slot 0's byte when it is FREE (bit 7):
        // with more than 128 peds a load "deleted" a ped that was not there and corrupted memory - freezes within
        // seconds, the heap's count negative, broken atomic lists (game-internals.md 9h). The call from there gets an
        // invalid handle for i >= 128: what the PS2's 70 slots did.
        constexpr uint32_t kPedGetAt = 0x00471788u, kClearGetAtReturn = 0x0018F870u;
        PS2Runtime::RecompiledFunction g_pedGetAt = nullptr;
        PS2_GUEST_CC void pedGetAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kClearGetAtReturn && GPR_U32(ctx, 5) >= 128u)
                SET_GPR_U32(ctx, 5, 0xFFFFFFFFu);
            g_pedGetAt(rdram, ctx, runtime);
        }

        // the pools' constructors, from CPools::Initialise only: larger pools when the main heap has the room.
        // VCS_POOLS=peds,vehicles sets the sizes for a test (0 or missing: the game's).
        bool largePools() { return vcs::memory::streamingMegabytes() > 0; }
        uint32_t poolSizeFor(uint32_t requested, int which, uint32_t large)
        {
            if (const char *env = std::getenv("VCS_POOLS"))
            {
                unsigned peds = 0, cars = 0;
                std::sscanf(env, "%u,%u", &peds, &cars);
                const unsigned want = which ? cars : peds;
                return want ? want : requested;
            }
            return largePools() ? std::max(requested, large) : requested;
        }
        PS2_GUEST_CC void pedPoolCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kPedPoolReturn)
                SET_GPR_U32(ctx, 5, poolSizeFor(GPR_U32(ctx, 5), 0, kLargePeds));
            g_pedPoolCtor(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void carPoolCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kCarPoolReturn)
                SET_GPR_U32(ctx, 5, poolSizeFor(GPR_U32(ctx, 5), 1, kLargeCars));
            g_carPoolCtor(rdram, ctx, runtime);
        }

        // Crowd LOD (the simulation by distance, as GTA IV / Project2DFX keep far things cheap). CWorld::Process
        // (FUN_001933C0) runs, for every moving entity, UpdateAnim (vtable +0x48, CEntity's FUN_003C50A8), ProcessControl
        // (+0x30; a random ped's FUN_002D8788) and ProcessCollision (+0x34, CPhysical's FUN_00348D40), one pass each,
        // in order. With the population distance most peds are beyond the PS2's own ped range (~65 m), and each costs
        // ~0.06 ms a frame (game-internals.md 9h). A random ped on foot farther than kLodDistance from the camera is
        // processed every second frame (every third past kLodFar; its slot against the frame counter spreads them) and
        // skips all three on the others. Its animation then advances by that many frames' time (UpdateAnim passes the
        // step to FUN_00154D10 in $f12 - a register, no global changes): the ped's speed comes from its animation's root
        // movement over the frame's step (FUN_001F1D38), so it walks the same distance a second and turns at a fraction
        // of the rate - at 70 m and more. And a random ped outside the camera's cone gets the root-only animation update
        // the game already gives vehicles and objects out of view ($a1 = 0; for peds UpdateAnim always asks the full one).
        // Only while a population slider is above 0: the game's own otherwise.
        // VCS_CROWD_LOD=0 turns it off (A / B measures).
        constexpr uint32_t kUpdateAnim = 0x003C50A8u, kRandomPedControl = 0x002D8788u, kProcessCollision = 0x00348D40u;
        constexpr uint32_t kAnimUpdate = 0x00154D10u; // RpAnimBlendClumpUpdateAnimations(dt $f12, clump $a0, all $a1)
        constexpr uint32_t kFrameCounter = 0x004CD124u, kCameraPos = 0x006F3C00u;
        constexpr uint32_t kCameraFront = 0x006F44E0u; // TheCamera's matrix: forward (+0x10)
        constexpr float kLodDistance = 70.0f, kLodFar = 110.0f; // every second frame past the first, every third past this
        PS2Runtime::RecompiledFunction g_updateAnim = nullptr, g_randomPedControl = nullptr, g_processCollision = nullptr,
                                       g_animUpdate = nullptr;
        struct LodState
        {
            int32_t frame = -1;
            uint8_t every = 1;     // processed one frame in `every` (1: every frame, not a LOD ped)
            bool skip = false;     // not this frame
            bool unseen = false;   // a random ped outside the camera's cone: the cheap animation update
        };
        LodState g_lod[kLargePeds];
        uint32_t g_animClump = 0;  // the clump the next animation update is for, and how
        float g_animTimes = 1.0f;
        bool g_animCheap = false;
        uint64_t g_lodSkipped = 0, g_lodProcessed = 0, g_lodCheap = 0;
        bool crowdLodOn()
        {
            static const bool on = !(std::getenv("VCS_CROWD_LOD") && std::getenv("VCS_CROWD_LOD")[0] == '0');
            return on;
        }
        // this frame's decision for a ped (nullptr: not a ped, or the LOD off)
        const LodState *crowdLod(uint32_t ent)
        {
            const uint32_t pool = static_cast<uint32_t>(rd32(kPedPool));
            const bool larger = g_population.load(std::memory_order_relaxed) > 100 ||
                                g_traffic.load(std::memory_order_relaxed) > 100 ||
                                g_pedestrians.load(std::memory_order_relaxed) > 100;
            if (!pool || !crowdLodOn() || !larger)
                return nullptr;
            const uint32_t entries = static_cast<uint32_t>(rd32(pool)), size = static_cast<uint32_t>(rd32(pool + 8u));
            if (ent < entries || ent >= entries + size * 0xD20u || (ent - entries) % 0xD20u)
                return nullptr;
            const uint32_t slot = (ent - entries) / 0xD20u;
            if (slot >= kLargePeds)
                return nullptr;
            LodState &s = g_lod[slot];
            const int32_t frame = rd32(kFrameCounter);
            if (s.frame != frame)
            {
                s.frame = frame;
                const int32_t createdBy = rd32(ent + 0x8F4u), state = rd32(ent + 0x8C4u);
                const bool random = createdBy == 1 && state != 0x37 && state != 0x39 && state != 0x3A && rd32(ent + 0x538u) == 0;
                const float dx = rdf(ent + 0x30u) - rdf(kCameraPos), dy = rdf(ent + 0x34u) - rdf(kCameraPos + 4u);
                const float dz = rdf(ent + 0x38u) - rdf(kCameraPos + 8u);
                const float d2 = dx * dx + dy * dy;
                s.every = !random ? 1 : d2 > kLodFar * kLodFar ? 3 : d2 > kLodDistance * kLodDistance ? 2 : 1;
                s.skip = (static_cast<uint32_t>(frame) + slot) % s.every != 0;
                // outside a cone of acos(0.2) = 78 degrees around the camera's direction (wider than any field of view)
                const float along = dx * rdf(kCameraFront) + dy * rdf(kCameraFront + 4u) + dz * rdf(kCameraFront + 8u);
                s.unseen = random && d2 > 25.0f && along < 0.2f * std::sqrt(d2 + dz * dz);
            }
            return &s;
        }
        PS2_GUEST_CC void skipCall(R5900Context *ctx) { ctx->pc = GPR_U32(ctx, 31); }
        PS2_GUEST_CC void updateAnim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            const uint32_t ent = GPR_U32(ctx, 4);
            const LodState *s = crowdLod(ent);
            g_animClump = 0;
            if (s)
            {
                if (s->skip)
                {
                    ++g_lodSkipped;
                    return skipCall(ctx);
                }
                g_lodProcessed += s->every > 1;
                g_lodCheap += s->unseen;
                g_animClump = static_cast<uint32_t>(rd32(ent + 0x50u));
                g_animTimes = static_cast<float>(s->every);
                g_animCheap = s->unseen;
            }
            g_updateAnim(rdram, ctx, runtime);
        }
        // UpdateAnim's call: the step times the frames it stands for; outside the camera's cone the root's movement only
        // ($a1 = 0: the update the game gives vehicles and objects out of view - FUN_001552F8, the root-motion node)
        PS2_GUEST_CC void animUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_animClump && GPR_U32(ctx, 4) == g_animClump)
            {
                ctx->f[12] *= g_animTimes;
                if (g_animCheap)
                    SET_GPR_U32(ctx, 5, 0u);
            }
            g_animClump = 0;
            g_animUpdate(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void randomPedControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (const LodState *l = crowdLod(GPR_U32(ctx, 4)); l && l->skip)
                return skipCall(ctx);
            g_randomPedControl(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void processCollision(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (const LodState *l = crowdLod(GPR_U32(ctx, 4)); l && l->skip)
                return skipCall(ctx);
            g_processCollision(rdram, ctx, runtime);
        }

        // The sector entries (EntryInfoNode: one per entity and 80 m world cell it touches, 0x14 bytes {list, ptr node,
        // sector, prev, next}, chained at the entity's +0xE4) come from a CPool of 5200 in the game image (GAME.DTZ's
        // header hands it to *0x487A90; FUN_003321F0). The city's buildings and dummies hold ~4810 of them; a ped or a
        // vehicle takes 1-4. With the larger crowds it reached 5020 (Little Havana, 143 peds, 41 vehicles) and nothing
        // checks a full pool: CWorld::Add / Move (FUN_00347348 / FUN_00347740) then write the node at address 0 and leave
        // the entity in a sector list without its back link - out of the collision scans, or a dangling pointer once it is
        // deleted. FUN_00478BF0 (New for the 0x14-byte pools: a ring search from +0xC, wrapping once) and FUN_00478C88
        // (Delete) are done here: for that pool, past its last free entry, from an overflow pool of kNodeOverflowCount in
        // the port's reserve at the top of the upper memory (game/vcs_memory.h); Delete knows both. The other pools, and
        // this one while it has room, run the original. (Open Limit Adjuster's EntryInfoNode, game-internals.md 9h.)
        constexpr uint32_t kNodeNew = 0x00478BF0u, kNodeDelete = 0x00478C88u, kEntryInfoPool = 0x00487A90u;
        constexpr uint32_t kNodeOverflowCount = 8000u, kNodeSize = 0x14u;
        constexpr uint32_t kNodeOverflowEntries = PS2_RAM_SIZE - 2u * 1024u * 1024u; // the reserve's start
        constexpr uint32_t kNodeOverflowFlags = kNodeOverflowEntries + kNodeOverflowCount * kNodeSize;
        PS2Runtime::RecompiledFunction g_nodeNew = nullptr, g_nodeDelete = nullptr;
        bool g_overflowReady = false;
        uint32_t g_overflowNext = kNodeOverflowCount - 1u, g_overflowUsed = 0, g_overflowPeak = 0;
        // the original's search on a pool {entries, flags, size, last}: the next free entry after `last`, wrapping once
        uint32_t poolNew(uint8_t *rdram, uint32_t entries, uint32_t flags, uint32_t size, uint32_t &last)
        {
            bool wrapped = false;
            for (;;)
            {
                if (++last == size)
                {
                    if (wrapped)
                        return 0;
                    last = 0;
                    wrapped = true;
                }
                uint8_t &f = rdram[(flags + last) & PS2_RAM_MASK];
                if (f & 0x80u)
                {
                    f = static_cast<uint8_t>((f & 0x7Fu) + 1u) & 0x7Fu;
                    return entries + last * kNodeSize;
                }
            }
        }
        PS2_GUEST_CC void nodeNew(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pool = GPR_U32(ctx, 4);
            g_rdram = rdram;
            if (pool != static_cast<uint32_t>(rd32(kEntryInfoPool)))
                return g_nodeNew(rdram, ctx, runtime);
            // VCS_NODE_OVERFLOW_TEST=1: every new entry from the overflow pool (its path checked without a crowd)
            static const bool test = std::getenv("VCS_NODE_OVERFLOW_TEST") != nullptr;
            uint32_t last = static_cast<uint32_t>(rd32(pool + 12u));
            uint32_t node = test ? 0u : poolNew(rdram, static_cast<uint32_t>(rd32(pool)), static_cast<uint32_t>(rd32(pool + 4u)),
                                    static_cast<uint32_t>(rd32(pool + 8u)), last);
            wr32(pool + 12u, static_cast<int32_t>(last));
            if (!node)
            {
                if (!g_overflowReady)
                {
                    std::memset(rdram + kNodeOverflowFlags, 0x80, kNodeOverflowCount);
                    g_overflowReady = true;
                    std::fprintf(stderr, "[distance] sector entries: the game's 5200 are in use, the overflow pool of %u "
                                         "at 0x%08x takes the next\n", kNodeOverflowCount, kNodeOverflowEntries);
                }
                node = poolNew(rdram, kNodeOverflowEntries, kNodeOverflowFlags, kNodeOverflowCount, g_overflowNext);
                if (node)
                    g_overflowPeak = std::max(g_overflowPeak, ++g_overflowUsed);
            }
            SET_GPR_U32(ctx, 2, node);
            ctx->pc = GPR_U32(ctx, 31);
        }
        PS2_GUEST_CC void nodeDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t node = GPR_U32(ctx, 5);
            if (node < kNodeOverflowEntries || node >= kNodeOverflowFlags)
                return g_nodeDelete(rdram, ctx, runtime);
            const uint32_t index = (node - kNodeOverflowEntries) / kNodeSize;
            rdram[kNodeOverflowFlags + index] |= 0x80u;
            if (index < g_overflowNext)
                g_overflowNext = index;
            --g_overflowUsed;
            ctx->pc = GPR_U32(ctx, 31);
        }

        // AddToPopulation(nearest, farthest in view, nearest, farthest out of view) (FUN_002B17A8) places a ped on a pavement
        // node in one of two rings around the player: in view 35-60 x GenerationDistMultiplier (scaled with the peds'
        // reader), out of view a fixed 10-20 m (CPopulation::Update FUN_002AFE80: factor x 20 - 10 .. factor x 20). After a
        // camera cut (a teleport, a load, a cutscene's end, an interior's door, a respawn) Update counts down 0x4CD3BA
        // and fills the streets at once: FUN_002B0070 calls it 100 times with 10 m .. factor x 60 for both. The ring out
        // of view is the easiest to fill, so with the larger caps the crowd gathered around the player: 92 of 152 peds
        // within 30 m in Little Havana, the densest and slowest frames (game thread 15-27 ms). The ring out of view, and
        // the fill's both, are times the peds' population distance here - the area the caps were scaled for: 42 of 82
        // within 30 m, 7 ms. The game's own at 0.
        constexpr uint32_t kAddToPopulation = 0x002B17A8u, kFill = 0x002B0070u, kFillEnd = 0x002B0200u;
        PS2Runtime::RecompiledFunction g_addToPopulation = nullptr;
        PS2_GUEST_CC void addToPopulation(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const float k = pedDistance();
            if (ra >= kFill && ra < kFillEnd)
            {
                ctx->f[12] *= k; // nearest in view
                ctx->f[13] *= k; // farthest in view
            }
            ctx->f[14] *= k; // nearest out of view
            ctx->f[15] *= k; // farthest out of view
            g_addToPopulation(rdram, ctx, runtime);
        }

        // a slider 0..100 (0 the game's own) to a percent of the game's, up to `most`
        void addSlider(const char *key, const char *label, const char *help, const char *env, int most, std::atomic<int> &to)
        {
            settings::Spec s;
            s.key = key;
            s.page = "Graphics";
            s.section = "Distance and population";
            s.label = label;
            s.help = help;
            s.type = settings::Type::Integer;
            s.min = 0;
            s.max = 100;
            s.format = "%d";
            s.defaultValue = 0;
            s.env = env;
            s.onChange = [&to, most](int v) { to.store(100 + (most - 100) * std::clamp(v, 0, 100) / 100, std::memory_order_relaxed); };
            settings::add(s);
        }
    }

    void registerSettings()
    {
        addSlider("world.view_distance", "View distance",
                  "How far away objects are drawn: 0 is the original, 100 ten times as far (props, trees, crowds - "
                  "anything the game fades in with distance; the city's buildings stream by area).",
                  "VCS_VIEW_DISTANCE", 1000, g_view);
        addSlider("world.detail_distance", "Detail distance",
                  "How far away objects keep their detailed models: 0 is the original, 100 ten times as far (never "
                  "beyond the view distance).",
                  "VCS_DETAIL_DISTANCE", 1000, g_detail);
        addSlider("world.population_distance", "Population distance",
                  "How far away cars and people appear and stay: 0 is the original, 100 three times as far for cars "
                  "(twice for people). Their numbers grow with the area, so the streets stay as busy.",
                  "VCS_POPULATION_DISTANCE", 300, g_population);
        addSlider("world.vehicle_density", "Vehicle density",
                  "How many cars drive around: 0 is the original (the PS2 keeps it low), 100 four times as many. Missions "
                  "still empty the streets when they want. More than the PS2 needs Streaming memory above PS2.",
                  "VCS_VEHICLE_DENSITY", 400, g_traffic);
        addSlider("world.pedestrian_density", "Pedestrian density",
                  "How many people walk around: 0 is the original (the PS2 keeps it low), 100 four times as many. "
                  "Missions still empty the streets when they want. More than the PS2 needs Streaming memory above PS2.",
                  "VCS_PEDESTRIAN_DENSITY", 400, g_pedestrians);
    }

    void install(PS2Runtime &runtime)
    {
        g_original = runtime.lookupFunction(kCameraProcess);
        if (!g_original)
        {
            std::fprintf(stderr, "[distance] CCamera::Process 0x%x not found: the draw distance settings do nothing\n", kCameraProcess);
            return;
        }
        runtime.replaceFunction(kCameraProcess, &cameraProcess);
        hookReaders(runtime, std::make_index_sequence<kReaderCount>{});
        if ((g_atomicFromDistance = runtime.lookupFunction(kAtomicFromDistance)))
            runtime.replaceFunction(kAtomicFromDistance, &atomicFromDistance);
        if ((g_pedPoolCtor = runtime.lookupFunction(kPedPoolCtor)))
            runtime.replaceFunction(kPedPoolCtor, &pedPoolCtor);
        if ((g_carPoolCtor = runtime.lookupFunction(kCarPoolCtor)))
            runtime.replaceFunction(kCarPoolCtor, &carPoolCtor);
        if ((g_pedGetAt = runtime.lookupFunction(kPedGetAt)))
            runtime.replaceFunction(kPedGetAt, &pedGetAt);
        if ((g_spotFinder = runtime.lookupFunction(kSpotFinder)))
            runtime.replaceFunction(kSpotFinder, &spotFinder);
        if ((g_nodeList = runtime.lookupFunction(kNodeList)))
            runtime.replaceFunction(kNodeList, &nodeList);
        if ((g_addToPopulation = runtime.lookupFunction(kAddToPopulation)))
            runtime.replaceFunction(kAddToPopulation, &addToPopulation);
        if ((g_nodeNew = runtime.lookupFunction(kNodeNew)))
            runtime.replaceFunction(kNodeNew, &nodeNew);
        if ((g_nodeDelete = runtime.lookupFunction(kNodeDelete)))
            runtime.replaceFunction(kNodeDelete, &nodeDelete);
        if ((g_updateAnim = runtime.lookupFunction(kUpdateAnim)))
            runtime.replaceFunction(kUpdateAnim, &updateAnim);
        if ((g_animUpdate = runtime.lookupFunction(kAnimUpdate)))
            runtime.replaceFunction(kAnimUpdate, &animUpdate);
        if ((g_randomPedControl = runtime.lookupFunction(kRandomPedControl)))
            runtime.replaceFunction(kRandomPedControl, &randomPedControl);
        if ((g_processCollision = runtime.lookupFunction(kProcessCollision)))
            runtime.replaceFunction(kProcessCollision, &processCollision);
        // crowdlod -> ok skipped=<ped updates skipped> processed=<LOD updates run> overflow=<sector entries in the
        //             overflow pool> peak=<most at once>
        ps2control::addCommand("crowdlod", [](const std::string &, ps2control::Reply reply) {
            reply("ok skipped=" + std::to_string(g_lodSkipped) + " processed=" + std::to_string(g_lodProcessed) +
                  " cheap=" + std::to_string(g_lodCheap) +
                  " overflow=" + std::to_string(g_overflowUsed) + " peak=" + std::to_string(g_overflowPeak));
        });
        // distancestate -> ok view=<%> detail=<%> scale=<ms_lodDistScale> lod=<LODDistMultiplier>
        //                  gen=<GenerationDistMultiplier> population=<%> traffic=<%> pedestrians=<%> cars=<random>/<cap>
        //                  peds=<peds>/<cap> pools=<peds>,<vehicles> numbers=<car>,<ped>
        ps2control::addCommand("distancestate", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
                return reply("error the camera has not run yet");
            char text[320];
            std::snprintf(text, sizeof(text),
                          "ok view=%d detail=%d scale=%.3f lod=%.3f gen=%.3f population=%d traffic=%d pedestrians=%d "
                          "cars=%d/%d peds=%d/%d pools=%u,%u numbers=%.3f,%.3f",
                          g_view.load(), g_detail.load(), rdf(kLodDistScale), rdf(kLodMultiplier), rdf(kGenMultiplier),
                          g_population.load(), g_traffic.load(), g_pedestrians.load(), rd32(kRandomCars), rd32(kMaxCars),
                          rd32(kPeds), rd32(kMaxPeds), poolSize(kPedPool), poolSize(kCarPool), rdf(kCarNumber), rdf(kPedNumber));
            reply(text);
        });
    }
}
