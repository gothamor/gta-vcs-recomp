# Native renderer plan (D3D11)

Status: **approved** (2026-10-22). Stages 0-6 done (world, skinned and rigid lit models native); stage 7 left
deliberately on the VU1 (small: see there); stage 8 partly (internal resolution works with native draws and can be
set in vcs.ini; widescreen, MSAA and anisotropic filtering are not done - reasons in stage 8).

Overall, VU path (stage 3) -> native (stage 6), reference script, i7-7700K / GTX 1080 Ti:

| phase | game fps | render submit ms/frame |
|---|---|---|
| flyover 10-44 s | 18.2 -> 28.3 | 36.7 -> 12.1 |
| cutscene 55-86 s | 18.8 -> 30.0 (cap) | 38.1 -> 11.4 |
| gameplay 100-125 s | 29.9 -> 29.9 (cap) | 10.6 -> 5.3 |

Goal: get from today's emulated GS (raylib + OpenGL 3.3) to a native renderer that
draws the game's models directly, on D3D11, as fast as possible. Windows only for
now, but nothing Windows- or D3D-specific may leak outside two small directories so
macOS (Metal) and Linux (Vulkan) can be added later.

Non-goals for this plan: macOS/Linux backends, movies, a new frontend/UI.

## Why (where the frame goes today)

Title flyover, i7-7700K, `PS2X_GS=hw` (see PROGRESS.md, performance table):

| part | ms / frame |
|---|---|
| render submit: VIF1 unpack, compiled VU1, GS recording | ~34 |
| game code (EE) | ~15 |
| OpenGL execution (main thread) | ~2.5 |

- Swapping OpenGL for D3D11 alone moves the 2.5 ms, not the 34 ms. It is worth doing
  (tooling, driver stability, presentation control, a base for the native path), but
  it is not the speed-up.
- The speed-up is skipping VIF/VU1/GS for model draws: decode the game's prebuilt
  model packets once into GPU buffers and draw them natively (stages 4+).
- The emulated GS path stays forever as the fallback for anything not converted
  (HUD, post effects, loading screens) and as the reference to compare against.

## Target architecture

```
 runtime (third_party/PS2Recomp/ps2xRuntime)       game-specific (src/game)
   EE / VIF / VU1 / GS frontend  ──────────────┐     HLE packet handlers
                                               ▼            │
 src/render      GS HW backend: recorder (game thread) ◄────┘ native draw cmds
                 executor (main thread, API-agnostic, talks only to rhi::)
                                               │
 src/rhi         rhi.h (interface)  ── rhi_d3d11/ (only place with d3d11.h)
                                               │
 src/platform    platform.h (window, events, input, audio, time)
                 platform_sdl3/ (SDL3; only place with SDL / windows.h)
```

Rules:
- `windows.h`, `d3d11.h`, `dxgi*.h` only under `src/rhi/rhi_d3d11/` and
  `src/platform/`. SDL headers only under `src/platform/`. Everything else includes
  `rhi.h` / `platform.h`. (This also ends the raylib/`windows.h` name clashes.)
- Shaders are written in HLSL (SM 5.0), compiled at build time to bytecode headers
  (`fxc`/`dxc`). Later ports cross-compile the same source (Slang or
  DXC -> SPIR-V -> SPIRV-Cross for MSL); keep shaders free of D3D-only intrinsics
  where a portable form exists.
- The runtime reaches the platform and RHI through the host-backend seam it already
  has (`ps2_host_backend.h`); raylib is removed from it. Patches go in
  PS2RECOMP_PATCHES.md as usual. (Upstream's Vita build stops building; we don't
  ship it.)

### Platform layer: SDL3

- Window (resizable, fullscreen, hidden for `--hidden` runs), event pump, keyboard,
  mouse, gamepads (SDL's controller database, better than raylib's), audio stream,
  high-resolution timer.
- Dear ImGui through `imgui_impl_sdl3` + the RHI's ImGui renderer (`imgui_impl_dx11`
  on Windows). The F3 perf overlay moves from raylib `DrawText` to ImGui.
- Not SDL3's GPU API: it has no D3D11 backend.

### RHI: our own, thin

Own interface rather than bgfx / sokol_gfx / Diligent: GS emulation needs
render-target aliasing, sub-rectangle copies between targets, and texture readbacks,
which those libraries handle poorly or not at all. The interface stays small
(a few hundred lines) and shaped so it maps to Metal and Vulkan later:

- Resources: buffers (vertex, index, constant; immutable / dynamic per frame),
  2D textures (sampled, render target, depth), samplers.
- Pipelines: immutable objects combining shaders + blend + depth/stencil + raster
  state, created from a hashable key and cached (D3D11 state objects underneath).
  GS state (blend ABCD/FIX, ZTST, ATST, colour mask...) maps to a key.
- Explicit render passes: begin(colour target, depth target, load/clear ops),
  draws, end. D3D11 doesn't need them, Metal/Vulkan do; designing them in now is free.
- Copies: texture region -> texture region, buffer uploads, texture readback
  (staging, async with a fence/query, sync variant for screenshots and dumps).
- Swapchain: DXGI flip model (`FLIP_DISCARD`), vsync on/off, tearing when allowed,
  resize, exclusive-less fullscreen. Frame latency waitable object.
- Debug: D3D11 debug layer behind an env var, object names, PIX/RenderDoc markers.
- Features we rely on, checked against D3D11 FL 11_0: dual-source blending
  (the HW backend uses `SRC1_ALPHA`), 32F depth, R8G8B8A8 targets, point/line lists
  (GS points/lines are 1 px, no point size needed).

## Stages

Each stage ends with the game fully playable and a commit; nothing is a big-bang
switch. Sizes are relative (S < M < L < XL).

### Stage 0 - Baselines (S) - done 2026-10-22

- `python tools/scenes.py capture OUT` runs the game from an empty memory card
  (`VCS_DATA_DIR`) along a fixed script driven over the control channel: loading
  screen, three flyover shots, intro cutscene, gameplay with HUD after skipping the
  cutscene (Cross), the pause map, gameplay again. It saves screenshots, the perf
  overlay numbers per phase and a whole-run perf CSV (`PS2X_PERF_LOG`).
- `python tools/scenes.py compare REF NEW` reports per-scene mean difference and
  writes diff images. Static scenes (loading, pause map) must match (tolerance 2.0);
  two runs of the same build match exactly (0.00). Moving scenes drift with timing
  between runs (up to ~15 mean on the flyover road shot) and only fail when black
  or missing.
- `python tools/scenes.py explore OUT --every N --press T:KEY,...` was used to find
  the timeline (screenshots every N s plus a contact sheet).
- References live in `build/scenes/` (game imagery, never committed). Baseline
  before stage 1: `build/scenes/base_raylib_a` (raylib + GL, `PS2X_GS=hw`).
  Flyover 10-44 s: 17.9 game fps, render submit 37.5 ms, game 14.5 ms, GL execute
  3.2 ms per frame, host 59.6 fps; gameplay: ~29-30 game fps, render ~16.5 ms.
- `PS2X_HW_DUMP` render-target dumps were left out of the script: the frame number
  they key on drifts between runs; use them by hand when a scene differs.

### Stage 1 - SDL3 platform layer, raylib removed, still OpenGL (M) - done 2026-10-22

- SDL3 window with a GL 3.3 context; the HW backend's GL executor keeps working
  unchanged. The final present becomes a small GL blit (temporary, deleted in stage 3).
- Port the raylib call sites: `Kernel/Stubs/Pad.cpp` (input, ~46 calls),
  `ps2_runtime.cpp` (window, loop, present, ~20), `ps2_perf.cpp` (overlay, ~12),
  `ps2_audio_out.cpp` (audio stream), `ps2_debug_panel.cpp` (rlImGui ->
  imgui_impl_sdl3/opengl3), `ps2_control.cpp` (key names, screenshot).
- Control channel keeps working: screenshots, injected keys, `--hidden`.
- paraLLEl-GS (`PS2X_GS=gpu`): keep the readback path only; drop its Vulkan
  window present (`gs_vulkan_present`), which is built on raylib's GLFW window.
- Done when: raylib and rlImGui are gone from the build, stage 0 captures match,
  audio/input/saves behave as before.

Result:
- `src/platform/`: `platform.h` (window, events, keyboard / mouse / gamepad snapshot
  readable from any thread, audio pull callback, 60/s frame pacing, Dear ImGui frame,
  PNG writer), `platform_sdl3.cpp` (SDL 3.4.18, static, FetchContent),
  `gl_present.{h,cpp}` (the stage 1 GL stopgap: letterboxed frame quad, textures,
  off-screen capture). The runtime gets it through `PS2X_HOST_PLATFORM_TARGET`.
- Scene captures vs the raylib baseline: static scenes 0.03 (loading) and 0.17
  (pause map: the REC marker's font); moving scenes show the same pictures a second
  or two apart. Flyover 10-44 s: 17.9 game fps, render 37.4 ms, GL 3.2 ms, host
  59.9 fps - unchanged. Audio: 0-2 underruns per run, as before.
- Behaviour changes: Escape no longer closes the window (raylib's default exit key);
  L2 / R2 on a gamepad trigger at 30% travel (raylib needed ~65%); screenshots scale
  the overlay instead of laying it out again; window size still 640x448, screenshots
  at least 960x672 (bigger windows give bigger, letterboxed shots).
- The paraLLEl-GS Vulkan window present (`gs_vulkan_present.h`) was never
  implemented in this tree, so nothing had to be dropped: `PS2X_GS=gpu` reads frames
  back as before (still not to be run on the GTX 1080 Ti, TDR).

### Stage 2 - RHI + D3D11 device and presentation (M) - done 2026-10-22

- `rhi.h` + `rhi_d3d11`, swapchain and present on the SDL window (no GL context).
- The CPU renderer and paraLLEl-GS readback frames are shown by uploading into an
  RHI texture; ImGui draws through `imgui_impl_dx11`.
- `PS2X_GS=hw` (still GL) temporarily presents by reading back into the D3D11 path,
  or stays on a GL window for this stage only - whichever is less throwaway.
- Done when: CPU renderer, perf overlay, debug panel and screenshots work on D3D11.

Result:
- `src/rhi/rhi.h` (textures, buffers, samplers, immutable pipelines with blend /
  depth / dual-source factors, explicit passes, copies, readback, swapchain, the
  Dear ImGui renderer hook) and `src/rhi/d3d11/rhi_d3d11.cpp` (FL 11_0, flip-model
  `FLIP_DISCARD` swapchain, `PS2X_D3D_DEBUG=1` for the debug layer, PIX markers via
  `ID3DUserDefinedAnnotation`). Only that file includes D3D / DXGI headers.
- `src/render/presenter.{h,cpp}`: letterboxed picture + Dear ImGui into the back
  buffer, off-screen captures; `shaders/present.hlsl` compiled by fxc at build time
  (`src/render/CMakeLists.txt`, `vcs_shader()`). The platform window has no GL any
  more; ImGui uses `imgui_impl_sdl3` (input) + `imgui_impl_dx11` (through the RHI).
- Chosen stopgap for the still-OpenGL HW renderer: a hidden GL 3.3 context
  (`platform::glInit`, only with `PS2X_GS=hw`) and `src/render/gl_bridge.cpp`, which
  reads the display texture back through two pixel-pack buffers (shown one host
  frame later) and uploads it. A synchronous `glReadPixels` first cost ~4% game fps
  on the flyover; the asynchronous one is within run-to-run noise.
- Scene captures: static 0.02 / 0.02 vs the raylib baseline; flyover 17.2-18.0 game
  fps (baseline 17.9-18.2; runs vary by ~1 fps), main-thread GL time 3.7 ms (3.2
  before: the readback). Resizing, minimise / restore, hidden runs, the F3 overlay
  and `PS2X_GS=cpu` checked. `PS2X_GS=gpu` not run (TDR on the dev GPU).
- Display latency with `PS2X_GS=hw` is one host frame (16.7 ms) higher until stage 3.

### Stage 3 - GS HW backend executor on the RHI (M) - done 2026-10-22

The backend is already split: the game thread records API-neutral frame command
lists (`Frame` in `gs_hw.cpp`: draws with `HwState`, texture creates/deletes, RT
uploads/drops, clears, depth fills), and only the main-thread executor touches GL.

- Move the recorder to `src/render/` (or keep it in place, patched) unchanged; write
  an RHI executor next to the GL one and switch with an env var during the port.
- Port the GLSL shaders to HLSL (vertex: PS2 pixel coords -> clip; pixel: TFX
  modulate/decal/highlight, TCC, alpha test via `discard`, fog, colour clamp,
  `fbmsk`; blit for display and RT copies).
- Mapping notes: `glBlitFramebuffer` -> `CopySubresourceRegion` or shader blit;
  `glGetTexImage` -> staging readback; texture origin is top-left in D3D (the
  presenter contract is already "row 0 at the top"); GL's -1..1 clip depth vs
  D3D's 0..1 (vertices already carry 0..1 depth: check the projection);
  pixel centres are at .5 in both.
- `PS2X_HW_SCALE` upscaling and `PS2X_HW_DUMP` keep working.
- Done when: captures match the GL executor within tolerance on all reference
  scenes, then the GL executor and the stage 1 GL blit are deleted and `hw`
  (D3D11) becomes the runtime default.

Result:
- `RhiExecutor` in `gs_hw.cpp` runs the recorded command lists on the window's
  device; shaders `src/render/shaders/gs_draw.hlsl` (draw, with a dual-source
  variant) and `gs_util.hlsl` (blits, fills), exposed by `render/gs_shaders.h`.
  The recorder is unchanged except one line of behaviour (flat colours, below).
- D3D11 differences handled:
  - Y: render targets keep PS2 rows top-down; the vertex shader flips into NDC;
    scissor and copy coordinates stay as they were.
  - Flat shading: GL used the last vertex, D3D's `nointerpolation` the first; the
    recorder now writes the last vertex's colour to every vertex of a flat
    primitive (the GS rule), so the shader doesn't care.
  - D3D binds a depth buffer only with an equally sized colour target: when they
    differ, the smaller one grows (colour keeps its contents; depth is recreated
    cleared, as the GL version did when it grew).
  - Partial colour clears and the depth fills (VCS's Z clear through a colour
    draw) are scissored fill draws; depth fills use a depth-only pass.
  - A draw that samples its own render target reads a copy (D3D unbinds a bound
    target from the shader instead of reading it). VCS does this.
  - The displayed picture is drawn with alpha 1 by the blit shader (GL masked a
    clear).
- Parity (`PS2X_HW_PARITY=1`, kept for one commit, 2e85a69): the old GL executor ran
  the same frames in a hidden GL context and the displays were compared every 30
  host frames over the reference script: 80 of 116 identical, worst mean difference
  0.43/255, differences only on triangle edges (top-left fill rule now in PS2
  orientation, GL's was mirrored) and one horizon row.
- Reference scenes: static 0.02 / 0.17 vs the raylib baseline. Flyover 10-44 s:
  17.7-18.6 game fps over four runs (baseline 17.9-18.2); main-thread GPU submit
  0.4 ms (GL execution was 3.2 ms; 3.7-3.9 with the stage 2 readback). The extra
  frame of display latency from stage 2 is gone.
- Checked: `PS2X_HW_SCALE=2`, `PS2X_HW_DUMP` / F12 (render targets and display as
  PPM), hidden runs, resize. vcs.exe no longer imports opengl32.dll.

At this point the game runs on D3D11 with no OpenGL or raylib left. Frame rate is
about the same as before (the game thread's render submit is still the cost).

### Stage 4 - Packet-level HLE groundwork (L) - done 2026-10-22

Where to hook: the VIF1 DMA chain, not the recompiled game functions. Per
game-internals.md, each frame's chain CALLs prebuilt model packets in the model heap
(0x1200000-0x17FFFFF), with per-instance data (matrices, lighting) uploaded
before each CALL and VU1 programs uploaded by REF+MPG. Game code stays untouched;
recognised packets are drawn natively, everything else falls through.

1. **Survey tool**: per frame, for each CALL target record the VU1 program it runs
   (MSCAL address + which upload is resident), the VIF unpack layout (formats,
   addresses, counts), the preceding uploads, and the GS state it ends up with.
   Output a coverage report: share of vertices / GS time per (program, layout).
   This decides the order of stages 5-7 and documents each program in game-internals.md.
2. **Dispatcher**: in the DMA/VIF1 path, at a CALL into a packet whose program has
   a registered handler (`src/game/`, registered like the native VU0 programs),
   the handler consumes the packet and emits a native draw; VIF/VU1/GS work for it
   is skipped. `PS2X_HLE=off` / per-program allow lists for A/B comparison.
3. **Native draws in the same frame**: a new command type in the HW backend's frame
   list, so native and emulated draws keep their order and share the same render
   targets, depth buffer and upscale factor. Blending with HUD, post effects and
   framebuffer reuse then works without special cases.
4. **Mesh cache**: decode a packet once into an immutable vertex/index buffer keyed
   by packet address + content hash. Open question: invalidation when streaming
   reuses model-heap memory - find the model load/free functions and hook those,
   or fall back to a cheap hash check per CALL.
5. **Shaders** replicate what the VU1 program does: transform with the uploaded
   matrices, the game's projection and GS XY offset, lighting, fog factor, vertex
   colour scale (0x80 = 1.0), UV scaling; textures still come from the GS backend's
   texture cache (PSMT4/T8 + CLUT already decoded there).
- Done when: dispatcher, survey report and A/B switch exist, and one simple program
  is converted end to end with matching captures.

Result (what was built differs from the plan above in one way: the hook point is the VU1
program start (MSCAL), not the DMA CALL - most world geometry arrives through REF tags, not CALLed
packets, and the VU memory at MSCAL holds everything a draw needs):
- `PS2X_HLE_SURVEY=1` (runtime `ps2_hle_survey.{h,cpp}`): per VU1 program / stage variant / UNPACK
  layout, calls, GS vertices and VU1 time; CALLed packets recorded as segments of the flattened
  chain; sample dumps for reverse engineering. `tools/vudis.py` disassembles VU code.
  Finding: VU1 program 0x0030 (a dispatcher into stage routines, docs/game-internals.md) makes ~95% of
  all GS vertices.
- MSCAL hooks (`ps2hle::setMscalHooks`): a game handler runs before each VU1 program and may skip
  it; `ps2hle::gs()` + `GS::submitNative` record a native draw with the frontend's current GS state
  (PRIM written as the skipped kick's GIFtag would).
- `GSNativeDraw` / HW backend: native draw commands in the same frame list as emulated draws
  (order, render targets, depth, texture cache, blend / alpha-test logic all shared); static vertex
  data cached on the GPU by content hash (MeshCreate / MeshDelete, LRU of 12000).
- `src/game/vcs_hle_world.cpp`: the prelit world pipeline (stage table 0x19, routines 0x0da /
  0x2700-variant / 0x2b6, both entries 0x0e9 = clip test first and 0x0de = sector already known
  inside) reimplemented in C++ and checked against the VU1 with `PS2X_HLE_VERIFY=1`: 607,917
  fast-path packets / 29.2M vertices bit-identical and every cull decision right - after matching
  the VU's round-toward-zero arithmetic. Native drawing (default; `PS2X_HLE=0` = VU1 path):
  culled packets are skipped, the rest drawn by `vs_world` (gs_draw.hlsl), which does the VU's
  transform / 12.4 snapping / STQ / colour / fog on the GPU; packets the VU would send through its
  clipper are clipped by the GPU at the clip test's near / far planes (SV_ClipDistance).
- Flyover 10-44 s: **17.9 -> 27.1 game fps**; render submit 37.5 -> 14.5 ms per frame; median frame
  33.7 ms = the game's own 30 fps cap (it now waits for VBlank 7 ms a frame); VU1 programs per frame
  1455 -> 400; GS-emulated vertices 287k -> 66k.
- Oddity found on the way: the VU path's clipper output (GIF tag 50: context 2, no fog) did not
  appear on screen - interiors where the camera sits inside the room (the intro cutscene's office)
  were black. Native GPU clipping draws them (with context 1 and fog, as the unclipped path
  would). Not investigated further in the emulated path.
  **Superseded 2026-10-23** (game-internals.md 6h): the clipper path does not use tag 50 (that is the second lit
  pass's), and the VU clipper's output now draws the office correctly. GPU clipping drew the GON_C3 poolside ground
  black, so clipped world packets are back on the VU1 by default (`PS2X_HLE_CLIPPED=1` = GPU clipping) until the
  VU's per-triangle clipper is reproduced (milestone 3). Cost: flyover 30 -> 25.5 game fps.
  **Done (milestone 3, 2026-10-23):** `src/game/vcs_vu_clipper.cpp` reproduces the clipper bit-exactly (1.2M + 2.8M
  GIF packets checked against the VU's kicks); clipped world packets are drawn from its GIF packets (PATH1) - render
  time within 0.5 ms of GPU clipping, the right picture (game-internals.md 6h).
- Remaining VU1 work: the lit-model pipelines (stage tables 0x16 / 0x13: peds, vehicles, props with
  normals; ~10% of vertices) and sprites / particles (0x02c0).

### Stage 5 - World sectors (L) - prelit world pipeline done 2026-10-22

- Biggest vertex share; mostly MODULATE, alpha test GREATER 8, fogged, PSMT4 +
  CT32 CLUT, vertex colours from V4-5 unpacks (game-internals.md, GS usage / level data).
- Includes the water UV scroll pass and the three per-frame sector passes
  (`FUN_0023E748`).
- Done when: captures match, render submit time on the flyover drops measurably
  (target: most of the world's share of the 34 ms).

### Stage 6 - Peds and vehicles (L) - skinned / lit models (table 0x16) done 2026-10-22

Result: the lit pass (docs/game-internals.md "Lit models") is reproduced in C++ (`vcs_hle_world.cpp`:
4-bone skinning, clip test on skinned positions, transform, fog, 4 directional lights + ambient)
and checked bit-exact against the VU1 (`PS2X_HLE_VERIFY=1`, together with the world: 1,448,812
packets / 65.3M vertices) except two lanes the lit transform's software pipeline leaves
inconsistent in the runtime's VU1 core (STQ.w, ignored by the GS, and the fog value that is
computed from it); they are not compared. Native drawing: skinning stays on the CPU (its result is
written back to VU memory, as a later pass over the same buffer reads it), the skinned vertices go
to the GPU per draw (transient, not cached) and `vs_lit` does transform, fog and lighting.
Cutscene 55-86 s: 28.1 -> 29.9 game fps (the cap), render submit 17.3 -> 11.8 ms, VU1 programs
565 -> 238 per frame. `PS2X_HLE_LIT=0` keeps lit models on the VU1.
Rigid lit models (vehicles, props: the world pipeline with lighting stage 0x4ff) followed the same
way: bit-exact reference, native through `vs_lit` with the world's folded matrices and a cached
mesh (also covered by `PS2X_HLE_LIT=0`).


- Skinning, the lighting dispatch program (0x0418, JALR table), vehicle env maps /
  specular, damage states, shadows if they go through the same programs.

### Stage 7 - Remaining programs (M-L) - left on the VU1 for now

Survey with everything above native (`build/scenes/survey4`): what still runs on the VU1 is
program 0x02c0 (sprites / particles / text: ~15 ms of VU1 per second, ~0.5 ms per game frame) and
the second lit pass (stage table 0x13: 0x0da, 0x4ff, 0x2b6, then 0x5b1 / 0x2b2 - probably an
environment-map pass over the skinned buffer; ~140 calls/s). Most of the remaining time the survey
attributes to 0x0030 is now the native handlers' own CPU work (1.4-3.3 us per packet). Both stay
on the exact VU path until a profile says otherwise; the method for adding them is the one above
(survey sample -> vudis.py -> C++ reference checked with PS2X_HLE_VERIFY -> vertex shader).


- Particles, water, shadows, sky, 2D sprites drawn through VU1. Whatever the
  coverage report shows is left; anything rare can stay emulated.

### HUD layer (milestone 3 step 2, 2026-10-23)

The HUD, text and radar are drawn over the shown picture at the window's size instead of into the PS2 frame buffer:
- **Which draws.** `src/game/vcs_hud.cpp` collects the Im2D primitives of the HUD context (CHud::Draw to the frame
  kick), groups them into elements and anchors each one (game-internals.md 6f). The renderer asks it about every 2D
  primitive into a 640-wide target while 3D is shown (`gsHwSetHudClassifier`, by vertex positions); a named one is
  squeezed towards its anchor (widescreen) and recorded as a `HudDraw`, not a `Draw`.
- **Menus** go the same way: from the menu draw (FUN_003b53f8) to the frame kick, the 2D primitives are named too,
  always anchored in the middle (never to the screen edges), also in frames without 3D (the pause menu; the
  squeeze stays for 3D frames). `VCS_MENU_NATIVE=0` = menus into the frame buffer.
- **Which frame.** The executor keeps a layer per frame buffer: HUD draws go to their buffer's layer; any other
  drawing into the buffer (the next frame's scene) makes it stale, and the next HUD draw starts it afresh. A plain
  copy of a buffer takes its layer along: VCS copies its finished frame (FBP 140) into the displayed buffer (FBP 0)
  at the start of the next frame. The copy also closes the source's layer (the next HUD draw replaces it): the pause
  menu draws nothing else into FBP 140, so its layer grew by a frame's menu every frame before.
- **Drawing.** `composeDisplay` makes the picture at the output size (`gsHwSetOutputSize`: the window's letterboxed
  area, set by the runtime each host frame), blits the displayed buffer into it (bilinear, as the presenter scaled it
  before) and draws the displayed buffer's layer on top with the same draw shaders, textures and blend modes: target
  size in PS2 pixels = output / (640x448), scissor scaled likewise, its own depth buffer cleared to 0 (the game
  clears Z to 0 just before the HUD), alpha not written (the shown picture stays opaque).
- **The PS2's pixels, drawn larger** (lines seen at 1080p fullscreen, fixed 2026-10-23). The layer keeps what
  640x448 would have shown:
  - Coverage: every layer vertex moves to the pixel boundary the PS2's coverage gives (x' = ceil(x - 0.5)), its
    triangle's STQ / colour / Z / fog re-evaluated there; triangles that then cover nothing are dropped. Without it
    sub-pixel gaps between pieces got pixels of their own: the radar map showed in a line left of its circle (map
    scissored from pixel 27, corner mask from x 27.4).
  - Sampling: a textured quad (two triangles over one rectangle, CLAMP) samples only the texels inside its own
    rectangle (REGION_CLAMP). The fonts are atlases with cells side by side; at 1080p a glyph's edge pixels sample
    within half a texel of the cell border and bilinear filtering pulled in the next cell's outline (lines under the
    clock's digits, beside its colon).
- `VCS_HUD_NATIVE=0` = the HUD drawn into the frame buffer as before (with the anchoring still applied);
  `PS2X_HW_HUD_LOG=1` logs the layers every 2 s; `PS2X_HW_OUTPUT=WxH` forces the output size (a full-screen window
  in a test run, with `PS2X_CAPTURE_SIZE`).
- Textures: the GS backend's decoded texture cache (the same textures the emulated path samples) rather than a
  decoder for RenderWare rasters (DECISIONS.md).

### Post effects (milestone 3 step 3, started 2026-10-23)

The user's priority: authentic at the original 640x448 first, then at higher resolutions and other aspect ratios.
VCS's effects (game-internals.md 6g: frame copies, self-copy, bloom, trails, motion blur) are GS draws that read
render targets; at 640x448 the emulation of those draws is the reference, so the work so far makes that exact and
then makes it hold at higher resolutions:
- **GS pixel centres** (`gsPixelCentreOffset`, `PS2X_HW_PIXEL_CENTRE=0` = before): every draw (GS path and native
  draws: `rtOrigin.z` in the vertex shaders) moves by +0.5 pixel and the scissor with it, so each target pixel
  samples coverage, colour and texture coordinates where the GS samples (pixel X at X). Before, the per-frame copies
  averaged neighbouring texels and the picture was softer than the PS2's. The HUD layer's pixel snapping and the
  widescreen scissor squeeze follow the same rule.
- **Copies at full resolution** (`rtFull`, `PS2X_HW_RT_FULL=0` = before): a draw that reads a render target 1:1 (one
  texel per pixel, no rotation; checked per draw from its vertices) reads it at the target's resolution
  (`sampleRtFull`; wrap / clamp still in PS2 texels). At scale 1 it fetches the same texels. Before, every copy
  brought the picture back to 640x448, so a higher internal resolution was lost (the shown buffer is a copy).
  Resampling reads (bloom / trails downsamples and their blends back) keep PS2-texel sampling: they look as at
  640x448 at any scale.
- Checked: 640x448 before / after (sharper, matching the GS's sampling; static regions of a frame identical with and
  without `rtFull`), 3x internal (fence and wire at full detail instead of a 640x448 picture scaled up), HUD at
  1080p, reference scenes; game fps unchanged.
- Not done yet: the effects as native passes (needed for a native frame, step 4); in widescreen the bloom / trails
  kernels work in the anamorphic 640-wide frame, so they are 4:3 / aspect wider horizontally than at 4:3.

### Stage 8 - Enhancements (M each, optional order) - internal resolution done

- Internal resolution: native draws render into the same scaled render targets as the GS path, so
  `PS2X_HW_SCALE` (or `scale=<1..8>` in vcs.ini next to vcs.exe) raises the whole picture's
  resolution; checked at 3x (gameplay still at the 30 fps cap). Native draws keep the PS2's
  1/16-pixel vertex snapping (~0.2 px at 3x).
- Widescreen is not done yet; planned in docs/SETTINGS_MENU_WIDESCREEN.md. The game has its own
  widescreen mode (aspect global 0x487488, preference at *(0x48F3D0)+0x30, a HUD that follows it),
  which drives the camera, culling and both renderers - so no projection patching is needed.
- MSAA / anisotropic filtering are not done: the GS emulation samples textures with explicit
  texel loads (to reproduce the GS wrap / clamp modes and render-target textures) and reads render
  targets back as textures, which an MSAA target would need resolving for at every such use.
  Higher internal resolution is the supersampling route that works today.


Only possible cleanly once the 3D scene is native:
- Native-resolution 3D independent of the PS2 framebuffer size (GS path keeps
  `PS2X_HW_SCALE`), MSAA, anisotropic filtering, higher-precision depth.
- Widescreen: projection change in the camera code + HUD aspect handling
  (game-internals.md "Camera and projection" / "HUD" sections still to fill).
- Each as an option that defaults to authentic output.

## Verification

- Every stage: the stage 0 capture diff over the reference scenes; perf CSV
  before/after recorded in PROGRESS.md.
- HLE handlers: per-program A/B (`PS2X_HLE`), and a debug mode that draws the native
  and emulated versions of the same packet into two targets and reports the diff.
- D3D11 debug layer clean (no warnings) in debug runs.

## Risks and open questions

- GS tricks the native path must respect: Z cleared by drawing CT32 strips into the
  depth buffer's memory (handled today as `DepthFill`); framebuffers reused as
  textures for post effects; the PMODE two-circuit anti-flicker blend; 16-bit
  targets for effects. Keeping native draws inside the GS backend's frame list is
  what makes these keep working.
- Mesh cache invalidation under streaming (stage 4, item 4).
- Programs that read back results or depend on VU1 side effects (none known for
  rendering, but the survey must check XGKICK-free paths and MAC-flag use).
- Open bugs that may live in the GS backend (screen black for a few frames,
  frontend menu not appearing): don't let the port hide them - reproduce them on
  the GL executor first if they change behaviour.
- Upstream divergence grows: raylib removal touches several runtime files.

## Decisions to confirm in review

1. SDL3 for the platform layer.
2. Own thin RHI (D3D11 now) instead of an existing library.
3. Stage 1 keeps OpenGL briefly so the platform swap and the API swap are separate,
   bisectable steps (costs a small throwaway GL blit).
4. paraLLEl-GS kept as readback-only (its Vulkan window present is dropped).
5. HLE hooks at the VIF1 packet level with a mesh cache, not in recompiled game code.
6. The emulated GS path stays as fallback and reference indefinitely.
