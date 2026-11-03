# modloader/

Mods for GTA: Vice City Stories (this port), the way PC GTA's Mod Loader does it: **one folder per mod** in here.
Nothing of the game's is changed on disc; mods are read while the game runs, and each one can be switched on or off
in the game's settings (F2 > Mods), at once.

```
modloader/
  HD Textures/            <- a mod: any folder name
    SLUS-21590/replacements/....dds
  My HUD/
    1234abcd-5678ef90-00005994.dds
  modloader.ini           <- made by the game: priorities and ignored mods, in Mod Loader's own format
```

What a mod can hold so far: **game files** - GTA3PS2.IMG entries named `<model>.mdl` (`admiral.mdl`) or
`#<index>.bin`, from `tools/img.py extract` - replacing just those entries, and loose disc files by name
(`AMERICAN.GXT`, `MAIN.SCM`); and **texture packs** - PCSX2-named `.dds` (BC7 / BC1 / BC3, with mips) or `.png` files,
anywhere in the mod's folder (`dumps/` folders are skipped). When two mods have the same texture, the higher priority
wins - set it with the slider beside each mod in F2 > Mods, or in `modloader.ini` (the slider writes there):

```ini
[Profiles.Default.Priority]
HD Textures=60      ; 1-100, 50 by default; 0 ignores the mod
[Profiles.Default.IgnoreMods]
_old*               ; wildcards: mods not loaded at all
```

A folder whose name starts with `.` is not a mod. A new mod folder appears in F2 > Mods after a restart; changed
files are read again with F2 > Mods > Rescan. More: docs/MODS.md and docs/TEXTURE_PACKS.md.
