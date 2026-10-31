# The native renderer

The game's draws become native GPU draws: the world, characters, vehicles, HUD, menus and post effects are drawn
through the RHI (`src/rhi/rhi.h`) and its Vulkan backend on the PS5's GPU (the console's RADV driver, docs/PS5.md).
The game's code is untouched: its VU1 programs and GS packets are recognised where they start and drawn natively, and
whatever is not recognised falls through to the emulated path (the HW backend recording GS primitives), which stays
as the fallback and the reference to compare against.

Status: the 3D scene native (world, skinned and rigid lit models, the clipper, particles / sprites), the HUD and menus
in a native layer at the output's size, bloom / trails / frame copies / motion blur native, internal resolution up to
8x or the TV's own, MSAA, anisotropic filtering; **no VU1 program runs and no draw goes through the GS front end in a
normal frame** (docs/FULLY_NATIVE.md has the steps left). The numbers below were measured before the PS5 build
existed, at the PS2's resolution unless noted, and stay as the record of what each step gained.

Overall, VU path -> native (stage 6), reference scenes:

| phase | game fps | render submit ms/frame |
|---|---|---|
| flyover 10-44 s | 18.2 -> 28.3 | 36.7 -> 12.1 |
| cutscene 55-86 s | 18.8 -> 30.0 (cap) | 38.1 -> 11.4 |
| gameplay 100-125 s | 29.9 -> 29.9 (cap) | 10.6 -> 5.3 |

## Why

Where the frame went with every draw emulated (title flyover):

| part | ms / frame |
|---|---|
| render submit: VIF1 unpack, compiled VU1, GS recording | ~34 |
| game code (EE) | ~15 |
| GPU submission (main thread) | ~2.5 |

- The speed-up is skipping VIF / VU1 / GS for model draws: decode the game's model packets once into GPU buffers and
  draw them natively (stages 4+).
- The emulated GS path stays as the fallback for anything not converted and as the reference.

## Layers

```
 runtime (third_party/PS2Recomp/ps2xRuntime)       game-specific (src/game)
   EE / VIF / VU1 / GS frontend  ──────────────┐     HLE handlers (vcs_hle_world.cpp, vcs_vu_clipper.cpp, vcs_hud.cpp)
                                               ▼            │
 GS HW backend   recorder (render worker / GS stage) ◄──────┘ native draw cmds
 (gs_hw.cpp)     executor (main thread, talks only to rhi::)
                                               │
 src/rhi         rhi.h (interface) ── vulkan/rhi_vulkan.cpp (the only place with Vulkan)
                                               │
 src/platform    platform.h (display, controller, audio, time) ── platform_ps5.cpp (the PS5's own libraries)
 src/render      presenter, the Dear ImGui renderer, the shaders (HLSL -> SPIR-V at build time)
```

Rules:
- Vulkan headers only under `src/rhi/vulkan/`; the console's libraries only under `src/platform/`. Everything else
  includes `rhi.h` / `platform.h`.
- Shaders are written in HLSL (`src/render/shaders/`) and compiled to SPIR-V by glslang at build time
  (`tools/hlsl_spirv.py`, `vcs_shader()` in `src/render/CMakeLists.txt`).
- The runtime reaches the platform and the RHI through its host-backend seam (`ps2_host_backend.h`); patches go in
  PS2RECOMP_PATCHES.md as usual.
- `VCS_RHI=null` is a backend that accepts everything and draws nothing: the CPU side runs as with a real renderer, so
  it can be brought up apart from the GPU.

### The RHI

Own interface rather than a library: GS emulation needs render-target aliasing, sub-rectangle copies between targets
and texture readbacks. It stays small:
- Resources: buffers (vertex, index, constant; immutable / dynamic per frame), 2D textures (sampled, render target,
  depth; sample counts, mip levels), samplers (anisotropy).
- Pipelines: immutable objects combining shaders + blend + depth / stencil + raster state, created from a hashable key
  and cached. GS state (blend ABCD / FIX, ZTST, ATST, colour mask...) maps to a key.
- Explicit render passes: begin(colour target, depth target, load / clear ops), draws, end.
- Copies: texture region -> texture region, buffer uploads, readbacks (async with a fence, sync for dumps),
  `resolveTexture`, `generateMips`.
- The swapchain on the TV, vsync on / off. The Dear ImGui renderer hook (the settings menu, the trainer, the overlay).
- Features relied on: dual-source blending (the HW backend uses `SRC1_ALPHA`), 32F depth, R8G8B8A8 targets, point /
  line lists (GS points / lines are 1 px, no point size needed).

### The executor

`RhiExecutor` in `gs_hw.cpp` runs the recorded frame command lists (draws with `HwState`, texture creates / deletes,
render-target uploads / drops, clears, depth fills, native draws) on the device; shaders `gs_draw.hlsl` (draws, with a
dual-source variant) and `gs_util.hlsl` (blits, fills), exposed by `render/gs_shaders.h`. What it does for the GS:
- Y: render targets keep PS2 rows top-down; the vertex shader flips into clip space; scissor and copy coordinates stay
  as they are.
- Flat shading: the recorder writes the last vertex's colour to every vertex of a flat primitive (the GS rule), so the
  shader doesn't care which vertex the API takes.
- A depth buffer is bound only with an equally sized colour target: when they differ, the smaller one grows (colour
  keeps its contents; depth is recreated cleared).
- Partial colour clears and depth fills (VCS's Z clear through a colour draw) are scissored fill draws; depth fills use
  a depth-only pass.
- A draw that samples its own render target reads a copy. VCS does this.
- The displayed picture is drawn with alpha 1 by the blit shader.
- `PS2X_HW_SCALE` (internal resolution) and `PS2X_HW_DUMP` (render targets and display as PPM) work on every path.

## Stages

Each stage ended with the game fully playable and a commit.

### Stage 4 - Packet-level HLE groundwork

The hook point is the VU1 program start (MSCAL): most world geometry arrives through REF tags, not CALLed packets,
and the VU memory at MSCAL holds everything a draw needs. Game code stays untouched; recognised programs are drawn
natively, everything else falls through.
- `PS2X_HLE_SURVEY=1` (runtime `ps2_hle_survey.{h,cpp}`): per VU1 program / stage variant / UNPACK layout, calls, GS
  vertices and VU1 time; CALLed packets recorded as segments of the flattened chain; sample dumps for reverse
  engineering. `tools/vudis.py` disassembles VU code. Finding: VU1 program 0x0030 (a dispatcher into stage routines,
  docs/game-internals.md) makes ~95% of all GS vertices.
- MSCAL hooks (`ps2hle::setMscalHooks`): a game handler runs before each VU1 program and may skip it; `ps2hle::gs()` +
  `GS::submitNative` record a native draw with the frontend's current GS state (PRIM written as the skipped kick's
  GIFtag would).
- `GSNativeDraw` / HW backend: native draw commands in the same frame list as emulated draws (order, render targets,
  depth, texture cache, blend / alpha-test logic all shared); static vertex data cached on the GPU by content hash
  (MeshCreate / MeshDelete, LRU of 12000).
- `src/game/vcs_hle_world.cpp`: the prelit world pipeline (stage table 0x19, routines 0x0da / 0x2700-variant / 0x2b6,
  both entries 0x0e9 = clip test first and 0x0de = sector already known inside) reimplemented in C++ and checked
  against the VU1 with `PS2X_HLE_VERIFY=1`: 607,917 fast-path packets / 29.2M vertices bit-identical and every cull
  decision right - after matching the VU's round-toward-zero arithmetic. Native drawing (default; `PS2X_HLE=0` = VU1
  path): culled packets are skipped, the rest drawn by `vs_world` (gs_draw.hlsl), which does the VU's transform / 12.4
  snapping / STQ / colour / fog on the GPU.
- Flyover 10-44 s: **17.9 -> 27.1 game fps**; render submit 37.5 -> 14.5 ms per frame; VU1 programs per frame 1455 ->
  400; GS-emulated vertices 287k -> 66k.
- Clipped packets: `src/game/vcs_vu_clipper.cpp` reproduces the VU's per-triangle clipper bit-exactly (1.2M + 2.8M GIF
  packets checked against the VU's kicks); clipped world packets are drawn from its GIF packets (PATH1) - the right
  picture, where clipping on the GPU at the clip test's planes had drawn the GON_C3 poolside ground black
  (game-internals.md 6h; `PS2X_HLE_CLIPPED=1` = GPU clipping).

### Stage 5 - World sectors

The biggest vertex share: mostly MODULATE, alpha test GREATER 8, fogged, PSMT4 + CT32 CLUT, vertex colours from V4-5
unpacks (game-internals.md, GS usage / level data); includes the water UV scroll pass and the three per-frame sector
passes (`FUN_0023E748`). Done with the prelit world pipeline above.

### Stage 6 - Peds and vehicles

The lit pass (docs/game-internals.md "Lit models") is reproduced in C++ (`vcs_hle_world.cpp`: 4-bone skinning, clip
test on skinned positions, transform, fog, 4 directional lights + ambient) and checked bit-exact against the VU1
(`PS2X_HLE_VERIFY=1`, together with the world: 1,448,812 packets / 65.3M vertices) except two lanes the lit
transform's software pipeline leaves inconsistent in the runtime's VU1 core (STQ.w, ignored by the GS, and the fog
value computed from it); they are not compared. Skinning stays on the CPU (its result is written back to VU memory, as
a later pass over the same buffer reads it), the skinned vertices go to the GPU per draw (transient, not cached) and
`vs_lit` does transform, fog and lighting. Cutscene 55-86 s: 28.1 -> 29.9 game fps (the cap), render submit 17.3 ->
11.8 ms, VU1 programs 565 -> 238 per frame. `PS2X_HLE_LIT=0` keeps lit models on the VU1. Rigid lit models (vehicles,
props: the world pipeline with lighting stage 0x4ff) followed the same way: bit-exact reference, native through
`vs_lit` with the world's folded matrices and a cached mesh (also covered by `PS2X_HLE_LIT=0`).

### Stage 7 - The remaining programs

Program 0x02c0 (sprites / particles / text, its shine entry 0x0d) and the second lit pass (stage table 0x13: 0x0da,
0x4ff, 0x2b6, then 0x5b1 / 0x2b2 - an environment-map pass over the skinned buffer) are reproduced in C++ too
(game-internals.md 6d), as are the setup programs (matrices, per-batch tables, bone copies; ~330 runs a frame in the
cutscene) and the per-frame Z clear: VU1 programs a frame 195 -> 4 (cutscene), 86 -> 4 (gameplay), then 0
(`vu1_programs` in the reference scenes and the world tour). The method for any program is the same: survey sample ->
vudis.py -> C++ reference checked with `PS2X_HLE_VERIFY` -> vertex shader or packed draws.

### Stage 8 - Enhancements

Each is an option that defaults to the authentic output:
- Internal resolution: native draws render into the same scaled render targets as the GS path, so `PS2X_HW_SCALE` (or
  `display.scale` 1-8 in vcs.ini) raises the whole picture's resolution; native draws keep the PS2's 1/16-pixel vertex
  snapping (~0.2 px at 3x). The native frame (below) draws at the output's own size.
- Widescreen: the game has its own widescreen mode (aspect global 0x487488, preference at *(0x48F3D0)+0x30, a HUD that
  follows it), which drives the camera, culling and both renderers - so no projection patching is needed
  (docs/SETTINGS_MENU_WIDESCREEN.md).
- MSAA and anisotropic filtering: under "Native frame" below.

## HUD layer

The HUD, text and radar are drawn over the shown picture at the output's size instead of into the PS2 frame buffer:
- **Which draws.** `src/game/vcs_hud.cpp` collects the Im2D primitives of the HUD context (CHud::Draw to the frame
  kick), groups them into elements and anchors each one (game-internals.md 6f). The renderer asks it about every 2D
  primitive into a 640-wide target while 3D is shown (`gsHwSetHudClassifier`, by vertex positions); a named one is
  squeezed towards its anchor (widescreen) and recorded as a `HudDraw`, not a `Draw`.
- **Menus** go the same way: from the menu draw (FUN_003b53f8) to the frame kick, the 2D primitives are named too,
  always anchored in the middle (never to the screen edges), also in frames without 3D (the pause menu; the squeeze
  stays for 3D frames). `VCS_MENU_NATIVE=0` = menus into the frame buffer.
- **Which frame.** The executor keeps a layer per frame buffer: HUD draws go to their buffer's layer; any other drawing
  into the buffer (the next frame's scene) makes it stale, and the next HUD draw starts it afresh. A plain copy of a
  buffer takes its layer along: VCS copies its finished frame (FBP 140) into the displayed buffer (FBP 0) at the start
  of the next frame. The copy also closes the source's layer (the next HUD draw replaces it): the pause menu draws
  nothing else into FBP 140, so its layer grew by a frame's menu every frame before.
- **Drawing.** `composeDisplay` makes the picture at the output size (`gsHwSetOutputSize`: the letterboxed area on the
  TV, set by the runtime each host frame), blits the displayed buffer into it (bilinear) and draws the displayed
  buffer's layer on top with the same draw shaders, textures and blend modes: target size in PS2 pixels = output /
  (640x448), scissor scaled likewise, its own depth buffer cleared to 0 (the game clears Z to 0 just before the HUD),
  alpha not written (the shown picture stays opaque).
- **The PS2's pixels, drawn larger** (lines seen at 1080p, fixed 2026-10-23). The layer keeps what 640x448 would have
  shown:
  - Coverage: every layer vertex moves to the pixel boundary the PS2's coverage gives (x' = ceil(x - 0.5)), its
    triangle's STQ / colour / Z / fog re-evaluated there; triangles that then cover nothing are dropped. Without it
    sub-pixel gaps between pieces got pixels of their own: the radar map showed in a line left of its circle (map
    scissored from pixel 27, corner mask from x 27.4).
  - Sampling: a textured quad (two triangles over one rectangle, CLAMP) samples only the texels inside its own
    rectangle (REGION_CLAMP). The fonts are atlases with cells side by side; at 1080p a glyph's edge pixels sample
    within half a texel of the cell border and bilinear filtering pulled in the next cell's outline (lines under the
    clock's digits, beside its colon).
  - Widescreen (2026-10-27): the squeeze towards an element's anchor is applied in the layer after the snap, to the
    vertices and the scissor alike and unrounded (`DrawCmd::hudSqueeze`). Squeezed before the snap, the layer rounded
    to PS2 pixels of the squeezed frame - 6 screen pixels each at 32:9 - and the radar's scissor and corner mask landed
    on different ones: a column of map outside the circle (the user's report).
- `VCS_HUD_NATIVE=0` = the HUD drawn into the frame buffer as before (with the anchoring still applied);
  `PS2X_HW_HUD_LOG=1` logs the layers every 2 s; `PS2X_HW_OUTPUT=WxH` forces the output size.
- Textures: the GS backend's decoded texture cache (the same textures the emulated path samples) rather than a decoder
  for RenderWare rasters (DECISIONS.md).

## Post effects

The user's priority: authentic at the original 640x448 first, then at higher resolutions and other aspect ratios.
VCS's effects (game-internals.md 6g: frame copies, self-copy, bloom, trails, motion blur) are GS draws that read render
targets; at 640x448 the emulation of those draws is the reference, so the work makes that exact and then makes it hold
at higher resolutions:
- **GS pixel centres** (`gsPixelCentreOffset`, `PS2X_HW_PIXEL_CENTRE=0` = before): every draw (GS path and native
  draws: `rtOrigin.z` in the vertex shaders) moves by +0.5 pixel and the scissor with it, so each target pixel samples
  coverage, colour and texture coordinates where the GS samples (pixel X at X). Before, the per-frame copies averaged
  neighbouring texels and the picture was softer than the PS2's. The HUD layer's pixel snapping and the widescreen
  scissor squeeze follow the same rule.
- **Copies at full resolution** (`rtFull`, `PS2X_HW_RT_FULL=0` = before): a draw that reads a render target 1:1 (one
  texel per pixel, no rotation; checked per draw from its vertices) reads it at the target's resolution
  (`sampleRtFull`; wrap / clamp still in PS2 texels). At scale 1 it fetches the same texels. Before, every copy brought
  the picture back to 640x448, so a higher internal resolution was lost (the shown buffer is a copy). Resampling reads
  (bloom / trails downsamples and their blends back) keep PS2-texel sampling: they look as at 640x448 at any scale.
- **Native bloom and trails** (`nativePost`, `PS2X_HW_POSTFX=0` = as recorded, `=b` / `=t` one of them): the two chains
  are recognised in a frame's commands (bloom: downsample into a 256-wide buffer, threshold, n x (clear, 4-tap cross),
  add back; trails: downsample into a 512-wide buffer, warped blend back at FIX ~8, tint add) and drawn into buffers of
  our own with the same draws, shader and blend states (draw() overrides: target, texture, vertices, scissor scale). At
  4:3 they give the emulation's picture (static regions as close as two runs of one build). In widescreen the buffers
  are w = display aspect / (4/3) times wider: positions in a buffer scale by w and texture coordinates into one become
  u + (w - 1) * s * x (s = buffer texels per target unit at 4:3), so the taps and the warp, in texels, cover the same
  screen distance as at 4:3 - before, both were w times wider on screen (the glow beside a pole at 16:9 is narrower by
  about that). The buffers are written back, squeezed, into the effect buffers in VRAM for later readers (the game
  textures vehicles with the 128x128 corner of FBP 420). REPEAT wrapping is a modulo in the shader (the widened buffers
  are not powers of two; same result for GS sizes).
- **Frame copy and motion blur** (`nativeFrameCopy`, letter `c` of `PS2X_HW_POSTFX`): the full-screen 1:1 reads of
  another 640-wide target (u = x + 0.5) that the game draws as 40 strips: a plain copy (the shown buffer) is a texture
  copy, a blended one (motion blur, colour 127/128 at FIX 16) one full-screen quad with the draw's own state. Same
  picture as the strips (static regions within run-to-run noise). The self-copy over the sky stays a GS draw
  (depth-tested; 1:1, so read at full resolution).

## Native frame

"Internal resolution: Match window" (`display.scale` 0, `PS2X_HW_SCALE=0`; on the PS5 the TV output's size): the
picture's own targets - the 640-wide ones: the drawn frame FBP 140, the shown copy FBP 0, the Z buffer FBP 280 - are
drawn at the output's size (the letterboxed area at the 3D frames' aspect, `gsHwSetFrameSize`; it stays when a menu
shows at 4:3), every other target (effect buffers) at 1x - except the reflection map, which the game names a scaled
target (`gsHwSetTargetRole`, docs/FULLY_NATIVE.md step 3d; `PS2X_HW_REFLECTION_SCALE=0` = 1x). The radar has no
target: it is drawn in the HUD layer. So the frame is not a multiple of 640x448: at 16:9 1080p it is 1920x1080, 3 x
2.41 pixels per PS2 pixel.
- Each target and depth buffer has its own scale (`Rt::scale`, `scaleFor`): creation, growth (rescaled when the output
  changes), uploads, clears, depth fills, scissor, the display, write-backs; the shader reads a render target with
  separate x / y scales (`rtScale2`, `fetchRtFull` for fractional scales). A Z buffer is kept per scale (the effect
  buffers carry the frame's ZBP), and ZTST ALWAYS without Z writes binds none.
- With the copies native and 1:1 reads at full resolution, nothing brings the picture back to 640x448; the bloom /
  trails stay at their 1x buffers (their look as on the PS2). The HUD layer is drawn at the same size.
- `PS2X_HW_FRAME=WxH` forces the frame size (with `PS2X_HW_OUTPUT`).
- Checked: scenes at 16:9 1920x1080 against 1x (static scenes the same: loading 1.24, pause map 0.05 mean difference;
  3D sharper, effects as before), 1x unchanged by the per-target scales; game fps within run-to-run noise of 1x.
- **MSAA** (Display > Anti-aliasing, `display.msaa` / `PS2X_HW_MSAA` = 2 / 4 / 8, restart; off by default): the
  picture's targets (640 wide) and their depth buffers are multisampled; every read of such a target - render-target
  textures (effects, copies, the self-copy), the display, growth, dumps - sees a single-sampled copy resolved when the
  target was drawn to since the last read (`readable`, `Rt::dirty`); a multisampled frame copy is drawn instead of
  copied. Effect buffers stay single-sampled. Depth buffers are kept per scale and sample count.
- **Anisotropic filtering** (Display > Texture filtering, `display.aniso` / `PS2X_HW_ANISO` = 2-16, restart; off = the
  original bilinear): the game's textures get a mip chain, and bilinear-filtered draws with plain repeat / clamp
  wrapping (about 90% of textured draws outside the barracks) are sampled by the hardware - trilinear, anisotropic -
  instead of the GS's own bilinear (`linearIipFgeAte.x == 2`, `hwSampler`); region wraps, render targets and nearest
  filtering keep the exact GS path. `PS2X_HW_ANISO_LOG=1` counts which path textured draws take.
- **The GS bilinear by Gather** (2026-10-30, PERFORMANCE.md section 15): with filtering off, the same draws
  (power-of-two textures without a mip chain) keep the GS's bilinear but fetch its 2x2 texels with `Gather` through a
  nearest sampler of their wrapping (`linearIipFgeAte.x == 3`, `m_gatherSampler`), drawn with a pixel shader built for
  that path alone (`SAMPLE_MODE` 5 of the per-read variants, `drawPS[5][dual]`, pipeline key bits 24-26 -
  PERFORMANCE.md 18). The same texels and weights as the four loads (`PS2X_HW_GATHER=0`; `=verify` draws both and
  marks a differing pixel magenta). It made the helicopter's dust - 60 screens of textured overdraw - hold 60 fps at
  3x.
- Checked at 16:9 1080p native frame: MSAA 4x smooths the polygon edges (poles, roofs), AF 16x keeps the paving and
  ground decals sharp into the distance; static scenes unchanged (loading 0.55, pause map 0.13); game fps unchanged
  with both on.
- Not done yet: higher-precision depth (D32F already holds the PS2's 24-bit Z exactly); the radar at higher
  resolution.

## Render submission worker (2026-10-23)

Render submission - the VIF1 / GIF chains, VU1 microprograms (and the native draws they reach), the GS front end and
the HW backend's frame building - runs on its own thread, `RenderWorker` (`runtime/ps2_render_worker.h`,
`PS2X_RENDER_THREAD=0` for the synchronous path; on by default with `PS2X_GS=hw`). The game thread copies each chain
when kicked, marks the DMA complete at once, and posts the work; the worker runs it in kick order. The frame's end
(`endFrame` at VBlank, with the display registers read then) is posted too, and the game thread waits rather than get
more than one frame ahead (`PS2X_RENDER_THREAD_AHEAD`).
- **Fences**: whatever the game can observe of that work waits until the worker has run everything posted before: GS
  CSR / SIGLBLID reads and CSR writes, GIF / VIF1 registers, VIF1 / GIF FIFOs, VU1 memory, the HLE GS library stubs,
  and any other direct call into the GIF / VIF1 from the game thread (`FenceReason::Other`, a safety net). In gameplay
  only the CSR is read, ~5 times a frame (game-internals.md 4); waiting costs 2-8 ms per second.
- **In order with the chains**: VIF1 FIFO writes; the HUD anchor map (built at the frame kick, handed to the classifier
  through the queue - `vcs_hud.cpp`). The native GIF fast paths (`tryProcessNativeGif*`) step aside: their chains take
  the copied path to the worker.
- **Thread safety**: the VU1 start callbacks don't touch the game thread's context on the worker (no VU1 D / T stops
  there - VCS never enables them); the VU decode's MAC-read tables are locked (VU0 decodes on the game thread at the
  same time); chain buffers come from a pool shared by both threads; the worker runs with the game thread's MXCSR.
- `PS2X_RENDER_THREAD_LOG=1`: items / frames per second, game-thread waits, fences by reason. The perf line gains
  `worker_ms` (the worker's busy time per frame); `render_ms` is only the game thread's part (copying chains,
  waiting).
- Measured (60 fps mode): outside the barracks at the 60 cap with 4 ms to spare (game thread 11.5 ms, worker ~5 ms);
  driving (AI car from Fort Baxter) 26-29 -> 44-54 fps - game code ~17-20 ms was then the limit, worker ~17 ms in
  parallel; reference scenes' flyover 42.5 -> 59.9 fps.

## The GS stage (2026-10-25, docs/PERFORMANCE.md 7)

The render worker split in two (`PS2X_GS_THREAD=0` = one thread; on by default with the worker and the HW renderer).
The worker keeps the VIF1 chains, RSL replay, VU1 programs and their native reproductions (skinning, clipping,
lighting); the GS front end and the HW backend's frame building (draw state, texture binding and decoding, uploads,
CLUT loads, packed / native draw recording, the frame end) run on a second thread, `GsStage`.
- **Commands**: the GS entry points the worker calls (`processGIFPacket`, `submitPackedKick`, `submitNative`,
  `writeRegister`) go into a single-producer ring (64 MB, `ps2_render_worker::gsReserve / gsSend`) with their data
  copied - GIF packets, the native draw and its vertices - and the worker's thread-locals the GS reads (GIF path, PATH1
  producer = draw source, the upload's raster) captured; the stage runs them in order.
- **Answers the worker needs**: `submitPackedKick`'s "not that shape" fallback (`processGIFPacket`) runs on the stage;
  `submitNative`'s refusal is decided by the draw alone - the backend's `AcceptsNative`, which SubmitNative uses too -
  so the worker knows at once whether to run the VU program instead (PRIM is written either way).
- **In order with the commands**: the frame end (`endFrame`, run through `runOnGsStage`; the worker is never more than
  one frame end ahead of the stage) and the HUD anchor map (`vcs_hud.cpp`). The rest of the GS that the worker could
  reach (native packed / image upload paths, framebuffer clears, local->host transfers) drains the stage first and runs
  on the worker.
- **Fences** wait for the worker, then for the stage (`gsDrain`). Off while `PS2X_HLE_VERIFY` / `PS2X_HLE_SURVEY` read
  what the GS recorded from the worker.
- The perf line gains `gs_ms` (the stage's busy time per frame; `worker_ms` is the worker's own).
- Measured (uncapped): city 1325,-325 (~1,150 draws) 142 -> 215 fps (worker 6.7 ms -> 3.5 + stage 4.5 in parallel);
  the world tour's slowest points were all worker-bound.

## Draw sources - what is still drawn through the GS path

The "unconverted draws" counter. Every recorded GS-path draw carries where its GIF packet came from
(`runtime/gs/gs_draw_source.h`: PATH2, PATH3, or PATH1 from a VU1 program / from a C++ reproduction of one - the `hle`
packets: the clipper, the env pass, 0x02c0, variant C, which compute natively but hand GIF packets to the GS). The
executor counts per frame:
- perf line / overlay: `gs_draws` `gs_verts` (draws still made from GS primitives), `gs_vu1` `gs_hle` `gs_path2`
  `gs_path3` (the same by source), `native_draws`, `hud_draws` (native HUD layer), `post_draws` (GS draws replaced by
  the native post passes), `packed_draws` / `packed_verts`;
- by kind (source + VU1 program, target, texture / render target, blend, depth, "full" = covers its scissor, "far" =
  at the far plane): `PS2X_HW_DRAW_SOURCES=1` (logged every 5 s).

First survey (2026-10-24: reference scenes, the world tour, 12 missions 6-94), per game frame:

| | reference gameplay | city (tour) | missions |
|---|---|---|---|
| GS-path draws | 59 (9.9k verts) | 106 mean, 231 max | 62 mean, 150 max |
| of them `hle` (C++ reproductions) | 45 | ~88 | ~47 |
| VU1 | 0 | 9 (clipped 0x02c0 shine) | 4.4 in PHI_A1 |
| PATH2 | 14 | ~15 | ~15 |
| PATH3 | 0 | 0 | 0 |
| native / HUD layer / post | 237 / 18 / 16 | | |

- The VU1 draws were 0x02c0 entry 0x0d (shine) packets that cross the guard band or are culled, or that use one of two
  more code images: reproduced since (game-internals.md 6d), so **no VU1 draws**; with the setup programs, the Z clear
  and the table-0x13 packets through the clipper native too, **no VU1 program runs in a normal frame**.
- PATH2 (VIF DIRECT, the game's 2D / effect packets): the reflection map built in FBP 420 (a 128x128 sample of FBP 140
  plus a sun sprite), coronas / light sprites, and the full-screen effect draws not taken by the native post passes
  (colour-filter quads, the depth-tested self-copy over the sky, fades).
- Found with it: every VU1 XGKICK had gone to the GS **twice** since the paraLLEl-GS integration (a brace-less `else`
  in the VU1 core; PS2RECOMP_PATCHES.md). Only the VU1 path was hit: with `PS2X_HLE=0` the cutscene drew 220,848
  vertices a frame instead of 122,823; blended VU1 draws were doubled. The native path (default) was unaffected.

**Packed draws (2026-10-24).** The C++ reproductions' strips and fans (clipper polygons, env pass, 0x02c0 re-draws and
shines) don't go through the GS front end: `GS::submitPackedKick` takes a single-tag PACKED ST / RGBAQ / XYZF2 packet,
decodes its vertices as `writeRegisterPacked` does and hands them to the HW backend at once (`SubmitPacked` ->
`recordPacked`), which records them with the same per-primitive code as `Submit` (`recordPrimitive`: flat colour,
render-target sampling, widescreen squeeze, merging) - not asked: the HUD classifier (3D packets are never HUD);
depth-buffer / target bookkeeping once per packet. Register packets (TEX0, TEXFLUSH) still take the front end; the GS
registers and vertex queue are left as the per-vertex path leaves them. `PS2X_HLE_PACKED=0` = all through the front
end.
- Checked with `PS2X_HW_PACKED_VERIFY=1` (each packet also recorded one `Submit` per triangle into a scratch frame,
  every vertex and its draw state compared): a city spot 4,180,000 packets, the reference scenes 5,300,000, PHI_A1
  4,580,000 - all exact (2 split into a different number of draw calls with the same vertices).
- At (1325, -325) GS-path draws went from ~224 to ~15 a frame; render worker ms: city 14.5 -> 11.7, AI drive 21.2 ->
  18.2 (41.3 -> 44.9 fps).

**PATH2 packets recorded whole (2026-10-24, docs/FULLY_NATIVE.md step 1).** The game's own 2D / effect packets take
the same route: `GS::processGIFPacket` hands a PATH2 packet to `submitPackedKick` first, which also takes
- Im2D's packet - one PACKED tag, ST / RGBAQ / **XYZ2** (`0x512`: a 32-bit Z, fog from the FOG register), the shape of
  every PATH2 packet that draws (game-internals.md 6f) - and
- A+D vertex runs: `PRIM`, then only RGBAQ / ST / UV / XYZ2 / XYZ3 writes (the effect strips); a packet with any other
  register goes to the front end untouched (nothing written before the check).
Q starts at 1.0 at the tag, as for every GIFtag; the registers and vertex queue are left as the per-vertex path leaves
them. PATH2 primitives are asked about by the **HUD classifier** with their GS vertices, as `Submit` does; and the
classifier is asked about PATH2 draws only, in both paths: a 3D triangle (PATH1) whose corners happened to land on HUD
vertex positions had been taken into the HUD layer and squeezed (found at 16:9 by the check below).
`PS2X_HW_PATH2_PACKED=0` = the front end.
- Checked with `PS2X_HW_PACKED_VERIFY=1`, which also compares whether each vertex went to the HUD layer: a city spot at
  noon and night, 4:3: 3.9M packets exact; 16:9 with "Screen edges" HUD: 5.14M exact.

**The last three (2026-10-26, FULLY_NATIVE.md 5e).** A+D runs that continue the current PRIM (queue vertices first,
without a kick), A+D packets with pure state writes before PRIM (written through the register path after the whole
packet is checked), and Im2D line strips / lines (`recordPacked` mode 1, as `Submit`) - checked with
`PS2X_HW_PACKED_VERIFY=1`, 7.5M packets exact. **`gs_draws` 0** in the reference scenes and the world tour.
`PS2X_HW_PATH2_CONTINUED=0` / `PS2X_HW_PATH2_LEAD=0` / `PS2X_HW_PATH2_LINES=0` = the front end;
`PS2X_HW_FRONT_DRAW_LOG=<n>` logs the draws the front end still makes, with their packets.

## The Vulkan backend

`src/rhi/vulkan/rhi_vulkan.cpp` implements rhi.h on Vulkan 1.3 (dynamic rendering, synchronization2, push descriptors;
volk + VMA); on the PS5 the device is RADV and the surface `VK_KHR_display` (docs/PS5.md). The behaviour the renderer
is written against:
- Y-up clip space through a negative viewport height (the shaders keep their convention); depth 0..1 and pixel centres
  at +0.5, so `gsPixelCentreOffset` holds.
- One push-descriptor set with the HLSL registers as bindings (tools/hlsl_spirv.py: bN -> N, tN -> 16 + N, sN -> 32 +
  N). Unbound slots and a texture that is also the pass's target read zeros.
- State stays bound across draws and passes; beginPass unbinds textures and resets viewport / scissor.
- Dynamic buffers: a new slice of the frame's upload ring per update (discarding the old contents), and their CPU copy
  goes into the ring again when a later frame draws with them unchanged. Static buffers / textures are updated by
  copies.
- Copies, uploads and readbacks inside a pass suspend its rendering and resume it with LOAD (`PS2X_RHI_STATS=1` counts
  them). Textures rest in SHADER_READ_ONLY (or their attachment layout) and return there after each operation.
- `depthClip=false` is depth clamp.
- New targets start as zeros; pipelines are made per (colour format, depth format, samples) at the first draw.
- Dear ImGui is drawn through the RHI (render/ui_renderer.cpp, shaders/ui.hlsl).
- With no display there is no swapchain (an off-screen back buffer).

Still to check on the console: validation clean, the in-pass suspension count, the picture against the reference
scenes (docs/PS5.md's checklist).

## Verification

- Reference scenes (a fixed run from an empty memory card: loading screen, flyover, intro cutscene, gameplay, the pause
  map) compared picture by picture: static scenes must match (tolerance 2.0; two runs of one build match exactly);
  moving scenes drift with timing between runs and only fail when black or missing.
- HLE handlers: per-program A/B (`PS2X_HLE` and its per-pipeline switches) and the bit-exact check against the VU1
  (`PS2X_HLE_VERIFY=1`); packed draws against the front end (`PS2X_HW_PACKED_VERIFY=1`); Gather against the four loads
  (`PS2X_HW_GATHER=verify`).
- `PS2X_HW_DUMP`: render targets and the display as PPM, by hand when a scene differs.

## Risks and open questions

- GS tricks the native path must respect: Z cleared by drawing CT32 strips into the depth buffer's memory (handled as
  `DepthFill`); framebuffers reused as textures for post effects; the PMODE two-circuit anti-flicker blend; 16-bit
  targets for effects. Keeping native draws inside the GS backend's frame list is what makes these keep working.
- Mesh cache invalidation under streaming: content-hashed, LRU-evicted.
- Programs that read back results or depend on VU1 side effects (none known for rendering; the survey checks
  XGKICK-free paths and MAC-flag use).

## Decisions

1. Own thin RHI instead of an existing library.
2. HLE hooks at the VU1 program start with a mesh cache, not in recompiled game code.
3. The emulated GS path stays as fallback and reference indefinitely.
