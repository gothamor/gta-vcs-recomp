# GTA: Vice City Stories — PC port (PS2 static recompilation)

A native Windows port of **Grand Theft Auto: Vice City Stories** (PS2, USA, SLUS-21590), made by
recompiling the game's PS2 executable into C++.

**This repository contains no game code or game data.** You need your own copy of the game as an `.iso`.
The build reads the executable from your disc, and the finished game reads everything else from it while it runs.

Status: in development. See [docs/PROGRESS.md](docs/PROGRESS.md) for what works.

## Install

### 1. Install the tools (once)

- **Python 3**: [python.org](https://www.python.org/downloads/). During setup, tick *Add python.exe to PATH*.
- **Git**: [git-scm.com](https://git-scm.com/download/win) (the default options are fine). The first build uses it
  to download the libraries the game needs (SDL3, FFmpeg, ...), so it also needs an internet connection.
- **Visual Studio 2022 or newer** (the free Community edition, or just the
  [Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)). In the installer, pick the workload
  **Desktop development with C++**, and under *Installation details* on the right also tick
  **C++ Clang tools for Windows**.

That's all: CMake and Ninja come with Visual Studio, and you don't need to open Visual Studio or a special command
prompt.

### 2. Get the source and your disc

1. Get the source, e.g. into `C:\Games\vcs-recomp`:
   ```bash
   git clone <this repository's URL> C:\Games\vcs-recomp
   ```
   (*Code > Download ZIP* on GitHub works too.)
2. Copy your disc image into that folder. It must be the USA release, and its file name must contain
   `Vice City Stories`, like the usual `Grand Theft Auto - Vice City Stories (USA).iso`.

Optional check that the disc is the right one:
```bash
python tools/vcs_disc.py info "Grand Theft Auto - Vice City Stories (USA).iso"
```

### 3. Build

Open a terminal in the folder (in Explorer: right-click the folder > *Open in Terminal*) and run:
```bash
python tools/build.py --recompile
```
This recompiles the game from your disc and builds it, optimised. The first time takes a while: the game becomes
about 200 MB of C++ to compile. It ends with:
```
[build] ok: build\release\third_party\PS2Recomp\ps2xRuntime\vcs.exe (optimised: ThinLTO)
```
If your `.iso` is somewhere else, give its path: `python tools/build.py --recompile "D:\path\to\your.iso"`.

### 4. Play

Start `build\release\third_party\PS2Recomp\ps2xRuntime\vcs.exe` (a shortcut to it on the desktop works well).
The build already told it where your disc is.

- **F2** opens the settings (resolution, widescreen, frame rate, ...). The defaults match the PS2; every
  improvement is an option you turn on. Settings are saved in `vcs.ini` next to `vcs.exe`.
- **Alt+Enter** switches between full screen and the window.
- **F5** opens the trainer ([docs/TRAINER.md](docs/TRAINER.md)).
- Keyboard, mouse and gamepads (Xbox, PlayStation, Switch Pro, ...) all work: [docs/CONTROLS.md](docs/CONTROLS.md).
- Saves (the memory card) live in `%LOCALAPPDATA%\VCSRecomp`.

## Updating

After downloading a newer version of the source, build again the same way:
```bash
python tools/build.py --recompile
```

## Problems

| What you see | What to do |
|---|---|
| `vswhere.exe not found` | Visual Studio isn't installed: see step 1. |
| CMake configure fails, mentioning `clang-cl` | Open the Visual Studio Installer, *Modify*, and tick **C++ Clang tools for Windows**. |
| `no disc image` | Put the `.iso` in the repository folder, or give its path after `--recompile`. |
| `unsupported executable build` or `SLUS_215.90 not found on disc` | The disc isn't the USA release (SLUS-21590). Other regions aren't supported. |
| CMake configure fails while downloading | Check the internet connection and that `git --version` works in a new terminal. |
| `vcs.exe` does nothing when started | It can't find the disc: put `iso=C:\path\to\your.iso` in `vcs.ini` next to `vcs.exe`. |

The build's full output is in `build\build.log` (`build\configure.log` for the configure step).

## For developers

| Command | What |
|---|---|
| `python tools/build.py` | rebuild after code changes |
| `python tools/build.py --quick` | rebuild without link-time optimisation: much faster relinks, a somewhat slower game. It stays quick until `--opt lto` (switching recompiles everything, ~6-10 min) |
| `python tools/build.py --opt lto` | back to the optimised build (the default) |
| `python tools/build.py --opt pgo` | ThinLTO + profile-guided, the fastest game; trains a profile first (~30 min, needs the rig's card copies in `build/rig/saves`) |
| `python tools/recompile.py [ISO]` | disc → generated C++ only |
| `python tools/run.py 0` | run the game with its output logged to `build/run/run.log` (`run.py 60 name`: stop after 60 s, log to `name.log`) |
| `python tools/gamectl.py` | control a running game: screenshots, key presses, perf numbers, quit |
| `python tools/ghidra_analyze.py` | redo the Ghidra analysis (Ghidra 12 + JDK 21) |

All tools are Python 3 scripts using only the standard library. [AGENTS.md](AGENTS.md) is the guide to working
on the code; the documentation is in [docs/](docs).

| Path | What |
|---|---|
| `third_party/PS2Recomp` | Vendored PS2 recompiler + runtime (GPL-3.0) |
| `game/vcs` | Recompiler config and function map for SLUS_215.90 |
| `src` | Game-specific code (overrides, renderer, settings, platform) |
| `tools` | Disc reader, build pipeline, test and analysis scripts |
| `docs` | How the game works, decisions, progress |

## License

GPL-3.0 (inherited from PS2Recomp). Grand Theft Auto and all game content are property of Rockstar Games /
Take-Two Interactive.
