// [gta-recomp] Performance instrumentation.
//
// Every game frame (or VBlank, if the game never marks frames) becomes one sample: wall time, the EE thread's time
// split into phases (game code / render submit / IOP / waiting), render counters, host-side frame and GL times and
// the audio buffer state. In game:
//   F3  toggle the overlay (numbers + frame-time graph)
//   F4  start / stop recording samples to perf_<date>_<time>.csv in the working directory
// PS2X_PERF_LOG=<file.csv> records from boot (plus a "[perf]" summary line on stderr every 5 s) - for scripted runs;
// tools/perf_report.py summarises a recording.
#pragma once

#include <cstdint>
#include <string>

namespace ps2perf
{
    // Where the EE (game) thread spends its time. Game = whatever no scope claims.
    enum class Phase : uint8_t
    {
        Game,
        Render, // DMA chains, VIF, VU1 microprograms, GS command recording
        Iop,    // IOP modules + SPU2 mixing
        Wait,   // pacing to the VBlank clock, all threads blocked
        Count
    };

    enum class Counter : uint8_t
    {
        Draws,       // GL draw batches recorded by the HW renderer
        Vertices,    // vertices recorded by the HW renderer
        TexUploads,  // textures decoded and uploaded to the GPU
        DmaTags,     // VIF1 DMA chain tags walked
        Vu1Programs, // VU1 microprogram runs
        Count
    };

    void add(Counter counter, uint64_t amount = 1u);

    // Time spent until the scope ends goes to `phase` (nested scopes are subtracted from their parent).
    // EE thread only.
    class PhaseScope
    {
    public:
        explicit PhaseScope(Phase phase) noexcept;
        ~PhaseScope();
        PhaseScope(const PhaseScope &) = delete;
        PhaseScope &operator=(const PhaseScope &) = delete;

    private:
        PhaseScope *m_parent;
        uint64_t m_start;
        uint64_t m_childNs;
        Phase m_phase;
    };

    // EE thread: a game frame ended (game-specific hook). Without these calls each VBlank is a frame.
    void markGameFrame();
    // EE thread: a VBlank started.
    void markVBlank();

    // Main thread, once per host frame: how long the GPU renderer's command execution took.
    void hostFrame(double glMs);
    // Main thread, between BeginDrawing and EndDrawing: handles F3/F4 and draws the overlay when on.
    void drawOverlay(bool handleKeys = true);
    // [gta-recomp] Main thread: the F3 overlay's state (settings menu).
    void setOverlay(bool on);
    bool overlayOn();
    // The overlay's numbers for the last second as one line (control channel "perf").
    std::string summaryLine();
    // Closes a running recording (end of run).
    void shutdown();
}
