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
- [x] Whole screen goes black for a few frames now and then - **no longer seen** in the user's testing
      (2026-10-24); gone with the renderer / timing work since (which change is not known). The report: (2026-10-22, after the
      DMA chain fix; not investigated yet). Still seen by the user after the native renderer
      (2026-10-22): objects and the world turn black for a few frames. To check first: whether
      it follows native draws (PS2X_HLE=0 run for comparison), the prelit colour stage inputs
      (0x9c-0x9e tint) and lighting constants at those moments, texture cache misses, and
      render-target drops (RtDrop) around the black frames.
- [x] Water surface looks missing (dark grey where sea should be): the sea draws (turquoise, waves) in the
      2026-10-23 tours - gone with the render-target fix (milestone 2)
- [x] Movies (milestone 5, 2026-10-24: played natively)
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
  - [x] **Loading screens** in the native layer (2026-10-24; FULLY_NATIVE.md step 2): `LoadingScreen` (FUN_0021E170)
        opens a context like the menu's (centred, full-width pieces stretched) to its frame kick. Static `loading`
        scene 1.12 against the reference (ok); at 1080p the progress bar and its shadow draw at the output's
        resolution instead of a scaled-up 640x448 frame. `VCS_LOADING_NATIVE=0` = into the frame buffer. There is no
        title menu (game-internals.md 6f: the game boots into the opening sequence; the frontend is the pause menu).
- [x] **Textures without emulated VRAM in steady state** (2026-10-24, FULLY_NATIVE.md step 3a): CLUT loads take the
      palette from the queued upload that wrote it, and data-less upload calls no longer force the queue; swizzled VRAM
      writes while driving ~245k -> ~7 per 5 s, render worker idle 27% -> 41% on the drive (`writeRectFast` was 11% of
      its samples). Verify: 2.7M CLUT loads the same as from VRAM. Scenes ok, tour classes unchanged (frame p90 19.1 ->
      15.6 ms). `PS2X_HW_CLUT_FROM_UPLOAD=0` = from VRAM.
- [x] **Streamed uploads cheaper** (2026-10-24, FULLY_NATIVE.md step 3b): PATH2 packets processed in place, packet / upload
      buffers reused, a four-lane upload hash; render worker busy on the drive ~59% -> ~50% of samples, standing worker
      -0.3 to -0.5 ms (A/B, alternating).
- [x] **Textures decoded from their uploads** (2026-10-24, FULLY_NATIVE.md step 3c): a texture cache miss reads the
      queued upload's data at each texel's VRAM address (`GSMem::PixelBits`) instead of emulated VRAM; ~4,100 decodes over
      the reference scenes identical to VRAM's. Left in VRAM: the loading screen's art (its upload arrives in pieces).
      `PS2X_HW_DECODE_FROM_UPLOAD=0` = from VRAM.
- [x] **Textures named by raster, render targets by role** (2026-10-24, FULLY_NATIVE.md step 3d): every texture made
      over the reference scenes and the drive named by the game's raster (RAM address) through the chain's upload REFs;
      targets named by the game (`gsHwSetTargetRole`), the reflection map at the window's resolution. Step 3 done.
- [x] **Unchanged world / model blocks not copied** (2026-10-24, FULLY_NATIVE.md step 4a / 4b): guest RAM under the
      OS's write watch; a block whose pages are unchanged since a confirmed copy is replayed from its recording without
      being copied (2.4M decisions checked, 0 wrong). Chain copy per frame 4.6 -> 2.1 MB; drive 50 -> 54 fps, worker
      -0.5 ms (A/B alternating). "Other call" packets skipped too since 2026-10-25 (a 12-byte
      over-skip in the MARK handler had lost their marks; 2.06M decisions checked, 0 wrong). `VCS_RSL_SKIP=0` = copy everything.
- [x] **Draw calls as items** (2026-10-25, FULLY_NATIVE.md step 4c; step 4 done): world / model draws with a native
      flush write one 16-byte CNT (MARK) instead of their GS packet and CALL / REF; flush, GS registers and block go to
      the worker as an item, the block decided at the kick. Sectors cached by `FUN_00240ED8` keep their packets (the game
      sends the copy again). Verify: 3.0M draws / 1.4M blocks 0 differ. Speed even (the copy savings came with 4b).
      `VCS_RSL_ITEMS=0` = packets.
- [x] **State setters as item ops** (2026-10-25, FULLY_NATIVE.md step 5a): the GS queue senders, texture matrix,
      `FUN_001CEF18`, `FUN_002F9CE8` and all-CNT state flushes become VIF-byte ops in the item stream (not while a
      sector / material is cached). Verify 2.67M draws / ops 0 differ. `VCS_RSL_OPS=0` = their packets.
- [ ] **Step 3: post effects** (started 2026-10-23; NATIVE_RENDERER.md "Post effects", game-internals.md 6g).
  - [x] **GS pixel centres**: the renderer sampled half a pixel off the GS, so VCS's per-frame copies blurred the
        picture (softer than the PS2 at 640x448). Fixed for every draw; `PS2X_HW_PIXEL_CENTRE=0` = before.
  - [x] **Higher internal resolutions kept through the copies** (`rtFull`): before, the shown picture was always a
        640x448 copy scaled up. `PS2X_HW_RT_FULL=0` = before.
  - [x] **Bloom and trails as native passes** with widescreen-correct proportions (NATIVE_RENDERER.md "Post
        effects"): at 4:3 640x448 the same picture as the emulation (static regions within run-to-run noise), at 16:9
        the glow / warp no longer a third wider than at 4:3. `PS2X_HW_POSTFX=0` = as recorded.
  - [x] Frame copy and motion blur as native passes (texture copy / one full-screen quad); the self-copy over the
        sky stays a GS draw (depth-tested, read at full resolution).
- [ ] **Step 4: native frame** (started 2026-10-23; NATIVE_RENDERER.md "Native frame").
  - [x] Per-target scales; "Internal resolution: Match window" draws the picture's targets at the window's size
        (1920x1080 at 16:9 1080p), the effect buffers at 1x. Default stays 1x (the original).
  - [x] MSAA (2x-8x) and anisotropic filtering (2x-16x) as options, off by default (NATIVE_RENDERER.md "Native frame").
  - [x] The reflection map at the window's resolution with "Match window" (2026-10-24, FULLY_NATIVE.md step 3d); the
        radar has no target of its own (drawn in the HUD layer, already at the window's resolution).
- [x] **Draw-source counter** (2026-10-24; NATIVE_RENDERER.md "Draw sources"): what each frame still draws through
      the GS path, by source (VU1 program, C++ reproduction, PATH2, PATH3) - perf line `gs_*`, overlay, the rig's
      `gsdraws`, tour / missions record it. Survey: no PATH3; PATH2 ~15 draws (reflection map, coronas, effect quads);
      45-90 draws from the C++ reproductions' GIF packets; VU1 draws only from clipped / culled 0x02c0 shine packets.
- [x] **Clipped and culled 0x02c0 shine packets natively** (2026-10-24, game-internals.md 6d): the second pass is the
      clipper again with tag 50. PHI_A1 verify: 421,694 packets exact (9,629 clipped, 26,518 culled shines). VU1 draws
      0 in the tour subset and 12 missions.
- [x] **VU1 XGKICKs went to the GS twice** (found with the counter; PS2RECOMP_PATCHES.md): `PS2X_HLE=0` scenes drew
      ~2x the vertices (cutscene 220,848 -> 122,823). Default path unchanged (scenes 0.00 static).
- [x] **VU1 setup programs natively** (2026-10-24, game-internals.md 6d "Setup programs"): 0x0000, 0x0148, 0x01e0,
      0x0250, 0x03a8, 0x0418, 0x0478, 0x2b50 and the slot routines 0xc8 / lit 0x4e0 / lit 0x57e, each tied to a hash of
      its code; verify exact against the whole VU memory (reference scenes 434,692, PHI_A1 217,489, a city spot
      80,557). Two more 0x02c0 code images accepted (they differ only in code the shine never runs). VU1 programs a
      frame: cutscene 195 -> 4, gameplay 86 -> 4, tour subset mean 104.6 -> 3.4 (worker 8.76 -> 8.05 ms); no VU1
      draws anywhere in the tour subset or the 12 missions. `PS2X_HLE_SETUP=0` = VU1.
- [x] **World draws from the RSL world draw calls** (2026-10-24, milestone 7 / RSL_SUBMISSION.md phase 2): the world
      draw's REF replaced by a VIF MARK as the chain is copied, the block drawn there from a recording (UNPACK results,
      MSCALs, meshes) - no copy, VIF parsing, unpacking, hashing or vertex conversion of world geometry. Verify: 174,301
      replays against the packet path, 0 differ. Timing within noise (world geometry is ~5% of VIF1's payload).
      `VCS_RSL_WORLD=0` = packet path.
- [x] **Model instances from the RSL draw calls** (2026-10-24, RSL_SUBMISSION.md phase 2): the CALLed packets of
      `FUN_002F7D58` / `FUN_002F80A0` (and every other CALL in the packet buffer) recorded once - the whole CALL ... RET
      stream, DIRECT GS data, STCYCL, all draw programs - and replayed at a MARK the chain walker puts before them. The
      marked bytes now stay in the chain's copy and are skipped (world blocks too: the old REF replacement read guest
      memory when the worker processed the chain, possibly after streaming replaced it). Replays write only the fields
      an UNPACK wrote (found by the verify: V3 / write-protected w lanes). Verify: 1,217,887 blocks over a bench run,
      0 differ. Render worker (one bench run each): Fort Baxter 5.5 -> 4.2 ms, busy city spot 10.8 -> 9.2, drive
      15.8 -> 13.4. Scenes ok, tour classes unchanged. `VCS_RSL_MODELS=0` = packet path.
- [x] **Game thread: chain copy, VU0, state flush** (2026-10-24, RSL_SUBMISSION.md "The game thread after phase 2"):
      a caller-aware drive profile found the VIF1 chain copy reallocating every frame (pool fixed: ~10% -> ~5.7% of
      the game thread), four VU0 programs still interpreted (0x0C10, 0x0068, 0x0E98, 0x0E30: native, `PS2X_VU0_VERIFY`
      0 differ; ~5%), and the RSL state flush (native for flushes without plug-in callbacks, byte-exact over ~700k
      flushes; 2.3% -> 1.3%). A/B, two alternating rounds: drive game thread 18.6 -> 17.8 ms, 45.8 -> 51.0 fps.
      Scenes ok, tour classes unchanged. `VCS_RSL_NATIVE=0` = the original flush.
- [x] **Packed draws** (2026-10-24, NATIVE_RENDERER.md "Draw sources"): the C++ reproductions' strips / fans recorded
      without the GS front end (`GS::submitPackedKick`), verified exact against the per-vertex recording (14M packets).
      Render worker at the busiest bench spot 14.5 -> 11.7 ms, AI drive 21.2 -> 18.2 ms (one bench run each).
      `PS2X_HLE_PACKED=0` = front end.
- [x] **No VU1 in a normal frame** (2026-10-24, game-internals.md 6d): the Z-clear image's programs (0x0000, 0x0200,
      0x0400), the last 0x0250 (slot routines now named by the hash of their code) and table-0x13 packets through the
      clipper reproduced; verify exact (PHI_A1 setup programs 160,468; reference scenes env pass 23,913 with 216
      clipped). `vu1_programs` 0 in the reference scenes and at all 18 tour-subset points. The emulated VU1 stays as
      the reference (`PS2X_HLE=0`, `PS2X_HLE_SETUP=0`, `PS2X_HLE_VERIFY=1`).
- [ ] Rigid / skinned lit models still use GPU clipping for their clipped packets (no wrong picture seen yet; same
      method when one shows up).
- [ ] Lazy render-target patching (bloom buffers dropped on uploads).


## Milestone 4 - frame rate (started 2026-10-23)

- [x] Main loop, VBlank handler, frame limiter and CTimer mapped (game-internals.md 4).
- [x] 60 fps switch (Display > Frame rate, `VCS_FPS=60`; off by default): the limiter lets a frame through after one
      VBlank. On the i7-7700K it reaches 31-37 fps in gameplay: the game thread needs ~27 ms a frame (milestone 7
      first for a steady 60).
- [x] Performance (milestone 7 work, 2026-10-23): VU0 collision programs 0x020 / 0x3A0 native (bit-exact): game code
      19-24 -> ~12 ms a frame; 60 fps mode 31-37 -> 45-50 fps outside the barracks (render submission ~8 ms is next).
- [x] Render submission, round 1 (all checked against the old path): VU1 code-write log (only re-uploaded ranges
      re-decoded); UNPACK fast path (`PS2X_VIF_UNPACK_FAST=verify`); rect uploads with hoisted addressing and whole
      PSMT4 blocks (`PS2X_GS_RECT_VERIFY=1`); lazy uploads found through a per-block index (`PS2X_HW_LAZY_INDEX=0` for
      the old scan). 60 mode outside the barracks 45-50 -> 56-60 fps (frame 16.6-17.8 ms: game ~10.5, render ~6).
      Driving (AI car from Fort Baxter, `fpsdrive.py`) is the hard case: 24-28 -> 26-29 fps, ~1,700 draws a frame,
      game ~18 ms + render ~17 ms one after the other on the game thread; 30 mode also dips to 27-28 there. Holding 60
      when driving needs render submission off the game thread (VIF1 / VU1 / GS front end on a worker), not more
      trimming.
- [x] Render submission worker (2026-10-23, NATIVE_RENDERER.md "Render submission worker", `PS2X_RENDER_THREAD=0`
      for the old path): chains, VU1 and the GS front end run on their own thread behind the game thread. 60 mode:
      outside the barracks at the cap with ~4 ms to spare; driving 26-29 -> 44-54 fps; flyover 42.5 -> 59.9 fps.
      Game code (~17-20 ms driving) is now the limit.
- [x] Game code (2026-10-23): inline dispatch / checkpoint fast paths (`PS2X_FAST_DISPATCH=0` to compare) and a
      ThinLTO + profile-guided build (`tools/build.py --opt pgo`, trained on `tools/bench.py` + the reference scenes).
      `tools/bench.py`, 60 mode, game-thread ms (two runs each):

      | spot | plain /O2 | + fast dispatch | + PGO + ThinLTO |
      |---|---|---|---|
      | Fort Baxter | 10.8 / 11.0 | 13.1 / 13.3 (more draws) | 9.7 / 10.8 |
      | city 575,175 | 7.9 / 8.2 | 6.7 / 7.5 | 5.9 / 6.0 |
      | city -675,1425 | 14.5 / 13.6 | 13.2 / 14.2 | 9.1 / 9.3 |
      | city 1325,-325 | 9.6 / 11.4 | 8.4 / 8.9 | 7.2 / 7.5 |
      | city -1425,-575 | 12.2 / 12.2 | 10.5 / 11.3 | 8.0 / 8.3 |
      | AI drive | 22.9 / 21.8 (40.6 / 42.6 fps) | 20.3 / 20.3 (45 fps) | 16.4 / 16.9 (56.7 / 55.1 fps) |

      Fast dispatch alone is ~1 ms (dispatch / checkpoint / lookup left the profile but their work stays inline);
      PGO + ThinLTO ~20-30% of game code. Checked: static reference scenes unchanged (loading 0.00, pause map 0.33),
      world tour subset classes unchanged, its worst frame 21.0 -> 16.7 ms. Remaining on the game thread (driving):
      game code ~65%, memmove ~3%, VU0 programs ~6%, IOP ~2%.

      Retrained 2026-10-24 (after RSL phase 2 and the game-thread work: `--opt pgo --train`, ~45 min). A/B against
      the same code without PGO (`--opt none`), two alternating rounds: standing at Fort Baxter 11.7-12.9 -> 8.1-9.3 ms,
      AI drive 19.4-20.2 -> 14.8 ms (44.6-46.9 -> 56.8-56.9 fps). Reference scenes all ok (a first capture right
      after the build caught `loading` / `flyover_base` on black / mid-fade frames - timing; the next capture matched,
      `loading` at the usual 1.17).
- [x] Frame-rate independence, phase 1 - timing (2026-10-23): Display > Game timing (Original / Frame-rate
      independent, restart) and Frame rate cap (30 / 60 / 120 / 144 / display refresh / unlimited). Independent:
      host-clock CTimer in whole ms, the 0.5 minimum step lifted, host pacing in the limiter, frames presented as the
      game finishes them, EE clock kept at real time on the VBlank chain. Game clock = wall clock at 30 and 144
      (was 1.5-4% slow from CTimer's per-frame ms truncation, in original timing too). Original timing unchanged
      (static reference scenes 0.00). game-internals.md 4.
- [x] Frame-rate independence, phase 2 - audit rig (2026-10-24): `tools/framerate_audit.py` runs fall / run /
      sprint / jump / car / clock scenarios at forced steps (`VCS_TIMESTEP`), compared per simulated second.
      Findings: steps below 0.5 break ped movement (independent timing capped at 100 fps, DECISIONS.md); in
      1/15-1/100 car acceleration and running agree, falling and jumping vary with the rate (+7% fall time,
      +6-9% jump height from 30 to 60-100). game-internals.md 4 has the numbers and the frame counter worklist.
- [x] Measured on the PGO + ThinLTO build (2026-10-24), independent timing: game clock = wall clock (x1.001) at
      caps 60 and 100; cap 100 runs 91 fps outside the barracks, `tools/bench.py --fps 100 VCS_TIMING=1`: city
      spots 80-100 fps, AI drive 68 fps (game 13.7 ms, worker 14.0 ms). Original timing unchanged (static
      reference scenes 0.00).
- [x] User playtest (2026-10-24): missions play well in frame-rate independent timing at 60 and 100 fps.
- [ ] Well past 100 fps everywhere (started 2026-10-25, docs/PERFORMANCE.md). Done: guest memory accesses with one
      compare and the scratchpad inline (game thread ~2x faster), lean pc and `__restrict ctx` in the recompiler, the EE
      clock no longer holding the game at ~100 fps, the flip's CSR read without the render-worker fence, the main loop
      unpaced for an unlimited cap. Uncapped (`--opt none`): Fort Baxter 111 -> 209 fps, city spots 118-166 -> 135-331,
      AI drive 95 -> 126 fps. Then the ped-movement bug below step 0.5 patched: independent timing goes up to 240
      fps (was 100). Then the render worker split in two (the GS stage), guest functions without register saves
      (preserve_none), smaller game-thread items, PGO retrained. PGO build, independent timing, unlimited (= 240):
      Fort Baxter 223 fps, city 201-240, AI drive 150, world tour (40 points) median 208 / slowest 120; 30-minute
      uncapped soak clean. Left: heavy views (~1,950 draws) at ~120 fps - game thread and render worker both ~6-8 ms.
- [ ] Frame-rate independence, phases 3-4. Done: falling / jumping explained (Euler integration error, <= 6%, not a
      bug - game-internals.md 4); real-time pacing checked (game clock = wall clock at caps 60 / 100 / refresh,
      presents at the cap). HUD / radar flashing at the original rate above 30 fps (2D pass on a 30 Hz frame counter). Left: the four
      blinking readers outside the 2D pass;
      what breaks below step 0.5 (FUN_00337208's max(step, 0.5) is one) to lift the 100 cap; missions at 60 / 100
      (driving, shooting, flying, cutscenes); VRR / frame-time variance. (Phase 1 / 2 above; the user's choice over interpolation, DECISIONS.md 2026-10-23): CTimer on the host
      clock, a frame-rate cap setting, a forced-step test rig, then the audit of frame-count assumptions. Replaces
      the "60 fps fixes" item below.
- [ ] Fixes for what changes at the shorter step (physics, animations, script waits, particles, camera), each behind
      the switch; frame pacing.
## Milestone 5 - movies (2026-10-24) - done
- [x] STINGS.PSS (the Rockstar logos) and INTRO.PSS play natively at boot (`src/game/vcs_movie.cpp`): own PSS
      demuxer, MPEG-2 video through FFmpeg, shown by the presenter at 4:3 (pillarboxed in widescreen), 48 kHz PCM
      sound mixed into the audio output and used as the clock. Checked: logos 10.8 s, 321 pictures, none dropped;
      Start during the logos ignored (as on the PS2), the intro skipped by Start, the game then loads the save.
- [x] Display > Skip intro movies (`display.skip_intro`, `VCS_SKIP_MOVIES`; off = play them as on the PS2).
      Test tools (run.py, the rig, scenes) default to skipping them.
## Milestone 6 - retire the IOP (started 2026-10-24)
- [x] Step 1, streaming: CDSTREAM's stream server (RPC 0x65686577) replaced on the EE side by host-thread reads from
      the disc image (`src/game/vcs_stream.cpp`, `VCS_NATIVE_STREAM=0` for the IOP's). Loading as fast as before
      (boot 6.3-7.8 s, five LOAD_SCENE teleports 2.1 s - the game is the limit, not the reads: 14 MB in ~5 ms).
      Static reference scenes unchanged; world tour subset clean.
- [x] Step 2, sound (2026-10-24): CDSTREAM's two sound servers ported to the host (`src/game/vcs_sound.cpp`, the
      effects and music engines) on a native libsd (`src/sound/libsd.cpp`) and an SPU2 core of their own (the
      emulator's SPU2 class, now public), which is the audio output; `VCS_NATIVE_SOUND=0` for the IOP's. Tables the
      module carries (stream names, SPU2 addresses) are read from the disc's own CDSTREAM.IRX. Checked against the
      emulated IOP on identical input with `tools/soundcheck.py` (the IOP run's sound RPCs recorded with their IOP
      times and replayed into the native driver): **PHI_A1 + cutscene preload: 11 of 12 sounding 5 s windows
      bit-identical to the emulated IOP's output**, the 12th spans a jump in the reference's own timing; an AI drive
      with the radio: 99.5% of 19,314 SPU2 register writes / DMAs in the same second, none of the reference's
      missing, the rest of the audio differs by +-1-sample event timing (the emulated SPU2 runs up to one IOP batch
      behind the IOP clock). Found on the way: the radio started at volume 0 for 0.45 s after a station change (the
      preload must take its DMA time - game-internals.md 9a). The loop at a stream's end (no capture reaches one):
      a synthetic call sequence opening FLASH.VB 3 s before its end with looping on plays 13 s on without a gap.
      Fixed 2026-10-24 (the user heard it): bike engines restarting every frame, cars playing rev samples while
      slowing - the native "is voice v playing" (`0xA0A00`) read another voice's envelope. Same bike / car ride:
      156 key-ons on the IOP, 676 before the fix, 126 after; 0 / 91 / 0 voices re-triggered in back-to-back frames.
      The recordings now carry each call's reply and the replay compares them (43,138 calls: 21 differ, all a load or
      stop finishing sooner natively) - the check that would have caught it.
      Checked 2026-10-24 after the user missed handbrake screech and some door sounds: on scripted handbrake turns
      and exits / entries the game asks for the same sounds natively as on the emulated IOP (skid hiss 239, squeal 97,
      door open 217 / shut 216: 8 door sounds in both, at matching moments), the replay matches the IOP's replies, and
      through the strongest skid the native output equals the reference within ~1% loudness per quarter second.
      Same at the original 30 fps timing. Not reproduced as a native-driver difference; open until the user compares
      by ear with `VCS_NATIVE_SOUND=0`. (The scripted turns ran partly on sand, where GTA plays no screech.)
      2026-10-24, the user (comparing with the original): no tyre screech on a bike at speed with R1 held while
      turning, with native and emulated sound alike. On the airport runway the game does request the screech
      (sound 260, volume 25-36, pitch 16-18k, held through the skid; game-internals.md 9a) and the SPU2 plays it
      (voice output peak ~4000 against ~8300 for the engine; replay = live output). Open: whether it is quieter
      than on the original - a clip went to the user to judge.
      Mapping tools: `tools/irxdis.py --map / --decompile`
      (headless Ghidra), `tools/mipsdis.py` (MIPS I, standard library).
- [x] Step 3 (2026-10-24): no emulated IOP in the normal run - no IRX module loaded or run, no IOP time emulated
      (`retireIop`, game-internals.md 9b); `PS2X_IOP=1` runs it again (`VCS_NATIVE_SOUND=0` / `VCS_NATIVE_STREAM=0`
      do too). Boot to gameplay 4.9-6.1 s in all three configurations (game-bound); IOP 0.2 ms per frame (was
      0.3-0.4). Memory card save / Load Game / Delete Game through the menus pass (`tools/saves.py`, whose load check
      now looks at where the player ends up: the runtime's `[MC]` log lines arrive late). Tour subset (`--every 9
      --limit 18`): 15 ok, 2 relocated, 1 falling - identical with the IOP off, on, and everything emulated. Six story
      missions (JER_A3, PHI_A1, MAR_A1, LOU_A1, LAN_B1, BRY_B1) run without problems with the IOP off.
- [ ] Open (found 2026-10-24, not milestone 6): tour point (-925, 1175) falls through the world when the tour reaches
      it after the points before it, in every configuration; a fresh teleport there from Fort Baxter lands (z 14.5).
      Collision streaming order / timing at that spot needs a look.
## Phase 5 - stability
- [x] Vic falls through the world in some places - **no longer happens** at the spot the user saw it (user
      playtest 2026-10-24); fixed along the way by other work (which change is not known: the VU0 collision
      natives, the scheduler / timing work and the renderer changes all came in between). The original report:
      (reported by the user 2026-10-22, after the
      native renderer; not yet known whether it is new). Collision needs a look: whether the
      collision sectors / models are streamed in at those spots (CDSTREAM reads, IMG offsets),
      VU0 programs used by collision (`0x0000` in .vutext looked like line/sphere vs triangle
      tests, docs/game-internals.md "VU microcode"), and float edge cases in the recompiled code
      (FP contraction, round-to-zero, denormals). Compare with PS2X_HLE=0 to rule the renderer out.
- [x] Settings menu (F2) flickered between two layouts while the mouse sat between rows, and a click on a toggle
      (Skip intro movies) did not register - **fixed 2026-10-24**: the menu was an auto-sized, centred window whose
      help line grew and shrank with the hovered row, so the rows moved under a still mouse every other frame. Now a
      fixed-size window with a fixed-height help area (SETTINGS_MENU_WIDESCREEN.md "Menu v2"). Checked with real
      mouse / keyboard input (a script moving the cursor over the window, rows' boundary, clicks, arrows, Q / E,
      Escape). Settings locked by an environment variable (tools/run.py sets `VCS_SKIP_MOVIES=1`) now name it beside
      the label: that alone makes a click do nothing.
## Phase 6 - enhancements

## Useful switches
| Env var | Effect |
|---|---|
| `PS2X_GS=cpu\|hw\|gpu\|null` | GS renderer (default cpu) |
| `PS2X_HW_SCALE=n`, `PS2X_HW_DEBUG=1` | hardware renderer scale / stats |
| `VCS_FPS_LOG=1` | VBlanks and flips per second (was `VCS_FPS=1`; `VCS_FPS` is the frame rate cap) |
| `VCS_TIMING=0\|1`, `VCS_FPS=30\|60\|120\|144\|-1\|0` | game timing original / frame-rate independent; frame rate cap (-1 display refresh, 0 unlimited) |
| `VCS_TIMESTEP=N` | independent timing: exactly 1/N s per game frame, unpaced (frame-dependence tests) |
| `VCS_SKIP_MOVIES=0\|1` | play / skip the intro movies (Display > Skip intro movies); test tools default to 1 |
| `VCS_MOVIE_LOG=1` | movie playback: sound pushed / queued / played vs the pictures, every second |
| `VCS_NATIVE_STREAM=0`, `VCS_STREAM_LOG=1` | disc streaming through the IOP (the reference); native read stats every 5 s |
| `VCS_NATIVE_SOUND=0` | sound through the emulated IOP's CDSTREAM / LIBSD (the reference) |
| `VCS_SKID_LOG=1` | the skid-sound function's inputs for the player's vehicle (type, drive type, speed, wheel states, surface) |
| `PS2X_SPU2_VOICE_LOG=1` | SPU2 key-ons (address, pitch, ADSR, volume) and every sounding voice's address / envelope / output peak each 0.1 s |
| `PS2X_IOP=0\|1` | emulated IOP off / on (default: off when streaming and sound are native) |
| `PS2X_SPU2_TRACE=<file>` | every SPU2 register write and DMA to `<file>.<n>` per SPU2 instance (0 = the emulated IOP's) |
| `PS2X_RPC_RECORD=<file>`, `PS2X_RPC_RECORD_SIDS=<hex,...>` | record RPC calls with their IOP time (tools/soundcheck.py) |
| `VCS_SOUND_REPLAY=<record>`, `VCS_SOUND_REPLAY_OUT=<wav>` | replay recorded sound calls into the native driver, write its output, exit |
| `VCS_MIN_STEP=x` | independent timing's step floor (default 50/240: the 240 fps ceiling; lower makes an unlimited cap unlimited) |
| `VCS_TIMING_LOG=1` | independent timing: fps, host wait, EE clock vs real time, WaitVBlank callers every 2 s |
| `VCS_PERF_OVERLAY=0\|1` | the perf overlay setting for one run |
| `VCS_PROFILE=s` | sampling profiler (guest functions + host symbols) |
| `VCS_PROFILE_LINES=name` | with `VCS_PROFILE`: source lines of the host functions whose name contains `name` |
| `PS2X_VIF_UNPACK_FAST=0\|verify` | VIF1 UNPACK general loop only / both, compared |
| `PS2X_GS_RECT_VERIFY=1` | every 64th rect upload also written pixel by pixel and compared |
| `PS2X_HW_LAZY_INDEX=0` | lazy uploads to write found by rescanning the queue (the old way) |
| `VCS_SKIN_VERIFY=1` | native ped skinning (SSE) checked against the scalar reference, a line every 16k packets |
| `VCS_EE_CHARGE_SHIFT=<n>` | frame-rate independent timing charges guest work 1/2^n of its estimated EE cycles (default 2; 0 = as original timing) |
| `VCS_CSR_FENCE=1` | the flip's GS CSR read (0x370714) waits for the render worker as other CSR reads do |
| `PS2X_RECOMP_FULL_PC=1`, `PS2X_RECOMP_NO_RESTRICT=1` | when running the recompiler: a `ctx->pc` store before every load / store and branch as before; generated functions without `__restrict ctx` |
| `PS2X_GS_THREAD=0\|1` | the render worker's GS stage (GS front end + frame building on a second thread; default on with the worker and the HW renderer) |
| `PS2X_RENDER_THREAD=0\|1` | render submission on the game thread / on its own thread (default with `PS2X_GS=hw`) |
| `PS2X_RENDER_THREAD_LOG=1`, `PS2X_RENDER_THREAD_AHEAD=n` | worker items / fences every 5 s; frames the game may run ahead (1) |
| `VCS_PROFILE_THREAD=RenderWorker` | `VCS_PROFILE` samples the render worker instead of the game thread |
| `PS2X_FAST_DISPATCH=0` | guest calls / loop checkpoints always through the scheduler (no inline fast path) |
| `VCS_EXE=path` | tools/run.py (and the rig, bench, scenes on top of it) run that build |
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
| `PS2X_HW_PIXEL_CENTRE=0` | draws without the GS pixel-centre offset (softer copies, as before 2026-10-23) |
| `PS2X_HW_RT_FULL=0` | render targets read once per PS2 texel everywhere (higher internal resolutions lost in the copies) |
| `PS2X_HW_POSTFX=0` or letters `btc` | effects drawn as recorded / only these native: b bloom, t trails, c frame copy + motion blur |
| `PS2X_HW_SCALE=0` | native frame: the picture's targets at the window's size (menu: Internal resolution > Match window) |
| `PS2X_HW_MSAA=2/4/8` | MSAA for the picture's targets (menu: Anti-aliasing) |
| `PS2X_HW_ANISO=2..16`, `PS2X_HW_ANISO_LOG=1` | anisotropic filtering of the game's textures (menu: Texture filtering); count the draws per sampling path |
| `VCS_FPS=60` | 60 fps: the game's limiter waits one VBlank a frame instead of two (menu: Frame rate) |
| `PS2X_HW_FRAME=WxH` | the native frame's size whatever the window (tests) |
| `PS2X_HW_DUMP_ALLVERTS=1` | the F12 draw log prints every vertex of every draw |
| `VCS_MENU_NATIVE=0` | menus drawn into the PS2 frame buffer instead of the native layer |
| `VCS_LOADING_NATIVE=0` | loading screens drawn into the PS2 frame buffer instead of the native layer |
| `PS2X_HW_PATH2_PACKED=0` | PATH2 draw packets (Im2D, effect strips) through the GS front end vertex by vertex instead of recorded whole |
| `PS2X_TRACE_PATH2_TAGS=1` | PATH2 (VIF DIRECT) packets by the shape of their first GIFtag, and the register sequence of A+D packets that draw, every 5 s (`[path2]`) |
| `PS2X_WRITE_WATCH=0`, `PS2X_WRITE_WATCH_LOG=1` | guest RAM without the OS's write watch (no block can then be told unchanged); pages written per fold and the fold's cost, every 5 s |
| `VCS_RSL_ITEMS=0`, `VCS_RSL_ITEMS_VERIFY=1` | world / model draws as packets (no draw items); items built but the original writes the packets, compared with what each item stands for, and each block the walker copies with what an item would copy (FULLY_NATIVE.md step 4c) |
| `VCS_RSL_OPS=0` | the state setters (GS queue senders, texture matrix, `FUN_001CEF18`, `FUN_002F9CE8`, all-CNT flushes) write their packets instead of item ops (FULLY_NATIVE.md step 5a) |
| `VCS_RSL_CENSUS=1` | every 5 s: what the VIF1 chain still carries outside marked blocks and items - top-level tags by DMA id, first VIFcode and data region, per chain (FULLY_NATIVE.md step 5) |
| `VCS_RSL_SKIP=0`, `VCS_RSL_SKIP_VERIFY=1`, `VCS_RSL_SKIP_KINDS=<mask>` | copy every marked block; decide skips but copy anyway and check each against the recording it would have replayed; which kinds may be skipped (bit 0 world, 1 model, 2 model-b, 3 other call; default all) |
| `VCS_RSL_MEASURE=1` | with `VCS_RSL_WORLD_LOG`: per chain the bytes copied, the marked blocks and the distinct ones |
| `PS2X_HW_RT_LOG=1` | the render targets with their names (gsHwSetTargetRole), sizes and scales, every 5 s |
| `PS2X_HW_REFLECTION_SCALE=0` | the reflection map stays 1x with "Match window" |
| `VCS_RSL_RASTERS=0` | textures not named by raster (no upload REFs recorded) |
| `PS2X_HW_DECODE_FROM_UPLOAD=0`, `PS2X_HW_DECODE_VERIFY=1` | textures decoded from emulated VRAM (writing the queued uploads there first) instead of the queued upload's data; both, compared (`[gs-hw] textures from upload`) |
| `PS2X_HW_CLUT_FROM_UPLOAD=0`, `PS2X_HW_CLUT_VERIFY=1` | CLUT loads read emulated VRAM (writing the queued uploads there first) instead of the queued upload's data; both, compared (`[gs-hw] clut from upload`) |
| `VCS_TRACE_IM2D_DELAY=s`, `VCS_TRACE_IM2D_STACK=1` | start the Im2D trace after s seconds (default 30); print the guest stack's return addresses per call |
| `VCS_TRACE_HUDKEY=n` | log n 2D primitives the HUD classifier did not find (positions) |
| `PS2X_HW_OUTPUT=WxH` | the renderer's output size whatever the window (full-screen sizes in test runs; with `PS2X_CAPTURE_SIZE`) |
| `PS2X_HW_DUMP_TEX=<key hex>` | write that decoded texture (colour + alpha PPM, keys from the F12 draw log) - analysis only, never commit |
| `PS2X_HW_HUD_LOG=1` | the renderer's HUD layers (per frame buffer, stale or not) and the output size, every 2 s |
| `VCS_TRACE_IM2D=n`, `VCS_TRACE_IM2D_MARKERS=1..5` | log n Im2D calls (caller, primitive, vertices) / the 2D pass's calls, from 30 s on |
| `PS2X_HLE_STATS=1` | native draw counters; 0x0030 stage tables still on the VU1 every 10 s (+ one code / data sample each) |
| `VCS_TRACE_RSL=1` | which functions build the frame's packets: calls and bytes per frame per function, every 5 s (`[rsl]`; game-internals.md 6i) |
| `VCS_RSL_WORLD=0`, `VCS_RSL_MODELS=0`, `VCS_RSL_WORLD_VERIFY=1`, `VCS_RSL_WORLD_LOG=1` | world blocks / model (and other CALLed) packets through the packet path instead of record / replay at the RSL draw; check every replay of either against the packet path; counts per kind every 10 s (`[rsl-world]`) |
| `VCS_RSL_SHADOW=1` | RSL shadow draw list: each draw call checked against its tag in the frame's chain, 3D tags outside draws listed, each draw program's VU state checked against the draw's RSL state (`[rsl-shadow]`, `state (1b)`; not with `VCS_TRACE_RSL`) |
| `VCS_RSL_SHADOW_DUMP=n`, `VCS_RSL_SHADOW_FIND=1` | with `VCS_RSL_SHADOW`: n samples of draw state + VU memory (`rsl_shadow_NNN.bin`); where in guest RAM mismatching VU rows are |
| `PS2X_CHAIN_CENSUS=1` | the VIF1 chain by tag location / id / data source: tags and quadwords per chain, every 5 s (`[chain]`) |
| `VCS_PROFILE_GUEST_FILE=<path>` | with `VCS_PROFILE`: every guest function's samples per report window, appended (for sums by subsystem) |
| `VCS_PROFILE_CALLERS=name[,name]` | with `VCS_PROFILE`: for samples in host functions whose name contains one of these, the unwound call chain up to the first guest function (`called from <sampled function> < <caller> ...`; an unwind fault ends the chain, not the process) |
| `VCS_RSL_NATIVE=0`, `VCS_RSL_NATIVE_VERIFY=1`, `VCS_RSL_NATIVE_LOG=1` | the original RSL state flush instead of the native one; the original runs and the native bytes are compared with it; counts every 10 s (`[rsl-native]`) |
| `PS2X_HLE_PACKED=0`, `PS2X_HW_PACKED_VERIFY=1` | the C++ reproductions' strips / fans through the GS front end; check the packed recording against it per packet |
| `PS2X_HLE_SETUP=0` | VU1 setup programs (matrices, slot routines, block copy) on the VU1 instead of natively |
| `PS2X_HLE_DUMP_IMAGES=1` | write the code and data of the first 0x02c0 shine packet of each code image (`shine_<hash>_*.bin`) |
| `PS2X_HW_DRAW_SOURCES=1`, rig `gsdraws [reset]` | draws still made from GS primitives by kind and source, per frame (logged every 5 s / on request); totals always in the perf line (`gs_draws` ...) |
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
