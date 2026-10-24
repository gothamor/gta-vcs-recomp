# Render submission at the RSL level - plan

Status: **plan** (2026-10-24), milestone 7 of NATIVE_PORT_ROADMAP.md ("render submission at the RW level"). VCS does
not use stock RenderWare: it draws through Rockstar Leeds' own layer, RSL (game-internals.md 1), so the level to hook
is RSL's.

## Where things stand

Every 3D draw is native by now - no VU1 program runs in a normal frame, the C++ reproductions' strips skip the GS
front end (NATIVE_RENDERER.md "Draw sources") - but every draw still **starts as DMA packets**:

1. **Game thread** (recompiled EE code): the RSL mesh code (`0x2F62B8..0x2FB690`, `FUN_003298D8`) and the world
   sector walk (`FUN_0023E748` -> `FUN_00240ED8` -> `FUN_002409C8`) write, per visible model or sector, UNPACKs of the
   per-instance data (matrices, lights, tint) and a CALL / REF to the model's prebuilt packet in the model heap
   (`0x1200000..0x17FFFFF`), plus MPG uploads of the VU1 code, into the frame's packet buffer (`0x520F90`, two halves
   of 0xE6670 bytes). 7-14k DMA tags a frame (game-internals.md 6a).
2. **Render worker**: copies the chain, walks it through VIF1 (UNPACK into VU1 memory), and at each MSCAL the native
   handlers read the VU1 memory back to find what to draw (`vcs_hle_world.cpp`).
3. **Textures** reach the GPU through emulated GS memory: the game's IMAGE uploads are written into GS VRAM
   (swizzled), then decoded into the GS backend's texture cache, CLUTs hashed.

Measured (2026-10-24, busy city spot (1325, -325), 60 fps cap):
- render worker: VIF1 processing 13%, `memmove` (chain copies) 6%, GS VRAM writes (`writeRectFast`) 8%,
  `UploadImage` 3%, `ClutHash` 2% of its samples;
- game thread: the RSL mesh functions ~4% of samples standing still (the thread waits 36% at the cap). Driving is
  where the game thread is the limit (~17-20 ms; NATIVE_PORT_ROADMAP / PROGRESS milestone 4) - not yet broken down by
  function: the first measurement below.

What moving up to RSL buys: the game thread stops building packets, the worker stops parsing them, textures can be
decoded once at load instead of per upload, and the native draws get their inputs as data instead of as VU memory.
The emulated chain then only carries what is not converted yet (2D, effects) - and finally nothing.

## What is not known yet (phase 0 finds out)

- The RSL entry points: which function draws one model instance / one mesh (the one writing the CALL tags), its
  arguments and the instance structure (matrix, lighting, material list, mesh packet address); the same for world
  sectors, peds (skinned: bone matrices, `0x2b50`'s copy), vehicles (env map), the 0x02c0 instanced re-draws.
- Where render state comes from: GS register packets (TEX0 / ALPHA / TEST / CLUT loads) inline in the model packets,
  separate packets from the material code, or both; where texture uploads are queued (the RSL texture cache: VRAM
  placement, `.XTX` rasters, game-internals.md 7).
- Side effects of the packet building the game relies on: the packet buffer's fill level (`FUN_00476770`; draw code
  skips work below 0x12C00 bytes left), DMA ordering between 3D and the 2D / effect packets in the same chain, any
  per-frame state the mesh code updates (LOD, fade, visibility counters).

## Phases

Each phase keeps the packet path as the reference with a switch back, as everything before (AGENTS.md rule 1).

### Phase 0 - map the submission path (research, no behaviour change) - done 2026-10-24

Result in game-internals.md 6i: RSL keeps state in memory and flushes it at each draw; the draw entry points are
`FUN_002F7D58` (model instance, CALL), `FUN_002F80A0` (variant), `FUN_002F7EB0` (world geometry, REF), `FUN_002F8250`
(immediate geometry) and Im2D (`FUN_002FB9D0` -> `FUN_002FB690`); textures go through `FUN_003298D8`'s upload cache.
Numbers while driving: ~470 KB built a frame, ~4.7 MB through VIF1 (90% prebuilt), and about 17% of the game thread
(~3 ms of ~19) building and handing over packets. So phase 2 can take ~2-3 ms off the game thread while driving (the
state setters and culling stay game logic) and most of the worker's VIF1 / copy time; worth continuing, but it does
not by itself make 60 fps driving: the other ~80% of game code is game logic.

Tools: `VCS_TRACE_RSL=1`, `PS2X_CHAIN_CENSUS=1`, `VCS_PROFILE_GUEST_FILE`, `tools/eedecomp.py`.

Planned (kept for reference):

- A **submission trace** (`VCS_TRACE_RSL=1`): before-hooks (DECISIONS.md "Rule for hooks on recompiled functions") on
  the candidate functions log their arguments and the packet-buffer range each one writes; the worker tags every
  native draw with the chain offset it came from, so each draw is tied to the RSL call that queued it.
- Profile the game thread while driving (`tools/bench.py` drive leg with `VCS_PROFILE`), summed over the RSL / sector
  functions and the packet-buffer writes: how much of the ~20 ms phase 2 can remove.
- Deliverable: game-internals.md section "6i. RSL submission" (functions, structures, state and texture flow) and
  the numbers.

### Phase 1 - shadow draw list - 1a (coverage) done 2026-10-24

**1a, coverage** (`VCS_RSL_SHADOW=1`, game-internals.md 6i): the RSL draw functions account for every 3D draw in
the chain - model instances (`FUN_002F7D58` / `FUN_002F80A0`), world geometry (`FUN_002F7EB0`) and immediate geometry
(`FUN_002F8250`) each wrote exactly their own CALL / REF, all matched (driving: 495 / 79 / 19.5 a frame); what lies
outside them is frame and material state (the camera's packet, the sectors' cached setup blocks). So hooking these
functions is enough to see every 3D draw: phase 2 does not need to find draws anywhere else.

**1b, state** (done 2026-10-24): the shadow record carries the draw's RSL state (render context, dirty bits, the
matrix blocks `0x520860..`, VU 49-53 from `0x742AE0`, the viewport, guard); every MSCAL of a draw program is tied to
its draw by its offset in the copied chain (runtime chain observer: flat offsets, the chain's buffer as key, the
MSCAL's stream offset - works with the render worker) and its starting VU memory is checked group by group
(game-internals.md 6i, the table there). Result over a bench run (city spots and the drive): every MSCAL inside a draw's
stream; camera, camera position, slot table, tags, colour, fog, flags, clip planes and guard 100% the RSL state; the
object matrix and the viewport differ for model draws whose packets upload their own (a static object's matrix, a
depth range that then stays for the draws after it); **0.000% unexplained**.

What this means for phase 2: a world draw can be made from the RSL state alone; a model draw needs the RSL state plus
the state its packet uploads (decoded once per packet, the packets are static) and the VU-resident semantics (a value a
packet sets stays until something overwrites it). World sectors first, as planned.

### Phase 2 - world sectors: done 2026-10-24 (on by default, `VCS_RSL_WORLD=0` = packet path)

`src/game/vcs_rsl_world.cpp`. A world block (the REF of `FUN_002F7EB0`) is all the same shape (game-internals.md 6i):
per packet a header, positions, UVs and colours UNPACKed relative to TOP, then MSCAL 0x30 - no constants.
- **Where the draw goes in**: the RSL world draw call is recorded by a before-hook; as the frame's chain is copied a
  VIF MARK qword is put before its REF's payload (runtime chain observer; the qword's second word, a NOP, holds the
  payload's length). At the MARK - in stream order on the thread that processes the chain, after the draw's state
  flush and GS register writes - the block is drawn and the handler skips its bytes.
- **How**: the first time a block is seen it runs through the VIF interpreter (exactly the packet path) while a
  recorder notes each UNPACK's result (the VU qwords as written, relative to TOPS, and which fields it wrote), each
  MSCAL and the native handler's mesh (content key + converted vertices). Later frames replay: the recorded fields are
  copied into VU memory and each MSCAL runs through the runtime's own MSCAL (`PS2Memory::vu1Mscal`: TOP / DBF, the HLE
  hooks, the VU1 fallback), the native world handler taking the recorded mesh - no VIF parsing, unpacking, mesh
  hashing or vertex conversion. A recording is used while the block's bytes hash the same (streaming replaces cells)
  and the VIF mode / cycle match; one that would depend on state from before the block (a masked UNPACK before the
  block's STMASK / STROW, a COL field, MODE != 0, other cycles, other VIFcodes) is never replayed.
- **The bytes are the chain's copy**: the first version replaced the REF's payload by the MARK and read the block from
  guest memory when the MARK ran. The render worker processes a chain while the game builds the next frame, so a cell
  streamed out in between would have been read as whatever replaced it; the payload now stays in the copy (what guest
  memory held at the kick) and is only skipped. Copying it costs little next to processing it.
- **Checked** (`VCS_RSL_WORLD_VERIFY=1`: each hit drawn by the packet path and recorded again, compared op by op -
  UNPACK addresses / fields / data, MSCALs, meshes, DIRECT data, the mask / row / cycle the block sets): city spot 86k
  hits, a bench run (city spots and the drive, cells streaming in) 174,301 hits - **0 differ**; every block
  recordable. Reference scenes: static scenes identical, moving ones within timing noise; tour subset classes
  unchanged (15 ok, 2 relocated, 1 falling - the known spot).
- **Measured** (one bench run each): within run-to-run noise (busy city spot worker 10.2 -> 9.6 ms; the drive took
  another route). Expected: world geometry is ~5% of the VIF1 payload (phase 0) - the work it removes is small; the
  value is the path itself, which the model categories build on.
- Not combined with `VCS_RSL_SHADOW` (its MSCAL-to-draw mapping uses stream offsets, which replayed MSCALs do not
  set).

### Phase 2 - model instances: done 2026-10-24 (on by default, `VCS_RSL_MODELS=0` = packet path)

The same record / replay for the CALLed packets of `FUN_002F7D58` and `FUN_002F80A0` (rigid and skinned models,
vehicles, props - game-internals.md 6i "A model packet"):
- **Where**: both functions end by writing `CALL packet` (qwc 0); before-hooks note the packet. As the chain is copied
  the runtime walker puts the MARK qword after the CALL tag (and its own payload), copies the CALLed packet's tags and
  data as before, and writes the packet's length into the MARK qword when the matching RET returns (a CALL whose RET
  the chain never reaches keeps length 0: the packet path runs on). One recording covers the whole CALL ... RET
  stream, nested REFs included. Every other CALL in the packet buffer is marked too ("other call": FUN_002F80A0's
  state packet, per-frame packets) - a recording is exact whoever wrote the packet.
- **What a model packet adds** to what the world needed: DIRECT / DIRECTHL GS data (recorded and resent to PATH2
  through the interpreter's own `submitVif1DirectData`), STCYCL (the cycle each UNPACK saw is checked, the last one
  restored), the other draw programs' MSCALs (0x0478 setup, 0x02C0 re-draw; meshes only for 0x0030), and UNPACKs that
  write only some fields - V3 / V2 formats unmasked leave w as VU memory held, write-protected mask fields too. The
  first verify run found these (a skinned packet's w differed between two recordings of the same bytes:
  `0x802A` / `0x801A`); a replay now writes only the fields the UNPACK wrote, and restores only the mask / row / cycle
  the block itself set.
- **Packets the game rebuilds**: one whose bytes changed on three hits running is processed by the packet path for
  the next 63 hits without hashing or recording, then recorded once more to see whether it settled; one not
  replayable (a few state packets use BASE) for the next 255. Packets that alternate between rebuilt and unchanged
  frames are still recorded: the replays on the unchanged frames are worth more than the recordings cost.
- **Checked** (`VCS_RSL_WORLD_VERIFY=1`, which covers both): city spot (-700, -500) ~310k blocks per 10 s, a bench run
  1,217,887 blocks - **0 differ**. Reference scenes all ok (same flyover perf sample as the capture before this
  change); tour subset classes unchanged (15 ok, 2 relocated, 1 falling).
- **Measured** (one bench run each, `VCS_RSL_MODELS=0` vs on): render worker Fort Baxter 5.5 -> 4.2 ms, busy city
  spot (1325, -325) 10.8 -> 9.2 ms, the drive 15.8 -> 13.4 ms; game thread unchanged (the packets are still built).
  At the city spot per 10 s: 248k model, 106k model-b, 61k world blocks all replayed, ~1.29M draw packets replayed;
  other calls 89k (35k replayed, 36k recorded, 10k not replayable, 7k changing).

Next: the game thread still builds every packet (phase 0: ~17% of it). With draws replayed from recordings keyed by
packet, the hooks can move from "mark the packet" to "replay the packet" without the chain: the draw call's state
flush and its recording are what the native side needs (phase 2's planned form, below).

### The game thread after phase 2 (2026-10-24): profile first

A drive profile with callers (`VCS_PROFILE_CALLERS`, new) showed the draw calls' own packet writes are small; the game
thread's rendering overhead was elsewhere, and that was done first:
- **The chain copy** (~10% of the game thread: `memmove`, the CRT copy routine, `ZwFreeVirtualMemory`): the 4.7 MB
  chain was often copied into a small pooled buffer (the pool mixed GIF copies with chains) and reallocated its way
  up. Pooled buffers are now taken by size and the chain buffer is reserved from the last chain's size: ~5.7%.
- **VU0 microprograms still interpreted** (0x0C10, 0x0068, 0x0E98, 0x0E30: ~5%): native, bit-exact
  (game-internals.md 6c).
- **The state flush** `FUN_002F8710` (2.3%): native for the ~90% of flushes that call no plug-in callback, byte-exact
  against the original (`VCS_RSL_NATIVE_VERIFY=1`: ~700k flushes, 0 differ): 1.3% left (the callback flushes).
- A/B against the previous build (alternating, two rounds each): drive game thread 18.6 -> 17.8 ms, 45.8 -> 51.0 fps;
  standing unchanged (within noise).
What is left in the RSL range (~12% while driving) is spread over many emitters (texture setup `FUN_002F62B8` 1.5%,
the Im3D vertex writer `FUN_002FC210` 1.3%, Im2D, the camera / frustum setters); the chain copy is still ~3.5%.
Replacing the draws without the chain (above) removes the copy and the flush together.

Planned (kept for reference):

- The hooks build a native draw list from the RSL calls (mesh id + content key, matrices, lights / tint, material,
  render state) **alongside** the packet path, which still draws.
- Every frame the shadow list is matched against what the packet-level handlers decoded (same mesh key, bit-identical
  constants, same order relative to the other draws) - a `PS2X_HLE_VERIFY`-style counter: matched / unmatched / extra
  per category. Target: every world, rigid, skinned and re-draw packet matched.
- No picture change; cost only with the switch on.

### Phase 2 - RSL draws replace packet draws, one category at a time

- Order by payoff: world sectors (most draws, static), rigid models / vehicles (incl. env map), skinned peds, 0x02c0
  re-draws.
- Per category the hook replaces the packet building (full replacement is allowed by the hook rule) and posts the
  native draws to the render worker **in chain order** (the worker queue already orders chains and frame ends: the
  draws go in as items between the chain parts around them). `VCS_RSL_DRAW=0` (and per category) = packets.
- Checks: per-draw constants against the packet path in a dual mode (both run, one draws); reference scenes at 1x
  (static scenes identical); tour subset and missions; `tools/bench.py` game / worker ms before and after.
- Keep the side effects found in phase 0 (buffer fill level, counters) - reproduced or left to the game's code.

### Phase 3 - textures without GS memory (for 3D)

- Decode the RSL rasters (`.XTX`: PSMT4 / T8 + CLUT, game-internals.md 7) to GPU textures when they are loaded or
  streamed, keyed by raster; RSL-level draws bind them directly. The game's VRAM uploads for those textures are
  skipped once nothing reads them through GS memory.
- Render targets read as textures (reflection map, bloom, frame copies) stay in the GS-memory model until the effects
  are native (milestone 3); a draw that samples one keeps going through the GS backend's lookup.
- Revisits DECISIONS.md 2026-10-23 "Textures from the GS backend's decoded cache" - that choice named this point as
  when a raster decoder becomes worth having.

### Phase 4 - an empty chain

- What is left in the chain: 2D (Im2D: HUD, text, radar, menus - their primitives already go to the native HUD
  layer), effects (PATH2: reflection map, coronas, colour quads), the Z clear. Each moves to its RSL / Im2D call site
  (milestone 3 step 2's method); then a normal frame sends nothing through VIF1 or the GS front end, and the
  emulation stays as the switchable reference.

## Done when

- In gameplay, cutscenes and driving: no 3D draw comes from DMA packets (`gs_draws`, the VIF1 / UNPACK counters at
  ~0 for 3D), pictures as the packet path's (static scenes identical, moving ones within run-to-run noise), tour and
  missions clean.
- Game thread time while driving measurably lower (target from phase 0's breakdown), render worker no longer parsing
  3D packets.

## Risks

- **Ordering**: 3D and 2D / effects share one chain; draws that leave it must keep their place (the bloom and the
  reflection map read the frame mid-way, game-internals.md 6g).
- **Hidden dependencies** on the packet buffer or on VU1 memory contents left by earlier packets (the setup programs
  write matrices the next programs read - fine while both are native, a trap if only one side moves).
- **Hook rule**: the RSL functions must not be ones that yield to the scheduler mid-body; checked per function in
  phase 0.
- **Size**: L-XL. Phase 0 and 1 are low-risk and tell whether phase 2's gain is worth it; stop there if driving's
  game time turns out not to be packet building.
