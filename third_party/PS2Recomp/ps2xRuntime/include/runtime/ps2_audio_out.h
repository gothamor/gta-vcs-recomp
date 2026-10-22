// [gta-recomp] Host audio output for the emulated SPU2 (48 kHz stereo). PS2X_AUDIO=0 disables it.
#pragma once

#include <cstdint>

void ps2AudioInit();     // after InitAudioDevice (main thread)
void ps2AudioPush(const int16_t *stereo, uint32_t frames); // any thread
void ps2AudioShutdown(); // before CloseAudioDevice
