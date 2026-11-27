"""Check LARecomp scene syntax, timeline order, setting names, spots and routes."""

import argparse
import difflib
import json
import math
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE.parents[1] / "src"
DEFAULT_TARGET = "director"
TARGETS = ("legacy", "director")
# These messages identify game executables that understand Director spots and rigs.
DIRECTOR_EXE_MARKERS = (b"a spot name is letters, digits", b"cam: unknown rig")

WEATHER = ("nice", "cloudy", "stormy", "foggy")
EASES = ("linear", "in", "out", "inout")
SIDES = ("left", "right", "center", "top", "bottom")
WORLD_KEYS = ("hour", "weather", "traffic", "peds", "parked", "police", "timescale")
TOGGLES = ("traffic", "peds", "parked", "police")
OPAQUE = ("cam", "cut", "hud", "car", "start")
VERB_LIST = "hour, weather, traffic, peds, parked, police, timescale, set, label, mark, split, preset, preset_label, capture, cam, cut, hud, car, start, world, end, statelog"
MIN_CAPTURE_FPS = 10.0
CAPTURE_SINKS = ("none", "ffmpeg", "png", "raw")  # capture_core.h SinkKind (M2b)
TAKE_NAME = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$")  # a take's folder name (scene.cpp IsTakeName)
MAX_CAPTURE_FPS = 1000.0
MAX_SECONDS = 86400.0
MAX_HOUR = 48.0
MAX_TIMESCALE = 10.0
EPS = 1e-9
HUD_EXPECTED = "hud: expected off, on, an alpha from 0 to 1, 'fade <a>..<b> to <x>' or 'fade over <s> to <x>'"

NUMBER = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MARK = re.compile(r"^[A-Za-z0-9_.:-]+$")
CAR_REF = re.compile(r"\bcar:([A-Za-z_][A-Za-z0-9_]*)")
CAR_MODEL = re.compile(r"^[a-z0-9_.-]{1,47}$")
CAR_MAX_COORD = 100000.0
CAR_SPAWN_USAGE = "car <id> spawn <model|profile N> (at <x,y,z> | near <car> <right,up,forward>) [heading <deg>] [paint <n>]"
CAR_AMBIENT_USAGE = "car <id> ambient <model> (at <x,y,z> | near <car> <right,up,forward>) [heading <deg>] [paint <n>]"
CAR_PLACE_USAGE = "car player at <x,y,z> [heading <deg>]"
CAR_MAX_NEAR = 1000.0  # car_verbs.h kMaxNearOffset
AMBIENT_MODEL_CHARS = 31  # the layout item's name buffer
DENSITY_TRACKS = ("peds", "parked")  # on|off or a density scale
DENSITY_SETTING = {"peds": "ped_density_scale", "parked": "parked_car_scale"}
MAX_DENSITY_SCALE = 2.0
STATE_SCALE = 2  # Ev.state of a peds / parked key that carries a density scale in .to (scene.h kStateScale)
CAR_PATH_USAGE = "car <id> path <route file> [speed <x>] [mode brain|kinematic]"
ROUTE_NAME_CHARS = re.compile(r"^[A-Za-z0-9_./ -]+$")
ROUTE_MAX_NAME = 120
ROUTE_MAX_POINTS = 20000
PATH_MIN_SPEED, PATH_MAX_SPEED = 0.5, 120.0
PATH_MIN_SCALE, PATH_MAX_SCALE = 0.05, 10.0

# What a scene may `set`: the rule of CvarRefusal in src\mc_engine\cinematic\scene.cpp (scene_test.cpp and the selftest below check the same
# names). A scene file is data that gets sent around, so `set` must not start a program or write a file: every mcla_capture_* (the take's
# program, arguments, sink and folder), mcla_scene*, every name that holds "dump", ends in one of the suffixes below or is in the list.
REFUSED_CVARS = (
    "cl", "user_name", "import_vinyl", "export_vinyl", "capture_vinyl_shapes", "allow_game_relative_writes", "lzx_stats", "menu_cam_record",
    "wheel_learn", "mcla_gpu_trace_frame", "mcla_gpu_trace_count", "mcla_native_gfx_capture", "mcla_native_gfx_firstdraw",
    "mcla_native_gfx_ab_cvar", "rmr_server", "online_master_server", "server_browser_servers", "server_browser_pending",
    "mcla_state_log", "mcla_freecam_record", "mcla_freecam_out",
)
REFUSED_SUFFIXES = (
    "_path", "_paths", "_dir", "_dirs", "_folder", "_file", "_files", "_root", "_exe", "_args", "_command", "_cmd", "_csv",
    "_probe", "_diag", "_trace", "_census", "_telemetry",
)


def scene_cpp_refusal_lists():
    """(kRefusedCvars, kRefusedSuffixes) of src\\mc_engine\\cinematic\\scene.cpp, sorted, to compare with the lists above; None when no checkout is
    at hand (SCENE_CHECK_SCENE_CPP, or the default source folder's) or its scene.cpp predates the rule."""
    for cand in (os.environ.get("SCENE_CHECK_SCENE_CPP"), str(Path(DEFAULT_SRC) / "mc_engine" / "cinematic" / "scene.cpp")):
        if not cand or not Path(cand).is_file():
            continue
        text = Path(cand).read_text(encoding="utf-8", errors="replace")
        found = []
        for var in ("kRefusedCvars", "kRefusedSuffixes"):
            m = re.search(var + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
            if not m:
                found = []
                break
            body = re.sub(r"//[^\n]*", "", m.group(1))
            found.append(sorted(re.findall(r'"([^"]*)"', body)))
        if found:
            return found[0], found[1]
    return None


def cvar_refusal(name):
    """The sentence of scene.cpp CvarRefusal when a scene may not set `name`, or "" when it may."""
    n = name.lower()
    if n.startswith("mcla_scene") or n.startswith("mcla_director"):
        return f"a scene does not set {name}"
    if (n.startswith("mcla_capture_") or "dump" in n or n.endswith(REFUSED_SUFFIXES) or n in REFUSED_CVARS):
        return (f"a scene does not set {name}: it names a program, a path, a server or arguments, or makes the game write a file of its own "
                "(put it on the command line)")
    return ""


def car_slot_ok(s):
    return bool(re.fullmatch(r"\d{1,4}", s)) and int(s) <= 999


def car_paint_ok(s):
    """`paint <n>` of car_verbs.h: a whole number 0 to 9999 (SCENE-DETERMINISM)."""
    return bool(re.fullmatch(r"\d{1,4}", s))


def car_number(s):
    try:
        v = float(s)
    except ValueError:
        return None
    return v if math.isfinite(v) else None


def car_triple(w, i, first, key, shape, what):
    """car_verbs.h ReadTriple: the three numbers at w[first:] as one word a,b,c or as three words. (message or None, the numbers, the index after them)."""
    if first >= len(w):
        return f"{key} needs {shape}", None, i
    if "," in w[first]:
        parts = w[first].split(",")
        vals = [car_number(x) for x in parts]
        if len(parts) != 3 or any(v is None for v in vals):
            return f"bad {what} '{w[first]}' ({shape}: three numbers)", None, i
        return None, vals, first + 1
    if first + 2 >= len(w):
        return f"{key} needs {shape} (as {shape} or as three numbers)", None, i
    vals = [car_number(x) for x in w[first:first + 3]]
    for k, v in enumerate(vals):
        if v is None:
            return f"bad {what} '{w[first + k]}' ({shape}: three numbers)", None, i
    return None, vals, first + 3


def car_spawn_error(w, ambient=False):
    """The message of car_verbs.h for a bad `car <id> spawn ...` or, with `ambient`, `car <id> ambient ...` (w = the words after `car`), or None."""
    verb = "ambient" if ambient else "spawn"
    usage = CAR_AMBIENT_USAGE if ambient else CAR_SPAWN_USAGE
    if len(w) < 3:
        return f"{'ambient what?' if ambient else 'spawn what?'} {usage}"
    i = 3
    what = w[2].lower()
    if ambient:
        if what == "profile" or ":" in what:
            return (f"'{w[2]}' is not an ambient type: an ambient car is made from a vehicle name of the game's traffic list (va_gmc_yukon_05), "
                    "not a garage car")
        if not CAR_MODEL.match(what) or len(what) > AMBIENT_MODEL_CHARS:
            return f"'{w[2]}' is not an ambient type name (lower case letters, digits and _ - ., up to 31)"
    elif what == "profile":
        if len(w) < 4 or not car_slot_ok(w[3]):
            return "spawn profile needs a garage slot, a whole number from 0 (car <id> spawn profile 2 at ...)"
        i = 4
    elif (what.startswith("garage:") and car_slot_ok(what[7:])) or (what.startswith("slot:") and car_slot_ok(what[5:])):
        pass
    elif ":" in what:
        return f"'{w[2]}' is not a car: a vehicle name, profile N, garage:N or slot:N"
    elif not CAR_MODEL.match(what):
        return f"'{w[2]}' is not a vehicle name (lower case letters, digits and _ - ., up to 47)"
    have_at = False
    have_near = False
    have_heading = False
    have_paint = False
    garage = what == "profile" or what.startswith("garage:") or what.startswith("slot:")
    while i < len(w):
        key = w[i].lower()
        if key == "at":
            if have_at:
                return f"{verb}: a second 'at'"
            why, vals, i = car_triple(w, i, i + 1, "at", "x,y,z", "position")
            if why:
                return why
            if any(abs(v) > CAR_MAX_COORD for v in vals):
                return "the position is outside the world (more than 100000 from the origin)"
            have_at = True
        elif key == "near":
            if have_near:
                return f"{verb}: a second 'near'"
            if i + 1 >= len(w):
                return "near needs a car and a place in its frame (near player 3.35,0,0: right, up, forward in metres)"
            anchor = w[i + 1]
            if not IDENT.match(anchor):
                return f"near needs the id of a car, or player ('{anchor}' is not one): near player 3.35,0,0 (right, up, forward in metres)"
            if anchor.lower() == "player":
                anchor = "player"
            if anchor == w[0]:
                return f"car {w[0]} cannot be near itself"
            why, vals, i = car_triple(w, i, i + 2, "near", "right,up,forward", "offset")
            if why:
                return why
            if any(abs(v) > CAR_MAX_NEAR for v in vals):
                return "the offset is more than 1000 m from the car it is near: a car that far is placed with `at`"
            have_near = True
        elif key == "heading":
            if have_heading:
                return f"{verb}: a second 'heading'"
            if i + 1 >= len(w) or car_number(w[i + 1]) is None:
                return "heading needs degrees (heading 270)"
            have_heading = True
            i += 2
        elif key in ("paint", "colour", "color"):
            if have_paint:
                return f"{verb}: a second 'paint'"
            if garage:
                return "a garage car (spawn profile N) keeps the paint the owner gave it: paint is for a model name or an ambient car"
            if i + 1 >= len(w) or not car_paint_ok(w[i + 1]):
                return "paint needs a whole number from 0 to 9999 (paint 3)"
            have_paint = True
            i += 2
        else:
            return f"{verb}: unknown option '{w[i]}' (at, near, heading, paint)"
    if have_at and have_near:
        return f"{verb}: `at` (a place in the world) and `near` (a place in a car's frame) do not go together: give one of them"
    if not have_at and not have_near:
        return f"{verb} needs a place: {usage}"
    return None


def car_place_error(w):
    """car_verbs.h ParsePlace: the message for a bad `car player at <x,y,z> [heading <deg>]` (w = the words after `car`), or None."""
    have_at = have_heading = False
    i = 1
    while i < len(w):
        key = w[i].lower()
        if key == "at":
            if have_at:
                return "car player at: a second 'at'"
            why, vals, i = car_triple(w, i, i + 1, "at", "x,y,z", "position")
            if why:
                return why
            if any(abs(v) > CAR_MAX_COORD for v in vals):
                return "the position is outside the world (more than 100000 from the origin)"
            have_at = True
        elif key == "heading":
            if have_heading:
                return "car player at: a second 'heading'"
            if i + 1 >= len(w) or car_number(w[i + 1]) is None:
                return "heading needs degrees (heading 270)"
            have_heading = True
            i += 2
        else:
            return f"car player at: unknown option '{w[i]}' (heading; a car near another is `spawn ... near`)"
    if not have_at:
        return f"car player at needs a place: {CAR_PLACE_USAGE}"
    return None


def route_name_ok(name):
    """car_verbs.h IsRoutePath: a relative path of letters, digits and _ - . / backslash and blanks; no .. part, no drive, no leading slash."""
    if not name or len(name) > ROUTE_MAX_NAME or name[0] in "/" + chr(92):
        return False
    flat = name.replace(chr(92), "/")
    if not ROUTE_NAME_CHARS.match(flat):
        return False
    return all(part not in ("..", ".") for part in flat.split("/"))


def speed_word_error(word):
    """path_follow.h ParseSpeedWord: the message for a bad speed word, or None."""
    w = word.lower()
    factor_msg = f"speed '{word}': a factor of the route's own speeds is x1.5 (or 1.5x)"
    if w.startswith("x"):
        v = car_number(w[1:])
        if v is None:
            return factor_msg
        scale = True
    elif len(w) > 1 and w.endswith("x"):
        v = car_number(w[:-1])
        if v is None:
            return factor_msg
        scale = True
    else:
        v = car_number(w)
        if v is None:
            return f"speed needs metres a second (speed 12) or a factor of the route's own speeds (speed x1.5), not '{word}'"
        scale = False
    if scale and not (PATH_MIN_SCALE <= v <= PATH_MAX_SCALE):
        return f"a speed factor from {PATH_MIN_SCALE:g} to {PATH_MAX_SCALE:g} (speed x1.5), not {v:g}"
    if not scale and not (PATH_MIN_SPEED <= v <= PATH_MAX_SPEED):
        return f"a speed from {PATH_MIN_SPEED:g} to {PATH_MAX_SPEED:g} metres a second (speed 12), not {v:g}"
    return None


def car_path_error(w):
    """car_verbs.h ParsePath: the message for a bad `car <id> path ...` (w = the words after `car`), or None."""
    if len(w) < 3:
        return f"path needs a route file: {CAR_PATH_USAGE}"
    if not route_name_ok(w[2]):
        return (f"'{w[2]}' is not a route file name (a name or a relative path next to the scene file or in its routes folder: letters, digits and "
                "_ - . / and blanks; no .. and no drive or leading slash)")
    have_speed = have_mode = False
    i = 3
    while i < len(w):
        key = w[i].lower()
        if key == "speed":
            if have_speed:
                return "path: a second 'speed'"
            if i + 1 >= len(w):
                return "speed needs metres a second (speed 12) or a factor of the route's own speeds (speed x1.5)"
            why = speed_word_error(w[i + 1])
            if why:
                return why
            have_speed = True
            i += 2
        elif key == "mode":
            if have_mode:
                return "path: a second 'mode'"
            if i + 1 >= len(w):
                return "mode needs brain or kinematic"
            if w[i + 1].lower() not in ("brain", "kinematic"):
                return f"mode '{w[i + 1]}': brain (the game's own AI drives) or kinematic (the car is put on the route at every update)"
            have_mode = True
            i += 2
        else:
            return f"path: unknown option '{w[i]}' (speed, mode)"
    return None


def route_text_error(text):
    """path_follow.cpp ParseRoute and PathSampler::Build: the message for the text of a bad route file, or None."""
    pts = []
    with_y = 0
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.split("#", 1)[0].replace(",", " ").replace("\t", " ").replace("\r", " ")
        w = line.split()
        if not w:
            continue
        where = f"line {n}: "
        if len(w) < 2 or len(w) > 4:
            return where + f"a route point is x,z[,speed[,y]] ({len(w)} numbers here)"
        vals = []
        for tok in w:
            v = car_number(tok)
            if v is None:
                return where + f"'{tok}' is not a number"
            vals.append(v)
        if abs(vals[0]) > 100000.0 or abs(vals[1]) > 100000.0 or (len(w) == 4 and abs(vals[3]) > 100000.0):
            return where + "a coordinate outside the world (more than 100000 from the origin)"
        if len(w) >= 3 and (vals[2] < 0.0 or vals[2] > 1000.0):
            return where + f"a speed from 0 to 1000 metres a second, not {vals[2]:g}"
        if len(pts) >= ROUTE_MAX_POINTS:
            return where + f"more than {ROUTE_MAX_POINTS} points"
        pts.append((vals[0], vals[1]))
        with_y += 1 if len(w) == 4 else 0
    if len(pts) < 2:
        return f"a route needs at least 2 points (x,z[,speed[,y]] a line), this one has {len(pts)}"
    if with_y not in (0, len(pts)):
        return f"the height (the fourth number) is on {with_y} of {len(pts)} points: give it on all of them or on none"
    kept = [pts[0]]
    length = 0.0
    for q in pts[1:]:
        d = math.hypot(q[0] - kept[-1][0], q[1] - kept[-1][1])
        if d < 0.01:
            continue
        length += d
        kept.append(q)
    if len(kept) < 2:
        return "the route has no two points more than 1 cm apart"
    if length < 1.0:
        return f"the route is {length:g} m long: a path needs at least 1 m"
    return None


def find_route_file(scene_dir, name):
    """cars.cpp FindRouteFile: (the file, None) or (None, the places looked in)."""
    rel = name.replace(chr(92), "/")
    names = [rel] + ([rel + ".route"] if "." not in Path(rel).name else [])
    tried = []
    for base in (scene_dir, scene_dir / "routes"):
        for n in names:
            c = base / n
            if c.is_file():
                return c, None
            tried.append(str(c))
    return None, tried


def check_flyby_routes(scene, scene_path):
    """The route files of the camera's `rig flyby path <route>`: found beside the scene file, routes, and with heights (or the key gives `y`).
    Adds errors to scene.diags."""
    seen = {}
    for e in scene.sorted_events():
        if e.track != "cam" or len(e.words) < 2 or e.words[0].lower() != "rig" or e.words[1].lower() != "flyby":
            continue
        why, info = rig_error(e.words)
        if why or not info["route"]:
            continue
        name = info["route"]
        if name not in seen:
            found, tried = find_route_file(Path(scene_path).resolve().parent, name)
            if found is None:
                seen[name] = (f"no route file '{name}' (looked for {', '.join(tried)})", False)
            else:
                try:
                    text = found.read_text(encoding="utf-8")
                    seen[name] = (route_text_error(text), route_height_state(text))
                except (OSError, UnicodeDecodeError) as ex:
                    seen[name] = (f"cannot read the route file: {ex}", False)
        problem, has_heights = seen[name]
        if problem:
            scene.diags.append(Diag(e.line, "error", f"cam: rig flyby: route '{name}': {problem}"))
        elif info["has_up"] and not has_heights:
            scene.diags.append(Diag(e.line, "error", f"cam: rig flyby: up is above the route's own height, and route '{name}' has none: use y <metres>"))
        elif not info["has_y"] and not has_heights:
            scene.diags.append(Diag(e.line, "error", f"cam: rig flyby: route '{name}' has no heights (x,z,speed,y): give the camera's height in the world with y <metres>"))


def check_routes(scene, scene_path):
    """The route files of the scene's path keys (and of the camera's flybys): they must be found beside the scene file and be routes. Adds errors to scene.diags."""
    check_flyby_routes(scene, scene_path)
    seen = {}
    for e in scene.sorted_events():
        if e.track != "car" or len(e.words) < 3 or e.words[1].lower() != "path" or not route_name_ok(e.words[2]):
            continue
        name = e.words[2]
        if name not in seen:
            found, tried = find_route_file(Path(scene_path).resolve().parent, name)
            if found is None:
                seen[name] = f"no route file '{name}' (looked for {', '.join(tried)})"
            else:
                try:
                    seen[name] = route_text_error(found.read_text(encoding="utf-8"))
                except (OSError, UnicodeDecodeError) as ex:
                    seen[name] = f"cannot read the route file: {ex}"
        if seen[name]:
            scene.diags.append(Diag(e.line, "error", f"route '{name}': {seen[name]}"))


def car_words_error(w):
    """car_verbs.cpp's ParseCarCommand: the message for a bad car statement, or None (fine, or another package's verb)."""
    if len(w) < 2:
        return "car <id> <what the car does>"
    verb = w[1].lower()
    player = w[0].lower() == "player"
    if verb == "spawn":
        if player:
            return "'player' is the player's own car: pick another id (only `car player profile N` and `car player at ...` apply to it)"
        return car_spawn_error(w)
    if verb == "ambient":
        if player:
            return "'player' is the player's own car: pick another id (only `car player profile N` and `car player at ...` apply to it)"
        return car_spawn_error(w, ambient=True)
    if verb == "at":
        if not player:
            return ("only the player's car is put with `at` (car player at <x,y,z> [heading <deg>]); a spawned car is placed by its own spawn or ambient line "
                    "(`at`, or `near` another car)")
        return car_place_error(w)
    if verb in ("delete", "despawn"):
        if player:
            return "'player' is the player's own car: it cannot be deleted"
        if len(w) != 2:
            return "car <id> delete takes nothing after it"
        return None
    if verb == "profile":
        if not player:
            return "only the player's car is dressed with `profile` (car player profile N); a spawned car takes its slot in `spawn profile N`"
        if len(w) != 3 or not car_slot_ok(w[2]):
            return "car player profile <N>: N is a garage slot, a whole number from 0"
        return None
    if verb == "path":
        if player:
            return "'player' is the player's own car: a path is for the scene's cars"
        return car_path_error(w)
    if verb == "stop":
        if player:
            return "'player' is the player's own car: it cannot be stopped by a scene"
        if len(w) != 2:
            return "car <id> stop takes nothing after it"
        return None
    return None


# ------------------------------------------------------------------------------------------------------------------------------- spots and rigs
# The spot words of spots.h (spot:<name>[@right,up,forward[,turn]]), the spots.txt reader (spots.cpp) and the grammar of `cam <T> rig <kind> ...`
# (camera.cpp, RigStatement). The rules and the messages are the game's; spots_test.cpp and camera_test.cpp have the same cases.
SPOT_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,39}$")
SPOT_MAX_OFFSET = 10000.0
SPOT_MAX_COORD = 100000.0


@dataclass
class Spot:
    name: str
    x: float
    y: float
    z: float
    heading: float = 0.0
    has_heading: bool = False
    note: str = ""
    line: int = 0
    file: str = ""


def is_spot_word(w):
    return len(w) >= 5 and w[:5].lower() == "spot:"


def spot_number(s):
    """A number the way spots.h reads one: plain decimal, an exponent, a sign (no hex, no inf or nan), or None."""
    return float(s) if NUMBER.match(s) else None


def format_spot_number(v, decimals=3):
    """spots.h FormatNumber: '%.3f' with the zeros after the point dropped."""
    s = f"{v:.{decimals}f}"
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def edit_distance(a, b):
    prev = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        cur = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (0 if a[i - 1] == b[j - 1] else 1))
        prev = cur
    return prev[len(b)]


def suggest_spots(table, name, limit=3):
    """spots.h SpotTable::Suggest: names that are near `name` (a part of one, or a few edits away), nearest first."""
    n = name.lower()
    scored = []
    for s in table.values():
        d = edit_distance(n, s.name)
        if n and (n in s.name or s.name in n):
            d = min(d, 1)
        if d <= 2 or (d <= 3 and len(n) >= 8):
            scored.append((d, s.name))
    scored.sort(key=lambda p: p[0])  # stable: the file's order among equals
    return [p[1] for p in scored[:limit]]


def parse_spot_ref(word):
    """spots.h ParseSpotRef: ((name, offset or None, turn or None), None) or (None, the message)."""
    rest = word[5:]
    at = rest.find("@")
    name = rest if at < 0 else rest[:at]
    if not SPOT_NAME.match(name):
        return None, f"'{word}': a spot name is letters, digits and _ - . (spot:hw-street, spot:hw-street@0,1.7,-8)"
    if at < 0:
        return (name, None, None), None
    parts = rest[at + 1:].split(",")
    vals = [spot_number(p) for p in parts]
    if len(parts) > 4 or any(v is None or abs(v) > SPOT_MAX_OFFSET for v in vals):
        return None, (f"'{word}': after @ come the right, up and forward metres, and optionally a turn in degrees "
                      f"(spot:{name}@1.5,0,-3 or spot:{name}@1.5,0,0,180)")
    if len(vals) < 3:
        return None, f"'{word}': @ needs right,up,forward (three numbers; spot:{name}@0,1.7,-8), got {len(vals)}"
    return (name, tuple(vals[:3]), vals[3] if len(vals) == 4 else None), None


def spot_position(spot, offset):
    """(x, y, z, None) of a spot and an offset in its heading frame, or (None, None, None, the message)."""
    x, y, z = spot.x, spot.y, spot.z
    if offset is None or offset == (0.0, 0.0, 0.0):
        return x, y, z, None
    if not spot.has_heading:
        return None, None, None, f"spot {spot.name} has no heading, so an offset in its frame (@right,up,forward) means nothing: write a heading in spots.txt"
    h = math.radians(spot.heading)
    r, u, f = offset
    return x + r * -math.cos(h) + f * math.sin(h), y + u, z + r * math.sin(h) + f * math.cos(h), None


def spot_heading(spot, turn):
    """(degrees in [0, 360), None) or (None, the message)."""
    if not spot.has_heading:
        return None, f"spot {spot.name} has no heading: write one in spots.txt (the fifth number of its line)"
    return (spot.heading + (turn or 0.0)) % 360.0, None


def expand_spot_words(words, table):
    """spots.h ExpandSpotWords: (the words with every spot word turned into numbers, None) or (None, the message for the statement's line).
    A spot word after `heading` or `yaw` is one word (degrees); any other is three (x y z). `table` None: no table is loaded."""
    if not any(is_spot_word(w) for w in words):
        return list(words), None
    orbit = any(w.lower() == "orbit" for w in words)
    # `car <id> spawn|ambient <model> near <car> <right,up,forward> [heading <deg>]` (car_verbs.h): after `near` the three numbers are an offset in the other car's frame and a
    # heading is relative to that car's heading, so a spot (a place and a heading in the world) means nothing there. near_at = where the word `near` is (-1: none). Only a car's
    # spawn and ambient have it: a camera's `near <clip>` is a number and takes no spot word.
    near_at = -1
    if len(words) > 3 and words[1].lower() in ("spawn", "ambient"):
        near_at = next((k for k in range(2, len(words)) if words[k].lower() == "near"), -1)
    out = []
    for i, w in enumerate(words):
        if not is_spot_word(w):
            out.append(w)
            continue
        if near_at >= 0 and i > near_at:
            if i == near_at + 2:
                return None, (f"'{w}': after `near <car>`, the three numbers are an offset in that car's frame (right up forward), not a place in the world: "
                              f"give the offset as numbers, or place the car with `at {w}`")
            if words[i - 1].lower() in ("heading", "yaw"):
                return None, (f"'{w}': after `near`, `heading` is degrees from the other car's heading, not a heading in the world: give a number, "
                              f"or place the car with `at` and a spot heading")
        ref, why = parse_spot_ref(w)
        if ref is None:
            return None, why
        if table is None:
            return None, f"'{w}': no spot table is loaded for this scene (a spots.txt beside the scene file or in the shared scenes folder)"
        name, offset, turn = ref
        spot = table.get(name.lower())
        if spot is None:
            near = suggest_spots(table, name)
            return None, f"unknown spot '{name}'" + (f" (did you mean {', '.join(near)}?)" if near else f" ({len(table)} spots are known: see spots.txt)")
        prev = words[i - 1].lower() if i > 0 else ""
        if prev in ("heading", "yaw"):
            deg, why = spot_heading(spot, turn)
            if deg is None:
                return None, why
            out.append(format_spot_number(deg, 2))
            continue
        if orbit and prev == "at":
            return None, f"'{w}': after an orbit, `at` is an offset in the car's frame (right up forward), not a place in the world"
        x, y, z, why = spot_position(spot, offset)
        if why:
            return None, why
        out += [format_spot_number(x), format_spot_number(y), format_spot_number(z)]
    return out, None


def parse_spots_text(text, file=""):
    """spots.cpp ParseSpots: (table {name: Spot}, [(line, severity, message)]). Later lines replace earlier ones of the same name."""
    table, problems = {}, []
    if text.startswith("﻿"):
        text = text[1:]
    for n, raw in enumerate(text.split("\n"), 1):
        body, note = raw, ""
        for i, c in enumerate(raw):
            if c == "#" and (i == 0 or raw[i - 1] in " \t"):
                body, note = raw[:i], raw[i + 1:].strip()
                break
        f = body.replace(",", " ").split()
        if not f:
            continue
        if not SPOT_NAME.match(f[0]):
            problems.append((n, "error", f"'{f[0]}' is not a spot name (letters, digits and _ - . , up to 40, starting with a letter or a digit)"))
            continue
        if len(f) not in (4, 5):
            problems.append((n, "error", f"spot {f[0]}: <name> <x> <y> <z> [<heading>] (got {len(f) - 1} numbers)"))
            continue
        vals = [spot_number(t) for t in f[1:]]
        bad = next((t for t, v in zip(f[1:], vals) if v is None), None)
        if bad is not None:
            problems.append((n, "error", f"spot {f[0]}: '{bad}' is not a number"))
            continue
        if any(abs(v) > SPOT_MAX_COORD for v in vals[:3]):
            problems.append((n, "error", f"spot {f[0]}: a coordinate outside the world (more than 100000 from the origin)"))
            continue
        name = f[0].lower()
        before = table.get(name)
        if before is not None and before.file == file:
            problems.append((n, "warning", f"spot {name} was already given on line {before.line}: this line replaces it"))
        table[name] = Spot(name, vals[0], vals[1], vals[2], (vals[3] % 360.0) if len(vals) == 4 else 0.0, len(vals) == 4, note, n, file)
    return table, problems


def spots_files_for(scene_path):
    """spots.cpp SpotFilesFor: the spots files that apply to a scene file, shared first: <dir>/../scenes/spots.txt, <dir>/../spots.txt, <dir>/spots.txt."""
    d = Path(scene_path).resolve().parent
    out, seen = [], set()
    for c in (d / ".." / "scenes" / "spots.txt", d / ".." / "spots.txt", d / "spots.txt"):
        if c.is_file():
            key = os.path.normcase(str(c.resolve()))
            if key not in seen:
                seen.add(key)
                out.append(c)
    return out


def load_spots_for(scene_path):
    """(the table, [problems as text]) for a scene file: every spots file that applies, the one beside the scene on top."""
    table, problems = {}, []
    for p in spots_files_for(scene_path):
        try:
            t, probs = parse_spots_text(p.read_text(encoding="utf-8"), str(p))
        except (OSError, UnicodeDecodeError) as ex:
            problems.append(f"{p}: error: cannot read the file: {ex}")
            continue
        table.update(t)
        problems += [f"{p}:{n}: {sev}: {msg}" for n, sev, msg in probs if sev == "error"]
    return table, problems


# --- the rigs: `cam <T> rig <kind> ...`
RIG_KINDS = "chase, orbit, tripod, flyby, hood, bumper or roof"
RIG_OWN = {
    "chase": (("dist", 1), ("height", 1), ("side", 1), ("lag", 1), ("aim", 1), ("ahead", 1)),
    "orbit": (("r", 1), ("az", 1), ("el", 1), ("over", 1), ("at", 3), ("lag", 1)),
    "tripod": (("at", 3), ("aimlag", 1)),
    "flyby": (("path", 1), ("speed", 1), ("up", 1), ("y", 1), ("aimlag", 1)),
    "mount": (("ahead", 1), ("lag", 1), ("shift", 3)),
}
RIG_COMMON = (("fov", 1), ("roll", 1), ("near", 1), ("far", 1), ("ease", 1))
RIG_AIM = (("yaw", 1), ("pitch", 1))
RIG_MOUNTS = ("hood", "bumper", "roof")
CAM_CAR = re.compile(r"^car:([A-Za-z0-9_]+)$", re.I)


def rig_number(args, word, lo, hi, unit, default, kind):
    """(value, None) or (None, the message): camera.cpp RigNumber."""
    g = args.get(word)
    if g is None:
        return default, None
    v = spot_number(g[0])
    if v is None:
        return None, f"cam: rig {kind}: '{word}' needs a number (got '{g[0]}')"
    if v < lo or v > hi:
        return None, f"cam: rig {kind}: {word} {fmt(v)} ({fmt(lo)} to {fmt(hi)}{unit})"
    return v, None


def rig_triple(words, word, limit, kind):
    vals = []
    for s in words:
        v = spot_number(s)
        if v is None or abs(v) > limit:
            return None, f"cam: rig {kind}: '{word}' needs three numbers within {fmt(limit)} (got '{s}')"
        vals.append(v)
    return vals, None


def rig_pass_error(passed):
    """The rules camera.cpp's key parser applies to the words a rig hands on (fov, roll, near, far, ease, yaw, pitch), or None."""
    vals = {}
    i = 0
    while i < len(passed):
        key, arg = passed[i].lower(), passed[i + 1]
        i += 2
        if key == "ease":
            if arg.lower() not in EASES:
                return f"cam: bad ease '{arg}' (linear, in, out or inout)"
            continue
        v = spot_number(arg)
        if v is None:
            return f"cam: '{key}' needs a number (got '{arg}')"
        vals[key] = v
    if "fov" in vals and not (1.0 <= vals["fov"] <= 170.0):
        return f"cam: fov {fmt(vals['fov'])} (1 to 170 degrees, vertical)"
    if "near" in vals and not (0.0 < vals["near"] <= 1000.0):
        return f"cam: near {fmt(vals['near'])} (more than 0, at most 1000 metres)"
    if "far" in vals and not (0.0 < vals["far"] <= 100000.0):
        return f"cam: far {fmt(vals['far'])} (more than 0, at most 100000 metres)"
    if "near" in vals and "far" in vals and vals["far"] <= vals["near"]:
        return f"cam: far {fmt(vals['far'])} is not beyond near {fmt(vals['near'])}"
    return None


def rig_error(words):
    """camera.cpp RigStatement: (the message for a bad `cam <T> rig ...` (words = the words after the time), or None; what the lint needs of it:
    {"kind", "sweep" (seconds of an az/el sweep, else 0), "route" (a flyby's path), "has_y", "has_up"})."""
    info = {"kind": "", "sweep": 0.0, "route": None, "has_y": False, "has_up": False}
    if len(words) < 2:
        return f"cam: rig needs a kind ({RIG_KINDS})", info
    kind = words[1].lower()
    if kind not in ("chase", "orbit", "tripod", "flyby") + RIG_MOUNTS:
        return f"cam: unknown rig '{words[1]}' ({RIG_KINDS})", info
    info["kind"] = kind
    own = RIG_OWN["mount" if kind in RIG_MOUNTS else kind]
    aim = kind in ("tripod", "flyby")
    subject = not aim

    def fail(msg):
        return f"cam: rig {kind}: {msg}", info

    def lst(t):
        return ", ".join(w for w, _ in t)

    args, passed = {}, []
    car, track, track_car, track_nums, has_aim_words = None, False, False, [], False
    i = 2
    while i < len(words):
        key = words[i].lower()
        m = CAM_CAR.match(words[i])
        if m:
            if not subject:
                return fail(f"'{words[i]}' goes after track (track {words[i]})")
            if car is not None:
                return fail(f"two cars ({car} and {m.group(1)})")
            car = m.group(1)
            i += 1
            continue
        if key == "track" and not subject:
            if track:
                return fail("track twice")
            track = True
            i += 1
            if i < len(words) and CAM_CAR.match(words[i]):
                track_car = True
                i += 1
                if i < len(words) and spot_number(words[i]) is not None:
                    if i + 2 >= len(words) or spot_number(words[i + 1]) is None or spot_number(words[i + 2]) is None:
                        return fail("track car:<id> takes no offset or three numbers (<dx> <dy> <dz>)")
                    track_nums = words[i:i + 3]
                    i += 3
            else:
                if i + 2 >= len(words) or any(spot_number(words[i + k]) is None for k in range(3)):
                    return fail("track needs a car (track car:<id> [<dx> <dy> <dz>]) or a place (track <x> <y> <z>)")
                track_nums = words[i:i + 3]
                i += 3
            continue
        found, is_pass = None, False
        for t in own:
            if key == t[0]:
                found = t
        for t in RIG_COMMON:
            if key == t[0]:
                found, is_pass = t, True
        if aim:
            for t in RIG_AIM:
                if key == t[0]:
                    found, is_pass, has_aim_words = t, True, True
        if found is None:
            names = lst(own) + ", " + lst(RIG_COMMON) + (", " + lst(RIG_AIM) if aim else "")
            return fail(f"unknown word '{words[i]}' ({names}{', car:<id>' if subject else ', track'})")
        if i + found[1] >= len(words):
            return fail(f"'{words[i]}' needs {'three numbers' if found[1] == 3 else 'a value'}")
        a = words[i + 1:i + 1 + found[1]]
        if is_pass:
            passed += [words[i]] + a
        else:
            if key in args:
                return fail(f"'{words[i]}' twice")
            args[key] = a
        i += 1 + found[1]

    def need_car():
        return None if car is not None else f"cam: rig {kind}: needs a car: rig {kind} car:<id> (car:player is the player's)"

    def nums(*specs):
        for word, lo, hi, unit, default in specs:
            v, why = rig_number(args, word, lo, hi, unit, default, kind)
            if why:
                return why
        return None

    M, S = " metres", " seconds"
    if kind == "chase":
        why = need_car() or nums(("dist", 0.5, 200.0, M, 6.0), ("height", -20.0, 200.0, M, 1.8), ("side", -100.0, 100.0, M, 0.0), ("lag", 0.0, 10.0, S, 0.3),
                                 ("aim", -10.0, 100.0, M, 1.0), ("ahead", -200.0, 200.0, M, 0.0))
        if why:
            return why, info
    elif kind == "orbit":
        why = need_car() or nums(("r", 0.1, 5000.0, M, 8.0), ("over", 0.05, 3600.0, S, 0.0), ("lag", 0.0, 10.0, S, 0.0))
        if why:
            return why, info
        ranges = {}
        for word, limit, what in (("az", 3600.0, "az needs degrees (az 90) or a sweep (az 20..200), got '{}'"),
                                  ("el", 89.0, "el needs degrees from -89 to 89 (el 10) or a sweep (el 5..30), got '{}'")):
            g = args.get(word)
            if g is None:
                continue
            parts = g[0].split("..")
            vals = [spot_number(p) for p in parts]
            if len(parts) > 2 or any(v is None or abs(v) > limit for v in vals):
                return fail(what.format(g[0]))
            ranges[word] = len(parts) == 2
        sweep = any(ranges.values())
        if sweep and "over" not in args:
            return fail(f"a sweep ({'az' if ranges.get('az') else 'el'} a..b) needs its length: over <seconds>")
        if not sweep and "over" in args:
            return fail("over is the length of an az or el sweep (az 20..200 over 6)")
        if "at" in args:
            _, why = rig_triple(args["at"], "at", 1000.0, kind)
            if why:
                return why, info
        info["sweep"] = float(args["over"][0]) if sweep else 0.0
    elif kind == "tripod" or kind == "flyby":
        if kind == "tripod":
            if "at" not in args:
                return fail("needs a place: at <x> <y> <z> (or at spot:<name>)")
            _, why = rig_triple(args["at"], "at", 100000.0, kind)
            if why:
                return why, info
            why = nums(("aimlag", 0.0, 10.0, S, 0.0))
            if why:
                return why, info
        else:
            if "path" not in args:
                return fail("needs a route: path <route file> (beside the scene or in its routes folder)")
            if not route_name_ok(args["path"][0]):
                return fail(f"'{args['path'][0]}' is not a route file name (a name or a relative path next to the scene file or in its routes folder: letters, "
                            "digits and _ - . / and blanks; no .. and no drive or leading slash)")
            if "speed" in args:
                why = speed_word_error(args["speed"][0])
                if why:
                    return fail(why)
            why = nums(("up", -50.0, 500.0, M, 2.0), ("aimlag", 0.0, 10.0, S, 0.0), ("y", -10000.0, 10000.0, M, 0.0))
            if why:
                return why, info
            if "y" in args and "up" in args:
                return fail("up (above the route's own height) and y (in the world) are two ways to give the height: use one")
            info.update(route=args["path"][0], has_y="y" in args, has_up="up" in args)
        if track and has_aim_words:
            return fail("track and yaw/pitch are two ways to aim: use one")
        if not track and not has_aim_words:
            return fail("needs an aim: track car:<id> [<dx> <dy> <dz>], track <x> <y> <z>, or yaw <deg> pitch <deg>")
        if track:
            if track_car:
                if track_nums:
                    _, why = rig_triple(track_nums, "track", 1000.0, kind)
                    if why:
                        return why, info
            else:
                _, why = rig_triple(track_nums, "track", 100000.0, kind)
                if why:
                    return why, info
                if args.get("aimlag") and float(args["aimlag"][0]) > 0.0:
                    return fail("aimlag lags a track on a car (a place does not move)")
    else:  # hood, bumper, roof
        why = need_car() or nums(("ahead", 1.0, 1000.0, M, 30.0), ("lag", 0.0, 10.0, S, 0.0))
        if why:
            return why, info
        if "shift" in args:
            _, why = rig_triple(args["shift"], "shift", 5.0, kind)
            if why:
                return why, info
    why = rig_pass_error(passed)
    return (why, info) if why else (None, info)


def route_height_state(text):
    """True when every point of a route file has a height (x,z,speed,y), False when none does; the file is assumed to be fine (route_text_error)."""
    n = with_y = 0
    for raw in text.split("\n"):
        w = raw.split("#", 1)[0].replace(",", " ").split()
        if w:
            n += 1
            with_y += 1 if len(w) == 4 else 0
    return n > 0 and with_y == n


def num(s):
    """A number the way the game reads one (no hex, no inf or nan), or None."""
    s = s.strip()
    if s.startswith("+"):
        s = s[1:]
        if s.startswith("+"):
            return None
    if not NUMBER.match(s):
        return None
    v = float(s)
    return v if math.isfinite(v) else None


def parse_time(s):
    """Seconds ('3', '3.5') or minutes:seconds ('1:30.5'); not negative. None when it is neither."""
    if ":" in s:
        m, _, sec = s.partition(":")
        mv, sv = num(m), num(sec)
        if mv is None or sv is None or mv < 0 or mv != math.floor(mv) or sv < 0 or sv >= 60:
            return None
        v = mv * 60 + sv
    else:
        v = num(s)
    if v is None or v < 0 or v > MAX_SECONDS:
        return None
    return v


def parse_duration(s):
    v = num(s)
    return v if v is not None and 0 < v <= MAX_SECONDS else None


def parse_step(s):
    if "/" in s:
        a, _, b = s.partition("/")
        av, bv = num(a), num(b)
        if av is None or bv is None or bv <= 0:
            return None
        v = av / bv
    else:
        v = num(s)
    return v if v is not None and 0 < v <= 10 else None


def parse_range(s):
    """(a, b, is_range) from 'a..b' or 'a'; None when a number is wrong."""
    if ".." in s:
        a, _, b = s.partition("..")
        av, bv = num(a), num(b)
        return None if av is None or bv is None else (av, bv, True)
    av = num(s)
    return None if av is None else (av, 0.0, False)


def hud_alpha_error(word):
    """None when the word is off, on or an alpha from 0 to 1 (hud_track.cpp ParseAlpha), else the message."""
    w = word.lower()
    if w in ("off", "on"):
        return None
    v = num(w) if re.fullmatch(r"[0-9+\-.eE]+", w) else None
    if v is None:
        return f"hud: '{word}' is not off, on or an alpha from 0 to 1"
    if v < 0 or v > 1:
        return f"hud: alpha {word} is outside 0 to 1"
    return None


def hud_error(words):
    """The message for the words after `hud` when hud_track.cpp's ParseHudKey would refuse them, else None."""
    if not words:
        return HUD_EXPECTED
    if words[0].lower() != "fade":
        return HUD_EXPECTED if len(words) != 1 else hud_alpha_error(words[0])
    over = len(words) == 5 and words[1].lower() == "over"
    if not over and len(words) != 4:
        return HUD_EXPECTED
    to_at = 3 if over else 2
    if words[to_at].lower() != "to":
        return HUD_EXPECTED
    why = hud_alpha_error(words[to_at + 1])
    if why:
        return why
    if over:
        v = num(words[2]) if re.fullmatch(r"[0-9+\-.eE]+", words[2]) else None
        if v is None or v < 0:
            return f"hud fade: '{words[2]}' is not a number of seconds (0 or more)"
        return None
    rng = words[1]
    a_s, dots, b_s = rng.partition("..")
    a = num(a_s) if dots and re.fullmatch(r"[0-9+\-.eE]+", a_s) else None
    b = num(b_s) if dots and re.fullmatch(r"[0-9+\-.eE]+", b_s) else None
    if a is None or b is None:
        return f"hud fade: '{rng}' is not <a>..<b>, two scene times in seconds"
    if a < 0:
        return f"hud fade: the start time {a_s} is before the scene starts"
    if b < a:
        return f"hud fade: {rng} ends before it starts"
    return None


def split_words(line):
    """(words, unterminated): blanks split, "double quotes" keep a word (it may be empty), # at the start of a word is a comment."""
    words, cur, quoted, have = [], [], False, False
    for i, c in enumerate(line):
        if not quoted and c == "#" and (i == 0 or line[i - 1] in " \t"):
            break
        if c == '"':
            quoted = not quoted
            have = True
        elif not quoted and c in " \t\r":
            if have:
                words.append("".join(cur))
            cur, have = [], False
        else:
            cur.append(c)
            have = True
    if have:
        words.append("".join(cur))
    return words, quoted


def fmt(v):
    return f"{v:.2f}"


@dataclass
class Diag:
    line: int
    severity: str  # "error" | "warning"
    message: str

    def text(self, name):
        head = name if self.line <= 0 else f"{name}:{self.line}"
        return f"{head}: {self.severity}: {self.message}"


@dataclass
class Ev:
    t: float
    line: int
    track: str  # hour weather traffic peds parked police timescale set label mark split capture cam cut hud car start
    to: float = 0.0
    frm: object = None  # the start of a range (a float), else None
    over: float = 0.0
    ease: str = "linear"
    release: bool = False
    index: int = -1
    state: int = 0
    name: str = ""
    text: str = ""
    side: str = ""
    fade: float = 0.0
    words: list = field(default_factory=list)

    def end_time(self):
        if self.track == "hud" and self.words[:1] == ["fade"] and len(self.words) >= 2:
            if self.words[1].lower() == "over" and len(self.words) >= 3:
                v = num(self.words[2])
                if v is not None and v >= 0:
                    return self.t + v
            else:
                r = parse_range(self.words[1])
                if r and r[2]:
                    return max(self.t, r[1])
        return self.t + (self.over if self.over > 0 else 0.0)


@dataclass
class Scene:
    name: str = ""
    fps: float = 0.0
    step: float = 0.0
    end: float = -1.0
    end_line: int = 0
    header_line: int = 1
    statelog: bool = False  # the option of the whole scene: `statelog on [dir <folder>]` (M3a)
    statelog_dir: str = ""
    statelog_line: int = 0
    events: list = field(default_factory=list)  # in file order
    diags: list = field(default_factory=list)

    def sorted_events(self):
        return sorted(self.events, key=lambda e: e.t)  # stable


# ------------------------------------------------------------------------------------------------------------------------------- targets
def resolve_target(target=None):
    """`main` or `director`: the argument, else DEFAULT_TARGET (the default); an unknown name is a ValueError."""
    t = (target or DEFAULT_TARGET or "director").lower()
    if t not in TARGETS:
        raise ValueError(f"unknown target '{t}' ({', '.join(TARGETS)})")
    return t


def exe_target(path):
    """The target of an executable: `director` when it holds a message of the Director's parsers (DIRECTOR_EXE_MARKERS), else `main`. OSError when the file cannot be read."""
    import mmap
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size == 0:
            return "legacy"
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            return "director" if any(m.find(mark) >= 0 for mark in DIRECTOR_EXE_MARKERS) else "legacy"


def combine_targets(*targets):
    """The target a scene must be fine for when it runs on all of them: `main` as soon as one is main (the Director's words are refused), else `director`."""
    ts = [t for t in targets if t]
    return "legacy" if (not ts or "legacy" in ts) else "director"


MAIN_RIG_HELP = ("older game builds have no rigs: write the keys of camera.h (pos <x> <y> <z> look <x> <y> <z> for a camera that stands and aims, attach car:<id> <dx> <dy> <dz> heading "
                 "follow <s> look car:<id> for a chase)")


def director_word_errors(verb, a, spots):
    """The messages (a list; empty when the statement is fine for legacy) of a cam, car or start statement `a` (the words after the verb, spot words not yet expanded) that uses a word only
    the Director's parsers read: a `cam <T> rig ...`, and a `spot:<name>[@r,u,f]` word. Where the spot words can be turned into numbers the message gives the statement written with them."""
    out = []
    is_rig = verb == "cam" and bool(a) and a[0].lower() == "rig"
    if is_rig:
        out.append("cam: 'rig' is a Director word and the exe of this target (legacy) does not read it (\"nothing it can do in 'rig'\"): " + MAIN_RIG_HELP)
    spot_words = [w for w in a if is_spot_word(w)]
    if spot_words:
        msg = (f"'{spot_words[0]}': a spot word is a Director word; the exe of this target (legacy) reads numbers there (\"bad position\", \"cam: 'look' needs three numbers\"): "
               "write x y z (a heading in degrees)")
        if spots is not None and not is_rig:
            words, why = expand_spot_words(list(a), spots)
            if not why:
                msg += f"; for these spots: {verb} {' '.join(words)}"
        out.append(msg)
    return out


# ------------------------------------------------------------------------------------------------------------------------------- parser
class Parser:
    def __init__(self, spots=None, target=None):
        self.spots = spots  # {name: Spot} of the spots files that belong to the scene, or None: no table is loaded
        self.target = resolve_target(target)  # `main`: the Director's words are errors
        self.s = Scene()
        self.statements = 0
        self.header_line = 0

    def err(self, line, msg):
        self.s.diags.append(Diag(line, "error", msg))

    def warn(self, line, msg):
        self.s.diags.append(Diag(line, "warning", msg))

    def run(self, text):
        if text.startswith("\ufeff"):
            text = text[1:]
        for i, raw in enumerate(text.split("\n"), 1):
            self.line(i, raw.rstrip("\r"))
        if self.statements == 0:
            self.err(0, "the file has no statements")
        return self.s

    def line(self, n, raw):
        stripped = raw.lstrip()
        if stripped.startswith("@"):
            self.statements += 1
            self.err(n, "a template line (@...): run the expander first; the game's parser takes the expanded file")
            return
        words, unterminated = split_words(raw)
        if unterminated:
            self.err(n, "a double quote is not closed")
            return
        if not words:
            return
        self.statements += 1
        for w in words:
            if re.search(r"\{[A-Za-z_][A-Za-z0-9_]*\}", w):
                self.err(n, f"'{w}' is a {{VAR}} left in the text: run the expander first")
                return
        head = words[0].lower()
        if head == "scene":
            return self.header(n, words)
        t, first, verb = 0.0, 1, head
        if head == "at":
            if len(words) < 3:
                return self.err(n, "at <time> <statement>: the statement is missing")
            t = parse_time(words[1])
            if t is None:
                return self.err(n, f"bad time '{words[1]}' (seconds, or minutes:seconds)")
            verb, first = words[2].lower(), 3
        elif head in ("cam", "cut", "end"):
            if len(words) < 2:
                return self.err(n, f"{head} <time>{' <...>' if head == 'cam' else ''}: the time is missing")
            t = parse_time(words[1])
            if t is None:
                return self.err(n, f"bad time '{words[1]}' (seconds, or minutes:seconds)")
            first = 2
        self.verb(n, t, verb, words[first:])

    def pairs(self, n, verb, args, start, on):
        i = start
        while i < len(args):
            if i + 1 >= len(args):
                self.err(n, f"{verb}: '{args[i]}' needs a value")
                return False
            if not on(args[i].lower(), args[i + 1]):
                return False
            i += 2
        return True

    def header(self, n, w):
        if self.header_line:
            return self.err(n, f"a second 'scene' line (the first is on line {self.header_line})")
        self.header_line = n
        self.s.header_line = n
        if len(w) < 2:
            return self.err(n, "scene <name> [fps <n>] [step <seconds or 1/n>]: the name is missing")
        self.s.name = w[1]

        def on(key, value):
            if key == "fps":
                v = num(value)
                if v is None or v <= 0 or v > 1000:
                    self.err(n, f"bad fps '{value}' (more than 0, at most 1000)")
                    return False
                self.s.fps = v
                return True
            if key == "step":
                v = parse_step(value)
                if v is None:
                    self.err(n, f"bad step '{value}' (seconds, or 1/n)")
                    return False
                self.s.step = v
                return True
            self.err(n, f"scene: unknown option '{key}' (fps, step)")
            return False

        self.pairs(n, "scene", w, 2, on)

    def add(self, ev):
        self.s.events.append(ev)

    def verb(self, n, t, verb, a):
        if verb == "world":
            return self.world(n, t, a)
        if verb == "hour":
            return self.hour(n, t, a)
        if verb == "weather":
            return self.weather(n, t, a)
        if verb in DENSITY_TRACKS:
            return self.density(n, t, a, verb)
        if verb in TOGGLES:
            return self.toggle(n, t, a, verb)
        if verb == "timescale":
            return self.timescale(n, t, a)
        if verb == "set":
            return self.set_(n, t, a)
        if verb == "label":
            return self.label(n, t, a)
        if verb == "mark":
            return self.mark(n, t, a)
        if verb == "preset":
            if len(a) != 1 or a[0] not in ("vanilla", "enhanced", "enhanced_rt", "custom"):
                return self.err(n, "preset needs vanilla, enhanced, enhanced_rt or custom")
            self.s.events.append(Ev(t, n, verb, text=a[0]))
            return
        if verb == "preset_label":
            if len(a) != 1 or a[0] not in ("on", "off"):
                return self.err(n, "preset_label needs on or off")
            self.s.events.append(Ev(t, n, verb, text=a[0]))
            return
        if verb == "split":
            return self.split(n, t, a)
        if verb == "capture":
            return self.capture(n, t, a)
        if verb == "end":
            return self.end(n, t, a)
        if verb in OPAQUE:
            return self.opaque(n, t, verb, a)
        if verb == "statelog":
            return self.statelog(n, t, a)
        self.err(n, f"unknown statement '{verb}' ({VERB_LIST})")

    def world(self, n, t, a):
        if not a:
            return self.err(n, "world needs keys: hour, weather, traffic, peds, parked, police, timescale")
        i = 0
        while i < len(a):
            key = a[i].lower()
            if key == "hold":
                i += 1
                continue
            if key not in WORLD_KEYS:
                return self.err(n, f"world: unknown key '{a[i]}' (hour, weather, traffic, peds, parked, police, timescale)")
            if i + 1 >= len(a):
                return self.err(n, f"world: '{a[i]}' needs a value")
            self.verb(n, t, key, [a[i + 1]])
            i += 2

    def ramp_options(self, n, verb, e, a):
        state = {"over": False}

        def on(key, value):
            if key == "over":
                d = parse_duration(value)
                if d is None:
                    self.err(n, f"bad length '{value}' after over (seconds, more than 0)")
                    return False
                e.over, state["over"] = d, True
                return True
            if key == "ease":
                if value.lower() not in EASES:
                    self.err(n, f"unknown ease '{value}' (linear, in, out, inout)")
                    return False
                e.ease = value.lower()
                return True
            self.err(n, f"{verb}: unknown option '{key}' (over, ease)")
            return False

        ok = self.pairs(n, verb, a, 1, on)
        return ok, state["over"]

    def hour(self, n, t, args):
        a = [w for w in args if w.lower() != "hold"]
        e = Ev(t, n, "hour")
        if not a:
            return self.err(n, "hour needs a value: hour <h>, hour <a>..<b> over <seconds>, or hour free")
        if a[0].lower() == "free":
            if len(a) > 1:
                self.err(n, "hour free takes nothing after it")
            e.release = True
            return self.add(e)
        r = parse_range(a[0])
        if r is None or not (0 <= r[0] <= MAX_HOUR) or (r[2] and not (0 <= r[1] <= MAX_HOUR)):
            return self.err(n, f"bad hour '{a[0]}' (a number from 0 to 24, or a range a..b; up to 48 to run past midnight)")
        if r[2]:
            e.frm, e.to = r[0], r[1]
        else:
            e.to = r[0]
        ok, have_over = self.ramp_options(n, "hour", e, a)
        if not ok:
            return
        if r[2] and not have_over:
            return self.err(n, f"an hour ramp needs a length: hour {a[0]} over <seconds>")
        if not r[2] and have_over:
            return self.err(n, "a single hour does not move: write hour <a>..<b> over <seconds> for a ramp")
        self.add(e)

    def split(self, n, t, a):
        """split <x> | split <a>..<b> over <s> [ease <e>] | split off: the renderer's before/after split (M5b, scene.cpp Split), x from 0 to 1."""
        e = Ev(t, n, "split")
        if not a:
            return self.err(n, "split needs a value: split <x> (0 to 1 of the picture's width), split <a>..<b> over <seconds>, or split off")
        if a[0].lower() == "off":
            if len(a) > 1:
                self.err(n, "split off takes nothing after it")
            e.release = True
            return self.add(e)
        r = parse_range(a[0])
        if r is None or not (0 <= r[0] <= 1) or (r[2] and not (0 <= r[1] <= 1)):
            return self.err(n, f"bad split position '{a[0]}' (a number from 0 to 1 of the picture's width, or a range a..b; split off for none)")
        if r[2]:
            e.frm, e.to = r[0], r[1]
        else:
            e.to = r[0]
        ok, have_over = self.ramp_options(n, "split", e, a)
        if not ok:
            return
        if r[2] and not have_over:
            return self.err(n, f"a split ramp needs a length: split {a[0]} over <seconds>")
        if not r[2] and have_over:
            return self.err(n, "a single split position does not move: write split <a>..<b> over <seconds> for a ramp")
        self.add(e)

    def weather(self, n, t, a):
        if len(a) != 1:
            return self.err(n, "weather <nice|cloudy|stormy|foggy|game>")
        e = Ev(t, n, "weather")
        if a[0].lower() == "game":
            e.release = True
        elif a[0].lower() in WEATHER:
            e.index = WEATHER.index(a[0].lower())
        else:
            return self.err(n, f"unknown weather '{a[0]}' (nice, cloudy, stormy, foggy or game)")
        self.add(e)

    def toggle(self, n, t, a, verb):
        v = a[0].lower() if len(a) == 1 else None
        if v in ("on", "1", "true"):
            state = 1
        elif v in ("off", "0", "false"):
            state = 0
        else:
            return self.err(n, f"{verb} on|off")
        self.add(Ev(t, n, verb, state=state))

    def density(self, n, t, a, verb):
        """peds, parked: on|off (the game's own switch) or a density scale, a number from 0 to 2 (scene.cpp Density)."""
        setting = DENSITY_SETTING[verb]
        if len(a) != 1:
            return self.err(n, f"{verb} on|off, or a density scale (a number from 0 to 2: the setting {setting})")
        word = a[0].lower()
        if word in ("on", "true"):
            return self.add(Ev(t, n, verb, state=1))
        if word in ("off", "false"):
            return self.add(Ev(t, n, verb, state=0))
        v = num(a[0])
        if v is None or v < 0.0 or v > MAX_DENSITY_SCALE:
            return self.err(n, f"bad {verb} '{a[0]}' (on, off, or a density scale from 0 to 2: the setting {setting})")
        self.add(Ev(t, n, verb, state=STATE_SCALE, to=v))

    def timescale(self, n, t, a):
        if not a:
            return self.err(n, "timescale <x> [over <seconds>] [ease <e>], or timescale <a>..<b> over <seconds>")
        e = Ev(t, n, "timescale")
        r = parse_range(a[0])
        if r is None or not (0 <= r[0] <= MAX_TIMESCALE) or (r[2] and not (0 <= r[1] <= MAX_TIMESCALE)):
            return self.err(n, f"bad time scale '{a[0]}' (0 to 10; 1 is the game's own speed)")
        if r[2]:
            e.frm, e.to = r[0], r[1]
        else:
            e.to = r[0]
        ok, have_over = self.ramp_options(n, "timescale", e, a)
        if not ok:
            return
        if r[2] and not have_over:
            return self.err(n, f"a time scale ramp needs a length: timescale {a[0]} over <seconds>")
        smallest = min(r[0], r[1]) if r[2] else r[0]
        if smallest < 0.01:
            self.warn(n, "a time scale under 0.01 is held at the game's own lowest step (about 0.006 at 60 FPS)")
        self.add(e)

    def set_(self, n, t, a):
        if len(a) < 2:
            return self.err(n, "set <cvar> <value...>")
        if not IDENT.match(a[0]):
            return self.err(n, f"'{a[0]}' is not a setting name (letters, digits and _)")
        why = cvar_refusal(a[0])
        if why:
            return self.err(n, why)
        self.add(Ev(t, n, "set", name=a[0], text=" ".join(a[1:])))

    def label(self, n, t, a):
        if not a or a[0] == "":
            return self.err(n, 'label "<text>" [side <s>] [fade <seconds>] [for <seconds>]: the text is missing')
        e = Ev(t, n, "label", text=a[0])

        def on(key, value):
            if key == "side":
                if value.lower() not in SIDES:
                    self.err(n, f"unknown label side '{value}' (left, right, center, top, bottom)")
                    return False
                e.side = value.lower()
                return True
            if key == "fade":
                v = num(value)
                if v is None or v < 0 or v > 60:
                    self.err(n, f"bad fade '{value}' (seconds, 0 to 60)")
                    return False
                e.fade = v
                return True
            if key == "for":
                d = parse_duration(value)
                if d is None:
                    self.err(n, f"bad length '{value}' after for (seconds, more than 0)")
                    return False
                e.over = d
                return True
            self.err(n, f"label: unknown option '{key}' (side, fade, for)")
            return False

        if self.pairs(n, "label", a, 1, on):
            self.add(e)

    def mark(self, n, t, a):
        if len(a) != 1 or not MARK.match(a[0]):
            return self.err(n, "mark <name> (letters, digits and _ - . :)")
        self.add(Ev(t, n, "mark", name=a[0]))

    def capture(self, n, t, a):
        # capture fps <n> [step 1/<n>] starts a take (state 1, the rate in `to`); capture off ends it (state 0)
        if not a:
            return self.err(n, "capture fps <n> [step 1/<n>] [name <take>] [sink <sink>] [statelog on|off], or capture off: nothing after 'capture'")
        e = Ev(t, n, "capture")
        if a[0].lower() == "off":
            if len(a) != 1:
                return self.err(n, "capture off takes nothing after it")
            return self.add(e)
        if a[0].lower() != "fps":
            return self.err(n, f"capture fps <n> [step 1/<n>] [name <take>] [sink <sink>] [statelog on|off], or capture off: '{a[0]}' is neither")
        got = {}

        def on(key, value):
            if key == "fps":
                v = num(value)
                if v is None or not (MIN_CAPTURE_FPS <= v <= MAX_CAPTURE_FPS):
                    self.err(n, f"bad fps '{value}' ({fmt(MIN_CAPTURE_FPS)} to {fmt(MAX_CAPTURE_FPS)}: the engine timer cannot step slower than 10 a second)")
                    return False
                got["fps"] = v
                return True
            if key == "step":
                v = parse_step(value)
                if v is None:
                    self.err(n, f"bad step '{value}' (seconds, or 1/n)")
                    return False
                got["step"] = v
                return True
            if key == "name":
                if not TAKE_NAME.match(value):
                    self.err(n, f"bad take name '{value}' (letters, digits and _ - . , at most 64, not starting with . or -)")
                    return False
                e.name = value
                return True
            if key == "sink":
                if value.lower() not in CAPTURE_SINKS:
                    self.err(n, f"bad sink '{value}' (none, ffmpeg, png or raw)")
                    return False
                e.text = value.lower()
                return True
            if key == "statelog":
                if value.lower() not in ("on", "off"):
                    self.err(n, f"statelog on or statelog off, not '{value}'")
                    return False
                e.index = 1 if value.lower() == "on" else 0
                return True
            self.err(n, f"capture: unknown option '{key}' (fps, step, name, sink, statelog)")
            return False

        if not self.pairs(n, "capture", a, 0, on):
            return
        if "step" in got and abs(got["step"] * got["fps"] - 1.0) > 1e-4:
            return self.err(n, f"the step of a capture is 1/fps (here 1/{fmt(got['fps'])}): one step a scene, and slow motion is a timescale key with a higher capture fps")
        e.state, e.to = 1, got["fps"]
        self.add(e)

    def end(self, n, t, a):
        if a:
            return self.err(n, "end <time> takes nothing after the time")
        if self.s.end_line:
            return self.err(n, f"a second 'end' (the first is on line {self.s.end_line})")
        self.s.end_line, self.s.end = n, t

    def statelog(self, n, t, a):
        """`statelog on [dir <folder>]` / `statelog off`: the per-update state log (state_log.h) is an option of the whole scene: no time."""
        shape = "statelog on [dir <folder>], or statelog off"
        if t > EPS:
            return self.err(n, f"statelog is an option of the whole scene: write it without a time ({shape})")
        if self.s.statelog_line:
            return self.err(n, f"a second 'statelog' (the first is on line {self.s.statelog_line})")
        if not a:
            return self.err(n, f"{shape}: nothing after 'statelog'")
        state = a[0].lower()
        if state not in ("on", "off"):
            return self.err(n, f"{shape}: '{a[0]}' is neither")
        folder = ""
        if state == "off":
            if len(a) != 1:
                return self.err(n, "statelog off takes nothing after it")
        else:
            def on(key, value):
                nonlocal folder
                if key == "dir":
                    if not value:
                        self.err(n, "statelog dir needs a folder")
                        return False
                    folder = value
                    return True
                self.err(n, f"statelog: unknown option '{key}' (dir)")
                return False

            if not self.pairs(n, "statelog", a, 1, on):
                return
        self.s.statelog_line = n
        self.s.statelog = state == "on"
        self.s.statelog_dir = folder

    def opaque(self, n, t, verb, a):
        if verb in ("cam", "car", "start") and self.target == "legacy":
            problems = director_word_errors(verb, a, self.spots)
            if problems:
                for why in problems:
                    self.err(n, why)
                return
        if verb in ("cam", "car", "start"):
            # a spot word is a place or a heading by name: numbers from here on, as in the game's parser (scene.cpp Opaque)
            a, why = expand_spot_words(list(a), self.spots)
            if why:
                return self.err(n, why)
        e = Ev(t, n, verb, words=list(a))
        if verb == "cam" and not a:
            return self.err(n, "cam <time> <what the camera does>: nothing after the time")
        if verb == "cut" and a:
            return self.err(n, "cut <time> takes nothing after the time")
        if verb == "hud" and not a:
            return self.err(n, HUD_EXPECTED)
        if verb == "car":
            if len(a) < 2 or not IDENT.match(a[0]):
                return self.err(n, "car <id> <what the car does> (the id is letters, digits and _)")
            why = car_words_error(a)
            if why:
                return self.err(n, why)
        if verb == "cam" and a and a[0].lower() == "rig":
            why, info = rig_error(a)
            if why:
                return self.err(n, why)
            e.over = info["sweep"]  # an az or el sweep makes a second key that long after this one
        if verb == "start" and not a:
            return self.err(n, "start <what the take starts from>: nothing after it")
        self.add(e)


# ------------------------------------------------------------------------------------------------------------------------------- the game's settings
@dataclass
class Cvar:
    name: str
    kind: str
    lifecycle: str = "kHotReload"
    debug_only: bool = False
    allowed: list = field(default_factory=list)
    lo: object = None
    hi: object = None
    where: str = ""


DEF = re.compile(r"REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING)\s*\(\s*(\w+)\s*,")


def _statement_end(text, start):
    """Index of the ';' that ends the statement starting at `start` (outside strings and parentheses), or -1."""
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
        elif c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i)
            i = n if j < 0 else j + 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == ";" and depth <= 0:
            return i
        i += 1
    return -1


def scan_cvars(src):
    """{name: Cvar} of the REXCVAR_DEFINE_* in the .cpp and .h files under src (thirdparty left out)."""
    src = Path(src)
    manifest = src / "settings" / "generated" / "manifest.json"
    if manifest.is_file():
        types = {"bool": "BOOL", "i32": "INT32", "u32": "UINT32", "i64": "INT64",
                 "u64": "UINT64", "double": "DOUBLE", "f64": "DOUBLE", "std::string": "STRING"}
        found = {}
        for name, spec in json.loads(manifest.read_text(encoding="utf-8")).items():
            kind = types.get(spec.get("type"))
            if not kind:
                continue
            cv = Cvar(name, kind, where=manifest.name)
            for trait in spec["traits"]:
                if trait in ("rex::kRequiresRestart", "rex::kInitOnly"):
                    cv.lifecycle = trait.split("::")[1]
                elif trait == "rex::kDebugOnly":
                    cv.debug_only = True
                elif trait.startswith("rex::Range{"):
                    lo, hi = trait[11:-1].split(",", 1)
                    cv.lo, cv.hi = num(lo.strip()), num(hi.strip())
                elif trait.startswith("rex::Allowed"):
                    cv.allowed = re.findall(r'"([^"]*)"', trait)
            found[name] = cv
        return found

    found = {}
    for path in src.rglob("*"):
        if path.suffix not in (".cpp", ".h") or "thirdparty" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in DEF.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if "//" in text[line_start:m.start()]:
                continue  # inside a comment
            end = _statement_end(text, m.start())
            chain = text[m.start(): end if end > 0 else m.end() + 600]
            cv = Cvar(m.group(2), m.group(1), where=path.name)
            lc = re.search(r"Lifecycle::(kHotReload|kRequiresRestart|kInitOnly)", chain)
            if lc:
                cv.lifecycle = lc.group(1)
            cv.debug_only = ".debug_only()" in chain
            al = re.search(r"\.allowed\(\s*\{(.*?)\}\s*\)", chain, re.S)
            if al:
                cv.allowed = re.findall(r'"([^"]*)"', al.group(1))
            rg = re.search(r"\.range\(\s*([^,()]+?)\s*,\s*([^,()]+?)\s*\)", chain)
            if rg:
                cv.lo, cv.hi = num(rg.group(1)), num(rg.group(2))
            found.setdefault(cv.name, cv)
    return found


# ------------------------------------------------------------------------------------------------------------------------------- the checks
def check(scene, cvars=None, cars=None):
    """Adds the lint's own diagnostics to scene.diags."""
    d = scene.diags

    def err(line, msg):
        d.append(Diag(line, "error", msg))

    def warn(line, msg):
        d.append(Diag(line, "warning", msg))

    events = scene.events

    # key order: per track (the camera's cam and cut together, each car's keys together); across tracks only a warning
    groups = {}
    for e in events:
        key = "cam" if e.track in ("cam", "cut") else (f"car {e.words[0]}" if e.track == "car" and e.words else e.track)
        groups.setdefault(key, []).append(e)
    flagged = set()
    for key, evs in groups.items():
        top = None
        for e in evs:
            if top is not None and e.t + EPS < top.t:
                err(e.line, f"the {key} key at t={fmt(e.t)} comes after line {top.line} (t={fmt(top.t)}): keys of one track must be in time order")
                flagged.add(e.line)
            if top is None or e.t > top.t:
                top = e
    top = None
    for e in events:
        if top is not None and e.t + EPS < top.t and e.line not in flagged:
            warn(e.line, f"t={fmt(e.t)} comes after line {top.line} (t={fmt(top.t)}): keep the file in time order")
        if top is None or e.t > top.t:
            top = e

    # a camera key inside an orbit sweep (`rig orbit az a..b over s` makes a second key s seconds on): the keys of the camera run in time order
    cam_events = [e for e in scene.sorted_events() if e.track in ("cam", "cut")]
    for e in cam_events:
        if e.track != "cam" or e.over <= 0 or len(e.words) < 2 or e.words[0].lower() != "rig" or e.words[1].lower() != "orbit":
            continue
        for f in cam_events:
            if f is not e and e.t + EPS < f.t < e.t + e.over - EPS:
                warn(f.line, f"the camera key at t={fmt(f.t)} comes inside the orbit sweep of line {e.line} (t={fmt(e.t)} to {fmt(e.t + e.over)}): "
                             f"keys run in time order, so the sweep's end key is taken after it")

    # the take: one `capture fps`, an optional `capture off` after it
    cap_start = cap_stop = None
    for e in scene.sorted_events():
        if e.track != "capture":
            continue
        if e.state == 1:
            if cap_start:
                err(e.line, f"a second 'capture fps' (the first is on line {cap_start.line}): a scene has one capture rate, and slow motion is a timescale key with a higher fps")
            else:
                cap_start = e
        elif not cap_start:
            err(e.line, "'capture off' with no 'capture fps' before it")
        elif cap_stop:
            warn(e.line, f"a second 'capture off' (the first is on line {cap_stop.line}): the first ends the take")
        elif e.t <= cap_start.t + EPS:
            err(e.line, f"'capture off' at t={fmt(e.t)} ends a take that starts at t={fmt(cap_start.t)} (line {cap_start.line}): nothing would be captured")
        else:
            cap_stop = e
    if cap_start:
        if scene.fps > 0 and abs(scene.fps - cap_start.to) > 1e-6:
            warn(cap_start.line, f"the header says fps {fmt(scene.fps)} and the capture says {fmt(cap_start.to)}: the capture's is used")
        if not cap_stop and scene.end < 0:
            warn(cap_start.line, "no 'end' and no 'capture off': the take runs until the scene is turned off")

    # two keys of one world track at one time
    last = {}
    for e in scene.sorted_events():
        if e.track in WORLD_KEYS or e.track in ("split", "preset", "preset_label"):
            p = last.get(e.track)
            if p is not None and abs(p.t - e.t) < EPS:
                warn(e.line, f"two {e.track} keys at t={fmt(e.t)} (line {p.line} and this one): the later wins")
            last[e.track] = e

    # durations
    if scene.end < 0:
        warn(0, "no 'end': the scene never finishes, and its last values hold until it is turned off")
    else:
        for e in events:
            if e.t > scene.end + EPS:
                err(e.line, f"the {e.track} key at t={fmt(e.t)} is after the scene's end (t={fmt(scene.end)}, line {scene.end_line})")
            elif e.end_time() > scene.end + EPS:
                err(e.line, f"the {e.track} key runs to t={fmt(e.end_time())}, past the scene's end (t={fmt(scene.end)}): it would be cut off there")
    by_track = {}
    for e in scene.sorted_events():
        if e.track in ("hour", "timescale", "split") and e.over > 0:
            by_track.setdefault(e.track, []).append(e)
    for track, ramps in by_track.items():
        allk = [x for x in scene.sorted_events() if x.track == track]
        for r in ramps:
            nxt = next((x for x in allk if x.t > r.t + EPS), None)
            if nxt is not None and nxt.t + EPS < r.t + r.over:
                warn(r.line, f"the {track} ramp at t={fmt(r.t)} over {fmt(r.over)} s is cut off by the key at t={fmt(nxt.t)} (line {nxt.line})")
    if scene.fps > 0 and scene.step > 0 and abs(scene.step - 1.0 / scene.fps) > 1e-6:
        warn(scene.header_line, f"step {scene.step:.6f} s does not match fps {scene.fps:g} (1/fps is {1.0 / scene.fps:.6f} s)")
    for e in events:
        if e.track == "label" and e.over > 0 and e.fade > 0 and e.fade * 2 > e.over + EPS:
            warn(e.line, f"the label's fade ({fmt(e.fade)} s) is more than half of its time on screen ({fmt(e.over)} s)")
        if e.track == "start" and e.t > EPS:
            warn(e.line, "start is the preamble: it has no time (it is taken to be at 0)")
    starts = [e for e in events if e.track == "start"]
    for e in starts[1:]:
        err(e.line, f"a second 'start' (the first is on line {starts[0].line})")

    # the HUD's keys, the grammar of src/mc_engine/cinematic/hud_track.cpp (ParseHudKey): off, on, <alpha>, fade <a>..<b> to <x>, fade over <s> to <x>
    for e in events:
        if e.track != "hud":
            continue
        why = hud_error(e.words)
        if why:
            err(e.line, why)

    # settings
    if cvars is not None:
        for e in events:
            if e.track != "set":
                continue
            cv = cvars.get(e.name)
            if cv is None:
                close = difflib.get_close_matches(e.name, list(cvars), n=3, cutoff=0.7)
                err(e.line, f"no setting called '{e.name}'" + (f" (did you mean {', '.join(close)}?)" if close else ""))
                continue
            if cv.lifecycle != "kHotReload":
                err(e.line, f"'{e.name}' is read only at start; a scene cannot change it")
                continue
            msg = check_value(cv, e.text)
            if msg:
                err(e.line, f"'{e.name}': {msg}")

    # cars: spawned before they are used
    spawn = {}  # id -> (t, line)
    gone = {}  # id -> (t, line)
    ambient_ids = set()  # the ids made with `car <id> ambient`
    for e in scene.sorted_events():
        if e.track != "car":
            continue
        cid, verb = e.words[0], e.words[1].lower()
        if verb == "ambient":
            ambient_ids.add(cid)
        if verb in ("spawn", "ambient"):
            if cid in spawn:
                err(e.line, f"car {cid} is spawned twice (line {spawn[cid][1]} and this one)")
            else:
                spawn[cid] = (e.t, e.line)
            gone.pop(cid, None)
            if verb == "spawn" and cars is not None and len(e.words) >= 3:
                kind = e.words[2].lower()
                if kind != "profile" and not re.match(r"^(garage|slot):\d+$", kind) and kind not in cars:
                    close = difflib.get_close_matches(kind, sorted(cars), n=3, cutoff=0.6)
                    err(e.line, f"car {cid}: unknown car '{kind}'" + (f" (did you mean {', '.join(close)}?)" if close else ""))
        elif verb in ("delete", "despawn"):
            if cid not in spawn:
                err(e.line, f"car {cid} is removed at t={fmt(e.t)} but is never spawned before")
            else:
                gone[cid] = (e.t, e.line)
    for e in scene.sorted_events():
        refs = []
        if e.track == "car":
            if e.words[1].lower() not in ("spawn", "ambient"):
                refs.append(e.words[0])
            else:
                # the car a `near` names must be there (player, or spawned earlier; at the same time it must be on an earlier line: the Director takes the lines in file order)
                for k in range(2, len(e.words) - 1):
                    if e.words[k].lower() == "near":
                        anchor = "player" if e.words[k + 1].lower() == "player" else e.words[k + 1]
                        refs.append(anchor)
                        if anchor in spawn and abs(e.t - spawn[anchor][0]) <= EPS and spawn[anchor][1] > e.line:
                            err(e.line, f"car {e.words[0]} is placed near car {anchor}, which is spawned on a later line (line {spawn[anchor][1]}): at the same time the file's order decides")
            refs += CAR_REF.findall(" ".join(e.words[2:]))
        elif e.track in ("cam", "hud", "start", "cut"):
            refs += CAR_REF.findall(" ".join(e.words))
        for cid in refs:
            if cid == "player":
                continue
            if e.track == "car" and e.words[0] == cid and e.words[1].lower() in ("delete", "despawn"):
                continue
            if cid not in spawn:
                err(e.line, f"car '{cid}' is used at t={fmt(e.t)} but is never spawned (car {cid} spawn ...)")
            elif e.t + EPS < spawn[cid][0]:
                err(e.line, f"car {cid} is used at t={fmt(e.t)} before it is spawned at t={fmt(spawn[cid][0])} (line {spawn[cid][1]})")
            elif cid in gone and e.t > gone[cid][0] + EPS:
                err(e.line, f"car {cid} is used at t={fmt(e.t)} after it was removed at t={fmt(gone[cid][0])} (line {gone[cid][1]})")

    # an ambient car stands where it is put: no route (the Director refuses a path or a stop on it)
    for e in scene.sorted_events():
        if e.track == "car" and len(e.words) >= 2 and e.words[0] in ambient_ids and e.words[1].lower() in ("path", "stop"):
            err(e.line, f"car {e.words[0]} is an ambient car: it stands where it was placed and takes no route (spawn a car for a path)")

    # the parked spawner makes an ambient car from its layout item: closed when the car is asked for (parked off, a parked scale of 0), no car appears
    for e in scene.sorted_events():
        if e.track != "car" or len(e.words) < 2 or e.words[1].lower() != "ambient":
            continue
        closing = None
        for k in scene.sorted_events():
            if k.track != "parked" or k.t > e.t + EPS:
                continue
            closing = k if (k.state == 0 or (k.state == STATE_SCALE and k.to <= 0.0)) else None
        if closing is not None:
            warn(e.line, f"car {e.words[0]} is an ambient car, which the game's parked spawner makes from a layout item, and the parked spawner is closed at "
                         f"t={fmt(e.t)} (line {closing.line}): no car will appear (use a parked scale above 0, or leave parked alone)")


def check_value(cv, text):
    """A reason the value does not fit the setting, or ''."""
    if cv.kind == "BOOL":
        if text.lower() not in ("true", "false", "1", "0"):
            return f"'{text}' is not a bool (true, false)"
    elif cv.kind in ("INT32", "INT64", "UINT32", "UINT64"):
        if not re.match(r"^[+-]?\d+$", text):
            return f"'{text}' is not a whole number"
        v = int(text)
        if cv.kind.startswith("UINT") and v < 0:
            return f"'{text}' is negative"
        if cv.lo is not None and cv.hi is not None and not cv.lo <= v <= cv.hi:
            return f"{v} is out of range ({cv.lo:g} to {cv.hi:g})"
    elif cv.kind == "DOUBLE":
        v = num(text)
        if v is None:
            return f"'{text}' is not a number"
        if cv.lo is not None and cv.hi is not None and not cv.lo <= v <= cv.hi:
            return f"{v:g} is out of range ({cv.lo:g} to {cv.hi:g})"
    elif cv.kind == "STRING":
        if cv.allowed and text not in cv.allowed:
            return f"'{text}' is not one of {', '.join(cv.allowed)}"
    return ""


def lint(text, cvars=None, cars=None, spots=None, target=None):
    """target: `legacy` or `director` (resolve_target): the exe the scene runs on; None = DEFAULT_TARGET."""
    sc = Parser(spots, target).run(text)
    check(sc, cvars, cars)
    return sc


def load_cars(path):
    return {ln.strip() for ln in Path(path).read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.strip().startswith("#")}


# ------------------------------------------------------------------------------------------------------------------------------- output
def describe(e):
    if e.track == "hour":
        return "hour free" if e.release else (f"hour {fmt(e.frm)}..{fmt(e.to)} over {fmt(e.over)} s {e.ease}" if e.frm is not None else f"hour {fmt(e.to)}")
    if e.track == "weather":
        return "weather game" if e.release else f"weather {WEATHER[e.index]}"
    if e.track in TOGGLES:
        return f"{e.track} {'on' if e.state else 'off'}"
    if e.track == "timescale":
        s = f"timescale {fmt(e.frm)}..{fmt(e.to)}" if e.frm is not None else f"timescale {fmt(e.to)}"
        return s + (f" over {fmt(e.over)} s {e.ease}" if e.over > 0 else "")
    if e.track == "set":
        return f"set {e.name} = {e.text}"
    if e.track == "label":
        return f'label "{e.text}"' + (f" side {e.side}" if e.side else "") + (f" for {fmt(e.over)} s" if e.over > 0 else "")
    if e.track == "mark":
        return f"mark {e.name}"
    if e.track in ("preset", "preset_label"):
        return f"{e.track} {e.text}"
    if e.track == "split":
        return "split off" if e.release else (f"split {fmt(e.frm)}..{fmt(e.to)} over {fmt(e.over)} s {e.ease}" if e.frm is not None else f"split {fmt(e.to)}")
    if e.track == "capture":
        if not e.state:
            return "capture off"
        extra = (f" name {e.name}" if e.name else "") + (f" sink {e.text}" if e.text else "") + (f" statelog {'on' if e.index else 'off'}" if e.index >= 0 else "")
        return f"capture fps {fmt(e.to)}{extra}"
    return e.track + " " + " ".join(e.words)


def main(argv=None):
    ap = argparse.ArgumentParser(description="lint LARecomp scene files", add_help=True)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--src", default=None)
    ap.add_argument("--cars", default=None)
    ap.add_argument("--spots", default=None, help="one spots file instead of the ones beside the scene and in the shared scenes folder")
    ap.add_argument("--target", default=None, choices=TARGETS, help="the exe the scene runs on: legacy refuses the Director's spot words and rigs (default: director)")
    ap.add_argument("--exe", action="append", default=[], help="an exe file: its target is read from it (repeatable; one without the Director's words makes the target legacy)")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--duration", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    if args.files == ["selftest"]:
        return selftest()
    if not args.files:
        ap.print_usage(sys.stderr)
        return 2
    try:
        exe_targets = [exe_target(x) for x in args.exe]
    except OSError as ex:
        print(f"scene_check: --exe: cannot read the exe: {ex}", file=sys.stderr)
        return 2
    target = combine_targets(args.target, *exe_targets) if (args.target or exe_targets) else resolve_target()
    src = args.src or DEFAULT_SRC
    cvars = None
    if Path(src).is_dir():
        cvars = scan_cvars(src)
    elif args.src or not args.duration:
        print(f"scene_check: no source folder at {src}: the names of settings are not checked (--src DIR)", file=sys.stderr)
    cars = load_cars(args.cars) if args.cars else None
    bad = False
    out_json = []
    for f in args.files:
        p = Path(f)
        try:
            text = p.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as ex:
            print(f"{f}: error: cannot read: {ex}")
            bad = True
            continue
        if args.spots:
            spots, spot_problems = parse_spots_text(Path(args.spots).read_text(encoding="utf-8"), args.spots)
            spot_problems = [f"{args.spots}:{n}: {sev}: {msg}" for n, sev, msg in spot_problems if sev == "error"]
        else:
            spots, spot_problems = load_spots_for(p)
        for problem in spot_problems:
            print(problem, file=sys.stderr)
        sc = lint(text, cvars, cars, spots, target)
        check_routes(sc, p)
        errors = sum(1 for x in sc.diags if x.severity == "error")
        warns = len(sc.diags) - errors
        if args.duration:
            print(f"{sc.end:g}" if sc.end >= 0 and errors == 0 else "none")
            bad = bad or errors > 0
            continue
        order = sorted(sc.diags, key=lambda x: (x.line, x.severity != "error"))
        if args.json:
            out_json.append({"file": str(p), "name": sc.name, "end": sc.end if sc.end >= 0 else None, "errors": errors, "warnings": warns,
                             "diagnostics": [{"line": x.line, "severity": x.severity, "message": x.message} for x in order]})
        else:
            for x in order:
                print(x.text(p.name))
            if args.list:
                for e in sc.sorted_events():
                    print(f"  t={e.t:8.2f}  line {e.line:<4} {describe(e)}")
            print(f"{p.name}: {errors} error(s), {warns} warning(s); {len(sc.events)} keys; " + (f"ends at {fmt(sc.end)} s" if sc.end >= 0 else "no end"))
        bad = bad or errors > 0 or (args.strict and warns > 0)
    if args.json:
        print(json.dumps(out_json, indent=2))
    return 1 if bad else 0


# ------------------------------------------------------------------------------------------------------------------------------- selftest
FAKE_SRC = {
    "mcla_gpu_ao": Cvar("mcla_gpu_ao", "STRING", allowed=["auto", "off", "high", "ultra"]),   # the setting has four values
    "mcla_gpu_scale": Cvar("mcla_gpu_scale", "DOUBLE", lo=0.5, hi=3.0),
    "mcla_gpu_msaa": Cvar("mcla_gpu_msaa", "INT32", lo=0, hi=8),
    "mcla_flag": Cvar("mcla_flag", "BOOL"),
    "mcla_gpu_split_effects": Cvar("mcla_gpu_split_effects", "STRING", allowed=["all", "ao", "dither"]),
    "mcla_restart_only": Cvar("mcla_restart_only", "BOOL", lifecycle="kRequiresRestart"),
}


def selftest():
    global DEFAULT_TARGET
    saved_target, DEFAULT_TARGET = DEFAULT_TARGET, "director"  # the recipes and the Director's examples use spot words and rigs: not the session's the default
    failures = []
    count = [0]

    def check_(ok, what):
        count[0] += 1
        if not ok:
            failures.append(what)

    def has(sc, sev, line, part):
        return any(x.severity == sev and x.line == line and part in x.message for x in sc.diags)

    # the grammar: one bad line each (line 2 of a scene with a header and an end)
    bad_lines = [
        ("at 1 weather sunny", "unknown weather 'sunny'"), ("at 1 weather", "weather <nice|cloudy|stormy|foggy|game>"),
        ("at -1 mark a", "bad time '-1'"), ("at 1:75 mark a", "bad time '1:75'"), ("at 3", "the statement is missing"),
        ("cam", "the time is missing"), ("at 1 hour 49", "bad hour '49'"), ("at 1 hour 17..19", "an hour ramp needs a length"),
        ("at 1 hour 17 over 5", "a single hour does not move"), ("at 1 hour 17..19 over 4 ease wobbly", "unknown ease 'wobbly'"),
        ("at 1 hour 17..19 over", "'over' needs a value"), ("at 1 traffic maybe", "traffic on|off"),
        ("at 1 timescale 11", "bad time scale '11'"), ("at 1 timescale 1..0.2", "a time scale ramp needs a length"),
        ("at 1 set mcla_ao", "set <cvar> <value...>"), ("at 1 set not-a-name 1", "is not a setting name"),
        ("at 1 label", "the text is missing"), ("at 1 label \"x\" side up", "unknown label side 'up'"),
        ("at 1 label \"x\" for 0", "bad length '0'"), ("at 1 label \"x", "a double quote is not closed"),
        ("at 1 mark a/b", "mark <name>"), ("cam 1", "nothing after the time"), ("cut 1 now", "takes nothing after the time"),
        ("at 1 hud", "hud: expected off, on, an alpha"), ("at 1 hud blink", "'blink' is not off, on or an alpha"), ("at 1 hud fade 1..2 to 3", "alpha 3 is outside 0 to 1"),
        ("at 1 hud fade 2..1 to 0", "ends before it starts"), ("at 1 hud fade 1 to 0", "is not <a>..<b>"),
        ("at 1 hud 1.5", "alpha 1.5 is outside 0 to 1"), ("at 1 hud off now", "hud: expected off, on"), ("at 1 hud fade over x to 0", "is not a number of seconds"),
        ("at 1 hud fade over 2 off", "hud: expected off, on"), ("at 1 hud fade -1..2 to 0", "before the scene starts"), ("at 1 car", "car <id>"),
        ("at 1 car player spawn x", "'player' is the player's own car"), ("start", "nothing after it"),
        ("at 1 dance", "unknown statement 'dance'"), ("world", "world needs keys"),
        ("at 1 split", "split needs a value"), ("at 1 split 1.5", "bad split position '1.5'"), ("at 1 split left", "bad split position 'left'"),
        ("at 1 split 0.2..0.8", "a split ramp needs a length"), ("at 1 split 0.5 over 2", "a single split position does not move"),
        ("at 1 split 0.2..0.8 over 2 ease wobbly", "unknown ease 'wobbly'"), ("at 1 split 0.2..0.8 over 2 speed 3", "split: unknown option 'speed'"),
        ("at 1 split off now", "split off takes nothing after it"), ("world fog thick", "world: unknown key 'fog'"),
        ("@include other", "a template line"), ("at 1 set x {VALUE}", "left in the text"),
        ("at 1 set mcla_scene x", "a scene does not set mcla_scene"),
        ("at 1 capture", "nothing after 'capture'"), ("at 1 capture fps", "'fps' needs a value"), ("at 1 capture fps 5", "bad fps '5'"),
        ("at 1 capture fps 60 step 1/50", "the step of a capture is 1/fps"), ("at 1 capture fps 60 speed 2", "unknown option 'speed'"),
        ("at 1 capture fps 60 sink mp4", "bad sink 'mp4' (none, ffmpeg, png or raw)"), ("at 1 capture fps 60 name a/b", "bad take name 'a/b'"),
        ("at 1 capture fps 60 name .x", "bad take name '.x'"), ("at 1 capture fps 60 statelog maybe", "statelog on or statelog off, not 'maybe'"),
        ("at 1 capture fps 60 sink", "'sink' needs a value"), ("at 1 capture off name x", "capture off takes nothing after it"),
        ("at 1 capture now", "is neither"), ("at 1 capture off", "'capture off' with no 'capture fps' before it"),
    ]
    for line, part in bad_lines:
        sc = lint("scene t\n" + line + "\nend 10\n", FAKE_SRC)
        check_(has(sc, "error", 2, part), f"'{line}' -> error on line 2 saying '{part}'; got {[x.text('t') for x in sc.diags]}")
    # what a scene may set: the same names as scene_test.cpp TestCvarRefusal (scene.cpp CvarRefusal)
    refused = [
        "mcla_capture_ffmpeg", "mcla_capture_ffmpeg_args", "mcla_capture_sink", "mcla_capture_out", "mcla_capture_fps", "mcla_capture_ring",
        "mcla_capture_lag", "mcla_capture_delay_ms", "mcla_capture_future_setting", "mcla_scene", "mcla_scene_restart", "mcla_scene_t",
        "log_file", "dump_path", "dump_shaders", "dump_source", "cache_root", "game_data_root", "user_data_root", "update_data_root",
        "metadata_root", "hid_mappings_file", "keycaps_dir", "larecomp_texture_folder", "string_folder", "shader_pack_dir", "perf_log_csv",
        "cl", "user_name", "import_vinyl", "rmr_server", "online_master_server", "server_browser_servers", "server_browser_pending",
        "some_future_path", "some_future_exe", "some_future_args", "some_future_command",
        "cam_trace", "cam_probe_dump", "export_vinyl", "capture_vinyl_shapes", "dump_vinyl_shapes", "fxc_probe", "lzx_stats",
        "rubberband_dump", "menu_cam_record", "wheel_learn", "allow_game_relative_writes", "mcla_gpu_texture_dump", "mcla_gpu_lock_dump",
        "mcla_gpu_trace_frame", "mcla_gpu_trace_count", "mcla_native_gfx_capture", "mcla_native_gfx_census", "mcla_native_gfx_telemetry",
        "mcla_native_gfx_geomdump", "mcla_native_gfx_firstdraw", "mcla_native_gfx_diag", "mcla_native_gfx_exposure_probe",
        "string_dump_enabled", "model_mods_diag", "mcla_gfx_probe", "mcla_native_gfx_ab_cvar",
        "mcla_capture_dir", "mcla_capture_name", "mcla_capture_statelog", "mcla_capture_audio", "mcla_capture_memory_mb",
        "mcla_state_log", "mcla_freecam_record", "mcla_freecam_out", "MCLA_STATE_LOG",
        "MCLA_CAPTURE_FFMPEG", "Mcla_Capture_Sink", "LOG_FILE", "Mcla_Scene",
    ]
    allowed = [
        "mcla_gpu_ao", "mcla_gpu_ao_view", "mcla_gpu_scale", "mcla_gpu_split", "mcla_gpu_split_effects", "mcla_gpu_msaa", "mcla_gpu_aa",
        "mcla_gpu_dlss_mode", "mcla_gpu_upscaler", "mcla_gpu_hdr", "mcla_gpu_resolution_scale", "mcla_cam_log", "mcla_cin_hud_alpha",
        "mcla_fixed_step_fps", "aspect_ratio", "display_gamma", "speed_units", "weather", "time_of_day", "ui_interpolate", "x", "some_text",
        "mcla_capture", "capture_fps", "mcla_gpu_sync_builds", "mcla_freecam_speed", "mcla_freecam_log",
    ]
    for name in refused:
        why = cvar_refusal(name)
        check_(why.startswith(f"a scene does not set {name}"), f"a scene may not set {name}; got '{why}'")
        sc = lint(f"scene t\nat 1 set {name} 1\nend 10\n", FAKE_SRC)
        check_(has(sc, "error", 2, f"a scene does not set {name}"), f"'set {name}' is an error on its line; got {[x.text('t') for x in sc.diags]}")
    for name in allowed:
        check_(cvar_refusal(name) == "", f"a scene may set {name}; got '{cvar_refusal(name)}'")
    check_(cvar_refusal("mcla_scene") == "a scene does not set mcla_scene", "the sentence for mcla_scene")
    check_("makes the game write a file of its own" in cvar_refusal("log_file"), "the sentence says why")
    sc = lint("scene a\nat 0 set mcla_gpu_ao off\nat 1 set mcla_capture_ffmpeg x\nat 2 set mcla_gpu_ao high\nend 5\n", FAKE_SRC)
    check_(len([x for x in sc.diags if x.severity == "error"]) == 1 and has(sc, "error", 3, "a scene does not set mcla_capture_ffmpeg"),
           f"one refused line is one error, on its line; got {[x.text('a') for x in sc.diags]}")
    # the lists of the game's parser are the lists above (read from scene.cpp when a checkout has it)
    cpp = scene_cpp_refusal_lists()
    if cpp is not None:
        check_(cpp[0] == sorted(REFUSED_CVARS), f"scene.cpp kRefusedCvars matches REFUSED_CVARS; scene.cpp has {cpp[0]}")
        check_(cpp[1] == sorted(REFUSED_SUFFIXES), f"scene.cpp kRefusedSuffixes matches REFUSED_SUFFIXES; scene.cpp has {cpp[1]}")
    check_(has(lint("scene a fps zero\nend 3\n"), "error", 1, "bad fps 'zero'"), "bad fps")
    check_(has(lint("scene a\nscene b\nend 3\n"), "error", 2, "a second 'scene' line"), "second scene line")
    check_(has(lint(""), "error", 0, "no statements"), "empty file")

    # a clean scene has nothing to say, in every spelling of its numbers
    clean = lint("# c\nscene c fps 60 step 1/60\nat 0 capture fps 60 step 1/60\nworld hour 17.4 hold weather nice traffic off\nat 2 timescale 1..0.2 over 1 ease inout\nat 1:00.5 mark late\nend 2:00\n", FAKE_SRC)
    check_(not clean.diags, f"a clean scene is silent; got {[x.text('c') for x in clean.diags]}")
    check_(abs(clean.end - 120) < 1e-9 and abs(clean.sorted_events()[-1].t - 60.5) < 1e-9, "1:00.5 is 60.5 s and 2:00 is 120 s")

    pr = lint("scene s\npreset vanilla\nat 3 preset enhanced\nat 6 preset enhanced_rt\nat 6 preset_label on\nend 9\n", FAKE_SRC)
    check_(not pr.diags and len(pr.events) == 4, "preset names and a label key parse")
    check_(has(lint("preset unknown\n", FAKE_SRC), "error", 1, "preset needs"), "reject an unknown preset")
    check_(has(lint("preset_label maybe\n", FAKE_SRC), "error", 1, "preset_label needs"), "reject a non-boolean preset label")

    # the split track (M5b): a clean sweep, a ramp cut off by the next key, a key past the end, the effects setting
    sp = lint("scene s\nat 0 set mcla_gpu_split_effects ao\nat 1 split 0.15\nat 2 split 0.15..0.85 over 6 ease inout\nat 10 split 0.5\nat 12 split off\nend 14\n", FAKE_SRC)
    check_(not sp.diags, f"a clean split sweep is silent; got {[x.text('s') for x in sp.diags]}")
    sp = lint("scene s\nat 1 split 0.2..0.8 over 6\nat 3 split 0.5\nend 10\n", FAKE_SRC)
    check_(has(sp, "warning", 2, "ramp at t=1.00 over 6.00 s is cut off by the key at t=3.00"), "a split ramp cut off by the next key")
    sp = lint("scene s\nat 8 split 0.1..0.9 over 4\nend 10\n", FAKE_SRC)
    check_(has(sp, "error", 2, "past the scene's end"), "a split ramp past the end")
    sp = lint("scene s\nat 2 split 0.2\nat 2 split 0.8\nend 10\n", FAKE_SRC)
    check_(has(sp, "warning", 3, "two split keys at t=2.00 (line 2 and this one)"), "two split keys at one time")
    sp = lint("scene s\nat 2 split 0.5\nat 1 split 0.2\nend 10\n", FAKE_SRC)
    check_(any("split" in x.message and "order" in x.message for x in sp.diags), f"split keys out of order are reported; got {[x.text('s') for x in sp.diags]}")
    sp = lint("scene s\nat 0 set mcla_gpu_split_effects shadows\nend 10\n", FAKE_SRC)
    check_(has(sp, "error", 2, "'shadows' is not one of all, ao, dither"), "an effect the split does not cut")

    # the take: one rate, an off after the start, the header's fps against it
    sc = lint("scene a\nat 0 capture fps 60\nat 3 capture fps 30\nend 5\n")
    check_(has(sc, "error", 3, "a second 'capture fps' (the first is on line 2)"), "a second capture rate")
    sc = lint("scene a\nat 2 capture fps 60\nat 2 capture off\nend 5\n")
    check_(has(sc, "error", 3, "nothing would be captured"), "an off at the start's own time")
    sc = lint("scene a fps 30\nat 0 capture fps 60\nend 5\n")
    check_(has(sc, "warning", 2, "the capture's is used"), "a header fps that differs from the capture's")
    sc = lint("scene a\nat 1 capture fps 60\nat 7 capture off\nend 5\n")
    check_(has(sc, "error", 3, "after the scene's end"), "a capture off after the end")
    # the take folder's verbs (M2b): a name, a sink, a state log
    sc = lint("scene a\nat 0 capture fps 60 step 1/60 name my_take.v2 sink PNG statelog on\nend 5\n")
    check_(not sc.diags, f"a capture with a name, a sink and a state log is clean; got {[x.text('a') for x in sc.diags]}")
    ev = [e for e in sc.sorted_events() if e.track == "capture"][0]
    check_(ev.name == "my_take.v2" and ev.text == "png" and ev.index == 1, "the options are kept on the event (sink lower-cased)")
    check_(describe(ev) == f"capture fps {fmt(60)} name my_take.v2 sink png statelog on", f"describe says them: {describe(ev)}")
    sc = lint("scene a\nat 0 capture fps 30 statelog off\nend 5\n")
    check_(not sc.diags and describe([e for e in sc.sorted_events() if e.track == "capture"][0]) == f"capture fps {fmt(30)} statelog off", "statelog off")

    # the state log option of the whole scene (M3a)
    for text, part in (("at 2 statelog on", "an option of the whole scene"), ("statelog", "nothing after 'statelog'"), ("statelog maybe", "'maybe' is neither"),
                       ("statelog on dir", "'dir' needs a value"), ("statelog on file x.csv", "unknown option 'file' (dir)"), ("statelog on dir \"\"", "statelog dir needs a folder"),
                       ("statelog off now", "statelog off takes nothing after it")):
        sc = lint("scene a\n" + text + "\nend 5\n")
        check_(has(sc, "error", 2, part), f"statelog: '{text}' says '{part}'")
    sc = lint("scene a\nstatelog on\nstatelog off\nend 5\n")
    check_(has(sc, "error", 3, "a second 'statelog' (the first is on line 2)"), "a second statelog")
    sc = lint("scene a\nstatelog on dir \"takes/one\"\nend 5\n")
    check_(not sc.diags and sc.statelog and sc.statelog_dir == "takes/one", "statelog on dir <folder> is clean and kept")
    sc = lint("scene a\nat 0 STATELOG OFF\nend 5\n")
    check_(not sc.diags and not sc.statelog and sc.statelog_line == 2, "statelog off at time 0, any case")
    sc = lint("statelog on\nend 4\n")
    check_(not [x for x in sc.diags if x.severity == "error"], "a scene of only statelog and end lints")
    sc = lint("scene a\nstatelo on\nend 5\n")
    check_(has(sc, "error", 2, "end, statelog)"), "the unknown-statement message names statelog")

    # key order
    sc = lint("scene o\nat 5 hour 12\nat 2 hour 14\nat 3 weather nice\nend 9\n")
    check_(has(sc, "error", 3, "hour key at t=2.00 comes after line 2 (t=5.00)"), "hour keys out of order: error on the later line")
    sc = lint("scene o\nat 5 mark a\nat 2 weather nice\nend 9\n")
    check_(not any(x.severity == "error" for x in sc.diags) and has(sc, "warning", 3, "comes after line 2"), "keys of different tracks out of order: a warning only")
    sc = lint("scene o\ncam 0 pos 1 2 3\ncam 4 pos 4 5 6\ncut 2\nend 9\n")
    check_(has(sc, "error", 4, "cam key at t=2.00 comes after line 3"), "cam and cut are one track")
    sc = lint("scene o\ncar p1 spawn garage:1 at 1 2 3\nat 5 car p1 path r1\nat 2 car p1 speed 1\nend 9\n")
    check_(has(sc, "error", 4, "car p1 key at t=2.00 comes after line 3"), "one car's keys are one track")

    # durations
    sc = lint("scene d\nat 7 hour 12\nend 5\n")
    check_(has(sc, "error", 2, "after the scene's end (t=5.00, line 3)"), "a key after the end")
    sc = lint("scene d\nat 4 hour 10..12 over 3\nend 5\n")
    check_(has(sc, "error", 2, "runs to t=7.00, past the scene's end"), "a ramp past the end is an error in the lint")
    sc = lint("scene d\nat 4 label \"x\" for 3\nend 5\n")
    check_(has(sc, "error", 2, "past the scene's end"), "a label past the end")
    sc = lint("scene d\nat 1 hud fade 3..9 to 0\nend 5\n")
    check_(has(sc, "error", 2, "past the scene's end"), "a hud fade past the end")
    sc = lint("scene d\nat 0 hour 10..14 over 8\nat 4 hour 20\nend 9\n")
    check_(has(sc, "warning", 2, "ramp at t=0.00 over 8.00 s is cut off by the key at t=4.00 (line 3)"), "a ramp cut off by the next key")
    sc = lint("scene d\nat 1 mark a\n")
    check_(has(sc, "warning", 0, "no 'end'"), "no end")
    sc = lint("scene d fps 60 step 1/50\nend 1\n")
    check_(has(sc, "warning", 1, "does not match fps"), "step against fps")
    sc = lint("scene d\nat 1 timescale 0.001\nend 3\n")
    check_(has(sc, "warning", 2, "under 0.01"), "a time scale under the engine's lowest step")
    sc = lint("scene d\nat 1 label \"x\" fade 2 for 3\nend 5\n")
    check_(has(sc, "warning", 2, "more than half"), "a label fade longer than half its time")
    sc = lint("scene d\nat 1 hour 10\nat 1 hour 12\nend 5\n")
    check_(has(sc, "warning", 3, "two hour keys at t=1.00"), "two keys at one time")
    sc = lint("scene d\nstart save a\nstart save b\nend 5\n")
    check_(has(sc, "error", 3, "a second 'start'"), "a second start")

    # settings
    sc = lint("scene s\nat 1 set mcla_gpu_ao off\nat 2 set mcla_gpu_ao low\nat 3 set mcla_gpu_scale 9\nat 4 set mcla_gpu_scale 1.5\nat 5 set mcla_gpu_msaa 4.5\n"
              "at 6 set mcla_flag yes\nat 7 set mcla_flag true\nat 8 set mcla_restart_only true\nat 9 set mcla_gpu_oa off\nend 10\n", FAKE_SRC)
    check_(not any(x.line == 2 for x in sc.diags), "an allowed value is fine")
    check_(has(sc, "error", 3, "'low' is not one of auto, off, high, ultra"), "a value the setting does not take")
    check_(has(sc, "error", 4, "9 is out of range (0.5 to 3)"), "a value out of range")
    check_(not any(x.line == 5 for x in sc.diags), "a value in range is fine")
    check_(has(sc, "error", 6, "'4.5' is not a whole number"), "an int that is not whole")
    check_(has(sc, "error", 7, "'yes' is not a bool"), "a bool that is not one")
    check_(not any(x.line == 8 for x in sc.diags), "true is a bool")
    check_(has(sc, "error", 9, "read only at start"), "a restart-only setting")
    check_(has(sc, "error", 10, "no setting called 'mcla_gpu_oa' (did you mean mcla_gpu_ao"), "an unknown setting, with a suggestion")
    check_(not any(x.severity == "error" and "setting" in x.message for x in lint("scene s\nat 1 set anything 1\nend 3\n").diags), "no source: setting names are not checked")

    # cars
    sc = lint("scene c\ncam 2 look car:p1 pos 1 2 3\ncar p1 spawn garage:5 at 1 2 3\nat 5 hud off\nend 9\n")
    check_(not has(sc, "error", 2, "before it is spawned"), "a bare car statement is at 0: the camera key at 2 is after it")
    sc = lint("scene c\ncam 2 look car:p1 pos 1 2 3\nat 5 car p1 spawn garage:5 at 1 2 3\nend 9\n")
    check_(has(sc, "error", 2, "car p1 is used at t=2.00 before it is spawned at t=5.00 (line 3)"), "a car used by the camera before its spawn")
    sc = lint("scene c\nat 1 car p1 path route.r\nat 5 car p1 spawn garage:5 at 1 2 3\nend 9\n")
    check_(has(sc, "error", 2, "car p1 is used at t=1.00 before it is spawned at t=5.00"), "a car given a path before its spawn")
    sc = lint("scene c\ncam 2 look car:p9\nend 9\n")
    check_(has(sc, "error", 2, "car 'p9' is used at t=2.00 but is never spawned"), "a car that is never spawned")
    sc = lint("scene c\ncam 2 look car:player\nend 9\n")
    check_(not sc.diags, "the player's car needs no spawn")
    sc = lint("scene h\nat 1 hud 0.5\nat 2 hud fade over 2 to off\nat 5 hud fade 5..6 to on\nat 7 hud on\nend 9\n")
    check_(not sc.diags, "every form of the hud track (alpha, fade over, fade range, on)")
    sc = lint("scene h\nat 8 hud fade over 3 to off\nend 9\n")
    check_(has(sc, "error", 2, "past the scene's end"), "a hud fade over seconds past the end")
    sc = lint("scene c\ncar p1 spawn garage:5 at 1 2 3\ncar p1 spawn garage:6 at 1 2 3\nend 9\n")
    check_(has(sc, "error", 3, "car p1 is spawned twice (line 2 and this one)"), "spawned twice")
    sc = lint("scene c\ncar p1 spawn garage:5 at 1 2 3\nat 4 car p1 despawn\ncam 6 look car:p1\nend 9\n")
    check_(has(sc, "error", 4, "car p1 is used at t=6.00 after it was removed at t=4.00 (line 3)"), "used after the removal")
    sc = lint("scene c\nat 3 car p1 despawn\nend 9\n")
    check_(has(sc, "error", 2, "is removed at t=3.00 but is never spawned"), "removing a car that was never there")
    sc = lint("scene c\ncar p1 spawn vp_nsn_skyline_99 at 1 2 3\ncar p2 spawn vp_nsn_skylyne_99 at 1 2 3\ncar p3 spawn garage:3 at 1 2 3\nend 9\n", None, {"vp_nsn_skyline_99", "vp_chv_impala_96"})
    check_(not any(x.line == 2 for x in sc.diags) and not any(x.line == 4 for x in sc.diags), "a listed car name and garage:N are fine")
    check_(has(sc, "error", 3, "unknown car 'vp_nsn_skylyne_99' (did you mean vp_nsn_skyline_99"), "an unknown car name, with a suggestion")

    # the car verbs of the cars package (M4b; car_verbs.cpp has the same rules and cars_test.cpp the same cases)
    car_bad = [
        ("at 1 car p1 spawn", "spawn what?"), ("at 1 car p1 spawn vp_x", "spawn needs a place"), ("at 1 car p1 spawn vp_x at", "at needs x,y,z"),
        ("at 1 car p1 spawn vp_x at 1,2", "bad position '1,2'"), ("at 1 car p1 spawn vp_x at 1 2", "at needs x,y,z"),
        ("at 1 car p1 spawn vp_x at 1 2 z", "bad position 'z'"), ("at 1 car p1 spawn vp_x at 1,2,3 at 4,5,6", "a second 'at'"),
        ("at 1 car p1 spawn vp_x at 1,2,3 heading", "heading needs degrees"), ("at 1 car p1 spawn vp_x at 1,2,3 heading north", "heading needs degrees"),
        ("at 1 car p1 spawn vp_x at 1,2,3 sparkle red", "unknown option 'sparkle'"), ("at 1 car p1 spawn vp_x at 1,2,3 paint red", "paint needs a whole number"), ("at 1 car p1 spawn profile 2 at 1,2,3 paint 1", "keeps the paint the owner gave it"), ("at 1 car p1 spawn vp_x at 1,2,3 paint 1 colour 2", "spawn: a second 'paint'"), ("at 1 car p1 spawn vp_x at 999999,0,0", "outside the world"),
        ("at 1 car p1 spawn profile at 1,2,3", "needs a garage slot"), ("at 1 car p1 spawn profile 1000 at 1,2,3", "needs a garage slot"),
        ("at 1 car p1 spawn car:3 at 1,2,3", "is not a car"), ("at 1 car p1 spawn Vp.Bad/Name at 1,2,3", "not a vehicle name"),
        ("at 1 car p1 delete now", "takes nothing after it"), ("at 1 car player delete", "'player' is the player's own car"),
        ("at 1 car player profile", "car player profile <N>"), ("at 1 car player profile 1 2", "car player profile <N>"),
        ("at 1 car p1 profile 3", "only the player's car"),
    ]
    car_bad += [
        ("at 1 car p1 path", "path needs a route file"), ("at 1 car p1 path ../x.route", "not a route file name"), ("at 1 car p1 path a/../../b.route", "not a route file name"),
        ("at 1 car p1 path /abs/x.route", "not a route file name"), ("at 1 car p1 path C:x.route", "not a route file name"), ("at 1 car p1 path a;b.route", "not a route file name"),
        ("at 1 car p1 path a.route speed", "speed needs metres a second"), ("at 1 car p1 path a.route speed fast", "speed needs metres a second"),
        ("at 1 car p1 path a.route speed 200", "from 0.5 to 120 metres a second"), ("at 1 car p1 path a.route speed 0.1", "from 0.5 to 120 metres a second"),
        ("at 1 car p1 path a.route speed x50", "a speed factor from 0.05 to 10"), ("at 1 car p1 path a.route speed xx", "a factor of the route's own speeds is x1.5"),
        ("at 1 car p1 path a.route speed 5 speed 6", "a second 'speed'"), ("at 1 car p1 path a.route mode", "mode needs brain or kinematic"),
        ("at 1 car p1 path a.route mode flying", "brain (the game's own AI drives) or kinematic"), ("at 1 car p1 path a.route mode brain mode brain", "a second 'mode'"),
        ("at 1 car p1 path a.route colour red", "path: unknown option 'colour'"), ("at 1 car player path a.route", "'player' is the player's own car"),
        ("at 1 car p1 stop now", "takes nothing after it"), ("at 1 car player stop", "'player' is the player's own car"),
    ]
    car_bad += [
        ("at 1 car player at", "at needs x,y,z"), ("at 1 car player at 1,2", "bad position '1,2'"), ("at 1 car player at 1 2", "at needs x,y,z (as"),
        ("at 1 car player at 1,2,3 at 4,5,6", "a second 'at'"), ("at 1 car player at 1,2,3 heading", "heading needs degrees"),
        ("at 1 car player at 1,2,3 heading 1 heading 2", "a second 'heading'"), ("at 1 car player at 999999,0,0", "outside the world"),
        ("at 1 car player at 1,2,3 paint 3", "unknown option 'paint'"), ("at 1 car player at 1,2,3 near n1 1,0,0", "unknown option 'near'"),
        ("at 1 car p1 at 1,2,3", "only the player's car is put with `at`"), ("at 1 car player spawn x at 1,2,3", "car player at ..."),
        ("at 1 car p1 spawn vp_x near", "near needs a car"), ("at 1 car p1 spawn vp_x near 3x 1,0,0", "is not one"), ("at 1 car p1 spawn vp_x near player", "near needs right,up,forward"),
        ("at 1 car p1 spawn vp_x near player 1,2", "bad offset '1,2'"), ("at 1 car p1 spawn vp_x near player 1 2", "near needs right,up,forward (as"),
        ("at 1 car p1 spawn vp_x near player 1 2 z", "bad offset 'z'"), ("at 1 car p1 spawn vp_x near player 1,2,3 near player 1,2,3", "a second 'near'"),
        ("at 1 car p1 spawn vp_x near player 1,2,3 at 1,2,3", "do not go together"), ("at 1 car p1 spawn vp_x at 1,2,3 near player 1,2,3", "do not go together"),
        ("at 1 car p1 spawn vp_x near p1 1,0,0", "cannot be near itself"), ("at 1 car p1 spawn vp_x near player 5000,0,0", "more than 1000 m"),
        ("at 1 car p1 spawn vp_x near player 1,2,3 heading", "heading needs degrees"), ("at 1 car p1 ambient vp_x near player 1,2,3 heading 1 heading 2", "ambient: a second 'heading'"),
        ("at 1 car p1 spawn vp_x heading 5", "spawn needs a place"),
    ]
    for line, part in car_bad:
        sc = lint("scene t\n" + line + "\nend 10\n")
        check_(has(sc, "error", 2, part), f"'{line}' -> error on line 2 saying '{part}'; got {[x.text('t') for x in sc.diags]}")
    sc = lint("scene c\nat 0 car player at 832.19,16.9,-787.53 heading 62\nat 0.5 car n1 spawn vp_x near player -3.35,0.3,0\nat 0.5 car n2 spawn vp_x near Player 3.35 0.3 0 heading 180\n"
              "at 0.5 car a1 ambient va_gmc_yukon_05 near n1 0,0,-6\nat 0.5 car n3 spawn profile 2 near n2 0,0,-5\nat 5 car player at 1 2 3\nend 9\n", FAKE_SRC)
    check_(not sc.diags, f"car player at and near are silent; got {[x.text('c') for x in sc.diags]}")
    sc = lint("scene c\nat 1 car n2 spawn vp_x near n1 1,0,0\nat 2 car n1 spawn vp_x at 1,2,3\nend 9\n")
    check_(has(sc, "error", 2, "car n1 is used at t=1.00 before it is spawned at t=2.00"), "near a car that is spawned later")
    sc = lint("scene c\nat 1 car n2 spawn vp_x near n1 1,0,0\nat 1 car n1 spawn vp_x at 1,2,3\nend 9\n")
    check_(has(sc, "error", 2, "placed near car n1, which is spawned on a later line (line 3)"), "near a car spawned later in the file at the same time")
    sc = lint("scene c\nat 1 car n1 spawn vp_x at 1,2,3\nat 2 car n2 spawn vp_x near n9 1,0,0\nend 9\n")
    check_(has(sc, "error", 3, "car 'n9' is used at t=2.00 but is never spawned"), "near a car that is never spawned")
    sc = lint("scene c\nat 1 car n1 spawn vp_x at 1,2,3\nat 3 car n1 delete\nat 4 car n2 spawn vp_x near n1 1,0,0\nend 9\n")
    check_(has(sc, "error", 4, "car n1 is used at t=4.00 after it was removed at t=3.00"), "near a car that was deleted")
    sc = lint("scene c\nat 1 car a spawn vp_x at 1,2,3 heading 90\nat 2 car b spawn profile 2 at 1 2 3\nat 3 car player profile 4\nat 4 car a delete\nat 5 car b path r.route speed 1\nend 9\n")
    check_(not sc.diags, f"the car statements are silent; got {[x.text('c') for x in sc.diags]}")
    sc = lint("scene c\nat 1 car a spawn vp_x at 1,2,3\nat 2 car a path r.route speed x1.5 mode brain\nat 3 car a path routes/r2.route speed 12 mode kinematic\nat 4 car a stop\nend 9\n")
    check_(not sc.diags, f"path (both modes, both speed forms) and stop are silent; got {[x.text('c') for x in sc.diags]}")
    sc = lint("scene c\nat 2 car a path r.route\nat 3 car a spawn vp_x at 1,2,3\nend 9\n")
    check_(has(sc, "error", 2, "car a is used at t=2.00 before it is spawned at t=3.00"), "a path before the spawn")
    sc = lint("scene c\nat 1 car a spawn vp_x at 1,2,3\nat 4 car a delete\nat 5 car a stop\nend 9\n")
    check_(has(sc, "error", 4, "car a is used at t=5.00 after it was removed at t=4.00"), "a stop after the delete")
    check_(route_name_ok("a.route") and route_name_ok("routes/x y.route") and route_name_ok("m4c-a_1.route") and route_name_ok(chr(100) + chr(92) + "a.route"), "route names that are fine")
    check_(not route_name_ok("") and not route_name_ok("..") and not route_name_ok("./a") and not route_name_ok(chr(92) + "a") and not route_name_ok("a" * 121), "route names that are not")
    check_(route_name_ok("a" * 120), "120 characters are fine")

    # the route files of the path keys (path_follow.cpp has the same rules; path_follow_test.cpp the same cases)
    route_bad = [
        ("1,2\n", "at least 2 points"), ("", "at least 2 points"), ("1,2\n3\n", "line 2: a route point is x,z"), ("1,2\n3,4,5,6,7\n", "line 2: a route point is x,z"),
        ("1,2\n3,x\n", "line 2: 'x' is not a number"), ("1,2\n3,nan\n", "line 2: 'nan' is not a number"), ("1,2\n3,4,-1\n", "line 2: a speed from 0 to 1000"),
        ("1,2\n999999,4\n", "line 2: a coordinate outside the world"), ("1,2,5,1\n3,4,5\n", "the height (the fourth number) is on 1 of 2 points"),
        ("0,0\n0,0.5\n", "at least 1 m"), ("0,0\n0,0.005\n", "no two points more than 1 cm apart"),
    ]
    for text, part in route_bad:
        check_(part in (route_text_error(text) or ""), f"route text {text!r} -> '{part}'; got {route_text_error(text)}")
    for text in ("1,2\n3,4\n", "# a street\n819.60,-808.40,4.0\n819.62,-807.49,4.0\n\n819.64 -806.59 6\n", "1,2,5,17.5\n3,4,5,17.7\r\n", "1,2,5 # the first\n3,4,5\n"):
        check_(route_text_error(text) is None, f"route text {text!r} is fine; got {route_text_error(text)}")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "routes").mkdir()
        (d / "good.route").write_text("0,0,10\n0,30,10\n", encoding="utf-8")
        (d / "routes" / "deep.route").write_text("0,0\n0,30\n", encoding="utf-8")
        (d / "bad.route").write_text("0,0\nx,3\n", encoding="utf-8")
        scene_file = d / "t.scene"
        text = ("scene t\nat 1 car a spawn vp_x at 1,2,3\nat 2 car a path good.route\nat 3 car a path deep\nat 4 car a path routes/deep.route\n"
                "at 5 car a path missing.route\nat 6 car a path bad.route\nend 9\n")
        sc = lint(text)
        check_routes(sc, scene_file)
        check_(not has(sc, "error", 3, "route"), "a route beside the scene is found")
        check_(not has(sc, "error", 4, "route") and not has(sc, "error", 5, "route"), "a route in the routes folder is found, with or without the extension")
        check_(has(sc, "error", 6, "no route file 'missing.route' (looked for"), "a missing route is an error on its line")
        check_(has(sc, "error", 7, "route 'bad.route': line 2: 'x' is not a number"), "a route that is not one is an error on its line")

    # the examples with paths name routes that exist and are routes
    for pth in sorted((HERE / "scenes" / "recipes").glob("*.scene")):
        sc = lint(pth.read_text(encoding="utf-8"), None, None, load_spots_for(pth)[0])
        check_routes(sc, pth)
        check_(not [x for x in sc.diags if x.severity == "error"], f"{pth.name}: its route files exist and are routes; got {[x.text(pth.name) for x in sc.diags]}")

    sc = lint("scene c\nat 1 car a spawn vp_x at 1,2,3\nat 4 car a delete\ncam 6 look car:a\nend 9\n")
    check_(has(sc, "error", 4, "car a is used at t=6.00 after it was removed at t=4.00"), "delete is a removal like despawn")
    sc = lint("scene c\nat 3 car a delete\nend 9\n")
    check_(has(sc, "error", 2, "is removed at t=3.00 but is never spawned"), "deleting a car that was never there")
    sc = lint("scene c\nat 1 car a spawn profile 3 at 1,2,3\nend 9\n", None, {"vp_x"})
    check_(not sc.diags, "spawn profile N needs no name from the car list")

    # the scanner on a fixture of real definitions (scenes\fixture-src), then the scenes: the examples are clean, the planted ones say what they plant
    cvars = scan_cvars(HERE / "scenes" / "fixture-src")
    ao = cvars.get("mcla_gpu_ao")
    check_(ao is not None and ao.kind == "STRING" and ao.allowed == ["auto", "off", "high", "ultra"] and ao.lifecycle == "kHotReload" and not ao.debug_only,
           "the scanner reads a string setting with its allowed values from a multi-line definition")
    fe = cvars.get("mcla_gpu_ao_fade_end")
    check_(fe is not None and fe.kind == "DOUBLE" and fe.lo == 20.0 and fe.hi == 1000.0 and fe.debug_only, "the scanner reads a range and .debug_only()")
    rs = cvars.get("mcla_fixture_restart")
    check_(rs is not None and rs.lifecycle == "kRequiresRestart", "the scanner reads the lifecycle")
    check_("mcla_in_a_comment" not in cvars, "a definition inside a // comment is not a setting")
    scenes = HERE / "scenes"
    for p in sorted((scenes / "recipes").glob("*.scene")):
        sc = lint(p.read_text(encoding="utf-8"), cvars, None, load_spots_for(p)[0])
        check_(not sc.diags, f"{p.name} is clean; got {[x.text(p.name) for x in sc.diags]}")
        check_(sc.end >= 0, f"{p.name} has an end")
    for p in sorted((scenes / "bad").glob("*.scene")):
        text = p.read_text(encoding="utf-8")
        sc = lint(text, cvars, None, load_spots_for(p)[0])
        expect = []  # (severity, line, text); a `#! expect-file` is about the whole file (line 0)
        for n, raw in enumerate(text.splitlines(), 1):
            m = re.search(r"#! expect (error|warning) (.+?)\s*$", raw)
            if m:
                expect.append((m.group(1), n, m.group(2)))
            m = re.search(r"^#! expect-file (error|warning) (.+?)\s*$", raw)
            if m:
                expect.append((m.group(1), 0, m.group(2)))
        check_(len(expect) > 0, f"{p.name} says what it plants (#! expect error TEXT at the end of the line)")
        for sev, line, part in expect:
            check_(has(sc, sev, line, part), f"{p.name}: expected {sev} on line {line} saying '{part}'; got {[x.text(p.name) for x in sc.diags]}")
        listed = {line for sev, line, _ in expect if sev == "error"}
        for x in sc.diags:
            if x.severity == "error" and x.line not in listed:
                check_(False, f"{p.name}: an error nobody planted: {x.text(p.name)}")

    # ambient cars and the density keys (ambient-scenes: car_verbs.h `ambient`, scene.cpp Density; cars_test.cpp and scene_test.cpp have the same cases)
    amb_bad = [
        ("at 1 car a1 ambient", "ambient what?"), ("at 1 car a1 ambient vp_x", "ambient needs a place"), ("at 1 car a1 ambient vp_x at", "at needs x,y,z"),
        ("at 1 car a1 ambient vp_x at 1,2", "bad position '1,2'"), ("at 1 car a1 ambient vp_x at 999999,0,0", "outside the world"),
        ("at 1 car a1 ambient vp_x at 1,2,3 at 4,5,6", "ambient: a second 'at'"), ("at 1 car a1 ambient vp_x at 1,2,3 heading 1 heading 2", "ambient: a second 'heading'"),
        ("at 1 car a1 ambient vp_x at 1,2,3 sparkle red", "ambient: unknown option 'sparkle'"), ("at 1 car a1 ambient vp_x at 1,2,3 paint 10000", "paint needs a whole number"),
        ("at 1 car a1 ambient profile 3 at 1,2,3", "not an ambient type"), ("at 1 car a1 ambient garage:3 at 1,2,3", "not an ambient type"),
        ("at 1 car a1 ambient car:3 at 1,2,3", "not an ambient type"), ("at 1 car a1 ambient Vp.Bad/Name at 1,2,3", "not an ambient type name"),
        ("at 1 car a1 ambient abcdefghijklmnopqrstuvwxyz_01234 at 1,2,3", "up to 31"), ("at 1 car player ambient vp_x at 1,2,3", "'player' is the player's own car"),
        ("at 1 car p1 spawn vp_x at 1,2,3 at 4,5,6", "spawn: a second 'at'"),
        ("at 1 peds 2.5", "bad peds '2.5'"), ("at 1 parked -0.1", "bad parked '-0.1'"), ("at 1 peds lots", "bad peds 'lots'"), ("at 1 peds", "peds on|off, or a density scale"),
        ("at 1 parked 1 2", "parked on|off, or a density scale"), ("at 1 parked nan", "bad parked 'nan'"), ("at 1 peds maybe", "ped_density_scale"),
        ("at 1 parked maybe", "parked_car_scale"), ("at 1 traffic 0.5", "traffic on|off"), ("world peds", "world: 'peds' needs a value"),
    ]
    for line, part in amb_bad:
        sc = lint("scene t\n" + line + "\nend 10\n", FAKE_SRC)
        check_(has(sc, "error", 2, part), f"'{line}' -> error on line 2 saying '{part}'; got {[x.text('t') for x in sc.diags]}")
    sc = lint("scene c\nworld hour 17 hold weather nice traffic off police off peds 0 parked 0.25\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3 heading 45\nat 3 car a2 ambient va_mb_eclipse_04 at 4 5 6\nat 6 car a1 delete\nend 9\n", FAKE_SRC)
    check_(not sc.diags, f"ambient cars with traffic off, peds 0 and a parked scale: silent; got {[x.text('c') for x in sc.diags]}")
    sc = lint("scene c\nat 1 peds off\nat 2 parked on\nat 3 peds true\nat 4 parked false\nat 5 traffic 1\nat 6 police 0\nat 7 peds 0\nat 8 parked 2.0\nend 9\n", FAKE_SRC)
    check_(not sc.diags, f"on, off, true, false and the scales parse; got {[x.text('c') for x in sc.diags]}")
    states = [(e.track, e.state) for e in sc.sorted_events()]
    check_(states[0] == ("peds", 0) and states[6] == ("peds", STATE_SCALE) and states[7] == ("parked", STATE_SCALE), "a bare 0 on peds is a scale, `off` is the switch")
    sc = lint("scene c\nworld parked off\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(has(sc, "warning", 3, "parked spawner is closed at t=2.00 (line 2)"), "parked off before an ambient car: a warning on the car's line")
    sc = lint("scene c\nworld parked 0\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(has(sc, "warning", 3, "parked spawner is closed"), "a parked scale of 0 closes it too")
    sc = lint("scene c\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nat 3 parked off\nend 9\n", FAKE_SRC)
    check_(not any("parked spawner" in x.message for x in sc.diags), "closing it after the car was asked for is not the warning's business")
    sc = lint("scene c\nworld parked off\nat 1 parked 0.5\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(not any("parked spawner" in x.message for x in sc.diags), "a scale above 0 opens it again")
    sc = lint("scene c\nworld parked 0\nat 1 parked on\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(not any("parked spawner" in x.message for x in sc.diags), "on opens it again")
    sc = lint("scene c\nworld parked off\nat 2 car a1 spawn vp_x at 1,2,3\nend 9\n", FAKE_SRC)
    check_(not any("parked spawner" in x.message for x in sc.diags), "a spawned racer has nothing to do with the parked spawner")
    sc = lint("scene c\nat 1 car a1 ambient va_gmc_yukon_05 at 1,2,3\nat 2 car a1 path r.route\nat 3 car a1 stop\nend 9\n", FAKE_SRC)
    check_(has(sc, "error", 3, "car a1 is an ambient car") and has(sc, "error", 4, "car a1 is an ambient car"), "a path or a stop on an ambient car is an error")
    sc = lint("scene c\ncam 2 look car:a1 pos 1 2 3\nat 5 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(has(sc, "error", 2, "car a1 is used at t=2.00 before it is spawned at t=5.00"), "the camera on an ambient car before it is placed")
    sc = lint("scene c\nat 1 car a1 ambient va_gmc_yukon_05 at 1,2,3\ncam 3 look car:a1 pos 1 2 3\nat 4 car a1 delete\nend 9\n", FAKE_SRC)
    check_(not sc.diags, f"an ambient car is placed, looked at and deleted without a word; got {[x.text('c') for x in sc.diags]}")
    sc = lint("scene c\nat 1 car a1 ambient va_gmc_yukon_05 at 1,2,3\nat 2 car a1 ambient va_gmc_yukon_05 at 1,2,3\nend 9\n", FAKE_SRC)
    check_(has(sc, "error", 3, "car a1 is spawned twice (line 2 and this one)"), "an id placed twice")
    sc = lint("scene c\nat 1 car a1 ambient va_not_in_the_player_list at 1,2,3\nend 9\n", None, {"vp_x"})
    check_(not sc.diags, f"an ambient type is not checked against the player's car list; got {[x.text('c') for x in sc.diags]}")

    # spots (spots.h and spots.cpp; spots_test.cpp has the same cases)
    check_(is_spot_word("spot:x") and is_spot_word("SPOT:x") and is_spot_word("Spot:") and not is_spot_word("spot") and not is_spot_word("spots:x") and not is_spot_word("car:x"),
           "what a spot word is")
    ok_names = ["hw-street", "k2", "a.b_c-d", "9lives", "a" * 40]
    bad_names = ["", "-x", "_x", ".x", "a b", "a@b", "a,b", "a:b", "a" * 41]
    check_(all(SPOT_NAME.match(n) for n in ok_names) and not any(SPOT_NAME.match(n) for n in bad_names), "spot names")
    for word, why in (("spot:", "a spot name is letters, digits"), ("spot:a b", "a spot name is letters, digits"), ("spot:a@", "after @ come the right, up and forward metres"),
                      ("spot:a@1,2", "@ needs right,up,forward"), ("spot:a@1,2,x", "after @ come"), ("spot:a@1,2,3,4,5", "after @ come"), ("spot:a@1,,3", "after @ come"),
                      ("spot:a@nan,0,0", "after @ come"), ("spot:a@99999,0,0", "after @ come"), ("spot:a@0x,1,2", "after @ come")):
        ref, msg = parse_spot_ref(word)
        check_(ref is None and why in (msg or ""), f"spot word {word!r} -> {why!r}; got {msg}")
    ref, msg = parse_spot_ref("SPOT:Hw-Street@1.5,0,-3,180")
    check_(ref == ("Hw-Street", (1.5, 0.0, -3.0), 180.0), f"a spot word with an offset and a turn; got {ref}")
    check_(parse_spot_ref("spot:a")[0] == ("a", None, None), "a bare spot word")
    tbl, probs = parse_spots_text("# c\nhw-street 832.19 16.5 -787.53 62   # the street; src: x\n  Hw-Lot, 819.6, 17.6, -808.4, 332\ncorner 100 5 200\nneg -150 0 -3 -90\nx 1 2\nbad# 1 2 3\n"
                                  "a 1 2 3 4\nA 9 9 9 9\n", "s.txt")
    check_(sorted(tbl) == ["a", "corner", "hw-lot", "hw-street", "neg"], f"the good lines are kept; got {sorted(tbl)}")
    check_(tbl["hw-street"].note == "the street; src: x" and tbl["hw-street"].heading == 62.0 and tbl["hw-street"].has_heading, "a spot with its note")
    check_(not tbl["corner"].has_heading and tbl["neg"].heading == 270.0 and tbl["a"].x == 9.0, "no heading; a heading below 0 wraps; a name twice: the later wins")
    errs = [(n, m) for n, sev, m in probs if sev == "error"]
    check_(len(errs) == 2 and errs[0][0] == 6 and "(got 2 numbers)" in errs[0][1] and errs[1][0] == 7 and "is not a spot name" in errs[1][1], f"two bad lines, on their own lines; got {errs}")
    check_(any(sev == "warning" and n == 9 and "line 8" in m for n, sev, m in probs), "a name twice in one file is a warning")
    st = {"hw-street": Spot("hw-street", 832.19, 16.5, -787.53, 62.0, True), "hw-lot": Spot("hw-lot", 819.6, 17.6, -808.4, 332.0, True), "corner": Spot("corner", 100.0, 5.0, 200.0)}
    for words, want in (
            ("spawn vp_x at spot:hw-lot heading spot:hw-lot paint 3", "spawn vp_x at 819.6 17.6 -808.4 heading 332 paint 3"),
            ("pos 1 2 3 yaw spot:hw-street pitch -4", "pos 1 2 3 yaw 62 pitch -4"),
            ("warp spot:hw-lot", "warp 819.6 17.6 -808.4"),
            ("rig tripod at spot:hw-lot track car:p1", "rig tripod at 819.6 17.6 -808.4 track car:p1"),
            ("SPOT:HW-LOT", "819.6 17.6 -808.4"),
            ("at spot:hw-street@1.4,0,0,180 heading spot:hw-street@1.4,0,0,180", None),
            ("pos 1 2 3", "pos 1 2 3")):
        got, why = expand_spot_words(words.split(), st)
        check_(why is None and (want is None or " ".join(got) == want), f"expand {words!r} -> {want!r}; got {got} {why}")
    got, _ = expand_spot_words("at spot:hw-street@1.4,0,0,180 heading spot:hw-street@1.4,0,0,180".split(), st)
    check_(len(got) == 6 and got[-1] == "242", f"the turn is added to a heading and ignored for a place; got {got}")
    got, _ = expand_spot_words("pos spot:hw-street@0,1.7,-8".split(), st)
    h = math.radians(62.0)
    check_(abs(float(got[1]) - (832.19 - 8 * math.sin(h))) < 1e-3 and abs(float(got[2]) - 18.2) < 1e-9 and abs(float(got[3]) - (-787.53 - 8 * math.cos(h))) < 1e-3,
           f"8 m behind a spot facing 62 degrees; got {got}")
    got, _ = expand_spot_words("pos spot:hw-street@2,0,0".split(), st)
    check_(abs(float(got[1]) - (832.19 - 2 * math.cos(h))) < 1e-3 and abs(float(got[3]) - (-787.53 + 2 * math.sin(h))) < 1e-3, f"the right of a heading is (-cos, sin); got {got}")
    for words, part in (("pos spot:nope", "unknown spot 'nope'"), ("pos spot:hw-stret", "did you mean hw-street?"), ("pos spot:corner@1,0,0", "has no heading"),
                        ("heading spot:corner", "has no heading"), ("pos spot:hw-lot@1,2", "@ needs right,up,forward"), ("orbit car:p1 r 8 at spot:hw-lot", "offset in the car's frame")):
        got, why = expand_spot_words(words.split(), st)
        check_(got is None and part in (why or ""), f"expand {words!r} says {part!r}; got {why}")
    got, why = expand_spot_words("pos spot:hw-lot".split(), None)
    check_(got is None and "no spot table is loaded" in why, "no table: an error, not a silent zero")
    check_(expand_spot_words("orbit car:p1 look spot:hw-lot".split(), st)[1] is None, "a look of an orbit is a place")
    # spot words and the pose words together: `car player at spot:...` is a place; after `near` a spot means nothing (an offset in a car's frame, a relative heading)
    got, why = expand_spot_words("player at spot:hw-lot heading spot:hw-lot".split(), st)
    check_(why is None and got == "player at 819.6 17.6 -808.4 heading 332".split(), f"car player at takes a spot place and heading; got {got} {why}")
    got, why = expand_spot_words("n1 spawn vp_x near player -3.35,0.3,0 heading 180".split(), st)
    check_(why is None and got == "n1 spawn vp_x near player -3.35,0.3,0 heading 180".split(), "near with numbers is left alone")
    got, why = expand_spot_words("n1 spawn vp_x at spot:hw-street@-1.5,0,0 heading spot:hw-street".split(), st)
    check_(why is None and got[:4] == ["n1", "spawn", "vp_x", "at"] and got[-1] == "62", "spawn at a spot still works")
    for words, part in (("n1 spawn vp_x near player spot:hw-lot", "after `near <car>`, the three numbers are an offset"),
                        ("n1 ambient va_x near n0 spot:hw-lot@1,0,0 heading 90", "not a place in the world"),
                        ("n1 spawn vp_x near player 3,0,0 heading spot:hw-lot", "after `near`, `heading` is degrees from the other car's heading")):
        got, why = expand_spot_words(words.split(), st)
        check_(got is None and part in (why or ""), f"expand {words!r} says {part!r}; got {why}")
    check_(expand_spot_words("pos spot:hw-lot near 0.1".split(), st)[1] is None, "a camera's `near <clip>` takes no spot word and is not an offset")
    sc = lint("scene s\nat 1 car player at spot:hw-street heading spot:hw-street\nat 2 car n1 spawn vp_x near player -3.35,0.3,0\nat 2 car n2 spawn vp_x at spot:hw-lot heading spot:hw-lot\nend 9\n", FAKE_SRC, None, st)
    check_(not sc.diags, f"the pose words and the spot words in one scene are silent; got {[x.text('s') for x in sc.diags]}")
    sc = lint("scene s\nat 2 car n1 spawn vp_x near player spot:hw-lot\nend 9\n", FAKE_SRC, None, st)
    check_(has(sc, "error", 2, "after `near <car>`"), "a spot word as the offset of near is an error on its line")
    check_(suggest_spots(st, "hw-stret") == ["hw-street"] and suggest_spots(st, "zzzzzz") == [], "did you mean")
    sc = lint("scene s\nstart warp spot:hw-lot\nat 1 car p1 spawn vp_x at spot:hw-street@-1.5,0,0 heading spot:hw-street\ncam 2 pos spot:hw-lot@0,1.5,-6 look spot:hw-street fov 40\nend 9\n", FAKE_SRC, None, st)
    check_(not sc.diags, f"spot words in a car, a camera and a start statement are silent; got {[x.text('s') for x in sc.diags]}")
    car_ev = [e for e in sc.events if e.track == "car"][0]
    check_(car_ev.words[:4] == ["p1", "spawn", "vp_x", "at"] and len(car_ev.words) == 9 and car_ev.words[-1] == "62", "the statement keeps numbers")
    sc = lint("scene s\nat 1 car p1 spawn vp_x at spot:hw-stret\nend 3\n", FAKE_SRC, None, st)
    check_(has(sc, "error", 2, "unknown spot 'hw-stret' (did you mean hw-street?)"), "an unknown spot is an error on its line")
    sc = lint("scene s\nat 1 car p1 spawn vp_x at spot:hw-lot\nend 3\n")
    check_(has(sc, "error", 2, "no spot table is loaded"), "without a table a spot word is an error")
    sc = lint("scene s\nat 1 car p1 spawn vp_x at spot:hw-lot heading\nend 3\n", FAKE_SRC, None, st)
    check_(has(sc, "error", 2, "heading needs degrees"), "the car rules judge what is left")
    sc = lint("scene s\nat 1 label \"spot:hw-lot\"\nat 2 mark spot-x\nend 3\n")
    check_(not sc.diags, "a label and a mark may hold the text spot:")
    sc = lint("scene s\nat 1 car p1 spawn vp_x at spot:hw-lot@0,0,5000\nend 3\n", FAKE_SRC, None, {"hw-lot": Spot("hw-lot", 99999.0, 0.0, 0.0, 90.0, True)})
    check_(has(sc, "error", 2, "outside the world"), "an offset that leaves the world is the car rule's error")

    # the shared spots.txt: every spot has a heading and a note with its source, nothing is given twice, the line is one line
    shared = HERE / "scenes" / "spots.txt"
    if shared.is_file():
        stbl, sprobs = parse_spots_text(shared.read_text(encoding="utf-8"), str(shared))
        check_(not sprobs, f"scenes\\spots.txt reads clean; got {sprobs}")
        check_("hw-lot" in stbl and "hw-street" in stbl and "hw-lane1" in stbl, "the recipe spots are present")
        for sp in stbl.values():
            check_(sp.has_heading and 0.0 <= sp.heading < 360.0, f"spot {sp.name} has a heading")
        route_file = HERE / "scenes" / "recipes" / "routes" / "street-lane1.route"
        rows = [ln.split(",") for ln in route_file.read_text(encoding="utf-8").splitlines() if ln and ln[0] != "#"]
        for word, row in (("spot:hw-lane1@0,0,-25", rows[0]), ("spot:hw-lane1@0,0,25", rows[-1])):
            got, _ = expand_spot_words(["pos", word], stbl)
            check_(abs(float(got[1]) - float(row[0])) < 0.015 and abs(float(got[3]) - float(row[1])) < 0.015,
                   f"{word} matches the authored path endpoint")

    # the rigs (camera.cpp RigStatement; camera_test.cpp has the same cases)
    rig_bad = [
        ("cam 1 rig", "rig needs a kind"), ("cam 1 rig dolly car:p1", "unknown rig 'dolly' (chase, orbit, tripod, flyby, hood, bumper or roof)"),
        ("cam 1 rig chase", "rig chase: needs a car"), ("cam 1 rig chase car:p1 car:p2", "rig chase: two cars (p1 and p2)"),
        ("cam 1 rig chase car:p1 dist 0", "rig chase: dist 0.00 (0.50 to 200.00 metres)"), ("cam 1 rig chase car:p1 dist 500", "rig chase: dist 500.00"),
        ("cam 1 rig chase car:p1 dist far", "rig chase: 'dist' needs a number (got 'far')"), ("cam 1 rig chase car:p1 dist", "rig chase: 'dist' needs a value"),
        ("cam 1 rig chase car:p1 dist 3 dist 4", "rig chase: 'dist' twice"), ("cam 1 rig chase car:p1 lag 11", "rig chase: lag 11.00 (0.00 to 10.00 seconds)"),
        ("cam 1 rig chase car:p1 speed 3", "rig chase: unknown word 'speed' (dist, height, side, lag, aim, ahead, fov, roll, near, far, ease, car:<id>)"),
        ("cam 1 rig chase car:p1 track car:p2", "rig chase: unknown word 'track'"), ("cam 1 rig chase car:p1 fov 500", "cam: fov 500.00 (1 to 170 degrees, vertical)"),
        ("cam 1 rig chase car:p1 ease wobbly", "bad ease 'wobbly'"), ("cam 1 rig chase car:p-1", "unknown word 'car:p-1'"),
        ("cam 1 rig chase car:p1 near 5 far 2", "cam: far 2.00 is not beyond near 5.00"),
        ("cam 1 rig orbit", "rig orbit: needs a car"), ("cam 1 rig orbit car:p1 az 20..200", "a sweep (az a..b) needs its length: over <seconds>"),
        ("cam 1 rig orbit car:p1 el 0..30", "a sweep (el a..b) needs its length"), ("cam 1 rig orbit car:p1 over 4", "over is the length of an az or el sweep"),
        ("cam 1 rig orbit car:p1 az sideways", "az needs degrees (az 90) or a sweep (az 20..200)"), ("cam 1 rig orbit car:p1 el 95", "el needs degrees from -89 to 89"),
        ("cam 1 rig orbit car:p1 r 0", "rig orbit: r 0.00 (0.10 to 5000.00 metres)"), ("cam 1 rig orbit car:p1 at 1 2", "'at' needs three numbers"),
        ("cam 1 rig orbit car:p1 az 1..2 over 0", "rig orbit: over 0.00 (0.05 to 3600.00 seconds)"),
        ("cam 1 rig orbit car:p1 dist 4", "rig orbit: unknown word 'dist' (r, az, el, over, at, lag, fov, roll, near, far, ease, car:<id>)"),
        ("cam 1 rig tripod track car:p1", "needs a place: at <x> <y> <z>"), ("cam 1 rig tripod at 1 2 3", "needs an aim: track car:<id>"),
        ("cam 1 rig tripod at 1 2 3 track car:p1 yaw 5", "two ways to aim"), ("cam 1 rig tripod at 1 2 3 car:p1", "'car:p1' goes after track (track car:p1)"),
        ("cam 1 rig tripod at 1 2 3 track", "track needs a car (track car:<id> [<dx> <dy> <dz>]) or a place"), ("cam 1 rig tripod at 1 2 3 track 1 2", "track needs a car"),
        ("cam 1 rig tripod at 1 2 3 track car:p1 1 2", "track car:<id> takes no offset or three numbers"), ("cam 1 rig tripod at 1 2 3 track car:p1 track car:p2", "track twice"),
        ("cam 1 rig tripod at 1 2 x track car:p1", "'at' needs three numbers"), ("cam 1 rig tripod at 1 2 3 track 1 2 3 aimlag 0.3", "aimlag lags a track on a car"),
        ("cam 1 rig tripod at 1 2 3 track car:p1 aimlag 20", "rig tripod: aimlag 20.00 (0.00 to 10.00 seconds)"),
        ("cam 1 rig tripod at 1 2 3 track car:p1 dist 3", "rig tripod: unknown word 'dist' (at, aimlag, fov, roll, near, far, ease, yaw, pitch, track)"),
        ("cam 1 rig flyby y 3 track car:p1", "rig flyby: needs a route: path <route file>"), ("cam 1 rig flyby path ../x.route y 3 track car:p1", "is not a route file name"),
        ("cam 1 rig flyby path r.route up 2 y 3 track car:p1", "up (above the route's own height) and y (in the world) are two ways"),
        ("cam 1 rig flyby path r.route y 3", "needs an aim: track car:<id>"), ("cam 1 rig flyby path r.route y 3 speed fast track car:p1", "speed needs metres a second"),
        ("cam 1 rig flyby path r.route y 3 speed 200 track car:p1", "a speed from 0.5 to 120"), ("cam 1 rig flyby path r.route y 3 path o.route track car:p1", "'path' twice"),
        ("cam 1 rig flyby path r.route y 3 car:p1", "goes after track"),
        ("cam 1 rig hood", "rig hood: needs a car"), ("cam 1 rig roof car:p1 ahead 0", "rig roof: ahead 0.00 (1.00 to 1000.00 metres)"),
        ("cam 1 rig bumper car:p1 shift 9 0 0", "'shift' needs three numbers within 5.00"),
        ("cam 1 rig hood car:p1 height 3", "rig hood: unknown word 'height' (ahead, lag, shift, fov, roll, near, far, ease, car:<id>)"),
    ]
    for line, part in rig_bad:
        sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncar p2 spawn vp_x at 4,5,6\n" + line + "\nend 10\n")
        check_(has(sc, "error", 4, part), f"'{line}' -> error on line 4 saying '{part}'; got {[x.text('t') for x in sc.diags]}")
    rig_ok = [
        "cam 1 rig chase car:p1", "cam 1 rig chase car:p1 dist 7 height 2 side 1 lag 0.5 aim 1.2 ahead 3 fov 45 roll 2 ease inout", "cam 1 rig chase height 3 car:player fov 50 dist 9",
        "cam 1 rig orbit car:p1", "cam 1 rig orbit car:p1 r 8 az 20..200 over 6 el 10", "cam 1 rig orbit car:p2 r 5 az 0..90 el 5..30 over 4 at 0 1 0 fov 40 ease inout",
        "cam 1 rig tripod at 90 2 40 track car:p1 0 1 0", "cam 1 rig tripod at 1 2 3 track 0 0 100", "cam 1 rig tripod at 1 2 3 yaw 90 pitch -10 roll 3",
        "cam 1 rig tripod track car:p2 0 0.5 0 at 5 6 7 aimlag 0.5", "cam 1 rig hood car:p1", "cam 1 rig bumper car:p2 fov 70 near 0.3", "cam 1 rig roof car:p1 ahead 50 lag 0.2 shift 0 0.1 0",
    ]
    for line in rig_ok:
        sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncar p2 spawn vp_x at 4,5,6\n" + line + "\nend 10\n")
        check_(not sc.diags, f"'{line}' is clean; got {[x.text('t') for x in sc.diags]}")
    sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 1 rig chase car:p9\nend 10\n")
    check_(has(sc, "error", 3, "car 'p9' is used at t=1.00 but is never spawned"), "a rig's car must be spawned")
    sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 1 rig tripod at 1 2 3 track car:p1\nat 4 car p1 delete\ncam 6 rig chase car:p1\nend 10\n")
    check_(has(sc, "error", 5, "car p1 is used at t=6.00 after it was removed"), "a rig's car must still be there")
    # an orbit sweep is a second key `over` seconds on: it must be before the end, and no other camera key may be inside it
    sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 6 rig orbit car:p1 az 0..90 over 6\nend 10\n")
    check_(has(sc, "error", 3, "the cam key runs to t=12.00, past the scene's end (t=10.00)"), "a sweep past the end")
    sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 0 rig orbit car:p1 az 0..90 over 8\ncam 5 fov 30\nend 10\n")
    check_(has(sc, "warning", 4, "comes inside the orbit sweep of line 3 (t=0.00 to 8.00)"), "a camera key inside a sweep is a warning")
    sc = lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 0 rig orbit car:p1 az 0..90 over 4\ncam 5 fov 30\nend 10\n")
    check_(not sc.diags, f"a key after the sweep is fine; got {[x.text('t') for x in sc.diags]}")
    check_(describe([e for e in lint("scene t\ncar p1 spawn vp_x at 1,2,3\ncam 1 rig chase car:p1 dist 7\nend 10\n").sorted_events() if e.track == "cam"][0]) == "cam rig chase car:p1 dist 7", "describe keeps the rig's words")

    # a flyby's route: found beside the scene or in its routes folder, a route, with heights or a `y` on the key
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "routes").mkdir()
        (d / "high.route").write_text("0,0,10,5\n100,0,10,5\n", encoding="utf-8")
        (d / "routes" / "flat.route").write_text("0,0,10\n100,0,10\n", encoding="utf-8")
        (d / "bad.route").write_text("0,0\nx,3\n", encoding="utf-8")
        text = ("scene t\ncar p1 spawn vp_x at 1,2,3\n"
                "cam 1 rig flyby path high.route track car:p1\n"            # 3 fine: heights, up 2
                "cam 2 rig flyby path flat track car:p1\n"                  # 4 no heights, no y
                "cam 3 rig flyby path flat y 3 track car:p1\n"              # 5 fine
                "cam 4 rig flyby path flat up 1 track car:p1\n"             # 6 up with no heights
                "cam 5 rig flyby path high.route up 1 track car:p1\n"       # 7 fine
                "cam 6 rig flyby path missing.route y 3 track car:p1\n"     # 8 missing
                "cam 7 rig flyby path bad.route y 3 track car:p1\n"         # 9 not a route
                "end 20\n")
        sc = lint(text)
        check_routes(sc, d / "t.scene")
        check_(not any(x.line in (3, 5, 7) for x in sc.diags), f"a flyby route with heights (or a y) is fine; got {[x.text('t') for x in sc.diags]}")
        check_(has(sc, "error", 4, "route 'flat' has no heights (x,z,speed,y): give the camera's height in the world with y <metres>"), "a route with no heights and no y")
        check_(has(sc, "error", 6, "up is above the route's own height, and route 'flat' has none: use y <metres>"), "up on a route with no heights")
        check_(has(sc, "error", 8, "cam: rig flyby: route 'missing.route': no route file 'missing.route' (looked for"), "a missing flyby route")
        check_(has(sc, "error", 9, "route 'bad.route': line 2: 'x' is not a number"), "a flyby route that is not one")
    check_(route_height_state("0,0,10,5\n1,1,10,5\n") and not route_height_state("0,0,10\n1,1,10\n") and not route_height_state("0,0,10,5\n1,1,10\n"), "a route's heights: all, none or mixed")

    # the Director's settings: a scene may not set them (scene.cpp CvarRefusal)
    for name in ("mcla_director", "mcla_director_info", "mcla_director_root", "MCLA_DIRECTOR", "mcla_directorx"):
        check_(cvar_refusal(name).startswith(f"a scene does not set {name}"), f"a scene may not set {name}")
        check_(has(lint(f"scene t\nat 1 set {name} 1\nend 5\n", FAKE_SRC), "error", 2, f"a scene does not set {name}"), f"'set {name}' is an error on its line")
    check_(cvar_refusal("mcla_dire") == "" and cvar_refusal("direct_mode") == "", "names that only look like it are free")

    # the recipes: clean, with an end, their spots and routes exist, and every spot word of them is a numbers-only statement afterwards
    recipes = HERE / "scenes" / "recipes"
    for p in sorted(recipes.glob("*.scene")):
        spots, sp_problems = load_spots_for(p)
        check_(not sp_problems, f"{p.name}: its spots files read clean; got {sp_problems}")
        sc = lint(p.read_text(encoding="utf-8"), cvars, None, spots)
        check_routes(sc, p)
        check_(not sc.diags, f"recipe {p.name} is clean; got {[x.text(p.name) for x in sc.diags]}")
        check_(sc.end >= 0, f"recipe {p.name} has an end")
        check_(not any(is_spot_word(w) for e in sc.events for w in e.words), f"recipe {p.name}: no spot word is left unresolved")
        check_(any(e.track == "cam" and e.words[:1] == ["rig"] for e in sc.events), f"recipe {p.name} shows a rig")
    check_(len(list(recipes.glob("*.scene"))) >= 6, "the recipes are there (chase, orbit reveal, fly-by, night street, two-car pass, fixed-camera test, all rigs)")

    # Check diagnostics for game executables that predate Director syntax.
    tbl = parse_spots_text("hw-street 832.19 16.5 -787.53 62\nrace 1 2 3 90\n")[0]
    director_text = ("scene t\n"
                     "at 0 car player at spot:hw-street@-1.5,0,0 heading spot:hw-street\n"                  # 2 spot words
                     "cam 0 rig tripod at spot:hw-street@-5,1.4,10 track car:player 1.75 0.8 0 fov 50\n"    # 3 a rig, spot words
                     "cam 1 pos 1 2 3 look spot:hw-street fov 50\n"                                          # 4 a spot word in a look
                     "at 2 car t1 spawn vp_x at spot:hw-street\n"                                            # 5 a spawn at a spot
                     "at 3 car t2 spawn vp_x near player 3.5,0.3,0\n"                                        # 6 main's words
                     "cam 4 pos 1 2 3 look 4 5 6 fov 50\n"                                                   # 7 main's words
                     "at 5 car player at 832.89,16.5,-788.85 heading 62\n"                                   # 8 main's words
                     "end 9\n")
    sc = lint(director_text, None, None, tbl, "legacy")
    check_(all(has(sc, "error", n, "Director word") for n in (2, 3, 4, 5)) and not any(x.line in (6, 7, 8) for x in sc.diags), f"target legacy: the Director's words are errors, ordinary keys are accepted; got {[x.text('t') for x in sc.diags]}")
    check_(has(sc, "error", 2, "car player at 832.894 16.5 -788.854 heading 62"), "target legacy: the message gives the statement with the spot's numbers")
    check_(has(sc, "error", 3, "'rig' is a Director word") and has(sc, "error", 3, "'spot:hw-street@-5,1.4,10'"), "target legacy: a rig line says both the rig and its spot word")
    check_(has(sc, "error", 4, "cam pos 1 2 3 look 832.19 16.5 -787.53 fov 50"), "target legacy: a spot in a look is shown as numbers")
    sc = lint(director_text, None, None, tbl, "director")
    check_(not sc.diags, f"target director: the same text is clean; got {[x.text('t') for x in sc.diags]}")
    sc = lint(director_text, None, None, None, "legacy")
    check_(has(sc, "error", 2, "a spot word is a Director word") and not has(sc, "error", 2, "for these spots"), "target legacy with no spot table: still refused, no numbers offered")
    DEFAULT_TARGET = "legacy"
    check_(has(lint(director_text, None, None, tbl), "error", 2, "Director word") and not lint(director_text, None, None, tbl, "director").diags, "target None = DEFAULT_TARGET (the default)")
    DEFAULT_TARGET = "director"
    check_(resolve_target("LEGACY") == "legacy" and resolve_target() == "director", "the target names")
    try:
        resolve_target("nope")
        check_(False, "an unknown target is an error")
    except ValueError:
        check_(True, "an unknown target is an error")
    check_(combine_targets("legacy", "director") == "legacy" and combine_targets(None, "director") == "director" and combine_targets() == "legacy" and combine_targets("director", None, "legacy") == "legacy",
           "several exes: one without the Director's words makes the target legacy")
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "main.exe").write_bytes(b"MZ" + b"\0" * 100 + b"cam: '%s' needs three numbers (x y z)\0")
        (d / "dir.exe").write_bytes(b"MZ" + b"\0" * 100 + b"unknown spot '%s'\0a spot name is letters, digits and _ - . (spot:hw-street)\0")
        (d / "dir2.exe").write_bytes(b"MZ\0\0cam: unknown rig '%s'\0")
        (d / "empty.exe").write_bytes(b"")
        check_(exe_target(d / "main.exe") == "legacy" and exe_target(d / "dir.exe") == "director" and exe_target(d / "dir2.exe") == "director" and exe_target(d / "empty.exe") == "legacy",
               "an exe's target is read from its Director messages")
        (d / "bad.scene").write_text(director_text, encoding="utf-8")
        (d / "spots.txt").write_text("hw-street 832.19 16.5 -787.53 62\n", encoding="utf-8")
        import contextlib
        import io

        def cli(*argv):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return main(list(argv) + [str(d / "bad.scene")])
        check_(cli("--target", "legacy") == 1 and cli("--target", "director") == 0 and cli() == 0, "scene_check --target legacy refuses the Director's words, --target director and the default take them")
        check_(cli("--exe", str(d / "main.exe")) == 1 and cli("--exe", str(d / "dir.exe")) == 0 and cli("--exe", str(d / "dir.exe"), "--exe", str(d / "main.exe")) == 1
               and cli("--target", "director", "--exe", str(d / "main.exe")) == 1 and cli("--exe", str(d / "none.exe")) == 2, "scene_check --exe takes the target from the exe(s)")
    DEFAULT_TARGET = saved_target

    for f in failures:
        print("FAILED:", f)
    print(f"{count[0]} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
