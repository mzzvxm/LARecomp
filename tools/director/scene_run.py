"""Validate the manifest and files from a Director take."""

import argparse
import csv
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SINKS = ("none", "ffmpeg", "png", "raw")
TAKE_JSON_VERSION = 1
FNV_OFFSET = 14695981039346656037


def find_ffmpeg(given=None):
    if given and Path(given).expanduser().is_file():
        return str(Path(given).expanduser().resolve())
    found = shutil.which(given or "ffmpeg")
    if given and not found:
        raise ValueError(f"FFmpeg executable not found: {given}")
    return found or "ffmpeg"


def fnv1a64(data, seed=FNV_OFFSET):
    """The FNV-1a 64 of take_folder.h Fnv1a64 (take.json's scene.fnv64 is its 16 hex digits)."""
    h = seed
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def hex64(v):
    return "%016x" % v


def find_ffprobe(ffmpeg):
    p = Path(ffmpeg)
    if p.is_absolute():
        for cand in (p.with_name("ffprobe.exe"), p.with_name("ffprobe")):
            if cand.is_file():
                return str(cand)
    return shutil.which("ffprobe")


REQUIRED = {
    "take_json_version": int, "name": str, "state": str, "error": str, "started_utc": str, "finished_utc": str, "wall_s": (int, float),
    "exe": dict, "picture": dict, "sink": dict, "frames": dict, "gate": dict, "memory": dict, "cvars": dict,
}
REQUIRED_FRAMES = ("count", "first_update", "last_update", "updates_run", "handed_over", "lost", "dropped", "repeated", "out_of_order", "skipped_updates", "exact")


def read_csv_rows(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def probe_frames(ffprobe, mkv):
    try:
        r = subprocess.run([ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", str(mkv)],
                           capture_output=True, text=True, timeout=600)
        return int(r.stdout.strip().split()[0])
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


def check_take(take, folder, check_files=True, ffprobe=None, expect_scale=None):
    """The verdicts on a take: (problems, notes). A take is good when `problems` is empty: state complete, one picture for every update, none dropped
    or repeated, the output files are what take.json says."""
    problems, notes = [], []
    for key, typ in REQUIRED.items():
        if key not in take:
            problems.append(f"take.json has no '{key}'")
        elif not isinstance(take[key], typ):
            problems.append(f"take.json '{key}' is not a {typ if not isinstance(typ, tuple) else 'number'}")
    if problems:
        return problems, notes
    fr = take["frames"]
    missing = [k for k in REQUIRED_FRAMES if k not in fr]
    if missing:
        return [f"take.json frames has no {', '.join(missing)}"], notes
    if take["take_json_version"] != TAKE_JSON_VERSION:
        problems.append(f"take.json version {take['take_json_version']}, this script reads {TAKE_JSON_VERSION}")
    if take["state"] != "complete":
        problems.append(f"the take's state is '{take['state']}'" + (f": {take['error']}" if take["error"] else ""))
    elif take["error"]:
        problems.append(f"the take is complete but has an error: {take['error']}")
    count = fr["count"]
    if count <= 0:
        problems.append("the take has no frames")
    if count != fr["updates_run"]:
        problems.append(f"{count} frames for {fr['updates_run']} updates")
    if fr["last_update"] - fr["first_update"] + 1 != count:
        problems.append(f"updates {fr['first_update']}..{fr['last_update']} are {fr['last_update'] - fr['first_update'] + 1}, not {count} frames")
    if fr["handed_over"] != fr["updates_run"]:
        problems.append(f"{fr['handed_over']} frames handed over for {fr['updates_run']} updates")
    for key in ("lost", "dropped", "repeated", "out_of_order", "skipped_updates"):
        if fr[key] != 0:
            problems.append(f"frames {key} = {fr[key]} (must be 0)")
    if not fr["exact"]:
        problems.append(f"the take says it is not exact: {fr.get('verdict', '')}")
    gate = take["gate"]
    if gate.get("timeouts", 0) != 0 or gate.get("switched_off"):
        problems.append(f"the gate timed out {gate.get('timeouts')} time(s)" + (" and was switched off" if gate.get("switched_off") else ""))
    pic = take["picture"]
    if not pic.get("width") or not pic.get("height"):
        problems.append("the picture has no size")
    elif expect_scale:
        w, h = int(round(1280 * expect_scale)), int(round(720 * expect_scale))
        if (pic["width"], pic["height"]) != (w, h):
            notes.append(f"picture {pic['width']}x{pic['height']}, a render scale of {expect_scale:g} gives {w}x{h} (an upscaler or a window size changes it)")
    mem = take["memory"]
    if mem.get("cap_bytes") and mem.get("plan_bytes", 0) > mem["cap_bytes"]:
        problems.append(f"the memory plan ({mem['plan_bytes']} bytes) is over the cap ({mem['cap_bytes']})")
    if mem.get("ring_buffers", 0) < 2:
        problems.append(f"the ring has {mem.get('ring_buffers')} buffers")
    if mem.get("ram_free_mb_min", -1) >= 0:
        notes.append(f"free RAM at the start {mem.get('ram_free_mb_start')} MB, lowest {mem.get('ram_free_mb_min')} MB, at the end {mem.get('ram_free_mb_end')} MB")
    if not take["exe"].get("commit"):
        notes.append("the exe's commit is not known (no SET-SOURCE.txt next to the exe, no LARECOMP_EXE_COMMIT)")
    sink = take["sink"]
    kind = sink.get("kind")
    if kind not in SINKS:
        problems.append(f"unknown sink '{kind}'")
    if not check_files or kind not in SINKS:
        return problems, notes
    folder = Path(folder)
    w, h = pic.get("width", 0), pic.get("height", 0)
    if kind == "ffmpeg":
        if sink.get("ffmpeg_exit") != 0:
            problems.append(f"ffmpeg exited with {sink.get('ffmpeg_exit')}")
        mkv = folder / (sink.get("output") or "video.mkv")
        if not mkv.is_file() or mkv.stat().st_size == 0:
            problems.append(f"{mkv.name} is missing or empty")
        else:
            if mkv.stat().st_size != sink.get("output_bytes"):
                problems.append(f"{mkv.name} is {mkv.stat().st_size} bytes, take.json says {sink.get('output_bytes')}")
            if ffprobe:
                n = probe_frames(ffprobe, mkv)
                if n is None:
                    notes.append("ffprobe could not count the mkv's frames")
                elif n != count:
                    problems.append(f"{mkv.name} holds {n} frames, the take {count}")
                else:
                    notes.append(f"ffprobe counts {n} frames in {mkv.name}")
            else:
                notes.append("ffprobe not found: the mkv's frame count is not checked")
    elif kind == "png":
        if sink.get("ffmpeg_exit") != 0:
            problems.append(f"ffmpeg exited with {sink.get('ffmpeg_exit')}")
        names = sorted(p.name for p in (folder / "frames").glob("*.png")) if (folder / "frames").is_dir() else []
        if len(names) != count:
            problems.append(f"{len(names)} pictures in frames/ for {count} frames")
        elif names != ["f%06d.png" % k for k in range(count)]:
            problems.append("the pictures in frames/ are not f000000.png ... in a row")
        if sink.get("output_files") != len(names):
            problems.append(f"take.json counts {sink.get('output_files')} pictures, frames/ has {len(names)}")
    elif kind == "raw":
        raw = folder / (sink.get("output") or "video.bgra")
        want = count * w * h * 4
        if not raw.is_file():
            problems.append(f"{raw.name} is missing")
        elif raw.stat().st_size != want:
            problems.append(f"{raw.name} is {raw.stat().st_size} bytes, {count} frames of {w}x{h}x4 are {want}")
    # frames.csv and state.csv: one row for every frame, the updates in a row
    fcsv = folder / "frames.csv"
    rows = []
    if not fcsv.is_file():
        problems.append("frames.csv is missing")
    else:
        rows = read_csv_rows(fcsv)
        if len(rows) != count:
            problems.append(f"frames.csv has {len(rows)} rows for {count} frames")
        updates = [int(r["update"]) for r in rows]
        if updates != list(range(fr["first_update"], fr["first_update"] + len(updates))):
            problems.append("frames.csv: the updates are not first_update, first_update + 1, ... in a row")
        if any(int(r["lost"]) for r in rows):
            problems.append("frames.csv has lost frames")
    sl = take.get("state_log")
    if sl:
        scsv = folder / sl.get("file", "state.csv")
        if not scsv.is_file():
            problems.append(f"{scsv.name} is missing")
        else:
            srows = read_csv_rows(scsv)
            if len(srows) != count:
                problems.append(f"{scsv.name} has {len(srows)} rows for {count} frames")
            elif rows and [r["update"] for r in srows] != [r["update"] for r in rows]:
                problems.append(f"{scsv.name}: the updates differ from frames.csv's")
            if sl.get("rows") != len(srows):
                problems.append(f"take.json counts {sl.get('rows')} state rows, {scsv.name} has {len(srows)}")
    scene = take.get("scene")
    if scene:
        copy = folder / (scene.get("copy") or "scene.scene")
        if not copy.is_file():
            problems.append(f"the scene's copy ({copy.name}) is missing")
        elif hex64(fnv1a64(copy.read_bytes())) != scene.get("fnv64"):
            problems.append("the scene's copy does not match the hash take.json records")
    return problems, notes


def check_json_cmd(a):
    path = Path(a.take_json)
    try:
        take = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as ex:
        print(f"{path}: cannot read: {ex}")
        return 1
    folder = Path(a.folder) if a.folder else path.parent
    problems, notes = check_take(take, folder, check_files=not a.schema_only, ffprobe=find_ffprobe(find_ffmpeg(a.ffmpeg)), expect_scale=a.scale)
    for n in notes:
        print("  note: " + n)
    for p in problems:
        print("  PROBLEM: " + p)
    fr = take.get("frames", {}) if isinstance(take, dict) else {}
    print(f"{path.name}: state {take.get('state') if isinstance(take, dict) else '?'}, {fr.get('count')} frames for {fr.get('updates_run')} updates, "
          f"dropped {fr.get('dropped')}, repeated {fr.get('repeated')}: " + ("GOOD" if not problems else f"{len(problems)} problem(s)"))
    return 0 if not problems else 1


def sample_take(kind="ffmpeg", count=12, first=100, w=64, h=36, state_log=True, scene_fnv=None):
    return {
        "take_json_version": 1, "name": "t", "state": "complete", "error": "", "started_utc": "2026-11-13T09:05:01Z", "finished_utc": "2026-11-13T09:05:41Z", "wall_s": 3.5,
        "scene": {"name": "s", "source": "s.scene", "copy": "scene.scene", "fnv64": scene_fnv or "0" * 16, "first_frame_scene_t": 0.0166667},
        "exe": {"path": "larecomp.exe", "commit": "test-commit", "version": "v"},
        "picture": {"width": w, "height": h, "readback_format": "bgra8", "fps": 60, "step_ticks": 831250, "step_ms": 16.6667},
        "sink": {"kind": kind, "output": {"ffmpeg": "video.mkv", "png": "frames/f%06d.png", "raw": "video.bgra", "none": ""}[kind], "output_bytes": 0,
                 "output_files": count if kind == "png" else 0, "ffmpeg_exit": 0 if kind in ("ffmpeg", "png") else -2, "ffmpeg_command": "ffmpeg"},
        "frames": {"count": count, "first_update": first, "last_update": first + count - 1, "updates_run": count, "handed_over": count, "lost": 0, "dropped": 0,
                   "repeated": 0, "out_of_order": 0, "skipped_updates": 0, "exact": True, "verdict": "exact", "frames_file": "frames.csv"},
        "gate": {"lag": 0, "timeouts": 0, "switched_off": False},
        "memory": {"ring_buffers": 4, "gpu_readback_buffers": 4, "plan_bytes": 1000, "cap_bytes": 2000, "note": "", "ring_max_fill": 2, "ring_stalls": 0,
                   "ram_free_mb_start": 30000.0, "ram_free_mb_min": 29000.0, "ram_free_mb_end": 30000.0},
        "state_log": {"file": "state.csv", "rows": count} if state_log else None,
        "cvars": {"mcla_capture_sink": kind},
    }


def make_folder(root, take):
    """Writes the files a take of this description leaves (for the selftest)."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    fr = take["frames"]
    count, first = fr["count"], fr["first_update"]
    w, h = take["picture"]["width"], take["picture"]["height"]
    with open(root / "frames.csv", "w", newline="") as f:
        f.write("frame,update,scene_t,t_ms,lost\n")
        for k in range(count):
            f.write(f"{k},{first + k},{0.0166667 * (k + 1):.6f},1.0,0\n")
    if take.get("state_log"):
        with open(root / "state.csv", "w", newline="") as f:
            f.write("frame,update,scene_t\n")
            for k in range(count):
                f.write(f"{k},{first + k},{0.0166667 * (k + 1):.6f}\n")
    text = b"scene text\n"
    (root / "scene.scene").write_bytes(text)
    take["scene"]["fnv64"] = hex64(fnv1a64(text))
    kind = take["sink"]["kind"]
    if kind == "png":
        (root / "frames").mkdir(exist_ok=True)
        for k in range(count):
            (root / "frames" / ("f%06d.png" % k)).write_bytes(b"x")
        take["sink"]["output_bytes"] = count
    elif kind == "raw":
        (root / "video.bgra").write_bytes(b"\0" * (count * w * h * 4))
        take["sink"]["output_bytes"] = count * w * h * 4
    elif kind == "ffmpeg":
        (root / "video.mkv").write_bytes(b"mkv" * 10)
        take["sink"]["output_bytes"] = 30
    (root / "take.json").write_text(json.dumps(take, indent=2), encoding="utf-8")


def selftest():
    checks, failed = 0, 0

    def check(ok, what):
        nonlocal checks, failed
        checks += 1
        if not ok:
            failed += 1
            print("FAILED: " + what)

    def good(kind, **kw):
        t = sample_take(kind, **kw)
        d = Path(tempfile.mkdtemp(prefix="scene_run_selftest_"))
        make_folder(d, t)
        return t, d

    def problems_of(t, d, **kw):
        return check_take(t, d, **kw)[0]

    # a good take of every sink
    for kind in SINKS:
        t, d = good(kind)
        p = problems_of(t, d)
        check(not p, f"a good {kind} take has no problems; got {p}")
        shutil.rmtree(d, ignore_errors=True)
    t, d = good("none", state_log=False)
    check(not problems_of(t, d), "a take with no state log")
    shutil.rmtree(d, ignore_errors=True)

    # every way a take is not good
    def broken(kind, mutate, want, **kw):
        t, d = good(kind, **kw)
        mutate(t, d)
        p = problems_of(t, d)
        check(any(want in x for x in p), f"[{kind}] {want!r} is reported; got {p}")
        shutil.rmtree(d, ignore_errors=True)

    broken("ffmpeg", lambda t, d: t["frames"].update(dropped=1), "frames dropped = 1")
    broken("ffmpeg", lambda t, d: t["frames"].update(repeated=2), "frames repeated = 2")
    broken("ffmpeg", lambda t, d: t["frames"].update(lost=1), "frames lost = 1")
    broken("ffmpeg", lambda t, d: t["frames"].update(count=11), "11 frames for 12 updates")
    broken("ffmpeg", lambda t, d: t["frames"].update(last_update=120), "are 21, not 12 frames")
    broken("ffmpeg", lambda t, d: t["frames"].update(handed_over=13), "13 frames handed over for 12 updates")
    broken("ffmpeg", lambda t, d: t["frames"].update(exact=False), "not exact")
    broken("ffmpeg", lambda t, d: t.update(state="failed", error="the writer is stuck"), "state is 'failed': the writer is stuck")
    broken("ffmpeg", lambda t, d: t.update(state="running"), "state is 'running'")
    broken("ffmpeg", lambda t, d: t.update(state="inexact"), "state is 'inexact'")
    broken("ffmpeg", lambda t, d: t["gate"].update(timeouts=1), "gate timed out 1")
    broken("ffmpeg", lambda t, d: t["sink"].update(ffmpeg_exit=1), "ffmpeg exited with 1")
    broken("ffmpeg", lambda t, d: (d / "video.mkv").unlink(), "video.mkv is missing")
    broken("ffmpeg", lambda t, d: t["memory"].update(plan_bytes=5000), "over the cap")
    broken("ffmpeg", lambda t, d: t["memory"].update(ring_buffers=1), "ring has 1")
    broken("ffmpeg", lambda t, d: (d / "frames.csv").unlink(), "frames.csv is missing")
    broken("ffmpeg", lambda t, d: (d / "state.csv").write_text("frame,update\n0,100\n"), "state.csv has 1 rows")
    broken("ffmpeg", lambda t, d: (d / "scene.scene").write_bytes(b"changed"), "does not match the hash")
    broken("ffmpeg", lambda t, d: (d / "frames.csv").write_text("frame,update,scene_t,t_ms,lost\n" + "".join(f"{k},{100 + k + (k > 5)},0,0,0\n" for k in range(12))), "not first_update")
    broken("png", lambda t, d: (d / "frames" / "f000005.png").unlink(), "11 pictures in frames/ for 12 frames")
    broken("png", lambda t, d: ((d / "frames" / "f000005.png").rename(d / "frames" / "g000005.png")), "are not f000000.png")
    broken("raw", lambda t, d: (d / "video.bgra").write_bytes(b"\0" * 100), "video.bgra is 100 bytes")
    p = check_take({k: v for k, v in sample_take().items() if k != "exe"}, ".", False)[0]
    check(any("no 'exe'" in x for x in p), f"a take.json without exe is refused; got {p}")
    t = sample_take()
    t["take_json_version"] = 2
    check(any("version 2" in x for x in check_take(t, ".", False)[0]), "another version is reported")
    t = sample_take()
    del t["frames"]["dropped"]
    check(any("no dropped" in x for x in check_take(t, ".", False)[0]), "a missing field of frames is reported")
    # schema only: no files are looked at
    t = sample_take("ffmpeg")
    check(not check_take(t, "no-such-folder", check_files=False)[0], "schema-only does not look for files")
    # the scale note
    t, d = good("none", w=1280, h=720)
    notes = check_take(t, d, True, None, 3.0)[1]
    check(any("gives 3840x2160" in n for n in notes), f"a picture that does not fit the render scale is a note, not a problem; got {notes}")
    shutil.rmtree(d, ignore_errors=True)

    # FNV agrees with take_folder.h (test vectors of take_folder_test.cpp)
    check(hex64(fnv1a64(b"")) == "cbf29ce484222325" and hex64(fnv1a64(b"a")) == "af63dc4c8601ec8c" and hex64(fnv1a64(b"foobar")) == "85944171f73967e8", "FNV-1a 64")
    print(f"{checks} checks, {failed} failed")
    return 0 if failed == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("selftest", help="test with synthetic take files")
    check = sub.add_parser("check-json", help="verify a take folder")
    check.add_argument("take_json")
    check.add_argument("--folder", help="take folder (default: beside take.json)")
    check.add_argument("--schema-only", action="store_true", help="check only the manifest")
    check.add_argument("--ffmpeg", help="find ffprobe next to this FFmpeg executable")
    check.add_argument("--scale", type=float, default=0.0)
    args = ap.parse_args(argv)
    try:
        return selftest() if args.command == "selftest" else check_json_cmd(args)
    except (OSError, ValueError) as e:
        print(f"scene_run: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
