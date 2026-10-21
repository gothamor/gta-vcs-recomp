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
