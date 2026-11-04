#!/usr/bin/env python3
"""PC VC car (.dff) -> VCS vehicle model (`ldm`), FORK.md T9; docs/MODS.md "PC VC cars in VCS".

    python tools/dff2mdl.py convert CAR.dff TEMPLATE.mdl OUT.mdl [--txd CAR.txd]
                                                     the car's parts onto a VCS vehicle's model (the template, from
                                                     tools/img.py extract; OUT replaces it: `admiral.mdl` in a mod).
                                                     With the .txd, materials naming a texture it lacks map to VCS's
                                                     shared ones or go; its textures: tools/txd2xtx.py
    python tools/dff2mdl.py --self-test [--img DIR]   every PS2 mesh of the extracted vehicles (tools/img.py extract)
                                                     decoded to its raw vertex values and encoded again: the bytes
                                                     must come out identical to the game's

A mesh's DMA data (docs/game-internals.md 9d, tools/mdlinfo.py): a DMA tag qword (0x60000000 | qwords - 1), then
batches of at most 70 vertices - the next batch repeats the last two (a strip) - each {UNPACK V4-32 x1 at 0x8000: 0, 0,
n, n | 0x8000 (the program's header); STMASK 0x40404040, STROW 0, UNPACK V3-16 positions at 1; STMASK 0x50505050,
STROW (0, 0, 0xFF, 0), UNPACK V2-8 unsigned UVs at 0x8D; UNPACK V3-8 normals at 0x47; MSCAL 0x06} padded with zeros to
a qword.
"""
import heapq
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdlinfo  # noqa: E402

BATCH = 70


def decode_raw(d, at):
    """A mesh's vertices as stored: (x, y, z) int16, (u, v) uint8, (nx, ny, nz) int8 or None; the strip in order."""
    qwords = (struct.unpack_from("<I", d, at)[0] & 0xFFFF) + 1
    end, w = at + qwords * 16, at + 16
    out, first = [], True
    while w < end and struct.unpack_from("<I", d, w)[0] == 0x6C018000:
        n = struct.unpack_from("<I", d, w + 16)[0] & 0x7FFF
        w += 20
        arrays = {}
        while True:
            code = struct.unpack_from("<I", d, w)[0]
            cmd = code >> 24
            if cmd == 0x14:
                w += 4
                while w < end and struct.unpack_from("<I", d, w)[0] == 0:
                    w += 4
                break
            if cmd == 0x20:
                w += 8
            elif cmd == 0x30:
                w += 20
            else:
                size = {0x9: 6, 0x6: 2, 0xA: 3}[cmd & 0xF]
                arrays[cmd & 0xF] = w + 4
                w += 4 + (((code >> 16) & 0xFF) * size + 3) // 4 * 4
        for i in range(0 if first else 2, n):
            p = struct.unpack_from("<3h", d, arrays[0x9] + i * 6)
            t = struct.unpack_from("<2B", d, arrays[0x6] + i * 2)
            nrm = struct.unpack_from("<3b", d, arrays[0xA] + i * 3) if 0xA in arrays else None
            out.append((p, t, nrm))
        first = False
    return out, end - at


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def encode(verts):
    """The DMA data for a strip of raw vertices (the inverse of decode_raw)."""
    body = b""
    start = 0
    while True:
        batch = verts[start:start + BATCH]
        n = len(batch)
        body += struct.pack("<5I", 0x6C018000, 0, 0, n, n | 0x8000)
        body += struct.pack("<2I", 0x20000000, 0x40404040) + struct.pack("<5I", 0x30000000, 0, 0, 0, 0)
        body += struct.pack("<I", 0x79008001 | n << 16) + pad4(b"".join(struct.pack("<3h", *v[0]) for v in batch))
        body += struct.pack("<2I", 0x20000000, 0x50505050) + struct.pack("<5I", 0x30000000, 0, 0, 0xFF, 0)
        body += struct.pack("<I", 0x7600C08D | n << 16) + pad4(b"".join(struct.pack("<2B", *v[1]) for v in batch))
        if batch[0][2] is not None:
            body += struct.pack("<I", 0x6A008047 | n << 16) + pad4(b"".join(struct.pack("<3b", *v[2]) for v in batch))
        body += struct.pack("<I", 0x14000006)
        body += b"\0" * (-(16 + len(body)) % 16)
        if start + BATCH >= len(verts):
            break
        start += BATCH - 2
    return struct.pack("<4I", 0x60000000 | (len(body) // 16), 0, 0, 0) + body


def self_test(folder):
    meshes = same = 0
    bad = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".mdl"):
            continue
        m = mdlinfo.Mdl(open(os.path.join(folder, name), "rb").read())
        group = m.ptr(0x20)
        for e in m.elements(group):
            g = m.ptr(e + 0x14)
            if not g:
                continue
            h = g + 32
            n = struct.unpack_from("<I", m.d, h + 16)[0] >> 20
            base = h + 0x40 + n * 0x30
            for i in range(n):
                at = base + struct.unpack_from("<I", m.d, h + 0x40 + i * 0x30 + 28)[0]
                verts, length = decode_raw(m.d, at)
                meshes += 1
                if encode(verts) == m.d[at:at + length]:
                    same += 1
                elif len(bad) < 5:
                    bad.append("%s mesh at 0x%x" % (name, at))
    print("dff2mdl self-test: %d / %d meshes encoded byte for byte%s" % (same, meshes, ("; differ: " + ", ".join(bad)) if bad else ""))
    return same == meshes and meshes > 0


# ---- the PC .dff -------------------------------------------------------------------------------------------------
def finite(values):
    """NaN / infinity (some exporters write them) read as 0."""
    return tuple(v if math.isfinite(v) else 0.0 for v in values)


def read_dff(data):
    """Frames {name, parent, rot (9), pos (3)}, geometries {verts, normals, uvs, tris (a, b, c, material), materials
    {colour, texture}}, atomics {frame, geometry} of a RenderWare 3 clump (RW binary stream; tools/rwinfo.py)."""
    import rwinfo as rw
    top = next(rw.chunks(data, 0, len(data)))
    out = {"frames": [], "geometries": [], "atomics": []}
    for kind, a, e, _ in rw.chunks(data, top[1], top[2]):
        if kind == rw.FRAMELIST:
            s = rw.child(data, a, e, rw.STRUCT)
            n = struct.unpack_from("<I", data, s[0])[0]
            for i in range(n):
                f = struct.unpack_from("<12fi", data, s[0] + 4 + i * 56)
                out["frames"].append({"rot": finite(f[0:9]), "pos": finite(f[9:12]), "parent": f[12], "name": ""})
            i = 0
            for k, xa, xe, _ in rw.chunks(data, s[1], e):
                if k == rw.EXTENSION and i < n:
                    for pk, pa, pe, _ in rw.chunks(data, xa, xe):
                        if pk == rw.FRAME_NAME:
                            out["frames"][i]["name"] = data[pa:pe].decode("latin1")
                    i += 1
        elif kind == rw.GEOMLIST:
            for k, ga, ge, libid in rw.chunks(data, a, e):
                if k != rw.GEOMETRY:
                    continue
                s = rw.child(data, ga, ge, rw.STRUCT)
                at = s[0]
                flags, ntris, nverts, nmorph = struct.unpack_from("<4I", data, at)
                at += 16
                if rw.version(libid) < 0x34000:
                    at += 12
                if flags & 0x08:  # prelit colours
                    at += nverts * 4
                nsets = (flags >> 16) & 0xFF or (2 if flags & 0x80 else 1 if flags & 0x04 else 0)
                uvs = [finite(struct.unpack_from("<2f", data, at + i * 8)) for i in range(nverts)] if nsets else [(0.0, 0.0)] * nverts
                at += nsets * nverts * 8
                tris = []
                for i in range(ntris):
                    w0, w1 = struct.unpack_from("<2I", data, at + i * 8)
                    tris.append((w0 >> 16, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF))  # v0, v1, v2, material
                at += ntris * 8 + 16  # + the morph target's bounding sphere
                has_verts, has_normals = struct.unpack_from("<2I", data, at)
                at += 8
                verts = [finite(struct.unpack_from("<3f", data, at + i * 12)) for i in range(nverts)]
                at += nverts * 12
                normals = [finite(struct.unpack_from("<3f", data, at + i * 12)) for i in range(nverts)] if has_normals else None
                mats, own = [], []
                ml = rw.child(data, ga, ge, rw.MATLIST)
                for mk, ma, me, _ in rw.chunks(data, ml[0], ml[1]) if ml else ():
                    if mk == rw.MATERIAL:
                        rgba = struct.unpack_from("<4B", data, rw.child(data, ma, me, rw.STRUCT)[0] + 4)
                        tex = rw.child(data, ma, me, rw.TEXTURE)
                        own.append({"colour": rgba, "texture": rw.string(data, *tex) if tex else ""})
                if ml:  # the list's struct: {count, index per entry}: -1 a material of its own (in order), else a copy
                    ls = rw.child(data, ml[0], ml[1], rw.STRUCT)
                    count = struct.unpack_from("<I", data, ls[0])[0]
                    k = 0
                    for ref in struct.unpack_from("<%di" % count, data, ls[0] + 4):
                        if ref < 0:
                            mats.append(own[k] if k < len(own) else {"colour": (255, 255, 255, 255), "texture": ""})
                            k += 1
                        else:
                            mats.append(mats[ref])
                # The triangles' material numbers are often all 0 (exporters leave them); the bin mesh (0x50E, what RW
                # draws) splits by material: {flags (1: strips), meshes, indices} then {count, material, indices}.
                ext = rw.child(data, ga, ge, rw.EXTENSION)
                bm = rw.child(data, ext[0], ext[1], 0x50E) if ext else None
                if bm:
                    bflags, nmesh = struct.unpack_from("<2I", data, bm[0])
                    p, from_mesh = bm[0] + 12, []
                    for _ in range(nmesh):
                        cnt, mat = struct.unpack_from("<2I", data, p)
                        idx = struct.unpack_from("<%dI" % cnt, data, p + 8)
                        p += 8 + 4 * cnt
                        if bflags & 1:
                            for j in range(cnt - 2):
                                a_, b_, c_ = idx[j:j + 3]
                                if a_ != b_ and b_ != c_ and a_ != c_:
                                    from_mesh.append((a_, b_, c_, mat) if j % 2 == 0 else (b_, a_, c_, mat))
                        else:
                            from_mesh += [(idx[j], idx[j + 1], idx[j + 2], mat) for j in range(0, cnt - 2, 3)]
                    if from_mesh:
                        tris = from_mesh
                out["geometries"].append({"verts": verts, "normals": normals, "uvs": uvs, "tris": tris, "materials": mats})
        elif kind == rw.ATOMIC:
            s = rw.child(data, a, e, rw.STRUCT)
            frame, geometry = struct.unpack_from("<2I", data, s[0])
            out["atomics"].append({"frame": frame, "geometry": geometry})
    return out


def apply(rot, pos, v):
    """A frame's local transform (RW: right, up, at as rows) on a point."""
    return (v[0] * rot[0] + v[1] * rot[3] + v[2] * rot[6] + pos[0],
            v[0] * rot[1] + v[1] * rot[4] + v[2] * rot[7] + pos[1],
            v[0] * rot[2] + v[1] * rot[5] + v[2] * rot[8] + pos[2])


IDENTITY = ((1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


def mat_mul(a, b):
    """a then b (RW's row vectors: v * a, then * b) - each sum left to right, as src/mods/rwconv.cpp does it."""
    ra, pa = a
    rb, pb = b
    r = tuple(ra[3 * i] * rb[j] + ra[3 * i + 1] * rb[3 + j] + ra[3 * i + 2] * rb[6 + j] for i in range(3) for j in range(3))
    p = tuple(pa[0] * rb[j] + pa[1] * rb[3 + j] + pa[2] * rb[6 + j] + pb[j] for j in range(3))
    return (r, p)


def mat_inv(m):
    """The inverse (the 3x3 part by cofactors); a singular matrix gives the identity."""
    (a, b, c, d, e, f, g, h, i), p = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        return IDENTITY
    r = tuple(x / det for x in (e * i - f * h, c * h - b * i, b * f - c * e,
                                 f * g - d * i, a * i - c * g, c * d - a * f,
                                 d * h - e * g, b * g - a * h, a * e - b * d))
    q = tuple(-(p[0] * r[j] + p[1] * r[3 + j] + p[2] * r[6 + j]) for j in range(3))
    return (r, q)


def mat_apply(m, v, translate=True):
    r, p = m
    return tuple(v[0] * r[j] + v[1] * r[3 + j] + v[2] * r[6 + j] + (p[j] if translate else 0.0) for j in range(3))


# Where VCS holds each weapon against VC (tools/weaponfit.py, game-internals.md 9g): VCS's weapon models are mostly VC's,
# each moved ~5-8 cm along the barrel and turned 2-30 degrees in the hand (VCS's animations hold them there). A VC weapon
# mod sits where VC's own does: its parts go through the weapon's matrix (RW's row vectors) to sit where VCS's does.
WEAPON_PLACES = {
    "brassknuckle": ((0.9813, 0.17595, -0.07807, -0.17236, 0.98373, 0.0506, 0.0857, -0.03619, 0.99566), (0.044, -0.0101, 0.0293)),
    "golfclub": ((0.93319, -0.14079, -0.33066, 0.10191, 0.98597, -0.13219, 0.34463, 0.08966, 0.93445), (0.0623, 0.0027, 0.0958)),
    "nitestick": ((-0.85385, -0.51806, 0.05049, 0.51697, -0.85534, -0.0337, 0.06064, -0.00267, 0.99816), (0.179, 0.1029, 0.1433)),
    "knifecur": ((-0.95642, -0.05523, 0.28671, 0.06682, -0.99729, 0.03079, 0.28424, 0.0486, 0.95752), (0.209, 0.072, 0.0123)),
    "bat": ((0.94768, -0.04479, -0.31607, -0.0009, 0.98973, -0.14296, 0.31923, 0.13577, 0.9379), (0.0726, -0.0044, 0.1301)),
    "machete": ((0.93837, 0.05883, -0.34059, -0.0713, 0.99716, -0.0242, 0.3382, 0.047, 0.9399), (0.0559, 0.0045, 0.0482)),
    "katana": ((0.9437, -0.10628, -0.31325, 0.06772, 0.98899, -0.13155, 0.32378, 0.10293, 0.94052), (0.0603, 0.0103, 0.1143)),
    "chnsaw": ((0.9426, 0.04396, -0.331, -0.052, 0.99853, -0.01548, 0.32984, 0.03181, 0.9435), (0.0798, -0.0177, 0.1003)),
    "grenade": ((0.99613, 0.04887, 0.07307, -0.05643, 0.99285, 0.10516, -0.06741, -0.10888, 0.99177), (0.0735, -0.0053, 0.036)),
    "teargas": ((0.8495, 0.47256, -0.23461, -0.46982, 0.87989, 0.07114, 0.24005, 0.04979, 0.96948), (0.0884, -0.0268, 0.0339)),
    "molotov": ((0.9901, -0.05241, -0.13024, 0.03373, 0.98934, -0.14169, 0.13628, 0.1359, 0.9813), (0.074, 0.002, 0.1622)),
    "beretta": ((0.99806, -0.03646, 0.05054, 0.03706, 0.99925, -0.01098, -0.0501, 0.01283, 0.99866), (0.0659, 0.0022, 0.0121)),
    "python": ((0.99123, 0.09083, 0.09594, -0.09474, 0.99481, 0.03702, -0.09208, -0.04578, 0.9947), (0.0523, -0.0084, 0.0107)),
    "ak47": ((0.99194, -0.01218, 0.12616, 0.00537, 0.99852, 0.05417, -0.12663, -0.05306, 0.99053), (0.1082, -0.0045, 0.0099)),
    "chromegun": ((0.99905, 0.0, 0.04362, -0.00076, 0.99985, 0.01744, -0.04361, -0.01745, 0.9989), (0.0695, -0.0106, -0.004)),
    "shotgspa": ((0.99863, -0.00223, 0.0523, -0.00348, 0.99407, 0.10868, -0.05224, -0.10871, 0.9927), (0.0571, -0.009, -0.0044)),
    "buddyshot": ((0.99976, 0.00109, -0.02181, 0.00043, 0.99757, 0.06974, 0.02184, -0.06973, 0.99733), (0.0942, -0.004, 0.0213)),
    "m16": ((0.98729, -0.03168, 0.15572, 0.02014, 0.99697, 0.07519, -0.15763, -0.0711, 0.98493), (0.0565, 0.0167, -0.0116)),
    "skorpion": ((0.99528, 0.06955, 0.06769, -0.06859, 0.99751, -0.0163, -0.06866, 0.01158, 0.99757), (0.0595, -0.0165, 0.0465)),
    "uzi": ((0.98588, 0.13665, 0.09683, -0.13288, 0.99014, -0.0444, -0.10195, 0.03091, 0.99431), (0.0565, 0.0002, 0.0203)),
    "ingramsl": ((0.98463, 0.0999, 0.14327, -0.09211, 0.99394, -0.05998, -0.1484, 0.04586, 0.98786), (0.0406, -0.0227, 0.0139)),
    "mplng": ((0.98335, 0.17483, 0.04963, -0.17475, 0.98459, -0.00613, -0.04994, -0.00265, 0.99875), (0.0551, -0.0073, -0.004)),
    "sniper": ((0.99423, -0.01176, 0.10664, 0.01533, 0.99935, -0.03272, -0.10619, 0.03417, 0.99376), (0.1348, -0.0136, 0.0091)),
    "laser": ((0.97627, 0.00777, 0.21643, -0.00612, 0.99995, -0.00831, -0.21648, 0.00679, 0.97626), (0.1027, 0.0066, -0.0451)),
    "rocketla": ((0.98851, 0.04751, 0.14349, -0.02756, 0.99005, -0.13796, -0.14861, 0.13242, 0.97999), (0.0355, -0.0303, -0.0046)),
    "flame": ((0.93937, 0.07882, -0.33373, -0.12057, 0.987, -0.10625, 0.32102, 0.14005, 0.93666), (0.0616, -0.0239, 0.0928)),
    "m249": ((0.99726, -0.05204, 0.05249, 0.04857, 0.99668, 0.06531, -0.05571, -0.06258, 0.99648), (0.0376, 0.0159, -0.0141)),
    "minigun": ((0.85471, 0.09521, -0.51031, -0.13979, 0.98894, -0.04962, 0.49994, 0.11374, 0.85856), (0.0355, -0.0364, 0.1903)),
    "minigun2": ((0.99986, 0.00471, 0.01636, -0.01302, 0.8302, 0.55731, -0.01096, -0.55744, 0.83015), (-0.0698, 0.0021, 0.0011)),
}

# VCS helicopters' still rotors -> VC's rotors. The moving ones stay VCS's: a blur disc (texture `rotor`) the game fades
# in with the rotor's speed - VC has none (its blades simply turn); the .xtx keeps the textures it needs (txd2xtx --keep).
ALIASES = {"static_rotor": "toprotor", "static_rotor2": "rearrotor"}
KEEP = ("moving_rotor", "moving_rotor2")


def uv_tiles(tris, uvs):
    """A material's triangles as meshes: one when its UVs span at most two repeats (8-bit UVs over 0 .. 2 keep the
    game's own precision), else one per triangle's repeat (its lowest whole u, v), each shifted back by it."""
    us = [uvs[i][0] for t in tris for i in t]
    vs = [uvs[i][1] for t in tris for i in t]
    if not tris or (max(us) - math.floor(min(us)) <= 2.0 and max(vs) - math.floor(min(vs)) <= 2.0):
        return [tris]
    tiles = {}
    for t in tris:
        tile = (math.floor(min(uvs[i][0] for i in t)), math.floor(min(uvs[i][1] for i in t)))
        tiles.setdefault(tile, []).append(t)
    return list(tiles.values())


def naive_sum(values):
    """Left to right in doubles: the same bits on every Python (3.12's sum() compensates) and in src/mods/rwconv.cpp."""
    total = 0.0
    for v in values:
        total += v
    return total


def strip_of(tris):
    """Triangles -> one strip (PS2 GTA draws without back-face culling: a strip's winding does not matter). Strips are
    started at the triangle with the fewest free neighbours, grown along shared edges to the neighbour with the fewest
    free neighbours, both ways, and joined by repeating a vertex at each end. Degenerate input triangles are dropped.
    src/mods/rwconv.cpp's stripOf does the same, step for step."""
    tris = [t for t in tris if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]]
    count = len(tris)

    def edge(x, y):
        return (x, y) if x < y else (y, x)

    by_edge = {}
    for t, (a, b, c) in enumerate(tris):
        for e in (edge(a, b), edge(b, c), edge(c, a)):
            by_edge.setdefault(e, []).append(t)
    neighbours = []
    for t, (a, b, c) in enumerate(tris):
        n = []
        for e in (edge(a, b), edge(b, c), edge(c, a)):
            for u in by_edge[e]:
                if u != t and u not in n:
                    n.append(u)
        neighbours.append(n)
    used = [False] * count
    free = [len(n) for n in neighbours]
    heap = [(free[t], t) for t in range(count)]
    heapq.heapify(heap)

    def grow(s, taken):
        for _ in range(2):  # forward, then from the other end
            while True:
                best = None
                for u in by_edge.get(edge(s[-2], s[-1]), ()):
                    if not used[u] and u not in taken and (best is None or (free[u], u) < (free[best], best)):
                        best = u
                if best is None:
                    break
                taken.add(best)
                s.append(next(v for v in tris[best] if v != s[-2] and v != s[-1]))
            s.reverse()
        return s

    out = []
    while heap:
        f, t = heapq.heappop(heap)
        if used[t] or f != free[t]:
            continue  # taken, or an older entry for it
        a, b, c = tris[t]
        best = None
        for start in ((a, b, c), (b, c, a), (c, a, b)):
            taken = {t}
            s = grow(list(start), taken)
            if best is None or len(taken) > len(best[1]):
                best = (s, taken)
        s, taken = best
        for k in sorted(taken):
            used[k] = True
        for k in sorted(taken):
            for u in neighbours[k]:
                if not used[u]:
                    free[u] -= 1
                    heapq.heappush(heap, (free[u], u))
        if out:
            out += [out[-1], s[0]]
        out += s
    return out


# ---- the VCS model (template) ------------------------------------------------------------------------------------
PRIMARY, SECONDARY = (0x3C, 0xFF, 0x00), (0xFF, 0x00, 0xAF)  # VC's paint colours in a .dff
# The vehicle's paint lists (the data at 0x20: {element group, extras count, extras, paint}): materials the game colours
# with the car's first colour at 0x2C (30, null-ended when fewer), its second at 0xA4 (25).
PAINT_LISTS = ((0x2C, 30), (0xA4, 25))
# VC car names VCS shortened (its names are 8 letters at most): a VC mod's `police.dff` is VCS's `policem`
VC_NAMES = {"boxville": "boxvil", "enforcer": "enforce", "fbiranch": "fbiran", "police": "policem", "sabretur": "sabretb",
            "washing": "washin",
            # and VC's weapons VCS has under its own names
            "colt45": "beretta", "m4": "m16", "tec9": "skorpion", "mp5lng": "mplng", "m60": "m249", "ruger": "ak47",
            # ... and SA's
            "micro_uzi": "uzi", "sawnoff": "buddyshot"}
# VC's shared vehicle textures (generic.txd) -> VCS's own (their layouts differ: the closest, not the same picture)
VC_GENERIC = {"vehiclegeneric256": "xv_vehiclegeneric_sl", "vehiclescratch64": "xv_scratch_sl",
              "vehiclegrunge256": "xv_vehiclegrunge_sl", "vehiclesteering": "xv_vehiclesteering_sl"}


def build_geometry(m, template_geom, verts, normals, uvs, groups, material_ptrs):
    """A geometry block for `m` (an Mdl being extended), appended at its end later: {RslGeometry, PS2 header, meshes,
    DMA data, material pointer list}; returns (bytes, relocation offsets inside it)."""
    xs = [v[0] for v in verts] or [0.0]
    ys = [v[1] for v in verts] or [0.0]
    zs = [v[2] for v in verts] or [0.0]
    centre = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2)
    scale = tuple(max(1e-3, (max(c) - min(c)) / 2 * 1.001) for c in (xs, ys, zs))
    radius = max(((v[0] - centre[0]) ** 2 + (v[1] - centre[1]) ** 2 + (v[2] - centre[2]) ** 2) ** 0.5 for v in verts) if verts else 0.0

    def q16(x):
        return max(-32767, min(32767, int(round(x * 32767.5))))

    meshes, dma, total, boxes = [], b"", 0, []
    for material, strip in groups:
        sv = [verts[i] for i in strip]
        # UVs are unsigned in the PS2 format (0 .. 2 * the mesh's scale): the mesh shifted by whole repeats of its
        # texture so that none is negative
        su = [uvs[i] for i in strip]
        off = (math.floor(min(u[0] for u in su)), math.floor(min(u[1] for u in su)))
        su = [(u[0] - off[0], u[1] - off[1]) for u in su]
        uv_scale = (max(1.0, max(u[0] for u in su) / 2), max(1.0, max(u[1] for u in su) / 2))
        raw = []
        for i, u in zip(strip, su):
            v = verts[i]
            n = normals[i] if normals else (0.0, 0.0, 1.0)
            raw.append(((q16((v[0] - centre[0]) / scale[0]), q16((v[1] - centre[1]) / scale[1]), q16((v[2] - centre[2]) / scale[2])),
                        (max(0, min(255, int(round(u[0] / uv_scale[0] * 127.5)))), max(0, min(255, int(round(u[1] / uv_scale[1] * 127.5))))),
                        tuple(max(-127, min(127, int(round(c * 127)))) for c in n)))
        packet = encode(raw)
        # the mesh's box in the geometry's int16 space, two units out (as the game's own: game-internals.md 9d)
        box = tuple(max(-32768, min(r[0][k] for r in raw) - 2) for k in range(3)) + \
            tuple(min(32767, max(r[0][k] for r in raw) + 2) for k in range(3))
        boxes.append(box)
        mc = tuple(naive_sum(v[k] for v in sv) / len(sv) for k in range(3))
        mr = max(((v[0] - mc[0]) ** 2 + (v[1] - mc[1]) ** 2 + (v[2] - mc[2]) ** 2) ** 0.5 for v in sv)
        meshes.append(struct.pack("<4f2fiIHh6h", *mc, mr, *uv_scale, 0, len(dma), len(strip) - 2, material, *box))
        dma += packet
        total += len(strip)
    n = len(meshes)
    gbox = tuple(min(b[k] for b in boxes) for k in range(3)) + tuple(max(b[3 + k] for b in boxes) for k in range(3))
    header = struct.pack("<4fIiHH6h3f3f", *centre, radius, (n << 20) | (len(dma) & 0xFFFFF), 7, total & 0xFFFF, 0x40 + n * 0x30,
                         *gbox, *scale, *centre)
    body = header + b"".join(meshes) + dma
    body += b"\0" * (-len(body) % 4)
    list_at = 32 + len(body)
    geom = bytearray(m.d[template_geom:template_geom + 32])
    struct.pack_into("<3I", geom, 0x0C, list_at, len(material_ptrs), len(material_ptrs))  # filled with absolute offsets later
    block = bytes(geom) + body + struct.pack("<%dI" % len(material_ptrs), *material_ptrs)
    block += b"\0" * (-len(block) % 16)
    return block, [0x0C] + [list_at + 4 * i for i in range(len(material_ptrs))]


def convert(dff_path, template_path, out_path, txd_path=None, weapon=None):
    """The .dff's parts onto the template's: a VCS vehicle .mdl, or a model of one element (a weapon); with the .txd,
    materials naming a texture it lacks map to VCS's shared ones (VC_GENERIC) or lose their texture. `weapon`: the VCS
    weapon replaced - its parts move from VC's place in the hand to VCS's (WEAPON_PLACES), and its HUD icon (the texture
    of its name) stays VCS's: VC's texture of that name is used as `<name>_pc`, as txd2xtx --preserve writes it."""
    m = mdlinfo.Mdl(open(template_path, "rb").read())
    dff = read_dff(open(dff_path, "rb").read())
    textures, texture_names = None, {}
    if txd_path:
        import rwinfo
        texture_names = {t["name"].lower(): t["name"] for t in rwinfo.read_txd(open(txd_path, "rb").read())["textures"]}
        textures = set(texture_names)
    frames = dff["frames"]
    by_name = {f["name"].lower(): i for i, f in enumerate(frames)}
    group = m.ptr(0x20)
    data = bytearray(m.d[:m.data_size])
    relocs = set(r for r in m.relocs if r < m.data_size)
    # a vehicle's data starts with its element group (object type 2); a weapon's (or any simple model's) with its one
    # element (type 1): no paint lists, no extras, every VC part into that element
    single = m.d[group] == 1
    weapon = (weapon or "").lower()
    place = WEAPON_PLACES.get(weapon) if single else None  # a weapon: from VC's place in the hand to VCS's

    root = m.ptr(group + 4)
    extra_list, extra_count = m.ptr(0x28), m.u32(0x24)
    extra_elements = [m.ptr(extra_list + 4 * k) for k in range(extra_count)] if extra_list and extra_count <= 64 and not single else []
    group_elements = [group] if single else m.elements(group)
    elements = group_elements + [e for e in extra_elements if e and e not in group_elements]

    def rr_vc(i):
        """A VC frame's matrix relative to the clump's root (the root's own is the vehicle's place: the game sets it)."""
        mat, guard = IDENTITY, 0
        while frames[i]["parent"] >= 0 and guard < 64:
            mat = mat_mul(mat, (tuple(frames[i]["rot"]), tuple(frames[i]["pos"])))
            i = frames[i]["parent"]
            guard += 1
        return mat

    def node_local(n):
        rows = [struct.unpack_from("<3f", data, n + 0x10 + 16 * k) for k in range(4)]
        return (rows[0] + rows[1] + rows[2], rows[3])

    def rr_vcs(n):
        """A VCS node's matrix relative to the root, from the data as it stands (extras' nodes hang free: their own)."""
        mat, guard = IDENTITY, 0
        while n and n != root and guard < 64:
            mat = mat_mul(mat, node_local(n))
            n = m.ptr(n + 4)
            guard += 1
        return mat

    # the nodes: where the VC frame of the same name is, relative to the root - whatever the trees between
    placed = [nd["at"] for nd in m.node(root) if nd["at"] != root]
    for e in elements:
        if m.ptr(e + 4) and m.ptr(e + 4) not in placed:
            placed.append(m.ptr(e + 4))
    moved = 0
    for n in placed:
        name = m.cstr(m.ptr(n + 0xA8)).lower()
        i = by_name.get(name)
        if not name or i is None or frames[i]["parent"] < 0:
            continue
        want = rr_vc(i)
        parent = m.ptr(n + 4)
        loc = mat_mul(want, mat_inv(rr_vcs(parent))) if parent and parent != root else want
        for k in range(3):
            struct.pack_into("<3f", data, n + 0x10 + 16 * k, *loc[0][3 * k:3 * k + 3])
        struct.pack_into("<3f", data, n + 0x40, *loc[1])
        moved += 1

    # the template's paint: its first-colour materials lend their effects (matfx: the reflection) to the new ones
    paint_fx = 0
    for k in range(0 if single else PAINT_LISTS[0][1]):
        mat = m.ptr(PAINT_LISTS[0][0] + 4 * k)
        if not mat:
            break
        if mat + 12 in m.relocs:
            paint_fx = m.u32(mat + 12)
            break
    for at, count in () if single else PAINT_LISTS:  # emptied: the new materials go in
        for k in range(count):
            struct.pack_into("<I", data, at + 4 * k, 0)
            relocs.discard(at + 4 * k)
    paint_used = [0, 0]
    names = {}

    def string(text):
        """A texture name in the data (the game keeps the materials' names: found at load)."""
        if text not in names:
            names[text] = len(data)
            data.extend(text.encode("latin1")[:31] + b"\0")
            data.extend(b"\0" * (-len(data) % 4))
        return names[text]

    def material(mat):
        """A new material for a VC one: {texture name, colour, 2, effects}; paint goes into the paint lists."""
        colour = tuple(mat["colour"][:3])
        kind = None if single else 0 if colour == PRIMARY else 1 if colour == SECONDARY else None
        tex = mat["texture"].lower()
        if textures is not None and tex and tex not in textures:
            tex = VC_GENERIC.get(tex, "")  # not the car's own: VCS's shared one, or none (drawn in its colour)
        if weapon and tex == weapon:  # VCS's texture of that name stays (a weapon's HUD icon): VC's is renamed
            tex = (texture_names.get(tex, tex) + "_pc")[:31]
        rgba = (255, 255, 255, 255) if kind is not None else tuple(mat["colour"])
        name_at = string(texture_names.get(tex, tex)) if tex else 0
        data.extend(b"\0" * (-len(data) % 4))
        at = len(data)
        data.extend(struct.pack("<I4BII", name_at, *rgba, 2, paint_fx if kind is not None else 0))
        if name_at:
            relocs.add(at)
        if kind is not None and paint_fx:
            relocs.add(at + 12)
        if kind is not None:
            base, count = PAINT_LISTS[kind]
            if paint_used[kind] < count:
                struct.pack_into("<I", data, base + 4 * paint_used[kind], at)
                relocs.add(base + 4 * paint_used[kind])
                paint_used[kind] += 1
            else:
                print("warning: more %s-colour materials than the list holds (%d): the rest stay white" % (
                    ("first", "second")[kind], count))
        return at

    # Each VCS element gets VC parts: the part of its node's name (chassis_hi, a bike's forks_front, a boat's boat_hi,
    # extra1); X_dummy's two elements (intact - object flag 4 -, damaged) X_hi_ok and X_hi_dam (X_hi_ok again when VC has
    # no damaged one);
    # a helicopter's rotors their VC rotor. VC parts left over: damaged ones, extras and LODs go (VCS has no place for
    # them); the rest (wings VCS builds into its chassis, small parts) join the chassis (or a boat's hull) where they
    # are. Elements no VC part reaches are emptied: VCS's own parts (glass_hi, part1, car_parts) would show through.
    atoms = dff["atomics"]
    names_of = [frames[a["frame"]]["name"].lower() for a in atoms]
    used = set()

    def take(name, keep=False):
        for k, n in enumerate(names_of):
            if n == name and (keep or k not in used):
                used.add(k)
                return k
        return None

    by_node = []
    for e in elements:
        name = m.cstr(m.ptr(m.ptr(e + 4) + 0xA8)).lower()
        for entry in by_node:
            if entry[0] == name:
                entry[1].append(e)
                break
        else:
            by_node.append((name, [e]))
    assign = {}
    for name, els in by_node:
        if name.endswith("_dummy"):
            # intact elements have object flag 4 (+2: shown), damaged ones not - in any order in the list, and some
            # dummies hold more than one of a kind (a van's door and its window): VC's part goes into the first, the
            # others are emptied
            intact = [e for e in els if m.d[e + 2] & 4]
            damaged = [e for e in els if not m.d[e + 2] & 4]
            if not intact:
                intact, damaged = els[:1], els[1:]
            base = name[:-len("_dummy")]
            ok, dam = take(base + "_hi_ok"), take(base + "_hi_dam")
            for slot, k in ((intact, ok if ok is not None else dam), (damaged, dam if dam is not None else ok)):
                if slot and k is not None:
                    assign[slot[0]] = [k]
        elif name in ALIASES:
            k = take(ALIASES[name], keep=True)
            if k is not None:
                assign[els[0]] = [k]
        else:
            for e in els:
                k = take(name)
                if k is not None:
                    assign[e] = [k]
    host = group if single else next((e for name, els in by_node if name in ("chassis_hi", "boat_hi") for e in els), None)
    merged, dropped = [], []
    # A weapon's .dff can hold a part VCS keeps as a model of its own - SA's minigun has its spinning barrels
    # (`minigun2`, VCS's model 366, which the game turns while firing). Converted onto that model, only that part goes
    # in, in its own frame (as VC's minigun2.dff has it); onto any other, a part named like a VCS weapon model stays out.
    own = [k for k, n in enumerate(names_of) if single and weapon and n == weapon and frames[atoms[k]["frame"]]["parent"] >= 0]
    for k, n in enumerate(names_of):
        if k in used:
            continue
        # a weapon's muzzle flash (SA's `gunflash` part: shown by SA only while firing; VCS draws its own)
        if (n.endswith("_dam") or n.startswith("extra") or n.endswith("_vlo") or host is None or (single and n == "gunflash")
                or (own and k not in own) or (single and n in WEAPON_PLACES and n != weapon)):
            dropped.append(n)
        else:
            assign.setdefault(host, []).append(k)
            merged.append(n)
    data.extend(b"\0" * (-len(data) % 16))
    replaced, blanked = [], []
    any_geom = next((m.ptr(e + 0x14) for e in elements if m.ptr(e + 0x14)), 0)

    def put(element, verts, normals, uvs, groups, mats):
        """A new geometry block at the end of the data, the element pointed at it."""
        block, inner = build_geometry(m, m.ptr(element + 0x14) or any_geom, verts, normals, uvs, groups, mats)
        data.extend(b"\0" * (-len(data) % 16))  # the DMA data inside on qwords
        at = len(data)
        block = bytearray(block)
        struct.pack_into("<I", block, 0x0C, at + struct.unpack_from("<I", block, 0x0C)[0])
        data.extend(block)
        relocs.update(at + r for r in inner)
        struct.pack_into("<I", data, element + 0x14, at)

    kept = []
    for e in elements:
        node = m.ptr(e + 4)
        parts = assign.get(e, [])
        if not parts and m.cstr(m.ptr(node + 0xA8)).lower() in KEEP:
            kept.append(m.cstr(m.ptr(node + 0xA8)))
            continue
        if not parts:
            blank = material({"colour": (0, 0, 0, 0), "texture": ""})
            put(e, [(0.0, 0.0, 0.0)] * 3, None, [(0.0, 0.0)] * 3, [(0, [0, 1, 2])], [blank])
            blanked.append(m.cstr(m.ptr(node + 0xA8)))
            continue
        to_node = mat_inv(rr_vcs(node))
        verts, normals, uvs, mats, by_key = [], [], [], [], {}
        has_normals = any(dff["geometries"][atoms[k]["geometry"]]["normals"] for k in parts)
        for k in parts:
            g = dff["geometries"][atoms[k]["geometry"]]
            # the part's frame -> the element's node (a weapon model's own part: its frame is the model's space)
            to = mat_mul(IDENTITY if k in own else rr_vc(atoms[k]["frame"]), to_node)
            if place:
                to = mat_mul(to, place)
            base = len(verts)
            verts += [mat_apply(to, v) for v in g["verts"]]
            normals += [mat_apply(to, n, False) for n in g["normals"]] if g["normals"] else [(0.0, 0.0, 1.0)] * len(g["verts"])
            uvs += g["uvs"]
            for mi, mat in enumerate(g["materials"]):
                tris = [(a + base, b + base, c + base) for a, b, c, t in g["tris"] if t == mi]
                if not tris:
                    continue
                key = (mat["texture"].lower(), tuple(mat["colour"]))  # one mesh per distinct material
                if key not in by_key:
                    by_key[key] = (len(mats), [])
                    mats.append(material(mat))
                by_key[key][1].extend(tris)
        groups = []
        for k, tris in by_key.values():
            for part in uv_tiles(tris, uvs):
                s = strip_of(part)
                if s:
                    groups.append((k, s))
        if not groups:
            blank = material({"colour": (0, 0, 0, 0), "texture": ""})
            put(e, [(0.0, 0.0, 0.0)] * 3, None, [(0.0, 0.0)] * 3, [(0, [0, 1, 2])], [blank])
            blanked.append(m.cstr(m.ptr(node + 0xA8)))
            continue
        put(e, verts, normals if has_normals else None, uvs, groups, mats)
        replaced.append(m.cstr(m.ptr(node + 0xA8)) + "<" + "+".join(names_of[k] for k in parts))

    # The file's tail (past the data: the global table, the relocation table and the strings - texture names, read at
    # load, past the data the game keeps) is written anew as: the global table, the relocation table, the strings in
    # their order. A vehicle's tail is already in that order (G = the data's end, R, then the strings); a weapon's has
    # the relocation table first, the global table after it and strings on both sides. Pointers into it and relocation
    # entries in it move with it.
    G, R, F, D = m.global_tab, m.reloc_tab, m.file_size, m.data_size
    R_end = R + 4 * m.num_relocs
    G_end = R if G < R else G
    if G >= R:  # the global table's end: its entries are pointers (relocated)
        while G_end in m.relocs and G_end < F:
            G_end += 4
    others, at = [], D
    for a, b in sorted(((G, G_end), (R, R_end))):
        if a > at:
            others.append((at, a))
        at = max(at, b)
    if at < F:
        others.append((at, F))
    data_size = len(data)
    relocs.update(r - G + data_size for r in m.relocs if G <= r < G_end)
    tail_at = data_size + (G_end - G) + 4 * len(relocs)
    place, pos = [], tail_at
    for a, b in others:
        place.append((a, b, pos))
        pos += b - a

    def moved_to(v):
        if G <= v < G_end:
            return v - G + data_size
        for a, b, p in place:
            if a <= v < b:
                return v - a + p
        return None

    for r in m.relocs:  # the template's own pointers into the tail (new geometry's pointers are already final)
        if r < D and D <= m.u32(r) < F and m.u32(r) == struct.unpack_from("<I", data, r)[0] and moved_to(m.u32(r)) is not None:
            struct.pack_into("<I", data, r, moved_to(m.u32(r)))
    gtab = bytearray(m.d[G:G_end])
    for r in m.relocs:  # the global table's entries point into the data: unchanged; any into the tail: moved
        if G <= r < G_end:
            v = struct.unpack_from("<I", gtab, r - G)[0]
            if D <= v < F and moved_to(v) is not None:
                struct.pack_into("<I", gtab, r - G, moved_to(v))
    data += gtab
    reloc_tab = len(data)
    rel = sorted(relocs)
    data += struct.pack("<%dI" % len(rel), *rel)
    assert len(data) == tail_at
    for a, b in others:
        data += m.d[a:b]
    struct.pack_into("<7I2H", data, 0, m.ident, m.shrink, len(data), data_size, reloc_tab, len(rel), data_size,
                     m.num_classes, m.num_funcs)
    open(out_path, "wb").write(bytes(data))
    print("%s: %d nodes placed; elements: %s; emptied: %s; kept: %s; left out: %s -> %s (%d bytes, was %d)" % (
        os.path.basename(dff_path), moved, ", ".join(replaced), ", ".join(blanked) or "none", ", ".join(kept) or "none",
        ", ".join(dropped) or "none", out_path, len(data), m.file_size))
    return out_path


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "convert":
        txd = args[args.index("--txd") + 1] if "--txd" in args else None
        weapon = args[args.index("--weapon") + 1] if "--weapon" in args else None
        pos = [a for i, a in enumerate(args) if a not in ("--txd", "--weapon") and (i == 0 or args[i - 1] not in ("--txd", "--weapon"))]
        convert(pos[1], pos[2], pos[3] if len(pos) > 3 else os.path.splitext(os.path.basename(pos[2]))[0] + ".mdl", txd, weapon)
        return 0
    if args[:1] == ["--self-test"]:
        import toolchain as tc
        folder = args[args.index("--img") + 1] if "--img" in args else os.path.join(tc.ROOT, "build", "img", "GTA3PS2")
        return 0 if self_test(folder) else 1
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
