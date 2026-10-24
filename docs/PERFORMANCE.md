# Performance: well past 100 fps everywhere

The goal (the user, 2026-10-25): at the PS2's resolution the port runs well past 100 fps everywhere, without changing
what the game does. This is the working list: what was measured, what was done (with numbers), what is next.

## Where it stands (2026-10-25)

Display > Game timing "Frame-rate independent" with the frame rate cap at 240 or Unlimited (= up to 240; the game's
time step stops getting shorter there). The PGO + ThinLTO build (`tools/build.py --opt pgo`), i7-7700K + GTX 1080 Ti,
640x448 internal resolution:

| | before (uncapped, floor lowered) | now |
|---|---|---|
| Fort Baxter | 111 fps | 223 |
| city 575,175 / -675,1425 / 1325,-325 / -1425,-575 | 166 / 118 / 130 / 125 | 240 (ceiling) / 201 / 226 / 220 |
| AI drive | 95 | 150 |
| world tour, 40 points (median / slowest 10% / slowest) | - (200 / 140 / 110 after step 1) | 208 / 165 / 120 |
| 30-minute uncapped autodrive soak | - | 60/60 legs ok; frame time median 6.5 ms, worst 30 s average 9.0 ms |

Before this work the game could not run above 100 fps in any setting (independent timing's ceiling) and the AI drive
did not hold 60 in original timing. Original timing (30 / 60) is unchanged: reference scenes identical throughout.
Where the slowest frames are now: heavy views (~1,950 draws: a rooftop facing the city) at ~120 fps, the game thread
(6.2 ms + 1.6 ms submitting the chain) and the render worker (6.2 ms) both near the limit. Memory: the RSL replay's
recordings grow with the blocks seen (~1.3 GB private after 15 minutes of driving, then flat - keyed by address, so
bounded by the world; predates this work).

## How it is measured

- `python tools/bench.py OUT --fps 0 VCS_TIMING=1 VCS_MIN_STEP=0.05`: uncapped, frame-rate independent timing with the
  step floor lowered (so the 100 fps cap of independent timing does not apply - see "The 100 fps cap" below). Five
  standing spots (Fort Baxter, four city spots from the world tour) and the AI drive from the first city spot.
  Columns: fps, game-thread ms, render-worker ms, GS-stage ms, frame ms. The build is `--opt none` unless noted (PGO
  was ~20-30% faster on the game thread before this work, ~5-10% after it). Since section 5 the floor is 50/240 by
  default: `VCS_TIMING=1 --fps 0` alone is the user-facing "Unlimited (up to 240)".
- `VCS_TIMING=1 VCS_FPS=0 tools/tour.py OUT --every 3 --limit 40`: the world tour uncapped (perf per point).
- At the default cap (`tools/bench.py` with no options: 60 fps, original timing) `game` is the game thread's work
  per frame, the rest of the frame is waiting.
- Game-thread / worker profiles: `VCS_PROFILE=5` (+ `VCS_PROFILE_THREAD=RenderWorker`, `VCS_PROFILE_CALLERS=name,...`
  for call stacks).
- Behaviour: `tools/scenes.py capture` + `compare` against `build/scenes/reference`, `tools/tour.py OUT --every 9
  --limit 18` (15 ok, 2 relocated, 1 falling at (-925, 1175) - unchanged since before this work).

## Where it started (2026-10-25, `--opt none`)

At 60 fps (original timing): game thread Fort Baxter 10.9 ms, city 6.7 / 11.2 / 7.0 / 10.2, AI drive 17.1 ms (52 fps:
the drive did not hold 60). Uncapped the game was held at ~100 fps by the EE clock (below).

## Done

### 1. Guest memory accesses: one compare (2026-10-25)

Every guest load / store ran ~25 instructions inline: a vector compare against five special ranges, the debug
watchpoint test (two global loads), a wrap-around check, then the access - and jumps to and from an out-of-line
block. Now (`ps2_runtime_macros.h`, `ps2Read` / `ps2Write`): `addr < g_ps2FastReadLimit` (main RAM, 0x01FFFFF0) ->
the access in place; everything else (scratchpad, I/O, the last 16 bytes of RAM, mirrors, and every access while a
`VCS_WATCH` / `VCS_RWATCH` is armed - the limit drops to 0) goes through the out-of-line checked path, which does what
every access did before. Also:
- the scratchpad: the recompiler resolves VCS's scratchpad addresses (render state at 0x70002020+) as constants and
  calls `runtime->Load32 / Store32` for them - an out-of-line call, a `try`, PS2Memory's range decoding and, for
  32-bit stores, a mutex (`drainCompletedDmacHandlers`). `PS2Runtime::LoadN / StoreN` are inline now with the
  scratchpad in place (an aligned access: one subtract, one test); DMAC completions are flagged by an atomic, so the
  32-bit store checks one flag instead of taking the mutex;
- guest register writes are a 64-bit store of the low half instead of a 128-bit read-blend-write (no dependency on the
  register's previous value).

The executable went from 154 MB to 73 MB. Game thread at 60 fps: Fort Baxter 10.9 -> 4.8 ms, city 6.7 -> 3.3,
11.2 -> 5.3, 7.0 -> 3.7, 10.2 -> 4.7, AI drive 17.1 -> 8.7 ms (now at the 60 cap). Scenes and tour unchanged.

### 2. Recompiler: pc stores only where something reads them (lean pc)

The generated code stored `ctx->pc` before every load / store and around every branch (the branch's pc, the delay
slot's pc, `in_delay_slot`, `branch_pc`, the target, the fallthrough) - four to six stores per branch. Nothing reads
them while straight-line code runs except the runtime calls it makes. Now (ps2xRecomp `pc_observation.h`):
- RAM / scratchpad accesses take their pc as a constant `ps2AccessPc` next to the access, stored only by the checked
  path (I/O, watches, exceptions);
- branches whose delay slot reads no pc: no pc / delay-slot stores; forward gotos and the not-taken fallthrough store
  nothing; back-edges keep theirs (a checkpoint resumes at the loop head); JAL's dispatch stores the pc itself;
- a function entered at its start (the common case) skips the resume `switch`.
What sees a stale pc now: the sampling profiler's guest pc and crash reports (the last call / loop head / I/O
access). `PS2X_RECOMP_FULL_PC=1` when running the recompiler restores the old emission. Game thread at 60 fps:
city 3.1-4.8 ms (-5-10%), AI drive 8.7 -> 7.6 ms.

### 3. Recompiler: `ctx` as `__restrict`

Guest registers live in `ctx`; every guest store goes through `rdram` (a `uint8_t *`, which may alias anything), so
the compiler reloaded every register from `ctx` after each store. Generated functions now take
`R5900Context *__restrict ctx`: every runtime call that touches the context is handed `ctx` (checked: dispatch,
syscalls, breaks, traps, VU0 calls, the unlisted-resume report; `eeCheckpointDue()` only reads the scheduler's clock),
and the guest's RAM never holds the register file. The busiest function's assembly went from 72.8k to 25.7k lines.
Fort Baxter 5.3 -> 4.4 ms, city a few percent. `PS2X_RECOMP_NO_RESTRICT=1` (recompiler) = off.

### 4. Runtime overheads on the game thread

- **The EE clock held the game at ~100 fps.** Guest work is charged estimated EE cycles (8 per call, 32 per loop
  iteration); above ~100 fps the clock ran ahead of real time and the scheduler held the EE at every due event (VBlank)
  until real time caught up: `wait_ms` ~3 ms a frame. Frame-rate independent timing tops the clock up to real time at
  each frame's end anyway, so it now charges a quarter (`EeScheduler::setGuestChargeShift(2)`, `VCS_EE_CHARGE_SHIFT`);
  original timing is unchanged.
- **The flip waited for the render worker.** FUN_003706c8 reads GS CSR for the FIELD bit; a CSR read fences on the
  render worker (SIGNAL / FINISH come from GS work), so the game thread waited for the worker to drain once a frame -
  ~25% of the game thread uncapped. FIELD is set at VBlank on the game thread: that one read skips the fence
  (game-internals.md 6a; `VCS_CSR_FENCE=1` = as before).
- **Unlimited frame rate was 180 fps.** The main loop executes the frames the game queued (at most 3 ahead) and was
  paced at `max(60, cap)` - 60 for an unlimited cap: at most 180 game frames a second. Unlimited paces it at the
  platform's maximum (1000 Hz).
- VU0 microprogram calls: the native lookup (a `memcmp` of the program every call) is redone only when VU0 code
  memory changed (its write generation); the VU's rounding mode is set in MXCSR alone (`fesetround` also reprograms
  the x87 unit); the call no longer reads the clock unless `PS2X_TRACE_VU0` is set.
- The scheduler's event queue and due-deadline list are buffers kept between calls (a container was allocated and
  freed per call, ~2% of the game thread with the other allocations).

Uncapped after 1-4 (`--opt none`): Fort Baxter 209 fps (game 4.2 ms), city 331 / 202 / 135 / 232, AI drive 126 fps
(game 5.6 ms, worker 8.0 ms). The render worker is now the limit at the busy city spot (7.4 ms) and on the drive.

### 5. Past 100 fps: the ped-movement step bug (2026-10-25)

Frame-rate independent timing was capped at 100 fps because below CTimer's 0.5 step (1/100 s) peds ran at a quarter
speed. Write watches on the player's move speed at 1/100 vs 1/110 s steps traced it to one branch in FUN_001f1d38
that the PS2 never takes (game-internals.md 4); one instruction patch fixes it without changing steps >= 0.5. The audit
matches 1/30 - 1/100 up to 1/240 s, so the ceiling is now 240 fps: caps 120 / 144 / 165 / 240 added, display refresh
and unlimited go up to 240 (DECISIONS.md 2026-10-25). Also fixed on the way: `tools/gamectl.py` closes its sockets
without TIME_WAIT (the audit at 200+ fps ran out of ports) and the rig kills the game with its launcher when it does
not quit.

### 6. Render worker, first pass (2026-10-25)

Now the limit at busy spots (the game thread waits on it ~400 ms a second at city 1325,-325). Done: ped skinning in SSE
lanes (same operation order, bit-identical - `VCS_SKIN_VERIFY=1` checked 850k packets), recorded unpacks copied in one
`memcpy` / one blend per qword, the CLUT hash in four lanes (VCS loads a CLUT before nearly every draw). City 1325,-325
worker 7.4 -> 6.7 ms, drive 8.0 -> 7.1 ms. What the worker spends its time on there (inclusive profile): native draws
from VU1 programs ~55% (of which the packed draws' recording ~25%), GIF data (texture / CLUT uploads: ~1,070 uploads
and ~930 CLUT loads a frame, nearly all overwritten before use) ~30%, the chain / replay ~15%.

Uncapped at the user-facing settings (independent timing, unlimited = up to 240, `--opt none`): Fort Baxter 170 fps,
city 149 / 192 / 205 / 239 (at the ceiling), AI drive 136 fps.

Then: packed 3D draws recorded per packet (one state check, triangles appended at once - the same draws), and a memo of
texture binds (by their registers, valid while the texture's pages / CLUT / render targets / evictions are unchanged).
Small gains: at the busy spot ~2,700 binds a frame, 69% the same as the last, ~840 through the full lookup (which finds
the texture cached 99.9% of the time; ~1 texture a frame is decoded); the memo catches ~20% of those - VCS uploads each
texture into shared VRAM before its draw, so the pages' generations change constantly. Content-derived page generations
would let more of them hit (not done).

### 7. The GS stage: the render worker on two threads (2026-10-25)

The world tour uncapped (40 points, `VCS_TIMING=1 VCS_FPS=0 tools/tour.py`) had a median of 200 fps and its slowest
points at ~110 fps, all limited by the render worker (8.5-8.8 ms, ~1,900 draws). Its work splits cleanly: the VIF1 /
VU1 / native-draw side ~45%, the GS front end and frame building ~55%. The GS side now runs on its own thread behind
the worker (NATIVE_RENDERER.md "The GS stage", `PS2X_GS_THREAD=0` = off). Uncapped (`--opt none`, independent timing):

| spot | before | after (game / worker / GS stage ms) |
|---|---|---|
| Fort Baxter | 211 | 194 (4.6 / 2.1 / 1.7) |
| city 575,175 | 235 | 239 - the ceiling |
| city -675,1425 | 204 | 213 (4.2 / 0.8 / 0.7) |
| city 1325,-325 | 151 | 215 (3.6 / 3.5 / 4.5) |
| city -1425,-575 | 220 | 203 (4.5 / 0.9 / 0.7) |
| AI drive | 129-136 | 133 (6.3 / 4.4 / 4.3) |

(standing spots vary +-10% between runs with the scene). Now the game thread is the limit nearly everywhere.

### 8. Guest functions without register saves (2026-10-25)

Every generated function used the Win64 convention: its prologue saved 8 general and 6 vector registers and its
epilogue restored them, on every guest call - though generated code keeps nothing in registers across a guest call (the
guest's state lives in R5900Context). Guest functions now use clang's `preserve_none` (`ps2_guest_cc.h`,
`PS2_GUEST_CC`): the recompiler emits it on every generated function and stub wrapper, `RecompiledFunction` carries it,
and the host functions that replace guest ones (src/game, src/debug) are marked; named stub / syscall handlers keep the
platform convention behind a thunk. Host code calling into guest code pays for the saves at that boundary (the
scheduler, an override calling its original). The scheduler's `EeDispatcherTransfer` exceptions unwind through the
guest frames as before. A/B against the build before (same route, two alternating rounds, 60 fps): game thread
standing 4.18 -> 3.90 ms, driving 7.18 -> 6.97 ms (~3-7%). Scenes unchanged.

### 9. Game-thread odds and ends (2026-10-25)

- The FPU multiply took its out-of-line path for every zero result (`ps2_fnormal(0)` is false) - multiplying by 0 is
  common; an exact +-0 product is the PS2's result too, so it returns inline now (`ps2_fmul_slow` was 1.3% driving).
- Chain buffers taken from the render worker's pool went back to PS2Memory's own 4-buffer pool for the transfers handled
  on the game thread; it filled and the rest were freed every frame (VirtualFree, ~1.6% driving) while the worker's
  pool allocated new ones. They go back where they came from.
- FUN_003F9E50 (identity matrix: 16 words zeroed in a loop, a checkpoint per iteration) runs natively
  (`VCS_NATIVE_IDENTITY=0` = the original).
Each was ~1%: gone from the profile (guest share 65.7 -> 67.8% of the busy game thread), below what the A/B resolves.
Scenes unchanged.

### 10. PGO retrained; what did not help (2026-10-25)

- PGO + ThinLTO retrained on the new code (`--opt pgo --train`): smaller gain than before (the codegen work removed much
  of what it used to optimise). Uncapped, user-facing settings: Fort Baxter 213 fps, city 212 / 239 / 221 / 223, busy
  city 1325,-325 223, AI drive 144 (game thread 6.0 ms + 0.8 ms submission; worker 4.2, GS stage 3.8).
- The write-watch fold (~0.155 ms per frame, ~430 pages written per fold) cannot be narrowed to a page range: the
  marked blocks span pages 376-8155 of 8192. Its cost follows the pages written, not the range. Not changed.
- The sampling profiler counted a host sample as guest code when it fell within 1 MB after a generated function; it now
  uses each generated function's own symbol size (ThinLTO / PGO builds interleave runtime and generated functions).

### 11. VU0 natives on the context's registers (2026-10-25)

The VU0 natives (~3,100 calls a frame driving: box clip, 3x matrix product, sphere / segment vs triangle) copied the
whole VU0 register file in and out per call; they now work on the context's VF / ACC in place (VF0 reads (0, 0, 0, 1)
during the call, the context's own VF0 put back after, as the copy left it). `PS2X_VU0_VERIFY=1`: millions of runs of
each against the interpreter, 0 differed. Their share of the game thread 6.75 -> 6.4% (smaller than the copy's size
suggested).

### 12. Render worker: rounding mode without the CRT; what is left there (2026-10-25)

The native draws switch the SSE rounding mode around every VU1 program (VU maths rounds toward zero, the caller's mode
is put back for the GIF packets): `std::fegetround` / `std::fesetround` also read and reprogram the x87 unit, unused on
x64, and were ~3% of the render worker's first stage driving. They are now `sseRounding()` / `setSseRounding()` in
`src/game/vcs_hle_world.cpp` (MXCSR bits 13-14 only). `vcsvu::vuExact`'s `std::signbit` (a CRT call, `ldsign` in the
profile, from every matrix set-up) is a bit test. `PS2X_HLE_VERIFY=1` at a city spot: re-draws 147,761 exact, C++
clipper and env pass exact; the world fast path's 16 packets "within 1" (37 vertices, ST LSBs) are the same on the build
before (14 / 34 there). Scenes: static pictures identical.

What the first stage does now (`--opt none`, uncapped drive, worker 3.8-4.4 ms a frame against the game thread's
6-8 ms): idle ~58% of the time; of the rest, about a third is copying - the RSL replay writing recorded unpacks into VU
memory, `GS::submitNative` / `submitPackedKick` putting draws on the GS stage's ring, VIF DIRECT data to the GIF. Those
copies are what step 4 of docs/FULLY_NATIVE.md removes (draws replayed without going through VU memory); smaller
trims there are worth little while the game thread is the slower of the two. The 16 KB save of VU memory before each
native re-draw (restored if it falls back) is not visible in the profile.

Looked at and left (measured on the drive, uncapped):
- **The write-watch fold** at each VIF1 kick: 0.15 ms per fold, ~400 of 8,192 pages written per frame. With the watch
  off (`PS2X_WRITE_WATCH=0`) the standing game thread is ~0.35 ms faster (the fold and the observer's page snapshots),
  but driving is slower overall: the worker loses the RSL replay's skips (worker 3.9 -> 4.3 ms, 121 -> 113 fps). The
  skip decisions are taken during the walk, so the fold cannot run beside it.
- **Copying the VIF1 chain off the game thread** (the kick's walk and copy, 1.0-1.6 ms): the game has no VIF1 DMA wait
  before rewriting what the chain reads - the flip (`FUN_003706c8`) kicks it through `FUN_002721f8` / `FUN_002723E8`
  (game-internals.md 6a); the DMA syncs (`FUN_00272480`) are only in other paths. The chain CALLs packets rebuilt every
  frame and reads RSL's matrix blocks, so a copy running beside the next frame's code would race with it.
- **Checked memory accesses** (`ps2ReadChecked` / `ps2WriteChecked`, ~0.8% of the game thread): nearly all from the
  busiest draw function `FUN_002b8ed0` through scratchpad pointers held in registers; the scratchpad path behind the
  check is already inline (section 4).

## Next

- **The heavy views (~120 fps)**: the game thread's chain submission there (1.4-1.6 ms: the VIF1 chain copy, the walker
  and the RSL observer, the write-watch fold) and the render worker's first stage (6 ms: RSL replay copying recorded
  unpacks into VU memory, the native draws' setup) - e.g. draws replayed without going through VU memory
  (docs/FULLY_NATIVE.md step 4).
- **Game thread** (guest code ~80% of it now, flat: the top function 2.3%): deeper codegen (guest registers kept in host
  registers across calls), `inflate_fast` natively for streaming bursts (zlib 1.2.2, game-internals.md 8).
- GS stage: per-upload / per-CLUT-load bookkeeping; content-derived page generations (more texture-bind memo hits).
- Playtest missions above 100 fps; sprint at 1/240 s (-11%).
- The RSL recordings' memory (an LRU bound).
