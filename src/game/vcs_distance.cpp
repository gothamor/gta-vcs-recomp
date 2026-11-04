// Draw distances as settings (docs/game-internals.md 9h) - Project2DFX's DrawDistance, done the game's own way.
//
// VCS draws its entities (objects, the ped crowds, anything with a model info and LOD distances) as VC does:
// CRenderer's SetupEntityVisibility (FUN_0035EFD8) picks a model's atomic by `distance < lodDistance[i] *
// LODDistMultiplier` (GetAtomicFromDistance FUN_003A52F8, GetLargestLodDistance FUN_003A5380; the model info's LOD
// distances at +0x2C, its atomics at +0x28), fades it in past the last one and has it streamed 50 m before. The camera's
// LODDistMultiplier (0x6F4CAC, camera +0x7DC) is rebuilt every frame in CCamera::Process (FUN_00378DD8): 70 / FOV, more
// above 55 m, at most 2, copied to GenerationDistMultiplier (+0x7D0: the population's distances), then times
// CRenderer::ms_lodDistScale (0x4887F8) - a constant 1.0 that nothing writes and only CCamera::Process reads. Writing the
// setting's factor there before each CCamera::Process scales every entity's draw (and stream) distance and nothing else;
// 100 % writes the game's 1.0 back. The city itself (the streamed sectors, game/vcs_world.cpp) has no distance LOD.
#include "game/vcs_distance.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace vcs::distance
{
    namespace
    {
        constexpr uint32_t kCameraProcess = 0x00378DD8u; // CCamera::Process
        constexpr uint32_t kLodDistScale = 0x004887F8u;  // CRenderer::ms_lodDistScale (1.0)
        constexpr uint32_t kLodMultiplier = 0x006F4CACu; // TheCamera.LODDistMultiplier (camera 0x6F44D0 + 0x7DC)
        constexpr uint32_t kGenMultiplier = 0x006F4CA0u; // TheCamera.GenerationDistMultiplier (+0x7D0)

        PS2Runtime::RecompiledFunction g_original = nullptr;
        std::atomic<int> g_objects{100}; // percent of the game's object distances
        uint8_t *g_rdram = nullptr;

        float rdf(uint32_t addr)
        {
            float f;
            std::memcpy(&f, g_rdram + (addr & PS2_RAM_MASK), 4);
            return f;
        }

        // before the original (DECISIONS.md, hooks): it reads the scale once, near its end
        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            const float scale = static_cast<float>(g_objects.load(std::memory_order_relaxed)) / 100.0f;
            std::memcpy(rdram + kLodDistScale, &scale, 4);
            g_original(rdram, ctx, runtime);
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
        s.max = 400;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_OBJECT_DISTANCE";
        s.onChange = [](int v) { g_objects.store(v, std::memory_order_relaxed); };
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
        // distancestate -> ok objects=<percent> scale=<ms_lodDistScale> lod=<LODDistMultiplier> gen=<GenerationDistMultiplier>
        ps2control::addCommand("distancestate", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
                return reply("error the camera has not run yet");
            char text[160];
            std::snprintf(text, sizeof(text), "ok objects=%d scale=%.3f lod=%.3f gen=%.3f", g_objects.load(),
                          rdf(kLodDistScale), rdf(kLodMultiplier), rdf(kGenMultiplier));
            reply(text);
        });
    }
}
