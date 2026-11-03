#!/usr/bin/env python3
"""Game files from mods (src/game/vcs_stream.cpp setStreamFiles, src/mods/modloader.cpp; docs/MODS.md "Game files"):
one GTA3PS2.IMG entry replaced from a mod's file, the archive untouched.

    python tools/gamefiles_check.py [OUT] [--save DIR]

Needs build/img/streaming.json (python tools/img.py table). Three hidden runs from the card copy (default the rig's
user copy), each spawning an ADMIRAL (model 171) at a city street and taking a picture: no mod; a mod holding
admiral.mdl = the disc's own entry (must look the same: the overlay path changes nothing); a mod holding the CHEETAH's
entry as admiral.mdl (must look different: the ADMIRAL drawn as a Cheetah, no crash). Mod folders under OUT; the
extracted entries stay there (game data: never in git). Results: OUT/result.json. Exit code 0 = pass.
"""
import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img  # noqa: E402
import mods_check  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

STREET = (575.0, 175.0, 0.0)


def run(out, name, mods, save):
    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)} if mods else {}
    g = rig.Game.start(save=save, name='gamefiles_' + name, seconds=300, env=env)
    try:
        g.wait_ingame(timeout=180)
        g.set_time(12)
        g.weather(0)
        g.teleport(STREET[0], STREET[1], -100.0, heading=STREET[2])
        time.sleep(4)
        g.spawn_car(171, ahead=4.0)
        time.sleep(5)
        shot = out / (name + '.png')
        g.cmd('screenshot ' + str(shot))
        alive = g.alive()
    finally:
        g.quit()
    return shot, Path(g.log_path).read_text(encoding='utf-8', errors='replace'), alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'gamefiles_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    table = img.load_table()
    by_name = {e.get('name'): e for e in table['entries']}
    entries = out / 'entries'
    with open(img.iso_path(None), 'rb') as iso:
        def entry(name):
            e = by_name[name]
            iso.seek((table['img']['lsn'] + e['offset']) * 2048)
            return iso.read(e['sectors'] * 2048)
        admiral, cheetah = entry('admiral'), entry('cheetah')
    for mod, data in (('same', admiral), ('swap', cheetah)):
        folder = out / ('modloader_' + mod) / ('Test ' + mod)
        if folder.parent.is_dir():
            shutil.rmtree(folder.parent)
        folder.mkdir(parents=True)
        (folder / 'admiral.mdl').write_bytes(data)
    shots, logs, alive = {}, {}, {}
    for name, mods in (('original', None), ('same', out / 'modloader_same'), ('swap', out / 'modloader_swap')):
        shots[name], logs[name], alive[name] = run(out, name, mods, a.save)
    result = {name: mods_check.picture_stats(shots[name], shots['original']) for name in shots}
    result['resolved'] = {n: 'admiral.mdl -> GTA3PS2.IMG entry 171' in logs[n] for n in ('same', 'swap')}
    result['alive'] = alive
    result['pass'] = (all(result['resolved'].values()) and all(alive.values()) and
                      result['swap']['diff_from_original'] > max(3 * result['same']['diff_from_original'], 1.0) and
                      not any('[crash]' in t for t in logs.values()))
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    print(json.dumps(result, indent=1))
    print('PASS' if result['pass'] else 'FAIL', '-', out)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
