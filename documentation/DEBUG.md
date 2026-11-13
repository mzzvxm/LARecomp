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
