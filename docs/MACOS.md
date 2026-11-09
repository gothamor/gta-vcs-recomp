# macOS

The native macOS build for Apple silicon: the same source and the same `python3 tools/build.py` pipeline as Linux
(docs/LINUX.md), built with Homebrew's LLVM, drawing with the Vulkan RHI backend on MoltenVK (Vulkan on Metal).
Decision: DECISIONS.md, 2026-11-09. Status: in bring-up (work list below).

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
- The game is `build/vcs`, its settings `build/vcs.ini`. Saves and caches go to `~/Library/Application Support/VCSRecomp`
  (`VCS_DATA_DIR` works as elsewhere).

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

## Work list

| Step | Status |
|---|---|
| Toolchain: Homebrew LLVM, `--check-deps` with the brew command, `PS2X_CPU=ARM64` | done (2026-11-09): recompiler builds on an M5 (macOS 27) |
| Runtime, IOP, platform, RHI, renderer and `src/` compile for arm64 (checked against stand-in generated code) | done |
| Window + Vulkan on MoltenVK (`vcs_window_smoke`: swapchain, present, read-back) | done: Apple M5, MoltenVK 1.4.2 |
| Recompile from the disc, link the game, boot to the title screen | to do (needs the disc) |
| The game's shaders on MoltenVK (SPIR-V -> MSL at pipeline creation) | to do |
| Bit-exact checks on ARM: `PS2X_HLE_VERIFY=1`, `PS2X_VU0_VERIFY=1`, `--iop-tests` | to do |
| Retina: the window at the display's pixel density (`SDL_WINDOW_HIGH_PIXEL_DENSITY`) | to do: drawn at 1x and scaled by macOS today |
| Sampling profiler on macOS (`thread_get_state`) | to do |
| The launcher (Cocoa window, Metal or Vulkan) and an .app bundle with the loader + MoltenVK inside | to do |
