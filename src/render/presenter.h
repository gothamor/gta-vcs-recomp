// Shows the game's picture in the window through the RHI (stage 2 of docs/NATIVE_RENDERER.md): the swapchain, the
// letterboxed picture, Dear ImGui on top, and off-screen screenshots. Main thread only.
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
    // A picture to show: made on the CPU (software GS, paraLLEl-GS readback) or a renderer's own texture.
    struct Picture
    {
        std::shared_ptr<rhi::Texture> texture;
        int width = 0;
        int height = 0;
        bool linear = false; // bilinear filtering when scaled
    };

    bool init(void *nativeWindow, int width, int height);
    void shutdown();
    rhi::Device *device();

    Picture createPicture(int width, int height, bool linear);
    void updatePicture(const Picture &picture, const void *rgba); // whole picture, rows top-down, tightly packed

    // Per host frame: beginFrame() before platform::beginFrame() (Dear ImGui renderer), drawWindow() after
    // platform::endUiFrame(), then optional captures, then present().
    void beginFrame();
    // The window's back buffer (resized to windowWidth x windowHeight): black, the top-left src part of the picture
    // letterboxed at display aspect `aspect` (width / height), the Dear ImGui output on top.
    void drawWindow(const Picture &picture, int srcWidth, int srcHeight, float aspect, int windowWidth, int windowHeight);
    void present();

    // The same picture and Dear ImGui output (scaled) rendered off-screen at width x height; RGB rows top-down.
    bool capture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int width, int height, std::vector<uint8_t> &rgb);

    // Display aspect of the game picture (docs/SETTINGS_MENU_WIDESCREEN.md): 4:3 as on a PS2's TV, or the widescreen
    // ratio. displayAspect(false) is for frames without 3D (loading screens, menus): always 4:3. Any thread.
    void setDisplayAspect(float aspect);
    float displayAspect(bool showing3d);
}
