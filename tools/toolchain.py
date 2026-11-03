"""Shared helpers for the build scripts (tools/recompile.py, ps5.py, release.py, ...). Standard library only.

The build machine is Ubuntu 24.04 or WSL 2 (the PS5 payload SDK's tools run there): the newest LLVM 19+ (Ubuntu's
/usr/lib/llvm-<N>/bin, apt.llvm.org, or clang on PATH) goes first on PATH, so clang, clang++, ld.lld and llvm-profdata
are one version; CMake and Ninja: the first new enough one on PATH or in the usual places (sudo apt install cmake
ninja-build), else the projects' own Linux builds, downloaded once (pinned, SHA-256 checked) into build/build-tools.
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
# The build folder: build/ in the repository, or one chosen elsewhere (another drive with more room) - VCS_BUILD_DIR for
# one run, or a path in .vcs-build-dir. The CMake trees, the PS5 app folder and the logs live there; the source and
# generated/ stay here.
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
# the build machine's tools tree: the recompiler (tools/recompile.py)
BUILD_DIR = os.path.join(BUILD, "release")


def show(path):
    """A path for messages: relative to the repository when it is inside it, else absolute."""
    path = os.path.abspath(path)
    try:
        rel = os.path.relpath(path, ROOT)
    except ValueError:
        return path
    return path if rel.startswith("..") else rel


# our progress lines and the tools' output interleave in order even when stdout is a pipe
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(line_buffering=True)
ISO_GLOB = "*Vice City Stories*.iso"

RECOMP_EXE = os.path.join(BUILD_DIR, "third_party", "PS2Recomp", "ps2xRecomp", "ps2_recomp")
PY = [sys.executable, "-I"]


def tool(name):
    return os.path.join(ROOT, "tools", name)


def fail(message, code="error", log=None):
    """Stops with `error: message`. (`code` and `log` name the failure for callers that keep them.)"""
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


ROOT_INI = os.path.join(ROOT, "vcs.ini")


def find_iso(given=None):
    """The disc image: `given`, VCS_ISO, iso=... in the repository root's vcs.ini, or a '*Vice City Stories*.iso' in the
    repository root."""
    if given:
        if not os.path.isfile(given):
            fail("disc image not found: " + given, "disc_not_found")
        return os.path.abspath(given)
    if os.environ.get("VCS_ISO"):
        return find_iso(os.environ["VCS_ISO"])
    value = ini_value(ROOT_INI, "iso")
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
    """Runs `cmd` with its output to `log` under an elapsed-time line; returns the exit code. A failure ends the log
    with how the program ended (describe_exit): a crash leaves nothing else there."""
    cmd = resolve(cmd, env)
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)  # a fresh checkout has no build/ yet
    with open(log, "w", encoding="utf-8") as f, Progress(label) as progress:
        try:
            code = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        except OSError as e:
            fail("could not start %s: %s" % (cmd[0], e))
        if code != 0:
            f.write("\n[%s] %s ended after %s: %s\n" % (os.path.basename(sys.argv[0]) or "tools",
                                                       os.path.basename(cmd[0]), _duration(time.time() - progress.start),
                                                       describe_exit(code)))
    print(progress.summary())
    return code


def describe_exit(code):
    """A line saying how a program ended: "exit code 1", or "killed by SIGILL (...)" - a crash prints nothing itself
    (a recompiler built for a newer CPU stopped at its first instruction, with an empty log)."""
    if code is None:
        return "no exit code"
    if code < 0:  # killed by a signal
        import signal
        try:
            name = signal.Signals(-code).name
        except ValueError:
            name = "signal %d" % -code
        hint = {"SIGILL": "an instruction this CPU doesn't have", "SIGSEGV": "the program crashed",
                "SIGKILL": "killed (out of memory?)", "SIGABRT": "the program stopped itself"}.get(name, "")
        return "killed by %s%s" % (name, " (%s)" % hint if hint else "")
    return "exit code %d" % code


# ---- Ghidra (the analysis tools: ghidra_analyze.py, eedecomp.py, irxdis.py) ---------------------------------------
GHIDRA_VERSION = "12.1.4"  # the analysed EE project and the ghidra-emotionengine-reloaded extension are for this one


def ghidra_home(env_var="GHIDRA_HOME"):
    """The Ghidra install the analysis tools use: `env_var`, else GHIDRA_VERSION's release folder as its zip unpacks
    (ghidra_<version>_PUBLIC) in the home folder or /opt. A newer Ghidra wants a newer JDK (12.2: JDK 25)."""
    if os.environ.get(env_var):
        return os.environ[env_var]
    for base in (os.path.expanduser("~"), "/opt"):
        found = sorted(d for d in glob.glob(os.path.join(base, "ghidra_%s*" % GHIDRA_VERSION)) if os.path.isdir(d))
        if found:
            return found[-1]
    return os.path.join(os.path.expanduser("~"), "ghidra_%s_PUBLIC" % GHIDRA_VERSION)


# ---- compiler environment ---------------------------------------------------------------------------------------
MIN_CLANG = 19  # PS2_GUEST_CC's preserve_none (ps2_guest_cc.h) and the PS5 target


def clang_version(clang):
    """The major version of a clang binary, 0 when it doesn't run."""
    try:
        out = subprocess.run([clang, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r"clang version (\d+)", out)
    return int(m.group(1)) if m else 0


def _llvm_bin(env):
    """The bin folder of the newest LLVM >= MIN_CLANG: /usr/lib/llvm-<N>/bin (Ubuntu) or clang on PATH."""
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
    """Environment for CMake / Ninja / the compiler: the newest LLVM 19+ first on PATH."""
    env = dict(os.environ)
    llvm = _llvm_bin(env)
    if llvm:
        env["PATH"] = os.pathsep.join([llvm, env.get("PATH", "")])
    return env


def _which(name, env):
    return shutil.which(name, path=env.get("PATH") if env else None)


def resolve(cmd, env):
    """`cmd` with a bare program name replaced by its full path (cmake: build_tools' choice)."""
    if env is None or os.path.dirname(cmd[0]):
        return cmd
    if cmd[0] == "cmake":
        return [build_tools(env)["cmake"]] + list(cmd[1:])
    found = _which(cmd[0], env)
    return [found] + list(cmd[1:]) if found else cmd


# ---- CMake and Ninja --------------------------------------------------------------------------------------------
# build_tools() takes the first CMake >= MIN_CMAKE and Ninja >= MIN_NINJA that run: env's PATH first, then the places
# installers and pip leave them, and downloads the projects' own Linux builds (pinned, SHA-256 checked) into
# build/build-tools when none will do. The build runs them by full path (resolve, CMAKE_MAKE_PROGRAM), never by name.
# VCS_CMAKE / VCS_NINJA choose them by hand.
MIN_CMAKE = (3, 21)  # cmake_minimum_required in CMakeLists.txt
MIN_NINJA = (1, 10)
TOOLS_DIR = os.path.join(BUILD, "build-tools")
# machine: (url, sha256, the folder it is unpacked to). The CMake hashes are Kitware's (cmake-4.4.5-SHA-256.txt),
# Ninja publishes none: hashed when pinned.
CMAKE_DOWNLOADS = {
    "linux-x86_64": ("https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-linux-x86_64.tar.gz",
                     "ed85135c5a5135b45e50423cb406dea04ca3338da0b8ef3c9ede963559680be0", "cmake-4.4.5-linux-x86_64"),
    "linux-aarch64": ("https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-linux-aarch64.tar.gz",
                      "301d5063ab259577c1b57ce5490b1aab9ea5c56dc4c822f73499553dab94925f", "cmake-4.4.5-linux-aarch64"),
}
NINJA_DOWNLOADS = {
    "linux-x86_64": ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip",
                     "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6", "ninja-1.13.2"),
    "linux-aarch64": ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux-aarch64.zip",
                      "fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc", "ninja-1.13.2"),
}
_PROBED = {}  # (tool, path) -> version tuple or None: each candidate runs once per process


def download_platform():
    """The key of CMAKE_DOWNLOADS / NINJA_DOWNLOADS for this machine, None when there is no build for it."""
    if platform.system() == "Linux":
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            return "linux-x86_64"
        if machine in ("aarch64", "arm64"):
            return "linux-aarch64"
    return None


def _probe(tool, path, run):
    key = (tool, os.path.abspath(path))
    if key not in _PROBED:
        _PROBED[key] = None
        try:
            _PROBED[key] = run(path)
        except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError):
            pass
    return _PROBED[key]


def cmake_version(path):
    """(major, minor, patch) of a CMake that runs and makes Ninja builds, else None."""
    def run(path):
        caps = json.loads(subprocess.run([path, "-E", "capabilities"], capture_output=True, text=True,
                                         timeout=60).stdout)
        if not any(g.get("name") == "Ninja" for g in caps.get("generators", [])):
            return None
        v = caps["version"]
        return int(v["major"]), int(v["minor"]), int(v["patch"])
    return _probe("cmake", path, run)


def ninja_version(path):
    """(major, minor, patch) of a Ninja that runs, else None."""
    def run(path):
        out = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=60).stdout
        m = re.match(r"(\d+)\.(\d+)(?:\.(\d+))?", out.strip())
        return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)) if m else None
    return _probe("ninja", path, run)


def _numbers(path):
    return [int(n) for n in re.findall(r"\d+", path)]


def _newest(pattern):
    """The folders matching `pattern`, the highest version number first."""
    return sorted(glob.glob(pattern), key=_numbers, reverse=True)


def _tool_dirs(env):
    """Where CMake and Ninja may be, in order: env's PATH, the usual places, pip's, and last build_tools' own
    downloads."""
    home = os.path.expanduser("~")
    dirs = [p.strip().strip('"') for p in env.get("PATH", "").split(os.pathsep)]
    dirs += ["/usr/local/bin", "/usr/bin", "/snap/bin", os.path.join(home, ".local", "bin")]
    dirs += _newest("/opt/cmake*/bin")
    import sysconfig
    for scheme in (None, os.name + "_user"):  # pip's cmake / ninja packages for this Python
        try:
            dirs.append(sysconfig.get_path("scripts", scheme) if scheme else sysconfig.get_path("scripts"))
        except KeyError:
            pass
    key = download_platform()
    for downloads, sub in ((CMAKE_DOWNLOADS, "bin"), (NINJA_DOWNLOADS, "")):
        if key in downloads:
            dirs.append(os.path.join(TOOLS_DIR, downloads[key][2], sub))
    unique, seen = [], set()
    for d in dirs:
        norm = os.path.abspath(d) if d else ""
        if norm and norm not in seen and os.path.isdir(d):
            seen.add(norm)
            unique.append(d)
    return unique


def _find_tool(names, probe, minimum, dirs):
    """(path, version, [(path, version) skipped]) of the first program called one of `names` in `dirs` that runs and
    is at least `minimum`."""
    skipped = []
    for d in dirs:
        for name in names:
            path = shutil.which(name, path=d)
            if not path:
                continue
            version = probe(path)
            if version and version >= minimum:
                return path, version, skipped
            skipped.append((path, version))
    return None, None, skipped


def _version_text(version):
    return ".".join(str(n) for n in version)


def _download_tool(title, downloads, probe, minimum, names):
    """Unpacks this machine's build from `downloads` into TOOLS_DIR (once) and returns its program; fails when there is
    none for this machine or it can't be fetched."""
    import hashlib
    import http.client
    import tarfile
    import urllib.request
    import zipfile
    key = download_platform()
    need = "%s %s or newer" % (title, _version_text(minimum))
    if key not in downloads:
        fail("%s not found and there is no build of it to download for this machine: sudo apt install cmake ninja-build"
             % need, "no_cmake")
    url, sha256, folder = downloads[key]
    target = os.path.join(TOOLS_DIR, folder)
    archive = os.path.join(TOOLS_DIR, url.rsplit("/", 1)[1])
    unpacked = target + ".unpack"
    print("[toolchain] no %s found: downloading %s" % (need, url))
    try:
        os.makedirs(TOOLS_DIR, exist_ok=True)
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=120) as r, open(archive + ".part", "wb") as f, \
                Progress("[toolchain] " + title) as progress:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                f.write(block)
                digest.update(block)
                done += len(block)
                progress.update(done >> 20, total >> 20, "MB")
    except (OSError, http.client.HTTPException) as e:
        fail("%s not found, and downloading it failed (%s): sudo apt install cmake ninja-build" % (need, e), "no_cmake")
    if digest.hexdigest() != sha256:
        os.remove(archive + ".part")
        fail("the %s download is not the file it should be (SHA-256 %s, expected %s): try again"
             % (title, digest.hexdigest(), sha256), "download_failed")
    os.replace(archive + ".part", archive)
    shutil.rmtree(unpacked, ignore_errors=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
    else:
        with tarfile.open(archive) as t:
            if hasattr(tarfile, "data_filter"):
                t.extractall(unpacked, filter="data")
            else:
                t.extractall(unpacked)
    os.remove(archive)
    inner = os.listdir(unpacked)  # CMake's archives hold one folder, Ninja's the program alone
    shutil.rmtree(target, ignore_errors=True)
    source = os.path.join(unpacked, inner[0]) if len(inner) == 1 and os.path.isdir(os.path.join(unpacked, inner[0])) \
        else unpacked
    os.replace(source, target)
    shutil.rmtree(unpacked, ignore_errors=True)
    for d in (os.path.join(target, "bin"), target):
        for name in names:
            path = os.path.join(d, name)
            if os.path.isfile(path):
                os.chmod(path, 0o755)  # zip keeps no file modes
                if (probe(path) or ()) >= minimum:
                    return path
    fail("the downloaded %s (%s) does not run" % (title, target), "no_cmake")


def build_tools(env, download=True):
    """The CMake and Ninja the build runs, {"cmake": path, "ninja": path, "cmake_version": (...), ...}: VCS_CMAKE /
    VCS_NINJA when set (checked), else the first good one _tool_dirs holds, else (download=True) a download. Also kept
    in env's VCS_CMAKE / VCS_NINJA, so later calls and child tools take the same."""
    found = {}
    for tool, title, names, probe, minimum, downloads in (
            ("cmake", "CMake", ["cmake"], cmake_version, MIN_CMAKE, CMAKE_DOWNLOADS),
            ("ninja", "Ninja", ["ninja", "ninja-build"], ninja_version, MIN_NINJA, NINJA_DOWNLOADS)):
        var = "VCS_" + tool.upper()
        given = env.get(var)
        if given:
            version = probe(given) if os.path.isfile(given) else None
            if not version or version < minimum:
                fail("%s=%s is not a %s %s or newer that runs (%s)" % (var, given, title, _version_text(minimum),
                     "version " + _version_text(version) if version else "not found or doesn't start"), "no_cmake")
            found[tool], found[tool + "_version"] = given, version
            continue
        path, version, skipped = _find_tool(names, probe, minimum, _tool_dirs(env))
        if not download:  # a check: silent, nothing kept
            found[tool], found[tool + "_version"] = path, version
            continue
        for old, old_version in skipped:
            print("[toolchain] skipped %s %s: %s" % (title, old, "version %s, %s needed" % (
                _version_text(old_version), _version_text(minimum)) if old_version else "doesn't run"))
        if not path:
            path = _download_tool(title, downloads, probe, minimum, names)
            version = probe(path)
        print("[toolchain] %s %s: %s" % (title, _version_text(version), path))
        env[var] = path
        found[tool], found[tool + "_version"] = path, version
    return found


def stale_tools(build_dir):
    """True when the CMake or Ninja a build tree was set up with is gone (uninstalled, another build folder): it can
    neither build nor set itself up again, so it is configured afresh with today's."""
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
            for line in f:
                key, sep, value = line.partition("=")
                if sep and key.split(":")[0] in ("CMAKE_COMMAND", "CMAKE_MAKE_PROGRAM") and value.strip():
                    if not os.path.isfile(value.strip()):
                        return True
    except OSError:
        pass
    return False


# ---- file dates -------------------------------------------------------------------------------------------------
# Files dated after the clock (a zip extracted in a time zone behind the packager's - zip times carry no zone - or a
# clock set back) are newer than anything a build writes: Ninja re-runs CMake for ever ("manifest 'build.ninja' still
# dirty after 100 tries"). The recompile sets them to now.
FUTURE_SLACK = 60  # seconds: a file written a moment ago on a drive with its own clock (network, FAT) is no problem
_NO_DATE_CHECK = {".git", "build", "generated"}  # ours (written by this machine), or no build input


def future_files(root=ROOT):
    """[(path, seconds ahead)] for the files under `root` dated after now (build folder, generated/ and .git not looked
    at)."""
    now = time.time()
    skip = {os.path.normcase(os.path.abspath(BUILD))}
    found, todo = [], [root]
    while todo:
        try:
            entries = list(os.scandir(todo.pop()))
        except OSError:
            continue
        for e in entries:
            try:
                if e.is_dir(follow_symlinks=False):
                    if not (e.name in _NO_DATE_CHECK and os.path.dirname(e.path) == root) and \
                            os.path.normcase(e.path) not in skip:
                        todo.append(e.path)
                elif e.is_file(follow_symlinks=False):
                    t = e.stat().st_mtime
                    if t > now + FUTURE_SLACK:
                        found.append((e.path, t - now))
            except OSError:
                continue
    return found


def fix_future_files(root=ROOT):
    """Sets the files future_files finds to now, with one line saying so. The number of files that stay in the
    future (could not be changed)."""
    found = future_files(root)
    if not found:
        return 0
    left = 0
    for path, _ in found:
        try:
            os.utime(path, None)
        except OSError:
            left += 1
    path, ahead = max(found, key=lambda f: f[1])
    print("[build] %d %s dated in the future (up to %s ahead, %s): set to now. A zip extracted in a time zone "
          "behind the one it was made in does this; so does a clock set back"
          % (len(found), "file was" if len(found) == 1 else "files were", _ahead(ahead), show(path)))
    if left:
        print("[build] %d of them could not be changed (read-only?): the build may loop - check the clock" % left)
    return left


def _ahead(seconds):
    return "%.1f h" % (seconds / 3600) if seconds >= 3600 else "%d min" % max(1, seconds // 60)


# ---- CMake ------------------------------------------------------------------------------------------------------
def configure(env, force=False, build_dir=BUILD_DIR, defines=None):
    """CMake configure of the build machine's tools tree (first time, when forced, or with `defines`)."""
    force = drop_foreign_cache(build_dir, ROOT) or stale_tools(build_dir) or force
    if os.path.isfile(os.path.join(build_dir, "build.ninja")) and not force and not defines:
        return
    cmd = ["cmake", "-S", ROOT, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"]
    cmd += ["-D%s=%s" % (k, v) for k, v in (defines or {}).items()]
    cmd += compiler_defines(env)
    print("[build] configuring " + show(build_dir) + "".join(" %s=%s" % kv for kv in (defines or {}).items()))
    os.makedirs(BUILD, exist_ok=True)
    log = os.path.join(BUILD, "configure.log")
    if run_with_progress(cmd, log, "[build] configure", env=env) != 0:
        print(tail(log, 30))
        fail("CMake configure failed (full log: %s)" % show(log), "configure_failed", log)


def drop_foreign_cache(build_dir, source):
    """A build tree configured from another copy of the source (a moved source folder) makes CMake stop ("does not
    match the source used to generate cache"): its cache goes, and the tree is set up again. True when that happened."""
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


def compiler_defines(env):
    """-DCMAKE_C(XX)_COMPILER for a fresh tools tree: clang 19+ (build_env put it first on PATH); and build_tools'
    Ninja (CMAKE_MAKE_PROGRAM: by name it would be looked up on PATH)."""
    ninja = ["-DCMAKE_MAKE_PROGRAM=" + build_tools(env)["ninja"]]
    clang, clangxx = _which("clang", env), _which("clang++", env)
    if not clang or not clangxx or clang_version(clang) < MIN_CLANG:
        fail("clang %d or newer not found: sudo apt install clang-20 lld-20" % MIN_CLANG)
    if not _which("ld.lld", env):
        fail("ld.lld not found (the LLVM linker, next to clang): sudo apt install lld-%d" % clang_version(clang))
    defines = ["-DCMAKE_C_COMPILER=" + clang, "-DCMAKE_CXX_COMPILER=" + clangxx] + ninja
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


_INTERESTING = re.compile(r"error|FAILED|Linking|warning: unused")
_NINJA_STEP = re.compile(r"^\[(\d+)/(\d+)\] (\S+)")


def cmake_build(env, target, log_name, verbose=False, build_dir=BUILD_DIR):
    """Builds `target` under a progress line (Ninja's [done/total]); prints errors and link steps (everything with
    verbose). False on failure."""
    log = os.path.join(BUILD, log_name)
    os.makedirs(os.path.dirname(log), exist_ok=True)
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
        if "still dirty after" in tail(log, 400).lower():
            print("[build] Ninja kept finding files newer than its own: some are dated after the clock. Check the "
                  "clock and time zone, then build again (files in the folder dated in the future are set to now)")
    return ok
