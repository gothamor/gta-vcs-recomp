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
