# This fork: enhancements from PC mods, done natively

A fork of [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) that brings the effects of PC mods
(Proper Shaders VC, SkyGFX, Project2DFX, texture packs) to the native port. Everything in upstream's AGENTS.md holds
here: no game code or data in git, authentic by default (every enhancement is a setting, and at its default the game
draws as on the PS2), a switch back to the reference for every hook, game knowledge written into
[game-internals.md](game-internals.md). [MODS.md](MODS.md) maps each mod effect to the game; this file is the work list.

- **Base**: upstream `main` at 0.2.0 (`728c8c8`). The fork's `main` is upstream plus the commits below.
- **Not in git**: `Mods/` (the PC mods, kept locally for reference), the player's mods in `modloader/` (all but its
  README; `modloader.ini` too), texture packs, the release-zip extras of a player install (`deps/`, `python/`,
  `RELEASE.json`).

**Where it stands (2026-11-02)**: the game's fog as a setting; texture packs in DDS (BC7) or PNG with PCSX2 names; a
mod loader in PC Mod Loader's style - `modloader/` folders as mods, `modloader.ini`, F2 > Mods with a switch and a
priority slider per mod, everything applied while the game runs. Next: T1c (game files from mods) or T2 (draw distance).

## Changes so far

| Commit | What | Settings | Check |
|---|---|---|---|
| Exposure on the shown picture | A brightness pass in the presenter (`present.hlsl`, `presenter.cpp`) on the finished picture, HUD included. Interim: T6 replaces it with tone mapping before the HUD | Graphics > Image quality: `graphics.post_process` (off), `graphics.exposure` (100 %) | - |
| Fog: the game's own fog range as settings | `src/game/vcs_fog.cpp` wraps `FUN_002F6008(start, end)`, the RSL fog-range setter, and scales its arguments. The fog stays the game's: per vertex from the depth, in the time cycle's colour and weather, in every renderer path (VU1, CPU HLE, native shaders, clipper), at no cost | Graphics > Fog: `fog.distance` (10-100 %), `fog.start` (0-300 %); 100 % = the game's own values, untouched. `VCS_FOG_HOOK=0` = reference, `VCS_TRACE_FOG=1`, command `fogstate` | `tools/fog_check.py` (passes) |
| Fork documentation | This file, MODS.md, README's note, `.gitignore` for `Mods/` and texture packs | - | - |
| Texture packs (T1): upstream PR #8 + DDS | PR #8's PCSX2 PNG packs (its commits and author, merged onto 0.2.0's helper-thread textures), then DDS read as stored - BC1-BC5 / BC7, RGBA8 / BGRA8, the file's mips - through new block formats in the RHI (D3D11, Vulkan), a DDS winning over a PNG of the same name, and `VCS_TEXTURE_DUMP` to make packs (docs/TEXTURE_PACKS.md) | `graphics.texture_pack` (vcs.ini); `VCS_TEXTURE_PACK`, `VCS_TEXTURE_DUMP=1` | `tools/texture_check.py` loader / `--validate-pack` / game / `--dump-check` (all pass) |
| Texture packs switched live; sources with priorities | `gsHwSetTextureReplacements`: the GS stage swaps the pack at its next frame (the old pack's GPU textures dropped, the game's or the new pack's back as they are drawn); `TextureReplacements` indexes several folders, a name going to the highest priority | - | `tools/mods_check.py` |
| Mod loader (T1b) | `src/mods/modloader.cpp`: `modloader/<mod>/` folders as mods, Mod Loader's `modloader.ini` (made by the game in `modloader/`; priorities 1-100, ignore lists), F2 > Mods, live; test runs authentic. Settings registry: `Spec::saved = false`, a Toggle's `partner` slider; `platform::openFolder` | F2 > Mods: `mods.enabled`, `graphics.texture_replacements` (now live, on), `mods.<name>` + its priority slider (in modloader.ini), Rescan, Open folder; `VCS_MODLOADER_DIR`, `VCS_MODS`, `VCS_TEST_MODS` | `tools/mods_check.py` (passes) |

What was learned on the way is in game-internals.md 6l (the fog setters, the camera's begin-update `FUN_0018EE48`,
camera +0x78 near / +0x7C far / +0x80 fog plane, FOGCOL from `0x4E1B30`; measured: far plane 2000 everywhere, fog start
~55 at noon, 22 at night, -28.6 in foggy weather) and DECISIONS.md 2026-11-02 (why not a screen-space overlay: a first
attempt painted a gradient over the picture by screen height and was removed).

## Work list

One task at a time, in this order. T3 is the base for T4-T7.

| # | Task | Depends on | Status | Issue |
|---|---|---|---|---|
| T1 | Texture replacement: PCSX2-compatible packs, **DDS** first-class, PNG kept | - | **Done** (Vulkan compiled, not run) | [#1](https://github.com/willzmkv/gta-vcs-recomp/issues/1) |
| T1b | Mod loader (PC Mod Loader's way): `modloader/` folders as mods, `modloader.ini`, F2 > Mods switched live with priority sliders; texture packs as its first content | T1 | **Done** | [#10](https://github.com/willzmkv/gta-vcs-recomp/issues/10) |
| T1c | Mod loader: VCS's own game files from mods (archive entries `.mdl` / `.xtx` / ..., `MAIN.SCM`, `.GXT`) - the `std.stream` of PC Mod Loader | T1b | Planned (research first) | [#11](https://github.com/willzmkv/gta-vcs-recomp/issues/11) |
| T2 | Draw distance (Project2DFX): view distance, object / LOD distance, HD world further, population distance, distant lights | - | Next (research first) | [#2](https://github.com/willzmkv/gta-vcs-recomp/issues/2) |
| T3 | Pre-HUD post-processing pass (colour + depth before the 2D) | - | Planned | [#3](https://github.com/willzmkv/gta-vcs-recomp/issues/3) |
| T4 | Volumetric height fog (Proper Shaders `[VolumetricFog]`) | T3 | Planned | [#4](https://github.com/willzmkv/gta-vcs-recomp/issues/4) |
| T5 | SMAA 1x | T3 | Planned | [#5](https://github.com/willzmkv/gta-vcs-recomp/issues/5) |
| T6 | Simulated HDR: tone mapping, exposure, bloom (replaces the interim Exposure) | T3 | Planned | [#6](https://github.com/willzmkv/gta-vcs-recomp/issues/6) |
| T7 | Sun rays | T3 | Planned | [#7](https://github.com/willzmkv/gta-vcs-recomp/issues/7) |
| T8 | Presets and player docs (Authentic / Enhanced), release notes | T1-T7 | Planned | [#8](https://github.com/willzmkv/gta-vcs-recomp/issues/8) |
| T9 | PC `.dff` / `.txd` in mods: convert RW 3 clumps / TXDs to VCS's formats, or draw them natively (research) | T1c, RSL submission | Research | [#12](https://github.com/willzmkv/gta-vcs-recomp/issues/12) |

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

**Result (2026-11-02).** Steps 1-6 done. With the user's HD pack (6279 BC7 DDS, full mips, 851 MiB): all 6279 load;
1095 matched and shown across Fort Baxter and four city spots, each loaded once (119 MiB), 30 fps held; the dump round
trip brings back 753 / 753 dumped textures, and 742 of those names are in the HD pack (PCSX2's identities). PNG packs:
PR #8's loader checks still pass. Open: running the Vulkan backend with a pack (it compiles; this PC has no
glslangValidator for a Vulkan build) - on Linux, `tools/texture_check.py --pack DIR`.

### T1b - Mod loader

**Done (core, 2026-11-02)**: `src/mods/modloader.cpp`, docs/MODS.md "The mod loader". `modloader/<mod>/` folders,
Mod Loader's `modloader.ini` (made by the game in `modloader/`; priorities, ignore lists), F2 > Mods (Load mods,
Replace textures, a switch and a priority slider 1-100 per mod - the slider writes modloader.ini -, Rescan, Open
folder), switches applied while the game runs (`gsHwSetTextureReplacements`), test runs authentic.
Checked by `tools/mods_check.py`. Content read so far: texture packs.

### T1c - Game files from mods (VCS's formats)

**Goal.** A file in a mod named as one of the game's - an entry of `GTA3PS2.IMG` / the world's `.IMG` (models `.mdl`,
textures `.xtx`, ...) or a loose file (`MAIN.SCM`, `.GXT`) - is read in place of the disc's, as PC Mod Loader's
`std.stream` / `std.scm` / `std.text` do. **Not** PC `.dff` / `.txd` (VCS has neither; T9).

**Steps.** Research first: the archives' directories (names, offsets, sizes; where VCS keeps them), how the native
streaming (`src/game/vcs_stream.cpp`) reads an entry and what the game assumes about its size; then an override table
built from the enabled mods (priority as for textures), served by the stream layer; loose files at their open. A
check: a mod replacing one model / one text, seen in game.

### T9 - PC `.dff` / `.txd` (research)

Whether, and how, PC RenderWare 3 models and texture dictionaries can stand in for VCS's: converted ahead of time to
RSL structures with PS2 geometry (and XTX), or loaded on the host and drawn by the native renderer in place of a
model the game submits (draws at the RSL level, docs/RSL_SUBMISSION.md). Outcome: a written answer with a prototype or
the reasons against. (A `.txd`'s textures can already be used today as a texture pack.)

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

## Progress log

- **2026-11-02 - fork set up.** Rebased onto upstream 0.2.0; the screen-space "fog" of a first attempt replaced by
  the game's own fog range as settings (game-internals.md 6l); docs and the work list (this file, MODS.md); issues
  #1-#8; the launcher's release update off for a clone (no `RELEASE.json`).
- **2026-11-02 - T1, texture packs.** Upstream PR #8 (PNG packs) onto 0.2.0 with its author; DDS (BC1-BC7, RGBA8 /
  BGRA8, the file's mips) through block formats in the RHI; the texture dump. With the user's HD pack (6279 BC7): all
  load, 1095 shown in a city tour, each once (119 MiB), 30 fps; dump round trip 753 / 753, 742 names in the pack.
- **2026-11-02 - T1b, the mod loader.** `modloader/` folders as mods, `modloader.ini` in Mod Loader's format, F2 > Mods
  (switches, priority sliders, Rescan, Open folder), all applied while the game runs. With the user's two mods: HUD
  Remastered (160 PNG, 152 shared with the HD pack) supplies 8 textures at 50 / 50 and all 160 at 60. PC `.dff` / `.txd`
  analysed: VCS has neither format; its own files from mods are T1c, PC formats research (T9).

## How the work is done

- Each task: a GitHub issue on the fork, a branch `task/T<n>-<name>` from the fork's `main`, commits that say why, a
  pull request into the fork's `main`; game knowledge into game-internals.md, choices into DECISIONS.md, switches into
  PROGRESS.md's table, a check tool when a behaviour can regress.
- Upstream: `git fetch upstream` and merge `upstream/main` into `main` (no rewriting of the fork's published history).
  Never the launcher's "Update" for a fork checkout: it replaces the release's files with upstream's zip. A player
  folder turned into a clone must lose its `RELEASE.json` (the release marker `tools/update.py` reads); without it the
  launcher says "a git clone: update it with git pull".
  A finished task can be offered upstream as a pull request from a branch on `upstream/main`.
- Before committing: `git status` shows no `Mods/`, packs, `generated/`, `build/`, disc images.
