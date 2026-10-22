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
- [~] Hardware GS backend (`PS2X_GS=hw`, `src/lib/gs/gs_hw.cpp`; D3D11 since stage 3 of NATIVE_RENDERER.md, was OpenGL): runs at
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
      IRQ address, AutoDMA, IOP DMA 4/7) mixed to a 48 kHz host audio stream (SDL3 since stage 1 of NATIVE_RENDERER.md). Music, SFX and
      voices confirmed by ear. IOP time follows the wall clock, so audio no longer stutters
      when the game drops below 60 VBlanks/s (0 underruns at 20-60 VBlanks/s).
- [ ] Audio: reverb, noise, pitch modulation, Gaussian interpolation, exact volume sweeps
- [x] Memory card save: the game's own save routine writes its folder, icons and slot file
      to `%LOCALAPPDATA%/VCSRecomp/mc0` (verified with `VCS_TEST_SAVE`); boot finds the save
- [ ] Memory card load: not verified - the frontend menu (Start/Load Game) does not appear
      on screen, so Load Game cannot be picked yet
- [x] Frontend menus render (pause menu, LOAD/NEW/DELETE menu when a save exists)
- [x] World vanishing when moving fast / driving: HW renderer never cleared the bottom half of
      the Z buffer (see PS2RECOMP_PATCHES.md, gs_hw.cpp depth fill)
- [x] Vic / vehicles / HUD flickering out in busy views: DMA chains were cut at 4096 tags.
      Cost: full frames are 7-14k tags; the title flyover dropped from ~45 to ~16 game fps.
      First perf recording: render submit (VIF/VU1/GS recording) 41 ms of a 61 ms frame,
      game code 15 ms, IOP 0.7 ms, GL execution 2.5 ms on the main thread.
- [~] Performance: render submit (title flyover, frames 10-60 s, ~245k vertices/frame):
      | step | render ms | game fps |
      |---|---|---|
      | start (full DMA chains) | 44.6 | 15.8 |
      | recycle HW frame records, chain buffers; perf counters per frame | 39.1 | 17.5 |
      | slice-addressed texture uploads | 39.9* | 16.9 |
      | lazy uploads (block level) | 36.8 | 18.3 |
      | texture binding cache | 33.9 | 19.3 |
      (* flyover load varies run to run; compare per-vertex.) Next: HwBackend::Submit (7%),
      VIF1 unpack (5%), compiled-VU1 pipeline bookkeeping (runLoop / progressXgkick /
      commitReadyPipelines ~8%), GS::writeRegisterPacked (4%), lazy-upload closure (3%).
- [ ] Whole screen goes black for a few frames now and then (reported 2026-10-22 after the
      DMA chain fix; not investigated yet)
- [ ] Water surface looks missing (dark grey where sea should be) - to check
- [ ] Movies
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
| `VCS_TEST_SAVE=s` | save to slot 1 through the game's code s seconds after boot |
| `VCS_TRACE_SECTORS=1`, `VCS_TRACE_CELL=1` | world sector draw / current hex cell vs camera |
| `VCS_DUMP_RAM=s,s,...` | write EE RAM to ram_<s>.bin at those seconds |
| `PS2X_DMA_HIST=1` | VIF1 payload per MB of RAM every 5 s |
| `VCS_TRACE_PACKETS=1` | peak use of the game's DMA packet buffer |
| F3 / F4, `PS2X_PERF_LOG=f.csv` | perf overlay / CSV recording (see CONTROLS.md); `python tools/perf_report.py f.csv` |
| `PS2X_HW_LAZY_UPLOADS=0`, `PS2X_HW_UPSTATS=1` | write texture uploads immediately / upload stats |
| `PS2X_SLOW_VBLANK_MS=n` | stall the EE n ms per VBlank (simulate a slow PC) |
| `PS2X_VU0_VERIFY=1`, `PS2X_VU0_NATIVE=0` | check / disable native VU0 programs |
| `PS2X_VU1_VERIFY=1`, `PS2X_VU1_COMPILED=0` | check / disable compiled VU1 code |
| `PS2X_TRACE_VU0=1`, `PS2X_TRACE_VU1=1`, `PS2X_VU1_STAT=1` | VU program statistics |
| `VCS_COUNT_CALLS=addr,...` | calls per 5 s of guest functions |
| `VCS_RWATCH=lo,hi` | which code reads a guest range |
| `PS2X_HW_DUMP=<frame>` | dump HW render targets + display as PPM |
| `python tools/run.py N name --hidden` + `python tools/gamectl.py ...` | hidden test run driven over the control channel |
| `python tools/scenes.py capture OUT` / `compare REF NEW` | reference scenes (screenshots + perf) from an empty card; diff two captures |
| `VCS_DATA_DIR=dir` | save / cache folder instead of %LOCALAPPDATA%/VCSRecomp |
| `PS2X_D3D_DEBUG=1` | D3D11 debug layer (needs Windows' Graphics Tools feature) |
| `python tools/sendkey.py Return`, `python tools/screenshot.py f.png` | key press / screenshot (control channel if open, else the window) |
| `python tools/eedis.py <elf> <addr or name>` | EE disassembly with function names |
| `python tools/minidbg.py <exe> <iso>` | catch fail-fast crashes with a stack |
