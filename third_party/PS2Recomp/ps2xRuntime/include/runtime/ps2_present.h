// [gta-recomp] Presenting game frames as the game finishes them, instead of at each emulated VBlank.
//
// With the hardware renderer a GS frame ends (and goes to the screen) at every emulated VBlank: at most 60 a second,
// whatever the game's own rate. A game hook that knows where a frame ends (VCS: its frame limiter) calls
// ps2PresentGameFrame there; with present-on-game-frames on, the VBlanks end a frame only while no game frame was
// presented for a while (loading screens, movies), so nothing freezes.
#pragma once

// Game thread: end the GS frame now (the display registers as they are), as a VBlank would.
void ps2PresentGameFrame();

// Any thread: on = frames end at ps2PresentGameFrame (VBlanks only as the fallback); off = at every VBlank (default).
void ps2SetPresentOnGameFrames(bool on);
