# VCS engine notes

Running notes on how GTA: Vice City Stories (PS2) works internally, gathered
while recompiling it. Addresses are for SLUS_215.90 (see `docs/DISC.md`).
Names in `CamelCase::Like()` are our labels, modelled on the GTA III-era
naming (re3/reVC) where the code is clearly the same lineage; they're guesses
until marked **confirmed**.

## Background (from outside sources)
- Rockstar Leeds engine, shared with the PSP original (2006). The PS2 port (2007)
  reuses PSP-side data formats (see `.PSP` frontend files).
- Descends from the GTA III / VC codebase but renders through Leeds' own
  "RenderWare Stories Lite"-style layer, not stock RenderWare.
- The PS2 version runs at 30 fps with a frame limiter. GTA III-era game code
  scales most movement by `CTimer::ms_fTimeStep` (1.0 = 1/50 s), so 60 fps mostly
  works, but earlier games had known 60 fps bugs (physics, some anims,
  script timers, water cannon). PSP recomp (OverkillLabs3) and ThirteenAG's
  PPSSPP fix both patch these for 60 fps - use as references.
- The built-in 16:9 option is anamorphic Vert- (crops top/bottom).

## Code map (SLUS_215.90)
- 9,550 functions (Ghidra 12.1.4 + EE extension). 8,521 below `0x443000`
  are game code; 1,029 from `0x443000` up are Sony SDK / libc / libgcc.
- SDK libraries identified by ps2_analyzer's SCE symbol DB (see `game/vcs/vcs.toml`):
  - `0x4431F8..` libgcc (`__divdi3`, `__muldi3`, soft-double `dpadd`...)
  - `0x447670..` libgraph (`sceGsResetGraph`, `sceGsSwapDBuff`, `sceGsSyncV`)
  - `0x448688..` libdma (`sceDmaSend`...)
  - `0x448D98..` libpad (`scePadRead`... - libpad, not libpad2)
  - `0x44E5F8..` libcdvd incl. **streaming** `sceCdStInit/StStart/StRead/StStop`
  - `0x44F668..` libmc (`sceMcOpen/Read/Write/GetDir`...)
  - `0x452320..` libmpeg (`sceMpegCreate`, `sceMpegDemuxPssRing`) + libipu
  - `0x45C1D8..` libsdr (`sceSdRemoteInit`, `sceSdRemote`) - audio goes over SIF RPC to SDRDRV.IRX
  - `0x467xxx` kernel syscall wrappers, `0x469E00..` libsif / RPC, `0x46B980..` libfileio (`sceOpen/sceRead`)
  - `0x46EA98..` Deci2 (dev-kit debug channel; dead on retail)
- `fiprintf` at `0x1A9EF0` lives inside game code (newlib linked in early).
- Largest game functions (likely big state machines / script interpreter):
  `0x141FE8` (41.7 KB), `0x13A0A0` (25 KB), `0x337208` (25 KB), `0x33F010` (20 KB).
- Analyzer found only 2 jump tables; the recompiler reports ~1,150 unresolved
  `jr` sites (switch tables) and falls back to dispatch through the function table.
  Performance TODO: emit real C++ switches for these from Ghidra's switch analysis.

## Boot
- Entry `0x001DD6A0` is standard crt0: zero GPRs/FPRs, clear .bss
  (`0x4CD100..0x74DF54`), set up gp/sp, then the SDK init and `main`.
- `sceSifInitRpc`, then IOP modules are loaded from the disc in this order:
  `SIO2MAN, LIBSD, SDRDRV, CDVDSTM, CDSTREAM, PADMAN, SIO2MAN (again), MCMAN, MCSERV`.
  All nine run fine as real IRX code on the emulated IOP (SDRDRV prints
  "SDR driver version 4.0.1"). `SDRDRV`, `CDVDSTM`, `MCMAN`, `MCSERV` stay resident (start=2).
- `_lf_bind` (`0x46CD10`) polls for the ROM LOADFILE server - not present in the
  recomp, stubbed to succeed.
- **Disc indexing**: `FUN_002209F0(path)` walks `cdrom0:` recursively with
  `sceDopen/sceDread/sceDclose`, and for each file (`st_mode & 0xF000 == 0x2000`)
  calls `sceCdSearchFile`-like `0x44E480` and appends (name ptr, lsn, size) to a
  table at `0x5038A8` (count at `0x486E8C`, string pool cursor at `0x486E88`).
  So the game resolves every file to an LSN up front and then reads by sector.

## Sections still to fill
- Main loop and frame limiter
- Timer / time step
- Camera and projection (FOV, aspect)
- HUD / 2D drawing and aspect handling
- Renderer: DMA chains, VU1 microprograms (`.vutext`), GS setup
- Streaming: CDSTREAM.IRX protocol, IMG archives, LVZ
- Audio: SDRDRV/LIBSD, VB streams
- Threads and their priorities

## VU microcode (confirmed from the ELF)
All VU microcode is in `.vutext` as four VIF `MPG` packets (DVP overlays):

| EE addr of MPG | VU code addr | Size (instr pairs) | DVP overlay |
|---|---|---|---|
| `0x48055C` | `0x000` | 245 | `.DVP.overlay..0x0.3273603939.50.0` |
| `0x4810EC` | `0x400` | 4 | `..0x400.3422538243.196.0` |
| `0x48146C` | `0x000` | 24 | `..0x0.3422538243.316.1` |
| `0x48176C` | `0x200` | 20 | `..0x200.3422538243.453.2` |

No other MPG packets in `.text/.data/.rodata/.vudata` (scan with VIF cmd 0x4A).
`.vudata` (28 KB at `0x492000`) holds no microcode - probably VIF/GIF packet templates.

**Correction after disassembly** (`build/analysis/vu1_prog0.txt`, not committed - made with hctp-recomp's
`vu1dis.py`): program 0 does *not* look like a renderer. It has E-bit exits at
`0x000` and `0x010` (entry stubs that set `vi1` and stop), converts fixed-point
inputs with `ITOF0` and `MULi 1/128`, builds edge vectors and runs
`CLIP`/`FMAND`/`FCAND`/`DIV` tests - i.e. a VU0 macro/micro helper, most likely
collision (line/sphere vs triangle). The tiny programs 1-3 are helpers too.
**So the VU1 rendering microcode is not in the ELF as static MPG packets.**
Candidates: `PS2/GAME.DTZ` (2 MB, compressed game data), or code sent at run
time. To confirm: log VIF1 MPG uploads at run time (Phase 3).

## GS usage (VCS_GS_STATS, title/intro scene)
- Primitives: ~95% triangle strips; also triangles, fans, sprites, points.
- Targets: CT32 colour (some CT16 for effects); Z24 depth (some Z32), test GEQUAL.
- Blend: 84% `(Cs-Cd)*As+Cd`; also `(Cs-Cd)*FIX+Cd`, `Cs*FIX+Cd` (FIX up to 255),
  `Cs*FIX`, `Cd-Cs*FIX`.
- Alpha test GREATER (+KEEP) and GEQUAL with AFAIL=FB_ONLY; no DATE.
- Textures: 83% PSMT4 + CT32 CLUT (world), CT32, T8, CT16; modulate/decal;
  repeat/clamp/region clamp; bilinear. Fog on most world geometry.
  CT32/CT16 textures inside render targets = post effects (framebuffer reuse).
- COLCLAMP on, no dithering, no FBA, no PABE.
- Display: PMODE 0x8027 = both circuits on the same buffer one line apart,
  blended (anti-flicker); DISPFB FBP 0, FBW 10 (640), DBY 2 / 1; 640x445.

## Data loaded at boot
- `PS2/GAME.DTZ` = zlib -> 6.6 MB `GTAG` v1 memory image (header: size, then
  table offsets), loaded at 0x016FE800 and relocated by FUN_00324A50; main
  copies ~50 global pointers out of it. VU1 microcode (~14 KB) is uploaded from
  data at run time, not from the ELF.

## Textures: `.XTX` (e.g. `LOADSC0..11.XTX`, the 12 loading screens)
Confirmed from `LOADSC*.XTX` (263,372 bytes each, 512x512 8-bit):
- `0x00` magic `"xet\0"`; `0x08` file size; `0x0C`/`0x10` offset of the trailing
  relocation table (0x404B0); `0x60` name (`"loadsc0"`).
- Pixels at **0xB0**: 512x512 linear PSMT8 indices (no swizzle).
- CLUT at **0x400B0**: 256 x RGBA8888, alpha 0x80 = opaque (PS2 range), already in
  **CSM1 order** (index bits 3 and 4 swapped), i.e. ready to upload as a 16x16 CT32 block.
- Tail: relocation table (offsets of pointers inside the header).
- The game uploads both unchanged: CLUT to CBP 13584 (16x16 CT32), then the
  pixels to TBP 8960 (TBW 8, T8). Upload path: VIF1 chain, one DIRECT with
  the BITBLTBUF/TRXPOS/TRXREG/TRXDIR + IMAGE GIFtag, then a REF DMAtag whose
  upper half is `NOP; DIRECTHL n` carrying the data. (This split exposed the
  upstream VIF bug in `docs/PS2RECOMP_PATCHES.md`.)
- The legal screen (copyright text + logo) is also a 512x512 T8 upload.

## Sound system (EE side + CDSTREAM.IRX)
- CDSTREAM.IRX is Rockstar's IOP driver for **both** disc streaming and sound.
  It registers three SIF RPC servers (IOP threads):
  - `0x65686577` - stream reads. EE client `0x505B48`: fn 1 = read
    `{channel, EE dest, LSN, -bytes}` (size is negated), fn 2 = poll
    (`0xFA` busy, `0` done). The first request is GAME.DTZ (0x217E70 bytes).
  - `0x65686578` - EE client `0x4CE2C0`, bound by `FUN_0010DCE8`.
  - `0x65686579` - EE client `0x4D3D40`, bound by `FUN_0010E918`. Commands go
    through `FUN_0010E970(cmd, arg)`: send/receive buffer `0x4CE300`, the reply's
    first word is the result. Seen: `0xA0000` (count, 0x6D8 = 1752 sounds),
    `0xA0B00`/`0xA0C00` per-sound queries, `0xA0D00` load bank,
    `0xB0100` bank-load status (1 = loaded).
- `FUN_0010D868` = sound init (skipped if `*(0x481900) != 0`), called from
  `FUN_002A6330`, which then loads a bank and loops
  `while (FUN_0010DE78() != 1) DelayThread(1000)`.
- On the IOP the load handler (`0x22DC0`) builds `\AUDIO\SET%s_PS.SDT;1` with
  sysclib `sprintf` and spins `while (!sceCdSearchFile()) WaitVblankStart()`.
  Sound banks: `AUDIO/SETn/SFXnn_PS.RAW` (ADPCM) + `.SDT` (table).
- CDSTREAM's audio thread (entry `0x1FCE8`) waits on a semaphore signalled by
  libsd transfer-complete and SPU2 IRQ handlers (`sceSdSetTransCallback`,
  `sceSdSetIRQCallback`) - real SPU2 emulation is needed for audio.
- `DelayThread` (`0x468318`) = `SetTimerAlarm` (SDK timer lib on EE T0,
  handler `0x451038`) + `WaitSema`; a fresh semaphore per call.
- libsd `sceSdInit` waits for `STATX & 0x7FF == 0` after reset; STATX bit 7
  (DMA busy) must only be set while `ATTR & 0x30` (DMA mode) is set.
- The sound-bank RPC handler (fn `0xA0D00`) signals CDSTREAM's reader thread
  with a semaphore and then polls for completion *inside the RPC call*. The
  runtime runs RPC handlers as nested calls, not on the server thread, so the
  reader never ran: the IOP emulator now yields to ready IOP threads after
  thread/semaphore/event imports made from a nested RPC call (`yieldFromCall`),
  and a `DelayThread` there waits by running the IOP scheduler.
- Bank load: ~1.7 MB of ADPCM by IOP DMA channel 4/7 into SPU2 RAM; music and
  dialogue stream through voice loops refilled on SPU2 IRQ-address interrupts.

## Level data (WRLD)
- `MAINLA.LVZ` / `BEACH.LVZ` / `MALL.LVZ` are zlib streams of a `WRLD` image
  (header bytes `"DLRW"`); MAINLA inflates to 0x54F358 bytes at `0x756680`
  (read into `0xA8FE00`, inflated by `FUN_0021FA38`, relocated by `FUN_00324A50`).
- World sectors then stream from the matching `.IMG` through CDSTREAM
  (e.g. MAINLA.IMG reads into 0xCB7A80..0x112A660).
- VIF1 DMA payloads come from packet buffers (0x500000-0x6FFFFF) and the model
  heap (0x1200000-0x17FFFFF). Peds, vehicles, props render; static world
  geometry did not (no DMA ever referenced the WRLD image or sector buffers) -
  under investigation, possibly the SQRT.S fix.
- In-world draws are mostly MODULATE + alpha test GREATER 8, fogged, vertex
  colours from V4-5 unpacks.

## Renderer (found while fixing the missing world)
- One VIF1 DMA chain per frame, kicked at `0x27245C` (D1_CHCR = 0x145, chain +
  TTE); TADR alternates between static root lists `0x480D20` / `0x480DE0`.
- Models are prebuilt packets in the model heap, entered with CALL tags and
  ended by RET; CALL tags are written by the RSL mesh code (`0x2F62B8..0x2FB690`,
  `FUN_003298D8`). VU1 microcode sits inside `.text` and is uploaded each frame
  by REF tags with `MPG` VIFcodes (`0x2AC130`, `0x2F13D0`, `0x368730`, ...).
- World sectors: `FUN_0023E748(level, pass)` (3 passes per frame; takes a lock,
  scrolls water UVs at level+0x5D0/0x5D4) walks the sector slots at
  level+0x248.., and per entry calls `FUN_00240ED8`, which culls with
  `FUN_002F7528` = unit box transformed by the sector matrix, tested by a VU0
  microprogram (CMSAR0 0x19F, param VI15 = 0xDC) via `FUN_0030C8D4`; result 0 =
  culled, 1 = inside, 2 = partly. Visible sectors go to `FUN_002409C8`.
- Entities: `CCamera::IsSphereVisible` = `FUN_003816F8` (VU0 macro transform of the
  centre by the camera matrix, near/far at 0x4CD130 / 0x4CD12C, planes at cam+0xB60).
- Level object `0x750340` (pointer at `0x48F4F8`).
- The world is a hex grid of cells (125 x 108.25 units, odd rows offset by half a
  cell; origin = ((x + 0.5 - (y & 1) / 2) * 125 - 2400, (y + 0.5) * 108.25 - 2000),
  `FUN_00241F00`). Cell slots at `0x75036C` (+0xA0 each): grid x/y at +8/+0xC,
  loaded flag +0x18, draw lists via +0x20. `level+0x248` = current cell,
  `+0x250` = next one; `FUN_0023C820` (per frame) follows TheCamera
  (`0x6F44D0`, position at +0x30) and `FUN_0023D740` swaps them. Each cell
  streams ~0.3-0.5 MB of MAINLA.IMG in 6 pieces into `0xCB7A80..0x112A660`.
  Fort Baxter is around cells (5..7, 16..18).
- Z buffer: Z24 640x448 at zbp 280, cleared each frame by 40 column strips
  (640x224 CT32 in total) at FBP 280 whose own Z write goes to ZBP 350 (the
  second half).
- DMA packet buffer `0x520F90`: two halves of 0xE6670 bytes; `FUN_00476770`
  returns the bytes used, draw code skips work when < 0x12C00 are left. Peak
  use seen: ~400 KB.
- The frame's VIF1 chain can exceed 4096 tags (CALL/RET per model).
- VU1 setup programs that never XGKICK (normal): 0x0148 (clear state), 0x0418
  (lighting dispatch via JALR table at VU mem 34..37), 0x0478 (matrices),
  0x2B50 (matrix copy).

## Memory card (found while testing saves)
- libmc calls are HLE (`Kernel/Stubs/MemoryCard.cpp`), files live in
  `%LOCALAPPDATA%/VCSRecomp/mc0` (set in `src/main.cpp`).
- Card manager object `0x510480`; its status word `+0x54` reads 27 (`0x1B`)
  when the card is idle / the last operation was fine.
- `FUN_00108128` = the save menu's "save to slot" action (slot byte at
  `0x4818AC`): `FUN_00276bf0(mgr, 0)` scans the card (8 slots, `SAVESLOTn`),
  the flag at `0x48A664` goes up, `FUN_002778a8(mgr, slot)` saves, flag down.
- `FUN_002778a8`: creates `BASLUS-21590GTA40001/` with `icon.sys` and
  `vcsicon.ico` if missing, then writes the slot file (named after the
  mission, e.g. `1 In the beginning`, 98,304 bytes) and the
  `BASLUS-21590GTA40001` file (179,350 bytes).
- `FUN_00277688(mgr, slot)` checks the card and free space before a save
  (0 ok, 2 not enough space, 3 error); the game calls it at boot.
- Load: `FUN_003adae0()` (generic load) runs from world init `FUN_00255308`
  when a load was requested from the menu; it reads through `FUN_002759e8`
  (`sceMcRead`).
- At boot the game reads `icon.sys` (964 bytes) to list the saves.
- With a save on the card the game boots into a LOAD / NEW / DELETE menu over
  the title flyover; without one Start begins a new game.
- `VCS_TEST_SAVE=<s>` (src/game/vcs_overrides.cpp) runs the save sequence as
  guest tail calls from `scePadRead`. A direct host call of a guest function
  is not safe: a scheduler checkpoint inside it unwinds the call and the rest
  of the routine never runs.
