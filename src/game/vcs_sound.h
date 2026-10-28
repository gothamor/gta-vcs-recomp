// VCS sound without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 2; docs/game-internals.md 9).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // Serves CDSTREAM.IRX's two sound RPC servers on the host - 0x65686579 (sound effects: banks, the per-frame voice
    // block) and 0x65686578 (streamed music / radio / cutscene audio) - through a native libsd and its own SPU2 core,
    // and makes that SPU2 the audio output. False with VCS_NATIVE_SOUND=0, which keeps the emulated IOP's (the
    // reference).
    bool installSoundHooks(PS2Runtime &runtime);

    // Audio > Voices: scales the effect voices that play speech and mission dialogue (special sounds, banks 2 and
    // 0x43), 0..1 (1: the original, bit-exact). Native driver only.
    void setSpeechVolume(float gain);
    uint32_t speechKeyOns(); // speech sounds keyed on since start (checks)
    uint32_t speechLevel();  // the last speech voice's volume: the game's << 16 | after the gain (checks)
}
