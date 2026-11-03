#!/usr/bin/env python3
"""A PC VC car mod in the game (tools/dff2mdl.py, FORK.md T9): the .dff converted onto a VCS vehicle's model, loaded
from a mod folder by the mod loader with its .txd as the vehicle's texture dictionary (tools/txd2xtx.py), spawned and
photographed.

    python tools/carmod_check.py [OUT] [--dff FILE] [--txd FILE] [--vehicle admiral] [--model 171] [--save DIR]

Needs the extracted vehicles (python tools/img.py extract). Two hidden runs from the card copy (default the rig's user
copy) at a city street: the original vehicle, then the converted one (mod folder under OUT - game data: never in git),
the player put in it and the picture taken from behind. Pass = the converted files resolved by the mod loader, the
game alive, no crash, a different picture. Results: OUT/result.json. Exit code 0 = pass.
"""
import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import txd2xtx  # noqa: E402
import gamefiles_check  # noqa: E402
import mods_check  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

DEFAULT_DFF = os.path.join(tc.ROOT, 'Mods', 'Fixed_Xbox_Vehs_1.3', 'Comet with PC style body and lights', 'comet.dff')
DEFAULT_TXD = os.path.join(tc.ROOT, 'Mods', 'Fixed_Xbox_Vehs_1.3', 'Fixed Xbox Vehicles', 'gta3.img', 'comet.txd')


def run(out, name, mods, save, model):
    """One hidden run: the car spawned at gamefiles_check's street with the player in it, photographed from behind.
    (picture, log, alive)"""
    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)} if mods else {}
    g = rig.Game.start(save=save, name='carmod_' + name, seconds=300, env=env)
    x, y, heading = gamefiles_check.STREET
    shot = out / (name + '.png')
    try:
        g.wait_ingame(timeout=180)
        g.set_time(12)
        g.weather(0)
        # the game puts the player back at the save's place in its first seconds, after a teleport too: until the
        # player stays there (two looks 3 s apart)
        for _ in range(6):
            g.teleport(x, y, -100.0, heading=heading)
            there = []
            for _ in range(2):
                time.sleep(3)
                _, (px, py, _pz), _ = g.player()
                there.append(abs(px - x) + abs(py - y) < 10.0)
            if all(there):
                break
        g.spawn_car(model, ahead=4.0)
        time.sleep(5)
        g.cmd('screenshot ' + str(shot))
        alive = g.alive()
    finally:
        g.quit()
    return shot, Path(g.log_path).read_text(encoding='utf-8', errors='replace'), alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--dff', default=DEFAULT_DFF)
    ap.add_argument('--txd', default=DEFAULT_TXD, help="the car's textures ('' for none: the template's names stay)")
    ap.add_argument('--vehicle', default='admiral', help='the VCS vehicle replaced (its model is the template)')
    ap.add_argument('--model', type=int, default=171, help="the vehicle's model number (spawned)")
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    ap.add_argument('--no-original', action='store_true', help='skip the run without the mod')
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'carmod_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    template = os.path.join(tc.ROOT, 'build', 'img', 'GTA3PS2', a.vehicle + '.mdl')
    mods = out / 'modloader'
    if mods.is_dir():
        shutil.rmtree(mods)
    folder = mods / ('Car ' + Path(a.dff).stem)
    folder.mkdir(parents=True)
    dff2mdl.convert(a.dff, template, str(folder / (a.vehicle + '.mdl')), a.txd or None)
    if a.txd:
        txd2xtx.convert(a.txd, str(folder / (a.vehicle + '.xtx')))
    runs = {} if a.no_original else {'original': None}
    runs['converted'] = mods
    shots, logs, alive = {}, {}, {}
    for name, m in runs.items():
        shots[name], logs[name], alive[name] = run(out, name, m, a.save, a.model)
    resolved = '%s.mdl -> GTA3PS2.IMG entry %d' % (a.vehicle, a.model)
    result = {'resolved': resolved in logs['converted'],
              'textures': (a.vehicle + '.xtx -> GTA3PS2.IMG entry') in logs['converted'] if a.txd else None, 'alive': alive,
              'crash': any('[crash]' in t for t in logs.values())}
    if 'original' in shots:
        result['picture'] = mods_check.picture_stats(shots['converted'], shots['original'])
    result['pass'] = (result['resolved'] and result['textures'] is not False and all(alive.values()) and not result['crash'] and
                      ('picture' not in result or result['picture']['diff_from_original'] > 1.0))
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    print(json.dumps(result, indent=1))
    print('PASS' if result['pass'] else 'FAIL', '-', out)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
