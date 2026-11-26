#include "video/yuv.h"

#include <cstddef>

namespace video
{
    namespace
    {
        // BT.601 studio range in 16.16 fixed point
        constexpr int kY = 76309;   // 255 / 219
        constexpr int kRV = 104597; // 1.402 * 255 / 224
        constexpr int kGU = 25675;  // 0.344136 * 255 / 224
        constexpr int kGV = 53279;  // 0.714136 * 255 / 224
        constexpr int kBU = 132201; // 1.772 * 255 / 224

        inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
    }

    void yuv420ToRgba(const uint8_t *y, int yStride, const uint8_t *cb, const uint8_t *cr, int cStride, int width, int height,
                      uint8_t *dst)
    {
        for (int j = 0; j < height; ++j)
        {
            const uint8_t *yl = y + j * yStride;
            const uint8_t *ul = cb + (j >> 1) * cStride;
            const uint8_t *vl = cr + (j >> 1) * cStride;
            uint8_t *d = dst + static_cast<size_t>(j) * width * 4;
            for (int i = 0; i < width; ++i)
            {
                const int l = (yl[i] - 16) * kY + 32768;
                const int u = ul[i >> 1] - 128, v = vl[i >> 1] - 128;
                d[0] = clamp8((l + kRV * v) >> 16);
                d[1] = clamp8((l - kGU * u - kGV * v) >> 16);
                d[2] = clamp8((l + kBU * u) >> 16);
                d[3] = 255;
                d += 4;
            }
        }
    }
}
