# Settings menu and widescreen - plan

Status: **v1 built** (2026-10-22): spike done, settings menu and Hor+ widescreen (4:3, 16:10,
16:9) in; see "What v1 built" below for what differs from the plan.

Two pieces, built in that order:
1. An in-game **settings menu** of our own (not the PS2 game's menus), opened with a pad chord (touchpad + Options),
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
  for settings something else also changes (the performance overlay). No separate ini module: it is ~60 lines.
  `settings::state` / `setState` keep text values in vcs.ini without a menu entry.
- `src/ui/settings_menu.{h,cpp}`: the menu, drawn in the runtime's debug-UI callback. Tabs per
  page (L1 / R1 switch), a two-column table per page, the selected row's help line,
  "applies after a restart" under changed Restart settings. Touchpad + Options toggles it (no hold:
  the press is enough); Circle closes it (not while a combo list is open). Dear ImGui's gamepad navigation is switched
  on only while the menu is open.
- Input: `ps2control::setGameInputBlocked` - `physicalInputOn()` is false while the menu is open
  and after it closes until every button is released (so the Circle that closed it never reaches the game). The
  chord's own buttons are kept from the game too (docs/CONTROLS.md, "Menus").
- Settings (main.cpp `registerSettings`): Display > Aspect ratio (`display.aspect` = 43 / 1610 /
  169, env `VCS_ASPECT`), HUD (`display.hud`, env `VCS_HUD`: 0 keep 4:3 / 2 screen edges / 1 stretch), Internal resolution
  (`display.scale`: 1-8 or 0 = Match window, the native frame; restart, env `PS2X_HW_SCALE`); Graphics > Performance
  overlay.
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
  - Checked with reference captures at 16:9 and 4:3: gameplay HUD in the middle 4:3 with
    a round, clipped radar; flyovers and the intro cutscene wide (the cutscene keeps its own
    letterbox bars inside the 16:9 picture); loading screen and pause map at 4:3.
- **HUD: Screen edges** (milestone 3 step 2, 2026-10-23): each HUD element keeps its 4:3 shape but is squeezed towards
  the screen edge it sits near (radar left; clock, weapon, money, zone name right; help text left; centred text in the
  middle) instead of all towards the middle. The game names its own HUD primitives (`src/game/vcs_hud.cpp`, found by
  their vertex positions; game-internals.md 6f), so the "far plane = HUD" heuristic only covers 2D outside CHud::Draw's
  frame. `gsHwSetHudClassifier` in the renderer; the pivot is x 0 / 640 / 320 of the 640-wide frame.
- **Entity culling** (2026-10-27): the claim "RenderWare derives the frustum from the view window, so culling
  follows" held for the world sectors only. Buildings, peds and cars go through `CCamera::IsSphereVisible`, whose
  side planes the game builds from the FOV global (4:3); they vanished in the outer eighths of a 16:9 picture.
  `vcs_display.cpp` now widens those planes to the Hor+ edge too (game-internals.md 5; `VCS_WIDE_CULL=0` = before).
- **Ultrawide** (2026-10-27, asked for by the user): Aspect ratio 21:9 (`219`, exactly 21 / 9), 32:9 (`329`) and
  Match window (`0`: the shape of the picture's output, clamped to 4:3 .. 32:9 - on the PS5 the TV's 16:9). Nothing
  else had to change: the Hor+ widening, the entity culling, the presenter's letterbox, the HUD squeeze / anchoring and
  the post effects' buffer widths all take the ratio as a number. Limits: narrower than 4:3 is shown letterboxed
  (Hor+ only widens; a portrait picture would need Vert+), wider than 32:9 is not checked. At 32:9 the horizontal view
  is 124 degrees (tan 35 x 2.67), so near objects at the sides stretch - rectilinear projection, as in any game.
  Checked at 21:9 (2520x1080 frame) and 32:9 (3840x1080), Internal resolution Match window, HUD Screen edges: the
  reference scenes (flyover, cutscene with its bars inside the picture, gameplay HUD at the edges, loading screen and
  pause map at 4:3 pillarboxed), two AI drives through the city (buildings, traffic and peds out to the edges, no
  unstreamed cells - the streaming follows the camera's cell, not the view), entity planes at 32:9 edge 1.867 = view
  window. Cost: the game thread about the same as at 16:9; the render worker roughly doubles with the pixels and the
  extra draws.
- **Integer resolutions in widescreen** (2026-10-27, the user's request): 1x-8x render the picture's targets as many
  times wider as the view (640 x scale x r by 448 x scale: 1x at 16:9 853x448, 3x at 32:9 5120x1344, at most 8192
  wide) through the native frame's per-target scales; before, 640 x scale was stretched across (2.67 times softer
  across than down at 32:9). 4:3 unchanged. `PS2X_HW_WIDE_SCALE=0` = before. Cost follows the pixels: a 32:9 3x is a
  5120-wide picture; 1x / 2x stay cheap, Match window renders at the output's own size.
- **Radar streak at 32:9** (2026-10-27, reported by the user): a thin column of radar map right of the circle. The HUD
  layer squeezed the HUD before snapping it to PS2 pixels, and at a squeeze of 0.375 a PS2 pixel is 6 screen pixels:
  the map's scissor and its corner mask rounded to different ones. Now snapped at the 4:3 positions, then squeezed
  with the scissor exactly (NATIVE_RENDERER.md "HUD layer"). Checked on the reference scene that showed it.
- **4:3 frames inside the game's frame** (2026-10-27, the user's decision): frames without 3D (the pause menu, loading
  screens) stay 4:3 but are placed inside the rectangle the game's picture takes - the output letterboxed at the
  chosen ratio - instead of the whole output (`present::fitRect`; the output, screenshots and the HUD layer's output
  size use it). A 32:9 game on a 16:9 TV shows its pause menu in the 32:9 strip between the bars, not at the
  output's full height. Intro movies keep the whole output. At Match window or 4:3 nothing changes (the frame is the
  output).
- **The pause menu switches at once** (same day): before, a frame was shown 4:3 only after 32 frames without 3D, so for
  half a second the menu's full-width backdrop was stretched across the wide picture, then the picture snapped to 4:3
  and for a frame or a few the menu was drawn 25% narrower (62.5% at 32:9) and popped back (the user's "resizes and
  scales up"). Why: each draw read the 3D flag as the main thread updated it, while the presenter used the newest
  value - frames recorded for a wide picture were shown at 4:3. Now (gs_hw.cpp): a frame keeps the flag it first read
  (`frameWide`), the presenter shows each frame with its own (`showing3d` = the shown frame's), and a frame with the
  game's menu primitives and no 3D (the classifier ors `GSHwAnchorMenu` into the anchors of FUN_003b53f8's
  primitives) empties the 3D history: 4:3 from the first menu frame. `PS2X_HW_MENU_SWITCH=0` = the 32-frame wait as
  before. Checked frame by frame (screenshots ~50 ms apart) entering and leaving the pause menu at 32:9 on a 16:9
  screen, 16:9 and 4:3, and the reference scenes at 32:9 on a 16:9 screen (loading screen and pause map in the strip).
- **No shifted frame at the switch** (same day, reported by the user: "for a frame the menu and the game view shift").
  Every presented frame captured (`burst`, below) showed, leaving the menu, about four host frames of the game squashed
  into the 4:3 box, then one wide frame with the HUD unsqueezed. Two causes:
  - **VCS shows the previous frame**: its finished picture (FBP 140) is copied into the displayed buffer (FBP 0) at the
    start of the next frame, so at a switch the picture on screen is the other shape's. The shape now belongs to the
    picture: each frame buffer keeps the shape of the frame that last drew into it (HUD-layer draws included - the pause
    menu draws nothing else), a plain copy carries it along like the HUD layer, and the output uses the displayed
    buffer's (`RhiExecutor::shapeOf`).
  - **The shape came from a history the frames had not caught up with**: decided when each frame is executed from all
    of it (`decideShape`): a menu frame without 3D is 4:3 at once, and the first 3D frame after it wide at once (before,
    4 frames with 3D were needed). The HUD layer's squeeze is recorded in any widescreen frame and applied when the
    frame is executed and its shape known; only draws squeezed into the buffer itself (unclassified 2D at the far plane)
    still go by the recording-time guess.
  Checked with `burst` (every presented frame, 50 entering and 50 leaving) at 32:9 three times, 21:9 and 16:9 on a 16:9
  screen: every frame's shape matches its picture (the build before: 3 of 100 did not) and the HUD is the same in the
  first gameplay frame as later; the reference scenes at 32:9 unchanged. Regression check: `tools/regress.py` step
  "menushape" (fails with `PS2X_HW_MENU_SWITCH=0`: 29 frames of the menu stretched, 3 of the game squashed).

## Menu v2 (2026-10-24): stable layout, pages and sections

Reported by the user: the menu switched between two layouts every other frame, and a toggle did nothing. Causes and
fixes (`src/ui/settings_menu.cpp`):
- **Layout feedback loop.** The menu was `AlwaysAutoResize` and centred every frame; the help line below the table
  showed the selected row's help (one to four wrapped lines) or nothing, so the menu grew and shrank with the
  selection - two layouts alternating. The "applies after a restart" line under a changed Restart row moved the rows
  below it too.
- Now: the menu's size comes from the viewport only (0.92 x width up to 720 px, 0.88 x height up to 540 px, both times
  the menu scale, which follows the screen height); the rows scroll in a child of the remaining height; the help area
  is as tall as the longest help text of any setting at the current width, so nothing changes size with the selection
  or the page. The restart note is a `*` after the label and one footer line that always keeps its space.
- **Help is sticky**: it shows the row last navigated to, highlighted.
- **Locked settings** (set by their environment variable for this run) show the variable beside the greyed label
  (`Skip intro movies (VCS_SKIP_MOVIES)`) and say how to unlock them in the help.
- The performance overlay draws on Dear ImGui's background draw list (under the menu; it covered the first tabs).

Pages and sections: `settings::Spec::section`; pages in order of first registration, sections likewise, rows in
registration order; a page whose rows are all hidden is not shown.

## Player settings (2026-10-28): display, sound, gamepad, presets

Asked for by the user ("substantially flexible settings that players can change"). Pages: Display, Graphics, Audio,
Game, Gamepad, Advanced.
- **Display**: *Presets* first - Authentic (PS2) (the Display / Graphics defaults, but original timing at 30 fps: the
  default frame rate is 60 with frame-rate independent timing since 2026-10-30), Enhanced (Match window aspect, HUD at
  the screen edges, Match window resolution, 4x MSAA, 16x AF, independent timing at the display's refresh) and
  Widescreen, light (Match window, 1x, no MSAA, 4x AF, independent at 60). A preset sets only those keys; controls and
  sound stay. *Screen*: **TV output** (`display.ps5_output`: Automatic = 4K when the TV has it, 1080p, 1440p, 4K;
  restart), Aspect ratio, HUD. *Frame rate*: Game timing, Frame rate cap, **Vertical sync** (`display.vsync`),
  **Show frame rate** (game frames per second over half a second, in a chosen corner).
- **Graphics**: Internal resolution, Anti-aliasing (MSAA), Texture filtering.
- **Audio**: **Master volume**, **Movie volume** (0-100%, squared to a gain so the steps sound even), **Speakers**
  (stereo / mono / swapped). **Music volume** and **Sound effects volume** are the game's own levels (its pause menu's
  Music / SFX Volume, 0..127 as percent: defaults 63% / 88% = its 5 / 7 bars): a change here shows in the pause menu
  and a change there comes back here and into vcs.ini; a save load keeps the player's levels (game-internals.md 9c,
  `VCS_GAME_VOLUME=0` leaves the game's levels alone). A Voices volume was removed again the same day (DECISIONS.md).
- **Game**: Skip intro movies.
- **Gamepad**: the bindings, sticks, triggers and vibration (docs/CONTROLS.md).
- **Advanced**: Performance overlay; **Reset all settings** (asks for a second press).

Registry additions (`settings::Spec`): `Type::Action` (a button: `button` text, `confirm` for a second press; no
value, nothing in vcs.ini, skipped by applyAll and the page reset); `hidden` (no row of its own; a page of hidden rows
is not shown). Layout: the help area is the longest help's height but at most four lines; a longer help is drawn
smaller (down to 60%) to fit, so the rows keep about two thirds of the menu whatever the texts.

Checked: `tools/controls_check.py` (21 checks: every default binding in the pad report, the presets, Reset all
settings, input back after the menu, the game's vibration reaching the pad) and `tools/ps5.py padtest` (the DualSense
path, 62 checks).

## The plan's architecture (2026-10-22)

```
 src/settings/   settings.h/.cpp   registry: typed settings, values, defaults, change callbacks, vcs.ini read / write
 src/ui/         settings_menu.h/.cpp   the menu: input routing, layout generated from the registry
 main.cpp        registers everything once at startup, hands the runtime a UI draw callback
```

- **Registry, not hand-written screens.** A setting is declared once with its metadata; the menu
  draws whatever is registered, grouped by page and section. Adding a setting = one
  `settings::add({...})` plus its apply callback; no menu code changes.
- **One source of truth for values**: vcs.ini. Keys use `section.name`. Environment variables keep winning for
  testing (`PS2X_HW_SCALE` over `display.scale`), shown in the menu as "set by environment" and greyed out.
- **Writing vcs.ini**: rewrite the file keeping lines we do not own, update / append ours, write to
  a temporary file and rename (no half-written ini after a crash).
- **Rendering**: Dear ImGui (the platform layer's context, the RHI's renderer), drawn over the game every frame in the
  runtime's UI hook (`PS2Runtime::setDebugUiCallbacks`).
- **Pausing**: the game keeps running with input blocked while the menu is open.
- **Live vs restart**: settings that change GPU resources the renderer allocates once (internal resolution, MSAA, the
  TV output) are `Apply::Restart`.

## The plan's widescreen part (2026-10-22)

### Modes

- **4:3 (original)**: the picture presented at the correct 4:3 display aspect.
- **16:9**: the game's own widescreen mode (preference on), presented stretched to 16:9 - replaced by Hor+ after the
  spike (above).
- **16:10, 21:9, Match window**: the aspect global overridden to the target ratio; presented at that ratio.

### How each part works

1. **Presentation.** The presenter (`render/presenter.cpp`) gets a display aspect: 4:3, or the widescreen ratio, and
   scales the 640x448 frame to it (pillar- / letterboxing the rest of the output).
2. **Resolution where it's needed.** In widescreen the game still draws 640 pixels across a wider view, so horizontal
   detail drops (by 25% at 16:9, 43% at 21:9). The HW renderer has separate horizontal and vertical scales: render
   targets, scissor, the render-target texel scale in the GS shader and the native draws' targets all take a 2D scale.
   With "Internal resolution 3x" at 16:9 the targets become 2560x1344 instead of 1920x1344 and the presented pixels
   are square again.
3. **HUD and 2D.** "HUD: keep 4:3 in the middle" squeezes the 2D draws in the renderer by gameAspect / displayAspect
   around the centre; "Screen edges" anchors each HUD element to the edge it is near (above).
4. **Full-screen 2D** (loading screens, the legal screen, fades, cutscene letterbox bars): kept at 4:3 (pillarboxed)
   where the game draws a 640x448 image.

### Risks / open questions

- Cutscenes may use their own cameras / FOV and the fixed-16:9 camera.
- Ratios beyond 16:9: culling and LOD distances follow the camera, but sector streaming is driven by the hex cell
  around the camera (docs/game-internals.md "Level data"), so very wide views could reach unloaded cells at the edges
  (checked at 21:9 and 32:9: none).
- Memory card: the game saves its preferences; with our setting authoritative the saved value is overwritten each
  session - harmless.

## Known issues noted for later (from play-testing, 2026-10-22)

Tracked in PROGRESS.md: objects / the world turning black for a few frames, and Vic falling
through the world in some places (collision).
