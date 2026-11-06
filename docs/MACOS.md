# macOS (Apple Silicon)

The same source, the same `python3 tools/build.py` pipeline and the same game as Windows and Linux, built natively for
arm64 macOS. It draws with the Vulkan backend of the RHI (`src/rhi/vulkan`) through MoltenVK and links with Apple's
clang + ld64 (ThinLTO). Apple Silicon is what it is developed and tested on (an M3 MacBook Air, 16 GB, macOS 27);
Intel Macs should build but are untested. Decisions: DECISIONS.md, 2026-10-31.

## Requirements

- **Xcode Command Line Tools** (`xcode-select --install`): Apple clang 19+ and the SDK.
- **Homebrew packages**:

  ```
  brew install cmake ninja glslang ffmpeg pkgconf vulkan-loader molten-vk
  python3 tools/build.py --check-deps       # what is missing, with the brew line
  ```

  `vulkan-loader` + `molten-vk` are the graphics driver the game runs on; `glslang` compiles the shaders at build
  time; FFmpeg decodes the movies; `pkgconf` finds it. Homebrew LLVM is **not** required: Apple clang builds and
  links (its ThinLTO bitcode is the one ld64 reads). If it is installed, its `llvm-symbolizer` reads the offsets in
  crash reports. `VCS_APPLE_CLANG=0` builds with Homebrew LLVM instead (then without ThinLTO: its bitcode is newer
  than ld64 reads, and lld 23 mis-links the current SDK's Objective-C stubs).

## Build

Put your own disc image (USA, SLUS-21590) anywhere the app may read it and build:

```
python3 tools/build.py --recompile "/path/to/Grand Theft Auto - Vice City Stories (USA).iso"
```

The first build generates the recompiled code and compiles it: with Apple clang the batches emit ThinLTO bitcode
quickly and the link does the code generation, so the whole build is about 5 minutes on an M3 (16 GB). Later builds:
`python3 tools/build.py` (the default is ThinLTO on macOS).

PGO (optional, a few percent): `python3 tools/build.py --opt pgo --train`. It builds an instrumented game, trains it
(`tools/bench.py`, `tools/scenes.py`) and merges the profiles with Xcode's `llvm-profdata` (Homebrew LLVM's newer one
rejects Apple clang's raw profile format). Training starts from `build/rig/saves/fortbaxter`; without a free-roam
card the intro keeps `bench.py` out of gameplay.

A `.app` for Finder (ad-hoc signed): `python3 tools/package_mac.py --out VCS.app`, then `open VCS.app`.

## Run

- `build/vcs` or `VCS.app`. The disc is `iso=...` in `vcs.ini` next to the executable, `argv[1]`, or the disc chosen
.
- **Settings: ` (backquote)**, or F2 with Fn held (on Mac keyboards F2 is the brightness key). **Trainer: \**, or Fn+F5.
  Both are rebindable in Settings > Keyboard & mouse; Alt+Enter switches full screen.
- **The pause menu works as on a PC** (docs/CONTROLS.md; the same on every platform since 2026-11-05): Escape opens it,
  Enter selects, the arrows move, the mouse points and clicks. Saving happens where the PS2 game saves: in a
  safehouse - walk into its save marker (there is no Save Game in the pause menu).
- Settings, the trainer, the control channel (`PS2X_CONTROL=1`, `tools/gamectl.py`), `tools/run.py` and the rig
  scripts work as on Linux. Debug switches: docs/PROGRESS.md.
- Memory cards and caches: `~/Library/Application Support/VCSRecomp` (or `VCS_DATA_DIR`).
- **The disc and macOS privacy**: a GUI app may not read Desktop, Documents or Downloads without permission
  (Terminal runs inherit the terminal's grant - that is why a shell run never asks). Keep the image in a plain
  folder such as `~/VCS`, or allow the prompt once.
- **Vulkan**: MoltenVK behind Homebrew's loader. `dlopen` ignores rpaths, so the platform layer finds the loader
  itself (`/opt/homebrew/lib/libvulkan.1.dylib`, `/usr/local/lib/...`; `VCS_VULKAN_LOADER=<path>` overrides) and the
  RHI initialises volk from it. The ICD is found by the loader normally (no `VK_ICD_FILENAMES` needed).
  `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset` are enabled automatically; a MoltenVK that reports
  Vulkan 1.3 with dynamic rendering, synchronization2, push descriptors and demote is required (1.3.0+).
- **Distribution**: release zips are Windows-only; macOS builds from source, like Linux.
- **Retina**: the window is 640x448 logical; make it full screen (F2 > Display > Window mode) and raise
  Graphics > Internal resolution (2x is a good start, "Match window" is the sharpest).
- **Cursor**: in full screen it hides after 2 s without mouse movement (move the mouse to bring it back); in menus it
  is visible. During cutscenes the game itself has the input, which is normal.

## Performance and heat

Measured on an M3 MacBook Air (16 GB), fullscreen 1710x1073, intro scene:

| Setting | CPU (all threads) |
|---|---|
| 60 fps, internal 2x | ~1.0 core |
| 60 fps, internal 1x | ~1.0 core (the internal resolution is GPU work) |
| 30 fps, internal 2x | ~0.57 core |
| 30 fps, internal 1x | ~0.53 core |

The defaults here are a 60 fps cap with vsync, internal 2x, MSAA off and 4x anisotropic filtering - comfortable on a
fanless Air. It gets hot with "Match window" internal resolution, 4x MSAA, 16x anisotropic filtering and the frame
cap at the display's refresh (all of it present in one place: F2 > Display/Graphics). To run cooler: frame rate cap
30, or the "Authentic (PS2)" preset (30 fps, 1x), or internal resolution 1x (GPU only).

## Launcher

The launcher is not part of this port: 0.1.0 rewrote it as a native Win32/D3D11 app, and 0.1.1 added Linux (X11 /
Wayland, Vulkan) to the same shared UI. On macOS build and run with `tools/build.py` and `build/vcs`, or package a
`VCS.app` with `tools/package_mac.py`. A macOS front end can follow once that UI gets a Cocoa / MoltenVK back end.

## Differences from Windows / Linux, and why

| Area | macOS | Why |
|---|---|---|
| Renderer | Vulkan (MoltenVK) | D3D11 is Windows only |
| Compiler / linker | Apple clang + ld64, ThinLTO | ld64 cannot read Homebrew LLVM's newer ThinLTO bitcode, and lld 23 mis-links the current SDK's Objective-C stubs |
| SSE intrinsics | `sse2neon` (`USE_SSE2NEON`, fetched by `third_party/PS2Recomp`) | No SSE on arm64; the runtime, generated code and `src/game` include the same header |
| VU1 `vu1v` vector path | 128-bit path on NEON; the AVX2-only double-precision blocks (`fused`, `floatToInt`) run per lane | AVX2 does not exist on Apple Silicon; the lane maths is the same |
| Guest calling convention | default (no `preserve_none`) | The attribute is x86-only (`ps2_guest_cc.h`) |
| Crash handler | POSIX: `sys/ucontext.h`, PC from `__ss.__pc`, `pthread_self` | The Linux branch read `REG_RIP` and `gettid` |
| Profiler | supported: `VCS_PROFILE=5` (Mach threads + `pthread_kill(SIGPROF)`, `dladdr` symbols) | The Linux profiler used `/proc` and `tgkill`; `VCS_PROFILE_CALLERS` / `VCS_PROFILE_LINES` stay Windows-only |
| Saves / cache | `~/Library/Application Support/VCSRecomp` | macOS convention |
| Menu keys | ` and \ (F2 / F5 with Fn also work) | F2/F5 are the brightness and backlight keys on Mac keyboards |

## What was checked (2026-10-31 - 11-03, M3 MacBook Air 16 GB)

- Builds from the USA disc (SLUS-21590): 10 383 functions discovered, 10 210 recompiled and 173 SDK functions
  (libpad, libgraph, DMA) as native stubs; 0 skipped, 0 decode failures, 50 of 50 VU1 programs (3 032 pairs) compiled,
  ~222 MB of C++. The 561 unhandled-instruction sites compile to `throw` inside functions no tested scene executes
  (no `[guest-branch:unlisted-resume]` in any log); 1 196 indirect branches were promoted to fallback entries.
- Boots to the intro; `[present] Vulkan on Apple M3 (MoltenVK 1.4.2)`; 60 fps locked, `game_ms` 2.4-4.0, ~1400 native
  draws and 75 k vertices per frame; PGO + ThinLTO build: 90 fps with the cap at 240 and vsync off in the city test
  scene.
- Settings (F2), presets, widescreen (16:9), MSAA, internal resolution, frame cap, vsync; perf overlay; trainer (\)
  with teleports, time, weather; HUD and radar.
- Movies (FFmpeg), input (keyboard, mouse, and an SDL virtual gamepad on the stick), save to the memory card at a safehouse marker and load
  back through the game's own menus (the save wrote a 96 KB slot; Load Game listed it and the runtime opened it).
- A 25-minute run (170 cycles of teleports across the map, weather and time changes, walking): no crash, RSS
  0.31-0.45 GB (peak 0.45), 60 fps after each teleport settles, `VU fallback 0` throughout; the last frames still
  render the street, traffic, HUD and radar. An earlier 8-minute run peaked at ~0.6 GB while loading a save.
- `tools/cruise.py` (0.2.0's stutter rig; the default city flight, 60 fps cap, ~138 s): 59.4 fps, p50 16.7 ms,
  p95 16.7, p99 23.4, max 105.9; 13 hitches (242 ms lost a minute), 86 texture uploads/s. With the Vulkan free-threaded
  creation that 0.2.0 added as an opt-in (`PS2X_HW_EARLY_TEXTURES=1`) the same route gives 59.5 fps, p99 23.1, max
  94.4 and 8 hitches (126 ms lost a minute) - the helper threads pay off on the M3's cores, so the flag is worth
  trying on macOS even though it stays off by default.
- The project's own checks on macOS, from a real save made in game at a safehouse: `tools/saves.py` (save, load,
  delete and the card backup through the menus), `tools/soundcheck.py all` (the native sound driver against the
  emulated IOP: 100% of the SPU2 register writes in the same second, the first 4332 write-orders identical, audio
  envelopes correlate 0.988) and `tools/bench.py` (60 fps at the city spots; 58 at two during a run after hours of
  testing on the fanless Air), plus `tools/trainer_check.py` (41 of 41 checks: vehicles, weapons, teleports, time,
  weather, camera, missions), and `tools/controls_check.py` (72 of 72 on 0.2.2: every default binding, presets, reset,
  the Audio page, a save load, free aim on the keyboard / mouse and the gamepad, the sniper scope turned 1:1 and
  zoomed by the wheel, vibration and the HUD)..

Not available on this machine: a physical gamepad (the input path is checked with an SDL virtual pad,
`VCS_VIRTUAL_GAMEPAD=1`), and the Windows-only profiler switches.

## Troubleshooting

| What you see | What to do |
|---|---|
| Nothing happens on F2 / F5 | They are the brightness / backlight keys: use ` and \, Fn+F2 / Fn+F5, or rebind them |
| "no disc image" | Put the `.iso` in `vcs.ini` next to the game (`iso=/path/...`) or pass it as the first argument |
| `unsupported executable build` / `SLUS_215.90 not found` | The disc is not the USA release |
| "no Vulkan loader" / "no GPU with Vulkan 1.3" | `brew install vulkan-loader molten-vk`; `VCS_VULKAN_LOADER=/path/libvulkan.1.dylib` selects a loader |
| The app asks for access to a folder | macOS privacy (Desktop / Documents / Downloads); allow it or keep the disc in `~/VCS` |
| Gatekeeper: "unidentified developer" | The app is ad-hoc signed locally: right click > Open the first time |
| No sound | F2 > Audio > Master volume (or `audio.volume` in `vcs.ini`) |
| CMake configure fails while downloading | Check the internet; `tools/deps.py fetch` makes a release folder offline |
| Slow build | `VCS_COMPILE_JOBS` (default: one job per 3 GB) and `PS2X_LTO_JOBS` in the CMake cache |
