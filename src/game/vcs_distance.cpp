// Draw distances and the population as settings (docs/game-internals.md 9h) - Project2DFX's DrawDistance and the
// limit adjuster's pools, done the game's own way.
//
// Objects. VCS draws its entities (objects, the ped crowds, anything with a model info and LOD distances) as VC does:
// CRenderer's SetupEntityVisibility (FUN_0035EFD8) picks a model's atomic by `distance < lodDistance[i] *
// LODDistMultiplier` (GetAtomicFromDistance FUN_003A52F8, GetLargestLodDistance FUN_003A5380; the model info's LOD
// distances at +0x2C, its atomics at +0x28), fades it in past the last one and has it streamed 50 m before. The camera's
// LODDistMultiplier (0x6F4CAC, camera +0x7DC) is rebuilt every frame in CCamera::Process (FUN_00378DD8): 70 / FOV, more
// above 55 m, at most 2, copied to GenerationDistMultiplier (+0x7D0: the population's distances), then times
// CRenderer::ms_lodDistScale (0x4887F8) - a constant 1.0 that nothing writes and only CCamera::Process reads. Writing the
// setting's factor there before each CCamera::Process scales every entity's draw (and stream) distance and nothing else;
// 100 % writes the game's 1.0 back. The city itself (the streamed sectors, game/vcs_world.cpp) has no distance LOD.
//
// Population distance. GenerationDistMultiplier is read by seven functions: cars made 100-160 x and removed past 160 x
// (FUN_00430290, FUN_00433378), peds made 35-60 x and removed past 51-65 x (FUN_002AFE80, FUN_002B0200), the vehicles'
// LOD and fade distances 60-150 x (FUN_0036EBF8, VC's CVisibilityPlugins), FUN_003DA070 and FUN_003F29D8. The game
// thread runs them in CGame::Process and the render's setup, never inside CCamera::Process, which writes the value: so
// the first of them after a CCamera::Process multiplies the fresh value once (wrapped at their entry), and every reader
// sees the same scaled value until the next CCamera::Process. Before CCamera::Process the value is set to -1: still -1 at
// the first reader means it wrote none that frame, and the game's previous value is scaled again (never a scaled one).
// 100 % from the start never writes it.
//
// Density. The numbers of cars and peds are the zone's density times the scripts' multipliers (0x48A968 cars,
// 0x487FF8 peds: missions set them) times two constants the PS2 version keeps low - 0x48A680 cars 0.7, 0x48A67C peds
// 0.4 (as VC's CarNumberMultiplier / PedNumberMultiplier) - capped by MaxNumberOfCarsInUse 0x48A9A8 (16) and the peds'
// 0x488004 / 0x488008 (25 / 30); GenerateRandomCars also stops at 30 random cars (a constant in the code, patched to 127
// in game/vcs/overrides.toml). The settings scale the constants and the caps by the density times the population
// distance squared (the same density over the larger area); the scripts' multipliers stay theirs. The caps are kept
// within the pools: VCS has room for 70 peds and 40 vehicles (CPools::Initialise FUN_00288100); with the streaming heap
// in the upper memory (game/vcs_memory.h: the main heap keeps the 5.4 MB) the pools are built for 280 / 110 instead
// (their constructors FUN_00478F28 / FUN_00478FC0, called only from there, given the larger size).
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

        // the functions that read GenerationDistMultiplier (not FUN_00219898: its +0x7D0 is a ped's)
        constexpr uint32_t kGenReaders[] = {0x002AFE80u, 0x002B0200u, 0x0036EBF8u, 0x003DA070u, 0x003F29D8u, 0x00430290u, 0x00433378u};
        constexpr size_t kReaderCount = sizeof(kGenReaders) / sizeof(kGenReaders[0]);

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
        PS2Runtime::RecompiledFunction g_readers[kReaderCount] = {};
        std::atomic<int> g_objects{100};    // percent of the game's object distances
        std::atomic<int> g_population{100}; // percent of the game's population distances
        std::atomic<int> g_traffic{100}, g_pedestrians{100}; // percent of the game's density
        uint8_t *g_rdram = nullptr;
        // the population distance on GenerationDistMultiplier (game thread only): CCamera::Process has run since the last
        // scaling; the value is ours to keep (a factor other than 1 since); the game's last value (unscaled)
        bool g_genFresh = false, g_genTouched = false;
        float g_genBase = 1.0f;
        constexpr float kGenUnwritten = -1.0f; // written before CCamera::Process: still there = it wrote none this frame

        // the game's constants, read once
        bool g_haveDefaults = false;
        float g_carNumber = 0.7f, g_pedNumber = 0.4f;
        int32_t g_maxCars = 16, g_maxPeds = 25, g_maxPedsB = 30;
        uint8_t g_roadDensity[4] = {0, 4, 7, 15};

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
            const float area = percent(g_population) * percent(g_population);
            const float cars = percent(g_traffic) * area, peds = percent(g_pedestrians) * area;
            if (cars == 1.0f && peds == 1.0f)
            {
                wrf(kCarNumber, g_carNumber); // the game's own (a setting back at 100 %)
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
            wrf(kLodDistScale, percent(g_objects));
            applyDensity();
            g_genFresh = true;
            if (percent(g_population) != 1.0f || g_genTouched)
            {
                g_genTouched = true;
                wrf(kGenMultiplier, kGenUnwritten);
            }
            g_original(rdram, ctx, runtime);
        }

        // a reader of GenerationDistMultiplier: the first after a CCamera::Process scales the game's value, once (the
        // previous one when CCamera::Process wrote none); at 100 % from the start nothing is ever written
        template <size_t I>
        PS2_GUEST_CC void genReader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // the offscreen vehicle distances the patched code reads (game/vcs/overrides.toml): always written, the RAM
            // there starts at zero
            g_rdram = rdram;
            const float k = percent(g_population);
            wrf(kOffscreenSlots, 60.0f * k);
            wrf(kOffscreenSlots + 4u, 90.0f * k);
            wrf(kOffscreenSlots + 8u, 70.0f * k);
            if (g_genFresh && g_genTouched)
            {
                g_rdram = rdram;
                const float v = rdf(kGenMultiplier), k = percent(g_population);
                g_genBase = v == kGenUnwritten ? g_genBase : v;
                wrf(kGenMultiplier, g_genBase * k);
                g_genTouched = k != 1.0f;
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

        // The spot finder FUN_00267928(x, y, dirx, diry, distance, cos, nodes, ...) looks for a road node `distance` (110 x
        // GenerationDistMultiplier for traffic) ahead along dir, among a list it rebuilds every 64 frames with FUN_00269228:
        // up to 250 nodes in a square of +-310 m around the player, in grid order. With the population distance the ring
        // in view a new car has to stand in (100-160 x gen) moves past that square, and nearly every new car was deleted.
        // The list's centre moves ahead instead, distance x (1 - 1 / factor) along the finder's direction: the same square,
        // so no more nodes than the game's, over the ring. The game's own at 100 %.
        constexpr uint32_t kSpotFinder = 0x00267928u, kSpotFinderEnd = 0x00267E98u, kNodeList = 0x00269228u;
        PS2Runtime::RecompiledFunction g_spotFinder = nullptr, g_nodeList = nullptr;
        float g_spotDir[2] = {0.0f, 0.0f}, g_spotDistance = 0.0f;
        PS2_GUEST_CC void spotFinder(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::memcpy(&g_spotDir[0], &ctx->f[14], 4);
            std::memcpy(&g_spotDir[1], &ctx->f[15], 4);
            std::memcpy(&g_spotDistance, &ctx->f[16], 4);
            g_spotFinder(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void nodeList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float k = percent(g_population);
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

        // the pools' constructors, from CPools::Initialise only: larger pools when the main heap has the room
        bool largePools() { return vcs::memory::streamingMegabytes() > 0; }
        // VCS_POOLS=peds,vehicles: the pool sizes for a test (0 or missing: the game's)
        uint32_t testPool(int which)
        {
            const char *env = std::getenv("VCS_POOLS");
            unsigned peds = 0, cars = 0;
            if (!env || std::sscanf(env, "%u,%u", &peds, &cars) < 1)
                return 0;
            return which ? cars : peds;
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

        // [T2 probe] VCS_PED_SLOTS=n: CPool<CPed>::New (FUN_004767b0) never hands out a slot >= n (as the game's, else)
        PS2_GUEST_CC void pedNew(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const int limit = std::getenv("VCS_PED_SLOTS") ? std::atoi(std::getenv("VCS_PED_SLOTS")) : 0;
            const uint32_t pool = GPR_U32(ctx, 4);
            auto r32 = [&](uint32_t a) { int32_t v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
            const uint32_t entries = static_cast<uint32_t>(r32(pool)), flags = static_cast<uint32_t>(r32(pool + 4u));
            const int32_t size = std::min(r32(pool + 8u), limit);
            int32_t i = r32(pool + 12u);
            bool wrapped = false;
            uint32_t result = 0;
            for (;;)
            {
                ++i;
                if (i >= size)
                {
                    if (wrapped)
                        break;
                    i = 0;
                    wrapped = true;
                }
                uint8_t &f = rdram[(flags + i) & PS2_RAM_MASK];
                if (f & 0x80u)
                {
                    f &= 0x7Fu;
                    f = static_cast<uint8_t>((f & 0x80u) | ((f + 1u) & 0x7Fu));
                    result = entries + static_cast<uint32_t>(i) * 0xD20u;
                    break;
                }
            }
            std::memcpy(rdram + ((pool + 12u) & PS2_RAM_MASK), &i, 4);
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = GPR_U32(ctx, 31);
        }

        PS2_GUEST_CC void pedPoolCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kPedPoolReturn && std::getenv("VCS_POOLS"))
            {
                if (testPool(0))
                    SET_GPR_U32(ctx, 5, testPool(0));
            }
            else if (GPR_U32(ctx, 31) == kPedPoolReturn && largePools() && GPR_U32(ctx, 5) < kLargePeds)
                SET_GPR_U32(ctx, 5, kLargePeds);
            g_pedPoolCtor(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void carPoolCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kCarPoolReturn && std::getenv("VCS_POOLS"))
            {
                if (testPool(1))
                    SET_GPR_U32(ctx, 5, testPool(1));
            }
            else if (GPR_U32(ctx, 31) == kCarPoolReturn && largePools() && GPR_U32(ctx, 5) < kLargeCars)
                SET_GPR_U32(ctx, 5, kLargeCars);
            g_carPoolCtor(rdram, ctx, runtime);
        }
    }

    // [T2 probe] GenerateOneRandomCar's funnel: entries into its steps from inside it (counted before the call)
    namespace funnel
    {
        constexpr uint32_t kLo = 0x00430290u, kHi = 0x00432788u;
        struct Step
        {
            uint32_t addr;
            const char *name;
            PS2Runtime::RecompiledFunction original;
            std::atomic<uint64_t> count;
        };
        Step g_steps[] = {{0x00430290u, "attempt", nullptr, {}}, {0x00432888u, "model", nullptr, {}},
                          {0x00268880u, "node", nullptr, {}},
                                                    {0x0043C938u, "found", nullptr, {}},
                          {0x00195380u, "space", nullptr, {}},   {0x003E68E0u, "new", nullptr, {}},
                          {0x0022C848u, "delA", nullptr, {}},    {0x001344D0u, "delB", nullptr, {}},
                          {0x0019C548u, "delC", nullptr, {}},    {0x00335A20u, "delD", nullptr, {}}};
        std::atomic<uint64_t> g_deleteAt[6]; // <60, <150, <290, <370, <500, more (m)
        template <size_t I>
        PS2_GUEST_CC void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (I == 0 || (ra >= kLo && ra < kHi))
            {
                g_steps[I].count.fetch_add(1, std::memory_order_relaxed);
                if (g_steps[I].name[0] == 'd') // a deleting destructor: how far from the player the new car was
                {
                    float c[3], p[3];
                    uint32_t ped;
                    const uint32_t car = GPR_U32(ctx, 4);
                    std::memcpy(&ped, rdram + 0x004E4910u, 4);
                    std::memcpy(c, rdram + ((car + 0x30u) & PS2_RAM_MASK), 12);
                    std::memcpy(p, rdram + ((ped + 0x30u) & PS2_RAM_MASK), 12);
                    const float d = std::hypot(c[0] - p[0], c[1] - p[1]);
                    const int b = d < 60.0f ? 0 : d < 150.0f ? 1 : d < 290.0f ? 2 : d < 370.0f ? 3 : d < 500.0f ? 4 : 5;
                    g_deleteAt[b].fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_steps[I].original(rdram, ctx, runtime);
        }
        template <size_t... I>
        void install(PS2Runtime &runtime, std::index_sequence<I...>)
        {
            const PS2Runtime::RecompiledFunction hooks[] = {&hook<I>...};
            for (size_t i = 0; i < sizeof...(I); ++i)
                if ((g_steps[i].original = runtime.lookupFunction(g_steps[i].addr)))
                    runtime.replaceFunction(g_steps[i].addr, hooks[i]);
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "distance.objects";
        s.page = "Graphics";
        s.section = "Draw distance";
        s.label = "Object distance";
        s.help = "How far away objects and their detail are drawn, percent of the original (100): the props, crowds and "
                 "anything else the game fades in with distance. The city's buildings stream by area and do not change.";
        s.type = Type::Integer;
        s.min = 100;
        s.max = 1000;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_OBJECT_DISTANCE";
        s.onChange = [](int v) { g_objects.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "distance.population";
        s.page = "Graphics";
        s.section = "Draw distance";
        s.label = "Traffic and pedestrian distance";
        s.help = "How far away cars and people appear, are drawn and are taken away, percent of the original (100). "
                 "Their numbers grow with the area, so the streets stay as busy (Game > Population sets how busy).";
        s.type = Type::Integer;
        s.min = 100;
        s.max = 300;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_POPULATION_DISTANCE";
        s.onChange = [](int v) { g_population.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "population.traffic";
        s.page = "Game";
        s.section = "Population";
        s.label = "Traffic density";
        s.help = "How many cars drive around, percent of the original (100; the PS2 keeps it low). Missions still empty "
                 "the streets when they want. Above the PS2's limits it needs Streaming memory above PS2 (more room).";
        s.type = Type::Integer;
        s.min = 25;
        s.max = 400;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_TRAFFIC_DENSITY";
        s.onChange = [](int v) { g_traffic.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "population.pedestrians";
        s.page = "Game";
        s.section = "Population";
        s.label = "Pedestrian density";
        s.help = "How many people walk around, percent of the original (100; the PS2 keeps it low). Missions still empty "
                 "the streets when they want. Above the PS2's limits it needs Streaming memory above PS2 (more room).";
        s.type = Type::Integer;
        s.min = 25;
        s.max = 400;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_PEDESTRIAN_DENSITY";
        s.onChange = [](int v) { g_pedestrians.store(v, std::memory_order_relaxed); };
        settings::add(s);
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
        if ((g_pedPoolCtor = runtime.lookupFunction(kPedPoolCtor)))
            runtime.replaceFunction(kPedPoolCtor, &pedPoolCtor);
        if ((g_carPoolCtor = runtime.lookupFunction(kCarPoolCtor)))
            runtime.replaceFunction(kCarPoolCtor, &carPoolCtor);
        if (std::getenv("VCS_PED_SLOTS"))
            runtime.replaceFunction(0x004767B0u, &pedNew);
        if ((g_pedGetAt = runtime.lookupFunction(kPedGetAt)))
            runtime.replaceFunction(kPedGetAt, &pedGetAt);
        if ((g_spotFinder = runtime.lookupFunction(kSpotFinder)))
            runtime.replaceFunction(kSpotFinder, &spotFinder);
        if ((g_nodeList = runtime.lookupFunction(kNodeList)))
            runtime.replaceFunction(kNodeList, &nodeList);
        funnel::install(runtime, std::make_index_sequence<sizeof(funnel::g_steps) / sizeof(funnel::g_steps[0])>{});
        ps2control::addCommand("genfunnel", [](const std::string &, ps2control::Reply reply) {
            std::string text = "ok";
            for (const auto &s : funnel::g_steps)
                text += std::string(" ") + s.name + "=" + std::to_string(s.count.load());
            text += " deletedAt";
            for (const auto &d : funnel::g_deleteAt)
                text += " " + std::to_string(d.load());
            reply(text);
        });
        // distancestate -> ok objects=<%> scale=<ms_lodDistScale> lod=<LODDistMultiplier> gen=<GenerationDistMultiplier>
        //                  population=<%> traffic=<%> pedestrians=<%> cars=<random cars>/<cap> peds=<peds>/<cap>
        //                  pools=<peds>,<vehicles> numbers=<car>,<ped>
        ps2control::addCommand("distancestate", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
                return reply("error the camera has not run yet");
            char text[320];
            std::snprintf(text, sizeof(text),
                          "ok objects=%d scale=%.3f lod=%.3f gen=%.3f population=%d traffic=%d pedestrians=%d cars=%d/%d "
                          "peds=%d/%d pools=%u,%u numbers=%.3f,%.3f",
                          g_objects.load(), rdf(kLodDistScale), rdf(kLodMultiplier), rdf(kGenMultiplier), g_population.load(),
                          g_traffic.load(), g_pedestrians.load(), rd32(kRandomCars), rd32(kMaxCars), rd32(kPeds), rd32(kMaxPeds),
                          poolSize(kPedPool), poolSize(kCarPool), rdf(kCarNumber), rdf(kPedNumber));
            reply(text);
        });
    }
}
