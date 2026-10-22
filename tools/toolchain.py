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


VCS_EXE = exe(os.path.join(BUILD_DIR, "third_party", "PS2Recomp", "ps2xRuntime", "vcs"))
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
def configure(env, force=False):
    if os.path.isfile(os.path.join(BUILD_DIR, "build.ninja")) and not force:
        return
    for name in ("cmake", "ninja"):
        if not _which(name, env):
            fail(f"{name} not found on PATH")
    cmd = ["cmake", "-S", ROOT, "-B", BUILD_DIR, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"]
    if IS_WINDOWS:
        cmd += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    elif _which("clang", env) and _which("clang++", env):
        cmd += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
    print("[build] configuring " + os.path.relpath(BUILD_DIR, ROOT))
    log = os.path.join(BUILD, "configure.log")
    with open(log, "w", encoding="utf-8") as f:
        if subprocess.run(cmd, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT).returncode != 0:
            print(tail(log, 30))
            fail("CMake configure failed (full log: build/configure.log)")


_INTERESTING = re.compile(r"error|FAILED|Linking|warning: unused")


def cmake_build(env, target, log_name, verbose=False):
    """Builds `target`; prints errors and link steps (everything with verbose). False on failure."""
    log = os.path.join(BUILD, log_name)
    os.makedirs(BUILD, exist_ok=True)
    cmd = ["cmake", "--build", BUILD_DIR, "--target", target]
    with open(log, "w", encoding="utf-8") as f:
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        for line in proc.stdout:
            f.write(line)
            if verbose or (_INTERESTING.search(line) and "Wdeprecated" not in line):
                print(line, end="")
        ok = proc.wait() == 0
    if not ok:
        print(f"[build] {target} failed (full log: build/{log_name})")
    return ok
