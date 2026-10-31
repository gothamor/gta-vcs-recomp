// Shows the game's picture on the TV through the RHI (docs/NATIVE_RENDERER.md): the swapchain, the letterboxed
// picture, Dear ImGui on top, and off-screen screenshots. Main thread only.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi
{
    class Device;
    class Texture;
}

namespace present
{
    // A picture to show: made on the CPU (the software GS) or a renderer's own texture.
    struct Picture
    {
        std::shared_ptr<rhi::Texture> texture;
        int width = 0;
        int height = 0;
        bool linear = false; // bilinear filtering when scaled
    };

    // The renderer (RHI backend): Vulkan, or VCS_RHI=null (no GPU work).
    int backend(); // rhi::Backend init uses (or used)

    // On the TV's surface (platform::init first); width x height: the back buffer until the first drawOutput sizes it.
    bool init(int width, int height);
    void shutdown();
    rhi::Device *device();

    Picture createPicture(int width, int height, bool linear);
    void updatePicture(const Picture &picture, const void *rgba); // whole picture, rows top-down, tightly packed

    // Per host frame: beginFrame() before platform::beginFrame() (Dear ImGui renderer), drawOutput() after
    // platform::endUiFrame(), then optional captures, then present().
    void beginFrame();
    // The back buffer (resized to outputWidth x outputHeight, the TV mode): black, the top-left src part of the picture
    // letterboxed at display aspect `aspect` (width / height) inside the game's frame (fitRect), the Dear ImGui output
    // on top.
    void drawOutput(const Picture &picture, int srcWidth, int srcHeight, float aspect, int outputWidth, int outputHeight,
                    float frameAspect = 0.0f);
    void present();
    void setFrameInputTime(int64_t steadyNs); // the shown frame's input snapshot (rhi::Device::setFrameInputTime)
    // How present() shows frames (the Display > Vertical sync setting; main thread).
    void setPresentMode(int mode); // rhi::Device::PresentMode: 0 Immediate, 1 Vsync, 2 Tearing

    // The same picture and Dear ImGui output (scaled) rendered off-screen at width x height; RGB rows top-down.
    bool capture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int width, int height, std::vector<uint8_t> &rgb,
                 float frameAspect = 0.0f);

    // Where a picture of display aspect `aspect` goes in a width x height target: as large as fits inside the game's
    // frame - the target letterboxed at frameAspect (0: the whole target) - and centred. A 4:3 menu in a 32:9 game on a
    // 16:9 TV sits in the 32:9 strip, between the bars, not over them.
    void fitRect(float aspect, float frameAspect, float width, float height, float &x0, float &y0, float &w, float &h);

    // Brightness probe (automation rig, docs/NATIVE_PORT_ROADMAP.md milestone 1): the picture's middle region
    // (HUD margins left out) shrunk to 32x24 and read back; mean luma 0..255. Stalls the GPU briefly: opt-in.
    bool frameLuma(const Picture &picture, int srcWidth, int srcHeight, float &mean);

    // Display aspect of the game picture (docs/SETTINGS_MENU_WIDESCREEN.md): 4:3 as on a PS2's TV, or the widescreen
    // ratio. displayAspect(false) is for frames without 3D (loading screens, menus): always 4:3. Any thread.
    void setDisplayAspect(float aspect);
    float displayAspect(bool showing3d);
}
