#!/usr/bin/env python3
"""Mod loader check (src/mods/modloader.cpp, docs/MODS.md "The mod loader"): mods found, ignored and prioritised as
modloader.ini says, and F2 > Mods' switches applied while the game runs.

    python tools/mods_check.py [OUT] --pack DIR [--port 47666]

Builds a modloader/ under OUT: "HD" (a link to the texture pack DIR, priority 50), "Tinted" (half the textures the game
dumps at a city spot, painted magenta, priority 60 in modloader.ini), "_old" (ignored by modloader.ini) and ".hidden"
(not a mod). Then one hidden run (VCS_TEST_MODS=1) at that spot, switching with the `setting` command as F2 does:
everything on (magenta: Tinted beats HD), Tinted off (no magenta, HD stays), Replace textures off (the game's own),
on again, Load mods off. Each step: a screenshot, the magenta share, the mean difference from the game's own picture,
the [textures] / [mods] log. Results: OUT/result.json. Exit code 0 = pass.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

SPOT = bench.SPOTS[0]


def link(target, link_path):
    """A folder link: a junction on Windows (no rights needed), a symlink elsewhere."""
    if tc.IS_WINDOWS:
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(link_path), str(target)], check=True, capture_output=True)
    else:
        os.symlink(str(target), str(link_path), target_is_directory=True)


def unlink(path):
    if path.is_symlink() or (tc.IS_WINDOWS and path.is_dir() and os.path.realpath(path) != str(path.resolve(strict=False))):
        os.rmdir(path) if tc.IS_WINDOWS else os.unlink(path)


def picture_stats(path, reference=None):
    w, h, rows = tc.read_png(str(path))
    rows = rows[:h * 4 // 5]  # the HUD's bottom band aside
    magenta = sum(1 for row in rows for x in range(0, w * 3, 3) if row[x] > 200 and row[x + 1] < 60 and row[x + 2] > 200)
    stats = {'magenta': round(magenta / (w * len(rows)), 4)}
    if reference:
        _, _, ref = tc.read_png(str(reference))
        total = count = 0
        for y in range(0, len(rows), 4):
            for x in range(0, w * 3, 12):
                total += abs(rows[y][x] - ref[y][x]) + abs(rows[y][x + 1] - ref[y][x + 1]) + abs(rows[y][x + 2] - ref[y][x + 2])
                count += 3
        stats['diff_from_original'] = round(total / count, 2)
    return stats


def start(name, out, env, port):
    g = rig.Game.start(name=name, seconds=400, data_dir=str(out / ('data_' + name)), port=port,
                       env=dict({'VCS_ASPECT': '43', 'VCS_PERF_OVERLAY': '0'}, **env))
    g.new_game()
    g.set_time(12)
    g.weather(0)
    g.invincible()
    g.teleport(SPOT[1], SPOT[2], -100, heading=SPOT[3])
    time.sleep(6)
    return g


def check(out, pack, port):
    out.mkdir(parents=True, exist_ok=True)
    mods = out / 'modloader'
    if mods.is_dir():
        for child in mods.iterdir():
            if child.name == 'HD':
                unlink(child)
        shutil.rmtree(mods, ignore_errors=True)
    dumps = out / 'dumps'
    if not any(dumps.glob('*.dds')):
        g = start('mods_dump', out, {'VCS_TEXTURE_DUMP': '1', 'VCS_TEXTURE_DUMP_DIR': str(dumps)}, port)
        g.quit()
    names = sorted(p.name for p in dumps.glob('*.dds'))
    if not names:
        raise RuntimeError('no textures dumped')
    (mods / 'Tinted').mkdir(parents=True)
    for name in names[::2]:
        data = bytearray((dumps / name).read_bytes())
        for i in range(128, len(data) - 3, 4):
            data[i:i + 3] = b'\xff\x00\xff'
        (mods / 'Tinted' / name).write_bytes(bytes(data))
    link(Path(pack).resolve(), mods / 'HD')
    (mods / '_old').mkdir()
    shutil.copy(dumps / names[0], mods / '_old' / names[0])
    (mods / '.hidden').mkdir()
    (mods / 'modloader.ini').write_text('[Profiles.Default.Priority]\nTinted=60\n[Profiles.Default.IgnoreMods]\n_old*\n',
                                        encoding='ascii')

    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)}
    steps = [('all_on', None), ('tinted_off', 'mods.tinted 0'), ('textures_off', 'graphics.texture_replacements 0'),
             ('textures_on', 'graphics.texture_replacements 1'), ('mods_off', 'mods.enabled 0')]
    shots = {}
    g = start('mods_check', out, env, port)
    try:
        for step, command in steps:
            if command:
                reply = g.cmd('setting ' + command)
                if not reply.startswith('ok') and not reply[:1].isdigit():
                    raise RuntimeError('setting %s: %s' % (command, reply))
            time.sleep(5)  # the switch at the next frame, the pack's images loaded in the background
            shots[step] = out / ('%s.png' % step)
            g.cmd('screenshot ' + str(shots[step]))
    finally:
        g.quit()
    log = Path(g.log_path).read_text(encoding='utf-8', errors='replace')
    (out / 'run.log').write_text(log, encoding='utf-8')
    result = {'dumped': len(names), 'tinted': len(names[::2])}
    for step, _ in steps:
        result[step] = picture_stats(shots[step], shots['textures_off'])
    found = re.search(r'\[mods\] (\d+) mod\(s\)', log)
    result['mods_found'] = int(found.group(1)) if found else -1
    result['ignored_old'] = '[mods] _old: ignored by modloader.ini' in log
    result['switches'] = len(re.findall(r'\[textures\] pack switched', log))
    result['tinted_in_use'] = re.findall(r'\[textures\]   Tinted \(priority (\d+)\): (\d+) in use', log)[:1]
    ok = (result['mods_found'] == 2 and result['ignored_old'] and result['switches'] >= 5 and
          result['all_on']['magenta'] > 0.005 and result['tinted_off']['magenta'] < 0.005 and
          result['textures_on']['magenta'] < 0.005 and
          result['tinted_off']['diff_from_original'] > 2 * max(result['mods_off']['diff_from_original'], 0.5) and
          '[crash]' not in log and '[textures] failed' not in log)
    result['pass'] = ok
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--pack', required=True, help='a texture pack folder (PCSX2 names), linked in as the "HD" mod')
    ap.add_argument('--port', type=int, default=47666)
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'mods_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    result = check(out, a.pack, a.port)
    print(json.dumps(result, indent=1))
    print('PASS' if result['pass'] else 'FAIL', '-', out)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
