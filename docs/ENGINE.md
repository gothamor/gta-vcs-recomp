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

## Boot
- Entry `0x001DD6A0` (crt0).

## Sections still to fill
- Main loop and frame limiter
- Timer / time step
- Camera and projection (FOV, aspect)
- HUD / 2D drawing and aspect handling
- Renderer: DMA chains, VU1 microprograms (`.vutext`), GS setup
- Streaming: CDSTREAM.IRX protocol, IMG archives, LVZ
- Audio: SDRDRV/LIBSD, VB streams
- Threads and their priorities
- Save/memory card format
