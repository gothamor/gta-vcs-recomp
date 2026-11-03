"""The PS5 game's link (docs/PS5.md): CMake's link rule for the PS5 tree (ps5/cmake/ps5.cmake) runs this instead of the
compiler driver. Standard library only.

    ps5_link.py --sdk SDK --triangle TRI --clang CLANG [--radv A] [--lto-cache DIR] -o OUT <CMake's flags, objects, libs>

It links the game's objects and libraries with the PS5 recipe of ps5-plume-triangle (tools/radv-link.sh, as the
gta4-ps5 port uses it) into a PIE ELF (build/ps5/vcs.elf) that tools/ps5.py turns into the signed eboot.bin:

- the start-up code and C++ runtime pieces of the triangle's build (build/obj/app_crt.o, app_cpp_runtime.o: the IEEE
  floating-point state, static constructors, operator new on the heap below), its linker script (tooling/psbc) and the
  libSceAgc stubs RADV imports from;
- RADV (PS5_Mesa's libvulkan_radeon.ps5.a), whole, with the SDK's libc++, libc++abi, libunwind and the clang builtins;
- the payload SDK fork's platform layer (libps5platform.a): malloc and its family wrapped onto a heap in direct memory
  (libc's own runs out long before the game does), pthread_create onto thread stacks there, and the libc functions the
  console lacks or refuses mapped onto its own (--defsym);
- stat / lstat wrapped (ps5/runtime/ps5_stat.c: stat("") answering ENOENT); the emulated TLS is ps5/runtime/ps5_emutls.c,
  compiled into the game by CMake;
- the SDK's system-library stubs (libkernel, libScePad, libSceAudioOut, ...), as needed.

The flags are those of the SDK's own prospero-lld wrapper (-pie, 16 KB pages, emulated TLS for LTO code generation)
with ld.lld from the same LLVM as the compiler.
"""
import argparse
import glob
import os
import shlex
import shutil
import subprocess
import sys

# The recipe's wrapped and redirected names (ps5-plume-triangle tools/radv-link.sh)
WRAPPED = ["pthread_create", "pthread_join", "pthread_detach", "fclose", "fflush",
           "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc", "memalign",
           "malloc_usable_size", "reallocf", "reallocarray", "getline", "getdelim",
           "stat", "lstat"]  # (the last two: ps5/runtime/ps5_stat.c)
REDIRECTED = [
    "qsort_r", "mkstemps", "openlog", "popen", "pclose", "open_memstream", "__xuname", "__assert",
    "__memset_chk", "regcomp", "regexec", "regfree", "regerror", "localtime_r", "newlocale", "freelocale",
    "strtod_l", "strtof_l", "dladdr", "utimensat", "localeconv_l", "strtoll_l", "strtoull_l", "strtold_l",
    "snprintf_l", "sscanf_l", "asprintf_l", "strcoll_l", "strxfrm_l", "strftime_l", "wcscoll_l", "wcsxfrm_l",
    "btowc_l", "wctob_l", "iswctype_l", "mbrlen_l", "mbrtowc_l", "mbsrtowcs_l", "mbsnrtowcs_l", "wcrtomb_l",
    "wcsnrtombs_l", "mbtowc_l", "___mb_cur_max_l", "___runetype_l", "___tolower_l", "___toupper_l",
    "__runes_for_locale", "catopen", "catgets", "catclose", "backtrace", "backtrace_symbols_fd",
    "__cxa_thread_atexit_impl",
    "arc4random", "arc4random_buf", "arc4random_uniform", "gmtime_r", "statvfs", "fstatvfs",
    "futimens", "clock_nanosleep", "getaddrinfo", "freeaddrinfo", "if_nameindex", "if_freenameindex",
    "opendir", "fdopendir", "readdir", "rewinddir", "dirfd", "closedir", "nl_langinfo", "nl_langinfo_l",
    "getpwuid_r", "posix_fallocate", "access",
    "openat", "unlinkat", "fchmodat", "fstatat", "mkdirat", "renameat", "memfd_create",
]

# compiler-driver flags CMake hands a link that mean nothing to ld.lld here
DROPPED_PREFIXES = ("-fuse-ld", "-flto", "-march", "-mtune", "-O", "-g", "-fno-omit-frame-pointer", "-fprofile",
                    "-fexperimental-library", "-stdlib", "-isysroot", "-isystem", "-f", "--target", "-target")
DROPPED = {"-rdynamic", "-pthread", "-pie", "-no-pie", "-fPIC", "-fPIE", "-lpthread", "-ldl", "-lm", "-lc", "-lrt",
           "-lstdc++", "-lc++", "-lc++abi"}
DROPPED_WITH_VALUE = {"-isysroot", "-isystem", "-target", "-stdlib++-isystem", "-Xclang"}  # (the next word is theirs)


def fail(message):
    print("[ps5-link] " + message, file=sys.stderr)
    sys.exit(2)


def expand(args):
    """CMake's response files (@file, Ninja's long command lines) read in place."""
    out = []
    for a in args:
        if a.startswith("@") and os.path.isfile(a[1:]):
            with open(a[1:], encoding="utf-8", errors="replace") as f:
                out += expand(shlex.split(f.read()))
        else:
            out.append(a)
    return out


def split_inputs(args):
    """(objects and libraries, flags for ld.lld) from CMake's link arguments."""
    inputs, flags = [], []
    i = 0
    while i < len(args):
        a = args[i]
        i += 1
        if a.startswith("-Wl,"):
            flags += [w for w in a[4:].split(",") if w]
        elif a in ("-Xlinker",) and i < len(args):
            flags.append(args[i])
            i += 1
        elif a in DROPPED_WITH_VALUE:
            i += 1
        elif a == "-mllvm" and i < len(args):
            flags += [a, args[i]]
            i += 1
        elif a in DROPPED or a.startswith(DROPPED_PREFIXES):
            continue
        elif a.startswith("-l") or a.startswith("-L"):
            flags.append(a)
        elif a.startswith("-"):
            flags.append(a)  # (anything else: ld.lld says when it does not know it)
        else:
            inputs.append(a)
    return inputs, flags


def archive_symbols(path):
    """The global symbols an ar archive's index lists (the GNU "/" or "/SYM64/" index llvm-ar writes, thin archives
    too); an empty set when it has none or can't be read."""
    try:
        with open(path, "rb") as f:
            if f.read(8) not in (b"!<arch>\n", b"!<thin>\n"):
                return set()
            header = f.read(60)
            name = header[:16].rstrip(b" ")
            if len(header) < 60 or name not in (b"/", b"/SYM64/"):
                return set()
            data = f.read(int(header[48:58].strip() or b"0"))
    except (OSError, ValueError):
        return set()
    width = 8 if name == b"/SYM64/" else 4
    count = int.from_bytes(data[:width], "big")
    return {n.decode("latin-1") for n in data[width + count * width:].split(b"\0")[:count]}


# PS5_Mesa's build bundles zlib (ps5-plume-triangle tools/build-radv.sh: -Dzlib=enabled --force-fallback-for=zlib, the
# wrap's 1.3.1) into libvulkan_radeon.ps5.a, which is linked whole: the game's own zlib (src/platform's FetchContent,
# the same 1.3.1) would then define every zlib symbol twice. With these in RADV's index, its zlib is the game's too.
ZLIB_CORE = {"deflate", "deflateInit2_", "inflate", "inflateInit2_", "adler32", "crc32"}


def find_lld(clang):
    for cand in (os.path.join(os.path.dirname(os.path.realpath(shutil.which(clang) or clang)), "ld.lld"),
                 shutil.which("ld.lld")):
        if cand and os.path.isfile(cand):
            return cand
    fail("ld.lld not found next to %s or on PATH (the LLVM linker: e.g. sudo apt install lld-19)" % clang)


def find_builtins(clang, triangle):
    """compiler-rt's builtins for x86-64 (the recipe takes the host clang's, else the triangle's copy)."""
    try:
        resource = subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True, timeout=60).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        resource = ""
    cands = []
    if resource:
        cands += [os.path.join(resource, "lib", "linux", "libclang_rt.builtins-x86_64.a")]
        cands += glob.glob(os.path.join(resource, "lib", "*x86_64*", "libclang_rt.builtins.a"))
        cands += glob.glob(os.path.join(resource, "lib", "*", "libclang_rt.builtins-x86_64.a"))
    cands.append(os.path.join(triangle, "vendor", "ps5", "sdk", "libclang_rt.builtins-x86_64.a"))
    for c in cands:
        if os.path.isfile(c):
            return c
    fail("compiler-rt builtins for x86-64 not found (install libclang-rt-<N>-dev, or build ps5-plume-triangle)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", required=True, help="the PS5 payload SDK (the fork's: target/lib/libps5platform.a)")
    ap.add_argument("--triangle", required=True, help="ps5-plume-triangle checkout, built once")
    ap.add_argument("--clang", required=True, help="the compiler the game was built with (ld.lld is taken beside it)")
    ap.add_argument("--radv", help="libvulkan_radeon.ps5.a (default: the triangle's .deps/native/radv-release)")
    ap.add_argument("--lto-cache", help="ThinLTO cache folder (PS2X_LTO=THIN)")
    ap.add_argument("--dry-run", action="store_true", help="print the ld.lld command, link nothing")
    ap.add_argument("-o", dest="output", required=True)
    known, rest = ap.parse_known_args()

    sdk, tri = os.path.abspath(known.sdk), os.path.abspath(known.triangle or "")
    if not known.triangle or not os.path.isdir(tri):
        fail("VCS_PS5_TRIANGLE is not set to a ps5-plume-triangle checkout (docs/PS5.md: clone it and build it once)")
    lib = os.path.join(sdk, "target", "lib")
    radv = os.path.abspath(known.radv) if known.radv else os.path.join(tri, ".deps", "native", "radv-release", "lib",
                                                                       "libvulkan_radeon.ps5.a")
    needed = {
        "the triangle's start-up code (build it once: make in the triangle)": [
            os.path.join(tri, "build", "obj", "app_crt.o"), os.path.join(tri, "build", "obj", "app_cpp_runtime.o")],
        "the triangle's libSceAgc stubs": [
            os.path.join(tri, "build", "stubs", "libSceAgc.so"), os.path.join(tri, "build", "stubs", "libSceAgcDriver.so")],
        "the triangle's linker script and symbol map": [
            os.path.join(tri, "tooling", "psbc", "ps5-pie-unwind.ld"), os.path.join(tri, "tooling", "native", "ps5-pie.ld"),
            os.path.join(tri, "tooling", "native", "app-symbols.map")],
        "RADV (PS5_Mesa's libvulkan_radeon.ps5.a: the triangle's make deps builds it)": [radv],
        "the SDK fork's platform layer and C++ runtime (the triangle's .deps/native/ps5-payload-sdk)": [
            os.path.join(lib, n) for n in ("libps5platform.a", "libc++.a", "libc++abi.a", "libunwind.a")],
    }
    missing = [(what, p) for what, paths in needed.items() for p in paths if not os.path.isfile(p)]
    if missing:
        for what, p in missing:
            print("[ps5-link] missing %s: %s" % (what, p), file=sys.stderr)
        fail("the PS5 link needs a built ps5-plume-triangle and the payload SDK fork (docs/PS5.md)")

    inputs, flags = split_inputs(expand(rest))
    # absolute: tools/ps5.py links again from elsewhere with the saved response file (the RELRO padding)
    inputs = [os.path.abspath(i) for i in inputs]
    flags = ["-L" + os.path.abspath(f[2:]) if f.startswith("-L") and len(f) > 2 else f for f in flags]
    own_zlib = [i for i in inputs if os.path.basename(i) == "libz.a"]
    if own_zlib and ZLIB_CORE <= archive_symbols(radv):
        inputs = [i for i in inputs if i not in own_zlib]
        print("[ps5-link] zlib: RADV's (it bundles zlib 1.3.1, linked whole); the game's own left out (%s)"
              % ", ".join(os.path.relpath(i) for i in own_zlib))
    out = os.path.abspath(known.output)
    work = os.path.join(os.path.dirname(out), "ps5-link")
    os.makedirs(work, exist_ok=True)
    # the redirected names stay local to the game (the recipe's version script)
    local_map = os.path.join(work, "platform-local.map")
    with open(local_map, "w", encoding="utf-8") as f:
        f.write("{\n    local:\n" + "".join("        %s;\n" % n for n in REDIRECTED + ["___mb_cur_max"]) + "};\n")

    lld = find_lld(known.clang)
    cmd = [lld, "-m", "elf_x86_64", "-pie", "--eh-frame-hdr", "-z", "max-page-size=0x4000", "--hash-style=gnu",
           "-mllvm", "-emulated-tls",
           "-T", os.path.join(tri, "tooling", "psbc", "ps5-pie-unwind.ld"), "-L", os.path.join(tri, "tooling", "native"),
           "--no-dynamic-linker", "--exclude-libs=ALL", "-e", "_start", "-o", out]
    cmd += ["--wrap=" + n for n in WRAPPED]
    cmd += ["--defsym=%s=ps5_%s" % (n, n) for n in REDIRECTED] + ["--defsym=___mb_cur_max=ps5____mb_cur_max_l"]
    cmd += ["--version-script", local_map, "--version-script", os.path.join(tri, "tooling", "native", "app-symbols.map")]
    if known.lto_cache:
        cmd += ["--thinlto-cache-dir=" + known.lto_cache, "--thinlto-cache-policy=cache_size_bytes=1g:prune_after=72h"]
    cmd += ["-Map", os.path.join(work, "vcs.map")]
    cmd += flags
    cmd += [os.path.join(tri, "build", "obj", "app_crt.o"), os.path.join(tri, "build", "obj", "app_cpp_runtime.o")]
    cmd += ["--start-group"] + inputs + ["--end-group"]
    cmd += [os.path.join(tri, "build", "stubs", "libSceAgc.so"), os.path.join(tri, "build", "stubs", "libSceAgcDriver.so")]
    cmd += ["-L", lib, "--whole-archive", radv, "--no-whole-archive",
            "--start-group", os.path.join(lib, "libc++.a"), os.path.join(lib, "libc++abi.a"), os.path.join(lib, "libunwind.a"),
            find_builtins(known.clang, tri), os.path.join(lib, "libps5platform.a"), "--end-group"]
    cmd += ["--as-needed"] + sorted(glob.glob(os.path.join(lib, "*.so")))
    with open(os.path.join(work, "link.rsp"), "w", encoding="utf-8") as f:
        f.write("\n".join(shlex.quote(c) for c in cmd[1:]) + "\n")
    if known.dry_run:
        print(" ".join(shlex.quote(c) for c in cmd))
        return 0
    print("[ps5-link] %d objects and libraries + RADV -> %s" % (len(inputs), os.path.relpath(out)))
    result = subprocess.run([lld, "@" + os.path.join(work, "link.rsp")])
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
