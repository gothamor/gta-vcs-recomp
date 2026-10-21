// Game-specific runtime bindings for SLUS_215.90, applied when the ELF loads.
//
// Use this (not game/vcs/overrides.toml) for quick experiments: changing a stub
// in the recompiler config rewrites the generated headers and rebuilds every
// unity batch (~9 min), while this file only needs a relink.
// Once a binding is settled it can move to overrides.toml.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <iostream>

namespace
{
    struct Binding
    {
        uint32_t address;
        const char *handler; // any runtime syscall/stub name, or ret0/ret1/reta0
        const char *why;
    };

    // Add entries here while bringing the game up.
    constexpr Binding kBindings[] = {
        {0, nullptr, nullptr},
    };

    void applyVcs(PS2Runtime &runtime)
    {
        for (const Binding &b : kBindings)
        {
            if (!b.handler)
                continue;
            if (!ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
                std::cerr << "[vcs] failed to bind 0x" << std::hex << b.address << std::dec << " -> " << b.handler << std::endl;
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS-21590)", "SLUS_215.90", 0x001DD6A0u, 0x079B8BE6u, &applyVcs)
