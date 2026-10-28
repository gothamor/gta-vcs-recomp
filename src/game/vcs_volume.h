// The game's own Music / SFX volume (its pause menu's Audio page) as settings, and a Voices level for speech
// (docs/game-internals.md 9c).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::volume
{
    // Audio > Music volume, Sound effects volume, Voices volume (registered between the master and movie volumes).
    void registerSettings();
    // Hooks the menu's two volume setters, so a save load or a new game applies the player's levels instead of the
    // save's or the defaults; the `soundstate` command.
    void install(PS2Runtime &runtime);
    // At the frame kick (game thread): writes a changed setting into the game, notices the pause menu's changes.
    void onGameFrame(uint8_t *rdram);
    // Host frame: the pause menu's changes into the settings (and vcs.ini).
    void onHostFrame();
}
