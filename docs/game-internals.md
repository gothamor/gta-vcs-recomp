# Game internals - GTA: Vice City Stories (PS2, SLUS-21590)

Everything known about how the game works inside, gathered while recompiling it. Addresses are EE
RAM addresses in SLUS_215.90 (identical to ELF virtual addresses; see [DISC.md](DISC.md)). Names in
`CamelCase::Like()` are our labels, modelled on the GTA III-era naming (re3 / reVC) where the code is
clearly the same lineage; they are guesses until marked **confirmed**.

Each fact says how it was established, so it can be re-checked. Tools: `tools/eedis.py` (EE
disassembly with names from `game/vcs/functions.csv`), `tools/vudis.py` (VU microcode), Ghidra
(`ghidra/`), the runtime's traces (`PS2X_*`, `VCS_*` switches in PROGRESS.md), the control channel
(`tools/gamectl.py`) and frame dumps (`PS2X_HW_DUMP`). Listings derived from game code go to
`build/analysis/` and are never committed.

**When you learn something about the game, add it here in the same change.**

---

## 1. Background (outside sources)

- Rockstar Leeds engine, shared with the PSP original (2006). The PS2 port (2007) reuses PSP-side
  data formats (`.PSP` frontend files).
- Descends from the GTA III / VC codebase but renders through Leeds' own "RenderWare Stories"-style
  layer (RSL), not stock RenderWare.
- Runs at 30 fps with a frame limiter. GTA III-era code scales movement by `CTimer::ms_fTimeStep`
  (1.0 = 1/50 s), so 60 fps mostly works, but earlier games had known 60 fps bugs (physics, some
  animations, script timers, water cannon). The PSP recomp (OverkillLabs3) and ThirteenAG's PPSSPP
  fix both patch these for 60 fps - references for milestone 4 of NATIVE_PORT_ROADMAP.md.

## 2. Code map

- 9,550 functions (Ghidra 12.1.4 + EE extension) plus 833 indirectly referenced ones Ghidra missed
  (`tools/find_missing_functions.py`). 8,521 below `0x443000` are game code; 1,029 from `0x443000`
  up are Sony SDK / libc / libgcc.
- SDK libraries, identified by ps2_analyzer's SCE symbol DB (`game/vcs/vcs.toml`):
  - `0x4431F8..` libgcc (`__divdi3`, `__muldi3`, soft-double `dpadd`...)
  - `0x447670..` libgraph (`sceGsResetGraph`, `sceGsSwapDBuff`, `sceGsSyncV`)
  - `0x448688..` libdma (`sceDmaSend`...)
  - `0x448D98..` libpad (`scePadRead`... - libpad, not libpad2)
  - `0x44E5F8..` libcdvd incl. **streaming** `sceCdStInit/StStart/StRead/StStop`
  - `0x44F668..` libmc (`sceMcOpen/Read/Write/GetDir`...; stubs: `0x450120` = `sceMcGetDir`,
    `0x44fe80` = `sceMcSync`)
  - `0x452320..` libmpeg (`sceMpegCreate`, `sceMpegDemuxPssRing`) + libipu
  - `0x45C1D8..` libsdr (`sceSdRemoteInit`, `sceSdRemote`) - audio goes over SIF RPC to SDRDRV.IRX
  - `0x467xxx` kernel syscall wrappers, `0x469E00..` libsif / RPC, `0x46B980..` libfileio
  - `0x46EA98..` Deci2 (dev-kit debug channel; dead on retail)
- `fiprintf` at `0x1A9EF0` lives inside game code (newlib linked early). `FUN_0045f378` = `sprintf`
  (seen building memory card paths).
- Largest game functions (state machines / script interpreter candidates): `0x141FE8` (41.7 KB),
  `0x13A0A0` (25 KB), `0x337208` (25 KB), `0x33F010` (20 KB).
- The analyzer found only 2 jump tables; the recompiler reports ~1,150 unresolved `jr` sites (switch
  tables) and falls back to dispatch through the function table. The memory card state machine
  `FUN_002788e0` is one (`jr` through the table at `0x4B0400`, indexed by the state at `0x487730`).

- Small hot leaf functions seen in game-thread profiles (2026-10-25): **FUN_003F9E50** = 4x4 identity matrix at $a0
  (zeroes 16 words in a loop, then 1.0 at +0, +0x14, +0x28, +0x3C; native since 2026-10-25); **FUN_00408058** = rand()
  (Park-Miller: seed at 0x48A5B8, q 127773, a 16807, r 2836; `div` then two `mult`, + 0x7FFFFFFF when <= 0);
  **FUN_002560D8** = the script VM's command dispatch (reads the 16-bit opcode at the script's ip, bit 15 = negate the
  result, handler table at 0x4BEAF0, 8 bytes per entry).

## 3. Boot

- Entry `0x001DD6A0` is standard crt0: zero GPRs / FPRs, clear .bss (`0x4CD100..0x74DF54`), set
  gp / sp, SDK init, `main`.
- `sceSifInitRpc`, then IOP modules from the disc in this order: `SIO2MAN, LIBSD, SDRDRV, CDVDSTM,
  CDSTREAM, PADMAN, SIO2MAN (again), MCMAN, MCSERV`. All nine run as real IRX code on the emulated IOP
  (SDRDRV prints "SDR driver version 4.0.1"); `SDRDRV`, `CDVDSTM`, `MCMAN`, `MCSERV` stay resident.
- `_lf_bind` (`0x46CD10`) polls for the ROM LOADFILE server - absent in the recomp, stubbed.
- **Disc indexing**: `FUN_002209F0(path)` walks `cdrom0:` recursively with
  `sceDopen/sceDread/sceDclose`, and for each file (`st_mode & 0xF000 == 0x2000`) calls the
  `sceCdSearchFile`-like `0x44E480` and appends (name ptr, lsn, size) to a table at `0x5038A8`
  (count at `0x486E8C`, string pool cursor at `0x486E88`). Every file is resolved to an LSN up
  front; reads are by sector afterwards.
- **Movies** (milestone 5, 2026-10-24): `FUN_0021CA70` (from `main`) initialises the player (FUN_0040D7D8), calls
  **CMoviePlayer::Play(path, 0, n) at 0x40D8B8** for `cdrom0:\MOVIES\STINGS.PSS;1` (0x4AC5F8, the Rockstar logos,
  n = 1,000,000) and `cdrom0:\MOVIES\INTRO.PSS;1` (0x4AC618, n = 10), shuts it down (FUN_0040D898) and clears
  the scratchpad. Play -> FUN_0040D930 (libmpeg + IPU, sound through the IOP) compares a frame count with n before
  it reads the pad: the logos can't be skipped, the intro can after 10 frames. `INTROP` / `STINGSP` are the PAL
  versions (unused by the USA game).
- **PSS layout** (read from the files): an MPEG-2 program stream; video in stream 0xE0 (MPEG-2, 512x448, 29.97 fps,
  shown across a 4:3 TV; STINGS 10.6 s, INTRO 84 s); sound in private stream 1 (0xBD), sub-stream 0xFF, each
  packet's payload behind a 4-byte header (`ff a0 00 00`) and split at odd byte counts. The joined bytes are a Sony
  "SShd" header (u32 size 0x18, type 1 = **16-bit little-endian PCM**, 48000 Hz, 2 channels, interleave 512 bytes),
  "SSbd" + u32 data size, then blocks of 256 left samples, then 256 right (byte order and interleave told apart by
  how smooth the samples are). First PTS: sound 0.043 s, video 0.110 s. FFmpeg's demuxer finds the video only.
- **Played natively** (`src/game/vcs_movie.cpp`): Play is replaced; the file read from the disc, demuxed, the video
  decoded with FFmpeg and shown by the presenter at 4:3 (`ps2ShowMovieFrame`), the sound mixed into the audio
  output (`ps2AudioMoviePush`), whose played-sample count is the clock (the host clock without audio output, and
  after the sound ends - the last pictures are due 30 ms after it). Start / Cross skip once the game's count
  allows. Display > Skip intro movies skips both. The game thread waits in Play, as in the game's player.

## 4. Frame and timing

- The game updates and draws at 30 fps on the 60 Hz VBlank: in a frame dump the 3D is in every
  other VBlank's GS frame; the in-between VBlanks carry no draws (found when `PS2X_HW_DUMP` kept
  landing on empty frames, 2026-10-22; the dump now waits for a frame with draws).
- **Main loop** (2026-10-23, from the guest-stack return addresses of the 2D work and the callers in the generated
  code): `entry` (0x1dd6a0) -> FUN_0021c928 -> FUN_0021c5e8 (start-up: init calls, then the loop) -> per frame
  FUN_0021ed88: CTimer / game updates (FUN_002f4858 / FUN_002f4960 / FUN_0021e720 ...), FUN_0021e950 (render: 3D,
  then the 2D pass FUN_0021f348, the frontend FUN_0021f7d8), then the frame's end.
- **VBlank**: FUN_00370480 creates a semaphore (id at 0x488E9C) and installs **FUN_00370300** as the INTC handler for
  VBlank start (cause 2) and end (3), through FUN_002f46f8 (AddIntcHandler + EnableIntc with a use count). On
  VBlank start it sets a flag (0x6F4268 = 1), counts VBlanks (0x488EA0) and calls up to 8 callbacks registered with
  FUN_003703f0(fn, arg) (table 0x6F4248, args 0x7490B0); on VBlank end it clears the flag. FUN_003703a8 returns the
  count, **FUN_003703b8 = WaitVBlank** (spins on the flag, clears it).
- **Frame limiter**: the graphics object at 0x520F90 counts VBlanks at +0x4 (callback FUN_00317aa8 ->
  FUN_00317ac8: +1, registered by FUN_00317788). At the frame's end **FUN_003179a0** (from FUN_0018f450) waits for
  the previous frame's work, kicks the next display list (FUN_00272268), then `do { WaitVBlank(); } while (vblanks
  < 2);` (0x317a48..0x317a58) and resets the count: **30 fps**, two VBlanks a frame.
- **CTimer** (read from the generated code of Update and Initialise, 2026-10-23; names as in reVC, whose
  CTimer::Update has the same shape): FUN_002f4858 = Initialise (registers VBlank callback 0x2f4840, which only
  counts VBlanks at 0x488290; step = 1.0, time scale 0x4CD168 = 1.0, pauses / times / frame counter = 0).
  **FUN_002f4960 = Update**:
  - Ticks: **FUN_00451C00(timer id 0x488288)** = a kernel timer's 64-bit count at the bus clock (147.456 MHz),
    only ever called from here; previous count at 0x488298, this frame's ticks at 0x4CD564. FUN_002f47c0 creates
    the timer when missing (called twice per Update); FUN_0030c984 = `cvt.w.s` (float -> int, truncating).
  - Clocks: 0x488280 (ms, also while paused) += ticks / 147456; `m_snPreviousTimeInMilliseconds` 0x4CD16C;
    **`m_snTimeInMilliseconds` 0x4CD104** and 0x4CD448 += (int)(ticks * scale / 147456) - truncated per frame.
  - Paused (`m_UserPause` 0x4CD2AC or `m_CodePause` 0x4CD11C): the step goes to 0x4CD174 (pause-mode step) and the
    frame's ticks are zeroed, so the game clocks stand still.
  - **`ms_fTimeStep` 0x4CD110** = ticks * scale * 50 / 147456000 (1.0 = 1/50 s; 0x34B60B61 = 3.3908e-7), then
    **at least 0.5** (1/100 s; VC had 0.01) unless paused or a chase recording (0x481CF8) is on;
    `ms_fTimeStepNonClipped` 0x4CD300 = that; then unless replaying (0x4CD444 == 2) **at most 3.333** (1/15 s) and
    the ms clock at most +60 per frame. Chase recording mode (0x4CD138 == 1): step 1.0, clock +16 ms.
  - **`m_FrameCounter` 0x4CD124** += 1.
  - (The earlier note "time step at 0x4CD174" was wrong: that is the pause-mode step.)
- **Frame-rate dependence found in CTimer itself**: the ms clock adds each frame's milliseconds truncated, losing up to
  1 ms a frame - game time (script waits, timers, the in-game clock) ran 1.5% slow at 30 fps and ~4% at 80 fps
  (measured: game clock vs wall clock, `m_snTimeInMilliseconds`). And the 0.5 minimum step makes the game run fast
  above 100 fps. Independent timing feeds whole milliseconds of ticks and lifts the minimum (below).
- **Display flip FUN_003706c8** (right after the limiter's wait): toggles the buffer index (+0x44 of the struct at
  *0x488E88), reads CSR, writes PMODE, SMODE2, DISPLAY1/2, DISPFB1/2. The limiter's first loop waits VBlanks until
  the previous display list is done (+0xC of 0x520F90 back to -1; set by the DMA end); with our DMA completing at
  once it does not loop. In gameplay the limiter is the only caller of WaitVBlank (counted by return address).
- **Independent timing** (`src/game/vcs_framerate.cpp`, Display > Game timing): FUN_00451C00 returns the host clock
  in whole milliseconds of bus ticks (`VCS_TIMESTEP=<fps>`: exactly 1/N s per frame, unpaced); the second
  FUN_002f47c0 call (ra 0x2F4C04) puts back a step the 0.5 minimum raised (floor 0.05); the limiter's WaitVBlank
  (ra 0x317A50) presents the frame, paces to the cap on the host clock, brings the EE clock up to real time on the
  VBlank chain (`EeScheduler::syncToRealTime`) and sets the VBlank count to 2. Measured: game clock = wall clock
  (x1.001) at 30 and 144; steps 1.65-1.70 at 30, 0.55-0.60 at ~85 fps.
- **Steps below 0.5 break the game** (frame-rate audit `tools/framerate_audit.py`, 2026-10-24): with the minimum lifted
  the player runs at 4.7 m/s up to 1/100 s steps, then 1.4 m/s at 1/110, 1.0 at 1/120, 0.5 at 1/240 (per-frame
  movement shrinks with the step squared); with the game's own 0.5 floor at 1/110 he runs 5.15 m/s (= 4.7 x 0.5 /
  0.4545, the step just too long). Code was written for steps >= 0.5 (VC's minimum was 0.01; VCS raised it) - not
  found yet. Independent timing therefore kept the 0.5 floor and capped at 100 fps until 2026-10-25 (below).
- **Found (2026-10-25): FUN_001f1d38 (ped animation movement)** turns the frame's animation root displacement into the
  ped's target velocity at +0x794 / +0x798 (FUN_001f25f0 then eases the move speed +0x140 towards it):
  `if (ms_fTimeStep < 0.5) v *= 0.5; else v *= 1.0 / ms_fTimeStep;` (0x1f2154 - 0x1f21c0, the 0.5 loaded once by
  `lui $at, 0x3F00` at 0x1f2158). The `< 0.5` branch never runs on the PS2 and is wrong - `* 0.5` where `/ step` is
  2 or more - so below the floor the ped got ~a quarter of its velocity. At 1/110 s (`VCS_TIMESTEP` feeds 9 / 10 ms
  frames, steps 0.45 / 0.5) the speed alternated 0.021 / 0.083 units - found with a write watch on the player's move
  speed (`VCS_WATCH`, writers by return address), then on +0x794. Patched (game/vcs/overrides.toml: the constant
  becomes 0.001, so every real step takes the `/ step` path); steps >= 0.5 are unchanged. Audit afterwards: run /
  drive / fall at 1/144 - 1/240 as at 1/30 - 1/100, sprint -5% / -11% at 1/200 / 1/240. Independent timing's ceiling
  is now 240 fps (step floor 50/240, `vcs_framerate.cpp` kMaxFps).
- **Audit results in the supported range** (`tools/framerate_audit.py`, steps 1/15 .. 1/100, per simulated second,
  2026-10-24): game clock exact (1/15 gets 60 ms a frame: the +60 ms cap, below ~17 fps the game slows, as on the
  PS2); car acceleration the same within 3% (speed at 2 / 3 / 4 s, distance in 4 s); running / sprinting the same
  from 1/30 up (1/15 runs ~10% faster); **falling and jumping depend on the frame rate**: 15 m of fall takes 0.90 /
  1.11 / 1.19 / 1.20 s at 1/15 / 30 / 60 / 100, a jump rises 0.73 / 0.81 / 0.86 / 0.88 m. **This is the
  engine's integration, not a per-frame bug**: the jump heights follow explicit Euler's error exactly - a peak
  v0^2/2g - v0*dt/2 with a continuous peak of 0.893 m predicts 0.726 / 0.81 / 0.85 / 0.868 (measured 0.725 / 0.814 /
  0.864 / 0.884) - and the ped's vertical speed agrees within 1-3% between 1/30 and 1/60 through a whole fall,
  quadratic air drag included (traced at ped+0x148). Shorter frames are closer to the continuous motion, as on the
  PC GTAs; matching 1/30 exactly would need a correction term in the integration (not done: <= 6%).
- **A `max(step, 0.5)` in ped code**: FUN_00337208 (25 KB, ped processing) divides by `max(ms_fTimeStep, 0.5) *
  0.008` (GRAVITY) at 0x33bb2c - one of the places that assume the 0.5 minimum (found by searching the gravity
  constant 0x3C03126F near the move speed's z, +0x14C).
- **Physics read for the audit**: ped -> its vehicle at ped+0x450 (also +0x480 / +0x484; found by scanning the ped for
  the address of the car it was warped into); **CPhysical move speed (x, y, z) at +0x140** (units per 1/50 s, x 50 =
  m/s; z found at +0x148 mid-fall, y at +0x144 in a car driving north); written through a vector setter
  FUN_00356288 / FUN_003562c0 (write watch on +0x148: callers FUN_001f93b0, 00232de8, 001f25f0 (peds),
  00348128 / 00348538 / 00348628); FUN_00348098 = ApplyMoveSpeed (position += move speed * step). Terminal fall
  speed ~1.0 units (50 m/s) after 3.5 s. A new car (`CREATE_CAR`) faces north; its matrix can be poked (right
  +0x00, forward +0x10, up +0x20, position +0x30).
- **Frame counter readers** (`m_FrameCounter` 0x4CD124; the generated code has constant addresses folded, so a text
  search of generated/ee lists them - 52 functions; the time step 0x4CD110 has 93, the ms clock 0x4CD104 198):
  - work spread over frames, `(entity seed (+0x48 >> 54) + frame) & mask` or `frame & mask` with masks 1-0xFF:
    FUN_0011c958, 00138b68, 00140b28, 00186aa8, 001effb0, 0020db30, 00203240, 00238bb0, 00243730, 0025bba8,
    00267928, 00268048, 002c3e48, 0033d5a0, 00378dd8, 00421320 ... - run more often per second at higher rates;
    harmless where the work itself scales by the step;
  - blinking, `frame & 8 / 4 / 2` toggles: FUN_0031e3f8, 0031fce8, 00321500 (& 8), 003209d0, 00320f28 (& 4) -
    right after CHud::Draw (0x31c628), reVC's CHud flashing (a flashed item on `& 8`, health under 10 on `& 4`);
    001419d0, 0033dca0, 003f29d8 (& 2), 002af6e0 (& 8) elsewhere. They flashed twice as fast at 60 fps. **Fixed**
    for the 2D pass (radar, HUD, text: FUN_0021f348): above 30 fps it runs on a counter whose low 6 bits advance
    30 times a game second (from the game clock), put back as CTimer::Update starts (`vcs_framerate.cpp`
    twoDPass / timerUpdate) - the value stays within 63 of the real counter for anything else reading it then.
    The four outside the 2D pass are not covered yet;
  - timestamps (stored, compared with frame - 1: "seen last frame"): FUN_00383028 .. 003868c0, 003ac9c8 - harmless.
- **Emulated time and the VBlank chain** (found when independent timing first ran at 5 fps): VBlank events are chained
  in EE cycles (+1/60 s each) and each is also held to its host-clock moment. An EE clock moved past the chain makes
  every following VBlank due at once by cycles, and the EE is held at each until its real-time moment - the game
  crawls. Topping the clock up must therefore aim at the chain's real-time position, not at elapsed host time.
- **60 fps, original timing** (`src/game/vcs_framerate.cpp`): the VBlank-counting callback adds 2 instead of 1, so the limiter lets a
  frame through after one VBlank. Measured outside the barracks (2026-10-23, i7-7700K): 31-37 fps - a gameplay
  frame costs ~27 ms on the game thread (game code 19-24 ms, render submission ~8 ms), more than 1/60 s; the
  emulated VBlank keeps real-time pace only while the game thread keeps up (events need both the EE cycle and the
  host deadline). A steady 60 needs ~40% less game-thread time per frame (milestone 7).
- **What the game watches of the GS / VIF1** (counted by the render worker's fences, `PS2X_RENDER_THREAD_LOG=1`,
  2026-10-23): in gameplay the GS privileged CSR / SIGLBLID ~5 reads a frame (300/s at 60 fps); GIF and VIF1
  registers only a few times at boot; never VU1 memory or the VIF1 / GIF FIFOs. The work it hands over is ~3 items a
  frame: the frame's VIF1 display list (one chain, FUN_00272268 above), one or two GIF chains (texture uploads), then
  the VBlank. It never enables the VU1 D / T stop bits (FBRST). So the GS and VU1 can run behind the game thread
  as long as the CSR reads see the finished work (runtime/ps2_render_worker.h).

## 5. Camera, projection and widescreen

Found in the widescreen spike (SETTINGS_MENU_WIDESCREEN.md), by forcing the preference and logging
the globals each frame:

| What | Where | Notes |
|---|---|---|
| FOV global | `0x487484` (float, degrees) | written by `FUN_002653e0` (a `CDraw::SetFOV`); 45 in the frontend, 120 in the flyover, 70 in game |
| Aspect global | `0x487488` (float) | written only by `FUN_002653f0` (`CDraw::CalculateAspectRatio`) |
| Aspect choice | `FUN_002653f0` | widescreen preference on: 16/9, or 5/3 when `[0x6f4d14]` is set; off: 4/3, or 1.25 when set. `[0x6f4d14]` is 0 in the frontend and 1 in game (not a PAL flag) |
| Widescreen preference | `*(int *)(*(0x48F3D0) + 0x30)` | getter `FUN_002af658` (`lw v0, 0x30(a0)`); the object at `[0x48F3D0]` (prefs / menu manager) is created by `FUN_00470fa8` |
| View window | `FUN_001ab9e0(camera a0, raster rect a1, tan f12, aspect f13)` | x = tan(FOV/2), y = x / aspect, raster 640x448 (null rect), then `FUN_0018f2d8` = RwCameraSetViewWindow. Called at `0x21cdd4` with the aspect global, at `0x21f9c8` with a fixed 16/9 (same camera `[0x51E490]`; probably the letterboxed cutscene view) |

- The game's own widescreen is **Vert-**: same horizontal view, vertical view cut, for a TV that
  stretches the anamorphic picture. The HUD does not change with it (the radar keeps its shape); the
  preference's 106 readers are mostly 3D-projected sprites (e.g. `0x1147e0`: a size without the 4/3
  factor).
- RenderWare derives the frustum (EE culling) and the VU matrices from the view window, so widening
  it (our Hor+, `vcs_display.cpp`) widens 3D, culling and both renderers at once.

## 6. Renderer

### 6a. Frame structure

- One VIF1 DMA chain per frame, kicked at `0x27245C` (D1_CHCR = 0x145, chain + TTE); TADR alternates
  between static root lists `0x480D20` / `0x480DE0`. The kick: the flip `FUN_003706c8` -> `FUN_002721f8` (the chain
  pointer at `0x510438`, cleared after) -> `FUN_002723E8(channel, tadr, chcr regs)`, the DMA library's start (clears
  the channel's D_STAT bit, writes TADR / QWC, then CHCR). There is no wait for VIF1 to finish before the next frame
  rewrites what the chain reads: `FUN_00272480(channel, timeout)` (the DMA sync) is called only from `FUN_003177e0`,
  `FUN_00317930`, `FUN_00317b50`. (2026-10-25, from the recompiled code's disassembly comments: the callers of
  `func_2723E8` / `func_272480`; Ghidra was not available.) Full frames are 7-14k tags (CALL / RET per
  model) - more than the runtime's original 4096-tag guard.
- Models are prebuilt packets in the model heap, entered with CALL tags and ended by RET; CALL tags
  are written by the RSL draw functions (6i; `FUN_003298D8`, named here before, builds texture uploads). VU1 microcode sits inside
  `.text` and is uploaded each frame by REF tags with `MPG` VIFcodes (`0x2AC130`, `0x2F13D0`,
  `0x368730`, ...).
- VIF1 payloads come from packet buffers (`0x500000-0x6FFFFF`) and the model heap
  (`0x1200000-0x17FFFFF`). DMA packet buffer `0x520F90`: two halves of 0xE6670 bytes;
  `FUN_00476770` returns the bytes used; draw code skips work when < 0x12C00 are left (peak ~400 KB).
- Z buffer: Z24 640x448 at ZBP 280, cleared each frame by 40 column strips (640x224 CT32 in total)
  drawn at FBP 280, whose own Z write goes to ZBP 350 (the second half).
- Display: PMODE 0x8027 = both circuits on the same buffer one line apart, blended (anti-flicker);
  DISPFB FBP 0, FBW 10 (640), DBY 2 / 1; 640x445 shown.
- **The flip (FUN_003706c8) reads GS CSR** at `0x370714` (`ld $v1, 0($a0)`, $a0 = 0x12001000) and keeps only the
  FIELD bit: `dsrl $v1, 13` then `andi $s4, $v1, 1` in the next branch's delay slot; $v1 is overwritten right after.
  It is the game's only CSR read per frame (2026-10-25, a game-thread profile with call stacks: the runtime's CSR
  fence - waiting for the render worker to finish all queued GS work, for SIGNAL / FINISH - was ~25% of the game
  thread uncapped). FIELD is set at VBlank by the EE scheduler, so that read skips the fence
  (`PS2Runtime::addCsrReadWithoutFence`, src/game/vcs_display.cpp; `VCS_CSR_FENCE=1` = as before).
- **Scratchpad**: the per-frame render state lives at `0x70002020+` (e.g. FUN_002b8ed0, the busiest draw function,
  stores ~10 words there per call through `$s0 = 0x70002020`). The recompiler resolves those addresses as constants;
  the runtime's Load / Store for them are inline since 2026-10-25 (they were out-of-line calls taking a mutex).

### 6b. World and culling

- World sectors: `FUN_0023E748(level, pass)` (3 passes per frame; takes a lock, scrolls water UVs at
  level+0x5D0/0x5D4) walks the sector slots at level+0x248.., and per entry calls `FUN_00240ED8`,
  which culls with `FUN_002F7528` = unit box transformed by the sector matrix, tested by a VU0
  microprogram (CMSAR0 0x19F, VI15 = 0xDC) via `FUN_0030C8D4`; 0 = culled, 1 = inside, 2 = partly.
  Visible sectors go to `FUN_002409C8`.
- Entities: `CCamera::IsSphereVisible` = `FUN_003816F8` (VU0 macro transform of the centre by the
  camera matrix, near / far at `0x4CD130` / `0x4CD12C`, planes at cam+0xB60).
- Level object `0x750340` (pointer at `0x48F4F8`). The world is a hex grid of cells (125 x 108.25
  units, odd rows offset by half a cell; origin = ((x + 0.5 - (y & 1) / 2) * 125 - 2400,
  (y + 0.5) * 108.25 - 2000), `FUN_00241F00`). Cell slots at `0x75036C` (+0xA0 each): grid x / y at
  +8 / +0xC, loaded flag +0x18, draw lists via +0x20. `level+0x248` = current cell, `+0x250` = next;
  `FUN_0023C820` (per frame) follows TheCamera (`0x6F44D0`, position at +0x30) and `FUN_0023D740`
  swaps them. Each cell streams ~0.3-0.5 MB of MAINLA.IMG in 6 pieces into `0xCB7A80..0x112A660`.
  Fort Baxter is around cells (5..7, 16..18).
- In-world draws are mostly MODULATE + alpha test GREATER 8, fogged, vertex colours from V4-5
  unpacks.
- **Levels**: the world is three levels, each a `WRLD` image (`.LVZ`) plus an archive (`.IMG`): 1 = BEACH (east
  island), 2 = MAINLA (mainland: Little Havana, Little Haiti, Downtown, the airport, Fort Baxter), 3 = MALL. The
  executable names them with the developers' paths (`cdrom0:/RUNDATA/BEACH.LVZ`... table at `0x4ADAE0`, resolved
  through the disc index). Only one level's WRLD is in memory. `FUN_0023baf0(level, newLevel, ...)` switches;
  `FUN_0023c188(level, camera position)` (per frame) calls it when the wanted level - global `0x4CD118`, set from
  the player's zone - differs from the loaded one (`level + 0x18`). Teleporting across islands switches levels by
  itself within half a second (rig `level_test`, 2026-10-23).
- The cell the streaming focuses on is computed from the camera position in `FUN_0023c188` with float maths and
  `cvt.w.s` (float -> int). The EE FPU's `cvt.w.s` truncates; the runtime rounded to nearest, so at places the
  game picked a neighbouring hex cell 100+ units away (camera (-675, 1172) -> cell (14, 30), centre
  (-587.5, 1301.6), instead of (13/14, 29)) and streamed that cell's geometry and collision: nothing drawn or solid
  under the player, who fell through the world (the user's falling-through report; `VCS_TRACE_CELL=1` shows the
  focus).

### 6c. VU microcode

- `.vutext` holds four small VIF `MPG` packets (DVP overlays: `0x48055C` -> VU 0x000, 245 pairs;
  `0x4810EC` -> 0x400, 4; `0x48146C` -> 0x000, 24; `0x48176C` -> 0x200, 20). They are **VU0**
  helpers: program 0 (0x000..0x7A8; identical in VU0 code memory during play) holds **0x020 = segment vs
  triangle** and **0x3A0 = sphere vs triangle** (with the nearest-point-on-segment subroutine at 0x228, called with
  `BAL vi15`); they were ~22% of the EE thread interpreted (2026-10-23, `PS2X_TRACE_VU0`) and are native now
  (`src/game/vcs_vu0_native.cpp`, checked bit-exact with `PS2X_VU0_VERIFY=1` over 4.7M runs while driving). Inputs:
  vf17 (12.4 / 7-bit fixed point: normal or segment), vf14..16 the triangle (7-bit fixed point), vf12 / vf13 the
  sphere or segment; out vi1 = hit, vf1 contact point, vf2 / vf3 direction and depth. Program 0 has E-bit exits at 0x000 / 0x010, converts fixed point with `ITOF0` /
  `MULi 1/128`, builds edge vectors and runs `CLIP` / `FMAND` / `FCAND` / `DIV` - most likely
  collision (line / sphere vs triangle). Found by disassembling with hctp-recomp's `vu1dis.py`.
- **The other VU0 programs while driving** (2026-10-24, `PS2X_TRACE_VU0=1` per 5 s; code dumped with
  `PS2X_VU0_DUMP=0c10`, disassembled with `tools/vudis.py`; all native now, `PS2X_VU0_VERIFY=1` 0 differ): 0x0CF8
  sector box vs three plane sets (~240k calls), 0x0EB8 three 4x4 matrix products (~220k), 0x03A0 / 0x0020 above,
  **0x0C10 sphere vs three plane sets** (19k calls, 190 ms interpreted - 10 us a call: vf1 = centre + radius in w,
  matrices at vi15+24, vf31 = (1,1,1,1); vi1 / vi2 = per set the MAC sign flags of radius - distance /
  distance + radius, read by FMAND 4 pairs after the FMAC), **0x0068** = the segment-vs-triangle program entered after
  its fixed-point conversion (5.8k, 60 ms), **0x0E98** = the 0x0EB8 loop on the matrices at vi15 into vi15+12 (2.9k,
  25 ms), **0x0E30** = vf1..vf12 stored to vi15+0..11, the last in the E bit's delay slot (2.9k, 8 ms). Together the
  four were ~5% of the game thread while driving. 0x0CF8 / 0x0C10 are called through `FUN_0030C8D4` / `FUN_0030C8A8`,
  the matrix programs through `FUN_0030C940` / `FUN_0030C900` (e.g. `FUN_002F7620`, which builds the camera's plane
  matrices from `0x742AC0` into `0x520AD0` / `0x5208A0` / `0x520860`).
- The **VU1** rendering microcode is not a static MPG packet: it is uploaded from `.text` each frame
  (6a). `game/vcs/vu1_uploads.txt` lists the uploads seen; `tools/vu1_recomp.py` compiles them.
- Setup programs that never XGKICK (normal): 0x0148 (clear state), 0x0418 (lighting dispatch via
  JALR table at VU mem 34..37), 0x0478 (matrices), 0x2B50 (matrix copy).

### 6d. VU1 pipeline (found with PS2X_HLE_SURVEY, NATIVE_RENDERER.md stage 4)

Program **0x0030** produces ~95% of all GS vertices (3.0-3.5M/s) and ~88% of VU1 time. 0x02c0
(V4-32 x4 per call: sprites / particles / text) ~3%. Disassemble samples with
`python tools/vudis.py <vucode.bin> --start 0x30 --to-ebit` (`PS2X_HLE_SURVEY_DUMP=2
PS2X_HLE_SURVEY_DUMP_EVERY=40000`).

**0x0030 is a dispatcher**: `JR` to instruction `[38].x` inside itself, then a chain of `JALR vi15`
calls whose targets / `vi01` parameters come from VU data qwords 38-41 (set per batch by the setup
programs). Stage tables (instruction indices):

| entry | stages (vi01) | used by |
|---|---|---|
| 0x19 | 0x0da, 0x4e0 (0x9c), 0x2b6 | prelit world: V3-16 pos @1, V2-8 uv @0x48, V4-5 colour @0x8f (+ V4-32 matrix @0x2c x3 in some) |
| 0x19 + 0x4ff | 0x0da, 0x4ff (0x9c), 0x2b6 | rigid lit models (vehicles, props): int8 normals V3-8 at `[47].y` |
| 0x16 | 0x5a5 (0xa8), 0x0da, 0x4ff (0x9c), 0x2b6 | lit, skinned models (peds): V3-16 pos, V2-8 uv @0x55, V3-8 normal @0x2b, V4-32 weights @0x7f |
| 0x13 | 0x0da, 0x4ff (0x9c), 0x2b6, 0x5b1 (0xa8), 0x2b2 | rigid lit models with an **environment map** (vehicles): native since 2026-10-23 |

Code at 0x2700.. (instr 0x4e0..) is swapped by per-pass MPG uploads, so programs are keyed by the
code at the start PC *and* 0x2700-0x2fff.

VU data layout of the world pipeline (qwords; "top" = XTOP double buffer):
- 28-31: combined world -> GS window matrix (rows; includes the viewport scale / 2048 offset).
  24-27: the clip matrix.
- 42: position scale (1/32768 for int16), 43: position offset (folded into the matrix).
- 44-46: 3x3 matrix for the V2-8 texture coordinates, applied with I = 1/255.
- 47: input offsets: .x positions (1), .y normals, .z uv (0x48), .w colours (0x8f).
- 48: .z = mask applied to TOP, .w = output buffer offset (0xd6).
- 49 / 50: GIF tags for the kick (PACKED, NREG 3: ST, RGBAQ, XYZF2; PRIM strip with IIP TME FGE ABE);
  NLOOP = vertex count from `[pos-1].w`. 51: default RGBA (255,255,255,128) / material colour.
- 52: .x transform routine (0x0e9 normal / 0x0da first call), .y flags (bit 1 fog, bit 0 extra pass
  at 0x1440), .z kick variant (698 / 711). 53: fog (.z bias 255.6, .w scale -0.128 per unit of w).
- 0x9c-0x9e (vi01 of stage 0x4e0): colour scale (time-of-day tint), offset, clamp (255).

Routines (byte addresses): 0x0748 normal path (`BAL 0x0c38` clip test; all inside -> 0x06f0 fast
path; some outside -> clipper 0x11f8 + 0x13d8); 0x07f0 transform (`p = M * (itof(pos) * scale +
offset)`, `Q = 1/p.w`, XYZ = FTOI4(p.xyz * Q), STQ = (uvMatrix * itof(uv) / 255) * Q); 0x1048 fog
(F = clamp(53.z + 53.w * w, 0, 255)); 0x2700 prelit colour (RGBA = MINI(itof(c) * [0x9c] + [0x9d],
[0x9e])); 0x15b0 GIF tag + `XGKICK`. Lit models: 0x2d28 (stage 0x5a5) 4-bone skinning (weights'
low bits are bone indices, bones 4 qwords each from 0xa9, result written back over the position /
normal slots); 0x27f8 (stage 0x4ff) 4 directional lights + ambient, scaled by `[51]` * 1/510, RGB
only. All reproduced bit-exact in `src/game/vcs_hle_world.cpp` (`PS2X_HLE_VERIFY=1`), except the
STQ.w lane and its fog in the lit pass, which the runtime's VU1 core leaves inconsistent.

**Environment map (stage table 0x13, 2026-10-23).** The rigid lit pass (0x0da, 0x4ff with 0x9c, kick 0x2b6 = tag 49)
followed by **0x5b1** (vi01 = 41.y = 0xa8) and kick **0x2b2** (tag 50). 0x5b1 first XGKICKs the GIF packet at vi01
(seen: one A+D write of TEXFLUSH), then for every vertex (at least 6): ST = (rows vi01+5..7, x / y lanes times
I = 1/256) . normal + 0.5, times the vertex's Q from the output, written over the output's S and T. Kick 0x2b2 sends
the same output (colours, XYZF2, Q unchanged) again with tag 50: PRIM 0x25c - strip, Gouraud, textured, blended,
no fog, **context 2** (the environment texture set by the game's own GS packets). The kick tag's x lane is the
packet header's .w via ILW + MFIR - sign-extended (0xffff80nn). Reproduced (`envKicks` in vcs_hle_world.cpp) and
checked against the VU's three kicks: 27,243 packets over the reference scenes, all exact; native drawing: pass 1
as a rigid lit model on the GPU, the TEXFLUSH and the second pass as the VU's kicks (PATH1). Clipped table-0x13
packets stay on the VU1.

**Code images.** The code below 0x2700 is not fixed either: VCS uploads different variants of the transform
(0x07f0) and clip test (0x0c38) per pass - every reproduction is tied to a hash of the exact code it was read from.
Seen so far:
- the prelit world image (integer positions folded with 42 / 43, integer UVs through the texture matrix 44-46 / 255,
  prelit colour stage at 0x2700);
- the **float images**: positions are floats, the matrices 24-27 / 28-31 are used as stored (no fold), RGBA is
  qword 51 from the transform, the fog stage FTOI4s into XYZF.w. Two flavours: the one 0x02c0 runs with (UVs as
  floats, used as they are) and the "variant C" prelit one (UVs ITOF0'd through the texture matrix, a colour stage
  at 0x2700 that copies the colour qwords - layout V4-32 @001 / @049 / @091 x72; a second copy differs only in
  padding after a JR). Variant C's transform epilogue leaves the *next* vertex's w in STQ.w for vertices N-4..N-2
  (N = max(count, 7) processed): a MOVE.w waits for the MULw right before it - and the fog reads w there.
- Clip tests run over at least 5 / 6 vertices, so stale data past the count reaches CLIP: the VU treats exponent-255
  bit patterns as huge finite numbers (no NaN / Inf), so such a vertex can flag the packet for the clipper -
  reproduced with the VU's operand / result clamping (vcs_vu_math.h, from the runtime's vu1fast helpers).

**0x02c0 - instanced re-draws** (setup slot 0xc8, entry 0x1c): copies the packet's object matrix (4 qwords at TOP)
to 0-3, 0x04d8 builds clip (view 16-19 x object) and window (viewport 12-15 x clip) matrices into 24-27 / 28-31,
0xc8 the camera in object space (33 = R^T (32 - T)), then transform 0x0da + kick only, over a mesh resident at
absolute addresses (48.z mask 0). **Entry 0x0d** draws a resident mesh twice: setup slot 35 = 0x4e0 normalises the
object's rotation rows (ERLENG) into a 3x2 environment matrix (P+5..7, P = 0x9c); 0x4ff / 0x509 write an A+D TEX0_1
/ TEX0_2 qword at P+1 from P+2 / P+3 and kick the packet at P; pass 1 = the float transform + tag 49; 0x546 sets
RGBA = Q.xyz (Q = 0xa4) with alpha = FTOI0(clamp(Q+1.x * w + Q+1.y)); 0x513 kicks P again and writes the
environment ST ((P+5..7 x / y * 0.5) . normal + 0.5) * Q; pass 2 = tag 50 (context 2). A shine over a texture.

**Shine through the clipper / culled (2026-10-24).** The kick routines load their tag into vf01 and jump through
52.z: 0x15b0 (stage 0x2b6) loads tag 49, **0x1590 (stage 0x2b2) tag 50**; 52.z is 698 (0x15d0, plain kick) or 711
(0x1638, the per-triangle clipper, which builds its fan tags from vf01 - 0x1650 / 0x1660). So a shine object that
crosses the guard band is clipped twice: pass 1 with tag 49, then 0x509 / 0x546 / 0x513 run as usual over the
clip test's output (ST undivided with w in .w, the clip position + fog) and pass 2 is the same clipper with tag 50.
A culled packet (all vertices outside one plane) leaves the stage table at once: 0x0748 -> `B 0x0178` = XGKICK of
qword 58 (the empty tag) and the program's end - only the TEX0_1 kick of 0x4ff was sent. Both reproduced
(`shineKicks`, `vcsclip::clipFromOutput(..., tagQword)`); PHI_A1 in verify mode: 421,694 0x02c0 packets exact,
among them 9,629 clipped and 26,518 culled shines.

**Setup programs** (VU1 starts other than 0x0030 / 0x02c0, no primitives; `PS2X_TRACE_VU1=1`, code dumps in
`build/analysis/vu1setup`; one flyover sample each, so other code images may differ):
- 0x01e0 (and 0x02c8, a copy): object matrix at TOP -> 0-3, 0x04d8 (vi01..04 = 0, 16, 12, 24: clip = view 16-19 x
  object into 24-27, window = viewport 12-15 x clip into 28-31), then the slot routines in qwords 34-37 (routine in
  .x, vi01 = .y: 0xc8 camera into object space, 0x3a nothing, 0x4e0 env matrix), then `B 0x0178` (kick qword 58, end).
- 0x0250: the slot part of 0x01e0 alone, then the kick. 0x03a8: the slots from the .w lanes, then the kick.
- 0x0418: the .w slots, then falls into 0x0478 and ends without a kick. 0x0478: 0x04d8 (4, 8, 12, 16), then
  0x04d8 (0, 16, 12, 24).
- 0x0000 (same image): 42 / 43 (position scale / offset) from TOP, then the end kick. 0x0148: qword 58 = the empty
  tag (0x8000, 0, 0, 0). 0x2b50 (lit image): a block copy - the header at TOP to header.z, then 4 x header.x qwords
  after it (the bone matrices), four loads then four stores per step.
- Slot routines of the lit image (code at 0x2700+, so per image): **0x4e0** (byte 0x2700, vi01 = P = 0x9c) the light
  directions P+0..2 into object space: rows 0-2 normalised (ERLENG, MUL.xyz), P+9+i = L0 r.x + L1 r.y + L2 r.z (all
  lanes); **0x57e** (byte 0x2bf0, vi01 = A = 0xa8) the env matrix: A+5+i .xy = rows A+2..4 . normalised row i, stored
  as whole qwords - A+7.z is the last ERLENG, the other z / w lanes are stale registers (the env pass reads x / y).
- **The Z-clear image** (a second low image, different code before 0x0178; about 2 runs a frame each, reproduced
  2026-10-24 in `zclearProgram`): **0x0200** kicks two 4-qword GIF packets at qwords 0 and 4 in turn, the strips' x
  edges stepped by I = 32 (FTOI4 into the x lanes of qwords 2 / 3 of the other buffer), [0].w pairs of kicks (10:
  20 strips over 640) - the Z clear into FBP 280 that the renderer turns into depth fills; **0x0000** the same with
  6-qword packets at 0 / 6, x lanes of 2-5 stepped by the floats in 4 / 5 (read once; the floats carry 2^23 so the
  sum's low bits are the coordinate) - an SQ paired with an ADD of the same register stores the value from before the
  ADD; **0x0400** XGKICKs the packet at qword 128. Their kicks go to the GS front end (depth fills, register packets).
- **Slot routines are found by their code**, not their number: the light and env routines are position independent
  and 0xf8 bytes long, and the lit images put them at 0x2700 or 0x2bf0 - in one image slot 0x4e0 is the env routine
  (that 0x0250 fell back until slots were named by the hash at their address).
- Table-0x13 packets through the clipper (reproduced 2026-10-24, `envClipKicks`): the clip test / fog write the output
  buffer as for the prelit world (ST undivided + clip w, qword 51, clip position + fog), the lighting stage 0x4ff
  writes RGBA over it, kick 0x2b6 = clipper with tag 49; 0x5b1 kicks the packet at 41.y and writes the env ST (lane z
  of the output as Q) over x / y; kick 0x2b2 = clipper with tag 50. Reference scenes in verify: 216 exact.
With these, **no VU1 program runs in a normal frame** (reference scenes, tour subset: `vu1_programs` 0).
About 330 runs a frame in the flyover and cutscenes before (`PS2X_TRACE_VU1=1`: 0x01e0 30k, 0x03a8 12.6k, 0x0478
9.3k, 0x0250 5.3k, 0x0418 1.5k per 5 s). **Reproduced (2026-10-24)**, each routine tied to a hash of its code
(`CodeCheck` in vcs_hle_world.cpp), checked with `PS2X_HLE_VERIFY=1` against the whole VU memory after the VU's run
(stale lanes excepted): reference scenes 434,692, PHI_A1 217,489, a city spot 80,557 - all exact. The matrix and
camera routines need the VU's float edge cases (0x0478 / 0x0418 reach denormal or huge inputs: `vuMul` / `vuMadd` /
`vuAdd` / `vuSub`). Left on the VU1: 3-4 programs a frame (the second low image). `PS2X_HLE_SETUP=0` = VU1.

**0x02c0 code images.** Three images with the shine / re-draw stage routines are seen (PS2X_HLE_DUMP_IMAGES=1 writes
the first shine packet of each: build/analysis/shine): hash (0x30..0x2b20) 1a8edffd (the one reproduced first),
da70874b (differs only at 0x1030 - dead code after a JR's delay slot) and 97eecb5d (also in 0x0a88-0x0c10 and
0x0ef0-0x1020, alternative transform / clip-test routines the stage table never calls: the shine and re-draw enter
through 52.x = 0xde / 0xe9 only). The second and third were seen in the city (e.g. (-675, -75)) and fell back to the
VU1 until accepted; verify there: 85,413 re-draws exact.

All of these are reproduced in `vcs_hle_world.cpp` (floatKicks / shineKicks) with the VU memory they write, and
checked against the VU's kicks and memory (174,110 packets over the reference scenes, MAR_A1 / GON_C3 / a drive
~50k / 51k / 120k - all exact). With them native, **nothing of the 3D runs on the VU1** in the reference scenes
except clipped environment-pass packets (63 in two minutes); clipped 0x02c0 / variant-C packets go through the C++
clipper read back from VU memory (`vcsclip::clipFromOutput`).

### 6e. GS usage (VCS_GS_STATS, title / intro)

- ~95% triangle strips; also triangles, fans, sprites, points. CT32 colour (some CT16 for effects);
  Z24 (some Z32), test GEQUAL.
- Blend: 84% `(Cs-Cd)*As+Cd`; also `(Cs-Cd)*FIX+Cd`, `Cs*FIX+Cd` (FIX up to 255), `Cs*FIX`,
  `Cd-Cs*FIX`. Alpha test GREATER (+KEEP) and GEQUAL with AFAIL=FB_ONLY; no DATE.
- Textures: 83% PSMT4 + CT32 CLUT (world), CT32, T8, CT16; modulate / decal; repeat / clamp /
  region clamp; bilinear. Fog on most world geometry. CT32 / CT16 textures inside render targets
  = post effects. COLCLAMP on, no dithering, no FBA, no PABE.

### 6f. 2D: HUD, text, radar, menus, effects

Found from frame dumps (`PS2X_HW_DUMP`, draw lists with depth) during the widescreen work:
- RenderWare keeps **ZTE on for 2D** (ZTE=0 is prohibited on the GS) and uses ZTST ALWAYS or GEQUAL.
- The HUD, text (font texture 256x256), radar and menus (RwIm2D) are all drawn at the **largest Z**
  (0xFFFFFF; 0.99999994 after scaling by 2^-24, never exactly 1.0).
- 3D-projected sprites - clouds (64x64 textures at z ~0.0006), coronas, the mission marker glow -
  take the same RwIm2D path but at their real depth.
- The sky is a full-width gradient at z = 1 (n=18 strip, x -0..852); a horizon band full width too.
- The colour filter / trails effect is drawn as **strips that sample the frame** itself (textures
  inside render targets, `texIsRt`), plus an alpha-only quad (FBMSK 0xFFFFFF) over x 80..560 at
  z = 0.
- The radar is drawn straight onto the screen (corrected 2026-10-24 from a frame dump at Fort Baxter: there is no
  off-screen radar target - the 128x128 at FBP 420 is the reflection map, 6g): the round outline is a fan at far Z
  (GEQUAL, Z write on) and the map tiles (128x128 T4 textures, far Z, GEQUAL) are clipped by the **scissor** to the
  radar's rectangle (37,336 - 120,412 at 4:3). Menu labels are scissored too.
- Pause menus have no depth-tested geometry below the far plane (that is how "3D frame" vs "2D
  frame" is told apart for the widescreen presentation).

**At the RenderWare level (2026-10-23, `src/game/vcs_hud.cpp`).** Found from the HUD's format strings (`$%08d` at
0x4b73a0, `%02d:%02d` at 0x4b73a8, used at 0x31ec08 in the money / clock routine FUN_0031eb18) and a trace of the
calls (`VCS_TRACE_IM2D=<n>`, `VCS_TRACE_IM2D_MARKERS=5`):
- **Im2D**: FUN_002fbe08(prim, vertices, count) = RwIm2DRenderPrimitive, FUN_002fbe68(prim, vertices, count, indices,
  indexCount) = the indexed one. **RwRenderStateSet** = FUN_00175198(state, value) (a switch over states 1..12; 6 =
  ZTESTENABLE). Font setters around 0x3f4000-0x3f4050 write the font state at 0x711f30.
- **Vertex** (16 bytes; the PSP-style packed form): u, v as u16 fractions | RGBA bytes | x, y as s16 pixels in
  640x448 | z (u32; the HUD's is 0xfffffe).
- **The 2D pass** (FUN_0021f348, after the 3D): sky gradient (0x3e0c6c) and horizon band (0x3e0d4c), the radar map into
  its off-screen target (indexed, 0x39fec4), clouds (0x3a1260), then **CHud::Draw = FUN_0031c628**: text glyphs
  (0x3dffc0, quads), the weapon icon (0x3a07a8: a sprite routine, z 0x4dcc3, depth test off), ammo digits (0x3dfe24),
  the radar on screen (0x3e0018, 0x3e0094, 0x3dff64); then FUN_001676c8, FUN_00396670, FUN_003aee80, FUN_002a9708,
  FUN_0025cd98 (messages / text when shown); the frame's display list goes out after (FUN_002723e8).
- The radar map is masked to its circle by **depth**: its HUD draws are depth-tested and their z matters.

The **16:9 weapon-icon bug** (the icon at the right edge, away from its ammo count): the renderer's widescreen placement
treats far-plane 2D as HUD, and the icon is not at the far plane. Fixed at the source: from CHud::Draw to the frame's
end, Im2D primitives drawn with the depth test off get the HUD's z (`VCS_HUD_RW=0` = off).

**From Im2D to the screen (2026-10-23, F12 draw dumps at 16:9, `PS2X_HW_DUMP`; `VCS_TRACE_HUDKEY=<n>`).**
- The GS sees an Im2D vertex at (x - 0.5, y - 0.5) (the 2D code's pixel-centre bias) for most HUD draws, within 1/16 of
  (x, y) for others (the zone name, fades): a vertex's whole-pixel position finds it again with either rounding.
- The game draws the frame into the buffer at FBP 140 and copies it into the displayed one (FBP 0, DISPFB DBY = 2)
  with a plain full-screen copy in 40 strips (decal, no blending) at the start of the **next** frame. So what is shown
  is the previous frame's FBP 140, HUD included.
- Just before the HUD, a full-screen draw writes Z = 0 everywhere (FBMSK 0xffffffff: Z only). HUD draws use ZTST
  ALWAYS (most) or GEQUAL with Z writes at z = 1.0 (help text, zone name): later depth-tested HUD draws do not overlap
  earlier ones. Blending is (Cs - Cd) * As + Cd or additive; no draw seen uses the destination alpha.
- (Corrected 2026-10-24: the 128x128 drawn into FBP 420 is the reflection map, not the radar's map - the radar's tiles
  are drawn onto the screen directly.)
- Indoors the radar is the ring alone. Help text hides the rest of the HUD while it is up; on leaving the barracks
  the radar flashes for a while (a tutorial).
- **Radar on screen** (dump outside the barracks door, 16:9): first an untextured, colourless draw (blend
  (0 - 0) * As + Cd, Z write) of 24 triangles fills the radar box's corners outside the circle with Z 0xFFFFFE; then
  the map as four 128x128 tiles (large quads around the box) at Z 0xFFFFFD with ZTST GEQUAL, scissored to the box:
  they fail in the corners. Then the ring (64x64), the north / blip icons (16x16), all with bilinear filtering.
- **Menus** (2026-10-23; `VCS_TRACE_IM2D_STACK=1` lists the return addresses on the guest stack at each 2D call):
  FUN_0021f7d8 is the frontend's draw, called every frame; it returns early unless a menu is up, else calls
  FUN_00471400, then **FUN_003b53f8 (the menu / pause-menu draw)** (through 0x3b568c / 0x3b56d8 and 0x3cdfbc to the
  page drawing), then FUN_003e0b10 (a full-screen overlay). In the pause menu no 3D is drawn and CHud::Draw is not
  called: the frame is the copy of FBP 140 into FBP 0, then 2D only into FBP 140 (backdrop, map, icons, text, the
  overlay). The map's player arrow blinks.
- **HUD textures**: the clock and money digits (orange and green, with outlines) and the wanted star are one
  128x64 atlas with 16-texel cells side by side; the ammo count and text use a 256x256 font atlas. All HUD textures
  are bilinear-filtered (TEX1 MMAG / MMIN linear) and drawn smaller than their texels (a clock digit: 15 x 21.5
  texels into about 5 x 9 pixels).

Milestone 3 step 2 builds on this (docs/NATIVE_RENDERER.md "HUD layer"): `vcs_hud.cpp` collects the HUD context's Im2D
primitives each frame, groups them into elements (boxes closer than 12 px across / 3 px down: a line of text, the
radar and its blips, the clock / weapon / money cluster) and gives each an anchor from where it sits on the 4:3
screen: left edge when its middle is left of 280 and it starts before 160, right edge when its middle is right of 360
and it ends after 480, the middle otherwise; full-width draws (fades, letterbox bars) stretch. At the frame kick the
whole-pixel positions of its vertices go to the renderer, which finds each GS primitive's element by them.

**What reaches the GS as PATH2** (2026-10-24, `PS2X_TRACE_PATH2_TAGS=1`, decompiled with `tools/eedecomp.py`):
- **Im2D's packet** (`FUN_002FB690`, called through `FUN_002FB9D0`): `CNT` (3n + 1 qwords) with `DIRECT 3n + 1`, one
  PACKED GIFtag - NLOOP n, EOP, PRE with the RenderWare primitive as PRIM (`0x5D` fan / `0x5B` triangles / `0x4D`
  untextured fan / `0x25C` context-2 strip ...), NREG 3, REGS `0x512` = ST, RGBAQ, XYZ2 - then per vertex ST (u, v
  from the u16 fractions, Q 1.0), RGBAQ (the bytes, halved when `*0x488370`) and XYZ2 (x + 0x6C0 / y + 0x720 in
  1/16 pixel, minus 8: the half-pixel bias; z 32-bit). Every PATH2 packet that draws in normal play has this shape
  except a few effect strips.
- **Effect strips** in A+D packets: `PRIM`, then per vertex `UV` / `RGBAQ` / `XYZ2` register writes (the full-screen
  flat quads, 20 strips of 6 vertices; the reflection map's copies of FBP 140 into FBP 420, context 2, `PRIM 0x35C`
  with UV). Packets that set other registers before their `PRIM` (TEX0, ALPHA, TEST, ...) are state.
- The rest of PATH2 is A+D register setting and texture uploads (IMAGE).

**Frames outside gameplay** (2026-10-24): `FUN_0021ED88` is the main loop around `FUN_0021E950`, the per-frame
function (`FUN_0021CE80` begins the 3D frame - camera, clear, sky via `FUN_0018DDC0`; then the world, the 2D pass
`FUN_0021F348`, the menu `FUN_0021F7D8` -> `FUN_003B53F8`, and `FUN_0021DAD8(1)` ends the frame). **`FUN_0021E170`
is `LoadingScreen`**: its own frame - `FUN_0021CD20` begins it (clear), the splash full-screen through the sprite
routine `FUN_003DFF20` (raster from `FUN_0021DBF0`), the loading texts (`*0x486E64` 1..3), the progress bar
(`FUN_0021DFB0`, steps of 1/18 or 1/11) - and `FUN_0021DAD8(0)` ends it (kicked through `FUN_002723E8` like every
frame). It is called from many load paths (`0x346BAC`, `0x3C817C`, `0x45F3E4`, `0x46FE98`, `FUN_0021E950` when a game
is being loaded). **There is no title menu**: with an empty card the game goes from the loading screens straight into
the opening sequence (the reference scenes' "flyover"), where Start does nothing; the frontend menu is the pause menu.

### 6g. How a frame is put together (draw-list dumps, 2026-10-23)

From `PS2X_HW_DUMP` / F12 with `PS2X_HW_DUMP_BATCHES=4` at the hospital (-884, -470), noon and 20:00; the draw
log names the render target each copy reads (`src rt fbp=...`). The game draws a frame in one VBlank and nothing in
the next (30 fps). In order:

1. Opaque full-screen copy of buffer **140 -> 0** (240 vertices: strips) - the previous picture kept in FBP 0.
2. Sky gradient (24 vertices, `Cs*FIX`, z = far), cloud sprites (64x64, additive), horizon band (full width).
3. Alpha-only full-screen quad into 140 (FBMSK 0xFFFFFF), then the **native 3D** (~900-1300 world / lit draws,
   all into FBP 140).
4. Opaque full-screen **self-copy 140 -> 140** (120 vertices) - a shifted / filtered copy of the scene onto itself.
5. The reflection map: a 128x128 sample of 140 into FBP 420 plus sun glow sprites (not the radar - corrected
   2026-10-24); more 3D (particles, peds' second pass) into 140, the vehicles sampling 420.
6. Bloom: 256x256 ping-pong between FBP 420 and 452 (4 passes: clear `Cs*FIX` black, add the other at FIX 128 with
   tint 0x13), then added into 140 (`Cs*FIX + Cd`, FIX ~100).
7. Trails / colour: 512x512 downsample 140 -> FBP 384 (opaque), blended back into 140 at FIX 8 (`(Cs-Cd)*FIX+Cd`,
   624 vertices of strips) and added with a tint (0x10, 0x03, 0x09 at noon; 0x10, 0x01, 0x04 at 20:00).
8. Blend with the previous picture: FBP 0 -> 140 at FIX 16 (motion blur), alpha-only clear.
9. HUD / text at the far plane; the cinematic bars (zero height when closed) and text boxes (alpha 0x40).

**The post effects' exact draws** (2026-10-23, noon outside the barracks, 640x448 4:3, `PS2X_HW_DUMP_ALLVERTS=1`;
UV mapping per primitive fitted from the vertices, u = du/dx * x + offset in texels, x the GS position):
- Copy 140 -> 0 (frame start) and motion blur 0 -> 140 (`(Cs-Cd)*16/128 + Cd`): bilinear, **u = x + 0.5,
  v = y + 0.5** - on the GS (which samples pixel X at X) exactly texel X's centre: a plain copy.
- Self-copy 140 -> 140 after the 3D: point sampling, REGION_CLAMP, colour 0x7f, alpha 0, ZTST GEQUAL at z = 0 (only
  where nothing was drawn: the sky); strips with u = x + 0.94..1.88, v = 1.004 y + 0.94 - the sky shifted by about
  a pixel.
- Bloom: 640x448 -> 256x256 at FBP 420 (bilinear, 2.5 x 1.75 texels per pixel, +0.5), threshold `Cd - 255 * 35/128`,
  four blur passes (420 <-> 452: clear, then 35 quads at FIX 128 with colour 0x13 = a ~15% weight each, point
  sampling, tap offsets of a texel), added into 140 at FIX 100 (bilinear upsample, 0.4 x 0.57 texels per pixel).
- Trails: 640x448 -> 512x512 at FBP 384 (bilinear, 1.25 x 0.875), blended back at FIX 7 from 103 strips with offsets
  of up to 11 texels (a smear), then added at FIX 128 with a tint colour (0x10 0x03 0x09 at noon).
- A 128x128 sample of the picture (5 x 3.5 texels per pixel, no offset) goes into the corner of FBP 420, blended
  with what is there at FIX ~6-19 (a running average), plus an additive sun / glow sprite (64x64); right after,
  3D draws (vehicles, z ~0.001, blend FIX 6) are textured from that 128x128 corner - a reflection map refreshed
  every frame. After the bloom (which overwrites FBP 420) the sample is added again for the next frame.

**Pixel centres** (found from the copies above, fixed 2026-10-23): the GS samples a pixel at its integer position,
D3D at +0.5. Our renderer had drawn without the offset, so the copies (u = x + 0.5) read halfway between two texels:
the shown picture and the motion-blur feedback were averaged with their neighbours every frame - visibly softer than
the PS2 (barbed wire and fences smeared at 640x448). NATIVE_RENDERER.md "Post effects".

**Evening black frames (fixed 2026-10-23)**: between about 20:00 and 22:15 most frames came out black -
everything but the last text - with normal draw counts, native and VU path alike. Found by dumping the render target
after every full-screen draw (`PS2X_HW_DUMP_STEPS`): the scene in FBP 140 was fine until the bloom was added back
(step 6) and then 0 - the GPU copy of FBP 140 had been thrown away mid-frame. Cause, in our HW renderer, not the
game:
- In the evening the game uploads a 512x512 8-bit texture to block 8960 (page 280 = the Z-buffer area; Z is no
  longer needed by then). It also uploads a 16x16 CLUT at block 13588 (page 424, inside the bloom buffer FBP 420)
  and streams textures into the VRAM around 420 all the time (~6000 small uploads a second). VCS reuses VRAM freely.
- The renderer drops a render target (forgets its GPU copy) when an upload touches its pages. FBP 140 was tracked
  24 page rows tall (pages 140-380) instead of 14 (140-280), because the sky gradient's vertices reach y 738 and
  the height was taken from vertices, not from what the scissor (0..447) lets the GS write. So the Z-area texture
  "hit" the picture and dropped it. FBP 0 likewise stayed 32 rows tall after the boot-time clear of all VRAM.
- Fix (gs_hw.cpp `noteTarget`, `shrinkTargets`): a target's height is the last scissored line it can be drawn to;
  heights shrink back to what was drawn in the last 300 frames. Now 19:30-22:45 show a picture with the night
  bloom, and no target is dropped by the Z-area texture. A regression check (`tools/regress.py`, step "evening")
  keeps it fixed.
- Still dropped (as before): the bloom buffers FBP 420 / 384 when textures are uploaded around them, and the Z-clear
  view FBP 280. Copying uploads into targets block by block (`PS2X_HW_PATCH_TARGETS=1`) is exact but costs ~200 ms
  a frame with VCS's upload rate; it needs lazy patching (resolve only when the target is read) - milestone 3.

### 6h. The VU1 clipper and native drawing (GON_C3, 2026-10-23)

Found by the full mission smoke test: in mission 94 (`GON_C3`) the poolside cutscene showed the ground as black /
fog-blue on the native path, dim brown on the VU1 path (`PS2X_HLE=0`). `PS2X_HLE_VERIFY=1` was clean (26.5M
fast-path vertices exact, every cull decision right), and drawing only the *clipper* packets on the VU1
(`PS2X_HLE_CLIPPED`, now the default) reproduced the reference - so the difference is the GPU clipping that native
drawing used for packets the VU sends through its clipper. Not the cause (each tried and reverted): the GIF tag
(the kick's tag 49 is right - tag 50 is the second lit pass's, routine 0x2b2 = byte 0x1590, PRIM 0x20c: context 2,
untextured; stage table 0x19 always ends with 0x2b6 = 0x15b0, tag 49), perspective-correct ST, perspective-correct
depth, and the near / far clip planes.

The VU's clipper path, read from vu1_code_full.txt and reproduced bit-exactly in `src/game/vcs_vu_clipper.cpp`
(milestone 3, 2026-10-23):
- **Clip test 0x0c38** (all vertices, at least 6) writes per vertex three qwords to the output buffer: ST from the
  texture matrix *not* divided by w, with the clip-space w in lane w; the default colour (qword 51); the clip-space
  position (folded matrix 24-27). The 12 clip flags (plain in bits 0-5, guard band x 65 in bits 6-11) go into the
  input position's .w.
- **0x11f8** fog (when 52.y bit 1): F = clamp(53.w * w + 53.z, 0, 255) into the position's .w (w stays in the ST
  qword). **0x13d8** runs the backface pass 0x1440 when 52.y bit 0 (not reproduced: such packets stay on the VU1;
  none seen). The prelit colour stage then writes integer RGBA as on the fast path.
- **Kick 0x2b6 -> 52.z = 711 = 0x1638**, the per-triangle clipper. Tag 49's **w lane holds a second upper half with
  PRIM 0x7d** (triangle fan, IIP TME FGE ABE); the clipper builds its tags from it (y lane = 49.w). Qword 58 is an
  empty GIF tag (NLOOP 0, EOP) it kicks before every triangle - a sync, draws nothing.
- Per strip triangle (i, i+1, i+2): skip when the third vertex has ADC (0x8000) or all three share a plain flag;
  when no vertex crosses the guard band (flags & 0xfc0 = 0), emit the three as they are (0x23c0); otherwise copy
  them (colour as float) into a ping-pong buffer (66+1 / 111+1, four qwords per vertex) and clip:
  - planes (a x + b y + c w + d, from qwords 64..59): w > 0.3 (near, flag 32, routine 0x1b10), w < 2000 (far, flag
    16), y = -w, y = w, x = -w, x = w (flags 8, 4, 2, 1; 45-degree planes, routine 0x1f68) - each only when the
    polygon's OR of flags has its bit; inside / outside by the flag bit, intersection t = d0 / (d0 - d1), every
    lane of ST+w, colour and position lerped (a * (1 - t) + b * t, the inside vertex first), flags recomputed;
  - after the near plane, colours are multiplied by w (perspective-correct interpolation) and divided again when
    emitted (0x2608; after the near plane alone 0x2520 emits them undivided);
  - rejected when the AND of the output flags is non-zero; emitted as soon as no guard-band bit of a later plane is
    set (masks 1984, 960, 448, 192, 64, 0) - the GS scissor handles the rest.
- Emit: viewport matrix 12-15 applied to (x, y, z, w), / w, FTOI4 with the fog in F; STQ = ST / w; colours FTOI0
  after + 0.001. One GIF packet per polygon (fan, NLOOP = vertex count, EOP).

Checked with `PS2X_HLE_VERIFY=1` against the VU1's own XGKICKs (`runtime/ps2_vu1_tap.h`): GON_C3 98,906 clipped
packets / 1,199,117 GIF packets, the reference scenes 143,963 / 2,837,560 - all exact (ST, Q, RGBA, XYZF2; STQ.w is
not read by the GS). Native drawing hands those packets to the GS as PATH1 (`PS2X_HLE_CLIPPER=0` = the VU1; the
GPU clipping that drew the ground black is `PS2X_HLE_CLIPPED=1`). Cost over the reference scenes, means per
section: flyover render 14.6 ms (GPU clipping 14.1, VU1 17.7), cutscene 11.8 (11.3, 14.5), gameplay 5.9 (5.6, 7.6);
frame time at the 30 fps cap again. The intro office, which the stage-5 notes called black on the VU clipper, draws
correctly on it.

### 6i. RSL submission: how a frame's packets are built (2026-10-24, docs/RSL_SUBMISSION.md phase 0)

Found by decompiling the candidates (`tools/eedecomp.py`, headless Ghidra on the analysed EE project; output in
`build/analysis/decomp/`), a scan of the code for the packet buffer object, and two runtime measurements:
`VCS_TRACE_RSL=1` (before-hooks on every function that moves the cursor; bytes per function per frame,
`src/debug/rsl_trace.cpp`) and `PS2X_CHAIN_CENSUS=1` (every VIF1 chain tag by where it sits, its id and where its data
comes from).

**The packet buffer** is an object at `0x520F90`: +0x14 the half in use (two halves of 0xE6670 bytes from +0x20),
+0x18 the DMA cursor growing up from the half's start, +0x1C a data pointer growing down from its end.
`FUN_00476770` = bytes used (both ends), `FUN_00478628` = bytes free (checked before each write). About 33 functions
write it (the scan: `0x2F0360..0x2FB690`, `0x1CEF18`, `0x266868`, `0x2A92C0`, `0x2ABF68`, `0x32CD60..0x32D880`,
`0x40C430`, `0x40F6C8`).

**State, then draw.** RSL keeps render state in memory and writes it into the buffer only when a draw needs it:
- Matrices: blocks at `0x520860..0x520B8C` (view / projection / object, with dirty flags); `FUN_002F7620` computes the
  camera's. `FUN_002F8710` is the **state flush**: every dirty block as CNT + UNPACK into VU1 memory. Small setters
  upload single constants directly: `FUN_002F7090` (3 qwords at VU 0x2c = 44-46, the texture matrix),
  `FUN_002F9FA0` (one vector), `FUN_001CEF18` (3 qwords at VU 0).
- GS registers: queued as A+D pairs at `0x729510`, count at `0x488308`; each draw (and `FUN_002F7B30` on its own)
  sends the queue as one DIRECT GIF packet (CNT, `0x51` DIRECT, tag `0x1000000000008000 | n`).
- Textures: the texture bind `FUN_00408F30` -> `FUN_00317FB8` (material) -> `FUN_002F62B8` / `FUN_002F6A78` queue
  TEX0 / CLUT state and, when the raster is not resident, its upload: `FUN_003298D8` keeps a binary tree of transfer
  headers (nodes of 0x70 bytes from `0x6EE3F0`, pool at `*0x488760`, used / max `0x4CD58C` / `0x4CD588`: a cached
  BITBLTBUF / TRXPOS / TRXREG / TRXDIR packet per destination) and writes two REF tags - the header packet and the
  image - both sent as VIF DIRECT (PATH2).
- **Texture streaming** (2026-10-24, `FUN_002F62B8` decompiled; `PS2X_HW_UPSTATS=1`): `FUN_002F62B8(slot, psm,
  log2 w, log2 h, mip count, ..., source address, flags)` uploads a raster into a fixed VRAM slot - base
  `*(0x742B30 + slot * 8)`, each further mip at the next 64-block boundary - and skips everything only when the slot's
  key `0x742B50[slot]` (psm, sizes, mips, source) is unchanged. Flag bit 0 per mip: T8 uploaded as CT32 (and T4 as CT16)
  at half width / height - the RAM data is laid out for that. It then queues TEX0 (with the CLUT) / TEX1 / MIPTBP1 /
  MIPTBP2 for the draw. The texture's CLUT is uploaded with it, so while driving every bind uploads texture + CLUT
  (~390k uploads / 5 s, nearly all the same slots again). The header packet ends with the IMAGE GIFtag; the pixels
  follow in the next DIRECT: a REF straight to the raster in RAM whose VIFcodes are `NOP; DIRECTHL n` with n = its
  qwc (how docs/FULLY_NATIVE.md step 3d names textures by raster). T4 textures (the radar tiles among them) are sent
  as CT16, and their CLUT goes into the same GS page right after (other blocks).
- **Draws**, each starting with the state flush and the GS-register queue:
  - `FUN_002F7D58(packet)`: a **model instance** - CALL to the model's prebuilt packet in the model heap (444 calls a
    frame while driving); `FUN_002F80A0(packet)` the same shape (a variant);
  - `FUN_002F7EB0(addr, qwc)`: **world geometry** - REF to a sector's prebuilt packet in the streamed cells (called by
    the sector draw `FUN_002409C8`, whose texture / material setup is itself cached: the first frame copies the
    setup packet into a block on the heap at `0x6F3DB8`, later frames CALL it);
  - `FUN_002F8250(...)`: REF with its own UNPACK / MSCAL (immediate geometry);
  - `FUN_002FB9D0` -> `FUN_002FB690`: **Im2D** (the 2D primitives of 6f: CNT + DIRECT, vertices converted from the
    16-byte Im2D format; 515 calls a frame while driving).

**Measured** (one bench run, i7-7700K, 60 fps cap; per frame):

| | standing in the city | driving (AI car) |
|---|---|---|
| bytes built in the packet buffer | 90-150 KB | ~470 KB |
| VIF1 chain | 720-1,300 tags, ~33-48k payload qwords | ~9,600 tags, ~294k qwords (~4.7 MB) |

Driving, by function (bytes are charged to the hooked function entered last, so a draw's own CALL / REF lands on
`FUN_002F8710`, which every draw enters first): `2F8710` 848 calls / 135 KB, `2FB690` 515 / 125 KB (2D),
`2F62B8` 326 / 64 KB (textures), `2F7090` 304 / 33 KB, `2F7908` 238 / 23 KB, the rest < 20 KB each. The chain while
driving: REFs from cell packets to data in the `0x6EDC70..0xCB7A80` range 78k qwords, the per-frame REFs into the
model heap 55k, cell geometry 34k, CALLed model packets 31k, per-frame CNT data (2,250 tags) 16k, VU1 code (REFs to
`.text`) 8k. **About 10% of what VIF1 receives is built each frame; the rest is prebuilt** (cells, model heap,
cached blocks).

Game thread while driving (`VCS_PROFILE=5`, `VCS_PROFILE_GUEST_FILE`, the two drive windows: 70% guest code, no
waiting): the RSL range `0x2F0000..0x2FD000` 12.5% of samples (the packet emitters 10.1%), the world sector walk 1.5%,
plus `memmove` 3.8% (the runtime flattening the ~4.7 MB chain when it is kicked) - **about 17% of the game thread,
~3 ms of a ~19 ms frame**, goes into building and handing over packets. The rest is spread thin (no other function
above 2.3%).

**Coverage of the draw functions** (docs/RSL_SUBMISSION.md phase 1a, `VCS_RSL_SHADOW=1`, `src/debug/rsl_shadow.cpp`):
before-hooks record every call of the draw functions with its argument and the cursor at entry; as the runtime copies
the frame's chain (`runtime/ps2_chain_observer.h`) each packet-buffer tag is placed in the record whose range holds
it. Every draw wrote exactly its own tag - a model instance a CALL to its packet, a world draw a REF to its geometry
(address and qwc): city spot (1325, -325) 394 model / 42-47 world draws a frame, driving 495 / 79 plus 19.5 immediate
(`FUN_002F8250`) - **all matched**. The only VU1-bound tags outside a draw's range (7 a frame standing, 14 driving)
are state: at the frame's start `FUN_002F53D8` and `FUN_002F9FA0` CALL the camera object's packet (`0x16E7B90`,
object at `*0x4882DC` + 0x10), the sector draw `FUN_002409C8` CALLs its cached setup blocks, one CALL follows a
`FUN_001CEF18` upload. The blocks allocated from the top of the half (+0x1C) hold only RET tags (the ends of CALLed
per-frame blocks). All 32 functions that store the cursor (a scan of the code following register copies) are hooked.

**What a draw carries in state** (`FUN_002F8710`, the flush every draw runs first; dirty bits in `0x4882A0`):
bit 0 CALL the current object's packet (object `*0x4882A4` + 0x10) and its plug-ins' callbacks (object + 0x114 list,
+0x48); bit 1 CALL `*0x4882A8`; bit 2 three qwords from `FUN_001CFAE0(object + 0x134, ...)`; bits 0 / 4 with 0x200 /
0x400 MSCAL 0x4A / 0x75 (the setup programs 0x0250 / 0x03A8, 6d); bit 0x2000 CALLs from the table at `0x742AB0`;
bit 8 UNPACK 5 qwords to VU 49-53 (the GIF tags, colour, flags, fog) from `0x742AE0`; bit 0x1000 UNPACK the viewport
(VU 12-15) from `0x520920`, MSCAL 0x8F (setup 0x0478), the guard band (VU 65) from `0x520AC0`.

**Where a draw program's VU state comes from** (phase 1b, `VCS_RSL_SHADOW=1`: each MSCAL of 0x0030 / 0x02C0 is tied to
the draw whose stream started it by its offset in the copied chain, and the VU memory it starts with is compared with
the draw's RSL state snapshotted at the draw call; the map below found from `VCS_RSL_SHADOW_DUMP` samples):

| VU qwords | what | RSL source | same as the RSL source |
|---|---|---|---|
| 0-3 | object matrix | `0x520860` | 85-97%; the rest whole matrices the chain itself uploads (object packets, cell data) |
| 4-11 | camera matrices | `0x5208A0`, `0x5208E0` | 100% |
| 12-15 | viewport | `0x520920` | 63-98%; the rest a depth range (rows 14-15) some model packets upload, which stays for the draws after them |
| 32 | camera position | `0x520990` | 100% |
| 34-37 | setup slot table | render context (`*0x4882A4`) + 0x60 | 100% |
| 49-51, 53 | GIF tags, colour, fog | `0x742AE0` | 100% |
| 52.y | flags | `0x742AE0` + 0x34 | 100% (52.x / .z are written per packet / by the programs) |
| 59-64 | clip planes | `0x5209A0` | 100% |
| 65 | guard band | `0x520AC0` | 100% |
| 16-31 | view, clip and window matrices | computed by the setup programs (6d) from the rows above | - |

Every differing value is accounted for (bench run: city spots and the drive, 0.000% unexplained): uploaded by the
draw's own stream (its state flush, setters, its packet), carried over from an earlier draw (VU memory persists), or a
whole block uploaded earlier in the chain. World draws match the RSL state completely; model draws add their packets'
own state. The "render context" (`*0x4882A4`) is not a per-instance object: a few values recur (`0x16EEEC0`,
`0x16EF040`, `0x16EF1C0`); the per-instance matrix is the block at `0x520860`.

**Sectors cache their own packets** (2026-10-25, `FUN_00240ED8` read from its decompile; found when packet-buffer
tags turned up in the chain of later frames). `FUN_00240ED8(level, sector entry)` draws a sector through
`FUN_002409C8(level, sector, pass)`, and the first time a sector is drawn in the caching branch (the frustum test
`FUN_002F7528` not 2, `*0x487484 >= 60.0`, the entry's cache word `entry + 4` still 0) it notes the cursor, lets
`FUN_002409C8` write the sector's packets (its material setup, the GS registers and every world draw's flush, GS packet
and REF), puts a RET after them, copies the whole range into a block allocated from the pool at `0x6F3DB8`
(`FUN_002BC730`, copied by `FUN_0045CE10`), stores the block in `entry + 4` and draws it with `FUN_002F7D58(block)` -
a model draw of the copy. Later frames only make that model draw: the sector's packets as written on the first frame
are sent again for as long as the block lives. The caching call of `FUN_002409C8` returns to `0x241060`. (When the
pool has no room the sector is drawn a second time by a plain call and the first copy's packets stay in the buffer.)
`FUN_002409C8` itself caches each material's setup the same way (texture bind, `FUN_002F7B30`'s GS packet) into a
block at `entry + 4` of the material and CALLs it on later frames: the cursor noted, `FUN_00408F30` called from
`0x240D14` (returns to `0x240D1C`), `FUN_002F7B30` from `0x240D1C` (returns to `0x240D24`), the range copied by
`FUN_0045CE10` at `0x240D9C`. Code that writes something frame-specific into
the packet buffer while a sector is cached therefore has it sent again in later frames - the draw items of
docs/FULLY_NATIVE.md step 4c stay packets there.

**A world block** (the REF payload of `FUN_002F7EB0`, sector geometry in the streamed cells; listed from a RAM dump,
2026-10-24) is one shape throughout: per packet `UNPACK V4-32 x1 -> TOP+0` (the header: count, nloop), `STMASK`,
`STROW`, `UNPACK V3-16 x n -> TOP+1` (positions, masked: w from ROW), `STMASK`, `STROW`, `UNPACK V2-8 usn x n -> TOP+0x48`
(UVs, masked), `UNPACK V4-5 x n -> TOP+0x8F` (colours), `MSCAL 6` (program 0x0030) - no constants; everything else the
world program reads is VU-resident state from the draw's flush (6d table 0x19). Sizes seen: 208-2,340 qwords, packets
of up to 70 vertices. Phase 2 draws these from recordings (docs/RSL_SUBMISSION.md).

**A model packet** (the CALL of `FUN_002F7D58` / `FUN_002F80A0`; decompiled with `tools/eedecomp.py`, packets listed
from a RAM dump and checked by the phase 2 recorder, 2026-10-24):
- Both functions run the state flush, write the queued GS registers (`0x729510`, count `0x488308`) as a
  `CNT` + `DIRECTHL`, then `CALL a0` with qwc 0 and VIFcodes 0 (`a0 << 32 | 0x50000000`). `FUN_002F80A0` first CALLs
  the packet at `*0x48830C` when bit 31 of the dirty word `0x4882A0` is set (and clears it) - a state packet shared
  by the draws after it.
- The CALLed packet is a sub-chain, not one block: up to ~64 tags (`CNT` with inline data, `REF`s into the model
  heap - e.g. 6-qword constant blocks shared between packets - and a closing `RET`); single-packet models are one
  `RET` with its payload. VIFcodes seen: FLUSHE, UNPACK V4-32 (constants: matrices, the depth range of 6i's VU table
  rows 14-15), DIRECTHL (GS registers), STMASK / STROW, UNPACK V3-16 / V2-8 / V4-5 / V3-8, STCYCL, MSCAL 0x0030
  (most), 0x0478 (setup) and 0x02C0 (re-draw).
- **Fields an UNPACK leaves alone**: an unmasked V3 / V2 UNPACK writes only x y z (x y) - w keeps what VU memory held
  (the runtime's VIF interpreter; not checked against hardware) - and masked UNPACKs leave write-protected
  fields (mask 3). Skinned packets rely on it: two recordings of the same packet bytes differed only in such a w
  (`0x802A` / `0x801A`), left by the packet drawn before.
- **Other CALLs** in the packet buffer: packets at `0x1FDA020`-`0x1FDA120` (320-1,088 bytes) are rebuilt every frame or
  every other frame; a few state packets in the `0x16EExxx`-`0x16EFxxx` render-context blocks (6i above) use VIF BASE.
- Volume at city spot (-700, -500), per 10 s: 248k `FUN_002F7D58` CALLs, 106k `FUN_002F80A0` CALLs, 89k other CALLs,
  61k world REFs; ~1.29M draw program MSCALs among them.

**Texture uploads** (2026-10-25, from the decompiles of `FUN_003298D8`, `FUN_002F62B8`, `FUN_002F6A78`; native as
reference ops since FULLY_NATIVE.md step 5b):
- `FUN_003298D8(slot, psm, log2 w, log2 h, bytes, source, dest)` writes **two REFs into a 32-byte slot its caller
  reserved**: one to the destination's transfer header (qwc 6, VIFcodes FLUSHE, DIRECTHL 6) and one to the pixels
  (qwc `bytes / 16`, VIFcodes NOP, DIRECTHL `bytes / 16`). The header is a 0x70-byte node of a binary tree rooted at
  `0x6EE3F0`: key at +0x60 = `(dest >> 6) << 48 | (psm & 0xFFFF) << 32 | (h & 0xFFFF) << 16 | (w & 0xFFFF)` (compared
  unsigned; smaller keys left at +0x68, larger right at +0x6C), the packet at +0x00: GIFtag `0x1000000000000004` A+D,
  BITBLTBUF (0x50), TRXPOS (0x51), TRXREG (0x52) from the sizes, TRXDIR (0x53) and the IMAGE GIFtag
  (`bytes >> 4 | 1 << 59`). A missing node is made from the pool at `*0x488760` (used / max `0x4CD58C` / `0x4CD588`);
  with the pool full both REFs are written empty (qwc 0).
- `FUN_002F62B8` (the bind into a VRAM slot) reserves 32 bytes per mip level and calls it for each (128 bytes and
  `FUN_00266608` instead when `*0x488318 == 0` - not seen), then writes a CNT 5 with DIRECTHL 5 (A+D: TEXFLUSH, MIPTBP1,
  MIPTBP2 for the slot, register 0x42, TEXFLUSH) at the cursor, and queues TEX0 into `0x4882E0 + slot * 8` with the
  dirty bits. `FUN_002F6A78` (the CLUT) reserves 32 bytes for one call (CT32 16x16 or 8x2 by the palette size) and may
  write a CNT 2 (A+D: register 0x42, TEXFLUSH).

**Im2D's packet writer** `FUN_002FB690(prim, n, vertices, indices, context, screen)` (2026-10-25, from the recompiled
code's disassembly; its only caller is `FUN_002FB9D0`, which does not touch the packet after it): nothing while
`*0x488398` is set; else one CNT of 3n + 1 qwords whose VIFcodes are NOP, DIRECT 3n + 1. The GIFtag: NLOOP n, EOP,
PRE, PRIM = prim | IIP | TME (when the context's texture entry `0x6EDCBC + context * 4` is set) | ABE | CTXT (context << 9), NREG 3, REGS
`0x512` (ST, RGBAQ, XYZ2). Per vertex (16-byte Im2D vertices, through the index list when there is one): ST = the
16-bit u / v * 2^-15 with Q 1.0 (kept from the vertex before when untextured), RGBA from bytes 4-7 with alpha halved
(every channel halved while `*0x488370` is set), XYZ2 = ((x + 0x6C0) << 4) - 8, ((y + 0x720) << 4) - 8 from the
16-bit coordinates (or x << 4, y << 4 when `screen` is set) and Z = the 32-bit word at +0xC through int -> float ->
int. The HUD layer draws these (NATIVE_RENDERER.md "HUD layer"); since FULLY_NATIVE.md step 5c they reach it as ops.

**The frame's fixed structure** (2026-10-25, `PS2X_DUMP_CHAIN=<n>` - every tag of one kick - read with the census).
The chain starts in the static root list (`0x480D20` / `0x480DE0`, 6a): CALLs to fixed packets in `.data` around
`0x480EA0`-`0x481760` that send GS setup (CNT + DIRECT), VU1 setup (CNT STCYCL + UNPACK, NEXT, CNT MPG - VU1
microcode - and RET with an MSCAL) and finally a NEXT into the packet buffer's half. **Render-context packets**
(CALLed by the state flush, bits 0x1 / 0x2 / 0x2000; in the model heap and the heap) open with a REF whose VIFcodes
are **BASE / OFFSET** (VU1's double buffer), then CALL a block of **MPG REFs into `.text`** (the draw programs: e.g.
five of 128 / 112 qwords from `0x2AC130`, one of 27 from `0x25E890`, three from `0x368730`) and end with a REF of the
context's matrices (STCYCL, UNPACK) and an MSCAL. Every frame re-sends the same microcode (the runtime copies it only
when it differs). The Z clear (6a: 40 column strips) is a VU1 program in the chain, run natively (`zclearProgram`,
`src/game/vcs_hle_world.cpp`).

**The other packet writers** (2026-10-25, step 5d of FULLY_NATIVE.md; callers from the recompiled code's `jal`s, shapes
from decompiles and the census of what each writes): `FUN_002F8250` is **Im3D's submit** (called by `FUN_002FC2E8` /
`FUN_002FC838`, `FUN_002FC210` writing the vertices): it runs the state flush, then REFs the vertices with an UNPACK
and an MSCAL. `FUN_0040F6C8(vu, src)` (called by `FUN_0040F798`) writes one CNT with a 9-qword UNPACK and returns 9;
`FUN_002A92C0(program, block)` (called by `FUN_002A9538`) writes a CNT UNPACK, a REF to the block's data with an UNPACK
and an empty REF carrying an MSCAL - immediate geometry with its own program. `FUN_0040C430` and `FUN_00266868` have
no direct callers (reached through function pointers - plug-in or pipeline callbacks). `FUN_002F0360` and
`FUN_0032D880` (which calls `FUN_0032D480` / `FUN_0032D7C8`) are called from `FUN_001ACE40`; `FUN_002F39D8` from
`FUN_0035C9E0`, `FUN_002F3DD0` from `FUN_002F4080`, `FUN_002F5520` from `FUN_0018F1C8` / `FUN_0018F450`, `FUN_002ABF68`
from `FUN_002AC048`. None of them patches its packets after returning (checked: every captured region is unchanged
at the kick).

**The state setters** (2026-10-25, decompiled before / read from the recompiled code's disassembly comments; native as
state ops since FULLY_NATIVE.md step 5a):
- `FUN_002F7908` and `FUN_002F7B30` are the same code: queue FOG (6) / FOGCOL (7) / registers 0x14 / 0x15 by the
  dirty bits 0x80 / 0x100 / 0x20 / 0x40 (values at `0x4882E0` / `E8` / `F0` / `F8`), clear bits 0x1E0, and when the
  queue is not empty send it as the draws do (CNT n + 1, FLUSHE, DIRECTHL n + 1, GIFtag `0x1000000000008000 | n`, the
  pairs) and empty it.
- `FUN_002F7090(m)`: CNT 3, FLUSHE, UNPACK V4-32 x3 -> VU 0x2C, the three qwords at `m` (the texture matrix, VU
  44-46; with too little room it writes them through a null pointer).
- `FUN_001CEF18(vu, m)`: CNT 3, FLUSHE, UNPACK V4-32 x3 -> VU `vu` (the code word `vu | 0x6C030000`), the three
  qwords at `m`; returns 3. No room check.
- `FUN_002F9CE8()`: CNT 0 with NOP, FLUSHA.
- `FUN_002F9FA0(x, y, z)` runs the flush, then an UNPACK of a vector computed with float divides (`/ 65535.0`,
  `* -32768.0`); `FUN_002F53D8(...)` the frame's camera packet: CNT 0 (STCYCL 1 1, FLUSHA), CALL `*0x4882DC + 0x10`,
  dirty word `0x3E1F`.

**The state flush** `FUN_002F8710` (decompiled with `tools/eedecomp.py`, 2026-10-24; native in
`src/game/vcs_rsl_native.cpp`, byte-exact against the original). The dirty word `0x4882A0`, in the order written:
- 0x1 render context `*0x4882A4` changed: `CALL context+0x10`, then each plug-in on the context's list (head
  `context+0x114`, end `context+0xB0`, next `+0x64`) with a callback at `+0x48` is called (args at `+0x54` / `+0x58`);
  0x2: `CALL *0x4882A8`; 0x4: `FUN_001CFAE0` -> UNPACK of 3 qwords; then the plug-ins' `+0x4C` callbacks (bit 0x1) or
  `+0x50` (bit 0x10 alone); 0x11 unless 0x600 are both set: MSCAL 0x75 / 0x4A (the ones bits 0x400 / 0x200 do not
  send themselves).
- 0x2000: three CALLs from the table `0x742AB0[*0x4CD57C]`; 0x8: UNPACK V4-32 x5 -> VU 0x31 from `0x742AE0`;
  0x1000: viewport `0x520920` -> 0x0C (4 qw, MSCAL 0x8F) and `0x520AC0` -> 0x41; 0x800: `0x5208E0` -> 0x08 (MSCAL
  0x8F) and lights `0x5209A0` -> 0x3B (6 qw); 0x400: `0x520990` -> 0x20 and `0x5208A0` -> 0x04 (MSCAL 0x83); 0x200
  (when `*0x4882B0 == 0`): the instance matrix `0x520860` -> 0x00 (MSCAL 0x3C), or a REF to `*0x4882B8` instead.
- GS registers to the queue `0x729510` (`{value, register}` pairs, count `0x488308`; the draw sends them as a
  `CNT` + `DIRECTHL` right after the flush): 0x80 FOG (6) `0x4882E0`, 0x100 FOGCOL (7) `0x4882E8`, 0x20 register
  0x14 `0x4882F0`, 0x40 register 0x15 `0x4882F8`. The word is left with bit 31 only.
- **The plug-in callbacks** (2026-10-25, `VCS_RSL_PLUGIN_LOG=1`: which callbacks the flushes would call, while
  driving; the callers' code read from the recompiled functions): only `+0x50` callbacks (bit 0x10 without 0x1) occur,
  called as `cb(node[+0x34], node[+0x54], node[+0x58])` in the list's order, after the CALLs of bits 0x1 / 0x2 and before
  the MSCALs; their return values are not used. Four functions: `0x1CEF90` = `FUN_001CEF18(vu, src)` (CNT 3: 3 qwords
  -> VU `vu`), `0x40F798` = `FUN_0040F6C8(vu, src)` when the word at `src + 0x90` is set (CNT 9: 9 qwords, and the word
  cleared), `0x2A9538` -> `FUN_002A92C0` and `0x2F4080` -> `FUN_002F3DD0` (immediate geometry with REFs and their own
  program). The first two are ~85% of the callback flushes; the native flush writes them since FULLY_NATIVE.md 5f.
- **Inside a draw, by return address** (2026-10-25): a hook cannot tell "inside a draw" from "after it" by the guest
  stack pointer alone - a draw's caller often calls a helper next, whose own draws run deeper on the stack than the
  first draw did. What runs inside a draw is called from the draw function itself (`FUN_002F7EB0` 0x2F7EB0-0x2F8024,
  `FUN_002F7D58` 0x2F7D58-0x2F7EAC, `FUN_002F80A0` 0x2F80A0-0x2F8250), its flush (`FUN_002F8710` 0x2F8710-0x2F937C),
  the callback wrappers (`0x2A9538`, `0x2F4080`, `0x40F798`, `0x1CEF90`) or `FUN_001CFAE0`: a hook is inside a draw
  when it is deeper on the stack *and* its `$ra` lies in one of those (bounds from `game/vcs/functions.csv`). With
  stack depth alone every later draw of the caller's helper counted as "inside" (items fell from ~31k to ~12k per 10 s
  while driving).
- **Emitters draw models** (2026-10-25): some captured emitters call the RSL draw functions inside them (the draws run
  the original there: their packets belong to the emitter's region). The draws' tags then sit inside the emitter's
  region, in the order the draws were made. A region that takes such a tag into an item must also drop the draw from
  the frame's draw list - the walker matches the list against the chain's tags in order, and one missing tag puts it
  out of step for the rest of the frame: no model packet is ever learned (`not known`), and standing at Fort Baxter no
  model draw became an item at all.
- The buffer's free space is `0xE6670 - FUN_00476770(0x520F90)` (`FUN_00478628`); with too little the flush writes
  through a null pointer (never seen). While driving ~9% of flushes have plug-in callbacks (skinned / effect
  plug-ins) and ~1% bit 0x4; those still run the original.

## 7. Textures: `.XTX`

Confirmed from `LOADSC*.XTX` (263,372 bytes each, 512x512 8-bit):
- `0x00` magic `"xet\0"`; `0x08` file size; `0x0C` / `0x10` offset of the trailing relocation table
  (0x404B0); `0x60` name (`"loadsc0"`).
- Pixels at **0xB0**: 512x512 linear PSMT8 indices (no swizzle). CLUT at **0x400B0**: 256 x RGBA8888,
  alpha 0x80 = opaque, already in **CSM1 order** (index bits 3 and 4 swapped).
- The game uploads both unchanged: CLUT to CBP 13584 (16x16 CT32), pixels to TBP 8960 (TBW 8, T8),
  through a VIF1 DIRECT (BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE GIF tag) and a REF tag whose
  upper half is `NOP; DIRECTHL n` (this split exposed the upstream VIF bug in PS2RECOMP_PATCHES.md).
- The legal screen (copyright text + logo) is also a 512x512 T8 upload.

## 8. Data loaded at boot

- `PS2/GAME.DTZ` = zlib -> 6.6 MB `GTAG` v1 memory image (header: size, then table offsets), loaded
  at `0x016FE800` and relocated by `FUN_00324A50`; main copies ~50 global pointers out of it.
- `MAINLA.LVZ` / `BEACH.LVZ` / `MALL.LVZ` are zlib streams of a `WRLD` image (header `"DLRW"`);
  MAINLA inflates to 0x54F358 bytes at `0x756680` (read into `0xA8FE00`, inflated by
  `FUN_0021FA38`, relocated by `FUN_00324A50`). Sectors then stream from the matching `.IMG`
  through CDSTREAM.
- **zlib 1.2.2** (version string at file offset 0x3A6250; message strings "invalid distance too far back" ...),
  built with the EE's 64-bit `long`: `z_stream` = next_in 0, avail_in 4, total_in 8 (8 bytes), next_out 0x10,
  avail_out 0x14, total_out 0x18, msg 0x20, state 0x24; `inflate_state` = mode 0, ..., wsize 0x2C, whave 0x30,
  write 0x34, window 0x38, hold 0x40 (8 bytes), bits 0x48, lencode 0x58, distcode 0x5C, lenbits 0x60, distbits
  0x64 (mode BAD = 27). **FUN_00184428 = inflate_fast** (OFF = 1, pre-increment pointers) - read from its loads,
  2026-10-25. It is the busiest guest function while streaming: ~12% of guest time in a standing profile window.

## 9. Streaming and sound (CDSTREAM.IRX)

- CDSTREAM.IRX is Rockstar's IOP driver for **both** disc streaming and sound. Three SIF RPC servers
  (IOP threads):
  - `0x65686577` - stream reads. EE client `0x505B48`: fn 1 = read `{channel, EE dest, LSN, -bytes}`
    (size negated), fn 2 = poll (`0xFA` busy, `0` done). The first request is GAME.DTZ (0x217E70 bytes).
    EE side (2026-10-24): **FUN_00221818(channel, dest, lsn, size)** - size in sectors or -bytes - flushes the
    cache over the buffer, calls fn 1 (reply 1 = accepted), then **FUN_00221678(error word)** shows the disc error
    screens (codes 0xFB-0xFF, jump table 0x4ACFA0); **FUN_002218f0(channel)** = fn 2; **FUN_00221968()** = fn 5
    (no arguments, always 0 seen); fn 0 = init `{6}` from FUN_002215a0. Channel 5 carries most reads. Counted in
    boot + 35 s of play: fn 5 x3011, fn 2 x577, fn 1 x336. **Native since milestone 6 step 1**
    (`src/game/vcs_stream.cpp`): the three EE functions replaced, reads copied from the disc image by a host
    thread, a poll waits up to 20 ms for its read (the game polls at once and would otherwise lose a frame per
    read). 14 MB take ~5 ms; boot and LOAD_SCENE teleports as fast as through the IOP (game-bound).
  - `0x65686578` - EE client `0x4CE2C0`, bound by `FUN_0010DCE8`.
  - `0x65686579` - EE client `0x4D3D40`, bound by `FUN_0010E918`. Commands go through
    `FUN_0010E970(cmd, arg)`: buffer `0x4CE300`, the reply's first word is the result. Seen:
    `0xA0000` (count, 0x6D8 = 1752 sounds), `0xA0B00` / `0xA0C00` per-sound queries, `0xA0D00` load
    bank, `0xB0100` bank-load status (1 = loaded).
- `FUN_0010D868` = sound init (skipped if `*(0x481900) != 0`), called from `FUN_002A6330`, which then
  loads a bank and loops `while (FUN_0010DE78() != 1) DelayThread(1000)`.
- On the IOP the load handler (`0x22DC0`) builds `\AUDIO\SET%s_PS.SDT;1` with sysclib `sprintf` and
  spins `while (!sceCdSearchFile()) WaitVblankStart()`. Banks: `AUDIO/SETn/SFXnn_PS.RAW` (ADPCM) +
  `.SDT` (table).
- CDSTREAM's audio thread (entry `0x1FCE8`) waits on a semaphore signalled by libsd
  transfer-complete and SPU2 IRQ handlers (`sceSdSetTransCallback`, `sceSdSetIRQCallback`).
- The bank-load RPC handler (fn `0xA0D00`) signals the reader thread and then polls for completion
  *inside the RPC call* (the runtime's nested-RPC model needed `yieldFromCall` for this).
- Bank load: ~1.7 MB of ADPCM by IOP DMA channel 4/7 into SPU2 RAM; music and dialogue stream
  through voice loops refilled on SPU2 IRQ-address interrupts.
- `DelayThread` (`0x468318`) = `SetTimerAlarm` (EE T0, handler `0x451038`) + `WaitSema`; a fresh
  semaphore per call. libsd `sceSdInit` waits for `STATX & 0x7FF == 0` after reset; STATX bit 7
  (DMA busy) only while `ATTR & 0x30`.
- SPU2 features in use (measured 2026-10-23 with `PS2X_TRACE_SPU2=1` at Fort Baxter, on foot): no pitch
  modulation (PMON 0), no noise (NON 0). Reverb is configured *off* the way libsd does it: both cores route every
  voice to the wet path (VMIXEL/ER `0xFFFFFF`, MMIX `0x0FF0` / `0x0FFC` with the wet gates set), but the work area
  is an 8-halfword dummy at the top of sound RAM (core 0 ESA `0xEFFF8`..EEA `0xEFFFF`, core 1 `0xFFFF8`..`0xFFFFF`),
  ATTR bit 7 (effects enable) is clear (`0xC000` / `0xC041`), EVOL and vIIR are 0. Main volumes `0x3FFF` (direct).
  Gameplay peaks at Fort Baxter are low (~400-1700 of 32767); AutoDMA input is idle on foot.

- **CDSTREAM.IRX's structure** (48,540 bytes, ~6,400 MIPS I instructions; `tools/irxdis.py`, 2026-10-24): the three
  servers are registered with handlers at module offsets 0xDF4 (0x65686577, streaming), 0x36DC (0x65686578) and
  0x57C0 (0x65686579, sound effects); the sound code is ~4,800 instructions (module 0x0F00-0x5A00). It drives the
  SPU2 only through libsd - no direct SPU2 register access: SetParam x36, VoiceTrans x7 + VoiceTransStatus x8
  (bank / stream data into SPU2 RAM), SetSwitch x7 (key on / off), SetCoreAttr x6, SetAddr x5, GetParam x1 (ENVX),
  Set{Trans,IRQ}Callback, Init; cdvdman (Read / Sync / SearchFile / GetError / DiskReady / Break ...) for its own
  reads; 7 threads, 8 semaphores. libsd's import ordinals (LIBSD.IRX's order, 4 = sceSdInit) were told apart by the
  call arguments (GetParam(core | voice << 1 | 0x500 = ENVX), 36 SetParam sites).
- **What the EE asks of the IOP** (`PS2X_TRACE_RPC`, boot + 35 s of play including driving, 2026-10-24): only
  CDSTREAM's three servers. 0x65686579 (sound effects) 23,901 calls in 46 functions: `0xA0A00`-`0xA0A2B` (0xA0A07,
  0xA0A09, 0xA0A2B ~950 each), `0xA0B00` / `0xA0C00` x7722 each (per-sound queries), `0xB0000` x3016, `0xB0100`
  x1908 (bank status), `0xA0D00` x3 (bank loads), `0xA0000` once. 0x65686578: 6,595 calls - `0xB00` x3022,
  `0x80E00` x3207, `0xC00` x224, `0x80700` x120, `0x80000`-`0x80221`, `0x90000`-`0x90400` a few. No PADMAN /
  MCSERV RPCs in play (pads and the card don't go through RPC here). All nine IRX modules run as IOP code.

### 9a. The sound engine, read from CDSTREAM.IRX (native since milestone 6 step 2, 2026-10-24)

How it was read: `tools/irxdis.py --decompile` (headless Ghidra on the module as a raw MIPS image, import stubs and
exports named, `tools/ghidra/DecompileAll.java`) for the logic, the plain listing for register-level detail, and the
SPU2 write traces of the emulated IOP (`PS2X_SPU2_TRACE`) as the final word. **Ghidra drops stores to hardware
registers it thinks are dead** (a write followed by another to the same address, e.g. libsd's `ATTR &= ~0x30` before
the voice reset) and reorders stores (SPDIF: the IRX writes 0x7C8, 0x7C0, 0x7C6) - the traces caught both. Offsets
below are module offsets (CDSTREAM loads at IOP 0x1D600 on the emulated IOP); "data" is module data.

- **Replies**: both servers answer with a pointer to one result word (0x8160 effects, 0x7FD0 music); the EE asks for
  64 bytes (1444 for `0xB0000`) and only the first word means anything. Commands that return nothing leave the old
  word in place (`0xA0F00`, `0xB0000`, `0xA0200`; music `0xB00`, `0xC00`, `0xD00`, `0x100`, `0x90400`, `0x90500`).
  The request's first word is the argument; `cmd & 0xFF` is a second one.
- **Effects server 0x65686579** (handler 0x57C0): `0xA0000` init(extra sounds) -> 1; `0xA0200` shutdown (key off
  all); `0xA0A00+v` voice v playing (ENVX != 0); `0xA0B00` / `0xA0C00` a sound's length in samples (bytes x 7 / 4) /
  sample rate; `0xA0D00` request bank (arg >> 8 = bank, arg & 0xFF = 0xFF to load); `0xA0F00` effects volume
  (signed byte, 0x7F); `0xB0000` the voice block; `0xB0100` bank status (1 loaded, 2 loading); `0xB0200` / `0xB0300`
  is a special sound ready / load one (arg >> 8 = sound, & 0xFF = slot type).
- **The game decides by `0xA0A00+v`** whether an effect voice is still sounding (ENVX != 0, ~150 queries a second
  while driving): an engine loop it believes stopped is keyed on again. A native port that read the wrong voice's
  envelope (`index | core << 1` for libsd's `core | index << 1`) made the bike's engine restart every frame (a short
  sample looping fast, sometimes silence at speed) and cars play rev samples while slowing (found 2026-10-24 by the
  user; 676 key-ons vs the IOP's 156 over the same ride, 91 back-to-back restarts vs 0).
- **Vehicle skid sounds: `FUN_001BA600`** (cAudioManager::ProcessVehicleSkidding; found 2026-10-24 through the float
  constants 13000.0 / 35000.0 of the pitch formula; a1 = cVehicleParams: +0 type, +8 squared distance, +0xC vehicle,
  +0x10 transmission, +0x18 speed along the vehicle's forward axis). Vehicle types 0 / 9 have four wheels (count of
  wheels on the ground +0x7FC, gas pedal +0x800, wheel states +0x804), 6 / 8 (bikes) two (+0x668 / +0x66C / +0x670);
  wheel states 0 normal, 1 spinning, 2 skidding, 3 locked. Per wheel: drive type (transmission +0x5C, 'F' / 'R' /
  '4') picks `FUN_001BA9A8` for driven wheels (spinning: (gas - 0.4) x 1.667; locked / skidding: max of that and
  |speed| / max velocity (transmission +0x50)) or `FUN_001BAB18` for the others (skidding only), both raised to
  |turn speed z| x 20 (vehicle +0x78), capped at 1. The largest x 55 is the volume before distance (40 m). Sound by
  the surface under the vehicle (+0x137 - 2, jump table 0x4A7EA0): grass / hedge sound 239 (a hiss) at
  35000 + 13000 x skid, 1/4 volume; gravel / sand sound 175 at 5000 + 3000 x skid; everything else (tarmac) **sound
  260, the screech, at 11000 + 5000 x skid, +2000 for bikes**. A bike at 120 km/h held on R1 and turned: front wheel
  skidding, rear locked, screech at volume ~36 (gas held) / ~25 (gas off), pitch 16,000-18,000, as long as the skid
  lasts (`VCS_SKID_LOG=1` logs the inputs for the player's vehicle).
- **Init** (0x3B10): `sceSdInit(0)`, SPDIF mode 0x800, main volumes 0 then 0x3FFF, a 128 KB IOP buffer, then every
  bank's `\AUDIO\SET<b/10>\SFX<b>_PS.SDT` read into one sound table: **12 bytes per sound** - offset in the bank's
  .RAW, bytes, sample rate - indexed from bank b's first sound (68 constants written by 0x3B10 to data 0x9FC0; last
  bank sized by the init argument: 1). ADSR of all 48 voices 0x0F0F / 0x1FC7.
- **The voice block** (`0xB0000`, 1444 bytes, sent every frame): words 3-4 key-on mask per core, 5-6 key-off mask
  per core, then eight arrays of 44 words from word 9: sound, bank, -, -, rate (Hz), pan (0-127), volume, -. Voices
  0-23 = core 0, 24-43 = core 1 voices 0-19 (core 1 voices 22 / 23 are the music). Applied by the thread 0x4B8C:
  key off; per voice: start address = sound offset + the bank's SPU2 address + 0x30 (past the VAG header) - a voice
  whose bank is not loaded is not keyed on -, pitch = rate x 4096 / 48000, volume L / R = min(2 x vol x (127 - pan) /
  127, vol) / min(2 x vol x pan / 127, vol), times the effects volume; then key on (KOFF + KON of the same mask).
- **Banks** (68; SPU2 address table in data 0x8000): 0-1 at 0x79810, 2 = specials at 0x21810, 3-0x27 "level" banks
  sharing 0xD010 (loading one unloads the others), 0x28-0x3C "building" banks in two slots at 0x21810 (the quieter
  slot is replaced; the loader prints "BUILDING BANKS NOT USED"), 0x3D-0x42 one area at 0x6F010, 0x43 specials.
  Loading (thread 0x50DC): the .RAW through the 128 KB buffer, `sceSdVoiceTrans` on core 0 in 128 KB pieces.
- **Special sounds** (speech, mission audio): sounds 0x125-0x16BA come out of bank 2's .RAW, sounds above 0x1750 out
  of bank 0x43's, one at a time, into a slot by type: 0-4 fixed (0x3A810, 0x45010, 0x4F810, 0x5A010, 0x64810), 5-7
  at 0x74010, 0xFF a ring of 5 at 0x21810 + n x 0x5000 (voices find a ring sound among the last two loaded).
- **Music server 0x65686578** (handler 0x36DC, thread 0x26E8): `0x80000` init (finds the 112 streams - names at
  data 0x6410, 40 bytes apart: radio stations, ambience, news, cutscene audio; SLES builds swap in WAVE_EU.VB) ->
  the thread id; `0x80200+n` open stream n at arg ms; `0x80700` stop; `0x80E00` state; `0x90000` a stream's length in
  ms (bytes x 7 / 256); `0x90100` position in ms; `0x90200+n` preload n held (cutscenes) / `0x90300` start it;
  `0x90400` mono; `0x90500` loop; `0xB00` volume at the next step; `0xC00` volume now (left voice gets the high
  half, right voice the low half); `0xD00+core` a core's main volume.
- **Streams** (.VB): stereo ADPCM at 32 kHz, interleaved in 8 KB chunks (L R L R ...). Two 256 KB IOP buffers read
  from disc; two 16 KB SPU2 rings at 0x5010 (left) / 0x9010 (right) played by core 1 voices 22 / 23 as loops. The
  IOP marks the ADPCM block flags in each 8 KB chunk before sending it (first block 6 = loop start or 2, last block
  of the ring 3 = jump back). The IRQ address sits at the start of the half that just started playing: the IRQ
  (every 448 ms) refills the other half - left, then right on its DMA completion, then the IRQ moves. A start
  position in ms maps to a 16 KB-aligned offset piecewise over eighths of the file (0x1524).
- **The music thread** is a state machine driven by one semaphore (max count 1) signalled by commands, DMA
  completions (`sceSdSetTransCallback`) and the SPU2 IRQ (`sceSdSetIRQCallback`): 0x80200 open -> fill both disc
  buffers -> 0x1000 / 0x7000 / 0x2000 / 0x8000 send the four ring halves -> 0x3000 IRQ armed -> 0x4000 preloaded ->
  0x5000 playing (keys on, volume, IRQ enable) -> 0x6000 stop. A stop request takes effect at the next step.
- **Timing the game relies on**: after opening a radio station the EE sends the station's volume 3 samples later;
  on the IOP the preload's four 8 KB DMAs take ~22 samples, so the volume is in before key-on. A preload done
  instantly started the radio at the old volume (0) for one refill period (found by the replay comparison).
- **libsd** (LIBSD.IRX, `src/sound/libsd.cpp`): entries are `core | voice << 1 | param << 8` (+0x80 for per-core
  volumes, core stride 0x28 instead of 0x400); the register table is data 0x4CA0. `sceSdSetCoreAttr` 2 / 4 / 6 =
  ATTR bit 7 / 6 / 14, 8 = noise clock, 10 = SPDIF. VoiceTrans mode 0 = TSA, ATTR DMA-write mode (0x20), DMA; the
  DMA interrupt clears ATTR & 0x30 and calls the channel's callback. The SPU2 IRQ handler clears the IRQ-enable bit
  of the core that hit, then calls the IRQ callback. `sceSdInit(0)` resets the hardware, writes a silent looping
  block at 0x5000 through the data port, points every voice at it, keys all on and off.

### 9b. Running without the IOP (milestone 6 step 3, 2026-10-24)

- With streaming and sound native the EE's only remaining IOP call is the stream server's fn 0 at boot (CDSTREAM
  0x0000: read queues + CD thread; the IOP's own sound code reads through its channels 2 and 3, so with the emulated
  IOP on this call must still reach it). `retireIop` (`src/game/vcs_stream.cpp`) answers it natively and switches
  the emulated IOP off: no IRX module is loaded or run (loads report success), no IOP time is emulated.
- Pads and the memory card never went through the IOP here (EE-side HLE in the runtime), nor did the EE's own
  `cdrom0:` file access. IOPRP300.IMG is never needed.
- Boot to gameplay from the rig card: 4.9-6.1 s with the IOP off, on with the native paths, or fully emulated (the
  game is the limit). IOP time per frame: 0.2 ms off, 0.3-0.4 ms emulated.

## 10. Memory card and saves

libmc calls are HLE (`Kernel/Stubs/MemoryCard.cpp`); files live in `%LOCALAPPDATA%/VCSRecomp/mc0`
(`src/main.cpp`), one host folder per card folder.

### 10a. Card manager

- Card manager object `0x510480`, one 0x4C0-byte block per port. Status word `+0x54` reads 27 (0x1B)
  when idle / the last operation was fine, 14 (0xE) after "no entry", 1 after "no format".
- `FUN_00275068(mgr, port)` / `FUN_002760d8(mgr, port)` list the current folder:
  `sceMcGetDir(port, 0, "*", 0, 15, mgr + port*0x4C0 + 0x100)` (path string at `0x4B0148`), then
  `sceMcSync(0, ...)` - **synchronous**, waits for the result. The 15-entry table (64 bytes per
  entry, name at +0x20, attributes at +0x14, size at +0x10) is what everything else reads.
- `FUN_002788e0` is the save / load state machine (states 0..5 at `0x487730`, error at `0x487744`,
  open fd at `0x487748`); it builds paths with `sprintf` (formats at `0x4B03D8` / `0x4B03E8`) and calls
  `sceMcGetDir` with 1 or 20 entries for single files.

### 10b. Save folder and slot files

- `FUN_002778a8(mgr, slot)` (from the save menu's "save to slot", `FUN_00108128`, slot byte at
  `0x4818AC`) creates `BASLUS-21590GTA40001/` with `icon.sys` and `vcsicon.ico` if missing, then
  writes the slot file and the `BASLUS-21590GTA40001` data file (179,350 bytes). Order of creation
  on a fresh card (host creation times): `icon.sys`, `vcsicon.ico`, `BASLUS-21590GTA40001`, then the
  slot file.
- Slot files are named `"<slot> <mission name>"` (e.g. `1 In the beginning`, `1 Soldier`), 98,304
  bytes; the data starts with the block magic `"SIMP"`. Saving over an existing slot (checked 2026-10-23 with
  `VCS_TEST_SAVE` on a copy of the user's card): the game lists the folder, reads both slot-1 headers, issues two
  `sceMcDelete`s (both succeed), then opens `1 Soldier` with flags 0x203 (create | write | truncate), writes
  98,300 + 4 bytes and rewrites `icon.sys`. No new file appears; a stray older `1 ...` file (made before the
  directory-order fix) is left alone.
- `FUN_00277688(mgr, slot)` checks the card and free space (0 ok, 2 not enough space, 3 error); the
  game calls it at boot.

### 10c. How the game finds its saves - card directory order matters

`FUN_00276bf0` (loop at `0x276da0..0x2772dc`) scans the listing for **8 slots**: for slot s (1..8)
it walks **table entries 5 to 14**, and takes an entry when its name is non-empty, its attributes
have bit 0x80 (closed) set, its size is > 0x17FFF, and its first character equals `sprintf("%d", s)`
(format at `0x4B00A8`). Then it reads each slot file's first 268 bytes; with two files for slot 1
on the user's card it read both headers and then loaded the newer one (`1 Soldier`) in full.

It **skips entries 0-4**, assuming they are `.`, `..` and the three files created with the folder.
That is only true in a real card's **directory-table order** (creation order; a deleted entry's slot
is reused by the next file created). Our first `sceMcGetDir` sorted names alphabetically, which put
`1 ...` at index 2 - so saves were written but never found: no load at boot, an empty Load Game
list, and a new save to slot 1 created a second `1 ...` file next to the old one (found 2026-10-22
from the user's card: `1 In the beginning` and `1 Soldier` side by side). Fix: each host card folder
keeps its order in a hidden `.ps2x-order` file (PS2RECOMP_PATCHES.md).

### 10d. Loading

- At boot the game reads `icon.sys` (964 bytes), lists the folder, reads every slot file's header
  and then the newest slot file in full (98,300 + 4 bytes), showing "Loading data..." and "Load
  Successful." before the world loads at the save point.
- `FUN_003adae0()` (generic load) runs from world init `FUN_00255308` when a load was requested; it
  reads through `FUN_002759e8` (`sceMcRead`).
- `VCS_TEST_SAVE=<s>` (`src/game/vcs_overrides.cpp`) runs the save sequence as guest tail calls from
  `scePadRead`. A direct host call of a guest function is not safe: a scheduler checkpoint inside it
  unwinds the call and the rest of the routine never runs.

### 10e. The save / load / delete menus (driven 2026-10-23, `tools/saves.py`)

- **Save Game** is opened by scripts with `ACTIVATE_SAVE_MENU` (opcode 0x0260, no arguments; handler entry at
  `0x4BFDF0` in the opcode table) - the rig can open it anywhere. "Save the current game?" -> Save Game -> the
  8-slot list ("1 Soldier  22 Oct 2026 23:31:27", "Save File 2 Not Present" ...) with the cursor on **Cancel**;
  an empty slot asks "Are you sure you want to save this game?", an used one "Proceed with overwriting this saved
  game?" (both default to **No**), then "Save Successful. Select OK to continue."
- Saving advances the clock (12:02 -> 22:00 at Fort Baxter). Loading that save then starts the story's next
  mission intro (script `ch_jerr`, "Take Sgt. Martinez's bike") - game flow, not a load problem; the original
  Fort Baxter save loads straight into the barracks.
- **Load Game / New Game / Delete Game** are on the pause menu's *Game* tab (Map, Brief, Game, Stats, Controls,
  Audio, Display). Load lists the slots with their dates, asks "All unsaved progress in your current game will be
  lost. Proceed with loading?", and reads the slot file in full (98,300 + 4 bytes). Delete asks "Proceed with
  deleting this saved game?" and ends with "Delete Successful"; the file and its `.ps2x-order` entry go.
- With two files for slot 1 on the card (the stray `1 In the beginning` from before the directory-order fix) the
  menus show only the newer one.
- At boot the newest slot (by the header date) is loaded automatically; with slots 1 and 2 present it took slot 2.

## 11. Scripts: MAIN.SCM and the script VM

Found 2026-10-22 for the automation rig (`src/debug/rig.cpp`, `tools/rig.py`) from a RAM dump at the Fort Baxter
save (`rig.py` `dump`), a read watch on the script (`VCS_RWATCH`), and disassembly. Opcode *names* come from the
Sanny Builder Library's VCS database (`vcs.json`); every number we use was checked against the handler table
below (e.g. 0x0023 terminates, 0x0289 loads a mission, 0x0238 names a script).

### 11a. MAIN.SCM layout

`MAIN.SCM` (root of the disc, 1,876,562 bytes):
- Header: u32 main-section size (0x3CAE3), u32 largest mission size (0x96A9).
- From file offset 8 the script itself; segments are separated by `GOTO` (opcode 0x0002, int32 target):
  globals (file 0x0F..0x63F4) -> segment 2: u8 0, u32 count 1092, 1092 x u16 (a model-id table; searched by
  `FUN_00255188`) -> segment 3: u8 0, u16 0x14E2, u16 0x0419, u32 0x96A9, u32 **101 missions**, 101 x u32 mission
  start IPs -> main code from file 0x7814 (`SCRIPT_NAME "MAIN"`).
- Missions follow the main section in the file; mission n is IP `table[n]`..`table[n+1]` (file offset = IP + 8),
  loaded on demand. Names (their `SCRIPT_NAME`): 0-5 `INITPU INITCG INITGA INITOB INITPLA INITGEN` (the main
  script launches these six at start), side activities (`PARAMED VIG FIRETRK CHOPLIF TAXI` races, trials...) and the
  story chains `JER_A* PHI_A* MAR_A* LOU_A*/B* LAN_B*/C* UMB_B* BRY_B* MEN_C* REN_C* GON_C* DIA_C*` (45..100).
- `INITPLA` creates the player: `CREATE_PLAYER 0` at (-1710.7, -306.7, 14.86) into **global 0x2A5**, then
  `SET_CHAR_HEADING` on that same global - VCS uses one handle for the player and the player's char.

### 11b. In memory

| What | Where |
|---|---|
| ScriptSpace | `*(0x48741C)` = `0x1DE0800` = MAIN.SCM file offset 8; an IP is an offset from it |
| Model table / mission table (segments 2, 3) | `*(0x487420)` (count `0x4CD3CC`); mission IPs copied to `0x7404D0[101]` by `FUN_0025b420` |
| Mission count / largest mission size | u16 `0x4CD3EE` (101) / u32 `0x4CD3E8` |
| Mission space offset | `*(0x4CD3D8)` (where `LOAD_AND_LAUNCH_MISSION_INTERNAL` reads the mission to) |
| A mission is running | u8 `0x4CD3E7` |
| MAIN.SCM file handle | `*(0x487418)` (kept open; missions are read from the disc) |
| Script id counter | `0x48742C` |
| Active / idle script lists | `0x4CD3C4` / `0x4CD3C8` (doubly linked: +0 next, +4 prev; insert at head) |
| Script parameters (CollectParameters output) | `0x50DEF8` |
| Opcode table | `0x4BEAF0`: 8-byte GCC pointer-to-member entries {s16 delta, u16 valid, u32 handler}, 1381 opcodes, 1338 with handlers |

`CRunningScript` (0x218 bytes, pool around `0x738948`): +0 next, +4 prev, +8 id, +0x10 IP, +0x14 16-word stack
(gosub), +0x54 0x6A-word local variables (timers at locals 0x68 / 0x69), +0x1FC local frame base, +0x200 wake
time, +0x208 active, +0x20A mission script, +0x20C NOT flag of the current condition, +0x20D, +0x20F name (8
chars; `Init` writes `"id%02i"`), +0x217 "is the mission" (cleared by terminate).

Functions: `FUN_002551d0` Init, `FUN_00255950(ip)` StartNewScript (idle -> active, id, Init, IP, +0x208 = 1),
`FUN_002558f0` / `FUN_00255928` list remove / add, `FUN_002560d8` ProcessOneCommand (u16 opcode at IP, bit 15 =
NOT, dispatch through the table), `FUN_00254b90(script, &ip, n, out)` CollectParameters, `FUN_00254ff8` variable
address, `FUN_003b2208` LOAD_AND_LAUNCH_MISSION_INTERNAL (seek MAIN.SCM to `table[n] + 8`, read into the mission
space, StartNewScript, mark it the mission, clear the mission locals), `FUN_003b1d68` TERMINATE_THIS_SCRIPT.

58 scripts run at the Fort Baxter save: `storysc` (story), `pager`, `rampage`, `usj` (unique jumps), the
vehicle-mission monitors (`paramon`, `vigmon`, `firemon`, `taximon`...), `ga_*` (gang), `safehou`...

### 11c. Bytecode encoding

- Opcode: u16 little-endian; bit 15 = NOT for conditions.
- Argument = type byte + data (`FUN_00254b90`, jump table `0x4AE120`): 0 end of a variable argument list,
  1 / 2 zero (int / float), 3 / 4 / 5 float with only its top 1 / 2 / 3 bytes stored, 6 int32, 7 int8, 8 int16,
  9 float32, 10 NUL-terminated string. A byte >= 0x0B is a variable (`FUN_00254ff8`): 0x0B / 0x0C the two timers,
  0x0D-0x6C local variable (byte - 0x0D + frame base), 0x6D-0xCC local arrays, 0xCD-0xE5 global
  ((byte - 0xCD) << 8 | next byte; address ScriptSpace + 4 * index), >= 0xE6 global arrays.
- Example (the rig's teleport): `EA 02 01` SET_AREA_VISIBLE 0, `53 02 09 x 09 y 09 z` LOAD_SCENE,
  `44 00 CF A5 09 x 09 y 09 z` SET_CHAR_COORDINATES $PLAYER (z <= -100 finds the ground), `23 00` terminate.

### 11d. Running our own scripts (the rig's `scm` command)

A host-side copy of StartNewScript at the frame kick (between frames, never during script processing) takes an
idle script, initialises it, puts our bytecode in its own locals 0x30.. (`+0x114`, 176 bytes; locals 0..0x2F
stay free for outputs) and sets the IP to that address minus ScriptSpace (the interpreter adds them with 32-bit
wrap-around, so code outside the script space works). The game's interpreter then runs it like any script; it ends
with `TERMINATE_THIS_SCRIPT` and the rig answers when +0x208 drops to 0. Missions start the same way, with the
game's own `LOAD_AND_LAUNCH_MISSION_INTERNAL` (refuse while `0x4CD3E7` is set).

## 12. Player, areas and the world

- **Player ped**: `*(0x4E4910)` = `CWorld::Players[0].m_pPed` (`FUN_00237fb0(ped)` returns `&Players[0]` for the
  player's ped). The ped object starts with its matrix: right (+0), forward (+0x10), up (+0x20), position (+0x30),
  each row with a word after it. Found by searching RAM for vectors near the camera that moved when the player
  walked (`findvec`), then the pointers to the object (`findu32`).
- **TheCamera** `0x6F44D0`: +0x30 is the point it follows (the player); it also holds pointers to the player ped
  (+0xE0, +0x350, +0x7F8).
- **Current area** (interior): `CGame::currArea` = `0x489F7C`, set by `SET_AREA_VISIBLE` (opcode 0x02EA, handler
  `0x1e31a8`, then `FUN_003cc0d0`). 0 = outdoors. The Fort Baxter save loads with **area 2** (the barracks), so a
  teleport outside must set area 0 first (the user's guess, confirmed by reading the global). VCS has no
  SET_CHAR_AREA_VISIBLE in the opcode database.
- Teleports are clamped to the map: about x -2178..1564, y ..1926.
- **Ped teleport**: `FUN_0020b3d8(ped, CVector *pos)` (remove from the world via `FUN_0018fa50(ped, 2)`, set the
  position `FUN_00472688`, clear movement state +0x1c8 / +0x814..+0x81c / +0x150, add back `FUN_0018f8f8`); reached
  through a vtable, every caller seen goes through `0x4127d0`. `VCS_TRACE_TELEPORT=1` logs each call.
- **The safehouse script moves the player back once**: after loading the Fort Baxter save (inside the barracks
  safehouse, area 2) and teleporting away, the *next* teleport far from Fort Baxter is followed by two teleports
  done by the script `safehou` (IP 0x20E86, then 0x20EA4): to (-1648.87, -262.50, 15.94) and (-1648.87, -261.00,
  14.50) - the barracks door. Once, whatever the destination; later teleports land. Found with a write watch on the
  ped's x (`VCS_WATCH`, pc `0x47268c` inside the teleport), then `VCS_TRACE_TELEPORT`, which hooks the interpreter's
  ProcessOneCommand to name the script that was running. First mistaken for entrance markers. The rig's
  `Game.prime()` triggers it on purpose before working. The world tour's other "relocated" points are the map-edge
  clamp.
- While the ped sits in a vehicle its own matrix is not its world position (it read (-0.4, 0.0, 21.6) in a car
  parked at (-596, 600)); use the vehicle's.
- `CREATE_CAR` (0x0048, handler `0x3d03b0`): 4 parameters (model, x, y, z; z = -100 finds the ground) plus the
  handle output (e.g. 0x1201 = vehicle pool slot 0x12). Vehicle model ids used by MAIN.SCM: 171..276 (211 most).
- `SET_CHAR_COORDINATES` with z = -100 puts the player on the ground (or a roof: (-1400, 1250) landed at z 84).

## 13. Open questions / next research targets

- Main loop, frame limiter, CTimer (section 4) - milestone 4 (60 fps).
- Vehicle structure, "in vehicle" / "in water" ped flags.
- After dying (hospital respawn, clock jumps to ~01:25) the world around the hospital was not drawn for ~40 s,
  only lights (autodrive soak 2026-10-23) - streaming or rendering, to find out (milestone 2).
- Mission-passed flags (storyline globals) - for reaching late-game states.
- Cheat handlers (weapons, health, vehicles, weather) - the script commands cover these for now.
- Collision: VU0 program 0 (section 6c), collision streaming - the fall-through bug.
- HUD / font / sprite call sites (CHud, CFont, CSprite2d, CSprite) - milestone 3 (2D at the RW level).
- Sound driver command protocol in full (section 9) - milestone 6.
- Threads and their priorities.
