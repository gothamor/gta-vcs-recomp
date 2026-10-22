# Game internals - GTA: Vice City Stories (PS2, SLUS-21590)

Everything known about how the game works inside, gathered while recompiling it. Addresses are EE
RAM addresses in SLUS_215.90 (identical to ELF virtual addresses; see [DISC.md](DISC.md)). Names in
`CamelCase::Like()` are our labels, modelled on the GTA III-era naming (re3 / reVC) where the code is
clearly the same lineage; they are guesses until marked **confirmed**.

Each fact says how it was established, so it can be re-checked. Tools: `tools/eedis.py` (EE
disassembly with names from `game/vcs/functions.csv`), `tools/vudis.py` (VU microcode), Ghidra
(`ghidra/`), the runtime's traces (`PS2X_*`, `VCS_*` switches in PROGRESS.md), the control channel
(`tools/gamectl.py`) and frame dumps (`PS2X_HW_DUMP`). Listings derived from game code go to
`build/analysis/` and are never committed.

**When you learn something about the game, add it here in the same change.**

---

## 1. Background (outside sources)

- Rockstar Leeds engine, shared with the PSP original (2006). The PS2 port (2007) reuses PSP-side
  data formats (`.PSP` frontend files).
- Descends from the GTA III / VC codebase but renders through Leeds' own "RenderWare Stories"-style
  layer (RSL), not stock RenderWare.
- Runs at 30 fps with a frame limiter. GTA III-era code scales movement by `CTimer::ms_fTimeStep`
  (1.0 = 1/50 s), so 60 fps mostly works, but earlier games had known 60 fps bugs (physics, some
  animations, script timers, water cannon). The PSP recomp (OverkillLabs3) and ThirteenAG's PPSSPP
  fix both patch these for 60 fps - references for milestone 4 of NATIVE_PORT_ROADMAP.md.

## 2. Code map

- 9,550 functions (Ghidra 12.1.4 + EE extension) plus 833 indirectly referenced ones Ghidra missed
  (`tools/find_missing_functions.py`). 8,521 below `0x443000` are game code; 1,029 from `0x443000`
  up are Sony SDK / libc / libgcc.
- SDK libraries, identified by ps2_analyzer's SCE symbol DB (`game/vcs/vcs.toml`):
  - `0x4431F8..` libgcc (`__divdi3`, `__muldi3`, soft-double `dpadd`...)
  - `0x447670..` libgraph (`sceGsResetGraph`, `sceGsSwapDBuff`, `sceGsSyncV`)
  - `0x448688..` libdma (`sceDmaSend`...)
  - `0x448D98..` libpad (`scePadRead`... - libpad, not libpad2)
  - `0x44E5F8..` libcdvd incl. **streaming** `sceCdStInit/StStart/StRead/StStop`
  - `0x44F668..` libmc (`sceMcOpen/Read/Write/GetDir`...; stubs: `0x450120` = `sceMcGetDir`,
    `0x44fe80` = `sceMcSync`)
  - `0x452320..` libmpeg (`sceMpegCreate`, `sceMpegDemuxPssRing`) + libipu
  - `0x45C1D8..` libsdr (`sceSdRemoteInit`, `sceSdRemote`) - audio goes over SIF RPC to SDRDRV.IRX
  - `0x467xxx` kernel syscall wrappers, `0x469E00..` libsif / RPC, `0x46B980..` libfileio
  - `0x46EA98..` Deci2 (dev-kit debug channel; dead on retail)
- `fiprintf` at `0x1A9EF0` lives inside game code (newlib linked early). `FUN_0045f378` = `sprintf`
  (seen building memory card paths).
- Largest game functions (state machines / script interpreter candidates): `0x141FE8` (41.7 KB),
  `0x13A0A0` (25 KB), `0x337208` (25 KB), `0x33F010` (20 KB).
- The analyzer found only 2 jump tables; the recompiler reports ~1,150 unresolved `jr` sites (switch
  tables) and falls back to dispatch through the function table. The memory card state machine
  `FUN_002788e0` is one (`jr` through the table at `0x4B0400`, indexed by the state at `0x487730`).

## 3. Boot

- Entry `0x001DD6A0` is standard crt0: zero GPRs / FPRs, clear .bss (`0x4CD100..0x74DF54`), set
  gp / sp, SDK init, `main`.
- `sceSifInitRpc`, then IOP modules from the disc in this order: `SIO2MAN, LIBSD, SDRDRV, CDVDSTM,
  CDSTREAM, PADMAN, SIO2MAN (again), MCMAN, MCSERV`. All nine run as real IRX code on the emulated IOP
  (SDRDRV prints "SDR driver version 4.0.1"); `SDRDRV`, `CDVDSTM`, `MCMAN`, `MCSERV` stay resident.
- `_lf_bind` (`0x46CD10`) polls for the ROM LOADFILE server - absent in the recomp, stubbed.
- **Disc indexing**: `FUN_002209F0(path)` walks `cdrom0:` recursively with
  `sceDopen/sceDread/sceDclose`, and for each file (`st_mode & 0xF000 == 0x2000`) calls the
  `sceCdSearchFile`-like `0x44E480` and appends (name ptr, lsn, size) to a table at `0x5038A8`
  (count at `0x486E8C`, string pool cursor at `0x486E88`). Every file is resolved to an LSN up
  front; reads are by sector afterwards.
- Movies: `FUN_0021CA70` (from `main`) plays `STINGS.PSS` then `INTRO.PSS` through the movie player
  at `0x40D8B8` (libmpeg + IPU). Skipped today (`vcs_overrides.cpp`; DECISIONS.md).

## 4. Frame and timing

- The game updates and draws at 30 fps on the 60 Hz VBlank: in a frame dump the 3D is in every
  other VBlank's GS frame; the in-between VBlanks carry no draws (found when `PS2X_HW_DUMP` kept
  landing on empty frames, 2026-10-22; the dump now waits for a frame with draws).
- Main loop, frame limiter and the CTimer time step: **not mapped yet** (milestone 4).

## 5. Camera, projection and widescreen

Found in the widescreen spike (SETTINGS_MENU_WIDESCREEN.md), by forcing the preference and logging
the globals each frame:

| What | Where | Notes |
|---|---|---|
| FOV global | `0x487484` (float, degrees) | written by `FUN_002653e0` (a `CDraw::SetFOV`); 45 in the frontend, 120 in the flyover, 70 in game |
| Aspect global | `0x487488` (float) | written only by `FUN_002653f0` (`CDraw::CalculateAspectRatio`) |
| Aspect choice | `FUN_002653f0` | widescreen preference on: 16/9, or 5/3 when `[0x6f4d14]` is set; off: 4/3, or 1.25 when set. `[0x6f4d14]` is 0 in the frontend and 1 in game (not a PAL flag) |
| Widescreen preference | `*(int *)(*(0x48F3D0) + 0x30)` | getter `FUN_002af658` (`lw v0, 0x30(a0)`); the object at `[0x48F3D0]` (prefs / menu manager) is created by `FUN_00470fa8` |
| View window | `FUN_001ab9e0(camera a0, raster rect a1, tan f12, aspect f13)` | x = tan(FOV/2), y = x / aspect, raster 640x448 (null rect), then `FUN_0018f2d8` = RwCameraSetViewWindow. Called at `0x21cdd4` with the aspect global, at `0x21f9c8` with a fixed 16/9 (same camera `[0x51E490]`; probably the letterboxed cutscene view) |

- The game's own widescreen is **Vert-**: same horizontal view, vertical view cut, for a TV that
  stretches the anamorphic picture. The HUD does not change with it (the radar keeps its shape); the
  preference's 106 readers are mostly 3D-projected sprites (e.g. `0x1147e0`: a size without the 4/3
  factor).
- RenderWare derives the frustum (EE culling) and the VU matrices from the view window, so widening
  it (our Hor+, `vcs_display.cpp`) widens 3D, culling and both renderers at once.

## 6. Renderer

### 6a. Frame structure

- One VIF1 DMA chain per frame, kicked at `0x27245C` (D1_CHCR = 0x145, chain + TTE); TADR alternates
  between static root lists `0x480D20` / `0x480DE0`. Full frames are 7-14k tags (CALL / RET per
  model) - more than the runtime's original 4096-tag guard.
- Models are prebuilt packets in the model heap, entered with CALL tags and ended by RET; CALL tags
  are written by the RSL mesh code (`0x2F62B8..0x2FB690`, `FUN_003298D8`). VU1 microcode sits inside
  `.text` and is uploaded each frame by REF tags with `MPG` VIFcodes (`0x2AC130`, `0x2F13D0`,
  `0x368730`, ...).
- VIF1 payloads come from packet buffers (`0x500000-0x6FFFFF`) and the model heap
  (`0x1200000-0x17FFFFF`). DMA packet buffer `0x520F90`: two halves of 0xE6670 bytes;
  `FUN_00476770` returns the bytes used; draw code skips work when < 0x12C00 are left (peak ~400 KB).
- Z buffer: Z24 640x448 at ZBP 280, cleared each frame by 40 column strips (640x224 CT32 in total)
  drawn at FBP 280, whose own Z write goes to ZBP 350 (the second half).
- Display: PMODE 0x8027 = both circuits on the same buffer one line apart, blended (anti-flicker);
  DISPFB FBP 0, FBW 10 (640), DBY 2 / 1; 640x445 shown.

### 6b. World and culling

- World sectors: `FUN_0023E748(level, pass)` (3 passes per frame; takes a lock, scrolls water UVs at
  level+0x5D0/0x5D4) walks the sector slots at level+0x248.., and per entry calls `FUN_00240ED8`,
  which culls with `FUN_002F7528` = unit box transformed by the sector matrix, tested by a VU0
  microprogram (CMSAR0 0x19F, VI15 = 0xDC) via `FUN_0030C8D4`; 0 = culled, 1 = inside, 2 = partly.
  Visible sectors go to `FUN_002409C8`.
- Entities: `CCamera::IsSphereVisible` = `FUN_003816F8` (VU0 macro transform of the centre by the
  camera matrix, near / far at `0x4CD130` / `0x4CD12C`, planes at cam+0xB60).
- Level object `0x750340` (pointer at `0x48F4F8`). The world is a hex grid of cells (125 x 108.25
  units, odd rows offset by half a cell; origin = ((x + 0.5 - (y & 1) / 2) * 125 - 2400,
  (y + 0.5) * 108.25 - 2000), `FUN_00241F00`). Cell slots at `0x75036C` (+0xA0 each): grid x / y at
  +8 / +0xC, loaded flag +0x18, draw lists via +0x20. `level+0x248` = current cell, `+0x250` = next;
  `FUN_0023C820` (per frame) follows TheCamera (`0x6F44D0`, position at +0x30) and `FUN_0023D740`
  swaps them. Each cell streams ~0.3-0.5 MB of MAINLA.IMG in 6 pieces into `0xCB7A80..0x112A660`.
  Fort Baxter is around cells (5..7, 16..18).
- In-world draws are mostly MODULATE + alpha test GREATER 8, fogged, vertex colours from V4-5
  unpacks.

### 6c. VU microcode

- `.vutext` holds four small VIF `MPG` packets (DVP overlays: `0x48055C` -> VU 0x000, 245 pairs;
  `0x4810EC` -> 0x400, 4; `0x48146C` -> 0x000, 24; `0x48176C` -> 0x200, 20). They are **VU0**
  helpers: program 0 has E-bit exits at 0x000 / 0x010, converts fixed point with `ITOF0` /
  `MULi 1/128`, builds edge vectors and runs `CLIP` / `FMAND` / `FCAND` / `DIV` - most likely
  collision (line / sphere vs triangle). Found by disassembling with hctp-recomp's `vu1dis.py`.
- The **VU1** rendering microcode is not a static MPG packet: it is uploaded from `.text` each frame
  (6a). `game/vcs/vu1_uploads.txt` lists the uploads seen; `tools/vu1_recomp.py` compiles them.
- Setup programs that never XGKICK (normal): 0x0148 (clear state), 0x0418 (lighting dispatch via
  JALR table at VU mem 34..37), 0x0478 (matrices), 0x2B50 (matrix copy).

### 6d. VU1 pipeline (found with PS2X_HLE_SURVEY, NATIVE_RENDERER.md stage 4)

Program **0x0030** produces ~95% of all GS vertices (3.0-3.5M/s) and ~88% of VU1 time. 0x02c0
(V4-32 x4 per call: sprites / particles / text) ~3%. Disassemble samples with
`python tools/vudis.py <vucode.bin> --start 0x30 --to-ebit` (`PS2X_HLE_SURVEY_DUMP=2
PS2X_HLE_SURVEY_DUMP_EVERY=40000`).

**0x0030 is a dispatcher**: `JR` to instruction `[38].x` inside itself, then a chain of `JALR vi15`
calls whose targets / `vi01` parameters come from VU data qwords 38-41 (set per batch by the setup
programs). Stage tables (instruction indices):

| entry | stages (vi01) | used by |
|---|---|---|
| 0x19 | 0x0da, 0x4e0 (0x9c), 0x2b6 | prelit world: V3-16 pos @1, V2-8 uv @0x48, V4-5 colour @0x8f (+ V4-32 matrix @0x2c x3 in some) |
| 0x19 + 0x4ff | 0x0da, 0x4ff (0x9c), 0x2b6 | rigid lit models (vehicles, props): int8 normals V3-8 at `[47].y` |
| 0x16 | 0x5a5 (0xa8), 0x0da, 0x4ff (0x9c), 0x2b6 | lit, skinned models (peds): V3-16 pos, V2-8 uv @0x55, V3-8 normal @0x2b, V4-32 weights @0x7f |
| 0x13 | 0x0da, 0x4ff (0x9c), 0x2b6, 0x5b1 (0xa8), 0x2b2 | lit models, second pass (env map?) - still on the VU1 |

Code at 0x2700.. (instr 0x4e0..) is swapped by per-pass MPG uploads, so programs are keyed by the
code at the start PC *and* 0x2700-0x2fff.

VU data layout of the world pipeline (qwords; "top" = XTOP double buffer):
- 28-31: combined world -> GS window matrix (rows; includes the viewport scale / 2048 offset).
  24-27: the clip matrix.
- 42: position scale (1/32768 for int16), 43: position offset (folded into the matrix).
- 44-46: 3x3 matrix for the V2-8 texture coordinates, applied with I = 1/255.
- 47: input offsets: .x positions (1), .y normals, .z uv (0x48), .w colours (0x8f).
- 48: .z = mask applied to TOP, .w = output buffer offset (0xd6).
- 49 / 50: GIF tags for the kick (PACKED, NREG 3: ST, RGBAQ, XYZF2; PRIM strip with IIP TME FGE ABE);
  NLOOP = vertex count from `[pos-1].w`. 51: default RGBA (255,255,255,128) / material colour.
- 52: .x transform routine (0x0e9 normal / 0x0da first call), .y flags (bit 1 fog, bit 0 extra pass
  at 0x1440), .z kick variant (698 / 711). 53: fog (.z bias 255.6, .w scale -0.128 per unit of w).
- 0x9c-0x9e (vi01 of stage 0x4e0): colour scale (time-of-day tint), offset, clamp (255).

Routines (byte addresses): 0x0748 normal path (`BAL 0x0c38` clip test; all inside -> 0x06f0 fast
path; some outside -> clipper 0x11f8 + 0x13d8); 0x07f0 transform (`p = M * (itof(pos) * scale +
offset)`, `Q = 1/p.w`, XYZ = FTOI4(p.xyz * Q), STQ = (uvMatrix * itof(uv) / 255) * Q); 0x1048 fog
(F = clamp(53.z + 53.w * w, 0, 255)); 0x2700 prelit colour (RGBA = MINI(itof(c) * [0x9c] + [0x9d],
[0x9e])); 0x15b0 GIF tag + `XGKICK`. Lit models: 0x2d28 (stage 0x5a5) 4-bone skinning (weights'
low bits are bone indices, bones 4 qwords each from 0xa9, result written back over the position /
normal slots); 0x27f8 (stage 0x4ff) 4 directional lights + ambient, scaled by `[51]` * 1/510, RGB
only. All reproduced bit-exact in `src/game/vcs_hle_world.cpp` (`PS2X_HLE_VERIFY=1`), except the
STQ.w lane and its fog in the lit pass, which the runtime's VU1 core leaves inconsistent.

### 6e. GS usage (VCS_GS_STATS, title / intro)

- ~95% triangle strips; also triangles, fans, sprites, points. CT32 colour (some CT16 for effects);
  Z24 (some Z32), test GEQUAL.
- Blend: 84% `(Cs-Cd)*As+Cd`; also `(Cs-Cd)*FIX+Cd`, `Cs*FIX+Cd` (FIX up to 255), `Cs*FIX`,
  `Cd-Cs*FIX`. Alpha test GREATER (+KEEP) and GEQUAL with AFAIL=FB_ONLY; no DATE.
- Textures: 83% PSMT4 + CT32 CLUT (world), CT32, T8, CT16; modulate / decal; repeat / clamp /
  region clamp; bilinear. Fog on most world geometry. CT32 / CT16 textures inside render targets
  = post effects. COLCLAMP on, no dithering, no FBA, no PABE.

### 6f. 2D: HUD, text, radar, menus, effects

Found from frame dumps (`PS2X_HW_DUMP`, draw lists with depth) during the widescreen work:
- RenderWare keeps **ZTE on for 2D** (ZTE=0 is prohibited on the GS) and uses ZTST ALWAYS or GEQUAL.
- The HUD, text (font texture 256x256), radar and menus (RwIm2D) are all drawn at the **largest Z**
  (0xFFFFFF; 0.99999994 after scaling by 2^-24, never exactly 1.0).
- 3D-projected sprites - clouds (64x64 textures at z ~0.0006), coronas, the mission marker glow -
  take the same RwIm2D path but at their real depth.
- The sky is a full-width gradient at z = 1 (n=18 strip, x -0..852); a horizon band full width too.
- The colour filter / trails effect is drawn as **strips that sample the frame** itself (textures
  inside render targets, `texIsRt`), plus an alpha-only quad (FBMSK 0xFFFFFF) over x 80..560 at
  z = 0.
- The radar: an off-screen 128x128 target at FBP 420 (ZBP 0) is drawn and composited; on screen
  the round outline is a fan at far Z (GEQUAL, Z write on) and the map tiles (128x128 textures, far
  Z, GEQUAL) are clipped by the **scissor** to the radar's rectangle. Menu labels are scissored
  too.
- Pause menus have no depth-tested geometry below the far plane (that is how "3D frame" vs "2D
  frame" is told apart for the widescreen presentation).

## 7. Textures: `.XTX`

Confirmed from `LOADSC*.XTX` (263,372 bytes each, 512x512 8-bit):
- `0x00` magic `"xet\0"`; `0x08` file size; `0x0C` / `0x10` offset of the trailing relocation table
  (0x404B0); `0x60` name (`"loadsc0"`).
- Pixels at **0xB0**: 512x512 linear PSMT8 indices (no swizzle). CLUT at **0x400B0**: 256 x RGBA8888,
  alpha 0x80 = opaque, already in **CSM1 order** (index bits 3 and 4 swapped).
- The game uploads both unchanged: CLUT to CBP 13584 (16x16 CT32), pixels to TBP 8960 (TBW 8, T8),
  through a VIF1 DIRECT (BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE GIF tag) and a REF tag whose
  upper half is `NOP; DIRECTHL n` (this split exposed the upstream VIF bug in PS2RECOMP_PATCHES.md).
- The legal screen (copyright text + logo) is also a 512x512 T8 upload.

## 8. Data loaded at boot

- `PS2/GAME.DTZ` = zlib -> 6.6 MB `GTAG` v1 memory image (header: size, then table offsets), loaded
  at `0x016FE800` and relocated by `FUN_00324A50`; main copies ~50 global pointers out of it.
- `MAINLA.LVZ` / `BEACH.LVZ` / `MALL.LVZ` are zlib streams of a `WRLD` image (header `"DLRW"`);
  MAINLA inflates to 0x54F358 bytes at `0x756680` (read into `0xA8FE00`, inflated by
  `FUN_0021FA38`, relocated by `FUN_00324A50`). Sectors then stream from the matching `.IMG`
  through CDSTREAM.

## 9. Streaming and sound (CDSTREAM.IRX)

- CDSTREAM.IRX is Rockstar's IOP driver for **both** disc streaming and sound. Three SIF RPC servers
  (IOP threads):
  - `0x65686577` - stream reads. EE client `0x505B48`: fn 1 = read `{channel, EE dest, LSN, -bytes}`
    (size negated), fn 2 = poll (`0xFA` busy, `0` done). The first request is GAME.DTZ (0x217E70 bytes).
  - `0x65686578` - EE client `0x4CE2C0`, bound by `FUN_0010DCE8`.
  - `0x65686579` - EE client `0x4D3D40`, bound by `FUN_0010E918`. Commands go through
    `FUN_0010E970(cmd, arg)`: buffer `0x4CE300`, the reply's first word is the result. Seen:
    `0xA0000` (count, 0x6D8 = 1752 sounds), `0xA0B00` / `0xA0C00` per-sound queries, `0xA0D00` load
    bank, `0xB0100` bank-load status (1 = loaded).
- `FUN_0010D868` = sound init (skipped if `*(0x481900) != 0`), called from `FUN_002A6330`, which then
  loads a bank and loops `while (FUN_0010DE78() != 1) DelayThread(1000)`.
- On the IOP the load handler (`0x22DC0`) builds `\AUDIO\SET%s_PS.SDT;1` with sysclib `sprintf` and
  spins `while (!sceCdSearchFile()) WaitVblankStart()`. Banks: `AUDIO/SETn/SFXnn_PS.RAW` (ADPCM) +
  `.SDT` (table).
- CDSTREAM's audio thread (entry `0x1FCE8`) waits on a semaphore signalled by libsd
  transfer-complete and SPU2 IRQ handlers (`sceSdSetTransCallback`, `sceSdSetIRQCallback`).
- The bank-load RPC handler (fn `0xA0D00`) signals the reader thread and then polls for completion
  *inside the RPC call* (the runtime's nested-RPC model needed `yieldFromCall` for this).
- Bank load: ~1.7 MB of ADPCM by IOP DMA channel 4/7 into SPU2 RAM; music and dialogue stream
  through voice loops refilled on SPU2 IRQ-address interrupts.
- `DelayThread` (`0x468318`) = `SetTimerAlarm` (EE T0, handler `0x451038`) + `WaitSema`; a fresh
  semaphore per call. libsd `sceSdInit` waits for `STATX & 0x7FF == 0` after reset; STATX bit 7
  (DMA busy) only while `ATTR & 0x30`.

## 10. Memory card and saves

libmc calls are HLE (`Kernel/Stubs/MemoryCard.cpp`); files live in `%LOCALAPPDATA%/VCSRecomp/mc0`
(`src/main.cpp`), one host folder per card folder.

### 10a. Card manager

- Card manager object `0x510480`, one 0x4C0-byte block per port. Status word `+0x54` reads 27 (0x1B)
  when idle / the last operation was fine, 14 (0xE) after "no entry", 1 after "no format".
- `FUN_00275068(mgr, port)` / `FUN_002760d8(mgr, port)` list the current folder:
  `sceMcGetDir(port, 0, "*", 0, 15, mgr + port*0x4C0 + 0x100)` (path string at `0x4B0148`), then
  `sceMcSync(0, ...)` - **synchronous**, waits for the result. The 15-entry table (64 bytes per
  entry, name at +0x20, attributes at +0x14, size at +0x10) is what everything else reads.
- `FUN_002788e0` is the save / load state machine (states 0..5 at `0x487730`, error at `0x487744`,
  open fd at `0x487748`); it builds paths with `sprintf` (formats at `0x4B03D8` / `0x4B03E8`) and calls
  `sceMcGetDir` with 1 or 20 entries for single files.

### 10b. Save folder and slot files

- `FUN_002778a8(mgr, slot)` (from the save menu's "save to slot", `FUN_00108128`, slot byte at
  `0x4818AC`) creates `BASLUS-21590GTA40001/` with `icon.sys` and `vcsicon.ico` if missing, then
  writes the slot file and the `BASLUS-21590GTA40001` data file (179,350 bytes). Order of creation
  on a fresh card (host creation times): `icon.sys`, `vcsicon.ico`, `BASLUS-21590GTA40001`, then the
  slot file.
- Slot files are named `"<slot> <mission name>"` (e.g. `1 In the beginning`, `1 Soldier`), 98,304
  bytes; the data starts with the block magic `"SIMP"`. How the game replaces an existing slot's
  file (delete first, or overwrite by name) is not confirmed yet - it depends on finding it in the
  listing (10c).
- `FUN_00277688(mgr, slot)` checks the card and free space (0 ok, 2 not enough space, 3 error); the
  game calls it at boot.

### 10c. How the game finds its saves - card directory order matters

`FUN_00276bf0` (loop at `0x276da0..0x2772dc`) scans the listing for **8 slots**: for slot s (1..8)
it walks **table entries 5 to 14**, and takes an entry when its name is non-empty, its attributes
have bit 0x80 (closed) set, its size is > 0x17FFF, and its first character equals `sprintf("%d", s)`
(format at `0x4B00A8`). Then it reads each slot file's first 268 bytes; with two files for slot 1
on the user's card it read both headers and then loaded the newer one (`1 Soldier`) in full.

It **skips entries 0-4**, assuming they are `.`, `..` and the three files created with the folder.
That is only true in a real card's **directory-table order** (creation order; a deleted entry's slot
is reused by the next file created). Our first `sceMcGetDir` sorted names alphabetically, which put
`1 ...` at index 2 - so saves were written but never found: no load at boot, an empty Load Game
list, and a new save to slot 1 created a second `1 ...` file next to the old one (found 2026-10-22
from the user's card: `1 In the beginning` and `1 Soldier` side by side). Fix: each host card folder
keeps its order in a hidden `.ps2x-order` file (PS2RECOMP_PATCHES.md).

### 10d. Loading

- At boot the game reads `icon.sys` (964 bytes), lists the folder, reads every slot file's header
  and then the newest slot file in full (98,300 + 4 bytes), showing "Loading data..." and "Load
  Successful." before the world loads at the save point.
- `FUN_003adae0()` (generic load) runs from world init `FUN_00255308` when a load was requested; it
  reads through `FUN_002759e8` (`sceMcRead`).
- `VCS_TEST_SAVE=<s>` (`src/game/vcs_overrides.cpp`) runs the save sequence as guest tail calls from
  `scePadRead`. A direct host call of a guest function is not safe: a scheduler checkpoint inside it
  unwinds the call and the rest of the routine never runs.

## 11. Open questions / next research targets

- Main loop, frame limiter, CTimer (section 4) - milestone 4 (60 fps).
- Player / ped / vehicle structures and the player pointer - milestone 1 (teleport, state).
- `MAIN.SCM` and the script VM (CRunningScript list, mission start, mission-passed flags) - milestone 1
  (mission launcher).
- Cheat handlers (weapons, health, vehicles, weather) - milestone 1.
- Collision: VU0 program 0 (section 6c), collision streaming - the fall-through bug.
- HUD / font / sprite call sites (CHud, CFont, CSprite2d, CSprite) - milestone 3 (2D at the RW level).
- Sound driver command protocol in full (section 9) - milestone 6.
- Threads and their priorities.
