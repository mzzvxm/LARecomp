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
| TOOLS | Texture dump and reload for texture mods, buttons for F4 and the telemetry card, the leftover TestMovie button, squash and stretch. |
| LOGGING | One switch per diagnostic that adds lines to the log and can be flipped while the game runs (UI colour, map mouse, wheel, music bank, warp, online and others). |
| INFO | SHOW CHANGED SETTINGS, COPY LAUNCH ARGUMENTS, RESET DEBUG SETTINGS. |

## Launch arguments for restart-only diagnostics

A debug setting that the game reads only at start cannot be a row: a change would not take effect, and it could not be saved for the next start. These are launch arguments:

```powershell
larecomp.exe --debug_menu --mcla_gpu_pass_timing=true
```

The same text works in the launcher's **More game options**, and as `mcla_gpu_pass_timing = true` in `larecomp.toml`. A switch takes `true` or `false`. Never pass an empty value (`--name=`): the game drops its whole command line. With the debug menu unlocked, F4 shows each of these with its description (use the search box).

**Where the list comes from.** It is a snapshot of the source: every cvar whose `REXCVAR_DEFINE_*` has `.debug_only()` and a restart lifecycle (`kRequiresRestart` or `kInitOnly`), and that no row of a `kDebug*` table names. The scan used the definition parser of `tools/check_settings_exposure.py`, which reads the chained calls the same way. `perf_no_trees` is not here: its lifecycle is live, so it is a row on the PERFORMANCE page. The SDK's `graphics_backend` (`d3d12` or `vulkan`) is also tagged debug-only at start (`kSdkDebugCvars` in `src/mc_engine/debug_menu.cpp`) and is a launch argument too; it is defined in the SDK, not here, so it is not counted.

The list, 112 names in 13 groups (the number in brackets is the group's size):

**The unlock** (1)
`debug_menu`

**Shader packs and the pipeline cache (the launcher runs `build_shader_packs` and `precache_pipelines` itself)** (8)
`build_shader_packs`, `build_shader_packs_force`, `shader_pack_dir`, `shader_pack_threads`, `precache_pipelines`, `precache_pipelines_backend`, `precache_pipelines_force`, `precache_pipelines_threads`

**Which renderer runs (the pause menu's RENDERER row holds these for the next start)** (4)
`mcla_gpu`, `mcla_native_gfx`, `mcla_native_gfx_nocp`, `mcla_native_gfx_continuous`

**Plume renderer: timing and statistics** (7)
`mcla_gpu_pass_timing`, `mcla_gpu_scene_profile`, `mcla_gpu_ab_interleave`, `mcla_gpu_memory_census`, `mcla_gpu_census_ps`, `mcla_gpu_census_targets`, `mcla_gpu_census_scale`

**Plume renderer: finding picture bugs** (12)
`mcla_gpu_edram_aliasing`, `mcla_gpu_coverage_queries`, `mcla_gpu_edram_capture`, `mcla_gpu_flicker_probe`, `mcla_gpu_flicker_probe_dumps`, `mcla_gpu_flicker_probe_shots`, `mcla_gpu_verify_constants`, `mcla_gpu_validate`, `mcla_gpu_watch_base`, `mcla_gpu_watch_size`, `mcla_gpu_watch_capture`, `mcla_gpu_texture_decode_selftest`

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
| `set <setting> <value>` | the setting, live ones only; the old value comes back at the end |
| `label "<text>" [side s] [fade s] [for s]`, `mark <name>` | logged (labels are data for a compositor) |
| `cam`, `cut`, `hud`, `car`, `start` | kept for the camera, HUD and car packages of the toolkit; read and linted, not applied yet |

The log lines start with `[scene]`: `armed`, `loaded` (with the counts), `start`, one line for every key as its time comes, a `state` line every 2 s that puts the scene's hour, weather and time scale next to what the game reports, `finished`. Natives are called one or two an update, on the game thread.

The format is written out at the top of `src/mc_engine/cinematic/scene.h`. `cmake --build <dir> --target larecomp_scene_test` builds the checks of the parser, the timeline and the reload rule; they need no game. `larecomp_scene_test a.scene` parses files and says what the game would say. The test harness has a lint, `scene_check.py`, that also checks key order, durations, setting names and values, and that a car is spawned before it is used.

## The state log and take_diff (the trailer toolkit)

A take is only as good as the proof that the next one matches. The state log writes one CSV row per game update: the player's car (the entity's world matrix and the speed), the scene's cars, the camera block the game publishes, the hour and weather, the engine's clocks, the scene's own clock and the state of the random number generator. `take_diff.py` reads two of them and says *identical*, *identical within tolerance*, or the first row, column and values where they part. Nothing is read or written while no log is open.

Two ways to ask for it, both debug-only and live:

```powershell
larecomp.exe --debug_menu --mcla_scene=statelog-park --mcla_state_log=C:\takes\a.csv
```

- `mcla_state_log`: a `.csv` file, or a folder that gets `state.csv`. Set it and rows start with the next update that has a player car; clear it and the log is closed. A named file is overwritten; a folder that already holds `state.csv` gets `state-2.csv`, `state-3.csv`.
- A scene's `statelog on [dir <folder>]` (no time: it is an option of the whole scene; `statelog off` is the default). The log opens when the scene starts and closes when it ends, and a scene that starts again (`mcla_scene_restart`, a reload) is a new file. It goes to `<folder>\state.csv`, else the take folder (`statelog::SetTakeFolder`, for the take package), else `statelogs\<scene>-<date>-<time>.csv` next to the game. `mcla_state_log` outranks it.

Rows are only written while the player has a car (no menus, no loading screens); `tick` counts every update since the log opened, so a gap shows. A row is a few guest reads and one formatted line on the game thread; a writer thread takes them through a queue capped at 32 MiB. A stalled disk costs rows, never a wait: the next row's `lost` column says how many, and the log's last line, `#end rows=N written=N dropped=N`, closes a log that ended properly (a log open when the game exits has none). The columns, in order, are in the header of `src/mc_engine/cinematic/state_log_core.h`: `tick mark wall_ms lost`, `scene_state scene_t`, `dt dt_unscaled time_scale frame_ctr clock_total clock_game mode_timer`, `rng hour weather paused live`, `p_ok p_m0..p_m14 p_speed` (the matrix rows right, up, back, position; the fourth word of each row is padding and is left out), `cam_ok cam_m0..cam_m14`, `n_cars` and six slots `c<k>_id c<k>_ok c<k>_m0..c<k>_m14` for the scene's cars, and the hashes `h_player h_cars h_world h_cam hash chain` (exact bits; the generator, the clocks and the wall time are not in `hash`). Floats have 9 digits and read back exactly. The cars come from a car source (`statelog::RegisterCarSource`, which the cars package registers with its registry); with none registered `n_cars` is 0.

```powershell
python -I ports\scripts\take_diff.py A.csv B.csv                       # the scene's own start is the alignment; the scene's rows are compared
python -I ports\scripts\take_diff.py A.csv B.csv --align mark=go       # a scene's `mark go`
python -I ports\scripts\take_diff.py A.csv B.csv --tol pos=0.002,col:^cam_m12$=0.01
python -I ports\scripts\take_diff.py A.csv B.csv --json                # one JSON line for a script
python -I ports\scripts\take_diff.py --growth A.csv B.csv --at 1,5,10  # how far apart the player and the scene cars are over time
python -I ports\scripts\take_diff.py --selftest [--exe state_log_test.exe]
```

The exit code is 0 identical (either kind), 1 diverges, 2 bad input. Each column has a tolerance by class, absolute and in the log's units: `pos` 1 mm (a car's position), `rot` 1e-5, `speed` 1 mm/s, `campos` 2 mm and `camrot` 1e-4 (the published camera), `hour` 1e-4; the weather, pause, scene state, which cars exist and their ids must be equal. `--tol CLASS=VALUE` or `--tol col:REGEX=VALUE` change them (0 means equal); `--strict` makes every column, the hashes and the generator bit for bit. The clocks (`dt`, `scene_t`, and the engine clocks as time elapsed since the first compared row) are compared only when both logs were taken under a fixed step (every `dt` the same, so the rows are at the same game times) or with `--clocks`; in real time only a still world compares row for row, and the clocks are listed outside the verdict. The generator (`rng`) is seeded from the clock and is outside the verdict unless `--rng`. The report names the first row (with the scene time), every column over its tolerance in it with both values, the difference and the tolerance, a table of the columns that go over (first row, largest difference, rows over), a one-line reading, and notes for dropped rows and a log the game did not close. The older bit-for-bit comparison of the experiment's logs (`--exact`, `--shift`, `--curve`) still works on both formats.

What to expect (E1 and E1b): a parked or placed car stays within 8 micrometres to a millimetre, so a camera-only scene, a held world or a wipe on a parked car matches; a driven take does not repeat, and the report then names the first row and the car. `ports\scripts\scenes\statelog-park.scene` is the example, `perf\scripts\chain-m3a.ps1` the check. `cmake --build <dir> --target larecomp_state_log_test` builds the checks of the row, the hashes, the file's place, the queue and the writer (`larecomp_state_log_test --sample out.csv` writes a made-up log through the real writer, for `take_diff.py`); they need no game.

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
