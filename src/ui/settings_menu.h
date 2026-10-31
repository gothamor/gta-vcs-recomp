// The in-game settings menu (docs/SETTINGS_MENU_WIDESCREEN.md): Dear ImGui pages generated from the settings
// registry (settings/settings.h). The menu key (controls.menu_key, F2) or holding Back + Start on a gamepad opens and
// closes it; Escape / B closes it. While it is open the game gets no keyboard, mouse or gamepad input.
#pragma once

#include <string>
#include <functional>

namespace ui
{
    // Opens the menu on a page (by its name), or closes it: the control channel's "menu" command, for screenshots of
    // the menu in hidden test runs (where it otherwise never opens).
    void openSettingsMenu(const std::string &page);
    void closeSettingsMenu();
    void scrollSettingsMenu(float fraction); // the page's rows scrolled to 0 (top) .. 1 (bottom)

    // Main thread, once per host frame inside the Dear ImGui frame (the runtime's debug-UI draw callback).
    void drawSettingsMenu();
    bool settingsMenuOpen();
    bool settingsMenuBlocksInput(); // open, or its keys still held after closing (main.cpp blocks the game's input)
    // Shared setting controls hosted by the game's styled Options page. Returns true for Back, except while
    // cancelling a binding or closing a popup. The label callback draws game-font labels at the current cursor.
    bool drawEmbeddedSettings(const std::string &page, const std::function<void(const std::string &)> &label);
    void resetEmbeddedSettings();
}
