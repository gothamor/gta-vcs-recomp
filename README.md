# GTA: Vice City Stories — native PS5 port (PS2 static recompilation)

A native **PS5** port of **Grand Theft Auto: Vice City Stories** (PS2, USA, SLUS-21590), made by recompiling the game's
PS2 executable into C++ and building it as a homebrew app for jailbroken PS5 consoles. No emulator: the game's own code
runs natively on the console, drawn with Vulkan on the PS5's GPU, with the DualSense as the DualShock 2.

**This repository contains no game code or game data.** You need your own copy of the game as an `.iso`. The build
reads the executable from your disc, and the finished game reads everything else from it while it runs.

Status: in development, not run on a console yet. [docs/PS5.md](docs/PS5.md) has what is tested, the first-run
checklist and how it works; [docs/PROGRESS.md](docs/PROGRESS.md) the state of each area.

## What you need

- A jailbroken PS5 that runs homebrew apps from `/data/homebrew/`, and a payload loader with an FTP server (etaHEN,
  ftpsrv) or a USB drive to copy the app over.
- Your own disc image of GTA: Vice City Stories, **USA release (SLUS-21590)**, as an `.iso`.
- A build machine: Ubuntu 24.04 or WSL 2 (the PS5 SDK's tools run there), with Python 3, Git, clang 19+ and lld,
  CMake 3.21+, Ninja and glslang (`sudo apt install python3 git clang-20 lld-20 cmake ninja-build glslang-tools`).

## Build

In the source folder (the release's `VCS-Recomp-<version>-ps5.zip`, or a git clone), with your disc image in it or
its path given:

1. **The game's code from your disc** (the recompiler turns the executable into C++, about 200 MB):
   ```bash
   python3 tools/recompile.py "Grand Theft Auto - Vice City Stories (USA).iso"
   ```
2. **The PS5 toolchain**, once - [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle): the PS5 payload
   SDK, the console's Vulkan driver (RADV) and the tools that make `eboot.bin`. This fetches it next to the source
   folder at a pinned commit and builds it; it lists the packages it is missing first:
   ```bash
   python3 tools/ps5.py setup
   ```
3. **Check, then build the app**:
   ```bash
   python3 tools/ps5.py check
   python3 tools/ps5.py build --zip        # -> build/ps5/app/PPSA94590/ and build/ps5/PPSA94590.zip
   ```
   The first build compiles the whole game (about as long as the recompile's output is big: 15-30 minutes).
4. Optional, the movies: `python3 tools/ps5.py ffmpeg` builds FFmpeg's MPEG-2 decoder for the PS5, then build again.
   Without it the intro movies are skipped.

The app you build holds the game's code made from your disc: it is for your own console, never to share.

## Install and play

1. Put the app on the PS5 over its FTP payload (`--port 1337` for etaHEN's), with your disc image:
   ```bash
   python3 tools/ps5.py deploy --host <your PS5's IP> --iso "Grand Theft Auto - Vice City Stories (USA).iso"
   ```
   or unzip `build/ps5/PPSA94590.zip` into `/data/homebrew/` with a USB drive and put your `.iso` in
   `/data/homebrew/PPSA94590/disc/` (or on a USB drive, top folder or `games/`, named `*Vice City Stories*.iso`).
2. Start it from the PS5's home screen.

| On the console | What |
|---|---|
| `/data/homebrew/PPSA94590/disc/` | your `.iso` |
| `/data/homebrew/PPSA94590/vcs.ini` | the settings (the settings menu writes it) |
| `/data/homebrew/PPSA94590/userdata/` | the memory card (your saves) |
| `/data/homebrew/PPSA94590/logs/vcs.log` | the last run's log: send it with a bug report |

To update, copy the new app folder over the old one: the disc, the saves and the settings stay.

## Controls

The DualSense is the DualShock 2, button for button:

| DualSense | PS2 | |
|---|---|---|
| Cross, Circle, Square, Triangle | the same | sprint / accelerate, attack, jump / brake, enter a vehicle |
| L1 / R1, L2 / R2 (the triggers), L3 / R3 | the same | R1: target, handbrake |
| Left stick / right stick, d-pad | the same | move / camera |
| Options | Start | the pause menu |
| **Touchpad click** | **Select** | the Create button belongs to the system |

Hold the touchpad and press **Options** for the settings menu, **R1** for the trainer, **L1** for the free camera (the
game sees neither button). In the menus Cross selects, Circle backs out, L1 / R1 change pages. Every button can be
rebound on the settings menu's Controller page. More in [docs/CONTROLS.md](docs/CONTROLS.md).

## Features

The defaults are the PS2 game as it was, except the frame rate: 60 fps with frame-rate independent timing. Every other
improvement is an option in the settings menu, and *Display > Presets > Authentic (PS2)* goes back to the PS2's 30 fps;
*Enhanced* turns on the usual improvements at once.

- **Native**: the game's own code, recompiled to C++; the 3D world, characters, vehicles, HUD and menus drawn by a
  native renderer (Vulkan on the console's GPU); sound through the console's audio output; the disc streamed from the
  `.iso`; the movies decoded with FFmpeg.
- **Picture**: output to the TV in 4K (or 1440p / 1080p: *Display > TV output*); Hor+ widescreen (16:9 shows more to
  the sides; nothing cropped or stretched), the HUD at 4:3, at the screen edges or stretched; internal resolution from
  1x (640x448) up to 8x or the TV's own, MSAA 2x / 4x / 8x, anisotropic filtering up to 16x.
- **Frame rate**: frame-rate independent timing with game time following real time (60 by default), or the original
  timing at 30 fps; things the PS2 game counted per frame (vehicle physics, mission meters and timers) put back on real
  time; a frame-rate counter and a performance overlay.
- **Controls**: the DualShock 2's layout on the DualSense, every button rebindable, stick deadzones, camera speed,
  trigger press point, swapped sticks, inverted camera axes, the game's rumble on the DualSense from 0% to 200%;
  free aim (off by default): with a gun, R1 aims where the right stick points, through the game's own aiming circle.
- **Sound**: master, music, sound effects and movie volume; stereo, mono or swapped.
- **Built-in trainer** (touchpad + R1), a list menu in the style of RDR2's Native Trainer
  ([docs/TRAINER.md](docs/TRAINER.md)): god mode, wanted level, money, any weapon, any of the 110 vehicles, teleports,
  any of the 59 story missions or a side job, time, weather, game speed - through the game's own script commands, and
  nothing of it is ever saved.
- **Also**: skip the intro movies (off by default, as on the PS2); saves on a memory card written and read by the
  game's own save code, zipped into `userdata/backups/` before every change; every setting kept in `vcs.ini`.

## Problems

| What you see | What to do |
|---|---|
| `python3 tools/ps5.py check` lists something missing | It names the command that fixes each line. |
| `unsupported executable build` / `SLUS_215.90 not found` | The disc isn't the USA release (SLUS-21590). Other regions aren't supported. |
| The app closes right away, with a notification | The notification says why (no disc image found, the wrong disc...). |
| Anything else on the console | Send `/data/homebrew/PPSA94590/logs/vcs.log`; [docs/PS5.md](docs/PS5.md) has the first-run checklist. |

The build's output is in `build/ps5/build.log` (`build/ps5/configure.log` for the configure step).

## For developers

| Command | What |
|---|---|
| `python3 tools/recompile.py [ISO]` | disc → generated C++ (`--check`: is generated/ current?) |
| `python3 tools/ps5.py setup` | ps5-plume-triangle fetched at its pinned commit and built |
| `python3 tools/ps5.py check` | what the PS5 build needs, what is missing, and how to get it |
| `python3 tools/ps5.py build [--zip] [--lto]` | build/ps5 (CMake + Ninja) → `build/ps5/app/<title id>/` |
| `python3 tools/ps5.py package [--zip]` | the app folder again from `build/ps5/vcs.elf` |
| `python3 tools/ps5.py deploy --host IP [--iso F]` | the app folder to `/data/homebrew/<title id>` over FTP |
| `python3 tools/ps5.py ffmpeg` | FFmpeg for the PS5 (the movies) |
| `python3 tools/ps5.py compile-check --sdk DIR` | the PS5 build compiled against stand-in code: no disc needed (CI) |
| `python3 tools/ps5.py padtest` | the DualSense → DualShock 2 path checked with the console's pad functions mocked (71 checks; built on its own) |
| `python3 tools/release.py` | the release zip: the source and `PS5-README.txt`, through a guard that refuses game files |
| `python3 tools/clean.py [--apply]` | delete what the build folder holds besides the builds (scratch output, logs) |
| `python3 tools/perf_report.py F.csv` | a performance recording (settings menu: Advanced > Record performance) |
| `python3 tools/ghidra_analyze.py` | redo the Ghidra analysis (Ghidra 12 + JDK 21) |

All tools are Python 3 scripts using only the standard library. [AGENTS.md](AGENTS.md) is the guide to working on the
code; the documentation is in [docs/](docs).

| Path | What |
|---|---|
| `third_party/PS2Recomp` | Vendored PS2 recompiler + runtime (GPL-3.0) |
| `game/vcs` | Recompiler config and function map for SLUS_215.90 |
| `src` | Game-specific code (overrides, renderer, settings, the PS5 platform layer) |
| `ps5` | The PS5 build's toolchain file and link rule, start-up code, the controller test |
| `tools` | Disc reader, recompile, the PS5 build and packaging, analysis scripts |
| `docs` | How the game works, the PS5 build, decisions, progress |

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp): the static recompiler and runtime this port is built on.
- The PS5 toolchain: [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) (Nazky),
  [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)
  (mihawk-99), the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) (John Törnblom), and the
  [gta4-ps5](https://github.com/chonzytron/gta4-ps5) port, whose way to the console this one follows.

## License

GPL-3.0 (inherited from PS2Recomp). Grand Theft Auto and all game content are property of Rockstar Games /
Take-Two Interactive. An unofficial fan project, not affiliated with or endorsed by Rockstar Games, Take-Two
Interactive or Sony.
