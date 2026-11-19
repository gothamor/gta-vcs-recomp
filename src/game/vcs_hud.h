// VCS 2D at the RenderWare level (vcs_hud.cpp).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

class PS2Runtime;

namespace vcs
{
    void installHudHooks(PS2Runtime &runtime);
    void onFrameKick(); // the frame's display list goes out: the HUD context ends
    // Widescreen, HUD "Screen edges": each HUD element squeezed towards the screen edge it sits near (any thread)
    void setHudAnchoring(bool on);
    // Widescreen: the HUD's squeeze ((4:3) / aspect; 1 = none, HUD "Stretch"), for the scope mask's side bars (any thread)
    void setHudSqueeze(float factor);
    uint32_t hudDrawCount(); // CHud::Draw calls drawn so far (the trainer's Hide HUD skips them)

    // The native HUD layer's classifier (pure; VCS_SELFTEST checks it). The frame's HUD primitives as the game drew them:
    // their vertex positions (hudPositionKey) with their anchors (GSHwAnchor*), and the colours (0xRRGGBB) of the
    // full-width ones.
    struct HudAnchors
    {
        std::unordered_map<uint32_t, uint8_t> positions;
        std::vector<uint32_t> fullWidthColours;
    };
    uint32_t hudPositionKey(int x, int y); // whole pixels
    // A full-width HUD primitive's colour as Im2D got it (0xRRGGBB), kept as is and halved: RenderWare's textured 2D
    // reaches the GS halved (0x80 is 1.0)
    void addHudFullWidthColour(HudAnchors &anchors, uint32_t rgb);
    // A GS triangle's vertices (x, y in pixels; r, g, b per vertex): its anchor, or GSHwAnchorNone if not the HUD's
    uint8_t classifyHudPrimitive(const HudAnchors &anchors, const float *xy, const uint8_t *rgb, uint32_t count);
    uint8_t hudPositionsAnchor(const HudAnchors &anchors, const float *xy, uint32_t count); // the positions alone
}
