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
| 3 | Textures and render targets without GS memory | L | planned |
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
