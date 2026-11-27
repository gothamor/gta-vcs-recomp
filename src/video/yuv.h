// YCbCr 4:2:0 to RGBA for the movies (src/video/mpeg2.h's pictures).
#pragma once

#include <cstddef>
#include <cstdint>

namespace video
{
    // ITU-R BT.601, studio range (Y 16..235, C 16..240), each chroma sample for its 2x2 luma samples - the PS2 IPU's
    // conversion, and what FFmpeg's swscale gave the movies before (SWS_POINT). Alpha 255. dst holds width * height * 4.
    void yuv420ToRgba(const uint8_t *y, int yStride, const uint8_t *cb, const uint8_t *cr, int cStride, int width, int height,
                      uint8_t *dst);
}
