# Settings menu and widescreen - plan

Status: **v1 built** (2026-10-22): spike done, settings menu and Hor+ widescreen (4:3, 16:10,
16:9) in; see "What v1 built" below for what differs from the plan.

Two pieces, built in that order:
1. An in-game **settings menu** of our own (not the PS2 game's menus), opened with a key binding,
   built so that new settings are one registration each.
2. **Widescreen** support as the menu's first real setting.

## Findings this plan is based on

From the user's ELF (`build/elf/SLUS_215.90`; addresses are in the game's code, names are ours):

| What | Where | Notes |
|---|---|---|
| FOV global | `0x487484` (float) | written by `FUN_002653e0` (a `CDraw::SetFOV`), read by the camera code at `0x21cd98`, `0x21ce90`, `0x21ec4c`, `0x21f9ac` and others |
| Aspect-ratio global | `0x487488` (float) | written only by `FUN_002653f0` (a `CDraw::CalculateAspectRatio`); read by the camera (`0x21cdcc`, `0x21cf20`, `0x21ec60`) and 2D code (`0x39fd2c`: world-to-screen with 640/448 and 448.0) |
| Aspect choice | `FUN_002653f0` | widescreen pref on: 16/9 (5/3 when `[0x6f4d14]` is set); off: 4/3 (1.25 when set). `[0x6f4d14]` looks like the PAL/NTSC flag (non-square pixel variants) |
| Widescreen preference | `*(int *)(*(0x48F3D0) + 0x30)` | read through the getter `FUN_002af658` (`lw v0, 0x30(a0)`); the object at `[0x48F3D0]` is created by `FUN_00470fa8` if missing (a menu manager / prefs object - 533 loads of that pointer) |
| HUD awareness | 106 calls of `FUN_002af658` | the first ones (`0x1147e0`, `0x1149e8`, `0x11589c`) sit next to the HUD's own 4/3 constants (`0x1147f8`, `0x114a18`, `0x1158ac`): the HUD already adjusts for widescreen |
| Camera view window | `FUN_001ab9e0(camera, flag, tan(fov/2), aspect)` | called at `0x21cdd4` with the aspect global; at `0x21f9d0` with a fixed 16/9 (a second camera - to identify) |

So **VCS has its own widescreen mode** (as the PS2 release did, for 16:9 TVs): with the preference
set it renders a horizontally squeezed (anamorphic) 640-pixel picture with a wider view and a HUD
laid out for it, expecting the TV to stretch the picture to 16:9. The game's camera, culling and
2D projection all follow the aspect global. The plan uses that path instead of patching
projection matrices, and treats other aspect ratios as an extension of it.

Also found: **our presentation shows the 640x448 frame at 1.43:1** (square pixels). A PS2 frame is
meant to be shown at 4:3 (non-square pixels), so today's picture is ~7% too wide. The widescreen
work fixes that too ("Display aspect" below).

## Spike result (2026-10-22) - the plan's widescreen route changes

Forcing the game's preference (`VCS_WIDESCREEN=1`, src/game/vcs_display.cpp) and logging the
globals showed:
- The writes stick (preference 1; aspect 16/9 in the frontend, 5/3 in game - `[0x6f4d14]` is 1 in
  game and 0 in the frontend, so it is not a PAL flag but a frontend / in-game switch; in 4:3 the
  aspect is 4/3 / 1.25).
- The FOV global is the same in both modes (45 frontend, 120 flyover, 70 in game).
- `FUN_001ab9e0` = set the camera's view window: x = tan(FOV / 2), y = x / aspect, raster
  640x448, through `FUN_0018f2d8` (RwCameraSetViewWindow). So the game's widescreen is **Vert-**:
  same horizontal view, vertical view cut by 25%, for a TV that stretches the picture.
- The HUD does not change with the preference (the radar keeps its 4:3 shape); the preference's
  readers are 3D-projected sprites (e.g. `0x1147e0`: size without the 4/3 factor).
- The camera at `0x21f9c8` (same camera object `[0x51E490]`) gets a fixed 16/9: probably the
  letterboxed cutscene view.

New route for 16:9 (and other ratios): **Hor+ through the view-window setter**. Replace
`FUN_001ab9e0` with a wrapper that widens the horizontal extent by r = display aspect / (4:3) and
keeps the vertical one (tan * r, aspect * r); RenderWare derives the frustum (EE culling) and the
VU matrices from the view window, so 3D, culling and both renderers follow. The game's own
preference stays off. The renderer squeezes 2D draws (no depth test, not full width) by 1/r so the
HUD keeps its shape (in the middle 4:3 of the screen), and frames without 3D (loading screens, the
pause map) are shown at 4:3, pillarboxed.

## What v1 built (2026-10-22)

- `src/settings/settings.{h,cpp}`: the registry (Toggle / Choice / Integer / Key; Live / Restart),
  values as ints, vcs.ini read / write (unknown lines and comments kept, temp file + rename, only
  non-default values appended), environment overrides per setting (read at load, shown greyed out,
  never written back), an `alias` for older keys (`scale=` -> `display.scale`) and a `read` hook
  for settings a hotkey also changes (F3 overlay). No separate ini module: it is ~60 lines.
- `src/ui/settings_menu.{h,cpp}`: the menu, drawn in the runtime's debug-UI callback. Tabs per
  page (LB / RB or Q / E switch), a two-column table per page, the selected row's help line,
  "applies after a restart" under changed Restart settings. F2 (`controls.menu_key`) or pressing
  Back + Start together toggles it (no half-second hold: the press is enough, see below); Escape /
  B closes it (not while a combo list is open). Dear ImGui keyboard / gamepad nav is switched on
  only while the menu is open.
- Input: `ps2control::setGameInputBlocked` - `physicalInputOn()` is false while the menu is open
  and after it closes until every key / button is released (so the B or Escape that closed it
  never reaches the game).
- Settings (main.cpp `registerSettings`): Display > Aspect ratio (`display.aspect` = 43 / 1610 /
  169, env `VCS_ASPECT`), HUD (`display.hud`: keep 4:3 / stretch), Internal resolution
  (`display.scale`, restart, env `PS2X_HW_SCALE`); Graphics > Performance overlay; Controls >
  Settings menu key.
- Widescreen: the Hor+ route from the spike - `vcs_display.cpp` wraps `FUN_001ab9e0`, the presenter
  shows 3D frames at the display aspect (2D-only frames - loading screens, the pause menu - at
  4:3), the HW renderer squeezes the HUD by (4:3) / aspect. What tells them apart (found with frame
  dumps, `PS2X_HW_DUMP`):
  - RenderWare keeps ZTE on for 2D (ZTE=0 is prohibited on the GS) and uses ALWAYS or GEQUAL, so
    "no depth test" finds nothing. VCS draws its whole HUD, text, radar and menus at the **largest
    Z** (0xFFFFFF; 0.99999994 after scaling, not 1.0), while projected sprites (clouds, coronas,
    the mission arrow's glow) take the same RwIm2D path at their real depth. HUD = at the far
    plane, not full width (sky, horizon band, fades), not sampling a render target (the colour
    filter is drawn in strips from the frame itself).
  - The radar map and menu labels are clipped by the scissor: squeezed draws get a squeezed
    scissor.
  - 3D = a depth-tested draw (GEQUAL / GREATER) not at the far plane, or a native draw. The game
    draws 3D in every other VBlank (30 fps), so "3D" is judged over the last 32 frames: 4 or more
    3D -> wide, none -> 4:3, otherwise unchanged. The game thread reads the same flag to decide
    whether to squeeze (a frame or two late at a switch).
  - Checked with `tools/scenes.py capture` at 16:9 and 4:3: gameplay HUD in the middle 4:3 with
    a round, clipped radar; flyovers and the intro cutscene wide (the cutscene keeps its own
    letterbox bars inside the 16:9 picture); loading screen and pause map at 4:3. Not yet looked
    at: vehicles' HUD, the phone / mission text boxes, the frontend menus over a 3D background.
- Not done from the plan: 21:9 and "match window" (the same mechanism; streaming at the edges
  needs a look first), separate horizontal / vertical internal scale, window mode, pausing,
  control-channel `menu` / `set` commands (environment overrides cover scripted captures:
  `VCS_ASPECT=169 python tools/scenes.py capture ...`).
- Gamepad chord: opening on the Back + Start press means the game may see Start for one frame
  (and open its pause menu) before the block applies. A hold time would not avoid that either; a
  later fix is to delay Start to the game by a frame while Back is held.

## Part 1 - the settings menu

### What the player sees

- **F2** opens / closes the menu (default; rebindable in the menu, stored in vcs.ini). On a
  gamepad: hold **Back + Start** for half a second (a chord, so neither button's game meaning is
  lost). Escape / Circle (gamepad B) closes it.
- A centred window over the game: a list of pages on the left (Display, Graphics, Controls,
  Advanced), the selected page's settings on the right. Keyboard, mouse and gamepad navigation
  (Dear ImGui's keyboard / gamepad nav).
- Each setting shows its value, a one-line description of the selected item, and a marker when it
  needs a restart ("applies after restart"). "Reset page to defaults" at the bottom of each page.
- While the menu is open the game gets no physical input (it keeps running in v1; see "Pausing").
- Changes apply immediately where possible and are saved to vcs.ini right away.

### Architecture

```
 src/settings/   settings.h/.cpp   registry: typed settings, values, defaults, change callbacks
                 ini.h/.cpp        vcs.ini read / write (keeps unknown keys, comments and order)
 src/ui/         settings_menu.h/.cpp   the menu: input routing, layout generated from the registry
 src/game/       vcs_settings.cpp  VCS-specific settings (widescreen, ...) and what they do
 main.cpp        registers everything once at startup, hands the runtime a UI draw callback
```

- **Registry, not hand-written screens.** A setting is declared once with its metadata; the menu
  draws whatever is registered, grouped by page and section. Adding a setting = one
  `settings::add({...})` plus its apply callback; no menu code changes.

  ```cpp
  namespace settings
  {
      enum class Type { Toggle, Choice, Integer, Number, Key, Action };
      enum class Apply { Live, Restart };
      struct Choice { const char *label; int value; };
      struct Spec
      {
          const char *key;         // "display.aspect" - also the vcs.ini key
          const char *page;        // "Display"
          const char *section;     // optional group heading inside the page
          const char *label;       // "Aspect ratio"
          const char *help;        // one line, shown for the selected item
          Type type;
          int defaultValue = 0;    // Toggle / Choice / Integer / Key
          float defaultNumber = 0; // Number
          int min = 0, max = 0, step = 1;  // Integer (Number: as floats)
          std::vector<Choice> choices;     // Choice
          Apply apply = Apply::Live;
          std::function<void()> onChange;  // called after a change (Live) or ignored (Restart)
          std::function<bool()> visible;   // optional: hide when not applicable
      };
      void add(Spec spec);
      int value(const char *key);                       // and number(), for Number
      void set(const char *key, int value);             // validates, saves, notifies
      void load(const std::filesystem::path &ini);      // before anything reads a value
      void subscribe(const char *key, std::function<void()> fn);
  }
  ```
- **One source of truth for values**: vcs.ini (already holds `iso=` and `scale=`). Keys use
  `section.name`. Environment variables keep winning for testing (`PS2X_HW_SCALE` over
  `display.scale`), shown in the menu as "set by environment" and greyed out.
- **Writing vcs.ini**: rewrite the file keeping lines we do not own, update / append ours, write to
  a temporary file and rename (no half-written ini after a crash).
- **Rendering**: Dear ImGui is already up (platform layer context, RHI renderer, drawn over the
  game every frame - the F3 overlay uses it). The menu draws in the runtime's existing UI hook:
  `PS2Runtime::setDebugUiCallbacks` (draw callback per host frame; our `main.cpp` does not use it
  yet). A small style pass (larger font, game-like colours) - Dear ImGui loads TTF fonts if we
  want one later.
- **Input routing**: `ps2control::physicalInputOn()` is a fixed value today; it becomes "not hidden
  and not injected-only **and no menu open**" (an atomic flag the menu sets), so keyboard, mouse
  and gamepad stop reaching the PS2 pad while the menu is open. The menu itself reads input through
  Dear ImGui (SDL3 backend already feeds it) - gamepad nav needs `ImGuiConfigFlags_NavEnableGamepad`.
- **Key binding**: `controls.menu_key` (a `Key` type setting: the menu's key-capture widget waits
  for the next key). Debug keys (F3, F4, F12) can move into the registry the same way later.
- **Pausing**: v1 leaves the game running with input blocked (simple, no risk). v2: pause the EE
  thread at its next VBlank wait (the scheduler already sleeps there) and keep the IOP / audio
  ticking so the sound driver doesn't starve - a separate, testable step.
- **Live vs restart**: settings that change GPU resources the renderer allocates once (internal
  resolution today) start as `Apply::Restart`; making them live is per-setting work later.
- **Testing**: `tools/scenes.py` gets `--set key=value` (written to a temporary vcs.ini / passed as
  environment) so captures can compare settings; the control channel gets `menu open|close` and
  `set key value` commands for scripted checks of the menu itself.

### First settings (v1)

| Page | Setting | Values | Apply |
|---|---|---|---|
| Display | Aspect ratio | 4:3 (original), 16:9, 16:10, 21:9, Match window | live |
| Display | HUD | Game layout / Keep 4:3 in the middle (for non-16:9 ratios) | live |
| Display | Internal resolution | 1x-8x | restart |
| Display | Window | Windowed / Borderless fullscreen | live |
| Graphics | Native renderer | On / Off (`PS2X_HLE`) | restart |
| Graphics | Performance overlay | On / Off (F3) | live |
| Controls | Menu key | key capture | live |

v1 ships the framework with Display > Aspect ratio (4:3 and 16:9), Internal resolution and the
overlay toggle; the rest are registrations once their behaviour exists.

## Part 2 - widescreen

### Modes

- **4:3 (original)**: today's picture, but presented at the correct 4:3 display aspect.
- **16:9**: the game's own widescreen mode (preference on), presented stretched to 16:9.
- **16:10, 21:9, Match window**: the game's widescreen mode with the aspect global overridden to
  the target ratio; presented at that ratio.

### How each part works

1. **Game side - the preference.** Write `*(int *)(*(0x48F3D0) + 0x30)` (1 / 0) when the setting
   changes and whenever the object is (re)created or the game loads its settings from the memory
   card (it would otherwise restore the saved value). Simplest robust form: a per-frame check in
   `vcs_overrides.cpp` (the frame-kick hook already exists) that sets the field if it differs.
   Calling `FUN_002653f0` afterwards recomputes the aspect global at once. Not hooking the getter
   `FUN_002af658`: it is a plain field accessor, so other objects may share it.
   - To check: whether the game's own Display menu (the pause menu has a "display" tab) exposes
     the option. If it does, our setting mirrors it: read the field back and show the game's value
     when the player changed it there.
2. **Game side - other ratios.** Replace `FUN_002653f0` (runtime `replaceFunction`) with a C++
   version: the game's choice for 4:3 / 16:9 (keeping the PAL variants), the setting's ratio
   otherwise. The camera view window, the EE's frustum planes and the VU clip / screen matrices are
   all built from the aspect global, so 3D, culling and both renderers (emulated and native) follow
   without further changes.
3. **Presentation.** The presenter (`render/presenter.cpp`) letterboxes the picture by its pixel
   size today. It gets a display aspect instead: 4:3, or the widescreen ratio, and scales the
   640x448 frame to it (pillar- / letterboxing the rest of the window).
4. **Resolution where it's needed.** In widescreen the game still draws 640 pixels across a wider
   view, so horizontal detail drops (by 25% at 16:9, 43% at 21:9). Give the HW renderer separate
   horizontal and vertical scales (`PS2X_HW_SCALE` -> scale x / y): render targets, scissor, the
   render-target texel scale in the GS shader and the native draws' targets all take a 2D scale.
   With "Internal resolution 3x" at 16:9 the targets become 2560x1344 instead of 1920x1344 and the
   presented pixels are square again.
5. **HUD and 2D.** At 16:9 the game's own HUD layout applies (106 places check the preference). At
   ratios beyond 16:9 the HUD would stretch; "HUD: keep 4:3 in the middle" squeezes the 2D draws in
   the renderer (draws with no depth test in screen space: sprites, text, the radar) by
   gameAspect / displayAspect around the centre. A later step can anchor HUD elements to the screen
   edges by finding the HUD's x-coordinate code around `0x1147e0`.
6. **Full-screen 2D** (loading screens, the legal screen, fades, cutscene letterbox bars): check
   each in widescreen. Loading screens are 640x448 images that would stretch; if the game does not
   handle them, the renderer keeps such full-screen texture blits at 4:3 (pillarboxed).

### Things to verify first (spike, before building the menu)

- Force the preference on (debug env var) and capture the reference scenes: is the picture
  anamorphic with a wider view, does the HUD move, does anything pop in at the edges, how do the
  loading screens, pause menu, radar and cutscenes look.
- Find what the second camera with a fixed 16/9 (`0x21f9b0`) is (radar? cutscene?), and whether
  `[0x6f4d14]` is the PAL flag.
- Check that the guard-band / clip constants (VU data 65, matrices 24-31) change with the aspect
  (they should, being built from the camera).

### Risks / open questions

- The game might also widen the field of view in widescreen differently from a pure aspect change
  (Hor+ vs Vert-); the spike shows which. A separate "Field of view" setting (scaling the FOV
  global through a `FUN_002653e0` override) can be added later.
- Cutscenes may use their own cameras / FOV and the fixed-16:9 camera.
- Ratios beyond 16:9: culling and LOD distances follow the camera, but sector streaming is driven
  by the hex cell around the camera (docs/game-internals.md "Level data"), so very wide views may reach
  unloaded cells at the edges - check at 21:9.
- Memory card: the game saves its preferences; with our setting authoritative the saved value is
  overwritten each session - fine, but the save then stores our value (harmless).

## Milestones

| # | What | Size |
|---|---|---|
| 1 | Spike: force the widescreen preference, capture, answer the questions above | S |
| 2 | Settings core (registry, vcs.ini read / write), menu shell (F2 / Back+Start, pages, input blocking, ImGui nav), control-channel and scenes.py hooks | M |
| 3 | Display aspect in the presenter (fixes today's 1.43:1), Aspect ratio 4:3 / 16:9 through the game's preference | S-M |
| 4 | Separate horizontal / vertical internal scale | M |
| 5 | Other ratios (16:10, 21:9, match window) via the `FUN_002653f0` replacement; HUD "keep 4:3" squeeze; full-screen 2D kept at 4:3 | M-L |
| 6 | More settings as registrations: window mode, native renderer, menu key, overlay | S each |

## Known issues noted for later (from play-testing, 2026-10-22)

Tracked in PROGRESS.md: objects / the world turning black for a few frames, and Vic falling
through the world in some places (collision).
