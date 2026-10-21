# Local changes to vendored PS2Recomp

Upstream: https://github.com/ran-j/PS2Recomp at the commit in
`third_party/PS2Recomp/UPSTREAM_COMMIT`. Every local change is marked in the
source with a `[gta-recomp]` comment and listed here.

| File | Change | Why |
|---|---|---|
| `CMakeLists.txt`, `ps2x*/CMakeLists.txt` | `PS2X_ROOT_DIR` instead of `CMAKE_SOURCE_DIR` | Lets our repo `add_subdirectory()` the tree |
| `ps2xRuntime/cmake/ReleaseMode.cmake` | clang-cl branch: drop `/Qspectre-`, `/GL`, `/LTCG`, `/fp:fast`; LTO only with lld-link | clang-cl rejects `/Qspectre-`; `/GL` needs MSVC's linker. `/fp:fast` dropped for clang because it lets the compiler assume no NaN/Inf and reorder, which fights the runtime's PS2 float emulation |
| `ps2xRuntime/CMakeLists.txt` | `PS2X_RUNNER_SOURCE_DIR`, `PS2X_RUNNER_EXTRA_SOURCES`, `PS2X_RUNNER_MAIN` cache vars; adds `ps2_disc_image.cpp` | Parent project supplies generated code + its own `main()` |
| `ps2xRuntime/include/runtime/ps2_disc_image.h`, `src/lib/ps2_disc_image.cpp` (new) | `IPS2DiscImage` + global setter | One disc source for every CD path |
| `ps2xRuntime/src/lib/Kernel/Stubs/Helpers/Support.h` | `registerCdFile`, `readCdSectors`, `isResolvableCdLbn`, `tryGetCdImageTotalSectors` use the disc image when set | Real on-disc LSNs (the game streams by sector), memcpy from a mapping instead of `ifstream` per read |
| `ps2xRuntime/src/lib/ps2_vfs.cpp` | `cdrom0:` open/stat served from the disc image (`DiscOpenFile`) | Same |
| `ps2xIOP/include/ps2x/iop/iop_host.h` | optional `findDiscFile` / `readDiscSectors` virtuals | IOP cdvdman (CDSTREAM.IRX etc.) sees the same disc |
| `ps2xIOP/src/emulator/imports/iop_cdvd.cpp` | `sceCdSearchFile` and `sceCdRead` try the host disc hooks first | Upstream's `searchFile` used the folder-built virtual ISO even when a real image was set, so LSNs could disagree with reads |
| `ps2xRuntime/src/lib/ps2_iop_host.{h,cpp}` | implement the hooks via `ps2_disc::image()` | |
| `ps2xRuntime/src/lib/ps2_iop_host.{h,cpp}` | `translateGuestPath` maps `cdrom0:` to a disc pseudo-path; `openHostFile`/`readHostFile`/`hostFileSize` serve it from the image | IOP loads the real IRX modules from the disc |
| `ps2xRuntime/include/ps2_call_list.h` | `sceDopen`, `sceDread`, `sceDclose` added to the stub list | Directory API (the recompiler embeds this list, so rebuild `ps2_recomp` after changing it) |
| `ps2xRuntime/src/lib/Kernel/Stubs/Dir.cpp` (new) | `sceDopen/sceDread/sceDclose` over `IPS2DiscImage::listDirectory` | VCS scans `cdrom0:` recursively at boot |
| `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp` | `PS2X_LOG_MPG` / `PS2X_DUMP_MPG_DIR` log/dump each distinct VU1 microcode upload | Find the renderer's microcode |
| `ps2xRuntime/cmake/ReleaseMode.cmake` | `PS2X_LTO=OFF|THIN|FULL` (default OFF) for clang-cl; `-fno-math-errno` | Full LTO made every relink re-codegen the whole 150 MB game; math-errno kept `roundf`/`lroundf` as libcalls in the GS rasterizer |
| `ps2xIOP/src/emulator/imports/iop_cdvd.cpp` | cdvdman imports 14 `sceCdTrayReq`, 15 `sceCdStop`, 21 `sceCdCheckCmd`, 38 `sceCdPause`, 39 `sceCdBreak` | Used by Rockstar's CDSTREAM.IRX |
| `ps2xRuntime/include/runtime/ps2_vu1.h`, `ps2_vu1_compiled.h`, `ps2_vu_code_log.h`, `ps2_host_profile.h`, `src/lib/vu/*` | **Replaced** with the VU1 subsystem from BigDaddyJRich/PS2Recomp (hctp branch, GPL-3.0): throughput ("fast") mode by default, hooks for compiled VU1 programs. `VENDETTA_*` env switches renamed `PS2X_*`; GIF PATH1 via `gsSetCurrentGifPath`. Upstream copies kept in `_ext/upstream-vu-backup` (not in repo) | Upstream's cycle-exact interpreter was 75% of the frame |
| `ps2xRuntime/src/lib/vu/ps2_vu_code_log.cpp` (new) | Minimal code-write log (always "unknown" → full re-decode) | The hctp VU1 core expects it |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | `advanceIopEeCycles` batches 4096 EE cycles before running the IOP | IOP scheduler scans ran at every guest call (34% of the frame) |
| `ps2xRuntime/src/lib/gs/gs_gpu.cpp` | `PS2X_GS=null` discards all GS work | Profiling everything but the GS |
| `ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp` | Invalidate the one-page texture cache before CLUT loads, whole-texture decodes and on uploads | CLUT loads read a stale cached page (wrong palettes: posterized colours) |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` (reset) | Interrupt / async-callback stacks carved from 0x80000-0x100000 (`kAsyncCallbackStackFloor`) | Upstream carved them from the top of RAM, over VCS's main stack (`SetupThread` at 0x1FE0000): returns to 0xF82 |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | `VCS_FPS` frame counter; `PS2X_GS=cpu\|hw\|gpu\|null` selection; GPU/HW renderer present + shutdown | Renderer selection |
| `ps2xRuntime/include/runtime/gs/gs_hw.h`, `src/lib/gs/gs_hw.cpp` (new) | OpenGL 3.3 hardware GS backend (`PS2X_GS=hw`): records on the game thread, draws in raylib's GL context; `PS2X_HW_SCALE`, `PS2X_HW_DEBUG`, `PS2X_HW_DUMP=<frame>` | Fast GS that runs on modest GPUs |
| `ps2xRuntime/include/runtime/gs/gs_gpu.h`, `gs_gpu*.cpp`, `gs_frontend.{h,cpp}`, `ps2_gif_arbiter.cpp`, `CMakeLists.txt` | paraLLEl-GS backend (from hctp), GIF path tagging, null renderer | Opt-in GPU compute GS |
| `ps2xRuntime/src/lib/gs/gs_stats.h` (new), `gs_cpu_backend.cpp` | `VCS_GS_STATS=<s>` GS state histogram; `DecodeTexture`/`ClutHash` for the HW renderer | Renderer survey |
| `ps2xRuntime/include/ps2_runtime.h` | `ps2TraceGuestWrite` write watchpoint (`VCS_WATCH`, backend in `src/debug/watch.cpp`) | Find memory corruptors |
| `ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp` | `PS2X_TRACE_RPC=<n>` | SIF RPC tracing |
| `ps2xIOP/src/emulator/imports/iop_sysmem.cpp`, `iop_kernel.h`, `iop_emulator.h`, `iop_subsystem.cpp` | IOP heap-exhaustion diagnostics, IOP thread list for `VCS_WATCHDOG` | Hang hunting |
| `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp`, `include/runtime/ps2_memory.h` | **Fix:** an IMAGE GIFtag left short by one DIRECT is finished by the data of the *next* DIRECT; a DIRECT cut off at the end of a VIF1 buffer continues explicitly (`m_vif1DirectRemainQwc`) | Upstream fed the raw VIF stream after the DIRECT to the GIF as image data, so the next DMAtag's VIFcodes (`NOP`, `DIRECTHL n`) went in as the first 8 bytes and every texture/CLUT was shifted 8 bytes (wrong palettes on everything sent as GIFtag-DIRECT + data-DIRECT) |
| `ps2xRecomp/src/lib/function_emitter.cpp`, `ps2_runtime.{h,cpp}` | Generated functions call `ps2ReportUnlistedResume` when entered mid-body at a pc without a resume label | Such an entry silently restarted the function from its prologue |
| `ps2xRuntime/src/lib/ps2_runtime.cpp` | Missing-target report lists stack slots below `$sp` holding the bad target (`stackHits=`) | Corrupted saved `$ra` hunting |
| `ps2xIOP/src/emulator/core/iop_memory.h` | IOP heap base 0x120000 -> 0x80000 (heap 832 KB -> 1.4 MB) | VCS modules end near 0x5E000; CDSTREAM/SDRDRV then failed `AllocSysMemory(0x40000)` ("Failed to allocate IOP buffer for streaming") |
| `ps2xRuntime/src/lib/ps2_pad.cpp` | Keyboard + mouse + gamepad combined; WASD/IJKL drive the analog sticks, analog triggers count as L2/R2 | GTA needs analog movement; upstream used keyboard only without a pad, and only for digital buttons |
| `ps2xIOP/src/emulator/imports/iop_sysclib.{h,cpp}` | **Fix:** real `sprintf`/`vsprintf` formatting of guest varargs (o32: a2, a3, stack from sp+16; va_list for vsprintf) | Upstream copied the format string, so CDSTREAM looked up the literal `\AUDIO\SET%s_PS.SDT;1` forever: the sound bank never loaded and the game sat on the loading screen |
| `ps2xIOP/src/emulator/imports/iop_cdvd.cpp` | Log failed `sceCdSearchFile` lookups; fix `"/\:"` separator literal | Diagnostics |
| `ps2xIOP/src/emulator/iop_emulator.cpp`, `core/iop_memory.cpp`, `imports/iop_intrman.cpp` | `PS2X_TRACE_SPU=1`: SPU2/IOP-DMA register reads/writes, interrupt registrations, IOP calls that never return; `PS2X_TRACE_IOP_DMA=1`: SPU DMA completions | Sound/IOP debugging |
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` | Log when a guest thread goes dormant | Hang hunting |
| `ps2xRuntime/src/lib/ps2_runtime.cpp`, `Kernel/EeScheduler.cpp` | **Fix:** explicit checkpoint-unwind flag (`g_checkpointUnwind`): set when `eeCheckpointDue` or the call-site check fires, cleared when the scheduler starts guest code; while set, `dispatchGuestBranch` never treats the callee as returned | Upstream treated "callee came back with pc == its entry" as a normal return. When a checkpoint preempted a *recursive* call (VCS walks frame trees: 0x1071C0 -> 0x36ED38 -> 0x1B4618 -> 0x1071C0), pc == entry too, so the outer frame continued with the inner frames still on the stack; `sp` was off by their sizes and a later return loaded `$ra` from float locals (returns to 0x3F333333 etc.) |
| `ps2xRecomp/src/lib/instruction_translator.cpp` | LQ/SQ/LQC2/SQC2 mask the low 4 address bits | R5900 behaviour (was not the cause of the crash above, but correct) |
| `ps2xRecomp/src/lib/function_emitter.cpp` | Skip the per-instruction `ctx->pc` store for pure ALU/FPU/MMI instructions | Performance (nothing can observe pc there) |
| `ps2xRuntime/include/ps2_runtime.h`, `src/debug/watch.cpp` | `VCS_WATCH=lo,hi,value,mask`: match any 32-bit lane (incl. 128-bit stores) under a mask | Catching float-like corruption |
| `ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp` | **Fix:** UNPACK V4-5 expands RGBA5551 to 8-bit positions (R<<3, G>>2, B>>7, A 0x8000>>8) | Upstream stored the raw 5/1-bit fields: vertex colours 8x too dark and alpha 1 instead of 0x80, so alpha-tested (ATST GREATER 8) props vanished |
| `ps2xRecomp/src/lib/fpu_translator.cpp` | **Fix:** `SQRT.S fd, ft` reads the ft field | On the R5900 the operand is in ft (fs is always 0 - all 694 sqrt.s in VCS); upstream read fs, so every sqrtf returned sqrt($f0) (257 sites with ft != 0) |
| `ps2xRuntime/src/lib/gs/gs_hw.cpp` | F12 (or `PS2X_HW_DUMP=<frame>`) lists the frame's draws (`[draw]` lines) and dumps render targets | Renderer debugging |
| `ps2xRuntime/src/lib/ps2_memory.cpp`, `ps2_vif1_interpreter.cpp` | `PS2X_TRACE_VIF=1` (unknown VIFcodes, scratchpad DMA), `PS2X_TRACE_DMA_RANGE=lo,hi` (chain payloads from a guest range) | Finding where geometry comes from |
