#!/usr/bin/env python3
"""Check the PNG replacement loader and an installed PCSX2 pack (hidden, isolated card).

    python tools/texture_check.py [OUT] [--pack SLUS-21590] [--loader-only]

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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        size_t loaded = 0;
        while (!expected.empty() && std::chrono::steady_clock::now() < deadline) {
            for (const auto &image : pack.takeReady()) {
                CHECK(expected.contains(image->id));
                const auto size = expected.at(image->id);
                CHECK(image->width == size.w && image->height == size.h);
                CHECK(image->pixels.size() == size_t(size.w) * size.h);
                expected.erase(image->id); ++loaded;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(expected.empty());
        std::printf("pack image validation passed: %zu PNGs decoded\n", loaded);
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
    r = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=15)
    (out / 'loader.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    if r.returncode or r.stderr.count('failed to load') != 1:
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
        for path in sorted(images.rglob('*')):
            if path.suffix.lower() != '.png':
                continue
            fields = path.stem.split('-')
            if len(fields) == 2:
                fields.insert(1, '0')
            if len(fields) != 3 or any(not re.fullmatch('[0-9a-fA-F]+', f) for f in fields):
                continue
            with path.open('rb') as f:
                header = f.read(24)
            if header[:8] != b'\x89PNG\r\n\x1a\n':
                raise RuntimeError('invalid PNG header: ' + str(path))
            w, h = struct.unpack('>II', header[16:24])
            records.append(' '.join(fields) + ' %d %d' % (w, h))
        manifest.write_text('\n'.join(records), encoding='ascii')
        env['VCS_TEXTURE_PACK'] = str(pack)
        r = subprocess.run([str(exe), str(manifest)], env=env, capture_output=True, text=True, timeout=60)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?', default='build/texture_check/latest')
    ap.add_argument('--pack', default='SLUS-21590')
    ap.add_argument('--loader-only', action='store_true')
    ap.add_argument('--validate-pack', action='store_true', help='decode every full-texture PNG in --pack without running the game')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    root = (Path(tc.ROOT) / 'build').resolve()
    if not out.is_relative_to(root):
        ap.error('test output must be under the checkout build/ directory')
    out.mkdir(parents=True, exist_ok=True)
    loader_checks(out, a.pack if a.validate_pack else None)
    if not a.loader_only:
        game_check(out, Path(a.pack).resolve())


if __name__ == '__main__':
    main()
