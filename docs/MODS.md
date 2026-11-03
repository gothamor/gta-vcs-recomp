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
| ASI loader, CLEO, ModLoader | Trainer (F5) and the rig's `scm` for scripts | Not ported (no PC executable) |

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
