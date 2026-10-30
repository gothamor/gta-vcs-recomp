"""Shared helpers for the build scripts (tools/build.py, recompile.py, run.py, ...). Standard library only.

- Windows: finds the newest Visual Studio with the C++ x64 toolset (vswhere), takes the vcvars64 environment
  (cached in build/msvc_env.json) and builds with clang-cl + Ninja. Standalone CMake / Ninja / LLVM / JDK from
  scoop are preferred when installed.
- Linux: the newest LLVM 19+ (Debian / Ubuntu's /usr/lib/llvm-<N>/bin, apt.llvm.org, or the distro's own clang on
  PATH) goes first on PATH, so clang, clang++, ld.lld and llvm-profdata are one version; CMake and Ninja from PATH.
  `check_linux_deps()` lists the packages the build needs (docs/LINUX.md).
"""
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The build folder: build/ in the repository, or one chosen elsewhere (another drive...) - VCS_BUILD_DIR for one run,
# or kept in .vcs-build-dir (tools/build_dir.py, the launcher's Folder step). The built game, its vcs.ini, the CMake
# trees and the logs live there; the source and generated/ stay here. The test tools keep their output in build/.
DEFAULT_BUILD = os.path.join(ROOT, "build")
BUILD_POINTER = os.path.join(ROOT, ".vcs-build-dir")


def read_build_pointer():
    try:
        with open(BUILD_POINTER, encoding="utf-8") as f:
            value = f.read().strip()
        return os.path.abspath(value) if value else None
    except OSError:
        return None


BUILD = os.path.abspath(os.environ.get("VCS_BUILD_DIR") or read_build_pointer() or DEFAULT_BUILD)
BUILD_DIR = os.path.join(BUILD, "release")


def show(path):
    """A path for messages: relative to the repository when it is inside it, else absolute (the build folder can be on
    another drive, where os.path.relpath fails)."""
    path = os.path.abspath(path)
    try:
        rel = os.path.relpath(path, ROOT)
    except ValueError:
        return path
    return path if rel.startswith("..") else rel
IS_WINDOWS = platform.system() == "Windows"

# our progress lines and the tools' output interleave in order even when stdout is a pipe
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(line_buffering=True)
ISO_GLOB = "*Vice City Stories*.iso"


def exe(path):
    """`path` with the platform's executable suffix."""
    return path + ".exe" if IS_WINDOWS else path


def game_exe(build_dir):
    """vcs.exe of a build tree: the main one (build/release) puts it at the top of build/ (CMakeLists.txt), others
    (build/pgo-gen) in their own runtime folder."""
    if os.path.normcase(os.path.abspath(build_dir)) == os.path.normcase(os.path.abspath(BUILD_DIR)):
        return exe(os.path.join(BUILD, "vcs"))
    return exe(os.path.join(build_dir, "third_party", "PS2Recomp", "ps2xRuntime", "vcs"))


# where vcs.exe was built before it moved to build/ (2026-10-29): build.py takes the player's vcs.ini from there
OLD_GAME_DIR = os.path.join(BUILD_DIR, "third_party", "PS2Recomp", "ps2xRuntime")


# VCS_EXE=<path>: run another build (tools/run.py and everything on top of it), e.g. the instrumented PGO build
VCS_EXE = os.environ.get("VCS_EXE") or game_exe(BUILD_DIR)
RECOMP_EXE = exe(os.path.join(BUILD_DIR, "third_party", "PS2Recomp", "ps2xRecomp", "ps2_recomp"))
PY = [sys.executable, "-I"]


def tool(name):
    return os.path.join(ROOT, "tools", name)


# ---- events for the launcher ------------------------------------------------------------------------------------
# With --progress json (build.py) or VCS_PROGRESS=json the tools also print machine-readable lines, `@vcs {...}`, between
# their usual output: the launcher (launcher/, docs/LAUNCHER.md) reads those and shows the rest as the log. One JSON
# object per line: {"stage": id, "title": text}, {"progress": done, "total": n, "eta": s, "note": text},
# {"error": code, "message": text, "log": path}, {"result": "ok", ...}. Child tools inherit the mode (environment).
EVENT_PREFIX = "@vcs "


def json_progress():
    return os.environ.get("VCS_PROGRESS") == "json"


def set_json_progress():
    os.environ["VCS_PROGRESS"] = "json"


def event(**fields):
    if json_progress():
        print(EVENT_PREFIX + json.dumps(fields), flush=True)


def stage(stage_id, title):
    """A new step of a long run, for the launcher (the tools print their own `[tag] ...` line for people)."""
    event(stage=stage_id, title=title)


def fail(message, code="error", log=None):
    """Stops with `error: message`. `code` names the failure for the launcher (its table of messages and fixes)."""
    event(error=code, message=message, log=show(log).replace(os.sep, "/") if log else None)
    sys.exit("error: " + message)


def ini_value(path, key):
    """`key=value` from an ini-style file (vcs.ini), None when absent."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                k, sep, value = line.partition("=")
                if sep and k.strip().lower() == key and value.strip():
                    return value.strip().strip('"')
    except OSError:
        pass
    return None


# vcs.ini next to vcs.exe (the player's settings and iso=), and the repository root's (older setups)
GAME_INI = os.path.join(BUILD, "vcs.ini")
ROOT_INI = os.path.join(ROOT, "vcs.ini")


def find_iso(given=None):
    """The disc image: `given`, VCS_ISO, iso=... in build/vcs.ini (the one vcs.exe reads) or the repository root's
    vcs.ini, or a '*Vice City Stories*.iso' in the repository root."""
    if given:
        if not os.path.isfile(given):
            fail("disc image not found: " + given, "disc_not_found")
        return os.path.abspath(given)
    if os.environ.get("VCS_ISO"):
        return find_iso(os.environ["VCS_ISO"])
    for ini in (GAME_INI, ROOT_INI):
        value = ini_value(ini, "iso")
        if value:
            return find_iso(value)
    found = sorted(glob.glob(os.path.join(ROOT, ISO_GLOB)))
    if found:
        return found[0]
    fail("no disc image: pass your Vice City Stories (USA) .iso, set VCS_ISO, or put the .iso in the repository root",
         "no_disc")


# ---- the source's identity --------------------------------------------------------------------------------------
def source_version():
    """VERSION (a release's number) and the git commit when this is a clone: {"version": "0.1.0", "commit": "abc..."}
    ("" for what isn't there). Read from the files, so it works without git installed."""
    version = ""
    try:
        with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
            version = f.read().strip()
    except OSError:
        pass
    return {"version": version, "commit": git_commit()}


def git_commit():
    git = os.path.join(ROOT, ".git")
    if os.path.isfile(git):  # a worktree: "gitdir: <path>"
        with open(git, encoding="utf-8") as f:
            git = f.read().partition(":")[2].strip()
    try:
        with open(os.path.join(git, "HEAD"), encoding="utf-8") as f:
            head = f.read().strip()
    except OSError:
        return ""
    if not head.startswith("ref:"):
        return head
    ref = head[4:].strip()
    common = git
    try:  # a worktree keeps its refs in the main repository (commondir)
        with open(os.path.join(git, "commondir"), encoding="utf-8") as f:
            common = os.path.normpath(os.path.join(git, f.read().strip()))
    except OSError:
        pass
    for base in (git, common):
        try:
            with open(os.path.join(base, ref), encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            pass
        try:
            with open(os.path.join(base, "packed-refs"), encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 2 and parts[1] == ref:
                        return parts[0]
        except OSError:
            pass
    return ""


def test_window(env, visible=False):
    """Test runs are hidden by default (the user's choice, 2026-10-24): no window on the desktop, physical input
    ignored, driven through the control channel (tools/gamectl.py). Sound stays on (PS2X_AUDIO=1; the user mutes the
    game in Windows). visible=True or VCS_TEST_VISIBLE=1 shows the window."""
    if visible or os.environ.get("VCS_TEST_VISIBLE") == "1":
        env.pop("PS2X_HIDDEN", None)
        return env
    env["PS2X_HIDDEN"] = "1"
    env.setdefault("PS2X_AUDIO", "1")
    # the timing the regression baselines and checks were made with: original, 30 fps. Players' default is 60 with
    # frame-rate independent timing (2026-10-30); a test that wants another sets VCS_TIMING / VCS_FPS itself, and one
    # that tests the setting (controls_check: the presets) passes VCS_TIMING="" - set, so not pinned, and empty, so
    # the game ignores it and the setting stays unlocked
    env.setdefault("VCS_TIMING", "0")
    env.setdefault("VCS_FPS", "30")
    return env


def tail(path, lines=20):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return ""


def read_png(path):
    """An 8-bit RGB / RGBA PNG (the game's screenshots) without Pillow: (width, height, rows of RGB bytes)."""
    import struct
    import zlib
    data = open(path, "rb").read()
    pos, idat, width, height, colour = 8, b"", 0, 0, 2
    while pos < len(data):
        length, = struct.unpack(">I", data[pos:pos + 4])
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, _, colour = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    bpp = 4 if colour == 6 else 3
    stride = width * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):  # the PNG filters
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], (prev[x - bpp] if x >= bpp else 0)
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line) if bpp == 3 else bytes(v for k, v in enumerate(line) if k % 4 != 3))
        prev = line
    return width, height, rows


# ---- progress ---------------------------------------------------------------------------------------------------
def _duration(seconds):
    seconds = int(seconds)
    return "%dh%02dm" % (seconds // 3600, seconds // 60 % 60) if seconds >= 3600 else "%dm%02ds" % (seconds // 60, seconds % 60)


class Progress:
    """One status line for a long step: `[build] 1234/10512 11%  3m12s, ~24m left  compiling`.

    On a terminal the line is redrawn in place every second (a timer keeps it moving while the tool is quiet, e.g.
    a ThinLTO link); when stdout is a pipe or a file it prints a line at every 5% and at least once a minute.
    Steps without a count (CMake configure, the recompiler) show the elapsed time only. Use as a context manager.
    """

    def __init__(self, label):
        self.label = label
        self.done = self.total = 0
        self.note = ""
        self.start = time.time()
        self.tty = sys.stdout.isatty()
        self.lock = threading.Lock()
        self.shown = ""  # what the in-place line holds (a terminal)
        self.last_print = self.start
        self.last_pct = -5
        self.stop = threading.Event()
        self.timer = threading.Thread(target=self._tick, daemon=True)

    def __enter__(self):
        self.timer.start()
        return self

    def __exit__(self, kind, value, trace):
        self.stop.set()
        self.timer.join()
        with self.lock:
            self._clear()

    def _text(self):
        elapsed = time.time() - self.start
        if self.total:
            text = "%s %d/%d %d%%  %s" % (self.label, self.done, self.total, 100 * self.done // self.total, _duration(elapsed))
            if 0 < self.done < self.total and elapsed > 10:
                text += ", ~%s left" % _duration(elapsed * (self.total - self.done) / self.done)
        else:
            text = "%s %s" % (self.label, _duration(elapsed))
        return text + ("  " + self.note if self.note else "")

    def _clear(self):
        if self.tty and self.shown:
            sys.stdout.write("\r" + " " * len(self.shown) + "\r")
            sys.stdout.flush()
            self.shown = ""

    def _draw(self, force=False):
        now = time.time()
        if self.tty:
            text = self._text()
            width = shutil.get_terminal_size((120, 20)).columns - 1
            text = text[:width]
            sys.stdout.write("\r" + text + " " * max(0, len(self.shown) - len(text)))
            sys.stdout.flush()
            self.shown = text
            return
        pct = 100 * self.done // self.total if self.total else -1
        if force or pct >= self.last_pct + 5 or now - self.last_print >= 60:
            print(self._text())
            self.last_print = now
            if pct >= 0:
                self.last_pct = pct - pct % 5

    def _tick(self):
        while not self.stop.wait(1.0):
            with self.lock:
                self._draw()

    def update(self, done=None, total=None, note=None):
        with self.lock:
            if done is not None:
                self.done = done
            if total is not None:
                self.total = total
            if note is not None:
                self.note = note
            self._draw()
            self._event()

    def _event(self):
        """A progress event for the launcher, at most 4 a second (Ninja steps come by the hundred)."""
        now = time.time()
        if not json_progress() or (now - getattr(self, "_last_event", 0) < 0.25 and self.done < self.total):
            return
        self._last_event = now
        elapsed = now - self.start
        eta = int(elapsed * (self.total - self.done) / self.done) if 0 < self.done < self.total and elapsed > 10 else None
        event(progress=self.done, total=self.total, eta=eta, note=self.note)

    def message(self, text):
        """A line printed above the status line (errors, link steps)."""
        with self.lock:
            self._clear()
            print(text, end="" if text.endswith("\n") else "\n")
            self._draw()

    def summary(self):
        return "%s %s" % (self.label, _duration(time.time() - self.start)) + (" (%d steps)" % self.total if self.total else "")


def run_with_progress(cmd, log, label, cwd=ROOT, env=None):
    """Runs `cmd` with its output to `log` under an elapsed-time line; returns the exit code."""
    cmd = resolve(cmd, env)
    with open(log, "w", encoding="utf-8") as f, Progress(label) as progress:
        try:
            code = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        except OSError as e:
            fail("could not start %s: %s" % (cmd[0], e), "no_cmake" if _is_cmake(cmd) else "error")
    print(progress.summary())
    return code


# ---- compiler environment ---------------------------------------------------------------------------------------
def _scoop_paths():
    home = os.path.expanduser("~")
    paths = [os.path.join(home, "scoop", "shims"), os.path.join(home, "scoop", "apps", "llvm", "current", "bin")]
    jdk = os.path.join(home, "scoop", "apps", "temurin21-jdk", "current")
    if os.path.isdir(jdk):
        paths.append(os.path.join(jdk, "bin"))
    return [p for p in paths if os.path.isdir(p)], jdk if os.path.isdir(jdk) else None


def _vs_root():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer",
                           "vswhere.exe")
    if not os.path.isfile(vswhere):
        fail("vswhere.exe not found: install Visual Studio 2022 or newer with the C++ workload", "no_vs")
    out = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    if not out:
        fail("no Visual Studio with the C++ x64 toolset found", "no_cpp_tools")
    return out.splitlines()[0]


MIN_CLANG = 19  # PS2_GUEST_CC's preserve_none (ps2_guest_cc.h)


def clang_version(clang):
    """The major version of a clang binary, 0 when it doesn't run."""
    try:
        out = subprocess.run([clang, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r"clang version (\d+)", out)
    return int(m.group(1)) if m else 0


def _linux_llvm_bin(env):
    """The bin folder of the newest LLVM >= MIN_CLANG: /usr/lib/llvm-<N>/bin (Debian / Ubuntu) or clang on PATH."""
    found = []
    for d in glob.glob("/usr/lib/llvm-*/bin") + glob.glob("/usr/lib64/llvm*/bin"):
        clang = os.path.join(d, "clang")
        if os.path.isfile(clang):
            found.append((clang_version(clang), d))
    on_path = _which("clang", env)
    if on_path:
        found.append((clang_version(on_path), os.path.dirname(os.path.realpath(on_path))))
    found = [f for f in found if f[0] >= MIN_CLANG]
    return max(found)[1] if found else None


def build_env():
    """Environment for CMake / Ninja / the compiler."""
    env = dict(os.environ)
    if not IS_WINDOWS:
        llvm = _linux_llvm_bin(env)
        if llvm:
            env["PATH"] = os.pathsep.join([llvm, env.get("PATH", "")])
        return env
    cache = os.path.join(BUILD, "msvc_env.json")
    vs = _vs_root()
    loaded = None
    if os.path.isfile(cache):
        try:
            with open(cache, encoding="utf-8") as f:
                data = json.load(f)
            if data.get("vs") == vs:
                loaded = data["env"]
        except (OSError, ValueError, KeyError):
            loaded = None
    if loaded is None:
        vcvars = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat")
        # one string: CreateProcess passes it to cmd as written (cmd's own quoting rules, no list escaping)
        result = subprocess.run(f'cmd /s /c ""{vcvars}" >nul 2>nul && set"', capture_output=True, text=True)
        if result.returncode != 0:
            fail("vcvars64.bat failed", "vcvars_failed")
        loaded = {}
        for line in result.stdout.splitlines():
            key, sep, value = line.partition("=")
            if sep and key:
                loaded[key] = value
        os.makedirs(BUILD, exist_ok=True)
        with open(cache, "w", encoding="utf-8") as f:
            json.dump({"vs": vs, "env": loaded}, f)
        print(f"[toolchain] Visual Studio: {vs}")
    env = {k.upper(): v for k, v in loaded.items()}
    front, jdk = _scoop_paths()
    back = [os.path.join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", p) for p in ("CMake\\bin", "Ninja")]
    env["PATH"] = os.pathsep.join(front + [env.get("PATH", "")] + [p for p in back if os.path.isdir(p)])
    if jdk:
        env["JAVA_HOME"] = jdk
    clang = _which("clang-cl", env)
    if not clang:
        fail('clang-cl not found: in the Visual Studio Installer, Modify, tick "C++ Clang tools for Windows"', "no_clang")
    if clang_version(clang) < MIN_CLANG:
        fail("clang-cl %d or newer needed (found %d at %s): update Visual Studio (Visual Studio Installer, Update)"
             % (MIN_CLANG, clang_version(clang), clang), "old_clang")
    return env


def _which(name, env):
    return shutil.which(name, path=env.get("PATH"))


def resolve(cmd, env):
    """`cmd` with a bare program name replaced by its full path on `env`'s PATH. On Windows a child's program is looked
    up on the PATH of this process, not the one passed as env= (CreateProcess): a cmake that only build_env's Visual
    Studio folders hold (Build Tools without a standalone CMake) was found by the checks and then failed to start with
    FileNotFoundError."""
    if env is None or os.path.dirname(cmd[0]):
        return cmd
    found = _which(cmd[0], env)
    return [found] + list(cmd[1:]) if found else cmd


def _is_cmake(cmd):
    return os.path.splitext(os.path.basename(cmd[0]))[0].lower() == "cmake"


# ---- CMake ------------------------------------------------------------------------------------------------------
def configure(env, force=False, build_dir=BUILD_DIR, defines=None):
    """CMake configure of `build_dir` (first time, when forced, or with `defines`: {"PS2X_LTO": "THIN", ...})."""
    force = drop_foreign_cache(build_dir, ROOT) or force
    if os.path.isfile(os.path.join(build_dir, "build.ninja")) and not force and not defines:
        return
    for name in ("cmake", "ninja"):
        if not _which(name, env):
            fail(f'{name} not found on PATH (Visual Studio\'s "C++ CMake tools for Windows" bring both)', "no_cmake")
    cmd = ["cmake", "-S", ROOT, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"]
    cmd += ["-D%s=%s" % (k, v) for k, v in (defines or {}).items()]
    cmd += compiler_defines(env) + deps_defines()
    if os.path.normcase(os.path.abspath(build_dir)) == os.path.normcase(BUILD_DIR):
        cmd.append("-DVCS_GAME_DIR=" + BUILD.replace(os.sep, "/"))  # vcs.exe at the top of the build folder
    print("[build] configuring " + show(build_dir) + "".join(" %s=%s" % kv for kv in (defines or {}).items()))
    stage("configure", "Configuring the build (CMake)")
    log = os.path.join(BUILD, "configure.log")
    if run_with_progress(cmd, log, "[build] configure", env=env) != 0:
        print(tail(log, 30))
        fail("CMake configure failed (full log: %s)" % show(log), configure_failure(log), log)


def drop_foreign_cache(build_dir, source):
    """A build tree configured from another copy of the source (a build folder reused - tools/build_dir.py - or a
    moved source folder) makes CMake stop ("does not match the source used to generate cache"): its cache goes, and
    the tree is set up again (Ninja then rebuilds what changed). True when that happened."""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    home = None
    try:
        with open(cache, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    home = line.partition("=")[2].strip()
                    break
    except OSError:
        return False
    if not home or os.path.normcase(os.path.abspath(home)) == os.path.normcase(os.path.abspath(source)):
        return False
    print("[build] %s was set up from another source folder (%s): setting it up again" % (show(build_dir), home))
    os.remove(cache)
    shutil.rmtree(os.path.join(build_dir, "CMakeFiles"), ignore_errors=True)
    return True


DEPS_DIR = os.path.join(ROOT, "deps")


def deps_defines(deps_dir=DEPS_DIR):
    """A release carries the sources CMake would download (deps/, made by tools/deps.py): each folder there replaces
    its FetchContent download (FETCHCONTENT_SOURCE_DIR_<NAME>), the FFmpeg archive its URL - no git, no internet.
    Nothing when deps/ is absent: a clone downloads as before."""
    if not os.path.isdir(deps_dir):
        return []
    defines = []
    for name in sorted(os.listdir(deps_dir)):
        path = os.path.join(deps_dir, name)
        if os.path.isdir(path):
            defines.append("-DFETCHCONTENT_SOURCE_DIR_%s=%s" % (name.upper(), path.replace(os.sep, "/")))
        elif name.startswith("ffmpeg-") and name.endswith(".zip"):
            defines.append("-DFFMPEG_PREBUILT_URL=" + path.replace(os.sep, "/"))
    if os.path.isfile(os.path.join(deps_dir, "manifest.json")):  # complete (tools/deps.py fetch): never download
        defines.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    return defines


def configure_failure(log):
    """The launcher's name for why a configure failed, read from its log."""
    text = tail(log, 200).lower()
    if "no space left" in text or "not enough space" in text:
        return "disk_full"
    if any(w in text for w in ("failed to clone", "could not resolve host", "unable to access", "download failed",
                               "couldn't connect", "timed out", "could not find git", "git_executable")):
        return "download_failed"
    if "clang-cl" in text:
        return "no_clang"
    return "configure_failed"


def compiler_defines(env):
    """-DCMAKE_C(XX)_COMPILER for a fresh build tree: clang-cl on Windows, clang 19+ elsewhere (build_env put it first
    on PATH)."""
    if IS_WINDOWS:
        return ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    clang, clangxx = _which("clang", env), _which("clang++", env)
    if not clang or not clangxx or clang_version(clang) < MIN_CLANG:
        fail("clang %d or newer not found - install it (docs/LINUX.md; e.g. sudo apt install clang-20 lld-20 llvm-20), "
             "or run python tools/build.py --check-deps" % MIN_CLANG)
    if not _which("ld.lld", env):
        fail("ld.lld not found (the LLVM linker, next to clang): e.g. sudo apt install lld-%d" % clang_version(clang))
    defines = ["-DCMAKE_C_COMPILER=" + clang, "-DCMAKE_CXX_COMPILER=" + clangxx]
    gcc = _gcc_with_libstdcxx(clangxx)
    if gcc:
        flag = "--gcc-install-dir=" + gcc
        defines += ["-DCMAKE_C_FLAGS=" + flag, "-DCMAKE_CXX_FLAGS=" + flag]
    return defines


def _gcc_with_libstdcxx(clangxx):
    """clang uses the newest GCC folder for libstdc++, even one without it: Ubuntu 24.04's clang-20 brings
    libgcc-14-dev but not libstdc++-14-dev, and links fail with "cannot find -lstdc++". The newest GCC folder that has
    libstdc++ then, or None when clang's own choice is fine."""
    try:
        out = subprocess.run([clangxx, "-v"], capture_output=True, text=True, timeout=30).stderr
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"^Selected GCC installation: (.+)$", out, re.M)
    if not m or os.path.isfile(os.path.join(m.group(1).strip(), "libstdc++.so")):
        return None
    candidates = re.findall(r"^Found candidate GCC installation: (.+)$", out, re.M)
    usable = [c.strip() for c in candidates if os.path.isfile(os.path.join(c.strip(), "libstdc++.so"))]
    if not usable:
        return None
    best = max(usable, key=lambda d: [int(x) for x in re.findall(r"\d+", os.path.basename(d))] or [0])
    print("[toolchain] clang's GCC folder %s has no libstdc++; using %s (or install libstdc++-%s-dev)"
          % (m.group(1).strip(), best, os.path.basename(m.group(1).strip())))
    return best


# ---- Linux packages ---------------------------------------------------------------------------------------------
# (what is checked, how, the package per distro family: Debian / Ubuntu, Fedora, Arch)
LINUX_DEPS = [
    ("cmake", "cmake", ("cmake", "cmake", "cmake")),
    ("ninja", "ninja", ("ninja-build", "ninja-build", "ninja")),
    ("git", "git", ("git", "git", "git")),
    ("pkg-config", "pkg-config", ("pkg-config", "pkgconf-pkg-config", "pkgconf")),
    ("glslangValidator", "glslangValidator", ("glslang-tools", "glslang", "glslang")),
    ("FFmpeg (movies)", "pc:libavcodec libavformat libavutil libswscale libswresample",
     ("libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev", "ffmpeg-free-devel", "ffmpeg")),
    # SDL3 is built from source (src/platform): without these it builds, silently, without a video / audio / input backend
    ("X11 (SDL3)", "pc:x11 xext xrandr xcursor xi xfixes xscrnsaver",
     ("libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev",
      "libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel libXScrnSaver-devel",
      "libx11 libxext libxrandr libxcursor libxi libxfixes libxss")),
    ("Wayland (SDL3)", "pc:wayland-client wayland-protocols xkbcommon",
     ("libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev", "wayland-devel wayland-protocols-devel "
      "libxkbcommon-devel libdecor-devel", "wayland wayland-protocols libxkbcommon libdecor")),
    ("audio (SDL3)", "pc:libpipewire-0.3 libpulse alsa",
     ("libpipewire-0.3-dev libpulse-dev libasound2-dev", "pipewire-devel pulseaudio-libs-devel alsa-lib-devel",
      "pipewire libpulse alsa-lib")),
    ("gamepads (SDL3)", "pc:libudev dbus-1", ("libudev-dev libdbus-1-dev", "systemd-devel dbus-devel", "systemd-libs dbus")),
]


def _linux_family():
    try:
        with open("/etc/os-release", encoding="utf-8") as f:
            text = f.read().lower()
    except OSError:
        return 0
    like = " ".join(re.findall(r"^(?:id|id_like)=\"?([^\"\n]*)", text, re.M))
    if any(w in like for w in ("fedora", "rhel", "centos")):
        return 1
    if any(w in like for w in ("arch", "manjaro", "endeavouros")):
        return 2
    return 0


def check_linux_deps(env):
    """Missing build dependencies on Linux, with the install command for this distro; True when nothing is missing."""
    missing = []
    for label, check, packages in LINUX_DEPS:
        if check.startswith("pc:"):
            mods = check[3:].split()
            ok = bool(_which("pkg-config", env)) and subprocess.run(["pkg-config", "--exists"] + mods, env=env).returncode == 0
        else:
            ok = bool(_which(check, env))
        if not ok:
            missing.append((label, packages))
    family = _linux_family()
    llvm = _linux_llvm_bin(env)
    if not llvm:
        missing.append(("clang %d+ / lld / llvm" % MIN_CLANG, ("clang-20 lld-20 llvm-20", "clang lld llvm", "clang lld llvm")))
    elif not os.path.isfile(os.path.join(llvm, "ld.lld")) and not _which("ld.lld", env):
        missing.append(("lld", ("lld-%d" % clang_version(os.path.join(llvm, "clang")), "lld", "lld")))
    if not missing:
        print("[deps] all build dependencies found" + (" (LLVM: %s)" % llvm if llvm else ""))
        return True
    for label, _ in missing:
        print("[deps] missing: " + label)
    pkgs = " ".join(p[family] for _, p in missing)
    cmd = ("sudo apt install " if family == 0 else "sudo dnf install " if family == 1 else "sudo pacman -S --needed ") + pkgs
    print("[deps] install them with:\n    " + cmd)
    return False


_INTERESTING = re.compile(r"error|FAILED|Linking|warning: unused")


_NINJA_STEP = re.compile(r"^\[(\d+)/(\d+)\] (\S+)")


def cmake_build(env, target, log_name, verbose=False, build_dir=BUILD_DIR):
    """Builds `target` under a progress line (Ninja's [done/total]); prints errors and link steps (everything with
    verbose). False on failure."""
    log = os.path.join(BUILD, log_name)
    os.makedirs(BUILD, exist_ok=True)
    cmd = resolve(["cmake", "--build", build_dir, "--target", target], env)
    with open(log, "w", encoding="utf-8") as f, Progress("[build] " + target) as progress:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        for line in proc.stdout:
            f.write(line)
            step = _NINJA_STEP.match(line)
            if step:
                verb = step.group(3)
                progress.update(int(step.group(1)), int(step.group(2)),
                                "linking" if verb == "Linking" else "compiling" if verb == "Building" else "")
            if verbose or (_INTERESTING.search(line) and "Wdeprecated" not in line):
                progress.message(line)
        ok = proc.wait() == 0
    if ok:
        print(progress.summary() if progress.total else "[build] %s up to date" % target)
    else:
        print(f"[build] {target} failed (full log: {show(log)})")
        event(error=build_failure(log), message=f"building {target} failed", log=show(log).replace(os.sep, "/"))
    return ok


def build_failure(log):
    """The launcher's name for why a build failed, read from its log."""
    text = tail(log, 400).lower()
    if "no space left" in text or "not enough space" in text or "disk full" in text:
        return "disk_full"
    if "file name too long" in text or "filename too long" in text or "path too long" in text:
        return "path_too_long"
    if "being used by another process" in text or "permission denied" in text or "access is denied" in text:
        return "file_locked"
    if "out of memory" in text or "bad_alloc" in text or "not enough memory" in text:
        return "out_of_memory"
    return "build_failed"
