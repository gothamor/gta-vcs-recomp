# AGENTS.md - working on the GTA: Vice City Stories native PS5 port

Read this first. It is the contract for anyone (human or AI agent) changing this repository. The
detailed knowledge lives in `docs/`:

| Doc | What's in it |
|---|---|
| [docs/PS5.md](docs/PS5.md) | The PS5 build: toolchain, the link recipe, packaging, the platform layer, what is tested, the first-run checklist, the work list |
| [docs/game-internals.md](docs/game-internals.md) | Everything known about how the game works inside: code map, boot, camera, renderer, VU1 pipeline, 2D, streaming, sound, memory card - with addresses and how each fact was found |
| [docs/NATIVE_PORT_ROADMAP.md](docs/NATIVE_PORT_ROADMAP.md) | The plan to completion (milestones 1-8) and what "done" means |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Status per area, open bugs, and the table of debug switches (`PS2X_*`, `VCS_*`) |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Why things are the way they are; the user's decisions |
| [docs/PS2RECOMP_PATCHES.md](docs/PS2RECOMP_PATCHES.md) | Every change to the vendored PS2Recomp runtime, with the reason |
| [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) | The native renderer: layers, the RHI, stages, HLE method |
| [docs/SETTINGS_MENU_WIDESCREEN.md](docs/SETTINGS_MENU_WIDESCREEN.md) | Settings registry / menu, Hor+ widescreen |
| [docs/FULLY_NATIVE.md](docs/FULLY_NATIVE.md) | The steps left until a normal frame is fully native, with their status - the working list |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Frame rate: what was done in the game code, what is measured on the PS5 next |
| [docs/RSL_SUBMISSION.md](docs/RSL_SUBMISSION.md) | Plan: render submission at the RSL level (milestone 7) - draws from the game's render calls instead of DMA packets |
| [docs/DISC.md](docs/DISC.md), [docs/CONTROLS.md](docs/CONTROLS.md) | Disc layout; the DualSense mapping |
| [docs/TRAINER.md](docs/TRAINER.md) | The trainer (touchpad + R1): what it does, how (the game's script commands), its check |

**When you learn something about the game, write it into docs/game-internals.md in the same change.**
Record addresses and how you found them. When something is weird, use judgment, fix or work around it,
and write it down (game-internals for the game, PS2RECOMP_PATCHES for the runtime, DECISIONS for
choices).

## What the project is

- GTA: Vice City Stories (PS2, SLUS-21590 USA) statically recompiled to C++ (PS2Recomp, vendored and
  heavily modified in `third_party/PS2Recomp`), turned step by step into a native PS5 homebrew app for jailbroken
  consoles: native rendering of the game's models (`src/game/vcs_hle_world.cpp`) through the RHI's Vulkan backend on
  the console's RADV driver, the PS5 platform layer (`src/platform/platform_ps5.cpp`: the TV, the DualSense, audio),
  settings menu, trainer. docs/PS5.md has the whole of the PS5 side.
- It is cross-compiled for `x86_64-sie-ps5` with clang 19+ against the PS5 payload SDK and linked with
  ps5-plume-triangle's recipe (`tools/ps5_link.py`); `tools/ps5.py` builds and packages it.
- Distribution is source-only, bring your own disc. **Never commit game code or data**: no ISO, no
  ELF, no generated C++ (`generated/`), no disassembly listings, no extracted assets, no built eboot. Analysis dumps
  go to `build/analysis/` (ignored). Function maps (names + addresses) and docs describing behaviour are fine.
- Authentic by default; every enhancement is an option (settings registry, `src/settings`).

## Layout

| Path | What |
|---|---|
| `src/main.cpp` | The app's start: the disc, the log, settings registration, runtime start |
| `src/game/` | Game-specific code: overrides / hooks (`vcs_overrides.cpp`), native draw HLE (`vcs_hle_world.cpp`), display (`vcs_display.cpp`), the trainer (`vcs_trainer.cpp`), frame timing (`vcs_framerate.cpp`, per-frame mission meters `vcs_script_timing.cpp`), the player's controls (`vcs_input.cpp`; free aim `vcs_aim.cpp`), the game's music / effects levels as settings (`vcs_volume.cpp`) |
| `src/render/`, `src/rhi/`, `src/platform/` | Presenter + shaders (HLSL, compiled to SPIR-V by `tools/hlsl_spirv.py`), the RHI (`rhi.h`; the PS5 draws with `vulkan/`; only the backends see a graphics API), the PS5 platform layer (`platform_ps5.cpp`) |
| `src/sound/` | Sony's libsd on the host (the native sound driver in `src/game/vcs_sound.cpp` drives the SPU2 through it) |
| `src/settings/`, `src/ui/` | Settings registry + vcs.ini, the settings menu (touchpad + Options), the trainer menu (touchpad + R1) |
| `src/debug/` | Crash handler, watchdog, self-test, memory watch, the overlay's game-state panel (`game_info.cpp`, also `gamectl.py gameinfo`), the control channel's automation commands (`rig.cpp`: the stutter rig's `cruise` among them) |
| `third_party/PS2Recomp/ps2xRuntime` | The runtime (EE, VIF, VU1, GS HW backend, IOP host, stubs); our changes marked `[gta-recomp]` |
| `game/vcs/` | Recompiler config, function map, VU1 upload list |
| `ps5/` | The PS5 build's own pieces: the CMake toolchain file and link rule (`cmake/`), start-up code linked into the game (`runtime/`: emulated TLS, `stat` wrapper, RELRO padding), the controller test (`tests/`) |
| `tools/` | Python 3 tools (standard library only) - see below |

## Build and run

```
python3 tools/recompile.py [ISO]                              # disc -> generated C++ (generated/; --check: is it current?)
python3 tools/ps5.py setup                                    # ps5-plume-triangle fetched (pinned) and built, once
python3 tools/ps5.py check                                    # what the build needs, what is missing, how to get it
python3 tools/ps5.py build [--zip] [--lto]                    # build/ps5 -> build/ps5/app/<title id>/ (+ its zip)
python3 tools/ps5.py package [--zip]                          # the app folder again from build/ps5/vcs.elf
python3 tools/ps5.py deploy --host IP [--iso F]               # the app folder to /data/homebrew/<title id> over FTP
python3 tools/ps5.py ffmpeg                                   # FFmpeg for the PS5 (the movies), then build again
python3 tools/ps5.py compile-check --sdk DIR [--triangle T]   # the PS5 build against stand-in code (no disc; CI)
python3 tools/ps5.py padtest                                  # the DualSense -> DualShock 2 path, console functions mocked (71 checks)
python3 tools/release.py [--ref vX | --worktree]              # the release zip (build/dist): the source + PS5-README.txt;
                                                              # refuses game files. The number is VERSION (root; 0.3.2):
                                                              # bump it before tagging - the tag must match
python3 tools/gamectl.py ...                                  # the game's control channel (runtime/ps2_control.h)
python3 tools/perf_report.py logs/perf_*.csv                  # a performance recording (Advanced > Record performance)
python3 tools/clean.py [--apply] [--keep-days 7]              # delete the build folder's scratch output (lists first)
python3 tools/ghidra_analyze.py                               # redo the Ghidra analysis (Ghidra 12 + JDK 21)
python3 tools/irxdis.py --decompile X.IRX OUT.c               # an IOP module as C (headless Ghidra; analysis only)
python3 tools/eedecomp.py 0x2f7d58 [...] [--out FILE]         # game functions as C (the analysed EE project; analysis only)
```

- The build machine is Ubuntu 24.04 or WSL 2 (the PS5 SDK's tools run there); the game is built for and runs on the
  console only - the build machine builds the recompiler (build/release) and the controller test, never the game.
- On the console the app's folder is `/data/homebrew/<title id>/`: `vcs.ini` (the settings), `userdata/` (the memory
  card), `logs/vcs.log` (the last run; `vcs.prev.log` the one before). Start-up failures are also PS5 notifications.
- Rebuild costs: a change to `ps2_memory.h`, `gs_backend.h`, `ps2_hle_survey.h` or anything the
  generated code includes rebuilds everything; runtime-only changes about a minute; `src/` only seconds. Prefer
  `src/game/vcs_overrides.cpp` for experiments.
- `compile-check` and `padtest` need no disc and no console: run them after any change to `src/`, `ps5/` or the
  runtime (`padtest` builds the controller's path alone: `ps5/tests/CMakeLists.txt`). The first run on a console is
  docs/PS5.md's checklist.
- LTO is off by default on the PS5 (`--lto` turns ThinLTO on: not measured on a console yet). Never edit sources while
  a build is compiling them.

## Rules that keep things working

1. **Reference first.** A native replacement of an emulated path is checked against it (bit-exact
   where possible: `PS2X_HLE_VERIFY=1`; picture diff otherwise) and keeps a switch back
   (`PS2X_HLE=0`-style). The emulated path is never deleted.
2. **Hooks on recompiled functions** (DECISIONS.md): a generated function may return to the EE
   scheduler mid-body and be resumed through the function table, bypassing a wrapper. A hook may work
   *before* calling the original or replace it entirely; never rely on code after the original
   returns. Never call a guest function directly from host code expecting it to run to the end -
   chain it as a guest tail call (see `VCS_TEST_SAVE`).
3. **Floating point**: no FP contraction in generated / VU code; VU maths rounds toward zero
   (`FE_TOWARDZERO` where reproducing it). Bit-exactness depends on both. The game computes in the IEEE state (MXCSR
   0x1f80), not the console's flush-to-zero default (docs/PS5.md).
4. **The console's limits** (docs/PS5.md): no file mappings of big files (the disc is read with `pread`), `thread_local`
   at most 32-byte aligned, no `preserve_none` for the PS5 target, `clock_gettime` is a system call (the TSC for hot
   timing), the Create button is the system's, there is no keyboard or mouse, and an app started from the home screen
   gets no environment (the `PS2X_*` / `VCS_*` switches: docs/PS5.md's work list).
5. **Patch scripts**: shell heredocs can turn `\n` inside Python strings into real newlines and corrupt C++ string
   literals. Use the Edit tool, or write the script to a file first.
6. **Scripts are Python 3, standard library only**, runnable from any shell; extend `tools/*.py`
   (shared helpers in `tools/toolchain.py`) instead of adding shell scripts.
7. **Runtime changes** in `third_party/PS2Recomp` get a `[gta-recomp]` marker and a row in
   PS2RECOMP_PATCHES.md.
8. **Commit** at every milestone / major change, with a message that says why. End commit messages
   with the attribution line the environment asks for.

## Recognising old problems

- Wrong colours on every texture: data shifted by VIFcodes (VIF1 DIRECT continuation, fixed).
- Stuck on a loading screen, no crash: an IOP-side wait (`VCS_TRACE_DELAY`, `PS2X_TRACE_SPU`).
- Thread "returns" to a float bit pattern (`0x3F333333`...): a saved `$ra` overwritten - the
  checkpoint-unwind class of bug (`VCS_WATCH=lo,hi,...`).
- Saves written but never found: card directory order (game-internals.md 10c).
- A mission stuck after a ped should talk, or peds vanishing: check the mission audio slots and who deletes the ped
  (`VCS_TRACE_PED_REMOVE`, game-internals.md 12); Shakedown's was the recompiler's 32-bit compare-with-zero branches.
- Something wrong only above 100 fps: a `ms_fTimeStep < 0.5` branch the PS2 never takes (game-internals.md 4).
- A mission bar / meter / timer faster at high frame rates: the mission script adds a fixed amount every frame; add
  the instruction to `src/game/vcs_script_timing.cpp` (game-internals.md 11f has the survey and how to read a mission).
- Load Game stuck on "Loading..." then a return to text (`0x4548435F`) from the script VM: host code took a game
  script for one of its own (a remembered script slot the load had refilled; game-internals.md 11d, `[rig] script
  slot` in the log).
- "Loading..." over black after a mission's last cutscene, the game running behind it (clock and scripts move): the
  screen left faded out for a script that never comes - Last Stand out of story order waits for the credits
  (game-internals.md 11g). The draw log (`PS2X_HW_DUMP`) shows the black quad and the text; the active scripts show
  who is missing.
- A failure on the console: start from docs/PS5.md's first-run checklist (`logs/vcs.log` in the app's folder); crash
  lines give offsets into `build/ps5/vcs.elf` and the `llvm-symbolizer` command for them.
- A save gone after saving (an empty slot file, or the whole save folder deleted): a card name a file system could not
  store, or a failed card write - the game deletes its save folder after one (game-internals.md 10f). The card is
  zipped into `userdata/backups/auto-*.zip` before every change (`[MC] card backup` in the log).
- First things to grep in a log: `[ee] thread N -> dormant`, `[guest-branch:unlisted-resume]`.

## Working style for long autonomous runs

- Follow docs/NATIVE_PORT_ROADMAP.md milestone by milestone without pausing for routine decisions;
  record decisions in DECISIONS.md and progress / numbers in PROGRESS.md.
- Measure before and after anything on the hot path (the performance overlay, the `[perf]` lines in the log); keep
  the numbers in the docs.
- Keep `compile-check` and `padtest` green; add a check when you fix a bug so it stays fixed.
