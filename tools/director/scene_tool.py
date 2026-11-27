"""Write, check, preview and render LARecomp scenes. See README.md for examples."""
import argparse
import difflib
import hashlib
import importlib.util
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENES = HERE / "scenes"
RECIPES = SCENES / "recipes"
SPOTS_FILE = SCENES / "spots.txt"
SCENE_CHECK = HERE / "scene_check.py"
SCENE_RUN = HERE / "scene_run.py"
TAKE_DIFF = HERE / "take_diff.py"
DEFAULT_SRC = HERE.parents[1] / "src"
WORK_FOLDER = "director-work"

EXIT_OK, EXIT_FINDINGS, EXIT_USAGE, EXIT_ENV = 0, 1, 2, 3
PROTO = 1
DEFAULT_SPACING_M = 2.0

# register under the name the other file loads us by (scene_preview.py: sys.modules["scene_tool"]), also when this file runs as __main__
sys.modules.setdefault("scene_tool", sys.modules[__name__])


# ------------------------------------------------------------------------------------------------------------------------------- errors, output
class ToolError(Exception):
    """A problem the command line reports as one sentence: `code` (bad_args, not_found, no_atlas, no_game, unreachable, timeout, refused, failed ...), exit status 2 for
    bad arguments (code bad_args / not_found) and 3 for the environment."""

    def __init__(self, code, msg, line=None, exit_code=None):
        super().__init__(msg)
        self.code, self.msg, self.line = code, msg, line
        self.exit_code = exit_code if exit_code is not None else (EXIT_USAGE if code in ("bad_args", "not_found") else EXIT_ENV)

    def as_json(self):
        e = {"code": self.code, "msg": self.msg}
        if self.line is not None:
            e["line"] = self.line
        return {"ok": False, "error": e}


def emit(a, result, lines, code):
    """Prints the result: one JSON object with --json, else the text lines. Returns the exit code."""
    if getattr(a, "json", False):
        print(json.dumps(result, indent=2))
    else:
        for ln in lines:
            print(ln)
    return code


def load_module(path, name):
    """A sibling script by path (python -I has no sibling imports); the module is registered first because its dataclasses look themselves up there."""
    spec = importlib.util.spec_from_file_location(name, str(path))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_SCENE_CHECK = []


def scene_check_module():
    if not _SCENE_CHECK:
        _SCENE_CHECK.append(sys.modules.get("scene_check_mod") or load_module(SCENE_CHECK, "scene_check_mod"))
    return _SCENE_CHECK[0]


def resolve_scene(name):
    """Find a scene by path or by recipe name."""
    p = Path(name).expanduser()
    if not p.name:
        raise ToolError("bad_args", "specify a scene file or recipe name")
    choices = [p] if p.suffix.lower() == ".scene" else [p, p.with_name(p.name + ".scene")]
    cands = choices + [RECIPES / x for x in choices if not x.is_absolute()]
    for c in cands:
        if c.is_file():
            return c.resolve()
    raise ToolError("not_found", f"no scene '{name}' (looked for {', '.join(str(c) for c in cands)})")


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def sha1_of(text):
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]


def fmt(v, digits=2):
    return f"{v:.{digits}f}".rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def num_or_none(s):
    try:
        v = float(s)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


# ------------------------------------------------------------------------------------------------------------------------------- vectors, frames
def heading_forward(h_deg):
    """The unit vector (x, z) a heading looks along: 0 along +Z, 90 along +X (car_verbs.h, camera.h)."""
    r = math.radians(h_deg)
    return math.sin(r), math.cos(r)


def heading_right(h_deg):
    """The (x, z) of the right-hand side of a heading: looking along +Z the right is -X."""
    r = math.radians(h_deg)
    return -math.cos(r), math.sin(r)


def offset_in_frame(base, heading, r, u, f):
    """base (x, y, z) moved r to the right, u up and f forward in the frame of a heading: the offset convention of `near`, `attach` and `spot:<name>@r,u,f`."""
    fx, fz = heading_forward(heading)
    rx, rz = heading_right(heading)
    return base[0] + f * fx + r * rx, base[1] + u, base[2] + f * fz + r * rz


def heading_of(dx, dz, keep=0.0):
    if abs(dx) < 1e-9 and abs(dz) < 1e-9:
        return keep
    return math.degrees(math.atan2(dx, dz)) % 360.0


def dist_xz(a, b):
    return math.hypot(a[0] - b[0], a[-1] - b[-1])


def lerp(a, b, u):
    return a + (b - a) * u


# ------------------------------------------------------------------------------------------------------------------------------- spots
@dataclass
class Spot:
    name: str
    x: float
    y: float
    z: float
    heading: float
    text: str = ""
    file: str = ""
    line: int = 0
    has_heading: bool = True

    def pos(self):
        return (self.x, self.y, self.z)


SPOT_REF = re.compile(r"^spot:([A-Za-z0-9][A-Za-z0-9_.-]{0,39})(?:@(.*))?$")


def parse_spots_text(text, file=""):
    """({lower name: Spot}, [problems]) of a spots.txt (spots.h, DIRECTOR-DESIGN 4.2): `<name> <x> <y> <z> [heading]  # note`, blanks or commas between the fields, `#` starts the note;
    a name written twice: the later line wins (a warning)."""
    spots, problems = {}, []
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        body, _, comment = line.partition("#")
        w = [x for x in re.split(r"[\s,]+", body.strip()) if x]
        if not w:
            continue
        if len(w) not in (4, 5):
            problems.append(f"{file}:{n}: a spot is `<name> <x> <y> <z> [heading]  # note`, not {len(w)} fields: '{body.strip()}'")
            continue
        vals = [num_or_none(v) for v in w[1:]]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,39}", w[0]) or any(v is None for v in vals):
            problems.append(f"{file}:{n}: bad spot line '{body.strip()}'")
            continue
        key = w[0].lower()
        if key in spots:
            problems.append(f"{file}:{n}: spot '{w[0]}' is already defined on line {spots[key].line}: this one wins")
        spots[key] = Spot(w[0], vals[0], vals[1], vals[2], (vals[3] % 360.0) if len(vals) == 4 else 0.0, comment.strip(), file, n, len(vals) == 4)
    return spots, problems


def spots_files_for(scene_dir):
    """The spots files that apply to a scene in `scene_dir`, shared first (spots.cpp SpotFilesFor): <dir>/../scenes/spots.txt, <dir>/../spots.txt, <dir>/spots.txt."""
    d = Path(scene_dir).resolve()
    out, seen = [], set()
    for c in (d / ".." / "scenes" / "spots.txt", d / ".." / "spots.txt", d / "spots.txt"):
        if c.is_file():
            key = os.path.normcase(str(c.resolve()))
            if key not in seen:
                seen.add(key)
                out.append(c)
    return out


def load_spots(scene_dir=None, extra=None):
    """({lower name: Spot}, [problems]): for a scene in `scene_dir` the files the game reads for it (spots_files_for: the one beside the scene is on top); without a scene the shared
    scenes/spots.txt; then `extra` (a file) on top of those."""
    spots, problems = {}, []
    files = spots_files_for(scene_dir) if scene_dir is not None else ([SPOTS_FILE] if SPOTS_FILE.is_file() else [])
    if extra:
        files = files + [Path(extra)]
    for f in files:
        if f.is_file():
            got, probs = parse_spots_text(f.read_text(encoding="utf-8", errors="replace"), f.name)
            spots.update(got)
            problems += probs
    return spots, problems


def spots_text(spots):
    """A spots.txt for a table (one line a spot: name x y z [heading]  # note)."""
    lines = []
    for s in spots.values():
        lines.append(f"{s.name} {s.x:g} {s.y:g} {s.z:g}" + (f" {s.heading:g}" if s.has_heading else "") + (f"  # {s.text}" if s.text else ""))
    return "\n".join(lines) + "\n"


def read_vec_word(word):
    """'x,y,z' (or 'x,z') as floats, or None."""
    parts = word.split(",")
    if len(parts) not in (2, 3):
        return None
    vals = [num_or_none(p) for p in parts]
    return None if any(v is None for v in vals) else vals


def resolve_spot_ref(word, spots):
    """'spot:gate@3,0,10[,turn]' -> ((x, y, z), heading) or None when the word is not a spot reference. The offset is right, up, forward in the spot's heading frame; the heading
    is the spot's plus the optional turn. ToolError bad_args for an unknown spot, a bad offset, or an offset on a spot with no heading (spots.h says the same)."""
    m = SPOT_REF.match(word)
    if not m:
        return None
    s = spots.get(m.group(1).lower())
    if s is None:
        near = difflib.get_close_matches(m.group(1).lower(), list(spots), n=3)
        raise ToolError("bad_args", f"unknown spot '{m.group(1)}'" + (f" (did you mean {', '.join(near)}?)" if near else f" (known: {', '.join(sorted(spots)) or 'none'})"))
    heading, pos = s.heading, s.pos()
    if m.group(2):
        four = [num_or_none(x) for x in m.group(2).split(",")]
        if len(four) not in (3, 4) or any(v is None for v in four):
            raise ToolError("bad_args", f"spot offset '@{m.group(2)}' is @right,up,forward (three numbers) and optionally a turn in degrees")
        if not s.has_heading and any(v for v in four[:3]):
            raise ToolError("bad_args", f"spot {s.name} has no heading, so an offset in its frame (@right,up,forward) means nothing: write a heading in spots.txt")
        pos = offset_in_frame(pos, s.heading, four[0], four[1], four[2])
        if len(four) == 4:
            heading = (s.heading + four[3]) % 360.0
    return pos, heading


SPOT_WORD = re.compile(r"(?P<prev>\b(?:heading|yaw)\s+)?(?P<ref>spot:[A-Za-z0-9][A-Za-z0-9_.-]*(?:@[^\s\"#]*)?)")


def expand_spots(text, spots):
    """The scene text with every spot word turned into numbers, the way the engine's parser does when it reads a statement (spots.h): `spot:gate@1,0,5` -> x,y,z, `heading spot:gate`
    and `yaw spot:gate` -> degrees. Line numbers and everything else stay. -> (text, [{line, severity, message}]) for the spots that do not resolve (those words stay as written)."""
    out, problems = [], []
    for n, raw in enumerate(text.split("\n"), 1):
        body, hash_, comment = raw.partition("#")

        def repl(m):
            try:
                pos, heading = resolve_spot_ref(m.group("ref"), spots)
            except ToolError as e:
                problems.append({"line": n, "severity": "error", "message": e.msg})
                return m.group(0)
            if m.group("prev"):
                return m.group("prev") + f"{heading:g}"
            return f"{pos[0]:.3f},{pos[1]:.3f},{pos[2]:.3f}"

        out.append(SPOT_WORD.sub(repl, body) + hash_ + comment)
    return "\n".join(out), problems


def resolve_point(word, spots):
    """A point word: spot:NAME[@r,u,f], x,z or x,y,z -> (x, y|None, z, heading|None)."""
    sp = resolve_spot_ref(word, spots)
    if sp:
        return sp[0][0], sp[0][1], sp[0][2], sp[1]
    v = read_vec_word(word)
    if v is None:
        raise ToolError("bad_args", f"'{word}' is not a point: spot:<name>[@right,up,forward], x,z or x,y,z")
    return (v[0], None, v[1], None) if len(v) == 2 else (v[0], v[1], v[2], None)


# ------------------------------------------------------------------------------------------------------------------------------- the atlas (offline world knowledge: atlas.py, atlas)
_ATLAS_MOD = []


def atlas_module():
    """atlas.py (lights, roads, routes, ground, spots; the same answers as the game's query.* commands), loaded by path."""
    if not _ATLAS_MOD:
        f = HERE / "atlas.py"
        if not f.is_file():
            raise ToolError("no_atlas", f"{f} is not there")
        _ATLAS_MOD.append(sys.modules.get("atlas_mod") or load_module(f, "atlas_mod"))
    return _ATLAS_MOD[0]


def find_atlas(folder=None, need_rails=False):
    """Load the specified atlas or the current one in the user's atlas folder."""
    am = atlas_module()
    path = folder or am.default_atlas_dir()
    if not path or not os.path.isdir(path):
        return None
    try:
        at = am.load(str(path))
    except (OSError, ValueError, KeyError) as ex:
        raise ToolError("failed", f"cannot read atlas {path}: {ex}") from ex
    return at if at.rails or not need_rails else None


def atlas_id(at):
    return (getattr(at, "meta", None) or {}).get("hash") or (Path(at.path).name if getattr(at, "path", None) else "")


def atlas_road_dist(at, x, z, r=150.0, heading=None):
    """Metres from (x, z) to the nearest lane (rails) within r, or None when the atlas has no roads; r itself when none is that near. With a heading only lanes that run that way count
    (atlas.py skips lanes more than 75 degrees off it): a car on the wrong side of the road is not on a lane of its own."""
    if at is None or not at.rails:
        return None
    res = atlas_module().nearest_road(at, [x, z], r, 1, heading)
    near = (res.get("result") or {}).get("nearest") if res.get("ok") else None
    return float(near[0]["dist_m"]) if near else float(r)


def atlas_answer(at, what, args, spots):
    """atlas.py's query(<what>, args) with the spots table of this tool: {ok, result|error} exactly like the game's query.<what>."""
    table = {s.name: {"pos": [s.x, s.y, s.z], "heading": s.heading, "text": s.text} for s in spots.values()}
    return atlas_module().query(at, what, args, table)


# ------------------------------------------------------------------------------------------------------------------------------- routes
@dataclass
class RoutePt:
    x: float
    z: float
    speed: float = -1.0  # negative: the file gave none
    y: float = 0.0
    has_y: bool = False


def parse_route_text(text):
    """([RoutePt], [problem]) of a route file (path_follow.h): `x,z[,speed[,y]]` a line, commas or blanks, # comments; the height all or nothing."""
    pts, problems = [], []
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p for p in re.split(r"[,\s]+", line) if p]
        vals = [num_or_none(p) for p in parts]
        if len(parts) < 2 or len(parts) > 4 or any(v is None for v in vals):
            problems.append(f"line {n}: a route point is x,z[,speed[,y]] (numbers), not '{line}'")
            continue
        pts.append(RoutePt(vals[0], vals[1], vals[2] if len(vals) >= 3 else -1.0, vals[3] if len(vals) == 4 else 0.0, len(vals) == 4))
    if pts and len({p.has_y for p in pts}) > 1:
        problems.append("some points have a height and others do not (all or none)")
    if len(pts) > 20000:
        problems.append(f"{len(pts)} points (at most 20000)")
    return pts, problems


def read_route(path):
    pts, problems = parse_route_text(Path(path).read_text(encoding="utf-8"))
    if problems:
        raise ToolError("bad_args", f"{path}: {problems[0]}")
    return pts


@dataclass
class SpeedSpec:
    kind: str = "file"  # file | mps | scale
    value: float = 1.0


def parse_speed_word(word):
    """'12' (m/s), 'x1.5' / '1.5x' (a factor of the route's speeds) -> SpeedSpec; ToolError bad_args otherwise (the limits of path_follow.h)."""
    w = word.strip().lower()
    if w.startswith("x") and num_or_none(w[1:]) is not None:
        spec = SpeedSpec("scale", float(w[1:]))
    elif w.endswith("x") and num_or_none(w[:-1]) is not None:
        spec = SpeedSpec("scale", float(w[:-1]))
    elif num_or_none(w) is not None:
        spec = SpeedSpec("mps", float(w))
    else:
        raise ToolError("bad_args", f"speed '{word}': metres a second (12) or a factor of the route's speeds (x1.5)")
    if spec.kind == "scale" and not (0.05 <= spec.value <= 10.0):
        raise ToolError("bad_args", f"a speed factor from 0.05 to 10, not {spec.value:g}")
    if spec.kind == "mps" and not (0.5 <= spec.value <= 120.0):
        raise ToolError("bad_args", f"a speed from 0.5 to 120 m/s, not {spec.value:g}")
    return spec


class RouteSampler:
    """Where a car is on a route at a time: the exact closed form of path_follow.h PathSampler (speed linear in the distance between points, the time of a segment
    L ln(v1/v0)/(v1-v0)), the heading of the chord from 2 m behind to 2 m ahead. A port for the map, the checks and the mock; the game's own code is the authority."""

    MIN_V, MAX_V, WINDOW = 0.5, 120.0, 4.0

    def __init__(self, points, spec=None):
        spec = spec or SpeedSpec()
        pts = []
        for p in points:
            if pts and math.hypot(p.x - pts[-1].x, p.z - pts[-1].z) < 0.01:
                continue
            pts.append(p)
        if len(pts) < 2:
            raise ToolError("bad_args", "a route needs 2 points at least 1 cm apart")
        self.pts = pts
        self.has_y = all(p.has_y for p in pts)
        self.v = []
        for p in pts:
            s = p.speed if p.speed >= 0 else 12.0
            if spec.kind == "mps":
                s = spec.value
            elif spec.kind == "scale":
                s = s * spec.value
            self.v.append(min(self.MAX_V, max(self.MIN_V, s)))
        self.cum_s, self.cum_t = [0.0], [0.0]
        for i in range(1, len(pts)):
            L = math.hypot(pts[i].x - pts[i - 1].x, pts[i].z - pts[i - 1].z)
            self.cum_s.append(self.cum_s[-1] + L)
            self.cum_t.append(self.cum_t[-1] + self._seg_time(L, self.v[i - 1], self.v[i]))

    @staticmethod
    def _seg_time(L, v0, v1):
        return L / v0 if abs(v1 - v0) < 1e-9 else L * math.log(v1 / v0) / (v1 - v0)

    def length(self):
        return self.cum_s[-1]

    def duration(self):
        return self.cum_t[-1]

    def _point(self, s):
        s = max(0.0, min(self.length(), s))
        lo, hi = 0, len(self.pts) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.cum_s[mid] <= s:
                lo = mid
            else:
                hi = mid
        L = self.cum_s[hi] - self.cum_s[lo]
        u = 0.0 if L <= 0 else (s - self.cum_s[lo]) / L
        a, b = self.pts[lo], self.pts[hi]
        speed = lerp(self.v[lo], self.v[hi], u)
        return (lerp(a.x, b.x, u), lerp(a.z, b.z, u), (lerp(a.y, b.y, u) if self.has_y else None), speed)

    def at_distance(self, s):
        x, z, y, v = self._point(s)
        x0, z0, _, _ = self._point(s - self.WINDOW / 2)
        x1, z1, _, _ = self._point(s + self.WINDOW / 2)
        return {"x": x, "z": z, "y": y, "heading": heading_of(x1 - x0, z1 - z0), "speed": v, "s": max(0.0, min(self.length(), s))}

    def at_time(self, tau):
        if tau <= 0:
            return dict(self.at_distance(0.0), finished=False)
        if tau >= self.duration():
            r = self.at_distance(self.length())
            r["speed"], r["finished"] = 0.0, True
            return r
        lo, hi = 0, len(self.pts) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.cum_t[mid] <= tau:
                lo = mid
            else:
                hi = mid
        L = self.cum_s[hi] - self.cum_s[lo]
        v0, v1, dt = self.v[lo], self.v[hi], tau - self.cum_t[lo]
        if abs(v1 - v0) < 1e-9:
            s = self.cum_s[lo] + v0 * dt
        else:
            k = (v1 - v0) / L
            s = self.cum_s[lo] + (v0 * math.exp(k * dt) - v0) / k
        return dict(self.at_distance(s), finished=False)

    def polyline(self):
        return [(p.x, p.z) for p in self.pts]

    def nearest(self, x, z):
        best = (1e18, 0.0)
        for i in range(1, len(self.pts)):
            a, b = self.pts[i - 1], self.pts[i]
            dx, dz = b.x - a.x, b.z - a.z
            ll = dx * dx + dz * dz
            u = 0.0 if ll <= 1e-12 else max(0.0, min(1.0, ((x - a.x) * dx + (z - a.z) * dz) / ll))
            d = math.hypot(x - (a.x + u * dx), z - (a.z + u * dz))
            if d < best[0]:
                best = (d, self.cum_s[i - 1] + u * (self.cum_s[i] - self.cum_s[i - 1]))
        return {"distance": best[0], "s": best[1]}


def catmull_rom_xz(ctrl, spacing=DEFAULT_SPACING_M):
    """A smooth polyline through the control points (centripetal Catmull-Rom, no loops or cusps), resampled `spacing` metres apart; [(x, z)], the last point is the end."""
    pts = list(ctrl)
    if len(pts) < 2:
        raise ToolError("bad_args", "a route needs two points")
    dense = []
    ext = [pts[0]] + pts + [pts[-1]]
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        if p0 == p1:
            p0 = (2 * p1[0] - p2[0], 2 * p1[1] - p2[1])
        if p3 == p2:
            p3 = (2 * p2[0] - p1[0], 2 * p2[1] - p1[1])

        def tj(a, b):
            return max(1e-6, math.hypot(b[0] - a[0], b[1] - a[1]) ** 0.5)

        t0, t1, t2, t3 = 0.0, tj(p0, p1), 0.0, 0.0
        t2 = t1 + tj(p1, p2)
        t3 = t2 + tj(p2, p3)
        for k in range(24):
            t = lerp(t1, t2, k / 24.0)

            def mix(a, b, ta, tb):
                u = (t - ta) / (tb - ta)
                return (a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u)

            a1, a2, a3 = mix(p0, p1, t0, t1), mix(p1, p2, t1, t2), mix(p2, p3, t2, t3)
            b1, b2 = mix(a1, a2, t0, t2), mix(a2, a3, t1, t3)
            dense.append(mix(b1, b2, t1, t2))
    dense.append(pts[-1])
    return resample_xz(dense, spacing)


def resample_xz(poly, spacing):
    cum = [0.0]
    for i in range(1, len(poly)):
        cum.append(cum[-1] + math.hypot(poly[i][0] - poly[i - 1][0], poly[i][1] - poly[i - 1][1]))
    total = cum[-1]
    if total < 1.0:
        raise ToolError("bad_args", f"the route is {total:.2f} m long (at least 1 m)")
    n = max(1, int(round(total / spacing)))
    out, j = [], 1
    for k in range(n + 1):
        s = total * k / n
        while j < len(cum) - 1 and cum[j] < s:
            j += 1
        seg = cum[j] - cum[j - 1]
        u = 0.0 if seg <= 0 else (s - cum[j - 1]) / seg
        out.append((lerp(poly[j - 1][0], poly[j][0], u), lerp(poly[j - 1][1], poly[j][1], u)))
    return out


def plan_speeds(poly, vmax, a_lat=5.0, a_acc=3.0, a_dec=6.0, v_start=None, v_end=None, vmin=3.0):
    """Speeds (m/s) for the points of a polyline: the cruising speed `vmax` limited by the corners (a_lat / curvature, Menger curvature of three points), then no more
    acceleration than a_acc forward and no more braking than a_dec (two passes), the first / last point held to v_start / v_end when given."""
    n = len(poly)
    lim = [vmax] * n
    for i in range(1, n - 1):
        ax, az = poly[i - 1]
        bx, bz = poly[i]
        cx, cz = poly[i + 1]
        area2 = abs((bx - ax) * (cz - az) - (bz - az) * (cx - ax))  # twice the triangle's area
        la, lb, lc = math.hypot(bx - ax, bz - az), math.hypot(cx - bx, cz - bz), math.hypot(cx - ax, cz - az)
        kappa = 0.0 if la * lb * lc < 1e-9 else 2.0 * area2 / (la * lb * lc)
        if kappa > 1e-6:
            lim[i] = min(vmax, max(vmin, math.sqrt(a_lat / kappa)))
    if v_start is not None:
        lim[0] = min(lim[0], v_start)
    if v_end is not None:
        lim[-1] = min(lim[-1], v_end)
    v = list(lim)
    for i in range(1, n):
        d = math.hypot(poly[i][0] - poly[i - 1][0], poly[i][1] - poly[i - 1][1])
        v[i] = min(v[i], math.sqrt(v[i - 1] ** 2 + 2.0 * a_acc * d))
    for i in range(n - 2, -1, -1):
        d = math.hypot(poly[i + 1][0] - poly[i][0], poly[i + 1][1] - poly[i][1])
        v[i] = min(v[i], math.sqrt(v[i + 1] ** 2 + 2.0 * a_dec * d))
    return [max(RouteSampler.MIN_V, x) for x in v]


def route_text(header, poly, speeds, ys=None):
    lines = [f"# {h}" for h in header]
    for i, p in enumerate(poly):
        row = f"{p[0]:.2f},{p[1]:.2f},{speeds[i]:.1f}"
        if ys is not None:
            row += f",{ys[i]:.2f}"
        lines.append(row)
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------------------------------------------------------------------- the scene model (offline)
class WordError(Exception):
    pass


def car_ref(word):
    m = re.fullmatch(r"car:([A-Za-z_][A-Za-z0-9_]*)", word)
    return m.group(1) if m else None


def read_vec(words, i, spots):
    """A point at words[i:]: spot:NAME[@r,u,f], x,y,z in one word, or three numbers. -> (x, y, z), next index; WordError otherwise."""
    if i >= len(words):
        raise WordError("a point is missing")
    w = words[i]
    try:
        sp = resolve_spot_ref(w, spots)
    except ToolError as e:
        raise WordError(e.msg)
    if sp:
        return sp[0], i + 1
    v = read_vec_word(w)
    if v is not None and len(v) == 3:
        return tuple(v), i + 1
    if i + 2 < len(words):
        vals = [num_or_none(x) for x in words[i:i + 3]]
        if all(x is not None for x in vals):
            return tuple(vals), i + 3
    raise WordError(f"'{w}' is not a point (x,y,z or spot:<name>)")


def read_offset(words, i):
    """An optional three-number offset at words[i:] (dx dy dz, or one word dx,dy,dz): (offset or None, next index)."""
    if i < len(words):
        v = read_vec_word(words[i])
        if v is not None and len(v) == 3:
            return tuple(v), i + 1
        if i + 2 < len(words):
            vals = [num_or_none(x) for x in words[i:i + 3]]
            if all(x is not None for x in vals):
                return tuple(vals), i + 3
    return None, i


@dataclass
class CamKey:
    t: float
    line: int
    words: list
    text: str
    pos: dict = None      # {kind: world|attach|orbit, point, car, heading_only, follow, r, az, el}
    aim: dict = None      # {kind: angles|point|car, yaw, pitch, point, car, offset}
    rig: dict = None      # {kind, car, at, path, params}
    fov: float = None
    roll: float = None
    ease: str = "linear"
    off: bool = False
    problem: str = ""

    def car(self):
        """The car this key is about (look / attach / orbit / rig subject), or ''."""
        for part in (self.aim, self.pos, self.rig):
            if part and part.get("car"):
                return part["car"]
        return ""


RIG_STOP_WORDS = ("fov", "roll", "ease", "near", "far", "look", "pos", "yaw", "pitch", "attach", "orbit")


def parse_cam_words(t, line, words, spots):
    """The words after `cam <T>` into a CamKey (camera.h THE KEYS, plus the rig sugar of DIRECTOR-DESIGN 4.3). A word it cannot read sets .problem (the lint says why)."""
    k = CamKey(t, line, list(words), "cam " + " ".join(words))
    i = 0
    try:
        while i < len(words):
            w = words[i].lower()
            if w == "off":
                k.off = True
                i += 1
            elif w == "pos":
                v, i = read_vec(words, i + 1, spots)
                k.pos = {"kind": "world", "point": v}
            elif w == "attach":
                c = car_ref(words[i + 1]) if i + 1 < len(words) else None
                if not c:
                    raise WordError("attach needs car:<id>")
                off, i = read_offset(words, i + 2)
                k.pos = {"kind": "attach", "car": c, "point": off or (0.0, 2.0, -6.0), "heading_only": False, "follow": 0.0}
            elif w == "orbit":
                c = car_ref(words[i + 1]) if i + 1 < len(words) else None
                if not c:
                    raise WordError("orbit needs car:<id>")
                k.pos = {"kind": "orbit", "car": c, "r": 8.0, "az": 0.0, "el": 10.0, "point": (0.0, 0.0, 0.0), "follow": 0.0}
                i += 2
            elif w in ("r", "az", "el") and k.pos and k.pos["kind"] == "orbit":
                v = num_or_none(words[i + 1]) if i + 1 < len(words) else None
                if v is None:
                    raise WordError(f"{w} needs a number")
                k.pos[w] = v
                i += 2
            elif w == "heading":
                if k.pos:
                    k.pos["heading_only"] = True
                i += 1
            elif w in ("follow", "aimfollow"):
                v = num_or_none(words[i + 1]) if i + 1 < len(words) else None
                if v is None:
                    raise WordError(f"{w} needs seconds")
                if w == "follow" and k.pos:
                    k.pos["follow"] = v
                i += 2
            elif w == "at" and k.pos and k.pos["kind"] == "orbit":
                off, i = read_offset(words, i + 1)
                if off is None:
                    raise WordError("at needs dx dy dz")
                k.pos["point"] = off
            elif w == "yaw":
                v = num_or_none(words[i + 1]) if i + 1 < len(words) else None
                if v is None and i + 1 < len(words) and SPOT_REF.match(words[i + 1]):
                    try:
                        v = resolve_spot_ref(words[i + 1], spots)[1]
                    except ToolError as e:
                        raise WordError(e.msg)
                if v is None:
                    raise WordError("yaw needs degrees")
                k.aim = dict(k.aim or {}, kind="angles", yaw=v)
                k.aim.setdefault("pitch", 0.0)
                i += 2
            elif w == "pitch":
                v = num_or_none(words[i + 1]) if i + 1 < len(words) else None
                if v is None:
                    raise WordError("pitch needs degrees")
                k.aim = dict(k.aim or {"kind": "angles", "yaw": 0.0}, pitch=v)
                i += 2
            elif w == "look":
                c = car_ref(words[i + 1]) if i + 1 < len(words) else None
                if c:
                    off, i = read_offset(words, i + 2)
                    k.aim = {"kind": "car", "car": c, "offset": off or (0.0, 0.0, 0.0)}
                else:
                    v, i = read_vec(words, i + 1, spots)
                    k.aim = {"kind": "point", "point": v}
            elif w in ("roll", "fov", "near", "far"):
                v = num_or_none(words[i + 1]) if i + 1 < len(words) else None
                if v is None:
                    raise WordError(f"{w} needs a number")
                if w == "roll":
                    k.roll = v
                elif w == "fov":
                    k.fov = v
                i += 2
            elif w == "ease":
                k.ease = words[i + 1].lower() if i + 1 < len(words) else "linear"
                i += 2
            elif w == "rig":
                k.rig = {"kind": words[i + 1].lower() if i + 1 < len(words) else "", "car": "", "at": None, "path": "", "params": {}}
                i += 2
                while i < len(words) and words[i].lower() not in RIG_STOP_WORDS:
                    x = words[i]
                    xl = x.lower()
                    if car_ref(x):
                        k.rig["car"] = car_ref(x)
                        i += 1
                    elif xl == "track" and i + 1 < len(words) and car_ref(words[i + 1]):
                        k.rig["car"] = car_ref(words[i + 1])
                        i += 2
                    elif xl == "at":
                        k.rig["at"], i = read_vec(words, i + 1, spots)
                    elif xl == "path" and i + 1 < len(words):
                        k.rig["path"] = words[i + 1]
                        i += 2
                    elif i + 1 < len(words):
                        k.rig["params"][xl] = words[i + 1]
                        i += 2
                    else:
                        i += 1
            else:
                raise WordError(f"unknown word '{words[i]}'")
    except WordError as ex:
        k.problem = str(ex)
    return k


@dataclass
class CarInfo:
    id: str
    line: int
    t: float
    kind: str = "spawn"        # spawn | ambient | player
    model: str = ""
    slot: int = -1
    pos: tuple = None          # (x, y, z) world
    near: tuple = None         # (car id, (right, up, forward))
    heading: float = None
    paths: list = field(default_factory=list)   # [(t, route name, SpeedSpec, mode, line)]
    stops: list = field(default_factory=list)   # [t]
    delete_t: float = None
    problem: str = ""


class SceneModel:
    """A scene file read for what the tool needs: the cars (spawn, route, stop, delete), the camera keys and cuts, the capture span, the end, the step. Built on scene_check's
    parser (the events with their words); spot references are resolved from spots.txt. Pure: no game."""

    def __init__(self, path, text, check_module=None, spots=None):
        self.path = Path(path) if path else None
        self.text = text
        self.dir = self.path.parent if self.path else Path(".")
        sc = check_module or scene_check_module()
        if spots is None:
            spots, _ = load_spots(self.dir)
        self.spots = spots
        self.parsed = sc.Parser().run(expand_spots(text, spots)[0])
        self.sc = sc
        self.name = self.parsed.name
        self.fps = self.parsed.fps
        self.step = self.parsed.step or (1.0 / self.parsed.fps if self.parsed.fps else 1.0 / 60.0)
        self.end = self.parsed.end if self.parsed.end >= 0 else None
        self.events = self.parsed.sorted_events()
        self.cam_keys, self.cuts, self.cars, self.problems = [], [], {}, []
        self.capture_on = None
        self.capture_off = None
        self.capture_lines = []
        self._samplers = {}
        for e in self.events:
            if e.track == "cam":
                self.cam_keys.append(parse_cam_words(e.t, e.line, e.words, self.spots))
            elif e.track == "cut":
                self.cuts.append(e.t)
            elif e.track == "car":
                self._car_event(e)
            elif e.track == "capture":
                self.capture_lines.append(e.line)
                if e.state and self.capture_on is None:
                    self.capture_on = e.t
                if not e.state and self.capture_on is not None and self.capture_off is None:
                    self.capture_off = e.t
        for k in self.cam_keys:
            if k.problem:
                self.problems.append(f"line {k.line}: {k.problem}")

    # ---- cars
    def _car_event(self, e):
        w = e.words
        cid, verb = w[0], w[1].lower()
        try:
            if verb in ("spawn", "ambient"):
                c = CarInfo(cid, e.line, e.t, "ambient" if verb == "ambient" else "spawn")
                i = 2
                if verb == "spawn" and i + 1 < len(w) and w[i].lower() in ("profile", "garage", "slot") and num_or_none(w[i + 1]) is not None:
                    c.slot, c.model, i = int(float(w[i + 1])), f"profile {int(float(w[i + 1]))}", i + 2
                elif verb == "spawn" and i < len(w) and re.fullmatch(r"(garage|slot):\d+", w[i].lower()):
                    c.slot = int(w[i].split(":")[1])
                    c.model, i = f"profile {c.slot}", i + 1
                elif i < len(w):
                    c.model, i = w[i], i + 1
                while i < len(w):
                    key = w[i].lower()
                    if key == "at":
                        c.pos, i = read_vec(w, i + 1, self.spots)
                    elif key == "near" and i + 1 < len(w):
                        off, j = read_offset(w, i + 2)
                        c.near, i = (w[i + 1], off or (0.0, 0.0, 0.0)), j
                    elif key == "heading" and i + 1 < len(w):
                        sp = resolve_spot_ref(w[i + 1], self.spots) if SPOT_REF.match(w[i + 1]) else None
                        c.heading = sp[1] if sp else num_or_none(w[i + 1])
                        i += 2
                    elif key in ("paint", "colour", "color"):
                        i += 2
                    else:
                        i += 1
                self.cars[cid] = c
            elif verb == "path":
                c = self.cars.get(cid)
                if c is None:
                    return
                speed, mode, j = SpeedSpec(), "kinematic", 3
                while j < len(w):
                    if w[j].lower() == "speed" and j + 1 < len(w):
                        speed = parse_speed_word(w[j + 1])
                        j += 2
                    elif w[j].lower() == "mode" and j + 1 < len(w):
                        mode, j = w[j + 1].lower(), j + 2
                    else:
                        j += 1
                c.paths.append((e.t, w[2], speed, mode, e.line))
            elif verb == "stop":
                if cid in self.cars:
                    self.cars[cid].stops.append(e.t)
            elif verb == "delete":
                if cid in self.cars and self.cars[cid].delete_t is None:
                    self.cars[cid].delete_t = e.t
            elif cid == "player" and verb == "at":
                c = CarInfo("player", e.line, e.t, "player")
                c.pos, i = read_vec(w, 2, self.spots)
                if i + 1 < len(w) and w[i].lower() == "heading":
                    c.heading = num_or_none(w[i + 1])
                self.cars["player"] = c
        except (WordError, ToolError) as ex:
            self.problems.append(f"line {e.line}: {getattr(ex, 'msg', ex)}")

    def route_sampler(self, name, spec):
        key = (name, spec.kind, spec.value)
        if key not in self._samplers:
            found = None
            try:
                found, _ = self.sc.find_route_file(self.dir, name)
            except Exception:
                found = None
            if found is None:
                for base in (self.dir, self.dir / "routes", SCENES / "routes", HERE / "routes"):
                    for nm in (name, name + ".route"):
                        if (base / nm).is_file():
                            found = base / nm
                            break
                    if found:
                        break
            try:
                self._samplers[key] = RouteSampler(read_route(found), spec) if found else None
            except ToolError:
                self._samplers[key] = None
        return self._samplers[key]

    def car_base(self, cid, _seen=()):
        """(x, y, z, heading) where the car starts: its `at`, or its `near` another car's place at that time."""
        c = self.cars.get(cid)
        if c is None:
            return None
        if c.pos is not None:
            return c.pos[0], c.pos[1], c.pos[2], (c.heading if c.heading is not None else 0.0)
        if c.near and c.near[0] not in _seen and c.near[0] != cid:
            if c.near[0] == "player":
                return None
            p = self.car_pose(c.near[0], c.t, _seen=_seen + (cid,))
            if p:
                r, u, f = c.near[1]
                x, y, z = offset_in_frame((p["x"], p["y"] or 0.0, p["z"]), p["heading"], r, u, f)
                return x, y, z, (c.heading if c.heading is not None else p["heading"])
        return None

    def car_pose(self, cid, t, _seen=()):
        """{x, y, z, heading, speed, on_path} of a scene car at scene time t (a kinematic car's place is a pure function of the time), or None before it exists / after delete."""
        c = self.cars.get(cid)
        if c is None or t < c.t - 1e-9 or (c.delete_t is not None and t >= c.delete_t):
            return None
        base = self.car_base(cid, _seen)
        if base is None:
            return None
        x, y, z, h = base
        pose = {"x": x, "y": y, "z": z, "heading": h, "speed": 0.0, "on_path": False}
        active = None
        for pt in c.paths:
            if pt[0] <= t + 1e-9 and pt[3] != "brain":
                active = pt
        if active is not None:
            t0, name, spec = active[0], active[1], active[2]
            stop = next((s for s in c.stops if t0 <= s <= t), None)
            tau = (stop if stop is not None else t) - t0
            rs = self.route_sampler(name, spec)
            if rs is not None:
                p = rs.at_time(tau)
                pose.update(x=p["x"], z=p["z"], y=(p["y"] if p["y"] is not None else y), heading=p["heading"], speed=(0.0 if stop is not None else p["speed"]), on_path=True)
        return pose

    def brain_cars(self):
        return sorted({cid for cid, c in self.cars.items() for p in c.paths if p[3] == "brain"})

    # ---- the camera
    def active_cam_key(self, t):
        """The last cam key at or before t (None: the game's camera)."""
        cur = None
        for k in self.cam_keys:
            if k.t <= t + 1e-9:
                cur = k
        return None if cur is None or cur.off else cur

    def subject_at(self, t):
        """The id of the car a shot at t is about: the latest key (not past a `cam off`) that names one, else the only scene car, else ''."""
        for k in reversed([k for k in self.cam_keys if k.t <= t + 1e-9]):
            if k.off:
                break
            if k.car() and k.car() in self.cars:
                return k.car()
        ids = [cid for cid in self.cars if cid != "player"]
        return ids[0] if len(ids) == 1 else ""

    def _key_pos(self, k, tt):
        p = k.pos
        if p is None and k.rig:
            rig = k.rig
            if rig["at"]:
                return rig["at"]
            if rig["kind"] == "flyby" and rig["path"]:   # a place that moves along a route from this key's time (camera.h: rig flyby)
                try:
                    spec = parse_speed_word(rig["params"]["speed"]) if "speed" in rig["params"] else SpeedSpec()
                except ToolError:
                    spec = SpeedSpec()
                rs = self.route_sampler(rig["path"], spec)
                if rs is None:
                    return None
                pt = rs.at_time(max(0.0, tt - k.t))
                if "y" in rig["params"] and num_or_none(rig["params"]["y"]) is not None:
                    y = float(rig["params"]["y"])
                else:
                    y = (pt["y"] if pt["y"] is not None else 0.0) + (num_or_none(rig["params"].get("up", "2")) or 2.0)
                return (pt["x"], y, pt["z"])
            car = self.car_pose(rig["car"], tt) if rig["car"] else None
            if car is None:
                return None
            base = (car["x"], car["y"] or 0.0, car["z"])
            dist = num_or_none(rig["params"].get("dist", "6")) or 6.0
            height = num_or_none(rig["params"].get("height", "1.8")) or 1.8
            side = num_or_none(rig["params"].get("side", "0")) or 0.0
            if rig["kind"] in ("chase", ""):
                return offset_in_frame(base, car["heading"], side, height, -dist)
            if rig["kind"] == "orbit":
                r = num_or_none(rig["params"].get("r", "8")) or 8.0
                az = num_or_none(str(rig["params"].get("az", "0")).split("..")[0]) or 0.0
                el = num_or_none(rig["params"].get("el", "10")) or 10.0
                return self._orbit(car, r, az, el, (0, 0, 0))
            return offset_in_frame(base, car["heading"], side, height, 0.0)
        if p is None:
            return None
        if p["kind"] == "world":
            return p["point"]
        car = self.car_pose(p["car"], tt)
        if car is None:
            return None
        if p["kind"] == "attach":
            return offset_in_frame((car["x"], car["y"] or 0.0, car["z"]), car["heading"], *p["point"])
        return self._orbit(car, p["r"], p["az"], p["el"], p["point"])

    def est_camera(self, t):
        """An ESTIMATE of the camera at t from the keys (linear between positions; the engine's spline and lag are not here): {pos, yaw, pitch, fov, kind} or None when the
        game's own camera is on. For the map before a game has said where the camera really was."""
        if self.active_cam_key(t) is None:
            return None
        allk = [k for k in self.cam_keys if (k.pos or k.rig) and not k.off]
        prev = [k for k in allk if k.t <= t + 1e-9]
        nxt = [k for k in allk if k.t > t + 1e-9]
        if not prev:
            return None
        a = prev[-1]
        pos = self._key_pos(a, t)
        if nxt and nxt[0].t > a.t and not any(a.t < c <= nxt[0].t for c in self.cuts):
            pb = self._key_pos(nxt[0], t)
            if pos and pb:
                u = (t - a.t) / (nxt[0].t - a.t)
                pos = tuple(lerp(pos[i], pb[i], u) for i in range(3))
        if pos is None:
            return None
        yaw, pitch, tgt, look = 0.0, 0.0, None, None
        before = [k for k in self.cam_keys if k.t <= t + 1e-9]
        for k in reversed(before):
            if k.off:
                break
            if k.aim:
                look = k.aim
                break
            if k.rig and k.rig["car"]:
                look = {"kind": "car", "car": k.rig["car"], "offset": (0.0, 0.5, 0.0)}
                break
        if look is not None:
            if look["kind"] == "angles":
                yaw, pitch = look.get("yaw", 0.0), look.get("pitch", 0.0)
            elif look["kind"] == "point":
                tgt = look["point"]
            else:
                car = self.car_pose(look["car"], t)
                if car:
                    tgt = offset_in_frame((car["x"], car["y"] or 0.0, car["z"]), car["heading"], *look["offset"])
        elif a.pos and a.pos["kind"] in ("attach", "orbit"):
            car = self.car_pose(a.pos["car"], t)
            if car and a.pos["kind"] == "attach":
                yaw = car["heading"]
            elif car:
                tgt = offset_in_frame((car["x"], car["y"] or 0.0, car["z"]), car["heading"], *a.pos["point"])
        if tgt is not None:
            dx, dy, dz = tgt[0] - pos[0], tgt[1] - pos[1], tgt[2] - pos[2]
            yaw = heading_of(dx, dz)
            pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
        fov = next((k.fov for k in reversed(before) if k.fov is not None), None)
        return {"pos": [round(v, 3) for v in pos], "yaw": round(yaw, 2), "pitch": round(pitch, 2), "fov": fov, "kind": "estimate"}

    @staticmethod
    def _orbit(car, r, az, el, target_off):
        tx, ty, tz = offset_in_frame((car["x"], car["y"] or 0.0, car["z"]), car["heading"], *target_off)
        ce = math.cos(math.radians(el))
        # az 0 behind, 90 on the right (camera.h OrbitPoint), in the heading frame
        return offset_in_frame((tx, ty, tz), car["heading"], r * ce * math.sin(math.radians(az)), r * math.sin(math.radians(el)), -r * ce * math.cos(math.radians(az)))

    def duration(self):
        if self.end is not None:
            return self.end
        last = max([e.t for e in self.events] or [0.0])
        return last + 2.0

    def time_range(self):
        """(from, to): where the picture matters: the take (capture on .. off), else the first camera key .. `cam off`, else the whole scene."""
        end = self.duration()
        if self.capture_on is not None:
            return self.capture_on, min(end, self.capture_off if self.capture_off is not None else end)
        first = next((k.t for k in self.cam_keys if not k.off), None)
        if first is not None:
            off = next((k.t for k in self.cam_keys if k.off and k.t > first), end)
            return first, min(end, off)
        return 0.0, end

    def routes(self):
        """[(car id, route name, [(x, z)], speeds, mode, line)] of the routes the cars drive (for the map and the road check)."""
        out = []
        for cid, c in self.cars.items():
            for (t0, name, spec, mode, line) in c.paths:
                rs = self.route_sampler(name, spec)
                if rs is not None:
                    out.append((cid, name, rs.polyline(), list(rs.v), mode, line))
        return out


# ------------------------------------------------------------------------------------------------------------------------------- the command channel (client)
@dataclass
class Reply:
    ok: bool
    result: object = None
    error: dict = None
    raw: dict = None

    @property
    def code(self):
        return (self.error or {}).get("code", "")

    @property
    def msg(self):
        return (self.error or {}).get("msg", "")


_DC_MOD = []


def director_client_module():
    """director_client.py (the wire client and its mock server, documentation\\DIRECTOR-PROTOCOL.md), loaded by path."""
    if not _DC_MOD:
        f = HERE / "director_client.py"
        if not f.is_file():
            raise ToolError("no_game", f"{f} is not there")
        _DC_MOD.append(sys.modules.get("director_client") or load_module(f, "director_client"))
    return _DC_MOD[0]


class Channel:
    """One connection to a game's command channel, on top of director_client.DirectorClient: requests give a Reply (ok or not), `call` raises ToolError with the game's own code
    and sentence, `wait`/`timeout_s` are the wire's request fields (DIRECTOR-PROTOCOL.md), events are read in order with wait_event / drain."""

    def __init__(self, info):
        dc = director_client_module()
        self.info = info
        try:
            self.dc = dc.DirectorClient("127.0.0.1", int(info["port"]), info["token"], name="scene_tool", timeout=10.0)
        except dc.DirectorError as e:
            raise ToolError("refused" if e.code == "refused" else e.code, f"the game's channel refused us ({e.code}: {e.msg})")
        except OSError as ex:
            raise ToolError("unreachable", f"cannot connect to the game's channel at 127.0.0.1:{info['port']} ({ex}): is the game still running? (scene_tool.py slots)")
        self.hello = self.dc.hello

    def request(self, cmd, args=None, timeout=30.0, wait=None, timeout_s=None):
        """Sends one command and waits for its reply: a Reply, ok or not. ToolError timeout / unreachable when there is none."""
        dc = director_client_module()
        try:
            raw = self.dc.request(cmd, args or {}, wait=wait, timeout_s=timeout_s, timeout=timeout)
        except dc.DirectorError as e:
            raise ToolError("timeout" if e.code == "timeout" else "unreachable", f"'{cmd}': {e.msg}")
        if raw.get("ok"):
            return Reply(True, raw.get("result"), None, raw)
        err = raw.get("error") if isinstance(raw.get("error"), dict) else {"code": "failed", "msg": str(raw.get("error"))}
        return Reply(False, None, err, raw)

    def call(self, cmd, args=None, timeout=30.0, wait=None, timeout_s=None):
        """The result of a command that has to work: ToolError with the game's own code and sentence when it says no."""
        r = self.request(cmd, args, timeout, wait, timeout_s)
        if not r.ok:
            raise ToolError(r.code or "failed", f"{cmd}: {r.msg}", exit_code=EXIT_ENV)
        return r.result if r.result is not None else {}

    def wait_event(self, name, pred=None, timeout=30.0):
        """The next event called `name` (for which pred(event) is true), or None after `timeout` seconds. Events before it are dropped."""
        dc = director_client_module()
        try:
            return self.dc.wait_event(name, pred, timeout)
        except dc.DirectorError:
            return None

    def drain(self):
        return list(self.dc.events())

    def close(self):
        try:
            self.dc.close()
        except Exception:
            pass


def read_director_info(path):
    p = Path(path).resolve()
    try:
        info = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as ex:
        raise ToolError("not_found", f"{p}: cannot read Director connection file ({ex})") from ex
    if (not isinstance(info, dict) or type(info.get("port")) is not int
            or not 1 <= info["port"] <= 65535 or not isinstance(info.get("token"), str)
            or not info["token"]):
        raise ToolError("bad_args", f"{p}: expected a Director connection file with a port and token")
    info["_path"] = str(p)
    info["_alive"] = director_client_module().pid_alive(info.get("pid"))
    return info


def list_infos(game_dir=None, attach=None):
    """Look for a Director connection in the current directory and game directory."""
    paths = [Path.cwd() / "director.json"]
    if game_dir:
        paths.append(Path(game_dir).expanduser() / "director.json")
    out = {}
    for path in paths:
        try:
            info = read_director_info(path)
        except ToolError:
            continue
        out[os.path.normcase(info["_path"])] = info
    if attach:
        info = resolve_attach(attach)
        out[os.path.normcase(info["_path"])] = info
    return list(out.values())


def resolve_attach(path=None, game_dir=None):
    if path:
        p = Path(path).expanduser()
        return read_director_info(p / "director.json" if p.is_dir() else p)
    live = [i for i in list_infos(game_dir) if i["_alive"]]
    if len(live) == 1:
        return live[0]
    if live:
        raise ToolError("bad_args", "several live Director connections found; select one with --attach: "
                        + ", ".join(i["_path"] for i in live))
    raise ToolError("no_game", "no live director.json found; use --game-dir or --attach, or start the game with launch --exe FILE")


def open_channel(info):
    if not director_client_module().pid_alive(info.get("pid")):
        raise ToolError("no_game", f"{info['_path']}: game PID {info.get('pid')} is no longer running")
    if info.get("proto", PROTO) != PROTO:
        print(f"scene_tool: the game speaks protocol {info.get('proto')}, this tool {PROTO}: commands may differ", file=sys.stderr)
    return Channel(info)


def wait_world(ch, timeout=300.0, poll=1.0):
    """Wait for the city to load. The transport also answers on the title screen."""
    deadline = time.monotonic() + timeout
    last = ""
    while True:
        r = ch.request("transport.get", None, timeout=15.0)
        if r.ok:
            res = r.result or {}
            world = res.get("world")
            if world is None:
                c = ch.request("cars.list", None, timeout=15.0)
                world = bool(c.ok and any(x.get("id") == "player" for x in ((c.result or {}).get("cars") or [])))
            if world:
                return res
            last = "no city loaded yet (the title screen, a menu or a loading screen)"
        else:
            last = f"{r.code}: {r.msg}"
            if r.code != "no_world":
                raise ToolError(r.code or "failed", f"transport.get: {r.msg}", exit_code=EXIT_ENV)
        if time.monotonic() >= deadline:
            raise ToolError("timeout", f"the game has no world after {timeout:g} s ({last}); is it past the title screen?")
        time.sleep(poll)


def parse_cmd_line(text):
    """The console grammar of the panel: `camera.set pos=1,2,3 yaw=80 fov=40`, or a whole JSON request {"cmd":..,"args":..}. -> (cmd, args dict). Values: JSON when they
    parse (numbers, true, [..], {..}, "text"), a comma list of numbers as an array, else a plain string."""
    t = text.strip()
    if t.startswith("{"):
        try:
            obj = json.loads(t)
        except ValueError as ex:
            raise ToolError("bad_args", f"not JSON: {ex}")
        if "cmd" not in obj:
            raise ToolError("bad_args", 'a JSON request needs "cmd"')
        return obj["cmd"], obj.get("args") or {}
    try:
        words = shlex.split(t, posix=False)
    except ValueError as ex:
        raise ToolError("bad_args", f"cannot read the command line: {ex}")
    if not words:
        raise ToolError("bad_args", "an empty command")
    args = {}
    for w in words[1:]:
        if "=" not in w:
            raise ToolError("bad_args", f"'{w}': arguments are key=value (camera.set pos=1,2,3 yaw=80)")
        k, _, v = w.partition("=")
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] == '"':
            args[k] = v[1:-1]
            continue
        try:
            args[k] = json.loads(v)
            continue
        except ValueError:
            pass
        parts = v.split(",")
        nums = [num_or_none(x) for x in parts]
        if len(parts) > 1 and all(x is not None for x in nums):
            args[k] = [int(x) if x == int(x) and "." not in p else x for x, p in zip(nums, parts)]
        else:
            args[k] = v
    return words[0], args


# ------------------------------------------------------------------------------------------------------------------------------- starting a game
def add_game_args(p, *, work_dir=False, wait=False):
    p.add_argument("--attach", metavar="FILE_OR_DIR", help="a director.json file or its directory")
    p.add_argument("--game-dir", help="also look here for director.json")
    if work_dir:
        p.add_argument("--work-dir", help="local staging folder (default: director-work beside director.json)")
    if wait:
        p.add_argument("--world-timeout-s", type=positive_seconds, default=120.0, help="seconds to wait for the city to load")


def positive_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("expected a finite positive number")
    return seconds


def wait_info(proc, info_path, timeout):
    deadline = time.monotonic() + timeout
    while True:
        rc = proc.poll()
        if rc is not None:
            raise ToolError("no_game", f"the game exited with code {rc} before its Director was ready")
        try:
            info = read_director_info(info_path)
        except ToolError:
            pass
        else:
            if info.get("pid") == proc.pid:
                ch = open_channel(info)
                ch.close()
                return info
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ToolError("timeout", f"the game did not publish its Director at {info_path} within {timeout:g} s")
        time.sleep(min(0.1, remaining))


def cmd_launch(a):
    exe = Path(a.exe).expanduser().resolve()
    if not exe.is_file():
        raise ToolError("not_found", f"game executable not found: {exe}")
    info_path = exe.parent / "director.json"
    try:
        existing = read_director_info(info_path)
    except ToolError:
        existing = None
    if existing and existing["_alive"]:
        raise ToolError("bad_args", f"a process already owns {info_path}; attach to it or close it first")
    root = Path(a.work_dir).expanduser().resolve() if a.work_dir else exe.parent / WORK_FOLDER
    cmd = [str(exe), "--mcla_director=true", f"--mcla_director_info={info_path}",
           f"--mcla_director_root={root}", f"--mcla_capture_dir={root / 'captures'}"]
    if a.scene:
        cmd.append(f"--mcla_scene={resolve_scene(a.scene)}")
    if a.ffmpeg:
        sr = load_module(SCENE_RUN, "scene_run_mod")
        cmd.append(f"--mcla_capture_ffmpeg={sr.find_ffmpeg(a.ffmpeg)}")
    root.mkdir(parents=True, exist_ok=True)
    log = root / "launch.log"
    # CHECK: confirm launch and the game's connection file with a local game build.
    with log.open("ab") as output:
        proc = subprocess.Popen(cmd, cwd=exe.parent, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT,
                                start_new_session=(os.name != "nt"))
    try:
        info = wait_info(proc, info_path, a.timeout_s)
    except (ToolError, OSError, KeyboardInterrupt) as ex:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
        if isinstance(ex, ToolError):
            raise ToolError(ex.code, f"{ex.msg}; game output: {log}") from ex
        raise
    return emit(a, {"ok": True, "info": info["_path"], "pid": proc.pid, "work_dir": str(root), "log": str(log)},
                [f"Director ready (PID {proc.pid}): {info_path}", f"work folder: {root}"], EXIT_OK)


class Session:
    """Open a connection to a running game for one CLI operation."""

    def __init__(self, a):
        self.a = a

    def __enter__(self):
        self.info = resolve_attach(self.a.attach, self.a.game_dir)
        self.ch = open_channel(self.info)
        return self

    def root(self):
        work_dir = getattr(self.a, "work_dir", None)
        return (Path(work_dir).expanduser().resolve() if work_dir
                else Path(self.info["_path"]).parent / WORK_FOLDER)

    def stage(self, path, label, text, linted):
        root = self.root()
        root.mkdir(parents=True, exist_ok=True)
        return stage_scene(path, Path(tempfile.mkdtemp(prefix=f"{label}-", dir=root)), text, linted)

    def __exit__(self, *exc):
        self.ch.close()


def lint_one(path, src=None, cars=None, no_src=False, *, text=None):
    """The lint of one scene file as a dict {file, name, end, keys, errors, warnings, diagnostics:[{line, severity, message, source}]} (scene_check.py's own checks, then the
    spot names when scene_check has no spot rule yet)."""
    sc = scene_check_module()
    if text is None:
        text = Path(path).read_text(encoding="utf-8")
    cvars = None
    if not no_src:
        s = src or DEFAULT_SRC
        if Path(s).is_dir():
            cvars = sc.scan_cvars(s)
    table, spot_problems = sc.load_spots_for(Path(path))
    extra = [{"line": 0, "severity": "error", "message": m} for m in spot_problems]
    scene = sc.lint(text, cvars, cars, table)
    sc.check_routes(scene, Path(path))
    diags = [{"line": d.line, "severity": d.severity, "message": d.message} for d in scene.diags] + extra
    seen = set()
    diags = [d for d in diags if not ((d["line"], d["message"]) in seen or seen.add((d["line"], d["message"])))]
    diags.sort(key=lambda d: (d["line"], d["severity"] != "error"))
    lines = text.split("\n")
    for d in diags:
        d["source"] = lines[d["line"] - 1].rstrip("\r") if 1 <= d["line"] <= len(lines) else ""
    errors = sum(1 for d in diags if d["severity"] == "error")
    return {"file": str(path), "name": scene.name, "end": scene.end if scene.end >= 0 else None, "keys": len(scene.events), "errors": errors,
            "warnings": len(diags) - errors, "diagnostics": diags, "_scene": scene}


def cmd_lint(a):
    cars = scene_check_module().load_cars(a.cars) if a.cars else None
    files, lines, bad = [], [], False
    for f in a.files:
        p = resolve_scene(f)
        r = lint_one(p, a.src, cars, a.no_src)
        scene = r.pop("_scene")
        files.append(r)
        for d in r["diagnostics"]:
            lines.append(f"{p.name}:{d['line']}: {d['severity']}: {d['message']}" if d["line"] > 0 else f"{p.name}: {d['severity']}: {d['message']}")
            if d["severity"] == "error" and d["source"].strip():
                lines.append(f"  {d['line']:>4} | {d['source'].strip()[:160]}")
        if a.list:
            sc = scene_check_module()
            for e in scene.sorted_events():
                lines.append(f"  t={e.t:8.2f}  line {e.line:<4} {sc.describe(e)}")
        lines.append(f"{p.name}: {r['errors']} error(s), {r['warnings']} warning(s); {r['keys']} keys; " + (f"ends at {fmt(r['end'])} s" if r["end"] is not None else "no end"))
        bad = bad or r["errors"] > 0 or (a.strict and r["warnings"] > 0)
    result = {"ok": not bad, "errors": sum(f["errors"] for f in files), "warnings": sum(f["warnings"] for f in files), "files": files}
    return emit(a, result, lines, EXIT_FINDINGS if bad else EXIT_OK)


# ------------------------------------------------------------------------------------------------------------------------------- query
LIVE_ONLY = ("pick", "streaming", "visible")
QUERY_WHATS = ("spots", "lights", "road", "route", "garages", "cars", "ground", "pick", "streaming", "visible")


def parse_near(word, spots):
    """--near: spot:NAME[@r,u,f], x,z or x,y,z -> [x, z]."""
    x, _, z, _ = resolve_point(word, spots)
    return [x, z]


def query_args(what, a, near, spots):
    """The arguments of query.<what> (DIRECTOR-ATLAS-NOTES 2: the same shapes offline, answered by atlas.py, and live, answered by the game)."""
    args = {}
    if what in ("lights", "spots"):
        if near is None and what == "lights":
            raise ToolError("bad_args", "query lights needs --near spot:NAME | x,z (and --r metres)")
        if near is not None:
            args = {"near": near, "r": a.r if a.r is not None else 60.0}
        if what == "lights":
            args["n"] = a.limit
    elif what == "road":
        if near is None:
            raise ToolError("bad_args", "query road needs --near spot:NAME | x,z")
        args = {"pos": near, "r": a.r if a.r is not None else 40.0, "n": min(a.limit, 6)}
    elif what == "route":
        if not a.from_ or not a.to:
            raise ToolError("bad_args", "query route needs --from and --to (spots or x,z), or --file <route file> for a file's numbers")
        pt = lambda w: [resolve_point(w, spots)[0], resolve_point(w, spots)[2]]
        args = {"from": pt(a.from_), "to": pt(a.to), "via": [pt(v) for v in a.via or []]}
    elif what == "ground":
        if near is None:
            raise ToolError("bad_args", "query ground needs --near x,z (or a spot)")
        args = {"x": near[0], "z": near[1]}
    elif what == "cars":
        args = {"search": a.name} if a.name else {}
    elif what == "pick":
        if not a.uv:
            raise ToolError("bad_args", "query pick needs --uv u,v (0..1 of the picture)")
        u, v = (float(x) for x in a.uv.split(","))
        args = {"u": u, "v": v}
    elif what == "visible":
        args = {"subject": a.subject or "car:p1"}
    for kv in a.arg or []:
        k, _, v = kv.partition("=")
        try:
            args[k] = json.loads(v)
        except ValueError:
            args[k] = v
    return args


def cmd_query(a):
    spots, sp_problems = load_spots(None, a.spots)
    what = a.what
    near = parse_near(a.near, spots) if a.near else None
    if what == "route" and a.file:
        pts = read_route(a.file)
        rs = RouteSampler(pts)
        xs, zs = [p.x for p in pts], [p.z for p in pts]
        item = {"file": str(a.file), "points": len(pts), "length_m": round(rs.length(), 1), "duration_s": round(rs.duration(), 1), "speed_min": round(min(rs.v), 1),
                "speed_max": round(max(rs.v), 1), "bbox": [min(xs), min(zs), max(xs), max(zs)], "has_height": rs.has_y}
        at = find_atlas(a.atlas or None, need_rails=True)
        if at is not None:
            any_lane = [atlas_road_dist(at, p.x, p.z) for p in pts]
            own_lane = []
            for i, p in enumerate(pts):
                q0, q1 = pts[max(0, i - 1)], pts[min(len(pts) - 1, i + 1)]
                own_lane.append(atlas_road_dist(at, p.x, p.z, 150.0, heading_of(q1.x - q0.x, q1.z - q0.z)))
            item["max_dist_to_road_m"], item["mean_dist_to_road_m"] = round(max(any_lane), 1), round(sum(any_lane) / len(any_lane), 1)
            item["max_dist_to_lane_m"], item["mean_dist_to_lane_m"] = round(max(own_lane), 1), round(sum(own_lane) / len(own_lane), 1)   # a lane that runs the route's way
        return emit(a, {"ok": True, "what": what, "source": "route file", "result": item}, [json.dumps(item)], EXIT_OK)
    args = query_args(what, a, near, spots)
    if a.live:
        with Session(a) as s:
            res = s.ch.request(f"query.{what}", args, timeout=a.timeout_s)
        if not res.ok:
            raise ToolError(res.code or "failed", f"query.{what}: {res.msg}")
        result, source = res.result, "live"
    elif what in LIVE_ONLY:
        raise ToolError("bad_args", f"query {what} reads the running world: add --live and select a running Director with --attach or --game-dir")
    elif what == "spots":
        items = []
        for s in spots.values():
            if a.name and a.name.lower() not in s.name.lower():
                continue
            d = None if near is None else dist_xz((s.x, s.z), near)
            if d is not None and d > args["r"]:
                continue
            items.append({"name": s.name, "pos": [s.x, s.y, s.z], "heading": s.heading, "text": s.text, **({"dist": round(d, 1)} if d is not None else {})})
        items.sort(key=lambda i: (i.get("dist", 0), i["name"]))
        result, source = {"spots": items}, "spots.txt"
    else:
        at = find_atlas(a.atlas or None)
        if at is None:
            raise ToolError("no_atlas", f"no atlas under {ATLAS_ROOT}: build one (python atlas.py build --game <folder>, or the game's atlas.dump) or add --live")
        ans = atlas_answer(at, what, args, spots)
        if not ans.get("ok"):
            e = ans.get("error", {})
            raise ToolError(e.get("code", "failed"), f"query.{what}: {e.get('msg', '')}")
        result, source = ans["result"], f"atlas {atlas_id(at)}"
    out = {"ok": True, "what": what, "source": source, "args": args, "result": result, "problems": sp_problems}
    lines = [f"query.{what} ({source}): {json.dumps(args)}"]
    listy = next((v for v in result.values() if isinstance(v, list)), None) if isinstance(result, dict) else None
    if listy is not None:
        lines += ["  " + json.dumps(it, ensure_ascii=False) for it in listy[:a.limit]]
        extra = {k: v for k, v in result.items() if not isinstance(v, list)}
        if extra:
            lines.append("  " + json.dumps(extra, ensure_ascii=False))
    else:
        lines.append("  " + json.dumps(result, ensure_ascii=False))
    lines += [f"  warning: {p}" for p in sp_problems]
    return emit(a, out, lines, EXIT_OK)


# ------------------------------------------------------------------------------------------------------------------------------- lamps
# `lamps <pos> [<radius>]` (RT-LIGHTS-D0 2026-11-22; the helper of the lamp shadows' and the RT lights' gates): the street lamps (and, with --src all, the fill lights) of the atlas near a place, with the ids the live
# lights list uses, and, with --cam/--look, which of them a camera sees and how many lamps light each pixel of the ground in view by the law of the heat pass (src\gpu\rt_lights_heat_rules.h, city_hit_shade_rules.h,
# citypack\lamp_table.h): the 4 a pixel cap is reached where a pixel has 4 or more lamps. OFFLINE only: the atlas knows lamps, not buildings; a pixel the view shows is judged by the GROUND PLANE (no walls, no
# occlusion: the game's own grid applies no shadow either, so the count is the game's, not the picture's).
LAMP_LIT_FLOOR = 1.0e-4      # rt_lights_heat_rules.h kLitFloor: a lamp whose luma at a pixel is under this does not count as lighting it
LAMP_REACH_K = 100.0         # city_hit_shade_rules.h kLampReach: a lamp lights out to sqrt(100 x I) (where its term is 1 per cent)
LAMP_GRID = 4                # the lamps one pixel takes (rt_lights_heat_rules.h kGridLamps: the game's grid holds 4)
_CONE_CACHE = {}


def lamp_cone_cosines(cone_deg, spread_deg):
    """lamp_table.h LampConeCosines: (cos outer, cos inner) of a spot's cone, or None for an omni light / 360 degrees (no cone)."""
    key = (cone_deg, spread_deg)
    if key not in _CONE_CACHE:
        if cone_deg >= 359.0:
            _CONE_CACHE[key] = None
        else:
            half = 0.5 * min(max(cone_deg, 0.0), 360.0)
            s = 0.5 * (1.0 if spread_deg == 0.0 else spread_deg)
            inner = outer = half
            if s >= 0.0:
                outer = half + s
            else:
                inner = half + s
            inner = min(max(inner, 0.0), 179.0)
            outer = min(max(outer, inner + 0.1), 179.9)
            _CONE_CACHE[key] = (math.cos(math.radians(outer)), math.cos(math.radians(inner)))
    return _CONE_CACHE[key]


def lamp_luma_at(l, p, normal=(0.0, 1.0, 0.0)):
    """The luma one atlas light gives a surface point (the heat pass's law: colour luma x min(1, I / d^2) x the cone x N.L, 0 beyond sqrt(100 I); the night schedule is 1). `l` is an atlas
    light {pos, color, dir, intensity, cone, spread, type}; `p` (x, y, z)."""
    inten = l["intensity"]
    if inten <= 0.0:
        return 0.0
    lp = l["pos"]
    dx, dy, dz = lp[0] - p[0], lp[1] - p[1], lp[2] - p[2]
    d2 = max(dx * dx + dy * dy + dz * dz, 1e-6)
    if d2 >= LAMP_REACH_K * inten:
        return 0.0
    d = math.sqrt(d2)
    nl = (normal[0] * dx + normal[1] * dy + normal[2] * dz) / d
    if nl <= 0.0:
        return 0.0
    cone = 1.0
    cc = lamp_cone_cosines(l.get("cone", 360.0), l.get("spread", 0.0)) if l.get("type") == "spot" else None
    if cc is not None:
        ax = l["dir"]
        alen = math.sqrt(ax[0] ** 2 + ax[1] ** 2 + ax[2] ** 2)
        if alen > 1e-3:
            c = -(ax[0] * dx + ax[1] * dy + ax[2] * dz) / (alen * d)   # the axis against the direction from the lamp to the point
            cone = min(max((c - cc[0]) / (cc[1] - cc[0]), 0.0), 1.0)
    col = l.get("color") or (1.0, 1.0, 1.0)
    luma = 0.2126 * col[0] + 0.7152 * col[1] + 0.0722 * col[2]
    return luma * (1.0 if d2 <= inten else inten / d2) * cone * nl


def camera_axes(pos, look):
    """(forward, right, up) of a camera at `pos` looking at `look` (roll 0, world up +Y): right = (-fz, 0, fx) as the heading frame's (looking along +Z the right is -X)."""
    f = [look[i] - pos[i] for i in range(3)]
    n = math.sqrt(sum(c * c for c in f))
    if n < 1e-6:
        raise ToolError("bad_args", "the camera and its look-at point are the same place")
    f = [c / n for c in f]
    hz = math.hypot(f[0], f[2])
    if hz < 1e-6:
        raise ToolError("bad_args", "the camera looks straight up or down: give a look-at point off its vertical")
    r = [-f[2] / hz, 0.0, f[0] / hz]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    return f, r, u


@dataclass
class ViewCam:
    pos: tuple
    f: tuple
    r: tuple
    u: tuple
    tan_y: float   # tan of half the vertical field of view
    tan_x: float   # the same across (the display aspect)

    @staticmethod
    def make(pos, look, fov_deg=60.0, aspect=16.0 / 9.0):
        f, r, u = camera_axes(pos, look)
        ty = math.tan(math.radians(fov_deg) * 0.5)
        return ViewCam(tuple(pos), tuple(f), tuple(r), tuple(u), ty, ty * aspect)

    def project(self, p):
        """(u, v, depth) of a world point, u across 0..1 from the left, v down 0..1 from the top; None behind the camera."""
        d = [p[i] - self.pos[i] for i in range(3)]
        z = d[0] * self.f[0] + d[1] * self.f[1] + d[2] * self.f[2]
        if z <= 0.05:
            return None
        x = d[0] * self.r[0] + d[1] * self.r[1] + d[2] * self.r[2]
        y = d[0] * self.u[0] + d[1] * self.u[1] + d[2] * self.u[2]
        return 0.5 + 0.5 * x / (z * self.tan_x), 0.5 - 0.5 * y / (z * self.tan_y), z

    def ray(self, u, v):
        """The unit direction through the picture point (u, v)."""
        a, b = (2.0 * u - 1.0) * self.tan_x, (1.0 - 2.0 * v) * self.tan_y
        d = [self.f[i] + a * self.r[i] + b * self.u[i] for i in range(3)]
        n = math.sqrt(sum(c * c for c in d))
        return [c / n for c in d]


def ground_census(cam, lights, ground_y, w=96, h=54, max_range=150.0, grid=LAMP_GRID):
    """What the heat pass's `lamps a pixel` counter would see of the GROUND in a view: every pixel of a w x h picture whose ray meets the plane y = ground_y within max_range m (the rest is sky, buildings and
    what lies beyond: not judged) is lit by the atlas lights `lights` that give it luma over the floor. -> {pixels, ground (pixels on the plane), lit, lit_share (of the whole picture), by_count {1..5, 5 = five and
    more: lit pixels by their lamps}, max_lamps, at_grid_share (of the lit pixels with `grid` or more lamps), over_grid_share (with more than `grid`), per_light {light id: pixels it lights}}."""
    by_count = {1: 0, 2: 0, 3: 0, 4: 0, 5: 0}
    per_light = {}
    n_ground = n_lit = max_lamps = 0
    near_lights = [l for l in lights if l["intensity"] > 0.0]
    for iv in range(h):
        v = (iv + 0.5) / h
        for iu in range(w):
            d = cam.ray((iu + 0.5) / w, v)
            if d[1] > -1e-3:
                continue
            t = (ground_y - cam.pos[1]) / d[1]
            if t <= 0.0 or t > max_range:
                continue
            n_ground += 1
            p = (cam.pos[0] + d[0] * t, ground_y, cam.pos[2] + d[2] * t)
            lit = 0
            for l in near_lights:
                lp = l["pos"]
                dx, dz = lp[0] - p[0], lp[2] - p[2]
                if dx * dx + dz * dz > LAMP_REACH_K * l["intensity"]:
                    continue
                if lamp_luma_at(l, p) > LAMP_LIT_FLOOR:
                    lit += 1
                    per_light[l["i"]] = per_light.get(l["i"], 0) + 1
            if lit:
                n_lit += 1
                by_count[min(lit, 5)] += 1
                max_lamps = max(max_lamps, lit)
    over = sum(c for k, c in by_count.items() if k > grid)
    at = sum(c for k, c in by_count.items() if k >= grid)
    return {"pixels": w * h, "ground": n_ground, "lit": n_lit, "lit_share": n_lit / float(w * h), "by_count": by_count, "max_lamps": max_lamps,
            "at_grid_share": at / float(n_lit) if n_lit else 0.0, "over_grid_share": over / float(n_lit) if n_lit else 0.0, "per_light": per_light}


def _first_lamp_index(at):
    cached = getattr(at, "_first_lamp", -1)
    if cached == -1:
        cached = next((l["i"] for l in at.lights if l.get("src") == "lamp"), None)
        try:
            at._first_lamp = cached
        except AttributeError:
            pass
    return cached


def lamps_near(at, x, z, r, src="lamp", min_intensity=1.0, n=100000):
    """The atlas lights (src: lamp = the street lamps, file = lights.xlm's fills, all) within r metres of (x, z), nearest first, each with `dist`, `reach_m` and `lamp` (the street lamp's number in the decode
    order, light id minus the first lamp's; None for a fill). -> (lights, how many are in the radius)."""
    ans = atlas_module().near_lights(at, [x, z], r, n, min_intensity, None, None if src == "all" else src)
    if not ans.get("ok"):
        e = ans.get("error", {})
        raise ToolError(e.get("code", "failed"), f"lamps: {e.get('msg', '')}")
    out = ans["result"]["lights"]
    first = _first_lamp_index(at)
    for l in out:
        l["lamp"] = (l["i"] - first) if l.get("src") == "lamp" and first is not None else None
    return out, ans["result"]["in_radius"]


def ground_y_near(at, cam):
    """The ground height under the camera: the nearest road's height within 25 m of the camera's place (a hill beside a road is not known offline), else 1.7 m under the camera."""
    if at is not None and getattr(at, "rails", None):
        res = atlas_module().nearest_road(at, [cam.pos[0], cam.pos[2]], 25.0, 1)
        near = (res.get("result") or {}).get("nearest") if res.get("ok") else None
        if near:
            return float(near[0]["pos"][1])
    return cam.pos[1] - 1.7


def cmd_lamps(a):
    spots, sp_problems = load_spots(None, a.spots)
    x, _, z, _ = resolve_point(a.pos, spots)
    r = a.radius if a.radius is not None else 60.0
    at = find_atlas(a.atlas or None)
    if at is None:
        raise ToolError("no_atlas", f"no atlas under {ATLAS_ROOT}: build one (scene_tool.py atlas build) first")
    lamps, total = lamps_near(at, x, z, r, a.src, a.min_intensity)
    view = {}
    if a.cam:
        cx, cy, cz, _ = resolve_point(a.cam, spots)
        if cy is None:
            raise ToolError("bad_args", "--cam needs x,y,z or a spot (the camera's height)")
        if not a.look:
            raise ToolError("bad_args", "--cam needs --look (a point or spot the camera looks at)")
        lx, ly, lz, _ = resolve_point(a.look, spots)
        ly = cy if ly is None else ly       # a look-at point with no height looks level
        cam = ViewCam.make((cx, cy, cz), (lx, ly, lz), a.fov, a.aspect)
        for l in lamps:
            pr = cam.project(l["pos"])
            l["view"] = None if pr is None else {"u": round(pr[0], 3), "v": round(pr[1], 3), "depth_m": round(pr[2], 1), "in_frame": bool(0.0 <= pr[0] <= 1.0 and 0.0 <= pr[1] <= 1.0)}
        view = {"cam": [cx, cy, cz], "look": [lx, ly, lz], "fov": a.fov, "aspect": round(a.aspect, 4),
                "heads_in_frame": sum(1 for l in lamps if l.get("view") and l["view"]["in_frame"])}
        if a.pixels:
            gy = a.ground_y if a.ground_y is not None else ground_y_near(at, cam)
            pool, _t = lamps_near(at, cx, cz, a.max_range + 110.0, a.pixel_src, 0.001)
            cen = ground_census(cam, pool, gy, a.pixels_w, int(round(a.pixels_w * 9 / 16.0)), a.max_range)
            lit_by = cen.pop("per_light")
            for l in lamps:
                l["lit_pixels"] = lit_by.get(l["i"], 0)
            view["ground_y"] = round(gy, 2)
            view["census"] = cen
    shown = lamps[:a.limit]
    out = {"ok": True, "what": "lamps", "source": f"atlas {atlas_id(at)}", "center": [x, z], "r": r, "src": a.src, "in_radius": total, "lamps": shown, "view": view, "problems": sp_problems}
    lines = [f"lamps near x {x:.1f} z {z:.1f} r {r:g} m (atlas {atlas_id(at)}, src {a.src}, min intensity {a.min_intensity:g}): {total} in radius, {len(shown)} shown"]
    for l in shown:
        s = (f"  i {l['i']:<6d} {('lamp %d' % l['lamp']) if l.get('lamp') is not None else 'fill':<10s} d {l['dist']:6.1f}  pos {l['pos'][0]:.1f},{l['pos'][1]:.1f},{l['pos'][2]:.1f}  I {l['intensity']:g} reach {l['reach_m']:g} m"
             f"  {l.get('model', l.get('type', ''))}")
        if "view" in l:
            v = l["view"]
            s += "  [behind the camera]" if v is None else f"  [{'IN VIEW' if v['in_frame'] else 'out'} u {v['u']:.2f} v {v['v']:.2f} depth {v['depth_m']:.0f} m]"
        if "lit_pixels" in l:
            s += f"  lights {l['lit_pixels']} ground px"
        lines.append(s)
    if view:
        lines.append(f"view: camera {view['cam']} looks at {view['look']} fov {view['fov']:g} aspect {view['aspect']}: {view['heads_in_frame']} of the {len(lamps)} listed lamp heads in frame")
        if "census" in view:
            c = view["census"]
            lines.append(f"ground (plane y {view['ground_y']:g}, within {a.max_range:g} m): {c['ground']} of {c['pixels']} px, lit {c['lit']} ({100 * c['lit_share']:.1f} % of the picture); lamps a lit pixel "
                         f"1:{c['by_count'][1]} 2:{c['by_count'][2]} 3:{c['by_count'][3]} 4:{c['by_count'][4]} 5+:{c['by_count'][5]}, max {c['max_lamps']}; {100 * c['at_grid_share']:.0f} % of the lit pixels at the "
                         f"grid's cap (4+), {100 * c['over_grid_share']:.0f} % over it")
    lines += [f"  warning: {p}" for p in sp_problems]
    return emit(a, out, lines, EXIT_OK)


# ------------------------------------------------------------------------------------------------------------------------------- route
def resample_route(points, spacing):
    """[[x, z, speed, y]] resampled about `spacing` metres apart (linear in all four columns; the first and last point are kept)."""
    if spacing <= 0 or len(points) < 2:
        return points
    cum = [0.0]
    for i in range(1, len(points)):
        cum.append(cum[-1] + math.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1]))
    total = cum[-1]
    if total < 1.0:
        raise ToolError("failed", f"the route is {total:.2f} m long (at least 1 m)")
    n = max(1, int(round(total / spacing)))
    out, j = [], 1
    for k in range(n + 1):
        s = total * k / n
        while j < len(cum) - 1 and cum[j] < s:
            j += 1
        seg = cum[j] - cum[j - 1]
        u = 0.0 if seg <= 0 else (s - cum[j - 1]) / seg
        out.append([lerp(points[j - 1][c], points[j][c], u) for c in range(len(points[0]))])
    return out


def cmd_route(a):
    spots, sp_problems = load_spots(resolve_scene(a.scene).parent if a.scene else None, a.spots_file or None)
    stops = [a.from_] + list(a.via or []) + [a.to]
    pts = [resolve_point(w, spots) for w in stops]
    names = [re.sub(r"^spot:", "", w).split("@")[0] for w in stops]
    name = a.name or (names[0] + "-" + names[-1])
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,60}", name):
        raise ToolError("bad_args", f"--name '{name}': letters, digits and _ - . (at most 60)")
    folder = Path(a.dir) if a.dir else (resolve_scene(a.scene).parent / "routes" if a.scene else Path.cwd() / "routes")
    out = Path(a.out) if a.out else folder / (name if name.endswith(".route") else name + ".route")
    if out.exists() and not a.force:
        raise ToolError("bad_args", f"{out} exists: --force replaces it, --name picks another")
    xz = [(p[0], p[2]) for p in pts]
    if sum(dist_xz(xz[i], xz[i + 1]) for i in range(len(xz) - 1)) < 5.0:
        raise ToolError("bad_args", "the stops are less than 5 m apart")
    warnings, road, aid, streets = list(sp_problems), "none", "", []
    at = None if a.no_road else find_atlas(a.atlas or None, need_rails=True)
    ys = None
    heading = a.heading if a.heading is not None else pts[0][3]
    if at is not None:
        am = atlas_module()

        def endpoint(p):
            return [p[0], p[1], p[2]] if p[1] is not None else [p[0], p[2]]

        res = am.route(at, endpoint(pts[0]), endpoint(pts[-1]), [endpoint(p) for p in pts[1:-1]], a.speed, None, heading)
        if not res.get("ok"):
            e = res.get("error", {})
            raise ToolError("failed", f"no road route: {e.get('msg', '')} (--no-road makes a smooth line instead)")
        r = res["result"]
        rows = resample_route(r["points"], a.spacing)
        poly, speeds = [(p[0], p[1]) for p in rows], [p[2] for p in rows]
        if a.heights != "no":
            ys = [p[3] for p in rows]
        road, aid, streets = "atlas", atlas_id(at), r.get("names", [])
        for label, end in (("start", r["start"]), ("end", r["end"])):
            if end["dist_m"] > 6.0:
                warnings.append(f"the {label} is {end['dist_m']:.1f} m from the nearest lane: move the spot onto the road")
        crow = sum(dist_xz(xz[i], xz[i + 1]) for i in range(len(xz) - 1))
        if r["length_m"] > 2.5 * crow + 60.0:
            warnings.append(f"the road route is {r['length_m']:.0f} m for {crow:.0f} m as the crow flies: the stops are on lanes that run the other way, or the roads do not join directly "
                            "(read `preview`'s map; --via a stop on the way, or move a stop to the other lane)")
        if a.start_speed is not None:
            speeds[0] = min(speeds[0], a.start_speed)
        if a.end_speed is not None:
            speeds[-1] = min(speeds[-1], a.end_speed)
    else:
        if not a.no_road:
            warnings.append("no atlas with roads: this is a smooth line through the stops, NOT along the street (read the map of `preview`; build the atlas to follow roads)")
        poly = catmull_rom_xz(xz, a.spacing)
        speeds = plan_speeds(poly, a.speed, a.lat_accel, a.accel, a.brake, a.start_speed, a.end_speed)
        if a.heights == "yes":
            ys0 = [p[1] for p in pts]
            if all(y is not None for y in ys0):
                ys = _line_heights(xz, ys0, poly)
    header = [f"{out.stem}: {' -> '.join(stops)}; made by scene_tool.py route ({road}{' ' + aid if aid else ''})" + (f"; streets: {', '.join(streets)}" if streets else ""),
              f"cruise at most {a.speed:g} m/s, corners limited by their curvature; {a.spacing:g} m apart; x,z,speed(m/s)" + (",y" if ys else "")]
    text = route_text(header, poly, speeds, ys)
    p_back, probs2 = parse_route_text(text)
    if probs2:
        raise ToolError("failed", f"the route made is not a valid route file: {probs2[0]}")
    rs = RouteSampler(p_back)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    info = {"file": str(out), "points": len(poly), "length_m": round(rs.length(), 1), "duration_s": round(rs.duration(), 1), "speed_min": round(min(rs.v), 1),
            "speed_max": round(max(rs.v), 1), "road": road, "atlas": aid, "streets": streets, "heights": bool(ys), "scene_line": f"car p1 path {out.name} speed x1"}
    result = {"ok": True, **info, "warnings": warnings}
    lines = [f"wrote {out}: {info['points']} points, {info['length_m']} m, {info['duration_s']} s at {info['speed_min']}-{info['speed_max']} m/s (road: {road}"
             + (f", {', '.join(streets[:4])}" if streets else "") + ")", f"use it:  {info['scene_line']}"] + [f"warning: {w}" for w in warnings]
    return emit(a, result, lines, EXIT_OK)


def _line_heights(stops_xz, stops_y, poly):
    cum = [0.0]
    for i in range(1, len(stops_xz)):
        cum.append(cum[-1] + dist_xz(stops_xz[i - 1], stops_xz[i]))
    total_line = sum(dist_xz(poly[i], poly[i + 1]) for i in range(len(poly) - 1)) or 1.0
    out, run = [], 0.0
    for i, p in enumerate(poly):
        if i:
            run += dist_xz(poly[i - 1], p)
        s = run / total_line * (cum[-1] or 1.0)
        j = max(1, next((k for k in range(1, len(cum)) if cum[k] >= s), len(cum) - 1))
        u = 0.0 if cum[j] == cum[j - 1] else (s - cum[j - 1]) / (cum[j] - cum[j - 1])
        out.append(lerp(stops_y[j - 1], stops_y[j], max(0.0, min(1.0, u))))
    return out


# ------------------------------------------------------------------------------------------------------------------------------- new, cmd, slots
def list_recipes():
    out = []
    if RECIPES.is_dir():
        for f in sorted(RECIPES.glob("*.scene")):
            first = ""
            for ln in f.read_text(encoding="utf-8", errors="replace").splitlines():
                if ln.startswith("#") and ln.strip("# "):
                    first = re.sub(r"^RECIPE\s+\S+?:\s*", "", ln.lstrip("# ").strip())
                    break
            out.append({"name": f.stem, "file": str(f), "about": first})
    return out


def cmd_new(a):
    recipes = list_recipes()
    if a.list or not a.name:
        lines = [f"{r['name']}: {r['about']}" for r in recipes] or [f"no recipes in {RECIPES}"]
        return emit(a, {"ok": True, "recipes": recipes}, lines, EXIT_OK)
    if not a.recipe:
        raise ToolError("bad_args", "new <name> --recipe <" + "|".join(r["name"] for r in recipes) + ">  (new --list)")
    hit = next((r for r in recipes if r["name"] == a.recipe), None)
    if hit is None:
        raise ToolError("bad_args", f"no recipe '{a.recipe}' (have: {', '.join(r['name'] for r in recipes) or 'none'})")
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,59}", a.name):
        raise ToolError("bad_args", f"scene name '{a.name}': letters, digits and _ - . (a file name, at most 60)")
    folder = Path(a.dir) if a.dir else Path.cwd()
    dest = folder / (a.name + ".scene")
    if dest.exists() and not a.force:
        raise ToolError("bad_args", f"{dest} exists (--force replaces it)")
    text = Path(hit["file"]).read_text(encoding="utf-8")
    text, n = re.subn(r"(?m)^(\s*scene\s+)\S+", lambda m: m.group(1) + a.name, text, count=1)
    folder.mkdir(parents=True, exist_ok=True)
    dest.write_text(text, encoding="utf-8")
    recipe_spots, _ = load_spots(Path(hit["file"]).parent)
    existing_spots, _ = load_spots(folder)
    missing_spots = {k: spot for k, spot in recipe_spots.items() if k not in existing_spots}
    if missing_spots:
        with (folder / "spots.txt").open("a", encoding="utf-8") as f:
            f.write("\n" + spots_text(missing_spots))
    copied = []
    for m in re.finditer(r"(?m)^[^#\n]*\bpath\s+(\S+\.route|\S+)", text):
        for base in (RECIPES, RECIPES / "routes"):
            src = base / (m.group(1) if m.group(1).endswith(".route") else m.group(1) + ".route")
            if src.is_file():
                target = folder / "routes" / src.name
                if not target.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, target)
                    copied.append(str(target))
                break
    result = {"ok": True, "file": str(dest), "recipe": a.recipe, "routes_copied": copied}
    return emit(a, result, [f"made {dest} from the recipe {a.recipe}" + (f" (+ {len(copied)} route file(s))" if copied else ""), "next: edit it, `scene_tool.py lint " + a.name + "`, `scene_tool.py preview " + a.name + " ...`"], EXIT_OK)


def cmd_cmd(a):
    text = " ".join(a.command)
    cmd, args = parse_cmd_line(text)
    with Session(a) as s:
        r = s.ch.request(cmd, args, timeout=a.timeout_s + 10, wait="settled" if a.settle else None, timeout_s=a.timeout_s if a.settle else None)
    result = r.raw if r.raw is not None else {"ok": r.ok}
    if a.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(json.dumps(result.get("result") if r.ok else result.get("error"), indent=2, ensure_ascii=False))
    return EXIT_OK if r.ok else EXIT_ENV


def cmd_slots(a):
    rows = []
    for i in list_infos(a.game_dir, a.attach):
        rows.append({"pid": i.get("pid"), "port": i["port"], "scene": i.get("scene"), "started": i.get("started"),
                     "version": i.get("version"), "alive": i["_alive"], "info": i["_path"]})
    lines = [f"{r['info']}  pid {r['pid']}  port {r['port']}  " + ("live" if r["alive"] else "gone") for r in rows]
    return emit(a, {"ok": True, "slots": rows}, lines or ["no Director connection files found"], EXIT_OK)


# ------------------------------------------------------------------------------------------------------------------------------- picture diff and the diff command
def image_stats(pa, pb):
    """{mean, max, over2, over8, size} of two pictures' difference (Pillow, plain python); {"error"} when the sizes differ."""
    from PIL import Image, ImageChops
    a_, b_ = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
    if a_.size != b_.size:
        return {"error": f"sizes differ: {a_.size[0]}x{a_.size[1]} and {b_.size[0]}x{b_.size[1]}"}
    d = ImageChops.difference(a_, b_)
    r, g, b = d.split()
    worst = ImageChops.lighter(ImageChops.lighter(r, g), b)
    hist = worst.histogram()
    n = a_.size[0] * a_.size[1]
    mean = sum(i * c for i, c in enumerate(r.histogram())) + sum(i * c for i, c in enumerate(g.histogram())) + sum(i * c for i, c in enumerate(b.histogram()))
    mx = max((i for i, c in enumerate(hist) if c), default=0)
    return {"mean": round(mean / (3.0 * n), 4), "max": mx, "over2": round(sum(hist[3:]) / n, 5), "over8": round(sum(hist[9:]) / n, 5), "size": list(a_.size)}


def verdict_of(stats):
    """compare_shots.py's words: IDENTICAL, EXACT (nothing over 2 codes), CLOSE (mean under 0.5, under 1 percent over 8), LOOSE."""
    if "error" in stats:
        return "LOOSE"
    if stats["max"] == 0:
        return "IDENTICAL"
    if stats["max"] <= 2:
        return "EXACT"
    if stats["mean"] < 0.5 and stats["over8"] < 0.01:
        return "CLOSE"
    return "LOOSE"


VERDICT_ORDER = ["IDENTICAL", "EXACT", "CLOSE", "LOOSE"]


def pair_pictures(pa, pb, max_pairs=24):
    """[(label, file A, file B)]: two preview folders (preview.json: the same scene time, within 1 ms), two take folders (frames/ or shots at the same scene time from
    frames.csv), else pictures with the same stem."""
    pa, pb = Path(pa), Path(pb)
    if (pa / "preview.json").is_file() and (pb / "preview.json").is_file():
        ja, jb = json.loads((pa / "preview.json").read_text(encoding="utf-8")), json.loads((pb / "preview.json").read_text(encoding="utf-8"))
        bt = {round(s["t"], 3): s for s in jb["shots"]}
        out = []
        for s in ja["shots"]:
            o = bt.get(round(s["t"], 3))
            if o and s.get("file") and o.get("file"):
                out.append((f"t={s['t']:.2f}", pa / s["file"], pb / o["file"]))
        return out
    if (pa / "frames").is_dir() and (pb / "frames").is_dir():
        fa, fb = sorted((pa / "frames").glob("*.png")), sorted((pb / "frames").glob("*.png"))
        n = min(len(fa), len(fb))
        idx = sorted({round(i * (n - 1) / max(1, max_pairs - 1)) for i in range(max_pairs)}) if n else []
        return [(f"frame {i}", fa[i], fb[i]) for i in idx]
    ia = {p.stem: p for p in Path(pa).glob("*.png")}
    ib = {p.stem: p for p in Path(pb).glob("*.png")}
    return [(k, ia[k], ib[k]) for k in sorted(ia) if k in ib]


def diff_sheet(pairs, rows, out_png, cell_w=520):
    """A sheet of [A | B | difference x4] per pair, <= 1600 px wide, labelled; the pairs given only (a page)."""
    from PIL import Image, ImageChops, ImageDraw
    font = scene_font(14)
    ims = []
    for label, fa, fb in pairs:
        a_, b_ = Image.open(fa).convert("RGB"), Image.open(fb).convert("RGB")
        h = int(cell_w * a_.size[1] / a_.size[0])
        d = ImageChops.difference(a_, b_.resize(a_.size)).point(lambda v: min(255, v * 4))
        ims.append((label, [im.resize((cell_w, h)) for im in (a_, b_, d)], h))
    height = sum(h + 20 for _, _, h in ims) + 4
    sheet = Image.new("RGB", (cell_w * 3 + 8, height), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    y = 2
    for label, three, h in ims:
        dr.text((4, y), f"{label}    A | B | difference x4", fill=(255, 255, 255), font=font)
        for k, im in enumerate(three):
            sheet.paste(im, (k * (cell_w + 4), y + 18))
        y += h + 20
    sheet.save(out_png)


def scene_font(size):
    from PIL import ImageFont
    for name in ("consola.ttf", "arial.ttf", "DejaVuSansMono.ttf", "DejaVuSans.ttf"):
        for base in (Path(os.environ.get("WINDIR", "")) / "Fonts", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF"):
            p = Path(base) / name
            if p.is_file():
                try:
                    return ImageFont.truetype(str(p), size)
                except OSError:
                    pass
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


def kind_of(p):
    p = Path(p)
    if p.is_file():
        return "log"
    if (p / "preview.json").is_file():
        return "preview"
    if (p / "take.json").is_file():
        return "take"
    if p.is_dir():
        return "pictures"
    raise ToolError("not_found", f"{p}: not a file or a folder")


def run_take_diff(pa, pb, extra, want_json):
    cmd = [sys.executable, "-I", "-B", str(TAKE_DIFF), str(pa), str(pb)] + list(extra) + (["--json"] if want_json else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def cmd_diff(a):
    ka, kb = kind_of(a.a), kind_of(a.b)
    if ka != kb and not ({ka, kb} <= {"take", "preview", "pictures"}):
        raise ToolError("bad_args", f"cannot compare a {ka} with a {kb}")
    result, lines, code = {"ok": True, "kind": ka}, [], EXIT_OK
    if ka == "log":
        rc, out, err = run_take_diff(a.a, a.b, a.take_diff_args, a.json)
        result.update(take_diff_exit=rc)
        if a.json:
            try:
                result["take_diff"] = json.loads(out)
            except ValueError:
                result["take_diff_output"] = out
        lines = out.rstrip().splitlines() + ([err.strip()] if err.strip() else [])
        code = EXIT_OK if rc == 0 else (EXIT_FINDINGS if rc == 1 else EXIT_USAGE)
        result["ok"] = rc == 0
        return emit(a, result, lines, code)
    state_a, state_b = Path(a.a) / "state.csv", Path(a.b) / "state.csv"
    if ka == "take" and state_a.is_file() and state_b.is_file():
        rc, out, err = run_take_diff(state_a, state_b, a.take_diff_args, False)
        first = out.strip().splitlines()[:6]
        result["state_log"] = {"exit": rc, "head": first}
        lines += ["state logs (take_diff.py): " + ("IDENTICAL" if rc == 0 else "DIVERGES" if rc == 1 else "bad input")] + [f"  {x}" for x in first]
        code = max(code, EXIT_FINDINGS if rc != 0 else EXIT_OK)
    pairs = pair_pictures(a.a, a.b, a.max_pairs)
    if not pairs:
        lines.append("no pictures to pair (previews pair by scene time; takes need sink png; folders of pictures by file name)")
        result["pictures"] = []
        return emit(a, result, lines, code)
    stats, worst = [], "IDENTICAL"
    for label, fa, fb in pairs:
        st = image_stats(fa, fb)
        v = verdict_of(st)
        st.update(label=label, a=str(fa), b=str(fb), verdict=v)
        stats.append(st)
        worst = max(worst, v, key=VERDICT_ORDER.index)
        lines.append(f"  {label:<14} {v:<9} " + (st["error"] if "error" in st else f"mean {st['mean']:.3f}  max {st['max']}  over8 {st['over8'] * 100:.2f}%"))
    result["pictures"], result["verdict"] = stats, worst
    lines.insert(0, f"{len(stats)} picture pair(s): {worst}")
    if a.out:
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        shown = [(s["label"], s["a"], s["b"]) for s in stats if "error" not in s][:a.sheet_rows]
        if shown:
            diff_sheet(shown, a.sheet_rows, out / "diff.png")
            result["sheet"] = str(out / "diff.png")
            lines.append(f"sheet: {out / 'diff.png'}")
        (out / "diff.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    code = max(code, EXIT_FINDINGS if worst == "LOOSE" else EXIT_OK)
    result["ok"] = code == EXIT_OK
    return emit(a, result, lines, code)


# ------------------------------------------------------------------------------------------------------------------------------- render
def sanitize_scene_text(text):
    """The scene's text with every `capture` statement commented out (same line numbers): a preview or a channel take must not start a take of its own. -> (text, [line numbers])."""
    out, hit = [], []
    for n, raw in enumerate(text.split("\n"), 1):
        words = raw.split("#", 1)[0].split()
        if words and ((words[0].lower() == "capture") or (words[0].lower() == "at" and len(words) >= 3 and words[2].lower() == "capture")):
            out.append("# [scene_tool] " + raw.rstrip("\r"))
            hit.append(n)
        else:
            out.append(raw)
    return "\n".join(out), hit


def stage_scene(src_path, dest_dir, text, linted):
    """Stage a scene and the dependencies that its car paths and flybys resolve to."""
    text, captures = sanitize_scene_text(text)
    dest = dest_dir / src_path.name
    dest.write_text(text, encoding="utf-8")
    sc = scene_check_module()
    paths = set()
    for e in linted.events:
        if e.track == "car" and len(e.words) >= 3 and e.words[1].lower() == "path":
            paths.add(e.words[2])
        if e.track == "cam" and [w.lower() for w in e.words[:2]] == ["rig", "flyby"]:
            _, rig = sc.rig_error(e.words)
            paths.add(rig["route"])
    for name in sorted(paths):
        source, _ = sc.find_route_file(src_path.parent, name)
        if source is None:
            raise ToolError("not_found", f"route disappeared before staging: {name}")
        target = dest_dir / source.relative_to(src_path.parent)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    spots, _ = load_spots(src_path.parent)
    if spots:
        (dest_dir / "spots.txt").write_text(spots_text(spots), encoding="utf-8")
    return dest, captures


def cmd_render(a):
    path = resolve_scene(a.scene)
    text = path.read_text(encoding="utf-8")
    lr = lint_one(path, a.src, None, a.no_src, text=text)
    linted = lr.pop("_scene")
    if lr["errors"]:
        d = next(x for x in lr["diagnostics"] if x["severity"] == "error")
        raise ToolError("failed", f"{path.name}:{d['line']}: {d['message']}", line=d["line"], exit_code=EXIT_FINDINGS)
    model = SceneModel(path, text)
    fps = a.fps or next((e.to for e in model.events if e.track == "capture" and e.state), 0) or model.fps or 60.0
    name = a.name or f"{path.stem}-{stamp()}"
    sr = load_module(SCENE_RUN, "scene_run_mod")
    probe = sr.find_ffprobe(sr.find_ffmpeg(a.ffmpeg)) if a.sink == "ffmpeg" else None
    t_from = a.t_from if a.t_from is not None else 0.0
    t_to = a.t_to if a.t_to is not None else model.end
    if t_to is None:
        raise ToolError("bad_args", "this scene has no end; pass --to")
    if not (math.isfinite(t_from) and math.isfinite(t_to) and 0 <= t_from < t_to):
        raise ToolError("bad_args", "render requires finite times with 0 <= from < to")
    if model.end is not None and t_to > model.end + 1e-9:
        raise ToolError("bad_args", f"--to is past the scene end ({model.end:g})")
    if not 1 <= fps <= 960:
        raise ToolError("bad_args", "render FPS must be between 1 and 960")
    budget = a.timeout_s or max(300.0, model.duration() * 12.0)
    with Session(a) as s:
        ch = s.ch
        wait_world(ch, a.world_timeout_s)
        if ch.call("take.status").get("active"):
            raise ToolError("not_ready", "a take is already running")
        staged, hit = s.stage(path, "render", text, linted)
        ch.call("scene.load", {"path": str(staged)}, timeout=200.0, wait="settled", timeout_s=180.0)
        ch.call("events.set", {"only": ["take.done"]})
        args = {"fps": fps, "sink": a.sink, "name": name, "from": t_from, "to": t_to}
        # CHECK: confirm the take.done event and files from a live one-second take.
        res = ch.call("take.render", args, timeout=60.0)
        take_name = res.get("name", name)

        def own_take(event):
            if event.get("name") == take_name:
                return True
            # The game can rename a take to keep a prior folder or use a configured output.
            if not event.get("folder"):
                return False
            try:
                manifest = json.loads((Path(event["folder"]) / "take.json").read_text(encoding="utf-8"))
                source = manifest["scene"]["source"]
                return Path(source).resolve() == staged
            except (OSError, ValueError, KeyError, TypeError):
                return False

        done = ch.wait_event("take.done", pred=own_take, timeout=budget)
        if done is None:
            status = ch.call("take.status")
            raise ToolError("timeout", f"no take.done for {take_name} within {budget:g} s (take.status: {json.dumps(status)})")
    folder = done.get("folder") or ""
    problems, notes = [], []
    if done.get("state") != "complete":
        problems.append(f"the game reported {done.get('state')}: {done.get('error') or 'see the game log'}")
    tj = Path(folder) / "take.json" if folder else None
    if tj and tj.is_file():
        take = json.loads(tj.read_text(encoding="utf-8"))
        findings, notes = sr.check_take(take, tj.parent, ffprobe=probe)
        problems.extend(findings)
        if not findings:
            count = take["frames"]["count"]
            if done.get("frames") != count:
                problems.append(f"take.done reported {done.get('frames')} frames; take.json has {count}")
            scene = take.get("scene")
            if not isinstance(scene, dict) or Path(scene.get("source") or "").resolve() != staged:
                problems.append("take.json names a different scene")
            if take.get("name") != done.get("name") or take["sink"].get("kind") != a.sink:
                problems.append("take.json does not match the reported take name or requested sink")
    else:
        problems.append(f"no take.json in the folder reported by the game ({folder or 'none'})")
    result = {"ok": not problems, "scene": str(path), "take": done.get("name", take_name), "folder": folder, "fps": fps, "sink": a.sink,
              "captures_commented": hit, "problems": problems, "notes": notes, "done": done}
    lines = [f"take folder: {folder}"] + [f"  problem: {p}" for p in problems] + [f"  note: {n}" for n in notes]
    return emit(a, result, lines, EXIT_OK if not problems else EXIT_FINDINGS)


# ------------------------------------------------------------------------------------------------------------------------------- the command line
def load_preview():
    mod = sys.modules.get("scene_preview")
    return mod if mod is not None else load_module(HERE / "scene_preview.py", "scene_preview")


def cmd_preview(a):
    return load_preview().run_preview(a)


def cmd_selftest(_a):
    tests = load_module(HERE / "selftest.py", "director_selftest")
    return tests.run(sys.modules["scene_tool"])


def build_parser():
    ap = argparse.ArgumentParser(prog="scene_tool.py", description="write, check, preview and render LARecomp scenes (see the module's docstring and documentation\\SCENE-AUTHORING.md)")
    sub = ap.add_subparsers(dest="command", required=True, metavar="<command>")

    def mk(name, func, help_):
        p = sub.add_parser(name, help=help_, description=help_)
        p.add_argument("--json", action="store_true", help="one JSON object on stdout instead of text")
        p.set_defaults(func=func)
        return p

    p = mk("lint", cmd_lint, "check scene files (scene_check.py + spot names); exit 1 on an error")
    p.add_argument("files", nargs="+")
    p.add_argument("--strict", action="store_true", help="warnings fail too")
    p.add_argument("--list", action="store_true", help="print the timeline of keys")
    p.add_argument("--src", default=None, help="the source folder the settings' names are looked up in")
    p.add_argument("--no-src", action="store_true", help="do not look the settings' names up")
    p.add_argument("--cars", default=None, help="a file of car names (one a line)")

    p = mk("query", cmd_query, "world knowledge: spots, lights, road, route, garages, cars, ground (offline from the atlas, or --live); pick, streaming, visible (live only)")
    p.add_argument("what", choices=QUERY_WHATS)
    p.add_argument("--near", default="", help="spot:NAME[@r,u,f] | x,z | x,y,z")
    p.add_argument("--r", type=float, default=None, help="radius in metres (default 60; 40 for road)")
    p.add_argument("--from", dest="from_", default="", help="query route: the start (spot or x,z)")
    p.add_argument("--to", default="", help="query route: the end")
    p.add_argument("--via", action="append", default=[], help="query route: a stop on the way (repeat)")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--name", default="", help="spots / cars: a name contains this")
    p.add_argument("--file", default="", help="query route: the route file")
    p.add_argument("--atlas", default="", help="the atlas folder (default: the current user atlas)")
    p.add_argument("--spots", default="", help="another spots file")
    p.add_argument("--live", action="store_true", help="ask the running game (query.<what>) instead of the files")
    p.add_argument("--uv", default="", help="query pick --live: u,v of the picture (0..1)")
    p.add_argument("--subject", default="", help="query visible --live: car:ID")
    p.add_argument("--arg", action="append", default=[], metavar="K=V", help="more arguments of the live query")
    p.add_argument("--timeout-s", type=float, default=30.0)
    add_game_args(p)

    p = mk("lamps", cmd_lamps, "the atlas' street lamps near a place (ids, distances, models), and with --cam/--look which a camera sees and how many lamps light each ground pixel (the 4 a pixel cap)")
    p.add_argument("pos", help="spot:NAME[@r,u,f] | x,z | x,y,z")
    p.add_argument("radius", nargs="?", type=float, default=None, help="metres (default 60)")
    p.add_argument("--src", choices=("lamp", "file", "all"), default="lamp", help="the street lamps (default), lights.xlm's fill lights, or both")
    p.add_argument("--min-intensity", type=float, default=1.0, help="leave out the dead lamps (375 street lamps have intensity 0; default 1)")
    p.add_argument("--limit", type=int, default=40, help="how many to list (the count in the radius is always given)")
    p.add_argument("--cam", default="", help="a camera position (spot or x,y,z): marks the lamps it sees")
    p.add_argument("--look", default="", help="the point the camera looks at (spot, x,y,z or x,z for a level look)")
    p.add_argument("--fov", type=float, default=60.0, help="vertical field of view, degrees (default 60)")
    p.add_argument("--aspect", type=float, default=16.0 / 9.0, help="width over height of the picture (default 16/9)")
    p.add_argument("--pixels", action="store_true", help="with --cam: the lamps a pixel of the ground in view has (the heat pass's `lamps a pixel` counter by the law of the game's lamps; ground plane only)")
    p.add_argument("--pixels-w", type=int, default=96, help="the pixel grid's width (default 96, 54 high)")
    p.add_argument("--pixel-src", choices=("lamp", "file", "all"), default="all", help="which lights light a pixel (default all: the world frame's window holds the street lamps and the fills)")
    p.add_argument("--ground-y", type=float, default=None, help="the ground plane's height (default: the nearest road's under the camera)")
    p.add_argument("--max-range", type=float, default=150.0, help="the farthest ground judged, metres (default 150)")
    p.add_argument("--atlas", default="", help="the atlas folder (default: the current user atlas)")
    p.add_argument("--spots", default="", help="another spots file")

    p = mk("route", cmd_route, "a route file between spots: along the atlas roads, else a smooth line; speeds from the corners")
    p.add_argument("--from", dest="from_", required=True, help="spot:NAME[@r,u,f] | x,z")
    p.add_argument("--to", required=True)
    p.add_argument("--via", action="append", default=[], help="a stop on the way (repeat)")
    p.add_argument("--speed", type=float, default=14.0, help="the most the car goes, m/s (default 14; the roads' own limits apply below it)")
    p.add_argument("--lat-accel", type=float, default=5.0, help="no atlas: the most sideways acceleration in a corner, m/s2 (default 5)")
    p.add_argument("--accel", type=float, default=3.0, help="no atlas: m/s2 (default 3)")
    p.add_argument("--brake", type=float, default=6.0, help="no atlas: m/s2 (default 6)")
    p.add_argument("--start-speed", type=float, default=None)
    p.add_argument("--end-speed", type=float, default=None)
    p.add_argument("--spacing", type=float, default=DEFAULT_SPACING_M, help="metres between points (default 2)")
    p.add_argument("--heading", type=float, default=None, help="the direction of travel at the start, to pick the lane (default: the first spot's heading)")
    p.add_argument("--heights", choices=("auto", "yes", "no"), default="auto", help="write the y column: auto = when the road's heights are known")
    p.add_argument("--no-road", action="store_true", help="a smooth line through the stops even when the atlas is there")
    p.add_argument("--name", default="", help="the route's name (default <from>-<to>)")
    p.add_argument("--out", default="", help="the file to write (default <dir>\\<name>.route)")
    p.add_argument("--dir", default="", help="the folder (default ./routes, or the routes folder beside --scene)")
    p.add_argument("--scene", default="", help="put the file in the routes folder beside this scene")
    p.add_argument("--atlas", default="")
    p.add_argument("--spots-file", default="")
    p.add_argument("--force", action="store_true", help="replace an existing file")

    p = mk("new", cmd_new, "a new scene from a recipe (scenes\\recipes)")
    p.add_argument("name", nargs="?", default="")
    p.add_argument("--recipe", default="")
    p.add_argument("--list", action="store_true")
    p.add_argument("--dir", default="", help="where to write it (default: current directory)")
    p.add_argument("--force", action="store_true")

    p = mk("preview", cmd_preview, "capture a storyboard from a running game")
    load_preview().add_preview_args(p)

    p = mk("render", cmd_render, "render a take through a running game")
    p.add_argument("scene")
    p.add_argument("--fps", type=positive_seconds, help="frame rate (default: the scene rate or capture rate)")
    p.add_argument("--sink", default="ffmpeg", choices=("none", "ffmpeg", "png", "raw"))
    p.add_argument("--name", default="", help="the take's name")
    p.add_argument("--from", dest="t_from", type=float, default=None)
    p.add_argument("--to", dest="t_to", type=float, default=None)
    p.add_argument("--timeout-s", type=positive_seconds, help="seconds to wait for take.done")
    p.add_argument("--src", default=None)
    p.add_argument("--no-src", action="store_true")
    p.add_argument("--ffmpeg", help="find ffprobe next to this FFmpeg for video verification")
    add_game_args(p, work_dir=True, wait=True)

    p = mk("diff", cmd_diff, "two state logs (take_diff.py), takes or previews (a picture diff at the same scene times)")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--out", default="", help="write diff.png + diff.json here")
    p.add_argument("--max-pairs", type=int, default=24)
    p.add_argument("--sheet-rows", type=int, default=5)
    p.add_argument("--take-diff-args", nargs=argparse.REMAINDER, default=[], help="more arguments for take_diff.py (--align, --tol ...); last option")

    p = mk("cmd", cmd_cmd, "send one Director command, e.g. cmd help")
    p.add_argument("command", nargs="+")
    p.add_argument("--settle", action="store_true", help="ask for the reply only after the game settles")
    p.add_argument("--timeout-s", type=positive_seconds, default=30.0)
    add_game_args(p)

    # Forward all atlas arguments to its own parser, including its help and JSON options.
    sub.add_parser("atlas", add_help=False, help="build or query the local world atlas")

    p = mk("slots", cmd_slots, "list local Director connection files")
    add_game_args(p)

    p = mk("launch", cmd_launch, "start one game and wait for its Director to listen")
    p.add_argument("--exe", required=True, help="path to the game executable")
    p.add_argument("--scene", help="scene file or recipe to arm at startup")
    p.add_argument("--work-dir", help="output folder (default: director-work beside the executable)")
    p.add_argument("--ffmpeg", help="FFmpeg executable (default: the game's FFmpeg setting, normally PATH)")
    p.add_argument("--timeout-s", type=positive_seconds, default=60.0, help="seconds to wait for the Director")
    p = mk("selftest", cmd_selftest, "run all offline Director checks")
    return ap


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")   # a scene's text may hold what the console's code page cannot print
        except (AttributeError, ValueError):
            pass
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["atlas"]:
        return atlas_module().main(argv[1:])
    a = build_parser().parse_args(argv)
    try:
        return a.func(a)
    except ToolError as e:
        if getattr(a, "json", False):
            print(json.dumps(e.as_json(), indent=2))
        else:
            print(f"scene_tool: error: {e.msg}" + (f" (line {e.line})" if e.line else "") + f" [{e.code}]", file=sys.stderr)
        return e.exit_code
    except (OSError, ValueError) as e:
        return emit(a, ToolError("failed", str(e)).as_json(), [f"scene_tool: {e}"], EXIT_ENV)
    except KeyboardInterrupt:
        return 130


# ------------------------------------------------------------------------------------------------------------------------------- selftest
def selftest_tool():
    """The checks of this file: [(label, ok, detail)]. No game; Pillow only for the picture parts."""
    results = []

    def check(ok, label, detail=""):
        results.append((label, bool(ok), detail))

    def close(a_, b_, tol=1e-6):
        return abs(a_ - b_) <= tol

    tmp = Path(tempfile.mkdtemp(prefix="scene_tool_selftest_"))
    saved = (RECIPES, SPOTS_FILE, SCENES)
    try:
        # ---- spots and frames
        spots, probs = parse_spots_text("# spots\ngate 819.6 17.6 -808.4 62  # the lot\nstreet 830 17.6 -790 62\ngate 1 2 3 4\nbad 1 2\n")
        check(set(spots) == {"gate", "street"} and len(probs) == 2 and "already defined on line 2" in probs[0], "spots: two good, a duplicate and a short line are reported", str(probs))
        s = {"a": Spot("a", 10.0, 5.0, 20.0, 90.0)}
        p, h = resolve_spot_ref("spot:a@1,0,2", s)
        check(close(p[0], 12.0) and close(p[1], 5.0) and close(p[2], 21.0) and h == 90.0, "spots: @right,up,forward in the spot's heading frame (heading 90 faces +X, right is +Z)", str(p))
        p, h = resolve_spot_ref("spot:a", s)
        check(p == (10.0, 5.0, 20.0), "spots: a bare spot is its place")
        try:
            resolve_spot_ref("spot:gat", {"gate": Spot("gate", 0, 0, 0, 0)})
            check(False, "spots: an unknown spot is an error")
        except ToolError as e:
            check("did you mean gate" in e.msg and e.code == "bad_args", "spots: an unknown spot says what was meant", e.msg)
        check(resolve_spot_ref("12,3", s) is None and resolve_point("12,3", s)[0] == 12.0, "spots: a plain point is not a spot reference")
        sp2, pr2 = parse_spots_text("a 1 2 3\nb, 4, 5, 6, 90  # note src: x\nc 7 8 9 10\nc 1 1 1 0\n9bad\n")
        check(not sp2["a"].has_heading and sp2["b"].heading == 90.0 and sp2["b"].text.startswith("note") and sp2["c"].x == 1.0 and any("this one wins" in x for x in pr2) and any("9bad" in x for x in pr2),
              "spots: heading optional, commas allowed, a repeated name: the later wins with a warning, a short line is reported", str(pr2))
        try:
            resolve_spot_ref("spot:a@1,0,0", sp2)
            check(False, "spots: an offset on a spot with no heading is an error")
        except ToolError as e:
            check("no heading" in e.msg, "spots: an offset on a spot with no heading is an error", e.msg)
        p3, h3 = resolve_spot_ref("spot:b@0,0,0,180", sp2)
        check(p3 == (4.0, 5.0, 6.0) and h3 == 270.0, "spots: the fourth number turns the heading (the oncoming lane)", str((p3, h3)))
        k = parse_cam_words(1.0, 3, ["pos", "spot:b@0,1.7,-8", "yaw", "spot:b", "fov", "40"], sp2)
        check(not k.problem and k.aim["yaw"] == 90.0 and abs(k.pos["point"][1] - 6.7) < 1e-9, "spots: `yaw spot:b` is the spot's heading and `pos spot:b@0,1.7,-8` its frame", k.problem)
        sp3 = {"gate": Spot("gate", 100.0, 17.6, 200.0, 90.0), "hw": Spot("hw", 0.0, 0.0, 0.0, 62.0)}
        ex, exp = expand_spots("at 1 car a spawn m at spot:gate@1,0,2 heading spot:gate  # spot:nope in a comment\ncam 2 pos spot:hw@0,1.7,-8 yaw spot:hw@0,0,0,180\nat 3 mark spot:nothere\n", sp3)
        l1, l2, _l3 = ex.split("\n")[:3]
        check("at 102.000,17.600,201.000 heading 90" in l1 and "# spot:nope in a comment" in l1, "spots: expand_spots puts numbers where a spot word stood (a comment is left alone)", l1)
        check("yaw 242" in l2 and l2.startswith("cam 2 pos ") and len(exp) == 1 and exp[0]["line"] == 3 and "unknown spot" in exp[0]["message"], "spots: a turn changes `yaw spot:` only; an unknown name is one error on its line", str((l2, exp)))
        # ---- the route sampler against the closed form
        rs = RouteSampler([RoutePt(0, 0, 10.0), RoutePt(100, 0, 10.0)])
        check(close(rs.duration(), 10.0) and close(rs.at_time(5)["x"], 50.0, 1e-6) and close(rs.at_time(5)["heading"], 90.0, 1e-6), "route sampler: constant speed, 100 m in 10 s, heading +X")
        rs = RouteSampler([RoutePt(0, 0, 5.0), RoutePt(100, 0, 15.0)])
        check(close(rs.duration(), 100 * math.log(3) / 10, 1e-9), "route sampler: a speed ramp takes L ln(v1/v0)/(v1-v0)", f"{rs.duration():.4f}")
        t_end = rs.duration()
        pos = [rs.at_time(t_end * k / 40)["x"] for k in range(41)]
        steps = [b - a_ for a_, b in zip(pos, pos[1:])]
        check(close(pos[-1], 100.0, 1e-6) and all(x >= 0 for x in steps) and steps[-1] > steps[0], "route sampler: the car accelerates along the ramp and ends at the last point")
        dt = 1e-5
        v_num = (rs.at_time(3.0 + dt)["x"] - rs.at_time(3.0 - dt)["x"]) / (2 * dt)
        check(close(v_num, rs.at_time(3.0)["speed"], 1e-3), "route sampler: the speed at a time is the derivative of the position", f"{v_num:.4f} vs {rs.at_time(3.0)['speed']:.4f}")
        check(RouteSampler([RoutePt(0, 0, 10.0), RoutePt(10, 0, 10.0)], SpeedSpec("scale", 2.0)).duration() < 0.6, "route sampler: speed x2 halves the time")
        check(RouteSampler([RoutePt(0, 0), RoutePt(12, 0)]).duration() == 1.0, "route sampler: a point with no speed is 12 m/s")
        pts, pr = parse_route_text("# h\n1,2,3\n4 5 6 7\n")
        check(len(pts) == 2 and any("all or none" in x for x in pr), "route file: a mix of heights is refused")
        check(parse_route_text("1,2\nx,2\n")[1] and parse_speed_word("x1.5").kind == "scale" and parse_speed_word("12").kind == "mps", "route file and speed words are read")
        # ---- route generation
        poly = catmull_rom_xz([(0, 0), (50, 0), (50, 50)], 2.0)
        gaps = [math.hypot(b[0] - a_[0], b[1] - a_[1]) for a_, b in zip(poly, poly[1:])]
        check(poly[0] == (0, 0) and close(poly[-1][0], 50.0, 1e-6) and close(poly[-1][1], 50.0, 1e-6) and max(gaps) < 2.3, "route: a smooth line through three stops, about 2 m apart", f"{len(poly)} points, max gap {max(gaps):.2f}")
        near_mid = min(math.hypot(p_[0] - 50, p_[1]) for p_ in poly)
        check(near_mid <= 1.05, "route: the line passes the middle stop (within half the 2 m spacing)", f"{near_mid:.2f} m")
        sp = plan_speeds(poly, 14.0, 5.0, 3.0, 6.0)
        check(min(sp) < 10.0 and sp[0] <= 14.0 and max(sp) <= 14.0, "route: the corner slows the car", f"{min(sp):.1f}..{max(sp):.1f}")
        okacc = all(sp[i] ** 2 - sp[i - 1] ** 2 <= 2 * 3.0 * 2.2 + 1e-6 for i in range(1, len(sp)))
        okdec = all(sp[i - 1] ** 2 - sp[i] ** 2 <= 2 * 6.0 * 2.2 + 1e-6 for i in range(1, len(sp)))
        check(okacc and okdec, "route: speeds respect the acceleration and braking limits")
        # ---- lint
        globals()["SCENES"] = tmp / "scenes"
        (tmp / "scenes").mkdir()
        good = tmp / "scenes" / "good.scene"
        good.write_text("scene good fps 60 step 1/60\nworld hour 15 hold weather nice traffic off peds off parked off police off\ncam 1 pos 0 5 0 yaw 0 pitch 0 fov 40\nend 5\n")
        badf = tmp / "scenes" / "bad.scene"
        badf.write_text("scene bad fps 60 step 1/60\nat 1 weather sunny\ncam 1 pos 0 5 0\nend 5\n")
        spotf = tmp / "scenes" / "spot.scene"
        (tmp / "scenes" / "spots.txt").write_text("gate 819.6 17.6 -808.4 62\n")
        spotf.write_text("scene spot fps 60 step 1/60\ncam 1 pos spot:gat@0,5,0 yaw 0 pitch 0\nend 5\n")
        r = lint_one(good, no_src=True)
        check(r["errors"] == 0 and r["keys"] >= 2 and r["end"] == 5.0, "lint: a good scene has no errors", str([d["message"] for d in r["diagnostics"]]))
        r = lint_one(badf, no_src=True)
        d = r["diagnostics"][0] if r["diagnostics"] else {}
        check(r["errors"] >= 1 and d.get("line") == 2 and "weather" in d.get("message", "") and "sunny" in d.get("source", ""), "lint: a bad scene names the line, the message and the line's text", str(d))
        r = lint_one(spotf, no_src=True)
        check(any("spot" in d["message"].lower() and d["line"] == 2 for d in r["diagnostics"]), "lint: an unknown spot name is an error on its line", str([d["message"] for d in r["diagnostics"]]))
        ns = argparse.Namespace(files=[str(good), str(badf)], strict=False, list=False, src=None, no_src=True, cars=None, json=True)
        import io
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            code = cmd_lint(ns)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        check(code == 1 and j["ok"] is False and len(j["files"]) == 2 and j["files"][1]["errors"] >= 1, "lint: --json is one object, exit 1 when a file has an error")
        # ---- the scene model
        (tmp / "scenes" / "routes").mkdir()
        (tmp / "scenes" / "routes" / "r1.route").write_text("100,100,10\n200,100,10\n")
        text = ("scene m fps 60 step 1/60\nat 0 car a spawn vp_x at 100,17.6,100 heading 90\nat 0 car b spawn vp_y near a 3,0,-5\nat 2 car a path r1.route\nat 6 car a stop\n"
                "at 8 car b delete\ncam 1 attach car:a 0 2 -6 heading look car:a 0 1 0 fov 40\ncut 3\ncam 3 pos 150 6 120 look car:a fov 30\ncam 7 off\nend 10\n")
        m = SceneModel(tmp / "scenes" / "m.scene", text)
        pa = m.car_pose("a", 4.0)
        check(pa and close(pa["x"], 120.0, 1e-6) and pa["on_path"], "model: car a is 20 m along its route 2 s after the path key (10 m/s)", str(pa))
        pa6, pa9 = m.car_pose("a", 6.0), m.car_pose("a", 9.0)
        check(pa6 and pa9 and close(pa6["x"], pa9["x"]) and pa9["speed"] == 0.0, "model: `car a stop` freezes it where it is")
        pb = m.car_pose("b", 1.0)
        check(pb and close(pb["x"], 95.0, 1e-6) and close(pb["z"], 103.0, 1e-6), "model: a `near a 3,0,-5` car is 3 m right and 5 m behind car a (heading 90)", str(pb))
        check(m.car_pose("b", 8.5) is None and m.car_pose("a", -1) is None, "model: a deleted car and a car before its spawn are gone")
        check(m.subject_at(2.0) == "a" and m.subject_at(5.0) == "a", "model: the subject is the car the active key names")
        fm = SceneModel(tmp / "scenes" / "fly.scene", "scene f fps 60\nat 0 car a spawn m at 100,17.6,100 heading 90\ncam 2 rig flyby path r1.route speed 5 up 3 track car:a\nend 10\n")
        fe = fm.est_camera(4.0)
        check(fe and close(fe["pos"][0], 110.0, 1e-6) and close(fe["pos"][2], 100.0, 1e-6), "model: a flyby rig is a place that moves along the route from the key's time (5 m/s for 2 s = 10 m)", str(fe))
        e = m.est_camera(2.0)
        check(e and e["fov"] == 40.0 and close(e["pos"][0], 94.0, 1e-3) and close(e["pos"][1], 19.6, 1e-3) and close(e["yaw"], 90.0, 1.0), "model: the camera estimate sits 6 m behind car a (heading 90) and looks at it", str(e))
        check(m.est_camera(8.0) is None, "model: after `cam off` there is no scene camera")
        check(m.time_range() == (1.0, 7.0), "model: the camera work is from the first key to `cam off`", str(m.time_range()))
        stext, hit = sanitize_scene_text("scene x\nat 2 capture fps 60 name x\ncapture off\nat 3 mark a # capture\n")
        check(hit == [2, 3] and stext.count("\n") == 4 and stext.split("\n")[3].startswith("at 3 mark"), "stage: capture statements are commented out, the line count stays", str(hit))
        # ---- the console grammar
        cmd, args = parse_cmd_line("camera.set pos=819.6,19,-800 yaw=80 fov=40 name=hero wait=true")
        check(cmd == "camera.set" and args["pos"] == [819.6, 19, -800] and args["yaw"] == 80 and args["name"] == "hero" and args["wait"] is True, "cmd: the console grammar becomes args", str(args))
        check(parse_cmd_line('{"cmd":"ping"}') == ("ping", {}), "cmd: a JSON request is taken as it is")
        try:
            parse_cmd_line("camera.set oops")
            check(False, "cmd: a word without = is an error")
        except ToolError as e:
            check(e.code == "bad_args", "cmd: a word without = is an error")
        # ---- the atlas (atlas.py's own synthetic road: two rails along +X from 0 to 60 at z=0, 20 m/s, SYN_Test_St.), query and route
        am = atlas_module()
        import atlas_game as _g  # atlas.py put its folder on sys.path
        atlas_root = tmp / "atlas"
        adir = am.write_atlas(str(atlas_root), _g.parse_lights(_g.synth_lights()), _g.parse_railnet(_g.synth_railnet()), "scene_tool selftest")
        at = find_atlas(adir, need_rails=True)
        check(at is not None and len(at.rails) == 2 and atlas_id(at) == Path(adir).name, "atlas: a folder is loaded through atlas.py", atlas_id(at) if at else "none")
        check(find_atlas(str(tmp / "nowhere")) is None, "atlas: a folder that is not there is no atlas (None, not a crash)")
        check(close(atlas_road_dist(at, 15.0, 3.0), 3.0, 0.01) and atlas_road_dist(at, 15.0, 300.0) == 150.0, "atlas: the distance to the nearest road, 150 m when none is near", str(atlas_road_dist(at, 15.0, 3.0)))
        check(close(atlas_road_dist(at, 15.0, 3.0, heading=90.0), 3.0, 0.01) and atlas_road_dist(at, 15.0, 3.0, heading=270.0) == 150.0, "atlas: with a heading only a lane that runs that way counts (the wrong side of the road is no lane)")
        qa = argparse.Namespace(what="lights", near="2,3", r=20.0, limit=10, name="", file="", atlas=adir, spots="", live=False, uv="", subject="", arg=[], timeout_s=5.0, json=True, from_="", to="", via=[],
                                attach="", game_dir="")
        buf = io.StringIO()
        sys.stdout = buf
        try:
            code = cmd_query(qa)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        check(code == 0 and j["result"]["in_radius"] == 2 and j["source"] == f"atlas {atlas_id(at)}" and j["args"]["near"] == [2.0, 3.0], "query: lights near a point, offline, in the game's own answer shape", str(j)[:140])
        # ---- lamps: the law of the heat pass, the camera, the ground census and the command (RT-LIGHTS-D0, D0-SCENES-2)
        cc = lamp_cone_cosines(125.0, -50.0)
        check(cc is not None and close(cc[0], math.cos(math.radians(62.5)), 1e-9) and close(cc[1], math.cos(math.radians(37.5)), 1e-9) and lamp_cone_cosines(360.0, 0.0) is None,
              "lamps: a street lamp's cone (125 degrees, spread -50) is full to 37.5 and gone at 62.5 degrees from its axis; 360 degrees has no cone", str(cc))
        lamp_l = {"i": 0, "pos": [0.0, 8.0, 0.0], "color": [1.0, 1.0, 1.0], "dir": [0.0, -1.0, 0.0], "intensity": 80.0, "cone": 125.0, "spread": -50.0, "type": "spot"}
        side = lamp_luma_at(lamp_l, (10.0, 0.0, 0.0))
        check(close(lamp_luma_at(lamp_l, (0.0, 0.0, 0.0)), 1.0, 1e-6) and 0.14 < side < 0.16 and lamp_luma_at(lamp_l, (20.0, 0.0, 0.0)) == 0.0 and lamp_luma_at(lamp_l, (0.0, 0.0, 0.0), (0.0, -1.0, 0.0)) == 0.0
              and lamp_luma_at(dict(lamp_l, intensity=0.0), (0.0, 0.0, 0.0)) == 0.0,
              "lamps: the law gives 1 under a lamp (term 1 inside sqrt(I), cone full), about 0.15 at 10 m to the side, 0 outside the cone, 0 to a surface facing away, 0 for a dead lamp", str((side, lamp_luma_at(lamp_l, (20.0, 0.0, 0.0)))))
        vc = ViewCam.make((0.0, 2.0, 0.0), (0.0, 2.0, 10.0), 60.0)
        ahead, right_, behind = vc.project((0.0, 2.0, 10.0)), vc.project((-3.0, 2.0, 10.0)), vc.project((0.0, 2.0, -5.0))
        check(ahead and close(ahead[0], 0.5, 1e-9) and close(ahead[1], 0.5, 1e-9) and close(ahead[2], 10.0, 1e-9) and right_ and right_[0] > 0.5 and behind is None,
              "lamps: a camera looking along +Z has the point ahead in the middle, a point at -X to its right (u over 0.5), a point behind it none", str((ahead, right_, behind)))
        row = [dict(lamp_l, i=k, pos=[-14.0 + 14.0 * k, 8.0, 0.0]) for k in range(5)]
        cen = ground_census(ViewCam.make((-30.0, 4.0, 0.0), (10.0, 0.0, 0.0), 60.0), row, 0.0, 48, 27, 100.0)
        check(cen["lit"] > 100 and cen["max_lamps"] >= 2 and cen["by_count"][1] + cen["by_count"][2] + cen["by_count"][3] + cen["by_count"][4] + cen["by_count"][5] == cen["lit"] and cen["per_light"] and 0.0 <= cen["lit_share"] <= 1.0,
              "lamps: the ground census of a row of five lamps lights ground, counts lamps a pixel, and adds up", str({k: v for k, v in cen.items() if k != "per_light"}))
        lamp_atlas = am.write_atlas(str(atlas_root), [dict(lamp_l, i=k, pos=[10.0 + 30.0 * k, 8.0, 7.0], color=[1.0, 1.0, 1.0], on=True, **({"src": "lamp", "model": "m"} if k >= 1 else {})) for k in range(6)],
                                    _g.parse_railnet(_g.synth_railnet()), "lamps selftest")
        la = argparse.Namespace(pos="40,0", radius=45.0, src="lamp", min_intensity=1.0, limit=10, cam="-10,3,0", look="60,2,0", fov=60.0, aspect=16.0 / 9.0, pixels=True, pixels_w=32, pixel_src="all",
                                ground_y=0.0, max_range=100.0, atlas=lamp_atlas, spots="", json=True)
        buf = io.StringIO()
        sys.stdout = buf
        try:
            code = cmd_lamps(la)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        ids = [l["i"] for l in j["lamps"]]
        check(code == 0 and ids == [1, 2] and j["lamps"][0]["lamp"] == 0 and j["lamps"][1]["lamp"] == 1 and all(l["view"]["in_frame"] for l in j["lamps"] if l["view"]) and j["view"]["census"]["lit"] > 0 and j["in_radius"] == 2,
              "lamps: the command lists the street lamps within the radius nearest first with their numbers (light 0 is a fill: not in src lamp, so light 1 is lamp 0), marks the ones in frame and gives the ground census", str(ids))
        qa.what, qa.near = "road", "15,3"
        buf = io.StringIO()
        sys.stdout = buf
        try:
            cmd_query(qa)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        check(j["result"]["nearest"][0]["dist_m"] == 3.0 and j["result"]["on_road"], "query: the nearest road with its distance")
        qa.what = "ground"
        try:
            cmd_query(argparse.Namespace(**{**vars(qa), "live": False, "what": "visible"}))
            check(False, "query: visible without --live is an error")
        except ToolError as e:
            check(e.code == "bad_args" and "--live" in e.msg, "query: a live-only query without --live says to add --live", e.msg)
        globals()["SPOTS_FILE"] = tmp / "scenes" / "spots.txt"
        ra = argparse.Namespace(from_="2,1", to="58,-1", via=[], speed=14.0, lat_accel=5.0, accel=3.0, brake=6.0, start_speed=None, end_speed=None, spacing=2.0, heading=None,
                                heights="auto", no_road=False, name="road-test", out="", dir=str(tmp / "r"), scene="", atlas=adir, spots_file="", force=False, json=True)
        buf = io.StringIO()
        sys.stdout = buf
        try:
            code = cmd_route(ra)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        rp = RouteSampler(read_route(j["file"]))
        check(code == 0 and j["road"] == "atlas" and j["heights"] and rp.has_y and 54 < rp.length() < 60 and max(rp.v) <= 14.0 + 1e-6, "route: along the atlas road, a valid route file with heights and the speed cap",
              f"{j['points']} points {j['length_m']} m {j['speed_min']}-{j['speed_max']}")
        check("SYN_Test_St." in j["streets"] and any("SYN_Test_St." in ln for ln in Path(j["file"]).read_text().splitlines()[:1]), "route: the file's header names the atlas and the street")
        try:
            cmd_route(ra)
            check(False, "route: an existing file is not replaced")
        except ToolError as e:
            check("--force" in e.msg, "route: an existing file is not replaced without --force")
        ra.from_, ra.to, ra.name = "58,0", "2,0", "backwards"
        try:
            cmd_route(ra)
            check(False, "route: no way backwards along a one-way road")
        except ToolError as e:
            check(e.code == "failed" and "no road route" in e.msg, "route: a way the lanes do not allow is an error that says so", e.msg)
        ra.from_, ra.to, ra.name, ra.no_road, ra.heights = "0,0", "100,100", "line-test", True, "no"
        buf = io.StringIO()
        sys.stdout = buf
        try:
            cmd_route(ra)
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        check(j["road"] == "none" and not j["heights"] and j["points"] > 50, "route: --no-road is a smooth line even with an atlas", str(j)[:140])
        ra.no_road, ra.atlas, ra.name = False, str(tmp / "none"), "noatlas-test"
        globals()["ATLAS_ROOT"] = tmp / "no-atlas-root"
        _AM = atlas_module()
        _AM_ROOT = _AM.ATLAS_ROOT
        _AM.ATLAS_ROOT = str(tmp / "no-atlas-root")
        try:
            buf = io.StringIO()
            sys.stdout = buf
            try:
                cmd_route(ra)
            finally:
                sys.stdout = old
        finally:
            _AM.ATLAS_ROOT = _AM_ROOT
        j = json.loads(buf.getvalue())
        check(j["road"] == "none" and any("NOT along the street" in w for w in j["warnings"]), "route: without an atlas it warns that the line is not along the street", str(j["warnings"]))
        rr = resample_route([[0, 0, 5.0, 1.0], [10, 0, 15.0, 3.0]], 2.0)
        check(len(rr) == 6 and close(rr[3][2], 11.0, 1e-9) and close(rr[3][3], 2.2, 1e-9) and rr[-1][0] == 10, "route: resampling keeps the ends and interpolates speed and height", str(rr[3]))
        # ---- new, slots
        rec = tmp / "recipes"
        rec.mkdir()
        (rec / "hero.scene").write_text("# hero turntable\nscene hero fps 60 step 1/60\ncam 0 pos 0 2 0 yaw 0 pitch 0\nend 5\n")
        globals()["RECIPES"] = rec
        buf = io.StringIO()
        sys.stdout = buf
        try:
            code = cmd_new(argparse.Namespace(name="mine", recipe="hero", list=False, dir=str(tmp / "out"), force=False, json=True))
        finally:
            sys.stdout = old
        made = (tmp / "out" / "mine.scene").read_text()
        check(code == 0 and "scene mine fps 60" in made and "hero turntable" in made, "new: a recipe is copied with the scene's own name", made.splitlines()[1])
        # ---- diff
        try:
            from PIL import Image
            for nm, col in (("A", (10, 20, 30)), ("B", (10, 20, 30)), ("C", (200, 20, 30))):
                d = tmp / "pv" / nm
                (d / "shots").mkdir(parents=True)
                Image.new("RGB", (64, 36), col).save(d / "shots" / "s1.png")
                (d / "preview.json").write_text(json.dumps({"shots": [{"t": 1.0, "file": "shots/s1.png"}]}))
            sa = image_stats(tmp / "pv" / "A" / "shots" / "s1.png", tmp / "pv" / "B" / "shots" / "s1.png")
            sc_ = image_stats(tmp / "pv" / "A" / "shots" / "s1.png", tmp / "pv" / "C" / "shots" / "s1.png")
            check(verdict_of(sa) == "IDENTICAL" and verdict_of(sc_) == "LOOSE" and close(sc_["mean"], 190 / 3.0, 1e-3), "diff: identical and very different pictures get their verdicts", f"{sa} {sc_}")
            for who, want in (("B", 0), ("C", 1)):
                buf = io.StringIO()
                sys.stdout = buf
                try:
                    code = cmd_diff(argparse.Namespace(a=str(tmp / "pv" / "A"), b=str(tmp / "pv" / who), out=str(tmp / "dout" / who), max_pairs=24, sheet_rows=5, take_diff_args=[], json=True))
                finally:
                    sys.stdout = old
                check(code == want and (tmp / "dout" / who / "diff.png").is_file(), f"diff: two previews {'alike' if want == 0 else 'unlike'} exit {want} and write diff.png", f"code {code}")
        except ImportError:
            check(True, "SKIP (Pillow): picture diff")
        # ---- errors as JSON
        buf = io.StringIO()
        sys.stdout = buf
        try:
            code = main(["lint", str(tmp / "nothing.scene"), "--json"])
        finally:
            sys.stdout = old
        j = json.loads(buf.getvalue())
        check(code == 2 and j["ok"] is False and j["error"]["code"] == "not_found", "main: a missing scene is exit 2 and a JSON error", str(j)[:100])
    finally:
        globals().update(RECIPES=saved[0], SPOTS_FILE=saved[1], SCENES=saved[2])
        shutil.rmtree(tmp, ignore_errors=True)
    return results


if __name__ == "__main__":
    sys.exit(main())
