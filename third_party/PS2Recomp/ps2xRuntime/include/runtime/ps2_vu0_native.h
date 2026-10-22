// [gta-recomp] Native replacements for hot VU0 microprograms.
// A game registers a C++ function for a VU0 program by its start address and a hash of its code bytes (no game
// code is stored). PS2Runtime::executeVU0Microprogram runs the native function instead of the interpreter while VU0
// code memory still holds exactly those bytes. PS2X_VU0_NATIVE=0 turns it off; PS2X_VU0_VERIFY=1 runs both and
// reports differences in VF/VI/ACC registers and VU0 data memory.
#pragma once

#include <cstdint>

struct R5900Context;

// Runs the program on the EE-side VU0 state (ctx->vu0_vf, ctx->vi, ctx->vu0_acc) and VU0 data memory.
using Ps2Vu0NativeFn = void (*)(R5900Context *ctx, uint8_t *vu0Data, uint32_t vu0DataSize);

// codeBytes: length of the program from startPc (byte address); codeHash: ps2Vu0CodeHash of those bytes.
void ps2RegisterVu0Native(uint32_t startPc, uint32_t codeBytes, uint64_t codeHash, Ps2Vu0NativeFn fn, const char *name);

// FNV-1a 64 of a code range (for registering and checking).
uint64_t ps2Vu0CodeHash(const uint8_t *code, uint32_t bytes);
