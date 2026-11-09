# macOS

The native macOS build for Apple silicon: the same source and the same `python3 tools/build.py` pipeline as Linux
(docs/LINUX.md), built with Homebrew's LLVM, drawing with the Vulkan RHI backend on MoltenVK (Vulkan on Metal).
Decision: DECISIONS.md, 2026-11-09. Status: boots and plays the title flyover; bit-exact checks match x86 (work list below).

## Building

```
brew install llvm lld cmake ninja pkgconf glslang ffmpeg molten-vk vulkan-loader vulkan-headers
python3 tools/build.py --check-deps                    # what is missing, and the brew command that installs it
python3 tools/build.py --recompile "<your disc>.iso"   # disc -> generated C++ -> build/vcs
python3 tools/build.py --quick                         # day-to-day rebuilds (no LTO)
python3 tools/run.py 0 play                            # play (window; build/run/play.log)
```

- **Xcode or its command line tools** are needed for the macOS SDK (`xcode-select --install`), but not Xcode's clang:
  `tools/toolchain.py` puts Homebrew's `llvm` and `lld` first on the build's PATH (`VCS_LLVM=<bin folder>` takes
  another LLVM) and refuses Apple's clang, so the generated code is compiled by the same compiler as on Windows and
  Linux. The game is linked with `ld64.lld` (ThinLTO, as elsewhere).
- **The CPU level is `ARM64`** (`PS2X_CPU`, `-mcpu=apple-m1`): the runtime's x86 intrinsics go through sse2neon
  (fetched by CMake), and code that includes `<immintrin.h>` directly gets it through the one-line headers in
  `third_party/PS2Recomp/ps2xRuntime/include/arm_x86_shim`. `-ffp-contract=off` matters even more than on x86: ARM
  compilers fuse `a + b * c` by default.
- SDL3 is built from source as elsewhere; on macOS it needs nothing from Homebrew (Cocoa, Core Audio, GameController
  are the system's). Its Objective-C is compiled with `-fno-objc-msgsend-class-selector-stubs`: clang 23's
  `objc_msgSendClass$` stubs are made only by Apple's linker, and ld64.lld leaves them undefined.
- **Static libraries are archived with `llvm-ar`** (CMakeLists.txt, and `tools/toolchain.py` for new trees): Xcode's
  `/usr/bin/ar` silently drops bitcode from Homebrew's newer LLVM, so a ThinLTO build's `libps2_runtime.a` held 3 of
  60 objects and the link failed with "undefined symbol" for the whole runtime. (SDL3's archive still uses Apple's: it
  isn't LTO-compiled.)
- The game is `build/vcs`, its settings `build/vcs.ini`. Saves and caches go to `~/Library/Application Support/VCSRecomp`
  (`VCS_DATA_DIR` works as elsewhere).

## The app

`python3 tools/macos_app.py` makes `build/Vice City Stories.app` and copies it to `~/Applications` (Launchpad,
Spotlight, the Dock); `tools/build.py` makes it again after every successful macOS build (`VCS_NO_APP=1` skips that).
The app holds no game: `Contents/MacOS/vcs` links to `build/vcs`, started by `Contents/MacOS/vcs-launch` (a few lines
of shell: the log to `~/Library/Logs/VCSRecomp/play.log`, the previous one as `play.prev.log`, then `exec`). Started
through the link, macOS takes the game for the app (`lsappinfo`: "Vice City Stories", `local.vcs-recomp.game`), while
the game still finds `vcs.ini` and the disc next to `build/vcs` (`exeDirectory` resolves the link). The games category
turns on Game Mode in full screen. A missing build shows a dialog. The icon is the launcher's
(`launcher/make_icon.py`), 824 px on a 1024 px canvas, made into an .icns with `sips` / `iconutil` once
(`build/macos-icon`). `--check` says whether the installed app starts this build. It needs Homebrew's Vulkan loader
and FFmpeg at run time: an app that carries them (for other Macs) is on the work list.

## Running

- **Vulkan**: `[rhi] Vulkan loader /opt/homebrew/lib/libvulkan.1.dylib` and `Vulkan on Apple M5 (MoltenVK 1.4.2)` in the
  log. macOS has no system Vulkan loader and `dlopen` doesn't look in Homebrew's lib folder, so the loader is found by
  full path (`src/util/vulkan_loader_path.h`: next to the game, an .app's Frameworks, `$HOMEBREW_PREFIX/lib`,
  `/opt/homebrew/lib`, `/usr/local/lib`; `VCS_VULKAN_LIBRARY=<path>` chooses) - for SDL's window (its Vulkan library
  hint) and for volk. MoltenVK is a portability driver: the instance asks for `VK_KHR_portability_enumeration` and the
  device enables `VK_KHR_portability_subset` when listed.
- **Without the game**: `vcs_window_smoke` (`tests/window_smoke`, not built by default:
  `ninja -C build/release vcs_window_smoke` once a build tree exists) opens the game's window through the platform
  layer, makes the RHI device, presents 180 frames and reads an off-screen target back. Exit status 0 when all of it
  worked.

## Differences from Linux, and why

| Area | Linux | macOS | Why |
|---|---|---|---|
| Compiler | the distro's clang 19+ | Homebrew's clang (refuses Xcode's) + ld64.lld | The same compiler as Windows / Linux for the generated code's numerics |
| CPU | x86-64 (`AVX2` / `AVX` / `SSE4`) | arm64 (`ARM64`), x86 intrinsics through sse2neon | Apple silicon |
| Vulkan | the system's loader + Mesa / NVIDIA | Homebrew's loader + MoltenVK, found by full path; portability extensions | No Vulkan on macOS itself |
| Crash handler | signals, `uc_mcontext.gregs[REG_RIP]`, `gettid` | the same, PC from the Mach thread state, `pthread_threadid_np` | Different `ucontext` layout and thread ids |
| Sampling profiler (`VCS_PROFILE`) | SIGPROF through `tgkill` | not yet (a stub) | macOS has no `tgkill` or `/proc/self/task`: needs `thread_get_state` |
| Write watch | userfaultfd, opt-in | none (the default path) | No userfaultfd |
| Card creation time | `statx` birth time | `stat`'s `st_birthtimespec` | |
| Exe folder | `/proc/self/exe` | `_NSGetExecutablePath` | |
| Window | at the window's size | `SDL_WINDOW_HIGH_PIXEL_DENSITY`: Retina pixels | macOS scales a 1x drawable up (soft) |

## Work list

| Step | Status |
|---|---|
| Toolchain: Homebrew LLVM, `--check-deps` with the brew command, `PS2X_CPU=ARM64` | done (2026-11-09): recompiler builds on an M5 (macOS 27); quick and ThinLTO builds link |
| Performance: `tools/bench.py`, the uncapped frame rate, `cruise.py` | to do. ThinLTO flyover at the original 30 fps: game thread 6.5-7 ms, worker 2 ms, GS 1.6 ms (a glance, not a benchmark) |
| Runtime, IOP, platform, RHI, renderer and `src/` compile for arm64 (checked against stand-in generated code) | done |
| Window + Vulkan on MoltenVK (`vcs_window_smoke`: swapchain, present, read-back) | done: Apple M5, MoltenVK 1.4.2 |
| Recompile from the disc, link the game, boot to the title screen | done (2026-11-09): 10,386 generated files, quick build links in minutes; flyover at 30 fps (original timing), game thread ~4 ms, no crashes; hidden and windowed runs, the control channel and screenshots work |
| The game's shaders on MoltenVK (SPIR-V -> MSL at pipeline creation) | done: the flyover draws right (sky, world, cars, post effects); validation layer run to do |
| Bit-exact checks on ARM: `PS2X_HLE_VERIFY=1`, `PS2X_VU0_VERIFY=1`, `--iop-tests` | done (90 s flyover): VU0 every program 0 differed (millions of runs); setup programs, re-draws, C++ clipper, env pass exact; world fast path 11 within 1 / 14 off of 2.0 M - the same as Windows' llvm-mingw reference (11 / 14, docs/LINUX.md); SPU2 tests 0 failures. sse2neon's `_mm_setcsr` rounding and `-ffp-contract=off` give x86's numbers |
| Retina: the window at the display's pixel density (`SDL_WINDOW_HIGH_PIXEL_DENSITY`, macOS only) | done: a 640x448-point window is a 1280x896 swapchain; the mouse was already in drawable pixels |
| Sampling profiler on macOS (`thread_get_state`) | to do |
| An app for the Dock / Launchpad (`tools/macos_app.py`) | done (2026-11-09): starts this build, its own name and icon, log in ~/Library/Logs |
| A self-contained app (the Vulkan loader, MoltenVK and FFmpeg inside: no Homebrew to play) and the launcher (Cocoa) | to do |
