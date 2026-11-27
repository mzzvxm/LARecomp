"""Loopback Director protocol client and offline mock server.

See documentation/DIRECTOR-PROTOCOL.md for the command and event formats.
"""
import argparse
import json
import os
import queue
import socket
import struct
import sys
import threading
import time
import zlib
import hmac
import secrets
from pathlib import Path

PROTO = 1
MAX_LINE = 1 << 20
ERROR_CODES = ("bad_args", "unknown_cmd", "not_ready", "no_world", "stale", "refused", "timeout", "failed")


class DirectorError(Exception):
    def __init__(self, code, msg, details=None):
        super().__init__(f"{code}: {msg}")
        self.code = code
        self.msg = msg
        self.details = details or {}


# ---------------------------------------------------------------------------------------------------------------------------- the client

class DirectorClient:
    """One authenticated connection. A reader thread splits the lines: replies go to the request that waits for them (by id), events to a queue."""

    def __init__(self, host, port, token, name="python-client", timeout=10.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(None)
        self._lock = threading.Lock()
        self._next_id = 1
        self._waiters = {}
        self._events = queue.Queue()
        self._closed = False
        self._buf = b""
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        try:
            r = self._request({"cmd": "auth", "token": token, "name": name}, timeout=timeout)
        except Exception:
            self.close()
            raise
        if not r.get("ok"):
            e = r.get("error", {})
            self.close()
            raise DirectorError(e.get("code", "refused"), e.get("msg", "auth refused"), e)
        self.hello = r.get("result", {})

    @classmethod
    def from_info(cls, info_path, name="python-client", timeout=10.0):
        info = json.loads(Path(info_path).read_text(encoding="utf-8"))
        return cls("127.0.0.1", int(info["port"]), info["token"], name=name, timeout=timeout)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass

    def _read_loop(self):
        while not self._closed:
            try:
                chunk = self._sock.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            self._buf += chunk
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line.decode("utf-8"))
                except ValueError:
                    continue
                if "event" in obj:
                    self._events.put(obj)
                else:
                    with self._lock:
                        w = self._waiters.pop(obj.get("id"), None)
                    if w is not None:
                        w[1].update(obj)
                        w[0].set()
        self._closed = True
        with self._lock:
            ws = list(self._waiters.values())
            self._waiters.clear()
        for ev, box in ws:
            box.update({"ok": False, "error": {"code": "failed", "msg": "connection closed"}})
            ev.set()

    def _request(self, obj, timeout=30.0):
        with self._lock:
            rid = self._next_id
            self._next_id += 1
            obj = dict(obj)
            obj["id"] = rid
            ev, box = threading.Event(), {}
            self._waiters[rid] = (ev, box)
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        if len(data) > MAX_LINE:
            raise DirectorError("bad_args", "request longer than 1 MiB")
        try:
            self._sock.sendall(data)
        except OSError as e:
            raise DirectorError("failed", f"send failed: {e}")
        if not ev.wait(timeout):
            with self._lock:
                self._waiters.pop(rid, None)
            raise DirectorError("timeout", f"no reply to {obj.get('cmd')} in {timeout} s (client side)")
        return box

    def request(self, cmd, args=None, wait=None, timeout_s=None, timeout=None):
        """The raw reply object ({"id", "ok", "result" | "error"}); never raises for an error reply."""
        obj = {"cmd": cmd, "args": args or {}}
        if wait:
            obj["wait"] = wait
        if timeout_s is not None:
            obj["timeout_s"] = timeout_s
        t = timeout if timeout is not None else (timeout_s + 5.0 if timeout_s else 35.0)
        return self._request(obj, timeout=t)

    def call(self, cmd, /, wait=None, timeout_s=None, **args):
        """The result of a command; raises DirectorError on an error reply."""
        r = self.request(cmd, args, wait=wait, timeout_s=timeout_s)
        if r.get("ok"):
            return r.get("result", {})
        e = r.get("error", {})
        raise DirectorError(e.get("code", "failed"), e.get("msg", ""), {k: v for k, v in e.items() if k not in ("code", "msg")})

    def batch(self, cmds, stop_on_error=True):
        return self.call("batch", cmds=cmds, stop_on_error=stop_on_error)["results"]

    def help(self, cmd=None):
        return self.call("help", **({"cmd": cmd} if cmd else {}))

    def events(self, timeout=0.0):
        """Yields the events that are queued; with a timeout, waits that long for the next one (and goes on while they keep coming)."""
        while True:
            try:
                yield self._events.get(timeout=timeout) if timeout else self._events.get_nowait()
            except queue.Empty:
                return

    def wait_event(self, name, pred=None, timeout=30.0):
        """The next event called `name` (for which pred(event) is true); DirectorError(timeout) when none comes."""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise DirectorError("timeout", f"no '{name}' event in {timeout} s (client side)")
            try:
                ev = self._events.get(timeout=left)
            except queue.Empty:
                continue
            if ev.get("event") == name and (pred is None or pred(ev)):
                return ev


def pid_alive(pid):
    if type(pid) is not int or not 0 < pid <= 0xFFFFFFFF:
        return False
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            return ctypes.get_last_error() == 5  # Access denied: the process exists.
        try:
            return kernel.WaitForSingleObject(handle, 0) == 258  # WAIT_TIMEOUT
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except PermissionError:
        return True
    except (OSError, OverflowError):
        return False
    return True


# ---------------------------------------------------------------------------------------------------------------------------- the schema

# name -> (group, summary, [(arg, type, required, doc)], result, file, open_args). Types: any bool number int string array object vec3 target.
def _c(group, summary, args=(), result="", file=False, open_args=False):
    return {"group": group, "summary": summary, "args": [dict(zip(("name", "type", "required", "doc"), a)) for a in args], "result": result, "file": file,
            "open_args": open_args}


COMMANDS = {
    "help": _c("core", "every command with its arguments", [("cmd", "string", False, "one command")], "{proto, commands, errors, events}"),
    "ping": _c("core", "are you there", [], "{pong, frame, t, state}"),
    "batch": _c("core", "several commands in one update", [("cmds", "array", True, "[{cmd, args?}]"), ("stop_on_error", "bool", False, "default true")], "{results}"),
    "events.set": _c("core", "mute or filter this connection's events", [("on", "bool", False, ""), ("only", "array", False, "event names")], "{on, only}"),
    "log.tail": _c("core", "the game's recent log lines", [("n", "int", False, "default 50"), ("filter", "string", False, "substring"), ("level", "string", False, "")], "{lines, latest}"),
    "ui.open": _c("ui", "ask the panel to open", [("panel", "string", True, "")], "{}"),
    "ui.select": _c("ui", "point at a key", [("line", "int", True, "")], "{}"),
    "quit": _c("core", "end the game", [], "{quitting}"),
    "transport.get": _c("transport", "the clock", [], "{state,t,duration,fps,step,paused,frozen,frame,mode,scrubbable,scrub_hint,holding_for_ready,world}"),
    "transport.play": _c("transport", "release a pause or seek", [], "transport.get"),
    "transport.pause": _c("transport", "hold the clock, freeze the world", [], "transport.get"),
    "transport.restart": _c("transport", "run the scene again", [("t", "number", False, "default 0")], "transport.get"),
    "transport.seek": _c("transport", "pin the clock at t", [("t", "number", True, ""), ("settle_frames", "int", False, "default 20")], "{t,paused,frozen,scrubbable,scrub_hint,settled}"),
    "transport.step": _c("transport", "run N updates while paused", [("frames", "int", False, "default 1")], "{t,frames}"),
    "transport.mode": _c("transport", "fixed step on or off", [("fixed", "bool", False, ""), ("fps", "number", False, "")], "{mode,fps,step}"),
    "scene.load": _c("scene", "arm a scene file", [("path", "string", True, "")], "{path,state}"),
    "scene.get": _c("scene", "the scene as parsed", [("text", "bool", False, "")], "{path,name,rev,hash,events,diagnostics,car_ids,duration,fps,text?}"),
    "scene.edit": _c("scene", "line-addressed text patches", [("base_rev", "int", True, ""), ("ops", "array", True, ""), ("allow_errors", "bool", False, "")],
                     "{rev,hash,lines,changed,errors}"),
    "scene.propose": _c("scene", "a proposal beside the scene", [("note", "string", True, ""), ("path", "string", False, ""), ("text", "string", False, "")], "{path,errors,diagnostics}"),
    "scene.accept": _c("scene", "replace the scene with the proposal", [], "{rev,hash}"),
    "scene.reject": _c("scene", "drop the proposal", [], "{}"),
    "scene.undo": _c("scene", "the file before the last change", [], "{rev,hash}"),
    "scene.redo": _c("scene", "undo the undo", [], "{rev,hash}"),
    "world.get": _c("world", "the world's state", [], "{hour,weather,traffic,peds,parked,police,timescale,scene,override}"),
    "world.set": _c("world", "hour, weather, ... live or committed", [("hour", "number", False, ""), ("weather", "string", False, ""), ("traffic", "bool", False, ""),
                                                                    ("peds", "any", False, ""), ("parked", "any", False, ""), ("police", "bool", False, ""),
                                                                    ("timescale", "number", False, ""), ("commit", "bool", False, ""), ("t", "number", False, ""),
                                                                    ("clear", "bool", False, "")], "{committed,override}", file=True),
    "camera.get": _c("camera", "the final camera", [], "{mode,pose,rows,near,far,aspect,valid}"),
    "camera.set": _c("camera", "place the free camera", [("pos", "vec3", False, ""), ("yaw", "number", False, ""), ("pitch", "number", False, ""), ("roll", "number", False, ""),
                                                        ("fov", "number", False, ""), ("pose", "object", False, "")], "{mode,pose}"),
    "camera.look_at": _c("camera", "aim the free camera", [("target", "target", True, ""), ("from", "target", False, ""), ("fov", "number", False, "")], "{mode,pose}"),
    "camera.mode": _c("camera", "whose camera is final", [("mode", "string", True, "scene|freecam|game")], "{mode}"),
    "camera.key.add": _c("camera", "a cam key from the camera as it is", [("t", "number", False, "default now"), ("ease", "string", False, ""), ("cut", "bool", False, ""),
                                                                         ("rig", "string", False, "")], "{line,lines,text}", file=True),
    "camera.key.list": _c("camera", "the camera keys", [], "{keys}"),
    "cars.list": _c("cars", "the scene's cars", [], "{cars}"),
    "cars.models": _c("cars", "vehicle names", [("search", "string", False, "")], "{models}"),
    "car.place": _c("cars", "put a car there now (transient)", [("id", "string", True, ""), ("pos", "vec3", True, ""), ("heading", "number", False, "")], "{id,placed}"),
    "car.spawn": _c("cars", "a car statement + live spawn", [("id", "string", True, ""), ("model", "string", False, ""), ("profile", "int", False, ""), ("at", "any", True, ""),
                                                           ("heading", "any", False, ""), ("paint", "int", False, ""), ("t", "number", False, "")], "{line,lines,text}", file=True),
    "car.delete": _c("cars", "delete a car", [("id", "string", True, ""), ("t", "number", False, "")], "{line,lines,text}", file=True),
    "car.path": _c("cars", "a car drives a route", [("id", "string", True, ""), ("route", "string", True, ""), ("speed", "any", False, ""), ("mode", "string", False, ""),
                                                   ("t", "number", False, "")], "{line,lines,text}", file=True),
    "car.path.record": _c("cars", "record a car's drive", [("id", "string", True, ""), ("name", "string", True, ""), ("every_m", "number", False, "default 2")], "{recording}"),
    "car.path.record.stop": _c("cars", "end the recording and write the route", [], "{route,points,length_m}"),
    "path.waypoint": _c("cars", "append a waypoint to a route", [("route", "string", True, ""), ("at", "target", True, "\"camera\" or [x,y,z]"), ("speed", "number", False, "")], "{route,points}"),
    "path.list": _c("cars", "the route files", [], "{routes}"),
    "path.get": _c("cars", "a route", [("route", "string", True, "")], "{route,points,length_m}"),
    "atlas.dump": _c("query", "write the atlas files", [], "{dir}", open_args=True),
    "query.spots": _c("query", "named places", [], "{spots}", open_args=True),
    "query.lights": _c("query", "lamps", [], "{lights}", open_args=True),
    "query.road": _c("query", "the road network", [], "{...}", open_args=True),
    "query.route": _c("query", "a route between places", [], "{...}", open_args=True),
    "query.ground": _c("query", "ground height", [], "{y}", open_args=True),
    "query.pick": _c("query", "screen point -> world", [], "{...}", open_args=True),
    "query.garages": _c("query", "garages", [], "{garages}", open_args=True),
    "query.streaming": _c("query", "is streaming done", [], "{done}", open_args=True),
    "query.visible": _c("query", "is a subject in frame", [], "{...}", open_args=True),
    "shot": _c("capture", "a clean engine frame as a PNG", [("path", "string", True, ""), ("size", "any", False, ""), ("t", "number", False, "")], "{path,width,height,t,bytes,frame}"),
    "shot.schedule": _c("capture", "stills at exact scene times during a play-through", [("times", "array", True, ""), ("dir", "string", True, "")], "{count,dir}"),
    "take.render": _c("capture", "start a take", [("fps", "number", True, ""), ("sink", "string", False, ""), ("name", "string", False, ""), ("from", "number", False, ""),
                                                 ("to", "number", False, "")], "{name,fps,sink,from,to}"),
    "take.status": _c("capture", "the take", [], "{active,name,folder,fps,frames,state,error?,last}"),
}


def _type_ok(t, v):
    if t == "any":
        return True
    if t == "bool":
        return isinstance(v, bool)
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "int":
        return isinstance(v, int) and not isinstance(v, bool) or (isinstance(v, float) and v == int(v))
    if t == "string":
        return isinstance(v, str)
    if t == "array":
        return isinstance(v, list)
    if t == "object":
        return isinstance(v, dict)
    if t == "vec3":
        return isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v)
    if t == "target":
        return isinstance(v, str) or _type_ok("vec3", v)
    return False


def validate_args(cmd, args):
    """'' when the arguments fit the command's schema, else the bad_args message (the real game's rule)."""
    spec = COMMANDS[cmd]
    names = {a["name"] for a in spec["args"]}
    for a in spec["args"]:
        if a["name"] not in args:
            if a["required"]:
                return f"{cmd}: '{a['name']}' is required"
            continue
        if not _type_ok(a["type"], args[a["name"]]):
            return f"{cmd}: '{a['name']}' must be a {a['type']}"
    if not spec["open_args"]:
        for k in args:
            if k not in names:
                return f"{cmd}: unknown argument '{k}'"
    return ""


# ---------------------------------------------------------------------------------------------------------------------------- scene edit (the real op rules)

def parse_time(word):
    try:
        if ":" in word:
            m, s = word.split(":", 1)
            return int(m) * 60 + float(s)
        return float(word)
    except ValueError:
        return None


def fmt_time(t):
    s = f"{t:.3f}".rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def split_words(line):
    """[(start, end, text)] of the words of a statement; a '#' after a blank (outside quotes) starts a comment."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in " \t\r":
            i += 1
            continue
        if c == "#":
            break
        j = i
        if c == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 1
            j = min(j + 1, n)
        else:
            while j < n and line[j] not in " \t\r":
                j += 1
        out.append((i, j, line[i:j]))
        i = j
    return out


def read_statement(line):
    """(is_statement, has_time, t, verb, track, time_span)"""
    w = split_words(line)
    if not w:
        return (False, False, 0.0, "", "", None)
    verb = w[0][2]
    if verb in ("cam", "cut", "end") and len(w) > 1:
        t = parse_time(w[1][2])
        if t is not None:
            return (True, True, t, verb, "camera" if verb in ("cam", "cut") else verb, (w[1][0], w[1][1]))
    if verb == "at" and len(w) > 2:
        t = parse_time(w[1][2])
        if t is not None:
            v = w[2][2]
            track = v
            if v == "car" and len(w) > 3:
                track = "car:" + w[3][2]
            elif v == "set" and len(w) > 3:
                track = "set:" + w[3][2]
            return (True, True, t, v, track, (w[1][0], w[1][1]))
    track = verb
    return (True, False, 0.0, verb, track, None)


class EditError(Exception):
    def __init__(self, code, msg, op=-1, details=None):
        super().__init__(msg)
        self.code, self.msg, self.op, self.details = code, msg, op, details or {}


def split_lines(text):
    eol = "\r\n" if "\r\n" in text else "\n"
    final = text.endswith("\n")
    body = text[:-len(eol)] if final and text.endswith(eol) else (text[:-1] if final else text)
    lines = body.split(eol) if (text != "") else []
    return lines, eol, final


def apply_edit(text, ops, base_rev):
    """The real SceneEdit rules (see scene_edit.h). Returns (new_text, new_line_numbers_of_written_lines, changed_lines). Raises EditError."""
    lines, eol, final = split_lines(text)
    entries = [{"orig": i + 1, "text": l, "written": False} for i, l in enumerate(lines)]
    if not ops:
        raise EditError("bad_args", "no ops")

    def find(opi, line, expect):
        for e in entries:
            if e["orig"] == line:
                if e["text"].rstrip(" \t\r") != expect.rstrip(" \t\r"):
                    raise EditError("stale", f"op {opi}: line {line} is not what you read", opi, {"line": line, "actual": e["text"]})
                return e
        raise EditError("bad_args", f"op {opi}: no line {line} (the file has {len(lines)}; it may be deleted by an earlier op)", opi)

    for opi, op in enumerate(ops):
        kind = op.get("op")
        if kind == "insert":
            txt = op.get("text")
            if not isinstance(txt, str) or not txt.strip():
                raise EditError("bad_args", f"op {opi}: insert needs 'text'", opi)
            new = [{"orig": 0, "text": l, "written": True} for l in txt.replace("\r\n", "\n").split("\n")]
            _, has_time, t, verb, track, _ = read_statement(new[0]["text"])
            pos = None
            if has_time:
                for k, e in enumerate(entries):
                    st = read_statement(e["text"])
                    if st[0] and st[1] and st[4] == track and st[2] <= t + 1e-9:
                        pos = k + 1
            if pos is None:
                header = {"scene", "world", "statelog", "start"}
                for k, e in enumerate(entries):
                    st = read_statement(e["text"])
                    if st[0] and not st[1] and st[3] in header:
                        pos = k + 1
                if pos is None:
                    for k, e in enumerate(entries):
                        if read_statement(e["text"])[0]:
                            pos = k
                            break
                if pos is None:
                    pos = len(entries)
            entries[pos:pos] = new
        elif kind in ("replace", "delete", "move", "param"):
            line, expect = op.get("line"), op.get("expect")
            if not isinstance(line, int) or isinstance(line, bool) or not isinstance(expect, str):
                raise EditError("bad_args", f"op {opi}: {kind} needs 'line' and 'expect'", opi)
            e = find(opi, line, expect)
            if kind == "delete":
                entries.remove(e)
            elif kind == "replace":
                if not isinstance(op.get("text"), str):
                    raise EditError("bad_args", f"op {opi}: replace needs 'text'", opi)
                e["text"], e["written"] = op["text"], True
            elif kind == "move":
                t = op.get("t")
                if not isinstance(t, (int, float)) or isinstance(t, bool):
                    raise EditError("bad_args", f"op {opi}: move needs a number 't'", opi)
                st = read_statement(e["text"])
                if not st[1]:
                    raise EditError("bad_args", f"op {opi}: line {line} has no time word", opi)
                a, b = st[5]
                e["text"], e["written"] = e["text"][:a] + fmt_time(t) + e["text"][b:], True
            else:
                key, val = op.get("key"), op.get("value")
                if not isinstance(key, str) or not key or not isinstance(val, str):
                    raise EditError("bad_args", f"op {opi}: param needs 'key' and 'value'", opi)
                words = split_words(e["text"])
                n = max(1, len(val.split()))
                idx = None
                for k, w in enumerate(words):
                    if w[2] == key and k >= 1:
                        idx = k
                        break
                if idx is None:
                    ci = e["text"].find(" #")
                    head, tail = (e["text"], "") if ci < 0 else (e["text"][:ci].rstrip(), e["text"][ci:])
                    e["text"] = f"{head} {key} {val}{tail}"
                else:
                    last = min(idx + n, len(words) - 1)
                    a = words[idx + 1][0] if idx + 1 < len(words) else len(e["text"])
                    b = words[last][1] if idx + 1 < len(words) else len(e["text"])
                    if idx + 1 >= len(words):
                        e["text"] = e["text"].rstrip() + " " + val
                    else:
                        e["text"] = e["text"][:a] + val + e["text"][b:]
                e["written"] = True
        else:
            raise EditError("bad_args", f"op {opi}: unknown op '{kind}'", opi)
    new_lines = [e["text"] for e in entries]
    out = eol.join(new_lines) + (eol if final or not lines else "")
    written = [i + 1 for i, e in enumerate(entries) if e["written"]]
    return out, written, changed_lines(text, out)


def changed_lines(before, after):
    a, _, _ = split_lines(before)
    b, _, _ = split_lines(after)
    n, m = len(a), len(b)
    if n * m > 4000 * 4000:
        return [i + 1 for i in range(m) if i >= n or a[i] != b[i]]
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            dp[i][j] = dp[i + 1][j + 1] + 1 if a[i] == b[j] else max(dp[i + 1][j], dp[i][j + 1])
    keep, i, j = set(), 0, 0
    while i < n and j < m:
        if a[i] == b[j]:
            keep.add(j)
            i += 1
            j += 1
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    return [k + 1 for k in range(m) if k not in keep]


def text_hash(text):
    h = 0xcbf29ce484222325
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def tiny_png(w, h, rgb):
    # a gradient (a flat picture would look like a failed shot to a checker)
    raw = b"".join(b"\x00" + b"".join(bytes(((rgb[0] + x) & 255, (x * 4) & 255, (y * 7) & 255)) for x in range(w)) for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


# ---------------------------------------------------------------------------------------------------------------------------- the mock server

DEFAULT_SCENE = """# the mock's scene
scene mock-demo fps 30
world hour 14 hold
at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62
cam 0 pos 800 20 -820 look car:p1 fov 40
cam 4 pos 810 19 -815 look car:p1 fov 40
at 6 hour 14..18 over 4
end 10
"""

WEATHERS = ["nice", "cloudy", "stormy", "foggy"]
MODELS = [("vp_lam_murcielago_06", "exotic"), ("vp_ford_mustang_08", "muscle"), ("vp_nis_350z_06", "tuner"), ("vp_bmw_m3_08", "sport"), ("vp_ben_continental_07", "luxury")]
SPOTS = [{"name": "gate", "pos": [819.6, 17.6, -808.4], "heading": 62.0, "text": "the Hollywood lot gate"},
         {"name": "street", "pos": [840.0, 17.6, -790.0], "heading": 62.0, "text": "the street outside it"}]


class _Client:
    def __init__(self, sock, idx):
        self.sock, self.idx, self.authed, self.name = sock, idx, False, f"agent {idx}"
        self.buf = b""
        self.out_lock = threading.Lock()
        self.events_on, self.only = True, None


class MockServer:
    """A fake game. MockServer(root).start() -> (port, token). Time is simulated: it advances 1:1 with the wall clock while playing (set `speed`), or by
    `advance(dt)`. Everything is in memory except the scene file (written atomically on edit) and the files shots and takes make under `root`."""

    def __init__(self, root=".", scene_text=DEFAULT_SCENE, scene_path=None, token=None, speed=1.0, ready_after=1.0, auto_play=False):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.token = token or secrets.token_hex(16)
        self.speed = speed
        self.ready_after = ready_after
        self.lock = threading.RLock()
        self.clients = []
        self.cmd_log = []
        self.scene_path = Path(scene_path).resolve() if scene_path else self.root / "mock.scene"
        if scene_path and self.scene_path.is_file():
            scene_text = self.scene_path.read_text(encoding="utf-8")
        elif not self.scene_path.is_file():
            self.scene_path.write_text(scene_text, encoding="utf-8")
        self.text = scene_text
        self.rev, self.undo, self.redo = 1, [], []
        self.sim_t, self.playing, self.frozen, self.fixed, self.fps = 0.0, auto_play, False, False, 30.0
        self.frame = 0
        self.state = "running"
        self.cam_mode, self.cam = "scene", {"pos": [800.0, 20.0, -820.0], "yaw": 62.0, "pitch": 0.0, "roll": 0.0, "fov": 40.0}
        self.cars = {}  # id -> dict
        self.override = {}
        self.proposal = None
        self.recording, self.waypoints = None, {}
        self.schedule, self.take, self.last_take = [], None, None
        self.deferred = []  # (due_sim_wall, fn)
        self.log_lines = [{"seq": 1, "level": "info", "cat": "larecomp_app", "text": "[scene] mock armed"}]
        self.listen = None
        self.port = 0
        self._stop = False
        self._sync_cars_from_text()

    # ---------- lifecycle
    def start(self):
        self.listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listen.bind(("127.0.0.1", 0))
        self.listen.listen(8)
        self.port = self.listen.getsockname()[1]
        threading.Thread(target=self._accept_loop, daemon=True).start()
        threading.Thread(target=self._tick_loop, daemon=True).start()
        return self.port, self.token

    def info(self):
        return {"proto": PROTO, "pid": os.getpid(), "port": self.port, "token": self.token, "slot": "mock", "exe": "mock", "version": "mock",
                "scene": str(self.scene_path), "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}

    def write_info(self, path):
        Path(path).write_text(json.dumps(self.info()), encoding="utf-8")

    def stop(self):
        self._stop = True
        try:
            self.listen.close()
        except OSError:
            pass
        with self.lock:
            for c in self.clients:
                try:
                    c.sock.close()
                except OSError:
                    pass

    def _accept_loop(self):
        while not self._stop:
            try:
                sock, _ = self.listen.accept()
            except OSError:
                return
            self._accept_client(sock)

    def _accept_client(self, sock):
        with self.lock:
            live = sum(c.sock.fileno() != -1 for c in self.clients)
            client = _Client(sock, len(self.clients))
            client.over_limit = live >= 4
            self.clients.append(client)
        threading.Thread(target=self._client_loop, args=(client,), daemon=True).start()

    def _client_loop(self, c):
        s = c.sock
        s.settimeout(0.5)
        while not self._stop:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            c.buf += chunk
            if len(c.buf) > MAX_LINE and b"\n" not in c.buf:
                self._send(c, {"ok": False, "error": {"code": "bad_args", "msg": "line longer than 1 MiB"}})
                break
            while b"\n" in c.buf:
                line, c.buf = c.buf.split(b"\n", 1)
                line = line.strip()
                if line:
                    if not self._handle_line(c, line):
                        try:
                            s.close()
                        except OSError:
                            pass
                        return
        try:
            s.close()
        except OSError:
            pass
        self._publish("agent", client=c.idx, name=c.name, connected=False)

    def _send(self, c, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        with c.out_lock:
            try:
                c.sock.sendall(data)
            except OSError:
                pass

    def _publish(self, ev_name, /, **fields):
        name = ev_name
        ev = {"event": name}
        ev.update(fields)
        with self.lock:
            cs = list(self.clients)
        for c in cs:
            if c.authed and c.events_on and (c.only is None or name in c.only):
                self._send(c, ev)

    # ---------- the line handler
    def _handle_line(self, c, raw):
        try:
            req = json.loads(raw.decode("utf-8"))
            if not isinstance(req, dict):
                raise ValueError("not an object")
        except ValueError as e:
            self._send(c, {"ok": False, "error": {"code": "bad_args", "msg": f"not a JSON object: {e}"}})
            return c.authed
        rid = req.get("id")
        base = {} if "id" not in req else {"id": rid}
        if getattr(c, "over_limit", False):
            self._send(c, {**base, "ok": False, "error": {"code": "refused", "msg": "too many clients (4 at most)"}})
            return False
        if not c.authed:
            tok = req.get("token", req.get("args", {}).get("token", "") if isinstance(req.get("args"), dict) else "")
            if req.get("cmd") != "auth" or not isinstance(tok, str) or not hmac.compare_digest(tok, self.token):
                self._send(c, {**base, "ok": False, "error": {"code": "refused", "msg": "authenticate first: {\"cmd\":\"auth\",\"token\":...}"}})
                return False
            c.name = str(req.get("name", c.name))
            self._send(c, {**base, "ok": True, "result": {"proto": PROTO, "version": "mock", "pid": os.getpid(), "slot": "mock", "scene": str(self.scene_path),
                                                          "commands": len(COMMANDS) + 1, "client": c.idx}})
            self._publish("agent", client=c.idx, name=c.name, connected=True)  # the others hear it; this client is not authenticated yet
            c.authed = True
            return True
        cmd = req.get("cmd")
        args = req.get("args", {})
        if not isinstance(cmd, str):
            self._send(c, {**base, "ok": False, "error": {"code": "bad_args", "msg": "'cmd' must be a string"}})
            return True
        if args is None:
            args = {}
        if not isinstance(args, dict):
            self._send(c, {**base, "ok": False, "error": {"code": "bad_args", "msg": "'args' must be an object"}})
            return True
        wait = req.get("wait") == "settled"
        timeout_s = float(req.get("timeout_s", 30.0))
        with self.lock:
            self.cmd_log.append((cmd, args))
            reply = self._run(c, cmd, args, wait, timeout_s, base=base)
        if reply is None:  # deferred: the callback sends
            return True
        self._send(c, {**base, **reply})
        return True

    def _run(self, c, cmd, args, wait, timeout_s, base=None):
        """A reply dict ({"ok":...}) or None when it was deferred (self._defer sends it)."""
        if cmd == "auth":
            return self._err("refused", "already authenticated")
        if cmd not in COMMANDS:
            return self._err("unknown_cmd", f"no such command '{cmd}' (help lists them)")
        bad = validate_args(cmd, args)
        if bad:
            return self._err("bad_args", bad)
        fn = getattr(self, "_cmd_" + cmd.replace(".", "_"), None)
        if fn is None:
            return self._ok({"mock": True})
        ctx = {"client": c, "wait": wait, "timeout_s": timeout_s, "base": base or {}}
        try:
            return fn(args, ctx)
        except EditError as e:
            return self._err(e.code, e.msg, **e.details)
        except KeyError as e:
            return self._err("bad_args", f"missing {e}")

    @staticmethod
    def _ok(result=None):
        return {"ok": True, "result": result if result is not None else {}}

    @staticmethod
    def _err(code, msg, **more):
        e = {"code": code, "msg": msg}
        e.update(more)
        return {"ok": False, "error": e}

    # ---------- the simulated world
    def _tick_loop(self):
        last = time.monotonic()
        while not self._stop:
            time.sleep(0.01)
            now = time.monotonic()
            dt, last = now - last, now
            with self.lock:
                if self.playing and not self.frozen and self.state == "running":
                    self.advance(dt * self.speed)

    def advance(self, dt):
        with self.lock:
            dur = self._duration()
            before = self.sim_t
            self.sim_t += dt
            self.frame += max(1, int(dt * self.fps))
            if dur >= 0 and self.sim_t >= dur:
                self.sim_t = dur
                self.state = "finished"
                self.playing = False
                self._publish("scene.state", state="finished", t=self.sim_t, path=str(self.scene_path))
            for ev in self._events():
                if before < ev["t"] <= self.sim_t and ev["track"] in ("cam", "cut", "car", "hour"):
                    self._publish("key.reached", t=ev["t"], line=ev["line"], track=ev["track"], text=ev["text"])
            for car in self.cars.values():
                if not car["ready"] and self.sim_t >= car["t"] + self.ready_after:
                    car["ready"] = True
                    self._publish("car.ready", id=car["id"])
            while self.schedule and self.schedule[0][0] <= self.sim_t + 1e-9:
                t, path, idx, total = self.schedule.pop(0)
                self._write_shot(path)
                self._publish("shot.done", path=str(path), t=t, ok=True, index=idx, total=total)
            if self.take and self.take["active"]:
                tk = self.take
                tk["frames"] += max(1, int(dt * tk["fps"]))
                if self.sim_t >= tk["to"] or self.state == "finished":
                    self._finish_take()
            if self.recording:
                car = self.cars.get(self.recording["id"])
                if car:
                    self.recording["points"].append([car["pos"][0], car["pos"][1], car["pos"][2], 14.0])

    def _finish_take(self):
        tk = self.take
        tk["active"] = False
        folder = Path(tk["folder"])
        folder.mkdir(parents=True, exist_ok=True)
        # like the game's: the clock stands at `from` until the take begins, the first frame shows one step later, a frame for every step up to `to`, and frames.csv
        # (frame,update,scene_t) says which scene time each frame shows
        tk["frames"] = max(1, int(round((tk["to"] - tk["from"]) * tk["fps"])))
        # take.json's own shape (take_folder.cpp BuildTakeJson): "frames" is an object ({count, exact, verdict, ...}), the rate is picture.fps; a mock that wrote a bare number let a script
        # that did int(take["frames"]) pass its selftest and die on the real game's file
        (folder / "take.json").write_text(json.dumps({"take_json_version": 1, "state": "complete", "frames": {"count": tk["frames"], "exact": True, "verdict": "exact"},
                                                      "picture": {"fps": tk["fps"]}, "fps": tk["fps"], "mock": True}), encoding="utf-8")
        (folder / "frames.csv").write_text("frame,update,scene_t\n" + "".join(f"{i},{i},{tk['from'] + (i + 1) / tk['fps']:.6f}\n" for i in range(tk["frames"])), encoding="utf-8")
        if tk.get("sink") == "png":
            (folder / "frames").mkdir(exist_ok=True)
            for i in range(min(tk["frames"], 600)):
                (folder / "frames" / f"f{i:06d}.png").write_bytes(tiny_png(64, 36, (int(255 * ((tk["from"] + (i + 1) / tk["fps"]) % 10) / 10), 80, 160)))
        self.last_take = {"name": tk["name"], "folder": tk["folder"], "state": "complete", "frames": tk["frames"]}
        self._publish("take.done", name=tk["name"], folder=tk["folder"], state="complete", frames=tk["frames"])

    def _sync_cars_from_text(self):
        lines, _, _ = split_lines(self.text)
        seen = set()
        for i, l in enumerate(lines):
            w = [x[2] for x in split_words(l)]
            if len(w) >= 5 and w[0] == "at" and w[2] == "car" and w[4] in ("spawn", "ambient"):
                cid, t = w[3], parse_time(w[1]) or 0.0
                pos, heading, model = [0.0, 0.0, 0.0], 0.0, "?"
                if "at" in w[5:]:
                    k = w.index("at", 5)
                    try:
                        pos = [float(x) for x in w[k + 1].split(",")] if "," in w[k + 1] else [float(x) for x in w[k + 1:k + 4]]
                    except (ValueError, IndexError):
                        pass
                if "heading" in w:
                    try:
                        heading = float(w[w.index("heading") + 1])
                    except (ValueError, IndexError):
                        pass
                model = w[5] if len(w) > 5 else "?"
                seen.add(cid)
                old = self.cars.get(cid)
                if old and old["model"] == model and old["pos"] == pos and old["heading"] == heading and old["t"] == t:
                    continue
                self.cars[cid] = {"id": cid, "model": model, "pos": pos, "heading": heading, "speed": 0.0, "ready": False, "on_path": False, "t": t, "line": i + 1}
        for cid in list(self.cars):
            if cid not in seen and cid != "player":
                del self.cars[cid]
        if "player" not in self.cars:
            self.cars["player"] = {"id": "player", "model": "vp_player", "pos": [800.0, 17.6, -800.0], "heading": 0.0, "speed": 0.0, "ready": True, "on_path": False, "t": 0.0, "line": 0}

    def _events(self):
        out = []
        lines, _, _ = split_lines(self.text)
        for i, l in enumerate(lines):
            st = read_statement(l)
            if st[0] and st[1]:
                out.append({"line": i + 1, "t": st[2], "track": st[3], "text": " ".join(x[2] for x in split_words(l)),
                            "kind": {"cam": "camera", "cut": "cut", "car": "car", "hud": "hud", "split": "split", "label": "label", "mark": "mark", "set": "setting",
                                     "capture": "capture"}.get(st[3], "world"), "car": st[4].split(":", 1)[1] if st[4].startswith("car:") else ""})
        return out

    def _duration(self):
        for l in split_lines(self.text)[0]:
            w = split_words(l)
            if len(w) >= 2 and w[0][2] == "end":
                t = parse_time(w[1][2])
                if t is not None:
                    return t
        return -1.0

    def _diagnostics(self, text=None):
        diags = []
        for i, l in enumerate(split_lines(text if text is not None else self.text)[0]):
            w = [x[2] for x in split_words(l)]
            if w and w[0] == "cam" and len(w) > 1 and parse_time(w[1]) is None:
                diags.append({"line": i + 1, "severity": "error", "message": f"bad time '{w[1]}'"})
            if w and w[0] == "at" and (len(w) < 3 or parse_time(w[1]) is None):
                diags.append({"line": i + 1, "severity": "error", "message": "at needs a time and a statement"})
            if w and w[0] not in ("scene", "world", "end", "cam", "cut", "at", "statelog", "start"):
                diags.append({"line": i + 1, "severity": "error", "message": f"unknown statement '{w[0]}'"})
        return diags

    def _scrub_state(self):
        for l in split_lines(self.text)[0]:
            if "mode brain" in l and not l.lstrip().startswith("#"):
                return False, "a brain-mode car is in the scene: the world cannot rewind"
        return True, ""

    def _transport(self):
        ok, hint = self._scrub_state()
        return {"state": self.state, "t": round(self.sim_t, 6), "duration": self._duration(), "fps": 30.0, "step": (1.0 / self.fps) if self.fixed else 0.0,
                "paused": not self.playing, "frozen": self.frozen, "frame": self.frame, "mode": "fixed" if self.fixed else "realtime", "scrubbable": ok, "scrub_hint": hint,
                "holding_for_ready": False, "world": True}

    def _write_shot(self, path):
        p = self._safe_path(path)
        if p is None:
            return None
        p.parent.mkdir(parents=True, exist_ok=True)
        g = int(255 * (self.sim_t % 10) / 10)
        p.write_bytes(tiny_png(64, 36, (g, 80, 160)))
        return p

    def _safe_path(self, path, base=None):
        if not isinstance(path, str) or not path or os.path.isabs(path) or ":" in path or ".." in Path(path.replace("\\", "/")).parts:
            return None
        return (base or self.root) / path

    # ---------- deferral: a reply that comes later. The mock runs the handler's result through a timer so `wait:"settled"` is observable.
    def _deferred(self, ctx, delay, reply_fn):
        c = ctx["client"]

        def run():
            time.sleep(delay)
            with self.lock:
                r = reply_fn()
            self._send_late(c, ctx, r)

        threading.Thread(target=run, daemon=True).start()
        return None

    def _send_late(self, c, ctx, reply):
        self._send(c, {**ctx.get("base", {}), **reply})

    # ---------- the commands
    def _cmd_help(self, a, ctx):
        names = [a["cmd"]] if "cmd" in a else sorted(COMMANDS)
        if "cmd" in a and a["cmd"] not in COMMANDS:
            return self._err("unknown_cmd", f"no such command '{a['cmd']}'")
        cmds = [{"name": n, **COMMANDS[n]} for n in names]
        return self._ok({"proto": PROTO, "commands": cmds, "errors": list(ERROR_CODES),
                         "events": [{"name": n, "doc": ""} for n in ("scene.reloaded", "scene.state", "key.reached", "car.ready", "shot.done", "take.progress", "take.done", "edit", "agent")]})

    def _cmd_ping(self, a, ctx):
        return self._ok({"pong": True, "frame": self.frame, "t": self.sim_t, "state": self.state})

    def _cmd_events_set(self, a, ctx):
        c = ctx["client"]
        if "on" in a:
            c.events_on = bool(a["on"])
        if "only" in a:
            c.only = set(a["only"]) if a["only"] else None
        return self._ok({"on": c.events_on, "only": sorted(c.only) if c.only else None})

    def _cmd_log_tail(self, a, ctx):
        lines = [l for l in self.log_lines if a.get("filter", "") in l["text"]][-int(a.get("n", 50)):]
        return self._ok({"lines": lines, "latest": self.log_lines[-1]["seq"]})

    def _cmd_ui_open(self, a, ctx):
        return self._ok({})

    def _cmd_ui_select(self, a, ctx):
        return self._ok({})

    def _cmd_quit(self, a, ctx):
        threading.Timer(0.05, self.stop).start()
        return self._ok({"quitting": True})

    def _cmd_batch(self, a, ctx):
        results, stop = [], a.get("stop_on_error", True)
        for sub in a["cmds"]:
            if not isinstance(sub, dict) or not isinstance(sub.get("cmd"), str):
                results.append(self._err("bad_args", "each of cmds is {cmd, args?}"))
            else:
                r = self._run(ctx["client"], sub["cmd"], sub.get("args", {}) or {}, False, 30.0)
                results.append(r if r is not None else self._err("failed", "a held command cannot be run in a batch"))
            if stop and not results[-1].get("ok"):
                break
        return self._ok({"results": results})

    def _cmd_transport_get(self, a, ctx):
        return self._ok(self._transport())

    def _cmd_transport_play(self, a, ctx):
        if self.state == "finished":
            self.sim_t, self.state = 0.0, "running"
        self.playing, self.frozen = True, False
        return self._ok(self._transport())

    def _cmd_transport_pause(self, a, ctx):
        self.playing, self.frozen = False, True
        return self._ok(self._transport())

    def _cmd_transport_restart(self, a, ctx):
        self.sim_t, self.state, self.playing, self.frozen = float(a.get("t", 0.0)), "running", True, False
        self._sync_cars_from_text()
        for car in self.cars.values():
            if car["id"] != "player":
                car["ready"] = False
        self._publish("scene.state", state="running", t=self.sim_t, path=str(self.scene_path))
        return self._ok(self._transport())

    def _cmd_transport_seek(self, a, ctx):
        t = float(a["t"])
        dur = self._duration()
        if t < 0 or (dur >= 0 and t > dur):
            return self._err("bad_args", f"transport.seek: 't' must be within 0..{dur}")
        self.sim_t, self.playing, self.frozen, self.state = t, False, True, ("running" if self.state == "finished" else self.state)
        ok, hint = self._scrub_state()
        res = {"t": t, "paused": True, "frozen": True, "scrubbable": ok, "scrub_hint": hint, "settled": False}
        if ctx["wait"]:
            frames = int(a.get("settle_frames", 20))
            res["settled"] = True
            return self._defer(ctx, frames / 60.0, lambda: self._ok(res))
        return self._ok(res)

    def _cmd_transport_step(self, a, ctx):
        n = int(a.get("frames", 1))
        if n < 1 or n > 100000:
            return self._err("bad_args", "transport.step: 'frames' must be 1..100000")
        if not self.fixed:
            return self._err("not_ready", "transport.step needs the fixed step: call transport.mode {fixed:true} first (a step is exact only at a fixed step)")
        self.advance(n / self.fps)
        return self._ok({"t": self.sim_t, "frames": n})

    def _cmd_transport_mode(self, a, ctx):
        if "fps" in a:
            self.fps = float(a["fps"])
        if "fixed" in a:
            self.fixed = bool(a["fixed"])
        return self._ok({"mode": "fixed" if self.fixed else "realtime", "fps": self.fps, "step": 1.0 / self.fps if self.fixed else 0.0})

    def _cmd_scene_load(self, a, ctx):
        p = Path(a["path"])
        if not p.is_absolute():
            for cand in (self.root / p, self.root / "scenes" / p, self.root / (str(p) + ".scene"), self.root / "scenes" / (str(p) + ".scene")):
                if cand.is_file():
                    p = cand
                    break
        if not p.is_file():
            return self._err("failed", f"no such scene file: {a['path']}")
        self.scene_path, self.text = p.resolve(), p.read_text(encoding="utf-8")
        self.rev += 1
        self.undo, self.redo = [], []
        self.sim_t, self.state, self.playing, self.frozen = 0.0, "running", True, False
        self.cars = {k: v for k, v in self.cars.items() if k == "player"}
        self._sync_cars_from_text()
        self._publish("scene.state", state="running", t=0.0, path=str(self.scene_path))
        self._publish("scene.reloaded", rev=self.rev, by="agent", changed=[], errors=len([d for d in self._diagnostics() if d["severity"] == "error"]))
        res = {"path": str(self.scene_path), "state": self.state}
        if ctx["wait"]:
            return self._defer(ctx, 0.3, lambda: self._ok(res))
        return self._ok(res)

    def _cmd_scene_get(self, a, ctx):
        ev = self._events()
        res = {"path": str(self.scene_path), "name": "mock-demo", "rev": self.rev, "hash": text_hash(self.text), "events": ev, "diagnostics": self._diagnostics(),
               "car_ids": sorted(k for k in self.cars if k != "player"), "duration": self._duration(), "fps": 30.0}
        if a.get("text"):
            res["text"] = self.text
        return self._ok(res)

    def _write_text(self, new_text, by, lines=None, changed=None):
        self.undo.append(self.text)
        self.redo = []
        self.text = new_text
        self.rev += 1
        tmp = self.scene_path.with_suffix(self.scene_path.suffix + ".tmp")
        tmp.write_text(new_text, encoding="utf-8", newline="")
        os.replace(tmp, self.scene_path)
        old_cars = {k: dict(v) for k, v in self.cars.items()}
        self._sync_cars_from_text()
        for k, v in self.cars.items():
            if k in old_cars and (old_cars[k]["model"], old_cars[k]["pos"], old_cars[k]["heading"]) == (v["model"], v["pos"], v["heading"]):
                v["ready"] = old_cars[k]["ready"]  # a car whose statement did not change is left alone
        errs = len([d for d in self._diagnostics() if d["severity"] == "error"])
        self._publish("scene.reloaded", rev=self.rev, by=by, changed=changed or [], errors=errs, soft=(self.state == "running"))
        return errs

    def _cmd_scene_edit(self, a, ctx):
        if a["base_rev"] != self.rev:
            return self._err("stale", f"scene.edit: base_rev {a['base_rev']} is not the current revision {self.rev}", rev=self.rev)
        new, written, changed = apply_edit(self.text, a["ops"], a["base_rev"])
        diags = self._diagnostics(new)
        errs = [d for d in diags if d["severity"] == "error"]
        if errs and not a.get("allow_errors") and not [d for d in self._diagnostics() if d["severity"] == "error"]:
            return self._err("failed", "the edit makes the scene not parse", diagnostics=diags)
        self._write_text(new, "agent", written, changed)
        self._publish("edit", rev=self.rev, by="agent", ops=a["ops"], lines=written)
        return self._ok({"rev": self.rev, "hash": text_hash(new), "lines": written, "changed": changed, "errors": len(errs)})

    def _insert(self, text, t_key, what):
        new, written, changed = apply_edit(self.text, [{"op": "insert", "text": text}], self.rev)
        self._write_text(new, "agent", written, changed)
        self._publish("edit", rev=self.rev, by="agent", ops=[{"op": "insert", "text": text}], lines=written)
        return {"line": written[0] if written else 0, "lines": written, "text": text}

    def _cmd_scene_propose(self, a, ctx):
        if "text" in a:
            p = self._safe_path("proposals/proposal-%d.scene" % int(time.time() * 1000), self.scene_path.parent)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(a["text"], encoding="utf-8")
        elif "path" in a:
            p = self._safe_path(a["path"], self.scene_path.parent)
            if p is None:
                return self._err("refused", "the path must be relative, with no '..' and no drive")
            if not p.is_file():
                return self._err("failed", f"no such file: {a['path']}")
        else:
            return self._err("bad_args", "scene.propose: give 'path' or 'text'")
        self.proposal = p
        diags = self._diagnostics(p.read_text(encoding="utf-8"))
        return self._ok({"path": str(p), "errors": len([d for d in diags if d["severity"] == "error"]), "diagnostics": diags})

    def _cmd_scene_accept(self, a, ctx):
        if not self.proposal:
            return self._err("not_ready", "no proposal is pending")
        bak = self.scene_path.with_name(self.scene_path.name + ".bak-" + time.strftime("%Y%m%d-%H%M%S"))
        bak.write_text(self.text, encoding="utf-8", newline="")
        self._write_text(self.proposal.read_text(encoding="utf-8"), "accept")
        self.proposal = None
        return self._ok({"rev": self.rev, "hash": text_hash(self.text)})

    def _cmd_scene_reject(self, a, ctx):
        self.proposal = None
        return self._ok({})

    def _cmd_scene_undo(self, a, ctx):
        if not self.undo:
            return self._err("not_ready", "nothing to undo")
        cur = self.text
        prev = self.undo.pop()
        self.text = prev
        self.redo.append(cur)
        self.rev += 1
        self.scene_path.write_text(prev, encoding="utf-8", newline="")
        self._sync_cars_from_text()
        self._publish("scene.reloaded", rev=self.rev, by="undo", changed=changed_lines(cur, prev), errors=0)
        return self._ok({"rev": self.rev, "hash": text_hash(prev)})

    def _cmd_scene_redo(self, a, ctx):
        if not self.redo:
            return self._err("not_ready", "nothing to redo")
        cur = self.text
        nxt = self.redo.pop()
        self.undo.append(cur)
        self.text = nxt
        self.rev += 1
        self.scene_path.write_text(nxt, encoding="utf-8", newline="")
        self._sync_cars_from_text()
        self._publish("scene.reloaded", rev=self.rev, by="undo", changed=changed_lines(cur, nxt), errors=0)
        return self._ok({"rev": self.rev, "hash": text_hash(nxt)})

    def _cmd_world_get(self, a, ctx):
        return self._ok({"hour": 14.0, "weather": 0, "traffic": 0, "peds": 0, "parked": 0, "police": 0, "timescale": 1.0, "scene": {}, "override": dict(self.override), "mock": True})

    def _cmd_world_set(self, a, ctx):
        if a.get("clear"):
            self.override = {}
            return self._ok({"committed": False, "override": {}})
        if "weather" in a and a["weather"] not in WEATHERS + ["game"]:
            return self._err("bad_args", "world.set: 'weather' must be nice, cloudy, stormy, foggy or game")
        keys = {k: v for k, v in a.items() if k in ("hour", "weather", "traffic", "peds", "parked", "police", "timescale")}
        if a.get("commit"):
            t = float(a.get("t", self.sim_t))
            lines = []
            for k, v in keys.items():
                vv = ("on" if v else "off") if isinstance(v, bool) else v
                lines.append(self._insert(f"at {fmt_time(t)} {k} {vv}", t, k)["line"])
            return self._ok({"committed": True, "override": {}, "lines": lines})
        self.override.update(keys)
        return self._ok({"committed": False, "override": dict(self.override)})

    def _cmd_camera_get(self, a, ctx):
        c = self.cam
        return self._ok({"mode": self.cam_mode, "pose": {"pos": c["pos"], "yaw": c["yaw"], "pitch": c["pitch"], "roll": c["roll"], "fov": c["fov"]},
                         "rows": {"right": [-1, 0, 0], "up": [0, 1, 0], "back": [0, 0, -1]}, "near": 0.1, "far": 10000.0, "aspect": 16 / 9, "valid": True})

    def _cmd_camera_set(self, a, ctx):
        p = dict(a.get("pose", {})) if isinstance(a.get("pose"), dict) else {}
        p.update({k: v for k, v in a.items() if k != "pose"})
        for k in ("yaw", "pitch", "roll", "fov"):
            if k in p:
                if not _type_ok("number", p[k]):
                    return self._err("bad_args", f"camera.set: '{k}' must be a number")
                self.cam[k] = float(p[k])
        if "pos" in p:
            if not _type_ok("vec3", p["pos"]):
                return self._err("bad_args", "camera.set: 'pos' must be [x, y, z]")
            self.cam["pos"] = [float(x) for x in p["pos"]]
        self.cam_mode = "freecam"
        return self._ok({"mode": "freecam", "pose": dict(self.cam)})

    def _cmd_camera_look_at(self, a, ctx):
        import math
        tgt = a["target"]
        if isinstance(tgt, str):
            cid = "player" if tgt == "player" else tgt.split(":", 1)[1] if tgt.startswith("car:") else None
            if cid not in self.cars:
                return self._err("failed", f"camera.look_at: no car '{tgt}'")
            tgt = self.cars[cid]["pos"]
        frm = a.get("from", self.cam["pos"])
        if isinstance(frm, str):
            return self._err("bad_args", "camera.look_at: 'from' must be [x, y, z] in the mock")
        d = [tgt[i] - frm[i] for i in range(3)]
        self.cam["pos"] = list(frm)
        self.cam["yaw"] = math.degrees(math.atan2(d[0], d[2]))
        self.cam["pitch"] = math.degrees(math.atan2(d[1], math.hypot(d[0], d[2])))
        if "fov" in a:
            self.cam["fov"] = float(a["fov"])
        self.cam_mode = "freecam"
        return self._ok({"mode": "freecam", "pose": dict(self.cam)})

    def _cmd_camera_mode(self, a, ctx):
        if a["mode"] not in ("scene", "freecam", "game"):
            return self._err("bad_args", "camera.mode: 'mode' must be scene, freecam or game")
        self.cam_mode = a["mode"]
        return self._ok({"mode": self.cam_mode})

    def _cmd_camera_key_add(self, a, ctx):
        t = float(a.get("t", self.sim_t))
        c = self.cam
        if a.get("rig"):
            text = f"cam {fmt_time(t)} rig {a['rig']}"
        else:
            text = (f"cam {fmt_time(t)} pos {c['pos'][0]:.2f} {c['pos'][1]:.2f} {c['pos'][2]:.2f} yaw {c['yaw']:.2f} pitch {c['pitch']:.2f} roll {c['roll']:.2f} fov {c['fov']:.2f}"
                    + (f" ease {a['ease']}" if a.get("ease") else ""))
        if a.get("cut"):
            text += f"\ncut {fmt_time(t)}"
        return self._ok(self._insert(text, t, "camera"))

    def _cmd_camera_key_list(self, a, ctx):
        return self._ok({"keys": [{"line": e["line"], "t": e["t"], "kind": e["track"], "text": e["text"]} for e in self._events() if e["track"] in ("cam", "cut")]})

    def _cmd_cars_list(self, a, ctx):
        return self._ok({"cars": [{k: v[k] for k in ("id", "model", "pos", "heading", "speed", "ready", "on_path")} for v in self.cars.values()]})

    def _cmd_cars_models(self, a, ctx):
        s = a.get("search", "").lower()
        return self._ok({"models": [{"name": n, "class": c} for n, c in MODELS if s in n]})

    def _cmd_car_place(self, a, ctx):
        car = self.cars.get(a["id"])
        if not car:
            return self._err("failed", f"car.place: no car '{a['id']}'")
        if not car["ready"]:
            return self._err("not_ready", f"car {a['id']} is not ready")
        car["pos"] = [float(x) for x in a["pos"]]
        if "heading" in a:
            car["heading"] = float(a["heading"])
        return self._ok({"id": a["id"], "placed": True})

    def _cmd_car_spawn(self, a, ctx):
        if ("model" in a) == ("profile" in a):
            return self._err("bad_args", "car.spawn: give exactly one of 'model' and 'profile'")
        t = float(a.get("t", self.sim_t))
        at = a["at"] if isinstance(a["at"], str) else ",".join(f"{x:g}" for x in a["at"])
        what = f"profile {a['profile']}" if "profile" in a else a["model"]
        text = f"at {fmt_time(t)} car {a['id']} spawn {what} at {at}" + (f" heading {a['heading']}" if "heading" in a else "") + (f" paint {a['paint']}" if "paint" in a else "")
        res = self._insert(text, t, "car")
        if ctx["wait"]:
            cid = a["id"]
            return self._defer(ctx, self.ready_after, lambda: self._ok(res))
        return self._ok(res)

    def _cmd_car_delete(self, a, ctx):
        t = float(a.get("t", self.sim_t))
        return self._ok(self._insert(f"at {fmt_time(t)} car {a['id']} delete", t, "car"))

    def _cmd_car_path(self, a, ctx):
        t = float(a.get("t", self.sim_t))
        sp = a.get("speed")
        text = f"at {fmt_time(t)} car {a['id']} path {a['route']}" + (f" speed {sp}" if sp is not None else "") + (f" mode {a['mode']}" if a.get("mode") else "")
        return self._ok(self._insert(text, t, "car"))

    def _cmd_car_path_record(self, a, ctx):
        if a["id"] not in self.cars:
            return self._err("failed", f"car.path.record: no car '{a['id']}'")
        self.recording = {"id": a["id"], "name": a["name"], "points": []}
        return self._ok({"recording": True})

    def _cmd_car_path_record_stop(self, a, ctx):
        if not self.recording:
            return self._err("not_ready", "no recording")
        r, self.recording = self.recording, None
        pts = r["points"] or [[0, 0, 0, 14.0]]
        self._write_route(r["name"], pts)
        return self._ok({"route": r["name"], "points": len(pts), "length_m": 0.0})

    def _write_route(self, name, pts):
        p = self._safe_path(f"routes/{name}.route" if not name.endswith(".route") else f"routes/{name}", self.scene_path.parent)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("# mock route\n" + "".join(f"{x:.2f},{z:.2f},{s:.2f},{y:.2f}\n" for x, y, z, s in pts), encoding="utf-8")

    def _cmd_path_waypoint(self, a, ctx):
        at = self.cam["pos"] if a["at"] == "camera" else a["at"]
        if not _type_ok("vec3", at):
            return self._err("bad_args", "path.waypoint: 'at' must be \"camera\" or [x, y, z]")
        pts = self.waypoints.setdefault(a["route"], [])
        pts.append([at[0], at[1], at[2], float(a.get("speed", 14.0))])
        self._write_route(a["route"], pts)
        return self._ok({"route": a["route"], "points": len(pts)})

    def _cmd_path_list(self, a, ctx):
        d = self.scene_path.parent / "routes"
        return self._ok({"routes": [{"name": p.stem, "points": sum(1 for l in p.read_text().splitlines() if l and not l.startswith("#")), "length_m": 0.0} for p in sorted(d.glob("*.route"))] if d.is_dir() else []})

    def _cmd_path_get(self, a, ctx):
        p = self._safe_path(f"routes/{a['route']}" + ("" if a["route"].endswith(".route") else ".route"), self.scene_path.parent)
        if p is None or not p.is_file():
            return self._err("failed", f"path.get: no route '{a['route']}'")
        pts = []
        for l in p.read_text().splitlines():
            if l and not l.startswith("#"):
                x, z, s, y = (float(v) for v in l.split(",")[:4])
                pts.append([x, y, z, s])
        return self._ok({"route": a["route"], "points": pts, "length_m": 0.0})

    def _cmd_query_spots(self, a, ctx):
        return self._ok({"spots": SPOTS, "mock": True})

    def _cmd_query_ground(self, a, ctx):
        return self._ok({"y": 17.6, "mock": True})

    def _cmd_query_streaming(self, a, ctx):
        return self._ok({"done": True, "mock": True})

    def _cmd_shot(self, a, ctx):
        if "t" in a:
            self.sim_t, self.playing, self.frozen = float(a["t"]), False, True
        p = self._safe_path(a["path"])
        if p is None:
            return self._err("refused", "the path must be relative, with no '..' and no drive")
        res = {"path": str(p), "width": 64, "height": 36, "t": self.sim_t + ((1.0 / self.fps) if "t" in a else 0.0), "bytes": 0, "frame": self.frame}  # the frame shows the time one step on

        def finish():
            w = self._write_shot(a["path"])
            res["bytes"] = w.stat().st_size if w else 0
            self._publish("shot.done", path=str(p), t=self.sim_t, ok=True)
            return self._ok(res)

        if ctx["wait"] or "t" in a:
            return self._defer(ctx, 0.35, finish)
        return finish()

    def _cmd_shot_schedule(self, a, ctx):
        base = self._safe_path(a["dir"])
        if base is None:
            return self._err("refused", "the dir must be relative, with no '..' and no drive")
        if not self.fixed and a["times"]:
            return self._err("not_ready", "shot.schedule needs the fixed step: call transport.mode {fixed:true} first (the stills must fall on the take's frames)")
        times = sorted(float(t) for t in a["times"])
        self.schedule = [(t, base / f"t{int(round(t * 1000)):06d}.png", i + 1, len(times)) for i, t in enumerate(times)]
        return self._ok({"count": len(times), "dir": str(base)})

    def _cmd_take_render(self, a, ctx):
        if self.take and self.take["active"]:
            return self._err("not_ready", "a take is running")
        name = a.get("name", "mock-take")
        folder = self._safe_path(f"takes/{name}")
        to = float(a.get("to", self._duration() if self._duration() >= 0 else 10.0))
        self.take = {"active": True, "name": name, "folder": str(folder), "fps": float(a["fps"]), "frames": 0, "from": float(a.get("from", 0.0)), "to": to, "sink": a.get("sink", "none")}
        self.sim_t, self.state, self.playing, self.frozen = float(a.get("from", 0.0)), "running", True, False
        return self._ok({"name": name, "fps": float(a["fps"]), "sink": a.get("sink", "none"), "from": self.sim_t, "to": to})

    def _cmd_take_status(self, a, ctx):
        t = self.take or {}
        return self._ok({"active": bool(t.get("active")), "name": t.get("name", ""), "folder": t.get("folder", ""), "fps": t.get("fps", 0), "frames": t.get("frames", 0),
                         "state": "running" if t.get("active") else (self.last_take or {}).get("state", "none"), "last": self.last_take or {}})

    def _defer(self, ctx, delay, make):
        """Sends the reply later; the id is looked up from the request being handled (set by _handle_line through ctx['base'])."""
        return self._deferred(ctx, delay, make)


# ---------------------------------------------------------------------------------------------------------------------------- the self test

def _selftest():
    import tempfile
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)
            print("FAILED:", what)

    with tempfile.TemporaryDirectory() as td:
        m = MockServer(td)
        port, token = m.start()
        # bad token and no auth
        try:
            DirectorClient("127.0.0.1", port, "nope")
            check(False, "a wrong token is refused")
        except DirectorError as e:
            check(e.code == "refused", "a wrong token is refused (code refused)")
        d = DirectorClient("127.0.0.1", port, token, name="selftest")
        check(d.hello.get("proto") == 1, "auth answers the protocol version")
        # help
        h = d.help()
        names = {c["name"] for c in h["commands"]}
        check({"camera.set", "scene.edit", "shot", "transport.seek", "take.render"} <= names and h["proto"] == 1, "help lists the commands")
        check(d.help("camera.set")["commands"][0]["args"][0]["name"] == "pos", "help of one command")
        # ping, errors
        check(d.call("ping")["pong"] is True, "ping")
        for cmd, args, code in (("nope.nothing", {}, "unknown_cmd"), ("camera.set", {"fov": "wide"}, "bad_args"), ("camera.set", {"bogus": 1}, "bad_args"),
                                ("transport.seek", {}, "bad_args"), ("shot", {"path": "../x.png"}, "refused"), ("transport.seek", {"t": 999}, "bad_args")):
            r = d.request(cmd, args)
            check(not r["ok"] and r["error"]["code"] == code, f"{cmd} {args} -> {code} (got {r})")
        # camera
        r = d.call("camera.set", pos=[819.6, 19.0, -800], yaw=80, pitch=-4, roll=0, fov=40)
        check(r["mode"] == "freecam", "camera.set -> freecam")
        g = d.call("camera.get")
        check(g["pose"]["yaw"] == 80 and g["pose"]["pos"][0] == 819.6 and g["mode"] == "freecam", "camera.get reads it back")
        # transport
        d.call("transport.pause")
        t = d.call("transport.seek", t=3.2, wait="settled")
        check(t["t"] == 3.2 and t["frozen"] and t["settled"], "seek settled")
        check(d.call("transport.get")["t"] == 3.2, "transport.get at 3.2")
        try:
            d.call("transport.step", frames=3)
            check(False, "step without the fixed step is not_ready")
        except DirectorError as e:
            check(e.code == "not_ready", "step without the fixed step is not_ready")
        d.call("transport.mode", fixed=True, fps=30)
        s = d.call("transport.step", frames=3)
        check(s["frames"] == 3 and d.call("transport.get")["t"] > 3.2, "step moves the clock")
        # scene
        sc = d.call("scene.get", text=True)
        rev = sc["rev"]
        check(any(e["track"] == "cam" and e["kind"] == "camera" for e in sc["events"]) and sc["duration"] == 10, "scene.get events and duration (the track of a camera key is `cam`, as scene.h TrackName says; its kind is `camera`)")
        cam_line = next(e for e in sc["events"] if e["track"] == "cam" and e["t"] == 4)
        lines = sc["text"].splitlines()
        orig = lines[cam_line["line"] - 1]
        r = d.call("scene.edit", base_rev=rev, ops=[{"op": "param", "line": cam_line["line"], "expect": orig, "key": "fov", "value": "35"}])
        check(r["rev"] == rev + 1 and "fov 35" in (Path(td) / "mock.scene").read_text(), "scene.edit param rewrites the word and the file")
        ev = d.wait_event("scene.reloaded", timeout=2)
        check(ev["rev"] == rev + 1 and ev["by"] == "agent", "scene.reloaded event")
        try:
            d.call("scene.edit", base_rev=rev, ops=[{"op": "delete", "line": 3, "expect": "x"}])
            check(False, "an old base_rev is stale")
        except DirectorError as e:
            check(e.code == "stale", "an old base_rev is stale")
        try:
            d.call("scene.edit", base_rev=r["rev"], ops=[{"op": "delete", "line": 3, "expect": "not that"}])
            check(False, "a wrong expect is stale")
        except DirectorError as e:
            check(e.code == "stale" and "actual" in e.details, "a wrong expect is stale with the actual text")
        r2 = d.call("scene.edit", base_rev=r["rev"], ops=[{"op": "insert", "text": "cam 2 pos 1 2 3 yaw 0 pitch 0 fov 50"}, {"op": "move", "line": cam_line["line"], "expect": lines[cam_line["line"] - 1].replace("fov 40", "fov 35"), "t": 5}])
        txt = (Path(td) / "mock.scene").read_text()
        check("cam 5 pos" in txt and txt.index("cam 2 pos") < txt.index("cam 5 pos") and "# the mock's scene" in txt, "insert in time order, move, comments kept")
        u = d.call("scene.undo")
        check(u["rev"] == r2["rev"] + 1 and "cam 2 pos 1 2 3" not in (Path(td) / "mock.scene").read_text(), "undo")
        # camera key + car spawn: [file] commands
        k = d.call("camera.key.add", t=7)
        check("cam 7 pos" in (Path(td) / "mock.scene").read_text() and k["line"] > 0, "camera.key.add inserts a line")
        sp = d.call("car.spawn", wait="settled", id="p2", model="vp_ford_mustang_08", at=[10, 17.6, 20], heading=90)
        check(sp["line"] > 0 and "car p2 spawn" in (Path(td) / "mock.scene").read_text(), "car.spawn inserts and (settled) replies late")
        cars = d.call("cars.list")["cars"]
        check({c["id"] for c in cars} >= {"p1", "p2", "player"}, "cars.list")
        # shots
        sh = d.call("shot", path="shots/a.png", t=2.0, wait="settled")
        png = (Path(td) / "shots" / "a.png").read_bytes()
        check(png[:8] == b"\x89PNG\r\n\x1a\n" and sh["bytes"] == len(png), "shot writes a PNG")
        # batch
        res = d.batch([{"cmd": "ping"}, {"cmd": "nope"}, {"cmd": "ping"}])
        check(len(res) == 2 and res[0]["ok"] and not res[1]["ok"], "batch stops at the first error")
        res = d.batch([{"cmd": "ping"}, {"cmd": "nope"}, {"cmd": "ping"}], stop_on_error=False)
        check(len(res) == 3, "batch goes on when asked")
        # events filter
        d.call("events.set", only=["car.ready"])
        d.call("camera.key.add", t=8)
        check(not list(d.events(timeout=0.2)) or all(e["event"] == "car.ready" for e in d.events()), "events.set only filters")
        d.call("events.set", only=[])
        # a second client, and the cap
        extra = [DirectorClient("127.0.0.1", port, token) for _ in range(3)]
        try:
            DirectorClient("127.0.0.1", port, token)
            check(False, "a fifth client is refused")
        except DirectorError as e:
            check(e.code == "refused", "a fifth client is refused")
        for x in extra:
            x.close()
        # take
        tr = d.call("take.render", fps=30, sink="none", name="t1", **{"from": 0, "to": 1})
        ev = d.wait_event("take.done", timeout=5)
        check(ev["name"] == "t1" and (Path(td) / "takes" / "t1" / "take.json").is_file(), "take.render -> take.done and a take folder")
        d.close()
        m.stop()
    print("director_client selftest:", "FAILED %d" % len(fails) if fails else "ok")
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Director client, mock server and selftest")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("selftest")
    mk = sub.add_parser("mock")
    mk.add_argument("--root", default=".")
    mk.add_argument("--scene")
    mk.add_argument("--info", help="write director.json here")
    cm = sub.add_parser("cmd")
    target = cm.add_mutually_exclusive_group(required=True)
    target.add_argument("--info")
    target.add_argument("--port", type=int)
    cm.add_argument("--token")
    cm.add_argument("cmd")
    cm.add_argument("args", nargs="?", default="{}")
    cm.add_argument("--wait")
    cm.add_argument("--timeout", type=float)
    a = ap.parse_args(argv)
    if a.mode == "selftest":
        return _selftest()
    if a.mode == "mock":
        m = MockServer(a.root, scene_path=a.scene)
        port, token = m.start()
        print(json.dumps(m.info()), flush=True)
        if a.info:
            m.write_info(a.info)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0
    if a.mode == "cmd":
        if a.port is not None and not a.token:
            cm.error("--port requires --token")
        try:
            d = DirectorClient.from_info(a.info) if a.info else DirectorClient("127.0.0.1", a.port, a.token)
            with d:
                r = d.request(a.cmd, json.loads(a.args), wait=a.wait, timeout_s=a.timeout)
        except (DirectorError, OSError, TypeError, ValueError, KeyError) as e:
            print(f"director_client: {e}", file=sys.stderr)
            return 2
        print(json.dumps(r, indent=1))
        return 0 if r.get("ok") else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
