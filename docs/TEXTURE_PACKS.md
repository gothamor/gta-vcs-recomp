# PCSX2 texture packs

Texture replacements are optional. In F2 > Graphics > Image quality, enable **Texture replacements**, then restart.
The original disc textures remain the default and the fallback for every missing or unreadable replacement.

Put an unpacked pack in one of these locations:

- `textures/SLUS-21590/replacements/` beside `vcs.exe`.
- `SLUS-21590/replacements/` beside `vcs.exe`.
- For the source checkout's default `build/vcs.exe`, `SLUS-21590/replacements/` in the checkout root also works.

Subdirectories under `replacements/` are scanned too. Keep the PCSX2 filenames. The loader supports full-texture
base-level PNG replacements, with or without a palette hash, including old filenames carrying the TCC flag.
Region-specific filenames, separate `-mipN` files, DDS and zipped packs are not supported. Mipmaps for anisotropic
filtering are generated from the replacement's base image. Images can have non-power-of-two dimensions and
different horizontal and vertical scale factors. PNG colours and alpha are used unchanged, as in PCSX2; the GS's
alpha convention (128 = opaque in blending) is preserved.

For a pack elsewhere, add these lines to `vcs.ini` beside the executable (relative paths are relative to that file):

```ini
graphics.texture_replacements=1
graphics.texture_pack=D:/Texture Packs/SLUS-21590
```

Or set `VCS_TEXTURE_REPLACEMENTS=1` and `VCS_TEXTURE_PACK=<pack directory>` for one run. The path may name either
`SLUS-21590` or its `replacements` directory. `VCS_TEXTURE_CACHE_MB` sets the replacement GPU cache budget
(default 512 MiB, range 64-4096). CPU images are decoded on one background thread, with at most 128 MiB waiting for
upload (a single larger image is allowed); the decoded image is released after queued frames finish using it.
An image referenced by multiple game textures has one GPU copy. Textures used in the current or two previous frames
are never evicted (30 fps has empty alternate VBlanks), so the active working set may exceed the GPU budget.
GPU render targets, reflections, bloom
and frame copies are not replaced.

The log's `[textures]` lines report the indexed file count, first matches, image loads, errors and the final number
of unique pack files matched. A partial pack changes only those textures it contains. The original game still
streams its own assets; replacements are host GPU textures and do not change the ISO, guest memory or saves.

## Implementation

`src/render/texture_replacements.{h,cpp}` indexes names, uses xxHash's XXH3-64, and decodes PNGs with stb_image.
`gs_hw.cpp` computes PCSX2 identities only on original texture-cache misses whose format/size appears in the pack.
Palette hashes use the currently loaded, expanded CLUT. Pixel hashes follow GS block order, with expanded texels
for sub-block textures and partial-word formats. Both a base-only hash and hashes including the game's mip levels
are tried. PCSX2's filename format bits are independent of the existing runtime cache keys.

The same texture binding path serves world, native HUD, menus and loading art. Ready images are inserted into the
ordered frame command stream; bind memos are invalidated at that boundary. For nearest/bilinear reads the existing
full-resolution target sampling path maps GS coordinates into replacement pixels, applying the GS wrap and region
limits to each tap. Plain anisotropic reads use normalized UVs and the GPU mip chain. Original sampling stays as
before when replacements are disabled or unmatched.

The naming/hash reference is PCSX2's `GSTextureReplacements.cpp` and `GSTextureCache.cpp`:
https://github.com/PCSX2/pcsx2/tree/master/pcsx2/GS/Renderers/HW.
Dependency versions/licenses: `third_party/texture-dependencies.md`. Texture packs stay external and are ignored by
git; no replacement image belongs in a source release.

## Checks

`python tools/texture_check.py build/texture_check/latest --validate-pack` checks the loader, decodes every supported
PNG in the pack, then runs a hidden five-location city check with screenshots and performance readings. Use
`--loader-only --validate-pack` to validate images without booting the game; `--pack PATH` selects another pack.
All fixtures and cards are under `build/`, never the player's card. For a cache-pressure check, set
`VCS_TEXTURE_CACHE_MB=64` and `PS2X_HW_ANISO=0`. The game check rejects repeated reloads of active textures.

Checked 2026-10-31 on Windows/D3D11: all 160 PNGs decoded, 25 pack files matched and loaded across Fort Baxter and
the four benchmark city spots, no load failures or black scene captures. The original loading/pause-map static
captures versus replacements disabled had mean absolute differences 0.051 / 0.128 (project tolerance 2.0).
Both anisotropic 16x sampling (the local settings) and the full-resolution nearest/bilinear path were exercised.
The small-cache run exposed eviction on empty alternate VBlanks; keeping two frames of history prevents that churn.
Linux/Vulkan uses the same loader and existing sampling shaders, but has not been run for this change.
