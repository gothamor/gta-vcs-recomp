#pragma once
// A zlib stream (RFC 1950) of DEFLATE data (RFC 1951) for the PNG screenshots (writePng), in place of the zlib
// library (DECISIONS.md 2026-11-27): LZ77 over a 32 KB window with hash chains and lazy matching, each block coded
// with its own Huffman codes, or fixed codes / stored when that is smaller. Any inflater (zlib, Python's zlib,
// every image viewer) reads it back.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace platform
{
    std::vector<uint8_t> zlibCompress(const uint8_t *data, size_t size);
}
