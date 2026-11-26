// MPEG-2 video decoder (src/video/mpeg2.h). Section numbers are ISO/IEC 13818-2's.
#include "video/mpeg2.h"

#include "video/mpeg2_tables.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace video
{
    using namespace mpeg2;

    namespace
    {
        constexpr size_t kNoStartCode = static_cast<size_t>(-1);

        // the offset of the next 00 00 01 at or after `from`
        size_t findStartCode(const uint8_t *data, size_t size, size_t from)
        {
            for (size_t i = from; i + 3 < size;)
            {
                if (data[i + 2] > 1)
                    i += 3;
                else if (data[i + 2] == 1 && data[i + 1] == 0 && data[i] == 0)
                    return i;
                else
                    ++i;
            }
            return kNoStartCode;
        }

        // MSB-first bit reader; reads past the end give zeros (a damaged stream ends in an error, not a fault)
        class BitReader
        {
        public:
            BitReader(const uint8_t *data, size_t size, size_t bytePos) : data_(data), size_(size), pos_(bytePos * 8u) {}

            // the next n bits (1..32)
            uint32_t peek(int n) const
            {
                const size_t byte = pos_ >> 3;
                uint64_t v = 0;
                if (byte + 8u <= size_)
                {
                    std::memcpy(&v, data_ + byte, 8);
                    v = __builtin_bswap64(v);
                }
                else
                {
                    for (size_t i = 0; i < 8u; ++i)
                        v = (v << 8) | (byte + i < size_ ? data_[byte + i] : 0u);
                }
                v <<= (pos_ & 7u);
                return static_cast<uint32_t>(v >> (64 - n));
            }
            void skip(int n) { pos_ += static_cast<size_t>(n); }
            uint32_t get(int n)
            {
                const uint32_t v = peek(n);
                pos_ += static_cast<size_t>(n);
                return v;
            }
            bool overrun() const { return pos_ > size_ * 8u; }

        private:
            const uint8_t *data_;
            size_t size_;
            size_t pos_;
        };

        // a VLC's value, or false for a code the table doesn't have
        inline bool readVlc(BitReader &br, const VlcTable &table, int &value)
        {
            const VlcEntry e = table.entries[br.peek(table.bits)];
            if (e.length == 0)
                return false;
            br.skip(e.length);
            value = e.value;
            return true;
        }

        // 8x8 inverse DCT from its definition (A.1), separable, 13-bit cosines; raster order in and out, the result
        // saturated to -256..255 (7.5). Rows keep 2 extra fraction bits; the columns work in 64 bits.
        constexpr int kC1 = 8035, kC2 = 7568, kC3 = 6811, kC4 = 5793, kC5 = 4551, kC6 = 3135, kC7 = 1598; // cos(k pi/16) * 2^13
        constexpr int kPass1Bits = 2;

        void idct8x8(int32_t *block)
        {
            int32_t tmp[64];
            for (int r = 0; r < 8; ++r)
            {
                const int32_t *x = block + r * 8;
                int32_t *out = tmp + r * 8;
                if (!(x[1] | x[2] | x[3] | x[4] | x[5] | x[6] | x[7]))
                {
                    const int32_t dc = (x[0] * kC4 + (1 << (12 - kPass1Bits))) >> (13 - kPass1Bits);
                    for (int k = 0; k < 8; ++k)
                        out[k] = dc;
                    continue;
                }
                const int32_t e0 = (x[0] + x[4]) * kC4, e1 = (x[0] - x[4]) * kC4;
                const int32_t e2 = x[2] * kC6 - x[6] * kC2, e3 = x[2] * kC2 + x[6] * kC6;
                const int32_t E0 = e0 + e3, E1 = e1 + e2, E2 = e1 - e2, E3 = e0 - e3;
                const int32_t O0 = x[1] * kC1 + x[3] * kC3 + x[5] * kC5 + x[7] * kC7;
                const int32_t O1 = x[1] * kC3 - x[3] * kC7 - x[5] * kC1 - x[7] * kC5;
                const int32_t O2 = x[1] * kC5 - x[3] * kC1 + x[5] * kC7 + x[7] * kC3;
                const int32_t O3 = x[1] * kC7 - x[3] * kC5 + x[5] * kC3 - x[7] * kC1;
                constexpr int shift = 13 - kPass1Bits;
                constexpr int32_t round = 1 << (shift - 1);
                out[0] = (E0 + O0 + round) >> shift;
                out[7] = (E0 - O0 + round) >> shift;
                out[1] = (E1 + O1 + round) >> shift;
                out[6] = (E1 - O1 + round) >> shift;
                out[2] = (E2 + O2 + round) >> shift;
                out[5] = (E2 - O2 + round) >> shift;
                out[3] = (E3 + O3 + round) >> shift;
                out[4] = (E3 - O3 + round) >> shift;
            }
            for (int c = 0; c < 8; ++c)
            {
                const int32_t *x = tmp + c;
                const int64_t X0 = x[0], X1 = x[8], X2 = x[16], X3 = x[24], X4 = x[32], X5 = x[40], X6 = x[48], X7 = x[56];
                const int64_t e0 = (X0 + X4) * kC4, e1 = (X0 - X4) * kC4;
                const int64_t e2 = X2 * kC6 - X6 * kC2, e3 = X2 * kC2 + X6 * kC6;
                const int64_t E0 = e0 + e3, E1 = e1 + e2, E2 = e1 - e2, E3 = e0 - e3;
                const int64_t O0 = X1 * kC1 + X3 * kC3 + X5 * kC5 + X7 * kC7;
                const int64_t O1 = X1 * kC3 - X3 * kC7 - X5 * kC1 - X7 * kC5;
                const int64_t O2 = X1 * kC5 - X3 * kC1 + X5 * kC7 + X7 * kC3;
                const int64_t O3 = X1 * kC7 - X3 * kC5 + X5 * kC3 - X7 * kC1;
                constexpr int shift = 15 + kPass1Bits;
                constexpr int64_t round = int64_t(1) << (shift - 1);
                auto put = [&](int row, int64_t v) {
                    const int64_t r = (v + round) >> shift;
                    block[row * 8 + c] = static_cast<int32_t>(r < -256 ? -256 : (r > 255 ? 255 : r));
                };
                put(0, E0 + O0);
                put(7, E0 - O0);
                put(1, E1 + O1);
                put(6, E1 - O1);
                put(2, E2 + O2);
                put(5, E2 - O2);
                put(3, E3 + O3);
                put(4, E3 - O3);
            }
        }

        inline uint8_t clampPixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

        // A plane seen as a frame or as one of its fields (base on the field's first line, stride doubled).
        struct PlaneView
        {
            uint8_t *base;
            int stride, width, height;
        };

        // One prediction (7.6.4 - 7.6.7): a w x h block at integer (x, y) of `ref` with half-sample flags, put into
        // dst or averaged with what is there ((a + b + 1) >> 1). Samples outside the reference repeat its edge (the
        // standard forbids such vectors; a damaged stream mustn't read outside the buffer).
        void predict(const PlaneView &ref, int x, int y, int hx, int hy, uint8_t *dst, int dstStride, int w, int h, bool average)
        {
            const bool inside = x >= 0 && y >= 0 && x + w + hx <= ref.width && y + h + hy <= ref.height;
            auto at = [&](int px, int py) -> int {
                px = px < 0 ? 0 : (px >= ref.width ? ref.width - 1 : px);
                py = py < 0 ? 0 : (py >= ref.height ? ref.height - 1 : py);
                return ref.base[py * ref.stride + px];
            };
            for (int j = 0; j < h; ++j)
            {
                uint8_t *d = dst + j * dstStride;
                if (inside)
                {
                    const uint8_t *s0 = ref.base + (y + j) * ref.stride + x;
                    const uint8_t *s1 = s0 + ref.stride;
                    for (int i = 0; i < w; ++i)
                    {
                        int p;
                        if (!hx && !hy)
                            p = s0[i];
                        else if (hx && !hy)
                            p = (s0[i] + s0[i + 1] + 1) >> 1;
                        else if (!hx)
                            p = (s0[i] + s1[i] + 1) >> 1;
                        else
                            p = (s0[i] + s0[i + 1] + s1[i] + s1[i + 1] + 2) >> 2;
                        d[i] = static_cast<uint8_t>(average ? (d[i] + p + 1) >> 1 : p);
                    }
                }
                else
                {
                    for (int i = 0; i < w; ++i)
                    {
                        const int a = at(x + i, y + j), b = at(x + i + hx, y + j), c = at(x + i, y + j + hy),
                                  e = at(x + i + hx, y + j + hy);
                        const int p = (a + b + c + e + 2) >> 2; // also right for the cases without a half sample
                        d[i] = static_cast<uint8_t>(average ? (d[i] + p + 1) >> 1 : p);
                    }
                }
            }
        }

        struct Frame
        {
            std::vector<uint8_t> y, cb, cr;
            char type = '?';
        };

        enum MotionType : int
        {
            kMotionField = 1,
            kMotionFrame = 2,
            kMotionDualPrime = 3,
        };

        // what a macroblock predicts from: kept for the skipped macroblocks of B pictures that repeat it (7.6.6)
        struct Motion
        {
            bool forward = false, backward = false;
            int type = kMotionFrame;
            int mv[2][2][2] = {};       // [r: first / second field vector][s: forward / backward][t: x / y], half samples
            int fieldSelect[2][2] = {}; // [r][s]: the reference field (0 top, 1 bottom)
            int dualPrime[2][2] = {};   // dual prime's derived vectors: [current field][t]
        };
    }

    struct Mpeg2Decoder::State
    {
        const uint8_t *data = nullptr;
        size_t size = 0, pos = 0;
        bool ended = false;

        // sequence
        bool haveSequence = false, haveSequenceExtension = false;
        int width = 0, height = 0, mbWidth = 0, mbHeight = 0, lumaStride = 0, chromaStride = 0;
        int frameRateCode = 0, frameRateN = 0, frameRateD = 0;
        bool progressiveSequence = true;
        int chromaFormat = 1;
        uint8_t intraMatrix[64] = {}, nonIntraMatrix[64] = {};
        bool closedGop = false, brokenLink = false;

        // picture
        int pictureType = 0;
        int fCode[2][2] = {};
        int intraDcPrecision = 0, pictureStructure = 3;
        bool topFieldFirst = false, framePredFrameDct = true, concealmentVectors = false, qScaleType = false;
        bool intraVlcFormat = false, alternateScan = false;

        // frames: two references and the picture being decoded; -1 = none
        Frame frames[3];
        int forward = -1, backward = -1, current = -1;
        bool backwardShown = true; // the newest reference has been output
        int pendingOutput = -1;
        std::vector<uint8_t> mbDone;

        // slice
        int quantiserScale = 1;
        int dcPred[3] = {};
        int pmv[2][2][2] = {};
        Motion previous;

        Mpeg2Stats stats;
        std::string error;

        void fail(const char *what)
        {
            if (error.empty())
                error = what;
        }

        double frameRate() const
        {
            static const double kRates[9] = {0.0, 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0, 50.0, 60000.0 / 1001.0, 60.0};
            const double base = frameRateCode >= 1 && frameRateCode <= 8 ? kRates[frameRateCode] : 30000.0 / 1001.0;
            return base * (frameRateN + 1) / (frameRateD + 1);
        }

        // --- headers -------------------------------------------------------------------------------------------

        void sequenceHeader(size_t at)
        {
            BitReader br(data, size, at);
            const int w = static_cast<int>(br.get(12)), h = static_cast<int>(br.get(12));
            br.skip(4); // aspect_ratio_information
            frameRateCode = static_cast<int>(br.get(4));
            br.skip(18 + 1 + 10 + 1); // bit_rate_value, marker, vbv_buffer_size_value, constrained_parameters_flag
            if (br.get(1))
                for (int i = 0; i < 64; ++i)
                    intraMatrix[kZigzag[i]] = static_cast<uint8_t>(br.get(8));
            else
                std::memcpy(intraMatrix, kDefaultIntraMatrix, 64);
            if (br.get(1))
                for (int i = 0; i < 64; ++i)
                    nonIntraMatrix[kZigzag[i]] = static_cast<uint8_t>(br.get(8));
            else
                std::memset(nonIntraMatrix, 16, 64);
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
            {
                fail("sequence header with an impossible size");
                haveSequence = false;
                return;
            }
            if (w != width || h != height)
            {
                width = w;
                height = h;
                forward = backward = -1;
                backwardShown = true;
                allocate();
            }
            frameRateN = frameRateD = 0;
            haveSequence = true;
            haveSequenceExtension = false; // it must follow each sequence header in MPEG-2
        }

        void allocate()
        {
            mbWidth = (width + 15) / 16;
            mbHeight = progressiveSequence ? (height + 15) / 16 : 2 * ((height + 31) / 32);
            lumaStride = mbWidth * 16;
            chromaStride = mbWidth * 8;
            for (Frame &f : frames)
            {
                f.y.assign(static_cast<size_t>(lumaStride) * mbHeight * 16, 16);
                f.cb.assign(static_cast<size_t>(chromaStride) * mbHeight * 8, 128);
                f.cr.assign(static_cast<size_t>(chromaStride) * mbHeight * 8, 128);
            }
            mbDone.assign(static_cast<size_t>(mbWidth) * mbHeight, 0);
        }

        void extension(size_t at, bool inPicture)
        {
            BitReader br(data, size, at);
            const int id = static_cast<int>(br.get(4));
            if (id == 1) // sequence extension (6.2.2.3)
            {
                br.skip(8); // profile_and_level_indication
                const bool progressive = br.get(1) != 0;
                chromaFormat = static_cast<int>(br.get(2));
                const int wExt = static_cast<int>(br.get(2)), hExt = static_cast<int>(br.get(2));
                br.skip(12 + 1 + 8 + 1); // bit_rate_extension, marker, vbv_buffer_size_extension, low_delay
                frameRateN = static_cast<int>(br.get(2));
                frameRateD = static_cast<int>(br.get(5));
                if (wExt || hExt)
                    fail("picture larger than 4095 (size extension) is not supported");
                if (chromaFormat != 1)
                {
                    fail("only 4:2:0 is supported");
                    haveSequence = false;
                    return;
                }
                if (progressive != progressiveSequence)
                {
                    progressiveSequence = progressive;
                    forward = backward = -1;
                    backwardShown = true;
                    allocate(); // mb_height depends on it
                }
                haveSequenceExtension = true;
            }
            else if (id == 3) // quant matrix extension (6.2.3.2)
            {
                if (br.get(1))
                    for (int i = 0; i < 64; ++i)
                        intraMatrix[kZigzag[i]] = static_cast<uint8_t>(br.get(8));
                if (br.get(1))
                    for (int i = 0; i < 64; ++i)
                        nonIntraMatrix[kZigzag[i]] = static_cast<uint8_t>(br.get(8));
                // the chroma matrices are for 4:2:2 / 4:4:4 only
            }
            else if (id == 8 && inPicture) // picture coding extension (6.2.3.1)
            {
                fCode[0][0] = static_cast<int>(br.get(4));
                fCode[0][1] = static_cast<int>(br.get(4));
                fCode[1][0] = static_cast<int>(br.get(4));
                fCode[1][1] = static_cast<int>(br.get(4));
                intraDcPrecision = static_cast<int>(br.get(2));
                pictureStructure = static_cast<int>(br.get(2));
                topFieldFirst = br.get(1) != 0;
                framePredFrameDct = br.get(1) != 0;
                concealmentVectors = br.get(1) != 0;
                qScaleType = br.get(1) != 0;
                intraVlcFormat = br.get(1) != 0;
                alternateScan = br.get(1) != 0;
                // repeat_first_field, chroma_420_type, progressive_frame, composite_display: display hints only
            }
        }

        // --- macroblocks ---------------------------------------------------------------------------------------

        void setQuantiser(int code)
        {
            quantiserScale = qScaleType ? kNonLinearQuantiserScale[code & 31] : (code & 31) * 2;
        }

        void resetDcPredictors()
        {
            const int v = 1 << (7 + intraDcPrecision);
            dcPred[0] = dcPred[1] = dcPred[2] = v;
        }

        void resetMotionPredictors() { std::memset(pmv, 0, sizeof(pmv)); }

        // one motion_vector(r, s) (6.2.5.2) decoded into motion.mv[r][s] (7.6.3.1)
        bool motionVector(BitReader &br, Motion &m, int r, int s, bool fieldFormat, bool dualPrime, int dmv[2])
        {
            const Tables &t = tables();
            for (int c = 0; c < 2; ++c)
            {
                int code = 0;
                if (!readVlc(br, t.motionCode, code))
                    return false;
                if (code != 0 && br.get(1))
                    code = -code;
                const int f = fCode[s][c];
                if (f < 1 || f > 9)
                    return false;
                const int rSize = f - 1;
                int residual = 0;
                if (rSize != 0 && code != 0)
                    residual = static_cast<int>(br.get(rSize));
                if (dualPrime)
                {
                    if (br.peek(1) == 0)
                    {
                        br.skip(1);
                        dmv[c] = 0;
                    }
                    else
                        dmv[c] = br.get(2) == 2 ? 1 : -1; // '10' +1, '11' -1
                }
                const int fScale = 1 << rSize;
                const int high = 16 * fScale - 1, low = -16 * fScale, range = 32 * fScale;
                int delta;
                if (fScale == 1 || code == 0)
                    delta = code;
                else
                {
                    delta = (std::abs(code) - 1) * fScale + residual + 1;
                    if (code < 0)
                        delta = -delta;
                }
                const bool halve = fieldFormat && c == 1 && pictureStructure == 3;
                const int prediction = halve ? (pmv[r][s][c] >> 1) : pmv[r][s][c];
                int v = prediction + delta;
                if (v < low)
                    v += range;
                if (v > high)
                    v -= range;
                m.mv[r][s][c] = v;
                pmv[r][s][c] = halve ? v * 2 : v;
            }
            return true;
        }

        // motion_vectors(s) (6.2.5.1)
        bool motionVectors(BitReader &br, Motion &m, int s, int count, bool fieldFormat, bool dualPrime, int dmv[2])
        {
            if (count == 1)
            {
                if (fieldFormat && !dualPrime)
                    m.fieldSelect[0][s] = static_cast<int>(br.get(1));
                if (!motionVector(br, m, 0, s, fieldFormat, dualPrime, dmv))
                    return false;
                pmv[1][s][0] = pmv[0][s][0];
                pmv[1][s][1] = pmv[0][s][1];
                return true;
            }
            m.fieldSelect[0][s] = static_cast<int>(br.get(1));
            if (!motionVector(br, m, 0, s, fieldFormat, false, dmv))
                return false;
            m.fieldSelect[1][s] = static_cast<int>(br.get(1));
            return motionVector(br, m, 1, s, fieldFormat, false, dmv);
        }

        PlaneView view(std::vector<uint8_t> &plane, int stride, int lines, int field) const
        {
            // field: -1 the frame, 0 / 1 its top / bottom field
            if (field < 0)
                return PlaneView{plane.data(), stride, stride, lines};
            return PlaneView{plane.data() + field * stride, stride * 2, stride, lines / 2};
        }

        // the prediction of one direction into the current macroblock (7.6, frame pictures)
        void predictMacroblock(const Motion &m, int s, Frame &ref, Frame &cur, int mbx, int mby, bool average)
        {
            const int lumaLines = mbHeight * 16, chromaLines = mbHeight * 8;
            auto luma = [&](int field, int x, int y, int mvx, int mvy, int w, int h, uint8_t *dst, int dstStride, bool avg) {
                predict(view(ref.y, lumaStride, lumaLines, field), x + (mvx >> 1), y + (mvy >> 1), mvx & 1, mvy & 1, dst,
                        dstStride, w, h, avg);
            };
            auto chroma = [&](int field, int x, int y, int mvx, int mvy, int w, int h, int dstLine, int dstStep, bool avg) {
                const int cx = mvx / 2, cy = mvy / 2; // 4:2:0: halved toward zero (7.6.3.7)
                predict(view(ref.cb, chromaStride, chromaLines, field), x + (cx >> 1), y + (cy >> 1), cx & 1, cy & 1,
                        cur.cb.data() + dstLine * chromaStride + mbx * 8, chromaStride * dstStep, w, h, avg);
                predict(view(ref.cr, chromaStride, chromaLines, field), x + (cx >> 1), y + (cy >> 1), cx & 1, cy & 1,
                        cur.cr.data() + dstLine * chromaStride + mbx * 8, chromaStride * dstStep, w, h, avg);
            };
            uint8_t *lumaDst = cur.y.data() + mby * 16 * lumaStride + mbx * 16;
            if (m.type == kMotionFrame)
            {
                const int mvx = m.mv[0][s][0], mvy = m.mv[0][s][1];
                luma(-1, mbx * 16, mby * 16, mvx, mvy, 16, 16, lumaDst, lumaStride, average);
                chroma(-1, mbx * 8, mby * 8, mvx, mvy, 8, 8, mby * 8, 1, average);
                return;
            }
            if (m.type == kMotionField)
            {
                for (int r = 0; r < 2; ++r) // r: the current macroblock's top / bottom field lines
                {
                    const int mvx = m.mv[r][s][0], mvy = m.mv[r][s][1], sel = m.fieldSelect[r][s];
                    luma(sel, mbx * 16, mby * 8, mvx, mvy, 16, 8, lumaDst + r * lumaStride, lumaStride * 2, average);
                    chroma(sel, mbx * 8, mby * 4, mvx, mvy, 8, 4, mby * 8 + r, 2, average);
                }
                return;
            }
            // dual prime (7.6.3.6): each field the average of its own parity and the other one
            for (int r = 0; r < 2; ++r)
            {
                const int mvx = m.mv[0][0][0], mvy = m.mv[0][0][1];
                luma(r, mbx * 16, mby * 8, mvx, mvy, 16, 8, lumaDst + r * lumaStride, lumaStride * 2, false);
                chroma(r, mbx * 8, mby * 4, mvx, mvy, 8, 4, mby * 8 + r, 2, false);
                const int dx = m.dualPrime[r][0], dy = m.dualPrime[r][1];
                luma(1 - r, mbx * 16, mby * 8, dx, dy, 16, 8, lumaDst + r * lumaStride, lumaStride * 2, true);
                chroma(1 - r, mbx * 8, mby * 4, dx, dy, 8, 4, mby * 8 + r, 2, true);
            }
        }

        void predictAll(const Motion &m, int mbx, int mby)
        {
            Frame &cur = frames[current];
            // P pictures predict from the newest reference; B pictures forward from the older one (a closed GOP's
            // first B pictures have none: they only predict backward)
            const int fwd = pictureType == 2 ? backward : (forward >= 0 ? forward : backward);
            bool any = false;
            if (m.forward && fwd >= 0)
            {
                predictMacroblock(m, 0, frames[fwd], cur, mbx, mby, false);
                any = true;
            }
            if (m.backward && backward >= 0)
            {
                predictMacroblock(m, 1, frames[backward], cur, mbx, mby, any);
                any = true;
            }
            (void)any;
        }

        // one block (6.2.6, 7.2 - 7.4): its coefficients into `blk` (raster order, dequantised)
        bool block(BitReader &br, int index, bool intra, int32_t *blk)
        {
            const Tables &t = tables();
            const uint8_t *scan = alternateScan ? kAlternateScan : kZigzag;
            std::memset(blk, 0, 64 * sizeof(int32_t));
            int sum = 0;
            int i;
            const uint8_t *matrix = intra ? intraMatrix : nonIntraMatrix;
            const VlcTable *table = &t.dctB14;
            if (intra)
            {
                const int cc = index < 4 ? 0 : index - 3;
                int dcSize = 0;
                if (!readVlc(br, cc == 0 ? t.dcSizeLuma : t.dcSizeChroma, dcSize))
                    return false;
                int diff = 0;
                if (dcSize > 0)
                {
                    const int bits = static_cast<int>(br.get(dcSize));
                    diff = (bits & (1 << (dcSize - 1))) ? bits : bits - (1 << dcSize) + 1;
                }
                dcPred[cc] += diff;
                const int dc = dcPred[cc] * (8 >> intraDcPrecision);
                blk[0] = std::clamp(dc, -2048, 2047);
                sum = blk[0];
                i = 0;
                if (intraVlcFormat)
                    table = &t.dctB15;
            }
            else
                i = -1;
            bool first = !intra;
            for (;;)
            {
                int run, level;
                if (first && br.peek(1) == 1)
                {
                    br.skip(1); // "1s": run 0, level 1 as a non-intra block's first coefficient
                    run = 0;
                    level = br.get(1) ? -1 : 1;
                }
                else
                {
                    const VlcEntry e = table->entries[br.peek(table->bits)];
                    if (e.length == 0)
                        return false;
                    br.skip(e.length);
                    if (e.value == kDctEob)
                        break;
                    if (e.value == kDctEscape)
                    {
                        run = static_cast<int>(br.get(6));
                        level = static_cast<int>(br.get(12));
                        if (level & 0x800)
                            level -= 0x1000;
                        if (level == 0 || level == -2048)
                            return false;
                    }
                    else
                    {
                        run = e.value & 0xFF;
                        level = e.value >> 8;
                        if (br.get(1))
                            level = -level;
                    }
                }
                first = false;
                i += run + 1;
                if (i > 63)
                    return false;
                const int pos = scan[i];
                // 7.4.2.3: (2 QF + k) W q / 32, k = 0 intra, sign(QF) otherwise; "/" truncates toward zero
                const int k = intra ? 0 : (level > 0 ? 1 : -1);
                int v = ((2 * level + k) * matrix[pos] * quantiserScale) / 32;
                v = std::clamp(v, -2048, 2047);
                blk[pos] = v;
                sum += v;
                if (br.overrun())
                    return false;
            }
            if ((sum & 1) == 0) // mismatch control (7.4.4)
                blk[63] ^= 1;
            return true;
        }

        // where block `index` of a macroblock sits: plane, first sample, line step
        void blockTarget(int index, int mbx, int mby, bool fieldDct, uint8_t *&dst, int &stride)
        {
            Frame &cur = frames[current];
            if (index < 4)
            {
                const int x = mbx * 16 + (index & 1) * 8;
                if (fieldDct)
                {
                    dst = cur.y.data() + (mby * 16 + (index >> 1)) * lumaStride + x;
                    stride = lumaStride * 2;
                }
                else
                {
                    dst = cur.y.data() + (mby * 16 + (index >> 1) * 8) * lumaStride + x;
                    stride = lumaStride;
                }
                return;
            }
            std::vector<uint8_t> &plane = index == 4 ? cur.cb : cur.cr;
            dst = plane.data() + mby * 8 * chromaStride + mbx * 8;
            stride = chromaStride;
        }

        bool macroblock(BitReader &br, int address)
        {
            const Tables &t = tables();
            const int mbx = address % mbWidth, mby = address / mbWidth;
            int type = 0;
            const VlcTable &typeTable = pictureType == 1 ? t.mbTypeI : (pictureType == 2 ? t.mbTypeP : t.mbTypeB);
            if (!readVlc(br, typeTable, type))
                return false;
            const bool intra = (type & kMbIntra) != 0;
            Motion m;
            m.forward = (type & kMbForward) != 0;
            m.backward = (type & kMbBackward) != 0;
            m.type = kMotionFrame;
            if ((m.forward || m.backward) && !framePredFrameDct)
            {
                m.type = static_cast<int>(br.get(2));
                if (m.type == 0)
                    return false;
            }
            bool fieldDct = false;
            if (!framePredFrameDct && (intra || (type & kMbPattern)))
                fieldDct = br.get(1) != 0;
            if (type & kMbQuant)
            {
                const int code = static_cast<int>(br.get(5));
                if (code == 0)
                    return false;
                setQuantiser(code);
            }
            const bool fieldFormat = m.type != kMotionFrame;
            const bool dualPrime = m.type == kMotionDualPrime;
            const int count = m.type == kMotionField ? 2 : 1;
            int dmv[2] = {0, 0};
            if (m.forward || (intra && concealmentVectors))
            {
                // intra concealment vectors: one frame vector
                if (!motionVectors(br, m, 0, intra ? 1 : count, intra ? false : fieldFormat, intra ? false : dualPrime, dmv))
                    return false;
            }
            if (m.backward && !motionVectors(br, m, 1, count, fieldFormat, false, dmv))
                return false;
            if (intra && concealmentVectors)
                br.skip(1); // marker
            if (dualPrime)
            {
                const int mvx = m.mv[0][0][0], mvy = m.mv[0][0][1];
                for (int r = 0; r < 2; ++r)
                {
                    // r = 0: the top field from the bottom one; r = 1: the bottom field from the top one
                    const int mul = (r == 0) == topFieldFirst ? 1 : 3;
                    const int e = r == 0 ? -1 : 1;
                    m.dualPrime[r][0] = ((mvx * mul + (mvx > 0)) >> 1) + dmv[0];
                    m.dualPrime[r][1] = ((mvy * mul + (mvy > 0)) >> 1) + e + dmv[1];
                }
            }

            int32_t blk[64];
            if (intra)
            {
                if (!concealmentVectors)
                    resetMotionPredictors();
                for (int b = 0; b < 6; ++b)
                {
                    if (!block(br, b, true, blk))
                        return false;
                    idct8x8(blk);
                    uint8_t *dst;
                    int stride;
                    blockTarget(b, mbx, mby, fieldDct, dst, stride);
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            dst[y * stride + x] = clampPixel(blk[y * 8 + x]);
                }
                previous = Motion{};
                mbDone[address] = 1;
                return true;
            }

            resetDcPredictors();
            if (pictureType == 2 && !m.forward)
            {
                // P picture, no motion compensation: frame prediction from the reference with a zero vector
                resetMotionPredictors();
                m.forward = true;
                m.type = kMotionFrame;
            }
            predictAll(m, mbx, mby);
            previous = m;
            if (type & kMbPattern)
            {
                int cbp = 0;
                if (!readVlc(br, t.codedBlockPattern, cbp))
                    return false;
                for (int b = 0; b < 6; ++b)
                {
                    if (!(cbp & (32 >> b)))
                        continue;
                    if (!block(br, b, false, blk))
                        return false;
                    idct8x8(blk);
                    uint8_t *dst;
                    int stride;
                    blockTarget(b, mbx, mby, fieldDct, dst, stride);
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            dst[y * stride + x] = clampPixel(dst[y * stride + x] + blk[y * 8 + x]);
                }
            }
            mbDone[address] = 1;
            return true;
        }

        void skippedMacroblock(int address)
        {
            const int mbx = address % mbWidth, mby = address / mbWidth;
            resetDcPredictors();
            Motion m;
            if (pictureType == 2)
            {
                resetMotionPredictors();
                m.forward = true;
                m.type = kMotionFrame;
            }
            else
                m = previous; // B: the previous macroblock's prediction and vectors
            predictAll(m, mbx, mby);
            mbDone[address] = 1;
        }

        // slice() (6.2.4): false on a bitstream error (the rest of the slice is abandoned)
        bool slice(int row, size_t at)
        {
            if (row >= mbHeight)
                return false;
            BitReader br(data, size, at);
            const int code = static_cast<int>(br.get(5));
            if (code == 0)
                return false;
            setQuantiser(code);
            if (br.peek(1))
            {
                br.skip(1 + 1 + 7); // intra_slice_flag, intra_slice, reserved_bits
                while (br.peek(1))
                    br.skip(9); // extra_bit_slice, extra_information_slice
            }
            br.skip(1); // extra_bit_slice
            resetDcPredictors();
            resetMotionPredictors();
            previous = Motion{};
            const Tables &t = tables();
            const int rowEnd = (row + 1) * mbWidth;
            int address = -1;
            for (;;)
            {
                int increment = 0;
                for (;;)
                {
                    const uint32_t next = br.peek(11);
                    if (next == 0x008) // macroblock_escape
                    {
                        br.skip(11);
                        increment += 33;
                        continue;
                    }
                    if (next == 0x00F) // stuffing (MPEG-1)
                    {
                        br.skip(11);
                        continue;
                    }
                    int v = 0;
                    if (!readVlc(br, t.mbaIncrement, v) || v > 33)
                        return false;
                    increment += v;
                    break;
                }
                if (address < 0)
                    address = row * mbWidth + increment - 1;
                else
                {
                    if (pictureType == 1 && increment > 1)
                        return false; // no skipped macroblocks in I pictures
                    for (int k = 1; k < increment; ++k)
                    {
                        if (++address >= rowEnd)
                            return false;
                        skippedMacroblock(address);
                    }
                    ++address;
                }
                if (address >= rowEnd)
                    return false;
                if (!macroblock(br, address))
                    return false;
                if (br.overrun())
                    return false;
                if (br.peek(23) == 0) // the next start code: end of the slice
                    return true;
            }
        }

        // the macroblocks no slice reached: as the reference had them (or left as they were)
        void conceal()
        {
            const int ref = pictureType == 1 ? -1 : (pictureType == 2 || forward < 0 ? backward : forward);
            if (ref < 0 || ref == current)
                return;
            Frame &cur = frames[current], &src = frames[ref];
            for (int a = 0; a < mbWidth * mbHeight; ++a)
            {
                if (mbDone[a])
                    continue;
                const int mbx = a % mbWidth, mby = a / mbWidth;
                for (int y = 0; y < 16; ++y)
                    std::memcpy(&cur.y[(mby * 16 + y) * lumaStride + mbx * 16], &src.y[(mby * 16 + y) * lumaStride + mbx * 16], 16);
                for (int y = 0; y < 8; ++y)
                {
                    std::memcpy(&cur.cb[(mby * 8 + y) * chromaStride + mbx * 8], &src.cb[(mby * 8 + y) * chromaStride + mbx * 8], 8);
                    std::memcpy(&cur.cr[(mby * 8 + y) * chromaStride + mbx * 8], &src.cr[(mby * 8 + y) * chromaStride + mbx * 8], 8);
                }
            }
        }

        // skips the picture's slices (pos ends on the start code after them)
        void skipSlices()
        {
            for (;;)
            {
                const size_t sc = findStartCode(data, size, pos);
                if (sc == kNoStartCode)
                {
                    pos = size;
                    return;
                }
                const uint8_t code = data[sc + 3];
                if (code < 0x01 || code > 0xAF)
                {
                    pos = sc;
                    return;
                }
                pos = sc + 4;
            }
        }

        // picture_header() at `at` (after its start code), then its extensions and slices (6.2.3, 6.2.4)
        void picture(size_t at)
        {
            BitReader br(data, size, at);
            br.skip(10); // temporal_reference
            pictureType = static_cast<int>(br.get(3));
            pos = at;
            // the picture's extensions and user data, up to its first slice
            bool codingExtension = false;
            for (;;)
            {
                const size_t sc = findStartCode(data, size, pos);
                if (sc == kNoStartCode)
                {
                    pos = size;
                    return;
                }
                const uint8_t code = data[sc + 3];
                if (code == 0xB5)
                {
                    if (sc + 4 < size && (data[sc + 4] >> 4) == 8)
                        codingExtension = true;
                    extension(sc + 4, true);
                    pos = sc + 4;
                    continue;
                }
                if (code == 0xB2)
                {
                    pos = sc + 4;
                    continue;
                }
                pos = sc;
                break;
            }
            if (!haveSequence || !haveSequenceExtension || !codingExtension)
            {
                fail(haveSequence ? "MPEG-1 video (no MPEG-2 extensions) is not supported" : "picture before any sequence header");
                ++stats.unsupported;
                skipSlices();
                return;
            }
            if (pictureStructure != 3)
            {
                fail("field pictures are not supported");
                ++stats.unsupported;
                skipSlices();
                return;
            }
            if (pictureType < 1 || pictureType > 3)
            {
                ++stats.unsupported;
                skipSlices();
                return;
            }
            if (pictureType == 2 && backward < 0)
            {
                ++stats.unsupported; // a P picture with nothing to predict from
                skipSlices();
                return;
            }
            if (pictureType == 3 && (backward < 0 || (forward < 0 && !closedGop)))
            {
                ++stats.skippedB; // an open GOP's leading B pictures without the GOP before
                skipSlices();
                return;
            }
            current = 0;
            while (current == forward || current == backward)
                ++current;
            std::fill(mbDone.begin(), mbDone.end(), 0);

            for (;;)
            {
                const size_t sc = findStartCode(data, size, pos);
                if (sc == kNoStartCode)
                {
                    pos = size;
                    break;
                }
                const uint8_t code = data[sc + 3];
                if (code < 0x01 || code > 0xAF)
                {
                    pos = sc;
                    break;
                }
                if (!slice(code - 1, sc + 4))
                {
                    ++stats.sliceErrors;
                    fail("bitstream error in a slice");
                }
                pos = sc + 4;
            }
            conceal();
            ++stats.pictures;
            frames[current].type = "?IPB"[pictureType];
            if (pictureType == 3)
            {
                pendingOutput = current;
                return;
            }
            // a new reference: the previous one is shown now (display order)
            if (backward >= 0 && !backwardShown)
                pendingOutput = backward;
            forward = backward;
            backward = current;
            backwardShown = false;
            if (brokenLink && pictureType == 1)
            {
                forward = -1; // the B pictures after a broken link can't be decoded (6.3.8)
                brokenLink = false;
            }
        }

        void endOfSequence()
        {
            if (backward >= 0 && !backwardShown)
            {
                pendingOutput = backward;
                backwardShown = true;
            }
        }
    };

    Mpeg2Decoder::Mpeg2Decoder() : s_(std::make_unique<State>()) {}
    Mpeg2Decoder::~Mpeg2Decoder() = default;

    void Mpeg2Decoder::open(const uint8_t *data, size_t size)
    {
        s_ = std::make_unique<State>();
        s_->data = data;
        s_->size = size;
    }

    const Mpeg2Stats &Mpeg2Decoder::stats() const { return s_->stats; }
    const std::string &Mpeg2Decoder::lastError() const { return s_->error; }

    bool Mpeg2Decoder::next(Mpeg2Picture &out)
    {
        State &s = *s_;
        for (;;)
        {
            if (s.pendingOutput >= 0)
            {
                const Frame &f = s.frames[s.pendingOutput];
                if (s.pendingOutput == s.backward)
                    s.backwardShown = true;
                s.pendingOutput = -1;
                out.width = s.width;
                out.height = s.height;
                out.y = f.y.data();
                out.cb = f.cb.data();
                out.cr = f.cr.data();
                out.yStride = s.lumaStride;
                out.cStride = s.chromaStride;
                out.type = f.type;
                out.frameRate = s.frameRate();
                return true;
            }
            if (s.ended || !s.data)
                return false;
            const size_t sc = findStartCode(s.data, s.size, s.pos);
            if (sc == kNoStartCode)
            {
                s.ended = true;
                s.endOfSequence();
                continue;
            }
            const uint8_t code = s.data[sc + 3];
            s.pos = sc + 4;
            switch (code)
            {
            case 0xB3:
                s.sequenceHeader(sc + 4);
                break;
            case 0xB5:
                s.extension(sc + 4, false);
                break;
            case 0xB8: // group_of_pictures_header (6.2.2.6): time_code, closed_gop, broken_link
            {
                BitReader br(s.data, s.size, sc + 4);
                br.skip(25);
                s.closedGop = br.get(1) != 0;
                s.brokenLink = br.get(1) != 0;
                break;
            }
            case 0x00:
                s.picture(sc + 4);
                break;
            case 0xB7:
                s.endOfSequence();
                break;
            default:
                break; // user data, stray slices
            }
        }
    }
}
