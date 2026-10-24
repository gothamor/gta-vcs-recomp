# Performance: well past 100 fps everywhere

The goal (the user, 2026-10-25): at the PS2's resolution the port runs well past 100 fps everywhere, without changing
what the game does. This is the working list: what was measured, what was done (with numbers), what is next.

## How it is measured

- `python tools/bench.py OUT --fps 0 VCS_TIMING=1 VCS_MIN_STEP=0.05`: uncapped, frame-rate independent timing with the
  step floor lowered (so the 100 fps cap of independent timing does not apply - see "The 100 fps cap" below). Five
  standing spots (Fort Baxter, four city spots from the world tour) and the AI drive from the first city spot.
  Columns: fps, game-thread ms, render-worker ms, frame ms. The build is `--opt none` unless noted (PGO is ~20-30%
  faster on the game thread).
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

## Next

- **Render worker** (7-8 ms at city 1325,-325 and driving): profile, then the biggest items.
- **The 100 fps cap** of frame-rate independent timing: CTimer's 0.5 minimum step (1/100 s) - below it ped movement
  breaks (game-internals.md 4, DECISIONS.md 2026-10-24). Running past 100 fps needs what breaks below the floor fixed.
- **Game thread**: the per-call cost of guest functions (Win64 prologues save 8 GPRs + 6 XMM registers per call);
  `inflate_fast` natively (zlib 1.2.2, game-internals.md 8) for streaming; the VIF1 chain copy; the IOP / SPU2 mixing.
- PGO retrain after the codegen changes (`--opt pgo --train`) for release numbers.
