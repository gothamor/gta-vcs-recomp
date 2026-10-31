// The PNG writer for screenshots (platform.h), apart from the console's own functions in platform_ps5.cpp.
#include "platform/platform.h"

#include <zlib.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace platform
{
    namespace
    {
        // ---- PNG ----------------------------------------------------------------------------------------------
        uint32_t pngCrc32(uint32_t crc, const uint8_t *p, size_t n) // (not zlib's crc32)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            while (n--)
                crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x >> 24));
            v.push_back(static_cast<uint8_t>(x >> 16));
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, pngCrc32(0, out.data() + start, out.size() - start));
        }
    }

    bool writePng(const char *path, const uint8_t *rgb, int width, int height)
    {
        // Each row with the PNG filter (none / sub / up / paeth) whose bytes sum smallest as signed values - the
        // usual heuristic - then zlib. A 1080p screenshot was 6.2 MB with stored blocks.
        const size_t stride = static_cast<size_t>(width) * 3u;
        std::vector<uint8_t> raw(static_cast<size_t>(height) * (stride + 1u));
        std::vector<uint8_t> trial(stride);
        for (int y = 0; y < height; ++y)
        {
            const uint8_t *row = rgb + static_cast<size_t>(y) * stride;
            const uint8_t *above = y ? row - stride : nullptr;
            uint8_t *out = raw.data() + static_cast<size_t>(y) * (stride + 1u);
            uint64_t best = ~0ull;
            for (uint8_t filter = 0; filter < 5; ++filter)
            {
                if (filter == 3)
                    continue; // average: rarely better here
                uint64_t cost = 0;
                for (size_t i = 0; i < stride; ++i)
                {
                    const int a = i >= 3 ? row[i - 3] : 0, b = above ? above[i] : 0, c = (i >= 3 && above) ? above[i - 3] : 0;
                    int predicted = 0;
                    if (filter == 1)
                        predicted = a;
                    else if (filter == 2)
                        predicted = b;
                    else if (filter == 4)
                    {
                        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        predicted = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    }
                    trial[i] = static_cast<uint8_t>(row[i] - predicted);
                    cost += static_cast<uint64_t>(std::abs(static_cast<int8_t>(trial[i])));
                }
                if (cost < best)
                {
                    best = cost;
                    out[0] = filter;
                    std::memcpy(out + 1, trial.data(), stride);
                }
            }
        }
        uLongf zSize = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> z(zSize);
        if (compress2(z.data(), &zSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
            return false;
        z.resize(zSize);

        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        put32(ihdr, static_cast<uint32_t>(width));
        put32(ihdr, static_cast<uint32_t>(height));
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
        chunk(png, "IHDR", ihdr);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && ok;
    }
}
