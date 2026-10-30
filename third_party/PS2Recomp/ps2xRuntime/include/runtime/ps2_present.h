// [gta-recomp] Presenting game frames as the game finishes them, instead of at each emulated VBlank.
//
// With the hardware renderer a GS frame ends (and goes to the screen) at every emulated VBlank: at most 60 a second,
// whatever the game's own rate. A game hook that knows where a frame ends (VCS: its frame limiter) calls
// ps2PresentGameFrame there; with present-on-game-frames on, the VBlanks end a frame only while no game frame was
// presented for a while (loading screens, movies), so nothing freezes.
#pragma once

#include <cstdint>

// Game thread: end the GS frame now (the display registers as they are), as a VBlank would.
void ps2PresentGameFrame();

// Any thread: on = frames end at ps2PresentGameFrame (VBlanks only as the fallback); off = at every VBlank (default).
void ps2SetPresentOnGameFrames(bool on);

// [gta-recomp] Host-paced game frames (docs/PERFORMANCE.md 19): the main thread's frame start - its input just read -
// starts the game's next frame, so the game reads fresh input and its frame is ready for the next host frame. Main thread:
// ps2HostTick() right after reading the input. Game thread: ps2WaitHostTick() at the end of a game frame waits for a tick
// newer than the last one it took (at most one waits: a game slower than the host starts at once, never in a burst);
// false when host pacing is off (ps2SetHostPacedFrames; frame-rate independent timing at a cap of the host's rate).
// A tick finding frames still on their way (posted by the game, not yet taken: ps2NoteFramesTaken) beyond the one about
// to be shown is a backlog - a frame queued behind another, a host frame late for good (docs/PERFORMANCE.md 19). `lead`:
// called by the frame lead, before the host frame that is to show it (none may be on their way then). `paced`: the host
// frames sleep to a cap - a backlog's tick starts nothing, so the next one starts in time again; otherwise (the GPU or
// the CPU sets the pace) the frame waiting keeps the GPU fed, and the tick goes when it is taken.
void ps2HostTick(bool lead = false, bool paced = false);
void ps2NoteFramesTaken(uint32_t frames);
bool ps2WaitHostTick();
void ps2SetHostPacedFrames(bool on);

// Movie pictures (a game's movies played natively, e.g. VCS's STINGS / INTRO): shown in the window instead of the game
// picture, at display aspect `aspect`, until ps2EndMovieFrames. rgba: width x height, rows top-down, R G B A bytes.
// Any thread (copies the picture).
void ps2ShowMovieFrame(const uint8_t *rgba, int width, int height, float aspect);
void ps2EndMovieFrames();
