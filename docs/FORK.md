# This fork: enhancements from PC mods, done natively

A fork of [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) that brings the effects of PC mods
(Proper Shaders VC, SkyGFX, Project2DFX, texture packs) to the native port. Everything in upstream's AGENTS.md holds
here: no game code or data in git, authentic by default (every enhancement is a setting, and at its default the game
draws as on the PS2), a switch back to the reference for every hook, game knowledge written into
[game-internals.md](game-internals.md). [MODS.md](MODS.md) maps each mod effect to the game; this file is the work list.

- **Base**: upstream `main` at 0.2.0 (`728c8c8`). The fork's `main` is upstream plus the commits below.
- **Not in git**: `Mods/` (the PC mods, kept locally for reference), texture packs, the release-zip extras of a player
  install (`deps/`, `python/`, `RELEASE.json`).

## Changes so far

| Commit | What | Settings | Check |
|---|---|---|---|
| Exposure on the shown picture | A brightness pass in the presenter (`present.hlsl`, `presenter.cpp`) on the finished picture, HUD included. Interim: T6 replaces it with tone mapping before the HUD | Graphics > Image quality: `graphics.post_process` (off), `graphics.exposure` (100 %) | - |
| Fog: the game's own fog range as settings | `src/game/vcs_fog.cpp` wraps `FUN_002F6008(start, end)`, the RSL fog-range setter, and scales its arguments. The fog stays the game's: per vertex from the depth, in the time cycle's colour and weather, in every renderer path (VU1, CPU HLE, native shaders, clipper), at no cost | Graphics > Fog: `fog.distance` (10-100 %), `fog.start` (0-300 %); 100 % = the game's own values, untouched. `VCS_FOG_HOOK=0` = reference, `VCS_TRACE_FOG=1`, command `fogstate` | `tools/fog_check.py` (passes) |
| Fork documentation | This file, MODS.md, README's note, `.gitignore` for `Mods/` and texture packs | - | - |

What was learned on the way is in game-internals.md 6l (the fog setters, the camera's begin-update `FUN_0018EE48`,
camera +0x78 near / +0x7C far / +0x80 fog plane, FOGCOL from `0x4E1B30`; measured: far plane 2000 everywhere, fog start
~55 at noon, 22 at night, -28.6 in foggy weather) and DECISIONS.md 2026-11-02 (why not a screen-space overlay: a first
attempt painted a gradient over the picture by screen height and was removed).

## Work list

One task at a time, in this order. T3 is the base for T4-T7.

| # | Task | Depends on | Status |
|---|---|---|---|
| T1 | Texture replacement: PCSX2-compatible packs, **DDS** first-class, PNG kept | - | Next |
| T2 | Draw distance (Project2DFX): view distance, object / LOD distance, HD world further, population distance, distant lights | - | Planned (research first) |
| T3 | Pre-HUD post-processing pass (colour + depth before the 2D) | - | Planned |
| T4 | Volumetric height fog (Proper Shaders `[VolumetricFog]`) | T3 | Planned |
| T5 | SMAA 1x | T3 | Planned |
| T6 | Simulated HDR: tone mapping, exposure, bloom (replaces the interim Exposure) | T3 | Planned |
| T7 | Sun rays | T3 | Planned |
| T8 | Presets and player docs (Authentic / Enhanced), release notes | T1-T7 | Planned |

Backlog (not scheduled): SkyGFX's Xbox rim light / gloss / screen water drops, YCbCr correction, a larger reflection
map, shadow maps, deferred lights, PBR sky, volumetric clouds, car paint reflections, rain on cars, ragdolls.

### T1 - Texture replacement with DDS

**Goal.** Replacement packs in PCSX2's layout and naming, so existing packs and PCSX2's dumps work, with `.dds` as the
format of choice and `.png` still read.

**Facts.** The disc has no DDS: VCS's textures are GS formats inside `.XTX` / TXD data (83 % PSMT4 with a CT32 palette,
then CT32, T8, CT16; game-internals.md 6e and 7), decoded by the GS renderer's texture cache. DDS is still the right
*replacement* format: GPU-native block compression (BC1 / BC3 / BC7: 4-8x less memory than RGBA), mip levels stored in
the file, nothing to decode on load (PNG needs a CPU decode per image). PCSX2 reads `.dds` in replacement packs with the
same file names, so pack tools and conventions carry over. Upstream PR #8 (rofhima13, open) already does the PNG half:
PCSX2 hashes (XXH3-64 of the texels in GS block order + the expanded palette), an async loader, a GPU cache with a
budget, wrap / region / alpha kept, a Graphics toggle, `tools/texture_check.py`; it states "DDS ... not supported". It
is based on `9b9211d` (0.1.0); its first three commits are toolchain fixes that upstream may since have made its own.

**Steps.**
1. Bring PR #8 onto the fork (cherry-pick its commits with their author, drop what upstream already fixed), build,
   `texture_check.py --loader-only` and the in-game check green.
2. DDS reader (own code, no new library): `DDS_HEADER` + `DX10` extension; BC1-BC7 (UNORM / SRGB), RGBA8 / BGRA8,
   legacy FourCC DXT1 / DXT3 / DXT5 / ATI1 / ATI2; the mip chain; cube maps, arrays and volumes refused with a log line.
3. RHI: block-compressed formats and uploads of a whole mip chain, D3D11 and Vulkan (`src/rhi/`).
4. Lookup per texture identity: `.dds` before `.png`; the cache budget counts the file's real GPU size (known from the
   header, no decode).
5. Dump mode (`VCS_TEXTURE_DUMP=1`): the textures the game uses, written once each under PCSX2's names into
   `textures/SLUS-21590/dumps/` (PNG, as PCSX2 dumps; authors convert to DDS with texconv / Compressonator).
6. docs/TEXTURE_PACKS.md (DDS section, how to make a pack), `texture_check.py` with DDS fixtures it writes itself
   under `build/` (uncompressed and BC1 at least).

**Done when.** A pack of `.dds` files replaces textures in game on D3D11 and Vulkan builds; PNG packs still work;
replacements off = the original picture; the GPU budget holds; the check passes. Packs stay out of git.

### T2 - Draw distance (Project2DFX)

**Goal.** See the city much further with the game's own detailed models, with peds and traffic placed to match, as
settings (the game's distances at their defaults).

**Facts.** Project2DFX for VC (`VCLodLights.asi`) raises VC's far plane (`ViewDistance`), multiplies the LOD / object
distances (`DrawDistance` 1-5, `DetailDistance` 1-4) and adds distant lamp coronas from a table of VC's lamp positions;
the limit adjuster lifts VC's pools. VCS is built differently, so the mod's mechanics do not transfer: the world is a
streamed hex grid of cells (125 x 108.25 units) of one level's `WRLD` image + `.IMG`, the cell the streaming follows
comes from the camera (`FUN_0023C820` / `FUN_0023D740`, `FUN_0023c188`), each cell streams ~0.3-0.5 MB into
`0xCB7A80..0x112A660`; sectors are frustum-tested (`FUN_00240ED8`) and drawn by `FUN_002409C8`; entities are culled by
`CCamera::IsSphereVisible` against near / far at `0x4CD130` / `0x4CD12C`; the camera's far plane is 2000
(game-internals.md 5, 6b, 6l). The free camera saw the city at full detail 640 m from the player (12b): detail follows
the camera, not the player. The guest has the PS2's 32 MB.

**Steps.**
1. Research (rig, F12 draw log, the generated code): what VCS draws beyond the resident HD cells (low-detail geometry in
   the `WRLD` image?), how many cells are resident and how they are chosen, where the far plane and the entity / model
   draw distances come from (the "LOD / sprite scales 70 / FOV" readers of `0x487484`), the population and car
   spawn / removal radii (`CPopulation::Update` `FUN_00430148`, `CCarCtrl::RemoveDistantCars` `FUN_00433008`).
   Written into game-internals.md before code.
2. View distance: the far plane as a setting (the fog's end moves with it: the fog settings are relative to it).
3. Object distance: the entities' draw distances times a factor (Project2DFX's DrawDistance / DetailDistance).
4. HD world further: more detailed cells around the camera. Two routes, chosen from step 1: a larger streaming pool in
   guest memory (a runtime change past the PS2's 32 MB, full rebuild), or cells loaded and drawn by the host (the native
   RSL renderer) without guest memory.
5. Population: ped / car creation and removal distances follow the draw distance (no visible pop-in, same density).
6. Distant lights: coronas for street lamps from VCS's own 2D effects (VC's `.dat` is another map).

**Done when.** Settings for view / object distance (and HD radius if step 4 lands), 100 % = the game's own; screenshots
from fixed spots and `bench.py` numbers before / after in PERFORMANCE.md; `soak.py` hours at the maximum without hangs,
streaming stalls or leaks; peds and cars appear beyond sight.

### T3 - Pre-HUD post-processing pass

**Goal.** A native pass at the point of the frame where the 3D is done and the 2D has not started, with the frame's
colour and depth, the camera and the time cycle available - the base of T4-T7.

**Steps.** Find that point in the frame (game-internals.md 6a / 6g; `vcs_hud.cpp` sees the 2D at the RenderWare level;
the HW renderer's command stream); bind the frame (target 140) and the Z buffer (target 280), turn GS Z into view depth
with the camera's near / far (0.3 / 2000) and projection, rebuild world positions with the camera matrix (`0x520A80`)
and view window (`0x742AC0`); write the result back into the frame before the 2D. D3D11 and Vulkan; internal resolution
scale, 16:9, and MSAA (resolve, or effects off with MSAA - Proper Shaders does the latter).

**Done when.** A debug view (`VCS_POST_DEBUG=depth|height`) shows correct linear depth and world height at the bench
spots; HUD and menus untouched; off = the original picture; its cost measured.

### T4 - Volumetric height fog

Exponential height fog integrated along the view ray (no ray march first), density by weather (foggy weather thicker),
colour from the time cycle (`0x4E1B30`) so it meets the distance fog; Proper Shaders' `[VolumetricFog]` values
(`BaseHeight` 10, `HeightFalloff` 0.02, `MaxOpacity` 0.8...) as the starting point. Settings: on / off, density, height.
**Done when**: streets fill with fog in foggy weather and mornings, the HUD is untouched, < 0.5 ms at 1080p.

### T5 - SMAA 1x

SMAA's reference shaders and its AreaTex / SearchTex headers from iryoku/smaa (MIT; not the mod's files), three passes on
the pre-HUD picture, HLSL for D3D11 and SPIR-V through `tools/hlsl_spirv.py`. A choice next to MSAA in Graphics.
**Done when**: edges smoothed in screenshots, text and HUD sharp, cost measured.

### T6 - Simulated HDR

Tone mapping (an ACES fit or AgX), exposure, contrast / saturation, an optional bloom that does not double VCS's own
bloom / trails targets; replaces the interim Exposure (the setting moves). **Done when**: day / night / interior
screenshots, Authentic default = the original picture.

### T7 - Sun rays

The sun's screen position from the game (its corona / the time cycle's sun direction: to be found), a radial blur of
the sky (depth = far) masked by the depth. **Done when**: rays at sunrise / sunset, cut by buildings, none at night or
indoors.

### T8 - Presets and player docs

Graphics presets (Authentic = every default; Enhanced = the tasks' recommended values), the README's feature list,
release notes for a fork build.

## How the work is done

- Each task: a GitHub issue on the fork, a branch `task/T<n>-<name>` from the fork's `main`, commits that say why, a
  pull request into the fork's `main`; game knowledge into game-internals.md, choices into DECISIONS.md, switches into
  PROGRESS.md's table, a check tool when a behaviour can regress.
- Upstream: `git fetch upstream` and merge `upstream/main` into `main` (no rewriting of the fork's published history).
  A finished task can be offered upstream as a pull request from a branch on `upstream/main`.
- Before committing: `git status` shows no `Mods/`, packs, `generated/`, `build/`, disc images.
