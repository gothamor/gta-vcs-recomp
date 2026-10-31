# PS5

The game as a native PS5 homebrew app, for jailbroken consoles that run homebrew from `/data/homebrew/`. It is the same
source and the same recompiled game as the Windows and Linux builds: the platform layer, the start-up code and the
link are the PS5's own, and the Vulkan renderer (the Linux one, `src/rhi/vulkan`) draws through RADV, Mesa's AMD
driver, linked into the game. No emulator, no SDL. Decisions: DECISIONS.md, 2026-10-31.

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
| Vulkan: RADV's entry point instead of a loader (`volkInitializeCustom`) | written; not run on a console yet |
| The disc read with `pread`, not mapped (`src/disc/iso_image.cpp`) | done: byte-identical to the mapped reads on a test image (every file, past-the-end reads; ASan / UBSan clean), `VCS_ISO_PREAD=1` checks it on a PC |
| Paths (`/app0`), the log file, start-up errors as notifications, the FP state, crash reports | written and compiled; not run on a console yet |
| FFmpeg for the movies (`tools/ps5.py ffmpeg`) | written, not run (FFmpeg's configure was not run where it was written): without it the movies are skipped |
| `tools/ps5.py compile-check` + CI (`.github/workflows/ps5.yml`): the PS5 build compiled without a disc | done |
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
6. Then the game as on a PC (the same log lines as docs/LINUX.md's).

## Building

On a Linux PC (Ubuntu 24.04 and WSL 2 work; the payload SDK's tools are Linux programs):

1. **The game's code from your disc**, once (as for the PC build): `python3 tools/build.py --recompile "<your>.iso"`
   (or only `python3 tools/recompile.py "<your>.iso"`). `generated/` is what the PS5 build compiles.
2. **ps5-plume-triangle**, once: `git clone https://github.com/Nazky/ps5-plume-triangle` next to this repository (or
   anywhere: `--triangle <dir>`, remembered) and `make` in it. That installs the payload SDK fork
   (`.deps/native/ps5-payload-sdk`), builds RADV (`.deps/native/radv-release`), the start-up code, the libSceAgc stubs
   and ps5-native-tool. Its README lists the packages (meson, glslang, python3-mako...).
3. `python3 tools/ps5.py check` - what is there and what is missing, with the command for each.
4. `python3 tools/ps5.py build` - configures `build/ps5` (CMake + Ninja), compiles the game for the PS5 (as long as
   a PC build: the same ~200 MB of generated C++), links `build/ps5/vcs.elf` and makes the app folder
   `build/ps5/app/PPSA94590/`. `--lto` for ThinLTO (not measured on a console yet), `--title-id` for another id.
5. Optional, the movies: `python3 tools/ps5.py ffmpeg` (FFmpeg's MPEG-2 decoder and swscale for the PS5, from the
   pinned 7.1.1 release), then `build` again.

`python3 tools/ps5.py compile-check --sdk <sdk>` compiles the PS5 build against stand-in generated code (declarations
and empty bodies for the generated functions the host code names - no game code): a check without a disc or a
console, which CI runs with the upstream SDK release. With a built triangle (`--triangle`) it links too.

## Installing and playing

1. Copy `build/ps5/app/PPSA94590/` to `/data/homebrew/PPSA94590/` on the PS5: `python3 tools/ps5.py deploy --host
   <PS5 IP>` (over an FTP payload: ftpsrv on 2121, `--port 1337` for etaHEN's; it uploads only, so the disc, the saves
   and vcs.ini on the console stay), or any FTP client / a USB drive.
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

The DualSense is the DualShock 2: Cross, Circle, Square, Triangle, L1 / R1, L2 / R2 (the triggers, pressed past
*Trigger press point*), L3 / R3, Options = Start, the sticks, the d-pad. The Create button belongs to the system (apps
never see it), so **the touchpad click is Select**, as in gta4-ps5. The PC port's pad chords open the menus:

| Hold the touchpad and press | Opens |
|---|---|
| Options | the settings menu (Circle closes it; L1 / R1 change pages) |
| R1 | the trainer |
| L1 | the free camera |

The Gamepad page of the settings rebinds every button and has the dead zones, camera speed, trigger point, swapped
sticks and inverted axes. Rumble works (the pad's dual-motor vibration mode). There is no keyboard or mouse page.

## How it works

- **Toolchain** (`ps5/cmake/ps5-toolchain.cmake`): the host's clang 19+ (the same compiler as the PC builds, so the
  same generated code and numerics) with `--target=x86_64-sie-ps5 -isysroot <sdk>`, the SDK's libc++ headers and the
  flags of the SDK's own `prospero-clang++` wrapper (`-femulated-tls -fno-plt -fno-stack-protector`), plus
  `-march=znver2` (the PS5's CPU: x86-64-v3 and more) and `-fdenormal-fp-math=ieee`. Compiler checks build static
  libraries: an executable needs the PS5 link. The recompiler is not built (it runs on the host).
- **The link** (`tools/ps5_link.py`, CMake's link rule in `ps5/cmake/ps5.cmake`): ld.lld beside the compiler with the
  flags of the SDK's `prospero-lld` (`-pie`, 16 KB pages, `-mllvm -emulated-tls`) and ps5-plume-triangle's recipe:
  its linker script and start-up objects, RADV whole, the SDK's libc++ / libc++abi / libunwind, compiler-rt's builtins,
  and the fork's `libps5platform.a`: `malloc` and its family wrapped onto a heap in direct memory (libc's own heap runs
  out long before a game does), `pthread_create` onto 2 MB stacks there (the PC's 1 MB on Windows is known to be
  enough for the game thread), and 83 libc functions the console lacks or refuses mapped onto the platform's
  (`--defsym`). The response file is kept (`build/ps5/ps5-link/link.rsp`) with a link map.
- **Start-up pieces in the game** (`ps5/runtime/`): emulated TLS that runs C++ `thread_local` destructors before
  freeing a thread's TLS (compiler-rt's, as the recipe writes it, with gta4-ps5's fix; its `__cxa_thread_atexit_impl`
  is weak - newer SDK forks have their own), and `stat("")` answering ENOENT as on Linux (the PS5 says EINVAL, and
  `std::filesystem` throws on that).
- **Packaging** (`tools/ps5.py package`): `ps5-native-tool link` (the ELF's layout and imports the PS5 way, module SDK
  0x02000009) and `self --sign` (fake SELF, magic 0x1D3D154F); when the converter finds no room after the RELRO for the
  process parameters, the link runs again with half a page of padding (`ps5/runtime/ps5_relro_pad.c`), as gta4-ps5
  does. `param.json` declares 60 Hz output (`attribute3` 0). The home-screen icon is drawn by the script (a sunset
  over the sea and "VCS"): no artwork of the game's.
- **The TV** (`platform_ps5.cpp`): a VK_KHR_display surface on the first display that reports modes - 4K at 60 Hz when
  the TV has it (*Display > TV output*: 1080p / 1440p / 4K), a plane that can show it, after
  `sceSystemServiceHideSplashScreen`. The swapchain and everything after it is the Linux renderer unchanged.
- **Vulkan without a loader**: there is no `libvulkan` on the PS5. The platform layer hands the RHI RADV's
  `vk_icdGetInstanceProcAddr` (`platform::vulkanGetInstanceProcAddr`, with the core 1.1 names it may only have under
  their KHR aliases), and volk loads everything through it (`volkInitializeCustom`).
- **The DualSense** (`scePad`): the head of `ScePadData` (buttons, sticks, triggers) as ps5-plume-triangle verified it
  on a console; a pad that reads all zero is taken as none (both sticks exactly in the top-left corner is not a real
  state). Rumble goes out at the next pump, renewed while it lasts, after `scePadSetVibrationMode(2)`. The user is the
  one who started the app. The pad also drives Dear ImGui's gamepad navigation (the settings menu, the trainer).
- **Sound** (`sceAudioOut`): the main port, 48 kHz (the runtime's rate), 256-frame buffers of 16-bit stereo, on a
  thread that `sceAudioOutOutput` paces; the runtime's ring keeps the delay as on a PC.
- **Time**: `clock_gettime` is a system call on the PS5, so frame pacing reads the TSC (`sceKernelReadTsc`), sleeping
  while more than 1.5 ms is left and spinning the rest.
- **The disc is read with `pread`**, not mapped: in gta4-ps5 a file mapping of a disc image froze the whole console.
  `IsoImage` keeps the descriptor and reads each request (thread-safe, no shared file position); `sectorPtr` is null
  in that mode (nothing uses it).
- **Files**: the app's folder is `/app0` (writable for an app run from `/data/homebrew`): `vcs.ini`, `userdata/`
  (`VCS_DATA_DIR`'s place), `logs/`. stdout and stderr go to `logs/vcs.log`, line-buffered, both appending (one file
  offset). Start-up failures (no disc, the wrong disc, no renderer) are also sent as a PS5 notification.
- **Floating point**: a PS5 title starts with MXCSR 0x9fe0 (flush-to-zero and denormals-are-zero); the triangle's
  start-up code sets 0x1f80, the PC's, before main, and main checks it (the game's numerics were only ever checked in
  the IEEE state; threads start with their creator's MXCSR).
- **Crash reports** (`src/debug/crash.cpp`): SIGSEGV / SIGBUS / SIGILL / SIGFPE on an alternate stack print the fault,
  the frame-pointer chain as offsets into `vcs.elf`, and the `llvm-symbolizer -e build/ps5/vcs.elf ...` command for
  file and line. The console's ucontext has 48 bytes more than FreeBSD's before the machine context (the SDK fork's
  probe measured it): the registers are read at the measured offset, whichever headers built the game.
- **Guest functions use the platform's calling convention**: clang refuses `preserve_none` for the PS5 (and PS4)
  targets, so `PS2_GUEST_CC` is empty there (`ps2_guest_cc.h`). That convention cost the PC 3-7% of game-thread time
  (PERFORMANCE.md 8).
- **`thread_local` alignment is at most 32 bytes** on the PS5: the CPU GS backend's per-thread snapshot backend
  (64-byte aligned) is a `thread_local unique_ptr` now, on every platform.

## Differences from the PC builds

| Area | PC | PS5 | Why |
|---|---|---|---|
| Platform layer | SDL3 (`platform_sdl3.cpp`) | `platform_ps5.cpp` on the console's libraries | No SDL backend for the PS5's display, pad and audio |
| Vulkan | volk loads the system's loader | RADV linked in, volk on its entry point | No loader on the PS5 |
| Window | resizable / full screen, placement remembered | the TV; *TV output* instead of *Window mode*; no *In the background* (the system suspends the app) | |
| Input | keyboard, mouse, any gamepad | the DualSense; touchpad = Select | Create is the system's |
| Disc | memory-mapped | `pread` | Mapping a disc image froze the console (gta4-ps5) |
| Guest calling convention | `preserve_none` | the platform's | clang refuses it for the PS5 target |
| Movies | FFmpeg (system / prebuilt) | FFmpeg built for the PS5 by `tools/ps5.py ffmpeg`, else skipped | |
| Control channel, rig, profiler | yes | not yet (the channel listens on 127.0.0.1 only; no `/proc` for the profiler) | |
| LTO | ThinLTO by default | off by default (`--lto`) | Not measured on a console yet |

## Work list

| Step | Status |
|---|---|
| First run on a console (the checklist above) | to do |
| FFmpeg built for the PS5 (`tools/ps5.py ffmpeg`), then the movies checked | to do |
| Frame rate on the console: game-thread time without `preserve_none`; the frame pacing's TSC; `steady_clock` (a system call) in hot paths | to do |
| ThinLTO on the console (`--lto`: lld's LTO code generation with emulated TLS) | to do |
| 120 Hz output and VRR (`param.json` attribute3 0x40 / 0x40000, as gta4-ps5 declares them) for the 120 fps cap | to do |
| DualSense extras (light bar, adaptive triggers), as gta4-ps5 has them | later |
| The control channel on the LAN (opt-in), so `tools/gamectl.py` and the rig can drive the console | later |
| The Vulkan pipeline cache kept between runs (`userdata/`) | later |
