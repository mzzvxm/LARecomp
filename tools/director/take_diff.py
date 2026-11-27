r"""take_diff.py: compare two state logs (the product's per-update state log, `statelog on` / mcla_state_log, and the fixed-step experiment's).

  python -I take_diff.py A.csv B.csv [--align scene|tick|mark=LABEL|col=EXPR] [--tol pos=1e-3,rot=1e-5,col:REGEX=V] [--clocks] [--rng] [--strict]
                         [--ignore a,b] [--only REGEX] [--json] [--exact] [--shift auto|N]
  python -I take_diff.py --growth A.csv B.csv [--at 1,2,5,10]
  python -I take_diff.py --curve A.csv [B.csv ...] [--at 1,2,5,10] [--cols p_speed,p_m12] [--from mark=go]
  python -I take_diff.py --selftest [--exe state_log_test.exe]

THE PRODUCT LOG (format v2, `#statelog v2`; src/mc_engine/cinematic/state_log_core.h has the columns). Two takes of the same scene in, one of:

  IDENTICAL                     every compared value is the same text (bit for bit), or
  IDENTICAL within tolerance    every value is within its column's tolerance; the largest differences are named, or
  DIVERGES                      the FIRST ROW (of the comparison, with the scene time), the columns over their tolerance in it with both values, the
                                difference and the tolerance, a per-column table (first row, largest difference, rows over), a one-line reading

The exit code is 0 identical (either kind), 1 diverges, 2 bad input. What is compared, by class of column (the tolerance is absolute, in metres,
metres a second or hours; `--tol CLASS=VALUE` or `--tol col:REGEX=VALUE` change it, 0 = equal):

  pos 1e-3   a car's position            rot 1e-4   a car's rotation cells    speed 1e-3   the player's speed       hour 1e-4
  campos 2e-3 / camrot 1e-4   the published camera       world, count (exact)   the weather, pause, scene state, which cars exist and their ids
  clock 1e-6 the game's clocks, dt, scene_t: compared only under a FIXED STEP (both logs have one dt for every row: the rows are then at the
             same game times) or with --clocks. In real time two takes' rows are at different game times, and only a still world (a parked
             car, a camera-only scene) compares row for row; the clocks are then listed outside the verdict.
  rng        the generator's state: seeded from the clock (E1b), so outside the verdict unless --rng (E1b: it is equal at a pinned start and nowhere else)
  hash       the exact-bit hashes: only with --strict, which also makes every tolerance 0 (bit for bit)

The rows are lined up on the scene's start (the first row with the scene running, `--align scene`, the default when both logs have one) and compared
while the scene runs in both; `--align mark=NAME` (a scene's `mark`), `col=EXPR`, `reset=EXPR` or `tick` line them up otherwise. `--exact` is the
older bit-for-bit text comparison (hashes, groups, `--shift`), the only mode for the experiment's logs. Notes are printed for rows the writer dropped
(`lost`) and for a log the game did not close (no `#end` line).

THE EXPERIMENT'S LOG (format v1, `#cin_state v1`, mcla_test_state_log of test/cin-e1b, cin-fixedstep.patch) is compared as below.

The logs have one row per game update: a hash and the state of the player car, the race cars, the random number state, the
camera, the hour, the clocks (see src/mc_engine/cin_fixedstep.cpp). Rows are compared as TEXT (the logger prints floats with
9 digits, which comes back exactly), so "identical" means bit for bit.

Two logs in, the first diverging row and which values:

  FIRST DIVERGENCE   the first row where any compared column differs: its number, the game time since the start of the
                     comparison, which GROUPS of values differ (player, racers, world, cam, amb, clock) and every column that
                     differs in that row with both values and the difference (in units in the last place for a float)
  PER-COLUMN TABLE   for every column that ever differs, the first row where it does: a column that differs from row 0 on
                     (a pointer, a stamp) stands apart from the ones that start later, and the order of the rows names the
                     cause (the random state first = a different number of draws; the car's matrix first = the physics)
  VERDICT            IDENTICAL over N rows (the final chain hashes agree), or DIVERGES; the exit code is 0 / 1 (2: bad input)

How to align rows when two takes start on different frames:
  --align tick        row i against row i (the default)
  --align mark=go     the first row labelled go in each log (a tour's `set mcla_test_state_mark go`)
  --align col=mode_timer>=9.3   the first row where the condition holds in each log (a race's start)
  --shift auto        line B up against A by the state hash (`hash` leaves the clocks out): the shift most rows agree on;
                      then the clocks (frame_ctr, clock_*, mode_timer, audio_*, rec_pos) are left out of the comparison
                      (they are offset by the shift); --keep-clocks compares them anyway.  --shift N: B's row i+N against A's row i.
Default ignored columns: tick, mark, wall_ms (meta) and p_vx, p_vy, p_vz (derived from two rows and the step). The group hashes
(h_*), `hash` and `chain` are compared but shown as groups, not as values.

--curve is the E2 table (several steps, same scene): for each log, the values of --cols at --at seconds of stepped game time
from its start (--from mark=LABEL or the first row), the step actually used, the sub-step count, how far the audio clock ran
against the stepped time.
"""
import argparse
import csv
import json
import math
import os
import re
import struct
import sys
import tempfile

INT_COLS = {"tick", "step_ticks", "frame_ctr", "substeps", "rng", "weather", "paused", "live", "p_ok", "n_cars", "cam_ok",
            "amb_en", "traffic", "rec_mode", "rec_pos", "audio_ms"}
HEX_COLS = {"amb_hash", "h_player", "h_racers", "h_world", "h_cam", "h_amb", "h_clock", "hash", "chain"}
STR_COLS = {"mark"}
GROUPS = ["h_player", "h_racers", "h_world", "h_cam", "h_amb", "h_clock"]
GROUP_NAMES = {"h_player": "player", "h_racers": "racers", "h_world": "world", "h_cam": "cam", "h_amb": "amb", "h_clock": "clock"}
META = {"tick", "mark", "wall_ms"}
PADDING = {"p_m3", "p_m7", "p_m11", "p_m15", "cam_m3", "cam_m7", "cam_m11", "cam_m15"}   # the fourth word of each row: uninitialised memory (1e19, NaN), never state
DERIVED = {"p_vx", "p_vy", "p_vz"} | PADDING
CLOCKS = {"frame_ctr", "clock_total", "clock_game", "mode_timer", "audio_clock", "audio_ms", "rec_pos", "h_clock"}
NOT_VALUES = set(GROUPS) | {"hash", "chain"}


class DiffError(Exception):
    pass


# ------------------------------------------------------------------------------------------------------------------ reading
class Log:
    def __init__(self, path):
        self.path = str(path)
        self.comments = []
        self.cols = None
        self.rows = []
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line:
                    continue
                if line.startswith("#"):
                    self.comments.append(line)
                    continue
                if self.cols is None:
                    self.cols = line.split(",")
                    continue
                parts = line.split(",")
                if len(parts) != len(self.cols):
                    continue  # a row cut short by the end of the run
                self.rows.append(parts)
        if self.cols is None:
            raise DiffError(f"{path}: no header row (is it a mcla_test_state_log file?)")
        self.index = {c: i for i, c in enumerate(self.cols)}
        self.version = None   # "v2": the product's log, "v1": the experiment's
        self.meta = {}        # the preamble's key=value pairs (v2)
        self.trailer = {}     # the `#end rows=.. written=.. dropped=..` line of a log that was closed
        for c in self.comments:
            parts = c.split()
            if c.startswith("#statelog ") and len(parts) > 1:
                self.version = parts[1]
                self.meta = dict(q.split("=", 1) for q in parts[2:] if "=" in q)
            elif c.startswith("#cin_state ") and len(parts) > 1:
                self.version = parts[1]
            elif c.startswith("#end "):
                self.trailer = dict(q.split("=", 1) for q in parts[1:] if "=" in q)

    def col(self, name):
        if name not in self.index:
            raise DiffError(f"{self.path}: no column {name!r}")
        i = self.index[name]
        return [r[i] for r in self.rows]

    def num(self, name):
        out = []
        for v in self.col(name):
            try:
                out.append(float(v))
            except ValueError:
                out.append(float("nan"))
        return out

    def describe_v2(self):
        bits = []
        if self.meta.get("scene", "-") != "-":
            bits.append(f"scene {self.meta['scene']}")
        bits.append(self.step_text() or "step unknown")
        if self.trailer:
            bits.append(f"closed: {self.trailer.get('written', '?')} written, {self.trailer.get('dropped', '?')} dropped")
        else:
            bits.append("not closed (no #end line)")
        return "; ".join(bits)

    def step_text(self):
        for c in self.comments:
            m = re.search(r"fixed_step_fps=([\d.eE+-]+) step_ticks=(\d+) tick_seconds=([\d.eE+-]+)", c)
            if m:
                fps, ticks, ts = float(m.group(1)), int(m.group(2)), float(m.group(3))
                if ticks:
                    return f"fixed step {ticks} ticks = {ticks * ts * 1000:.4f} ms ({1 / (ticks * ts):.2f} FPS)"
                return "real time (no fixed step set at the start)"
        return ""


# ------------------------------------------------------------------------------------------------------------------ alignment
def parse_cond(expr):
    m = re.fullmatch(r"\s*(\w+)\s*(>=|<=|==|!=|>|<)\s*(-?[\d.eE+-]+|\w+)\s*", expr)
    if not m:
        raise DiffError(f"bad condition {expr!r}: use  column OP value  (OP: >= <= == != > <)")
    return m.group(1), m.group(2), m.group(3)


def first_row(log, how):
    """Index of the first row of `log` that `how` (tick | mark=X | col=EXPR) names."""
    if how == "tick":
        return 0
    if how == "scene":
        first, _ = scene_rows(log)
        if first is None:
            raise DiffError(f"{log.path}: no row has the scene running (scene_state 3): it was not taken with a scene")
        return first
    if how.startswith("mark="):
        label = how[5:]
        for i, v in enumerate(log.col("mark")):
            if v == label:
                return i
        raise DiffError(f"{log.path}: no row is marked {label!r} (marks: {sorted(set(v for v in log.col('mark') if v))})")
    if how.startswith("reset="):
        # the first row that satisfies the condition AFTER the last big drop of the column (a race's mode timer restarts from 0 when the
        # race loads: free roam's timer is long past 9.3 s by then, so `col=mode_timer>=9.3` would name a row in free roam)
        name, op, value = parse_cond(how[6:])
        vals = []
        for v in log.col(name):
            try:
                vals.append(float(v))
            except ValueError:
                vals.append(float("nan"))
        drop = 0
        for i in range(1, len(vals)):
            if vals[i] + 5.0 < vals[i - 1]:
                drop = i
        b = float(value)
        for i in range(drop, len(vals)):
            ok = {">=": vals[i] >= b, "<=": vals[i] <= b, "==": vals[i] == b, "!=": vals[i] != b, ">": vals[i] > b, "<": vals[i] < b}[op]
            if ok:
                return i
        raise DiffError(f"{log.path}: no row after the last drop of {name} (row {drop}) has {name} {op} {value}")
    if how.startswith("col="):
        name, op, value = parse_cond(how[4:])
        vals = log.col(name)
        for i, v in enumerate(vals):
            try:
                a, b = float(v), float(value)
            except ValueError:
                a, b = v, value
            ok = {">=": a >= b, "<=": a <= b, "==": a == b, "!=": a != b, ">": a > b, "<": a < b}[op]
            if ok:
                return i
        raise DiffError(f"{log.path}: no row has {name} {op} {value}")
    raise DiffError(f"unknown alignment {how!r}")


def auto_shift(a, b, max_shift=None):
    """The shift s with the most rows where B[i + s].hash == A[i].hash (ties: the smaller |s|), and how many rows agree."""
    ha, hb = a.col("hash"), b.col("hash")
    pos = {}
    for j, h in enumerate(hb):
        pos.setdefault(h, []).append(j)
    votes = {}
    for i, h in enumerate(ha):
        js = pos.get(h)
        if not js or len(js) > 400:   # a state that repeats for ever (a parked car) says nothing about the shift
            continue
        for j in js:
            s = j - i
            if max_shift is not None and abs(s) > max_shift:
                continue
            votes[s] = votes.get(s, 0) + 1
    if not votes:
        return None, 0
    best = max(votes.items(), key=lambda kv: (kv[1], -abs(kv[0])))
    return best[0], best[1]


# ------------------------------------------------------------------------------------------------------------------ values
def f32_bits(x):
    return struct.unpack("<i", struct.pack("<f", x))[0]


def ulps(a, b):
    """Distance in floats between two finite float32 values (None when not finite)."""
    try:
        x, y = float(a), float(b)
    except ValueError:
        return None
    if not (math.isfinite(x) and math.isfinite(y)):
        return None

    def key(v):
        i = f32_bits(v)
        return i if i >= 0 else -(i & 0x7FFFFFFF)
    return abs(key(x) - key(y))


def describe(name, va, vb):
    if name in INT_COLS or name in HEX_COLS or name in STR_COLS:
        return f"{name}: A {va} | B {vb}"
    try:
        d = float(vb) - float(va)
        u = ulps(va, vb)
        extra = f"diff {d:+.6g}" + (f", {u} ulp" if u is not None else "")
    except ValueError:
        extra = ""
    return f"{name}: A {va} | B {vb}  ({extra})" if extra else f"{name}: A {va} | B {vb}"


# ------------------------------------------------------------------------------------------------------------------ compare
def compare(a, b, align="tick", shift=None, ignore=(), only=None, keep_clocks=False, report=12, out=print):
    """Returns 0 identical, 1 diverges. `shift` is None, 'auto' or an int."""
    if a.cols != b.cols:
        common = [c for c in a.cols if c in b.index]
        if len(common) < 10:
            raise DiffError("the two logs have different columns (not the same version of the logger?)")
        out(f"note: the logs have different columns; comparing the {len(common)} they share")
    cols = [c for c in a.cols if c in b.index]
    out(f"A: {a.path}\n   {len(a.rows)} rows; {a.step_text()}")
    out(f"B: {b.path}\n   {len(b.rows)} rows; {b.step_text()}")
    if not a.rows or not b.rows:
        raise DiffError("a log has no rows")

    ia = ib = 0
    how = ""
    if shift is None:
        ia, ib = first_row(a, align), first_row(b, align)
        how = f"{align}: A row {ia}, B row {ib}"
        if align == "tick":
            how = "tick: row i of A against row i of B"
    else:
        if "hash" not in a.index or "hash" not in b.index:
            raise DiffError("--shift needs the hash column")
        if shift == "auto":
            s, votes = auto_shift(a, b)
            if s is None or votes < max(5, min(len(a.rows), len(b.rows)) // 20):
                out("alignment: no shift lines the two logs up by their state hash (fewer than 5% of the rows agree at any shift).")
                out("           Diverged before the comparison starts, or the logs are of different scenes. Try --align mark=... or col=...")
                return 1
            how = f"shift auto: B row i{s:+d} against A row i ({votes} rows agree by the state hash at this shift)"
        else:
            s = int(shift)
            how = f"shift {s:+d}: B row i{s:+d} against A row i"
        if s >= 0:
            ia, ib = 0, s
        else:
            ia, ib = -s, 0
        if not keep_clocks:
            ignore = set(ignore) | CLOCKS
            how += "; the clocks are left out (offset by the shift)"
    out(f"alignment: {how}")

    n = min(len(a.rows) - ia, len(b.rows) - ib)
    if n <= 0:
        raise DiffError("the aligned logs do not overlap")
    skip = set(META) | set(DERIVED) | set(ignore)
    if only:
        rx = re.compile(only)
        skip |= {c for c in cols if not rx.search(c) and c not in NOT_VALUES}
    cmp_cols = [c for c in cols if c not in skip and c != "chain"]
    ai = {c: a.index[c] for c in cmp_cols}
    bi = {c: b.index[c] for c in cmp_cols}
    dt_i = a.index.get("dt")

    first = {}   # column -> first row (relative) where it differs
    first_row_hit = None
    sim = 0.0
    sim_at_hit = 0.0
    for k in range(n):
        ra, rb = a.rows[ia + k], b.rows[ib + k]
        hit = False
        for c in cmp_cols:
            if ra[ai[c]] != rb[bi[c]]:
                if c not in first:
                    first[c] = k
                hit = True
        if hit and first_row_hit is None:
            first_row_hit = k
            sim_at_hit = sim
        if dt_i is not None:
            try:
                sim += float(ra[dt_i])
            except ValueError:
                pass
    if "tick" in a.index and "tick" in b.index:
        out(f"compared: {n} rows (A ticks {a.rows[ia][a.index['tick']]}..{a.rows[ia + n - 1][a.index['tick']]}, "
            f"B ticks {b.rows[ib][b.index['tick']]}..{b.rows[ib + n - 1][b.index['tick']]})")
    else:
        out(f"compared: {n} rows")

    # the chain is over every row of a log from ITS first row: equal only when the two logs agree from the start, to the end
    chain_equal = (a.rows[-1][a.index["chain"]] == b.rows[-1][b.index["chain"]]) if "chain" in a.index and "chain" in b.index else None

    if first_row_hit is None:
        extra = ""
        if len(a.rows) - ia != len(b.rows) - ib:
            extra = f" (the logs differ in length: {len(a.rows) - ia} and {len(b.rows) - ib} rows from the start; the shorter one was compared)"
        out(f"\nVERDICT: IDENTICAL over {n} rows ({sim:.3f} s of stepped game time){extra}")
        if chain_equal is True and ia == 0 and ib == 0:
            out("         the final chain hashes agree too (every row from the first, to the last).")
        return 0

    k = first_row_hit
    ra, rb = a.rows[ia + k], b.rows[ib + k]
    tick_a = ra[a.index["tick"]] if "tick" in a.index else str(ia + k)
    tick_b = rb[b.index["tick"]] if "tick" in b.index else str(ib + k)
    out(f"\nFIRST DIVERGENCE: row {k} of the comparison (A tick {tick_a}, B tick {tick_b}), {sim_at_hit:.4f} s of stepped game time after its start")
    groups = [GROUP_NAMES[g] for g in GROUPS if g in a.index and g in b.index and ra[a.index[g]] != rb[b.index[g]] and g not in skip]
    if "hash" in a.index and "hash" in b.index:
        out(f"  state hash {'differs' if ra[a.index['hash']] != rb[b.index['hash']] else 'agrees'}; groups that differ: {', '.join(groups) if groups else 'none'}")
    mark_a = ra[a.index["mark"]] if "mark" in a.index else ""
    if mark_a:
        out(f"  (the row is marked {mark_a!r} in A)")
    differing = [c for c in cmp_cols if ra[ai[c]] != rb[bi[c]] and c not in NOT_VALUES]
    out(f"  {len(differing)} value column(s) differ in this row:")
    for c in differing[:report]:
        out("    " + describe(c, ra[ai[c]], rb[bi[c]]))
    if len(differing) > report:
        out(f"    ... and {len(differing) - report} more (the table below)")

    out("\nPER-COLUMN FIRST DIVERGENCE (row of the comparison, in order; the group hashes first):")
    order = sorted(first.items(), key=lambda kv: (kv[0] not in NOT_VALUES, kv[1], cmp_cols.index(kv[0])))
    shown = 0
    for c, row in order:
        if shown >= 40:
            out(f"  ... {len(order) - shown} more columns")
            break
        r1, r2 = a.rows[ia + row], b.rows[ib + row]
        label = f"group {GROUP_NAMES[c]}" if c in GROUPS else c
        value = "" if c in NOT_VALUES else "   " + describe(c, r1[ai[c]], r2[bi[c]])[len(c) + 2:]
        out(f"  row {row:>7}  {label:<14}{value}")
        shown += 1
    out(f"\nVERDICT: DIVERGES at row {k} ({sim_at_hit:.4f} s): {', '.join(differing[:4]) if differing else 'only hashes'}"
        + (f" (and {len(differing) - 4} more)" if len(differing) > 4 else ""))
    # a one-line reading of the order
    early = [c for c, r in order if r == k and c not in NOT_VALUES]
    if early:
        if "rng" in early:
            out("  reading: the random number state differs at the first row: a different number of draws before it (traffic, ambient, effects).")
        elif any(c.startswith("p_m") or c in ("p_speed",) for c in early):
            out("  reading: the player car's own state differs at the first row: physics (pool threads, contact order) or the input.")
        elif any(c.startswith("c") and c[1:2].isdigit() for c in early):
            out("  reading: a race car's state differs first: the AI or the physics of an opponent.")
        elif any(c.startswith("cam_") for c in early):
            out("  reading: only the camera differs first: the camera filter (dt-driven) or its input, not the world.")
    return 1


# ------------------------------------------------------------------------------------------------------------------ curve (E2)
def growth(a, b, align, at, out=print):
    """E1b: how far apart the two runs are at each time after the alignment: the player's position, and the race cars matched by
    nearest position (the heap scan lists the cars in another order in each run, so slot k of A is not slot k of B)."""
    ia, ib = first_row(a, align), first_row(b, align)
    n = min(len(a.rows) - ia, len(b.rows) - ib)
    if n <= 0:
        raise DiffError("the aligned logs do not overlap")
    dt = 1.0 / 60.0
    if "dt" in a.index:
        d0 = a.num("dt")[ia:ia + min(n, 200)]
        good = [d for d in d0 if d == d and d > 0]
        if good:
            dt = sum(good) / len(good)
    pa = [a.num(c) for c in ("p_m12", "p_m13", "p_m14")] if "p_m12" in a.index else None
    pb = [b.num(c) for c in ("p_m12", "p_m13", "p_m14")] if "p_m12" in b.index else None

    def cars(log, row):
        res = []
        for c in range(8):
            try:
                if f"c{c}_m12" in log.index:   # the product log: the scene's cars, a pose when c<k>_ok is 1
                    if log.rows[row][log.index[f"c{c}_ok"]] != "1":
                        continue
                    x, y, z = (float(log.rows[row][log.index[f"c{c}_m{w}"]]) for w in (12, 13, 14))
                else:
                    x, y, z = (float(log.rows[row][log.index[f"c{c}_{ax}"]]) for ax in "xyz")
            except (KeyError, ValueError, IndexError):
                continue
            if x == x and (x or y or z):
                res.append((x, y, z))
        return res

    out(f"growth after the alignment ({align}: A row {ia}, B row {ib}; {n} rows, step {dt * 1000:.3f} ms):")
    out("  t_s     player max|dx,dy,dz|   race cars: nearest-match distance (A cars listed in A's order)")
    for t in at:
        k = int(round(t / dt))
        if k >= n:
            break
        if pa and pb:
            d = max(abs(pa[c][ia + k] - pb[c][ib + k]) for c in range(3))
            ptxt = f"{d:14.6g}"
        else:
            ptxt = "      (no player)"
        ca, cb = cars(a, ia + k), cars(b, ib + k)
        ctxt = ""
        if ca and cb:
            ds = []
            for (x, y, z) in ca:
                ds.append(min(math.sqrt((x - u) ** 2 + (y - v) ** 2 + (z - w) ** 2) for (u, v, w) in cb))
            ctxt = "  " + " ".join(f"{v:.3g}" for v in ds[:6])
        out(f"  {t:5.1f}   {ptxt}   {ctxt}")
    # the first row where the player's position differs at all, and where it differs by more than 1 cm / 1 m
    if pa and pb:
        marks = [(0.0, None), (0.01, None), (1.0, None)]
        res = {}
        for k in range(n):
            d = max(abs(pa[c][ia + k] - pb[c][ib + k]) for c in range(3))
            for thr in (0.0, 0.01, 1.0):
                if thr not in res and d > thr:
                    res[thr] = k
        names = {0.0: "differs at all", 0.01: "more than 1 cm apart", 1.0: "more than 1 m apart"}
        for thr in (0.0, 0.01, 1.0):
            out(f"  player {names[thr]}: " + (f"row {res[thr]} (t = {res[thr] * dt:.3f} s)" if thr in res else f"never in {n} rows"))
    return 0


def curve(paths, at, cols, start):
    logs = [Log(p) for p in paths]
    out = print
    for lg in logs:
        i0 = first_row(lg, start) if start != "tick" else 0
        dts = lg.num("dt")[i0:]
        sub = lg.col("substeps")[i0:] if "substeps" in lg.index else []
        out(f"\n{lg.path}: {len(lg.rows) - i0} rows from row {i0}; {lg.step_text()}")
        if dts:
            good = [d for d in dts if d == d and d > 0]
            if good:
                out(f"  dt: min {min(good) * 1000:.4f} ms, mean {sum(good) / len(good) * 1000:.4f} ms, max {max(good) * 1000:.4f} ms  ({1 / (sum(good) / len(good)):.2f} updates per game second)")
        if sub:
            try:
                s = [int(x) for x in sub]
                out(f"  physics sub-steps per update (r24): min {min(s)}, mean {sum(s) / len(s):.3f}, max {max(s)}")
            except ValueError:
                pass
        if "audio_clock" in lg.index:
            ac = lg.num("audio_clock")[i0:]
            sim = sum(d for d in dts[:-1] if d == d)   # row k holds the state after update k-1 and the step of update k: the last row's step is not in the clock yet
            if ac and sim > 0:
                out(f"  audio clock (0x8286CF34) advanced {ac[-1] - ac[0]:.4f} s while the game was stepped {sim:.4f} s: ratio {(ac[-1] - ac[0]) / sim:.5f}")
        if "frame_ctr" in lg.index:
            fc = lg.col("frame_ctr")[i0:]
            try:
                out(f"  engine update counter advanced {int(fc[-1]) - int(fc[0])} over {len(fc) - 1} rows")
            except ValueError:
                pass
    out("\nvalues at stepped game time since the start (the first row at or after each time):")
    header = ["t_s"]
    for n, lg in enumerate(logs):
        for c in cols:
            header.append(f"{os.path.basename(lg.path)}:{c}" if len(logs) > 1 else c)
    rows = []
    for t in at:
        line = [f"{t:g}"]
        for lg in logs:
            i0 = first_row(lg, start) if start != "tick" else 0
            dts = lg.num("dt")
            acc = 0.0
            hit = None
            for i in range(i0, len(lg.rows)):
                if acc >= t - 1e-9:
                    hit = i
                    break
                acc += dts[i] if dts[i] == dts[i] else 0.0
            for c in cols:
                line.append(lg.col(c)[hit] if hit is not None and c in lg.index else "-")
        rows.append(line)
    widths = [max(len(header[i]), max((len(r[i]) for r in rows), default=0)) for i in range(len(header))]
    out("  " + "  ".join(h.rjust(w) for h, w in zip(header, widths)))
    for r in rows:
        out("  " + "  ".join(v.rjust(w) for v, w in zip(r, widths)))
    return 0


# ------------------------------------------------------------------------------------------------------------------ the product log (statelog v2)
POSE_WORDS = (0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14)   # the twelve matrix words that are logged (the fourth word of each row is padding)
MAX_CARS = 6
# Compared as the time elapsed since the first compared row (two takes reach the scene at different game ages: the absolute clocks differ by the offset).
RELATIVE = ("frame_ctr", "clock_total", "clock_game", "mode_timer")
CLOCK_COLS = {"scene_t", "dt", "dt_unscaled", "time_scale", "frame_ctr", "clock_total", "clock_game", "mode_timer"}
WORLD_COLS = {"weather", "paused", "live", "scene_state"}
COUNT_COLS = {"p_ok", "cam_ok", "n_cars"}
META2 = {"tick", "mark", "wall_ms", "lost"}
HASH2 = {"h_player", "h_cars", "h_world", "h_cam", "hash", "chain"}

# class -> default tolerance (absolute, in the log's own units: metres, metres a second, hours, seconds). 0 = the values must be equal.
# The numbers are what E1 / E1b measured for a parked or placed car (a millimetre unplaced, 8 micrometres placed) with room for the chase camera
# (not pinned: 2e-5 to 2e-4 apart) and the car's own settling; a hand-written scene is judged by these unless --tol says otherwise.
# rot was 1e-5 until 2026-11-14 (no measurement behind it): two takes of statelog-park (cint2-park-1/2) rest 3.4e-5 apart in the rotation cells from
# a one-ulp difference at row 14, constant to the end; 1e-4 is about 0.2 mm at the end of a car, inside the 1 mm position promise (owner's OK).
TOL_DEFAULTS = {"pos": 1e-3, "rot": 1e-4,"speed": 1e-3, "campos": 2e-3, "camrot": 1e-4, "hour": 1e-4, "world": 0.0, "count": 0.0, "other": 0.0,
                "clock": 1e-6, "rng": 0.0, "hash": 0.0}
CLASS_ORDER = ["pos", "rot", "speed", "campos", "camrot", "hour", "world", "count", "other", "clock", "rng", "hash"]
CLASS_HELP = {"pos": "a car's position (p_m12..14, cN_m12..14)", "rot": "a car's rotation cells", "speed": "p_speed",
              "campos": "the published camera's position", "camrot": "the camera's rotation cells", "hour": "the hour",
              "world": "weather, pause, free roam live, the scene's state (exact)", "count": "which cars exist and their ids (exact)",
              "other": "columns this tool does not know (exact)", "clock": "dt, scene_t, and the game's clocks as time elapsed since the first compared row (only under a fixed step)",
              "rng": "the generator's state (only with --rng)", "hash": "the exact-bit hashes (only with --strict)"}


def col_class(name):
    if name in META2:
        return "meta"
    if name in CLOCK_COLS:
        return "clock"
    if name == "rng":
        return "rng"
    if name == "hour":
        return "hour"
    if name in WORLD_COLS:
        return "world"
    if name in COUNT_COLS or re.fullmatch(r"c\d+_(ok|id)", name):
        return "count"
    if name == "p_speed":
        return "speed"
    m = re.fullmatch(r"(?:p|c\d+)_m(\d+)", name)
    if m:
        return "pos" if int(m.group(1)) in (12, 13, 14) else "rot"
    m = re.fullmatch(r"cam_m(\d+)", name)
    if m:
        return "campos" if int(m.group(1)) in (12, 13, 14) else "camrot"
    if name in HASH2:
        return "hash"
    return "other"


class Tolerances:
    """The tolerance of every column: a `col:REGEX=value` rule first (the first match), else the class's value."""

    def __init__(self, spec="", strict=False):
        self.classes = dict(TOL_DEFAULTS)
        self.rules = []   # (compiled regex, value, text)
        if strict:
            self.classes = {k: 0.0 for k in self.classes}
        for item in [s.strip() for s in (spec or "").split(",") if s.strip()]:
            if "=" not in item:
                raise DiffError(f"bad --tol item {item!r}: use CLASS=VALUE or col:REGEX=VALUE (classes: {', '.join(CLASS_ORDER)})")
            key, value = item.rsplit("=", 1)
            try:
                v = float(value)
            except ValueError:
                raise DiffError(f"bad --tol value {value!r} in {item!r}: a number (0 = equal)")
            if v < 0 or v != v:
                raise DiffError(f"bad --tol value {value!r} in {item!r}: not negative")
            key = key.strip()
            if key.startswith("col:"):
                try:
                    self.rules.append((re.compile(key[4:]), v, item))
                except re.error as e:
                    raise DiffError(f"bad regex in --tol {item!r}: {e}")
            elif key in self.classes:
                self.classes[key] = v
            else:
                raise DiffError(f"unknown tolerance class {key!r} in --tol (classes: {', '.join(CLASS_ORDER)}; or col:REGEX)")

    def of(self, name):
        for rx, v, _ in self.rules:
            if rx.search(name):
                return v
        return self.classes.get(col_class(name), 0.0)

    def line(self):
        parts = [f"{c} {self.classes[c]:g}" for c in CLASS_ORDER if c in ("pos", "rot", "speed", "campos", "camrot", "hour")]
        parts += [f"{rx.pattern} {v:g}" for rx, v, _ in self.rules]
        return ", ".join(parts)


def f32_ulp(x):
    """The distance to the next float32 above |x| (the game's clocks are float32 seconds: at an hour they are 0.2 ms apart)."""
    x = abs(x)
    if not math.isfinite(x) or x == 0.0:
        return 1.4e-45
    return 2.0 ** (math.floor(math.log2(x)) - 23)


def num_or_none(text):
    try:
        return float(text)
    except ValueError:
        return None


def value_diff(va, vb):
    """None when the two texts are the same value; else the absolute difference (a float; inf for non-numbers that differ, a NaN against a number)."""
    if va == vb:
        return None
    fa, fb = num_or_none(va), num_or_none(vb)
    if fa is None or fb is None:
        return float("inf")
    if fa != fa and fb != fb:
        return None
    if fa != fa or fb != fb:
        return float("inf")
    if fa == fb:
        return None
    return abs(fa - fb)


def scene_rows(log):
    """(first row with the scene running, how many rows from it the scene keeps running), or (None, 0)."""
    if "scene_state" not in log.index:
        return None, 0
    states = log.col("scene_state")
    first = next((i for i, v in enumerate(states) if v == "3"), None)
    if first is None:
        return None, 0
    run = 0
    for v in states[first:]:
        if v != "3":
            break
        run += 1
    return first, run


def is_fixed_step(log, start, n):
    """True when dt is one value over the compared rows (a fixed step: the rows are at the same game times in every take)."""
    if "dt" not in log.index or n < 2:
        return False
    vals = log.col("dt")[start:start + n]
    return len(set(vals)) == 1 and num_or_none(vals[0]) not in (None, 0.0)


class ColStat:
    __slots__ = ("name", "tol", "cls", "first_bad", "max_d", "max_row", "bad_rows", "first_diff", "d_first_bad")

    def __init__(self, name, tol, cls):
        self.name, self.tol, self.cls = name, tol, cls
        self.first_bad = None   # first row where the difference is over the tolerance
        self.max_d = 0.0
        self.max_row = None
        self.bad_rows = 0
        self.first_diff = None  # first row where the text differs at all
        self.d_first_bad = 0.0  # the difference in the first row over the tolerance (tells an offset that stays from a difference that grows)


def compare_tol(a, b, align=None, tol_spec="", clocks=False, rng=False, strict=False, ignore=(), only=None, report=8, as_json=False, out=print):
    """Two state logs of format v2 (the product's): the first row, column and values that differ by more than the column's tolerance.
    Returns 0 identical within tolerance, 1 diverges. Raises DiffError for bad input."""
    if as_json:
        out = lambda *args, **kwargs: None  # noqa: E731  (the report is the JSON line)
    tol = Tolerances(tol_spec, strict=strict)
    cols = [c for c in a.cols if c in b.index]
    if a.cols != b.cols:
        out(f"note: the logs have different columns; comparing the {len(cols)} they share")
    notes = []
    out(f"A: {a.path}\n   {len(a.rows)} rows; {a.describe_v2()}")
    out(f"B: {b.path}\n   {len(b.rows)} rows; {b.describe_v2()}")
    if not a.rows or not b.rows:
        raise DiffError("a log has no rows")
    for tag, lg in (("A", a), ("B", b)):
        if "lost" in lg.index:
            lost = sum(int(v) for v in lg.col("lost") if v.isdigit())
            if lost:
                notes.append(f"{tag} lost {lost} row(s) to a slow writer (the `lost` column): a gap in its rows, and a comparison at one row index is off after it")
        if lg.version == "v2" and not lg.trailer:
            notes.append(f"{tag} has no #end line: the game stopped before the log was closed (normal for mcla_state_log, which stays open until the game exits; the last rows may be missing)")
        elif lg.trailer.get("dropped", "0") not in ("0", ""):
            notes.append(f"{tag} was closed with {lg.trailer.get('dropped')} dropped row(s)")

    # alignment: the scene's own start by default (the two takes press nothing at the same wall time)
    sa, ra_run = scene_rows(a)
    sb, rb_run = scene_rows(b)
    how = ""
    until_scene_end = False
    if align is None:
        align = "scene" if (sa is not None and sb is not None) else "tick"
        if align == "tick":
            notes.append("neither log (or only one) has a running scene, so the rows are lined up by row number; use --align mark=NAME or col=EXPR for another start")
    if align == "scene":
        if sa is None or sb is None:
            raise DiffError(f"--align scene: {'A' if sa is None else 'B'} has no row with the scene running (scene_state 3); the log was not taken with a scene")
        ia, ib = sa, sb
        until_scene_end = True
        how = f"scene: the first row with the scene running: A row {ia}, B row {ib}; the scene runs for {ra_run} rows in A and {rb_run} in B"
    else:
        ia, ib = first_row(a, align), first_row(b, align)
        how = f"{align}: A row {ia}, B row {ib}" if align != "tick" else "tick: row i of A against row i of B"
    n = min(len(a.rows) - ia, len(b.rows) - ib)
    if until_scene_end:
        n_scene = min(ra_run, rb_run)
        n = min(n, n_scene)
    out(f"alignment: {how}")
    if n <= 0:
        raise DiffError("the aligned logs do not overlap")

    fixed = is_fixed_step(a, ia, n) and is_fixed_step(b, ib, n) and a.rows[ia][a.index["dt"]] == b.rows[ib][b.index["dt"]]
    in_verdict = set()
    skip = set(ignore)
    rx = re.compile(only) if only else None
    for c in cols:
        cls = col_class(c)
        if cls == "meta" or c in skip or (rx and not rx.search(c)):
            continue
        if cls == "clock" and not (clocks or fixed or strict):
            continue
        if cls == "rng" and not (rng or strict):
            continue
        if cls == "hash" and not strict:
            continue
        in_verdict.add(c)
    rel_cols = [c for c in cols if c in RELATIVE and c not in skip and not (rx and not rx.search(c))]
    cmp_cols = [c for c in cols if c in in_verdict and c not in RELATIVE]
    side_cols = [c for c in cols if c not in in_verdict and c not in RELATIVE and col_class(c) in ("clock", "rng") and c not in skip and not (rx and not rx.search(c))]
    order = {c: i for i, c in enumerate(cols)}
    out(f"compared: {n} rows, {len(cmp_cols) + len([c for c in rel_cols if c in in_verdict])} columns; tolerances: {tol.line()}")
    if fixed:
        out("fixed step: both logs have one dt for every row, so the rows are at the same game times and the clocks are compared too")
    elif not (clocks or strict):
        out("real time: the clocks are not compared (the two takes' rows are at different game times; only a still world, or a take under a fixed step, compares row for row)")
    if not (rng or strict):
        out("the generator's state (rng) is not compared: it is seeded from the clock (E1b); --rng compares it")
    stats = {c: ColStat(c, tol.of(c), col_class(c)) for c in cmp_cols}
    side = {c: ColStat(c, 0.0, col_class(c)) for c in side_cols}
    ai = [a.index[c] for c in cmp_cols]
    bi = [b.index[c] for c in cmp_cols]
    asi = [a.index[c] for c in side_cols]
    bsi = [b.index[c] for c in side_cols]
    for k in range(n):
        ra, rb = a.rows[ia + k], b.rows[ib + k]
        ta = [ra[i] for i in ai]
        tb = [rb[i] for i in bi]
        if ta != tb:
            for j, c in enumerate(cmp_cols):
                if ta[j] == tb[j]:
                    continue
                st = stats[c]
                if st.first_diff is None:
                    st.first_diff = k
                d = value_diff(ta[j], tb[j])
                if d is None:
                    continue
                if d > st.max_d:
                    st.max_d, st.max_row = d, k
                if d > st.tol:
                    st.bad_rows += 1
                    if st.first_bad is None:
                        st.first_bad = k
                        st.d_first_bad = d
        if side_cols:
            sa_, sb_ = [ra[i] for i in asi], [rb[i] for i in bsi]
            if sa_ != sb_:
                for j, c in enumerate(side_cols):
                    if sa_[j] == sb_[j]:
                        continue
                    st = side[c]
                    if st.first_diff is None:
                        st.first_diff = k
                    d = value_diff(sa_[j], sb_[j])
                    if d is not None and d > st.max_d:
                        st.max_d, st.max_row = d, k

    # the clocks that count from the start of the log: as elapsed time since the first compared row, with the float32 noise of a large value allowed
    rel_vals = {}
    for c in rel_cols:
        st = ColStat(c, tol.of(c) if c in in_verdict else 0.0, "clock")
        (stats if c in in_verdict else side)[c] = st
        va, vb = a.num(c)[ia:ia + n], b.num(c)[ib:ib + n]
        a0, b0 = va[0], vb[0]
        rel_vals[c] = (a0, b0)
        for k in range(n):
            d = abs((va[k] - a0) - (vb[k] - b0))
            if d != d:
                d = float("inf")
            slack = 0.0 if c == "frame_ctr" else 2.0 * (f32_ulp(va[k]) + f32_ulp(vb[k]))
            if d > slack:
                if st.first_diff is None:
                    st.first_diff = k
                if d > st.max_d:
                    st.max_d, st.max_row = d, k
                if d > st.tol + slack:
                    st.bad_rows += 1
                    if st.first_bad is None:
                        st.first_bad = k
                        st.d_first_bad = d

    scene_t = a.num("scene_t") if "scene_t" in a.index else None

    def when(k):
        if scene_t is not None and ia + k < len(scene_t) and scene_t[ia + k] >= 0:
            return f"scene time {scene_t[ia + k]:.3f} s"
        return f"row {ia + k} of A"

    def ticks(k):
        ta_ = a.rows[ia + k][a.index["tick"]] if "tick" in a.index else str(ia + k)
        tb_ = b.rows[ib + k][b.index["tick"]] if "tick" in b.index else str(ib + k)
        return ta_, tb_

    bad = sorted((s for s in stats.values() if s.first_bad is not None), key=lambda s: (s.first_bad, order[s.name]))
    result = {"verdict": "", "rows": n, "align": how, "fixed_step": fixed, "notes": notes, "columns": len(cmp_cols), "first": None, "max": {}, "outside": {}}
    for c in cmp_cols + [r for r in rel_cols if r in in_verdict]:
        if stats[c].first_diff is not None:
            result["max"][c] = {"max": stats[c].max_d, "row": stats[c].max_row, "tol": stats[c].tol, "first_bad": stats[c].first_bad}
    side_names = sorted(side, key=lambda c: order[c])   # the clocks and the generator that are not in the verdict
    for c in side_names:
        result["outside"][c] = {"first_diff": side[c].first_diff, "max": side[c].max_d}

    def side_report():
        shown = []
        for c in side_names:
            s = side[c]
            if s.first_diff is None:
                shown.append(f"{c}: equal")
            else:
                shown.append(f"{c}: differs from row {s.first_diff} (largest {s.max_d:g})")
        return shown

    if not as_json:
        for note in notes:
            out(f"note: {note}")
    verdict_line = ""
    if not bad:
        biggest = sorted((s for s in stats.values() if s.max_d > 0 and s.first_diff is not None), key=lambda s: -(s.max_d / (s.tol or 1e-12)))[:4]
        text_equal = all(s.first_diff is None for s in stats.values())
        dur = ""
        if scene_t is not None and scene_t[ia + n - 1] >= 0 and scene_t[ia] >= 0:
            dur = f" ({scene_t[ia + n - 1] - scene_t[ia]:.2f} s of scene time)"
        if text_equal:
            verdict_line = f"VERDICT: IDENTICAL over {n} rows{dur}: every compared value is the same text (bit for bit)"
            if ("chain" in a.index and "chain" in b.index and ia == 0 and ib == 0 and len(a.rows) == len(b.rows)
                    and a.rows[-1][a.index["chain"]] == b.rows[-1][b.index["chain"]]):
                verdict_line += "; the final chain hashes agree too (every row, from the first)"
        else:
            verdict_line = f"VERDICT: IDENTICAL within tolerance over {n} rows{dur}; largest differences: " + ", ".join(
                f"{s.name} {s.max_d:.3g} (tolerance {s.tol:g}, row {s.max_row})" for s in biggest)
        result["verdict"] = "identical" if text_equal else "within tolerance"
        out("")
        for line in side_report():
            out(f"not in the verdict, {line}")
        out(verdict_line)
        if as_json:
            print(json.dumps(result))
        return 0

    k = bad[0].first_bad
    ta_, tb_ = ticks(k)
    mark_a = a.rows[ia + k][a.index["mark"]] if "mark" in a.index else ""
    out(f"\nFIRST DIVERGENCE: row {k} of the comparison (A tick {ta_}, B tick {tb_}), {when(k)}" + (f"; the row is marked {mark_a!r} in A" if mark_a else ""))
    here = [s for s in bad if s.first_bad == k]
    groups = sorted({s.cls for s in here}, key=CLASS_ORDER.index)
    out(f"  {len(here)} column(s) over their tolerance in this row (classes: {', '.join(groups)}):")
    for s in here[:report]:
        va, vb = a.rows[ia + k][a.index[s.name]], b.rows[ib + k][b.index[s.name]]
        if s.name in RELATIVE:
            ea = float(va) - rel_vals[s.name][0]
            eb = float(vb) - rel_vals[s.name][1]
            out(f"    {s.name}: elapsed since the first compared row: A {ea:.9g} | B {eb:.9g}  (diff {eb - ea:+.6g}, tolerance {s.tol:g}; the values: A {va} | B {vb})")
        elif num_or_none(va) is None or num_or_none(vb) is None:
            out(f"    {s.name}: A {va!r} | B {vb!r}")
        else:
            out(f"    {s.name}: A {va} | B {vb}  (diff {float(vb) - float(va):+.6g}, tolerance {s.tol:g})")
    if len(here) > report:
        out(f"    ... and {len(here) - report} more (the table below)")
    out("\nPER-COLUMN (columns that go over their tolerance, by first row; largest difference and how many rows are over):")
    for s in bad[:40]:
        out(f"  row {s.first_bad:>7}  {s.name:<14} largest {s.max_d:.6g} at row {s.max_row} (tolerance {s.tol:g}), {s.bad_rows} row(s) over")
    if len(bad) > 40:
        out(f"  ... {len(bad) - 40} more columns")
    if side_names:
        out("")
        for line in side_report():
            out(f"not in the verdict, {line}")
    first_cols = [s.name for s in here]
    verdict_line = f"VERDICT: DIVERGES at row {k} ({when(k)}): {', '.join(first_cols[:4])}" + (f" (and {len(first_cols) - 4} more)" if len(first_cols) > 4 else "")
    result["verdict"] = "diverges"
    s0 = here[0]
    va, vb = a.rows[ia + k][a.index[s0.name]], b.rows[ib + k][b.index[s0.name]]
    result["first"] = {"row": k, "tick_a": ta_, "tick_b": tb_, "column": s0.name, "a": va, "b": vb, "tolerance": s0.tol, "columns": first_cols}
    out(verdict_line)
    early = set(first_cols)
    if early & {"scene_state"}:
        out("  reading: the scene's state differs at this row: one take's scene stopped or paused before the other's.")
    elif any(col_class(c) in ("pos", "rot", "speed") and c.startswith("p_") for c in early):
        out("  reading: the player's car differs first: the physics (a different start state, thread order) or the input.")
    elif any(re.fullmatch(r"c\d+_.*", c) for c in early):
        out("  reading: a scene car differs first: its spawn place, its path or the physics.")
    elif any(c.startswith("cam_") for c in early):
        out("  reading: only the camera block differs first: the chase camera's own state, not the world.")
    elif "hour" in early:
        out("  reading: the hour differs: the day cycle is not pinned (`world hour ... hold` pins it).")
    # an offset the takes start with (the pose the car settled into before the scene: nothing resets it at `go`) or a difference that grows while the scene runs
    grown = [s for s in bad if s.max_d > 1.5 * s.d_first_bad]
    if not grown:
        out(f"  growth: none: the {len(bad)} column(s) over their tolerance never differ by more than 1.5 times their first difference over it (largest {max(s.max_d for s in bad):.3g}); "
            + ("a constant offset the two takes started with, not a divergence while the scene ran." if k == 0 else "a step at this row that then stays, not a difference that grows."))
        result["growth"] = "constant"
    else:
        worst = max(grown, key=lambda s: s.max_d / (s.d_first_bad or 1e-30))
        out(f"  growth: {len(grown)} of {len(bad)} column(s) grow after their first row over the tolerance; {worst.name} from {worst.d_first_bad:.3g} in row {worst.first_bad} to {worst.max_d:.3g} in row {worst.max_row}: the takes drift apart while the scene runs.")
        result["growth"] = "grows"
    if as_json:
        print(json.dumps(result))
    return 1


# ------------------------------------------------------------------------------------------------------------------ self test
SELF_COLS = ["tick", "mark", "wall_ms", "step_ticks", "dt", "frame_ctr", "clock_total", "clock_game", "time_scale", "substeps",
             "mode_timer", "rng", "hour", "weather", "paused", "live", "p_ok", "p_m12", "p_m13", "p_m14", "p_speed", "p_vx",
             "n_cars", "c0_x", "c0_z", "cam_ok", "cam_m12", "amb_en", "traffic", "amb_hash", "rec_mode", "rec_pos",
             "audio_clock", "audio_ms", "h_player", "h_racers", "h_world", "h_cam", "h_amb", "h_clock", "hash", "chain"]


def _h(*vals):
    h = 1469598103934665603
    for v in vals:
        for byte in str(v).encode():
            h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def synth(path, n=400, start=0, car_dx=None, rng_every=None, mark_at=None, dt=1 / 60, comment=True):
    """A made-up log: a car that accelerates, a camera that follows it, a random state that steps every 7 rows."""
    rows = []
    chain = 0
    rng = 12345
    x = 100.0
    v = 0.0
    sim = 0.0
    for i in range(n):
        k = i + start
        v += 3.0 * dt
        x += v * dt
        if rng_every and i and i % rng_every == 0:
            rng = (rng * 1664525 + 1013904223) & 0xFFFFFFFF
        px = x + (car_dx[1] if car_dx and i >= car_dx[0] else 0.0)
        cam = px - 6.0
        mark = "go" if mark_at is not None and i == mark_at else ""
        d = {"tick": i, "mark": mark, "wall_ms": f"{i * 3.1:.3f}", "step_ticks": 831250, "dt": f"{dt:.9g}", "frame_ctr": k,
             "clock_total": f"{k * dt:.9g}", "clock_game": f"{k * dt:.9g}", "time_scale": 1, "substeps": 1, "mode_timer": f"{k * dt:.9g}",
             "rng": rng, "hour": 12, "weather": 0, "paused": 0, "live": 1, "p_ok": 1, "p_m12": f"{px:.9g}", "p_m13": 17.5, "p_m14": -700,
             "p_speed": f"{v:.9g}", "p_vx": f"{v:.9g}", "n_cars": 0, "c0_x": 0, "c0_z": 0, "cam_ok": 1, "cam_m12": f"{cam:.9g}",
             "amb_en": 0, "traffic": -1, "amb_hash": "0000000000000000", "rec_mode": 0, "rec_pos": 0, "audio_clock": f"{k * dt:.9g}",
             "audio_ms": int(k * dt * 1000)}
        d["h_player"] = _h(d["p_m12"], d["p_speed"])
        d["h_racers"] = _h(d["n_cars"])
        d["h_world"] = _h(d["rng"], d["mode_timer"])
        d["h_cam"] = _h(d["cam_m12"])
        d["h_amb"] = _h(d["traffic"])
        d["h_clock"] = _h(d["dt"], d["frame_ctr"])
        d["hash"] = _h(d["h_player"], d["h_racers"], d["h_world"] if False else d["rng"], d["h_cam"])  # no clocks
        chain = _h(chain, d["hash"])
        d["chain"] = chain
        rows.append([str(d[c]) for c in SELF_COLS])
        sim += dt
    with open(path, "w", encoding="utf-8", newline="") as f:
        if comment:
            f.write(f"#cin_state v1 fixed_step_fps={1 / dt:g} step_ticks=831250 tick_seconds=2.0050125e-08\n")
        f.write(",".join(SELF_COLS) + "\n")
        for r in rows:
            f.write(",".join(r) + "\n")


def selftest(exe=None):
    import io
    import contextlib
    tmp = tempfile.mkdtemp(prefix="take_diff_selftest_")
    p = lambda name: os.path.join(tmp, name)
    fails = []

    def run(argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    def check(name, cond, text=""):
        if not cond:
            fails.append(name)
            print(f"FAIL {name}\n{text}")
        else:
            print(f"ok   {name}")

    synth(p("a.csv"))
    synth(p("a2.csv"))
    rc, text = run([p("a.csv"), p("a2.csv")])
    check("identical logs", rc == 0 and "IDENTICAL over 400 rows" in text and "chain hashes agree" in text, text)

    synth(p("b.csv"), car_dx=(120, 1e-4))
    rc, text = run([p("a.csv"), p("b.csv")])
    check("a car that moves 1e-4 at row 120 diverges there", rc == 1 and "FIRST DIVERGENCE: row 120 " in text and "p_m12" in text and "VERDICT: DIVERGES at row 120" in text, text)
    check("   ... the player and camera groups, not the world", "groups that differ: player, cam" in text, text)
    check("   ... and the ulp distance is shown", " ulp" in text, text)

    synth(p("c.csv"), rng_every=7)
    synth(p("c2.csv"), rng_every=7)
    synth(p("d.csv"), rng_every=5)
    rc, text = run([p("c.csv"), p("c2.csv")])
    check("same random draws are identical", rc == 0, text)
    rc, text = run([p("c.csv"), p("d.csv")])
    check("a different draw rate names the rng", rc == 1 and "rng" in text and "random number state" in text, text)

    # B started 30 rows later: the world is the same 30 frames older, the clocks run 30 rows ahead
    synth(p("e.csv"), n=500)
    synth(p("f.csv"), n=470, start=0)
    # make F's states equal E's rows 30.. by building F from E's rows 30..: car at the state of tick 30
    import shutil
    lines = open(p("e.csv"), encoding="utf-8").read().splitlines()
    head = lines[:2]
    body = lines[2 + 30:]
    open(p("f.csv"), "w", encoding="utf-8", newline="").write("\n".join(head + body) + "\n")
    rc, text = run([p("e.csv"), p("f.csv")])
    check("a shifted log diverges at tick alignment", rc == 1, text)
    rc, text = run([p("e.csv"), p("f.csv"), "--shift", "auto"])
    check("--shift auto finds the 30 rows and agrees", rc == 0 and "shift auto: B row i-30 against A row i" in text and "IDENTICAL over 470 rows" in text, text)
    rc, text = run([p("e.csv"), p("f.csv"), "--shift", "-30"])
    check("--shift -30 is identical too", rc == 0 and "IDENTICAL over 470 rows" in text, text)
    rc, text = run([p("e.csv"), p("f.csv"), "--shift", "-29"])
    check("--shift -29 is not", rc == 1, text)

    synth(p("m1.csv"), mark_at=50)
    synth(p("m2.csv"), mark_at=50)
    rc, text = run([p("m1.csv"), p("m2.csv"), "--align", "mark=go"])
    check("align on a mark", rc == 0 and "mark=go: A row 50, B row 50" in text, text)
    rc, text = run([p("m1.csv"), p("m2.csv"), "--align", "mark=nope"])
    check("an unknown mark is an error", rc == 2, text)
    rc, text = run([p("m1.csv"), p("m2.csv"), "--align", "col=mode_timer>=1.0"])
    check("align on a column condition", rc == 0 and "A row 60" in text, text)

    rc, text = run(["--curve", p("a.csv"), "--at", "1,2,4", "--cols", "p_speed,p_m12"])
    check("curve table", rc == 0 and "audio clock (0x8286CF34) advanced" in text and "ratio 1.00000" in text and "p_speed" in text, text)
    rc, text = run([p("a.csv"), p("nonexistent.csv")])
    check("a missing file is an error", rc == 2, text)

    selftest_v2(run, check, p, exe)

    shutil.rmtree(tmp, ignore_errors=True)
    print("take_diff selftest:", "ALL OK" if not fails else f"{len(fails)} FAILED: {fails}")
    return 1 if fails else 0


# ------------------------------------------------------------------------------------------------------------------ self test: the product log (v2)
def v2_columns():
    cols = ["tick", "mark", "wall_ms", "lost", "scene_state", "scene_t", "dt", "dt_unscaled", "time_scale", "frame_ctr", "clock_total",
            "clock_game", "mode_timer", "rng", "hour", "weather", "paused", "live", "p_ok"]
    cols += [f"p_m{i}" for i in POSE_WORDS] + ["p_speed", "cam_ok"] + [f"cam_m{i}" for i in POSE_WORDS] + ["n_cars"]
    for c in range(MAX_CARS):
        cols += [f"c{c}_id", f"c{c}_ok"] + [f"c{c}_m{i}" for i in POSE_WORDS]
    return cols + ["h_player", "h_cars", "h_world", "h_cam", "hash", "chain"]


def synth2(path, rows=300, wait=20, dt=1 / 60, fixed=True, jitter=0.0, drift=None, car_drift=None, rng_seed=12345, rng_jump=None, mark_at=None,
           finish_at=None, lost_at=None, closed=True, clock_skew=0.0, scene=True, extra_col=None, speed=0.00022, n_cars=2, rot_off=None):
    """A made-up v2 log: `wait` rows with the scene waiting, then the scene running; a parked player car (x jitters by `jitter` metres, from `drift`=(row,
    metres) on it has moved), two scene cars, a camera that never moves. `fixed` = dt is one value; else it varies a little from row to row."""
    cols = v2_columns() + ([extra_col] if extra_col else [])
    lines = [f"#statelog v2 scene={'park' if scene else '-'} started=20261113-120000 fixed_step_fps={1 / dt:g} step_ticks=831250 tick_seconds=2.005012512e-08" if fixed
             else "#statelog v2 scene=park started=20261113-120000 fixed_step_fps=0 step_ticks=0 tick_seconds=2.005012512e-08", ",".join(cols)]
    chain = 0
    lost_pending = 0
    for i in range(rows):
        running = scene and i >= wait
        j = i - wait
        state = 0 if not scene else (2 if i < wait else (4 if finish_at is not None and j >= finish_at else 3))
        step = dt if fixed else dt * (1.0 + 0.01 * ((i * 7) % 5 - 2))
        t_scene = j * dt if state in (3, 4) else -1.0
        px = 819.610535 + (jitter * ((i % 3) - 1) if jitter else 0.0) + (drift[1] if drift and j >= drift[0] and running else 0.0)
        v = {"tick": i, "mark": "go" if mark_at is not None and i == mark_at else "", "wall_ms": f"{i * 16.7:.3f}", "lost": lost_pending,
             "scene_state": state, "scene_t": f"{t_scene:.6f}", "dt": f"{step:.9g}", "dt_unscaled": f"{step:.9g}", "time_scale": 1, "frame_ctr": 1000 + i,
             "clock_total": f"{(i * dt * (1 + clock_skew)):.9g}", "clock_game": f"{(i * dt):.9g}", "mode_timer": f"{(30 + i * dt * (1 + clock_skew)):.9g}",
             "rng": rng_seed + i + (rng_jump or 0), "hour": 17.3999996, "weather": 0, "paused": 0, "live": 1, "p_ok": 1,
             "p_speed": f"{speed:.9g}", "cam_ok": 1, "n_cars": n_cars}
        pose = {0: -0.880069196, 1: 0.0443580896, 2: -0.472769111, 4: 0.0215489343, 5: 0.998332322, 6: 0.0535558723, 8: 0.474356323, 9: 0.0369452015,
                10: -0.879557371, 12: px, 13: 17.5570717, 14: -808.407166}
        if rot_off:   # (offset, slope a scene row): the player's rotation cell p_m1 is `offset` off in every row (a rest pose from before the scene) and drifts by `slope` a row of the scene
            pose[1] += rot_off[0] + rot_off[1] * max(0, j)
        for w in POSE_WORDS:
            v[f"p_m{w}"] = f"{pose[w]:.9g}"
            v[f"cam_m{w}"] = f"{[1, 0, 0, 0, 1, 0, 0, 0, 1, 815.5, 19.0, -800.25][POSE_WORDS.index(w)]:.9g}"
        for c in range(MAX_CARS):
            present = c < n_cars
            v[f"c{c}_id"] = f"p{c + 1}" if present else ""
            v[f"c{c}_ok"] = 1 if present else 0
            for w in POSE_WORDS:
                val = 0.0
                if present:
                    base = [1, 0, 0, 0, 1, 0, 0, 0, 1, 817.0 + 3.0 * c, 17.6, -797.3][POSE_WORDS.index(w)]
                    val = base + (car_drift[2] if car_drift and car_drift[0] == c and w == car_drift[3] and j >= car_drift[1] and running else 0.0)
                v[f"c{c}_m{w}"] = f"{val:.9g}"
        for g in ("h_player", "h_cars", "h_world", "h_cam", "hash"):
            v[g] = _h(g, v["p_m12"], v["c1_m14"], v["cam_m12"])
        chain = _h(chain, v["hash"])
        v["chain"] = chain
        if extra_col:
            v[extra_col] = 0
        if lost_at is not None and i == lost_at:
            lost_pending = 3
        else:
            lost_pending = 0
        lines.append(",".join(str(v[c]) for c in cols))
    if closed:
        lines.append(f"#end rows={rows} written={rows} dropped=0")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(lines) + "\n")


def selftest_v2(run, check, p, exe=None):
    """The checks of the v2 comparison. `run(argv)` -> (rc, text); `check(name, ok, text)`; `p(name)` -> a path in the temp folder."""
    synth2(p("v_a.csv"))
    synth2(p("v_a2.csv"))
    rc, text = run([p("v_a.csv"), p("v_a2.csv")])
    check("v2: identical logs", rc == 0 and "VERDICT: IDENTICAL over 280 rows" in text and "bit for bit" in text, text)
    check("v2:    ... compared while the scene runs, aligned on its start", "A row 20, B row 20" in text and "real time" not in text, text)

    synth2(p("v_j.csv"), jitter=8e-6)
    rc, text = run([p("v_a.csv"), p("v_j.csv")])
    check("v2: a micrometre-scale jitter is identical within tolerance", rc == 0 and "IDENTICAL within tolerance over 280 rows" in text and "p_m12 8e-06" in text, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--strict"])
    check("v2: --strict makes the same jitter a divergence", rc == 1 and "FIRST DIVERGENCE" in text and "p_m12" in text, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--tol", "pos=1e-6"])
    check("v2: --tol pos=1e-6 too", rc == 1 and "tolerance 1e-06" in text, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--tol", "col:^p_m12$=1e-6"])
    check("v2: --tol col:REGEX=value for one column", rc == 1 and "p_m12" in text, text)

    # the player's car moves 1 cm at scene row 120
    synth2(p("v_d.csv"), drift=(120, 0.01))
    rc, text = run([p("v_a.csv"), p("v_d.csv")])
    check("v2: a 1 cm move at scene row 120 diverges there, naming column, values and tolerance",
          rc == 1 and "FIRST DIVERGENCE: row 120 " in text and "p_m12: A 819.610535 | B 819.620535" in text and "tolerance 0.001" in text
          and "scene time 2.000 s" in text and "VERDICT: DIVERGES at row 120" in text, text)
    check("v2:    ... the reading names the player's car", "reading: the player's car differs first" in text, text)
    rc, text = run([p("v_a.csv"), p("v_d.csv"), "--tol", "pos=0.02"])
    check("v2: --tol pos=0.02 lets the same move through", rc == 0, text)
    rc, text = run([p("v_a.csv"), p("v_d.csv"), "--only", "^c"])
    check("v2: --only ^c leaves the player out", rc == 0, text)
    rc, text = run([p("v_a.csv"), p("v_d.csv"), "--ignore", "p_m12"])
    check("v2: --ignore p_m12", rc == 0, text)

    # the player's rotation: a rest pose that differs by the same amount in every row (cint2-park-1 / -2: 3.4e-5) against a difference that grows
    synth2(p("v_ro.csv"), rot_off=(3.4e-5, 0.0))
    # (the default rot tolerance is 1e-4 since 2026-11-14: the two cases that name an offset or a drift use the old, tight 1e-5 explicitly)
    rc, text = run([p("v_a.csv"), p("v_ro.csv"), "--tol", "rot=1e-5"])
    check("v2: a constant rotation offset over a tight tolerance (rot=1e-5) is named, and the reading says it does not grow",
          rc == 1 and "FIRST DIVERGENCE: row 0 " in text and "p_m1" in text and "growth: none" in text and "a constant offset the two takes started with" in text, text)
    rc, text = run([p("v_a.csv"), p("v_ro.csv")])
    check("v2:    ... and the default rotation tolerance (1e-4) lets it through", rc == 0 and "IDENTICAL within tolerance" in text, text)
    synth2(p("v_rg.csv"), rot_off=(0.0, 1.5e-6))
    rc, text = run([p("v_a.csv"), p("v_rg.csv"), "--tol", "rot=1e-5"])
    check("v2: a rotation that drifts 1.5e-6 a row diverges once over a tight tolerance (rot=1e-5), and the reading says it grows",
          rc == 1 and "FIRST DIVERGENCE: row 7 " in text and "growth: 1 of 1 column(s) grow" in text and "the takes drift apart" in text, text)

    # a scene car's z moves at its row 50
    synth2(p("v_c.csv"), car_drift=(1, 50, 0.5, 14))
    rc, text = run([p("v_a.csv"), p("v_c.csv")])
    check("v2: a scene car that moves names its column", rc == 1 and "FIRST DIVERGENCE: row 50 " in text and "c1_m14" in text and "reading: a scene car" in text, text)

    # the second take started its scene later and ran a different number of rows: still identical
    synth2(p("v_s1.csv"), rows=300, wait=20)
    synth2(p("v_s2.csv"), rows=330, wait=47)
    rc, text = run([p("v_s1.csv"), p("v_s2.csv")])
    check("v2: aligned on the scene's start (A row 20, B row 47); the shorter run decides the length",
          rc == 0 and "A row 20, B row 47" in text and "compared: 280 rows" in text and "the scene runs for 280 rows in A and 283 in B" in text, text)
    # one scene finishes sooner: only the rows where both run are compared
    synth2(p("v_f.csv"), rows=300, finish_at=100)
    rc, text = run([p("v_a.csv"), p("v_f.csv")])
    check("v2: a scene that finished sooner ends the comparison", rc == 0 and "compared: 100 rows" in text, text)

    # real time: dt varies, so the clocks (and the generator) are outside the verdict; fixed step: they are in it
    synth2(p("v_r1.csv"), fixed=False, rng_seed=1)
    synth2(p("v_r2.csv"), fixed=False, rng_seed=99, clock_skew=0.013)   # the clock runs 1.3% fast: elapsed times differ
    rc, text = run([p("v_r1.csv"), p("v_r2.csv")])
    check("v2: real time: a different clock and generator are outside the verdict", rc == 0 and "real time: the clocks are not compared" in text
          and "not in the verdict, rng: differs from row 0" in text and "not in the verdict, clock_total: differs from row 1" in text, text)
    rc, text = run([p("v_r1.csv"), p("v_r2.csv"), "--rng"])
    check("v2: --rng puts the generator in the verdict", rc == 1 and "rng" in text, text)
    rc, text = run([p("v_r1.csv"), p("v_r2.csv"), "--clocks"])
    check("v2: --clocks puts the clocks in the verdict", rc == 1 and "FIRST DIVERGENCE" in text, text)
    synth2(p("v_x1.csv"), clock_skew=0.0)
    synth2(p("v_x2.csv"), clock_skew=0.013)
    rc, text = run([p("v_x1.csv"), p("v_x2.csv")])
    check("v2: a fixed step compares the clocks without being asked", rc == 1 and "fixed step: both logs have one dt" in text and "clock_total" in text, text)
    synth2(p("v_o1.csv"))
    synth2(p("v_o2.csv"))
    lines = open(p("v_o2.csv"), encoding="utf-8").read().splitlines()
    cols_ = lines[1].split(",")
    fi, ci, mi = cols_.index("frame_ctr"), cols_.index("clock_total"), cols_.index("mode_timer")
    shifted = lines[:2]
    for ln in lines[2:]:
        if ln.startswith("#") or not ln:
            shifted.append(ln)
            continue
        f = ln.split(",")
        f[fi] = str(int(f[fi]) + 5000)
        f[ci] = f"{float(f[ci]) + 90.0:.9g}"
        f[mi] = f"{float(f[mi]) + 90.0:.9g}"
        shifted.append(",".join(f))
    open(p("v_o2.csv"), "w", encoding="utf-8", newline="").write(chr(10).join(shifted) + chr(10))
    rc, text = run([p("v_o1.csv"), p("v_o2.csv")])
    check("v2: clocks are compared as elapsed time: a take that started at another game age matches", rc == 0 and "the clocks are compared too" in text, text)

    synth2(p("v_g1.csv"), rng_seed=1)
    synth2(p("v_g2.csv"), rng_seed=1, rng_jump=5)
    rc, text = run([p("v_g1.csv"), p("v_g2.csv")])
    check("v2: the generator alone does not fail a take", rc == 0 and "not in the verdict, rng: differs from row 0" in text, text)

    # notes: dropped rows, a log the game did not close, the legacy exact mode, a bad spec, no scene
    synth2(p("v_l.csv"), lost_at=60, closed=False)
    rc, text = run([p("v_a.csv"), p("v_l.csv")])
    check("v2: lost rows and a missing #end are noted", rc == 0 and "lost 3 row(s)" in text and "B has no #end line" in text, text)
    rc, text = run([p("v_a.csv"), p("v_a2.csv"), "--exact"])
    check("v2: --exact is the bit-for-bit text comparison", rc == 0 and "IDENTICAL over" in text, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--exact"])
    check("v2: --exact sees the jitter", rc == 1 and "FIRST DIVERGENCE" in text, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--tol", "pos"])
    check("v2: a bad --tol item is an error", rc == 2, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--tol", "nope=1"])
    check("v2: an unknown tolerance class is an error", rc == 2, text)
    rc, text = run([p("v_a.csv"), p("v_j.csv"), "--tol", "pos=-1"])
    check("v2: a negative tolerance is an error", rc == 2, text)
    synth2(p("v_n.csv"), scene=False)
    rc, text = run([p("v_a.csv"), p("v_n.csv"), "--align", "scene"])
    check("v2: --align scene on a log without a scene is an error", rc == 2, text)
    synth2(p("v_n2.csv"), scene=False)
    rc, text = run([p("v_n.csv"), p("v_n2.csv")])
    check("v2: two logs without a scene line up by row number", rc == 0 and "lined up by row number" in text, text)
    synth2(p("v_m1.csv"), mark_at=40)
    synth2(p("v_m2.csv"), mark_at=40)
    rc, text = run([p("v_m1.csv"), p("v_m2.csv"), "--align", "mark=go"])
    check("v2: --align mark=go", rc == 0 and "mark=go: A row 40, B row 40" in text, text)
    synth2(p("v_e.csv"), extra_col="zz_new")
    rc, text = run([p("v_a.csv"), p("v_e.csv")])
    check("v2: a column only one log has is left out, with a note", rc == 0 and "different columns" in text, text)

    # json
    rc, text = run([p("v_a.csv"), p("v_d.csv"), "--json"])
    try:
        import json as _json
        doc = _json.loads(text.strip().splitlines()[-1])
        check("v2: --json says the verdict and the first divergence", rc == 1 and doc["verdict"] == "diverges" and doc["first"]["column"] == "p_m12" and doc["first"]["row"] == 120
              and doc["rows"] == 280, text)
    except Exception as e:  # noqa: BLE001
        check("v2: --json is one JSON line", False, f"{e}: {text}")
    rc, text = run([p("v_a.csv"), p("v_a2.csv"), "--json"])
    check("v2: --json of identical logs", rc == 0 and '"verdict": "identical"' in text, text)

    # the growth table understands v2
    synth2(p("v_d2.csv"), drift=(120, 0.02))
    rc, text = run([p("v_a.csv"), p("v_d2.csv"), "--growth", "--at", "1,3"])
    check("v2: --growth reads the player and the scene cars", rc == 0 and "player more than 1 cm apart: row 120" in text, text)

    # logs made by the game's own formatter and writer (state_log_test --sample): the format this tool reads is the format the game writes
    if exe and os.path.exists(exe):
        import subprocess
        for name, extra in (("g_a.csv", []), ("g_b.csv", []), ("g_d.csv", ["80", "0.0005"]), ("g_e.csv", ["80", "0.004"])):
            cp = subprocess.run([exe, "--sample", p(name), "120"] + extra, capture_output=True, text=True)
            check(f"game writer: {name} written", cp.returncode == 0 and os.path.exists(p(name)), cp.stdout + cp.stderr)
        la = Log(p("g_a.csv"))
        check("game writer: the columns are the ones this tool knows", la.cols == v2_columns(), f"{len(la.cols)} vs {len(v2_columns())}: {[c for c in la.cols if c not in v2_columns()][:5]}")
        check("game writer: the preamble and trailer are read", la.version == "v2" and la.trailer.get("rows") == "120" and la.meta.get("scene") == "sample", str((la.version, la.trailer, la.meta)))
        rc, text = run([p("g_a.csv"), p("g_b.csv")])
        check("game writer: two identical samples are identical, bit for bit", rc == 0 and "IDENTICAL over" in text and "bit for bit" in text and "fixed step" in text, text)
        rc, text = run([p("g_a.csv"), p("g_d.csv")])
        check("game writer: 0.5 mm of drift is within the default tolerance", rc == 0 and "within tolerance" in text, text)
        rc, text = run([p("g_a.csv"), p("g_e.csv")])
        check("game writer: 4 mm of drift at row 80 diverges there", rc == 1 and "FIRST DIVERGENCE: row 80 " in text and "p_m12" in text, text)
        rc, text = run([p("g_a.csv"), p("g_e.csv"), "--align", "mark=go"])
        check("game writer: the mark is read (row 30)", "mark=go: A row 30, B row 30" in text, text)


# ------------------------------------------------------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description="Compare two state logs: the product's per-update log (#statelog v2) with tolerances, or the experiment's (#cin_state v1) bit for bit. See the top of this file.")
    ap.add_argument("logs", nargs="*", help="A.csv B.csv (or, with --curve, one or more logs)")
    ap.add_argument("--align", default=None, help="scene (the first row with the scene running: the default for the product log when both have one) | tick (the experiment's default) | mark=LABEL | col=NAME>=VALUE | reset=NAME>=VALUE (after the last big drop of NAME)")
    ap.add_argument("--tol", default="", help="product log: tolerances, comma separated: CLASS=VALUE (pos rot speed campos camrot hour world count other clock rng hash) or col:REGEX=VALUE; 0 = equal")
    ap.add_argument("--clocks", action="store_true", help="product log: compare the clocks (dt, clock_*, mode_timer, scene_t) even in real time")
    ap.add_argument("--rng", action="store_true", help="product log: compare the generator's state")
    ap.add_argument("--strict", action="store_true", help="product log: every column, hashes and generator included, bit for bit")
    ap.add_argument("--exact", action="store_true", help="the bit-for-bit text comparison (the only mode for the experiment's logs), also for the product log")
    ap.add_argument("--json", action="store_true", help="product log: print the verdict as one JSON line instead of the report")
    ap.add_argument("--exe", default=None, help="--selftest: also check against logs written by the game's own formatter (state_log_test.exe --sample)")
    ap.add_argument("--shift", default=None, help="auto, or a number of rows (B's row i+N against A's row i)")
    ap.add_argument("--ignore", default="", help="columns to leave out, comma separated")
    ap.add_argument("--only", default=None, help="compare only the columns matching this regex (plus the group hashes)")
    ap.add_argument("--keep-clocks", action="store_true", help="with --shift: compare the clock columns too")
    ap.add_argument("--report", type=int, default=12, help="columns listed for the first diverging row (default 12)")
    ap.add_argument("--curve", action="store_true", help="the E2 table over several logs")
    ap.add_argument("--at", default="1,2,5,10", help="--curve: seconds of stepped game time, comma separated")
    ap.add_argument("--cols", default="p_speed,p_m12,p_m14", help="--curve: the columns to show")
    ap.add_argument("--from", dest="start", default="tick", help="--curve: where each log starts: tick | mark=LABEL | col=EXPR")
    ap.add_argument("--growth", action="store_true", help="E1b: how far apart two runs are at the --at times after --align (player position, race cars by nearest match)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest(a.exe)
    try:
        if a.curve:
            if not a.logs:
                ap.error("--curve needs at least one log")
            return curve(a.logs, [float(x) for x in a.at.split(",") if x.strip()], [c.strip() for c in a.cols.split(",") if c.strip()], a.start)
        if a.growth:
            if len(a.logs) != 2:
                ap.error("--growth: two logs: A.csv B.csv")
            la, lb = Log(a.logs[0]), Log(a.logs[1])
            align = a.align
            if align is None:
                align = "scene" if (la.version == "v2" and lb.version == "v2" and scene_rows(la)[0] is not None and scene_rows(lb)[0] is not None) else "tick"
            return growth(la, lb, align, [float(x) for x in a.at.split(",") if x.strip()])
        if len(a.logs) != 2:
            ap.error("two logs: A.csv B.csv")
        shift = a.shift
        if shift is not None and shift != "auto":
            shift = int(shift)
        la, lb = Log(a.logs[0]), Log(a.logs[1])
        ignore = {c.strip() for c in a.ignore.split(",") if c.strip()}
        if la.version == "v2" and lb.version == "v2" and not a.exact and shift is None:
            return compare_tol(la, lb, align=a.align, tol_spec=a.tol, clocks=a.clocks, rng=a.rng, strict=a.strict, ignore=ignore, only=a.only,
                               report=a.report, as_json=a.json)
        if a.tol or a.clocks or a.rng or a.strict or a.json:
            print("take_diff: --tol, --clocks, --rng, --strict and --json are for two logs of the product's format (#statelog v2); "
                  "this comparison is bit for bit", file=sys.stderr)
            if not (la.version == "v2" and lb.version == "v2"):
                return 2
        return compare(la, lb, align=a.align or "tick", shift=shift, ignore=ignore, only=a.only, keep_clocks=a.keep_clocks, report=a.report)
    except (DiffError, OSError) as e:
        print(f"take_diff: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
