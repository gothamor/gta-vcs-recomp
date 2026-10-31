// Host front end around VCS's original menus: a startup menu after the intro movies and Options / Quit while paused.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace ui::frontend
{
    enum class StartupAction
    {
        NewGame,
        LoadGame,
        DeleteGame,
    };

    // Game thread: waits after INTRO.PSS while the host thread keeps drawing the startup menu.
    void waitAtStartup(PS2Runtime *runtime);
    bool selectStartup(StartupAction action); // control-channel checks; false when the startup menu is not open

    // Main thread, inside the runtime's Dear ImGui frame.
    void draw(PS2Runtime &runtime);
    bool blocksInput();

    // Game thread, at the normal frame boundary. Tracks the original pause menu and drives the verified route to its
    // Load / Delete screens after a startup choice.
    void onGameFrame(uint8_t *rdram);
}
