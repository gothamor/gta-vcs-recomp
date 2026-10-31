# Native front end and pause actions

The port supplies the title menu that the PS2 game does not have. It appears in the game window after the Rockstar
logos and intro movie (or immediately after their skipped calls when **Display > Skip intro movies** is enabled), before
the game's loading and opening sequence continues.

## Startup menu

- **Start Game > New Game** continues through the game's normal new-game path.
- **Start Game > Load Game** continues booting, waits until the original front end is available, then opens its
  **Game > Load Game** screen. Slot selection, confirmation and loading remain entirely the game's code.
- **Start Game > Delete Game** does the same for the original **Game > Delete Game** screen.
- **Options** opens the same registry-generated pages as F2. Closing Options returns to the startup menu.
- **Quit Game** asks for confirmation and requests a clean runtime shutdown.

The gate lives at the return from `CMoviePlayer::Play` for `INTRO.PSS`, not in `LoadingScreen`: the loading-screen
function is shared by boot, save loading and world transitions. The movie function already waits on the game thread
while the host window and Dear ImGui continue to run, so no guest scheduler state is altered.

## Pause menu

When the game's own user-pause flag is set, a small **Port Menu** panel appears over the original pause screen with
**Options**, **Quit Game**, and **Back to Pause Menu**. Options reuses the F2 pages. Back dismisses the panel for that
pause and restores input to all original VCS tabs; the panel returns the next time the player pauses. Code pauses and
loading waits do not open it.

The first visual theme is intentionally isolated in `src/ui/frontend_menu.cpp`: it uses the existing ImGui font with
VCS-inspired dark teal and pink colours, so layout and styling can be replaced without changing the boot/save logic.

## Checks

Hidden runs can select the startup path through `frontend new|load|delete` on the control channel. Save navigation
uses the same original-menu route checked by `tools/saves.py`; injected controller reports stop after reaching the
Load/Delete screen, where the player takes over.
