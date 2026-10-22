// The in-game settings menu (docs/SETTINGS_MENU_WIDESCREEN.md): Dear ImGui pages generated from the settings
// registry (settings/settings.h). The menu key (controls.menu_key, F2) or holding Back + Start on a gamepad opens and
// closes it; Escape / B closes it. While it is open the game gets no keyboard, mouse or gamepad input.
#pragma once

namespace ui
{
    // Main thread, once per host frame inside the Dear ImGui frame (the runtime's debug-UI draw callback).
    void drawSettingsMenu();
    bool settingsMenuOpen();
}
