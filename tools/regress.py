#!/usr/bin/env python3
"""Regression suite (milestone 1 of NATIVE_PORT_ROADMAP.md): one command that runs the rig's checks and compares
them with a stored baseline.

    python tools/regress.py [OUT] [--quick] [--set-baseline]

Steps (each a tool of its own, run in turn; the game window shows):
  scenes    tools/scenes.py capture + compare with the baseline's scenes (picture diffs, perf)
  tour      tools/tour.py on a fixed subset of the map grid (classes per point)
  missions  tools/missions.py on a few story missions (crash / hang / dark / frame time)
  drive     tools/drive.py on two spots (frame time, black / dip frames)
  evening   the hospital at 12:00 / 20:00 / 21:00 - no black pictures (the evening black frames)
  saves     tools/saves.py: save to slot 2, Load Game slot 1, Delete Game slot 2 through the game's menus

Writes OUT/report.md and OUT/metrics.json (default OUT: build/regress/<date-time>). --set-baseline stores this
run as build/regress/baseline (metrics.json + the scenes capture); later runs report what changed against it:
tour classes, mission results, frame times (+10% or more), new black / dip frames, failing scenes.
--quick skips missions and halves the rest.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
BASELINE = os.path.join(ROOT, "build", "regress", "baseline")


def run(args, log):
    print("  $", " ".join(args))
    with open(log, "w") as f:
        r = subprocess.run([sys.executable] + args, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    return r.returncode, open(log, errors="replace").read()


def step_scenes(out, quick):
    cap = os.path.join(out, "scenes")
    run([os.path.join(TOOLS, "scenes.py"), "capture", cap], os.path.join(out, "scenes_capture.log"))
    m = {"captured": sorted(f[:-4] for f in os.listdir(cap) if f.endswith(".png") and f != "sheet.png")} if os.path.isdir(cap) else {}
    ref = os.path.join(BASELINE, "scenes")
    if os.path.isdir(ref):
        rc, text = run([os.path.join(TOOLS, "scenes.py"), "compare", ref, cap], os.path.join(out, "scenes_compare.log"))
        m["compare_failed"] = [l.split()[0] for l in text.splitlines() if l.rstrip().endswith("FAIL") or "FAIL missing" in l]
    return m


def step_tour(out, quick):
    d = os.path.join(out, "tour")
    args = [os.path.join(TOOLS, "tour.py"), d, "--every", "9" if not quick else "18", "--limit", "27" if not quick else "12"]
    run(args, os.path.join(out, "tour.log"))
    recs = [json.loads(l) for l in open(os.path.join(d, "results.jsonl"))] if os.path.isfile(os.path.join(d, "results.jsonl")) else []
    classes = {}
    for r in recs:
        classes[r["class"]] = classes.get(r["class"], 0) + 1
    fts = sorted(r.get("perf", {}).get("frame_ms", 0) for r in recs if r.get("perf"))
    return {"points": len(recs), "classes": classes, "per_point": {"%d,%d" % tuple(r["target"]): r["class"] for r in recs},
            "frame_ms_median": fts[len(fts) // 2] if fts else None}


def step_missions(out, quick):
    if quick:
        return {}
    d = os.path.join(out, "missions")
    run([os.path.join(TOOLS, "missions.py"), d, "--only", "45,46,50,55", "--seconds", "30"], os.path.join(out, "missions.log"))
    path = os.path.join(d, "results.jsonl")
    recs = [json.loads(l) for l in open(path)] if os.path.isfile(path) else []
    return {str(r["mission"]): {k: r.get(k) for k in ("name", "result", "mission_running", "worst_frame_ms", "dark_shots")} for r in recs}


def step_drive(out, quick):
    d = os.path.join(out, "drive")
    _, text = run([os.path.join(TOOLS, "drive.py"), d, "--spots", "0,2", "--seconds", "45" if not quick else "25"],
                  os.path.join(out, "drive.log"))
    lines = [l for l in text.splitlines() if l.startswith("spot ")]
    fms = [float(re.search(r"frame_ms=([\d.]+)", l).group(1)) for l in lines]
    black = max([int(re.search(r"black=(\d+)", l).group(1)) for l in lines] or [0])
    dips = max([int(re.search(r"dips=(\d+)", l).group(1)) for l in lines] or [0])
    events = [l.strip() for l in text.splitlines() if "[frames]" in l]
    return {"samples": len(lines), "frame_ms_max": max(fms) if fms else None,
            "frame_ms_mean": round(sum(fms) / len(fms), 1) if fms else None, "black_frames": black, "dip_frames": dips,
            "events": events[-20:]}


def step_evening(out, quick):
    """The evening black frames (game-internals.md 6g): at the hospital, 20:00 and 21:00 must show a picture. Fixed
    2026-10-23 (render targets tracked taller than the scissor were dropped by a texture upload into the Z area)."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    import tour  # noqa: E402
    d = os.path.join(out, "evening")
    os.makedirs(d, exist_ok=True)
    g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="regress_evening")
    lumas = {}
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        g.teleport(-884.0, -470.0, -100.0)
        time.sleep(3)
        for h in (12, 20, 21):
            g.set_time(h, 0)
            time.sleep(2.5)  # the scene load's fade is over
            vals = []
            for k in range(3):
                path = os.path.join(d, "t%02d_%d.png" % (h, k))
                g.cmd("screenshot " + path)
                vals.append(round(tour.picture_stats(path)[0], 1))
                time.sleep(0.4)
            lumas["%02d:00" % h] = vals
    finally:
        g.quit()
    black = [t for t, v in lumas.items() if min(v) < 10]
    return {"luma": lumas, "black_times": black}


def step_saves(out, quick):
    d = os.path.join(out, "saves")
    run([os.path.join(TOOLS, "saves.py"), d], os.path.join(out, "saves.log"))
    path = os.path.join(d, "results.json")
    return {k: v["ok"] for k, v in json.load(open(path)).items()} if os.path.isfile(path) else {"error": "no results"}


def compare(base, new):
    notes = []
    bt, nt = base.get("tour", {}), new.get("tour", {})
    for p, c in nt.get("per_point", {}).items():
        b = bt.get("per_point", {}).get(p)
        if b and b != c:
            notes.append("tour %s: %s -> %s" % (p, b, c))
    for k, r in new.get("missions", {}).items():
        b = base.get("missions", {}).get(k)
        if b and (b.get("result") != r.get("result") or (r.get("dark_shots") or 0) > (b.get("dark_shots") or 0)):
            notes.append("mission %s %s: %s -> %s" % (k, r.get("name"), b, r))
        if b and b.get("worst_frame_ms") and r.get("worst_frame_ms") and r["worst_frame_ms"] > b["worst_frame_ms"] * 1.1:
            notes.append("mission %s worst frame %.1f -> %.1f ms" % (k, b["worst_frame_ms"], r["worst_frame_ms"]))
    bd, nd = base.get("drive", {}), new.get("drive", {})
    for k in ("frame_ms_mean", "frame_ms_max"):
        if bd.get(k) and nd.get(k) and nd[k] > bd[k] * 1.1:
            notes.append("drive %s %.1f -> %.1f" % (k, bd[k], nd[k]))
    for k in ("black_frames", "dip_frames"):
        if nd.get(k, 0) > bd.get(k, 0):
            notes.append("drive %s %d -> %d" % (k, bd.get(k, 0), nd.get(k, 0)))
    for t in new.get("evening", {}).get("black_times", []):
        notes.append("evening: black picture at %s (the evening black frames are back?)" % t)
    for k, ok in new.get("saves", {}).items():
        if ok is not True:
            notes.append("saves: %s failed (see saves.log and the screenshots)" % k)
    for s in new.get("scenes", {}).get("compare_failed", []):
        notes.append("scene %s differs from the baseline (see scenes_compare.log, diff images)" % s)
    return notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--set-baseline", action="store_true")
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "regress", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    metrics = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "quick": a.quick}
    for name, fn in (("scenes", step_scenes), ("tour", step_tour), ("missions", step_missions), ("drive", step_drive),
                     ("evening", step_evening), ("saves", step_saves)):
        print("[regress] %s" % name)
        t0 = time.time()
        try:
            metrics[name] = fn(out, a.quick)
        except Exception as e:  # a broken step must not hide the others
            metrics[name] = {"error": str(e)[:300]}
        metrics[name + "_s"] = round(time.time() - t0)
    json.dump(metrics, open(os.path.join(out, "metrics.json"), "w"), indent=1)

    base_path = os.path.join(BASELINE, "metrics.json")
    notes = compare(json.load(open(base_path)), metrics) if os.path.isfile(base_path) else ["(no baseline yet)"]
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# Regression run %s\n\n" % metrics["started"])
        f.write("## Changes against the baseline\n\n" + ("\n".join("- " + n for n in notes) if notes else "- none") + "\n\n")
        f.write("## Tour\n\n%s\n\n## Missions\n\n%s\n\n## Drive\n\n%s\n" % (
            json.dumps(metrics.get("tour", {}).get("classes")), json.dumps(metrics.get("missions"), indent=1),
            json.dumps({k: v for k, v in metrics.get("drive", {}).items() if k != "events"})))
    print(open(os.path.join(out, "report.md")).read())
    if a.set_baseline:
        if os.path.isdir(BASELINE):
            shutil.rmtree(BASELINE)
        os.makedirs(BASELINE)
        shutil.copy(os.path.join(out, "metrics.json"), base_path)
        if os.path.isdir(os.path.join(out, "scenes")):
            shutil.copytree(os.path.join(out, "scenes"), os.path.join(BASELINE, "scenes"))
        print("baseline stored in", BASELINE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
