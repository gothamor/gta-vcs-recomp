# PS5

The game as a native PS5 homebrew app, for jailbroken consoles that run homebrew from `/data/homebrew/`: the game's
own code recompiled to C++, the PS5 platform layer (the TV, the DualSense, audio), the PS5's start-up code and link,
and the RHI's Vulkan renderer (`src/rhi/vulkan`) drawing through RADV, Mesa's AMD driver, linked into the game. No
emulator. The tree is PS5-only: the build machine builds the recompiler and the controller test, never the game
(DECISIONS.md, 2026-10-31: the PS5, and the PC code stripped).

The way to the console is the one the [gta4-ps5](https://github.com/chonzytron/gta4-ps5) port (GTA IV on the PS5)
takes and has shipped: Nazky's [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) brings the
[PS5 payload SDK](https://github.com/mihawk-99/PS5_PayloadSDK) fork, [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)'s
RADV (`libvulkan_radeon.ps5.a`), the app start-up code, the clean-room `libc.prx` and `ps5-native-tool` (the ELF to
`eboot.bin` converter and fake-SELF signer). This port builds on those as gta4-ps5 does, without copying them.

## Status

| Step | Status |
|---|---|
| Toolchain: the host's clang 19+ for `x86_64-sie-ps5` against the payload SDK (`ps5/cmake/ps5-toolchain.cmake`) | done: every library and object of the game compiles (clang 19.1, the SDK's v0.42 release headers and the fork's) |
| The link: the ps5-plume-triangle recipe in Python (`tools/ps5_link.py`) | done: the whole game links into a PIE with the fork's libps5platform, libc++ and the system stubs (2026-10-31, stand-in generated code and an empty RADV archive: no disc or RADV build where it was written) |
| Packaging (`tools/ps5.py package`): eboot.bin, sce_module/libc.prx, sce_sys/param.json + icon0.png, disc/ | done; checked with a stand-in for ps5-native-tool (the real one was not run where it was written), including the RELRO padding fallback |
| Platform layer (`src/platform/platform_ps5.cpp`): the TV (VK_KHR_display), the DualSense (scePad), sound (sceAudioOut), TSC frame pacing, Dear ImGui from the pad, notifications | written and compiled; not run on a console yet |
| The DualSense as the DualShock 2 (`tools/ps5.py padtest`, `ps5/tests/`) | done: 71 checks pass - the real platform layer with the console's pad functions mocked, through the real bindings to the report the game reads (every button, both sticks, the triggers, Select tap / hold, the menu chords kept from the game, free aim's hand-over, a controller gone and back, the motors, the Controller page, the menus' gamepad keys). Built on its own for the build machine (no game build). The pad's button bits are ps5-plume-triangle's, verified on a console |
| Vulkan: RADV's entry point instead of a loader (`volkInitializeCustom`) | written; not run on a console yet |
| The disc read with `pread`, not mapped (`src/disc/iso_image.cpp`) | done: byte-identical to mapped reads on a test image (every file, past-the-end reads; ASan / UBSan clean); `pread` is the only way now |
| Paths (`/app0`), the log file, start-up errors as notifications, the FP state, crash reports | written and compiled; not run on a console yet |
| FFmpeg for the movies (`tools/ps5.py ffmpeg`) | written, not run (FFmpeg's configure was not run where it was written): without it the movies are skipped |
| `tools/ps5.py compile-check` + `padtest` in CI (`.github/workflows/ps5.yml`): the PS5 build compiled without a disc | done |
| The PC code stripped (SDL3 layer, D3D11, the launcher, keyboard / mouse, window settings, PC tools; 2026-10-31) | done: compile-check links, padtest 62 / 62, the recompiler builds for the build machine |
| main merged (the game / runtime fixes of 0.0.3 - 0.2.0: free aim, above-50-fps fixes, save names and card backups, the stutter work; DECISIONS.md 2026-11-03) | done: compile-check links, padtest 71 / 71, the recompiler builds |
| `tools/ps5.py setup` (ps5-plume-triangle fetched at its pinned commit and built), `build --zip`, `release.py` (the build kit) | done; setup's clone checked, its `make` not run where it was written |
| **First run on a console** | **to do** - the checklist below |

### First run: what to look at

`/data/homebrew/<title id>/logs/vcs.log`, in order:

1. `[crash] handlers on (vcs.elf at 0x...)` - main ran and the log works. A `[main] MXCSR 0x9fe0 at start` line just
   before it means the start-up code did not set the IEEE state (it should: ps5-plume-triangle's `app_crt.cpp`; main
   sets it then).
2. The disc: `No disc image found` / `Cannot read disc image` / `SLUS_215.90 not found` (each also a PS5 notification).
3. `[platform] PS5 (TSC ... MHz)` and `[platform] DualSense of user ...` (or `no controller yet`) - the platform layer.
4. `[platform] TV mode ...` lines, then `[platform] TV: 3840x2160 at 60.00 Hz`, `[present] Vulkan on AMD Radeon
   Graphics (RADV ...)`, `[rhi] Vulkan swapchain 3840x2160, 3 images, fifo` - the picture. A failure names the missing
   piece (a Vulkan 1.3 feature, push descriptors, the display).
5. `[platform] sound: port ...` - sound.
6. Then the game's own log lines (the runtime's `[ee]`, `[gs]`, `[sound]`...).

## Building

On the build machine (Ubuntu 24.04, WSL 2 or Fedora 41+: the payload SDK's tools run there), with Python 3, Git, clang
19+, lld and llvm-config (the payload SDK's make asks it where clang is: `llvm-20` on Ubuntu, `llvm-devel` on Fedora;
`tools/ps5.py setup` passes the one next to the build's clang as `LLVM_CONFIG`), CMake 3.21+, Ninja and glslang
(`tools/ps5.py setup` lists the full package line for both when something is missing):

1. **The game's code from your disc**, once: `python3 tools/recompile.py "<your>.iso"` builds the recompiler and turns
   the disc's executable into C++ (`generated/`, about 200 MB), which the PS5 build compiles.
2. **The PS5 toolchain**, once: `python3 tools/ps5.py setup` clones ps5-plume-triangle next to this folder at the
   commit this port was checked against (`--triangle <dir>` for elsewhere; remembered) and runs its `make`: the payload
   SDK fork (`.deps/native/ps5-payload-sdk`), RADV (`.deps/native/radv-release`), the start-up code, the libSceAgc
   stubs and ps5-native-tool. It lists the packages that are missing first (meson, glslang, python3-mako...). By hand:
   `git clone https://github.com/Nazky/ps5-plume-triangle` and `make` in it.
3. `python3 tools/ps5.py check` - what is there and what is missing, with the command for each.
4. `python3 tools/ps5.py build --zip` - configures `build/ps5` (CMake + Ninja), compiles the game for the PS5 (the
   ~200 MB of generated C++: 15-30 minutes the first time), links `build/ps5/vcs.elf` and makes the app folder
   `build/ps5/app/PPSA94590/` and its zip `build/ps5/PPSA94590.zip`. `--lto` for ThinLTO (not measured on a console
   yet), `--title-id` for another id. **The app folder holds the game's code made from your disc: it is for your own
   console, never to publish** (DECISIONS.md: source only).
5. Optional, the movies: `python3 tools/ps5.py ffmpeg` (FFmpeg's MPEG-2 decoder and swscale for the PS5, from the
   pinned 7.1.1 release), then `build` again.

`python3 tools/ps5.py compile-check --sdk <sdk>` compiles the PS5 build against stand-in generated code (declarations
and empty bodies for the generated functions the host code names - no game code): a check without a disc or a
console, which CI runs with the upstream SDK release. With a built triangle (`--triangle`) it links too.

`python3 tools/ps5.py padtest` checks the controller path on the build machine: `ps5/tests/CMakeLists.txt` builds
`ps5/tests/padtest.cpp` with the real `platform_ps5.cpp` (the console's pad, user and TSC functions mocked), the real
bindings (`vcs_input.cpp`), settings and the runtime's pad backend and control channel - nothing of the game - in
`build/ps5-padtest`, then feeds DualSense states through the bindings and checks the DualShock 2 report the game reads
(71 checks; free aim's `aim::update` is a stand-in there that records what the bindings hand over).

The build machine never builds the game: `tools/recompile.py` builds only the recompiler there (`build/release`, the
top-level CMakeLists.txt without the PS5 toolchain file). `python3 tools/clean.py [--apply]` deletes what the build
folder holds besides the builds (the recompiler's scratch output, release staging, logs). The build folder can be
elsewhere: `VCS_BUILD_DIR`, or its path in `.vcs-build-dir`.

### Releases

A release carries no game code (DECISIONS.md, 2026-10-21): `python3 tools/release.py` makes
`build/dist/VCS-Recomp-<version>-ps5.zip`, the source with `PS5-README.txt` (the steps above in short), through the same
game-file guard. Players build the app from their own disc. There is no ready-to-run PS5 zip to
publish: the eboot is the game recompiled.

## Installing and playing

1. Copy `build/ps5/app/PPSA94590/` to `/data/homebrew/PPSA94590/` on the PS5: `python3 tools/ps5.py deploy --host
   <PS5 IP>` (over an FTP payload: ftpsrv on 2121, `--port 1337` for etaHEN's; it uploads only, so the disc, the saves
   and vcs.ini on the console stay), or unzip `build/ps5/PPSA94590.zip` into `/data/homebrew/` (a USB drive, any FTP
   client).
2. Your disc image (USA, SLUS-21590) in the app's `disc/` folder (`deploy --iso <file>` uploads it), or on a USB drive
   (exFAT) as `*Vice City Stories*.iso` in its top folder or `games/`. `iso=<path>` in `vcs.ini` names one anywhere.
3. Start it from the home screen (your homebrew launcher lists `/data/homebrew` apps).

To update, copy the new app folder over the old one: `disc/`, `userdata/` (the memory card) and `vcs.ini` stay.

| On the console | What |
|---|---|
| `/data/homebrew/<id>/disc/` | your .iso |
| `/data/homebrew/<id>/vcs.ini` | the settings (the settings menu writes it) |
| `/data/homebrew/<id>/userdata/mc0` | the memory card: your saves (`userdata/cache`: the boot executable from the disc) |
| `/data/homebrew/<id>/logs/vcs.log` | the last run's log (`vcs.prev.log` the one before): send it with a bug report |

## Controls

The DualSense is the DualShock 2, button for button, as the game expects it (it reads the pad in analog mode: the
buttons and both sticks; game-internals.md 12a). The defaults:

| DualSense | PS2 (DualShock 2) | In the game |
|---|---|---|
| Cross | Cross | sprint, accelerate |
| Square | Square | jump, brake / reverse |
| Triangle | Triangle | enter / leave a vehicle |
| Circle | Circle | attack |
| L1 / R1 | L1 / R1 | R1: target, handbrake |
| L2 / R2 (the triggers, past *Trigger press point*, 30%) | L2 / R2 | |
| L3 / R3 (stick clicks) | L3 / R3 | |
| Left stick / right stick | left stick / right stick | move / camera |
| D-pad | D-pad | |
| Options | Start | the pause menu |
| **Touchpad click** | **Select** | the Create button belongs to the system (apps never see it), as in gta4-ps5 |

Pad chords open the menus; the game sees neither button of a chord:

| Hold the touchpad and press | Opens |
|---|---|
| Options | the settings menu (Cross selects, Circle backs out and closes, L1 / R1 change pages) |
| R1 | the trainer (d-pad, Cross applies, Circle goes back) |
| L1 | the free camera (sticks, R2 up, L2 down, Cross faster, Circle slower, d-pad up / down zoom) |

Because a chord starts on the touchpad, Select reaches the game once the touchpad has been held 150 ms on its own, or as
a short press when it is let go sooner; while it is held, Options / L1 / R1 do not reach the game, and a hold that
pressed one of them is a chord to the end (`src/game/vcs_input.cpp`).

The Controller page of the settings rebinds every PS2 button and has the dead zones (12%), camera speed, trigger point,
swapped sticks, inverted camera axes and the vibration strength. The game's rumble drives the DualSense's motors (the
pad's dual-motor vibration mode). The controller is the one of the user who started
the game (else the user in front, any logged-in user, last the system's); one switched on later is found within a
second.

## How it works

- **Toolchain** (`ps5/cmake/ps5-toolchain.cmake`): clang 19+ (the compiler the generated code and its numerics were
  checked with) with `--target=x86_64-sie-ps5 -isysroot <sdk>`, the SDK's libc++ headers and the
  flags of the SDK's own `prospero-clang++` wrapper (`-femulated-tls -fno-plt -fno-stack-protector`), plus
  `-march=znver2` (the PS5's CPU: x86-64-v3 and more) and `-fdenormal-fp-math=ieee`. Compiler checks build static
  libraries: an executable needs the PS5 link. The recompiler is not built (it runs on the host).
- **The link** (`tools/ps5_link.py`, CMake's link rule in `ps5/cmake/ps5.cmake`): ld.lld beside the compiler with the
  flags of the SDK's `prospero-lld` (`-pie`, 16 KB pages, `-mllvm -emulated-tls`) and ps5-plume-triangle's recipe:
  its linker script and start-up objects, RADV whole, the SDK's libc++ / libc++abi / libunwind, compiler-rt's builtins,
  and the fork's `libps5platform.a`: `malloc` and its family wrapped onto a heap in direct memory (libc's own heap runs
  out long before a game does), `pthread_create` onto 2 MB stacks there (twice what the game thread is known to
  need), and 83 libc functions the console lacks or refuses mapped onto the platform's
  (`--defsym`). The response file is kept (`build/ps5/ps5-link/link.rsp`) with a link map.
- **Start-up pieces in the game** (`ps5/runtime/`): emulated TLS that runs C++ `thread_local` destructors before
  freeing a thread's TLS (compiler-rt's, as the recipe writes it, with gta4-ps5's fix; its `__cxa_thread_atexit_impl`
  is weak - newer SDK forks have their own), and `stat("")` answering ENOENT (the PS5 says EINVAL, and
  `std::filesystem` throws on that).
- **Packaging** (`tools/ps5.py package`): `ps5-native-tool link` (the ELF's layout and imports the PS5 way, module SDK
  0x02000009) and `self --sign` (fake SELF, magic 0x1D3D154F); when the converter finds no room after the RELRO for the
  process parameters, the link runs again with half a page of padding (`ps5/runtime/ps5_relro_pad.c`), as gta4-ps5
  does. `param.json` declares 60 Hz output (`attribute3` 0). The home-screen icon is drawn by the script (a sunset
  over the sea and "VCS"): no artwork of the game's.
- **The TV** (`platform_ps5.cpp`): a VK_KHR_display surface on the first display that reports modes - 4K at 60 Hz when
  the TV has it (*Display > TV output*: 1080p / 1440p / 4K), a plane that can show it, after
  `sceSystemServiceHideSplashScreen`. The swapchain and everything after it is the RHI's Vulkan backend.
- **Vulkan without a loader**: there is no `libvulkan` on the PS5. The platform layer hands the RHI RADV's
  `vk_icdGetInstanceProcAddr` (`platform::vulkanGetInstanceProcAddr`, with the core 1.1 names it may only have under
  their KHR aliases), and volk loads everything through it (`volkInitializeCustom`).
- **The DualSense** (`scePad`): the head of `ScePadData` (buttons, sticks, triggers) and the button bits as
  ps5-plume-triangle verified them on a console; a trigger reads all the way when its button bit is set, else its
  travel (plume reads both). A pad that reads all zero is taken as none (both sticks exactly in the top-left corner is
  not a real state), a failed read closes the handle and it is opened again (at most once a second). Rumble goes out at
  the next pump, renewed while it lasts, after `scePadSetVibrationMode(2)`. The pad also drives Dear ImGui's gamepad
  keys (the settings menu, the trainer). All of it is checked by `tools/ps5.py padtest`.
- **Sound** (`sceAudioOut`): the main port, 48 kHz (the runtime's rate), 256-frame buffers of 16-bit stereo, on a
  thread that `sceAudioOutOutput` paces; the runtime's ring keeps the delay (60-250 ms).
- **Time**: `clock_gettime` is a system call on the PS5, so frame pacing reads the TSC (`sceKernelReadTsc`), sleeping
  while more than 1.5 ms is left and spinning the rest.
- **The disc is read with `pread`**, not mapped: in gta4-ps5 a file mapping of a disc image froze the whole console.
  `IsoImage` keeps the descriptor and reads each request (thread-safe, no shared file position).
- **Files**: the app's folder is `/app0` (writable for an app run from `/data/homebrew`): `vcs.ini`, `userdata/`
  (`VCS_DATA_DIR`'s place), `logs/`. stdout and stderr go to `logs/vcs.log`, line-buffered, both appending (one file
  offset). Start-up failures (no disc, the wrong disc, no renderer) are also sent as a PS5 notification.
- **Floating point**: a PS5 title starts with MXCSR 0x9fe0 (flush-to-zero and denormals-are-zero); the triangle's
  start-up code sets 0x1f80 (IEEE: denormals kept, round to nearest) before main, and main checks it (the game's numerics were only ever checked in
  the IEEE state; threads start with their creator's MXCSR).
- **Crash reports** (`src/debug/crash.cpp`): SIGSEGV / SIGBUS / SIGILL / SIGFPE on an alternate stack print the fault,
  the frame-pointer chain as offsets into `vcs.elf`, and the `llvm-symbolizer -e build/ps5/vcs.elf ...` command for
  file and line. The console's ucontext has 48 bytes more than FreeBSD's before the machine context (the SDK fork's
  probe measured it): the registers are read at the measured offset, whichever headers built the game.
- **Guest functions use the platform's calling convention**: clang refuses `preserve_none` for the PS5 (and PS4)
  targets, so `PS2_GUEST_CC` is empty there (`ps2_guest_cc.h`): every guest call saves and restores the registers
  `preserve_none` spares (3-7% of game-thread time when it was measured: PERFORMANCE.md 8).
- **`thread_local` alignment is at most 32 bytes** on the PS5: the CPU GS backend's per-thread snapshot backend
  (64-byte aligned) is a `thread_local unique_ptr`.

## Work list

| Step | Status |
|---|---|
| First run on a console (the checklist above) | to do |
| FFmpeg built for the PS5 (`tools/ps5.py ffmpeg`), then the movies checked | to do |
| Frame rate on the console: game-thread time without `preserve_none`; the frame pacing's TSC; `steady_clock` (a system call) in hot paths | to do |
| ThinLTO on the console (`--lto`: lld's LTO code generation with emulated TLS) | to do |
| 120 Hz output and VRR (`param.json` attribute3 0x40 / 0x40000, as gta4-ps5 declares them) for the 120 fps cap | to do |
| DualSense extras (light bar, adaptive triggers), as gta4-ps5 has them | later |
| The debug switches (`PS2X_*` / `VCS_*`, PROGRESS.md) from a file in the app's folder: an app started from the home screen gets no environment | to do |
| The control channel on the LAN (opt-in), so `tools/gamectl.py` and the rig's commands (`src/debug/rig.cpp`) can drive the console; it injects DualShock 2 buttons (`button` / `press`) | later |
| The Vulkan pipeline cache kept between runs (`userdata/`) | later |
