# Progress

## Phase 0 - setup (done)
- Repo, GPL-3.0, `.gitignore` blocking game files; `tools/vcs_disc.py`
- PS2Recomp vendored (changes in `docs/PS2RECOMP_PATCHES.md`)
- Toolchain: clang-cl 23 + MSVC 14.29 STL, CMake, Ninja, Ghidra 12.1.4 + EE ext, JDK 21

## Phase 1 - analysis (done)
- 9,550 Ghidra functions + 833 indirectly-referenced ones Ghidra missed
  (`tools/find_missing_functions.py`), SDK identification, `game/vcs/vcs.toml`

## Phase 2 - boot (done)
- ISO mapped as cdrom0: (EE, VFS, IOP), real IRX modules on the emulated IOP
- Fixed: interrupt stacks clobbering the main stack, ';1' names for the
  game's file table, cdvdman imports for CDSTREAM, many SDK stubs
- Game loop runs; title/intro 3D scene renders (CPU renderer)
- Movies skipped for now (`src/game/vcs_overrides.cpp`)

## Phase 3 - graphics / performance (in progress)
- [x] VU1: hctp's throughput-mode VU1 core (13 -> 23 VBlanks/s, GS discarded)
- [x] IOP time batching (23 -> 60 VBlanks/s, GS discarded) = full speed without GS
- [x] paraLLEl-GS integrated but opt-in: hangs Pascal GPUs into TDR resets
- [x] GS usage survey (`VCS_GS_STATS`): see docs/ENGINE.md
- [~] Hardware OpenGL GS backend (`PS2X_GS=hw`, `src/lib/gs/gs_hw.cpp`): runs at
      full speed and shows the loading screen. Fixed: overlapping render targets
      (back buffer FBP 140 = FBP 0 + 448 lines), stale page cache for CLUT loads.
- [x] Loading screens had wrong colours: VIF1 fed the next DMAtag's VIFcodes to
      the GIF as image data (8-byte shift of every texture/CLUT). Fixed.
- [x] ~25 s bad-pointer crash: gone after the IOP heap fix (CDSTREAM's 256 KB
      streaming buffer failed to allocate; the game ran an error path).
- [x] Stuck on the loading screen: IOP `sprintf` didn't format, so the sound
      bank file was never found. Fixed; the game now reaches the first in-game
      scene (Fort Baxter: Vic + army truck).
- [x] Crash in the first scene (returns to 0x3F333333-style addresses): runtime
      treated a preempted recursive call as returned (stack off by inner frames).
      Fixed; the game now plays the opening cutscene (Vic, Sgt. Martinez).
- [x] Props near-invisible: VIF UNPACK V4-5 didn't expand RGBA5551. Fixed.
- [x] Tilted horizon / broken camera maths: SQRT.S read the wrong register field
      (every sqrtf). Fixed: horizon level, palm trees and vehicles in place.
- [x] Static world (terrain, buildings) not drawn: VCALLMSR used vi[27] instead of
      CMSAR0, so the VU0 sector frustum test culled everything. Fixed; ground,
      buildings and the skyline render.
- [~] In-game speed: 11 -> ~55-60 VBlanks/s in gameplay (paced: never above 60) (compiled VU1, native VU0 box clip /
      matrix programs, texture/CLUT/upload/timer fixes, no FP contraction).
      Remaining: guest code ~31%, T4 swizzle ~9%, DMA chain copies ~7.5%
- [ ] Performance: per-instruction `ctx->pc` stores (emitter now skips them for
      pure ALU ops - needs a regenerate + test), `__restrict` for generated code

## Phase 4 - audio, input, memory card, movies
- [x] Input: keyboard + mouse + gamepad (docs/CONTROLS.md), verified in game: after the
      intro cutscene the player controls Vic at Fort Baxter (HUD, radar, mission marker)
- [x] Audio: SPU2 emulation (`ps2xIOP/.../iop_spu2.cpp`: 2x24 ADPCM voices, ADSR, loops,
      IRQ address, AutoDMA, IOP DMA 4/7) mixed to a 48 kHz raylib stream. Music, SFX and
      voices confirmed by ear. IOP time follows the wall clock, so audio no longer stutters
      when the game drops below 60 VBlanks/s (0 underruns at 35-55 VBlanks/s).
- [ ] Audio: reverb, noise, pitch modulation, Gaussian interpolation, exact volume sweeps
- [ ] Memory card, movies
## Phase 5 - stability
## Phase 6 - enhancements

## Useful switches
| Env var | Effect |
|---|---|
| `PS2X_GS=cpu\|hw\|gpu\|null` | GS renderer (default cpu) |
| `PS2X_HW_SCALE=n`, `PS2X_HW_DEBUG=1` | hardware renderer scale / stats |
| `VCS_FPS=1` | VBlanks and flips per second |
| `VCS_PROFILE=s` | sampling profiler (guest functions + host symbols) |
| `VCS_WATCHDOG=s` | EE + IOP thread states |
| `VCS_WATCH=lo,hi[,val]` | guest write watchpoint |
| `VCS_TRACE_FILES=1` | game file API / inflate / GTAG relocation trace |
| `PS2X_TRACE_RPC=n` | first n SIF RPC events |
| `PS2X_LOG_MPG=1`, `PS2X_DUMP_MPG_DIR=d` | VU1 microcode uploads |
| `VCS_GS_STATS=s` | GS state histogram |
| `VCS_SELFTEST=1` | recompiled libgcc helpers vs native |
| `VCS_TRACE_DELAY=1`, `VCS_TRACE_SOUND=1` | DelayThread calls; EE sound driver commands |
| `PS2X_TRACE_SPU=1`, `PS2X_TRACE_IOP_DMA=1` | IOP SPU2/DMA/interrupt tracing |
| `PS2X_TRACE_SPU2=1` | SPU2 key-ons, DMA bytes, IRQs, peak level per 5 s |
| `PS2X_AUDIO=0`, `PS2X_AUDIO_DUMP=f.wav`, `PS2X_AUDIO_STATS=1` | no audio output / record the SPU2 mix / underrun and fill stats |
| `PS2X_IOP_REALTIME=0` | IOP time purely from EE cycles (audio slows with the game) |
| `PS2X_VU0_VERIFY=1`, `PS2X_VU0_NATIVE=0` | check / disable native VU0 programs |
| `PS2X_VU1_VERIFY=1`, `PS2X_VU1_COMPILED=0` | check / disable compiled VU1 code |
| `PS2X_TRACE_VU0=1`, `PS2X_TRACE_VU1=1`, `PS2X_VU1_STAT=1` | VU program statistics |
| `VCS_COUNT_CALLS=addr,...` | calls per 5 s of guest functions |
| `VCS_RWATCH=lo,hi` | which code reads a guest range |
| `PS2X_HW_DUMP=<frame>` | dump HW render targets + display as PPM |
| `tools/sendkey.ps1 -Key Return` | press a key in the game window |
| `python tools/eedis.py <elf> <addr or name>` | EE disassembly with function names |
| `python tools/minidbg.py <exe> <iso>` | catch fail-fast crashes with a stack |
