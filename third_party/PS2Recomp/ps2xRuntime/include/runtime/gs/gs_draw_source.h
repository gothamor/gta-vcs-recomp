// [gta-recomp] Where the GIF packet the GS is processing came from, for the hardware renderer's draw-source
// counters (perf line gs_* fields, PS2X_HW_DRAW_SOURCES): the GIF path (gsSetCurrentGifPath, gs_frontend.h) and, for
// PATH1, whether a VU1 program sent it or a native reproduction of one (src/game/vcs_hle_world.cpp), with the
// program's start address. Thread-local, like the current GIF path. Kept out of gs_frontend.h: the generated code
// includes that one.
#pragma once

#include <cstdint>

// The VU1 program (start PC, VU code address in bytes / 8) whose XGKICKs follow on this thread; native = a C++
// reproduction of it sends them.
void gsSetPath1Producer(uint32_t vu1StartPc, bool native);

// path (1..3) | native << 2 | program start PC << 16 (path 1 only)
uint32_t gsCurrentDrawSource();

// set on draws recorded from a whole packet without the GS front end (GS::submitPackedKick)
constexpr uint32_t kGsDrawSourcePacked = 8u;

inline uint32_t gsDrawSourcePath(uint32_t source) { return source & 3u; }
inline bool gsDrawSourcePacked(uint32_t source) { return (source & kGsDrawSourcePacked) != 0u; }
inline bool gsDrawSourceNative(uint32_t source) { return (source & 4u) != 0u; }
inline uint32_t gsDrawSourceProgram(uint32_t source) { return source >> 16; }
