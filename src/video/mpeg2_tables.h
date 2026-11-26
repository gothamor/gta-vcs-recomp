// The tables of ISO/IEC 13818-2 Annex B and section 7 (src/video/mpeg2.cpp's only users).
#pragma once

#include <cstdint>

namespace video::mpeg2
{
    // A VLC as a direct lookup on its longest code: index = the next `bits` bits of the stream.
    struct VlcEntry
    {
        int16_t value;
        uint8_t length; // 0: not a valid code
    };

    struct VlcTable
    {
        int bits;
        const VlcEntry *entries;
    };

    // macroblock_type flags (tables B-2, B-3, B-4)
    enum : int
    {
        kMbQuant = 1,
        kMbForward = 2,
        kMbBackward = 4,
        kMbPattern = 8,
        kMbIntra = 16,
    };

    // macroblock_address_increment (B-1): 1..33, kMbaEscape (+33), kMbaStuffing (MPEG-1 only)
    constexpr int kMbaEscape = 34;
    constexpr int kMbaStuffing = 35;

    // DCT coefficients (B-14, B-15): run | level << 8; the sign bit follows the code
    constexpr int kDctEob = -1;
    constexpr int kDctEscape = -2;

    struct Tables
    {
        VlcTable mbaIncrement, mbTypeI, mbTypeP, mbTypeB, codedBlockPattern;
        VlcTable motionCode;           // |motion_code| 0..16, the sign bit follows a non-zero one (B-10)
        VlcTable dcSizeLuma, dcSizeChroma; // B-12, B-13
        VlcTable dctB14, dctB15;
    };

    const Tables &tables(); // built on first use

    extern const uint8_t kZigzag[64];          // scan index -> raster index (v * 8 + u)
    extern const uint8_t kAlternateScan[64];
    extern const uint8_t kDefaultIntraMatrix[64]; // raster order
    extern const uint8_t kNonLinearQuantiserScale[32];
}
