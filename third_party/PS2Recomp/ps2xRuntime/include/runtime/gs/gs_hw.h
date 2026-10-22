// [gta-recomp] Hardware GS renderer (D3D11 through the RHI, src/rhi; was OpenGL 3.3).
//
// The game thread records: GSHwBackend implements GSRasterBackend, keeps VRAM, transfers and CLUTs in an
// embedded GSCpuBackend (everything but drawing), turns primitives into batched draws, decodes textures
// (cached) and hands each frame's command list over at VBlank. The main thread executes the lists on the
// window's RHI device (render targets, depth, blending, alpha test, fog) and shows the display circuit.
//
// PS2X_GS=hw selects it. PS2X_HW_SCALE=<1..8> renders at that multiple of the PS2 resolution.
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

namespace rhi
{
    class Device;
    class Texture;
}

struct GSHwOptions
{
    uint32_t scale = 1;
    static GSHwOptions fromEnvironment();
};

// Main thread side.
class GSHwPresenter
{
public:
    virtual ~GSHwPresenter() = default;
    // Main thread, with the window's device: run every frame recorded since the last call. Returns the texture
    // holding the newest displayed picture (alpha = 1, row 0 at the top; owned by the renderer, valid until the next
    // call) and its size; false until a first frame has been displayed.
    virtual bool executeAndGetDisplay(rhi::Device &device, rhi::Texture *&texture, uint32_t &width, uint32_t &height) = 0;
    // [gta-recomp] Whether the displayed frames draw 3D (depth-tested or native draws), with some hysteresis: frames that
    // are only 2D (loading screens, menus) are presented at 4:3 in widescreen.
    virtual bool showing3d() const = 0;
};

class GSHwBackendHandle : public GSRasterBackend
{
public:
    // Game thread, at VBlankStart: close the frame's command list (privRegs: PMODE..BGCOLOR in GS order).
    virtual void endFrame(const uint64_t (&privRegs)[15]) = 0;
    virtual GSHwPresenter &presenter() = 0;
};

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options);

// [gta-recomp] Widescreen (docs/SETTINGS_MENU_WIDESCREEN.md): 2D draws (no depth test, into a 640-wide target, not
// covering the whole width) are squeezed horizontally by `factor` around the middle, so the HUD keeps its 4:3 shape in a
// wider picture. 1 = off. Any thread.
void gsHwSetHudSqueeze(float factor);
