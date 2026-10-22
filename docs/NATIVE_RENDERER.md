# Native renderer plan (D3D11)

Status: **approved** (2026-10-22). Stages 0-2 done; next is stage 3.

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

### Stage 3 - GS HW backend executor on the RHI (M)

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

At this point the game runs on D3D11 with no OpenGL or raylib left. Frame rate is
about the same as today.

### Stage 4 - Packet-level HLE groundwork (L)

Where to hook: the VIF1 DMA chain, not the recompiled game functions. Per
ENGINE.md, each frame's chain CALLs prebuilt model packets in the model heap
(0x1200000-0x17FFFFF), with per-instance data (matrices, lighting) uploaded
before each CALL and VU1 programs uploaded by REF+MPG. Game code stays untouched;
recognised packets are drawn natively, everything else falls through.

1. **Survey tool**: per frame, for each CALL target record the VU1 program it runs
   (MSCAL address + which upload is resident), the VIF unpack layout (formats,
   addresses, counts), the preceding uploads, and the GS state it ends up with.
   Output a coverage report: share of vertices / GS time per (program, layout).
   This decides the order of stages 5-7 and documents each program in ENGINE.md.
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

### Stage 5 - World sectors (L)

- Biggest vertex share; mostly MODULATE, alpha test GREATER 8, fogged, PSMT4 +
  CT32 CLUT, vertex colours from V4-5 unpacks (ENGINE.md, GS usage / level data).
- Includes the water UV scroll pass and the three per-frame sector passes
  (`FUN_0023E748`).
- Done when: captures match, render submit time on the flyover drops measurably
  (target: most of the world's share of the 34 ms).

### Stage 6 - Peds and vehicles (L)

- Skinning, the lighting dispatch program (0x0418, JALR table), vehicle env maps /
  specular, damage states, shadows if they go through the same programs.

### Stage 7 - Remaining programs (M-L)

- Particles, water, shadows, sky, 2D sprites drawn through VU1. Whatever the
  coverage report shows is left; anything rare can stay emulated.

### Stage 8 - Enhancements (M each, optional order)

Only possible cleanly once the 3D scene is native:
- Native-resolution 3D independent of the PS2 framebuffer size (GS path keeps
  `PS2X_HW_SCALE`), MSAA, anisotropic filtering, higher-precision depth.
- Widescreen: projection change in the camera code + HUD aspect handling
  (ENGINE.md "Camera and projection" / "HUD" sections still to fill).
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
