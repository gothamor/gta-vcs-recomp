# Controls

The DualSense drives the game's PS2 controller (a DualShock 2 in analog mode: the buttons and both sticks; the game
reads no pressure bytes - game-internals.md 12a). Every binding can be changed on the settings menu's **Gamepad** page
(src/game/vcs_input.cpp). The DualSense's side is the PS5 platform layer (src/platform/platform_ps5.cpp, docs/PS5.md).

## Default bindings

| DualSense | PS2 | In the game |
|---|---|---|
| Left stick | Left stick | move |
| Right stick | Right stick | the camera |
| D-pad | D-pad | |
| Cross | Cross | sprint, accelerate |
| Square | Square | jump, brake / reverse |
| Triangle | Triangle | enter / leave a vehicle |
| Circle | Circle | attack |
| L1 | L1 | |
| R1 | R1 | target (held: lock on), handbrake |
| L2 / R2 (the triggers, past *Trigger press point*) | L2 / R2 | |
| L3 / R3 (stick clicks) | L3 / R3 | |
| Options | Start | the pause menu |
| Touchpad click | Select | (the Create button belongs to the system: apps never see it) |

Start or Cross skips the intro movie; the Rockstar logos before it can't be skipped, as on the PS2 (Game > Skip intro
movies skips both). While driving, Cross accelerates, Square brakes then reverses and R1 is the handbrake (measured in
game: game-internals.md 12a).

## Gamepad page

- **Buttons**: the DualSense input for each PS2 button (any button, L2 / R2, the stick clicks, the d-pad, the touchpad
  click, or none); several PS2 buttons may share one.
- **Sticks and triggers**: deadzone per stick (12%; the range past it is stretched, so a small tilt beyond it still
  counts, and the stick's reach is round), **Camera speed** (the right stick's tilt scaled), **Trigger press point**
  (30%: L2 / R2 are buttons on the PS2), **Swap sticks**, **Invert camera** left / right and up / down.
- **Vibration**: the DualShock 2's motors on the DualSense, 0-200% (100% as on the PS2, 0% off). The game drives them
  itself (crashes, explosions, gunfire; game-internals.md 12a): its large motor is the DualSense's low-frequency one,
  its small one the high-frequency one.

## Menus

Hold the touchpad and press:

| With the touchpad held | Opens / closes |
|---|---|
| Options | the settings menu: Cross selects, Circle backs out and closes, L1 / R1 change pages |
| R1 | the trainer (docs/TRAINER.md): d-pad up / down select, left / right change a value, Cross applies, Circle goes back (and closes it on the first page) |
| L1 | the trainer's free camera: left stick moves, right stick looks, R2 up, L2 down, Cross faster, Circle slower, d-pad up / down zoom |

The game gets no input while a menu is open, and none from a chord: the touchpad (Select) reaches the game once it has
been held 150 ms on its own, or as a short press when let go sooner; while it is held, Options / L1 / R1 do not reach
the game, and a hold that pressed one of them is a chord to the end - so opening a menu neither changes the view nor
pauses the game behind it (`src/game/vcs_input.cpp`). After a menu closes, the game gets the pad back once every
button is up.

## Checked

`python3 tools/ps5.py padtest` (`ps5/tests/padtest.cpp`) runs the real platform layer with the console's pad functions
mocked through the real bindings to the report the game reads: every button, both sticks, the triggers, Select tap and
hold, the menu chords, a controller gone and back, the motors, and the pad as the menus' gamepad keys (62 checks).
`VCS_INPUT_MAP=0` falls back to the runtime's fixed built-in mapping (`ps2_pad.cpp`).
