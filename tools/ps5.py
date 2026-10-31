"""The PS5 build (docs/PS5.md): the game cross-compiled for a jailbroken PS5 and packaged as a homebrew app folder,
on a Linux PC (WSL works). Standard library only.

    python3 tools/ps5.py setup                      ps5-plume-triangle fetched (pinned) and built once, next to this folder
    python3 tools/ps5.py check                      what is there, what is missing, and how to get it
    python3 tools/ps5.py build [--triangle DIR]     build/ps5 (CMake, Ninja) -> build/ps5/app/<title id>/
    python3 tools/ps5.py package [--zip]            the app folder again from build/ps5/vcs.elf (--zip: also zipped)
    python3 tools/ps5.py ffmpeg                     FFmpeg for the PS5 (the movies) into build/ps5/ffmpeg; then build
    python3 tools/ps5.py deploy --host <PS5 IP>     the app folder to /data/homebrew/<title id> over FTP [--iso F]
    python3 tools/ps5.py compile-check --sdk DIR    the PS5 build compiled without a disc or console (CI)
    python3 tools/ps5.py padtest [--tree DIR]       the DualSense path checked on Linux (ps5/tests/padtest.cpp)

What it needs (check says what is missing):
- generated/: the game recompiled from your disc, once, on the PC: python3 tools/build.py --recompile <iso> (or
  python3 tools/recompile.py <iso>). The PS5 build compiles the same generated code.
- clang 19+ and lld (the PC's own: docs/LINUX.md), glslang, CMake 3.21+, Ninja.
- ps5-plume-triangle (https://github.com/Nazky/ps5-plume-triangle), built once with `make` in it: the PS5 payload
  SDK fork (.deps/native/ps5-payload-sdk), RADV (PS5_Mesa's Vulkan driver, .deps/native/radv-release), the start-up
  code, the libc.prx and ps5-native-tool, which makes eboot.bin. The gta4-ps5 port builds the same way. --triangle
  (or VCS_PS5_TRIANGLE) says where; it is remembered in build/ps5/ps5.json.

The app folder goes to /data/homebrew/<title id>/ on the console (deploy, or any FTP client / USB copy), with your
.iso in its disc/ folder or on a USB drive. Saves, settings and logs live in that folder too.
"""
import argparse
import ftplib
import glob
import hashlib
import json
import os
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

PS5_BUILD = os.path.join(tc.BUILD, "ps5")
STATE = os.path.join(PS5_BUILD, "ps5.json")
ELF = os.path.join(PS5_BUILD, "vcs.elf")
FFMPEG_PREFIX = os.path.join(PS5_BUILD, "ffmpeg")
TOOLCHAIN_FILE = os.path.join(tc.ROOT, "ps5", "cmake", "ps5-toolchain.cmake")
GENERATED = os.path.join(tc.ROOT, "generated", "ee", "register_functions.cpp")

# ps5-plume-triangle, pinned (tools/ps5.py setup): the commit this port was written and link-checked against
TRIANGLE_REPO = "https://github.com/Nazky/ps5-plume-triangle"
TRIANGLE_COMMIT = "d54d9adb084bc038023ec4d8bbd8a4ba23f5880d"
# what its `make` needs (its README; the Debian / Ubuntu package for each)
TRIANGLE_NEEDS = [("git", "git"), ("make", "make"), ("wget", "wget"), ("unzip", "unzip"), ("xz", "xz-utils"),
                  ("rsync", "rsync"), ("meson", "meson"), ("ninja", "ninja-build"), ("glslangValidator", "glslang-tools"),
                  ("bison", "bison"), ("flex", "flex"), ("gcc", "gcc"), ("g++", "g++"), ("curl", "curl")]

DEFAULT_TITLE_ID = "PPSA94590"  # a homebrew id (high PPSA range, ...590 for SLUS-21590): --title-id for another
TITLE_NAME = "GTA: Vice City Stories"
CONTENT_LABEL = "VCSRECOMPPS50000"  # 16 characters: the content id's tail
# the converter's module SDK versions and the fake-SELF magic (ps5-plume-triangle tools/build.sh, gta4-ps5)
MODULE_SDK, COMPANION_SDK, FSELF_MAGIC = "0x02000009", "0x08050001", "0x1D3D154F"

FFMPEG_VERSION = "7.1.1"
FFMPEG_URL = "https://ffmpeg.org/releases/ffmpeg-%s.tar.xz" % FFMPEG_VERSION
FFMPEG_SHA256 = "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"


# ---- where things are -------------------------------------------------------------------------------------------
def load_state():
    try:
        with open(STATE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(state):
    os.makedirs(PS5_BUILD, exist_ok=True)
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


def find_triangle(given):
    """--triangle, VCS_PS5_TRIANGLE, the remembered one, or a ps5-plume-triangle next to the repository / in ~."""
    state = load_state()
    for cand in (given, os.environ.get("VCS_PS5_TRIANGLE"), state.get("triangle"),
                 os.path.join(os.path.dirname(tc.ROOT), "ps5-plume-triangle"), os.path.expanduser("~/ps5-plume-triangle")):
        if cand and os.path.isdir(cand):
            return os.path.abspath(cand)
    return None


def find_sdk(given, triangle):
    for cand in (given, os.environ.get("PS5_PAYLOAD_SDK"), load_state().get("sdk"),
                 os.path.join(triangle, ".deps", "native", "ps5-payload-sdk") if triangle else None):
        if cand and os.path.isdir(os.path.join(cand, "target", "include", "c++", "v1")):
            return os.path.abspath(cand)
    return None


def native_tool(triangle):
    for cand in (os.path.join(triangle, "build", "host", "ps5-native-tool"),
                 os.path.join(triangle, "tooling", "native", "ps5-native-tool")):
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


def ps5_clang(env):
    clang, clangxx = shutil.which("clang", path=env.get("PATH")), shutil.which("clang++", path=env.get("PATH"))
    if clang and clangxx and tc.clang_version(clang) >= tc.MIN_CLANG:
        return clang, clangxx
    return None, None


# ---- setup -----------------------------------------------------------------------------------------------------
def cmd_setup(args):
    """ps5-plume-triangle at the pinned commit, next to this folder (or --triangle), and its `make` once: the PS5 payload
    SDK fork, RADV, the start-up code, libc.prx and ps5-native-tool (its tools/ scripts download and build them)."""
    if platform.system() != "Linux":
        tc.fail("the PS5 build runs on Linux (or WSL 2)")
    missing = [(tool, pkg) for tool, pkg in TRIANGLE_NEEDS if not shutil.which(tool)]
    try:
        import mako, ply, yaml  # noqa: F401 (Mesa's code generators; the triangle installs them with pip when missing)
    except ImportError:
        missing.append(("python3 mako / ply / yaml", "python3-mako python3-ply python3-yaml"))
    if missing:
        print("[ps5] missing: " + ", ".join(t for t, _ in missing))
        print("[ps5] on Debian / Ubuntu: sudo apt install " + " ".join(sorted({p for _, p in missing})))
        tc.fail("install the packages above, then run setup again", "ps5_setup_packages")
    dest = os.path.abspath(args.triangle or os.path.join(os.path.dirname(tc.ROOT), "ps5-plume-triangle"))
    if not os.path.isdir(os.path.join(dest, ".git")):
        print("[ps5] cloning %s into %s" % (TRIANGLE_REPO, dest))
        if subprocess.run(["git", "clone", TRIANGLE_REPO, dest]).returncode != 0:
            tc.fail("git clone failed")
    head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != TRIANGLE_COMMIT and not args.keep_commit:
        print("[ps5] ps5-plume-triangle at %s (pinned)" % TRIANGLE_COMMIT[:12])
        subprocess.run(["git", "-C", dest, "fetch", "origin", TRIANGLE_COMMIT])
        if subprocess.run(["git", "-C", dest, "checkout", "--detach", TRIANGLE_COMMIT]).returncode != 0:
            tc.fail("could not check out %s in %s (local changes? --keep-commit keeps its own)" % (TRIANGLE_COMMIT, dest))
    state = load_state()
    state["triangle"] = dest
    save_state(state)
    if args.clone_only:
        print("[ps5] ok: %s (not built: --clone-only)" % dest)
        return
    env = dict(os.environ)
    clang = shutil.which("clang", path=tc.build_env().get("PATH"))
    if clang:
        env.setdefault("PS5_CLANG", clang)  # (its scripts take clang from PATH otherwise)
    print("[ps5] building ps5-plume-triangle (make: the SDK, RADV, the start-up code; the first time takes a while)")
    log = os.path.join(PS5_BUILD, "triangle.log")
    os.makedirs(PS5_BUILD, exist_ok=True)
    if tc.run_with_progress(["make"], log, "[ps5] triangle make", cwd=dest, env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("ps5-plume-triangle's make failed (%s)" % tc.show(log), "ps5_setup_failed", log)
    print("[ps5] ok: %s - now python3 tools/ps5.py check" % dest)


# ---- check ------------------------------------------------------------------------------------------------------
def check(args, quiet=False):
    """(problems, paths): a list of what is missing (each with its fix) and the paths found."""
    env = tc.build_env()
    problems, notes = [], []

    def line(ok, what, detail):
        if not quiet:
            print("[ps5] %-4s %-28s %s" % ("ok" if ok else "--", what, detail))

    if platform.system() != "Linux":
        problems.append("the PS5 build runs on Linux (or WSL): the payload SDK and its tools are Linux programs")
    clang, clangxx = ps5_clang(env)
    line(bool(clang), "clang %d+" % tc.MIN_CLANG, clang or "not found: docs/LINUX.md (sudo apt install clang-20 lld-20)")
    if not clang:
        problems.append("clang %d or newer (docs/LINUX.md)" % tc.MIN_CLANG)
    lld = shutil.which("ld.lld", path=env.get("PATH"))
    line(bool(lld), "ld.lld", lld or "not found: the LLVM linker (sudo apt install lld-20)")
    if not lld:
        problems.append("ld.lld (lld)")
    glslang = shutil.which("glslangValidator", path=env.get("PATH"))
    line(bool(glslang), "glslang", glslang or "not found: sudo apt install glslang-tools")
    if not glslang:
        problems.append("glslangValidator (glslang-tools)")
    have_generated = os.path.isfile(GENERATED)
    line(have_generated, "generated/ (your disc)", "ok" if have_generated else
         "missing: python3 tools/build.py --recompile <your .iso> first (or tools/recompile.py)")
    if not have_generated:
        problems.append("generated/: recompile the game from your disc first (python3 tools/build.py --recompile <iso>)")

    tri = find_triangle(getattr(args, "triangle", None))
    line(bool(tri), "ps5-plume-triangle", tri or "not found: git clone https://github.com/Nazky/ps5-plume-triangle, "
         "make in it, then --triangle <dir>")
    sdk = find_sdk(getattr(args, "sdk", None), tri)
    paths = {"triangle": tri, "sdk": sdk, "clang": clang, "clangxx": clangxx}
    if not tri:
        problems.append("ps5-plume-triangle, built once (docs/PS5.md)")
    else:
        pieces = [
            ("start-up code", [os.path.join(tri, "build", "obj", "app_crt.o"), os.path.join(tri, "build", "obj", "app_cpp_runtime.o")]),
            ("libSceAgc stubs", [os.path.join(tri, "build", "stubs", "libSceAgc.so")]),
            ("RADV (PS5_Mesa)", [os.path.join(tri, ".deps", "native", "radv-release", "lib", "libvulkan_radeon.ps5.a")]),
            ("libc.prx", [os.path.join(tri, "runtime", "libc.prx")]),
        ]
        for what, files in pieces:
            ok = all(os.path.isfile(f) for f in files)
            line(ok, what, files[0] if ok else "missing: run make in %s once" % tri)
            if not ok:
                problems.append("%s of ps5-plume-triangle (make in %s)" % (what, tri))
        tool = native_tool(tri)
        line(bool(tool), "ps5-native-tool", tool or "missing: run make in %s once" % tri)
        if not tool:
            problems.append("ps5-native-tool (make in %s)" % tri)
    sdk_ok = bool(sdk) and os.path.isfile(os.path.join(sdk, "target", "lib", "libps5platform.a"))
    line(sdk_ok, "payload SDK (fork)", sdk if sdk_ok else
         "missing: the triangle's make installs it in .deps/native/ps5-payload-sdk (or --sdk / PS5_PAYLOAD_SDK)")
    if not sdk_ok:
        problems.append("the PS5 payload SDK fork with libps5platform.a (the triangle's make installs it)")
    ffmpeg = os.path.isfile(os.path.join(FFMPEG_PREFIX, "lib", "libavcodec.a"))
    line(ffmpeg, "FFmpeg (optional: movies)", FFMPEG_PREFIX if ffmpeg else "not built: python3 tools/ps5.py ffmpeg "
         "(without it the movies are skipped)")
    paths["ffmpeg"] = FFMPEG_PREFIX if ffmpeg else None
    if not quiet:
        print("[ps5] %s" % ("ready to build: python3 tools/ps5.py build" if not problems else
                            "missing:\n" + "\n".join("  - " + p for p in problems)))
    return problems, paths, env


# ---- build ------------------------------------------------------------------------------------------------------
def configure(env, paths, lto, force=False):
    defines = {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": TOOLCHAIN_FILE,
        "PS5_PAYLOAD_SDK": paths["sdk"],
        "VCS_PS5_TRIANGLE": paths["triangle"],
        "CMAKE_C_COMPILER": paths["clang"],
        "CMAKE_CXX_COMPILER": paths["clangxx"],
        "CMAKE_MAKE_PROGRAM": tc.build_tools(env)["ninja"],
        "PS2X_LTO": "THIN" if lto else "OFF",
        "PS2X_ENABLE_AGRESSIVE_LOGS": "OFF",
        "VCS_PS5_FFMPEG": paths["ffmpeg"] or "",
    }
    state = load_state()
    if os.path.isfile(os.path.join(PS5_BUILD, "build.ninja")) and not force and state.get("defines") == defines:
        return
    cmd = ["cmake", "-S", tc.ROOT, "-B", PS5_BUILD, "-G", "Ninja"] + ["-D%s=%s" % kv for kv in defines.items()]
    cmd += tc.deps_defines()
    print("[ps5] configuring " + tc.show(PS5_BUILD))
    tc.stage("configure", "Configuring the PS5 build (CMake)")
    os.makedirs(PS5_BUILD, exist_ok=True)
    log = os.path.join(PS5_BUILD, "configure.log")
    if tc.run_with_progress(cmd, log, "[ps5] configure", env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("CMake configure failed (full log: %s)" % tc.show(log), tc.configure_failure(log), log)
    state.update({"defines": defines, "triangle": paths["triangle"], "sdk": paths["sdk"]})
    save_state(state)


def cmd_build(args):
    problems, paths, env = check(args)
    if problems:
        tc.fail("the PS5 build is not ready (above)", "ps5_not_ready")
    configure(env, paths, args.lto, args.reconfigure)
    tc.stage("build", "Building the game for the PS5")
    if not tc.cmake_build(env, "ps2EntryRunner", os.path.join("ps5", "build.log"), args.verbose, build_dir=PS5_BUILD):
        tc.fail("the PS5 build failed (build/ps5/build.log)", "build_failed")
    package(args, paths)


# ---- package ----------------------------------------------------------------------------------------------------
def run(cmd):
    """None when `cmd` succeeds, else its output."""
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return result.stdout + result.stderr
    return None


def relink_with_padding(paths):
    """The link again with ps5/runtime/ps5_relro_pad.c (half a page more RELRO): room for the process parameters."""
    work = os.path.join(PS5_BUILD, "ps5-link")
    pad = os.path.join(work, "ps5_relro_pad.o")
    sdk = paths["sdk"]
    err = run([paths["clang"], "--target=x86_64-sie-ps5", "-isysroot", sdk, "-fPIC", "-O2", "-c",
               os.path.join(tc.ROOT, "ps5", "runtime", "ps5_relro_pad.c"), "-o", pad])
    if err:
        tc.fail("compiling the RELRO padding failed:\n" + err)
    padded = os.path.join(work, "vcs-padded.elf")
    with open(os.path.join(work, "link.rsp"), encoding="utf-8") as f:
        words = f.read().splitlines()  # tools/ps5_link.py writes one (quoted) argument per line
    if "-o" not in words:
        tc.fail("build/ps5/ps5-link/link.rsp has no -o: build again (python3 tools/ps5.py build)")
    words[words.index("-o") + 1] = shlex.quote(padded)
    with open(os.path.join(work, "link-padded.rsp"), "w", encoding="utf-8") as f:
        f.write("\n".join(words + [shlex.quote(pad)]) + "\n")
    lld = os.path.join(os.path.dirname(os.path.realpath(paths["clang"])), "ld.lld")
    err = run([lld if os.path.isfile(lld) else "ld.lld", "@" + os.path.join(work, "link-padded.rsp")])
    if err:
        tc.fail("the padded link failed:\n" + err)
    return padded


def package(args, paths=None):
    if paths is None:
        problems, paths, _ = check(args, quiet=True)
        if not paths["triangle"] or not paths["sdk"]:
            tc.fail("ps5-plume-triangle / the SDK not found: python3 tools/ps5.py check")
    if not os.path.isfile(ELF):
        tc.fail("no %s yet: python3 tools/ps5.py build" % tc.show(ELF))
    tri, sdk = paths["triangle"], paths["sdk"]
    tool = native_tool(tri)
    if not tool:
        tc.fail("ps5-native-tool not found in %s (make in it once)" % tri)
    title_id = args.title_id
    if len(title_id) != 9 or not title_id[:4].isalpha() or not title_id[4:].isdigit():
        tc.fail("a title id is 4 letters and 5 digits (PPSA94590)")
    work = os.path.join(PS5_BUILD, "ps5-link")
    os.makedirs(work, exist_ok=True)
    eboot_elf = os.path.join(work, "eboot.elf")
    stubs = [os.path.join(tri, "build", "stubs", n) for n in ("libSceAgc.so", "libSceAgcDriver.so")]

    def convert(elf):
        cmd = [tool, "link", "--in", elf, "--out", eboot_elf, "--stub-dir", os.path.join(sdk, "target", "lib")]
        for s in stubs:
            cmd += ["--stub", s]
        cmd += ["--module-sdk", MODULE_SDK, "--companion-sdk", COMPANION_SDK, "--file-name", "eboot.elf"]
        return run(cmd)

    tc.stage("package", "Making the PS5 app folder")
    print("[ps5] eboot.elf from %s" % tc.show(ELF))
    err = convert(ELF)
    if err and "no room for PS5 process parameters" in err:
        # the image's RELRO ends too near its page boundary for the process parameters (gta4-ps5 met it too)
        print("[ps5] linking again with 2 KB of RELRO padding (room for the process parameters)")
        err = convert(relink_with_padding(paths))
    if err:
        tc.fail("ps5-native-tool link failed:\n" + err)

    app = os.path.join(PS5_BUILD, "app", title_id)
    # the app folder is made anew, except what the console wrote into a copy of it here (never: deploy only uploads)
    if os.path.isdir(app):
        shutil.rmtree(app)
    for d in ("sce_sys", "sce_module", "disc"):
        os.makedirs(os.path.join(app, d), exist_ok=True)
    err = run([tool, "self", "--sign", "--in", eboot_elf, "--out", os.path.join(app, "eboot.bin"), "--magic", FSELF_MAGIC])
    if err:
        tc.fail("signing eboot.bin failed:\n" + err)
    shutil.copy2(os.path.join(tri, "runtime", "libc.prx"), os.path.join(app, "sce_module", "libc.prx"))
    write_param_json(os.path.join(app, "sce_sys", "param.json"), title_id)
    make_icon(os.path.join(app, "sce_sys", "icon0.png"))
    with open(os.path.join(app, "disc", "README.txt"), "w", encoding="utf-8") as f:
        f.write("Copy your own GTA: Vice City Stories (USA, SLUS-21590) disc image here as an .iso file.\n"
                "Or put it on a USB drive (exFAT), top folder or games/, named *Vice City Stories*.iso.\n")
    with open(os.path.join(app, "README.txt"), "w", encoding="utf-8") as f:
        f.write(APP_README.format(title_id=title_id))
    if getattr(args, "zip", False):
        archive = os.path.join(PS5_BUILD, "%s.zip" % title_id)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for base, _, names in os.walk(app):
                for n in sorted(names):
                    path = os.path.join(base, n)
                    z.write(path, os.path.join(title_id, os.path.relpath(path, app)))
        print("[ps5] zipped: %s - unzip it into /data/homebrew/ on your PS5 (USB / FTP). It holds the game's code built "
              "from your disc: for your console only, never to publish" % tc.show(archive))
    size = os.path.getsize(os.path.join(app, "eboot.bin"))
    print("[ps5] ok: %s (eboot.bin %.1f MB) - copy it to /data/homebrew/%s on the PS5 (python3 tools/ps5.py deploy "
          "--host <PS5 IP>)" % (tc.show(app), size / 1e6, title_id))
    tc.event(done="ps5_package", app=tc.show(app).replace(os.sep, "/"))


APP_README = """GTA: Vice City Stories - native PS5 port (PS2 static recompilation). No game files are included.

On the PS5 this folder is /data/homebrew/{title_id}/:
  disc/        your Vice City Stories (USA) .iso (or a USB drive: *Vice City Stories*.iso, top folder or games/)
  vcs.ini      the settings (made at the first start; the settings menu changes it)
  userdata/    the memory card (your saves) and a cache
  logs/        vcs.log of the last run (vcs.prev.log the one before): send it with a bug report

Controls: the DualSense as a DualShock 2. Touchpad click = Select. Touchpad + Options: settings menu.
Touchpad + R1: trainer. Touchpad + L1: free camera.
"""


def write_param_json(path, title_id):
    """The app's system parameters, as ps5-plume-triangle / gta4-ps5 write them (60 Hz output: attribute3 0)."""
    param = {
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "attribute": 0,
        "attribute2": 0,
        "attribute3": 0,
        "conceptId": title_id[4:],
        "contentBadgeType": 1,
        "contentId": "UP9000-%s_00-%s" % (title_id, CONTENT_LABEL),
        "contentVersion": "01.000.000",
        "downloadDataSize": 256,
        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": TITLE_NAME}},
        "masterVersion": "01.00",
        "pubtools": {"creationDate": time.strftime("%Y-%m-%d 00:00:00", time.gmtime()), "loudnessSnd0": "-28.00",
                     "toolVersion": "2.00"},
        "requiredSystemSoftwareVersion": "0x0000000000000000",
        "sdkVersion": "0x0000000000000000",
        "titleId": title_id,
        "versionFileUri": "",
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(param, f, indent=2)
        f.write("\n")


# ---- the home screen icon: drawn here (a sunset over the sea and "VCS"), no artwork of the game's ---------------------
GLYPHS = {  # 5 x 7
    "V": ["10001", "10001", "10001", "10001", "01010", "01010", "00100"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
}


def make_icon(path, size=512):
    def mix(a, b, t):
        return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

    horizon = int(size * 0.62)
    sun_x, sun_y, sun_r = size // 2, horizon, int(size * 0.30)
    text = "VCS"
    cell = size // 34
    text_w = (len(text) * 5 + (len(text) - 1) * 3) * cell
    text_x, text_y = (size - text_w) // 2, horizon + int(size * 0.08)
    lit = set()
    for n, ch in enumerate(text):
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(row):
                if bit == "1":
                    lit.add((n * 8 + gx, gy))
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            if y < horizon:
                t = y / horizon
                c = mix((24, 6, 52), (214, 40, 118), t) if t < 0.7 else mix((214, 40, 118), (255, 150, 60), (t - 0.7) / 0.3)
                dx, dy = x - sun_x, y - sun_y
                if dx * dx + dy * dy <= sun_r * sun_r:
                    st = (y - (sun_y - sun_r)) / sun_r  # 0 at the top of the sun, 1 at the horizon
                    band = int(st * 9)
                    if not (st > 0.45 and band % 2 == 1 and (y % (size // 28)) < (size // 28) * st * 0.7):
                        c = mix((255, 230, 90), (255, 70, 140), st)
            else:
                t = (y - horizon) / (size - horizon)
                c = mix((10, 70, 96), (4, 22, 40), t)
                if (y - horizon) % max(3, int(6 + t * 18)) == 0:
                    c = mix(c, (255, 120, 150), 0.35 * (1 - t))
                gx, gy = (x - text_x) // cell, (y - text_y) // cell
                if 0 <= y - text_y < 8 * cell and 0 <= x - text_x < text_w + cell:
                    if (gx, gy) in lit:
                        c = (250, 250, 250)
                    elif (gx - 1, gy - 1) in lit or ((x - text_x) % cell < cell // 2 and (gx - 1, gy) in lit and
                                                      (y - text_y) % cell >= cell // 2):
                        c = mix(c, (0, 0, 0), 0.55)  # a drop shadow down and to the right
            row += bytes(c)
        rows.append(bytes([0]) + bytes(row))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---- FFmpeg -----------------------------------------------------------------------------------------------------
def cmd_ffmpeg(args):
    """FFmpeg's MPEG-2 video decoder and swscale, static, for the PS5 (what src/game/vcs_movie.cpp and the runtime's
    MPEG stub use), from the pinned release; deps/ffmpeg-<version>.tar.xz is taken when there."""
    problems, paths, env = check(args, quiet=True)
    if not paths["clang"] or not paths["sdk"]:
        tc.fail("clang 19+ and the PS5 payload SDK are needed first: python3 tools/ps5.py check")
    work = os.path.join(PS5_BUILD, "ffmpeg-build")
    os.makedirs(work, exist_ok=True)
    archive = os.path.join(tc.DEPS_DIR, "ffmpeg-%s.tar.xz" % FFMPEG_VERSION)
    if not os.path.isfile(archive):
        archive = os.path.join(work, "ffmpeg-%s.tar.xz" % FFMPEG_VERSION)
        if not os.path.isfile(archive):
            print("[ps5] downloading %s" % FFMPEG_URL)
            urllib.request.urlretrieve(FFMPEG_URL, archive + ".part")
            os.replace(archive + ".part", archive)
    with open(archive, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != FFMPEG_SHA256:
        os.remove(archive)
        tc.fail("%s does not match its pinned sha256 (removed: run again)" % archive)
    source = os.path.join(work, "ffmpeg-%s" % FFMPEG_VERSION)
    if not os.path.isdir(source):
        with tarfile.open(archive) as t:
            try:
                t.extractall(work, filter="data")  # (Python 3.11.4+: no links or paths out of the folder)
            except TypeError:
                t.extractall(work)
    sdk, clang = paths["sdk"], paths["clang"]
    llvm = os.path.dirname(os.path.realpath(clang))
    target = "--target=x86_64-sie-ps5 -isysroot %s" % sdk
    cflags = target + " -march=znver2 -fPIC -fno-plt -fno-stack-protector -femulated-tls -fdenormal-fp-math=ieee -O2"
    # configure's checks link test programs against the console's libc and kernel stubs (as the SDK's prospero-clang
    # does), so a function it finds is one the PS5 has
    lib = os.path.join(sdk, "target", "lib")
    ldflags = "%s -fuse-ld=lld -nostdlib -Wl,-pie %s -L%s" % (target, os.path.join(lib, "crt1.o"), lib)
    cmd = [os.path.join(source, "configure"), "--prefix=" + FFMPEG_PREFIX, "--enable-cross-compile", "--target-os=freebsd",
           "--arch=x86_64", "--cc=" + clang, "--cxx=" + paths["clangxx"],
           "--ar=" + os.path.join(llvm, "llvm-ar"), "--ranlib=" + os.path.join(llvm, "llvm-ranlib"),
           "--nm=" + os.path.join(llvm, "llvm-nm"), "--strip=" + os.path.join(llvm, "llvm-strip"),
           "--extra-cflags=" + cflags, "--extra-ldflags=" + ldflags,
           "--extra-libs=-lc -lkernel_web -lSceLibcInternal",
           "--enable-static", "--disable-shared", "--disable-programs", "--disable-doc", "--disable-network",
           "--disable-autodetect", "--disable-x86asm", "--disable-inline-asm", "--disable-debug", "--disable-pthreads",
           "--disable-everything", "--disable-avformat", "--disable-avdevice", "--disable-avfilter", "--disable-swresample",
           "--enable-avcodec", "--enable-avutil", "--enable-swscale",
           "--enable-decoder=mpeg2video,mpeg1video", "--enable-parser=mpegvideo"]
    log = os.path.join(work, "configure.log")
    print("[ps5] configuring FFmpeg %s for the PS5" % FFMPEG_VERSION)
    if tc.run_with_progress(cmd, log, "[ps5] ffmpeg configure", cwd=source, env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("FFmpeg's configure failed (%s; its ffbuild/config.log has the details)" % tc.show(log))
    jobs = str(max(1, os.cpu_count() or 1))
    for step in (["make", "-j", jobs], ["make", "install"]):
        log = os.path.join(work, "%s.log" % step[-1].strip("-"))
        if tc.run_with_progress(step, log, "[ps5] ffmpeg " + " ".join(step[1:2]), cwd=source, env=env) != 0:
            print(tc.tail(log, 30))
            tc.fail("FFmpeg %s failed (%s)" % (" ".join(step), tc.show(log)))
    print("[ps5] ok: FFmpeg in %s - python3 tools/ps5.py build takes it (the movies)" % tc.show(FFMPEG_PREFIX))


# ---- compile check (no disc, no console) --------------------------------------------------------------------------
STUB_DIRS = ("src", "third_party/PS2Recomp/ps2xRuntime/src", "third_party/PS2Recomp/ps2xRuntime/include")


def write_stub_generated(out):
    """A stand-in for generated/ee with no game code in it: declarations and empty bodies for the generated functions
    the host code names (FUN_xxxxxxxx_0x...), and an empty function table. It compiles and links; it runs nothing."""
    import re
    names = set()
    for d in STUB_DIRS:
        for base, _, files in os.walk(os.path.join(tc.ROOT, d)):
            for f in files:
                if f.endswith((".cpp", ".h")):
                    with open(os.path.join(base, f), encoding="utf-8", errors="replace") as src:
                        names |= set(re.findall(r"\b[A-Za-z_]\w*_0x[0-9a-f]+\b", src.read()))
    names = sorted(names)
    ee = os.path.join(out, "ee")
    if os.path.isdir(ee):
        shutil.rmtree(ee)
    os.makedirs(ee)
    decl = "PS2_GUEST_CC void %s(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);\n"
    with open(os.path.join(ee, "ps2_recompiled_functions.h"), "w", encoding="utf-8") as f:
        f.write("// a stand-in (tools/ps5.py compile-check): no game code\n#ifndef PS2_RECOMPILED_FUNCTIONS_H\n"
                "#define PS2_RECOMPILED_FUNCTIONS_H\n#include \"ps2_guest_cc.h\"\n#include <cstdint>\n"
                "struct R5900Context;\nclass PS2Runtime;\n" + "".join(decl % n for n in names) + "#endif\n")
    with open(os.path.join(ee, "ps2_recompiled_stubs.h"), "w", encoding="utf-8") as f:
        f.write("#pragma once\n#include <cstdint>\n#include \"ps2_runtime.h\"\n#include \"ps2_syscalls.h\"\n")
    with open(os.path.join(ee, "register_functions.cpp"), "w", encoding="utf-8") as f:
        f.write("#include \"ps2_runtime.h\"\n#include <ps2_recompiled_functions.h>\n"
                "extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x100000u;\n"
                "extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x100004u;\n"
                "extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1u;\n"
                "PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1u] = {};\n")
    with open(os.path.join(ee, "stub_bodies.cpp"), "w", encoding="utf-8") as f:
        f.write("#include \"ps2_runtime.h\"\n#include <ps2_recompiled_functions.h>\n" +
                "".join("PS2_GUEST_CC void %s(uint8_t*, R5900Context*, PS2Runtime*) {}\n" % n for n in names))
    return ee, len(names)


def cmd_compile_check(args):
    """The PS5 tree built against stand-in generated code: every library and every object of the game, and the link
    too when a built ps5-plume-triangle and the SDK fork are there. Without them it stops before the link (CI: the
    upstream SDK release, .github/workflows/ps5.yml)."""
    env = tc.build_env()
    clang, clangxx = ps5_clang(env)
    if not clang:
        tc.fail("clang %d or newer not found (docs/LINUX.md)" % tc.MIN_CLANG)
    tri = find_triangle(args.triangle)
    sdk = find_sdk(args.sdk, tri)
    if not sdk:
        tc.fail("no PS5 payload SDK: --sdk <dir with target/include/c++/v1> (the upstream release is enough to compile)")
    linkable = bool(tri) and os.path.isfile(os.path.join(sdk, "target", "lib", "libps5platform.a"))
    check_dir = os.path.join(tc.BUILD, "ps5-check")
    ee, count = write_stub_generated(os.path.join(check_dir, "generated"))
    print("[ps5] stand-in generated code: %d functions (%s)" % (count, tc.show(ee)))
    defines = {
        "CMAKE_BUILD_TYPE": "Release", "CMAKE_TOOLCHAIN_FILE": TOOLCHAIN_FILE, "PS5_PAYLOAD_SDK": sdk,
        "VCS_PS5_TRIANGLE": tri or "", "CMAKE_C_COMPILER": clang, "CMAKE_CXX_COMPILER": clangxx,
        "CMAKE_MAKE_PROGRAM": tc.build_tools(env)["ninja"], "PS2X_LTO": "OFF", "PS2X_ENABLE_AGRESSIVE_LOGS": "OFF",
        "VCS_GENERATED_DIR": ee,
    }
    tree = os.path.join(check_dir, "tree")
    os.makedirs(tree, exist_ok=True)
    cmd = ["cmake", "-S", tc.ROOT, "-B", tree, "-G", "Ninja"] + ["-D%s=%s" % kv for kv in defines.items()] + tc.deps_defines()
    log = os.path.join(check_dir, "configure.log")
    if tc.run_with_progress(cmd, log, "[ps5] configure (check)", env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("CMake configure failed (%s)" % tc.show(log))
    ninja = tc.build_tools(env)["ninja"]
    if linkable:
        targets = ["ps2EntryRunner"]
    else:  # every object of the game, not its link (no triangle / SDK fork here)
        listed = subprocess.run([ninja, "-C", tree, "-t", "targets", "all"], capture_output=True, text=True).stdout
        targets = ["ps2_runtime", "ps2_iop", "vcs_platform", "vcs_rhi", "vcs_render", "vcs_ps5_runtime"]
        targets += [l.split(":")[0] for l in listed.splitlines()
                    if "CMakeFiles/ps2EntryRunner.dir/" in l and l.split(":")[0].endswith(".o")]
    log = os.path.join(check_dir, "build.log")
    with open(log, "w", encoding="utf-8") as f, tc.Progress("[ps5] compile check") as progress:
        proc = subprocess.Popen([ninja, "-C", tree, "-k", "0"] + targets, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", env=env)
        for line in proc.stdout:
            f.write(line)
            if "error" in line or "FAILED" in line:
                progress.message(line)
        ok = proc.wait() == 0
    if not ok:
        tc.fail("the PS5 compile check failed (%s)" % tc.show(log), "build_failed", log)
    print("[ps5] ok: the PS5 build compiles%s (%d targets)" % (" and links" if linkable else "", len(targets)))


# ---- the controller test (Linux) ----------------------------------------------------------------------------------
def cmd_padtest(args):
    """ps5/tests/padtest.cpp: the real platform_ps5.cpp with the console's pad functions mocked, linked against a Linux
    PC build's objects (the bindings, settings, the runtime's pad backend), run. The tree is build/release (the PC build,
    python3 tools/build.py first) or --tree; its generated code may be real or compile-check's stand-in."""
    import re
    if platform.system() != "Linux":
        tc.fail("the controller test runs on Linux (it links against a Linux build's objects)")
    env = tc.build_env()
    tree = os.path.abspath(args.tree or tc.BUILD_DIR)
    ninja = tc.build_tools(env)["ninja"]
    if not os.path.isfile(os.path.join(tree, "build.ninja")):
        tc.fail("no build tree at %s: build the game for Linux first (python3 tools/build.py)" % tc.show(tree))
    if subprocess.run([ninja, "-C", tree, "ps2EntryRunner"], env=env).returncode != 0:
        tc.fail("the Linux build failed")
    commands = subprocess.run([ninja, "-C", tree, "-t", "commands", "ps2EntryRunner"], capture_output=True, text=True,
                              env=env).stdout.splitlines()
    link = commands[-1].split()
    compile_input = next((c for c in commands if "src/game/vcs_input.cpp" in c and " -c " in c), None)
    if not compile_input:
        tc.fail("the tree's commands have no vcs_input.cpp compile")

    def absolute(path):
        return path if os.path.isabs(path) else os.path.join(tree, path)

    words = compile_input.split()
    flags = []
    for i, w in enumerate(words):
        if w == "-isystem" and i + 1 < len(words):
            flags += [w, words[i + 1]]
        elif re.match(r"-(I|D|std=|march=|isystem.)", w):
            flags.append(w)
    flags += ["-I" + os.path.join(tree, "_deps", "vulkanheaders-src", "include"), "-fno-strict-aliasing", "-g", "-O1"]
    objects = [absolute(w) for w in link if w.endswith(".o") and not w.endswith("/src/main.cpp.o")]
    libraries = [absolute(w) for w in link if w.endswith(".a") and "libvcs_platform.a" not in w and "SDL3" not in w]
    system = [w for w in link if w.startswith("-l") or w.endswith(".so")]
    out = os.path.join(tree, "ps5-padtest")
    os.makedirs(out, exist_ok=True)
    clangxx = shutil.which("clang++", path=env.get("PATH")) or "clang++"
    units = [(os.path.join(tc.ROOT, "src", "platform", "platform_ps5.cpp"), ["-DVK_NO_PROTOTYPES",
              "-I" + os.path.join(tc.ROOT, "ps5", "tests", "shim")]),
             (os.path.join(tc.ROOT, "src", "platform", "platform_common.cpp"), []),
             (os.path.join(tc.ROOT, "ps5", "tests", "padtest.cpp"), [])]
    built = []
    for src, extra in units:
        obj = os.path.join(out, os.path.basename(src) + ".o")
        cmd = [clangxx, "-c", src, "-o", obj] + flags + extra
        if subprocess.run(cmd, env=env, cwd=tree).returncode != 0:
            tc.fail("compiling %s failed" % tc.show(src))
        built.append(obj)
    exe = os.path.join(out, "padtest")
    cmd = [clangxx, "-fuse-ld=lld", "-o", exe] + built + objects + ["-Wl,--start-group"] + libraries + ["-Wl,--end-group"]
    cmd += system + ["-ldl", "-lm", "-lpthread"]
    if subprocess.run(cmd, env=env, cwd=tree).returncode != 0:
        tc.fail("linking the controller test failed")
    result = subprocess.run([exe], env=env, cwd=out, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    print(result.stdout.rstrip())
    if result.returncode != 0:
        tc.fail("the controller test failed")
    print("[ps5] ok: the DualSense path (%s)" % tc.show(exe))


# ---- deploy -----------------------------------------------------------------------------------------------------
def cmd_deploy(args):
    """The app folder to /data/homebrew/<title id>/ over FTP (an FTP payload on the console: etaHEN's on 1337, ftpsrv
    on 2121). Uploads files only - nothing on the console is deleted, so disc/, userdata/ and vcs.ini stay."""
    app = os.path.join(PS5_BUILD, "app", args.title_id)
    if not os.path.isfile(os.path.join(app, "eboot.bin")):
        tc.fail("no app folder yet: python3 tools/ps5.py build")
    remote = "/data/homebrew/%s" % args.title_id
    files = []  # (local file, its path on the console)
    for base, _, names in os.walk(app):
        for n in names:
            local = os.path.join(base, n)
            files.append((local, remote + "/" + os.path.relpath(local, app).replace(os.sep, "/")))
    if args.iso:
        files.append((os.path.abspath(args.iso), remote + "/disc/" + os.path.basename(args.iso)))
    print("[ps5] FTP %s:%d -> %s (%d files)" % (args.host, args.port, remote, len(files)))
    with ftplib.FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=30)
        ftp.login(args.user, args.password)
        made = set()

        def mkdirs(path):
            parts = path.strip("/").split("/")
            for i in range(1, len(parts) + 1):
                d = "/" + "/".join(parts[:i])
                if d in made:
                    continue
                try:
                    ftp.mkd(d)
                except ftplib.error_perm:
                    pass  # (there already)
                made.add(d)

        for f, dest in files:
            mkdirs(os.path.dirname(dest))
            size = os.path.getsize(f)
            print("[ps5]   %s (%.1f MB)" % (dest, size / 1e6))
            with open(f, "rb") as src:
                ftp.storbinary("STOR " + dest, src, blocksize=1 << 20)
    print("[ps5] ok: start it from the PS5's home screen (your homebrew launcher)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("setup")
    p.add_argument("--triangle", help="where ps5-plume-triangle goes (default: next to this folder)")
    p.add_argument("--keep-commit", action="store_true", help="leave an existing checkout at its own commit")
    p.add_argument("--clone-only", action="store_true", help="fetch it, don't build it")
    for name in ("check", "build", "package", "ffmpeg"):
        p = sub.add_parser(name)
        p.add_argument("--triangle", help="ps5-plume-triangle checkout, built once (remembered)")
        p.add_argument("--sdk", help="the PS5 payload SDK fork (default: the triangle's .deps/native/ps5-payload-sdk)")
        p.add_argument("--title-id", default=DEFAULT_TITLE_ID, help="the app's title id (default %s)" % DEFAULT_TITLE_ID)
        if name in ("build", "package"):
            p.add_argument("--zip", action="store_true", help="also build/ps5/<title id>.zip, to copy to your console")
        if name == "build":
            p.add_argument("--lto", action="store_true", help="ThinLTO (slower links; not yet measured on a console)")
            p.add_argument("--reconfigure", action="store_true", help="run CMake configure again")
            p.add_argument("-v", "--verbose", action="store_true", help="show all build output")
    p = sub.add_parser("compile-check")
    p.add_argument("--triangle", help="a built ps5-plume-triangle: the link is checked too")
    p.add_argument("--sdk", help="the PS5 payload SDK (the upstream release compiles; the link needs the fork)")
    p = sub.add_parser("padtest")
    p.add_argument("--tree", help="a Linux build tree with the game built (default build/release)")
    p = sub.add_parser("deploy")
    p.add_argument("--host", required=True, help="the PS5's IP address")
    p.add_argument("--port", type=int, default=2121, help="its FTP payload's port (default 2121; etaHEN: 1337)")
    p.add_argument("--user", default="anonymous")
    p.add_argument("--password", default="anonymous")
    p.add_argument("--title-id", default=DEFAULT_TITLE_ID)
    p.add_argument("--iso", help="also upload this disc image into the app's disc/ folder")
    args = ap.parse_args()
    if args.command == "setup":
        cmd_setup(args)
        return 0
    if args.command == "check":
        problems, _, _ = check(args)
        return 1 if problems else 0
    if args.command == "build":
        cmd_build(args)
    elif args.command == "package":
        package(args)
    elif args.command == "ffmpeg":
        cmd_ffmpeg(args)
    elif args.command == "deploy":
        cmd_deploy(args)
    elif args.command == "compile-check":
        cmd_compile_check(args)
    elif args.command == "padtest":
        cmd_padtest(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
