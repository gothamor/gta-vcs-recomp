# Launcher and releases

`VCSLauncher.exe`, shipped in every Windows release, takes a player from "extracted the zip" to "playing" without a
terminal: it finds and checks the disc, installs what the build needs, recompiles and builds the game with a progress
bar, and starts it. It is a front end over the Python tools in `tools/` - every button runs a script people can also
run by hand - so the launcher and the README's manual install are one pipeline.

Status (2026-10-30): built, and checked end to end from a release zip in a fresh folder (below). Windows only; the
code is SDL3 + Dear ImGui, so a Linux build is mostly the process / folder layer (`launcher/process.cpp`,
`launcher/system.cpp`). Decisions: DECISIONS.md 2026-10-30 "A launcher for releases".

## Why

The manual install (README) is: Python with *Add to PATH*, Git, Visual Studio with two specific options, clone, copy
the `.iso` in under the right name, a terminal, `python tools/build.py --recompile`, then find `build\vcs.exe`. Each
step loses players, and the README's *Problems* table is mostly those steps going wrong. Distribution is source-only
(DECISIONS.md 2026-10-21): the player's PC compiles the game, so the launcher is also a build front end.

## A release

`python tools/release.py` (CI on a `v*` tag: `.github/workflows/launcher.yml`) writes
`build/dist/VCS-Recomp-<version>-windows.zip` (82 MB) and its `.sha256`. One folder inside:

| What | |
|---|---|
| `VCSLauncher.exe` | the launcher, one static exe (4 MB) |
| `VERSION`, `RELEASE.json` | the release's number (the repository's `VERSION`, also built into VCSLauncher.exe's version information; a `v*` tag must match it); its repository, commit and file list (tools/update.py) |
| `python/` | python.org's embeddable CPython (the version that made the release) |
| `deps/` | the libraries CMake would download (tools/deps.py): SDL3, Dear ImGui, zlib, ELFIO, toml11, fmt, libdwarf, rabbitizer, nlohmann/json, and the FFmpeg LGPL build - 136 MB unpacked |
| `THIRD_PARTY_LICENSES.txt` | their licences, collected from the libraries themselves |
| the source | the repository at the tag (`git archive`; `--worktree` for tests) |

With `deps/` there, `toolchain.configure` points CMake at it (`FETCHCONTENT_SOURCE_DIR_<NAME>`,
`-DFFMPEG_PREBUILT_URL=<zip>`, `FETCHCONTENT_FULLY_DISCONNECTED=ON`): the build needs neither git nor internet. Only
Visual Studio's compiler is left to install, and the launcher does that.

**No game code, ever**: `release.py` checks every file of the release before zipping - names (`*.iso`, `SLUS_*`...),
folders (`generated/`, `build/`, the Ghidra project, memory cards), content (a MIPS ELF header, an ISO 9660 volume
at sector 16, anything over 200 MB) - and stops without writing the zip if one matches. Checked with planted files of
each kind.

**The launcher and the scripts must match.** The launcher is built from the working tree; `--ref` (default HEAD)
packages a commit. With uncommitted changes the two differ - a release made that way (2026-10-30) had the new launcher
next to the last commit's `tools/`, without `doctor.py`, and the launcher showed "build tools missing" because its check
never answered. So `release.py --ref` refuses a working tree with uncommitted changes (commit, `--worktree`, or
`--allow-dirty`), and every release runs its own launcher's `--self-test` (with its bundled Python and its scripts)
before zipping. The launcher itself checks at start that the scripts it runs are in `tools\`, and a check that gives no
answer shows as "Couldn't check" with the reason and *Check again* - never as something missing.

## The player's flow

One window (`launcher/ui.cpp`): a dark sidebar with the logo, the four setup steps as a tracker (Folder, Disc, Build
tools, Game: a teal check when done, an amber ! for something to look at, the step's number when missing, a spinner
while checking) and three more pages (Saves, Settings, Help); the selected page in the main column - a small pink
label, a title, a line of explanation, then cards; and a footer whose one pink button is the next thing to do (Choose
your disc / Install build tools / Build the game / Update the build / Play; Cancel while a task runs; Try again / Close
after a failure). Everything else is a quiet secondary or text button on its page. The page shown follows what's next
until the player picks one. Drawn with Dear ImGui's draw list - badges, pills, a thin rounded progress bar, a spinner,
the logo are shapes, no icon font - in one palette (dusk violet base, pink for the action, teal for done, amber / red
for attention) at the display's scale.

- **Folders** (tools/doctor.py `check_folder`): the **build folder** - `build\` next to the source, or one the
  player chooses (*Choose another folder*: a folder dialog, then tools/build_dir.py `check` says what it would mean -
  the folder used, free space, warnings, or why not - and *Build here* runs `set`; *Use the default* goes back) - and
  the source folder. Warns for either under Program Files, in a OneDrive / Dropbox folder, on a network drive, in a
  path over 60 characters or with non-ASCII letters; less than 3 GB free on the build folder's drive; memory under
  8 GB.
- **Disc**: the launcher's own check (`launcher/disc_check.cpp`, the same as vcs.exe's through
  `src/disc/vcs_disc_id.h`): SYSTEM.CNF's boot file, then the SHA-1 of `SLUS_215.90` - about a second. What a player can
  act on: another release ("the European release of Vice City Stories (SLES_546.22)", "Liberty City Stories (USA)"),
  not a PS2 disc, a `.bin` / `.chd` / `.cso` / archive with how to convert it. A file dialog, drag and drop, a search
  of Downloads / Desktop / Documents / its folder (one level deep, DVD-sized `.iso` files, Vice City Stories names
  first), or **Copy from a DVD drive** (tools/rip_disc.py). The choice goes into `build\vcs.ini` as `iso=` (vcs.exe's
  settings keep lines they don't own).
- **Build tools**: tools/doctor.py `check_compiler` (vswhere, one `-requires` query per component - `-path` ignores
  `-requires`; the clang-cl the build would take: scoop's LLVM, then Visual Studio's, then PATH). Missing parts:
  *Install build tools* runs tools/setup_vs.py.
- **Game**: built and current, or the reasons not (doctor's `check_build`: no exe, generated/ stale, no / other
  `build_info.json`, source files newer than vcs.exe). Build type: keep / Standard (ThinLTO) / Quick / Fastest (PGO).
- **Play**: `build\vcs.exe <iso>`, working folder `build\`, no console, output to `build\run\launcher_game.log`, in a
  job that outlives the launcher. The launcher minimises (or closes, or stays: *Options*) and comes back when the game
  ends; a non-zero exit is explained from vcs.exe's exit codes (`vcs_disc_id.h`: 2 no disc, 3 unreadable, 4 wrong disc,
  5 no window / graphics / sound; `0xC...` a crash, with the log).
- **Saves**: the card's folder, Back up now, the backups with Restore (asks first; tools/backup.py).
- **Settings**: what the launcher does when the game starts, updates (check now, check at start, Update now when one
  is found - the sidebar marks it), a desktop shortcut.
- **Help**: Make a report (tools/report.py), the game's last log, the build log, the README, version and folder.

A running task takes the main column: a large percentage and a bar while compiling (Ninja's step count; a sliding
bar otherwise), the time so far and left, its stages as a timeline (check, spinner, cross, with their times), Cancel (the tool runs in a Job Object: the whole tree stops, and a new build continues where
it stopped), the log on demand. A failure shows the code's title and fix (`errorHelp` in `launcher/tasks.cpp`): no
Visual Studio / C++ tools / clang, a download, a full disk, a long path, a locked file (antivirus, OneDrive), out of
memory, the installer declined... in a card with a red edge, with *Open the log* and *Make a report*. The PC is kept from sleeping while a build
or a disc copy runs. Closing during a task asks first.

## How it fits together

```
VCSLauncher.exe (launcher/: main.cpp window + CLI, ui.cpp, tasks.cpp, process.cpp, disc_check.cpp, system.cpp, json.cpp)
   ├─ itself: window, disc check, folder dialogs, vcs.ini's iso=, processes (Job Objects), shortcuts, the game
   └─ python\python.exe -I tools\<tool>.py ...  with VCS_PROGRESS=json
         doctor.py --json     the state: folder, memory, compiler, downloads, disc, build, "next"
         build.py --recompile-if-needed <iso> --progress json [--opt X]
         setup_vs.py install, backup.py create|list|restore, report.py, update.py check|apply, rip_disc.py list|rip
```

Python is the release's `python\`, else a `python.exe` on PATH (not the Microsoft Store stub), else `py -3` - each
tried first (a py.exe can exist with no Python behind it).

**Events** (tools/toolchain.py): with `--progress json` / `VCS_PROGRESS=json` the tools print, between their usual
lines, `@vcs {...}` lines: `{"stage": id, "title": text}`, `{"progress": n, "total": n, "eta": s, "note": text}` (at most
4 a second), `{"error": code, "message": text, "log": path}` (from `tc.fail(message, code, log)`;
`configure_failure` / `build_failure` read the code from the log), `{"result": "ok", ...}`. Everything else is log.

**State files**: `generated/recompile_stamp.json` (a hash of what the generated code depends on: game/vcs, the
recompiler, the runtime headers it compiles in, the translate / sync scripts; `recompile.py --check`),
`build/build_info.json` (source version, generated inputs, optimisation, disc; written by a successful game build),
`build/launcher.ini` (the launcher's options).

## The tools (all usable without the launcher)

| Command | What |
|---|---|
| `python tools/doctor.py [--json] [--iso X]` | every check and the next step |
| `python tools/build_dir.py [check PATH / set PATH / reset]` | where the game is built. PATH: a new or empty folder is used as it is, a build folder made before is used again, any other folder gets a `VCS-build` folder inside; refused inside the source folder (other than `build\`), on a file, at the top of a drive, where it can't be made or written (tried for real: the top of C: allows folders, not files). Kept in `.vcs-build-dir` (git-ignored; `VCS_BUILD_DIR` for one run); every tool follows it through `toolchain.BUILD`, CMake puts vcs.exe at its top (`VCS_GAME_DIR`), `vcs.ini` and `launcher.ini` come along on `set`. The old folder is left as it was. A tree set up from another source folder gets a fresh CMake cache (`toolchain.drop_foreign_cache`). generated/ stays in the source folder |
| `python tools/build.py --recompile-if-needed [ISO] [--progress json]` | recompile only when stale, then build |
| `python tools/build.py --launcher` | build the launcher (build/launcher, the exe in the root; no disc) |
| `python tools/setup_vs.py status / install [--dry-run]` | Visual Studio Build Tools 2022 + clang: nothing there - Microsoft's bootstrapper (aka.ms, Authenticode-checked: valid and Microsoft's, or it isn't run) with the C++ workload, recommended parts and the clang components; a Visual Studio without clang / CMake - its installer's `modify --add` (the components only: the workload's ID differs between products); clang too old - `update`. Elevated via ShellExecuteEx `runas`; 1223 = declined, 3010 = restart needed |
| `python tools/backup.py create / list / restore NAME [--force]` | the card zipped with a manifest of every entry's creation / modification time; restore backs up the current card first, recreates in creation order and puts the times back (SetFileTime); refused while vcs.exe runs |
| `python tools/report.py [OUT]` | build logs, the newest four game logs (crash reports included), state files, doctor's JSON, Windows / CPU / GPU names; never the disc, generated code or the card |
| `python tools/update.py check / apply [--zip F]` | GitHub's latest release vs RELEASE.json; apply replaces the release's files, removes the ones the new release dropped, keeps build/ and generated/ and anything added; files in use (the launcher, the running Python) are renamed `*.old` and removed next time; checks the `.sha256` |
| `python tools/rip_disc.py list / rip E: OUT.iso` | a disc's sectors (the size its ISO 9660 volume gives) to an .iso, retries, then the check |
| `python tools/deps.py list / fetch [DIR] / check [DIR]` | the FetchContent pins read from the CMake files (one list); nlohmann/json from its release archive (0.2 MB instead of a 188 MB clone) |
| `python tools/release.py [--ref vX / --worktree] [--version X] [--launcher EXE]` | the release zip |
| `python tools/vcs_disc.py check ISO [--json]` | the disc check, for scripts |

The launcher's command line: `--check`, `--build [--iso X]`, `--play [--iso X]`, `--self-test`, `--out FILE` (a GUI
exe prints to the terminal that started it only through AttachConsole; `--out` for scripts and CI), and
`--screenshot F.png [--step N] [--demo running|error] [--action build --every S] [--iso X]`: the window rendered
hidden into a texture and saved - tests never put a window on the desktop.

## Checked (2026-10-30)

- `--self-test` in the dev tree and in the release folder (repository, Python - the bundled one there -, doctor's
  JSON, the disc checks).
- Disc checks: the USA disc ok, the Liberty City Stories (USA) disc named as such (both by vcs_disc.py and the
  launcher), a text file, a missing file, a `.chd` name.
- Release: `release.py --worktree` 1 min (deps cached), the guard refusing a planted disc image, MIPS ELF, generated
  file and SLUS name. From the zip, extracted into an empty folder: the launcher's first start (Disc step), then
  `--action build`: every step green, *Play* (see "Numbers"); `--play` started the game (hidden, VCS_DATA_DIR).
- Update: 0.1.1-test applied over a 0.1.0-test folder: version, a dropped file removed, `build\vcs.ini` and a player's
  own file kept, the in-use Python files set aside and gone at the next run; a clone refuses (`git_checkout`); the
  GitHub check answers "no release published yet" (404) for now.
- Backups on a copy of the card (VCS_DATA_DIR): create, a slot file deleted, restore - files and both times identical.
- setup_vs.py: status; `install --dry-run` for no Visual Studio, missing clang, old clang (the commands it would run);
  the bootstrapper downloaded and its signature accepted, a tampered copy refused. **Not run for real** (it installs
  ~6 GB on the dev machine).
- rip_disc.py from an image file: a byte-identical copy in 10 s; the LCS image refused after copying. **No DVD drive on
  the dev machine**: reading `\\.\E:` itself is untested.
- 2026-10-31, from a player's report ("A tool could not run" at *Configuring the build*, a Python traceback): with
  Build Tools only, cmake is in Visual Studio's own folders, on the PATH `build_env` makes but not on the launcher's.
  The checks look on the former, Windows starts a child's program from the latter (CreateProcess ignores the `env=`
  PATH): FileNotFoundError. `toolchain.resolve` now gives cmake its full path (configure, `cmake --build`, the IOP
  tests), and a program that can't start is a `fail` (`no_cmake`), not a traceback. Checked with cmake taken off the
  process PATH: the old code fails like the report, the new one runs. The dev machine never saw it: scoop's cmake is
  on its PATH.

## Numbers

| | |
|---|---|
| Launcher build from scratch | 81 s (SDL3 + Dear ImGui), relinks 5 s |
| `doctor.py` | 0.8 s |
| Release zip | 82 MB; 1 min to make with the libraries cached, +17 s to fetch them |
| First build from the release, through the hidden launcher (i7-7700K, 8 threads, ThinLTO, offline) | **8 min 42 s**: configure 52 s (with the recompiler), MIPS -> C++ 36 s, configure again 41 s, compile + link 6 min 25 s (786 steps) |
| The release folder after it | 1.13 GB (build/ 0.75, generated/ 0.20, deps/ 0.12, python/ 0.02) |
| The built game started by `--play` (hidden, a test card) | boots from the disc, the intro movie 321 pictures, 0 dropped |

## Not done / next

- A run on a PC with nothing installed (Windows Sandbox: no Python, no Git, no Visual Studio) - the real test of
  setup_vs.py's install path.
- Code signing: SmartScreen warns about an unknown publisher (README says *More info > Run anyway*). An open-source
  signing service (SignPath's OSS programme) would remove that.
- The CI workflow is written, not run (it runs on the next push to GitHub); the first `v*` tag publishes the first
  release.
- Linux: the launcher's process / folder layer, and a Linux release (an AppImage-style bundle needs a different Python
  story).
