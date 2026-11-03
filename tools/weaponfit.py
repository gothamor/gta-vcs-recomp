#!/usr/bin/env python3
"""Where VCS holds each weapon, against VC (FORK.md T11, game-internals.md 9g): VC's original weapon models fitted onto
VCS's, one rigid transform per weapon - the table tools/dff2mdl.py WEAPON_PLACES and src/mods/rwconv.cpp kWeaponPlaces
carry.

    python tools/weaponfit.py --vc "D:/Games/Vice City/models/gta3.img" [--out DIR] [--only beretta,uzi] [--check]

VCS's weapon models are mostly VC's own meshes, each moved in the hand: 2-14 cm along the barrel and turned 2-30
degrees (the melee weapons most), so a VC weapon mod - made to sit where VC's own does - is drawn into Vic's palm, a
chainsaw's handles miss his hands. Each VC weapon (from your VC PC's models/gta3.img + .dir: read only) is converted
onto the VCS model it replaces without any placement (tools/dff2mdl.py), and the rigid transform that makes its
outlines cover VCS's best is searched: the side, top and front silhouettes (2 mm pixels), their mean intersection over
union, a pattern search over three turns (about the model's centre) and three moves. Silhouettes, not nearest points:
where VCS's model is another mesh (VC's M4 onto VCS's longer M16, VC's rocket launcher with its rocket) the grips and
magazines decide where it goes, not the long barrels. Starts: VC's place, the centres aligned, and that turned over
about x or z (VCS's nightstick has its side grip on the other side; one turned over must cover 0.03 better, as a tube
looks the same either way up); each searched at 4 mm, the best again at 2 mm; each degree turned costs 0.0002, so a
round part (a can) does not drift about its axis.
Writes OUT/<weapon>.png (VCS's green, VC's fitted red: side above top) and prints each fit and the table as Python and
C++; --check compares it with the table in dff2mdl.py (1 mm, 0.2 degrees). Needs the disc's weapons extracted (python
tools/img.py extract build/img/GTA3PS2 uzi ...). Outputs (VC's and VCS's files: game data) stay under build/weaponfit.
"""
import argparse
import contextlib
import io
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import mdlinfo  # noqa: E402
import toolchain as tc  # noqa: E402

# VC's weapon -> VCS's model of it (VC's names VCS has under its own: dff2mdl.VC_NAMES)
PAIRS = [("brassknuckle", "brassknuckle"), ("golfclub", "golfclub"), ("nitestick", "nitestick"), ("knifecur", "knifecur"),
         ("bat", "bat"), ("machete", "machete"), ("katana", "katana"), ("chnsaw", "chnsaw"), ("grenade", "grenade"),
         ("teargas", "teargas"), ("molotov", "molotov"), ("colt45", "beretta"), ("python", "python"), ("ruger", "ak47"),
         ("chromegun", "chromegun"), ("shotgspa", "shotgspa"), ("buddyshot", "buddyshot"), ("m4", "m16"),
         ("tec9", "skorpion"), ("uzi", "uzi"), ("ingramsl", "ingramsl"), ("mp5lng", "mplng"), ("sniper", "sniper"),
         ("laser", "laser"), ("rocketla", "rocketla"), ("flame", "flame"), ("m60", "m249"), ("minigun", "minigun"),
         ("minigun2", "minigun2")]
VIEWS = ((0, 2), (0, 1), (1, 2))  # side (x, z), top (x, y), front (y, z)
IDENTITY = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
TURN_COST = 0.0002  # overlap per degree turned in the search: no drift where turning gains nothing
FLIP_COST = 0.03  # a start turned over wins only by this much: a tube's outline is the same either way up


def extract(img, out, names):
    """VC's gta3.img entries by name (its .dir: 32-byte records {offset, size in sectors, name[24]})."""
    d = open(os.path.splitext(img)[0] + ".dir", "rb").read()
    want = {n.lower() for n in names}
    os.makedirs(out, exist_ok=True)
    with open(img, "rb") as f:
        for i in range(0, len(d), 32):
            off, size = struct.unpack_from("<2I", d, i)
            name = d[i + 8:i + 32].split(b"\0")[0].decode("latin1").lower()
            if name in want and not os.path.isfile(os.path.join(out, name)):
                f.seek(off * 2048)
                open(os.path.join(out, name), "wb").write(f.read(size * 2048))


def triangles(path):
    v = mdlinfo.vehicle(mdlinfo.Mdl(open(path, "rb").read()))
    out = []
    for e in v["elements"]:
        if e["geometry"]:
            for mm in e["geometry"]["meshes"]:
                out += [(mm["verts"][a][:3], mm["verts"][b][:3], mm["verts"][c][:3]) for a, b, c in mm["faces"]]
    return out


def apply(r, t, p):
    return tuple(r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2] + t[i] for i in range(3))


def mat3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def euler(ax, ay, az):
    """Rz(az) Ry(ay) Rx(ax), degrees."""
    cx, sx = math.cos(math.radians(ax)), math.sin(math.radians(ax))
    cy, sy = math.cos(math.radians(ay)), math.sin(math.radians(ay))
    cz, sz = math.cos(math.radians(az)), math.sin(math.radians(az))
    rx = [[1.0, 0.0, 0.0], [0.0, cx, -sx], [0.0, sx, cx]]
    ry = [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]]
    rz = [[cz, -sz, 0.0], [sz, cz, 0.0], [0.0, 0.0, 1.0]]
    return mat3(rz, mat3(ry, rx))


def silhouettes(tris, pixel):
    """The model's outline from the side, the top and the front: per view {row: bit mask of the pixels covered} (a
    triangle covers each pixel its strip of the row touches)."""
    out = []
    for a, b in VIEWS:
        rows = {}
        for t in tris:
            pts = [((p[a] + 4.0) / pixel, (p[b] + 4.0) / pixel) for p in t]
            vmin, vmax = min(p[1] for p in pts), max(p[1] for p in pts)
            for j in range(int(vmin), int(vmax) + 1):
                lo = hi = None
                for v in (max(vmin, j), min(vmax, j + 1.0), min(max(j + 0.5, vmin), vmax)):
                    for (u0, v0), (u1, v1) in ((pts[0], pts[1]), (pts[1], pts[2]), (pts[2], pts[0])):
                        if v0 <= v <= v1 or v1 <= v <= v0:
                            u = u0 if v1 == v0 else u0 + (u1 - u0) * (v - v0) / (v1 - v0)
                            lo = u if lo is None or u < lo else lo
                            hi = u if hi is None or u > hi else hi
                if lo is not None:
                    i0, i1 = int(lo), int(hi)
                    rows[j] = rows.get(j, 0) | (((1 << (i1 - i0 + 1)) - 1) << i0)
        out.append(rows)
    return out


def overlap(sa, sb):
    """The views' mean intersection over union."""
    total = 0.0
    for ra, rb in zip(sa, sb):
        inter = union = 0
        for j in set(ra) | set(rb):
            x, y = ra.get(j, 0), rb.get(j, 0)
            inter += bin(x & y).count("1")
            union += bin(x | y).count("1")
        total += inter / max(1, union)
    return total / len(sa)


def moved(tris, r, t):
    return [tuple(apply(r, t, p) for p in tri) for tri in tris]


def search(src, dst_sil, pixel, r0, t0, centre, step, stop):
    """Pattern search from (r0, t0): three turns about the placed model's centre and three moves, each step tried both
    ways, the steps halved when none helps. (r, t, overlap)."""
    c = apply(r0, t0, centre)

    def place(p):
        r = mat3(euler(p[0], p[1], p[2]), r0)
        rc = apply(r, [0.0, 0.0, 0.0], centre)
        return r, [c[i] - rc[i] + p[3 + i] for i in range(3)]

    def score(p):  # each degree turned costs a little: a round part (a can, a bat) does not drift about its axis
        r, t = place(p)
        return overlap(silhouettes(moved(src, r, t), pixel), dst_sil) - TURN_COST * (abs(p[0]) + abs(p[1]) + abs(p[2]))

    p, best = [0.0] * 6, None
    best = score(p)
    steps = [step, step, step, step / 400.0, step / 400.0, step / 400.0]  # 4 degrees ~ 1 cm
    while steps[0] > stop:
        better = False
        for i in range(6):
            for sign in (1.0, -1.0):
                q = p[:]
                q[i] += sign * steps[i]
                s = score(q)
                if s > best + 1e-9:
                    p, best, better = q, s, True
                    break
        if not better:
            steps = [x / 2 for x in steps]
    r, t = place(p)
    return r, t, overlap(silhouettes(moved(src, r, t), pixel), dst_sil)


def fit(src, dst):
    """VC's model (`src`, as converted) onto VCS's (`dst`): (r, t, overlap), q = r p + t."""
    centre = [sum(v[k] for tri in src for v in tri) / (3 * len(src)) for k in range(3)]
    target = [sum(v[k] for tri in dst for v in tri) / (3 * len(dst)) for k in range(3)]
    coarse = silhouettes(dst, 0.004)
    starts = []
    for flip in (IDENTITY, [[1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, -1.0]],  # turned over about x
                 [[-1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 1.0]]):  # ... about z
        for aligned in (False, True):
            if flip is not IDENTITY and not aligned:
                continue
            fc = apply(flip, [0.0, 0.0, 0.0], centre)
            t0 = [target[i] - fc[i] for i in range(3)] if aligned else [0.0, 0.0, 0.0]
            r, t, s = search(src, coarse, 0.004, flip, t0, centre, 8.0, 0.5)
            starts.append((s - (FLIP_COST if flip is not IDENTITY else 0.0), len(starts), r, t))
    starts.sort(key=lambda x: (-x[0], x[1]))
    _, _, r0, t0 = starts[0]
    return search(src, silhouettes(dst, 0.002), 0.002, r0, t0, centre, 1.0, 0.06)


def overlay_png(path, vcs_tris, vc_tris, size=320, scale=300.0):
    """VCS's weapon (green) and VC's fitted onto it (red): the side (x across, z up) above the top (x across, y down),
    the hand's origin a white cross - a look at each fit."""
    import zlib
    w, h = size * 2, size * 2
    img = bytearray(w * h * 3)

    def put(x, y, col):
        if 0 <= x < w and 0 <= y < h:
            i = (y * w + x) * 3
            for k in range(3):
                img[i + k] = max(img[i + k], col[k])

    for view, (ay, sign) in enumerate(((2, -1), (1, 1))):
        ox, oy = int(w * 0.3), view * size + size // 2
        for d in range(-6, 7):
            put(ox + d, oy, (255, 255, 255))
            put(ox, oy + d, (255, 255, 255))
        for tris, col in ((vcs_tris, (0, 210, 0)), (vc_tris, (235, 40, 40))):
            for t in tris[::max(1, len(tris) // 2000)]:
                pts = [(ox + v[0] * scale, oy + sign * v[ay] * scale) for v in t]
                for (x0, y0), (x1, y1) in ((pts[0], pts[1]), (pts[1], pts[2]), (pts[2], pts[0])):
                    n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
                    for i in range(n + 1):
                        put(int(x0 + (x1 - x0) * i / n), int(y0 + (y1 - y0) * i / n), col)
    raw = b"".join(b"\0" + bytes(img[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def placed_overlap(a_tris, b_tris):
    """How well two models' outlines cover each other (0..1, the fit's own measure at 2 mm)."""
    return overlap(silhouettes(a_tris, 0.002), silhouettes(b_tris, 0.002))


def row_vector(r, t):
    """dff2mdl's matrix (RW's row vectors, v * m + p) of q = R p + t, rounded."""
    return (tuple(round(r[j][i], 5) + 0.0 for i in range(3) for j in range(3)), tuple(round(x, 4) + 0.0 for x in t))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vc", required=True, help="VC PC's models/gta3.img (its gta3.dir beside it)")
    ap.add_argument("--out", default=os.path.join(tc.ROOT, "build", "weaponfit"))
    ap.add_argument("--only", default="")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    pairs = [p for p in PAIRS if not a.only or p[1] in a.only.split(",")]
    vc_dir = os.path.join(a.out, "vc")
    extract(a.vc, vc_dir, [n + ext for n, _ in pairs for ext in (".dff", ".txd")])
    table, bad = {}, 0
    for vc, vcs in pairs:
        template = os.path.join(tc.ROOT, "build", "img", "GTA3PS2", vcs + ".mdl")
        dff = os.path.join(vc_dir, vc + ".dff")
        if not os.path.isfile(template) or not os.path.isfile(dff):
            print("%-12s skipped: %s missing" % (vcs, template if not os.path.isfile(template) else dff))
            continue
        out = os.path.join(a.out, vc + ".mdl")
        with contextlib.redirect_stdout(io.StringIO()):
            dff2mdl.convert(dff, template, out, None)  # no weapon name: VC's place as it is
        vc_tris, vcs_tris = triangles(out), triangles(template)
        before = placed_overlap(vc_tris, vcs_tris)
        r, t, after = fit(vc_tris, vcs_tris)
        overlay_png(os.path.join(a.out, vcs + ".png"), vcs_tris, moved(vc_tris, r, t))
        angle = math.degrees(math.acos(max(-1.0, min(1.0, (r[0][0] + r[1][1] + r[2][2] - 1) / 2))))
        table[vcs] = row_vector(r, t)
        note = ""
        if a.check:
            have = dff2mdl.WEAPON_PLACES.get(vcs)
            if not have:
                note, bad = "  NOT IN THE TABLE", bad + 1
            else:
                dt = max(abs(x - y) for x, y in zip(have[1], table[vcs][1]))
                dr = max(abs(x - y) for x, y in zip(have[0], table[vcs][0]))
                if dt > 0.001 or dr > 0.0035:
                    note, bad = "  DIFFERS (%.4f m, %.4f)" % (dt, dr), bad + 1
        print("%-12s <- %-12s overlap %.3f -> %.3f  turned %5.1f  moved (%+.3f %+.3f %+.3f)%s" % (
            vcs, vc, before, after, angle, *t, note), flush=True)
    print("\n# tools/dff2mdl.py")
    for name, (r, p) in table.items():
        print('    "%s": ((%s), (%s)),' % (name, ", ".join(repr(x) for x in r), ", ".join(repr(x) for x in p)))
    print("\n// src/mods/rwconv.cpp")
    for name, (r, p) in table.items():
        print('            {"%s", {{%s}, {%s}}},' % (name, ", ".join(repr(x) for x in r), ", ".join(repr(x) for x in p)))
    if a.check:
        print("PASS" if not bad else "FAIL: %d differ" % bad)
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
