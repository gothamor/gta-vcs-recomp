#!/usr/bin/env python3
"""The intro movies check: STINGS.PSS (the logos) and INTRO.PSS play through our own MPEG-2 decoder (src/video/mpeg2.cpp)
with every picture shown, the sound as the clock, and pictures that match the reference.

    python tools/movie_check.py [OUT] [--baseline DIR] [--make-baseline] [--max-dropped 0] [--timeout 150]

Starts the game hidden on an empty card of its own (OUT/data), movies on (VCS_SKIP_MOVIES=0), until INTRO has played.
Passes when
- each movie decodes all its pictures (STINGS 321, INTRO 2527) without a slice error, and at most --max-dropped of them
  were dropped to keep up with the sound,
- the pictures follow the sound: every VCS_MOVIE_LOG line has the clock within 100 ms of the picture due (a run
  without an audio device plays on the host clock; that is reported, not failed),
- every 30th picture (VCS_MOVIE_DUMP, OUT/pictures) matches the baseline within 45 dB PSNR.
The baseline (default build/regress/baseline/movies; game pictures, never committed) was dumped by the FFmpeg decoder
the native one replaced (2026-11-26, docs/DECISIONS.md); --make-baseline writes this run's pictures there instead.
Results: OUT/result.json (default OUT: build/movie_check/<date-time>). Exit code 0 = pass.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOVIES = {"STINGS": 321, "INTRO": 2527}  # pictures (tools/pss_survey.py)
PSNR_MIN = 45.0
SHOWN = re.compile(r"\[movie\] cdrom0:\\MOVIES\\(\w+)\.PSS;1: (\d+) pictures shown, (\d+) dropped, ([\d.]+) s(.*)")
CLOCK = re.compile(r"\[movie\] [\d.]+ s: .* clock ([\d.]+), due ([\d.]+)")


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6" or parts[2] != b"255":
        raise ValueError("%s: not an 8-bit P6 picture" % path)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3][: w * h * 3]


def psnr(a, b):
    if len(a) != len(b):
        return 0.0
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    return 99.0 if se == 0 else 10 * math.log10(255.0 * 255.0 * len(a) / se)


def run_game(out, timeout):
    data = os.path.join(out, "data")
    pictures = os.path.join(out, "pictures")
    os.makedirs(data, exist_ok=True)
    os.makedirs(pictures, exist_ok=True)
    env = dict(os.environ)
    env.update({"VCS_DATA_DIR": data, "VCS_SKIP_MOVIES": "0", "VCS_MOVIE_LOG": "1", "VCS_MOVIE_DUMP": pictures})
    tc.test_window(env)
    log = os.path.join(out, "game.log")
    with open(log, "w", encoding="utf-8") as f:
        proc = subprocess.Popen([tc.VCS_EXE, tc.find_iso(None)], cwd=out, stdout=f, stderr=subprocess.STDOUT, env=env)
        t0 = time.time()
        try:
            while time.time() - t0 < timeout and proc.poll() is None:
                time.sleep(1.0)
                with open(log, encoding="utf-8", errors="replace") as r:
                    if sum(1 for m in SHOWN.finditer(r.read())) >= len(MOVIES):
                        break
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    with open(log, encoding="utf-8", errors="replace") as f:
        return f.read(), pictures


def check(out, baseline, max_dropped, timeout, make_baseline=False):
    text, pictures = run_game(out, timeout)
    problems, notes = [], []
    res = {"movies": {}, "pictures": {}}
    seen = {m.group(1): m for m in SHOWN.finditer(text)}
    for name, total in MOVIES.items():
        m = seen.get(name)
        if not m:
            problems.append("%s did not finish playing" % name)
            continue
        shown, dropped, seconds, rest = int(m.group(2)), int(m.group(3)), float(m.group(4)), m.group(5)
        res["movies"][name] = {"shown": shown, "dropped": dropped, "seconds": seconds, "note": rest.strip(", ")}
        if "skipped" in rest:
            problems.append("%s was skipped" % name)
        if shown + dropped != total:
            problems.append("%s: %d pictures shown + %d dropped, the stream has %d" % (name, shown, dropped, total))
        if dropped > max_dropped:
            problems.append("%s: %d pictures dropped (at most %d allowed)" % (name, dropped, max_dropped))
        if "no sound output" in rest:
            notes.append("%s played on the host clock (no audio device)" % name)
    for line in text.splitlines():
        if line.startswith("[movie]") and "slice errors" in line:
            problems.append(line.strip())
    lags = [float(d) - float(c) for c, d in CLOCK.findall(text)]
    res["clock_lines"] = len(lags)
    res["clock_largest_lead_s"] = round(max(lags), 3) if lags else None
    if lags and max(lags) > 0.1:
        problems.append("a picture waited %.3f s for the sound clock (more than 0.1 s)" % max(lags))

    files = sorted(f for f in os.listdir(pictures) if f.endswith(".ppm"))
    res["pictures"]["dumped"] = len(files)
    expected = sum((n + 29) // 30 for n in MOVIES.values())
    if len(files) != expected:
        problems.append("%d pictures dumped, %d expected" % (len(files), expected))
    if make_baseline:
        os.makedirs(baseline, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(pictures, f), os.path.join(baseline, f))
        notes.append("baseline written: %s" % baseline)
    elif not os.path.isdir(baseline):
        problems.append("no baseline in %s (--make-baseline writes one)" % baseline)
    else:
        worst, worst_file, compared = 99.0, None, 0
        for f in files:
            ref = os.path.join(baseline, f)
            if not os.path.isfile(ref):
                problems.append("%s: not in the baseline" % f)
                continue
            w1, h1, a = read_ppm(os.path.join(pictures, f))
            w2, h2, b = read_ppm(ref)
            p = psnr(a, b) if (w1, h1) == (w2, h2) else 0.0
            compared += 1
            if p < worst:
                worst, worst_file = p, f
            if p < PSNR_MIN:
                problems.append("%s: %.2f dB against the baseline (at least %.0f)" % (f, p, PSNR_MIN))
        res["pictures"].update({"compared": compared, "min_psnr_db": round(worst, 2), "worst": worst_file})
    res["problems"], res["notes"], res["pass"] = problems, notes, not problems
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--baseline", default=os.path.join(ROOT, "build", "regress", "baseline", "movies"))
    ap.add_argument("--make-baseline", action="store_true", help="write this run's pictures as the baseline")
    ap.add_argument("--max-dropped", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=150.0, help="seconds to wait for both movies")
    a = ap.parse_args()
    if not os.path.isfile(tc.VCS_EXE):
        tc.fail("not built yet: python tools/build.py")
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "movie_check", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    res = check(out, os.path.abspath(a.baseline), a.max_dropped, a.timeout, a.make_baseline)
    for name, m in res["movies"].items():
        print("[movie_check] %s: %d shown, %d dropped, %.1f s %s" % (name, m["shown"], m["dropped"], m["seconds"], m["note"]))
    print("[movie_check] clock: %d lines, largest wait for the sound %s s" % (res["clock_lines"], res["clock_largest_lead_s"]))
    pics = res["pictures"]
    if "compared" in pics:
        print("[movie_check] pictures: %d compared, lowest %.2f dB (%s)" % (pics["compared"], pics["min_psnr_db"], pics["worst"]))
    for n in res["notes"]:
        print("[movie_check] note: " + n)
    for p in res["problems"]:
        print("[movie_check] FAIL: " + p)
    print("[movie_check] %s (%s)" % ("PASS" if res["pass"] else "FAIL", out))
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
