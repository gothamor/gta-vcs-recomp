"""Shared helpers for the build scripts (tools/build.py, recompile.py, run.py, ...). Standard library only.

- Windows: finds the newest Visual Studio with the C++ x64 toolset (vswhere), takes the vcvars64 environment
  (cached in build/msvc_env.json) and builds with clang-cl + Ninja. Standalone CMake / Ninja / LLVM / JDK from
  scoop are preferred when installed.
- Elsewhere: the current environment, clang / clang++ when available.
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
BUILD = os.path.join(ROOT, "build")
BUILD_DIR = os.path.join(BUILD, "release")
IS_WINDOWS = platform.system() == "Windows"

# our progress lines and the tools' output interleave in order even when stdout is a pipe
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(line_buffering=True)
ISO_GLOB = "*Vice City Stories*.iso"


def exe(path):
    """`path` with the platform's executable suffix."""
    return path + ".exe" if IS_WINDOWS else path


def game_exe(build_dir):
    return exe(os.path.join(build_dir, "third_party", "PS2Recomp", "ps2xRuntime", "vcs"))


# VCS_EXE=<path>: run another build (tools/run.py and everything on top of it), e.g. the instrumented PGO build
VCS_EXE = os.environ.get("VCS_EXE") or game_exe(BUILD_DIR)
RECOMP_EXE = exe(os.path.join(BUILD_DIR, "third_party", "PS2Recomp", "ps2xRecomp", "ps2_recomp"))
PY = [sys.executable, "-I"]


def tool(name):
    return os.path.join(ROOT, "tools", name)


def fail(message):
    sys.exit("error: " + message)


def find_iso(given=None):
    """The disc image: `given`, VCS_ISO, iso=... in vcs.ini, or a '*Vice City Stories*.iso' in the repository root."""
    if given:
        if not os.path.isfile(given):
            fail("disc image not found: " + given)
        return os.path.abspath(given)
    if os.environ.get("VCS_ISO"):
        return find_iso(os.environ["VCS_ISO"])
    ini = os.path.join(ROOT, "vcs.ini")
    if os.path.isfile(ini):
        with open(ini, encoding="utf-8", errors="replace") as f:
            for line in f:
                key, _, value = line.partition("=")
                if key.strip().lower() == "iso" and value.strip():
                    return find_iso(value.strip().strip('"'))
    found = sorted(glob.glob(os.path.join(ROOT, ISO_GLOB)))
    if found:
        return found[0]
    fail("no disc image: pass your Vice City Stories (USA) .iso, set VCS_ISO, or put the .iso in the repository root")


def tail(path, lines=20):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return ""


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
    with open(log, "w", encoding="utf-8") as f, Progress(label) as progress:
        code = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
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
        fail("vswhere.exe not found: install Visual Studio 2022 or newer with the C++ workload")
    out = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    if not out:
        fail("no Visual Studio with the C++ x64 toolset found")
    return out.splitlines()[0]


def build_env():
    """Environment for CMake / Ninja / the compiler."""
    env = dict(os.environ)
    if not IS_WINDOWS:
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
            fail("vcvars64.bat failed")
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
    return env


def _which(name, env):
    return shutil.which(name, path=env.get("PATH"))


# ---- CMake ------------------------------------------------------------------------------------------------------
def configure(env, force=False, build_dir=BUILD_DIR, defines=None):
    """CMake configure of `build_dir` (first time, when forced, or with `defines`: {"PS2X_LTO": "THIN", ...})."""
    if os.path.isfile(os.path.join(build_dir, "build.ninja")) and not force and not defines:
        return
    for name in ("cmake", "ninja"):
        if not _which(name, env):
            fail(f"{name} not found on PATH")
    cmd = ["cmake", "-S", ROOT, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"]
    cmd += ["-D%s=%s" % (k, v) for k, v in (defines or {}).items()]
    if IS_WINDOWS:
        cmd += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    elif _which("clang", env) and _which("clang++", env):
        cmd += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
    print("[build] configuring " + os.path.relpath(build_dir, ROOT) + "".join(" %s=%s" % kv for kv in (defines or {}).items()))
    log = os.path.join(BUILD, "configure.log")
    if run_with_progress(cmd, log, "[build] configure", env=env) != 0:
        print(tail(log, 30))
        fail("CMake configure failed (full log: build/configure.log)")


_INTERESTING = re.compile(r"error|FAILED|Linking|warning: unused")


_NINJA_STEP = re.compile(r"^\[(\d+)/(\d+)\] (\S+)")


def cmake_build(env, target, log_name, verbose=False, build_dir=BUILD_DIR):
    """Builds `target` under a progress line (Ninja's [done/total]); prints errors and link steps (everything with
    verbose). False on failure."""
    log = os.path.join(BUILD, log_name)
    os.makedirs(BUILD, exist_ok=True)
    cmd = ["cmake", "--build", build_dir, "--target", target]
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
        print(f"[build] {target} failed (full log: build/{log_name})")
    return ok
