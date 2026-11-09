// The window and the renderer without the game (docs/MACOS.md): opens the game's window through the platform layer,
// makes the RHI device on it and presents frames of changing colours, then reads an off-screen target back. For
// bringing up a new platform before a disc is there - it needs no generated code.
//
//   vcs_window_smoke [frames] [vulkan|null]      (exit status 0: every step worked)
#include "platform/platform.h"
#include "rhi/rhi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    rhi::Backend backend = rhi::defaultBackend();
    if (argc > 2 && !rhi::backendFromName(argv[2], backend))
    {
        std::fprintf(stderr, "[smoke] unknown backend %s\n", argv[2]);
        return 2;
    }

    platform::WindowDesc wd;
    wd.title = "VCS window smoke test";
    wd.width = 960;
    wd.height = 672;
    wd.vulkan = backend == rhi::Backend::Vulkan;
    if (!platform::init(wd))
    {
        std::fprintf(stderr, "[smoke] FAIL: platform::init\n");
        return 1;
    }

    int w = 0, h = 0;
    platform::windowSize(w, h);
    rhi::DeviceDesc dd;
    dd.backend = backend;
    dd.nativeWindow = platform::nativeWindow();
    dd.vulkan.instanceExtensions = &platform::vulkanInstanceExtensions;
    dd.vulkan.createSurface = &platform::vulkanCreateSurface;
    dd.width = static_cast<uint32_t>(w);
    dd.height = static_cast<uint32_t>(h);
    dd.debug = std::getenv("PS2X_RHI_DEBUG") != nullptr;
    std::string error;
    auto device = rhi::createDevice(dd, error);
    if (!device)
    {
        std::fprintf(stderr, "[smoke] FAIL: createDevice: %s\n", error.c_str());
        platform::shutdown();
        return 1;
    }
    std::fprintf(stderr, "[smoke] %s on %s, window %dx%d\n", rhi::backendName(device->backend()),
                 device->adapterName().c_str(), w, h);

    int shown = 0;
    for (; shown < frames && !platform::quitRequested(); ++shown)
    {
        platform::pumpEvents();
        int nw = 0, nh = 0;
        platform::windowSize(nw, nh);
        if ((nw != w || nh != h) && nw > 0 && nh > 0)
        {
            w = nw;
            h = nh;
            device->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
        const float t = static_cast<float>(shown) / 60.0f;
        rhi::PassDesc pass;
        pass.clearColor = true;
        pass.clearRgba[0] = 0.5f + 0.5f * std::sin(t * 2.0f);
        pass.clearRgba[1] = 0.5f + 0.5f * std::sin(t * 2.0f + 2.1f);
        pass.clearRgba[2] = 0.5f + 0.5f * std::sin(t * 2.0f + 4.2f);
        pass.name = "smoke";
        device->beginPass(pass);
        device->endPass();
        device->present(rhi::Device::PresentMode::Vsync);
    }

    // an off-screen target, cleared and read back: the GPU really ran the commands
    rhi::TextureDesc td;
    td.width = 4;
    td.height = 4;
    td.format = rhi::Format::RGBA8;
    td.usage = rhi::UsageSampled | rhi::UsageRenderTarget;
    bool readOk = false;
    if (auto target = device->createTexture(td))
    {
        rhi::PassDesc pass;
        pass.color = target.get();
        pass.clearColor = true;
        pass.clearRgba[0] = 1.0f;
        pass.clearRgba[1] = 0.5f;
        pass.clearRgba[2] = 0.0f;
        pass.clearRgba[3] = 1.0f;
        device->beginPass(pass);
        device->endPass();
        unsigned char pixels[4 * 4 * 4] = {};
        if (device->readTexture(*target, 0, 0, 4, 4, pixels))
        {
            std::fprintf(stderr, "[smoke] read back %u %u %u %u (want 255 ~128 0 255)\n", pixels[0], pixels[1], pixels[2],
                         pixels[3]);
            readOk = backend == rhi::Backend::Null ||
                     (pixels[0] == 255 && pixels[1] >= 126 && pixels[1] <= 129 && pixels[2] == 0 && pixels[3] == 255);
        }
    }
    device.reset();
    platform::shutdown();
    std::fprintf(stderr, "[smoke] %s: %d frames presented, read-back %s\n", readOk ? "OK" : "FAIL", shown,
                 readOk ? "right" : "wrong");
    return readOk ? 0 : 1;
}
