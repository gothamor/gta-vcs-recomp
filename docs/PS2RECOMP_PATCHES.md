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
