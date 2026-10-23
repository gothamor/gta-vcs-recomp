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
- [x] GS usage survey (`VCS_GS_STATS`): see docs/game-internals.md
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
- [x] Audio: reverb, noise, pitch modulation, Gaussian interpolation, exact volume sweeps / ADSR timing
      (2026-10-23, from psx-spx; PS2RECOMP_PATCHES.md). VCS keeps reverb off and uses no PMON / NON in gameplay
      (game-internals.md 9), so what is audible is the interpolation and the envelopes
- [x] Memory card save: the game's own save routine writes its folder, icons and slot file
      to `%LOCALAPPDATA%/VCSRecomp/mc0` (verified with `VCS_TEST_SAVE`); boot finds the save
- [x] Memory card load (2026-10-22): the game finds its saves at start-up and loads the newest
      ("Load Successful", back at the save point) since `sceMcGetDir` lists folders in card order
      (PS2RECOMP_PATCHES.md); before, saves were written but never seen. Saving over a slot replaces its
      file (2026-10-23). Save (slot 2), Load Game (slot 1) and Delete Game through the game's own menus all work
      (2026-10-23, `tools/saves.py`, regress step "saves"; game-internals.md 10e)
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
      DMA chain fix; not investigated yet). Still seen by the user after the native renderer
      (2026-10-22): objects and the world turn black for a few frames. To check first: whether
      it follows native draws (PS2X_HLE=0 run for comparison), the prelit colour stage inputs
      (0x9c-0x9e tint) and lighting constants at those moments, texture cache misses, and
      render-target drops (RtDrop) around the black frames.
- [x] Water surface looks missing (dark grey where sea should be): the sea draws (turquoise, waves) in the
      2026-10-23 tours - gone with the render-target fix (milestone 2)
- [ ] Movies
## Milestone 1 - automation and coverage rig (NATIVE_PORT_ROADMAP.md), started 2026-10-22
- [x] Control-channel extension (`ps2control::addCommand`) and the rig (`src/debug/rig.cpp`, `tools/rig.py`):
      memory peek / poke / search / dump, pause, frame counter
- [x] Script VM mapped (game-internals.md 11): `scm` runs our bytecode as a game script; teleport (area, scene
      load, ground find), time, weather, invincibility, health, mission launch (game's own loader) all work
- [x] Player ped, current area (interior) found (game-internals.md 12)
- [x] World tour (`tools/tour.py`): grid over the map, classes ok / water / relocated / falling / dark / slow /
      error, map image, screenshots, perf; restarts the game on a crash
- [x] Brightness probe (`PS2X_FRAME_STATS=1`, on with the control channel): every presented frame's middle
      region shrunk to 32x24 and read back; perf line `black_frames` / `dip_frames`; log lines
      `[frames] black run ...` (whole picture black) and `[frames] dip ...` (much darker than the scene for up to
      2 s). Normal walking / running: 0 of ~3000 frames; driving: one-game-frame full-black flashes seen
      ("dip of 2 host frames ... scene luma 113, lowest 0.9") - the user's black-flash bug, now measurable
- [x] Mission smoke test (`tools/missions.py`): each mission launched from a fresh load, screenshots, perf
- [x] Drive loop (`tools/drive.py`): car spawned by script, driven with the steering alternating
- [x] Regression suite (`tools/regress.py [--set-baseline]`): scenes + tour subset + missions + drive against a
      baseline, report.md
- [ ] Soak runs; in-process falling detector; unconverted-draw counter; map image with the land outline

## Milestone 2 - correctness sweep (started 2026-10-23)
Findings so far (rig runs; details in game-internals.md):
- [x] **Evening black frames** (the user's "world turns black"): fixed 2026-10-23 - render targets were tracked
      taller than the scissor, so the Z-area texture the game uploads at night dropped the picture
      (game-internals.md 6g). Regression check: `tools/regress.py` step "evening".
- [x] After a death the world near the hospital drew only lights for ~40 s - it was the evening black frames
      (fixed with them; checked 2026-10-23: drawn from the first frame after the respawn fade).
- [ ] Frame time in busy scenes: 35-39 ms (game code ~18 ms, render submit ~16 ms), drive loop mean 39 ms (was
      54 before the render-target fix). Profile (busy street, 2026-10-23): VIF1 processing 8%, T4 texture
      swizzle 7%, scheduler checkpoints 4%, the VU1 interpreter for the programs still on it (sprites 0x02c0, the
      env-map pass) ~8% together - milestones 3 and 7.
- [x] The VU path's darker noon picture was another symptom of the dropped render targets: after the fix native
      and VU path agree (hospital, luma 90 / 95 at 12:00, 73 / 76 at 20:00).
- [x] Black flashes at teleports / car warps are the game's own load transitions (not bugs).
- [x] "Relocations" after loading: the safehouse script (rig.Game.prime).
- [x] Sea: drawn correctly everywhere the full tour went (240 points, 2026-10-23); the old "dark grey sea" was
      the dropped render targets too.
- [x] Audio: the missing SPU2 features are implemented (reverb, Gaussian interpolation, PMON, noise, exact
      envelopes / sweeps) with unit tests; the game itself uses none of reverb / PMON / noise (traced over
      gameplay and missions 36-54: 0 of 532 samples) - it is a PSP game first.
- [x] Saves: save to another slot, Load Game, Delete Game through the game's own menus (`tools/saves.py`, regress
      step "saves"); a card written by older builds (the user's) loads in the current one.
- [x] Full mission smoke test (`tools/missions.py`, missions 6-100, 2026-10-23): 95 / 95 ok (started, no crash or
      hang, pictures); dark shots were fades and night cutscenes except one bug:
- [x] **GON_C3 poolside ground black** on the native path: GPU clipping of the packets the VU sends through its
      clipper. Clipped world packets stay on the VU1 by default now (`PS2X_HLE_CLIPPED=1` for GPU clipping); cost
      flyover 30 -> 25.5 game fps (render 18.2 -> 24.6 ms), cutscene / gameplay still at 30. Reproducing the VU's
      per-triangle clipper natively is milestone 3 (game-internals.md 6h).
- [x] Soak (`tools/soak.py --hours 0.5`, 2026-10-23; the user settled on a short one): 60 samples over 30 min of
      autodrive across six spots, no crash, hang or falling; private memory 347 MB at start, then a saw-tooth between
      486 and 560 MB from minute 10 on (streaming), no drift. Driving frame time median 46.8 ms, p90 70.7 ms. (The
      "6-8 ms worse with clipped packets on the VU1" read from this was wrong: a same-session A/B of `drive.py --auto`
      gave 54.8-57.1 ms mean for GPU, VU1 and C++ clipping alike - driving runs vary more than the clipping costs.)
      One car per leg spawned against something and was replaced (the tool's retry).

Milestone 2 closed 2026-10-23. Open, carried to milestone 3: the VU clipper reproduced natively, bloom buffers
dropped on uploads (lazy render-target patching).

## Milestone 3 - native rendering, the rest of the frame (started 2026-10-23)
- [x] **The VU1 clipper in C++** (`src/game/vcs_vu_clipper.cpp`, game-internals.md 6h): bit-exact against the VU's
      XGKICKs (GON_C3 1.2M GIF packets, reference scenes 2.8M, 0 off); clipped world packets drawn from it. Section
      means over the reference scenes (C++ / VU1 / GPU clipping): flyover render 14.6 / 17.7 / 14.1 ms, cutscene
      11.8 / 14.5 / 11.3, gameplay 5.9 / 7.6 / 5.6; flyover frame 34.3 ms (30 fps cap) vs 36.2 on the VU1. GON_C3's
      poolside ground right natively.
- [x] **Environment-map pass** (stage table 0x13, vehicles): reproduced and checked against the VU's kicks (27,243
      packets exact); native by default (`PS2X_HLE_ENV=0` = VU1). Flyover render 14.7 -> 14.0 ms (same-session A/B);
      pictures identical. game-internals.md 6d.
- [x] Survey reports native and VU1 work apart (`PS2X_HLE_STATS=1` also lists the stage tables left).
- [x] **0x02c0** (instanced re-draws, entry 0x1c; two-pass shine over resident meshes, entry 0x0d) and the **leftover
      prelit packets** (a float code image, "variant C") native: reproduced with their VU memory writes and checked
      against the VU (174,110 packets exact over the reference scenes; two missions and a drive exact too). Only clipped
      environment-pass packets still run on the VU1 (63 in two minutes of scenes). Render time 0.3-0.7 ms lower per
      section; pictures identical (game-internals.md 6d). `PS2X_HLE_REDRAW=0` = VU1. Milestone 3 step 1 done.
- [ ] **Step 2: 2D at the RenderWare level** (started 2026-10-23). Found: Im2D (FUN_002fbe08 / FUN_002fbe68), its
      vertex format, RwRenderStateSet, CHud::Draw (FUN_0031c628) and the 2D pass's order (game-internals.md 6f);
      `src/game/vcs_hud.cpp` hooks them.
  - [x] **16:9 weapon icon at the screen edge** (reported by the user): the icon is drawn off the far plane, so the
        renderer's HUD placement missed it; HUD-context primitives without a depth test now get the HUD's z. Checked at
        16:9 (icon beside the clock / ammo as at 4:3, radar still round) and over the reference scenes (no other
        change). `VCS_HUD_RW=0` = before.
  - [x] **Per-element anchoring**: HUD "Screen edges" (`VCS_HUD=2`) - each HUD element squeezed towards the edge it
        sits near (radar left; clock / weapon / money / zone name right; help text left), the elements found from the
        frame's Im2D calls (game-internals.md 6f, DECISIONS.md).
  - [x] **Native HUD layer**: the HUD's primitives kept out of the PS2 frame buffer and drawn over the shown picture
        at the window's size (NATIVE_RENDERER.md "HUD layer"); on by default, `VCS_HUD_NATIVE=0` = into the frame
        buffer. Textures from the GS backend's decoded cache, not a RenderWare raster decoder (DECISIONS.md).
        Checked with `scenes.py` at 4:3 and 16:9 against `VCS_HUD_NATIVE=0` (build/m3/cmp43, cmp169): same HUD, the
        radar round and clipped, the static scenes (loading 0.95, pause map 0.8-0.9 mean difference) as close as two
        runs of one build; at 16:9 the HUD is sharper (no squeeze-then-stretch). No cost: 4:3 flyover 29.3 -> 30.0
        fps, gameplay at the 30 fps cap in both.
  - [x] **Lines in the HUD at full-screen sizes** (reported by the user at 1080p: a line through the radar's north
        icon, lines under / beside the clock's digits): sub-pixel gaps and atlas bleeding that 640x448 never showed.
        The layer now covers the PS2's pixels and samples inside each quad's own texels (NATIVE_RENDERER.md "HUD
        layer"). Checked at 1920x1080 (`PS2X_HW_OUTPUT`) outside the barracks; at 640x448 the layer still matches the
        emulated HUD.
  - [x] **Menus** in the native layer (pause menu pages; NATIVE_RENDERER.md "HUD layer"): sharper at 1080p, same
        layout and colours (scenes at 16:9 1080p against `VCS_MENU_NATIVE=0`: static scenes the same but the pause
        map, which differs by its sharper text). HUD placement default stays "Keep 4:3"; "Screen edges" is the menu
        choice Display > HUD (user's decision).
  - [ ] Loading screens and the title menu over the flyover: not looked at yet (loading unchanged).
- [ ] Rigid / skinned lit models still use GPU clipping for their clipped packets (no wrong picture seen yet; same
      method when one shows up).
- [ ] Lazy render-target patching (bloom buffers dropped on uploads).

## Phase 5 - stability
- [ ] Vic falls through the world in some places (reported by the user 2026-10-22, after the
      native renderer; not yet known whether it is new). Collision needs a look: whether the
      collision sectors / models are streamed in at those spots (CDSTREAM reads, IMG offsets),
      VU0 programs used by collision (`0x0000` in .vutext looked like line/sphere vs triangle
      tests, docs/game-internals.md "VU microcode"), and float edge cases in the recompiled code
      (FP contraction, round-to-zero, denormals). Compare with PS2X_HLE=0 to rule the renderer out.
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
| `PS2X_TRACE_SPU2=1` | SPU2 key-ons, DMA bytes, IRQs, peak level per 5 s; per core: wet input, reverb output, ESA/EEA, EVOL, PMON / NON use |
| `PS2X_SPU2_REVERB=0` | mute the SPU2 reverb (A/B listening) |
| `PS2X_AUDIO=0`, `PS2X_AUDIO_DUMP=f.wav`, `PS2X_AUDIO_STATS=1` | no audio output / record the SPU2 mix / underrun and fill stats |
| `PS2X_IOP_REALTIME=0` | IOP time purely from EE cycles (audio slows with the game) |
| `VCS_TEST_SAVE=s[:slot]` | save to slot 1 (or 1..8) through the game's code s seconds after boot (mid-script: prefer `tools/saves.py`) |
| `VCS_TRACE_SECTORS=1`, `VCS_TRACE_CELL=1` | world sector draw / current hex cell vs camera |
| `VCS_DUMP_RAM=s,s,...` | write EE RAM to ram_<s>.bin at those seconds |
| `PS2X_DMA_HIST=1` | VIF1 payload per MB of RAM every 5 s |
| `VCS_TRACE_PACKETS=1` | peak use of the game's DMA packet buffer |
| F3 / F4, `PS2X_PERF_LOG=f.csv` | perf overlay / CSV recording (see CONTROLS.md); `python tools/perf_report.py f.csv` |
| `PS2X_HLE_CLIPPED=1` | draw world packets that cross the clip planes with GPU clipping (wrong in places, game-internals.md 6h) |
| `PS2X_HLE_CLIPPER=0` | clipped world packets on the VU1 instead of the C++ clipper |
| `PS2X_HLE_ENV=0` | the environment-map pass (stage table 0x13) on the VU1 |
| `PS2X_HLE_REDRAW=0` | 0x02c0's re-draws / shine passes and the float-image prelit packets on the VU1 |
| `VCS_HUD_RW=0` | no HUD context at the RenderWare level (the 16:9 weapon icon goes back to the screen edge; no native HUD, no anchoring) |
| `VCS_HUD=0/2/1` | HUD in widescreen: keep 4:3 in the middle / screen edges / stretch (the menu's Display > HUD) |
| `VCS_HUD_NATIVE=0` | the HUD drawn into the PS2 frame buffer instead of the native layer at the window's size |
| `VCS_MENU_NATIVE=0` | menus drawn into the PS2 frame buffer instead of the native layer |
| `VCS_TRACE_IM2D_DELAY=s`, `VCS_TRACE_IM2D_STACK=1` | start the Im2D trace after s seconds (default 30); print the guest stack's return addresses per call |
| `VCS_TRACE_HUDKEY=n` | log n 2D primitives the HUD classifier did not find (positions) |
| `PS2X_HW_OUTPUT=WxH` | the renderer's output size whatever the window (full-screen sizes in test runs; with `PS2X_CAPTURE_SIZE`) |
| `PS2X_HW_DUMP_TEX=<key hex>` | write that decoded texture (colour + alpha PPM, keys from the F12 draw log) - analysis only, never commit |
| `PS2X_HW_HUD_LOG=1` | the renderer's HUD layers (per frame buffer, stale or not) and the output size, every 2 s |
| `VCS_TRACE_IM2D=n`, `VCS_TRACE_IM2D_MARKERS=1..5` | log n Im2D calls (caller, primitive, vertices) / the 2D pass's calls, from 30 s on |
| `PS2X_HLE_STATS=1` | native draw counters; 0x0030 stage tables still on the VU1 every 10 s (+ one code / data sample each) |
| `PS2X_CLIP_DUMP=dir[,n]` | with `PS2X_HLE_VERIFY=1`: write n clipped packets (VU memory + the VU's kicks) for offline checks |
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
| `PS2X_HLE=0` | draw the world through the VU1 program instead of natively (src/game/vcs_hle_world.cpp) |
| `PS2X_HLE_VERIFY=1` | compare the C++ world pipeline with the VU1's output (bit-exact expected) |
| `PS2X_HLE_SURVEY=1`, `PS2X_HLE_SURVEY_DUMP=n`, `PS2X_HLE_SURVEY_DUMP_EVERY=n` | VU1 program / layout survey, sample dumps |
| `PS2X_HLE_STATS=1` | stage tables the native handler passes over |
| `PS2X_HLE_LIT=0` | draw skinned / lit models through the VU1 program (world stays native) |
| **F2** (or gamepad Back + Start) | settings menu (docs/SETTINGS_MENU_WIDESCREEN.md); its values live in vcs.ini next to vcs.exe |
| `display.aspect=43\|1610\|169` in vcs.ini, `VCS_ASPECT=...` for one run | aspect ratio (Hor+ widescreen) |
| `display.hud=0\|1` | widescreen HUD: keep 4:3 in the middle / stretch |
| `window.*` in vcs.ini | where the window was last (monitor, position, size, maximized); written by the game, delete the lines to reset (docs/SETTINGS_MENU_WIDESCREEN.md) |
| `python tools/rig.py start --save DIR`, `python tools/rig.py <command>` | automation rig: start from a card copy; peek / poke / findvec / dump / scm / scripts / player / pause |
| `python tools/tour.py [OUT] [--step 250] [--every K]` | world tour over the map (classes, map.png, screenshots, perf) |
| `python tools/missions.py [OUT] [--only 45,46]`, `python tools/drive.py [OUT]` | mission smoke test; drive loop |
| `python tools/regress.py [OUT] [--quick] [--set-baseline]` | regression suite against build/regress/baseline |
| `PS2X_FRAME_STATS=1` | brightness probe without the control channel (black / dip frames in the perf line and log) |
| `VCS_TRACE_TELEPORT=1` | log every ped teleport (FUN_0020b3d8) with its caller |
| `display.scale=<1..8>` (old `scale=` still read) | internal render resolution, as `PS2X_HW_SCALE` (the environment wins) |
| `python tools/vudis.py code.bin --start 0x30 --to-ebit` | VU microcode disassembler |
| `python tools/sendkey.py Return`, `python tools/screenshot.py f.png` | key press / screenshot (control channel if open, else the window) |
| `python tools/eedis.py <elf> <addr or name>` | EE disassembly with function names |
| `python tools/minidbg.py <exe> <iso>` | catch fail-fast crashes with a stack |
