#!/usr/bin/env python3
"""Summarise a perf recording (F4 in game, or PS2X_PERF_LOG=<file.csv>).

    python tools/perf_report.py perf_20261022_170000.csv [--from S] [--to S] [--worst N]

Prints frame-rate and frame-time percentiles, where the EE thread's time goes (game code / render submit /
IOP / waiting for VBlank), render counters, host and audio state, a frame-time histogram, the worst frames
with their breakdown, and per-10-second segments. One row of the CSV = one game frame.
No third-party modules.
"""
import argparse
import csv
import sys

PHASES = ["game_ms", "render_ms", "iop_ms", "wait_ms"]
COUNTERS = ["draws", "verts", "tex_uploads", "dma_tags", "vu1_programs"]


def load(path, t_from, t_to):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if None in r.values() or None in r:  # last line cut off (game killed while recording)
                continue
            row = {k: float(v) for k, v in r.items() if v != ""}
            if t_from is not None and row["t_s"] < t_from:
                continue
            if t_to is not None and row["t_s"] > t_to:
                continue
            rows.append(row)
    return rows


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    return sorted_vals[min(len(sorted_vals) - 1, int(q * (len(sorted_vals) - 1) + 0.5))]


def mean(rows, key):
    return sum(r[key] for r in rows) / len(rows) if rows else 0.0


def summary_line(rows):
    secs = sum(r["frame_ms"] for r in rows) / 1000.0
    ms = sorted(r["frame_ms"] for r in rows)
    fps = len(rows) / secs if secs else 0.0
    vbl = sum(r["vblanks"] for r in rows) / secs if secs else 0.0
    return secs, fps, vbl, ms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t_from", type=float, help="ignore frames before this time (s)")
    ap.add_argument("--to", dest="t_to", type=float, help="ignore frames after this time (s)")
    ap.add_argument("--worst", type=int, default=10, help="how many worst frames to list")
    a = ap.parse_args()

    rows = load(a.csv, a.t_from, a.t_to)
    if not rows:
        sys.exit("no frames in range")
    secs, fps, vbl, ms = summary_line(rows)
    print(f"{a.csv}: {len(rows)} frames over {secs:.1f} s (t={rows[0]['t_s']:.1f}..{rows[-1]['t_s']:.1f})")
    print(f"game     {fps:6.1f} fps   frame ms: avg {secs * 1000 / len(rows):.1f}  p50 {pct(ms, .5):.1f}  "
          f"p90 {pct(ms, .9):.1f}  p95 {pct(ms, .95):.1f}  p99 {pct(ms, .99):.1f}  max {ms[-1]:.1f}")
    print(f"vblank   {vbl:6.1f} /s  (60 = full speed; {mean(rows, 'vblanks'):.2f} per game frame)")

    total = sum(mean(rows, p) for p in PHASES)
    print("EE thread per frame (ms, share of the frame):")
    for p in PHASES:
        m = mean(rows, p)
        print(f"  {p[:-3]:<7}{m:7.2f}  {100 * m / total if total else 0:5.1f}%  p95 {pct(sorted(r[p] for r in rows), .95):6.2f}")
    print("per frame: " + "  ".join(f"{c} {mean(rows, c):.0f}" for c in COUNTERS))
    host = [r for r in rows if r["host_fps"] > 0]
    if host:
        print(f"host     {mean(host, 'host_fps'):6.1f} fps  {mean(host, 'host_ms'):.1f} ms/frame  "
              f"GPU submit {mean(host, 'gl_ms'):.2f} ms (p95 {pct(sorted(r['gl_ms'] for r in host), .95):.2f})")
    print(f"audio    {mean(rows, 'audio_ms'):.0f} ms queued on average, min {min(r['audio_ms'] for r in rows):.0f}, "
          f"{int(sum(r['underruns'] for r in rows))} underruns")

    print("frame time histogram:")
    edges = [0, 17.5, 34, 50, 67, 100, 1e9]
    labels = ["<=17 (60)", "17-34 (30)", "34-50 (20)", "50-67 (15)", "67-100", ">100"]
    for lo, hi, lab in zip(edges, edges[1:], labels):
        n = sum(1 for v in ms if lo < v <= hi or (lo == 0 and v <= hi))
        print(f"  {lab:<11}{n:6d}  {'#' * int(50 * n / len(ms))}")

    print(f"worst {a.worst} frames:")
    print("  t_s      frame   game render    iop   wait  draws   verts  tex   tags")
    for r in sorted(rows, key=lambda r: -r["frame_ms"])[:a.worst]:
        print(f"  {r['t_s']:7.2f} {r['frame_ms']:7.1f} {r['game_ms']:6.1f} {r['render_ms']:6.1f} {r['iop_ms']:6.1f} "
              f"{r['wait_ms']:6.1f} {r['draws']:6.0f} {r['verts']:7.0f} {r['tex_uploads']:4.0f} {r['dma_tags']:6.0f}")

    print("10 s segments:")
    print("  from    fps   avg   p95  game render  iop  wait  draws  tags")
    start = rows[0]["t_s"]
    seg = []
    for r in rows + [None]:
        if r is None or r["t_s"] - start >= 10.0:
            if seg:
                s_secs, s_fps, _, s_ms = summary_line(seg)
                print(f"  {start:6.1f} {s_fps:5.1f} {s_secs * 1000 / len(seg):5.1f} {pct(s_ms, .95):5.1f} "
                      f"{mean(seg, 'game_ms'):5.1f} {mean(seg, 'render_ms'):6.1f} {mean(seg, 'iop_ms'):4.1f} "
                      f"{mean(seg, 'wait_ms'):5.1f} {mean(seg, 'draws'):6.0f} {mean(seg, 'dma_tags'):5.0f}")
            if r is None:
                break
            start, seg = r["t_s"], []
        seg.append(r)


if __name__ == "__main__":
    main()
