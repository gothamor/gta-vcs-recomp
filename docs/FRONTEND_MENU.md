# Native front end and pause actions

The port supplies the title menu that the PS2 game does not have. It appears in the game window after the Rockstar
logos, intro movie and boot loading screens (or after their skipped calls when **Display > Skip intro movies** is
enabled), immediately before the opening sequence begins.

## Startup menu

- **Start Game > New Game** continues through the game's normal new-game path.
- **Start Game > Load Game** continues booting, waits until the original front end is available, then opens its
  **Game > Load Game** screen. Slot selection, confirmation and loading remain entirely the game's code.
- **Start Game > Delete Game** does the same for the original **Game > Delete Game** screen.
- **Options** opens integrated pages over the native palm background, with game-font headings, category buttons and
  setting labels. The controls reuse F2's registry, binding checks, confirmations, help, reset and restart markers.
  Back returns from a settings page to the category list, then to the startup or pause menu.
- **F2** continues to open the original standalone settings window. It temporarily covers the native menu; closing it
  restores the same native Options page. Both views edit the same settings and values.
- **Quit Game** asks for confirmation and requests a clean runtime shutdown.

The return from `CMoviePlayer::Play` for `INTRO.PSS` arms the gate. The frame hook then observes the following boot
`LoadingScreen` sequence and opens the menu on its first normal frame. It blocks the game thread there while the host
window and Dear ImGui continue to run, so the last splash is presented before the menu and no guest scheduler state is
altered. Merely hooking `LoadingScreen` would be wrong because that function is also used by saves and world transitions.

## Pause menu

When the game's own user-pause flag is set, a small **Port Menu** panel appears over the original pause screen with
**Options**, **Quit Game**, and **Back to Pause Menu**. Options opens the integrated settings pages. Back dismisses the panel for that
pause and restores input to all original VCS tabs; the panel returns the next time the player pauses. Code pauses and
loading waits do not open it.

The presentation is intentionally isolated in `src/ui/frontend_menu.cpp`. When the configured PCSX2 pack contains
`db7fe6220bfa99e-400b40212c17643-00006213.png`, the menu uses its original purple palm artwork and black footer. When
it also contains `3ece7b57667e958b-f663bcea3bca5023-00006213.png`, labels are drawn directly from the game's proportional
font atlas, including its original outlines, rather than approximated with an OS font. Missing artwork or font files
fall back independently to the dark-purple background or Dear ImGui font. Both `Game` and `Options` use the original
script atlas (`87ad416320b92240-59ab262d214c676c-00006214.png`), at a larger size and on a shared baseline, with separate
advance widths for the slanted strokes. The footer uses the Pricedown atlas (`64e22c1534c81341-d49e156c51372d1c-00006214.png`)
and the pack's Triangle, Cross and directional-arrow icons. Menu entries have scale-aware row heights and native settings
have extra vertical cell padding. Loaded font and button masks expand PS2 alpha (0x80 opaque) to PNG/UI alpha
(0xff opaque), including the main menu face. Hovered/focused actions and native setting rows use muted yellow
(190,181,108), while titles remain pink and idle entries cyan. The initial page omits **Back**, because it has no parent; it
appears in the Start Game submenu and pause actions. **Display > HUD** also governs this host menu: Keep 4:3 uses a
centred 4:3 canvas, while Screen edges and Stretch use the full display. Styling remains separate from boot/save logic.

## Checks

Hidden runs can select the startup path through `frontend new|load|delete` on the control channel. Save navigation
uses the same original-menu route checked by `tools/saves.py`; injected controller reports stop after reaching the
Load/Delete screen, where the player takes over.
`frontend options [page]` opens the native category list or a registry page for isolated captures; `menu <page>` and F2
still open the standalone view. A custom ImGui bitmap-font loader samples the game atlas for native widgets, dropdowns,
numeric values, help and buttons as well as headings. Lowercase input uses the actual lowercase glyphs; digits and
punctuation have their own atlas mappings. The standalone F2 font is unchanged. Native Back lives in the footer
(clickable, or Escape/controller Triangle/B); no extra Back button occupies the category list or setting pages.
