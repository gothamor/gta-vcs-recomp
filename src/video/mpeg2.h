// MPEG-2 video decoder (ISO/IEC 13818-2), written for the intro movies (docs/game-internals.md 3 "Movies").
//
// Main Profile frame pictures in 4:2:0: I / P / B pictures, frame and field motion compensation (dual prime too),
// frame and field DCT, both VLC tables for intra blocks, zig-zag and alternate scan, linear and non-linear quantiser
// scale, quant matrices from the stream. Field pictures (picture_structure 1 / 2), MPEG-1 streams (no sequence
// extension), 4:2:2 / 4:4:4 and scalable extensions are refused with a message: the PSS files on the disc use none
// of them (tools/pss_survey.py). A damaged slice is abandoned at the next start code, its macroblocks left as the
// reference had them; the decoder never reads or writes outside its buffers.
//
// Pictures come out in display order. Usage: open(es, size) with the whole elementary stream (it must stay alive
// while decoding), then next() until it returns false.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace video
{
    struct Mpeg2Picture
    {
        int width = 0, height = 0; // the displayed size (horizontal_size / vertical_size)
        const uint8_t *y = nullptr, *cb = nullptr, *cr = nullptr;
        int yStride = 0, cStride = 0; // chroma planes are (width + 1) / 2 x (height + 1) / 2
        char type = '?';              // 'I', 'P' or 'B'
        double frameRate = 0.0;       // from the sequence header (frame_rate_code and its extension)
    };

    struct Mpeg2Stats
    {
        uint64_t pictures = 0;      // decoded
        uint64_t skippedB = 0;      // B pictures with no reference to predict from (an open GOP at the start)
        uint64_t sliceErrors = 0;   // slices abandoned on a bitstream error
        uint64_t unsupported = 0;   // pictures refused (field pictures and the like)
    };

    class Mpeg2Decoder
    {
    public:
        Mpeg2Decoder();
        ~Mpeg2Decoder();
        Mpeg2Decoder(const Mpeg2Decoder &) = delete;
        Mpeg2Decoder &operator=(const Mpeg2Decoder &) = delete;

        // The elementary stream (start codes 00 00 01 xx); decoding starts at its first sequence header.
        void open(const uint8_t *data, size_t size);
        // The next picture in display order; its planes stay valid until the next call. False at the end.
        bool next(Mpeg2Picture &out);

        const Mpeg2Stats &stats() const;
        // the first problem met, for a log line (empty if none)
        const std::string &lastError() const;

    private:
        struct State;
        std::unique_ptr<State> s_;
    };
}
