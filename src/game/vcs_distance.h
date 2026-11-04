// Draw distances as settings (FORK.md T2, docs/game-internals.md 9h).
#pragma once

class PS2Runtime;

namespace vcs::distance
{
    // Graphics > Draw distance: the objects' distance, percent of the game's (100 = the original).
    void registerSettings();
    // Wraps CCamera::Process (after free aim's wrapper: it chains whatever is there), the `distancestate` command.
    void install(PS2Runtime &runtime);
}
