# Debug menu and diagnostics

Notes for testers and contributors. A player never sees any of this: the debug menu stays hidden until someone unlocks it on purpose, and the pause menu, the launcher's normal pages and the README do not mention it.

---

## Unlock

Three ways. Each unlocks it for that run:

| How | Example |
|---|---|
| Command line | `larecomp.exe --debug_menu` |
| Environment | `$env:REX_DEBUG_MENU = "1"; Start-Process larecomp.exe` |
| `larecomp.toml` | `debug_menu = true` (written by hand; the game never writes this line) |

The command line wins over the environment, and the environment over the file, so `--debug_menu=false` locks it again for one run.

The launcher has a way too. Start it with `--advanced` (or set `LARECOMP_LAUNCHER_ADVANCED=1`), open Settings, then the Advanced tab, and turn on **Debug menu in the game**. Play then adds `--debug_menu=true`. The same tab has **More game options**, a box for extra launch arguments such as the ones listed below.

The answer is fixed at start. Nothing can unlock a running game: not a menu, not F4, not the console. The log says what happened (`logs/larecomp_NNN.log`, see [RUNNING.md](RUNNING.md)):

```
[debug] debug menu unlocked (command line)
```

or `[debug] debug menu locked`. The source is `command line`, `environment` or `larecomp.toml`.

## What changes when it is unlocked

- The pause menu's SETTINGS tab gets a **DEBUG** button, the last one before QUIT. It opens the seven pages below.
- **F4** lists every setting. Locked, it lists only the key bindings and the few settings that have no other place (the online server choices, a wheel's emergency stop for its force). Settings that apply after a restart are yellow, and the panel says debug settings are not saved.
- **F2** dumps the textures of the next frame and **F10** writes a raw trace of the next frame (Plume renderer). **F11** captures the next frame with RenderDoc. **F9** warps the car to a mod's warp point and back; it can break a mission. These four keys do nothing while locked. F3 (the telemetry card) and the other keys work for everyone.

## Debug changes are never saved

A debug-only setting that is changed while the game runs (a DEBUG page, F4, the console) lasts until the game closes. It is not written to `larecomp.toml`, and a line the file already has for it stays exactly as written. F4's Save says how many it left out. Values from the command line, the environment or the file are honoured as always.

To keep one, use a launch argument, the launcher's **More game options**, or a line in `larecomp.toml`.

For bug reports, the log lists every debug setting that is not at its default at each start, locked or not, with where the value came from (`command line`, `environment`, `larecomp.toml` or `this session`):

```
[settings] 2 debug setting(s) are not at their default:
[settings] fence_spin_throttle = false (larecomp.toml)
[settings] mcla_gpu_ao_view = 3 (command line)
```

On the DEBUG menu, **INFO > COPY LAUNCH ARGUMENTS** puts `--debug_menu --name=value ...` for every such setting on the clipboard (and in the log). Paste it into a report, or run it to repeat the session. **SHOW CHANGED SETTINGS** writes the list to the log again. **RESET DEBUG SETTINGS** puts the live ones back to their defaults for this session; a restart-only one stays where it is.

One thing is still written: a restart-only choice that a menu holds for the next start (the RENDERER row), because it cannot work any other way.

## The pages

Every row is live: it takes effect at once and is never saved. A page has at most 24 rows. Rows that only the Plume renderer has show only when it runs.

| Page | What it is for |
|---|---|
| FIXES | Each fix of the recomp with the switch that turns it off (real frame delta, suspension fix, smooth chase cam, the UI and dither fixes), to see whether a fix caused a bug. |
| PERFORMANCE | Pacing and speed switches: pace at presentation, fence spin yield, FPS limit spin, single tile, shadow phases, foliage, break FPS floor. |
| RENDERING | The Plume renderer's debug views (AO, HDR layers), performance markers, the ghost and memory logs, the aspect ratio override, and on the Xenia renderer XENIA VSYNC (it only slows the game's pacing to the vblank; it does not stop tearing). |
| CAMERA | The developer free camera, the free-look collision tests, the camera trace, the menu camera lock. |
| TOOLS | Texture dump and reload for texture mods, buttons for F4 and the telemetry card, the leftover TestMovie button, squash and stretch, HUD ALPHA (the game's own UI faded, see below), FIXED STEP and CAPTURE FPS (capture mode, see below). |
| LOGGING | One switch per diagnostic that adds lines to the log and can be flipped while the game runs (UI colour, map mouse, wheel, music bank, warp, online and others). |
| INFO | SHOW CHANGED SETTINGS, COPY LAUNCH ARGUMENTS, RESET DEBUG SETTINGS. |

## Diagnostics that write to the log

These only read: they change nothing in the game, they cost nothing while off, and each one is a line or two a tester can send. Switch one on from its row or with a launch argument; the log needs `log_level` info (the default). Why these and not the others of the old local test patches: `feature-parity/T4-DIAGNOSTICS.md`.

| Setting | Where | What it writes |
|---|---|---|
| `mcla_gpu_resolve_memory_check` | launch argument (restart) | Slow: every resolve encode also goes into a second, reference pool at once, and what the game reads is compared with it. `[gpu] resolve memory check: ... readers with a different page ...` every 600 frames and at the end `RESULT PASS` or `RESULT FAIL` (an error line for each difference, the first 16). For wrong or flickering textures that the delayed encode (`mcla_gpu_resolve_memory_encode_delay`, 2 by default) could cause. With the delay at 0 it must pass too (that checks the check). |
| `mcla_gpu_lock_ahead_verify` | launch argument (restart) | A lock whose pages an earlier lock read ahead reads them from the pool as before and compares: `[gpu] lock read-ahead check: ... N pages kept ahead, D different` (D must be 0). The vinyl editor is where it counts. |
| `mcla_gpu_readable_cache_check` | launch argument (restart) | Asks the guest heap again at every range check the per-frame page cache answers. `[gpu] readable cache: ... N cached answers differ from the heap (must be 0)` at the end, and a warning for each of the first 16. |
| `mcla_gpu_shader_memo_check` | launch argument (restart) | No draw takes its shaders from the frame's identity memo: every draw reads the shader objects and compares the memo with the full lookup. `[gpu] shader memo: ... N memo answers differ from the full lookup (must be 0)` at the end, and a warning for each of the first 16 (also for a shader object that changed its microcode within a frame). |
| `mcla_gpu_present_release_check` | launch argument (restart) | Releases the guest at every guest present, also when the GPU is behind, and counts each read or write of guest memory the render thread makes after that: `[gpu] present release: ... N reads or writes of guest memory after the release (must be 0)` at the end. |
| `mcla_gpu_vram_log` | RENDERING, VRAM LOG EVERY (live; frames, 0 off) | `[gpu] video memory (frame N): X MiB in use, budget Y MiB (Z% used) | held by the caches: textures ..., resolve products ..., EDRAM targets ..., resolve memory ... MiB`, at the first check past the interval (checks are 30 frames apart). The existing `[gpu] video memory: short` lines appear only when memory runs short; this one says how near a healthy run is. For stutter or crashes on a card with little video memory. `mcla_gpu_process_memory_log` does the same for the process's system memory. |
| `lowcpu_probe` | LOGGING, LOW CPU PROBE (live) | Every 10 s, `[LowCpuProbe] audio ...`: the audio callback's gaps over 8, 12 and 20 ms and how late the game's 1 ms clock tick was, and `[LowCpuProbe] cpu ...`: CPU time of the audio worker and the timer-queue thread. For crackling sound and stutter on a PC with few cores. The always-on `[Audio]` lines count only the gaps over 20 ms. |

## Launch arguments for restart-only diagnostics

A debug setting that the game reads only at start cannot be a row: a change would not take effect, and it could not be saved for the next start. These are launch arguments:

```powershell
larecomp.exe --debug_menu --mcla_gpu_pass_timing=true
```

The same text works in the launcher's **More game options**, and as `mcla_gpu_pass_timing = true` in `larecomp.toml`. A switch takes `true` or `false`. Never pass an empty value (`--name=`): the game drops its whole command line. With the debug menu unlocked, F4 shows each of these with its description (use the search box).

**Where the list comes from.** It is a snapshot of the source: every cvar whose `REXCVAR_DEFINE_*` has `.debug_only()` and a restart lifecycle (`kRequiresRestart` or `kInitOnly`), and that no row of a `kDebug*` table names. The scan used the definition parser of `tools/check_settings_exposure.py`, which reads the chained calls the same way. `perf_no_trees` is not here: its lifecycle is live, so it is a row on the PERFORMANCE page. The SDK's `graphics_backend` (`d3d12` or `vulkan`) is also tagged debug-only at start (`kSdkDebugCvars` in `src/mc_engine/debug_menu.cpp`) and is a launch argument too; it is defined in the SDK, not here, so it is not counted.

The list, 117 names in 13 groups (the number in brackets is the group's size):

**The unlock** (1)
`debug_menu`

**Shader packs and the pipeline cache (the launcher runs `build_shader_packs` and `precache_pipelines` itself)** (8)
`build_shader_packs`, `build_shader_packs_force`, `shader_pack_dir`, `shader_pack_threads`, `precache_pipelines`, `precache_pipelines_backend`, `precache_pipelines_force`, `precache_pipelines_threads`

**Which renderer runs (the pause menu's RENDERER row holds these for the next start)** (4)
`mcla_gpu`, `mcla_native_gfx`, `mcla_native_gfx_nocp`, `mcla_native_gfx_continuous`

**Plume renderer: timing and statistics** (7)
`mcla_gpu_pass_timing`, `mcla_gpu_scene_profile`, `mcla_gpu_ab_interleave`, `mcla_gpu_memory_census`, `mcla_gpu_census_ps`, `mcla_gpu_census_targets`, `mcla_gpu_census_scale`

**Plume renderer: finding picture bugs** (17)
`mcla_gpu_edram_aliasing`, `mcla_gpu_coverage_queries`, `mcla_gpu_edram_capture`, `mcla_gpu_flicker_probe`, `mcla_gpu_flicker_probe_dumps`, `mcla_gpu_flicker_probe_shots`, `mcla_gpu_verify_constants`, `mcla_gpu_validate`, `mcla_gpu_watch_base`, `mcla_gpu_watch_size`, `mcla_gpu_watch_capture`, `mcla_gpu_texture_decode_selftest`, `mcla_gpu_resolve_memory_check`, `mcla_gpu_lock_ahead_verify`, `mcla_gpu_readable_cache_check`, `mcla_gpu_shader_memo_check`, `mcla_gpu_present_release_check`

**Plume renderer: graphics-API debugging** (3)
`mcla_gpu_debug_layer`, `mcla_gpu_gpu_based_validation`, `mcla_gpu_object_names`

**Plume renderer: tuning and driver workarounds** (7)
`mcla_gpu_batch_draws`, `mcla_gpu_present_wait`, `mcla_gpu_present_release`, `mcla_gpu_pipeline_cache`, `mcla_gpu_readable_cache`, `mcla_gpu_texture_decode_threads`, `mcla_gpu_skip_shaders`

**Legacy native D3D12 runtime (Windows, experimental)** (11)
`mcla_native_gfx_present`, `mcla_native_gfx_aniso`, `mcla_native_gfx_gen_mips`, `mcla_native_gfx_mrt`, `mcla_native_gfx_msaa`, `mcla_native_gfx_own_device`, `mcla_native_gfx_own_heap_kb`, `mcla_native_gfx_own_swapchain`, `mcla_native_gfx_surface_key`, `mcla_native_gfx_upload_mb`, `mcla_gfx_probe`

**Speed experiments read at start** (9)
`perf_no_race_shadows`, `perf_fast_vehicle_shadows`, `perf_no_impostors`, `perf_no_fullscreen_blur`, `fma_float_fast`, `vsync_fast_poll`, `submit_on_primary_buffer_end`, `lzx_native`, `lzx_native_check_all`

**Game patches read at start** (4)
`disable_rubberbanding`, `showroom_unlimited`, `break_pairwise_collision`, `vehicle_records_relocate`

**Audio and music** (4)
`audio_mixer_priority`, `mcla_audio_output`, `custom_music`, `music_list`

**Probes and paths** (4)
`guest_profile`, `input_probe`, `dbg_print`, `string_folder`

**Model mod tuning (all `model_mods_*`; the `model_mods` switch itself has a launcher row)** (38)
`model_mods_anchor_bones`, `model_mods_bone`, `model_mods_cabin_light`, `model_mods_car_budget`, `model_mods_car_pack_write`, `model_mods_car_scale`, `model_mods_car_space_only`, `model_mods_car_textures`, `model_mods_car_verbatim`, `model_mods_car_yaw`, `model_mods_decimate`, `model_mods_diag`, `model_mods_glass_band`, `model_mods_grow`, `model_mods_grow_probe`, `model_mods_grow_slack`, `model_mods_offset_x`, `model_mods_offset_y`, `model_mods_offset_z`, `model_mods_part_growth`, `model_mods_passthrough`, `model_mods_pitch`, `model_mods_proportions`, `model_mods_rim_autoalign`, `model_mods_rim_inherit_shade`, `model_mods_rim_offset_x`, `model_mods_rim_pitch`, `model_mods_rim_roll`, `model_mods_rim_scale`, `model_mods_rim_shade_occlusion`, `model_mods_rim_yaw`, `model_mods_roll`, `model_mods_scale`, `model_mods_share_images`, `model_mods_submeshes`, `model_mods_textures`, `model_mods_weight_smoothing`, `model_mods_yaw`

## The game's own developer options

`debug_options.txt` in the game folder (next to `larecomp.exe`) switches on options from the game's own developer list (`unlockall`, `money`, `timeofday` and the rest of 540, named in `src/mc_engine/debug_options_table.inc`) when the game starts. One option per line: `name`, or `name=value` (a bare name means `1`). `#` and `;` start a comment. An unknown name is logged as `[DbgOpt] unknown option`. It needs no unlock and has no menu row.

## Scenes (the trailer toolkit)

A scene is a text file that says what the world and the settings do on a timeline, so a shot can be recorded again and a setting compared before and after. Nothing runs unless one is armed:

```powershell
larecomp.exe --debug_menu --mcla_scene=C:\scenes\intro.scene
```

`mcla_scene` is a path, or a name looked up next to the game and in its `scenes` folder (`.scene` may be left off). Empty means off. Like every debug setting it is live and never saved: set it in F4 or the console to arm another scene, clear it to stop. `mcla_scene_restart` (change the number) runs the scene again from its start; `mcla_scene_t` (0 or more) holds the clock at that many seconds, for stills. The file is read again when it changes and a reload starts the scene over. A file with an error is not run, and the log says which line (`[scene] intro.scene:7: error: unknown weather 'sunny'`).

The clock starts at 0 once a car is on the road. It runs on the game's frame time from before the time scale, so a slow-motion key does not slow the scene, and it stands still while the game is paused. At `end` everything goes back to the game's own: the day cycle, the weather, the ambient switches, the time scale and the settings the scene changed.

```
scene intro fps 60 step 1/60
world hour 16.5 hold weather nice traffic off peds off police off     # a bundle at time 0
at 2 hour 16.5..19.5 over 14 ease inout                               # a ramp: a..b over seconds
at 6 weather cloudy
at 10 timescale 1..0.2 over 1 ease inout                              # slow motion
at 12 set mcla_gpu_ao off                                             # a setting; the old value comes back
at 14 label "AO OFF" side left fade 0.3 for 3                         # data for a compositor, never drawn in the game
end 20
```

One statement a line, `#` starts a comment, times are seconds or `m:ss`. Eases are `linear` (default), `in`, `out`, `inout`. A key's value holds until the next key of the same track.

| Statement | What the game does |
|---|---|
| `hour <h>`, `hour <a>..<b> over <s>`, `hour free` | `TimeOfDay_RequestHour`: the hour is pinned (a value over 24 wraps); `free` gives the day cycle back |
| `weather nice\|cloudy\|stormy\|foggy\|game` | `Weather_RequestIndex`; `game` gives the weather back |
| `traffic`, `peds`, `parked`, `police` `on\|off` | the game's own script natives (`AmbientMgr_Set*Enable`, `PoliceManager_SetAllPoliceActive`); the enable byte is read back and the log says what it became |
| `timescale <x>`, `timescale <a>..<b> over <s>` | the engine timer's time scale (below about 0.006 at 60 FPS the game holds its own lowest step) |
| `set <setting> <value>` | the setting, live ones only; the old value comes back at the end. Not every setting: see "What a scene cannot set" |
| `label "<text>" [side s] [fade s] [for s]`, `mark <name>` | logged (labels are data for a compositor) |
| `capture fps <n> [step 1/<n>]`, `capture off` | a take (capture mode, below): the game steps by exactly 1/n a second and every frame is read back; the take ends at `capture off` or the scene's `end` |
| `cam`, `cut` | the camera (below) |
| `car <id> spawn\|delete\|profile ...` | cars in free roam and the player's garage car ("Cars in a scene", below) |
| `hud off\|on\|<alpha>`, `hud fade <a>..<b> to <x>`, `hud fade over <s> to <x>` | the game's own UI shown at an alpha ("The game's own UI on and off", below); `<x>` is `off`, `on` or an alpha |
| `start` | kept for the preamble package of the toolkit; read and linted, not applied yet |

The log lines start with `[scene]`: `armed`, `loaded` (with the counts), `start`, one line for every key as its time comes, a `state` line every 2 s that puts the scene's hour, weather and time scale next to what the game reports, `finished`. Natives are called one or two an update, on the game thread.

**What a scene cannot set.** A scene file is data that gets sent around, so `set` is never a way to start a program or write a file. The parser refuses the line (the scene does not load, and the log and the lint say `a scene does not set mcla_capture_ffmpeg: it names a program, a path, a server or arguments, or makes the game write a file of its own (put it on the command line)`) for:

- every `mcla_capture_*` setting (the take's program, its arguments, sink and folder; a scene starts a take with its own `capture` key, and where it goes is chosen on the command line) and `mcla_scene*`;
- a setting that names a program, a path, a folder, a server or arguments: a name ending in `_path`, `_dir`, `_folder`, `_file`, `_root`, `_exe`, `_args`, `_command`, `_csv`, and by name `cl`, `user_name`, `import_vinyl`, `rmr_server`, `online_master_server`, `server_browser_servers`;
- a setting that makes the game write a file of its own in the game's folder: a name with `dump` in it or ending in `_probe`, `_diag`, `_trace`, `_census`, `_telemetry`, and by name `export_vinyl`, `capture_vinyl_shapes`, `lzx_stats`, `menu_cam_record`, `wheel_learn`, `mcla_gpu_trace_frame`, `mcla_native_gfx_capture` and the like;
- `mcla_native_gfx_ab_cvar`, which flips another setting by name and would go round this list.

The rule is `CvarRefusal` in `scene.cpp` (lists `kRefusedCvars` and `kRefusedSuffixes`) and `cvar_refusal` in `scene_check.py`; both are checked against the same names. A new setting of that kind that does not follow a pattern goes into the list. All of them stay settable by hand: a launch argument, the launcher's **More game options**, a line in `larecomp.toml`, F4 or the console.

**A scene's settings are not saved.** A scene takes a setting through the same call a menu does, but remembers the value it had and where that value came from. Saving the settings in the middle of a scene (the pause menu, F4) leaves the scene's value out of `larecomp.toml` (a value the player had changed before the scene is written as the player left it), and the end of the scene puts back exactly the old value and its origin, so a value that came from the command line is still a command-line value and a later save does not write it. `cmake --build <dir> --target larecomp_config_save_test` builds the check of that rule.

The format is written out at the top of `src/mc_engine/cinematic/scene.h`. `cmake --build <dir> --target larecomp_scene_test` builds the checks of the parser, the timeline and the reload rule; they need no game. `larecomp_scene_test a.scene` parses files and says what the game would say. The test harness has a lint, `scene_check.py`, that also checks key order, durations, setting names and values, and that a car is spawned before it is used.

### The camera track

The `cam` and `cut` statements move the camera. Nothing changes unless a scene with a camera track runs: without one the camera hook costs a flag test and the renderer's cut code one atomic load a frame. The camera takes over at the first `cam` key and keeps going until `cam <t> off` or the scene's end. It replaces the game's own camera in every camera mode, at the one place they all pass (`0x822C0868`), so the shadows and the level of detail follow it as they follow the game's (where the sound's listener is, the car or the camera, is not known yet). The world follows it too: a camera 600 m from the car loads the city around itself, so the car does not move and nothing locks the streaming. While the game is paused, or in a menu or a loading screen, the game's own camera is left alone.

```
cam 0 pos 800 5 -800 yaw 80 pitch -4 roll 0 fov 40       # a point, an aim by angles
cam 4 look car:player 0 1.2 0 pos 830 3 -790 fov 35 ease inout   # a look at the car, one metre twenty above it
cam 6 attach car:player 0 2 -6 heading follow 0.15 fov 45  # six metres behind and two up, lagging the car by 0.15 s
cut 8                                                      # a cut
cam 8 orbit car:player r 7 az 0 el 12 fov 30               # on a sphere around the car
cam 10 orbit car:player r 9 az 180 el 20                   # azimuth, elevation and radius blend: an arc round the car
cam 12 off                                                 # the game's camera again
```

| Word | Meaning |
|---|---|
| `pos x y z` | the camera's place, in the game's metres (Y up) |
| `attach car:<id> [dx dy dz]` | at that offset in the car's frame: x right, y up, z forward (`0 2 -6` is behind and above). `heading` uses only the car's heading, so the camera does not pitch or roll with the car. `follow <s>` lags the car's place and heading by about that many seconds |
| `orbit car:<id> r <m> az <deg> el <deg> [at dx dy dz]` | on a sphere around the car (the point `at`, in its heading frame): az 0 is behind, 90 on the right, 180 in front; it looks at that point unless the key says how to aim |
| `yaw <deg> pitch <deg>` | the aim by angles: yaw 0 looks along +Z, 90 along +X; pitch + looks up |
| `look car:<id> [dx dy dz]`, `look x y z` | the aim at a car (a point at an offset in its heading frame) or at a world point. `aimfollow <s>` lags the aim behind a moving target |
| `roll <deg>` | + tilts the right side up |
| `fov <deg>`, `near <m>`, `far <m>` | the vertical field of view (a zoom is even: it is interpolated in focal length), near and far. A channel no key names stays the game's own |
| `ease linear\|in\|out\|inout` | how the segment that STARTS at this key runs |
| `cam <t> off` | the game's camera from `t` |
| `cut <t>` | a cut: no key before it blends with a key after it, and the frame at `t` starts a new shot (below) |

Each channel (position, aim, roll, fov, near, far) has its own keys and runs from key to key. The first key names a position and an aim. A position runs along a smooth curve through the keys (a Catmull-Rom spline; an attach or orbit key is a point that moves with its car, and two orbit keys of one car blend their angles); the aim is a slerp between what each key aims at; a value holds after its last key. After a cut a channel the new shot's first keys do not name carries on from where it was. `car:player` is the player's own car; the other ids are for the car package. A car that does not exist (yet) is held at its last place, or the origin, and the log says so once.

**Cuts.** The take-over, a `cut`, a restart of the scene, and the hand-back (`cam off`, the end) are announced to the renderer with the camera of that frame (`[cam] cut #n ...`). The first frame whose motion blur draw has that camera is the cut frame (`[gpu] cut: ...`; a request that nothing met in 240 frames is dropped with a warning). On that frame the temporal upscalers' history starts again (`[gpu] temporal: history starts again ... cut requested`), so does the occlusion's, and the game's motion blur is told that the previous frame looked the same, so it does not smear the picture (without it the first frame of a shot streaks as if the camera had moved from the old shot). The renderer's own cut rule (25 m, 30 degrees, 5% of the field of view) still applies to every other frame; a jump that is not a `cut` in the scene is only seen by it.

| Setting (debug only, live) | What it does |
|---|---|
| `mcla_cam_log` | log the camera every this many frames (`[cam] t=... at x y z, looking along ...`); 0: only the cuts |
| `mcla_cam_cut_blur_fix` | on by default: the motion blur fix of a cut. Off, for A/B pictures: the game's own constants |

`cmake --build <dir> --target larecomp_camera_test` builds the checks of the maths, the parser and the camera rig, and `larecomp_cut_request_test` those of the cut requests and the blur constants; neither needs the game. `larecomp_camera_test a.scene` reads the camera keys of scene files and says what the game would say (`camera: 9 position keys (5 attach, 2 orbit), 6 aim keys ...`). The test harness's `cut_frames.py` finds the cuts in a lossless burst clip of a run and says whether the frames round each one are smeared.

CHECK (nothing here has run in the game yet): that the racer matrix rows are the car's right, up and BACK (the camera's attach and look would be mirrored if not); that the camera object's c12 to c15 are the rows the hook wrote, with the field of view left out (the match of a cut to its frame); that c69 to c164 are the motion blur's only previous-frame constants; and that the hook runs once an update (a second run in one update gets the same camera and no second cut).

### Cars in a scene (spawn, delete, the player's garage car, poses)

The `car` statements of a scene put cars into free roam and dress the player's own. Nothing runs unless a scene has car keys (one flag test an update); they are `mcla_scene` features, so they are debug-only like the rest.

```
at 0 car p1 spawn vp_lam_murcielago_06 at 812.5,17.6,-799.5 heading 62    # a car by the game's vehicle name
at 0 car p2 spawn profile 1 at 818.0,17.6,-797.0 heading 242               # garage slot 1 of the save, dressed as the owner left it
at 0 car player profile 3                                                  # the player's own car becomes garage slot 3
at 20 car p1 delete
```

`at` takes `x,y,z` (or three numbers): a world position, x east, z south, y up. `heading` is where the nose points, in degrees, as the camera's yaw: 0 along +Z, 90 along +X (the game's own `Racer_Teleport` angle is this plus 180; the log shows both). Garage slots count from 0 (`garage:N` and `slot:N` also mean `profile N`). The id is what the camera's `car:<id>` will name; `player` takes only `profile`. A car is made the way E4 proved in free roam: `OpponentManager_CreateOpponent`, `LoadOpponent` (or `LoadOpponentByCarConfig` with the garage's config), `AddOpponentToWorld`, wait until the load state is 3 and the car is streamed, `Racer_Activate`, `Racer_SetStreamingLockedIn`, `Racer_Teleport`, then `Racer_GetPosition` as a check. That takes about 4 s: a car exists from the log line `car p1 is ready`, so give its spawn key a few seconds before the shot that needs it.

**Safe point.** The scene runner only queues the keys; `TickCars` (one line in `Patch_DeltaTimePre`, after `TickScene`) makes the calls on the game thread, one native an update at most, and only while the player's car is on the road, the ambient and opponent managers exist, the game is not paused and that has held for 10 updates. A racer is looked up in the opponent manager's active list before a call is made on it: if the world was rebuilt under the scene (a race, the front end) the car is dropped from the books with a warning and nothing is called. A name the game's car table does not have, a garage slot that is not there and an empty opponent pool each fail that car with a warning and nothing else; a car that is not streamed in after 20 s is deleted again.

**The end.** Every car the scene spawned is deleted when it ends (also on a restart, a reload and when `mcla_scene` is cleared), after the spawn has run to its end (a half-made opponent is never deleted: E4 only deleted finished ones), and the player's car goes back to the garage slot it wore before the scene changed it. Both take a few seconds after the scene's `finished` line and the log says so (`[cars] ... is deleted`, `[cars] nothing left to do`).

**Poses (for the camera).** `src/mc_engine/cinematic/cars.h`: `GetCarPose("p1", &pose)` and `CarMatrixById("p1", matrix)` give the position, the axes and the heading of the player (`"player"`) and of every scene car, read from the car entity's world matrix (`racer+48 -> entity, +28 -> matrix, +16`: right, up, back, position) at the moment of the call, with the last good pose as the fallback; a registry (`car_registry.h`) holds each car's last pose and the update that read it. `racer+0x470`, the race cars' own position copy, holds a fill value outside a race and is not used. `CarMatrixById` has the shape of the camera package's `CarResolver` and is installed as its car source when that package is in the build. The log writes a `[cars] pose <id> (x, y, z) heading h` line every 2 s for each car that has one, and `[cars] <id> placed:` once after the placement with the matrix, `Racer_GetPosition` and what the scene asked for side by side.

The log lines start with `[cars]`: the scene's keys as they come due, every native with its arguments and result (so the last line before a crash names the call), `ready`, `pose`, `placed`, `is deleted`. `cmake --build <dir> --target larecomp_cars_test` builds the checks of the statements, the pose maths, the registry and the whole spawn / delete / garage script against a pretend game (222 checks); they need no game.

## The game's own UI on and off (the trailer toolkit)

For a take without a HUD. `mcla_cin_hud_alpha` (0 to 1, default 1) is how much of the game's own UI the picture shows: the HUD, the minimap and the full map, the phone and its pop-ups, button prompts, gamertags and the 3D world icons. 1 is the game's frame and costs nothing; 0 is the 3D picture alone; in between is a fade. It is a debug-only setting like the others here (live, never saved), with a row on the TOOLS page, HUD ALPHA (the Plume renderer only; 1, 0.75, 0.5, 0.25, 0: a value a scene left shows as the nearest of them). The row writes the cvar, and the picture follows on the next frame.

```powershell
larecomp.exe --debug_menu --mcla_cin_hud_alpha=0
```

How it works: the renderer keeps the game's UI as a layer of its own (the one HDR output and frame generation use) and the present shows the 3D picture plus that layer times the alpha. Below 1 the layers run (about 0.4 ms at 3x on a desktop GPU: two copies of the UI target and a clear and a put-back), the present composes them (also in SDR), and frame generation's composed frame and DLSS frame generation's UI hint use the same alpha. At 1 nothing asks for the layers: no draw changes, nothing is copied, and a frame boundary costs a cvar read.

What is never faded: the pause menu, this DEBUG menu, the race pause and the full map's pause view (a frame whose UI target is the pause view's shows its UI whole), and the SDK's overlays (the F3 card, the F4 panel, toasts), which are drawn after the picture. The effect therefore shows once the menu is closed. What it cannot fade: 3D objects (race checkpoints, the race line, the GPS ribbon are scene, not UI), and frames whose UI the renderer cannot keep apart from the picture (loading, movies), which show it whole. The title screen's PRESS prompt is UI over a 3D picture too and fades with the rest (a tour that waits for it needs the alpha at 1 until it has pressed it).

A scene sets it through `mc::cinematic::SetHudAlpha(alpha, fade_seconds)` (`src/mc_engine/cinematic/hud.h`), which writes the cvar, so the row shows it and the last writer wins. `fade_seconds` is on the renderer's wall clock; a runner that steps its own clock (a capture) calls it with 0 on every update. The scene's keys are `at T hud off`, `hud on`, `hud <alpha>`, `hud fade <a>..<b> to <x>` (from scene time a to b, in a straight line, from the alpha it has at a) and `hud fade over <s> to <x>` (`src/mc_engine/cinematic/hud_track.h`); the scene runner applies the track (`hud_runtime.cpp`: it parses the keys when the scene starts, an error names the line and leaves the HUD alone, and calls `SetHudAlpha(alpha, 0)` on every update; `ReleaseHud()` at the end puts the alpha back to what it was before the scene).

Checks without a game: `larecomp_ui_fade_test` (the clamp and the ramp) and `larecomp_hud_track_test` (the key parser and the track), CMake targets that are not built with the rest.

## Capture mode (a take)

For a recording the game has to advance by exactly one frame's time per picture, whatever the picture costs to make, and no picture may be missed. Capture mode does that, and nothing else changes while it is off.

```
scene take fps 60
at 0 capture fps 60          # from here: a take at 60 frames a second
at 0 hour 17.5 hold
end 10                       # the take ends with the scene (or at `capture off`)
```

- **Fixed step.** Every update gives the engine timer exactly `1/n` s in the game's own ticks (60 fps is 831250 ticks), whatever the frame took. The FPS limit, the step cap and frame pacing stand aside. The step is the capture rate (`step 1/<n>` may be written, and must say the same); slow motion is a `timescale` key with a higher capture rate, not another step. The path of a car depends on the step, so a take is tied to its rate.
- **Frame gate.** The game does not start update `k + 1` until frame `k` has been read back (the 1:1 present output, before the overlays, 8-bit SDR, so the F3 card and menus are not in it; the output must not be HDR and frame generation must be off). The frame of an update is told at the hand-over of the frame to the render thread, where that thread is idle, so a take always holds exactly the pictures of its updates: the first is the take's first update and the last is the update before the take's end gate. The log says `[cap] frame count: N frames for N updates (a..b): exact`, or `[cap] FRAME COUNT: ...` with what is off (and `[cap] TAG:` / `frame of update N lost:` for the cause). A wait that runs out is a `[cap] gate timeout`; three in a row switch the waiting off for the take.
- **Scene clock.** During a take the upscaler's history rule and the UI movies' pacing read frames times the step instead of the wall clock, so a slow capture is not a stall to them (`[cap] summary` shows `upscaler: ... history_restarts=N (gap_over_100ms=K)`: K is every frame without the clock). `mcla_capture_virtual_clock` turns it off.
- **The first frame.** The take starts at the first update after the `capture` key; the scene time of its first frame (the key's update plus one step) is in the `[cap] take starts` line and `<prefix>.info.txt` (`first_scene_t`).

Settings (all debug-only and live; the sink and the files are read when a take starts). A scene cannot set any of them ("What a scene cannot set" above): they come from the command line, F4 or the console.

| Setting | Does |
|---|---|
| `mcla_capture_fps` (row CAPTURE FPS, Plume only), `mcla_fixed_step_fps` (row FIXED STEP) | a take, or the fixed step alone with no gate (for repeatability tests), at this rate; 0 is off. A scene's key wins over the take setting. |
| `mcla_capture_sink` | `none` (default: the readback and its copies are measured and dropped), `ffmpeg` (a pipe, FFV1 in mkv) or `raw` (B8G8R8A8 frames) |
| `mcla_capture_out` | the file prefix: `<prefix>.mkv` / `.bgra`, `.cap.csv` (one row of costs per frame), `.info.txt`, `.ffmpeg.log`. A relative prefix is relative to the game folder; a take with the same prefix overwrites the files. |
| `mcla_capture_ffmpeg`, `mcla_capture_ffmpeg_args` | the program and its output options (default `-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0`) |
| `mcla_capture_ring`, `mcla_capture_lag` | host buffers between the readback and the writer (2 to 8); how many frames the game may run ahead of the readback (0: lock-step) |
| `mcla_capture_gate_timeout_ms`, `mcla_capture_delay_ms`, `mcla_capture_log_every` | the gate's wait limit; an artificial wait per frame to try a slow capture; a `[cap] f=` line every n frames |

```powershell
larecomp.exe --debug_menu --mcla_scene=take --mcla_capture_sink=ffmpeg --mcla_capture_out=C:\takes\a --mcla_gpu_hdr=off
```

Memory: a take holds the ring (4 host buffers) and six GPU readback buffers of one frame each, 31.6 MiB a frame at 4K; an 8K take is eight times that per buffer, and ffmpeg's FFV1 encoder runs next to the game. Not tried at 8K. `cmake --build <dir> --target larecomp_capture_test` builds the checks of the step maths, the frame books and the clock (they need no game); `larecomp_scene_test` covers the `capture` statement.

## Adding a debug setting

A new cvar is one of three things, and the check below fails until it is:

1. a row a player can reach (pause menu, or the launcher), with no `.debug_only()`;
2. on the exception list in `tools/settings_exposure.toml` (visible in a locked F4; it needs the owner's say, and the same name in `kLockedPanelCvars` in `src/mc_engine/ui/settings.cpp`);
3. `.debug_only()`, which is the answer for a diagnostic, a probe or a tuning value:

```cpp
REXCVAR_DEFINE_BOOL(my_probe, false, "MCLA/Debug",
    "What it does, in plain words.")
    .debug_only();
```

Add `.lifecycle(rex::cvar::Lifecycle::kRequiresRestart)` if it is read only at start. It then has no row and is a launch argument.

**A row from the feature's own file.** A live debug-only cvar can have a row. Rows live in tables named `kDebug...`:

```cpp
const mcla_menu::ItemDef kDebugMyFeatureItems[] = {
    mcla_menu::Bool("PM_RxDbgMyProbe", "my_probe", "MY PROBE: "),
};

const mcla_menu::ItemDef* MyFeatureDebugRows(int& count) {
    count = int(sizeof(kDebugMyFeatureItems) / sizeof(kDebugMyFeatureItems[0]));
    return kDebugMyFeatureItems;
}
```

- One or two rows: put them in the closest table in `src/mc_engine/debug_menu_pages.cpp` (`kDebugFixesItems`, `kDebugPerformanceItems`, and so on).
- Several rows: keep the table in the feature's file, as `CameraLookMenuItems` does for the free-look rows, and add a page. That is a `MenuDef` in `kMenus` with your function as `extra` and `debug_only` (its last field) true (`src/mc_engine/pause_menu.cpp`, next to the `PM_RxDbgTab...` entries), a `Submenu` row for it in `kDebugHubItems`, and the file's name under `debug_files` in `tools/settings_exposure.toml`, so the check reads it.
- Keys start with `PM_RxDbg` and are unique. Labels use only A-Z, 0-9, space, `+`, `.` and parentheses (no `:` or `/`). Use `NativeOnly(...)` for a row of the Plume renderer only. An action row needs an `ActionId` in `menu_items.h` and a case in `DebugActionLabel` and `DebugActionRun`.
- **RowDynamics.** When a row's label, suffix or enabled state depends on other settings, give it a `RowDynamics` (declared in `menu_items.h`, each member optional): `WithDynamics(Bool(...), &kMyDynamics)` with `constexpr RowDynamics kMyDynamics = {.enabled = MyFeatureOn, .suffix = MyFeatureWhyOff};` (your own functions). A row whose cvar is chosen at run time (`RowDynamics::cvar`) is seen by the check only if the function is named like `MyFeatureCvar` and has the cvar names as string literals on its first line, as `VsyncCvar` does in `pause_menu.cpp`.

**Test-only cvars.** A cvar made for one experiment or one branch is named `mcla_test_<what>` and is `.debug_only()`. It needs no row and no other classification. Delete it with the experiment. The prefix is a naming convention; the check only looks at the tag.

## The rule, and how it is enforced

Every cvar is a row, an exception or `.debug_only()`. Debug rows must be live. `tools/check_settings_exposure.py` (Python 3.11 or newer; it reads the source and builds nothing) exits 1 when:

1. a cvar has no `.debug_only()`, no row and no place on the exception list;
2. a `.debug_only()` cvar has a row in a normal (non-debug) table, or is on the exception list;
3. a row of a debug table names a cvar that is not `.debug_only()`, or whose lifecycle is restart-only;
4. a cvar is defined twice and only one definition has the tag;
5. the exception list and `kLockedPanelCvars` (what a locked F4 shows) differ.

```powershell
python tools\check_settings_exposure.py        # every cvar
python tools\check_settings_exposure.py --dirs gpu mc_engine/hooks   # only these folders
```

Run it before you push. A short `pending` list in `tools/settings_exposure.toml` names debug-only cvars whose old row is still in a normal table; it shrinks as those rows move.
