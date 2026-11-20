# Director protocol (proto 1)

The Director is the trailer toolkit's command layer: a panel button, a scene-file line and an agent's request are the same thing, a **named command**
with JSON arguments. This file is the wire's reference. The design is `feature-parity\DIRECTOR-DESIGN.md`; the C++ interface is
`src\mc_engine\cinematic\director\director.h` (frozen: names and shapes only grow). `{"cmd":"help"}` on a live game returns the same command list with
every argument, so an agent needs no other document.

## Turning it on

Everything is debug-only and off by default; a scene may not set any of these (`mcla_director*` is a refused prefix of the scene's `set`).

| Setting | What |
|---|---|
| `mcla_director` | on: the game listens on `127.0.0.1` (loopback only) and runs the commands. Live: switching it on starts the listener, off stops it. |
| `mcla_director_info` | a file name: the game writes `director.json` there when the listener is up (and removes it at exit). `opt_run.py --director` passes it. |
| `mcla_director_root` | the folder commands may write to (shots, takes, routes, proposals). Empty: the game's working folder. A scene's own folder is always allowed too. |

`director.json`: `{"proto":1,"pid":1234,"port":51234,"token":"<32 hex>","slot":"d1","exe":"larecomp.exe","version":"0.11","scene":"scenes\\x.scene","started":"2026-11-19T20:00:00Z"}`.
The port is chosen by the OS (port 0): two game instances (two slots) never clash, and `ports\run\<slot>\director.json` says which is which.
`slot` is the name of the info file's folder. A listener on 127.0.0.1 raises no firewall prompt (the Director binds nothing else).

## The channel

TCP, UTF-8, **JSON lines**: one JSON object per line (`\n`, a `\r` before it is ignored), at most 1 MiB a line. At most 4 clients at once.

1. The first line of a client must be `{"cmd":"auth","token":"<token from director.json>","name":"my-agent"}` (`name` optional: shown in the panel). Anything else
   first, a wrong token, or a fifth client gets a `refused` reply and the connection is closed. Auth is answered with
   `{"ok":true,"result":{"proto":1,"version":"...","pid":...,"slot":"...","scene":"...","commands":N}}`.
2. Then requests: `{"id":7,"cmd":"camera.set","args":{"pos":[819.6,19.0,-800],"yaw":80,"pitch":-4,"roll":0,"fov":40}}`. `id` is yours (any JSON integer;
   a request without one is answered without one); `args` may be left out.
3. Replies, one line each, in the order the commands finish (a deferred one may finish after a later fast one; match on `id`):
   ```
   <- {"id":7,"ok":true,"result":{"mode":"freecam"}}
   <- {"id":8,"ok":false,"error":{"code":"not_ready","msg":"cars p1 not ready (spawned 1.2 s ago)"}}
   ```
4. Events, no `id`, whenever they happen: `<- {"event":"scene.reloaded","rev":12,"by":"panel","changed":[14,15],"errors":0}`.

Request fields beyond `id`, `cmd`, `args`:

| Field | Meaning |
|---|---|
| `wait` | `"settled"`: for `transport.seek`, `shot`, `scene.load`, `car.spawn`, `transport.step`, `transport.restart` the reply is held until the world has settled (the frames are rendered, every car is ready, the streaming is done). Without it they answer at once (`settled:false`). |
| `timeout_s` | how long a held reply may take (default 30); after it the reply is a `timeout` error. |

Error codes (frozen): `bad_args` (missing, unknown or mistyped argument, bad JSON, a value out of range; the message names the argument), `unknown_cmd`,
`not_ready` (the thing exists but is not usable yet), `no_world` (a menu, a loading screen or a race: there is no world to act on), `stale` (the scene's
revision or an edit's `expect` text is not what you saw: re-read), `refused` (a path or setting the Director does not allow; a bad token), `timeout`,
`failed` (anything else; the message says what). An error object may carry more fields (a `stale` edit has `line`, `actual` and `op`; a refused edit has `diagnostics`).

`batch` runs several commands in one update, in order: `{"cmd":"batch","args":{"cmds":[{"cmd":"camera.set","args":{...}},{"cmd":"shot","args":{...}}]}}`
-> `{"ok":true,"result":{"results":[{"ok":true,"result":{...}},{"ok":false,"error":{...}}]}}`; it stops at the first error unless `"stop_on_error":false`.

Conventions: times are scene seconds (numbers); positions are `[x, y, z]` in metres, world axes (x east, y up, z south); headings and angles are degrees in
the scene file's meaning (heading and yaw 0 look along +Z, 90 along +X; pitch + looks up; roll + lifts the right side); a car is named by its scene id
(`"player"` is always known). A **target** is `"car:p1"`, `"player"` or `[x, y, z]`.

**Paths.** A command that writes a file (`shot`, `shot.schedule`, `take.render`, `scene.propose`, `car.path.record`, `path.waypoint`) takes a path that is
relative, with no `..` and no drive: it is looked up under `mcla_director_root` (shots, takes) or the scene's folder (routes, proposals). Anything else is
`refused`. Every edit of the scene is also kept in `<scene folder>\.director\<scene name>\rev-N.scene` (local, git-ignored): `scene.undo` and the panel use it.

## The commands

`[file]` = the command also writes the scene file (it inserts or replaces a line; the new line shows in `result.lines`), then the scene reloads softly (the
clock and the cars are kept, only a car whose statement changed is made again). The rest (transport, a free-camera pose, shots, queries) never touch the file.

### core
| Command | Args -> result |
|---|---|
| `help` | `{cmd?}` -> `{proto, commands:[{name, group, summary, file, open_args, args:[{name, type, required, doc}], result}], errors:[...], events:[{name, doc}]}` (one command when `cmd` is given) |
| `ping` | `{}` -> `{pong:true, frame, t, state}` |
| `batch` | `{cmds:[{cmd, args?}], stop_on_error?:true}` -> `{results:[reply...]}` |
| `events.set` | `{on?:bool, only?:[names]}` -> `{on, only}`: mute the connection's events, or take only the named ones |
| `log.tail` | `{n?:50, filter?:text, level?:"warn"}` -> `{lines:[{seq, level, cat, text}], latest}`: the game's log lines (`[scene]`, `[cars]`, `[cap]`, `[dir]`) so an agent reads errors without the file |
| `ui.open` | `{panel}` -> `{}`: asks the panel to open (`"director"`, `"timeline"`, or a tab: `"camera"`, `"rigs"`, `"cars"`, `"paths"`, `"take"`, `"agent"`); the panel is `mcla_director_panel`, the key Ctrl+F12, a row on the DEBUG menu's TOOLS page. It also sets how the dock looks: `"collapse"` / `"expand"` (the bar alone, or the whole dock), `"hide"` / `"show"` (as Tab does), `"size-small"` / `"size-normal"` / `"size-tall"`, `"lanes-collapse"` / `"lanes-open"` (every lane but CAMERA a thin strip, or all open), `"tabs-left"` / `"tabs-right"` (scroll the tab strip by one) |
| `ui.select` | `{line}` -> `{}`: points at a key; the panel highlights it |
| `quit` | `{}` -> `{quitting:true}`: ends the game |

### transport (the scene's clock)
| Command | Args -> result |
|---|---|
| `transport.get` | `{}` -> `{state, t, duration, fps, step, paused, frozen, frame, mode, scrubbable, scrub_hint, holding_for_ready, world}`. `state`: off, pending, waiting (for the world), running, finished, failed. `mode`: `fixed` (a fixed step is in force) or `realtime`. `world`: a city is loaded (a car on the road, the ambient manager built); false in the title screen, a menu and while the city loads. transport.get answers in all of those, it does not say `no_world`: a tool that waits for a city reads this field. |
| `transport.play` | `{}` -> `transport.get` fields. Releases a pause or a seek; the clock goes on from where it stands. |
| `transport.pause` | `{}` -> same. Holds the clock and freezes the world (time scale floor). |
| `transport.restart` | `{t?:0, wait?}` -> same. Runs the scene again from the start (the cars are made again) and, when `t` > 0, seeks there. |
| `transport.seek` | `{t, settle_frames?:20}` -> `{t, paused:true, frozen:true, scrubbable, scrub_hint, settled}`. Pins the clock at `t` with the world frozen: exact for the camera, kinematic cars, hour, weather, hud and split; `scrubbable:false` and a hint when a brain-mode car, traffic or peds are in the scene (the world cannot rewind: use `transport.restart` and play). |
| `transport.step` | `{frames?:1}` -> `{t, frames}`. While paused: runs exactly that many updates at the fixed step and holds again. Needs the fixed step (`transport.mode {fixed:true}` first), else `not_ready`. |
| `transport.mode` | `{fixed?:true, fps?}` -> `{mode, fps, step}`. Preview at the scene's fixed step (the take's): `fps` defaults to the scene's header `fps`. |

### scene
| Command | Args -> result |
|---|---|
| `scene.load` | `{path}` -> `{path, state}`. Arms that scene file (a path, or a name looked up next to the game and in `scenes`). With `wait:"settled"` the reply comes when it runs. |
| `scene.get` | `{text?:false}` -> `{path, name, rev, hash, events:[{line, t, track, text, kind, car?}], diagnostics:[{line, severity, message}], car_ids, duration, fps, text?}` |
| `scene.edit` | `{base_rev, ops:[...], allow_errors?:false}` -> `{rev, hash, lines, changed, errors}`. See below. |
| `scene.propose` | `{note, path?, text?}` -> `{path, errors, diagnostics}`. A proposal is another file beside the scene (`text` writes one into `<scene folder>\proposals\`); the panel offers Preview / Accept / Reject. |
| `scene.accept`, `scene.reject` | `{}` -> `{rev, hash}` / `{}`. The panel's buttons: accept replaces the scene with the proposal (the old one is kept as `.bak-<time>`). |
| `scene.undo`, `scene.redo` | `{}` -> `{rev, hash}`. The file as it was before the last change (by anyone). |
| `world.get` | `{}` -> `{hour, weather, traffic, peds, parked, police, timescale, scene:{...the timeline's state at now}, override}` |
| `world.set` [file] | `{hour?, weather?:"nice"\|"cloudy"\|"stormy"\|"foggy", traffic?, peds?, parked?, police?, timescale?, commit?:false, t?}` -> `{committed, override}`. Without `commit` a live preview only (the file is untouched); `{clear:true}` drops it. With `commit:true` it inserts `at <t> hour ...` lines (`t` defaults to now). |

**scene.edit.** `base_rev` is the revision you read (`scene.get.rev`, or the one in the last `scene.reloaded` / `edit` event); if the file changed since, the reply
is `stale` and you re-read. The ops apply together or not at all, and their line numbers are those of the text of `base_rev` whatever earlier ops did:

| op | |
|---|---|
| `{"op":"insert","text":"cam 3.2 pos 1 2 3 yaw 80"}` | placed in time order after the last line of the same track at or before its time; else after `scene` / `world` |
| `{"op":"replace","line":14,"expect":"old line","text":"new line"}` | |
| `{"op":"delete","line":14,"expect":"old line"}` | |
| `{"op":"move","line":14,"expect":"...","t":4.0}` | rewrites only the time word |
| `{"op":"param","line":14,"expect":"...","key":"fov","value":"35"}` | rewrites the word after `key` (as many words as `value` has); appends `key value` when the line has none |

Comments, blank lines, line endings and order are kept. A result that makes the scene not parse is refused (`failed` with the diagnostics) unless `allow_errors`.
The file is written atomically, the Director recognises its own write (no reload from outside), and the scene reloads softly at once.

### camera
| Command | Args -> result |
|---|---|
| `camera.get` | `{}` -> `{mode, pose:{pos, yaw, pitch, roll, fov}, rows:{right, up, back}, near, far, aspect, valid}`. `mode`: `scene` (the scene's camera is the picture's), `freecam`, `game`. |
| `camera.set` | `{pos?, yaw?, pitch?, roll?, fov?}` (or the same inside `pose`) -> `{mode:"freecam", pose}`. The free camera is placed there (no key is written); fields left out stay. `no_world` when no city is loaded (the title screen, a menu, a loading screen): the pose would never be applied. |
| `camera.look_at` | `{target, from?, fov?}` -> same. `target`: a target; `from`: where to stand (default: where the camera is). |
| `camera.mode` | `{mode:"scene"\|"freecam"\|"game"}` -> `{mode}`. `game` leaves the picture to the game's own camera. |
| `camera.key.add` [file] | `{t?:now, ease?, cut?:false, rig?}` -> `{line, lines, text}`. Inserts `cam <t> pos ... yaw ... pitch ... roll ... fov ...` from the camera as it is now (plus a `cut <t>` line when `cut`); `rig` is a rig's words instead of a pose (`"chase car:p1 dist 6"`). |
| `camera.key.list` | `{}` -> `{keys:[{line, t, kind:"cam"\|"cut", text}]}` |

### cars
| Command | Args -> result |
|---|---|
| `cars.list` | `{}` -> `{cars:[{id, model, pos, heading, speed, ready, on_path}]}` (the scene's cars and `player`) |
| `cars.models` | `{search?}` -> `{models:[{name, class}]}`: the game's vehicle names for the picker |
| `car.place` | `{id, pos, heading?}` -> `{id, placed}`. Live and transient (nothing is written): the car is put there now. A car on a kinematic path goes back to its route. |
| `car.spawn` [file] | `{id, model\|profile, at, heading?, paint?, t?:now}` -> `{line, lines, text}`; with `wait:"settled"` it replies when the car is ready. `at` is `[x,y,z]` (or a spot word). |
| `car.delete` [file] | `{id, t?:now}` -> `{line, lines, text}` |
| `car.path` [file] | `{id, route, speed?, mode?:"kinematic"\|"brain", t?:now}` -> `{line, lines, text}` |
| `car.path.record` | `{id, name, every_m?:2}` -> `{recording:true}`: records a car's drive (the player's, or a scene car's) |
| `car.path.record.stop` | `{}` -> `{route, points, length_m}`: writes `routes\<name>.route` (`x,z,speed,y`, reduced to what the shape needs) |
| `path.waypoint` | `{route, at:"camera"\|[x,y,z], speed?}` -> `{route, points}`: appends a waypoint to `routes\<route>.route` (creating it) |
| `path.set` | `{route, points:[[x,y,z,speed]]}` -> `{route, points, length_m}`: writes the whole route file again (the panel's edits: a point moved or deleted, a speed changed, the route smoothed); 2 to 5000 points, speed 0.5 to 120 |
| `path.list` | `{}` -> `{routes:[{name, points, length_m}]}` |
| `path.get` | `{route}` -> `{route, points:[[x,y,z,speed]], length_m}` |

### query (package C: DIRECTOR-ATLAS-NOTES.md has the arguments)
`atlas.dump`, `query.spots`, `query.lights`, `query.road`, `query.route`, `query.ground`, `query.pick`, `query.garages`, `query.streaming`, `query.visible`.
Always `{ok, result|error}`; `query.ground` says `not streamed` where the world is not loaded there.

### capture
| Command | Args -> result |
|---|---|
| `shot` | `{path, size?, t?}` -> `{path, width, height, t, bytes, frame}`. A clean engine frame (no overlay, no ImGui) as a PNG, at the engine's 1:1 size (`size` is a hint: the result says the real one). With `t` it seeks there first and waits for the world to settle. |
| `shot.schedule` | `{times:[...], dir}` -> `{count, dir}`. Stills at exact scene times during one play-through (it plays the scene if it is paused; needs the fixed step, else `not_ready`); an event `shot.done` for each. `{times:[]}` cancels. |
| `take.render` | `{fps, sink?, name?, from?, to?}` -> `{name, fps, sink, from, to}`. Starts a take (the scene runs from `from` to `to`, default the whole scene); events `take.progress`, `take.done`. |
| `take.status` | `{}` -> `{active, name, folder, fps, frames, state, error?, last}` |

## Events
| Event | Fields |
|---|---|
| `scene.reloaded` | `rev, by` (`agent`, `panel`, `file`, `undo`, `accept`), `changed` (new line numbers), `errors`, `soft` (loaded in place: the clock and the cars were kept) |
| `scene.state` | `state` (`running`, `finished`, `failed`, ...), `t`, `path` |
| `key.reached` | `t, line, track, text`: the scene's clock passed a key |
| `car.ready` | `id` |
| `shot.done` | `path, t, ok, error?` (and `index, total` for a schedule) |
| `take.progress`, `take.done` | `name, frames` / `name, folder, state, frames, error?` |
| `edit` | `rev, by, ops, lines`: what a client changed in the scene |
| `agent` | `client, name, connected`: an agent came or left (the one that connects does not hear about itself) |
| `ui.geometry` | the Director panel (mcla_director_panel) says where its things are, in ImGui's units (`display` = ImGui's DisplaySize; the window's client area in pixels over it is the scale): `hidden, collapsed, size ("small"|"normal"|"tall"), font, tab, tabs:{first,last,overflow}, picture:[x,y,w], axis:{t0,t1,x0,x1}, lanes:[{title,y,h,collapsed}], markers:[{line,t,x,y,label}], widgets:{"PLAY":[x,y,w,h], "DOCK":[...], "TAB CARS":[...], "TAB >":[...], "LANES":[...], "TIMELINE":[...]}` (a tab scrolled out of view has no widget until the strip scrolls to it: `"TAB <"` and `"TAB >"` are the arrows). At most four a second, when something moved; `ui.open` asks for one at once. A test that clicks and drags with the real mouse reads it. |

## The pure parts' tests
`larecomp_director_test` (JSON, the command engine, the console grammar, the paths rule, the PNG writer, the route reducer, the projection, a loopback
round trip of the server) and `larecomp_scene_edit_test` (the ops, comments and layout kept, the stale rule, the diff) need no game. The Python side is
`ports\scripts\director_client.py` (`python -I director_client.py selftest` runs its client against its own mock server, which answers every command above with
plausible data so a tool can be built and tested with no game).
`ports\scripts\director_smoke.py --info <director.json>` runs the checks of the core against a live game (hello, camera, transport, shot, reload, take); with `--mock`
it runs them against the mock, which tests the script itself.
