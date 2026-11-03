# PC mods in `Mods/` and their native equivalents

`Mods/` (kept locally, ignored by git) holds **GTA Vice City (PC)** mods - not San Andreas, not PS2: `.asi` /
`d3d8.dll` / `rwd3d9.dll` plugins that hook `gta-vc.exe` and its Direct3D 8/9 RenderWare. None of them can load here:
vcs.exe is VCS (PS2) recompiled, drawing through its own D3D11 / Vulkan renderer, with RSL (Leeds' RenderWare Stories),
not PC RenderWare, and VCS's Vice City (1984) is a different map from VC's (1986). What can be ported is each mod's
**effect**, rebuilt natively as a setting (authentic by default, AGENTS.md). Never commit anything from `Mods/` (their
binaries, `.cso` shaders, textures, `.dat` files). The work list is [FORK.md](FORK.md).

| Mod | What it is |
|---|---|
| `Proper Shaders/` (Junior_Djjr, VC port by Jessica Natalia; version 07-30) | D3D9 proxy + ModLoader folder `Proper Shaders VC/`: per-pixel lit pipelines, shadows, SMAA, simulated HDR / tonemap / bloom, sun rays, PBR / simple sky, volumetric clouds, **volumetric height fog**, deferred lights, car reflections, rain on cars; `ProperShaders.ini` documents every knob |
| `SkyGFX/` (aap, 3.0b) | Makes PC III / VC look like the PS2 / Xbox: PS2 lighting, PS2 water, PS2 env-map blending, dual-pass alpha, Xbox (neo) rim / gloss / water-drop pipelines, YCbCr colour correction, Leeds radiosity |
| `Project2DFX/` (ThirteenAG; `VCLodLights.asi` + `.dat`, Open Limit Adjuster) | Draw distance for VC: `ViewDistance` (metres added to the far plane), `DrawDistance` (x LOD / object distance, 1-5), `DetailDistance` (x small objects and trees), `ShadowDistance`; distant lights (`DistantLights`, `DistantLightSize`, `DistantTraffic`, `Searchlights`) from a table of VC's lamp positions (`VCLodLights.dat`: model, colour, offset, size, distance, blink...). The limit adjuster lifts VC's pool limits (peds, vehicles, buildings, 2D effects, coronas...) |
| `Proper Ragdoll/` | Physics ragdolls for peds (gravity, joints measured on the game's animations, world collision, water), an in-game editor, impact sounds |

Texture replacement is not one of these mods: it comes from upstream PR #8 (PCSX2 PNG packs), extended here with DDS
(FORK.md T1).

## Map to vcs.exe

| Effect (mod) | In VCS Recomp | Status |
|---|---|---|
| Distance fog density / start (Proper Shaders' fog, foggy weathers) | The game's own GS fog: its range scaled at the setter (game-internals.md 6l, `src/game/vcs_fog.cpp`) | **Done** 2026-11-02 - Graphics > Fog; `tools/fog_check.py` |
| View distance, LOD / object draw distance, detail distance (Project2DFX) | VCS draws a streamed hex grid of world cells (game-internals.md 6b) under a far plane of 2000, entities against near / far at `0x4CD130` / `0x4CD12C`; what decides HD cell vs low detail and the entity distances is still to be read | T2 (research first) |
| Population and car spawn / removal distance (follows the draw distance) | `CPopulation::Update` `FUN_00430148`, `CCarCtrl::RemoveDistantCars` `FUN_00433008` (game-internals.md 12) | T2 |
| Distant lights (Project2DFX `VCLodLights`) | Coronas from VCS's own 2D effects (VC's `.dat` is the wrong map) | T2, last |
| Pool limits (Open Limit Adjuster) | Only if a larger draw distance hits VCS's pools; the guest has 32 MB | T2, when needed |
| Height / volumetric fog bank (Proper Shaders `[VolumetricFog]`) | Post pass before the HUD reading the Z buffer (target 280) + the camera (`0x520A80`) to rebuild world height | T4, on the pre-HUD pass (T3) |
| SMAA, simulated HDR / tonemap / bloom, YCbCr correction | Post pass on the 3D picture before the HUD | T5 / T6, on T3 |
| Sun rays | Post pass: sun position from the time cycle + depth occlusion | T7, on T3 |
| PS2 lighting, PS2 water, PS2 env map, dual-pass alpha, no backface culling, PS2 load screens (SkyGFX) | Already the case: the PS2's own renderer, reproduced | Nothing to do |
| Leeds radiosity / trails (SkyGFX `radiosity`, `trailsSwitch`) | VCS's own (the trails / bloom targets, game-internals.md 6g) | Already there |
| Larger reflection map (SkyGFX `envMapSize`) | The 128x128 reflection target (420, role set in `vcs_display.cpp`) | Backlog |
| Xbox rim light / gloss / screen water drops (SkyGFX neo) | Native shader variants in `vcs_hle_world.cpp` / `gs_draw.hlsl` | Backlog |
| Per-pixel lit pipelines, shadow maps, deferred lights, car paint reflections, rain on cars, PBR sky, volumetric clouds | The native renderer drawing the scene a second time (light view) and new shaders | Backlog; milestone-sized each |
| Ragdolls (Proper Ragdoll) | A physics engine driving the ped skeleton on death / falls, against VCS's collision | Backlog; gameplay, not render |
| ModLoader (`ModLoader/`: folders as mods, `modloader.ini`, an in-game menu, hot swapping) | The mod loader below: `modloader/`, the same ini, F2 > Mods, live | **Core done** 2026-11-02 (textures); game files: T1c |
| ModLoader's `.dff` / `.txd` replacement | VCS has neither format (below): its own files (`.mdl`, `.xtx` in the `.IMG` archives) through the mod loader; PC `.dff` / `.txd` only by conversion | T1c (VCS formats), T9 (PC formats, research) |
| ASI loader, CLEO | Trainer (F5) and the rig's `scm` for scripts | Not ported (no PC executable) |

## The mod loader

PC GTA's Mod Loader (LINK/2012; the copy in `Mods/ModLoader/`) made mods folders: nothing of the game's is replaced
on disc, everything is injected at run time, each mod can be switched off in a menu, and mods can be swapped while
the game runs. `src/mods/modloader.cpp` does the same for this port:

- **Where**: `modloader/` beside vcs.exe, or in the checkout's root when vcs.exe is in `build/` (`VCS_MODLOADER_DIR`
  elsewhere). Not `mods/`: on Windows that is the `Mods/` folder of PC mods kept for reference. `modloader/README.md`
  is the only file of it in git.
- **A mod** is any folder in it (a leading `.` hides one). `modloader/modloader.ini` is read in Mod Loader's own
  format, so a player's knowledge carries over: `[Folder.Config] Profile`, and the profile's `[...Priority]
  Mod=1..100` (50 by default, 0 ignores the mod; a higher one wins a file two mods have), `[...IgnoreMods]`
  (wildcards) and `[...Config] IgnoreAllMods`. Profiles beyond that (parents, `UseIfModule`) are not read.
- **F2 > Mods**: Load mods (`mods.enabled`), Replace textures (`graphics.texture_replacements`, which also governs the
  packs outside `modloader/`: `VCS_TEXTURE_PACK`, `graphics.texture_pack` in vcs.ini, `textures/SLUS-21590`), one
  switch per mod (`mods.<name>`, with its file count and priority as help), Rescan (files read again) and Open folder.
  A switch is applied at the next host frame: the sources are indexed again on the main thread (~20 ms for 6279 files
  here) and `gsHwSetTextureReplacements` hands the pack to the GS stage, which swaps it at its next frame (the old
  pack's GPU textures dropped; the game's own or the new pack's come back as they are drawn). A new mod folder shows
  after a restart (the settings registry is built at start-up).
- **What a mod can hold**: texture packs so far (PCSX2-named `.dds` / `.png` anywhere in it, `dumps/` aside;
  docs/TEXTURE_PACKS.md). Game files are next (T1c).
- **Test runs stay authentic**: a hidden run loads mods only with `VCS_MODS=1`, the other packs only with
  `VCS_TEXTURE_REPLACEMENTS=1`, or both with `VCS_TEST_MODS=1` (switches left free for the `setting` command).
- **Checked** (`tools/mods_check.py`, 2026-11-02): a `modloader/` of "HD" (the HD pack, linked), "Tinted" (half of a
  city spot's dumped textures painted magenta, priority 60), `_old` (ignored by the ini) and `.hidden`: 2 mods found,
  `_old` ignored, Tinted wins its 377 textures over HD; switched live with `setting` - everything on (magenta), Tinted
  off (no magenta, HD stays: 4.7 from the game's own picture), Replace textures off (the game's own), on again (HD
  back), Load mods off (0.7: walking peds) - 5 swaps, no failure.

### `.dff` / `.txd`: what PC mods replace, and what VCS has

PC GTA III / VC / SA mods are RenderWare 3 files: `.dff` (a clump: frames, geometry, materials, skin) and `.txd` (a
texture dictionary), loaded by the PC executable's RenderWare and streamed from `gta3.img`. **VCS on the PS2 has
neither**: its models are Leeds' RenderWare Stories (RSL) structures with PS2-native geometry (VU1 packets, game-
internals.md 6d), in `GTA3PS2.IMG` and the world's `BEACH / MAINLA / MALL.IMG` + `.LVZ`; its textures are `.XTX`
(GS formats, section 7). A PC `.dff` / `.txd` dropped into the game would not be read. Two different things can be
built:

1. **VCS's own files from a mod** (T1c) - what Mod Loader's `std.stream` does on PC, for VCS's formats: a file in a
   mod named as an archive entry (or a loose file: `MAIN.SCM`, the `.GXT` texts) is read in place of the disc's. The
   disc reads are native already (`src/game/vcs_stream.cpp`), so the bytes can come from the mod's file; what has to
   be found first is each archive's directory (entry names, offsets, sizes - where VCS keeps them: `GAME.DTZ`?) and
   what the game assumes about an entry's size (buffers sized from the directory). Mods made with VCS / LCS
   modding tools for the PS2 / PSP versions would then work.
2. **PC `.dff` / `.txd` themselves** (T9, research): either converted to VCS's formats ahead of time (RW 3 clump ->
   RSL structures with PS2 strips and VU1 data, TXD -> XTX), or loaded on the host and drawn by the native renderer in
   place of a model the game submits (needs draws at the RSL level: docs/RSL_SUBMISSION.md, milestone 7). Either is a
   project of its own; the texture half is already possible today as a texture pack (textures are matched by content,
   not by file).

## The pre-HUD pass (the infrastructure T4-T7 need)

Most of Proper Shaders' post effects need the finished 3D picture **before the HUD and menus** are drawn over it, plus
its depth. `present.hlsl` runs on the final picture (HUD included), so it can only do whole-picture colour (the
existing Exposure). The frame's structure (game-internals.md 6a / 6g) has a point where the world is done and 2D begins
(`vcs_hud.cpp` sees the 2D at the RenderWare level); a pass inserted there with the frame (target 140) and Z buffer
(target 280) bound is what volumetric fog, SMAA, tone mapping and sun rays build on.

## History

A first attempt took the mods for San Andreas ones, catalogued a pack that is not in `Mods/`, and drew fog as a
screen-height gradient over the finished picture; its four `MODS_*.md` files were never committed (DECISIONS.md
2026-11-02).
