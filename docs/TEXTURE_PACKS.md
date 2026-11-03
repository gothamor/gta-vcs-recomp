# PCSX2 texture packs

Texture packs are mods: put a pack in a folder of its own inside `modloader/` (docs/MODS.md "The mod loader") and
switch it in **F2 > Mods** - each mod on or off, and **Replace textures** for all of them - while the game runs: a
change shows at the next frame. The original disc textures remain the fallback for every missing or unreadable
replacement, and what the game shows with Replace textures off. When two mods have the same texture, the higher
priority in `modloader/modloader.ini` wins.

A pack is also read from these places (upstream PR #8's; switched by Replace textures alone):

- `textures/SLUS-21590/replacements/` beside `vcs.exe`.
- For the source checkout's default `build/vcs.exe`, `textures/SLUS-21590/replacements/` in the checkout root.
- `SLUS-21590/replacements/` beside `vcs.exe`.
- For the source checkout's default `build/vcs.exe`, `SLUS-21590/replacements/` in the checkout root also works.

The `textures/` locations take priority over the older layout. An explicit pack path in the environment or
`vcs.ini` takes priority over automatic discovery. `tools/texture_check.py` defaults to the checkout's
`textures/SLUS-21590` folder.

Subdirectories under `replacements/` are scanned too. Keep the PCSX2 filenames. The loader supports full-texture
base-level replacements as **DDS** or PNG files, with or without a palette hash, including old filenames carrying
the TCC flag. When one name exists as both, the DDS is used. Region-specific filenames, separate `-mipN` files and
zipped packs are not supported. Colours and alpha are used unchanged, as in PCSX2; the GS's alpha convention (128 =
opaque in blending) is preserved.

**DDS** is the format to make packs in. The disc has no DDS - VCS's textures are the GS's own paletted formats - but a
DDS holds what the GPU samples: block-compressed (BC7 for quality, BC1 / BC3 smaller), its mip levels already made,
nothing to decode on load (a PNG is decoded on the CPU, and takes 4-8x the GPU memory). Read: the `DX10` header with
BC1, BC2, BC3, BC4, BC5, BC7 (UNORM or SRGB, both used as stored, like a PNG's values) or RGBA8 / BGRA8, and the legacy
header with DXT1, DXT2 / DXT3, DXT4 / DXT5, ATI1 / BC4U, ATI2 / BC5U or 32-bit RGBA / BGRA with alpha; one 2D texture
(arrays, cube maps and volumes are refused); 1-8192 texels a side, a multiple of 4 when block-compressed. The file's
mip levels are used as they are for anisotropic filtering (Graphics > Texture filtering) and level 0 alone without
it; a DDS without mips is sampled without them. A refused or truncated file is reported in the log and keeps the
original texture. On Vulkan block compression needs the GPU's `textureCompressionBC` (every desktop GPU has it); a
block-compressed DDS keeps the original texture without it. PNGs: mipmaps for anisotropic filtering are generated
from the base image; images can have non-power-of-two dimensions and different horizontal and vertical scale factors.

### Making a pack: the dump

`VCS_TEXTURE_DUMP=1` writes every texture the game decodes, once per name, as an uncompressed DDS (RGBA8 with the
GS's alpha) under its PCSX2 name into `textures/SLUS-21590/dumps/` (relative to the working folder; `VCS_TEXTURE_DUMP_DIR`
elsewhere). Edit or upscale a dump, convert it to BC7 (or BC1 / BC3) with mips - `texconv -f BC7_UNORM -m 0 file.dds`
(DirectXTex) or Compressonator - keep the file name, and put it in `replacements/`. Names already dumped in an earlier
run are not rewritten. The names are the base-level identities the loader looks up first; PCSX2's own dumps work the
same way.

For a pack elsewhere, add these lines to `vcs.ini` beside the executable (relative paths are relative to that file):

```ini
graphics.texture_replacements=1
graphics.texture_pack=D:/Texture Packs/SLUS-21590
```

Or set `VCS_TEXTURE_REPLACEMENTS=1` and `VCS_TEXTURE_PACK=<pack directory>` for one run. The path may name either
`SLUS-21590` or its `replacements` directory. `VCS_TEXTURE_CACHE_MB` sets the replacement GPU cache budget
(default 512 MiB, range 64-4096; a DDS counts its file's levels, a PNG its RGBA8 size and generated chain). Images
are read (DDS) or decoded (PNG) on one background thread, with at most 128 MiB waiting for upload (a single larger
image is allowed); the image is released after queued frames finish using it.
An image referenced by multiple game textures has one GPU copy. Textures used in the current or two previous frames
are never evicted (30 fps has empty alternate VBlanks), so the active working set may exceed the GPU budget.
GPU render targets, reflections, bloom
and frame copies are not replaced.

The log's `[textures]` lines report the indexed file count, first matches, image loads, errors and the final number
of unique pack files matched. A partial pack changes only those textures it contains. The original game still
streams its own assets; replacements are host GPU textures and do not change the ISO, guest memory or saves.

## Implementation

`src/render/texture_replacements.{h,cpp}` indexes names, uses xxHash's XXH3-64, reads DDS headers itself (the levels
are copied, never decoded) and decodes PNGs with stb_image. A DDS becomes one `createTextureLevels` of its own format
(`rhi::Format` BGRA8 / BC1-BC7, D3D11 and Vulkan; `rhi::Device::blockCompression()`).
`gs_hw.cpp` computes PCSX2 identities only on original texture-cache misses whose format/size appears in the pack
(and for every decoded texture with `VCS_TEXTURE_DUMP=1`: `replacementBits` / `replacementPalette` / the base level).
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

`python tools/texture_check.py build/texture_check/latest --validate-pack` checks the loader, loads every supported
DDS / PNG in the pack, then runs a hidden five-location city check with screenshots and performance readings. Use
`--loader-only --validate-pack` to validate images without booting the game; `--pack PATH` selects another pack.
The loader checks include DDS fixtures the tool writes itself: BC7 with mips, legacy DXT5, BGRA8 with mips, a
truncated file (its whole levels kept), a DDS winning over a PNG of the same name, and refused files (a cube map,
BC1 6x6, RGBA32F, BC7 on a device without block compression). `--dump-check` is the dump's round trip: the game
dumps a city spot's textures, each is painted magenta into a pack of its own, and the spot with that pack must
show them (with `--pack`, the dumped names are also looked up in that pack).
All fixtures and cards are under `build/`, never the player's card. For a cache-pressure check, set
`VCS_TEXTURE_CACHE_MB=64` and `PS2X_HW_ANISO=0`. The game check rejects repeated reloads of active textures.

Checked 2026-11-02 (this fork, Windows / D3D11) with an HD pack of 6279 BC7 DDS files (DX10 header, full mip chains,
851 MiB): `--validate-pack` loads all 6279 (~1 min); the game check matched and loaded 1095 of them across Fort Baxter
and the four city spots, each once (119 MiB of BC7, no reloads), at 30 fps; replacements off unchanged. The dump
round trip: 753 textures dumped at Vice Point, 753 / 753 back as replacements, and 742 of the 753 names are in the HD
pack (made with PCSX2): the identities are PCSX2's. Vulkan: `rhi_vulkan.cpp` compiles with the block formats; not run
(no Vulkan build on this PC - it needs glslangValidator).

Checked 2026-10-31 on Windows/D3D11: all 160 PNGs decoded, 25 pack files matched and loaded across Fort Baxter and
the four benchmark city spots, no load failures or black scene captures. The original loading/pause-map static
captures versus replacements disabled had mean absolute differences 0.051 / 0.128 (project tolerance 2.0).
Both anisotropic 16x sampling (the local settings) and the full-resolution nearest/bilinear path were exercised.
The small-cache run exposed eviction on empty alternate VBlanks; keeping two frames of history prevents that churn.
Linux/Vulkan uses the same loader and existing sampling shaders, but has not been run for this change.
