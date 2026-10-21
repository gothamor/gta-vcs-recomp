# Controls

Keyboard, mouse and a gamepad (XInput / SDL-style, via raylib) all drive the
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
Square brakes). Mapping lives in `third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp`;
a remappable `vcs.ini` section is planned.
