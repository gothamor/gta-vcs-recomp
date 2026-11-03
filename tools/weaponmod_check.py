#!/usr/bin/env python3
"""PC VC weapon mods in the game (FORK.md T11, docs/MODS.md "PC VC weapons"): each weapon in Vic's hand, photographed.

    python tools/weaponmod_check.py [OUT] [--folder DIR] [--types 19,28,25] [--save DIR] [--no-original]

--folder: a PC weapon mod (default modloader/HD Weapons), copied under OUT as one mod: the mod loader converts each
`.dff` named as a VCS weapon (or VC's name for it: colt45 is VCS's beretta) with its `.txd`. Two hidden runs from the
card copy (default the rig's user copy) at gamefiles_check's street, noon: the original weapons, then the mod's. In
each, every weapon type (default: the ones the folder replaces) is given to Vic (the trainer), photographed close up by
the free camera from the front (`<run>_<type>_hand.png`) and his right side (`<run>_<type>_side.png`), and with the
game's camera and HUD (`<run>_<type>_hud.png`).

Pass = each .dff converted and put in its entry, the game alive, the close-up different from the original's (the mod's
model is drawn), the HUD's weapon icon the same as the original's (VCS keeps its icon: the .txd's texture of the
weapon's name is renamed, rwconv.h), and - before the runs, with tools/dff2mdl.py - each converted weapon's outlines
covering VCS's model better with VCS's place in the hand (WEAPON_PLACES) than with VC's (`placed` against `vc_place`:
tools/weaponfit.py's silhouette overlap, 0..1). Results: OUT/result.json. Exit code 0 = pass. Outputs are game pictures: build/.
"""
import argparse
import contextlib
import io
import json
import math
import os
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import gamefiles_check  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402
import weaponfit  # noqa: E402

# VCS's weapon models (game-internals.md 9g): name -> (model, weapon type the trainer gives)
WEAPONS = {"machete": (339, 9), "chnsaw": (341, 11), "teargas": (343, 16), "molotov": (344, 17), "beretta": (346, 19),
           "python": (347, 20), "ak47": (348, 29), "chromegun": (349, 21), "shotgspa": (350, 22), "buddyshot": (351, 23),
           "m16": (352, 28), "skorpion": (353, 24), "uzi": (354, 25), "ingramsl": (355, 26), "mplng": (356, 27),
           "sniper": (357, 30), "laser": (358, 31), "rocketla": (359, 32), "flame": (360, 33), "m249": (361, 34),
           "minigun": (362, 35)}
# the HUD's weapon icon, top right in the 640 x 448 picture (the game's own size: screenshots are scaled to it below)
ICON = (0.80, 0.02, 0.99, 0.20)


def crop_diff(a, b, box):
    """Mean absolute difference (0..255) of two screenshots inside `box` (fractions of the picture)."""
    wa, ha, ra = tc.read_png(a)
    wb, hb, rb = tc.read_png(b)
    if (wa, ha) != (wb, hb):
        return 255.0
    x0, y0, x1, y1 = int(box[0] * wa), int(box[1] * ha), int(box[2] * wa), int(box[3] * ha)
    total = n = 0
    for y in range(y0, y1, 2):
        la, lb = ra[y], rb[y]
        for x in range(x0 * 3, x1 * 3, 6):
            total += abs(la[x] - lb[x]) + abs(la[x + 1] - lb[x + 1]) + abs(la[x + 2] - lb[x + 2])
            n += 3
    return total / max(1, n)


def changed_pixels(a, b):
    """Pixels of two screenshots that differ clearly (RGB distance over 60): where the weapon in his hand changed."""
    wa, ha, ra = tc.read_png(a)
    wb, hb, rb = tc.read_png(b)
    if (wa, ha) != (wb, hb):
        return wa * ha
    n = 0
    for y in range(ha):
        la, lb = ra[y], rb[y]
        for x in range(0, wa * 3, 3):
            if abs(la[x] - lb[x]) + abs(la[x + 1] - lb[x + 1]) + abs(la[x + 2] - lb[x + 2]) > 60:
                n += 1
    return n


def run(out, name, mods, save, types):
    """One hidden run: each weapon type given to Vic, photographed close (free camera) and with the HUD."""
    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)} if mods else {}
    g = rig.Game.start(save=save, name='weaponmod_' + name, seconds=150 + 20 * len(types), env=env)
    x, y, heading = gamefiles_check.STREET
    shots = {}
    try:
        g.wait_ingame(timeout=180)
        g.set_time(12)
        g.weather(0)
        for _ in range(6):  # the game puts the player back at the save's place in its first seconds (carmod_check)
            g.teleport(x, y, -100.0, heading=heading)
            there = []
            for _ in range(2):
                time.sleep(3)
                _, (px, py, _pz), _ = g.player()
                there.append(abs(px - x) + abs(py - y) < 10.0)
            if all(there):
                break
        for t in types:
            g.cmd('trainer weapon %d' % t)
            time.sleep(3)
            ped, (px, py, pz), _ = g.player()
            fx, fy = g.peekf(ped + 0x10, 2)  # his forward vector
            n = math.hypot(fx, fy) or 1.0
            fx, fy = fx / n, fy / n
            rx, ry = fy, -fx  # his right
            # in front of him and to his right, at his waist: the weapon in his right hand
            cx, cy, cz = px + fx * 1.4 + rx * 0.9, py + fy * 1.4 + ry * 0.9, pz + 0.2
            tx, ty, tz = px + rx * 0.3, py + ry * 0.3, pz
            yaw = math.degrees(math.atan2(ty - cy, tx - cx))
            pitch = math.degrees(math.atan2(tz - cz, math.hypot(tx - cx, ty - cy)))
            g.cmd('campose %.3f %.3f %.3f %.2f %.2f' % (cx, cy, cz, yaw, pitch))
            time.sleep(1.5)
            hand = out / ('%s_%d_hand.png' % (name, t))
            g.cmd('screenshot ' + str(hand))
            g.cmd('campose off')
            time.sleep(1.5)
            hud = out / ('%s_%d_hud.png' % (name, t))
            g.cmd('screenshot ' + str(hud))
            # from his right side, at his hand
            ped, (px, py, pz), _ = g.player()
            fx, fy = g.peekf(ped + 0x10, 2)
            n = math.hypot(fx, fy) or 1.0
            fx, fy = fx / n, fy / n
            rx, ry = fy, -fx
            cx, cy, cz = px + rx * 1.3 + fx * 0.2, py + ry * 1.3 + fy * 0.2, pz + 0.1
            tx, ty, tz = px + rx * 0.3, py + ry * 0.3, pz - 0.1
            yaw = math.degrees(math.atan2(ty - cy, tx - cx))
            pitch = math.degrees(math.atan2(tz - cz, math.hypot(tx - cx, ty - cy)))
            g.cmd('campose %.3f %.3f %.3f %.2f %.2f' % (cx, cy, cz, yaw, pitch))
            time.sleep(1.0)
            side = out / ('%s_%d_side.png' % (name, t))
            g.cmd('screenshot ' + str(side))
            g.cmd('campose off')
            time.sleep(1.0)
            shots[t] = (hand, hud, side)
        alive = g.alive()
    finally:
        g.quit()
    return shots, Path(g.log_path).read_text(encoding='utf-8', errors='replace'), alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--folder', default=os.path.join(tc.ROOT, 'modloader', 'HD Weapons'))
    ap.add_argument('--types', default='', help='weapon types (comma separated); default: the folder\'s weapons')
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    ap.add_argument('--no-original', action='store_true', help='skip the run without the mod')
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'weaponmod_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    mods = out / 'modloader'
    if mods.is_dir():
        shutil.rmtree(mods)
    shutil.copytree(a.folder, mods / ('Test ' + Path(a.folder).name))  # its own name: the player's F2 switch stays theirs
    replaced = {}
    for f in sorted(os.listdir(a.folder)):
        stem, ext = os.path.splitext(f)
        name = dff2mdl.VC_NAMES.get(stem.lower(), stem.lower())
        if ext.lower() == '.dff' and name in WEAPONS:
            replaced[name] = WEAPONS[name][1]
    types = [int(t) for t in a.types.split(',') if t] or sorted(replaced.values())
    # where each converted weapon sits against VCS's model: with VC's place, and with VCS's (the mod loader's)
    placed = {}
    for f in sorted(os.listdir(a.folder)):
        stem, ext = os.path.splitext(f)
        name = dff2mdl.VC_NAMES.get(stem.lower(), stem.lower())
        template = os.path.join(tc.ROOT, 'build', 'img', 'GTA3PS2', name + '.mdl')
        if ext.lower() != '.dff' or name not in replaced or name not in dff2mdl.WEAPON_PLACES or not os.path.isfile(template):
            continue
        txd = os.path.join(a.folder, stem + '.txd')
        txd = txd if os.path.isfile(txd) else None
        dist = []
        for weapon in (None, name):
            path = str(out / ('%s_%s.mdl' % (name, 'placed' if weapon else 'vc')))
            with contextlib.redirect_stdout(io.StringIO()):
                dff2mdl.convert(os.path.join(a.folder, f), template, path, txd, weapon)
            dist.append(weaponfit.placed_overlap(weaponfit.triangles(path), weaponfit.triangles(template)))
        placed[name] = {'vc_place': round(dist[0], 3), 'placed': round(dist[1], 3)}
        print('%-10s outlines over VCS\'s model: %.3f with VC\'s place, %.3f with VCS\'s' % (name, dist[0], dist[1]), flush=True)
    runs = {} if a.no_original else {'original': None}
    runs['mod'] = mods
    shots, logs, alive = {}, {}, {}
    for name, m in runs.items():
        shots[name], logs[name], alive[name] = run(out, name, m, a.save, types)
    log = logs['mod']
    result = {'alive': alive, 'crash': any('[crash]' in t for t in logs.values()),
              'converted': re.findall(r'\[mods\] .*\\(\w+)\.(?:dff|txd) converted in ', log),
              'failed': re.findall(r'\[mods\] .* FAILED.*', log)[:10],
              'entries': len(re.findall(r'-> GTA3PS2\.IMG entry', log)), 'weapons': {}}
    ok = all(alive.values()) and not result['crash'] and not result['failed']
    for name, t in sorted(replaced.items(), key=lambda kv: kv[1]):
        if t not in types:
            continue
        w = {'type': t, 'model': WEAPONS[name][0]}
        if name in placed:
            w.update(placed[name])
            w['placed_ok'] = placed[name]['placed'] > placed[name]['vc_place']
            ok &= w['placed_ok']
        if 'original' in shots:
            w['hand_pixels'] = changed_pixels(shots['mod'][t][0], shots['original'][t][0])
            w['icon_diff'] = round(crop_diff(shots['mod'][t][1], shots['original'][t][1], ICON), 2)
            # the model drawn: the close-up changes around his hand (a grenade: ~1600 pixels); the icon VCS's: its
            # corner the same (clock digits aside)
            w['pass'] = w['hand_pixels'] > 500 and w['icon_diff'] < 3.0
            ok &= w['pass']
        result['weapons'][name] = w
    result['pass'] = ok
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    for name, w in result['weapons'].items():
        print('%-10s type %2d  close-up pixels changed %6s  icon diff %6s  outlines %s -> %s  %s' % (
            name, w['type'], w.get('hand_pixels', '-'), w.get('icon_diff', '-'), w.get('vc_place', '-'),
            w.get('placed', '-'), 'ok' if w.get('pass', True) and w.get('placed_ok', True) else 'FAIL'))
    print('converted: %s' % (', '.join(sorted(set(result['converted']))) or 'none'))
    if result['failed']:
        print('failed:', result['failed'])
    print('PASS' if ok else 'FAIL', '-', out)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
