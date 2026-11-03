#!/usr/bin/env python3
"""PC VC car (.dff) -> VCS vehicle model (`ldm`), FORK.md T9 step 3 - the encoder first.

    python tools/dff2mdl.py --self-test [--img DIR]   every PS2 mesh of the extracted vehicles (tools/img.py extract)
                                                     decoded to its raw vertex values and encoded again: the bytes
                                                     must come out identical to the game's

A mesh's DMA data (docs/game-internals.md 9d, tools/mdlinfo.py): a DMA tag qword (0x60000000 | qwords - 1), then
batches of at most 70 vertices - the next batch repeats the last two (a strip) - each {UNPACK V4-32 x1 at 0x8000: 0, 0,
n, n | 0x8000 (the program's header); STMASK 0x40404040, STROW 0, UNPACK V3-16 positions at 1; STMASK 0x50505050,
STROW (0, 0, 0xFF, 0), UNPACK V2-8 unsigned UVs at 0x8D; UNPACK V3-8 normals at 0x47; MSCAL 0x06} padded with zeros to
a qword.
"""
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
                out["frames"].append({"rot": f[0:9], "pos": f[9:12], "parent": f[12], "name": ""})
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
                uvs = [struct.unpack_from("<2f", data, at + i * 8) for i in range(nverts)] if nsets else [(0.0, 0.0)] * nverts
                at += nsets * nverts * 8
                tris = []
                for i in range(ntris):
                    w0, w1 = struct.unpack_from("<2I", data, at + i * 8)
                    tris.append((w0 >> 16, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF))  # v0, v1, v2, material
                at += ntris * 8 + 16  # + the morph target's bounding sphere
                has_verts, has_normals = struct.unpack_from("<2I", data, at)
                at += 8
                verts = [struct.unpack_from("<3f", data, at + i * 12) for i in range(nverts)]
                at += nverts * 12
                normals = [struct.unpack_from("<3f", data, at + i * 12) for i in range(nverts)] if has_normals else None
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


def strip_of(tris):
    """Triangles -> one strip: greedy strips along shared edges, joined by degenerate triangles (PS2 GTA draws
    without back-face culling, so the winding of each strip does not matter)."""
    by_edge = {}
    for t, (a, b, c) in enumerate(tris):
        for e in ((a, b), (b, c), (c, a)):
            by_edge.setdefault(frozenset(e), []).append(t)
    used = [False] * len(tris)

    def grow(s, mark):
        while True:
            nxt = None
            for t in by_edge.get(frozenset((s[-2], s[-1])), ()):
                if not used[t] and t not in mark:
                    nxt = t
                    break
            if nxt is None:
                return s
            mark.add(nxt)
            s.append(next(v for v in tris[nxt] if v != s[-2] and v != s[-1]))

    out = []
    for t, (a, b, c) in enumerate(tris):
        if used[t]:
            continue
        best = None
        for start in ((a, b, c), (b, c, a), (c, a, b)):
            mark = {t}
            s = grow(list(start), mark)
            if best is None or len(s) > len(best[0]):
                best = (s, mark)
        s, mark = best
        for k in mark:
            used[k] = True
        if out:
            out += [out[-1], s[0]]
            if len(out) % 2:  # keep the next strip's first vertex at an even position
                out.append(s[0])
        out += s
    return out


# ---- the VCS model (template) ------------------------------------------------------------------------------------
PRIMARY, SECONDARY = (0x3C, 0xFF, 0x00), (0xFF, 0x00, 0xAF)  # VC's paint colours in a .dff


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

    meshes, dma, total = [], b"", 0
    for material, strip in groups:
        sv = [verts[i] for i in strip]
        su = [uvs[i] for i in strip]
        uv_scale = (max(1.0, max(abs(u[0]) for u in su) / 2), max(1.0, max(abs(u[1]) for u in su) / 2))
        raw = []
        for i in strip:
            v, u = verts[i], uvs[i]
            n = normals[i] if normals else (0.0, 0.0, 1.0)
            raw.append(((q16((v[0] - centre[0]) / scale[0]), q16((v[1] - centre[1]) / scale[1]), q16((v[2] - centre[2]) / scale[2])),
                        (max(0, min(255, int(round(u[0] / uv_scale[0] * 127.5)))), max(0, min(255, int(round(u[1] / uv_scale[1] * 127.5))))),
                        tuple(max(-127, min(127, int(round(c * 127)))) for c in n)))
        packet = encode(raw)
        mc = tuple(sum(v[k] for v in sv) / len(sv) for k in range(3))
        mr = max(((v[0] - mc[0]) ** 2 + (v[1] - mc[1]) ** 2 + (v[2] - mc[2]) ** 2) ** 0.5 for v in sv)
        meshes.append(struct.pack("<4f2fiIHh6H", *mc, mr, *uv_scale, 0, len(dma), len(strip) - 2, material, *([0] * 6)))
        dma += packet
        total += len(strip)
    n = len(meshes)
    header = struct.pack("<4fIiHH6H3f3f", *centre, radius, (n << 20) | (len(dma) & 0xFFFFF), 7, total & 0xFFFF, 0x40 + n * 0x30,
                         *struct.unpack_from("<6H", m.d, template_geom + 32 + 28), *scale, *centre)
    body = header + b"".join(meshes) + dma
    body += b"\0" * (-len(body) % 4)
    list_at = 32 + len(body)
    geom = bytearray(m.d[template_geom:template_geom + 32])
    struct.pack_into("<3I", geom, 0x0C, list_at, len(material_ptrs), len(material_ptrs))  # filled with absolute offsets later
    block = bytes(geom) + body + struct.pack("<%dI" % len(material_ptrs), *material_ptrs)
    block += b"\0" * (-len(block) % 16)
    return block, [0x0C] + [list_at + 4 * i for i in range(len(material_ptrs))]


def convert(dff_path, template_path, out_path):
    m = mdlinfo.Mdl(open(template_path, "rb").read())
    dff = read_dff(open(dff_path, "rb").read())
    frames = dff["frames"]
    by_name = {f["name"].lower(): i for i, f in enumerate(frames)}
    group = m.ptr(0x20)
    data = bytearray(m.d[:m.data_size])
    relocs = set(r for r in m.relocs if r < m.data_size)

    # the nodes: VC's dummies' places over VCS's (same names)
    root = m.ptr(group + 4)
    moved = 0
    for node in m.node(root):
        i = by_name.get(node["name"].lower())
        if i is None or not node["name"]:
            continue
        f = frames[i]
        at = node["at"] + 0x10
        struct.pack_into("<3f", data, at, *f["rot"][0:3])
        struct.pack_into("<3f", data, at + 16, *f["rot"][3:6])
        struct.pack_into("<3f", data, at + 32, *f["rot"][6:9])
        struct.pack_into("<3f", data, at + 48, *f["pos"])
        moved += 1

    # the template's materials: paint (the vehicle's list at 0x2C) and a textured one for the rest
    paint = []
    for k in range(32):
        if 0x2C + 4 * k not in m.relocs:
            break
        paint.append(m.u32(0x2C + 4 * k))
    textured = None
    for e in m.elements(group):
        g = m.ptr(e + 0x14)
        for mat in (m.ptr(m.ptr(g + 0x0C) + 4 * i) for i in range(m.u32(g + 0x10))) if g else ():
            if mat and "868bit" in m.cstr(m.ptr(mat)):
                textured = textured or mat
    textured = textured or (paint[0] if paint else 0)

    # each VCS element by its node: chassis_hi / _vlo, and X_dummy's two (intact, damaged) from X_hi_ok / X_hi_dam
    by_node = {}
    for e in m.elements(group):
        name = m.cstr(m.ptr(m.ptr(e + 4) + 0xA8))
        by_node.setdefault(name, []).append(e)
    atoms = {frames[a["frame"]]["name"].lower(): a for a in dff["atomics"]}
    end = len(data) + (-len(data) % 16)
    data += b"\0" * (end - len(data))
    replaced = []

    def put(element, verts, normals, uvs, groups, mats):
        """A new geometry block at the end of the data, the element pointed at it."""
        block, inner = build_geometry(m, m.ptr(element + 0x14), verts, normals, uvs, groups, mats)
        at = len(data)
        block = bytearray(block)
        struct.pack_into("<I", block, 0x0C, at + struct.unpack_from("<I", block, 0x0C)[0])
        data.extend(block)
        relocs.update(at + r for r in inner)
        struct.pack_into("<I", data, element + 0x14, at)

    for node, elements in by_node.items():
        if node in ("chassis_hi", "chassis_vlo"):
            pairs = [(elements[0], node)]
        elif node.endswith("_dummy"):
            base = node[:-len("_dummy")]
            pairs = list(zip(elements, (base + "_hi_ok", base + "_hi_dam")))
        else:
            continue
        for element, part in pairs:
            atom = atoms.get(part)
            if not atom and node.endswith("_dummy") and node not in by_name:
                # a part the mod's car does not have (the ADMIRAL's rear doors on a two-door): one degenerate triangle
                put(element, [(0.0, 0.0, 0.0)] * 3, None, [(0.0, 0.0)] * 3, [(0, [0, 1, 2])], [textured])
                replaced.append("-" + node)
                continue
            if not atom:
                continue
            g = dff["geometries"][atom["geometry"]]
            f = frames[atom["frame"]]
            # vertices into the dummy's space: the part's own frame, when it sits under the dummy
            to_dummy = f["parent"] >= 0 and frames[f["parent"]]["name"].lower() == node and part != node
            verts = [apply(f["rot"], f["pos"], v) for v in g["verts"]] if to_dummy else g["verts"]
            normals = [apply(f["rot"], (0, 0, 0), n) for n in g["normals"]] if (to_dummy and g["normals"]) else g["normals"]
            mats, by_target = [], {}
            for mi, mat in enumerate(g["materials"]):
                tris = [(a, b, c) for a, b, c, t in g["tris"] if t == mi]
                if not tris:
                    continue
                colour = tuple(mat["colour"][:3])
                target = paint[0] if colour == PRIMARY and paint else paint[1] if colour == SECONDARY and len(paint) > 1 else textured
                if target not in mats:
                    mats.append(target)
                by_target.setdefault(mats.index(target), []).extend(tris)  # one mesh per VCS material
            groups = [(k, strip_of(t)) for k, t in sorted(by_target.items())]
            if not groups:
                continue
            put(element, verts, normals, g["uvs"], groups, mats)
            replaced.append(part)

    # The file's tail moves as a whole: the global table (G .. R), the relocation table (R .. T) and the strings after
    # it (T .. F: texture names, read at load, past the data the game keeps). Pointers into it and relocation entries in
    # it shift with it.
    G, R = m.global_tab, m.reloc_tab
    T, F = R + 4 * m.num_relocs, m.file_size
    data_size = len(data)
    shift_g = data_size - G  # the global table's new place
    relocs.update(r + shift_g for r in m.relocs if G <= r < R)
    tail_at = data_size + (R - G) + 4 * len(relocs)
    shift_t = tail_at - T
    for r in m.relocs:  # the template's own pointers into the tail (new geometry's pointers are already final)
        if r < m.data_size and T <= m.u32(r) < F and m.u32(r) == struct.unpack_from("<I", data, r)[0]:
            struct.pack_into("<I", data, r, m.u32(r) + shift_t)
    gtab = bytearray(m.d[G:R])
    for r in m.relocs:  # the global table's entries point into the data: unchanged; any into the tail: shifted
        if G <= r < R:
            v = struct.unpack_from("<I", gtab, r - G)[0]
            if v >= T:
                struct.pack_into("<I", gtab, r - G, v + shift_t)
    data += gtab
    reloc_tab = len(data)
    rel = sorted(relocs)
    data += struct.pack("<%dI" % len(rel), *rel)
    assert len(data) == tail_at
    data += m.d[T:F]
    struct.pack_into("<7I2H", data, 0, m.ident, m.shrink, len(data), data_size, reloc_tab, len(rel), data_size,
                     m.num_classes, m.num_funcs)
    open(out_path, "wb").write(bytes(data))
    print("%s: %d nodes placed from the .dff, %d parts converted (%s) -> %s (%d bytes, was %d)" % (
        os.path.basename(dff_path), moved, len(replaced), ", ".join(replaced), out_path, len(data), m.file_size))
    return out_path


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "convert":
        convert(args[1], args[2], args[3] if len(args) > 3 else os.path.splitext(os.path.basename(args[2]))[0] + ".mdl")
        return 0
    if args[:1] == ["--self-test"]:
        import toolchain as tc
        folder = args[args.index("--img") + 1] if "--img" in args else os.path.join(tc.ROOT, "build", "img", "GTA3PS2")
        return 0 if self_test(folder) else 1
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
