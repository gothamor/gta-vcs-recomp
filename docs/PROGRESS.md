# Progress

## Phase 0 - setup (done)
- [x] Repo skeleton, GPL-3.0, `.gitignore` blocking ISOs/ELF/generated code
- [x] `tools/vcs_disc.py`: ISO 9660 reader, ELF extraction + build check
- [x] PS2Recomp vendored, local changes listed in `docs/PS2RECOMP_PATCHES.md`
- [x] Toolchain: clang-cl 23 + MSVC 14.29 STL, CMake, Ninja, JDK 21, Ghidra 12.1.4 + EE extension
- [x] Top-level CMake building tools and `vcs.exe`

## Phase 1 - analysis (done)
- [x] Headless Ghidra export (`tools/ghidra_analyze.sh`), 9,550 functions
- [x] SDK identification via ps2_analyzer, merged by `tools/make_config.py`
- [x] Recompile: 9,584 C++ files, 243 MB, 0 errors

## Phase 2 - recompile and first boot (in progress)
- [x] `vcs.exe` builds and executes the game from its entry point
- [x] Memory-mapped ISO as `cdrom0:` for EE, VFS and IOP (real LSNs)
- [x] All 9 IOP modules load from disc and run on the emulated IOP
- [x] Directory API for the boot-time disc scan
- [ ] Reach the game's main loop

## Phase 3 - graphics
## Phase 4 - audio, input, memory card, movies
## Phase 5 - in-game stability and performance
## Phase 6 - enhancements

## Build times (i7-7700K)
- Full rebuild of `vcs.exe`: ~20 min. Any change to the generated stub/function
  headers rebuilds every unity batch (~8-9 min). Runtime-only change: ~6 min (link).
