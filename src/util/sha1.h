// Minimal SHA-1, only used to identify the game executable on the user's disc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace vcs
{
    inline std::string sha1Hex(const void *data, size_t size)
    {
        auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
        uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        const uint64_t bitLength = static_cast<uint64_t>(size) * 8u;
        const size_t padded = ((size + 8) / 64 + 1) * 64;

        for (size_t chunk = 0; chunk < padded; chunk += 64)
        {
            uint8_t block[64];
            for (size_t i = 0; i < 64; ++i)
            {
                const size_t pos = chunk + i;
                if (pos < size)
                    block[i] = bytes[pos];
                else if (pos == size)
                    block[i] = 0x80;
                else if (pos >= padded - 8)
                    block[i] = static_cast<uint8_t>(bitLength >> (8 * (padded - 1 - pos)));
                else
                    block[i] = 0;
            }
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                       (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
            for (int i = 16; i < 80; ++i)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                const uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }

        static const char *hex = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h)
            for (int s = 28; s >= 0; s -= 4)
                out.push_back(hex[(v >> s) & 0xF]);
        return out;
    }
}
