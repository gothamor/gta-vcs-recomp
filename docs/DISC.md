# VCS PS2 disc layout (SLUS-21590, USA)

Facts read from the user's own ISO. Nothing here is game data; it's a map.

## Identity
- Volume ID `GTA_VCS`, publisher/preparer `ROCKSTAR GAMES`, created 2007-01-02.
- ISO size 4,149,182,464 bytes (2,025,968 sectors) but only ~2.16 GB of files.
  Files are spread out over the disc (big gaps between LBAs) - typical
  seek-time layout. Real LBAs matter: the game reads by sector.
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_215.90;1`, `VER = 1.00`, `VMODE = NTSC`.
- Executable `SLUS_215.90` at LBA 692, 3,993,368 bytes,
  SHA-1 `d1832508caf0990c2291c0f255c96a889cce9f88`, CRC32 `0x079b8be6`.

## Executable (ELF)
- ELF32 little-endian MIPS, flags `0x20924001` (noreorder, 5900, eabi64, mips3).
- Entry point `0x001DD6A0`. One LOAD segment `0x00100000`, filesz `0x3CD0A8`, memsz `0x64DF54`.
- **Stripped**: no symbol table. Section headers are present:

| Section | Addr | Size | Notes |
|---|---|---|---|
| .text | 0x00100000 | 0x380548 | ~920k EE instructions |
| .vutext | 0x00480550 | 0x0012D0 | VU microcode (4.8 KB) |
| .DVP.ovlytab / ovlystrtab / 4x .DVP.overlay | - | 0x7A8, 0x20, 0xC0, 0xA0 | DVP (VU) overlays, not loaded by the LOAD segment |
| .data | 0x00481880 | 0x010778 | |
| .vudata | 0x00492000 | 0x006F90 | VU data / DMA packets |
| .rodata | 0x00499000 | 0x0340A8 | |
| .sbss | 0x004CD100 | 0x000598 | |
| .bss | 0x004CD700 | 0x280854 | 2.5 MB |

Ends at `0x0074DF54`; the rest of 32 MB is heap.

## Strings of interest in the ELF
- Leftover dev paths not on the retail disc: `cdrom0:/RUNDATA/...`, `cdrom0:/LUA/*.lua.lc`
  (deathmatch, multirace, capturetheflag, tank, ... - the PSP multiplayer modes,
  compiled Lua). The PS2 version has no multiplayer; these look like dead code.
- `IOPRP300.IMG` (IOP replacement image) is loaded at boot.
- Movies are referenced as `cdrom0:\MOVIES\INTRO.PSS;1` and `STINGS.PSS`.
  `INTROP.PSS` / `STINGSP.PSS` are probably the PAL (50 Hz) variants.

## File groups
| Folder | Files | Size | What |
|---|---|---|---|
| `AUDIO/` | 230 | 1362 MiB | `.VB` streams (cutscene audio, radio, SFX banks) |
| root | 17 | 586 MiB | `BEACH/MAINLA/MALL.IMG` + `.LVZ` (world, split by area), `GTA3PS2.IMG` (common models), `MAIN.SCM` (mission script), `*.GXT` (text), `EMPHUD.XTX` |
| `MOVIES/` | 4 | 72 MiB | `.PSS` (MPEG-2 + audio, IPU-decoded on PS2) |
| `PS2/` | 3 | 33 MiB | `GAME.DTZ` (compressed game data, likely zlib), `MOCAPPS2.DIR/IMG` (cutscene anims) |
| `SPLASH/` | 29 | 5 MiB | `.XTX` loading screens / legal screens |
| `FRONTEND/` | 48 | <1 MiB | Menu layouts with **`.PSP` extension** - the PS2 port reuses PSP frontend data. `*W.PSP` variants = widescreen layouts |
| `MODULES/` | 9 | <1 MiB | IOP: `CDSTREAM.IRX` (Rockstar streaming), `CDVDSTM`, `LIBSD`, `SDRDRV`, `PADMAN`, `MCMAN`, `MCSERV`, `SIO2MAN`, `IOPRP300.IMG` |
| `ICONS/` | 1 | | memory card icon |

Use `python tools/vcs_disc.py ls <iso>` for the full list with LBAs.

## CHD images

The game reads the disc from an `.iso` or from a `.chd` (MAME's compressed disc image), the form many players keep
their discs in, without a conversion. `src/disc/disc_source.cpp` reads the sectors through libchdr (v0.3.0, BSD-3,
pinned in `tools/deps.py`); `tools/chd.py` reads them in Python (standard library) for `vcs_disc.py` and the
recompile.

- **Two kinds.** `chdman createdvd` makes DVD CHDs (2048-byte sectors, metadata tag `DVD `), by default with the
  hard-disk codecs `lzma` / `zlib` / `huff` / `flac` (MAME `chdman.cpp`: `s_default_hd_compression`). Many PS2
  CHDs were made with `chdman createcd` instead: one `TYPE:MODE1` track (`CHT2` tag), 2448-byte frames (2048 bytes
  of data, then padding and subcode), codecs `cdlz` / `cdzl` / `cdfl`. The user's disc is one of these (2,025,968
  frames, hunks of 8 frames: 117,922 `cdlz`, 9,533 `cdzl`, 1 `cdfl` and 125,790 `self` hunks). Both kinds read, in
  the game and in `tools/chd.py`. A stored pregap (`PGTYPE:V...`) is refused: PS2 discs have none, and where sector 0
  then sits is not checked against chdman.
- **The launcher takes `.iso` only** (it has no third-party code). For a `.chd` it gives the `chdman` command by the
  CHD's kind: `extractcd` for a CD-mode CHD, `extractdvd` for a DVD CHD (`launcher/model/disc_check.cpp`;
  `tools/vcs_disc.py` the same). For a `MODE1` track (2048 data bytes per sector, as in the user's disc) the CUE's
  `.bin` is then the ISO. A `MODE1_RAW` / `MODE2_RAW` track comes out as 2352-byte sectors, which is no ISO; the
  advice cannot tell, because it reads only the CHD's first metadata tag, not the track type.
- **The reader.** `IsoImage` reads through a `DiscSource`: the mapped `.iso` as before, or the `.chd` with a cache of
  decoded hunks (~16 MB). libchdr sits in the `vcs_disc` library of its own, so its include folder reaches no other
  compile command. The launcher compiles the same file without CHD support. When a read fails, the emulated IOP reads
  the image file itself as raw sectors (`iop_cdvd.cpp` `readSectors`); the game gives it the path of an `.iso` only,
  never a `.chd`'s compressed bytes (the `[disc] raw fallback:` line at the start; `VCS_SELFTEST` compares the
  file's sector 16, the volume descriptor, with the disc's).
  Without one, that path goes on to upstream's pseudo-ISO of the `cdrom0:` folder (`buildVirtualIso`), as a read past
  the end of an `.iso` always did: a damaged `.chd` hunk can then come back as that pseudo-ISO's bytes on the
  emulated streaming path (`VCS_NATIVE_STREAM=0`, not the default) - left as it is, since it is upstream's own path.
  Without an argument or `iso=` in vcs.ini, the game searches the exe's folder, then the current one; in each, an
  `.iso` comes before a `.chd` (`VCS_DISC_FIND=<folder>` prints the choice for one folder). So a `.chd` next to
  vcs.exe comes before an `.iso` in the current folder.
- **Parallel decoding.** The hunks of one read decode on several decoders at once: libchdr handles on the same file,
  each reading at its own position (`pread`, or `ReadFile` with an offset). The reading thread decodes with the first;
  `VCS_CHD_DECODERS` - 1 worker threads hold the rest (default: half the cores, at most 4; 1 is the single-threaded
  reader, the reference). A decoded hunk is shared by the cache and every read that waits for it, so an eviction never
  takes a hunk from a read that still needs it.
- **What a .chd costs** (Windows, i5-10210U, `VCS_DISC_TIME=1`, from the boot through the title flyover, the movies
  skipped, one decoder): the game reads ~23 MB while the loading screen is up, then 0.3-2 MB every few seconds. The
  boot's reads took 26 ms from the `.iso` and 1.4 s from the `.chd`. With 4 decoders, a compare of the whole disc took
  121 s instead of 212 s (a quick build).
- **Checks.** `VCS_DISC_COMPARE=<image>` compares every sector with another image, then reads the other image again
  from 4 threads at scattered ranges, before the game starts. `tools/tests/test_disc_backends.py` runs it on DVD and
  CD-mode CHDs from a test-only writer (`tools/tests/chd_build.py`): 1 and 4 decoders, a cache of 3 hunks, a damaged
  hunk. With `VCS_TEST_PLAYER_DISC=1` it compares the player's whole CHD with its ISO.
