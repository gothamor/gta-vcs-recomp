# Native front end and pause actions

The port supplies the title menu that the PS2 game does not have. It appears in the game window after the Rockstar
logos, intro movie and boot loading screens (or after their skipped calls when **Display > Skip intro movies** is
enabled), immediately before the opening sequence begins.

## Startup menu

- **Start Game > New Game** continues through the game's normal new-game path.
- **Start Game > Load Game** continues booting, waits until the original front end is available, then opens its
  **Game > Load Game** screen. Slot selection, confirmation and loading remain entirely the game's code.
- **Start Game > Delete Game** does the same for the original **Game > Delete Game** screen.
- **Options** opens the same registry-generated pages as F2. Closing Options returns to the startup menu.
- **Quit Game** asks for confirmation and requests a clean runtime shutdown.

The return from `CMoviePlayer::Play` for `INTRO.PSS` arms the gate. The frame hook then observes the following boot
`LoadingScreen` sequence and opens the menu on its first normal frame. It blocks the game thread there while the host
window and Dear ImGui continue to run, so the last splash is presented before the menu and no guest scheduler state is
altered. Merely hooking `LoadingScreen` would be wrong because that function is also used by saves and world transitions.

## Pause menu

When the game's own user-pause flag is set, a small **Port Menu** panel appears over the original pause screen with
**Options**, **Quit Game**, and **Back to Pause Menu**. Options reuses the F2 pages. Back dismisses the panel for that
pause and restores input to all original VCS tabs; the panel returns the next time the player pauses. Code pauses and
loading waits do not open it.

The presentation is intentionally isolated in `src/ui/frontend_menu.cpp`. When the configured PCSX2 pack contains
`db7fe6220bfa99e-400b40212c17643-00006213.png`, the menu uses its original purple palm artwork and black footer. When
it also contains `3ece7b57667e958b-f663bcea3bca5023-00006213.png`, labels are drawn directly from the game's proportional
font atlas, including its original outlines, rather than approximated with an OS font. Missing artwork or font files
fall back independently to the dark-purple background or Dear ImGui font. Cyan/pink selection and the bottom control
legend follow the original pause-menu layout. Styling can be replaced without changing the boot/save logic.

## Checks

Hidden runs can select the startup path through `frontend new|load|delete` on the control channel. Save navigation
uses the same original-menu route checked by `tools/saves.py`; injected controller reports stop after reaching the
Load/Delete screen, where the player takes over.
