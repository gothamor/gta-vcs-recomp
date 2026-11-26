#!/usr/bin/env python3
"""Which MPEG-2 features the intro movies use (analysis only; docs/game-internals.md 3 "PSS layout").

    python tools/pss_survey.py [ISO] [--out build/analysis/pss_survey.txt]

Reads MOVIES/STINGS.PSS and MOVIES/INTRO.PSS from the disc (the ISO in build/vcs.ini when not given), joins the video
stream (0xE0) as src/game/vcs_movie.cpp's demuxer does and walks its start codes: sequence header and extensions,
GOPs, picture headers and coding extensions, quant matrices, slices. Prints what the native MPEG-2 decoder
(src/video/mpeg2.cpp) must handle and what the streams never use. Nothing from the disc is written out but counts.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcs_disc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOVIES = ["MOVIES/STINGS.PSS", "MOVIES/INTRO.PSS"]


class Bits:
    def __init__(self, data, pos=0):
        self.data, self.bit = data, pos * 8

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.bit >> 3] if (self.bit >> 3) < len(self.data) else 0
            v = (v << 1) | ((byte >> (7 - (self.bit & 7))) & 1)
            self.bit += 1
        return v


def demux_video(file):
    """The 0xE0 stream's payload bytes, joined (vcs_movie.cpp's demux)."""
    out = bytearray()
    i, n = 0, len(file)
    while i + 6 <= n:
        if file[i] != 0 or file[i + 1] != 0 or file[i + 2] != 1:
            i += 1
            continue
        sid = file[i + 3]
        if sid == 0xBA:
            if i + 14 > n:
                break
            i += 14 + (file[i + 13] & 7)
            continue
        if sid == 0xB9:
            break
        if sid in (0xBB, 0xBD, 0xBE, 0xBF) or 0xC0 <= sid <= 0xEF:
            length = (file[i + 4] << 8) | file[i + 5]
            body, end = i + 6, min(n, i + 6 + length)
            if sid == 0xE0:
                payload = body + 3 + file[body + 2]
                out += file[payload:end]
            i = end
            continue
        i += 1
    return bytes(out)


def start_codes(es):
    i = es.find(b"\x00\x00\x01")
    while i >= 0 and i + 3 < len(es):
        yield i, es[i + 3]
        i = es.find(b"\x00\x00\x01", i + 3)


def survey(name, es):
    c = collections.Counter()
    seen = collections.defaultdict(set)
    gop_pattern, gops, current = [], 0, ""
    ptype = None
    pic_sizes = collections.defaultdict(list)
    last_pic = None
    rows = set()
    for pos, code in start_codes(es):
        b = Bits(es, pos + 4)
        if code == 0xB3:
            c["sequence headers"] += 1
            w, h = b.u(12), b.u(12)
            aspect, rate = b.u(4), b.u(4)
            b.u(18), b.u(1), b.u(10), b.u(1)
            seen["size"].add("%dx%d" % (w, h))
            seen["aspect_ratio_information"].add(aspect)
            seen["frame_rate_code"].add(rate)
            if b.u(1):
                seen["load_intra_quantiser_matrix"].add(1)
                b.u(64 * 8)
            else:
                seen["load_intra_quantiser_matrix"].add(0)
            seen["load_non_intra_quantiser_matrix"].add(b.u(1))
        elif code == 0xB5:
            ext = b.u(4)
            if ext == 1:
                seen["profile_and_level"].add(hex(b.u(8)))
                seen["progressive_sequence"].add(b.u(1))
                seen["chroma_format"].add(b.u(2))
                b.u(2), b.u(2), b.u(12), b.u(1), b.u(8)
                seen["low_delay"].add(b.u(1))
                seen["frame_rate_extension"].add((b.u(2), b.u(5)))
            elif ext == 2:
                seen["sequence_display_extension"].add(1)
            elif ext == 3:
                seen["quant_matrix_extension"].add(1)
            elif ext == 8:
                fc = [b.u(4) for _ in range(4)]
                seen["f_code %s" % "IPB"[ptype - 1] if ptype in (1, 2, 3) else "f_code ?"].add(tuple(fc))
                for key, bits in (("intra_dc_precision", 2), ("picture_structure", 2), ("top_field_first", 1),
                                  ("frame_pred_frame_dct", 1), ("concealment_motion_vectors", 1),
                                  ("q_scale_type", 1), ("intra_vlc_format", 1), ("alternate_scan", 1),
                                  ("repeat_first_field", 1), ("chroma_420_type", 1), ("progressive_frame", 1)):
                    seen[key].add(b.u(bits))
            else:
                seen["other extensions"].add(ext)
        elif code == 0xB8:
            gops += 1
            if current:
                gop_pattern.append(current)
            current = ""
            b.u(25)
            seen["closed_gop"].add(b.u(1))
            seen["broken_link"].add(b.u(1))
        elif code == 0x00:
            if last_pic is not None:
                pic_sizes[last_pic[1]].append(pos - last_pic[0])
            b.u(10)
            ptype = b.u(3)
            c["pictures"] += 1
            current += "?IPBD"[ptype] if ptype < 5 else "?"
            last_pic = (pos, "?IPBD"[ptype] if ptype < 5 else "?")
            if ptype in (2, 3):
                b.u(16)
                seen["full_pel_forward_vector"].add(b.u(1))
        elif 0x01 <= code <= 0xAF:
            c["slices"] += 1
            rows.add(code)
            seen["slice quantiser_scale_code"].add(b.u(5))
        elif code == 0xB7:
            c["sequence end codes"] += 1
        elif code == 0xB2:
            c["user data"] += 1
        else:
            seen["other start codes"].add(hex(code))
    if current:
        gop_pattern.append(current)
    if last_pic is not None:
        pic_sizes[last_pic[1]].append(len(es) - last_pic[0])
    lines = ["== %s: %d bytes of video" % (name, len(es))]
    for k, v in sorted(c.items()):
        lines.append("  %-34s %d" % (k, v))
    lines.append("  %-34s %d (rows %d..%d)" % ("slice rows", len(rows), min(rows), max(rows)))
    lines.append("  %-34s %d" % ("GOPs", gops))
    lengths = collections.Counter(gop_pattern)
    lines.append("  GOP patterns (count: decode order):")
    for pattern, n in lengths.most_common(6):
        lines.append("    %4d: %s" % (n, pattern))
    for t, sizes in sorted(pic_sizes.items()):
        lines.append("  %s pictures: %d, avg %d bytes, max %d" % (t, len(sizes), sum(sizes) // len(sizes), max(sizes)))
    for k in sorted(seen):
        vals = seen[k]
        if k == "slice quantiser_scale_code":
            lines.append("  %-34s %d..%d" % (k, min(vals), max(vals)))
        else:
            lines.append("  %-34s %s" % (k, ", ".join(str(v) for v in sorted(vals, key=str))))
    return "\n".join(lines)


def default_iso():
    for ini in (os.path.join(ROOT, "build", "vcs.ini"), os.path.join(ROOT, "vcs.ini")):
        if os.path.isfile(ini):
            with open(ini, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.strip().startswith("iso="):
                        return line.split("=", 1)[1].strip()
    return None


def main(argv):
    out = os.path.join(ROOT, "build", "analysis", "pss_survey.txt")
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--out" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 2
            continue
        args.append(argv[i])
        i += 1
    iso = args[0] if args else default_iso()
    if not iso or not os.path.isfile(iso):
        sys.exit("usage: pss_survey.py [ISO] [--out FILE] (no ISO given and none in build/vcs.ini)")
    img = vcs_disc.IsoImage(iso)
    reports = []
    for path in MOVIES:
        rec = img.find(path)
        if not rec:
            reports.append("== %s: not on the disc" % path)
            continue
        reports.append(survey(path, demux_video(img.read_file(rec))))
    text = "\n\n".join(reports)
    print(text)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print("\n[pss_survey] written to %s" % out)


if __name__ == "__main__":
    main(sys.argv[1:])
