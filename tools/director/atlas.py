#!/usr/bin/env python3
"""Build and query a local atlas of roads and lights from the player's files.

Use atlas build --game DIR with the folder containing xarchive_cache.rpf.
The generated JSON stays in the user's atlas folder. Live atlas.dump output
can also be opened with --atlas DIR or copied into the atlas with adopt.

Every answer has the same shape as the game's `query.*` command: {"ok": true, "result": {...}} or {"ok": false, "error": {"code","msg"}}.
Coordinates are the game's: x east, y up, z south (a point is [x, y, z]; a 2-D point [x, z]); heading in degrees, 0 = +Z, 90 = +X (the
scene convention); speeds in m/s.

API:
  load(path=None) -> Atlas
  near_lights(atlas, pos, r=60, n=20, min_intensity=0, kind=None)
  nearest_road(atlas, pos, r=40, n=6, heading=None)
  route(atlas, a, b, via=(), speed=None)
  ground(atlas, points)
  spots(path) -> {name: {pos, heading, text}} ; resolve(atlas_or_spots, "spot:gate@1,0,5")
  query(atlas, name, args, spot_table=None) -> {ok, result|error}      (name = lights|road|route|ground|garages|cars|spots)

CLI:  python atlas.py build --game <folder> [--out DIR] [--src SRC] | adopt <folder> | compare A B | info | query NAME [JSON] | streets | route A B | selftest
"""
import argparse
import heapq
import json
import math
import os
import re
import struct
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ATLAS_ROOT = Path.home() / ".larecomp" / "director" / "atlas"

CELL = 64.0  # metres: the grid of the spatial indices
MAX_SPEED = 30.0  # m/s: the A* heuristic (the fastest road is 29.06)
TURN_SPEED = 9.0  # m/s through an intersection piece when the road gives none
LAT_ACCEL = 5.0  # m/s^2: the curve speed limit v = sqrt(LAT_ACCEL * R)
ACCEL, DECEL = 3.5, 6.0  # m/s^2: the speed profile of a route


def ok(result):
    return {"ok": True, "result": result}


def fail(code, msg):
    return {"ok": False, "error": {"code": code, "msg": msg}}


# ------------------------------------------------------------------------------------------------------------------ the atlas
class Atlas:
    def __init__(self, path=None):
        self.path = path
        self.lights = []
        self.nodes = []
        self.rails = []
        self.garages = []
        self.cars = []
        self.meta = {}
        self._rgrid = None
        self._lgrid = None

    # -- loading
    @classmethod
    def load(cls, path=None):
        a = cls(None)
        path = path or default_atlas_dir()
        if not path or not os.path.isdir(path):
            raise FileNotFoundError("no atlas folder (build one: atlas.py build --game <folder>): %s" % path)
        a.path = path

        def rd(name, key):
            p = os.path.join(path, name)
            if os.path.isfile(p):
                with open(p, encoding="utf-8") as f:
                    return json.load(f).get(key, [])
            return []

        a.lights = rd("lights.json", "lights")
        a.garages = rd("garages.json", "garages")
        a.cars = rd("cars.json", "cars")
        p = os.path.join(path, "rails.json")
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            a.nodes, a.rails = d.get("nodes", []), d.get("rails", [])
        p = os.path.join(path, "meta.json")
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                a.meta = json.load(f)
        return a

    @classmethod
    def from_data(cls, lights=None, railnet=None, garages=None, cars=None):
        a = cls(None)
        a.lights = lights or []
        if railnet:
            a.nodes, a.rails = railnet["nodes"], railnet["rails"]
        a.garages = garages or []
        a.cars = cars or []
        return a

    # -- spatial indices
    def rail_grid(self):
        if self._rgrid is None:
            g = {}
            for r in self.rails:
                pts = r["pts"]
                for s in range(len(pts) - 1):
                    x0, z0, x1, z1 = pts[s][0], pts[s][2], pts[s + 1][0], pts[s + 1][2]
                    for cx in range(int(math.floor(min(x0, x1) / CELL)), int(math.floor(max(x0, x1) / CELL)) + 1):
                        for cz in range(int(math.floor(min(z0, z1) / CELL)), int(math.floor(max(z0, z1) / CELL)) + 1):
                            g.setdefault((cx, cz), []).append((r["id"], s))
            self._rgrid = g
        return self._rgrid

    def light_grid(self):
        if self._lgrid is None:
            g = {}
            for l in self.lights:
                g.setdefault((int(math.floor(l["pos"][0] / CELL)), int(math.floor(l["pos"][2] / CELL))), []).append(l["i"] if "i" in l else 0)
            self._lgrid = g
        return self._lgrid

    def speed_of(self, rail):
        nd = self.nodes[rail["node"]] if 0 <= rail.get("node", -1) < len(self.nodes) else None
        if nd and nd.get("speed"):
            return nd["speed"]
        return TURN_SPEED

    def name_of(self, rail):
        nd = self.nodes[rail["node"]] if 0 <= rail.get("node", -1) < len(self.nodes) else None
        return (nd or {}).get("name", "")


def default_atlas_dir():
    lat = os.path.join(ATLAS_ROOT, "latest.txt")
    if os.path.isfile(lat):
        name = open(lat, encoding="utf-8").read().strip()
        p = os.path.join(ATLAS_ROOT, name)
        if os.path.isdir(p):
            return p
    return None


def load(path=None):
    return Atlas.load(path)


# ------------------------------------------------------------------------------------------------------------------ geometry
def _xz(p):
    """[x, z] from [x, y, z], [x, z], {"x","z"}; None when it is none of them."""
    if isinstance(p, dict) and "x" in p and "z" in p:
        return float(p["x"]), float(p["z"])
    if isinstance(p, (list, tuple)):
        if len(p) == 3:
            return float(p[0]), float(p[2])
        if len(p) == 2:
            return float(p[0]), float(p[1])
    return None


def _y(p):
    if isinstance(p, dict):
        return float(p["y"]) if "y" in p else None
    if isinstance(p, (list, tuple)) and len(p) == 3:
        return float(p[1])
    return None


def heading_deg(dx, dz):
    return (math.degrees(math.atan2(dx, dz)) + 360.0) % 360.0


def _seg_project(px, pz, a, b):
    """closest point of the segment a-b (3-D points, horizontal distance) to (px, pz): (dist_xz, u in 0..1, point)."""
    dx, dz = b[0] - a[0], b[2] - a[2]
    l2 = dx * dx + dz * dz
    u = 0.0 if l2 < 1e-12 else max(0.0, min(1.0, ((px - a[0]) * dx + (pz - a[2]) * dz) / l2))
    qx, qz = a[0] + u * dx, a[2] + u * dz
    return math.hypot(px - qx, pz - qz), u, [qx, a[1] + u * (b[1] - a[1]), qz]


def _cum(pts):
    c = [0.0]
    for a, b in zip(pts, pts[1:]):
        c.append(c[-1] + math.dist(a, b))
    return c


def _candidates(atlas, px, pz, r):
    g = atlas.rail_grid()
    seen = set()
    c0x, c1x = int(math.floor((px - r) / CELL)), int(math.floor((px + r) / CELL))
    c0z, c1z = int(math.floor((pz - r) / CELL)), int(math.floor((pz + r) / CELL))
    for cx in range(c0x, c1x + 1):
        for cz in range(c0z, c1z + 1):
            for item in g.get((cx, cz), ()):
                if item not in seen:
                    seen.add(item)
                    yield item


def project_on_rails(atlas, pos, r=40.0, heading=None, max_heading_err=75.0):
    """The nearest points on rails to pos, one per rail, closest first: [{rail, dist, u, seg, point, hdg}]. With a heading, rails more
    than max_heading_err degrees off it are skipped (a car on the wrong side of a road is not on that lane)."""
    xz = _xz(pos)
    if xz is None:
        return []
    px, pz = xz
    py = _y(pos)
    best = {}
    for rid, s in _candidates(atlas, px, pz, r):
        pts = atlas.rails[rid]["pts"]
        d, u, q = _seg_project(px, pz, pts[s], pts[s + 1])
        if d > r:
            continue
        dd = math.hypot(d, (q[1] - py)) if py is not None else d
        h = heading_deg(pts[s + 1][0] - pts[s][0], pts[s + 1][2] - pts[s][2])
        if heading is not None:
            e = abs((h - heading + 180.0) % 360.0 - 180.0)
            if e > max_heading_err:
                continue
        if rid not in best or dd < best[rid]["dist"]:
            best[rid] = {"rail": rid, "dist": dd, "u": u, "seg": s, "point": q, "hdg": h}
    return sorted(best.values(), key=lambda x: x["dist"])


def _t_along(atlas, rid, seg, u):
    pts = atlas.rails[rid]["pts"]
    c = _cum(pts)
    total = c[-1] or 1.0
    return (c[seg] + u * (c[seg + 1] - c[seg])) / total


# ------------------------------------------------------------------------------------------------------------------ queries
def near_lights(atlas, pos, r=60.0, n=20, min_intensity=0.0, kind=None, src=None):
    xz = _xz(pos)
    if xz is None:
        return fail("bad_args", "near must be [x,y,z], [x,z] or {x,z}")
    px, pz = xz
    py = _y(pos)
    g = atlas.light_grid()
    hits = []
    total = 0
    c0x, c1x = int(math.floor((px - r) / CELL)), int(math.floor((px + r) / CELL))
    c0z, c1z = int(math.floor((pz - r) / CELL)), int(math.floor((pz + r) / CELL))
    for cx in range(c0x, c1x + 1):
        for cz in range(c0z, c1z + 1):
            for i in g.get((cx, cz), ()):
                l = atlas.lights[i]
                dx, dz = l["pos"][0] - px, l["pos"][2] - pz
                d = math.hypot(dx, dz) if py is None else math.sqrt(dx * dx + dz * dz + (l["pos"][1] - py) ** 2)
                if d > r or l["intensity"] < min_intensity or (kind and l["type"] != kind) or (src and l.get("src", "file") != src):
                    continue
                total += 1
                hits.append((d, i))
    hits.sort()
    out = []
    for d, i in hits[:max(0, int(n))]:
        l = dict(atlas.lights[i])
        l["dist"] = round(d, 2)
        l["reach_m"] = round(math.sqrt(100.0 * max(0.0, l["intensity"])), 1)
        out.append(l)
    return ok({"count": len(out), "in_radius": total, "lights": out})


def nearest_road(atlas, pos, r=40.0, n=6, heading=None):
    if not atlas.rails:
        return fail("no_world", "this atlas has no rails.json")
    if _xz(pos) is None:
        return fail("bad_args", "pos must be [x,y,z], [x,z] or {x,z}")
    res = []
    for h in project_on_rails(atlas, pos, r, heading)[:max(1, int(n))]:
        rail = atlas.rails[h["rail"]]
        res.append({"rail": rail["id"], "node": rail["node"], "kind": rail["kind"], "name": atlas.name_of(rail),
                    "speed": round(atlas.speed_of(rail), 2), "dir": rail.get("dir"), "dist_m": round(h["dist"], 2),
                    "t": round(_t_along(atlas, rail["id"], h["seg"], h["u"]), 4), "pos": [round(v, 2) for v in h["point"]],
                    "heading_deg": round(h["hdg"], 1), "len_m": rail["len"], "next": rail["next"]})
    return ok({"on_road": bool(res) and res[0]["dist_m"] <= 6.0, "nearest": res})


def ground(atlas, points):
    """Offline ground height: the nearest road control height within 25 m (approximate: roads follow the ground, a hill beside one does not)."""
    single = isinstance(points, dict)
    if single:
        points = [points]
    if not points:
        return fail("bad_args", "points: [[x,z],...] or {x,z}")
    out = []
    for p in points:
        xz = _xz(p)
        if xz is None:
            return fail("bad_args", "point %r" % (p,))
        best = None
        for h in project_on_rails(atlas, [xz[0], xz[1]], 25.0)[:1]:
            best = {"x": xz[0], "z": xz[1], "y": round(h["point"][1], 2), "source": "rails", "approx": True, "dist_m": round(h["dist"], 2)}
        out.append(best or {"x": xz[0], "z": xz[1], "y": None, "source": "rails", "approx": True, "why": "no road within 25 m"})
    return ok(out[0] if single else {"points": out})


# ------------------------------------------------------------------------------------------------------------------ routing
def _endpoint(atlas, spec, spot_table, heading=None):
    """-> (rail id, seg, u, point, label) of a route end: a position, a spot name, or {"rail": id, "t": t}."""
    if isinstance(spec, str):
        p = resolve(spot_table or {}, spec)
        if p is None:
            raise ValueError("unknown spot %r" % spec)
        spec = p["pos"]
        heading = p.get("heading") if heading is None else heading
    if isinstance(spec, dict) and "rail" in spec:
        rid = int(spec["rail"])
        pts = atlas.rails[rid]["pts"]
        c = _cum(pts)
        want = float(spec.get("t", 0.0)) * (c[-1] or 1.0)
        for s in range(len(pts) - 1):
            if want <= c[s + 1] or s == len(pts) - 2:
                u = 0.0 if c[s + 1] == c[s] else min(1.0, max(0.0, (want - c[s]) / (c[s + 1] - c[s])))
                q = [pts[s][k] + u * (pts[s + 1][k] - pts[s][k]) for k in range(3)]
                return rid, s, u, q, 0.0
    hits = project_on_rails(atlas, spec, 80.0, heading)
    if not hits:
        hits = project_on_rails(atlas, spec, 400.0)
    if not hits:
        raise ValueError("no road near %r" % (spec,))
    h = hits[0]
    return h["rail"], h["seg"], h["u"], h["point"], h["dist"]


def _rail_slice(rail, seg0, u0, seg1, u1):
    """polyline of a rail from (seg0,u0) to (seg1,u1) along its direction."""
    pts = rail["pts"]

    def at(s, u):
        return [pts[s][k] + u * (pts[s + 1][k] - pts[s][k]) for k in range(3)]

    out = [at(seg0, u0)]
    if seg0 == seg1:
        out.append(at(seg1, u1))
    else:
        out += [list(p) for p in pts[seg0 + 1:seg1 + 1]]
        out.append(at(seg1, u1))
    return out


def _search(atlas, start_rails, goal_rail, goal_pos_t):
    """Dijkstra/A* over rails. start_rails: {rail: initial cost}. Returns the rail id list start..goal, or None."""
    rails = atlas.rails
    gp = rails[goal_rail]["pts"][len(rails[goal_rail]["pts"]) // 2]
    heap = []
    cost = {}
    prev = {}
    for rid, c in start_rails.items():
        cost[rid] = c
        heapq.heappush(heap, (c + math.dist(rails[rid]["pts"][-1], gp) / MAX_SPEED, c, rid))
    while heap:
        _, c, rid = heapq.heappop(heap)
        if c > cost.get(rid, 1e18):
            continue
        if rid == goal_rail:
            path = [rid]
            while path[-1] in prev:
                path.append(prev[path[-1]])
            return path[::-1]
        for nx in rails[rid]["next"]:
            nc = c + rails[nx]["len"] / max(1.0, atlas.speed_of(rails[nx]))
            if nc < cost.get(nx, 1e18):
                cost[nx] = nc
                prev[nx] = rid
                heapq.heappush(heap, (nc + math.dist(rails[nx]["pts"][-1], gp) / MAX_SPEED, nc, nx))
    return None


def _leg(atlas, a, b):
    """one leg between two projected ends: (list of (polyline, speed limit)) or an error string."""
    ra, sa, ua, pa, _ = a
    rb, sb, ub, pb, _ = b
    rails = atlas.rails
    ta, tb = (sa, ua), (sb, ub)
    pieces = []
    if ra == rb and (tb[0], tb[1]) >= (ta[0], ta[1]):
        pieces.append((ra, _rail_slice(rails[ra], sa, ua, sb, ub)))
        return pieces
    # leave the start rail at its end, enter the goal rail at its start
    c0 = {nx: rails[nx]["len"] / max(1.0, atlas.speed_of(rails[nx])) for nx in rails[ra]["next"]}
    if not c0 and ra != rb:
        return "no road continues from rail %d" % ra
    path = _search(atlas, c0, rb, None)
    if path is None:
        return "no drivable way from rail %d to rail %d" % (ra, rb)
    pa_pts = rails[ra]["pts"]
    pieces.append((ra, _rail_slice(rails[ra], sa, ua, len(pa_pts) - 2, 1.0)))
    for rid in path[:-1]:
        pieces.append((rid, [list(p) for p in rails[rid]["pts"]]))
    pieces.append((rb, _rail_slice(rails[rb], 0, 0.0, sb, ub)))
    return pieces


def _profile(points, speeds, cap):
    """curve limit, then an acceleration/deceleration pass: speeds in m/s per point."""
    n = len(points)
    v = [min(s, cap) for s in speeds]
    for i in range(1, n - 1):
        a, b, c = points[i - 1], points[i], points[i + 1]
        ab, bc, ac = math.dist(a, b), math.dist(b, c), math.dist(a, c)
        if ab < 0.05 or bc < 0.05 or ac < 0.05:
            continue
        # Menger curvature = 4 * area / (ab * bc * ac)
        s = (ab + bc + ac) / 2.0
        area2 = max(0.0, s * (s - ab) * (s - bc) * (s - ac))
        k = 4.0 * math.sqrt(area2) / (ab * bc * ac)
        if k > 1e-4:
            v[i] = min(v[i], max(3.0, math.sqrt(LAT_ACCEL / k)))
    for i in range(1, n):
        d = math.dist(points[i - 1], points[i])
        v[i] = min(v[i], math.sqrt(v[i - 1] ** 2 + 2.0 * ACCEL * d))
    for i in range(n - 2, -1, -1):
        d = math.dist(points[i], points[i + 1])
        v[i] = min(v[i], math.sqrt(v[i + 1] ** 2 + 2.0 * DECEL * d))
    return v


def route(atlas, a, b, via=(), speed=None, spot_table=None, heading=None):
    """A drivable route along the lane graph. a, b, via: a position, a spot name or {"rail","t"}. speed: a cap in m/s (default: the roads' limits).
    -> {points:[[x,z,speed,y]], rails:[ids], length_m, duration_s, start:{rail,dist_m}, end:{rail,dist_m}, names:[...]}"""
    if not atlas.rails:
        return fail("no_world", "this atlas has no rails.json")
    try:
        ends = [_endpoint(atlas, a, spot_table, heading)] + [_endpoint(atlas, v, spot_table) for v in via] + [_endpoint(atlas, b, spot_table)]
    except (ValueError, KeyError, IndexError) as ex:
        return fail("bad_args", str(ex))
    pieces = []
    for e0, e1 in zip(ends, ends[1:]):
        leg = _leg(atlas, e0, e1)
        if isinstance(leg, str):
            return fail("failed", leg)
        pieces += leg
    pts, lim, rail_ids = [], [], []
    for rid, poly in pieces:
        rail_ids.append(rid)
        v = atlas.speed_of(atlas.rails[rid])
        for p in poly:
            if pts and math.dist(pts[-1], p) < 0.05:
                continue
            pts.append(p)
            lim.append(v)
    if len(pts) < 2:
        return fail("failed", "the route has no length")
    cap = float(speed) if speed else MAX_SPEED
    v = _profile(pts, lim, cap)
    length = sum(math.dist(p, q) for p, q in zip(pts, pts[1:]))
    t = sum(math.dist(pts[i], pts[i + 1]) / max(0.5, 0.5 * (v[i] + v[i + 1])) for i in range(len(pts) - 1))
    names = []
    for rid in rail_ids:
        nm = atlas.name_of(atlas.rails[rid])
        if nm and (not names or names[-1] != nm):
            names.append(nm)
    return ok({"points": [[round(p[0], 2), round(p[2], 2), round(s, 2), round(p[1], 2)] for p, s in zip(pts, v)],
               "rails": [r for i, r in enumerate(rail_ids) if i == 0 or rail_ids[i - 1] != r], "length_m": round(length, 1),
               "duration_s": round(t, 1), "start": {"rail": ends[0][0], "dist_m": round(ends[0][4], 2)},
               "end": {"rail": ends[-1][0], "dist_m": round(ends[-1][4], 2)}, "names": names})


def route_file_text(result, header=""):
    """The `.route` text of a route() result (x,z,speed,y), a comment header on top."""
    lines = ["# " + h for h in header.splitlines()] if header else []
    for p in result["points"]:
        lines.append("%.2f,%.2f,%.2f,%.2f" % tuple(p))
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------------------------------------------------------ spots
_SPOT_RE = re.compile(r"^\s*([A-Za-z_][\w.\-]*)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?\s*(?:#\s*(.*))?$")


def spots(path):
    """spots.txt: `name x y z [heading]  # text` -> {name: {pos:[x,y,z], heading, text}}. Blank and `#` lines are skipped."""
    out = {}
    if not path or not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = _SPOT_RE.match(line)
            if m:
                out[m.group(1)] = {"pos": [float(m.group(2)), float(m.group(3)), float(m.group(4))],
                                   "heading": float(m.group(5)) if m.group(5) is not None else 0.0, "text": (m.group(6) or "").strip()}
    return out


def resolve(table, text):
    """`spot:name[@r,u,f]` (offset right, up, forward in the spot's heading frame) -> {pos, heading, text} or None."""
    m = re.match(r"^(?:spot:)?([\w.\-]+)(?:@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+))?$", text.strip())
    if not m or m.group(1) not in table:
        return None
    s = table[m.group(1)]
    p = list(s["pos"])
    if m.group(2) is not None:
        r, u, f = float(m.group(2)), float(m.group(3)), float(m.group(4))
        h = math.radians(s["heading"])
        fx, fz = math.sin(h), math.cos(h)  # forward; right = (-cos h, 0, sin h): camera.cpp HeadingBasis (forward x up)
        p = [p[0] + f * fx - r * fz, p[1] + u, p[2] + f * fz + r * fx]
    return {"pos": p, "heading": s["heading"], "text": s.get("text", "")}


def default_spots_path():
    return os.path.join(HERE, "scenes", "spots.txt")


# ------------------------------------------------------------------------------------------------------------------ the dispatcher
def query(atlas, name, args=None, spot_table=None):
    """The `query.<name>` answer of the atlas. args: the command's arguments (see the notes, section 2)."""
    args = args or {}
    name = name[6:] if name.startswith("query.") else name
    if spot_table is None:
        spot_table = spots(default_spots_path())

    def pos_arg(key="near"):
        v = args.get(key, args.get("pos"))
        if isinstance(v, str):
            s = resolve(spot_table, v)
            return s["pos"] if s else None
        return v

    if name == "lights":
        p = pos_arg("near")
        if p is None:
            return fail("bad_args", "near: [x,y,z] | spot:name")
        return near_lights(atlas, p, float(args.get("r", 60)), int(args.get("n", 20)), float(args.get("min_intensity", 0)), args.get("kind"), args.get("src"))
    if name == "road":
        p = pos_arg("pos")
        if p is None:
            return fail("bad_args", "pos: [x,y,z] | {x,z} | spot:name")
        return nearest_road(atlas, p, float(args.get("r", 40)), int(args.get("n", 6)), args.get("heading"))
    if name == "route":
        if "from" not in args or "to" not in args:
            return fail("bad_args", "from and to are needed")
        return route(atlas, args["from"], args["to"], args.get("via", ()), args.get("speed"), spot_table, args.get("heading"))
    if name == "ground":
        pts = args.get("points") or ({"x": args["x"], "z": args["z"]} if "x" in args and "z" in args else None)
        return ground(atlas, pts)
    if name == "garages":
        return ok({"garages": atlas.garages})
    if name == "cars":
        s = str(args.get("search", "")).lower()
        return ok({"cars": [c for c in atlas.cars if s in c["name"].lower()]})
    if name == "spots":
        near = pos_arg("near")
        out = []
        for k, v in sorted(spot_table.items()):
            d = math.dist(v["pos"], near) if near is not None else None
            if d is None or d <= float(args.get("r", 1e9)):
                out.append(dict(name=k, **v, **({"dist": round(d, 2)} if d is not None else {})))
        return ok({"spots": out})
    return fail("unknown_cmd", "query.%s" % name)


# ------------------------------------------------------------------------------------------------------------------ build
def output_root(path):
    root = Path(path or ATLAS_ROOT).expanduser().resolve()
    checkout = Path(__file__).resolve().parents[2]
    if root.is_relative_to(checkout):
        raise ValueError("choose an atlas output directory outside the LARecomp checkout")
    return root


def content_hash(n_lights, rails):
    """12 hex digits of FNV-1a (64 bit) over "ATLAS1", the light and rail counts (u32 LE) and every rail's control vertex indices (u16 count, u16 each,
    LE): the same bytes the game's atlas.dump hashes (atlas_core.cpp ContentHash), so one city has one folder name offline and live."""
    h = 1469598103934665603
    stream = bytearray(b"ATLAS1")
    stream += struct.pack("<II", n_lights, len(rails))
    for r in rails:
        stream += struct.pack("<H", len(r["cv"]))
        stream += struct.pack("<%dH" % len(r["cv"]), *r["cv"])
    for b in stream:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def write_atlas(out_root, lights, railnet, source, garages=None, cars=None):
    garages = garages or []
    n_lamps = sum(1 for l in lights if l.get("src") == "lamp")
    name = content_hash(len(lights) - n_lamps, railnet["rails"])[:12]  # the file's lights only: the same folder name as the live dump's
    d = os.path.join(out_root, name)
    os.makedirs(d, exist_ok=True)

    def wr(fn, obj):
        with open(os.path.join(d, fn), "w", encoding="utf-8") as f:
            json.dump(obj, f, separators=(",", ":"))

    wr("lights.json", {"lights": lights})
    wr("rails.json", {"nodes": railnet["nodes"], "rails": railnet["rails"]})
    wr("garages.json", {"garages": garages})
    if cars:
        wr("cars.json", {"cars": cars})
    counts = {"lights": len(lights), "rails": len(railnet["rails"]), "nodes": len(railnet["nodes"]), "garages": len(garages), "cars": len(cars or [])}
    if n_lamps:
        counts["lamps"] = n_lamps  # as the live dump's meta: "lights" counts the file's lights and the lamps
    wr("meta.json", {"version": 1, "source": source, "hash": name, "counts": counts})
    with open(os.path.join(out_root, "latest.txt"), "w", encoding="utf-8") as f:
        f.write(name)
    return d


def cmd_build(args):
    import atlas_game as g
    root = output_root(args.out)
    key = g.find_rpf_key([args.src] if args.src else [])
    lights, rn = g.build_from_game(args.game, key)
    d = write_atlas(root, lights, rn, "game files (offline)")
    n_lamps = sum(1 for l in lights if l.get("src") == "lamp")
    print("atlas written: %s (%d lights + %d street lamps, %d rails, %d nodes)" % (d, len(lights) - n_lamps, n_lamps, len(rn["rails"]), len(rn["nodes"])))
    return 0


def adopt(folder, out_root=None):
    """Copy a live atlas dump to the user atlas directory and make it current."""
    import shutil
    out_root = out_root or ATLAS_ROOT
    folder = os.path.normpath(folder)
    if not os.path.isfile(os.path.join(folder, "meta.json")):
        raise FileNotFoundError("not an atlas folder (no meta.json): %s" % folder)
    name = os.path.basename(folder)
    dest = os.path.join(out_root, name)
    os.makedirs(dest, exist_ok=True)
    if os.path.realpath(folder) != os.path.realpath(dest):
        for fn in ("meta.json", "lights.json", "rails.json", "garages.json", "cars.json"):
            source = Path(folder) / fn
            if source.is_file():
                shutil.copy2(source, Path(dest) / fn)
    with open(os.path.join(out_root, "latest.txt"), "w", encoding="utf-8") as f:
        f.write(name)
    return dest


def compare(a, b):
    """Compare a live dump to the offline atlas. -> (ok, [lines]). Rails are compared by control vertex lists and links, lights by position
    and type, to the rounding the files carry."""
    lines = []
    good = True

    def note(name, cond, extra=""):
        nonlocal good
        lines.append(("ok   " if cond else "FAIL ") + name + (" " + extra if extra else ""))
        good = good and cond

    note("rail count", len(a.rails) == len(b.rails), "%d / %d" % (len(a.rails), len(b.rails)))
    note("node count", len(a.nodes) == len(b.nodes), "%d / %d" % (len(a.nodes), len(b.nodes)))
    # The street lamps (src "lamp") are the game's own second list; the offline atlas decodes them from the props (atlas_game.read_lamps), an older offline atlas has none. The file's lights
    # are compared file list against file list; the lamps are compared when both sides have them, and a lamp-only side is reported, not failed.
    a_lamps = [l for l in a.lights if l.get("src") == "lamp"]
    b_lamps = [l for l in b.lights if l.get("src") == "lamp"]
    a_lights = [l for l in a.lights if l.get("src") != "lamp"]
    b_lights = [l for l in b.lights if l.get("src") != "lamp"]
    note("light count (the file's lights)", len(a_lights) == len(b_lights), "%d / %d" % (len(a_lights), len(b_lights)))
    both_lamps = bool(a_lamps) and bool(b_lamps)
    if (a_lamps or b_lamps) and not both_lamps:
        lines.append("info street lamps (one side has none: %s): %d / %d" % ("live only" if b_lamps else "offline only", len(a_lamps), len(b_lamps)))
    if len(a.rails) == len(b.rails):
        bad_cv = sum(1 for x, y in zip(a.rails, b.rails) if x["cv"] != y["cv"] or x["next"] != y["next"])
        note("rails: control vertices and links equal", bad_cv == 0, "(%d differ)" % bad_cv if bad_cv else "")
        far = 0
        for x, y in zip(a.rails, b.rails):
            if len(x["pts"]) != len(y["pts"]) or any(abs(p - q) > 0.05 for u, v in zip(x["pts"], y["pts"]) for p, q in zip(u, v)):
                far += 1
        note("rails: polylines equal to 5 cm", far == 0, "(%d differ)" % far if far else "")
        bad_n = sum(1 for x, y in zip(a.nodes, b.nodes) if x.get("name") != y.get("name") or abs((x.get("speed") or 0) - (y.get("speed") or 0)) > 0.01)
        note("nodes: names and speeds equal", bad_n == 0, "(%d differ)" % bad_n if bad_n else "")
    if len(a_lights) == len(b_lights):
        # A live dump's lights went through the game's own pack (the running game keeps them as 24-byte records, positions as 16-bit steps of the grid's extent: 0.1 m in x and 0.07 m in z,
        # colours and intensities as half floats), so a position is equal to 0.15 m, not to the file's rounding. The pack also CLAMPS a position to the box of its steps: the first full
        # compare (11-21) had 295 lights differ, every one of them a light the file puts beyond the box (294 above its top, y 80 to 377 m, kept at y 78.998 by the game; one west of its edge,
        # x -3467.953 kept at -3467.000). Those are the game's own lights as the game sees them, not a misread: see lights_equal.
        bad_l, clamped, worst, edges = lights_equal(a_lights, b_lights)
        extra = "(%d differ)" % bad_l if bad_l else "(largest other position difference %.3f m" % worst
        if not bad_l:
            extra += "; %d kept at the pack's box edge: %s)" % (clamped, ", ".join(sorted(edges)) or "none") if clamped else ")"
        note("lights: positions (to 0.15 m, or clamped to the pack's box edge) and types equal", bad_l == 0, extra)
    if both_lamps:
        note("street lamp count", len(a_lamps) == len(b_lamps), "%d / %d" % (len(a_lamps), len(b_lamps)))
        if len(a_lamps) == len(b_lamps):
            # the lamps go through the same pack: the box is the live set's (the file's lights and the lamps together)
            bad_p, clamped, worst, edges = lights_equal(a_lamps, b_lamps, box_from=b_lights + b_lamps)
            extra = "(%d differ)" % bad_p if bad_p else "(largest other position difference %.3f m" % worst
            if not bad_p:
                extra += "; %d kept at the pack's box edge: %s)" % (clamped, ", ".join(sorted(edges)) or "none") if clamped else ")"
            note("street lamps: positions (to 0.15 m, or clamped to the pack's box edge) and types equal", bad_p == 0, extra)
            bad_f, worst_f = lamps_fields_differ(a_lamps, b_lamps)
            note("street lamps: colour, direction, intensity, cone, on equal (half float rounding)", bad_f == 0,
                 "(%d differ)" % bad_f if bad_f else "(largest: colour %.4f, direction %.4f, intensity %.3f, cone %.2f)" % (worst_f["color"], worst_f["dir"], worst_f["intensity"], worst_f["cone"]))
    return good, lines


def lamps_fields_differ(a_lamps, b_lamps, tol_color=0.002, tol_dir=0.002, tol_val=0.01):
    """-> (number of lamps whose colour, direction, intensity, cone or `on` differ, the largest differences). The live values went through the game's pack (half floats: a colour or
    direction component is off by up to 0.0005), so the tolerances are a little above that; intensity and cone are exact."""
    worst = {"color": 0.0, "dir": 0.0, "intensity": 0.0, "cone": 0.0}
    bad = 0
    for x, y in zip(a_lamps, b_lamps):
        d = {"color": max(abs(p - q) for p, q in zip(x["color"][:3], y["color"][:3])), "dir": max(abs(p - q) for p, q in zip(x["dir"][:3], y["dir"][:3])),
             "intensity": abs(x["intensity"] - y["intensity"]), "cone": abs(x["cone"] - y["cone"])}
        for k, v in d.items():
            worst[k] = max(worst[k], v)
        if d["color"] > tol_color or d["dir"] > tol_dir or d["intensity"] > tol_val or d["cone"] > tol_val or bool(x.get("on", True)) != bool(y.get("on", True)):
            bad += 1
    return bad, worst


def lights_equal(a_lights, b_lights, tol=0.15, edge=0.02, box_from=None):
    """a = the offline atlas's lights, b = the live dump's (the same order). -> (number of lights that differ, number equal only through the pack's clamp, the largest difference of the
    others in metres, {"y max 78.998", ...} the edges that explained a clamp). A position is equal when each axis is within `tol`, or when the live value sits on the extreme of the LIVE set on
    that axis (within `edge`) and the file's value lies beyond it: the game's pack keeps a position inside the box of its 16-bit steps, so the box's edge is where the live set's lights pile
    up. Only the live side can be clamped; a live light on an edge whose file value is not beyond it still differs. `box_from` = the lights whose extremes are the box (default b_lights:
    for the street lamps pass the whole live set, the file's lights and the lamps share one box)."""
    ext = box_from if box_from else b_lights
    if ext:
        lo = [min(l["pos"][k] for l in ext) for k in range(3)]
        hi = [max(l["pos"][k] for l in ext) for k in range(3)]
    else:
        lo = hi = [0.0] * 3
    names = "xyz"
    bad, clamped, worst, edges = 0, 0, 0.0, set()
    for x, y in zip(a_lights, b_lights):
        if x["type"] != y["type"]:
            bad += 1
            continue
        differs, was_clamped = False, False
        for k in range(3):
            p, q = x["pos"][k], y["pos"][k]
            d = abs(p - q)
            if d <= tol:
                worst = max(worst, d)
            elif abs(q - hi[k]) <= edge and p > q:
                edges.add("%s max %.3f" % (names[k], hi[k]))
                was_clamped = True
            elif abs(q - lo[k]) <= edge and p < q:
                edges.add("%s min %.3f" % (names[k], lo[k]))
                was_clamped = True
            else:
                differs = True
                worst = max(worst, d)
        if differs:
            bad += 1
        elif was_clamped:
            clamped += 1
    return bad, clamped, worst, edges


def streets(atlas):
    """name -> {centre, bbox, rails, speed} over the road nodes (the named places of the city)."""
    out = {}
    for nd in atlas.nodes:
        if nd["kind"] != "road" or not nd.get("name"):
            continue
        e = out.setdefault(nd["name"], {"rails": 0, "speed": nd.get("speed"), "bbox": [1e9, 1e9, -1e9, -1e9]})
        for rid in nd["rails"]:
            e["rails"] += 1
            for p in atlas.rails[rid]["pts"]:
                b = e["bbox"]
                b[0], b[1], b[2], b[3] = min(b[0], p[0]), min(b[1], p[2]), max(b[2], p[0]), max(b[3], p[2])
    for e in out.values():
        b = e["bbox"]
        e["centre"] = [round((b[0] + b[2]) / 2, 1), round((b[1] + b[3]) / 2, 1)]
    return out


# ------------------------------------------------------------------------------------------------------------------ selftest
def _synthetic():
    import atlas_game as g
    rn = g.parse_railnet(g.synth_railnet())
    lights = g.parse_lights(g.synth_lights())
    return Atlas.from_data(lights, rn, [{"name": name, "pos": [i, 0, i]} for i, name in enumerate(("a", "b", "c"))], [{"index": 0, "name": "vp_test_car_00", "class": "player"}])


def _selftest():
    good = True

    def check(name, cond):
        nonlocal good
        print(("ok   " if cond else "FAIL ") + name)
        good = good and bool(cond)

    a = _synthetic()
    r = query(a, "road", {"pos": [15.0, 0.0, 3.0]}, {})
    check("road: nearest rail 0, 3 m, heading 90 (+X)", r["ok"] and r["result"]["nearest"][0]["rail"] == 0 and abs(r["result"]["nearest"][0]["dist_m"] - 3.0) < 0.01
          and abs(r["result"]["nearest"][0]["heading_deg"] - 90.0) < 0.5)
    check("road: name and speed of the synthetic road", r["result"]["nearest"][0]["name"] == "SYN_Test_St." and r["result"]["nearest"][0]["speed"] == 20.0)
    check("road: on_road false at 30 m", not query(a, "road", {"pos": [15, 0, 30], "r": 50}, {})["result"]["on_road"])
    check("road: heading filter drops the opposite way", query(a, "road", {"pos": [15, 0, 0], "heading": 270.0}, {})["result"]["nearest"] == [])
    rt = route(a, [2.0, 0.0, 1.0], [58.0, 0.0, -1.0])
    check("route: across the join (rails 0 then 1)", rt["ok"] and rt["result"]["rails"] == [0, 1] and abs(rt["result"]["length_m"] - 56.0) < 1.0)
    check("route: points are x,z,speed,y and end near the goal", rt["ok"] and len(rt["result"]["points"][0]) == 4 and abs(rt["result"]["points"][-1][0] - 58.0) < 0.5)
    check("route: speed capped by the road limit", all(p[2] <= 20.0 + 1e-6 for p in rt["result"]["points"]))
    check("route: no way backwards", not route(a, [58.0, 0, 0.0], [2.0, 0, 0.0])["ok"])
    txt = route_file_text(rt["result"], "test")
    check("route file text", txt.startswith("# test\n") and len(txt.strip().splitlines()) == len(rt["result"]["points"]) + 1)
    check("lights: 2 in 20 m of (2,2,3), 1 in 2 m", query(a, "lights", {"near": [2, 2, 3], "r": 20}, {})["result"]["in_radius"] == 2
          and query(a, "lights", {"near": [2, 2, 3], "r": 2}, {})["result"]["in_radius"] == 1)
    ls = query(a, "lights", {"near": [2.0, 2.0, 3.0], "r": 20.0, "kind": "omni"}, {})["result"]["lights"]
    check("lights: omni at 1.0 m, reach 44.7 m", len(ls) == 1 and ls[0]["reach_m"] == 44.7 and ls[0]["dist"] == 1.0)
    check("ground: from a road height", abs(query(a, "ground", {"x": 12.0, "z": 2.0}, {})["result"]["y"]) < 0.01)
    check("garages", len(query(a, "garages", {}, {})["result"]["garages"]) == 3)
    check("cars search", len(query(a, "cars", {"search": "TEST"}, {})["result"]["cars"]) == 1)
    check("unknown command", query(a, "bogus", {}, {})["error"]["code"] == "unknown_cmd")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        sp = os.path.join(td, "spots.txt")
        with open(sp, "w", encoding="utf-8") as f:
            f.write("# header\ngate 819.6 17.6 -808.4 62  # the Hollywood lot\nplain 1 2 3\n")
        t = spots(sp)
        check("spots: parse", t["gate"]["heading"] == 62.0 and t["gate"]["text"] == "the Hollywood lot" and t["plain"]["heading"] == 0.0)
        s = resolve(t, "spot:plain@2,1,5")  # heading 0: forward = +z, right = -x (the engine's HeadingBasis)
        check("spot offset in the heading frame", s and abs(s["pos"][0] - (-1.0)) < 1e-9 and abs(s["pos"][1] - 3.0) < 1e-9 and abs(s["pos"][2] - 8.0) < 1e-9)
        s90 = resolve(t, "spot:gate@0,0,10")  # heading 62: forward (sin 62, cos 62)
        check("spot forward offset follows the heading", abs(s90["pos"][0] - (819.6 + 10 * math.sin(math.radians(62)))) < 1e-9)
        check("spot unknown", resolve(t, "spot:nope") is None)
        r2 = query(a, "road", {"pos": "spot:plain"}, t)
        check("a spot name as a position", r2["ok"])
        out = os.path.join(td, "atlas")
        import atlas_game as g
        d = write_atlas(out, g.parse_lights(g.synth_lights()), g.parse_railnet(g.synth_railnet()), "selftest")
        b = Atlas.load(d)
        check("write_atlas / load round trip", len(b.rails) == 2 and len(b.lights) == 2 and b.meta["counts"]["rails"] == 2)
        check("default_atlas_dir follows latest.txt", os.path.basename(d) == open(os.path.join(out, "latest.txt")).read())
        out2 = os.path.join(td, "atlas2")
        d2 = adopt(d, out2)
        c, _ = compare(Atlas.load(d), Atlas.load(d2))
        check("adopt copies the files, compare says equal", os.path.isfile(os.path.join(d2, "rails.json")) and c)
        b2 = Atlas.load(d)
        b2.lights[0]["pos"][0] += 5.0
        check("compare finds a moved light", not compare(Atlas.load(d), b2)[0])
        # a live dump holds the street lamps too (src "lamp"): compared file list against file list, the lamps are reported, not failed
        live = Atlas.load(d)
        live.lights.append({"i": len(live.lights), "pos": [1.0, 4.0, 2.0], "color": [1, 0.8, 0.5, 0], "dir": [0, -1, 0, 0], "intensity": 60.0, "cone": 125.0, "spread": 0.0, "type": "spot", "on": True, "src": "lamp"})
        c2, ln2 = compare(Atlas.load(d), live)
        check("compare: a live dump's street lamps do not fail it, and are reported", c2 and any(x.startswith("info street lamps") and x.endswith("0 / 1") for x in ln2))
        # the game's pack clamps a position to its box: a live light on the live set's edge whose file position lies beyond it is equal; a light on the edge whose file value is NOT beyond it,
        # or one that is simply moved, still differs (the first full compare, 11-21: 295 lights, all of them the clamp)
        def lt(x, y, z, kind="omni"):
            return {"pos": [x, y, z], "type": kind}
        off_l = [lt(0.0, 10.0, 0.0), lt(5.0, 204.9, 5.0), lt(-3467.953, 3.0, 9.0), lt(9.0, 30.0, 9.0, "spot")]
        live_l = [lt(0.05, 10.0, 0.0), lt(5.02, 78.998, 5.0), lt(-3467.0, 3.0, 9.0), lt(9.0, 30.0, 9.0, "spot")]
        bad, nclamp, worst, edges = lights_equal(off_l, live_l)
        check("lights_equal: lights beyond the pack's box are equal through the clamp, the rest within 0.15 m", bad == 0 and nclamp == 2 and edges == {"y max 78.998", "x min -3467.000"} and abs(worst - 0.05) < 1e-9)
        live_m = [lt(0.0, 10.0, 0.0), lt(5.0, 78.998, 5.0), lt(-3467.0, 3.0, 9.0), lt(9.0, 30.0, 9.0, "spot")]
        off_m = [lt(0.0, 10.0, 0.0), lt(5.0, 70.0, 5.0), lt(-3467.953, 3.0, 9.0), lt(9.0, 30.0, 9.0, "spot")]
        check("lights_equal: a light on the edge whose file value is below it (not beyond) differs", lights_equal(off_m, live_m)[0] == 1)
        live_n = [lt(0.0, 10.0, 0.0), lt(5.0, 78.998, 5.0), lt(-3467.0, 3.0, 9.0), lt(14.0, 30.0, 9.0, "spot")]
        check("lights_equal: a moved light still differs next to the clamped ones", lights_equal(off_l, live_n)[0] == 1)
        live_t = [lt(0.0, 10.0, 0.0), lt(5.0, 78.998, 5.0), lt(-3467.0, 3.0, 9.0), lt(9.0, 30.0, 9.0, "omni")]
        check("lights_equal: a type change differs", lights_equal(off_l, live_t)[0] == 1)
        check("lights_equal: nothing clamped is reported as such", lights_equal(off_l[:1], live_l[:1])[1:2] == (0,))
        # lamps on both sides (the offline atlas now decodes them): compared like the file's lights, the box shared, the fields checked
        def lamp(i, x, y, z, **kw):
            d = {"i": i, "pos": [x, y, z], "color": [0.75, 0.8, 0.85], "dir": [-0.0133, -0.9888, -0.1473], "intensity": 55.0, "cone": 125.0, "spread": -50.0, "type": "spot", "on": True, "src": "lamp"}
            d.update(kw)
            return d
        off_a = Atlas.load(d)
        off_a.lights += [lamp(2, 10.0, 24.0, 31.0), lamp(3, 20.0, 204.9, 5.0), lamp(4, 30.0, 4.0, 5.0)]
        live_a = Atlas.load(d)
        live_a.lights += [lamp(2, 10.05, 24.0, 31.0, color=[0.7500, 0.7998, 0.8496], dir=[-0.0132, -0.9888, -0.1475]), lamp(3, 20.0, 78.998, 5.0), lamp(4, 30.0, 4.0, 5.0)]
        c3, ln3 = compare(off_a, live_a)
        check("compare: lamps on both sides: positions through the clamp, fields to half float rounding, equal", c3 and any(x.startswith("ok   street lamps: positions") and "1 kept at the pack's box edge: y max 78.998" in x for x in ln3)
              and any(x.startswith("ok   street lamps: colour") for x in ln3) and not any(x.startswith("info") for x in ln3))
        live_b = Atlas.load(d)
        live_b.lights += [lamp(2, 10.05, 24.0, 31.0), lamp(3, 20.0, 78.998, 5.0), lamp(4, 30.0, 4.0, 5.0, intensity=70.0)]
        c4, ln4 = compare(off_a, live_b)
        check("compare: a lamp with another intensity fails", not c4 and any(x.startswith("FAIL street lamps: colour") and "(1 differ)" in x for x in ln4))
        live_c = Atlas.load(d)
        live_c.lights += [lamp(2, 10.05, 24.0, 31.0), lamp(3, 20.0, 78.998, 5.0), lamp(4, 30.0, 9.0, 5.0)]
        check("compare: a moved lamp fails", not compare(off_a, live_c)[0])
        live_d = Atlas.load(d)
        live_d.lights += [lamp(2, 10.05, 24.0, 31.0)]
        c5, ln5 = compare(off_a, live_d)
        check("compare: another lamp count fails", not c5 and any(x.startswith("FAIL street lamp count") for x in ln5))
        out3 = os.path.join(td, "atlas3")
        d3 = write_atlas(out3, g.parse_lights(g.synth_lights()) + [lamp(2, 1.0, 2.0, 3.0), lamp(3, 4.0, 5.0, 6.0)], g.parse_railnet(g.synth_railnet()), "selftest lamps")
        b3 = Atlas.load(d3)
        check("write_atlas with lamps: the folder name (hash) is the file lights' (no lamp changes it), meta counts the lamps", os.path.basename(d3) == os.path.basename(d) and b3.meta["counts"]["lamps"] == 2
              and b3.meta["counts"]["lights"] == 4 and sum(1 for l in b3.lights if l.get("src") == "lamp") == 2)
        lq = Atlas.from_data(lights=live.lights)
        check("lights: src lamp / file select the lamps / the file's lights", query(lq, "lights", {"near": [1, 4, 2], "r": 500, "src": "lamp"}, {})["result"]["in_radius"] == 1
              and query(lq, "lights", {"near": [1, 4, 2], "r": 500, "src": "file"}, {})["result"]["in_radius"] == len(live.lights) - 1)
    print("selftest: " + ("PASS" if good else "FAIL"))
    return 0 if good else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("--game", required=True, help="the game folder holding xarchive_cache.rpf")
    b.add_argument("--out", help="atlas root (default ~/.larecomp/director/atlas)")
    b.add_argument("--src", help="LARecomp source tree (for the RPF key in aes256.cpp)")
    sub.add_parser("info").add_argument("--atlas")
    q = sub.add_parser("query")
    q.add_argument("name")
    q.add_argument("json", nargs="?", default="{}")
    q.add_argument("--atlas")
    q.add_argument("--spots")
    s = sub.add_parser("streets")
    s.add_argument("--atlas")
    r = sub.add_parser("route")
    r.add_argument("a")
    r.add_argument("b")
    r.add_argument("--via", action="append", default=[])
    r.add_argument("--speed", type=float)
    r.add_argument("--atlas")
    r.add_argument("--spots")
    r.add_argument("-o", "--out")
    ad = sub.add_parser("adopt", help="copy a live atlas dump and make it current")
    ad.add_argument("folder")
    cp = sub.add_parser("compare", help="compare a live atlas dump with an offline atlas")
    cp.add_argument("a")
    cp.add_argument("b")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return _selftest()
    if args.cmd in ("build", "adopt"):
        try:
            if args.cmd == "build":
                return cmd_build(args)
            print("adopted: " + adopt(args.folder, output_root(None)))
            return 0
        except (OSError, ValueError, struct.error) as ex:
            print(f"atlas: {ex}", file=sys.stderr)
            return 2
    if args.cmd == "compare":
        good, lines = compare(Atlas.load(args.a), Atlas.load(args.b))
        print(chr(10).join(lines))
        print("compare: " + ("PASS" if good else "FAIL"))
        return 0 if good else 1
    if not args.cmd:
        ap.print_help()
        return 2
    try:
        a = Atlas.load(getattr(args, "atlas", None))
    except FileNotFoundError as ex:
        print(json.dumps(fail("no_world", str(ex))))
        return 1
    if args.cmd == "info":
        print(json.dumps({"path": a.path, "meta": a.meta}, indent=1))
        return 0
    if args.cmd == "streets":
        print(json.dumps(ok(streets(a)), indent=1))
        return 0
    tbl = spots(getattr(args, "spots", None) or default_spots_path())

    def pt(s):
        if s.startswith("spot:") or re.match(r"^[A-Za-z_]", s):
            return s
        return [float(v) for v in s.split(",")]

    if args.cmd == "query":
        res = query(a, args.name, json.loads(args.json), tbl)
    else:
        res = route(a, pt(args.a), pt(args.b), [pt(v) for v in args.via], args.speed, tbl)
        if res["ok"] and args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(route_file_text(res["result"], "route %s -> %s, atlas %s" % (args.a, args.b, a.meta.get("hash", "?"))))
    print(json.dumps(res))
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
