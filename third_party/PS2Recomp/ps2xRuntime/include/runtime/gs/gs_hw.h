// [gta-recomp] Hardware (OpenGL 3.3) GS renderer.
//
// The game thread records: GSHwBackend implements GSRasterBackend, keeps VRAM, transfers and CLUTs in an
// embedded GSCpuBackend (everything but drawing), turns primitives into batched draws, decodes textures
// (cached) and hands each frame's command list over at VBlank. The main thread executes the lists in the
// host window's GL context (render targets, depth, blending, alpha test, fog) and shows the display circuit.
//
// PS2X_GS=hw selects it. PS2X_HW_SCALE=<1..8> renders at that multiple of the PS2 resolution.
#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

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
    // With the window's GL context current: run every frame recorded since the last call. Returns the GL
    // texture holding the newest displayed picture (alpha = 1, row 0 at the top) and its size; false until
    // a first frame has been displayed.
    virtual bool executeAndGetDisplay(uint32_t &glTexture, uint32_t &width, uint32_t &height) = 0;
    virtual void releaseGl() = 0;
};

class GSHwBackendHandle : public GSRasterBackend
{
public:
    // Game thread, at VBlankStart: close the frame's command list (privRegs: PMODE..BGCOLOR in GS order).
    virtual void endFrame(const uint64_t (&privRegs)[15]) = 0;
    virtual GSHwPresenter &presenter() = 0;
};

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options);
