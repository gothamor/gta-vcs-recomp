// The in-game settings menu (docs/SETTINGS_MENU_WIDESCREEN.md): Dear ImGui pages generated from the settings
// registry (settings/settings.h). Touchpad + Options on the DualSense opens and closes it; Circle closes it. While it
// is open the game gets no input.
#pragma once

#include <string>

namespace ui
{
    // Opens the menu on a page (by its name), or closes it: the control channel's "menu" command, for screenshots of
    // the menu in test runs (where it otherwise never opens).
    void openSettingsMenu(const std::string &page);
    void closeSettingsMenu();
    void scrollSettingsMenu(float fraction); // the page's rows scrolled to 0 (top) .. 1 (bottom)

    // Main thread, once per host frame inside the Dear ImGui frame (the runtime's debug-UI draw callback).
    void drawSettingsMenu();
    bool settingsMenuOpen();
    bool settingsMenuBlocksInput(); // open, or its buttons still held after closing (main.cpp blocks the game's input)
}
