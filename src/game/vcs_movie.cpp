// VCS movies (docs/NATIVE_PORT_ROADMAP.md milestone 5; docs/game-internals.md 3 "Movies").
//
// At boot FUN_0021CA70 plays cdrom0:\MOVIES\STINGS.PSS (the logos; skip threshold 1,000,000 frames - never) and
// INTRO.PSS (threshold 10) through CMoviePlayer::Play(path, 0, threshold) at 0x40D8B8, which drives libmpeg / the
// IPU and streams the sound through the IOP. Here Play is replaced: the file is read from the disc and demuxed
// (MPEG-2 program stream: video in stream 0xE0; sound in private stream 1, sub-stream 0xFF, behind a 4-byte header -
// a Sony "SShd" header (type 1 = 16-bit little-endian PCM, 48 kHz, 2 channels, 512-byte interleave: 256 left samples,
// then 256 right), "SSbd" + size, then the samples), the video decoded by our MPEG-2 decoder (src/video/mpeg2.h) into
// RGBA and shown by the presenter at 4:3 (runtime/ps2_present.h), the sound mixed into the audio output
// (runtime/ps2_audio_out.h) - its played-sample count is the clock the pictures follow. The game thread waits in here,
// as it did in the game's own player.
//
// VCS_MOVIE_DUMP=<dir>: before a movie plays, every 30th picture is written there as <MOVIE>_<n>.ppm (the reference
// tools/movie_check.py compares with).
#include "game/vcs_movie.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_present.h"
#include "video/mpeg2.h"
#include "video/yuv.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "runtime/ps2_host_std.h"

// The FFmpeg decoder as the reference the native one was checked against (VCS_MOVIE_DECODER=ffmpeg, VCS_MOVIE_VERIFY=1);
// removed with FFmpeg once checked.
#if __has_include(<libavcodec/avcodec.h>)
#define VCS_FFMPEG_REFERENCE 1
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#else
#define VCS_FFMPEG_REFERENCE 0
#endif

namespace vcs
{
    namespace
    {
        constexpr uint32_t kMoviePlay = 0x0040D8B8u; // CMoviePlayer::Play(path, 0, frames before a skip is allowed)
        std::atomic<bool> g_skipIntro{false};

        // The PSS file, split into its video elementary stream and its sound.
        struct Pss
        {
            std::vector<uint8_t> video;
            std::vector<uint8_t> sound; // the private stream's bytes, sub-header removed (SShd / SSbd / samples)
            double videoStart = 0.0, soundStart = 0.0; // seconds: the first PTS of each
        };

        double readPts(const uint8_t *p)
        {
            const uint64_t pts = (static_cast<uint64_t>((p[0] >> 1) & 7u) << 30) | (static_cast<uint64_t>(p[1]) << 22) |
                                 (static_cast<uint64_t>(p[2] >> 1) << 15) | (static_cast<uint64_t>(p[3]) << 7) | (p[4] >> 1);
            return pts / 90000.0;
        }

        bool demux(const std::vector<uint8_t> &file, Pss &out)
        {
            bool videoPts = false, soundPts = false;
            size_t i = 0;
            const size_t n = file.size();
            while (i + 6 <= n)
            {
                if (file[i] != 0 || file[i + 1] != 0 || file[i + 2] != 1)
                {
                    ++i;
                    continue;
                }
                const uint8_t id = file[i + 3];
                if (id == 0xBA) // pack header (MPEG-2): 14 bytes + stuffing
                {
                    if (i + 14 > n)
                        break;
                    i += 14u + (file[i + 13] & 7u);
                    continue;
                }
                if (id == 0xB9) // end
                    break;
                if (id == 0xBB || id == 0xBD || id == 0xBE || id == 0xBF || (id >= 0xC0 && id <= 0xEF))
                {
                    const size_t length = (static_cast<size_t>(file[i + 4]) << 8) | file[i + 5];
                    const size_t body = i + 6, end = std::min(n, body + length);
                    if (id == 0xE0 || id == 0xBD)
                    {
                        if (body + 3 > end)
                            break;
                        const uint8_t flags = file[body + 1], headerLength = file[body + 2];
                        size_t payload = body + 3 + headerLength;
                        if (payload > end)
                            break;
                        if (id == 0xE0)
                        {
                            if ((flags & 0x80) && !videoPts)
                            {
                                out.videoStart = readPts(&file[body + 3]);
                                videoPts = true;
                            }
                            out.video.insert(out.video.end(), file.begin() + payload, file.begin() + end);
                        }
                        else if (payload + 4 <= end && file[payload] == 0xFF) // sub-stream 0xFF: the sound
                        {
                            if ((flags & 0x80) && !soundPts)
                            {
                                out.soundStart = readPts(&file[body + 3]);
                                soundPts = true;
                            }
                            out.sound.insert(out.sound.end(), file.begin() + payload + 4, file.begin() + end);
                        }
                    }
                    i = end;
                    continue;
                }
                ++i;
            }
            return !out.video.empty();
        }

        // The SShd / SSbd sound as interleaved stereo frames (empty if the format is not the PCM VCS uses).
        std::vector<int16_t> soundFrames(const std::vector<uint8_t> &sound)
        {
            std::vector<int16_t> out;
            auto u32 = [&](size_t at) {
                uint32_t v;
                std::memcpy(&v, &sound[at], 4);
                return v;
            };
            if (sound.size() < 40 || std::memcmp(sound.data(), "SShd", 4) != 0 || std::memcmp(&sound[32], "SSbd", 4) != 0)
                return out;
            const uint32_t type = u32(8), rate = u32(12), channels = u32(16), interleave = u32(20);
            if (type != 1u || rate != 48000u || channels != 2u || interleave == 0u || interleave % 2u)
            {
                std::fprintf(stderr, "[movie] sound type %u, %u Hz, %u channels, interleave %u: not played\n", type, rate, channels,
                             interleave);
                return out;
            }
            const size_t dataSize = std::min<size_t>(u32(36), sound.size() - 40);
            const uint8_t *data = &sound[40];
            const size_t samplesPerBlock = interleave / 2u;
            const size_t pairs = dataSize / (interleave * 2u);
            out.resize(pairs * samplesPerBlock * 2u);
            for (size_t b = 0; b < pairs; ++b)
            {
                const uint8_t *left = data + b * interleave * 2u, *right = left + interleave;
                for (size_t k = 0; k < samplesPerBlock; ++k)
                {
                    int16_t l, r;
                    std::memcpy(&l, left + k * 2u, 2);
                    std::memcpy(&r, right + k * 2u, 2);
                    out[(b * samplesPerBlock + k) * 2u] = l;
                    out[(b * samplesPerBlock + k) * 2u + 1u] = r;
                }
            }
            return out;
        }

        // Start / Cross on the emulated pad (keyboard, gamepad or injected input; active low)
        bool skipPressed()
        {
            uint8_t pad[32] = {};
            PSPadBackend backend;
            if (!backend.readState(0, 0, pad, sizeof(pad)))
                return false;
            const uint16_t buttons = static_cast<uint16_t>(~(pad[2] | (pad[3] << 8)));
            return (buttons & (0x0008u | 0x4000u)) != 0u;
        }

        double nowSeconds()
        {
            return std::chrono::duration<double>(ps2x::hostNow().time_since_epoch()).count();
        }

        // Pictures in display order, as YCbCr 4:2:0 planes valid until the next call.
        class PictureSource
        {
        public:
            virtual ~PictureSource() = default;
            virtual bool next(video::Mpeg2Picture &out) = 0;
            virtual void report(const std::string &path) const { (void)path; }
        };

        class NativeSource final : public PictureSource
        {
        public:
            explicit NativeSource(const std::vector<uint8_t> &es) { decoder_.open(es.data(), es.size()); }
            bool next(video::Mpeg2Picture &out) override { return decoder_.next(out); }
            void report(const std::string &path) const override
            {
                const video::Mpeg2Stats &s = decoder_.stats();
                if (s.sliceErrors || s.unsupported || s.skippedB)
                    std::fprintf(stderr, "[movie] %s: %llu slice errors, %llu pictures not supported, %llu B pictures skipped (%s)\n",
                                 path.c_str(), static_cast<unsigned long long>(s.sliceErrors),
                                 static_cast<unsigned long long>(s.unsupported), static_cast<unsigned long long>(s.skippedB),
                                 decoder_.lastError().c_str());
            }

        private:
            video::Mpeg2Decoder decoder_;
        };

#if VCS_FFMPEG_REFERENCE
        class FfmpegSource final : public PictureSource
        {
        public:
            explicit FfmpegSource(const std::vector<uint8_t> &es) : es_(es)
            {
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
                decoder_ = codec ? avcodec_alloc_context3(codec) : nullptr;
                parser_ = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
                ok_ = decoder_ && parser_ && avcodec_open2(decoder_, codec, nullptr) >= 0;
                packet_ = av_packet_alloc();
                frame_ = av_frame_alloc();
            }
            ~FfmpegSource() override
            {
                av_frame_free(&frame_);
                av_packet_free(&packet_);
                if (parser_)
                    av_parser_close(parser_);
                avcodec_free_context(&decoder_);
            }
            bool next(video::Mpeg2Picture &out) override
            {
                while (ok_)
                {
                    const int got = avcodec_receive_frame(decoder_, frame_);
                    if (got == AVERROR(EAGAIN))
                    {
                        if (offset_ < es_.size())
                        {
                            uint8_t *data = nullptr;
                            int dataSize = 0;
                            const int used = av_parser_parse2(parser_, decoder_, &data, &dataSize, es_.data() + offset_,
                                                              static_cast<int>(std::min<size_t>(4096u, es_.size() - offset_)),
                                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                            offset_ += static_cast<size_t>(std::max(used, 0));
                            if (dataSize > 0)
                            {
                                packet_->data = data;
                                packet_->size = dataSize;
                                avcodec_send_packet(decoder_, packet_);
                            }
                        }
                        else if (!flushing_)
                        {
                            avcodec_send_packet(decoder_, nullptr);
                            flushing_ = true;
                        }
                        else
                            return false;
                        continue;
                    }
                    if (got < 0 || frame_->format != AV_PIX_FMT_YUV420P)
                        return false;
                    out.width = frame_->width;
                    out.height = frame_->height;
                    out.y = frame_->data[0];
                    out.cb = frame_->data[1];
                    out.cr = frame_->data[2];
                    out.yStride = frame_->linesize[0];
                    out.cStride = frame_->linesize[1];
                    out.type = av_get_picture_type_char(frame_->pict_type);
                    out.frameRate = decoder_->framerate.num > 0 && decoder_->framerate.den > 0 ? av_q2d(decoder_->framerate)
                                                                                                : 30000.0 / 1001.0;
                    return true;
                }
                return false;
            }

        private:
            const std::vector<uint8_t> &es_;
            AVCodecContext *decoder_ = nullptr;
            AVCodecParserContext *parser_ = nullptr;
            AVPacket *packet_ = nullptr;
            AVFrame *frame_ = nullptr;
            size_t offset_ = 0;
            bool ok_ = false, flushing_ = false;
        };

        // VCS_MOVIE_VERIFY=1: every picture of both decoders compared before the movie plays
        void verify(const std::string &path, const std::vector<uint8_t> &es)
        {
            NativeSource native(es);
            FfmpegSource reference(es);
            video::Mpeg2Picture a, b;
            uint64_t count = 0, typeMismatches = 0;
            double minPsnr = 99.0, sumPsnr = 0.0;
            int maxDiff = 0;
            uint64_t worst = 0;
            bool moreA = true, moreB = true;
            uint64_t onlyA = 0, onlyB = 0;
            for (;;)
            {
                moreA = moreA && native.next(a);
                moreB = moreB && reference.next(b);
                if (!moreA && !moreB)
                    break;
                if (moreA != moreB)
                {
                    ++(moreA ? onlyA : onlyB);
                    continue;
                }
                if (a.width != b.width || a.height != b.height)
                {
                    std::fprintf(stderr, "[movie-verify] %s picture %llu: size %dx%d vs %dx%d\n", path.c_str(),
                                 static_cast<unsigned long long>(count), a.width, a.height, b.width, b.height);
                    return;
                }
                double se = 0.0;
                int pictureMax = 0;
                auto plane = [&](const uint8_t *p, int ps, const uint8_t *q, int qs, int w, int h, bool luma) {
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const int d = int(p[y * ps + x]) - int(q[y * qs + x]);
                            if (luma)
                                se += double(d) * d;
                            pictureMax = std::max(pictureMax, std::abs(d));
                        }
                };
                plane(a.y, a.yStride, b.y, b.yStride, a.width, a.height, true);
                plane(a.cb, a.cStride, b.cb, b.cStride, (a.width + 1) / 2, (a.height + 1) / 2, false);
                plane(a.cr, a.cStride, b.cr, b.cStride, (a.width + 1) / 2, (a.height + 1) / 2, false);
                const double mse = se / (double(a.width) * a.height);
                const double psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
                if (psnr < minPsnr)
                {
                    minPsnr = psnr;
                    worst = count;
                }
                sumPsnr += psnr;
                maxDiff = std::max(maxDiff, pictureMax);
                if (a.type != b.type)
                    ++typeMismatches;
                if (psnr < 45.0)
                    std::fprintf(stderr, "[movie-verify] %s picture %llu (%c): Y %.2f dB, max difference %d\n", path.c_str(),
                                 static_cast<unsigned long long>(count), a.type, psnr, pictureMax);
                ++count;
            }
            native.report(path);
            std::fprintf(stderr,
                         "[movie-verify] %s: %llu pictures compared (%llu only native, %llu only FFmpeg, %llu types differ); Y min "
                         "%.2f dB (picture %llu), mean %.2f dB; largest sample difference %d; %s\n",
                         path.c_str(), static_cast<unsigned long long>(count), static_cast<unsigned long long>(onlyA),
                         static_cast<unsigned long long>(onlyB), static_cast<unsigned long long>(typeMismatches), minPsnr,
                         static_cast<unsigned long long>(worst), count ? sumPsnr / count : 0.0, maxDiff,
                         (onlyA == 0 && onlyB == 0 && typeMismatches == 0 && minPsnr >= 45.0) ? "PASS" : "FAIL");
        }
#endif

        bool useNative()
        {
#if VCS_FFMPEG_REFERENCE
            const char *e = std::getenv("VCS_MOVIE_DECODER");
            return !e || std::strcmp(e, "ffmpeg") != 0;
#else
            return true;
#endif
        }

        std::unique_ptr<PictureSource> makeSource(const std::vector<uint8_t> &es)
        {
#if VCS_FFMPEG_REFERENCE
            if (!useNative())
                return std::make_unique<FfmpegSource>(es);
#endif
            return std::make_unique<NativeSource>(es);
        }

        // VCS_MOVIE_DUMP=<dir>: every 30th picture of the movie as <dir>/<MOVIE>_<n>.ppm (n in display order)
        void dump(const std::string &path, const std::vector<uint8_t> &es, const char *dir)
        {
            std::string name = path;
            const size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos)
                name = name.substr(slash + 1);
            name = name.substr(0, name.find('.'));
            std::unique_ptr<PictureSource> source = makeSource(es);
            video::Mpeg2Picture p;
            std::vector<uint8_t> rgba;
            uint64_t index = 0, written = 0;
            while (source->next(p))
            {
                if (index % 30u == 0u)
                {
                    rgba.resize(static_cast<size_t>(p.width) * p.height * 4u);
                    video::yuv420ToRgba(p.y, p.yStride, p.cb, p.cr, p.cStride, p.width, p.height, rgba.data());
                    char file[64];
                    std::snprintf(file, sizeof(file), "/%s_%04llu.ppm", name.c_str(), static_cast<unsigned long long>(index));
                    if (FILE *f = std::fopen((std::string(dir) + file).c_str(), "wb"))
                    {
                        std::fprintf(f, "P6\n%d %d\n255\n", p.width, p.height);
                        for (size_t i = 0; i < rgba.size(); i += 4)
                            std::fwrite(&rgba[i], 1, 3, f);
                        std::fclose(f);
                        ++written;
                    }
                }
                ++index;
            }
            std::fprintf(stderr, "[movie] %s: %llu pictures, %llu written to %s (%s decoder)\n", path.c_str(),
                         static_cast<unsigned long long>(index), static_cast<unsigned long long>(written), dir,
                         useNative() ? "native" : "FFmpeg");
        }

        // Plays one movie; false if it could not be read or decoded (then the game goes on as if it was skipped).
        bool play(const std::string &path, uint32_t skipAfterFrames, PS2Runtime *runtime)
        {
            IPS2DiscImage *disc = ps2_disc::image();
            uint32_t lsn = 0, size = 0;
            bool directory = false;
            if (!disc || !disc->findFile(path, lsn, size, directory) || directory || size == 0)
            {
                std::fprintf(stderr, "[movie] %s not found on the disc\n", path.c_str());
                return false;
            }
            std::vector<uint8_t> file(size);
            if (!disc->readBytes(static_cast<uint64_t>(lsn) * 2048u, size, file.data()))
                return false;
            Pss pss;
            if (!demux(file, pss))
                return false;
            std::vector<uint8_t>().swap(file);
            const std::vector<int16_t> sound = soundFrames(pss.sound);

            if (const char *dir = std::getenv("VCS_MOVIE_DUMP"); dir && *dir)
                dump(path, pss.video, dir);
#if VCS_FFMPEG_REFERENCE
            if (const char *v = std::getenv("VCS_MOVIE_VERIFY"); v && *v == '1')
                verify(path, pss.video);
#endif
            std::unique_ptr<PictureSource> source = makeSource(pss.video);
            video::Mpeg2Picture picture;
            std::vector<uint8_t> rgba;

            // the sound starts the clock (or the host clock without audio output)
            const bool audio = ps2AudioMovieActive() && !sound.empty();
            ps2AudioMovieReset();
            size_t soundPushed = 0;
            const size_t soundFramesTotal = sound.size() / 2u;
            auto pushSound = [&] {
                if (!audio)
                    return;
                while (soundPushed < soundFramesTotal && ps2AudioMovieQueued() < 9600u) // keep ~200 ms queued
                {
                    const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(2400u, soundFramesTotal - soundPushed));
                    const uint32_t taken = ps2AudioMoviePush(&sound[soundPushed * 2u], chunk);
                    soundPushed += taken;
                    if (taken < chunk)
                        break;
                }
            };
            pushSound();
            const double hostStart = nowSeconds();
            // (the sound may end before the last pictures: from there the host clock carries on where it stopped)
            double soundClock = pss.soundStart, soundClockAt = hostStart;
            auto clock = [&] {
                if (!audio)
                    return pss.videoStart + (nowSeconds() - hostStart);
                if (soundPushed < soundFramesTotal || ps2AudioMovieQueued() > 0u)
                {
                    soundClock = pss.soundStart + ps2AudioMoviePlayed() / 48000.0;
                    soundClockAt = nowSeconds();
                    return soundClock;
                }
                return soundClock + (nowSeconds() - soundClockAt);
            };

            double frameRate = 30000.0 / 1001.0; // the stream's own rate (STINGS 29.97, INTRO 30)
            uint64_t shown = 0, decoded = 0, dropped = 0;
            bool skipHeld = skipPressed(), skipped = false;
            const double started = nowSeconds();
            while (!runtime->isStopRequested() && source->next(picture))
            {
                if (picture.frameRate > 0.0)
                    frameRate = picture.frameRate;
                const double due = pss.videoStart + decoded / frameRate;
                ++decoded;
                // late by more than a frame: drop it (keeps the pictures with the sound)
                if (clock() > due + 1.0 / frameRate)
                {
                    ++dropped;
                    continue;
                }
                rgba.resize(static_cast<size_t>(picture.width) * picture.height * 4u);
                video::yuv420ToRgba(picture.y, picture.yStride, picture.cb, picture.cr, picture.cStride, picture.width,
                                    picture.height, rgba.data());
                // wait for the picture's moment, feeding the sound and watching for a skip
                while (clock() < due && !runtime->isStopRequested())
                {
                    pushSound();
                    const bool pressed = skipPressed();
                    const bool allowed = (nowSeconds() - started) * 60.0 >= skipAfterFrames; // the game's count, VBlanks
                    if (pressed && !skipHeld && allowed)
                    {
                        skipped = true;
                        break;
                    }
                    skipHeld = pressed;
                    static const bool log = std::getenv("VCS_MOVIE_LOG") != nullptr;
                    static double lastLog = 0.0;
                    if (log && nowSeconds() - lastLog > 1.0)
                    {
                        lastLog = nowSeconds();
                        std::fprintf(stderr, "[movie] %.2f s: pushed %zu, queued %u, played %llu, clock %.2f, due %.2f\n",
                                     lastLog - started, soundPushed, ps2AudioMovieQueued(),
                                     static_cast<unsigned long long>(ps2AudioMoviePlayed()), clock(), due);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (skipped)
                    break;
                // the PS2 showed the 512x448 picture across a 4:3 TV
                ps2ShowMovieFrame(rgba.data(), picture.width, picture.height, 4.0f / 3.0f);
                ++shown;
                pushSound();
            }
            // the sound's tail
            while (!skipped && audio && !runtime->isStopRequested() && (soundPushed < soundFramesTotal || ps2AudioMovieQueued()))
            {
                pushSound();
                if (skipPressed() && !skipHeld && (nowSeconds() - started) * 60.0 >= skipAfterFrames)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ps2AudioMovieReset();
            ps2EndMovieFrames();
            source->report(path);
            std::fprintf(stderr, "[movie] %s: %llu pictures shown, %llu dropped, %.1f s%s%s (%s decoder)\n", path.c_str(),
                         static_cast<unsigned long long>(shown), static_cast<unsigned long long>(dropped), nowSeconds() - started,
                         skipped ? ", skipped" : "", audio ? "" : ", no sound output (host clock)", useNative() ? "native" : "FFmpeg");
            return decoded > 0;
        }

        // replaces CMoviePlayer::Play(path, 0, skipAfterFrames) (0x40D8B8)
        PS2_GUEST_CC void moviePlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const std::string path(reinterpret_cast<const char *>(rdram + (GPR_U32(ctx, 4) & 0x01FFFFFFu)));
            const uint32_t skipAfter = GPR_U32(ctx, 6);
            if (g_skipIntro.load(std::memory_order_relaxed))
                std::fprintf(stderr, "[movie] %s skipped (Display > Intro movies)\n", path.c_str());
            else
                play(path, skipAfter, runtime);
            SET_GPR_U64(ctx, 2, 0);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void setSkipIntroMovies(bool skip) { g_skipIntro.store(skip, std::memory_order_relaxed); }

    void installMovieHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(kMoviePlay, &moviePlay);
    }
}
