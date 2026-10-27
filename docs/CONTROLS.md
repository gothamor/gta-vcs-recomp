# Controls

Keyboard, mouse and a gamepad (anything SDL3 knows: Xbox, DualShock / DualSense,
Switch Pro, ...) all drive the
same PS2 controller (DualShock 2, analog mode), so they can be mixed.

| PS2 | Keyboard / mouse | Gamepad |
|---|---|---|
| Left stick | W A S D | Left stick |
| Right stick (camera) | I J K L | Right stick |
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

The game's own PS2 controls apply on top (e.g. driving: Cross accelerates,
Square brakes). Start or Cross (Enter / Shift) skips the intro movie; the Rockstar logos before it can't be
skipped, as on the PS2 (Display > Skip intro movies skips both). Mapping lives in `third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp`;
a remappable `vcs.ini` section is planned.

## Trainer
F5 (Controls > Trainer menu key) or Back + RB on a gamepad opens the trainer (docs/TRAINER.md). In it: Up / Down or
numpad 8 / 2 or the D-pad select, Left / Right or numpad 4 / 6 change a value, Enter / numpad 5 / A applies,
Backspace / numpad 0 / Escape / B goes back (and closes it on the first page). The game gets no input while it is open.

## Window
Alt+Enter switches between full screen and the window (Display > Window mode; saved in vcs.ini). Enter with Alt
held is not Start.

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
`key W down` / `up`, `perf`, `quit`. `--control` (`PS2X_CONTROL=1`) opens the same channel with the window shown;
`PS2X_INPUT=injected` ignores physical input with the window shown.
