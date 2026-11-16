// Enhancements > Saving: "Quick save" - a key (Keyboard > Menus, F12) opens the game's own Save menu anywhere outside a
// mission; the save holds where the player stood (and his vehicle) and the clock does not move on. Loaded - from the
// Load Game menu or at boot - the game comes back there (docs/mods/QUICKSAVE.md).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::quicksave
{
    // Enhancements > Saving: "Quick save" - Off (default) / On (VCS_QUICKSAVE=0/1); Keyboard > Menus: "Quick save" (F12).
    void registerSettings();
    // The hooks (ACTIVATE_SAVE_MENU in a vehicle, the save's 6-hour clock step) and the `quicksave` command.
    // VCS_QUICKSAVE_HOOKS=0: none.
    void install(PS2Runtime &runtime);
    // Host frame (the UI hook): the quick save key; `menus` = a menu of ours has the input (the key does nothing).
    void onHostFrame(bool menus);
    // Game thread, once per game frame (the frame kick): starts a quick save, restores one after a load.
    void onGameFrame(uint8_t *rdram);
}
