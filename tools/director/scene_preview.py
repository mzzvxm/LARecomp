"""Capture stills at scene times and summarize the camera, subjects and roads."""
import argparse
import json
import math
import os
import re
import shutil
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from pathlib import Path

if "scene_tool" in sys.modules:
    ST = sys.modules["scene_tool"]
else:
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location("scene_tool", str(Path(__file__).resolve().with_name("scene_tool.py")))
    ST = _ilu.module_from_spec(_spec)
    sys.modules["scene_tool"] = ST
    _spec.loader.exec_module(ST)

ToolError = ST.ToolError

DEFAULT_THR = {"inner": 0.90, "size_min": 0.03, "size_max": 0.90, "road_max_m": 4.0, "route_max_m": 6.0, "ground_min_m": 0.3, "streaming_retry_s": 10.0}
KIND_PRIORITY = {"cut": 0, "key": 1, "precut": 2, "start": 3, "end": 4, "mid": 5, "every": 6}
SHOT_SECONDS_BUDGET = 45.0


def add_preview_args(p):
    p.add_argument("scene")
    p.add_argument("--every", type=float, default=2.0, help="a shot every N seconds inside the take (and in the middle of a longer stretch between keys); default 2")
    p.add_argument("--from", dest="t_from", type=float, default=None, help="first scene time (default: the take's start, else the first camera key)")
    p.add_argument("--to", dest="t_to", type=float, default=None, help="last scene time (default: the take's end, else `cam off` / the end)")
    p.add_argument("--max-shots", type=int, default=36, help="at most this many pictures (the extras of the time set are thinned first)")
    p.add_argument("--size", default="1280x720", help="the pictures' size (default 1280x720)")
    p.add_argument("--out", default="", help="the output folder (default: director-work beside director.json)")
    p.add_argument("--subject", default="", help="the car id the shots are about, where no camera key names one (default: the scene's only car)")
    p.add_argument("--strategy", default="auto", choices=("auto", "seek", "step", "play"),
                   help="seek = pin the clock at each time (exact for kinematic scenes); step = restart paused and step to each time (the world really runs: brain cars, traffic); "
                        "play = shot.schedule during one play-through (pictures only); auto = seek, or step for a brain car / when the game says it cannot scrub")
    p.add_argument("--road-check", default="path", choices=("path", "all", "none"), help="which cars must be on a road: path (the ones that drive a route; default), all, none")
    p.add_argument("--thr", action="append", default=[], metavar="K=V", help="a threshold: " + ", ".join(DEFAULT_THR))
    p.add_argument("--atlas", default="", help="the atlas folder (default: the current user atlas)")
    p.add_argument("--cols", type=int, default=3, help="contact sheet columns (default 3; 3 columns of a 16:9 shot are 520 px each)")
    p.add_argument("--sheet-width", type=int, default=1560, help="contact sheet width in pixels (default 1560, never above 1600)")
    p.add_argument("--no-map", action="store_true")
    p.add_argument("--no-src", action="store_true", help="the lint does not look the settings' names up in the source")
    p.add_argument("--ready-timeout-s", type=ST.positive_seconds, default=120.0, help="how long to wait for the scene's cars to be ready (a spawn takes about 4 s each)")
    p.add_argument("--strict", action="store_true", help="a check that could not be made (unknown) fails too")
    p.add_argument("--timeout-s", type=ST.positive_seconds, help="maximum wait for scheduled stills")
    ST.add_game_args(p, work_dir=True, wait=True)


# ------------------------------------------------------------------------------------------------------------------------------- the time set
def shot_times(model, every=2.0, t_from=None, t_to=None, max_shots=36, min_gap=0.3):
    """The scene times to take a picture at: [{t, kind, line, text}] in time order. Every cam key (kind key; a cut: kind cut), one update before each cut (precut), the start, the
    middle of each stretch between keys longer than `every`, every `every` seconds inside [from, to], the last update. Times are on the scene's step grid; two within `min_gap`
    seconds keep the more important one; more than max_shots drop the least important first."""
    step = model.step or 1.0 / 60.0
    r0, r1 = model.time_range()
    t_from = r0 if t_from is None else t_from
    t_to = r1 if t_to is None else t_to
    end = model.duration()
    if not 0 <= t_from < t_to <= end + 1e-6:
        raise ToolError("bad_args", f"preview range must satisfy 0 <= from < to <= {end:g}")
    first = min(end, math.ceil(t_from / step - 1e-6) * step)
    last = max(first, (math.ceil(min(end, t_to) / step - 1e-6) - 1) * step)
    if first >= t_to:
        raise ToolError("bad_args", "preview range has no scene frame at this step")
    cut_lines = {e.t: e.line for e in model.events if e.track == "cut"}
    cand = []
    for k in model.cam_keys:
        if k.off or not t_from - 1e-6 <= k.t < t_to:   # a key after the take's end (or `cam off`) is not in the picture
            continue
        cand.append((k.t, "cut" if any(abs(k.t - c) < 1e-6 for c in model.cuts) else "key", k.line, k.text))
    for c in model.cuts:
        if not t_from - 1e-6 <= c < t_to:
            continue
        if not any(abs(c - k.t) < 1e-6 for k in model.cam_keys if not k.off):
            cand.append((c, "cut", cut_lines.get(c), f"cut {c:g}"))
        if c - step >= t_from:
            cand.append((c - step, "precut", cut_lines.get(c), f"one update before the cut at {c:g}"))
    if t_from is not None:
        cand.append((t_from, "start", None, "start of the take / camera work"))
    cand.append((last, "end", None, "the last update"))
    ktimes = sorted({c[0] for c in cand if c[1] in ("key", "cut") and t_from - 1e-6 <= c[0] <= t_to + 1e-6} | {t_from, t_to})
    for a, b in zip(ktimes, ktimes[1:]):
        if b - a > every + 1e-6:
            cand.append(((a + b) / 2.0, "mid", None, f"middle of {a:g}..{b:g}"))
    if every > 0:
        # Sub-frame intervals cannot add distinct stills.
        interval = max(every, step, (t_to - t_from) / (max_shots * 8))
        t = math.ceil(t_from / interval - 1e-9) * interval
        while t < t_to - 1e-6:
            cand.append((t, "every", None, f"regular interval ({interval:g} s)"))
            t += interval
    items = []
    for t, kind, line, text in cand:
        t = max(first, min(last, round(t / step) * step))
        items.append({"t": round(t, 6), "kind": kind, "line": line, "text": text})
    items.sort(key=lambda i: (KIND_PRIORITY[i["kind"]], i["t"]))

    def clash(a, b):
        """Two shots clash when closer than min_gap, except a key, a cut and the update before a cut: those are kept whenever they are a step apart."""
        gap = abs(a["t"] - b["t"])
        both_keys = KIND_PRIORITY[a["kind"]] <= 2 and KIND_PRIORITY[b["kind"]] <= 2
        return gap < (step * 0.5 if both_keys else min_gap)

    kept = []
    for it in items:
        if not any(clash(it, k) for k in kept):
            kept.append(it)
    kept.sort(key=lambda i: i["t"])
    while len(kept) > max_shots:
        pri = max(KIND_PRIORITY[k["kind"]] for k in kept)
        pool = [i for i, k in enumerate(kept) if KIND_PRIORITY[k["kind"]] == pri]
        kept.pop(pool[len(pool) // 2] if len(pool) > 1 else pool[0])
    return kept


# ------------------------------------------------------------------------------------------------------------------------------- the checks
def evaluate_shot(shot, thr, road_check="path"):
    """(checks, notes, passed) for one shot dict: pass / fail / unknown (could not be made) / n/a (does not apply) per check; passed = nothing failed."""
    checks, notes = {}, []
    subj = shot.get("subject") or {}
    if not subj.get("id"):
        checks["subject_in_frame"] = checks["subject_size"] = checks["not_occluded"] = "n/a"
        notes.append("no subject car: name one with --subject, or a camera key that looks at / attaches to a car")
    else:
        uv, inf = subj.get("screen"), subj.get("in_frame")
        if inf is False:
            checks["subject_in_frame"] = "fail"
            notes.append(f"{subj['id']} is outside the picture" + (f" (screen {uv[0]:.2f},{uv[1]:.2f})" if uv else " (behind the camera)"))
        elif uv is None or inf is None:
            checks["subject_in_frame"] = "unknown"
        else:
            half = thr["inner"] / 2.0
            ok = abs(uv[0] - 0.5) <= half and abs(uv[1] - 0.5) <= half
            checks["subject_in_frame"] = "pass" if ok else "fail"
            if not ok:
                notes.append(f"{subj['id']} is outside the inner {format(thr['inner'] * 100, 'g')}% of the picture (screen {uv[0]:.2f},{uv[1]:.2f})")
        sz = subj.get("size_frac")
        if sz is None:
            checks["subject_size"] = "unknown"
        else:
            ok = thr["size_min"] <= sz <= thr["size_max"]
            checks["subject_size"] = "pass" if ok else "fail"
            if not ok:
                notes.append(f"{subj['id']} fills {sz * 100:.1f}% of the picture (wanted {thr['size_min'] * 100:g} to {thr['size_max'] * 100:g}%)")
        occ = subj.get("occluded")
        checks["not_occluded"] = "unknown" if occ is None else ("fail" if occ else "pass")
        if occ:
            notes.append(f"{subj['id']} is hidden behind something")
    cars = [c for c in shot.get("cars", []) if road_check == "all" or (road_check == "path" and c.get("on_path"))]
    if road_check == "none" or not cars:
        checks["car_on_road"] = "n/a"
    else:
        ds = [c.get("dist_to_road_m") for c in cars]
        if all(d is None for d in ds):
            checks["car_on_road"] = "unknown"
        else:
            bad = [c for c in cars if c.get("dist_to_road_m") is not None and c["dist_to_road_m"] > thr["road_max_m"]]
            checks["car_on_road"] = "fail" if bad else "pass"
            for c in bad:
                notes.append(f"car {c['id']} is {c['dist_to_road_m']:.1f} m from the nearest lane running its way (at most {thr['road_max_m']:g}): off the road, or on the wrong side")
    sr = shot.get("streaming_ready")
    checks["streaming_ready"] = "unknown" if sr is None else ("pass" if sr else "fail")
    if sr is False:
        notes.append("the world was still streaming when the picture was taken")
    gc = shot.get("ground_clearance_m")
    if gc is None:
        checks["camera_above_ground"] = "unknown"
    else:
        checks["camera_above_ground"] = "pass" if gc >= thr["ground_min_m"] else "fail"
        if gc < thr["ground_min_m"]:
            notes.append(f"the camera is {gc:.2f} m above the ground (at least {thr['ground_min_m']:g})")
    return checks, notes, not any(v == "fail" for v in checks.values())


def _first(d, *names, default=None):
    for n in names:
        if isinstance(d, dict) and n in d and d[n] is not None:
            return d[n]
    return default


def norm_camera(res):
    """The camera of a camera.get result: {pos, yaw, pitch, roll, fov, aspect, mode} (the pose may sit under `pose`)."""
    pose = _first(res, "pose", default=res) or {}
    return {"pos": _first(pose, "pos", "position"), "yaw": _first(pose, "yaw"), "pitch": _first(pose, "pitch"), "roll": _first(pose, "roll", default=0.0), "fov": _first(pose, "fov"),
            "aspect": _first(res, "aspect"), "mode": _first(res, "mode")}


def norm_cars(res):
    lst = res if isinstance(res, list) else _first(res, "cars", default=[])
    out = []
    for c in lst or []:
        out.append({"id": c.get("id"), "model": c.get("model"), "pos": _first(c, "pos", "position"), "heading": c.get("heading"), "speed": _first(c, "speed", default=0.0),
                    "ready": c.get("ready", True), "on_path": bool(c.get("on_path"))})
    return out


def norm_subject(res, subject_id):
    if not isinstance(res, dict):
        return {"id": subject_id}
    scr = _first(res, "screen", "uv")
    return {"id": subject_id, "screen": list(scr) if scr else None, "in_frame": _first(res, "in_frame", "visible"), "size_frac": _first(res, "size_frac", "size"),
            "dist_m": _first(res, "dist_m", "distance"), "occluded": res.get("occluded")}


# ------------------------------------------------------------------------------------------------------------------------------- gathering the shots from the game
def parse_size(text):
    m = re.fullmatch(r"(\d{2,5})x(\d{2,5})", text.strip().lower())
    if not m:
        raise ToolError("bad_args", f"--size {text}: WIDTHxHEIGHT (1280x720)")
    return int(m.group(1)), int(m.group(2))


def resolve_shot_file(res, rel, root):
    """The file a `shot` made: the result's absolute path, else beside the game's root (the Director output folder)."""
    cands = []
    p = _first(res, "path", "file")
    if p:
        cands.append(Path(p))
        cands.append(Path(root) / p)
    cands.append(Path(root) / rel)
    for c in cands:
        if c.is_file():
            return c
    raise ToolError("failed", f"the game says it took '{rel}' but no such file is at {', '.join(str(c) for c in cands[:3])}")


class NeedStep(Exception):
    """The game says the clock cannot be pinned exactly (a brain car, traffic): the shots are taken by running the scene and stepping instead."""


class Gather:
    """Takes the pictures and asks the game what it knows at each shot. Three ways to be at a scene time: seek (pin the clock: exact for a scene made of pure functions of the
    time), step (restart paused and `transport.step` from shot to shot: the world really runs, so brain cars and traffic are what the take will see), schedule (shot.schedule during
    one play-through, when step is refused: pictures only, the per-shot data are unknown)."""

    def __init__(self, ch, model, atlas, thr, opts, root, shots_dir, size, label):
        self.ch, self.model, self.atlas, self.thr, self.opts = ch, model, atlas, thr, opts
        self.root, self.shots_dir, self.size = Path(root), Path(shots_dir), size
        self.rel_dir = f"director-preview/{label}"
        self.picture = None

    def road_dist(self, x, z, heading=None):
        if self.atlas is not None and self.atlas.rails:
            return ST.atlas_road_dist(self.atlas, x, z, 150.0, heading)
        args = {"pos": [x, z], "r": 150.0, "n": 1}
        if heading is not None:
            args["heading"] = heading
        r = self.ch.request("query.road", args, timeout=15.0)
        if r.ok and isinstance(r.result, dict):
            near = r.result.get("nearest") or []
            return float(near[0]["dist_m"]) if near else 150.0
        return None

    def ready_wait(self, timeout):
        """Waits until every spawned car says ready and streaming is done. -> a note or ''."""
        deadline = time.time() + timeout
        cam_until = time.time() + 20.0   # the map's zoom is over within seconds: a camera that stays up is not waited for longer
        while True:
            cars = self.ch.request("cars.list", None, timeout=15.0)
            lst = norm_cars(cars.result) if cars.ok else []
            st = self.ch.request("query.streaming", None, timeout=15.0)
            streaming = bool(_first(st.result, "done", "ready", default=True)) if st.ok else True
            # The game's own camera is the free-roam entry's zoom down from the map (9000 m up) for the first seconds of a world: a still of "t = 0" taken then is the map
            cam = self.ch.request("camera.get", None, timeout=15.0)
            res = cam.result if cam.ok and isinstance(cam.result, dict) else {}
            pos = (res.get("pose") or {}).get("pos") or [0.0, 0.0, 0.0]
            aloft = res.get("mode") not in (None, "scene") and len(pos) == 3 and pos[1] > 1500.0 and time.time() < cam_until
            if all(c["ready"] for c in lst) and streaming and not aloft:
                return ""
            if time.time() > deadline:
                return "not every car / the world was ready after %gs: %s" % (timeout, ", ".join(c["id"] for c in lst if not c["ready"]) or ("the game's camera was still up on the map's zoom" if aloft else "streaming"))
            time.sleep(0.5)

    def _streaming(self, t0):
        """(ready, retried): asks query.streaming, waiting up to streaming_retry_s from t0 for it to be done."""
        retried = False
        while True:
            st = self.ch.request("query.streaming", None, timeout=20.0)
            ready = bool(_first(st.result, "done", "ready", default=True)) if st.ok else None
            if ready is not False or time.time() - t0 >= self.thr["streaming_retry_s"]:
                return ready, retried
            time.sleep(0.5)
            retried = True

    def _seek(self, t, t0):
        """Pins the clock at t (waiting out `not_ready` for a few seconds). -> (scrubbable, retried)."""
        retried = False
        while True:
            r = self.ch.request("transport.seek", {"t": t, "settle_frames": 20}, timeout=60.0, wait="settled", timeout_s=30.0)
            if not r.ok and r.code == "not_ready" and time.time() - t0 < self.thr["streaming_retry_s"]:
                time.sleep(0.5)
                retried = True
                continue
            if not r.ok:
                raise ToolError(r.code or "failed", f"transport.seek {t:g}: {r.msg}", exit_code=ST.EXIT_ENV)
            return bool((r.result or {}).get("scrubbable", True)), retried

    def observe(self, idx, it, subject_id, t0, retried=False):
        """The game's answers at the current (pinned) time, then the picture."""
        ch, thr = self.ch, self.thr
        t = it["t"]
        ready, r2 = self._streaming(t0)
        retried = retried or r2
        cam = norm_camera(ch.call("camera.get", None, timeout=20.0))
        cars = norm_cars(ch.call("cars.list", None, timeout=20.0))
        subject = {"id": ""}
        if subject_id:
            vr = ch.request("query.visible", {"subject": f"car:{subject_id}"}, timeout=20.0)
            subject = norm_subject(vr.result if vr.ok else None, subject_id)
        clearance = None
        if cam["pos"]:
            gr = ch.request("query.ground", {"x": cam["pos"][0], "z": cam["pos"][2]}, timeout=20.0)
            if gr.ok and isinstance(gr.result, dict) and gr.result.get("ok", True) is not False:
                gy = _first(gr.result, "y", "height", "ground")
                if gy is not None:
                    clearance = round(cam["pos"][1] - float(gy), 3)
        for c in cars:
            if c["pos"]:
                d = self.road_dist(c["pos"][0], c["pos"][2], c.get("heading"))
                c["dist_to_road_m"] = d
                c["on_road"] = None if d is None else d <= thr["road_max_m"]
        rel = f"{self.rel_dir}/shots/s{idx:02d}-t{t:08.3f}-{it['kind']}.png"
        sh = ch.call("shot", {"path": rel, "size": list(self.size)}, timeout=90.0, wait="settled", timeout_s=60.0)
        src = resolve_shot_file(sh, rel, self.root)
        dest = self.shots_dir / Path(rel).name
        self.shots_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        if sh.get("width") and not self.picture:
            self.picture = [int(sh["width"]), int(sh["height"])]
        notes = []
        if ready is False:
            notes.append("the world was not fully streamed after %gs: the picture may lack distant things" % thr["streaming_retry_s"])
        elif retried:
            notes.append("waited for the streaming before the picture (retried)")
        return {"t": t, "kind": it["kind"], "line": it["line"], "text": it["text"], "file": f"shots/{dest.name}", "camera": cam, "subject": subject, "cars": cars,
                "streaming_ready": ready, "ground_clearance_m": clearance, "notes": notes}

    def seek_all(self, times, subject_default, allow_step=True):
        shots = []
        for i, it in enumerate(times, 1):
            t0 = time.time()
            scrub, retried = self._seek(it["t"], t0)
            if not scrub and allow_step:
                raise NeedStep()
            sid = self.model.subject_at(it["t"]) or subject_default
            shots.append(dict(self.observe(i, it, sid, t0, retried), n=i, scrubbable=scrub))
        return shots

    def step_all(self, times, subject_default):
        ch = self.ch
        r = ch.call("transport.restart", {"t": 0}, timeout=150.0, wait="settled", timeout_s=120.0)
        ch.call("transport.pause", None, timeout=30.0)
        step = self.model.step or 1.0 / 60.0
        cur = float((r or {}).get("t", 0.0) or 0.0)
        shots = []
        for i, it in enumerate(times, 1):
            t0 = time.time()
            frames = int(round((it["t"] - cur) / step))
            while frames > 0:
                n = min(frames, 600)
                res = ch.call("transport.step", {"frames": n}, timeout=150.0, wait="settled", timeout_s=120.0)
                frames -= n
                cur = float(res.get("t", cur + n * step)) if isinstance(res, dict) else cur + n * step
            sid = self.model.subject_at(it["t"]) or subject_default
            shots.append(dict(self.observe(i, it, sid, t0), n=i, scrubbable=False))
        return shots

    def schedule_all(self, times, subject_default, timeout):
        """shot.schedule during one play-through: the pictures at the exact scene times, nothing else known about them."""
        ch = self.ch
        ch.drain()
        # Restart releases the clock. Pin zero before sending the schedule, which releases it
        # again. Waiting for restart to settle would let the clock pass the first shot.
        # CHECK: Confirm a scheduled still at t=0 in a live play-through.
        ch.call("transport.restart", {"t": 0}, timeout=30.0)
        ch.call("transport.seek", {"t": 0}, timeout=30.0)
        rel_dir = f"{self.rel_dir}/shots"
        ch.call("shot.schedule", {"times": [i["t"] for i in times], "dir": rel_dir}, timeout=30.0)
        got, deadline = {}, time.time() + timeout
        while len(got) < len(times) and time.time() < deadline:
            ev = ch.wait_event("shot.done", timeout=min(5.0, max(0.1, deadline - time.time())))
            if ev is not None and ev.get("ok", True):
                got[round(float(ev.get("t", -1)), 3)] = ev
        shots = []
        for i, it in enumerate(times, 1):
            ev = got.get(round(it["t"], 3))
            base = {"n": i, "t": it["t"], "kind": it["kind"], "line": it["line"], "text": it["text"], "camera": {}, "subject": {"id": ""}, "cars": [], "streaming_ready": None,
                    "ground_clearance_m": None, "scrubbable": False}
            if ev is None:
                shots.append(dict(base, file=None, notes=["the game never reported this shot (shot.done)"]))
                continue
            rel = f"{rel_dir}/{Path(str(ev.get('path', ''))).name}"
            src = resolve_shot_file(ev, rel, self.root)
            dest = self.shots_dir / f"s{i:02d}-t{it['t']:08.3f}-{it['kind']}.png"
            self.shots_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            shots.append(dict(base, file=f"shots/{dest.name}", notes=["a play-through picture: nothing else is known about this moment, so its checks are unknown"],
                              subject={"id": self.model.subject_at(it["t"]) or subject_default}))
        return shots


def route_checks(model, atlas, thr):
    """[{id, route, mode, points, max_dist_to_road_m, pass}] for every route a car drives: within route_max_m of a road along the whole route (needs the atlas's rails)."""
    out = []
    for cid, name, poly, speeds, mode, line in model.routes():
        item = {"id": cid, "route": name, "mode": mode, "line": line, "points": len(poly), "max_dist_to_road_m": None, "pass": None}
        if atlas is not None and atlas.rails:
            ds = []
            for i, (x, z) in enumerate(poly):   # a lane that runs the way the route runs at this point
                a_, b_ = poly[max(0, i - 1)], poly[min(len(poly) - 1, i + 1)]
                ds.append(ST.atlas_road_dist(atlas, x, z, 150.0, ST.heading_of(b_[0] - a_[0], b_[1] - a_[1])))
            item["max_dist_to_road_m"] = round(max(ds), 2)
            item["pass"] = max(ds) <= thr["route_max_m"]
        out.append(item)
    return out


def parse_thr(items):
    thr = dict(DEFAULT_THR)
    for kv in items or []:
        k, _, v = kv.partition("=")
        if k not in thr or ST.num_or_none(v) is None:
            raise ToolError("bad_args", f"--thr {kv}: key=number with key one of {', '.join(thr)}")
        thr[k] = float(v)
    return thr


def build_result(path, model, lint, game, shots, routes, warnings, thr, a, strategy, out_dir, files, size):
    checked, failed, unknown = 0, [], 0
    for s in shots:
        s["checks"], notes, s["pass"] = evaluate_shot(s, thr, a.road_check)
        s["notes"] = list(s.get("notes", [])) + notes
        s.pop("scrubbable", None)
        for name, v in s["checks"].items():
            if v == "fail":
                failed.append(f"#{s['n']} t={s['t']:.2f} {name}")
            elif v == "unknown":
                unknown += 1
    for r in routes:
        if r["pass"] is False:
            failed.append(f"route {r['route']} ({r['id']}) is {r['max_dist_to_road_m']:.1f} m from the road at worst (at most {thr['route_max_m']:g})")
    if a.strict and unknown:
        failed.append(f"{unknown} check(s) could not be made (--strict)")
    r0, r1 = model.time_range()
    n_fail = sum(1 for s in shots if not s["pass"])
    summary = (f"{len(shots)} shots, {n_fail} with a failed check, {unknown} check(s) unknown; " + ("PASS" if not failed else "FAIL: " + "; ".join(failed[:6]) + ("; ..." if len(failed) > 6 else "")))
    return {"schema": 1, "tool": "scene_tool.py preview", "scene": {"path": str(path), "name": model.name, "hash": ST.sha1_of(model.text), "end": model.end, "fps": model.fps,
            "step": model.step, "range": [r0, r1], "lint": {"errors": lint["errors"], "warnings": lint["warnings"]}}, "game": game, "size": list(size), "every": a.every,
            "strategy": strategy, "thresholds": thr, "shots": shots, "routes": routes, "warnings": warnings, "files": files, "pass": not failed, "failed": failed, "summary": summary,
            "out": str(out_dir)}


def preview_text(res):
    lines, unknown = [], {}
    for s in res["shots"]:
        bad = [k for k, v in s["checks"].items() if v == "fail"]
        for k, v in s["checks"].items():
            if v == "unknown":
                unknown[k] = unknown.get(k, 0) + 1
        lines.append(f"  #{s['n']:<2} t={s['t']:7.2f}  {s['kind']:<6} {('L' + str(s['line'])) if s['line'] else '    ':<5} " + ("FAIL " + ", ".join(bad) if bad else "ok"))
        lines += [f"        {n}" for n in s["notes"] if bad or "streamed" in n]
    for r in res["routes"]:
        if r["pass"] is not None:
            lines.append(f"  route {r['route']} ({r['id']}): at worst {r['max_dist_to_road_m']:.1f} m from a road: {'ok' if r['pass'] else 'FAIL'}")
    if unknown:
        lines.append("  could not be checked: " + ", ".join(f"{k} ({n} of {len(res['shots'])} shots)" for k, n in unknown.items()))
    lines += [f"  warning: {w}" for w in res["warnings"]]
    lines.append(res["summary"])
    lines.append("read: " + ", ".join(res["files"].get("contact", []) + ([res["files"]["map"]] if res["files"].get("map") else []) + [str(Path(res["out"]) / "preview.json")]))
    return lines


# ------------------------------------------------------------------------------------------------------------------------------- the command
@contextmanager
def preview_scene(ch, staged, prior_transport, warnings):
    try:
        before = ch.call("scene.get")
    except ST.ToolError as e:
        # A game that has no scene loaded yet (a fresh start) answers not_ready: there is nothing to give back.
        if e.code != "not_ready":
            raise
        before = {}
    was_scene = before.get("path")
    try:
        # CHECK: confirm a staged scene, one still and restoration in a local game.
        ch.call("scene.load", {"path": str(staged)}, timeout=200.0, wait="settled", timeout_s=180.0)
        yield
    finally:
        def restore(cmd, args=None, settled=False):
            try:
                ch.call(cmd, args, timeout=130.0 if settled else 30.0,
                        wait="settled" if settled else None, timeout_s=120.0 if settled else None)
            except ToolError as e:
                warnings.append(f"could not restore the previous game state ({e.msg})")
                return False
            return True

        fixed = prior_transport.get("mode") == "fixed"
        step = prior_transport.get("step") or 0
        restore("transport.mode", {"fixed": fixed, **({"fps": 1 / step} if fixed and step else {})})
        if was_scene:
            if restore("scene.load", {"path": was_scene}, settled=True) and prior_transport.get("paused"):
                restore("transport.pause")
        else:
            restore("scene.unload")


def run_preview(a):
    path = ST.resolve_scene(a.scene)
    text = path.read_text(encoding="utf-8")
    lint = ST.lint_one(path, getattr(a, "src", None), None, a.no_src, text=text)
    linted = lint.pop("_scene")
    if lint["errors"]:
        d = next(x for x in lint["diagnostics"] if x["severity"] == "error")
        raise ToolError("failed", f"{path.name}:{d['line']}: {d['message']} ({lint['errors']} error(s): scene_tool.py lint {path.name})", line=d["line"], exit_code=ST.EXIT_FINDINGS)
    size = parse_size(a.size)
    thr = parse_thr(a.thr)
    if a.max_shots < 1 or not math.isfinite(a.every) or a.every < 0:
        raise ToolError("bad_args", "--max-shots must be positive and --every must be finite and nonnegative")
    if a.cols < 1 or a.sheet_width > 1600 or a.sheet_width < 400:
        raise ToolError("bad_args", "--cols 1 or more, --sheet-width 400 to 1600 (a picture a model reads)")
    model = ST.SceneModel(path, text)
    times = shot_times(model, a.every, a.t_from, a.t_to, a.max_shots)
    if not times:
        raise ToolError("bad_args", "the time set is empty (the scene has no length? --from / --to)")
    atlas = ST.find_atlas(a.atlas or None, need_rails=True)
    warnings = []
    if atlas is None:
        warnings.append("no atlas with roads: the map has no roads and the car-on-road check is unknown unless the game answers query.road (python atlas.py build --game <folder>)")
    if model.brain_cars():
        warnings.append(f"brain car(s) {', '.join(model.brain_cars())}: the game's AI drives them, so the take will not repeat these shots exactly")
    if model.end is None:
        warnings.append("the scene has no `end`")
    with ST.Session(a) as s:
        ch = s.ch
        st = ST.wait_world(ch, a.world_timeout_s)
        if ch.call("take.status").get("active"):
            raise ToolError("not_ready", "a take is already running")
        staged, hit = s.stage(path, "preview", text, linted)
        out_dir = (Path(a.out).expanduser().resolve() if a.out
                   else s.root() / f"{staged.parent.name}-results")
        out_dir.mkdir(parents=True, exist_ok=True)
        if hit:
            warnings.append(f"capture statements on lines {', '.join(map(str, hit))} are commented out in the staged scene")
        with preview_scene(ch, staged, st, warnings):
            fps = model.fps or (1.0 / model.step if model.step else 60.0)
            mr = ch.request("transport.mode", {"fixed": True, "fps": fps}, timeout=30.0)
            if not mr.ok:
                warnings.append(f"transport.mode fixed step was refused ({mr.code}: {mr.msg}): the pictures are not at the take's step")
            g = Gather(ch, model, atlas, thr, a, s.root(), out_dir / "shots", size, staged.parent.name)
            note = g.ready_wait(a.ready_timeout_s)
            if note:
                warnings.append(note)
            scene_get = ch.request("scene.get", None, timeout=30.0)
            gd = scene_get.result if scene_get.ok and isinstance(scene_get.result, dict) else {}
            for d in (gd.get("diagnostics") or []):
                if isinstance(d, dict) and d.get("severity", "error") == "error":
                    warnings.append(f"the game's parser: line {d.get('line')}: {d.get('message', d.get('msg'))}")
            strategy = a.strategy
            if strategy == "auto":
                strategy = "step" if model.brain_cars() else "seek"
            shots = None
            if strategy == "seek":
                try:
                    shots = g.seek_all(times, a.subject, allow_step=(a.strategy == "auto"))
                except NeedStep:
                    warnings.append("the game says the scene cannot be scrubbed exactly (traffic, peds or a brain car): the shots are taken by running the scene and stepping instead")
                    strategy = "step"
            if strategy == "step":
                try:
                    shots = g.step_all(times, a.subject)
                except ToolError as e:
                    if e.code not in ("unknown_cmd", "bad_args", "refused", "failed"):
                        raise
                    warnings.append(f"transport.step is not available ({e.msg}): pictures from one play-through (shot.schedule), the per-shot checks are unknown")
                    strategy = "play"
            if strategy == "play":
                shots = g.schedule_all(times, a.subject, a.timeout_s or (SHOT_SECONDS_BUDGET * len(times) + 120.0))
            game = {"info": s.info["_path"], "pid": s.info.get("pid"), "version": s.info.get("version"), "exe": s.info.get("exe"),
                    "world": st, "scene_rev": gd.get("rev"), "scene_hash": gd.get("hash")}
            picture = g.picture
    routes = route_checks(model, atlas, thr)
    res = build_result(path, model, lint, game, shots, routes, warnings, thr, a, strategy, out_dir, {}, picture or size)
    res["atlas"] = ST.atlas_id(atlas) if atlas is not None else None
    return finish_preview(a, res, model, atlas, out_dir)


def finish_preview(a, res, model, atlas, out_dir):
    """The pictures (Pillow: the contact sheet and the map), preview.json, and the printed result."""
    files = res["files"]
    try:
        files["contact"] = [str(p) for p in draw_contact(res, out_dir, a.cols, a.sheet_width)]
        if not a.no_map:
            files["map"] = str(draw_map(res, model, atlas, out_dir / "map.png"))
    except ImportError:
        res["warnings"].append("Pillow is not installed for this python: no contact sheet and no map (run with the plain `python`, not `python -I`); preview.json and shots\\ are there")
    (out_dir / "preview.json").write_text(json.dumps(res, indent=2, ensure_ascii=False), encoding="utf-8")
    lines = preview_text(res)
    code = ST.EXIT_OK if res["pass"] else ST.EXIT_FINDINGS
    return ST.emit(a, res if a.json else {"ok": res["pass"]}, lines, code)


# ------------------------------------------------------------------------------------------------------------------------------- drawing: the contact sheet
COL_OK, COL_FAIL, COL_UNK, COL_NA = (70, 200, 110), (235, 70, 70), (214, 176, 60), (120, 120, 130)


def _fit(draw, text, font, width):
    if draw.textlength(text, font=font) <= width:
        return text
    while text and draw.textlength(text + "...", font=font) > width:
        text = text[:-1]
    return text + "..."


def shot_color(s):
    vals = list((s.get("checks") or {}).values())
    if "fail" in vals:
        return COL_FAIL
    if "unknown" in vals:
        return COL_UNK
    return COL_OK


def draw_contact(res, out_dir, cols=3, sheet_w=1560, rows_per_page=4):
    """The contact sheet(s): the pictures in a grid <= 1600 px wide, numbered like the map, each with its time, kind, key line and verdict. -> [paths]; contact.png, contact-2.png ..."""
    from PIL import Image, ImageDraw
    sheet_w = min(1600, sheet_w)
    shots = [s for s in res["shots"] if s.get("file")]
    out_dir = Path(out_dir)
    if not shots:
        return []
    gap, margin, head_h, fsz = 6, 6, 26, 13
    cell_w = (sheet_w - 2 * margin - (cols - 1) * gap) // cols
    w0, h0 = res["size"]
    cell_h = max(40, int(cell_w * h0 / w0))
    font, big = ST.scene_font(fsz), ST.scene_font(15)
    cap_h = 3 * (fsz + 3) + 6
    per_page = cols * rows_per_page
    pages = [shots[i:i + per_page] for i in range(0, len(shots), per_page)]
    paths = []
    for pn, page in enumerate(pages, 1):
        rows = (len(page) + cols - 1) // cols
        im = Image.new("RGB", (sheet_w, head_h + margin + rows * (cell_h + cap_h + gap)), (22, 24, 28))
        dr = ImageDraw.Draw(im)
        verdict = "PASS" if res["pass"] else "FAIL"
        head = f"{res['scene']['name']}   page {pn}/{len(pages)}   {w0}x{h0} shots   {verdict}   (x = failed check, ? = could not be checked; numbers match map.png)"
        dr.text((margin, 4), _fit(dr, head, big, sheet_w - 2 * margin), font=big, fill=COL_OK if res["pass"] else COL_FAIL)
        for k, s in enumerate(page):
            r, c = divmod(k, cols)
            x, y = margin + c * (cell_w + gap), head_h + r * (cell_h + cap_h + gap)
            try:
                pic = Image.open(out_dir / s["file"]).convert("RGB").resize((cell_w, cell_h), Image.LANCZOS)
            except OSError:
                pic = Image.new("RGB", (cell_w, cell_h), (60, 30, 30))
            subj = s.get("subject") or {}
            if subj.get("screen") and subj.get("in_frame"):
                u, v = subj["screen"]
                px, py = int(u * cell_w), int(v * cell_h)
                pd = ImageDraw.Draw(pic)
                pd.ellipse([px - 9, py - 9, px + 9, py + 9], outline=(255, 235, 60), width=2)
                pd.line([px - 14, py, px - 9, py], fill=(255, 235, 60), width=2)
                pd.line([px + 9, py, px + 14, py], fill=(255, 235, 60), width=2)
            im.paste(pic, (x, y))
            col = shot_color(s)
            dr.rectangle([x - 1, y - 1, x + cell_w, y + cell_h], outline=col, width=3)
            marks = "".join("x" if v == "fail" else "?" for v in (s.get("checks") or {}).values() if v in ("fail", "unknown"))
            line1 = f"#{s['n']}  t={s['t']:.2f}s  {s['kind'].upper()}" + (f"  L{s['line']}" if s.get("line") else "") + (f"  fov {s['camera']['fov']:g}" if s.get("camera", {}).get("fov") else "")
            bad = [n for n, v in (s.get("checks") or {}).items() if v == "fail"]
            unk = [n for n, v in (s.get("checks") or {}).items() if v == "unknown"]
            line3 = ("x " + ", ".join(bad)) if bad else ("ok" + (("  ? " + ", ".join(unk)) if unk else ""))
            cy = y + cell_h + 3
            dr.rectangle([x, y + cell_h, x + cell_w - 1, y + cell_h + cap_h - 2], fill=(34, 36, 42))
            dr.text((x + 4, cy), _fit(dr, line1 + ("  " + marks if marks else ""), font, cell_w - 8), font=font, fill=(235, 235, 235))
            dr.text((x + 4, cy + fsz + 3), _fit(dr, s.get("text") or "", font, cell_w - 8), font=font, fill=(160, 170, 190))
            dr.text((x + 4, cy + 2 * (fsz + 3)), _fit(dr, line3, font, cell_w - 8), font=font, fill=col)
        p = out_dir / ("contact.png" if pn == 1 else f"contact-{pn}.png")
        im.save(p)
        paths.append(p)
    return paths


# ------------------------------------------------------------------------------------------------------------------------------- drawing: the map
def _nice_step(span, target=8):
    raw = span / target
    for s in (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000):
        if s >= raw:
            return s
    return 10000


def speed_color(v, lo, hi):
    u = 0.5 if hi <= lo else max(0.0, min(1.0, (v - lo) / (hi - lo)))
    stops = [(0.0, (60, 120, 255)), (0.5, (60, 220, 190)), (1.0, (255, 90, 60))]
    for (u0, c0), (u1, c1) in zip(stops, stops[1:]):
        if u <= u1:
            f = (u - u0) / (u1 - u0)
            return tuple(int(c0[i] + (c1[i] - c0[i]) * f) for i in range(3))
    return stops[-1][1]


def draw_map(res, model, atlas, out_png, width=1200):
    """The top-down map: roads (atlas), the cars' routes coloured by speed with arrows, spawn points, the camera path with a view wedge at every key and cut. x east is right,
    z south is down. The camera poses are the game's (shots) where it gave them, else the keys' estimate."""
    from PIL import Image, ImageDraw
    shots = res["shots"]
    routes = model.routes()
    pts = []
    for _, _, poly, _, _, _ in routes:
        pts += poly
    spawns = {}
    for cid, c in model.cars.items():
        b = model.car_base(cid)
        if b and cid != "player":
            spawns[cid] = b
            pts.append((b[0], b[2]))
    cams = []
    for s in shots:
        cam = s.get("camera") or {}
        pos = cam.get("pos")
        est = False
        if not pos:
            e = model.est_camera(s["t"])
            if e:
                cam, pos, est = e, e["pos"], True
        if pos and abs(pos[1]) <= 1500.0:   # a camera 9 km up is the map's zoom, not a place in the city: not drawn, and it must not stretch the picture
            cams.append({"n": s["n"], "t": s["t"], "kind": s["kind"], "x": pos[0], "z": pos[2], "yaw": cam.get("yaw") or 0.0, "fov": cam.get("fov") or 40.0, "est": est})
            pts.append((pos[0], pos[2]))
        for c in s.get("cars") or []:
            if c.get("pos"):
                pts.append((c["pos"][0], c["pos"][2]))
    if not pts:
        im = Image.new("RGB", (640, 120), (22, 24, 28))
        ImageDraw.Draw(im).text((12, 40), "nothing to draw: no routes, spawns or camera positions", font=ST.scene_font(15), fill=(220, 220, 220))
        im.save(out_png)
        return out_png
    xs, zs = [p[0] for p in pts], [p[1] for p in pts]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    span = max(x1 - x0, z1 - z0, 40.0)
    pad = max(20.0, span * 0.10)
    x0, x1, z0, z1 = x0 - pad, x1 + pad, z0 - pad, z1 + pad
    dx, dz = x1 - x0, z1 - z0
    W = width
    H = int(min(width, max(520, width * dz / dx)))
    m = 16
    sc = min((W - 2 * m) / dx, (H - 2 * m) / dz)
    ox, oy = (W - dx * sc) / 2 - x0 * sc, (H - dz * sc) / 2 - z0 * sc

    def P(x, z):
        return ox + x * sc, oy + z * sc

    im = Image.new("RGBA", (W, H), (26, 28, 33, 255))
    dr = ImageDraw.Draw(im)
    font, small = ST.scene_font(14), ST.scene_font(12)
    step = _nice_step(max(dx, dz))
    gx = math.ceil(x0 / step) * step
    while gx < x1:
        X, _ = P(gx, 0)
        dr.line([X, 0, X, H], fill=(40, 43, 50))
        dr.text((X + 3, 26), f"{gx:g}", font=small, fill=(110, 116, 130))
        gx += step
    gz = math.ceil(z0 / step) * step
    while gz < z1:
        _, Y = P(0, gz)
        dr.line([0, Y, W, Y], fill=(40, 43, 50))
        dr.text((3, Y + 2), f"{gz:g}", font=small, fill=(110, 116, 130))
        gz += step
    if atlas is not None and atlas.rails:
        for rail in atlas.rails:
            rp = rail["pts"]
            if max(p_[0] for p_ in rp) < x0 or min(p_[0] for p_ in rp) > x1 or max(p_[2] for p_ in rp) < z0 or min(p_[2] for p_ in rp) > z1:
                continue
            dr.line([P(p_[0], p_[2]) for p_ in rp], fill=(78, 84, 96), width=2)
    allv = [v for _, _, _, sp, _, _ in routes for v in sp]
    lo, hi = (min(allv), max(allv)) if allv else (0.0, 1.0)
    for cid, name, poly, sp, mode, line in routes:
        for i in range(1, len(poly)):
            dr.line([P(*poly[i - 1]), P(*poly[i])], fill=speed_color((sp[i - 1] + sp[i]) / 2, lo, hi), width=5)
        acc = 0.0
        for i in range(1, len(poly)):
            (ax, ay), (bx, by) = P(*poly[i - 1]), P(*poly[i])
            seg = math.hypot(bx - ax, by - ay)
            acc += seg
            if acc >= 70 and seg > 0:
                acc = 0.0
                ux, uy = (bx - ax) / seg, (by - ay) / seg
                tip = (bx, by)
                dr.polygon([tip, (bx - ux * 10 - uy * 5, by - uy * 10 + ux * 5), (bx - ux * 10 + uy * 5, by - uy * 10 - ux * 5)], fill=(245, 245, 245))
        sx, sy = P(*poly[0])
        dr.text((sx + 7, sy - 18), f"{name} ({cid}{', brain' if mode == 'brain' else ''})", font=small, fill=(210, 214, 224))
    for cid, (x, y, z, h) in spawns.items():
        X, Y = P(x, z)
        dr.ellipse([X - 6, Y - 6, X + 6, Y + 6], fill=(240, 240, 240), outline=(20, 20, 20))
        fx, fz = ST.heading_forward(h)
        dr.line([X, Y, X + fx * 16, Y + fz * 16], fill=(240, 240, 240), width=2)
        c = model.cars[cid]
        dr.text((X + 8, Y + 4), f"{cid} {c.model[:18]}", font=small, fill=(240, 240, 240))
    cuts = sorted(model.cuts)
    cams.sort(key=lambda c: c["t"])
    for a_, b_ in zip(cams, cams[1:]):
        if any(a_["t"] < ct <= b_["t"] + 1e-6 for ct in cuts):
            continue
        dr.line([P(a_["x"], a_["z"]), P(b_["x"], b_["z"])], fill=(250, 250, 250), width=1)
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    od = ImageDraw.Draw(over)
    ext = max(10.0, span * 0.07)
    asp = res["size"][0] / res["size"][1]
    for c in cams:
        if c["kind"] not in ("key", "cut"):
            continue
        half = math.atan(math.tan(math.radians(c["fov"]) / 2) * asp)
        pos = P(c["x"], c["z"])
        poly = [pos]
        for sgn in (-1, 1):
            hx, hz = ST.heading_forward(c["yaw"] + sgn * math.degrees(half))
            poly.append(P(c["x"] + hx * ext / math.cos(half), c["z"] + hz * ext / math.cos(half)))
        od.polygon(poly, fill=(255, 220, 60, 70), outline=(255, 220, 60, 230))
    im = Image.alpha_composite(im, over)
    dr = ImageDraw.Draw(im)
    for c in cams:
        X, Y = P(c["x"], c["z"])
        ring = (235, 70, 70) if c["kind"] == "cut" else (255, 220, 60)
        dr.ellipse([X - 10, Y - 10, X + 10, Y + 10], fill=(20, 20, 24), outline=ring, width=2)
        label = str(c["n"])
        dr.text((X - dr.textlength(label, font=small) / 2, Y - 7), label, font=small, fill=(255, 255, 255))
    # legend and scale
    fits = [s for s in (1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000) if s * sc <= W * 0.18]
    nice = max(fits) if fits else 1
    bx, by = 16, H - 22
    dr.line([bx, by, bx + nice * sc, by], fill=(240, 240, 240), width=3)
    dr.text((bx, by - 18), f"{nice:g} m", font=small, fill=(240, 240, 240))
    lx = W - 210
    dr.text((lx, H - 62), "x east ->   z south v", font=small, fill=(150, 156, 170))
    if allv:
        for i in range(120):
            dr.line([lx + i, H - 42, lx + i, H - 32], fill=speed_color(lo + (hi - lo) * i / 119, lo, hi))
        dr.text((lx, H - 28), f"{lo:.0f}", font=small, fill=(200, 200, 200))
        dr.text((lx + 80, H - 28), f"{hi:.0f} m/s", font=small, fill=(200, 200, 200))
    dr.text((16, 8), f"{res['scene']['name']}: {len(cams)} camera points, {len(routes)} route(s), numbers = contact sheet #; yellow wedge = view at a key, red ring = cut", font=small, fill=(210, 214, 224))
    im.convert("RGB").save(out_png)
    return out_png


# ------------------------------------------------------------------------------------------------------------------------------- the mock game
def _tiny_png(w, h, rgb):
    """A solid PNG with no Pillow (zlib only): for a mock without pictures."""
    import struct
    import zlib

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


class MockGame(ST.director_client_module().MockServer):
    """A protocol mock with scene-model camera and car poses for preview checks."""

    def __init__(self, root, world_delay=0.0, ready_delay=0.0, brain_unscrubbable=True,
                 no_step=False, force_unscrubbable=False, aloft_s=0.0, clock_advance=0.0):
        super().__init__(root)
        self.world_delay, self.ready_delay = world_delay, ready_delay
        self.brain_unscrubbable = brain_unscrubbable
        self.no_step, self.force_unscrubbable = no_step, force_unscrubbable
        self.aloft_s = aloft_s
        self.clock_advance = clock_advance
        self.model, self.t, self.load_time, self.loaded_text = None, 0.0, 0.0, ""
        self.t_start = time.time()
        self.take_folder = None

    def start(self):
        super().start()
        self.info_path = self.root / "director.json"
        self.write_info(self.info_path)
        return self.info_path

    def _run(self, client, cmd, args, wait, timeout_s, base=None):
        answer = self.handle(cmd, args)
        if answer is None:
            return super()._run(client, cmd, args, wait, timeout_s, base)
        ok, payload = answer
        return self._ok(payload) if ok else {"ok": False, "error": payload}

    # ---- the world
    def world_ready(self):
        return time.time() - self.t_start >= self.world_delay

    def cars_ready(self):
        return time.time() - self.load_time >= self.ready_delay

    def camera(self):
        e = self.model.est_camera(self.t) if self.model else None
        if e is None:
            return {"pos": [819.6, 19.0, -800.0], "yaw": 80.0, "pitch": -4.0, "roll": 0.0, "fov": 40.0}
        return {"pos": e["pos"], "yaw": e["yaw"], "pitch": e["pitch"], "roll": 0.0, "fov": e["fov"] or 40.0}

    def scene_cars(self):
        out = []
        if self.model:
            for cid, c in self.model.cars.items():
                p = self.model.car_pose(cid, self.t)
                if p is None or cid == "player":
                    continue
                out.append({"id": cid, "model": c.model, "pos": [p["x"], p["y"] or 17.6, p["z"]], "heading": p["heading"], "speed": p["speed"], "ready": self.cars_ready(),
                            "on_path": p["on_path"]})
        return out

    def project(self, cam, point):
        yaw, pitch = math.radians(cam["yaw"]), math.radians(cam["pitch"])
        f = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
        r = (-math.cos(yaw), 0.0, math.sin(yaw))
        u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
        d = [point[i] - cam["pos"][i] for i in range(3)]
        zc = sum(d[i] * f[i] for i in range(3))
        if zc <= 0.1:
            return None
        xc, yc = sum(d[i] * r[i] for i in range(3)), sum(d[i] * u[i] for i in range(3))
        vf = math.radians(cam["fov"])
        tv = math.tan(vf / 2)
        asp = 16 / 9
        uu, vv = 0.5 + 0.5 * xc / (zc * tv * asp), 0.5 - 0.5 * yc / (zc * tv)
        return uu, vv, 1.5 / (zc * 2 * tv), zc

    def visible(self, subject):
        cid = subject.split(":", 1)[-1]
        p = self.model.car_pose(cid, self.t) if self.model else None
        if p is None:
            return None
        pr = self.project(self.camera(), (p["x"], (p["y"] or 17.6) + 1.0, p["z"]))
        if pr is None:
            return {"in_frame": False, "screen": None, "size_frac": 0.0, "dist_m": None, "occluded": None}
        u, v, size, dist = pr
        return {"in_frame": 0 <= u <= 1 and 0 <= v <= 1, "screen": [round(u, 4), round(v, 4)], "size_frac": round(size, 4), "dist_m": round(dist, 2), "occluded": None}

    def picture(self, rel, size):
        w, h = size
        dest = self.root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            from PIL import Image, ImageDraw
        except ImportError:
            dest.write_bytes(_tiny_png(w, h, (40, 60, 90)))
            return dest
        im = Image.new("RGB", (w, h), (30, 50, 80))
        d = ImageDraw.Draw(im)
        for y in range(h):
            d.line([0, y, w, y], fill=(30 + y * 60 // h, 50 + y * 50 // h, 80 + y * 40 // h))
        d.text((8, 8), f"mock t={self.t:.3f}", fill=(255, 255, 255))
        for c in self.scene_cars():
            vis = self.visible("car:" + c["id"])
            if vis and vis["screen"] and vis["in_frame"]:
                x, y = vis["screen"][0] * w, vis["screen"][1] * h
                r = max(4, vis["size_frac"] * h / 2)
                d.rectangle([x - r * 1.6, y - r * 0.6, x + r * 1.6, y + r * 0.6], fill=(200, 60, 50))
        im.save(dest)
        return dest

    # ---- commands
    def handle(self, cmd, a):
        e = lambda code, msg: (False, {"code": code, "msg": msg})
        if cmd in ("ping", "help", "quit", "events.set"):
            return None
        if not self.world_ready():
            return e("no_world", "loading")
        if cmd == "transport.get":
            return True, {"state": "running", "t": self.t, "duration": self.model.duration() if self.model else 0.0, "paused": False, "mode": "realtime", "world": True}
        if cmd == "scene.load":
            p = Path(a.get("path", ""))
            if not p.is_file():
                return e("bad_args", f"no such scene file {p}")
            self.loaded_text = p.read_text(encoding="utf-8")
            self.model = ST.SceneModel(p, self.loaded_text)
            self.t, self.load_time = self.clock_advance, time.time()
            return True, {"path": str(p), "state": "running"}
        if cmd == "scene.get":
            if not self.model:
                return e("not_ready", "no scene is loaded: scene.load first")
            return True, self._scene_info(self.model.path)
        if cmd == "scene.unload":
            self.model = None
            return True, {}
        if cmd in ("transport.mode", "transport.pause", "transport.play"):
            return True, {}
        if cmd == "transport.restart":
            self.t = float(a.get("t", 0.0)) + self.clock_advance
            self.load_time = time.time()
            return True, {"t": self.t}
        if cmd == "transport.step":
            if self.no_step:
                return e("unknown_cmd", "no command transport.step")
            n = int(a.get("frames", 1))
            self.t += n / 60.0
            return True, {"t": self.t, "frames": n}
        if cmd == "take.status":
            return True, {"active": False}
        if self.model is None:
            return e("bad_args", "no scene loaded")
        if cmd == "transport.seek":
            if not self.cars_ready():
                return e("not_ready", "cars not ready")
            self.t = float(a["t"])
            return True, {"t": self.t, "paused": True, "frozen": True, "settled": True,
                          "scrubbable": not (self.force_unscrubbable or (self.brain_unscrubbable and bool(self.model.brain_cars())))}
        if cmd == "camera.get":
            if self.aloft_s and time.time() - self.t_start < self.world_delay + self.aloft_s:
                return True, {"mode": "game", "pose": {"pos": [-327.2, 8999.2, 111.5], "yaw": -180.0, "pitch": -89.0, "roll": 0.0, "fov": 25.0}, "rows": 4, "aspect": 16 / 9}
            return True, {"mode": "scene", "pose": self.camera(), "rows": 4, "aspect": 16 / 9}
        if cmd == "cars.list":
            return True, {"cars": self.scene_cars()}
        if cmd == "query.streaming":
            return True, {"done": self.cars_ready()}
        if cmd == "query.visible":
            v = self.visible(a.get("subject", ""))
            return (True, v) if v is not None else e("bad_args", "no such car")
        if cmd == "query.ground":
            return True, {"ok": True, "y": 17.6}
        if cmd == "shot":
            size = a.get("size") or [320, 180]
            if a.get("t") is not None:
                self.t = float(a["t"])
            dest = self.picture(a["path"], size)
            self._publish("shot.done", path=str(dest), t=self.t, ok=True)
            return True, {"path": str(dest), "width": size[0], "height": size[1], "t": self.t, "bytes": dest.stat().st_size, "frame": 0}
        if cmd == "shot.schedule":
            if a.get("times") and min(a["times"]) < self.t - (self.model.step or 1.0 / 60.0):
                return e("bad_args", "shot.schedule: the first time is behind the scene's clock: transport.restart first")
            threading.Thread(target=self._schedule, args=(a,), daemon=True).start()
            return True, {"scheduled": len(a.get("times", []))}
        if cmd == "take.render":
            self.take_folder = self._make_take(a)
            name = a.get("name", "")
            self._publish("take.done", name="unrelated", folder="", state="failed", frames=0)
            self._publish("take.done", name=self.take_folder.name, folder=str(self.take_folder), state="complete", frames=60)
            return True, {"name": name}
        return e("unknown_cmd", f"no command {cmd}")

    def _scene_info(self, p):
        m = self.model
        return {"path": str(p), "rev": 1, "hash": ST.sha1_of(self.loaded_text), "events": [], "diagnostics": [], "car_ids": sorted(m.cars) if m else [], "duration": m.duration() if m else 0.0}

    def _schedule(self, a):
        times = a.get("times", [])
        for i, t in enumerate(times, 1):
            self.t = float(t)
            rel = f"{a.get('dir', 'shots')}/mock-t{t:08.3f}.png"
            dest = self.picture(rel, a.get("size") or [320, 180])
            self._publish("shot.done", t=t, path=str(dest), ok=True, index=i, total=len(times))
            time.sleep(0.02)
        self._publish("scene.state", state="finished")

    def _make_take(self, a):
        sr = ST.load_module(ST.SCENE_RUN, "scene_run_mod")
        kind = a.get("sink", "none")
        take = sr.sample_take(kind, count=round((a["to"] - a["from"]) * a["fps"]))
        folder = self.root / "takes" / (a["name"] + "-2")
        take["scene"]["source"] = str(self.model.path)
        take["name"] = folder.name
        folder.mkdir(parents=True, exist_ok=True)
        sr.make_folder(folder, take)
        return folder


# ------------------------------------------------------------------------------------------------------------------------------- the selftest
MINI_SCENE = """scene mini fps 60 step 1/60
world hour 15 hold weather nice traffic off peds off parked off police off
at 0.5 car p1 spawn vp_lam_murcielago_06 at 100,17.6,100 heading 90
at 1 car p1 path mini.route
cam 2 orbit car:p1 r 9 az 20 el 8 fov 40
cam 5 orbit car:p1 r 7 az 110 el 12 fov 34 ease inout
cut 7
cam 7 attach car:p1 0 1.4 -6 heading look car:p1 0 1 0 fov 50
at 2 capture fps 60 name mini sink none
at 9 capture off
end 10
"""
MINI_ROUTE = "# mini\n100,100,8\n140,100,8\n180,100,8\n220,100,8\n"
AWAY_SCENE = MINI_SCENE.replace("cam 2 orbit car:p1 r 9 az 20 el 8 fov 40", "cam 2 pos 100 19 130 yaw 0 pitch 0 fov 40")
BRAIN_SCENE = MINI_SCENE.replace("path mini.route", "path mini.route mode brain")


def selftest():
    """The checks of scene_preview.py and of the channel client against the mock game: [(label, ok, detail)]."""
    results = []

    def check(ok, label, detail=""):
        results.append((label, bool(ok), detail))

    tmp = Path(tempfile.mkdtemp(prefix="scene_preview_selftest_"))
    try:
        sc_dir = tmp / "scenes"
        sc_dir.mkdir()
        (sc_dir / "mini.route").write_text(MINI_ROUTE)
        for name, text in (("mini", MINI_SCENE), ("away", AWAY_SCENE), ("brain", BRAIN_SCENE)):
            (sc_dir / f"{name}.scene").write_text(text)
        model = ST.SceneModel(sc_dir / "mini.scene", MINI_SCENE)
        # the time set
        times = shot_times(model, every=2.0)
        ts = [i["t"] for i in times]
        kinds = {i["kind"] for i in times}
        check(ts == sorted(ts) and len(set(ts)) == len(ts), "time set: sorted, no duplicates", str(ts))
        check({"key", "cut", "precut", "end"} <= kinds, "time set: keys, the cut, one update before it, the end", str(sorted(kinds)))
        check(any(abs(t - 2.0) < 1e-6 for t in ts) and any(abs(t - 5.0) < 1e-6 for t in ts) and any(abs(t - 7.0) < 1e-6 for t in ts), "time set: every cam key time is in it", str(ts))
        check(all(abs(t / model.step - round(t / model.step)) < 1e-3 for t in ts), "time set: all on the scene's step grid")
        check(ts[0] >= 1.99 and ts[-1] <= 9.0, "time set: inside the take (capture 2..9)", f"{ts[0]}..{ts[-1]}")
        one = shot_times(model, t_from=3.0, t_to=3.1, max_shots=1)
        check(len(one) == 1 and 3.0 <= one[0]["t"] < 3.1 and one[0]["kind"] == "start", "time set: a single still excludes earlier camera keys")
        small = shot_times(model, every=0.5, max_shots=6)
        check(len(small) <= 6 and any(i["kind"] == "cut" for i in small), "time set: --max-shots thins the extras first and keeps the cut", str([(i["t"], i["kind"]) for i in small]))
        # the checks
        thr = dict(DEFAULT_THR)

        def shot(**kw):
            base = {"subject": {"id": "p1", "screen": [0.5, 0.5], "in_frame": True, "size_frac": 0.2, "occluded": False}, "cars": [], "streaming_ready": True, "ground_clearance_m": 2.0}
            base.update(kw)
            return evaluate_shot(base, thr)

        c, n, ok = shot()
        check(ok and c["subject_in_frame"] == "pass" and c["car_on_road"] == "n/a", "checks: a centred subject passes")
        check(not shot(subject={"id": "p1", "screen": [0.97, 0.5], "in_frame": True, "size_frac": 0.2, "occluded": False})[2], "checks: the subject near the edge fails the inner 90%")
        check(not shot(subject={"id": "p1", "screen": None, "in_frame": False, "size_frac": 0.0, "occluded": None})[2], "checks: a subject behind the camera fails")
        check(not shot(subject={"id": "p1", "screen": [0.5, 0.5], "in_frame": True, "size_frac": 0.01, "occluded": False})[2], "checks: a tiny subject fails the size")
        check(not shot(subject={"id": "p1", "screen": [0.5, 0.5], "in_frame": True, "size_frac": 0.95, "occluded": False})[2], "checks: a huge subject fails the size")
        check(not shot(subject={"id": "p1", "screen": [0.5, 0.5], "in_frame": True, "size_frac": 0.2, "occluded": True})[2], "checks: an occluded subject fails")
        c2, n2, ok2 = shot(subject={"id": "p1", "screen": [0.5, 0.5], "in_frame": True, "size_frac": 0.2, "occluded": None})
        check(ok2 and c2["not_occluded"] == "unknown", "checks: occluded null is unknown, not a failure")
        check(shot(subject={"id": ""})[0]["subject_in_frame"] == "n/a", "checks: no subject is n/a")
        check(not shot(cars=[{"id": "a", "on_path": True, "dist_to_road_m": 7.0}])[2], "checks: a route car 7 m from the road fails")
        check(shot(cars=[{"id": "a", "on_path": True, "dist_to_road_m": 2.0}])[2], "checks: a route car 2 m from the road passes")
        check(shot(cars=[{"id": "a", "on_path": False, "dist_to_road_m": 90.0}])[2], "checks: a parked car is not held to the road")
        check(not shot(streaming_ready=False)[2], "checks: streaming not ready fails")
        check(not shot(ground_clearance_m=0.1)[2], "checks: the camera under 0.3 m above ground fails")
        check(shot(ground_clearance_m=None)[0]["camera_above_ground"] == "unknown", "checks: no ground height is unknown")
        results += _selftest_mock_runs(tmp, sc_dir)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return results


def _selftest_mock_runs(tmp, sc_dir):
    results = []

    def check(ok, label, detail=""):
        results.append((label, bool(ok), detail))

    def args_for(game, scene, out, **kw):
        info_file = getattr(game, "info_path", None) or game.info_path
        ns = argparse.Namespace(scene=str(scene), every=2.0, t_from=None, t_to=None, max_shots=36, size="640x360", out=str(out), subject="", strategy="auto", road_check="path", thr=[],
                                atlas=str(tmp / "no-atlas"), cols=3, sheet_width=1560, no_map=False, no_src=True, ready_timeout_s=20.0, strict=False, timeout_s=60.0, json=True, attach=str(info_file),
                                game_dir=None, work_dir=None, world_timeout_s=30.0, ffmpeg=None, src=None)
        for k, v in kw.items():
            setattr(ns, k, v)
        return ns

    def run(scene, out, game_kw=None, **kw):
        game = MockGame(tmp / f"game-{scene}-{len(results)}", **(game_kw or {}))
        game.start()
        try:
            a = args_for(game, sc_dir / f"{scene}.scene", out, **kw)
            import io
            buf = io.StringIO()
            old = sys.stdout
            sys.stdout = buf
            try:
                code = run_preview(a)
            finally:
                sys.stdout = old
            return code, json.loads(buf.getvalue()), game
        finally:
            game.stop()

    # a good scene against a mock that loads slowly: world delay and cars not ready at first
    out = tmp / "out-mini"
    code, res, game = run("mini", out, {"world_delay": 0.3, "ready_delay": 0.6})
    check(code == 0 and res["pass"], "preview: the mini scene passes against the mock", f"code {code}; {res.get('summary')}")
    check(res["strategy"] == "seek" and len(res["shots"]) >= 6, "preview: seek strategy, at least 6 shots", f"{res['strategy']} {len(res['shots'])}")
    check(all((out / s["file"]).is_file() for s in res["shots"]), "preview: every shot's picture is in shots\\")
    # Pillow-only thumbnail and map checks.
    try:
        from PIL import Image
    except ImportError:
        check(True, "SKIP (Pillow): contact sheet and preview map")
    else:
        contact = [Path(p) for p in res["files"].get("contact", [])]
        check(contact and all(Image.open(p).size[0] <= 1600 for p in contact), "preview: the contact sheet is at most 1600 px wide", str([Image.open(p).size for p in contact]))
        check(Path(res["files"].get("map", "")).is_file() and Image.open(res["files"]["map"]).size[0] <= 1600, "preview: map.png is made")
    check((out / "preview.json").is_file() and json.loads((out / "preview.json").read_text(encoding="utf-8"))["schema"] == 1, "preview: preview.json is written, schema 1")
    check(any("# [scene_tool]" in ln and "capture" in ln for ln in game.loaded_text.splitlines()) and not any(ln.split("#")[0].split()[:1] == ["capture"] or ln.split("#")[0].split()[2:3] == ["capture"]
          for ln in game.loaded_text.splitlines()), "preview: the capture keys are commented out in the scene the game loads (a preview never makes a take)")
    check([w for w in res["warnings"] if "capture" in w], "preview: it says it commented the captures out")
    cmds = [c for c, _ in game.cmd_log]
    check(cmds.index("scene.load") < cmds.index("transport.seek") and "transport.mode" in cmds, "preview: loads the scene, sets the fixed step, then seeks")
    s0 = res["shots"][0]
    check(s0["subject"]["id"] == "p1" and s0["subject"]["in_frame"] is True and s0["camera"]["pos"], "preview: the first shot knows its subject and the camera pose")
    # Wait for the free-roam entry zoom before taking the first shot.
    code, res, game = run("mini", tmp / "out-aloft", {"aloft_s": 2.5})
    check(code == 0 and all((s["camera"].get("pos") or [0, 0, 0])[1] < 1500 for s in res["shots"]), "preview: no shot is taken while the game's camera is still up on the map's zoom", f"code {code}; {[round((s['camera'].get('pos') or [0, 0, 0])[1]) for s in res['shots']][:4]}")
    # the camera looks away from the subject: a failing check, exit 1
    code, res, game = run("away", tmp / "out-away")
    check(code == 1 and not res["pass"], "preview: a camera that looks away from the car fails (exit 1)", f"code {code}")
    check(any("subject_in_frame" in f for f in res["failed"]), "preview: the failure names the check and the shot", str(res["failed"][:2]))
    # a brain car: the world really runs, stepped from shot to shot (transport.restart, pause, step), and every per-shot answer is asked at the exact time
    code, res, game = run("brain", tmp / "out-brain", {"ready_delay": 0.0})
    cmds = [c for c, _ in game.cmd_log]
    check(res["strategy"] == "step" and len(res["shots"]) >= 6 and all(s["file"] for s in res["shots"]), "preview: a brain car is previewed by stepping the scene", f"{res['strategy']} {len(res['shots'])}")
    check("transport.step" in cmds and "transport.seek" not in cmds and res["shots"][0]["subject"]["in_frame"] is not None, "preview: stepping asks the same questions at each shot, and never seeks")
    check([w for w in res["warnings"] if "brain" in w], "preview: it warns that a brain car is not exact in the take")
    steps = [a["frames"] for c, a in game.cmd_log if c == "transport.step"]
    check(sum(steps) == round(res["shots"][-1]["t"] * 60), "preview: the steps add up to the last shot's time", f"{sum(steps)} frames, last t {res['shots'][-1]['t']}")
    # the game says it cannot scrub a scene the tool thought was pure: auto goes over to stepping
    code, res, game = run("mini", tmp / "out-force", {"force_unscrubbable": True})
    check(res["strategy"] == "step" and [w for w in res["warnings"] if "cannot be scrubbed" in w], "preview: a scrubbable:false answer switches to stepping", f"{res['strategy']}")
    # no transport.step: pictures from one play-through, everything else unknown
    code, res, game = run("brain", tmp / "out-play", {"no_step": True, "clock_advance": 2.0}, t_from=0.0)
    check(res["strategy"] == "play" and all(s["file"] for s in res["shots"]) and all(v in ("unknown", "n/a") for s in res["shots"] for v in s["checks"].values() if v != "pass"),
          "preview: without transport.step it plays through once and says the checks are unknown", f"{res['strategy']}")
    cmds = [c for c, _ in game.cmd_log]
    schedule_at = cmds.index("shot.schedule")
    check(cmds[schedule_at - 2:schedule_at] == ["transport.restart", "transport.seek"] and res["shots"][0]["t"] == 0,
          "preview: rewinds and pins the advanced clock before scheduling the t=0 shot")
    # Director mock server (director_client.MockServer): the wire as the game speaks it; it has no query.visible and no pictures of cars, so the subject checks are unknown
    try:
        dcm = ST.director_client_module()
        ms = dcm.MockServer(root=tmp / "dc-root", scene_path=tmp / "dc-root" / "mock.scene", ready_after=0.2)
        port, token = ms.start()
        ms.write_info(tmp / "dc-root" / "director.json")
        try:
            a = args_for(argparse.Namespace(info_path=tmp / "dc-root" / "director.json"), sc_dir / "mini.scene", tmp / "out-dc", every=3.0)
            import io
            buf = io.StringIO()
            old = sys.stdout
            sys.stdout = buf
            try:
                code = run_preview(a)
            finally:
                sys.stdout = old
            res = json.loads(buf.getvalue())
            check(code in (0, 1) and len(res["shots"]) >= 4 and all((tmp / "out-dc" / s["file"]).is_file() for s in res["shots"]), "preview: runs against Director mock server end to end", f"code {code}, {len(res['shots'])} shots, {res['strategy']}")
            check(res["shots"][0]["camera"]["pos"] and res["shots"][0]["subject"].get("in_frame") is None, "preview: a game without query.visible gives unknown subject checks, not a crash")
        finally:
            ms.stop()
    except ToolError as e:
        check(False, "preview: Director mock server is usable", e.msg)
    # an attached game gets its own scene back, and the clock runs free again
    game = MockGame(tmp / "game-human")
    game.start()
    try:
        human = sc_dir / "away.scene"
        hc = ST.Channel({"port": game.port, "token": game.token})
        hc.call("scene.load", {"path": str(human)})
        hc.close()
        code, res = None, None
        a = args_for(game, sc_dir / "mini.scene", tmp / "out-human")
        import io
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            code = run_preview(a)
        finally:
            sys.stdout = old
        loads = [x["path"] for c, x in game.cmd_log if c == "scene.load"]
        modes = [x for c, x in game.cmd_log if c == "transport.mode"]
        check(len(loads) == 3 and loads[-1] == str(human) and modes[-1].get("fixed") is False, "preview: an attached game gets its own scene back and the free clock", f"{[Path(p).name for p in loads]} {modes[-1:]}")
    finally:
        game.stop()
    # a bad token is refused, a dead port is unreachable
    g = MockGame(tmp / "game-tok")
    g.start()
    try:
        ST.Channel({"_path": str(g.info_path), "port": g.port, "token": "wrong"})
        check(False, "channel: a wrong token is refused")
    except ToolError as e:
        check(e.code == "refused", "channel: a wrong token is refused", e.code)
    g.stop()
    try:
        ST.Channel({"_path": str(g.info_path), "port": g.port, "token": "x"})
        check(False, "channel: a closed port is unreachable")
    except ToolError as e:
        check(e.code == "unreachable", "channel: a closed port is unreachable", e.code)
    # the render path through the channel: scene.load, take.render, take.done, take.json checked
    game = MockGame(tmp / "game-render")
    game.start()
    try:
        ns = args_for(game, sc_dir / "mini.scene", tmp / "x", fps=60.0, sink="none", name="mini-take", t_from=0.0, t_to=1.0)
        import io
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            code = ST.cmd_render(ns)
        finally:
            sys.stdout = old
        res = json.loads(buf.getvalue())
        check(code == 0 and res["ok"], "render: a take through the channel is checked with scene_run's check_take", f"code {code} {res.get('problems')}")
        take_cmd = [a for c, a in game.cmd_log if c == "take.render"]
        check(take_cmd and take_cmd[0].get("fps") == 60.0 and take_cmd[0].get("name") == "mini-take" and res["take"] == "mini-take-2", "render: take.render gets the fps and the name", str(take_cmd))
    finally:
        game.stop()
    # wait_world: transport.get answers in every state; the `world` field (or the player's car in cars.list) says when a city is there
    class _R:
        def __init__(self, ok, result=None, code="", msg=""):
            self.ok, self.result, self.code, self.msg = ok, result, code, msg

    class _Ch:
        def __init__(self, worlds, cars_when):
            self.n, self.worlds, self.cars_when = 0, worlds, cars_when

        def request(self, cmd, args, timeout=0):
            if cmd == "transport.get":
                self.n += 1
                w = self.worlds[min(self.n - 1, len(self.worlds) - 1)]
                return _R(True, {"state": "off"} if w is None else {"state": "off", "world": w})
            if cmd == "cars.list":
                return _R(True, {"cars": [{"id": "player"}] if self.n >= self.cars_when else []})
            return _R(False, code="unknown_cmd")
    ch = _Ch([False, False, True], 99)
    r = ST.wait_world(ch, 10.0, poll=0.01)
    check(ch.n == 3 and r.get("world") is True, "wait_world: waits while transport.get says world false", f"{ch.n} asks")
    ch = _Ch([None], 3)
    ch.n = 0
    r = ST.wait_world(ch, 10.0, poll=0.01)
    check(ch.n == 3, "wait_world: an exe without the field waits for the player's car in cars.list", f"{ch.n} asks")
    try:
        ST.wait_world(_Ch([False], 99), 0.05, poll=0.01)
        check(False, "wait_world: a game that never has a city times out")
    except ToolError as e:
        check(e.code == "timeout", "wait_world: a game that never has a city times out", e.code)
    return results


def _mock_main(argv):
    ap = argparse.ArgumentParser(description="serve the mock game (for trying scene_tool.py preview / render with no game)")
    ap.add_argument("--root", required=True, help="the Director output folder: director.json and the pictures go here")
    ap.add_argument("--world-delay", type=float, default=0.0)
    ap.add_argument("--ready-delay", type=float, default=0.0)
    a = ap.parse_args(argv)
    g = MockGame(a.root, a.world_delay, a.ready_delay)
    info = g.start()
    print(f"mock game on 127.0.0.1:{g.port}; director.json: {info}\ntry: python scene_tool.py preview <scene> --attach {info}\n(Ctrl+C or the `quit` command stops it)")
    try:
        while not g._stop:
            time.sleep(0.5)
    except KeyboardInterrupt:
        g.stop()
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "mock":
        sys.exit(_mock_main(sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        bad = 0
        for label, ok, detail in selftest():
            print(("" if label.startswith("SKIP") else "PASS " if ok else "FAIL ") + label + ((": " + detail) if detail and not ok else ""))
            bad += 0 if ok else 1
        print("selftest: %d failed" % bad)
        sys.exit(1 if bad else 0)
    print(__doc__)
