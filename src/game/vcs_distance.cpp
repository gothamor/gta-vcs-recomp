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

        constexpr uint32_t kPedPoolCtor = 0x00478F28u, kCarPoolCtor = 0x00478FC0u;   // (memory, size, name)
        constexpr uint32_t kPedPoolReturn = 0x00288144u, kCarPoolReturn = 0x00288168u; // their calls in FUN_00288100
        constexpr uint32_t kPedPool = 0x00487A94u, kCarPool = 0x00487A98u;           // CPool *: +8 size
        // Peds at most 128: with 136 or more the game corrupts its memory within a minute even with five peds alive
        // (RpClumpRender looping on a broken atomic list, the main heap's header, a destructor over a garbage count),
        // 129 and fewer run clean - as if a ped's slot were kept in a signed byte somewhere (game-internals.md 9h).
        // Vehicles stay below 128 for the same reason (300 ran, but their slots never climbed that far).
        constexpr uint32_t kLargePeds = 128u, kLargeCars = 110u;
        constexpr uint32_t kCarsKept = 25u; // vehicle slots left for missions, parked cars, the player's
        constexpr uint32_t kPedsKept = 30u; // ped slots left for missions and the player, besides two a car

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
                return;
            }
            const uint32_t carPool = poolSize(kCarPool), pedPool = poolSize(kPedPool);
            const int32_t carRoom = std::max<int32_t>(g_maxCars, static_cast<int32_t>(carPool) - static_cast<int32_t>(kCarsKept));
            const int32_t maxCars = std::clamp(static_cast<int32_t>(std::lround(g_maxCars * cars)), 0, carRoom);
            const int32_t pedRoom = std::max<int32_t>(g_maxPedsB, static_cast<int32_t>(pedPool) - 2 * maxCars - static_cast<int32_t>(kPedsKept));
            wrf(kCarNumber, g_carNumber * cars);
            wrf(kPedNumber, g_pedNumber * peds);
            wr32(kMaxCars, maxCars);
            wr32(kMaxPeds, std::clamp(static_cast<int32_t>(std::lround(g_maxPeds * peds)), 0, pedRoom));
            wr32(kMaxPedsB, std::clamp(static_cast<int32_t>(std::lround(g_maxPedsB * peds)), 0, pedRoom));
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
