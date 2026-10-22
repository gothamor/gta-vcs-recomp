// [gta-recomp] Host audio output for the SPU2 mix: a ring buffer filled in IOP time (PS2IopHostAdapter::audioSamples)
// and drained by a raylib audio stream callback at 48 kHz stereo.
// The emulator produces audio in bursts and not exactly in real time, so the callback resamples with a playback rate
// steered by the buffer fill (target ~80 ms, rate 0.92..1.08, linear interpolation): no hard gaps when the buffer runs
// a little low, no dropped chunks when it runs a little high. After a real underrun playback waits for ~40 ms of
// audio before resuming (one short gap instead of a crackle). PS2X_AUDIO_STATS=1 prints underruns / fill every 5 s.
#include "runtime/ps2_audio_out.h"

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kRate = 48000u;
    constexpr size_t kCapacityFrames = kRate;             // 1 s ring
    constexpr double kTargetFrames = kRate * 0.08;        // aim for ~80 ms queued
    constexpr size_t kResumeFrames = kRate * 4 / 100;     // re-prime after an underrun (40 ms)
    constexpr size_t kMaxQueuedFrames = kRate * 30 / 100; // hard latency cap (300 ms)

    std::mutex g_mutex;
    std::vector<int16_t> g_ring(kCapacityFrames * 2u);
    size_t g_read = 0, g_write = 0, g_queued = 0;
    double g_frac = 0.0; // fractional position between g_read and g_read + 1
    double g_rate = 1.0; // smoothed playback rate
    bool g_primed = false;
    int16_t g_last[2] = {0, 0};
    AudioStream g_stream{};
    bool g_started = false;
    FILE *g_dump = nullptr; // PS2X_AUDIO_DUMP=<file.wav>: everything the SPU2 produced
    uint32_t g_dumpFrames = 0;
    uint64_t g_underrunFrames = 0, g_underruns = 0, g_droppedFrames = 0, g_playedFrames = 0, g_pushedFrames = 0;

    void writeWavHeader(FILE *f, uint32_t frames)
    {
        const uint32_t dataBytes = frames * 4u;
        const uint32_t riffSize = 36u + dataBytes;
        const uint16_t fmt[] = {1, 2};
        const uint32_t rate = kRate, byteRate = kRate * 4u;
        const uint16_t align = 4, bits = 16;
        const uint32_t fmtSize = 16;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        std::fwrite(&riffSize, 4, 1, f);
        std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmtSize, 4, 1, f);
        std::fwrite(fmt, 2, 2, f);
        std::fwrite(&rate, 4, 1, f);
        std::fwrite(&byteRate, 4, 1, f);
        std::fwrite(&align, 2, 1, f);
        std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f);
        std::fwrite(&dataBytes, 4, 1, f);
        std::fseek(f, 0, SEEK_END);
    }

    int16_t at(size_t frame, int channel) { return g_ring[(frame % kCapacityFrames) * 2u + channel]; }

    void callback(void *buffer, unsigned int frames)
    {
        int16_t *out = static_cast<int16_t *>(buffer);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_primed && g_queued >= kResumeFrames)
            g_primed = true;
        // steer the rate toward the target fill, smoothed so the pitch never jumps
        const double wanted = std::clamp(1.0 + (static_cast<double>(g_queued) - kTargetFrames) / (kTargetFrames * 5.0), 0.92, 1.08);
        g_rate += (wanted - g_rate) * 0.1;
        for (unsigned int i = 0; i < frames; ++i)
        {
            if (!g_primed || g_queued < 2u)
            {
                if (g_primed)
                {
                    g_primed = false;
                    ++g_underruns;
                }
                // hold the last sample and fade it out instead of clicking to zero
                for (int c = 0; c < 2; ++c)
                {
                    g_last[c] = static_cast<int16_t>(g_last[c] * 31 / 32);
                    out[i * 2u + c] = g_last[c];
                }
                ++g_underrunFrames;
                continue;
            }
            for (int c = 0; c < 2; ++c)
            {
                const double a = at(g_read, c), b = at(g_read + 1u, c);
                g_last[c] = static_cast<int16_t>(a + (b - a) * g_frac);
                out[i * 2u + c] = g_last[c];
            }
            g_frac += g_rate;
            while (g_frac >= 1.0 && g_queued >= 2u)
            {
                g_frac -= 1.0;
                g_read = (g_read + 1u) % kCapacityFrames;
                --g_queued;
            }
            ++g_playedFrames;
        }
    }

    void reportStats()
    {
        static const bool stats = std::getenv("PS2X_AUDIO_STATS") != nullptr;
        static auto last = std::chrono::steady_clock::now();
        if (!stats)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5))
            return;
        const double secs = std::chrono::duration<double>(now - last).count();
        last = now;
        std::fprintf(stderr, "[audio] produced %.0f%% of real time; played %llu, underruns %llu (%llu frames), dropped %llu; queued %.0f ms, rate %.3f\n",
                     g_pushedFrames * 100.0 / (secs * kRate), static_cast<unsigned long long>(g_playedFrames),
                     static_cast<unsigned long long>(g_underruns), static_cast<unsigned long long>(g_underrunFrames),
                     static_cast<unsigned long long>(g_droppedFrames), g_queued * 1000.0 / kRate, g_rate);
        g_playedFrames = g_underruns = g_underrunFrames = g_droppedFrames = g_pushedFrames = 0;
    }
}

void ps2AudioInit()
{
    const char *off = std::getenv("PS2X_AUDIO");
    if (g_started || (off && off[0] == '0') || !IsAudioDeviceReady())
        return;
    SetAudioStreamBufferSizeDefault(1024);
    g_stream = LoadAudioStream(kRate, 16, 2);
    SetAudioStreamCallback(g_stream, &callback);
    PlayAudioStream(g_stream);
    g_started = true;
}

void ps2AudioPush(const int16_t *stereo, uint32_t frames)
{
    static const char *dumpPath = std::getenv("PS2X_AUDIO_DUMP");
    if (dumpPath && !g_dump && (g_dump = std::fopen(dumpPath, "wb")) != nullptr)
        writeWavHeader(g_dump, 0);
    if (g_dump)
    {
        std::fwrite(stereo, 4, frames, g_dump);
        g_dumpFrames += frames;
        if ((g_dumpFrames % (kRate * 2u)) < frames) // keep the header valid every ~2 s
            writeWavHeader(g_dump, g_dumpFrames);
        std::fflush(g_dump);
    }
    if (!g_started)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pushedFrames += frames;
    for (uint32_t i = 0; i < frames; ++i)
    {
        g_ring[g_write * 2u] = stereo[i * 2u];
        g_ring[g_write * 2u + 1u] = stereo[i * 2u + 1u];
        g_write = (g_write + 1u) % kCapacityFrames;
        if (g_queued < kCapacityFrames)
            ++g_queued;
        else
            g_read = (g_read + 1u) % kCapacityFrames;
    }
    while (g_queued > kMaxQueuedFrames)
    {
        g_read = (g_read + 1u) % kCapacityFrames;
        --g_queued;
        ++g_droppedFrames;
    }
    reportStats();
}

void ps2AudioShutdown()
{
    if (!g_started)
        return;
    StopAudioStream(g_stream);
    UnloadAudioStream(g_stream);
    g_started = false;
}
