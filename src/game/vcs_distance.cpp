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
