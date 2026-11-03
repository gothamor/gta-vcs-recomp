#!/usr/bin/env python3
"""Check the PNG replacement loader and an installed PCSX2 pack (hidden, isolated card).

    python tools/texture_check.py [OUT] [--pack textures/SLUS-21590] [--loader-only]

Loader checks need a C++20 compiler, no game/disc: legacy flags, palette identity, recursive indexing,
non-power-of-two PNG decoding, unchanged alpha, asynchronous deduplication/reload, corrupt-file fallback,
and clean worker shutdown. The game check visits Fort Baxter and four city spots, captures pictures/perf,
and requires pack matches and successful uploads. All generated fixtures and output stay under build/.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench
import rig
import toolchain as tc


def png(path, width, height, rgba):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    data = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(data)) + chunk(b'IEND', b''))


DXGI = {'BC1': 71, 'BC3': 77, 'BC7': 98, 'RGBA8': 28, 'RGBA32F': 2}
BLOCK = {'BC1': 8, 'BC3': 16, 'BC7': 16}


def level_bytes(fmt, w, h):
    if fmt in BLOCK:
        return ((w + 3) // 4) * ((h + 3) // 4) * BLOCK[fmt]
    return w * h * (16 if fmt == 'RGBA32F' else 4)


def dds(path, width, height, fmt, levels, legacy=None, cube=False, extra=None, data=None):
    """A DDS fixture: `levels` levels claimed; the data is bytes i & 0xFF for the levels that fit `extra` (None: all)."""
    flags = 0x1007 | (0x20000 if levels > 1 else 0)
    if legacy in ('DXT1', 'DXT5'):
        pf = struct.pack('<2I4s5I', 32, 0x4, legacy.encode(), 0, 0, 0, 0, 0)
    elif legacy == 'BGRA':
        pf = struct.pack('<2I4s5I', 32, 0x41, b'\0\0\0\0', 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    else:
        pf = struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    header = (b'DDS ' + struct.pack('<7I', 124, flags, height, width, 0, 0, levels) + b'\0' * 44 + pf +
              struct.pack('<5I', 0x1000 | (0x400008 if levels > 1 else 0), 0xFE00 if cube else 0, 0, 0, 0))
    if legacy is None:
        header += struct.pack('<5I', DXGI[fmt], 3, 0, 1, 0)
    if data is None:
        size = sum(level_bytes(fmt, max(1, width >> i), max(1, height >> i)) for i in range(levels))
        size = size if extra is None else extra
        data = bytes(i & 0xFF for i in range(size))
    path.write_bytes(header + data)


HARNESS = r'''
#include "render/texture_replacements.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    CHECK(render::textureHash("", 0) == 0x2d06800538d394c2ull); // xxHash's published empty-input XXH3-64 vector
    render::TextureReplacements pack;
    if (argc == 2 && std::string_view(argv[1]) == "--empty") {
        CHECK(!pack.candidate(0x1994));
        CHECK(pack.takeReady().empty());
        return 0;
    }
    if (argc == 2) {
        std::ifstream manifest(argv[1]);
        CHECK(manifest.good());
        struct Size { unsigned w, h; };
        std::unordered_map<unsigned, Size> expected;
        uint64_t pixels, palette; unsigned bits, w, h;
        while (manifest >> std::hex >> pixels >> palette >> bits >> std::dec >> w >> h) {
            const auto id = pack.find(pixels, palette, bits & ~0x4000u);
            CHECK(id);
            expected[id] = {w, h};
            pack.request(id);
        }
        CHECK(!expected.empty());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
        size_t loaded = 0;
        while (!expected.empty() && std::chrono::steady_clock::now() < deadline) {
            for (const auto &image : pack.takeReady()) {
                CHECK(expected.contains(image->id));
                const auto size = expected.at(image->id);
                CHECK(image->width == size.w && image->height == size.h);
                CHECK(image->dds() ? !image->levels.empty() : image->pixels.size() == size_t(size.w) * size.h);
                expected.erase(image->id); ++loaded;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(expected.empty());
        std::printf("pack image validation passed: %zu images loaded\n", loaded);
        return 0;
    }
    CHECK(pack.candidate(0x1994));
    CHECK(!pack.candidate(0x1993));
    auto id = pack.find(0xabc, 0xdef, 0x1994); // old filename's TCC bit must be ignored
    CHECK(id != 0);
    CHECK(pack.find(0xabc, 0xdee, 0x1994) == 0); // same pixels, different palette must not match
    CHECK(pack.find(0xabd, 0xdef, 0x1994) == 0);
    CHECK(pack.find(0xaaa, 0, 0x1994) == 0); // -mip / region / malformed names were rejected
    for (int reload = 0; reload < 2; ++reload) {
        for (int i = 0; i < 100; ++i) pack.request(id);
        bool found = false;
        for (int i = 0; i < 200; ++i) {
            auto ready = pack.takeReady();
            if (!ready.empty()) {
                CHECK(ready.size() == 1);
                CHECK(ready[0]->id == id && ready[0]->width == 3 && ready[0]->height == 2);
                CHECK(ready[0]->pixels == std::vector<uint32_t>({0x801e140a, 0x00463228, 0xff645a50,
                                                              0x40786e64, 0x208c8278, 0x01a0968c}));
                found = true; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(found);
        CHECK(pack.takeReady().empty());
    }
    auto broken = pack.find(0x112, 0x334, 0x1994);
    CHECK(broken != 0);
    pack.request(broken);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < 100; ++i) pack.request(broken);
    CHECK(pack.takeReady().empty()); // no decoded image means the renderer keeps its original
    // DDS: the levels as stored, nothing decoded; a DDS wins over a PNG of the same name; refused files keep originals
    auto wait = [](render::TextureReplacements &from, uint32_t id) -> std::shared_ptr<const render::ReplacementImage> {
        if (!id) return {};
        from.request(id);
        for (int i = 0; i < 300; ++i) {
            for (auto &image : from.takeReady()) if (image->id == id) return image;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return {};
    };
    auto bc7 = wait(pack, pack.find(0xddd, 0xeee, 0x1994));
    CHECK(bc7 && bc7->dds() && bc7->format == rhi::Format::BC7 && bc7->width == 8 && bc7->height == 8);
    CHECK(bc7->levelOffsets == std::vector<size_t>({0, 64, 80, 96}) && bc7->levels.size() == 112);
    CHECK(bc7->gpuBytes(true) == 112 && bc7->gpuBytes(false) == 64 && bc7->pixels.empty());
    CHECK(bc7->levels[1] == 1 && bc7->levels[111] == 111); // copied as stored
    auto both = wait(pack, pack.find(0x777, 0x888, 0x1994));
    CHECK(both && both->dds() && both->format == rhi::Format::RGBA8 && both->levels.size() == 16);
    auto dxt5 = wait(pack, pack.find(0x555, 0x666, 0x1994));
    CHECK(dxt5 && dxt5->format == rhi::Format::BC3 && dxt5->levelOffsets.size() == 1 && dxt5->levels.size() == 16);
    auto bgra = wait(pack, pack.find(0x444, 0x666, 0x1994));
    CHECK(bgra && bgra->format == rhi::Format::BGRA8 && bgra->levelOffsets == std::vector<size_t>({0, 32}) && bgra->levels.size() == 40);
    auto cut = wait(pack, pack.find(0x333, 0x666, 0x1994)); // 4 levels claimed, level 0 and 4 bytes stored
    CHECK(cut && cut->format == rhi::Format::BC1 && cut->levelOffsets.size() == 1 && cut->levels.size() == 32);
    for (uint64_t refused : {0xc0cull, 0xc1cull, 0xc2cull}) // a cube map, BC1 6x6, RGBA32F
        CHECK(!wait(pack, pack.find(refused, 0x666, 0x1994)));
    {
        render::TextureReplacements noBc; // a GPU without block compression keeps the original, still takes RGBA8
        noBc.setBlockCompression(false);
        CHECK(!wait(noBc, noBc.find(0xddd, 0xeee, 0x1994)));
        CHECK(wait(noBc, noBc.find(0x777, 0x888, 0x1994)));
    }
    auto plain = pack.find(0x123, 0, 0x1994);
    CHECK(plain != 0);
    pack.request(plain); // destruction while work is queued must join cleanly
    std::puts("loader checks passed");
}
'''


def loader_checks(out, validate_pack=None):
    fixtures = out / 'fixtures' / 'replacements' / 'nested'
    fixtures.mkdir(parents=True, exist_ok=True)
    pixels = struct.pack('<6I', 0x801e140a, 0x00463228, 0xff645a50, 0x40786e64, 0x208c8278, 0x01a0968c)
    png(fixtures / 'abc-def-00005994.png', 3, 2, pixels)
    png(fixtures / '123-00001994.png', 3, 2, pixels)
    (fixtures / '112-334-00005994.PNG').write_bytes(b'not a PNG')
    for name in ('aaa-00005994-mip1.png', 'aaa-r16x16-00005994.png', 'aaa-00005994garbage.png'):
        png(fixtures / name, 3, 2, pixels)
    dds(fixtures / 'ddd-eee-00005994.dds', 8, 8, 'BC7', 4)
    png(fixtures / '777-888-00005994.png', 3, 2, pixels)
    dds(fixtures / '777-888-00005994.dds', 2, 2, 'RGBA8', 1)
    dds(fixtures / '555-666-00005994.dds', 4, 4, 'BC3', 1, legacy='DXT5')
    dds(fixtures / '444-666-00005994.dds', 4, 2, 'RGBA8', 2, legacy='BGRA')
    dds(fixtures / '333-666-00005994.dds', 8, 8, 'BC1', 4, extra=36)
    dds(fixtures / 'c0c-666-00005994.dds', 8, 8, 'BC1', 1, cube=True)
    dds(fixtures / 'c1c-666-00005994.dds', 6, 6, 'BC1', 1)
    dds(fixtures / 'c2c-666-00005994.dds', 2, 2, 'RGBA32F', 1)
    source = out / 'loader_check.cpp'
    source.write_text(HARNESS, encoding='utf-8')
    exe = out / ('loader_check.exe' if tc.IS_WINDOWS else 'loader_check')
    env = tc.build_env()
    compiler = shutil.which('clang-cl' if tc.IS_WINDOWS else 'clang++', path=env.get('PATH'))
    if not compiler:
        raise RuntimeError('C++ compiler not found')
    implementation = str(Path(tc.ROOT) / 'src/render/texture_replacements.cpp')
    include = str(Path(tc.ROOT) / 'src')
    if tc.IS_WINDOWS:
        args = [compiler, '/nologo', '/std:c++20', '/EHsc', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
                '/I' + include, str(source), implementation, '/Fe:' + str(exe), '/Fo:' + str(out) + os.sep]
    else:
        args = [compiler, '-std=c++20', '-O2', '-pthread', '-I', include, str(source), implementation, '-o', str(exe)]
    subprocess.run(args, env=env, cwd=tc.ROOT, check=True)
    env.update(VCS_TEXTURE_REPLACEMENTS='1', VCS_TEXTURE_PACK=str(out / 'fixtures'))
    r = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=60)
    (out / 'loader.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    # the broken PNG, three refused DDS, the BC7 one without block compression
    if r.returncode or r.stderr.count('failed to load') != 5:
        raise RuntimeError('loader checks failed: ' + r.stdout + r.stderr)
    print('loader checks passed', flush=True)
    for enabled, directory in [('0', out / 'fixtures'), ('1', out / 'missing_pack')]:
        empty_env = dict(env, VCS_TEXTURE_REPLACEMENTS=enabled, VCS_TEXTURE_PACK=str(directory))
        subprocess.run([str(exe), '--empty'], env=empty_env, check=True, capture_output=True, timeout=5)
    if validate_pack:
        pack = Path(validate_pack).resolve()
        images = pack / 'replacements' if (pack / 'replacements').is_dir() else pack
        manifest = out / 'pack_images.txt'
        records = []
        named = {}
        for path in sorted(images.rglob('*')):
            if path.suffix.lower() not in ('.png', '.dds'):
                continue
            fields = path.stem.split('-')
            if len(fields) == 2:
                fields.insert(1, '0')
            if len(fields) != 3 or any(not re.fullmatch('[0-9a-fA-F]+', f) for f in fields):
                continue
            with path.open('rb') as f:
                header = f.read(24)
            if path.suffix.lower() == '.dds':
                if header[:4] != b'DDS ':
                    raise RuntimeError('invalid DDS header: ' + str(path))
                h, w = struct.unpack('<II', header[12:20])
            else:
                if header[:8] != b'\x89PNG\r\n\x1a\n':
                    raise RuntimeError('invalid PNG header: ' + str(path))
                w, h = struct.unpack('>II', header[16:24])
            key = ' '.join(fields)
            if key not in named or path.suffix.lower() == '.dds':  # the loader takes the DDS of a name
                named[key] = '%s %d %d' % (key, w, h)
        records = list(named.values())
        manifest.write_text('\n'.join(records), encoding='ascii')
        env['VCS_TEXTURE_PACK'] = str(pack)
        r = subprocess.run([str(exe), str(manifest)], env=env, capture_output=True, text=True, timeout=300)
        (out / 'pack_images.log').write_text(r.stdout + r.stderr, encoding='utf-8')
        if r.returncode:
            raise RuntimeError('pack validation failed: ' + r.stdout + r.stderr)
        print(r.stdout.strip(), flush=True)


def game_check(out, pack):
    env = {'VCS_TEXTURE_REPLACEMENTS': '1', 'VCS_TEXTURE_PACK': str(pack),
           'VCS_ASPECT': '43', 'VCS_FPS': '60', 'VCS_PERF_OVERLAY': '0'}
    g = rig.Game.start(name='texture_check', seconds=300, data_dir=str(out / 'data'), port=47664, env=env)
    results = {}
    try:
        g.new_game()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        for name, x, y, heading in [('fortbaxter', None, None, 0)] + bench.SPOTS:
            if x is not None:
                g.teleport(x, y, -100, heading=heading)
            time.sleep(4)
            image = out / (name + '.png')
            reply = g.cmd('screenshot ' + str(image))
            if not image.is_file():
                raise RuntimeError(reply)
            w, h, rows = tc.read_png(str(image))
            # Central scene pixels (exclude HUD) must contain a visible world.
            samples = [sum(row[x:x + 3]) / 3 for row in rows[h // 4:3 * h // 4:4]
                       for x in range(w // 4 * 3, 3 * w // 4 * 3, 12)]
            luma = sum(samples) / len(samples)
            if luma < 8:
                raise RuntimeError(name + ': black picture')
            results[name] = {'mean_rgb': round(luma, 2), 'perf': bench.parse_perf(g.cmd('perf'))}
            print(name, 'visible, mean RGB', round(luma, 1), flush=True)
    finally:
        g.quit()
    log = Path(g.log_path).read_text(encoding='utf-8', errors='replace')
    (out / 'run.log').write_text(log, encoding='utf-8')
    results['matched'] = len(set(re.findall(r'\[textures\] matched (\S+)', log)))
    loads = re.findall(r'\[textures\] loaded (\S+)', log)
    results['loaded'] = len(set(loads))
    results['image_loads'] = len(loads)
    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    if not results['matched'] or not results['loaded']:
        raise RuntimeError('pack had no matches or successful loads; see run.log')
    if '[textures] failed' in log or '[crash]' in log:
        raise RuntimeError('texture load or game failure; see run.log')
    if len(loads) > results['loaded'] * 10:
        raise RuntimeError('replacement cache is repeatedly reloading active textures; see run.log')
    print('game check passed:', results['matched'], 'pack files matched,', results['loaded'], 'loaded', flush=True)


def dump_check(out, pack=None):
    """VCS_TEXTURE_DUMP round trip: the game writes the textures it decodes at a city spot (uncompressed DDS, PCSX2
    names); every one is painted magenta into a pack of its own, and the same spot with that pack has to show it - the
    dump's names are the loader's identities. With `pack`, the dumped names are also looked up in it (the same
    algorithm as the pack's author's PCSX2: a share of them should be there)."""
    dumps = out / 'dumps'
    if dumps.is_dir():
        shutil.rmtree(dumps)
    spot = bench.SPOTS[0]

    def visit(tag, env):
        g = rig.Game.start(name='texture_dump_' + tag, seconds=300, data_dir=str(out / ('data_' + tag)), port=47665,
                           env=dict({'VCS_ASPECT': '43', 'VCS_PERF_OVERLAY': '0'}, **env))
        try:
            g.new_game()
            g.set_time(12)
            g.weather(0)
            g.invincible()
            g.teleport(spot[1], spot[2], -100, heading=spot[3])
            time.sleep(6)
            image = out / ('dump_%s.png' % tag)
            g.cmd('screenshot ' + str(image))
        finally:
            g.quit()
        return image, Path(g.log_path).read_text(encoding='utf-8', errors='replace')

    _, log = visit('dump', {'VCS_TEXTURE_REPLACEMENTS': '0', 'VCS_TEXTURE_DUMP': '1', 'VCS_TEXTURE_DUMP_DIR': str(dumps)})
    names = sorted(p.name for p in dumps.glob('*.dds'))
    if not names:
        raise RuntimeError('no textures dumped; see the log of texture_dump_dump')
    tinted = out / 'tinted' / 'replacements'
    if tinted.parent.is_dir():
        shutil.rmtree(tinted.parent)
    tinted.mkdir(parents=True)
    for name in names:
        data = bytearray((dumps / name).read_bytes())
        if data[:4] != b'DDS ' or len(data) < 128 + 4:
            raise RuntimeError('a dump is not an uncompressed DDS: ' + name)
        for i in range(128, len(data) - 3, 4):  # RGBA8: magenta, the GS's alpha kept
            data[i:i + 3] = b'\xff\x00\xff'
        (tinted / name).write_bytes(bytes(data))
    picture, log = visit('tinted', {'VCS_TEXTURE_REPLACEMENTS': '1', 'VCS_TEXTURE_PACK': str(tinted.parent)})
    matched = len(set(re.findall(r'\[textures\] matched (\S+)', log)))
    w, h, rows = tc.read_png(str(picture))
    magenta = sum(1 for row in rows[:h * 4 // 5] for x in range(0, w * 3, 3)
                  if row[x] > 200 and row[x + 1] < 60 and row[x + 2] > 200)
    share = magenta / (w * (h * 4 // 5))
    result = {'dumped': len(names), 'matched_tinted': matched, 'magenta_share': round(share, 3)}
    if pack:
        images = Path(pack) / 'replacements' if (Path(pack) / 'replacements').is_dir() else Path(pack)
        def key(stem):  # the old TCC flag (bit 14) aside, as the loader does
            fields = stem.split('-')
            return '-'.join(fields[:-1] + ['%x' % (int(fields[-1], 16) & ~0x4000)])
        in_pack = {key(p.stem.lower()) for p in images.glob('*') if p.suffix.lower() in ('.dds', '.png')}
        result['dumped_in_pack'] = sum(key(Path(n).stem) in in_pack for n in names)
    (out / 'dump_results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('dump check:', result, flush=True)
    if matched < len(names) // 2 or share < 0.05:
        raise RuntimeError('the dumped names did not come back as replacements; see dump_results.json')
    print('dump check passed', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?', default='build/texture_check/latest')
    ap.add_argument('--pack', default='textures/SLUS-21590')
    ap.add_argument('--loader-only', action='store_true')
    ap.add_argument('--validate-pack', action='store_true', help='load every full-texture DDS / PNG in --pack without running the game')
    ap.add_argument('--dump-check', action='store_true', help='the VCS_TEXTURE_DUMP round trip (and the dumped names found in --pack)')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    root = (Path(tc.ROOT) / 'build').resolve()
    if not out.is_relative_to(root):
        ap.error('test output must be under the checkout build/ directory')
    out.mkdir(parents=True, exist_ok=True)
    loader_checks(out, a.pack if a.validate_pack else None)
    if a.dump_check:
        dump_check(out, Path(a.pack).resolve() if Path(a.pack).is_dir() else None)
    elif not a.loader_only:
        game_check(out, Path(a.pack).resolve())


if __name__ == '__main__':
    main()
