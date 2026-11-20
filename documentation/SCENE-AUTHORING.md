# Writing a scene: for agents, alone or live with a human

A scene is a text file: what the world, the cars and the camera do on a timeline. A trailer shot, a visual test and a before/after clip are all scenes. This file is
everything an agent needs to write one, check it, look at it and render it. The reference for the grammar is the top of `src\mc_engine\cinematic\scene.h`,
`camera.h`, `car_verbs.h` and `spots.h`; the wire of the live channel is `documentation\DIRECTOR-PROTOCOL.md`; the design is `feature-parity\DIRECTOR-DESIGN.md`.

Everything below is one command line, `ports\scripts\scene_tool.py` (written `st` here). Every command takes `--json` (one JSON object on stdout) and exits
**0** done, **1** the content is wrong (lint error, a failed preview check, two things differ), **2** bad arguments, **3** the environment (no atlas, no game,
the game said no). `lint`, `query`, `route`, `new` and `diff` of files need no game and run under `python -I`; `preview` needs a game and Pillow (plain `python`).

## The loop

```
st new  my-shot --recipe chase        # a lint-clean scene to start from (scenes\recipes; `st new --list`)
   ... edit ports\scripts\scenes\my-shot.scene (the places and numbers) ...
st lint my-shot                       # errors with the line and the line's text; run it after every edit
st preview my-shot --slot auto --exe-dir ports\exe\<build>     # or --attach <running game>: contact sheet + map + preview.json
   ... read contact.png, map.png, preview.json (below), fix, lint, preview again ...
st render my-shot --attach slot:0 --sink ffmpeg                 # the take; or --exe-dir <build> (scene_run.py)
```

Rules of the PC: one game at a time, slot runs side by side (`--slot auto` takes a free one through lockrun). If you may not start a game, print what would run
with `--plan` and ask the main session. `st slots` lists the games that run with the channel; `st preview --plan` prints the shots and the command.

## A scene in a minute

```
scene my-shot fps 60 step 1/60                   # name, the take's frame rate (the preview runs at this step too)
world hour 16.5 hold weather nice traffic off peds off parked off police off   # hold the world still: nothing drives into the cars
at 0 hud off                                     # the game's own UI off
at 0.5 car p1 spawn vp_lam_murcielago_06 at spot:hw-street@-1.5,0,-26 heading spot:hw-street     # a spawn takes about 4 s
at 6 car p1 path street-lane1.route speed 7      # a route file beside the scene or in its routes\ folder
cam 6 rig chase car:p1 dist 7 height 1.9 lag 0.35 fov 50                  # a shot in one line
cam 9 rig chase car:p1 dist 3.5 height 1.1 side 1.6 lag 0.2 fov 42 ease inout
at 6 capture fps 60 name my-shot sink png        # the take (one a scene); `capture off` or `end` closes it
at 13 capture off
cam 13 off                                       # the game's camera again
end 14.5
```

One statement a line; `#` starts a comment; times are seconds or `m:ss`; a key's value holds until the next key of its track; keys of a track are in time order
(the lint says when not). The clock starts at 0 when the cars are on the road.

| Statement | What it does |
|---|---|
| `scene <name> [fps n] [step 1/n]` | header; `fps` is the take's rate |
| `world hour <h> hold weather <w> traffic\|peds\|parked\|police on\|off` | the world at time 0; `hour a..b over <s> [ease e]` ramps; `weather nice\|cloudy\|stormy\|foggy\|game`; `timescale <x>`; peds/parked also take a density 0 to 2 |
| `set <setting> <value>` | a live setting for the scene (put back at the end); never a path, program or `mcla_capture_*`/`mcla_scene*`/`mcla_director*` |
| `label "text" [side s] [fade s] [for s]`, `mark <name>` | data for a compositor / an alignment point in the logs |
| `split <x>\|<a>..<b> over <s>\|off` | the before/after split of the renderer |
| `hud off\|on\|<alpha>`, `hud fade over <s> to <x>` | the game's own UI |
| `car <id> spawn <model>\|profile <N> at <place> [heading <deg>] [paint <n>]` | a car; `profile N` = garage slot N, dressed as the owner left it; `ambient <model>` = a real traffic car that stands still; `near <car> <right,up,forward>` instead of `at` |
| `car <id> path <route> [speed 12\|x1.5] [mode kinematic\|brain]`, `stop`, `delete`, `car player profile N`, `car player at <place>` | a route (kinematic = put on the route every update: exact; brain = the game's AI drives: not exact); the id is what `car:<id>` names |
| `cam <T> <keys>`, `cut <T>` | the camera (next section); `cut` = no interpolation across T, the renderer's histories restart |
| `capture fps <n> [name x] [sink none\|ffmpeg\|png\|raw] [statelog on]`, `capture off` | a take |
| `statelog on` | the per-update state log (take_diff.py) |
| `end <T>` | the end; without it the scene never finishes |

**Camera keys** (words in any order after `cam <T>`): `pos x y z`; `attach car:<id> dx dy dz [heading] [follow s]` (right, up, forward of the car; 0 2 -6 is six metres
behind); `orbit car:<id> r <m> az <deg> el <deg>` (az 0 behind, 90 right); `yaw <deg> pitch <deg>` (yaw 0 looks along +Z, 90 along +X); `look car:<id> [dx dy dz]` or `look x y z`;
`roll`, `fov` (vertical degrees), `near`, `far`; `ease linear|in|out|inout` (how the segment that starts here runs); `off`.

**Rigs** (`cam <T> rig <kind> ...`, also with `fov roll near far ease`): `chase car:p1 [dist 6] [height 1.8] [side 0] [lag 0.3] [aim 1] [ahead 0]`;
`orbit car:p1 [r 8] [az 20..200 over 8] [el 10]`; `tripod at <place> track car:p1 [dx dy dz] [aimlag s]`; `flyby path <route> [speed x1] [up 2] track car:p1`;
`hood|bumper|roof car:p1 [ahead 30]`. A rig is a function of the scene time, so the preview and the take agree.

## Places, headings, spots: no raw coordinates when a spot exists

World metres: x east, y up, z south. A **heading/yaw** is degrees: 0 faces +Z (south), 90 faces +X (east), 180 north, 270 west. `spots.txt` (scenes\, or beside the scene) names places:
`hw-street 832.19 16.5 -787.53 62  # the street outside the Hollywood gate`. In a `cam`, `car` or `start` statement:

| Word | Means |
|---|---|
| `spot:hw-street` | that place (x,y,z) |
| `spot:hw-street@-1.5,0,-26` | 1.5 m to the spot's left, 26 m back, in the spot's heading frame (right, up, forward: the frame of `attach` and `near`) |
| `spot:hw-street@1.5,0,0,180` | the same place offset, and the fourth number turns the heading (the oncoming lane) |
| `heading spot:hw-street`, `yaw spot:hw-street` | the spot's heading |

`st query spots [--near spot:hw-lot --r 80]` lists them. To add one: put a line in the scene folder's `spots.txt` (name, x y z heading, a one-line note with where the numbers came
from). Heights are the height a car stands at on the road there.

## Roads, lights, routes: ask, don't guess

The atlas (`ports\atlas\<hash>\`, built from the game's files: `st atlas build`) knows the roads (lanes with direction, speed and street name), the street lamps, the garages
and the car models. Offline answers have the shape of the game's `query.*` commands:

```
st query road   --near spot:hw-street          # the nearest lanes: name, speed, direction, distance
st query lights --near spot:hw-street --r 60   # lamps: position, colour, reach (a night shot wants some)
st query route  --from spot:hw-street@-1.5,0,-26 --to spot:hw-street@-1.5,0,24      # a dry run
st route --from spot:hw-street@-1.5,0,-26 --to spot:hw-street@-1.5,0,24 --name my-run --scene my-shot --speed 12   # writes scenes\routes\my-run.route
st query route --file scenes\routes\my-run.route     # length, time, speeds, how far from the road
```

`route` follows the lanes (the start's heading picks the lane; a start more than 6 m from a lane is warned about), then writes `x,z,speed,y` lines about 2 m apart with the speed from
the roads' limits and the curves (`--speed` caps it). Without an atlas it makes a smooth line through the stops and says so. Use it as `car p1 path my-run.route [speed x1.2]`.
`query ground|pick|streaming|visible` need a running game (`--live`).

## Recipes

`st new --list` shows what exists (scenes\recipes): `chase` (follow a car down the street), `flyby` (a camera along a route past a parked car), `orbit-reveal` (a parked car, a
camera circling it), `night-street` (real lamps, a tripod on a car driving through), `two-car-pass` (opposite lanes, a camera each side), `test-fixed-camera` (a visual test: spawned cars,
a fixed camera, a fixed step), `all-rigs`. A recipe uses spots and a route, no coordinates; copy, then change the spot, the car and the numbers.

## Preview: what to read

`st preview <scene> --slot auto --exe-dir <build> [--every 2] [--size 1280x720] [--out DIR] [--subject p1] [--json]`. It lints, starts (or attaches to) a game, loads a copy of the scene
with its `capture` keys commented out (a preview never makes a take), sets the take's fixed step, waits until every car is ready and the world is streamed, and takes a clean engine
picture at: every `cam` key, every `cut` and one update before it, the start, the middle of each long stretch, every `--every` seconds inside the take, and the last update.

- **`contact.png`** (`contact-2.png` ...): the pictures in a grid under 1600 px wide, each numbered `#n`, with its time, kind (KEY, CUT, PRECUT, MID, EVERY, END), the key's line and
  text, and a verdict (green ok, red `x check`, yellow `?` could not be checked). A yellow ring is where the game says the subject is.
- **`map.png`**: top-down, the numbers are the contact sheet's. Roads grey, car routes coloured by speed with arrows, a yellow wedge at every key and cut (the view), a red ring on a
  cut. This is where you see a route that leaves the street or a camera that stands inside a house.
- **`preview.json`** (also on stdout with `--json`): `pass`, `failed[]`, `warnings[]`, and per shot `t, kind, line, file, camera{pos,yaw,pitch,fov}, subject{id, screen[u,v], in_frame,
  size_frac, dist_m, occluded}, cars[{id,pos,speed,dist_to_road_m,on_road}], streaming_ready, ground_clearance_m, checks{...}`. Check values: `pass`, `fail`, `unknown` (could not be made:
  not a failure), `n/a`.
- **The checks** (thresholds in the file, `--thr key=value`): the subject (the car the active key looks at or follows; else `--subject`; else the scene's only car) has its centre in the
  inner 90 percent of the picture and fills 3 to 90 percent of its height; not occluded when the game can tell; every car that drives a route is within 4 m of a road at every shot (6 m
  along the whole route); the world was streamed (retried up to 10 s, then flagged); the camera is 0.3 m above the known ground.

Fix what `failed` names, then `lint` and `preview` again. The shots are at the scene's own step; `preview.json` `strategy` says how: `seek` pins the clock (exact for a scene of pure functions of
the time), `step` runs the scene stepwise (brain cars, traffic: what the take will see), `play` is pictures only.

**Preview equals take, for these:** camera keys and rigs, kinematic car routes (a car's place is the closed form of the time since its path key), hour, weather, hud, split, labels, spawn
places. **Not exact:** `mode brain` cars, traffic, peds, wheel physics and suspension; temporal effects need a few frames after a cut (the first frames of a take after a cut differ from a
scrubbed still). The lint and the preview warn about brain cars.

## The take

`st render <scene> --attach slot:0 --sink ffmpeg [--fps 60] [--name x] [--from t --to t]` makes the take through the running game (`take.render`), waits for `take.done` and checks `take.json`
(complete, one picture per update, none dropped or repeated, the files). Without `--attach` and `--channel`, `st render <scene> --exe-dir <build> --run-args --scale 3 --window 1920x1080` runs
`scene_run.py` (the scene needs its own `capture fps` key). The last line is `take folder: <path>`. Never edit the scene file while a take renders. `st diff A B` compares two state logs
(`take_diff.py`), two takes or two previews (a picture diff at the same scene times: IDENTICAL, EXACT, CLOSE, LOOSE; `--out DIR` writes a sheet).

## Working live with a human

The game runs with `mcla_director` (opt_run `--director`): `director.json` in the run folder says port and token. `st slots` finds it; `--attach slot:N` (or the run's name) uses it; a
game you attach to gets its own scene back after a preview.

- **Human leads** (the Director panel): every change the human makes writes the scene file; you see the event `scene.reloaded {by:"panel", changed}` and can refine. Read the file again
  before you edit; edit with line-addressed ops so you keep their comments and layout:
  `st cmd slot:0 '{"cmd":"scene.edit","args":{"base_rev":12,"ops":[{"op":"param","line":14,"expect":"cam 5 rig chase car:p1 dist 7 fov 50","key":"fov","value":"40"}]}}'`
  (`expect` is the old text of the line: if it moved, the answer is `stale`: read `scene.get` again). Ops: `insert`, `replace`, `delete`, `move` (the time word), `param` (one word pair).
- **Agent leads**: edit the file directly (the game reloads it softly and keeps the clock and cars) or send a proposal (`scene.propose`): the human sees what changed, previews, accepts or
  rejects; every revision is undoable (`scene.undo`).
- **Alone**: the loop above, no panel. A scene an agent built opens in the panel unchanged and a scene made in the panel lints and previews without a human.

Other commands, all one line: `st cmd slot:0 help` (every command with its arguments), `camera.get`, `camera.set pos=819.6,19,-800 yaw=80 pitch=-4 fov=40`, `cars.list`,
`transport.seek t=7 --settle`, `shot path=shots/a.png t=7`, `log.tail n=40 filter=scene`.

## Do and don't

- Do lint after every edit; do preview before you render; do read the map, not only the pictures.
- Do use a spot where one exists and `st route` for car routes; do not write coordinates you guessed (the m4c routes were 3 m off the road; `query route --file` says so).
- Do hold the world (`traffic off peds off parked off police off`) in a shot with cars; a spawn takes about 4 s, keep the first camera key after the cars are ready.
- Do keep one take a scene, and the `end` after it.
- Do give `expect` text with every edit made through the channel; on `stale` re-read.
- Don't edit the scene while a take renders. Don't put paths, programs or `mcla_capture_*`, `mcla_scene*`, `mcla_director*` in a `set`.
- Don't trust a brain-mode preview picture to repeat; use kinematic routes for anything that must match.
- Don't start a second game without the lock (`--slot auto`); don't leave a game you started running (`--keep-game` keeps it on purpose).
