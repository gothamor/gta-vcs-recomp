# Native port roadmap

Status: **approved** (2026-10-22): start with milestones 1-2; FFmpeg for movies; 60 fps as an option,
off by default; IOP retirement yes; Windows only - milestone 9 out of scope for now (DECISIONS.md). Follows NATIVE_RENDERER.md (stages 0-6 done) and
SETTINGS_MENU_WIDESCREEN.md (v1 done).

The goal: GTA: Vice City Stories playable from the first mission to the credits as a native PC
program - native rendering, audio, streaming, timing and movies, with the PS2 emulation layers kept
only as references and fallbacks - running at full speed on modest hardware, with authentic output by
default and the enhancements as options.

## Where we are

Updated 2026-10-26 (the table below the "When the roadmap was written" heading is the 2026-10-22 starting point):

| Layer | Now | Native? |
|---|---|---|
| Game code (EE) | Statically recompiled C++; guest registers / memory model | native code, emulated machine state |
| VU0 (collision, culling, maths) | Native C++ for the programs the game uses | yes |
| 3D: world, peds, vehicles, props | Draw items at the game's RSL draw calls -> D3D11, recordings checked bit-exact (FULLY_NATIVE.md 4-5) | yes |
| Sprites, particles, text, env-map pass, VU1 setup programs | C++ reproductions of the VU1 programs (`vcs_hle_world.cpp`), `vu1_programs` 0 in every mission | yes |
| HUD, menus, post effects, loading screens, effects | Native layers / packed draws; `gs_draws` 0 (no draw through the emulated GS front end) | yes |
| Presentation, input, window | SDL3 + RHI (D3D11) | yes |
| Streaming (CDSTREAM) | Host-thread reads from the disc image (`vcs_stream.cpp`) | yes |
| Sound driver (CDSTREAM's sound servers, LIBSD) | Ported to the host (`vcs_sound.cpp`, `src/sound/libsd.cpp`) | yes |
| Sound output / mixer | The SPU2 core (C++) behind the native libsd, mixed to SDL3 - the chosen "native mixer" (DECISIONS.md 2026-10-24) | yes |
| IOP | Off: no IRX module loaded or run (`PS2X_IOP=1` brings it back) | retired |
| Memory card, pad | EE-side HLE (`sceMc*`, pad stubs) | yes |
| Movies (`INTRO.PSS`, `STINGS.PSS`) | FFmpeg, sound into the same audio output (milestone 5) | yes |
| Frame rate | Independent timing, up to 240 fps as an option; authentic 30 fps by default | yes |

### When the roadmap was written (2026-10-22)

| Layer | Then | Native? |
|---|---|---|
| Game code (EE) | Statically recompiled C++ (920k instructions); guest registers / memory model | native code, emulated machine state |
| VU0 (collision, culling, maths) | Native C++ for the programs the game uses | yes |
| 3D: world, peds, vehicles, props | Packet-level HLE -> D3D11 (`vcs_hle_world.cpp`, `vs_world` / `vs_lit`), bit-exact checked | mostly: still walks the game's DMA packets |
| Sprites, particles, text, env-map pass | VU1 microcode (compiled) -> emulated GS | no |
| HUD, menus, post effects, loading screens | Emulated GS (HW backend on D3D11) | no |
| Presentation, input, window | SDL3 + RHI (D3D11) | yes |
| Streaming (CDSTREAM), sound driver (SDRDRV / LIBSD) | Real IRX modules on the emulated IOP | no |
| Sound output | SPU2 emulation -> SDL3 | emulated chip |
| Memory card, pad | EE-side HLE (`sceMc*`, pad stubs) | yes |
| Movies (`INTRO.PSS`, `STINGS.PSS`) | Skipped | missing |
| Frame rate | Game's own 30 fps cap on a 60 Hz VBlank | authentic only |

Cost per frame then (i7-7700K / GTX 1080 Ti, 33.3 ms budget at 30 fps):

| Scene | game code | render submit | IOP |
|---|---|---|---|
| Gameplay (Fort Baxter) | 14.2 ms | 6.8 ms | 0.6 ms |
| Flyover | 12.4 ms | 16.9 ms | 0.6 ms |
| Cutscene | 12.0 ms | 21.6 ms | 0.6 ms |

Known open bugs (PROGRESS.md): objects / the world turning black for a few frames, Vic falling
through the world in places, water missing, movies, the Load Game menu and saving over a slot not yet
checked by hand.

## What "done" means

1. **Complete**: every story mission, side activity and area plays from a new game to the credits,
   saves and loads, without crashes or hangs over multi-hour sessions.
2. **Native**: in a normal frame, nothing draws through the emulated GS or VU1, no IOP code is
   interpreted, and audio comes from a native mixer. The emulated paths stay in the source as
   switchable references (`PS2X_HLE=0`-style) for checking, not on the hot path.
3. **Fast**: a locked 30 fps (authentic) and 60 fps (option) on a 4-core CPU from ~2015 with integrated
   graphics at 1x, and on mid-range GPUs at 4K.
4. **Authentic by default**: the "Authentic" preset gives the PS2 picture, timing and sound;
   widescreen, resolution, 60 fps, MSAA and the like are opt-in settings.
5. **Portable**: Windows first; Linux and macOS builds from the same source (SDL3 + RHI backends).

## How I work autonomously (applies to every milestone)

- **Reference first**: every native replacement is checked against the emulated path (bit-exact where
  possible, picture / audio diff otherwise) before it becomes the default.
- **Machine-checked progress**: each milestone ends with the regression rig (milestone 1) green and
  its numbers recorded in PROGRESS.md; perf before / after for anything on the hot path.
- **Commit per step**, oddities written up in docs/, test runs visible (memory: window visibility).
- **What needs you**: feel and taste (handling at 60 fps, audio mix, picture comparisons with real
  hardware), the decisions listed at the end, and play-testing missions that the rig cannot reach.

---

## Part 1 - the next big chunk: a rig that plays the game, then a game that plays right

These two milestones are what lets me cover a large share of the game without you at the keyboard.
Today I can only see what the scripted boot path (title, intro, first walk at Fort Baxter) shows.

### Milestone 1 - Automation and coverage rig (L)

Debug hooks in the game (`src/debug/`, through the control channel and an env-var script runner),
all built on the game's own functions found in the ELF, so they behave as the game would:

- **Teleport / camera**: set the player's (or vehicle's) position and heading; free camera for
  looking around without moving the player. Needs: the player ped pointer, CPed / CVehicle position
  fields, the streaming request for a position (so the area loads before we arrive).
- **World state**: time of day, weather, wanted level, health / invulnerability, give weapons, spawn
  a vehicle - the PS2 cheat code handlers are in the game and can be called directly.
- **Mission launcher**: start any mission from `MAIN.SCM` and mark missions passed, so later story
  states (islands unlocked, empires, late missions) are reachable without playing up to them. Needs:
  the SCM VM (CRunningScript list, the "start mission" path, the mission-passed flags / globals).
- **Save-state substitute**: the game's own save / load, scripted (memory card files are the save
  states; a library of them grows as the rig and you reach new points).
- **Detectors**, run every frame, reported as events on the control channel and in a run log:
  - crash / hang (watchdog exists), EE exceptions, runtime warnings;
  - black or near-black frames, frames where the world is missing (sky-only ratio), picture freezes;
  - player falling (Z drops below the collision ground / below sea level outside water);
  - frame-time spikes, audio underruns, streaming stalls (CDSTREAM queue depth);
  - "unconverted" draws: VU1 programs and GS-path draws in a frame (coverage of the native renderer).
- **Runs built on top** (`tools/`):
  - **World tour**: a grid over the whole map (land and sea, at ground and driving heights, several
    times of day), at each point: settle, screenshot, perf line, detector events. Produces a map
    image with problem spots marked - the to-do list for milestone 2.
  - **Drive / fly loops**: a vehicle driven along roads by an autopilot (the game's own AI driving
    for a ped-controlled car, or a waypoint follower over our pad injection), to stress streaming and
    collision at speed.
  - **Mission smoke test**: launch each mission, run 60 s with the player idle / scripted inputs,
    check for crashes, black frames and fallbacks.
  - **Soak**: hours of tour + drive with memory and handle counts tracked.
  - **Regression suite**: the reference scenes + a fixed subset of the tour + mission smoke tests,
    compared to stored baselines (picture diffs, perf, event counts); one command, a report page.

Done when: the world tour covers the whole map unattended, the mission launcher starts every
mission, and the regression suite runs in one command and reports differences against a baseline.

### Milestone 2 - Correctness sweep driven by the rig (L, open-ended)

**Done 2026-10-23** (PROGRESS.md): tour 200/240 ok with no falling, 95/95 missions, saves through the menus, SPU2
complete, a 30-minute soak clean. Carried to milestone 3: the VU1 clipper natively, lazy render-target patching.

Work through what milestone 1 finds, worst first. Known items to start with:

- **Black frames** (objects / world black for a few frames): bisect with `PS2X_HLE=0`, prelit tint
  and lighting constants, render-target drops, texture cache misses (PROGRESS.md notes).
- **Falling through the world**: collision streaming at those spots, VU0 collision programs, FP
  semantics (round-to-zero, denormals, no contraction) - compare against the emulated reference.
- **Water** (dark grey sea), anything the tour marks on the map, every mission that fails its smoke
  test.
- **Saves**: Load Game menu, saving over a slot, several slots, a full story save carried across
  builds.
- **Audio correctness** while we are there: reverb, noise, pitch modulation, interpolation (the
  missing SPU2 features) - only the parts the game uses, measured with the rig's audio capture.

Done when: the world tour and every mission smoke test come back clean, and a soak run of several
hours has no crash, hang, leak or falling-through.

---

## Part 2 - becoming native, layer by layer

Ordered by impact; each milestone is independent enough to be done and shipped on its own. The rig
from part 1 is the safety net for all of them.

### Milestone 3 - Native rendering, the rest of the frame (L)

1. **Remaining VU1 programs**: sprites / particles / text (0x02c0) and the second lit pass (env maps,
   stage table 0x13) - same method as stages 5-6 (survey -> C++ reference checked bit-exact ->
   shader).
2. **2D at the RenderWare level**: the HUD, text, radar, menus and projected sprites (RwIm2D /
   CSprite2d / CFont / CSprite) drawn from their call sites instead of from GS packets. This replaces
   the widescreen HUD heuristic with real per-element placement (anchoring to screen edges, 21:9,
   "match window") and makes text crisp at any resolution.
3. **Post effects as shaders**: the colour filter, trails / motion blur, the frame-buffer copies
   (identified in the widescreen work: full-screen strips sampling the frame) re-implemented in HLSL.
4. **A native frame**: render targets at the window's resolution (not a multiple of 640x448),
   MSAA, anisotropic filtering, higher-precision depth - all possible once nothing reads the PS2
   framebuffer layout. The GS emulation becomes the fallback for anything left unconverted
   (the rig's "unconverted draws" counter must read 0 in normal play).

Done when: normal frames have no emulated GS draws; captures match the emulated reference at 1x;
widescreen HUD anchored properly; MSAA / AF / resolution settings in the menu.

### Milestone 4 - Frame rate (M-L)

- Map the main loop, frame limiter and CTimer time step (game-internals.md "sections still to fill").
- **60 fps option**: run the game's update at 60 by changing the limiter, then fix what breaks at
  the shorter step (known GTA trouble spots: physics at high fps, animations, script waits, particle
  rates, the camera). Each fix gated behind the option, so 30 fps stays authentic.
- Frame pacing: present on a steady cadence (VRR-friendly), no doubled frames.
- Later, optional: higher refresh rates through render interpolation between 60 Hz updates.

Done when: 60 fps plays a set of missions (driving, shooting, flying, cutscenes) without behaviour
differences you can notice, and the regression suite passes at both rates.

### Milestone 5 - Movies (S-M)

- Demux `INTRO.PSS` / `STINGS.PSS` (MPEG-2 video + ADPCM audio) from the disc and play them with a
  native decoder, drawn by the presenter (scaled cleanly, letter / pillarboxed by the display aspect).
- Replaces the skip in `vcs_overrides.cpp`; the game's movie player is bypassed at its entry point.

Done when: the intro and stings play in sync with sound, skippable as on the PS2.

### Milestone 6 - Retire the IOP (L-XL)

The IOP is cheap (0.6 ms) but it is the last emulated processor, it carries real Sony / Rockstar IRX
code, and it ties audio and streaming to emulation timing.

1. **Streaming**: HLE the CDSTREAM RPC protocol - reads go straight from the ISO on a host thread
   (async, prioritised), with the same completion semantics the game expects. Also faster loads.
2. **Sound driver**: HLE the SDRDRV / LIBSD command protocol on the EE side and feed a native mixer
   (ADPCM decode of the `.VB` banks and streams, voices, envelopes, reverb). The SPU2 emulation stays
   as the reference: same command stream, compare the output.
3. **Remove the IOP from the normal run**: no IRX modules loaded, no IOP interpreter thread; the
   emulated IOP remains switchable for checking.

Done when: a full regression run sounds and plays the same with the IOP off, and loading is at
least as fast.

### Milestone 7 - CPU side: the game's own code (M-L, ongoing)

Game code is 12-14 ms of the 33 ms frame - the main budget left for 60 fps on modest CPUs.

- **Recompiler output**: skip per-instruction `ctx->pc` stores for pure ALU ops (emitter change
  pending a regenerate), `__restrict`, keep guest registers in locals across basic blocks, cheaper
  memory access for known-RAM pointers, ThinLTO / PGO for release builds.
- **Render submission at the RW level** (RSL in VCS; plan in RSL_SUBMISSION.md, 2026-10-24): the game still builds DMA packets on the EE for every model
  (RpAtomic instancing, matrix uploads) that the native renderer then parses. Hooking the PS2 render
  pipeline callbacks instead removes both the packet building and the parsing.
- **Hot functions by profile**: the sampling profiler (`src/debug/profiler.cpp`) ranks them; the
  top ones (matrix / vector maths, collision, streaming bookkeeping, animation) get hand-written
  native equivalents behind the rule in DECISIONS.md "Rule for hooks on recompiled functions",
  each checked against the recompiled original.

Done when: game code under ~6 ms in gameplay on the reference machine, 60 fps with headroom on the
modest-hardware target.

### Milestone 8 - Settings, controls and polish (M)

- Settings menu growth: window mode / fullscreen / vsync / fps cap, MSAA, AF, resolution, 60 fps,
  audio volumes, the "Authentic" preset, reset to defaults.
- Controls: rebinding for keyboard / mouse / gamepad, mouse aiming that feels native (the game's
  free-aim inputs), controller glyphs.
- First-run flow for players: pick the ISO, check it, build the cache.

### Milestone 9 - Portability and distribution (L)

- RHI backends: Vulkan (Linux, also Windows), Metal (macOS); shaders cross-compiled from the HLSL
  (Slang or DXC + SPIRV-Cross).
- Toolchain: the recompile + build pipeline on Linux / macOS (clang, CMake), CI building all three.
- Distribution stays source-only with bring-your-own-disc (DECISIONS.md): a one-command build for
  players, release notes, crash reports that a player can attach.

---

## Order and sizes

```
M1 rig ──► M2 correctness ──► M3 native rendering ──► M7 CPU side ──► M9 portability
              │                     │
              │                     └─► M4 60 fps (needs M3.2 for HUD timing, M7 for headroom)
              ├─► M5 movies (independent, small)
              └─► M6 IOP retirement (independent; after M2 so audio bugs are known)
M8 settings / controls: alongside, as each feature lands
```

| # | Milestone | Size | Unattended? |
|---|---|---|---|
| 1 | Automation and coverage rig | L | yes |
| 2 | Correctness sweep | L, open-ended | mostly; mission play-testing needs you |
| 3 | Native rendering, rest of the frame | L | yes, picture sign-off by you |
| 4 | 60 fps | M-L | partly; handling and feel need you |
| 5 | Movies | S-M | yes |
| 6 | Retire the IOP | L-XL | yes, audio sign-off by you |
| 7 | CPU side | M-L | yes |
| 8 | Settings, controls, polish | M | yes, feel by you |
| 9 | Portability, distribution | L | yes |

**Proposed next step: milestones 1 and 2 as one autonomous run**, committed per step, with a short
report after the rig's first full world tour (the map of problems) and after each fixed class of bug.

## Decisions for you

1. Start with the rig + correctness sweep (recommended), or go straight for the remaining native
   rendering (milestone 3)?
2. Movie decoder: FFmpeg (LGPL build, large but proven) or a small MPEG-2-only decoder (e.g.
   libmpeg2, GPL)? Both fit the GPL-3.0 licence.
3. 60 fps: a menu option that stays off by default (authentic), as the goals say - confirm.
4. IOP retirement: worth the effort for "fully native" even though it costs ~0.6 ms today, or keep the
   emulated IOP as a permanent, isolated audio / streaming device?
5. Platforms after Windows: Linux first (Vulkan, Steam Deck) or macOS first?

## Risks

- **The mission launcher** depends on understanding the SCM VM well enough to set up mission states;
  if marking missions passed is unreliable, coverage falls back to your saves at key points.
- **60 fps** in GTA engines touches physics and scripts in many small places; some issues will only
  show in play, not in the rig.
- **RW-level hooks** (2D, render submission) leave the safe packet-level boundary (DECISIONS.md rule
  for hooks); each needs a reference comparison and a switch back.
- **Sound HLE** must reproduce the driver's timing well enough for music / dialogue sync in
  cutscenes; the SPU2 path stays as the fallback until it does.
- **Upstream divergence** from PS2Recomp keeps growing (PS2RECOMP_PATCHES.md); acceptable, the
  fork is the project now.
