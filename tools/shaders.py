#!/usr/bin/env python3
"""The Vulkan (SPIR-V) and Metal (MSL) shaders, compiled once and committed - no shader compiler on a player's machine
(DECISIONS.md 2026-11-27). Standard library only.

    python tools/shaders.py build              compile every shader of the manifests into their compiled/ folders
                                               (glslang + SPIRV-Cross from tools/shader_tools.py, built the first time)
    python tools/shaders.py check [--rebuild]  are compiled/ current? (exit 0 / 1); --rebuild also compiles everything
                                               and compares byte for byte (CI: the outputs are the same on every host)
    python tools/shaders.py header --spirv F.spv | --msl F.metal --name N --source S --out H [--entry E -D ...]
                                               the C header the build includes (what tools/hlsl_spirv.py /
                                               tools/hlsl_msl.py write from a live compile)

The manifests: src/render/shaders/shaders.txt (the game) and launcher/render/shaders.txt (the launcher) - one line per
shader, `file entry profile name header [DEFINE=VALUE ...]`, and `target-env vulkan1.x`. Next to each, compiled/ holds
spirv/<header>.spv, msl/<header>.metal and manifest.json: the hash of what they were made from (the HLSL in the
manifest's folder, the manifest, the converting scripts, the compilers' pins). D3D11's DXBC is still compiled at build
time, with Windows' own d3dcompiler (tools/hlsl_dxbc.py).
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import hlsl_msl  # noqa: E402
import hlsl_spirv  # noqa: E402

MANIFESTS = ["src/render/shaders/shaders.txt", "launcher/render/shaders.txt"]
# what the outputs depend on besides the manifest's own folder
TOOL_INPUTS = ["tools/hlsl_spirv.py", "tools/hlsl_msl.py", "tools/shader_tools/CMakeLists.txt"]


class Shader:
    def __init__(self, file, entry, profile, name, header, defines):
        self.file, self.entry, self.profile, self.name, self.header, self.defines = file, entry, profile, name, header, defines
        self.stage = "vert" if profile.startswith("vs") else "frag"
        self.base = header[:-2] if header.endswith(".h") else header


def read_manifest(path):
    """(target env, [Shader])"""
    target_env, shaders = "vulkan1.3", []
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            parts = line.split("#", 1)[0].split()
            if not parts:
                continue
            if parts[0] == "target-env" and len(parts) == 2:
                target_env = parts[1]
            elif len(parts) >= 5:
                shaders.append(Shader(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5:]))
            else:
                sys.exit("shaders: %s:%d: expected `file entry profile name header [DEFINE=VALUE ...]`" % (path, number))
    return target_env, shaders


def _show(path):
    try:
        return os.path.relpath(path, ROOT).replace(os.sep, "/")
    except ValueError:  # another drive (a temporary folder)
        return path


def _text_bytes(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n")  # a CRLF checkout is the same source


def inputs_hash(manifest):
    """SHA-256 over the manifest, every .hlsl below its folder and TOOL_INPUTS (line endings normalised)."""
    folder = os.path.dirname(manifest)
    files = [manifest] + [os.path.join(ROOT, t) for t in TOOL_INPUTS]
    for base, dirs, names in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d != "compiled")
        files += [os.path.join(base, n) for n in sorted(names) if n.endswith(".hlsl")]
    h = hashlib.sha256()
    for path in files:
        h.update(os.path.relpath(path, ROOT).replace(os.sep, "/").encode() + b"\0")
        h.update(_text_bytes(path) + b"\0")
    return h.hexdigest()


def compiled_dir(manifest):
    return os.path.join(os.path.dirname(manifest), "compiled")


def outputs(manifest):
    """{relative path in compiled/: Shader, kind}"""
    _, shaders = read_manifest(manifest)
    out = {}
    for s in shaders:
        out["spirv/%s.spv" % s.base] = (s, "spirv")
        out["msl/%s.metal" % s.base] = (s, "msl")
    return out


def compile_all(manifest, programs, into):
    """Every output of the manifest into the folder `into`."""
    target_env, shaders = read_manifest(manifest)
    folder = os.path.dirname(manifest)
    work = tempfile.mkdtemp(prefix="vcs_shaders_")
    try:
        for rel, (s, kind) in sorted(outputs(manifest).items()):
            source = os.path.join(folder, s.file)
            path = os.path.join(into, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if kind == "spirv":
                words = hlsl_spirv.compile_spirv(programs["glslang"], s.stage, s.entry, s.defines, source, target_env, work)
                with open(path, "wb") as f:
                    f.write(struct.pack("<%dI" % len(words), *words))
            else:
                msl = hlsl_msl.compile_msl(programs["glslang"], programs["spirv_cross"], s.stage, s.entry, s.defines,
                                           source, work)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(msl)
            print("[shaders] %s" % _show(path))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _programs():
    import toolchain as tc
    import shader_tools
    return shader_tools.ensure(tc.build_env())


def build(manifests=None):
    programs = _programs()
    for rel in manifests or MANIFESTS:
        manifest = os.path.join(ROOT, rel)
        target = compiled_dir(manifest)
        fresh = target + ".new"
        shutil.rmtree(fresh, ignore_errors=True)
        compile_all(manifest, programs, fresh)
        with open(os.path.join(fresh, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({"inputs": inputs_hash(manifest), "made_by": "tools/shaders.py build"}, f, indent=1)
            f.write("\n")
        shutil.rmtree(target, ignore_errors=True)
        os.replace(fresh, target)
        print("[shaders] %s: %d files" % (os.path.relpath(target, ROOT).replace(os.sep, "/"), len(outputs(manifest))))


def stale(manifests=None):
    """The manifests whose compiled/ does not match their inputs (or is missing files)."""
    out = []
    for rel in manifests or MANIFESTS:
        manifest = os.path.join(ROOT, rel)
        target = compiled_dir(manifest)
        try:
            with open(os.path.join(target, "manifest.json"), encoding="utf-8") as f:
                recorded = json.load(f).get("inputs")
        except (OSError, ValueError):
            recorded = None
        missing = [p for p in outputs(manifest) if not os.path.isfile(os.path.join(target, p))]
        if recorded != inputs_hash(manifest) or missing:
            out.append(rel)
    return out


def check(rebuild=False, warn=False, stamp=None, manifests=None):
    bad = stale(manifests)
    for rel in bad:
        msg = "%s changed since compiled/ was made: run python tools/shaders.py build and commit compiled/" % rel
        print(("warning: " if warn else "[shaders] ") + msg)
    if rebuild and not bad:
        programs = _programs()
        for rel in manifests or MANIFESTS:
            manifest = os.path.join(ROOT, rel)
            fresh = tempfile.mkdtemp(prefix="vcs_shaders_check_")
            try:
                compile_all(manifest, programs, fresh)
                for p in sorted(outputs(manifest)):
                    with open(os.path.join(fresh, p), "rb") as a, open(os.path.join(compiled_dir(manifest), p), "rb") as b:
                        if a.read() != b.read():
                            print("[shaders] %s/compiled/%s differs from a fresh compile" % (os.path.dirname(rel), p))
                            bad.append(rel)
            finally:
                shutil.rmtree(fresh, ignore_errors=True)
    if stamp:
        os.makedirs(os.path.dirname(os.path.abspath(stamp)), exist_ok=True)
        with open(stamp, "w", encoding="utf-8") as f:
            f.write("stale\n" if bad else "current\n")
    if not bad:
        print("[shaders] compiled shaders are current" + (" (and the same as a fresh compile)" if rebuild else ""))
    return not bad or warn


def header(a):
    if a.spirv:
        with open(a.spirv, "rb") as f:
            words = hlsl_spirv.words_of(f.read())
        hlsl_spirv.write_header(a.out, a.name, words, a.source)
    else:
        with open(a.msl, encoding="utf-8") as f:
            msl = f.read()
        hlsl_msl.write_header(a.out, a.name, msl, a.source, a.entry, a.defines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--manifest", action="append", help="only this manifest (repository-relative)")
    c = sub.add_parser("check")
    c.add_argument("--rebuild", action="store_true", help="also compile everything and compare")
    c.add_argument("--warn", action="store_true", help="report stale outputs as a warning and exit 0 (the build)")
    c.add_argument("--stamp", help="write this file (the build's dependency)")
    c.add_argument("--manifest", action="append", help="only this manifest (repository-relative)")
    h = sub.add_parser("header")
    g = h.add_mutually_exclusive_group(required=True)
    g.add_argument("--spirv")
    g.add_argument("--msl")
    h.add_argument("--name", required=True)
    h.add_argument("--source", required=True)
    h.add_argument("--out", required=True)
    h.add_argument("--entry", default="")
    h.add_argument("-D", dest="defines", action="append", default=[])
    a = ap.parse_args()
    if a.cmd == "build":
        build(a.manifest)
    elif a.cmd == "check":
        sys.exit(0 if check(a.rebuild, a.warn, a.stamp, a.manifest) else 1)
    else:
        header(a)


if __name__ == "__main__":
    main()
