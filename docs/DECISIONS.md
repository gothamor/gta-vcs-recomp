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

## 2026-10-23 - Post effects: exact GS emulation at 640x448 first, resolution-aware reads for higher resolutions

The user asked for the post effects to stay as authentic as possible at the original resolution first, then at
higher resolutions and other aspect ratios. At 640x448 the emulated GS draws are the reference, so the first step
made them match the GS (pixel centres) instead of replacing them with approximations, and kept them correct at
higher internal resolutions by reading 1:1 copies at full resolution. Effect-level shaders (one pass per effect,
independent of the PS2 frame-buffer layout) come with the native frame (milestone 3 step 4), checked against this
emulation at 640x448.

## 2026-10-23 - Native HUD: the game names its primitives, the GS packets still carry them

Milestone 3 step 2 draws the HUD natively (docs/NATIVE_RENDERER.md "HUD layer"). Choices:
- **The game's own Im2D calls still run** and build their GS packets; the renderer takes the HUD's primitives out of
  the frame by matching them against the Im2D calls of the frame (vertex positions), then draws them at the window's
  size. Replacing Im2D outright would also need RenderWare's texture cache (VRAM placement, CLUT uploads) and its
  render-state translation reproduced; going through the packets keeps those exact and leaves a switch back
  (`VCS_HUD_NATIVE=0`) at no cost.
- **Textures from the GS backend's decoded cache**, not a decoder for RenderWare rasters: the HUD textures reach VRAM
  through the game's own uploads, and the cache keys them by content. A raster decoder becomes worth having when 2D is
  drawn without GS packets (milestone 3 step 4, a native frame).
- **Anchors from where an element sits** (grouped boxes, the 4:3 screen in thirds with a dead zone around the middle),
  not a table of HUD call sites: it covers text boxes, timers and counters the game positions itself, with nothing to
  keep in step with the code. Elements may change side as they grow (a help box past the middle stays left: the
  rule uses its middle and its left edge).
- The HUD placement **default stays "Keep 4:3"**; "Screen edges" is a choice in the menu (authentic by default).

## 2026-10-23 - Clipped world packets: correct picture first, then the C++ clipper

GPU clipping of the packets the VU sends through its clipper drew GON_C3's poolside ground black. The user chose the
correct picture as the default (clipped packets on the VU1, flyover 30 -> 25.5 fps) and milestone 3 to start with the
VU clipper. It is reproduced bit-exactly in C++ (game-internals.md 6h); its GIF packets go to the GS as the VU's
kicks would, so the picture is the VU's and the cost is close to GPU clipping. GPU clipping stays behind
`PS2X_HLE_CLIPPED=1`.

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

## 2026-10-23 - Render submission on its own thread; frame-rate independence next
- The user's order: the render submission worker first, then frame-rate independence (a variable time step:
  "remove dependence on the framerate for gameplay"), not fixed-step interpolation. The worker comes first because
  with game code and render submission in series on one thread (~18 + ~17 ms driving) a variable step would barely
  raise the frame rate, and one large risky change at a time keeps bugs attributable.
- Design (runtime/ps2_render_worker.h): the game keeps its synchronous view - chains are copied when kicked and the
  DMA completes at once, as before - and the work runs behind it in kick order. Everything the game can read back
  fences (waits for the worker) instead of being made asynchronous; measured, the game only reads the GS CSR (~5 a
  frame), so fences cost little. Rare game-thread entries into the GS (library stubs, native GIF fast paths) fence
  or step aside rather than being ported to the queue. At most one frame of lead (latency stays one frame).
- Kept outside ps2_memory.h (a free-function module with its own buffer pool) so the change does not rebuild the
  generated code. `PS2X_RENDER_THREAD=0` is the switch back.

## 2026-10-23 - Profile-guided, ThinLTO build for play; plain build for development
- `tools/build.py --opt pgo` (ThinLTO + PGO) is what to play and measure with: game code ~20-30% faster
  (PROGRESS.md milestone 4). The profile comes from an instrumented build (build/pgo-gen) playing tools/bench.py and
  the reference scenes; it lives in build/pgo (derived from the user's own game, never committed) and is retrained
  with `--opt pgo --train` after large changes - a stale one still builds, changed functions just lose its benefit.
- `--opt none` stays the development setting (no ThinLTO relink); the choice is kept in the CMake cache until changed.
- Calls between recompiled functions got an inline fast path in the runtime header rather than a recompiler change:
  same effect on every call site, no regenerated code, and `PS2X_FAST_DISPATCH=0` switches it off.

## 2026-10-25 - Independent timing up to 240 fps; the ped-movement branch below step 0.5 patched
- Found what broke below the 0.5 step (docs/PERFORMANCE.md 5, game-internals.md 4): FUN_001f1d38 turns a ped's
  animation movement into a velocity with `* 0.5` instead of `/ step` when the step is under 0.5 - a branch the PS2
  never takes (CTimer floors the step). One instruction patch (game/vcs/overrides.toml: the constant it compares with
  becomes 0.001) sends every real step down the `/ step` path; steps of 0.5 or more run exactly as before.
- The audit then matches 1/30 - 1/100 at 1/144, 1/200 and 1/240 (running, driving, falling; sprint -5% / -11% at
  1/200 / 1/240), so independent timing's ceiling goes from 100 to 240 fps (step floor 50/240). Caps offered: 30 / 60
  / 100 / 120 / 144 / 165 / 240 / display refresh / unlimited (both up to 240). Missions above 100 fps are not
  playtested yet; the four frame-counter blinkers outside the 2D pass blink faster as before.

## 2026-10-24 - Independent timing capped at 100 fps for now
- VCS clamps its time step to at least 0.5 (1/100 s; Vice City used 0.01). With the clamp lifted, the frame-rate
  audit shows the player running at a fifth of the speed at 1/120 s - code somewhere assumes steps of 0.5 or more.
  Rather than run the game fast (keeping the clamp above 100 fps) or let it break, independent timing keeps the
  game's 0.5 floor and caps the frame rate at 100. The cap setting offers 30 / 60 / 100 / display refresh (up to
  100) / unlimited (up to 100). Finding and fixing what breaks below 0.5 (then 120 / 144) is phase 3 work;
  `VCS_MIN_STEP` lowers the floor to test it.

## 2026-10-24 - Movies played natively; the intro skippable from the settings
- CMoviePlayer::Play is replaced at its entry rather than feeding the game's own libmpeg / IPU player: no IPU, no
  movie sound through the IOP, the picture drawn by our presenter at 4:3 like the PS2's TV. The game thread waits in
  Play as it did in the game's player.
- Skipping as on the PS2: the logos can't be skipped (the game passes a threshold of 1,000,000 frames), the intro can
  with Start / Cross. The user asked to keep skipping the intro possible: Display > Skip intro movies skips both
  movies; off by default (authentic). Test tools set VCS_SKIP_MOVIES=1 so their boot timelines stay as they were.

## 2026-10-24 - Sound without the IOP: Rockstar's sound code ported onto a native libsd and the existing SPU2 core
- The roadmap's "native mixer" is the SPU2 core we already had (ADPCM, ADSR, interpolation, reverb - in C++, checked
  against hardware behaviour); what was emulated around it was MIPS code: Sony's libsd and Rockstar's CDSTREAM sound
  engine. Those are ported function by function (src/sound/libsd.cpp, src/game/vcs_sound.cpp), so the sound is the
  same by construction and checkable write for write; a different mixer would only have added a second way to sound
  different. A higher-quality mixer can later sit behind the same libsd.
- The IOP's threads became steps on the EE thread (RPC work in the call, the music thread one step per semaphore
  signal, max count 1 as the IRX creates it). Disc reads complete at once; DMA transfers keep their duration (the
  game opens a radio station and sends its volume 3 samples later - it relies on the preload taking that long).
- Tables the module carries (112 stream names, SPU2 addresses) are read from the disc's own MODULES\CDSTREAM.IRX at
  start (checked to be SLUS-21590's), so nothing from the game sits in the source; only code constants (bank first
  sound numbers, slot addresses) are written out.
- Checked by record / replay (tools/soundcheck.py) rather than by ear alone: the emulated IOP's sound RPCs are
  recorded with their IOP times and replayed into the native driver; SPU2 writes and audio are compared.
- With streaming and sound native the emulated IOP is off by default (no modules, no IOP time); `PS2X_IOP=1`,
  `VCS_NATIVE_SOUND=0` or `VCS_NATIVE_STREAM=0` bring it back for checking. The emulated IOP's code stays.

## 2026-10-24 - RSL world draws: record and replay at the VIF level, on by default

Phase 2 of RSL_SUBMISSION.md for world sectors. The world draw's geometry is drawn at a MARK put in place of its REF,
from a recording of what the VIF did with the block the first time (UNPACK results, MSCALs, meshes) rather than from a
decoder of our own:
- **Exact by construction**: the recorded VU qwords are the interpreter's own UNPACK results, the MSCALs run through
  the interpreter's own MSCAL routine (`PS2Memory::vu1Mscal`) and the native world handler; a recording is refused when
  its result could depend on state from before the block. A decoder of the UNPACK formats, masks and modes would have
  been a second implementation to keep in step.
- **The MARK keeps the order**: the draw happens where its REF was, after its state flush and GS register writes, on
  whichever thread processes the chain (render worker or not).
- **Invalidation by content**: a block is hashed each time it is drawn (~5% of the chain's bytes; cheaper than copying
  it) - streaming replaces cells without telling us, and the hash notices.
- **On by default** after the verify run (0 of 174,301 replays differ) and the picture / tour checks; `VCS_RSL_WORLD=0`
  keeps the packet path. The timing gain is within noise (world geometry is small); the path is the point - model
  instances come next on the same mechanism.

## 2026-10-24 - RSL model draws: the same record / replay; marked bytes stay in the chain's copy

Model instances (`FUN_002F7D58` / `FUN_002F80A0`) use the world's mechanism, with two changes that apply to both:
- **The marked bytes are copied and skipped, not replaced** (revises "cheaper than copying it" above): the render
  worker processes a chain while the game builds the next frame, so reading a block from guest memory at the MARK
  could see a cell or model streamed out after the kick. The copy is what the guest held at the kick; copying costs
  little next to processing. It also gives a CALLed packet - a sub-chain of tags - as one flat stream without a second
  chain walker of our own.
- **Every CALL in the packet buffer is marked**, not only the ones the hooked draw calls wrote: a recording is the
  interpreter's own result for those bytes, so it is exact whoever built the packet; the hooks only label the
  counts. Packets rebuilt every frame drop to the packet path by themselves (bytes changed three hits running).
- On by default after the verify run (1,217,887 blocks, 0 differ), scenes and tour; worker -1.3 to -2.4 ms in the
  bench. `VCS_RSL_MODELS=0` keeps the packet path for models.

## 2026-10-24 - Game thread: the profile's targets before draw-call replay

The plan after phase 2 was to replay draws at the RSL draw call instead of building their packets. A drive profile
with call chains showed the draw calls' own writes are a small part of the game thread; a reallocating chain copy
(~10%), four VU0 programs still interpreted (~5%) and the state flush (2.3%) were larger and each could be made
native or fixed with an exact check against the original. Those went first. Native guest functions follow the rules
already used for VU0 programs: bit- / byte-exact against the original in a verify mode, a switch back, and the
original runs whenever the native version would have to call other guest code (the flush's plug-in callbacks).
Draw-call replay stays the way to remove the remaining copy and flush together.

## 2026-10-24 - Block validity by write watch, not by reading the bytes

Step 4 (FULLY_NATIVE.md) needs to know at the frame kick whether a world / model block still holds the bytes its
recording was made from, without copying or hashing it (the distinct blocks are ~85% of the marked bytes: a per-frame
check by content would cost as much as the copy). Chosen: the OS's write watch on guest RAM, folded into per-page
generations once per kick. Not page protection: the stream thread reads the disc straight into guest RAM, and a kernel
write into a protected page fails instead of faulting. Not hooks on the game's streaming / instancing functions: every
path that writes packet memory would have to be found and kept up. Exact by construction (any write to any of a
block's pages makes it dirty), checked with a decide-copy-compare mode (2.4M decisions, 0 wrong).

## 2026-10-25 - Draw items: one tag in the chain per draw, the block decided at the kick

FULLY_NATIVE.md step 4 asked for world / model draws that make no packets. Chosen: the draw call becomes an item (its
flush when all CNTs, its GS registers, its block), and the packet buffer keeps one 16-byte CNT per draw whose MARK /
NOP name the item and the frame. Not "no tag at all": 2D, effects, texture uploads and the setters' own uploads still
go through the chain between the draws (step 5), and the tag is what keeps each draw in its place among them. The
block's validity is decided at the kick, after the write watch's fold, exactly as for a marked block - not at the draw
call: generations there only cover writes up to the last kick, and a cell streamed in this frame and drawn this frame
would be replayed from an old recording. A block that changed is copied from guest RAM at the kick (`flattenBlock`,
the walker's rules; checked against the walker's copy), so nothing reads guest RAM later on the worker.
Draws stay packets where an item would not stand for the same bytes later or cannot be made exactly: while
`FUN_00240ED8` caches a sector (the copy is sent again in later frames: game-internals.md 6i), when the flush calls
plug-in callbacks (guest code: ~15% of draws while driving), for a model block not yet seen copied by the walker.
Speed was not the reason (measured even: the walker's share falls by about what the items cost); the reason is the
path - the draws no longer depend on the chain's bytes, which step 5 needs.

## 2026-10-25 - Draws whose flush calls guest code: captured, not ported

Step 5f could have ported everything a state flush may call (FUN_001CFAE0's float maths, the two immediate-geometry
callbacks) so that every draw is built natively. Chosen instead: those draws run the original and the region they
wrote is captured into items (the 5d mechanism, with CALLs taken as blocks). The chain loses their packets all the
same, nothing new has to be checked bit for bit, and the guest code keeps doing what it does. A port stays possible
later if their CPU time ever matters (small: ~2% of flushes).

## 2026-10-26 - Gaps: take what is between hook points, in place

Step 5h could have gone on adding writers to the captured list one by one (the census by origin named them). Chosen:
every hook point takes what was written since the last one, whoever wrote it, so the list never has to be complete.
Taken in place (item tag + NEXT over the rest; the cursor never moves back), because a gap is settled at arbitrary
hook points where a caller may hold the cursor - one such caller (`FUN_00240ED8`) was found the hard way, and verify
mode cannot see this class (it keeps the packets). The price is a NEXT per run (~32 a frame) and dead bytes in the
buffer; the chain's walk in 5g skips both.

## 2026-10-26 - No second way to start a frame (FULLY_NATIVE.md 5g dropped)

5g would have handed the item list to the worker without walking the chain. Measured after 5h: the kick takes 0.23 ms
a chain, 0.19 ms of it the items' resolve, which stays either way; the walk and copy of the ~300 remaining tags (the
root lists, the item tags, the runs' NEXTs) cost ~0.04 ms. The chain is kept as what orders the frame: one path, the
walker's checks (verify modes, the census) keep working, and nothing is drawn from the chain's own bytes any more.

## 2026-10-26 - zlib natively as an exact port, not a faster inflater

`inflate_fast` could be replaced by a faster decoder (wide bit-buffer refills, multi-symbol tables) with the same output.
Chosen: zlib 1.2.2's own loop, so every call leaves `next_in` / `avail_in` / `hold` / `bits` exactly as the guest's -
checkable call for call against the original (`VCS_NATIVE_INFLATE_VERIFY`) - with only the copies that cannot overlap
done as `memcpy`. A faster decoder would stop at other points and leave zlib's state different between calls (the
final output the same); the ~20 ms an area load costs now did not justify losing the per-call check.

## 2026-10-27 - Ultrawide: fixed ratios plus Match window, Hor+ between 4:3 and 32:9

The user asked for 21:9 and 32:9. "21:9" monitors are 2.37 (2560x1080) or 2.39 (3440x1440), not 21 / 9 = 2.33. Chosen:
- The menu's 21:9 is exactly 21 / 9 (the label says what it is), and a Match window choice takes the window's own
  shape, so every monitor gets a picture without bars; it follows the window each host frame (resizing, full screen).
- Match window is clamped to 4:3 .. 32:9: narrower than 4:3 would cut the original horizontal view (we only do Hor+),
  wider than 32:9 is untested (triple screens would need checking: the view passes 140 degrees).
- No separate FOV setting: the game's horizontal FOV is kept for the middle 4:3 and the sides are added.

## 2026-10-26 - The trainer: the game's script commands, nothing saved, the game's input taken while open

The user asked for a trainer like RDR2's (docs/TRAINER.md). Chosen:
- Everything the game has a script command for goes through it (run on the game's own script VM), rather than
  writing the game's structures: the commands keep the game's own bookkeeping (wanted level and police, streaming,
  vehicle creation and ownership). Only values that drift every frame (health, ammunition, a clip, a vehicle's
  health) are written directly while their switch is on - a command per frame would cost a script each time.
- Switches are not saved in vcs.ini: the game starts authentic every time; the trainer is used, not configured.
- The menu takes the game's input while open (as the settings menu does): its keys are the game's D-pad, Start and
  Select. RDR2's trainer lets the game keep the keys it does not use; here every menu key is a game button.
- Trainer scripts must never be in a save: started only while the game runs, taken back out when it pauses.
- Models are streamed asynchronously with a time-out rather than `LOAD_ALL_MODELS_NOW`, which froze the game when the
  streaming memory was full.
