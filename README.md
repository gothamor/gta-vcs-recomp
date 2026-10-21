# GTA: Vice City Stories — PS2 static recompilation

A native PC port of **Grand Theft Auto: Vice City Stories (PS2, SLUS-21590 USA)**
made by statically recompiling the game's PS2 executable into C++.

**This repository contains no game code or game data.** You need your own copy
of the game: the build pipeline reads the executable from your ISO, and the
finished program reads all game files from your ISO at runtime.

Status: early bring-up. See [docs/PROGRESS.md](docs/PROGRESS.md).

## Goals
- Faithful to the PS2 original by default ("Authentic" preset).
- Optional, toggleable improvements: high resolution / supersampling, Hor+
  widescreen with correct HUD, 60 fps, later an independent frame rate.
- Fast. This is a 2007 game; it should run well on old hardware.

## Requirements
- Windows 10+, Visual Studio 2022 or newer (C++ workload, includes clang-cl), CMake, Ninja, Python 3.
- For re-doing the analysis only: Ghidra 12 + JDK 21.
- Your own `Grand Theft Auto - Vice City Stories (USA)` disc image (`.iso`).

## Quick check of your disc
```
python tools/vcs_disc.py info "path/to/your.iso"
```

## Layout
| Path | What |
|---|---|
| `third_party/PS2Recomp` | Vendored PS2 recompiler + runtime (GPL-3.0) |
| `game/vcs` | Recompiler config and function map for SLUS_215.90 |
| `src` | Game-specific runtime code (overrides, enhancements, host) |
| `tools` | Disc reader, build pipeline, analysis scripts |
| `docs` | Disc layout, engine notes, decisions, progress |

## License
GPL-3.0 (inherited from PS2Recomp). Grand Theft Auto and all game content are
property of Rockstar Games / Take-Two Interactive.
