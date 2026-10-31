# Texture replacement dependencies

- `xxhash/xxhash.h`: xxHash 0.8.3, from
  https://github.com/Cyan4973/xxHash/blob/v0.8.3/xxhash.h (BSD 2-Clause; license in the header).
- `stb/stb_image.h`: stb_image 2.30, commit `f0569113c93ad095470c54bf34a17b36646bbbb5`, from
  https://github.com/nothings/stb/blob/f0569113c93ad095470c54bf34a17b36646bbbb5/stb_image.h
  (MIT / public domain; license in the header). Only its PNG decoder is built.

Both are vendored so an offline build needs no additional downloads. PCSX2-compatible texture identities follow
`GSTextureCache::HashCacheKey::Create`, `HashTextureLevel`, `PaletteKeyHash`, and
`GSTextureReplacements::CreateTextureName` / `ParseReplacementName` in the GPL-3.0+ PCSX2 project
(https://github.com/PCSX2/pcsx2/tree/master/pcsx2/GS/Renderers/HW). No game assets are included.
