# Decisions log

Newest at the bottom. Each entry: what, why, and what would change it.

## 2026-10-21 - Base toolchain: vendored PS2Recomp
- PS2Recomp (ran-j, GPL-3.0) is vendored into `third_party/PS2Recomp` at upstream
  commit in `third_party/PS2Recomp/UPSTREAM_COMMIT`, without its `.git`,
  `android/` and `vita/` folders. Vendored (not a submodule) because we will patch
  the runtime heavily and can't publish a fork from here. Keep local changes
  small and listed in `docs/PS2RECOMP_PATCHES.md` so they can be upstreamed.
- The project is GPL-3.0 as a result.

## 2026-10-21 - Distribution: source only, bring your own disc
- The repo never contains the ISO, the ELF, or C++ generated from the ELF
  (`.gitignore` blocks them). Users run the pipeline on their own disc.
- The built exe reads game files straight from the user's ISO at runtime.
- Only one executable build is supported (SHA-1 in `tools/vcs_disc.py`).

## 2026-10-21 - Performance is a hard requirement
- Upstream runtime uses raylib + a CPU software GS renderer, which the upstream
  README calls "very bad" for speed. That's fine for bring-up only.
- Target: runs full speed on old/modest hardware (dev box: i7-7700K + GTX 1080 Ti,
  treat it as the *high* end). So:
  - GS: hardware-accelerated renderer (Vulkan/D3D), not a CPU rasterizer and
    preferably not a heavy compute rasterizer either. paraLLEl-GS is an option
    for reference accuracy, but the end state is a native renderer fed directly
    from the game's draw calls (HLE of VU1 programs).
  - VU1: translate the game's microprograms to C++ ahead of time, never interpret.
  - Disc I/O: memory-map the ISO, no per-read file opens.
  - Avoid per-instruction runtime dispatch in hot paths (e.g. indirect jumps
    through lookup tables) where we can resolve statically.

## 2026-10-21 - Function map: Ghidra functions only, no label records
- Ghidra's export script also emits every branch-target label (57,897 of them)
  as a separate "function". With those, ps2_recomp produced 67k files / 572 MB
  with each code chunk duplicated. Without them: 9,584 files / 243 MB, 0 errors.
- ps2_recomp still discovers internal entry points itself and maps each to its
  owner function (the function starts with a `switch (ctx->pc)` that jumps to the
  resume label), so jump tables and thread resumes stay inside one C++ function.
- BlackLine's archived VCS map (`gta.csv`) is for the PAL build SLES-54622, so its
  addresses don't apply to SLUS-21590. Its function count (9,549) matches ours,
  which is a good sanity check that Ghidra's boundaries are complete.

## 2026-10-21 - Compiler: clang-cl + MSVC 2019 STL
- The machine's VS 2022/2026 installs have no C++ toolset; VS 2019 Build Tools
  (MSVC 14.29) do. We build with LLVM clang-cl 23 against that STL and the
  10.0.19041 Windows SDK. clang-cl also handles the huge generated files better
  than cl.exe. `tools/toolchain.py` sets this up (vswhere + vcvars64).
- Baseline CPU: AVX2 (upstream's `/arch:AVX2`). Revisit if we want pre-Haswell
  CPUs; the generated code mostly needs SSE4.1.

## 2026-10-21 - Graphics plan
1. Bring-up: upstream CPU GS backend (slow, but no extra work).
2. Accurate + fast enough: paraLLEl-GS (Vulkan compute, LGPLv3+) for exact
   output with supersampling, as hctp-recomp does.
3. End state for performance and real hi-res/widescreen: native renderer fed by
   high-level hooks on the game's VU1 render paths (VCS has only ~4.8 KB of VU
   microcode + 4 small DVP overlays, so the set of programs is small).
- 2026-10-22: step 3 is now planned in detail in NATIVE_RENDERER.md (SDL3 platform
  layer, own RHI with a D3D11 backend, GS HW backend port, then packet-level HLE).
  Its six decisions were approved by the user on 2026-10-22.

## 2026-10-22 - Movies skipped during bring-up
- `FUN_0021CA70` (from `main`) plays `STINGS.PSS` then `INTRO.PSS` via the movie
  player at `0x40D8B8`. Under the runtime's sceMpeg stubs it stalls after ~8
  stream reads. Skipped (returns 0) in `src/game/vcs_overrides.cpp` for now.
- Plan (Phase 4): native playback - demux the PSS from the disc with FFmpeg and
  draw it ourselves (cheaper than emulating the IPU, and allows clean scaling).

## 2026-10-22 - Iteration speed
- New bindings go in `src/game/vcs_overrides.cpp` (relink only) rather than the
  recompiler config (rebuilds all generated code).
- LTO off for development builds; ThinLTO for release builds.

## 2026-10-22 - Rule for hooks on recompiled functions
- A generated function can return to the EE scheduler in the middle of its body
  (thread switch at a checkpoint, e.g. inside a `DelayThread` poll loop). It is
  later resumed through the function table at the *resume address*, which maps
  to the original generated function, not to a wrapper registered on its entry.
- So a hook may do work **before** calling the original, or replace the
  function entirely, but must not rely on code running after the original
  returns. For "after" logic, hook the next function the caller invokes.

## 2026-10-22 - paraLLEl-GS: integrated, but off by default
- Ported hctp-recomp's paraLLEl-GS backend (runtime/gs/gs_gpu*.{h,cpp}) and hooked
  it into upstream's GS frontend (raw GIF per path, register writes, IMAGE uploads,
  readbacks, present at VBlankStart, frames read back into the raylib window).
- On the dev GPU (GTX 1080 Ti, Pascal) every run hung the GPU > 2 s and Windows
  reset the display driver (TDR, event 4101); NVIDIA's OpenGL driver then
  aborts the process (fail-fast code 7 in nvoglv64.dll). Independent of the
  supersampling rate. Likely the compute rasterizer relies on behaviour Pascal
  lacks (no independent thread scheduling) or on a content-specific hang.
- Because the target is *modest* hardware, a compute-heavy rasterizer that
  doesn't run on a 2017 high-end card is the wrong default. The CPU renderer
  stays default; `PS2X_GS=gpu` opts in to paraLLEl-GS for newer GPUs.
- Next: a conventional hardware-rasterized GS backend (GSdx-HW style) behind the
  existing `GSRasterBackend` interface - broad GPU support, upscaling-friendly.

## 2026-10-22 - Performance status (i7-7700K)
- With GS work discarded (`PS2X_GS=null`) the game now runs at full speed
  (60 VBlanks/s, 15% idle) after two changes: hctp's VU1 throughput mode
  (13 -> 23 VBlanks/s) and batching IOP time (23 -> 60).
- With the CPU GS renderer it is 2.9 VBlanks/s: the rasterizer is ~95% of the
  frame. A GPU GS backend is the critical path.

## 2026-10-22 - VU1 microcode compiled ahead of time
- VCS uploads 22 distinct VU1 microcode blocks at run time (REF payloads from
  `.text`, the MPG VIFcode in the DMAtag). `game/vcs/vu1_uploads.txt` lists their
  ELF addresses; `tools/vu1_recomp.py` (vendored from BigDaddyJRich/hctp-recomp,
  GPL-3.0, plus an uploads-list input) reads the bytes from the user's ELF at
  build time and writes `generated/vu1/vu1_compiled.cpp` (never committed).
- Pieces are split at every upload boundary because VCS overlays parts of its
  big block with per-pass variants and a piece runs only while VU memory holds
  all of its bytes. Result: 99.97% of VU1 pairs run compiled.
- Verified with `PS2X_VU1_VERIFY=1`: registers, VU memory and XGKICK packets match
  the accurate interpreter; only cycle counts differ (throughput variant).
- Frame rate in game (i7-7700K): 11 -> ~35-50 VBlanks/s over this pass
  (compiled VU1, CLUT hash cache, content-keyed textures, upload fast path,
  batched timers). The EE thread is the single saturated thread. Next: VU0
  microprograms 0x0CF8 (sector box clip) and 0x0EB8 (~25% of the EE thread;
  they read MAC flags with pipeline timing, so they run on the exact path).

## 2026-10-22 - Native VU0 programs; no FP contraction
- VU0 programs 0x0CF8 (sector box clip) and 0x0EB8 (3x matrix product) ran
  ~160k times / 5 s on the interpreter's exact path (they read MAC flags):
  ~25% of the EE thread. They are now C++ (`src/game/vcs_vu0_native.cpp`),
  transcribed pair by pair with the flag latency spelled out, using the same
  per-lane maths as the compiled VU1 code. Registered by code hash.
- Verification (`PS2X_VU0_VERIFY=1`) first showed 1-ulp differences: the
  *interpreter* was wrong - /arch:AVX2 let clang fuse `acc + a * b` into FMA
  (one rounding). The PS2 rounds product and sum separately. The runtime and
  runner now build with -ffp-contract=off; after that, 4.2 million live calls
  matched exactly (VF, VI, ACC, VU0 memory), and compiled VU1 still matches.
- In game (i7-7700K): ~46-67 VBlanks/s (was 35-50 before, 11 at the start of
  the performance work). Remaining top costs: generated game code ~31%, T4
  texture swizzle (WriteP4) ~9%, DMA chain copy (memmove) ~7.5%, HW backend
  recording ~7%, scheduler checkpoints ~4%.

## 2026-10-22 - SPU2 emulated in the IOP; IOP time follows the wall clock
- The SPU2 lives in ps2xIOP (`core/iop_spu2.cpp`), behind the IOP's memory map
  and DMA channels 4/7, rather than as a high-level libsd replacement: VCS's
  CDSTREAM.IRX drives libsd directly, and its streaming depends on SPU2 IRQ
  addresses and DMA-complete callbacks, which an HLE layer would have to fake.
- The mix (48 kHz stereo, one frame per 768 IOP cycles) goes to a raylib audio
  stream through a ring buffer with an adaptive target fill (60 ms, +30 ms per
  underrun, max 250 ms). Drift is absorbed by resampling within +-0.5%. A first
  version resampled by up to +-8%: when the buffer ran low the music audibly
  slowed down, which the user heard as "audio slows down with the frame rate".
- IOP time was derived only from accounted EE cycles, so when the EE ran below
  full speed the SPU2 produced 70-90% of real-time audio and playback stuttered
  in step with the frame rate. On hardware the IOP/SPU2 keep real time whatever
  the EE does, so each IOP batch now brings IOP time up to the host clock (at
  most 50 ms at once; lag > 1 s is forgiven). Measured: 100% production and 0
  underruns at 20-60 VBlanks/s (slow host simulated with PS2X_SLOW_VBLANK_MS).
  Stream refills (DMA bytes, IRQs per second) are the same at 20 and 60
  VBlanks/s: CDSTREAM feeds music from the IOP side, not per game frame.
  `PS2X_IOP_REALTIME=0` turns it off.

## 2026-10-22 - Native port roadmap decisions (docs/NATIVE_PORT_ROADMAP.md)
The user's choices on the roadmap's open questions:
- Start with milestone 1 (automation / coverage rig), then milestone 2 (correctness sweep), and
  carry the roadmap out autonomously, committing at milestones and major changes.
- Movies: FFmpeg as the decoder.
- 60 fps: a settings-menu option, off by default (30 fps stays the authentic default).
- IOP retirement (milestone 6): do it.
- Windows only for now; milestone 9 (portability / distribution) is out of scope until further notice.
- Game knowledge goes into docs/game-internals.md (successor of ENGINE.md, each fact with how it was
  found); AGENTS.md is the working contract for agents.
