# Fully native: the steps left (2026-10-24)

The roadmap's "native" (NATIVE_PORT_ROADMAP.md "What done means", point 2): in a normal frame nothing draws through the
emulated GS or VU1, no IOP code is interpreted, audio comes from a native mixer - the emulated paths kept as
switchable references. This is the working list for the rest of it; update a step's status here as it moves and
write the details where they belong (game-internals.md, NATIVE_RENDERER.md, RSL_SUBMISSION.md, PROGRESS.md).

## Where it stands

- **VU1**: no VU1 program runs in a normal frame (game-internals.md 6d; `vu1_programs` 0 in the reference scenes and
  the tour subset).
- **IOP**: streaming and both sound engines on the host, the emulated IOP off by default (milestone 6).
- **3D geometry**: world sectors and model instances drawn natively, from recordings at the RSL draw calls
  (RSL_SUBMISSION.md phase 2); the packet path is the switch-back.
- **Counter**: the draw-source counter (NATIVE_RENDERER.md "Draw sources") says what still goes through the GS path -
  perf line `gs_draws` / `gs_path2` / ..., the rig's `gsdraws`, tour / missions records.

## The steps

| # | Step | Size | Status |
|---|---|---|---|
| 1 | The last GS draws | S-M | done 2026-10-24 (3 render-target reads left for step 3) |
| 2 | Loading screens in the native layer (there is no title menu) | S | done 2026-10-24 |
| 3 | Textures and render targets without GS memory | L | done 2026-10-24 (3a-3d) |
| 4 | Draw-call replay (3D without building packets) | M-L | planned |
| 5 | An empty chain | M | planned |

### 1. The last GS draws

About 15-20 draws a frame still go through the GS front end, all PATH2 (VIF DIRECT, the game's 2D / effect packets):
the reflection map built in FBP 420 (a 128x128 sample of FBP 140 plus a sun sprite), coronas / light sprites,
colour-filter quads, fades and the depth-tested self-copy over the sky. Make each one a native draw (from its call
site or as a recognised packet, as the post passes did), checked against the emulated picture (scenes at 1x). The
ones that sample a render target stay tied to GS memory until step 3; the counter shows which.

Done when: `gs_draws` reads 0 (or only render-target reads waiting for step 3) in the reference scenes, the tour
subset and the missions.

**Done 2026-10-24** (NATIVE_RENDERER.md "PATH2 packets recorded whole"): every PATH2 packet that draws was Im2D's
(one PACKED ST / RGBAQ / XYZ2 tag) or an A+D effect strip; both are recorded whole now, checked exact (5M+ packets at
4:3 and 16:9, HUD-layer membership included). GS-path draws 15.8-18.5 -> 3.0 a frame (tour subset max 3.0). The three
left read render target 140 (the reflection map's two copies into FBP 420, the self-copy over the sky) and set state
before their PRIM: step 3. Missions not re-run for this step.

### 2. Loading screens and the title menu

The last 2D outside the native HUD / menu layer (PROGRESS.md milestone 3 step 2: "Loading screens and the title menu
over the flyover: not looked at yet"). Same method as the menus: their Im2D / sprite calls drawn in the native layer
at the window's size, `VCS_..._NATIVE=0`-style switch back, scenes compared at 4:3 / 16:9.

Done when: the loading screens and the title / start menus draw from the native layer, sharp at 1080p, the static
`loading` scene unchanged at 1x.

**Done 2026-10-24** (game-internals.md 6f "Frames outside gameplay"): `LoadingScreen` (FUN_0021E170) opens a context
like the menu's; the static `loading` scene 1.12 against the reference, sharp at 1080p, `VCS_LOADING_NATIVE=0` = before.
There is no title menu: the game boots into the opening sequence (Start does nothing there) and the frontend menu is
the pause menu, native since milestone 3.

### 3. Textures and render targets without GS memory

RSL_SUBMISSION.md phase 3. Decode the game's rasters (`.XTX`: PSMT4 / T8 + CLUT, game-internals.md 7) into GPU textures
when they are loaded or streamed, keyed by raster; native draws bind them directly and the game's VRAM uploads stop
mattering. Render targets read as textures (reflection map, bloom, frame copies, the radar) become native targets -
this is also what lets the radar and reflection map draw at the window's resolution (PROGRESS.md milestone 3 step 4).
Revisits DECISIONS.md "Textures from the GS backend's decoded cache".

Done when: a normal frame binds no texture decoded from GS memory; the reference scenes match at 1x.

**What GS memory still does (found 2026-10-24).** Render targets already live on the GPU (`m_rts`: a draw that
samples one reads the target directly); the emulated GS memory carries the textures. VCS streams them: every bind
(`FUN_002F62B8`, game-internals.md 6i "Texture streaming") uploads the raster's mips and its CLUT from RAM into a
fixed VRAM slot. The renderer queues those uploads (lazy uploads), keys its GPU textures by the upload's content and
decodes a texture from emulated VRAM only on a cache miss - but every bind's CLUT load read the palette back from
VRAM, which wrote the queued uploads into it (swizzled): ~25% of the render worker's busy time while driving.

Phases:
- **3a - palettes from the upload, no VRAM in steady state** (done 2026-10-24): CLUT loads read the queued upload's
  data; uploads without data no longer force the queue. Swizzled VRAM writes while driving ~245k -> ~7 per 5 s; render
  worker idle 27% -> 41% of the drive. Details below.
- **3b - the uploads' own cost** (done 2026-10-24): the allocations, copies and the serial hash around each streamed
  upload. Details below. Left: the copy into the queue and the hash itself (`UploadImage` ~3.8% of the worker). The
  copy goes only if the queued upload can point at the pixels where they arrive - the chain's copy, alive while the
  chain is processed, with the uploads still queued at its end copied then - which needs the image continuation to
  reach the GS without being re-tagged into a buffer of its own (a GS front-end entry point: a full rebuild).
- **3c - textures decoded from the upload data** (done 2026-10-24): the cache-miss decode reads the upload's data
  instead of VRAM. A normal frame now needs no emulated VRAM for textures; the loading screens' art still goes
  through it. Details below.
- **3d - identity by raster, not by VRAM address** (done 2026-10-24): textures named by the game's raster (its RAM
  address), so draws made without packets (steps 4 and 5) can name their texture; render targets named by what they
  are; the reflection map at the window's resolution (the radar already was: it has no target of its own). Details
  below.

**3a done 2026-10-24.** `HwBackend::LoadClut` looks for the queued upload that last wrote every 256-byte block the
CLUT load reads; when one whole upload at the CLUT's base, in its format, covers the pixels, the palette is loaded from
its linear data (`GSCpuBackend::LoadClutFromRect`: the same CLD rules and CSM1 / CSM2 entry order as the VRAM load)
and nothing is written. Upload calls without data (the header packet's IMAGE tag, its pixels in the next DIRECT) no
longer force queued uploads out. Checked with `PS2X_HW_CLUT_VERIFY=1`: 1.5M loads on the drive, 1.2M over the
reference scenes, all the same as from VRAM. Reference scenes ok; tour classes unchanged, frame time median 11.7 ->
11.1, p90 19.1 -> 15.6 ms. `PS2X_HW_CLUT_FROM_UPLOAD=0` = from VRAM.

**3b done 2026-10-24.** A worker profile with call chains on the drive put the rest of the texture streaming in
allocations and copies around each packet: the GIF arbiter copied every packet into a new buffer before processing it,
`submitVif1DirectData` built a new buffer to re-tag each texture's pixel DIRECT, and the upload queue allocated (and
freed) a buffer per upload; the content hash was one serial multiply chain. Now: a PATH2 packet that would be drained
at once into an empty arbiter queue is processed in place (same order, same path tag); the arbiter's, the re-tag's and
the queue's buffers are reused; the hash runs four lanes. Worker profile on the drive: busy ~59% -> ~50% of samples
(allocator and `memset` gone from the top, `memmove` 4.6% -> 3.7%). A/B against the 3a build, two alternating rounds:
standing worker 3.7 -> 3.4 and ~4.3 -> 3.8 ms; the drive 10.6 -> 8.8 ms in one round, level in the other (route
variance). Reference scenes ok, tour classes unchanged.

**3c done 2026-10-24.** On a texture cache miss, `decodeFromQueuedUpload` takes the queued upload that is the latest
writer of every block the texture reads (the CLUT's rule) and reads each texel's bits out of it at the texel's own VRAM
address: `GSMem::PixelBits` gives a pixel's first bit and width in VRAM with the same swizzle tables as the reads and
writes, the upload's pixels are indexed by theirs, so a texture read in another format than it was sent in (T8 sent as
CT32) decodes the way the GS would. The texels then go through `DecodeTexture`'s own conversion (CLUT / TEXA,
`GSCpuBackend::DecodeTextureFromTexels`). Checked with `PS2X_HW_DECODE_VERIFY=1` over the reference scenes: every one
of ~4,100 decodes the same as from VRAM. Over the scenes 4,124 decodes came from uploads and 4 from VRAM - the loading
screen's art (T8 512x512 at TBP 8960), whose upload arrives in pieces and is written directly; while driving none from
VRAM, and 0-36 uploads a 5 s window written into it. No texture in these runs was read in another format than it was
sent in, so that case is handled but not yet exercised. Reference scenes ok; tour unchanged (the known fall-through
point at (-925, 1175) classed "water" this run - the same fall, landing in a water state). `PS2X_HW_DECODE_FROM_UPLOAD=0`
= from VRAM.

**3d done 2026-10-24.**
- **Render targets by what they are**: the game names its buffers (`gsHwSetTargetRole(fbp, fbw, name, windowScale)`,
  `src/game/vcs_display.cpp`): frame 140, shown picture 0, Z buffer 280, reflection map 420 / 128 wide, bloom 420 /
  452, trails 384. `PS2X_HW_RT_LOG=1` lists the targets with their names, sizes and scales every 5 s. The GPU targets
  stay keyed by FBP inside the renderer (the game reuses VRAM, and a draw names only its FBP).
- **The reflection map at the window's resolution**: with "Internal resolution: Match window" it takes the frame's
  scale (1080p: 384 x 309 texels instead of 128 x 128) - it is only ever sampled (vehicles' paint), never copied 1:1;
  kept at 1x when the GS bloom shares its memory (`PS2X_HW_POSTFX` without b) and with
  `PS2X_HW_REFLECTION_SCALE=0`. At 1x nothing changes. The reflection is faint by design (the game blends it in at
  ~5%), so the picture changes little.
- **The radar** needed nothing: a frame dump at Fort Baxter shows it drawn straight into the HUD layer from 128x128 T4
  tile textures under a scissor - at the window's resolution already (game-internals.md 6f corrected: the 128x128 at
  FBP 420 had been taken for the radar's; it is the reflection map).
- **Textures by raster**: each upload's pixels are a REF to the raster in RAM (`NOP; DIRECTHL n`, game-internals.md 6i).
  The RSL chain observer records where each such payload lands in the chain's copy (packet buffer and CALLed packets
  alike); the VIF interpreter reports each DIRECT's data (`ps2chain::notifyDirect`), and the one at a recorded offset
  tells the renderer whose pixels follow (`gsHwSetUploadSource`). A queued upload, and the upload info a texture is
  keyed by, carry that address; recordings of CALLed packets keep it for their replays. A texture made from such an
  upload is named by the raster (`gsHwTextureForRaster(address)` -> its key) - in any format it was sent in (T4 as
  CT16, T8 as CT32: then the queued upload that last wrote all its blocks names it). Over the reference scenes 4,112
  textures made, all named; while driving all named (~725 rasters). Reference scenes ok, tour unchanged.
  `VCS_RSL_RASTERS=0` = no names. A first attempt put a VIF MARK before the raster REF's payload - it landed inside the
  DIRECTHL's data and broke the stream; found at once by the RSL counts (replays collapsed), replaced by the offset
  lookup.
- Not done here: the GPU targets themselves are still found by FBP when a draw samples one; a draw made without
  packets will need its target by role (steps 4 / 5).

### 4. Draw-call replay

RSL_SUBMISSION.md phase 2's planned form. The world / model draw hooks replay their recordings directly instead of
writing packets, so the game thread no longer builds, copies (~3.5% of it while driving) or flushes (1.3%) those
draws; the worker gets the draws as items in chain order. The recordings' validity (content hashes) has to be
decided at the draw call - the packets' bytes are not copied any more (DECISIONS.md 2026-10-24 on why the copy was
kept).

Done when: world and model draws make no packets; verify mode against the packet path 0 differ; drive game thread
measurably lower (A/B, alternating builds).

### 5. An empty chain

RSL_SUBMISSION.md phase 4. What is left in the VIF1 chain after steps 1-4 - 2D (Im2D: already drawn natively, still
sent), effects, the Z clear, texture uploads - moves to its call sites. Then a normal frame sends nothing through
VIF1 or the GS front end; the emulation stays as the switchable reference.

Done when: in gameplay, cutscenes and driving the frame's chain is empty (or not kicked), pictures as the packet
path's, tour and missions clean.

## Not "native" but part of "done"

Completeness (milestone 2: every mission to the credits), the open fall-through spot (-925, 1175), the stale
regression baseline for the loading / pause-map scenes (needs the user's picture sign-off), 60 fps fixes (milestone
4), game code under ~6 ms (milestone 7), settings / controls polish (8), Vulkan / Metal and Linux / macOS (9).
