# Controls

The DualSense drives the game's PS2 controller (a DualShock 2 in analog mode: the buttons and both sticks; the game
reads no pressure bytes - game-internals.md 12a). Every binding can be changed on the settings menu's **Controller** page
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

Options (Start) or Cross skips the intro movie; the Rockstar logos before it can't be skipped, as on the PS2 (Game > Skip intro
movies skips both). While driving, Cross accelerates, Square brakes then reverses and R1 is the handbrake (measured in
game: game-internals.md 12a).

## Controller page

- **Buttons**: the DualSense input for each PS2 button (any button, L2 / R2, the stick clicks, the d-pad, the touchpad
  click, or none); several PS2 buttons may share one.
- **Sticks and triggers**: deadzone per stick (12%; the range past it is stretched, so a small tilt beyond it still
  counts, and the stick's reach is round), **Camera speed** (the right stick's tilt scaled), **Trigger press point**
  (30%: L2 / R2 are buttons on the PS2), **Swap sticks**, **Invert camera** left / right and up / down, **Free aim**
  (below; to aim with the left trigger, give R1 the L2 and move L2 to another button).
- **Vibration**: the DualShock 2's motors on the DualSense, 0-200% (100% as on the PS2, 0% off). The game drives them
  itself (crashes, explosions, gunfire; game-internals.md 12a): its large motor is the DualSense's low-frequency one,
  its small one the high-frequency one.

## Free aim

Controller > **Free aim** (off by default: the PS2's lock-on; src/game/vcs_aim.cpp, game-internals.md 12c):

- With a gun in hand (pistols, shotguns, SMGs, rifles, the M249, minigun and flame-thrower), holding R1's button puts
  up the game's own white aiming circle in the middle of the screen instead of locking on to the nearest person. The
  right stick turns the aim and the camera with it (2 rad/s at full tilt, finer near the centre; Camera speed and
  Invert camera apply). There is no aim assist.
- Circle fires **through the circle**: a shot hits what is under it, whatever the gun's own line. Letting go of R1 ends
  the aim at once (the camera leaves it 2 frames later), shots or not.
- **Moving while aiming** is what the game allows locked on with that gun - a pistol walks and strafes; no new movement
  is added. Letting go leaves the camera looking where the aim did. The camera moves into the aim in 0.1 s and out of
  it in 0.2 s, with every gun (the game blends 0.35-0.4 s, and cuts out of the aim with the guns that stand Vic still;
  its other camera changes keep their blends and cuts).
- When the gun runs dry while aiming the game puts the fists in Vic's hand: with R1 still held nothing locks on until it
  is let go, and choosing another gun aims again at once.
- Fists, melee weapons, thrown weapons and the scoped guns (sniper rifles, rocket launcher, camera, binoculars) keep R1
  as on the PS2.
- It is the game's own free aim (R1 then L3 on the PS2: the left stick moves an aiming point while Vic stands still),
  driven so that Vic can move and the right stick aims; `VCS_FREEAIM=0` leaves it out entirely.
- **Shooting from a car** (Jive Drive: Vic in the passenger seat while Lance drives - the game's own free aim, which the
  mission starts itself): with free aim on, the right stick turns the circle instead of the left stick (the game's help
  text still says the left stick), no button to hold, and **R1 fires** there (as Circle, which still fires too): the
  right thumb is on the stick. Only in this car aim and Phil's shooting range (below) - R1 is the target button
  everywhere else. The circle keeps its direction while the car turns, up to 20 degrees above level and 5 below, as the
  game allows there. With free aim off it is the PS2's left stick.
- **Phil's shooting range** (the game's free aim forced on foot, `SET_PLAYER_FORCE_FREE_AIMING`): the same - the right
  stick turns the circle, no button to hold, R1 and Circle fire. Up to 110 degrees either side of Vic's heading, 40
  above level and 20 below, as the game allows.

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

`python3 tools/ps5.py padtest` (`ps5/tests/`: built on its own for the build machine) runs the real platform layer
with the console's pad functions mocked through the real bindings to the report the game reads: every button, both
sticks, the triggers, Select tap and hold, the menu chords, free aim's hand-over (R1 and the right stick to it, its
R1 / L3 back in the report, once per game frame, a script's free aim firing with R1), a controller gone and back, the
motors, the Controller page, and the pad as the menus' gamepad keys (71 checks). `VCS_INPUT_MAP=0` falls back to the runtime's fixed built-in
mapping (`ps2_pad.cpp`: the same buttons by position). The DualSense is the only input: there is no keyboard or mouse
path anywhere in the game; the control channel (`runtime/ps2_control.h`, not reachable on the console yet) injects
DualShock 2 buttons (`button <name> down|up`, `press <name>`).
