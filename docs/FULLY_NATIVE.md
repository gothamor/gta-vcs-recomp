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
| 4 | Draw-call replay (3D without building packets) | M-L | done 2026-10-25 (4a-4c; draws whose flush calls plug-in callbacks stay packets) |
| 5 | An empty chain | L | 5a, 5b done 2026-10-25 (state ops, texture uploads); 5c-5g planned below |

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

**Sizing (2026-10-24, `VCS_RSL_MEASURE=1`, driving).** Per frame the chain copy is 4.6 MB, of which the marked world /
model blocks are 2.9 MB (740 blocks) and the distinct ones 2.4 MB (470): checking each distinct block's bytes once a
frame would read nearly as much as copying them. Validity without reading the bytes is the prerequisite - for skipping
the copy now, and for draws made with no chain at all (step 5).

**4a - write watch (done 2026-10-24).** Guest RAM is allocated with the OS's write watch (`runtime/ps2_write_watch.h`,
Windows `MEM_WRITE_WATCH`): every write is noted - guest code, host threads, the kernel reading the disc into it (the
stream thread does: page protection was ruled out for that reason) - at no cost until asked. At each VIF1 chain kick
the written pages are folded into per-page generations: 0.18 ms per frame, ~470 of 8,192 pages written per frame. The
RSL observer keeps, per marked block, the pages it is made of (a world REF's payload; a CALLed packet's tags and
payloads) and their generations at its last copy: 84% of copied blocks were unchanged since their previous copy. Write
watch costs guest writes nothing measurable (A/B, alternating: not slower).

**4b - unchanged blocks not copied (done 2026-10-24).** At the kick a world or model block may go uncopied when its
pages are unchanged since a copy the worker has confirmed its recording for (the worker hands confirmations - recording
key, chain - to the game thread at the end of each chain; the game thread keeps per block the copy since which its
pages are unchanged). The walker then writes only the MARK qword with the length word `kSkippedLength` and, after a
CALL, goes on at its return address as if the packet had returned. The worker replays the block's recording by key;
should it be unusable there (VIF mode / cycle differ), the block is rebuilt from guest RAM - unchanged by
construction (a small chain walker for CALLed packets). Results:
- Checked with `VCS_RSL_SKIP_VERIFY=1` (decide, copy anyway, compare the copy's hash with the recording the skip would
  have replayed): 1.1M decisions on the drive, 1.27M over the reference scenes - 0 wrong.
- Per frame while driving the chain copy fell from 4.6 MB to 2.1 MB (marked blocks 2.9 MB -> 0.37 MB); ~206k blocks
  per 10 s replayed without a copy, none rebuilt from guest RAM.
- A/B, same build, `VCS_RSL_SKIP=0` vs on, two alternating rounds: drive 50.1 / 50.4 -> 54.1 / 53.5 fps, render worker
  9.1 -> 8.5 ms; the game thread unchanged (the copy saved is spent again on the fold and the snapshots' bookkeeping).
  Reference scenes ok, tour unchanged.
- "Other call" packets (FUN_002F80A0's shared state packet, the per-frame ones) are skipped too (2026-10-25). At first
  skipping them lost state - VU1 programs ran again because stage tables were missing - and about a quarter of the
  skips decided for them never reached the worker as skip marks. The walker and the observer's open-block tracking
  agreed after all (every chain: skips decided == skip marks written); the marks were lost in the worker. The MARK
  handler's path for a block it records no longer ("changing", `rec.skip`) returned `12 + size` bytes to skip while
  the caller adds the MARK qword's 12 bytes itself: 12 bytes too many swallowed the next tag's VIFcodes - with them,
  when it followed directly, the next block's skip MARK. Before other calls were skipped this only cost the next block
  its recording. Fixed; `VCS_RSL_SKIP_KINDS` (a bit per kind) now defaults to all.
  - On the way the walker gained a safety net (`VifBoundaries`, ps2_memory.cpp): it follows VIF command lengths through
    what it copies and puts a MARK only at a command boundary, and gives a block no length when its payload ends inside
    a command. In VCS it never refuses one (`PS2X_CHAIN_LOG_REFUSED=<n>` logs refusals).
  - Checked: `VCS_RSL_SKIP_VERIFY=1` 759k decisions driving and 1.30M over the reference scenes, 0 wrong; with every
    kind skipped no VU1 program runs in gameplay (`vu1_programs` 0). Reference scenes ok, tour unchanged (15 ok, 2
    relocated, 1 falling as before).
  - Timing: A/B `VCS_RSL_SKIP=0` vs on (all kinds), two alternating rounds under background load: stand unchanged,
    drive game thread 19.3 / 18.7 -> 22.7 / 20.6 ms and fps 43.5 / 44.9 -> 40.7 / 44.9 - within this machine's noise,
    not a measured loss; a profile of the drive puts the walker's work and `skippable` at ~1-2% of the game thread.
    Measure again on a quiet machine.

**4c - draw calls as items (done 2026-10-25).** The world / model draw functions (`FUN_002F7EB0`, `FUN_002F7D58`,
`FUN_002F80A0`) are replaced whole when their state flush is native (game/vcs_rsl_native.h): the flush (when it is all
CNTs - else its tags stay in the buffer, before the item), the queued GS registers and the block become an **item**, and
the packet buffer gets one 16-byte CNT whose two VIFcodes - MARK `0x8000 + index`, a NOP holding `0xD4` and the frame's
number - keep the draw's place in the chain (DECISIONS.md 2026-10-25). The GS packet and the CALL / REF are not
written. At the kick, after the write watch's fold, each item's block is decided as a marked block is: skipped when
unchanged since a confirmed copy, else copied from guest RAM by `flattenBlock` (the walker's rules: tags' upper halves,
payloads, one nested CALL; the pages it is made of and its raster uploads noted as the walker's observer notes them).
At the MARK the worker runs the item's VIF bytes (the flush's UNPACKs / MSCALs, the GS registers' DIRECTHL) and replays
or processes the block - as the packet path, through the same interpreter and recordings.
- **What stays packets**: draws whose flush calls plug-in callbacks or `FUN_001CFAE0` (guest code; ~15% of draws
  while driving, ~1% standing in the city); a model block until the walker has once copied it as `flattenBlock`
  copies it; and every draw while `FUN_00240ED8` caches a sector (game-internals.md 6i: the sector's packets are copied
  into the heap and sent again on later frames - found because item tags from old frames turned up in later chains,
  drawing the wrong blocks and, for a cached sector drawn as an item, recursing into itself). Item tags carry the
  frame's number; a tag of another frame is counted and not drawn (0 in the runs below). `FUN_002F80A0` with its
  shared state packet (bit 31) keeps the flush, GS packet and that CALL in the buffer and makes the block an item.
- **Checked**: `VCS_RSL_ITEMS_VERIFY=1` (the original writes its packets; they are compared with what the item stands
  for, and every block the walker copies with what `flattenBlock` copies) - driving, 3.0M draws and 1.4M blocks,
  0 differ. `VCS_RSL_SKIP_VERIFY=1` with items: 1.04M skip decisions, 0 wrong. `VCS_RSL_WORLD_VERIFY=1` with items
  (each recording against the packet path): 375k, 0 differ. Reference scenes: static ones identical; tour subset
  classes unchanged (15 ok, 2 relocated, 1 falling).
- **Counts** (city spot (1325, -325), per 10 s): ~820k items made (74k world, 423k model, 327k model-b), 2k draws left
  to the packet path for their flush, 2k while caching; at the kick 60% skipped, 40% copied; all seen by the worker,
  none unplaced.
- **Measured** (A/B, same build, `VCS_RSL_ITEMS=0` vs on, three alternating rounds, `--opt none`, uncapped): driving
  110.5 -> 111.3 fps, the game thread's chain submission 1.12 -> 1.02 ms, render worker 4.38 -> 4.29 ms; standing within
  noise. The walker's share of the game thread falls (its copy, `skippable`) by about what the items cost to make
  (`itemDraw` ~0.5%): the copy savings were taken by 4b already. What 4c buys is that these draws no longer depend on
  the chain's bytes - the ground step 5 builds on.
- `VCS_RSL_ITEMS=0`: the draws' packets as before.

### 5. An empty chain

RSL_SUBMISSION.md phase 4. What is left in the VIF1 chain after steps 1-4 - 2D (Im2D: already drawn natively, still
sent), effects, the Z clear, texture uploads - moves to its call sites. Then a normal frame sends nothing through
VIF1 or the GS front end; the emulation stays as the switchable reference.

Done when: in gameplay, cutscenes and driving the frame's chain is empty (or not kicked), pictures as the packet
path's, tour and missions clean.

**What is left in the chain (2026-10-25, after step 4c).** `VCS_RSL_CENSUS=1` (top-level tags by DMA id, first
VIFcode and data region; items counted as one tag) and `VCS_TRACE_RSL=1` (bytes built per function per frame; it runs
with items off, so the draws' own tags are charged to the flush there):

| per frame | city spot (1325, -325), standing | AI drive |
|---|---|---|
| top-level tags / payload qwords | ~1,000-2,000 / 28-52k | ~1,000 / 29k |
| items (world / model draws) | ~280 tags | 120-320 |
| texture uploads: REF + DIRECTHL to raster data (model heap, cells) | ~310 tags, ~42k qwords | ~150, ~23k |
| state: CNT / REF + FLUSHE (UNPACKs of matrices, constants; context packets) | ~800 tags, ~4k qwords | ~400, ~2k |
| GS register packets: CNT + DIRECTHL | ~190 tags, ~3.2k qwords | ~110, ~2.8k |
| standalone MSCALs (setup programs), CALLs (context / cached packets) | ~100 / ~125 | ~45 / ~56 |
| 2D: CNT + DIRECT (Im2D) | ~54 tags, ~1.3k qwords | ~56, ~1.2k |

Who writes them (bytes per frame, standing / driving): Im2D `FUN_002FB690` 41 / 185 KB (103 / 797 calls); the GS
queue senders `FUN_002F7908` 25 / 21 KB and `FUN_002F7B30` 7.6 / 6.9 KB; texture binds `FUN_002F62B8` 19 / 44 KB and
`FUN_002F6A78` 3 / 7 KB (their uploads through `FUN_003298D8`); the texture matrix `FUN_002F7090` 5.5 / 21 KB;
`FUN_001CEF18` 2 / 16 KB; unidentified so far: `FUN_0040C430` (5 calls, 17 KB), `FUN_0032D480` (16 calls, 14 KB),
`FUN_0040F6C8` (1-20 calls, up to 6.6 KB), `FUN_00266868` (7 calls, 3 KB), `FUN_002F3DD0`, `FUN_002A92C0`; immediate
geometry `FUN_002F8250` (30 calls driving); the frame's camera packet (`FUN_002F53D8` / `FUN_002F9FA0`), VU1 code
uploads (MPG REFs into `.text`) and the root lists' fixed tags.

**The plan.** Each part the way step 4c went: a hook replaces the emitter when its effect can be stated exactly, the
effect travels as an item op in chain order, a verify mode lets the original write its packets and compares, and a
switch brings the packets back. Two rules learnt in 4c hold throughout:
- **Copied packets.** The game copies packet ranges and sends the copies again on later frames: `FUN_00240ED8` a
  sector's whole draw, `FUN_002409C8` each material's setup (texture bind + GS packet) - game-internals.md 6i. An
  item tag inside such a range would be sent again without its item. Every emitter below stays packets while a cache
  is being filled (`g_caching`, extended to `FUN_002409C8`'s material cache), or the cached block itself becomes a
  recorded block whose replay the worker owns.
- **Validity at the kick.** Anything an item takes from guest RAM by reference (a REF's payload, a raster, a CALLed
  packet) is decided after the write watch's fold and copied there if it changed - never read later on the worker.

Order (by what it removes, then risk):
- **5a - state ops** (M). Item ops without a block: VU memory writes (an UNPACK's result: address, fields, data -
  written as the recorder writes them), MSCAL of a setup program, GS register writes (A+D pairs, sent as the
  DIRECTHL they replace). Sources: the GS queue senders (`FUN_002F7908`, `FUN_002F7B30`), the setters (`FUN_002F7090`,
  `FUN_001CEF18`, `FUN_002F9FA0`, `FUN_002F9CE8`), the flush's CALL / REF cases (render-context packets, the table at
  `0x742AB0`, the instance matrix REF) as item blocks with the 4c rules, and the camera packet. Removes the CNT / REF
  FLUSHE, CNT DIRECTHL, CNT MSCAL and most CALL rows. Prerequisite for 5c (2D needs its GS state in order).
- **5b - textures** (L). The biggest volume: an upload becomes an item op "raster R (RAM address, psm, size, mips,
  CLUT) into VRAM slot (TBP, TBW)"; the raster's pages decide at the kick whether its bytes are copied (write watch,
  as blocks) or known; the worker applies it as the upload it replaces - VRAM written where something still reads VRAM
  (render targets sharing memory with textures: FBP 420's reflection map and bloom), else straight into the texture
  cache keyed by raster (step 3d's names), so an unchanged raster is neither copied nor swizzled nor decoded again.
  Checked against the VRAM path (`PS2X_HW_DECODE_VERIFY`-style, every texture a frame binds). This is also where
  `FUN_002F62B8`'s per-bind CLUT re-uploads stop costing (game-internals.md 6i "Texture streaming").
- **5c - 2D** (M). Im2D as items from `RwIm2DRenderPrimitive` (`FUN_002FBE08` / `FUN_002FBE68`) instead of
  `FUN_002FB690`'s CNT + DIRECT: the primitive's vertices (the 16-byte Im2D format) and the GS state from 5a; the HUD
  layer already draws them natively from the packets (NATIVE_RENDERER.md "HUD layer", "PATH2 packets recorded
  whole"), so this moves where the draw comes from, checked vertex by vertex against the packet path
  (`PS2X_HW_PACKED_VERIFY`'s comparison).
- **5d - immediate geometry and effects** (M-L, unknown until read). Identify `FUN_0040C430`, `FUN_0032D480`,
  `FUN_0040F6C8`, `FUN_00266868`, `FUN_002F3DD0`, `FUN_002A92C0`, `FUN_002F8250` (decompile: `tools/eedecomp.py` needs
  a JDK 25 for the installed Ghidra since its last update; the recompiled code's disassembly comments work meanwhile).
  Prebuilt blocks become item blocks (4c); per-frame vertices item data; their VU1 programs are native already.
- **5e - frame structure** (S-M). VU1 code uploads as "code image" ops (the native handlers check code by generation
  in VU1 code memory: the op keeps that memory as the upload would), the camera / root lists' fixed tags, the Z clear
  (the zclear program's strips as one native clear by target role), the three render-target reads (the reflection
  map's copies into FBP 420, the sky's self-copy) as native passes by role (step 3d).
- **5f - flushes with plug-in callbacks** (M). ~15% of draws while driving keep their packets because the flush calls
  guest plug-in code (skinned peds, vehicles). Read which plug-ins (render context list `+0x114`, game-internals.md
  6i) and port their callbacks' packet output natively, or let the callback write and take its bytes into the item
  at the next draw call (the 4c verify mode's way of reading what the original wrote).
- **5g - no walk** (S). When a frame's chain holds only item tags and fixed tags, the kick hands the item list to the
  worker without walking or copying the chain (the write watch's fold stays for the blocks' validity); the walker and
  the VIF interpreter remain the reference path (`VCS_RSL_ITEMS=0`).

**5a done 2026-10-25 - state ops.** An item may hold VIF bytes alone (`hasBlock` false). The GS queue senders
(`FUN_002F7908`, `FUN_002F7B30`), the texture matrix (`FUN_002F7090`), `FUN_001CEF18`, `FUN_002F9CE8` and the native
state flush when it is all CNTs (a standalone flush, `FUN_002F7388`'s, or one inside a draw left to the packet path)
write no packets: their VIF bytes - exactly what the walker would make of the CNTs they replace - go into the open item
or a new one with its tag; ops in a row share an item while nothing else is written to the packet buffer between them
(and a draw right after them joins it). Their effects on guest memory (the GS queue, the dirty word, `FUN_001CEF18`'s
return value) are kept. Packets stay packets while a sector or a material is being cached (`FUN_002409C8`'s material
cache: from `FUN_00408F30` called at `0x240D14` to `FUN_002F7B30` called at `0x240D1C`, game-internals.md 6i).
- Checked: `VCS_RSL_ITEMS_VERIFY=1` now also compares every op's original packets with the op (driving: 2.67M draws
  and ops, 0 differ; 563k blocks, 0 differ). Reference scenes: `loading` identical, `pause_map` within the run-to-run
  noise it has between any two captures (1-2 levels on ~7-10k pixels); tour subset classes unchanged.
- Counts (city spot, per 10 s): ~450k ops making items of their own, ~52k merged into the item before them; no draw
  merged yet - a texture bind's upload packets sit between the setters and the draw (5b). Census at that spot, same
  window, ops off / on: top-level tags 1,055 -> 848 a frame; CNT FLUSHE (the GS senders' and setters' CNTs start with
  FLUSHE) 236 -> 59, CNT MSCAL 68 -> 46, items 158 -> 222. CNT DIRECTHL (~90) is not theirs and stays.
- Measured: A/B, same build, `VCS_RSL_OPS=0` vs on, three alternating rounds, `--opt none`, uncapped: driving 104.1 -> 113.9 fps (game thread 8.49 -> 7.72 ms, chain submission 1.15 -> 1.03, worker 4.64 -> 4.37); standing even (183 / 182 fps). The machine's run-to-run noise is about 5%: the drive's gain is consistent across every column but should be taken as 'up to ~9%'.
- `VCS_RSL_OPS=0`: the setters' packets as before (items for draws stay).

**5b done 2026-10-25 - texture uploads as reference ops.** `FUN_003298D8` (game-internals.md 6i "Texture uploads")
is replaced when the destination's transfer header already exists (its tree lookup, read-only, natively; a first
upload to a destination runs the original, which makes the node): its two REFs become **reference ops** - the two
VIFcodes and the payload's address and size - in the open item or a new one, whose tag goes into the slot the caller
reserved (the rest of the slot gets empty CNTs; the cursor is not moved). At the kick, after the fold, every item with
references has its VIF bytes rebuilt: its inline bytes and each payload copied from guest RAM where the reference
stands - exactly what the walker copies for a REF - with each raster upload's place noted, so textures keep their
names (step 3d) when the worker sends the pixels. A texture's mip levels share one item.
- **A bug on the way**: an open item of inline ops that then took references kept `refAt` 0, so its uploads copied
  the frame's first references - wrong pixels under the right names, texture-cache misses on every bind and the GS
  stage decoding all the time (driving fell to 36 fps). The tag verify could not see it (it checks what the game
  writes, not how items resolve). Fixed; each reference now carries its item and the kick checks it (counted:
  "references not their item's", 0).
- Checked: `VCS_RSL_ITEMS_VERIFY=1` (the tags the original writes for each upload against the two REFs the op stands
  for) driving 4.85M draws / ops, 0 differ; texture binds 99.998% found cached (as before); reference scenes as before
  (static identical, `pause_map` within its noise; moving scenes looked over - same textures); tour subset unchanged.
- Census at the city spot: chain payload ~22k -> ~7.9k qwords a frame (the pixels and headers are in the items).
  Left as packets: uploads while a sector or a material is being cached, and a destination's first upload.
- Measured: not settled. The cleanest A/B (uploads off / on, three alternating rounds, per run) gave driving 107.1 -> 103.4 fps and standing even; the kick then copied each payload twice (into a buffer, then into the arena) and now builds the item's bytes in place - not re-measured, because the machine was under changing background load for the rest of the session (runs with uploads off fell to 66-74 fps as well). In one hour of runs ~half of the runs with uploads on had windows of 40-100 ms frames (game thread and render worker waiting together; the GS stage not saturated, no texture misses, no drains, no stale items); in the 27 runs after it none did, with or without instruments - most likely the same background load, not proven. To measure on a quiet machine: alternating runs of the same build (stand, then the bench drive), `VCS_RSL_UPLOADS=0` vs on. `VCS_RSL_SLOW_LOG=1` logs kicks whose resolve takes > 3 ms and chains the worker takes > 15 ms on; `PS2X_RENDER_THREAD_LOG=1` now also counts GS stage drains.
- `VCS_RSL_UPLOADS=0`: the uploads' REFs as before.

Expected gains, honestly: the game thread's share of handing frames over (the walk, copy, fold and bookkeeping:
~5% while driving) shrinks with every part; the render worker gains where an op replaces VIF parsing and VU-memory
round trips (5a, 5b), and most from 5b (texture bytes). Each part is measured A/B as 4c was.

## Not "native" but part of "done"

Completeness (milestone 2: every mission to the credits), the open fall-through spot (-925, 1175), the stale
regression baseline for the loading / pause-map scenes (needs the user's picture sign-off), 60 fps fixes (milestone
4), game code under ~6 ms (milestone 7), settings / controls polish (8), Vulkan / Metal and Linux / macOS (9).
