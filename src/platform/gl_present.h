// Stage 1 stopgap (docs/NATIVE_RENDERER.md): shows the game's picture in the window with OpenGL, in the platform
// window's GL context, until the RHI replaces it in stage 2. Main thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace glpresent
{
    struct Texture
    {
        uint32_t id = 0;
        int width = 0;
        int height = 0;
    };

    bool init();
    void shutdown();

    // RGBA8 textures for pictures made on the CPU. Rows top-down.
    Texture createTexture(int width, int height, bool linear);
    void updateTexture(const Texture &texture, const void *rgba);
    void destroyTexture(Texture &texture);

    // Clears the bound framebuffer (target x target pixels) to black and draws the top-left src x src part of a
    // texture (row 0 at the top) letterboxed into it, alpha-blended like raylib's DrawTexturePro did.
    void drawFrame(uint32_t textureId, int textureWidth, int textureHeight, int srcWidth, int srcHeight, int targetWidth,
                   int targetHeight);

    // The window's own framebuffer, width x height.
    void bindWindow(int width, int height);

    // Renders draw(width, height) into an off-screen width x height target and reads it back as RGB, rows
    // top-down (screenshots that work with the window hidden or covered).
    bool capture(int width, int height, const std::function<void(int, int)> &draw, std::vector<uint8_t> &rgb);
}
