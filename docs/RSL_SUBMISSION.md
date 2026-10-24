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

### Phase 1 - shadow draw list

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
