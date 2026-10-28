# Controls

Keyboard, mouse and a gamepad (anything SDL3 knows: Xbox, DualShock / DualSense, Switch Pro, ...) all drive the
same PS2 controller (DualShock 2, analog mode), so they can be mixed. Every binding can be changed in the settings
menu (F2): the **Keyboard** and **Gamepad** pages (src/game/vcs_input.cpp).

## Default bindings

| PS2 | Keyboard / mouse | Gamepad |
|---|---|---|
| Left stick | W A S D | Left stick |
| Right stick (camera) | I J K L (and the mouse, with Mouse camera on) | Right stick |
| D-pad | Arrow keys | D-pad |
| Cross | Shift | A / Cross |
| Square | Space | X / Square |
| Triangle | F | Y / Triangle |
| Circle | Ctrl, left mouse button | B / Circle |
| L1 | Q | LB |
| R1 | E, right mouse button | RB |
| L2 | 1, Z | LT |
| R2 | 3, X | RT |
| L3 / R3 | C / V | stick clicks |
| Start | Enter | Start / Menu |
| Select | Backspace | Back / View |
| Walk (hold) | - (unbound) | - (a small stick tilt) |

The game's own PS2 controls apply on top (e.g. driving: Cross accelerates, Square brakes). Start or Cross (Enter /
Shift) skips the intro movie; the Rockstar logos before it can't be skipped, as on the PS2 (Game > Skip intro movies
skips both).

## Keyboard page

- Every PS2 button, both sticks' directions and **Walk** have two slots. Click a slot, then press a key, a mouse
  button (left, right, middle, 4, 5) or turn the wheel (each notch is a short press); Esc cancels, a right-click on a
  slot clears it. Shift / Ctrl / Alt bind both sides, Enter is also Keypad Enter.
- A key does one thing: binding it to a control takes it from the control that had it. The menu keys (Settings menu
  F2, Trainer F5, same page) never take a control's key, and a control can't take theirs - the menu says which setting
  has it.
- **Walk (hold)**: the left stick at the **Walking speed** (45% by default) while held - GTA walks below about half
  a tilt, which keys can't do otherwise.
- **Mouse camera** (off by default): mouse movement turns the camera as the right stick does, and the mouse is held in
  the window while playing (free in the menus and when the window is in the background). The mouse's speed (counts
  per second) becomes the stick's tilt, so a movement turns the camera as far at any frame rate; the stick has a top
  speed, so very fast flicks turn no faster. **Mouse sensitivity** (100%: 1,500 counts a second is full tilt) and
  **Invert mouse up / down**.

## Gamepad page

- **Buttons**: the gamepad input for each PS2 button (any button, LT / RT, stick clicks, D-pad, or none); several PS2
  buttons may share one.
- **Sticks and triggers**: deadzone per stick (12%; the range past it is stretched, so a small tilt beyond it still
  counts), **Camera speed** (the right stick's tilt scaled), **Trigger press point** (LT / RT are L2 / R2 buttons),
  **Swap sticks**, **Invert camera** left / right and up / down.
- **Vibration**: the DualShock 2's motors on the gamepad's rumble, 0-200% (100% as on the PS2, 0% off). The game
  drives them itself (crashes, explosions, gunfire; game-internals.md 12a).

## Menus

F2 (or Back + Start on a gamepad) opens the settings, F5 (or Back + RB) the trainer (docs/TRAINER.md). In the
trainer: Up / Down or numpad 8 / 2 or the D-pad select, Left / Right or numpad 4 / 6 change a value, Enter / numpad
5 / A applies, Backspace / numpad 0 / Escape / B goes back (and closes it on the first page). The game gets no input
while a menu is open.

## Window

Alt+Enter switches between full screen and the window (Display > Window mode; saved in vcs.ini). Enter with Alt
held is not Start. Game > **In the background** decides what happens when another window is in front: keep running
(as before), mute, or pause.

## Debug keys
| Key | Effect |
|---|---|
| F3 | performance overlay: game fps and frame-time percentiles, EE thread time per phase (game / render / IOP / wait), GS and DMA counters, host frame and GPU submit times, audio buffer, and a frame-time graph |
| F4 | start / stop recording one CSV row per game frame to `perf_<date>_<time>.csv` (working directory); summarise with `python tools/perf_report.py <file>` |
| F12 | (hardware renderer) list the next frame's draws on stderr and dump its render targets |

## Test runs without a window
`python tools/run.py 600 test --hidden` starts the game with no window on the desktop, no sound, and the physical
keyboard / mouse / gamepad ignored (`PS2X_HIDDEN=1`). It listens on 127.0.0.1:47615 (`PS2X_CONTROL_PORT`) for
`tools/gamectl.py`: `screenshot out.png` (the picture plus the perf overlay, rendered off-screen), `press Return 150`,
`key W down` / `up` (also `mouseleft`, `mouseright`, `wheelup`, `rshift`, `kpenter`, ...), `perf`, `quit`, and from
the game layer `setting <key> [value]` (a setting read or changed; an action setting is pressed), `menu <page>` /
`menu scroll <0..1>` / `menu close` (the settings menu for screenshots) and `padstate` (the last pad report the
bindings made). `--control` (`PS2X_CONTROL=1`) opens the same channel with the window shown; `PS2X_INPUT=injected`
ignores physical input with the window shown. Injected keys go through the bindings like physical ones:
`tools/controls_check.py` checks them (also a `regress.py` step). `VCS_INPUT_MAP=0` falls back to the runtime's fixed
built-in mapping (`ps2_pad.cpp`, the old defaults).
