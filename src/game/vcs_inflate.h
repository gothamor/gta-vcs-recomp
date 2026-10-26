// zlib 1.2.2's inflate_fast (FUN_00184428) natively (docs/game-internals.md 8, docs/PERFORMANCE.md): the busiest guest
// function while the game streams (sectors, models, textures are zlib streams). The same decoding on the same guest
// memory - z_stream / inflate_state as the EE build lays them out - so the output and the state are byte for byte the
// recompiled original's. Falls back to the original when a buffer is outside main RAM or the data is bad (the original
// then writes zlib's error message and mode).
//
// VCS_NATIVE_INFLATE=0: the original. VCS_NATIVE_INFLATE_VERIFY=1: the original runs; the native decode goes to a shadow
// copy of the output and is compared with what the original wrote at the next call. VCS_NATIVE_INFLATE_LOG=1: counts
// every 10 s.
#pragma once

class PS2Runtime;

namespace vcs
{
    void installInflate(PS2Runtime &runtime);
}
