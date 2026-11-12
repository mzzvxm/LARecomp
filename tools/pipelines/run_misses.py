"""What a run's pipelines cost, from the run folder: the starter's misses per segment of a scripted tour.

  python run_misses.py RUN_FOLDER...        (C:\\mcv\\larecomp\\ports\\run\\<name>; needs larecomp.log at info level)
  python run_misses.py --selftest

A miss is a pipeline the starter list (assets/mcla_pipelines.bin) did not have: the game makes it when a draw first
asks, on the render thread (the frame waits: "created") or, when the list lacked it and workers are free, in the
background (the draw is left out for a few frames: "background"). The log's scene interval lines (about every
600 frames) count both, so the sum over a run is the number of new records the run's own list file holds beyond the
starter, and the sums per tour segment say where they were needed. The segments are tour_report.py's (ZOOM_OUT =
Start until the car exists, then the gaps between the tour's `mark`s, from marks.json); a line is put in the segment
its time falls in, so a segment's count is exact to one interval line (2.5-3.5 s) and a miss may show in the
segment after the one that needed it. A run without marks.json (no tour) prints the total only.

After a run with a new starter every segment should say 0 created, 0 background and 0 draws left out; what the
last line says ("new this session") is the number of records still missing.
"""
import json
import os
import re
import sys
import tempfile
import time

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.(\d{3})\]")
SCENE = re.compile(r"scene \((interval|final)\):.*?pipelines (\d+) total \(\+(\d+) created, (\d+) failed, [\d.]+ ms; "
                   r"precache (\d+) taken, (\d+) waited [\d.]+ ms, (\d+) made here, (\d+) shared; background (\d+) keys "
                   r"pending, (\d+) draws skipped, (\d+) taken \((\d+) of keys the list lacked\)")
STARTER = re.compile(r"pipelines: (\d+) records and \d+ layouts in .*mcla_pipelines\.bin: (\d+) new here \((\d+) dropped")
PRECACHE = re.compile(r"pipelines: precache done in ([\d.]+) ms: (\d+) ready, (\d+) failed")
PRECACHING = re.compile(r"pipelines: precaching (\d+) pipelines on (\d+) threads")
WRITTEN = re.compile(r"pipelines: (\d+) records \((\d+) new this session")


def epoch(line):
    m = LINE.match(line)
    if not m:
        return None
    return time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")) + int(m.group(2)) / 1000.0


def segments(marks):
    """[(name, epoch0, epoch1)]: ZOOM_OUT, then the gaps between the tour's marks (tour_report.py's segments)."""
    pts = []
    for k, v in marks.items():
        if isinstance(v, dict) and "epoch" in v and k.startswith("tour_") and not k.startswith("tour_shot_"):
            pts.append((float(v["epoch"]), k[5:]))
    pts.sort()
    out = []
    sp = marks.get("start_pressed", {}).get("epoch")
    live = marks.get("tour_live", {}).get("epoch") or marks.get("start_ok", {}).get("epoch")
    if sp and live and float(live) > float(sp):
        out.append(("ZOOM_OUT (Start -> the car exists)", float(sp), float(live)))
    for (t0, n0), (t1, n1) in zip(pts, pts[1:]):
        if t1 > t0:
            out.append((f"{n0} -> {n1}", t0, t1))
    return out


def read_run(folder):
    log = os.path.join(folder, "larecomp.log")
    marks = {}
    try:
        with open(os.path.join(folder, "marks.json"), encoding="utf-8-sig") as f:
            marks = json.load(f)
    except (OSError, ValueError):
        pass
    facts = {"intervals": [], "starter": None, "precaching": None, "precache": None, "written": None}
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            t = epoch(line)
            if t is None:
                continue
            m = SCENE.search(line)
            if m:
                g = [int(x) for x in m.groups()[1:]]
                # created, precache taken, background draws skipped, background taken (the list lacked it)
                facts["intervals"].append((t, {"in_use": g[0], "created": g[1], "failed": g[2], "taken": g[3],
                                               "skipped": g[8], "background": g[10]}))
                continue
            for key, rx in (("starter", STARTER), ("precaching", PRECACHING), ("precache", PRECACHE), ("written", WRITTEN)):
                m = rx.search(line)
                if m:
                    facts[key] = tuple(float(x) if "." in x else int(x) for x in m.groups())
    return facts, marks


def report(folder):
    facts, marks = read_run(folder)
    iv = facts["intervals"]
    lines = [f"RUN {os.path.basename(os.path.normpath(folder))}"]
    if facts["starter"]:
        r, new, dropped = facts["starter"]
        lines.append(f"  starter: {r} records read, {new} kept here (the rest are zeroed-bias duplicates), {dropped} dropped (shader not in the pack)")
    if facts["precaching"] and facts["precache"]:
        lines.append(f"  precache: {facts['precaching'][0]} pipelines on {facts['precaching'][1]} threads, done in "
                     f"{facts['precache'][0]:.0f} ms ({facts['precache'][1]} ready, {facts['precache'][2]} failed)")
    elif facts["precaching"]:
        lines.append(f"  precache: {facts['precaching'][0]} pipelines on {facts['precaching'][1]} threads (no 'done' line)")
    segs = segments(marks)
    tot = {k: sum(d[k] for _, d in iv) for k in ("created", "background", "skipped", "taken")}
    if segs:
        lines.append(f"  {'segment':44s} {'s':>6s} {'created':>8s} {'background':>10s} {'draws left out':>14s} {'taken':>6s}")
        for name, t0, t1 in segs:
            sel = [d for t, d in iv if t0 < t <= t1]
            c, b, s, k = (sum(d[x] for d in sel) for x in ("created", "background", "skipped", "taken"))
            flag = "  <- misses" if c or b else ""
            lines.append(f"  {name[:44]:44s} {t1 - t0:6.1f} {c:8d} {b:10d} {s:14d} {k:6d}{flag}")
        first, last = segs[0][1], segs[-1][2]
        outside = [d for t, d in iv if not (first < t <= last)]
        oc, ob = sum(d["created"] for d in outside), sum(d["background"] for d in outside)
        lines.append(f"  (outside the segments: {oc} created, {ob} background)")
    lines.append(f"  total: {tot['created']} created on the render thread + {tot['background']} built in the background "
                 f"= {tot['created'] + tot['background']} misses; {tot['skipped']} draws left out; {tot['taken']} precached pipelines taken"
                 + (f"; {iv[-1][1]['in_use']} pipelines in use at the end" if iv else ""))
    if facts["written"]:
        lines.append(f"  the run's list at exit: {facts['written'][0]} records, {facts['written'][1]} new this session (= the misses, "
                     "for a run with a starter)")
    return "\n".join(lines)


def selftest():
    with tempfile.TemporaryDirectory() as d:
        t0 = time.mktime((2026, 11, 11, 19, 0, 0, 0, 0, -1))

        def stamp(s):
            return time.strftime("[%Y-%m-%d %H:%M:%S.000]", time.localtime(t0 + s))

        def scene(s, created, taken, skipped, background):
            return (f"{stamp(s)} [info] [core] [t1] [gpu] scene (interval): 600 frames | draws 1.0/frame | pipelines 500 total "
                    f"(+{created} created, 0 failed, 1.0 ms; precache {taken} taken, 0 waited 0.0 ms, 0 made here, 0 shared; "
                    f"background 0 keys pending, {skipped} draws skipped, {background} taken ({background} of keys the list "
                    f"lacked), wait mean 0.0, longest 0 frames; made here 0 new) | input layouts 3")
        lines = [f"{stamp(0)} [info] [core] [t1] [gpu] pipelines: 1743 records and 91 layouts in C:\\x\\assets\\mcla_pipelines.bin: "
                 "1167 new here (0 dropped: a shader the pack does not have; 652 static depth biases zeroed)",
                 f"{stamp(0)} [info] [core] [t1] [gpu] pipelines: precaching 1167 pipelines on 6 threads",
                 f"{stamp(3)} [info] [core] [t1] [gpu] pipelines: precache done in 2650.5 ms: 1167 ready, 0 failed",
                 scene(10, 3, 100, 0, 0), scene(20, 0, 5, 0, 0), scene(30, 31, 5, 0, 12), scene(40, 0, 5, 4, 7), scene(50, 0, 5, 0, 0),
                 f"{stamp(55)} [info] [core] [t1] [gpu] pipelines: 1211 records (44 new this session, 1167 from the other API's file)"]
        with open(os.path.join(d, "larecomp.log"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        marks = {"start_pressed": {"epoch": t0 + 5}, "tour_live": {"epoch": t0 + 15}, "tour_start": {"epoch": t0 + 15},
                 "tour_roam": {"epoch": t0 + 25}, "tour_shot_01_x": {"epoch": t0 + 26}, "tour_pause": {"epoch": t0 + 35},
                 "tour_end": {"epoch": t0 + 45}}
        with open(os.path.join(d, "marks.json"), "w", encoding="utf-8") as f:
            json.dump(marks, f)
        text = report(d)
        print(text)
        ok = True
        for want in ("1167 pipelines on 6 threads, done in 2650 ms", "ZOOM_OUT (Start -> the car exists)",
                     "roam -> pause", "34 created on the render thread", "1211 records, 44 new this session"):
            if want not in text:
                ok = False
                print("FAIL: missing", want)
        # the 30 s line (31 created, 12 background) falls in roam -> pause; the 40 s line (7 background, 4 left out) in pause -> end
        if not re.search(r"roam -> pause\s+10\.0\s+31\s+12\s+0\s", text) or not re.search(r"pause -> end\s+10\.0\s+0\s+7\s+4\s", text):
            ok = False
            print("FAIL: a segment row has the wrong counts")
        print("selftest:", "ok" if ok else "FAILED")
        return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if args == ["--selftest"]:
        sys.exit(selftest())
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if args else 2)
    for folder in args:
        try:
            print(report(folder))
        except OSError as e:
            print(f"{folder}: {e}")


if __name__ == "__main__":
    main()
