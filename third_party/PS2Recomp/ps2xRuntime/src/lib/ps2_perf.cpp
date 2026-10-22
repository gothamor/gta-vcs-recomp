// [gta-recomp] Performance instrumentation: see runtime/ps2_perf.h.
#include "runtime/ps2_perf.h"
#include "runtime/ps2_audio_out.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace ps2perf
{
    namespace
    {
        constexpr size_t kPhases = static_cast<size_t>(Phase::Count);
        constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
        constexpr size_t kHistory = 600;     // samples kept for the overlay
        constexpr size_t kGraphSamples = 240; // samples shown in the graph
        constexpr const char *kPhaseNames[kPhases] = {"game", "render", "iop", "wait"};
        constexpr const char *kCounterNames[kCounters] = {"draws", "verts", "tex_uploads", "dma_tags", "vu1_programs"};

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        struct Sample
        {
            double t = 0.0;          // seconds since the first sample
            float frameMs = 0.0f;    // wall time of this game frame
            float phaseMs[kPhases]{};
            uint32_t vblanks = 0;
            uint32_t counters[kCounters]{};
            float hostFps = 0.0f, hostMs = 0.0f, glMs = 0.0f; // host frames presented during this game frame
            float audioMs = 0.0f;
            uint32_t underruns = 0;
        };

        std::atomic<uint64_t> g_phaseNs[kPhases];
        std::atomic<uint64_t> g_counters[kCounters];
        std::atomic<uint64_t> g_hostFrames{0}, g_hostNs{0}, g_glNs{0};
        thread_local PhaseScope *t_current = nullptr;

        // EE thread
        uint64_t g_firstMarkNs = 0, g_lastMarkNs = 0, g_lastGameMarkNs = 0;
        uint32_t g_vblanks = 0;
        uint64_t g_lastUnderruns = 0;

        // shared (g_mutex)
        std::mutex g_mutex;
        std::vector<Sample> g_ring(kHistory);
        size_t g_head = 0, g_size = 0;
        FILE *g_csv = nullptr;
        std::string g_csvName;
        bool g_logFromEnv = false;
        double g_lastSummaryT = 0.0;

        // main thread
        bool g_overlay = false;
        uint64_t g_lastHostNs = 0;

        void openCsv(const std::string &name)
        {
            g_csv = std::fopen(name.c_str(), "w");
            if (!g_csv)
            {
                std::fprintf(stderr, "[perf] cannot write %s\n", name.c_str());
                return;
            }
            g_csvName = name;
            std::fprintf(g_csv, "t_s,frame_ms,vblanks");
            for (const char *p : kPhaseNames)
                std::fprintf(g_csv, ",%s_ms", p);
            for (const char *c : kCounterNames)
                std::fprintf(g_csv, ",%s", c);
            std::fprintf(g_csv, ",host_fps,host_ms,gl_ms,audio_ms,underruns\n");
            std::fprintf(stderr, "[perf] recording to %s\n", name.c_str());
        }

        void closeCsv()
        {
            if (!g_csv)
                return;
            std::fclose(g_csv);
            g_csv = nullptr;
            std::fprintf(stderr, "[perf] recording saved: %s (summarise with: python tools/perf_report.py %s)\n", g_csvName.c_str(),
                         g_csvName.c_str());
        }

        std::string timestampedName()
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), "perf_%Y%m%d_%H%M%S.csv", &tm);
            return buf;
        }

        // Last `n` samples, oldest first. Caller holds g_mutex.
        std::vector<Sample> recent(size_t n)
        {
            n = std::min(n, g_size);
            std::vector<Sample> out;
            out.reserve(n);
            for (size_t i = 0; i < n; ++i)
                out.push_back(g_ring[(g_head + kHistory - n + i) % kHistory]);
            return out;
        }

        struct Summary
        {
            size_t frames = 0;
            double seconds = 0.0, fps = 0.0, avgMs = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, maxMs = 0.0;
            double phaseMs[kPhases]{};
            double counters[kCounters]{};
            double vblanksPerS = 0.0, hostFps = 0.0, hostMs = 0.0, glMs = 0.0, audioMs = 0.0;
            uint64_t underruns = 0;
        };

        Summary summarize(const std::vector<Sample> &s)
        {
            Summary r;
            r.frames = s.size();
            if (s.empty())
                return r;
            std::vector<float> ms;
            ms.reserve(s.size());
            double vblanks = 0.0, hostW = 0.0;
            for (const Sample &x : s)
            {
                ms.push_back(x.frameMs);
                r.seconds += x.frameMs / 1000.0;
                vblanks += x.vblanks;
                for (size_t p = 0; p < kPhases; ++p)
                    r.phaseMs[p] += x.phaseMs[p];
                for (size_t c = 0; c < kCounters; ++c)
                    r.counters[c] += x.counters[c];
                if (x.hostFps > 0.0f)
                {
                    r.hostFps += x.hostFps;
                    r.hostMs += x.hostMs;
                    r.glMs += x.glMs;
                    hostW += 1.0;
                }
                r.underruns += x.underruns;
            }
            const double n = static_cast<double>(s.size());
            for (double &p : r.phaseMs)
                p /= n;
            for (double &c : r.counters)
                c /= n;
            if (hostW > 0.0)
            {
                r.hostFps /= hostW;
                r.hostMs /= hostW;
                r.glMs /= hostW;
            }
            r.audioMs = s.back().audioMs;
            r.avgMs = r.seconds * 1000.0 / n;
            r.fps = r.seconds > 0.0 ? n / r.seconds : 0.0;
            r.vblanksPerS = r.seconds > 0.0 ? vblanks / r.seconds : 0.0;
            std::sort(ms.begin(), ms.end());
            auto pct = [&](double q) { return static_cast<double>(ms[std::min(ms.size() - 1, static_cast<size_t>(q * (ms.size() - 1) + 0.5))]); };
            r.p50 = pct(0.50);
            r.p95 = pct(0.95);
            r.p99 = pct(0.99);
            r.maxMs = ms.back();
            return r;
        }

        void closeFrame(uint64_t now)
        {
            if (!g_firstMarkNs)
            {
                g_firstMarkNs = g_lastMarkNs = now;
                for (auto &p : g_phaseNs)
                    p.store(0, std::memory_order_relaxed);
                for (auto &c : g_counters)
                    c.store(0, std::memory_order_relaxed);
                g_vblanks = 0;
                return;
            }
            Sample s;
            const uint64_t frameNs = now - g_lastMarkNs;
            g_lastMarkNs = now;
            s.t = (now - g_firstMarkNs) / 1e9;
            s.frameMs = static_cast<float>(frameNs / 1e6);
            uint64_t claimed = 0;
            for (size_t p = 1; p < kPhases; ++p)
            {
                const uint64_t ns = g_phaseNs[p].exchange(0, std::memory_order_relaxed);
                claimed += ns;
                s.phaseMs[p] = static_cast<float>(ns / 1e6);
            }
            g_phaseNs[0].store(0, std::memory_order_relaxed);
            s.phaseMs[0] = static_cast<float>((frameNs > claimed ? frameNs - claimed : 0) / 1e6);
            for (size_t c = 0; c < kCounters; ++c)
                s.counters[c] = static_cast<uint32_t>(g_counters[c].exchange(0, std::memory_order_relaxed));
            s.vblanks = g_vblanks;
            g_vblanks = 0;
            const uint64_t hostFrames = g_hostFrames.exchange(0, std::memory_order_relaxed);
            const uint64_t hostNs = g_hostNs.exchange(0, std::memory_order_relaxed);
            const uint64_t glNs = g_glNs.exchange(0, std::memory_order_relaxed);
            if (hostFrames)
            {
                s.hostMs = static_cast<float>(hostNs / 1e6 / hostFrames);
                s.hostFps = s.hostMs > 0.0f ? 1000.0f / s.hostMs : 0.0f;
                s.glMs = static_cast<float>(glNs / 1e6 / hostFrames);
            }
            s.audioMs = ps2AudioQueuedMs();
            const uint64_t underruns = ps2AudioUnderrunCount();
            s.underruns = static_cast<uint32_t>(underruns - g_lastUnderruns);
            g_lastUnderruns = underruns;

            std::lock_guard<std::mutex> lock(g_mutex);
            g_ring[g_head] = s;
            g_head = (g_head + 1) % kHistory;
            g_size = std::min(g_size + 1, kHistory);
            if (!g_csv && !g_logFromEnv)
            {
                g_logFromEnv = true;
                if (const char *path = std::getenv("PS2X_PERF_LOG"))
                    openCsv(path);
            }
            if (!g_csv)
                return;
            std::fprintf(g_csv, "%.4f,%.3f,%u", s.t, s.frameMs, s.vblanks);
            for (float p : s.phaseMs)
                std::fprintf(g_csv, ",%.3f", p);
            for (uint32_t c : s.counters)
                std::fprintf(g_csv, ",%u", c);
            std::fprintf(g_csv, ",%.1f,%.3f,%.3f,%.1f,%u\n", s.hostFps, s.hostMs, s.glMs, s.audioMs, s.underruns);
            if (s.t - g_lastSummaryT >= 5.0)
            {
                // one line per 5 s on stderr, for logs of scripted runs
                std::vector<Sample> window;
                for (size_t i = 0; i < g_size; ++i)
                {
                    const Sample &x = g_ring[(g_head + kHistory - 1 - i) % kHistory];
                    if (x.t <= g_lastSummaryT)
                        break;
                    window.push_back(x);
                }
                g_lastSummaryT = s.t;
                const Summary r = summarize(window);
                std::fprintf(stderr,
                             "[perf] t=%.0fs game %.1f fps (avg %.1f p95 %.1f max %.1f ms) vbl %.1f/s | ee game %.1f render %.1f iop %.1f wait %.1f ms"
                             " | %.0f draws %.0fk verts %.0f tex %.0f tags %.0f vu1 | host %.0f fps gl %.1f ms | audio %.0f ms %llu underruns\n",
                             s.t, r.fps, r.avgMs, r.p95, r.maxMs, r.vblanksPerS, r.phaseMs[0], r.phaseMs[1], r.phaseMs[2], r.phaseMs[3],
                             r.counters[0], r.counters[1] / 1000.0, r.counters[2], r.counters[3], r.counters[4], r.hostFps, r.glMs,
                             r.audioMs, static_cast<unsigned long long>(r.underruns));
                std::fflush(g_csv);
            }
        }

        // ---- overlay (main thread) ------------------------------------------------------------------------------
        constexpr Color kPhaseColors[kPhases] = {
            {235, 150, 50, 235},  // game
            {80, 160, 245, 235},  // render
            {110, 210, 110, 235}, // iop
            {120, 120, 130, 150}, // wait
        };

        std::string fmt(const char *f, ...)
        {
            char buf[256];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        void drawOverlayPanel(const std::vector<Sample> &samples, bool recording, const std::string &csvName)
        {
            const int screenH = GetScreenHeight();
            const int fs = std::max(10, screenH / 56);   // font size
            const int lh = fs + fs / 4;                  // line height
            const int pad = fs / 2;
            const int w = std::min(GetScreenWidth() - 16, fs * 36);
            const int graphH = fs * 7;
            const int lines = 7;
            const int h = pad * 3 + lines * lh + graphH + lh;
            const int x = 8, y = 8;
            DrawRectangle(x, y, w, h, Color{0, 0, 0, 175});

            // numbers: the last second, and the graph window for percentiles
            std::vector<Sample> lastSecond;
            for (auto it = samples.rbegin(); it != samples.rend(); ++it)
            {
                if (!lastSecond.empty() && lastSecond.front().t - it->t > 1.0)
                    break;
                lastSecond.insert(lastSecond.begin(), *it);
            }
            const Summary now = summarize(lastSecond);
            const Summary win = summarize(samples);
            const Color white{235, 235, 235, 255}, dim{170, 170, 175, 255};
            int ty = y + pad;
            auto line = [&](const std::string &text, Color c) {
                DrawText(text.c_str(), x + pad, ty, fs, c);
                ty += lh;
            };
            const Color fpsColor = now.fps >= 55.0 ? Color{120, 230, 120, 255} : now.fps >= 28.0 ? Color{240, 210, 90, 255} : Color{245, 100, 90, 255};
            line(fmt("GAME  %5.1f fps  %5.1f ms   p95 %.1f  p99 %.1f  max %.1f", now.fps, now.avgMs, win.p95, win.p99, win.maxMs), fpsColor);
            line(fmt("VBLANK %4.1f/s   HOST %4.0f fps %4.1f ms   GL %4.1f ms", now.vblanksPerS, now.hostFps, now.hostMs, now.glMs), white);
            line(fmt("EE ms   game %4.1f  render %4.1f  iop %4.1f  wait %4.1f", now.phaseMs[0], now.phaseMs[1], now.phaseMs[2],
                     now.phaseMs[3]),
                 white);
            line(fmt("GS  %5.0f draws  %5.1fk verts  %3.0f tex uploads", now.counters[0], now.counters[1] / 1000.0, now.counters[2]), white);
            line(fmt("DMA %5.0f tags   VU1 %5.0f programs", now.counters[3], now.counters[4]), white);
            line(fmt("AUDIO %3.0f ms queued   %llu underruns (last %zu frames)", now.audioMs, static_cast<unsigned long long>(win.underruns),
                     samples.size()),
                 now.audioMs < 20.0 ? Color{245, 100, 90, 255} : white);
            line(recording ? fmt("REC %s   [F4] stop   [F3] hide", csvName.c_str()) : std::string("[F4] record CSV   [F3] hide"),
                 recording ? Color{245, 90, 90, 255} : dim);

            // graph: one stacked bar per game frame, EE thread phases; frame time beyond the phases shows as the outline
            const int gx = x + pad, gy = ty + pad / 2, gw = w - pad * 2;
            // scale: 0-67 ms, or 133 / 267 ms when the frames do not fit
            const float maxMs = win.p95 <= 60.0 ? 66.7f : win.p95 <= 125.0 ? 133.3f : 266.7f;
            DrawRectangle(gx, gy, gw, graphH, Color{20, 20, 25, 200});
            const float barW = static_cast<float>(gw) / kGraphSamples;
            const size_t first = samples.size() > kGraphSamples ? samples.size() - kGraphSamples : 0;
            for (size_t i = first; i < samples.size(); ++i)
            {
                const Sample &s = samples[i];
                const float bx = gx + (i - first) * barW;
                float base = static_cast<float>(gy + graphH);
                for (size_t p = 0; p < kPhases; ++p)
                {
                    const float ph = std::min(s.phaseMs[p], maxMs) / maxMs * graphH;
                    const float top = std::max(base - ph, static_cast<float>(gy));
                    DrawRectangleRec(Rectangle{bx, top, std::max(barW - 0.5f, 1.0f), base - top}, kPhaseColors[p]);
                    base = top;
                }
                if (s.frameMs > maxMs)
                    DrawRectangleRec(Rectangle{bx, static_cast<float>(gy), std::max(barW - 0.5f, 1.0f), 3.0f}, Color{255, 60, 60, 255});
            }
            const struct { float ms; const char *label; Color c; } marks[] = {
                {1000.0f / 60.0f, "60", Color{120, 230, 120, 200}},
                {1000.0f / 30.0f, "30", Color{240, 210, 90, 200}},
                {50.0f, "20", Color{245, 100, 90, 200}},
                {100.0f, "10", Color{245, 100, 90, 200}},
                {200.0f, "5", Color{245, 100, 90, 200}},
            };
            for (const auto &m : marks)
            {
                if (m.ms >= maxMs)
                    continue;
                const int my = gy + graphH - static_cast<int>(m.ms / maxMs * graphH);
                DrawLine(gx, my, gx + gw, my, m.c);
                DrawText(m.label, gx + gw - MeasureText(m.label, fs * 3 / 4) - 2, my - fs * 3 / 4, fs * 3 / 4, m.c);
            }
            // legend
            int lx = gx;
            const int ly = gy + graphH + pad / 2;
            for (size_t p = 0; p < kPhases; ++p)
            {
                DrawRectangle(lx, ly + fs / 4, fs / 2, fs / 2, kPhaseColors[p]);
                DrawText(kPhaseNames[p], lx + fs * 3 / 4, ly, fs * 3 / 4 + 1, dim);
                lx += MeasureText(kPhaseNames[p], fs * 3 / 4 + 1) + fs * 3 / 2;
            }
            DrawText(fmt("last %zu frames, ms per frame (0-%.0f), lines = fps", std::min(samples.size(), kGraphSamples), maxMs).c_str(), lx, ly, fs * 3 / 4 + 1, dim);
        }
    }

    void add(Counter counter, uint64_t amount)
    {
        g_counters[static_cast<size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
    }

    PhaseScope::PhaseScope(Phase phase) noexcept : m_parent(t_current), m_start(nowNs()), m_childNs(0), m_phase(phase)
    {
        t_current = this;
    }

    PhaseScope::~PhaseScope()
    {
        const uint64_t elapsed = nowNs() - m_start;
        t_current = m_parent;
        g_phaseNs[static_cast<size_t>(m_phase)].fetch_add(elapsed > m_childNs ? elapsed - m_childNs : 0, std::memory_order_relaxed);
        if (m_parent)
            m_parent->m_childNs += elapsed;
    }

    void markGameFrame()
    {
        const uint64_t now = nowNs();
        g_lastGameMarkNs = now;
        closeFrame(now);
    }

    void markVBlank()
    {
        ++g_vblanks;
        const uint64_t now = nowNs();
        if (!g_lastGameMarkNs || now - g_lastGameMarkNs > 1000000000ull) // the game marks no frames: VBlank = frame
            closeFrame(now);
    }

    void hostFrame(double glMs)
    {
        const uint64_t now = nowNs();
        if (g_lastHostNs)
        {
            g_hostNs.fetch_add(now - g_lastHostNs, std::memory_order_relaxed);
            g_glNs.fetch_add(static_cast<uint64_t>(glMs * 1e6), std::memory_order_relaxed);
            g_hostFrames.fetch_add(1, std::memory_order_relaxed);
        }
        g_lastHostNs = now;
    }

    void drawOverlay()
    {
        if (IsKeyPressed(KEY_F3))
            g_overlay = !g_overlay;
        bool recording;
        std::string csvName;
        std::vector<Sample> samples;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (IsKeyPressed(KEY_F4))
            {
                g_logFromEnv = true; // a manual toggle wins over PS2X_PERF_LOG
                if (g_csv)
                    closeCsv();
                else
                    openCsv(timestampedName());
            }
            recording = g_csv != nullptr;
            csvName = g_csvName;
            if (g_overlay)
                samples = recent(kGraphSamples);
        }
        if (g_overlay)
            drawOverlayPanel(samples, recording, csvName);
        else if (recording)
            DrawText("REC", 10, 10, std::max(10, GetScreenHeight() / 46), Color{245, 90, 90, 255});
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        closeCsv();
    }
}
