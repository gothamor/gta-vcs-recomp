#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // [gta-recomp] The game's own input mapping (src/game/vcs_input.cpp: the player's bindings) fills the report
    // instead of the built-in one below when set: data[2..3] buttons (active low), data[4..7] rx, ry, lx, ly. Game
    // thread; false falls back to the built-in mapping.
    using Reader = bool (*)(uint8_t *data);
    static void setReader(Reader reader);

    // [gta-recomp] The game's motor levels from scePadSetActDirect (act 0: small motor on / off, act 1: large motor
    // 0..255, the DualShock 2's usual alignment). Game thread.
    using ActuatorSink = void (*)(uint8_t small, uint8_t large);
    static void setActuatorSink(ActuatorSink sink);
    static void actuators(uint8_t small, uint8_t large);
};

#endif
