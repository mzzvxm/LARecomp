# Settings Reference

Every cvar registered by code in this repository, plus the environment variables the build reads and the SDK cvars the code refers to. A cvar is one setting; it can be set in the pause menu (when a row exists), on the command line as `--name=value`, or in `larecomp.toml`. How and when the file is rewritten: [Getting Started](Getting-Started#when-the-game-rewrites-larecomptoml).

Counts, from the registration macros in `src/`:

| Page | Cvars |
|---|---|
| Graphics and performance | 46 |
| Audio | 9 |
| Input (controller, keyboard, wheel) | 81 |
| Camera and HUD | 73 |
| Gameplay and world | 55 |
| Online | 20 |
| Mods | 45 |
| Startup, saves and text | 10 |
| Debug and diagnostics | 12 |
| This page, total | 351 |
| [Settings Renderer Advanced](Settings-Renderer-Advanced) | 170 |
| All | 521 |

The README states "155 settings". The registration macros found in the source number 521; the README figure is not reproduced here.

## How to read the tables

| Column | Meaning |
|---|---|
| Name | The cvar name, as written in `larecomp.toml` and after `--`. |
| Type | `bool`, `string`, `double`, `int32`, `uint32`, `uint64`, or `command`. |
| Default | The default in the registration macro. `""` is an empty string. |
| Range / values | The `range` or `allowed` list attached to the cvar. `-` means none is declared. A range is not enforced by this wiki; it is what the registration declares. |
| Where | The pause menu row (path, label, and the values the row steps through) when one exists. `toml / CLI` means no menu row. |
| Effect | The description in the registration, shortened. "Needs a restart" marks cvars registered as restart-only. Cvars registered as hot-reloadable, or with no lifecycle declared, carry no note; the source does not say more about the latter. |
| Source | The file holding the registration. |

Notes:

- Menu rows named for a `disable_*` cvar are inverted: the row shows ON when the value is false.
- Some rows exist for one renderer only (`XeniaOnly` and `NativeOnly` in `src/mc_engine/menu_items.h`); the menu lists the rows of the renderer running at launch.
- Row labels are copied as they appear in `src/mc_engine/pause_menu.cpp`, except that the rubberband row is written without its first word.
- Steps listed for a numeric row are the values the menu cycles through. A value outside the steps can still be set in `larecomp.toml`; the row then shows that value as an entry of its own (`src/mc_engine/menu_items.h`).
- Descriptions that were empty or not in English in the source are restated from the code.

## Pause menu layout

Opened with Start or Escape, then Options, then the Settings tab. The tree below is built from the menu tables in `src/mc_engine/pause_menu.cpp` and `src/mc_engine/camera_look.cpp`.

- RECOMP SETTINGS
  - RUBBERBAND (`disable_rubberbanding`)
  - DISPLAY GAMMA (`display_gamma`)
  - REAL FRAME DELTA (`real_frame_delta`)
  - HIDE MOUSE POINTER (`hide_mouse_pointer`)
  - ORIGINAL INTRO SPEED (`intro_original_speed`)
  - DUALSENSE TRIGGERS (`adaptive_triggers`)
  - SUSPENSION FIX (`smooth_chassis_depth`)
  - VIBRATION (`controller_vibration`)
  - LEFT STICK DEAD ZONE (`left_stick_deadzone`)
  - RIGHT STICK DEAD ZONE (`right_stick_deadzone`)
- GRAPHICS
  - DISPLAY
    - FULLSCREEN (`fullscreen`)
    - VSYNC (`vsync`)
    - VSYNC (`mcla_gpu_vsync`)
    - RESOLUTION (`resolution`)
    - RES SCALE (`resolution_scale`)
    - RENDER RES (`mcla_gpu_resolution_scale`)
    - MSAA (`disable_msaa`)
    - MSAA (`mcla_gpu_msaa`)
    - FPS LIMIT (`fps_limit`)
    - PACE AT PRESENTATION (`pace_at_swap`)
    - SMOOTH UI (`ui_interpolate`)
    - FSR1
      - FSR1 (`mcla_gpu_fsr`)
      - QUALITY (`mcla_gpu_fsr_preset`)
      - SHARP REDUCE (`mcla_gpu_fsr_sharpness`)
  - RENDERER
    - GRAPHICS API (`graphics_backend`)
    - NATIVE AA (`mcla_gpu_aa`)
    - ANISOTROPIC (`mcla_gpu_anisotropy`)
  - FIDELITY FX
    - UPSCALER (`present_effect`)
    - FSR QUALITY (`present_fsr_quality_mode`)
    - CAS SHARPNESS (`present_cas_additional_sharpness`)
    - FSR SHARP REDUCE (`present_fsr_sharpness_reduction`)
  - PERFORMANCE
    - SHADOWS
      - SHADOWS (`perf_no_shadows`)
      - SHADOW PHASES (`perf_shadow_phase_bits`)
      - CHEAP CAR SHADOW (`perf_fast_vehicle_shadows`)
      - TREE IMPOSTORS (`perf_no_impostors`)
      - FOLIAGE (`perf_no_trees`)
    - RENDERING
      - SCREEN BLUR (`perf_no_fullscreen_blur`)
      - DEPTH OF FIELD (`disable_dof`)
      - SINGLE TILE (`single_tile`)
      - FENCE SPIN YIELD (`fence_spin_throttle`)
    - WORLD DENSITY
      - CITY AMBIENT CULLING (`enable_ambient_tuning`)
      - TRAFFIC RANGE (`traffic_unspawn_dist`)
      - PEDESTRIANS (`ped_density_scale`)
      - PARKED CARS (`parked_car_scale`)
      - PROP DRAW DIST (`global_max_draw_distance`)
      - BREAK FPS FLOOR (`breaking_frame_rate_limit`)
    - LOD AND FOLIAGE
      - FOLIAGE SHADOWS (`disable_imposter_shadows`)
      - TRAFFIC LOD (`lod_traffic_scale`)
      - CITY LOD (`lod_city_scale`)
  - TEXTURE MODS
    - TEXTURE MODS (`texture_mods`)
- GAMEPLAY
  - CAMERA
    - MOTION BLUR (`disable_motion_blur`)
    - CHASE CAMERA
      - SMOOTH CHASE CAM (`smooth_chase_cam`)
      - CAM SMOOTH FACTOR (`chase_cam_smoothing_factor`)
    - FOV
      - 1P FOV (`fov_1p_scale`)
      - 3P FOV (`fov_3p_scale`)
    - FREECAM
      - FREECAM (`debug_cam`)
      - CAM SPEED (`debug_cam_speed`)
    - REAR VIEW
      - REAR VIEW (`rearview`)
      - REAR VIEW POS (`rearview_position`)
      - REAR VIEW FRAME (`rearview_frame`)
    - LOOK AROUND
      - FREE LOOK (`cam_freelook`)
      - LOOK SENS (`cam_freelook_sens`)
      - INVERT LOOK X (`cam_freelook_invert_x`)
      - INVERT LOOK Y (`cam_freelook_invert_y`)
      - RECENTER DELAY (`cam_freelook_return_delay`)
      - RECENTER SPEED (`cam_freelook_return_rate`)
      - CAMERA COLLISION (`cam_freelook_collide`)
      - COLLISION FLIP (`cam_freelook_cs_invert`)
  - TIME OF DAY
    - REAL CLOCK (`time_of_day_realtime`)
    - HOLD TIME (`time_of_day_hold`)
    - TIME (`time_of_day`)
    - DAY SPEED (`time_of_day_speed`)
    - WEATHER (`weather`)
  - CARBON FIBER
- INTERFACE
  - UI
    - MENU BLUE (`ui_theme_accent`)
    - TITLES AND GLOBE (`ui_theme_title`)
    - FRAME GRAY (`ui_theme_frame`)
    - SPEED UNITS (`speed_units`)
    - BUTTON LAYOUT (`button_prompts`)
  - LANGUAGES
    - LANGUAGE (`language`)
- EXTRAS
  - EXTRA VINYL LAYERS (`extra_vinyl_layers`)
  - NO 8 PLAYER LIMITS (`net_lift_big_session_limits`)
  - START GARAGE (`start_garage`)
  - CUTSCENES
    - CUTSCENE (`cutscene_replay`)

Rows without a cvar:

| Menu | Row | Effect | Source |
|---|---|---|---|
| GRAPHICS > RENDERER | RENDERER | Not a cvar. Writes four restart-only cvars together: `mcla_gpu`, `mcla_native_gfx`, `mcla_native_gfx_nocp`, `mcla_native_gfx_continuous`. The row offers XENIA and, in builds with the Plume renderer, NATIVE. Held for the next launch. | `src/mc_engine/pause_menu.cpp` |
| GRAPHICS > TEXTURE MODS | Dump textures, Reload texture mods | Actions. Reload is the same as setting `texture_reload`. See [Texture Modding](Texture-Modding). | `src/mc_engine/pause_menu.cpp` |
| GAMEPLAY > CARBON FIBER | HOOD, TRUNK, DOORS, ROOF, BUMPERS, FENDERS & SKIRTS, SPOILER, GRILL & INTERCOOLER | Flip a bit in the customization data of the car being driven. Not a cvar; each car keeps its own selection in the save. | `src/mc_engine/menu_items.h`, `src/mc_engine/pause_menu.cpp` |
| EXTRAS > CUTSCENES | Play | Action; plays the chosen `cutscene_replay` cutscene. | `src/mc_engine/pause_menu.cpp` |
| QUIT | Yes, No | Close the game. The cursor starts on No. | `src/mc_engine/pause_menu.cpp` |

The README's tab list ("eight tabs") and `documentation/RUNNING.md` ("seven tabs") do not match this tree. The tree is what the code builds.

## Environment variables

Read from the process environment. Sources: `src/larecomp_app.h`, `src/mc_engine/hooks/frame_timing.cpp`, `documentation/RUNNING.md`.

| Variable | Default | Effect | Source |
|---|---|---|---|
| `REX_LOG_LEVEL` | `trace` on non-Release builds | Log level. Use `warn`. Must be an environment variable. | `documentation/RUNNING.md` |
| `LARECOMP_LOG_FILE` | automatic | Log file path. | `src/spdlog_console.cpp` |
| `MCLA_GAME_DATA` | auto-detect | Folder holding `default.xex`. Invalid values are reported and ignored. | `src/larecomp_app.h` |
| `LARECOMP_INSTALL_ISO` | unset | Path of an ISO to install unattended. | `src/isoinstaller/larecomp_iso_installer.cpp` |
| `MCLA_FPS_CAP` | unset | Overrides the `fps_limit` cvar. Read once at startup. | `src/mc_engine/hooks/frame_timing.cpp` |
| `MCLA_MAX_FRAME_MS` | `125` | Largest frame time the clock advances in one frame. Values below 16 become 16, above 1000 become 1000. Zero or negative values keep the default. | `src/mc_engine/hooks/frame_timing.cpp` |
| `MCLA_TIMING_LOG` | off | Frame timing log when set (the README says `1`). | `src/mc_engine/hooks/timing_log.cpp` |
| `MCLA_TEX_SOFT`, `MCLA_TEX_HARD` | derived from adapter VRAM | Texture cache soft and hard limits in MB. Without them, and when `larecomp.toml` does not set the SDK limits, the hard limit is half the VRAM clamped to 768 to 4096, the soft limit three quarters of that clamped to 384 to 2560; 2048 hard when VRAM cannot be read. | `src/larecomp_app.h` |
| `MCLA_TEX_RTT` | `64` | Render-to-texture cache limit in MB, unless `larecomp.toml` sets it. | `src/larecomp_app.h` |
| `MCLA_VSYNC` | `false` | Sets the SDK `vsync` cvar at startup. | `src/larecomp_app.h` |
| `MCLA_REFRESH_RATE` | `60` | Sets the SDK `video_mode_refresh_rate` at startup. | `src/larecomp_app.h` |
| `MCLA_ALLOW_INVALID_FETCH` | `true` | Sets the SDK `gpu_allow_invalid_fetch_constants`. | `src/larecomp_app.h` |
| `MCLA_NO_STUB_SWEEP` | unset | `1` skips the startup stub sweep. | `src/larecomp_app.h` |
| `MCLA_PROFILE` | unset | `1` enables the guest profiler (same as `guest_profile`). | `src/mc_engine/guest_profiler.cpp` |
| `MCLA_PROFILE_SLOW_MS` | built-in | Slow-frame threshold for the guest profiler; accepted when greater than 0.5 and less than 1000. | `src/mc_engine/guest_profiler.cpp` |
| `MCLA_PROFILE_THREAD` | unset | Thread name filter for the guest profiler. | `src/mc_engine/guest_profiler.cpp` |
| `MCLA_RESOLVE_SYMBOLS` | unset | `1` turns on symbol resolution in logs. Also on when `REX_LOG_LEVEL` is `debug`. | `src/mcla_symbol_resolver.h` |
| `MCLA_STRINGS_FILE` | unset | Extra path searched by the symbol resolver. | `src/mcla_symbol_resolver.h` |
| `MCLA_SLEEP_SPIN` | unset | `1` restores the legacy sleep spin. Undocumented beyond the code. | `src/mc_engine/threading.cpp` |
| `MCLA_SPIN_COUNTERS` | unset | `1` turns on the fence spin counters. | `src/mc_engine/hooks/frame_timing.cpp` |

The README lists the texture cache defaults as `1536` / `2048` / `64`. The code now derives the soft and hard limits from VRAM (see above); `64` for the render-to-texture limit still holds.

## Command line

Any cvar: `--name=value`. Values from the command line are used for that run only and are never written to `larecomp.toml` (`src/mc_engine/config_save.cpp`). The README documents `--game_data_root` and `--clock_no_scaling=true`. No other flags are parsed by code in this repository.

## SDK cvars referenced by this repository

These are registered by the ReXGlue SDK, not by code here, so type, default and range are not documented in the source of this repository.

| Name | Where referenced | Notes |
|---|---|---|
| `fullscreen` | menu: GRAPHICS > DISPLAY > FULLSCREEN | Boolean row. |
| `vsync` | menu: GRAPHICS > DISPLAY > VSYNC; `MCLA_VSYNC` | Boolean row. |
| `resolution` | menu: GRAPHICS > DISPLAY > RESOLUTION | Steps: `""` (DEFAULT), `720p`, `1080p`, `1440p`, `4k`, from `src/mc_engine/pause_menu.cpp`. |
| `resolution_scale` | menu: GRAPHICS > DISPLAY > RES SCALE (Xenia renderer only) | Steps: 1.0, 2.0, 3.0, 4.0. |
| `graphics_backend` | menu: GRAPHICS > RENDERER > GRAPHICS API | Steps: `auto`, `d3d12`, `vulkan`. Restart-only row. |
| `present_effect` | menu: GRAPHICS > FIDELITY FX > UPSCALER | Steps: `bilinear`, `cas`, `fsr`, `fsr2`, `fsr3`. Xenia renderer only. |
| `present_fsr_quality_mode` | menu: GRAPHICS > FIDELITY FX > FSR QUALITY | Steps: `auto`, `nativeaa`, `quality`, `balanced`, `performance`, `ultra_performance`. |
| `present_cas_additional_sharpness` | menu: GRAPHICS > FIDELITY FX > CAS SHARPNESS | Steps: 0.0, 0.25, 0.5, 0.75, 1.0. |
| `present_fsr_sharpness_reduction` | menu: GRAPHICS > FIDELITY FX > FSR SHARP REDUCE | Steps: 0.0, 0.2, 0.5, 1.0, 2.0. |
| `game_data_root` | `src/larecomp_app.h`, README | Game data folder. See [Getting Started](Getting-Started). |
| `user_data_root` | `src/larecomp_app.h` | When set, LARecomp does not choose the user data folder itself. |
| `user_name` | `src/profile/larecomp_profile_picker.cpp` | Name of the save to play. Interacts with the save picker and `profile_auto_select`. |
| `clock_no_scaling` | README | Workaround for the long-session frame rate collapse. |
| `skip_intro` | README | Named in the README as a way to skip the intro movies; not referenced elsewhere in this repository. |
| `bind_settings`, `bind_debug_overlay`, `bind_achievements` | `src/larecomp_app.h` | Key bindings the app re-registers under its own names; the old value seeds the new key. |

Further SDK cvars are written by `ApplyGpuFlags` in `src/larecomp_app.h` at startup (texture cache limits, `anisotropic_override` = 5 unless configured, `async_shader_compilation`, `d3d12_bindless`, readback and submission flags). They are overwritten at launch and not intended as user settings.

## Cvars by area

Types, defaults and ranges below are copied from the registration macros. Where a cvar has no description in the source the row says so.

## Graphics and performance

### `MCLA/Patches` (18)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `carbon_all_parts` | bool | `true` | - | toml / CLI | Carbon fiber can be applied to any body part, not just the hood. | `src/mc_engine/carbon_parts.cpp` |
| `dbg_print` | bool | `false` | - | toml / CLI | Enable DbgPrint console outputs. Needs a restart. | `src/mc_engine/hooks/diagnostics.cpp` |
| `real_frame_delta` | bool | `true` | - | menu: RECOMP SETTINGS > REAL FRAME DELTA; toml / CLI | Feeds the simulation the measured frame time instead of the engine's fixed 30 Hz timestep, and unlocks presentation from every-other-vblank. Required for correct physics, camera and traffic above 30 FPS. This is NOT a frame rate cap - see fps_limit for that. | `src/mc_engine/hooks/frame_timing.cpp` |
| `intro_original_speed` | bool | `true` | - | menu: RECOMP SETTINGS > ORIGINAL INTRO SPEED; toml / CLI | Present the start-up legal screens and logos 30 times a second, as the console does. They are drawn by a loop that never runs the engine timer, so fps_limit never reached them and they played as fast as the host could present. | `src/mc_engine/hooks/frame_timing.cpp` |
| `disable_motion_blur` | bool | `false` | - | menu: GAMEPLAY > CAMERA > MOTION BLUR (row shows ON when the value is false); toml / CLI | Disable Motion Blur completely. Needs a restart. | `src/mc_engine/hooks/graphics.cpp` |
| `disable_imposter_shadows` | bool | `true` | - | menu: GRAPHICS > PERFORMANCE > LOD AND FOLIAGE > FOLIAGE SHADOWS (row shows ON when the value is false) (label shows RESTART); toml / CLI | Performance Mode: Foliage won't cast shadows. Needs a restart. | `src/mc_engine/hooks/graphics.cpp` |
| `disable_msaa` | bool | `false` | - | menu: GRAPHICS > DISPLAY > MSAA (row shows ON when the value is false) (label shows RESTART); toml / CLI | Disable Anti-Aliasing (MSAA). Needs a restart. | `src/mc_engine/hooks/graphics.cpp` |
| `disable_dof` | bool | `true` | - | menu: GRAPHICS > PERFORMANCE > RENDERING > DEPTH OF FIELD (row shows ON when the value is false); toml / CLI | Disable Depth of Field (DoF) completely. | `src/mc_engine/hooks/graphics.cpp` |
| `aspect_ratio` | string | `16:9` | one of `16:9`, `16:10`, `21:9`, `32:9` | toml / CLI | Screen Aspect Ratio | `src/mc_engine/hooks/graphics.cpp` |
| `disable_rubberbanding` | bool | `false` | - | menu: RECOMP SETTINGS > RUBBERBAND (row shows ON when the value is false) (label shows RESTART); toml / CLI | Disable the computer-controlled opponents RubberBand system outright. Note this also removes MinThrottle, the leash that slows the computer-controlled opponents when it is AHEAD — so it makes races harder, not fairer. Prefer the rubberband_* scales under MCLA/Difficulty. Needs a restart. | `src/mc_engine/hooks/rubberband.cpp` |
| `skip_intro` | bool | `false` | - | toml / CLI | Skip the intro videos to prevent graphical issues. Needs a restart. | `src/mc_engine/hooks/ui.cpp` |
| `break_pairwise_collision` | bool | `false` | - | toml / CLI | Disables pairwise collision resolution. Needs a restart. | `src/mc_engine/hooks/vehicle.cpp` |
| `unlock_ride_height` | bool | `false` | - | toml / CLI | Allow the full stock ride-height table in the shop (down to rh_800 / -8). | `src/mc_engine/hooks/vehicle.cpp` |
| `unlock_wheel_fit` | bool | `false` | - | toml / CLI | Allow every stock rim size, tire profile, tire width and ride height regardless of the car's clearance metrics. | `src/mc_engine/hooks/vehicle.cpp` |
| `warp_transition_fix` | bool | `true` | - | toml / CLI | Finish a cinematic warp (pause menu -> garage from far away) whose camera flight ended between two frames. | `src/mc_engine/hooks/warper.cpp` |
| `garage_tire_offset` | bool | `true` | - | toml / CLI | Add a TIRE OFFSET row to the garage wheels > dimensions screen, editing the per-car TireOffset0/1 bytes retail parses but never uses. | `src/mc_engine/pause_menu.cpp` |
| `tire_offset_step` | double | `0.012` | - | toml / CLI | Metres of lateral wheel movement per TIRE OFFSET notch (17 notches, -8..+8, so the default spans about +/-10 cm). 0 disables the effect while leaving the garage row usable. | `src/mc_engine/tire_offset.cpp` |
| `tire_offset_targets` | uint32 | `31` | - | toml / CLI | Bitmask (decimal) of the per-wheel matrix blocks TIRE OFFSET moves: 1 = model+1088 (the tyre), 2 = +1344, 4 = +1600, 8 = +1856, 16 = +1920. 31 moves all five; drop bits to find which block owns which part. | `src/mc_engine/tire_offset.cpp` |

### `MCLA/Performance` (22)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `fma_float_fast` | bool | `true` | - | toml / CLI | On a CPU without FMA3 (Intel before Haswell, AMD before Piledriver), fused multiply-adds whose operands are single-precision values are computed with a double multiply and add, which gives the same bits, instead of the C runtime's software fma (157 to 165 cycles a call). Nothing changes on a CPU with FMA3. Needs a restart. | `src/mc_engine/fma_fast.cpp` |
| `fps_limit` | int32 | `60` | 0 to 360 | menu: GRAPHICS > DISPLAY > FPS LIMIT; steps: 30.0, 60.0, 120.0, 144.0, UNCAPPED; toml / CLI | Frame rate cap (0 = uncapped, 60 = 60 FPS, 120 = 120 FPS, 144 = 144 FPS). | `src/mc_engine/hooks/frame_timing.cpp` |
| `frame_limit_spin` | bool | `true` | - | toml / CLI | Hit the frame-limit deadline with a PAUSE spin. Off restores the sleep-then-yield wait, which gives the core back instead of burning it. | `src/mc_engine/hooks/frame_timing.cpp` |
| `vsync_fast_poll` | bool | `true` | - | toml / CLI | Drive the SDK's gpu_vsync_fast_poll. With vsync off the guest vblank interval is 1 ms, so fast poll keeps the GPU VSync worker spinning a whole core. Off restores the 1 ms sleep. Needs a restart. | `src/mc_engine/hooks/frame_timing.cpp` |
| `pace_at_swap` | bool | `true` | - | menu: GRAPHICS > DISPLAY > PACE AT PRESENTATION; toml / CLI | Hold each frame to the FPS limit right before it is presented instead of before the game's update: even present intervals (best on a variable refresh display), at the cost of up to one more frame of input latency. | `src/mc_engine/hooks/frame_timing.cpp` |
| `fence_spin_throttle` | bool | `true` | - | menu: GRAPHICS > PERFORMANCE > RENDERING > FENCE SPIN YIELD; toml / CLI | Off restores the stock guest spin. | `src/mc_engine/hooks/frame_timing.cpp` |
| `single_tile` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > RENDERING > SINGLE TILE; toml / CLI | Render the scene in a single predicated-tiling tile instead of two. Halves draw calls and state traffic with MSAA on. Requires the enlarged virtual EDRAM (SDK >= this build). | `src/mc_engine/hooks/graphics.cpp` |
| `submit_on_primary_buffer_end` | bool | `false` | - | toml / CLI | Drive the SDK's d3d12_submit_on_primary_buffer_end. Ends a D3D12 submission at every PM4 primary buffer end instead of batching the frame. Measured ~7% slower on a GTX 1650. With graphics_backend = vulkan the SDK's own vulkan_submit_on_primary_buffer_end (on by default) applies instead; not measured here, so it is left alone. Needs a restart. | `src/mc_engine/hooks/graphics.cpp` |
| `pump_wake_on_demand` | bool | `true` | - | toml / CLI | The game's frame-pacing thread (it frees the draw command ring's slots and sleeps out 7 ms a round) comes round at once when the main thread is waiting for a slot, instead of after its Sleep. Lifts the frame rate an uncapped game was held to when a frame takes less than 7 ms; changes nothing under a frame rate cap below that. | `src/mc_engine/hooks/pump_wake.cpp` |
| `perf_no_shadows` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > SHADOWS > SHADOWS (row shows ON when the value is false); toml / CLI | Drop every real-time shadow render phase. Largest single framerate win; the world loses cast shadows. Applied straight to the renderer's phase enable mask, so it takes effect immediately. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_shadow_phase_bits` | string | `0x65E0` | - | menu: GRAPHICS > PERFORMANCE > SHADOWS > SHADOW PHASES; steps: 0x61E0 (STOCK), 0x400 (BLEND ONLY), 0x65E0 (STOCK+BLEND), 0x7DF0 (ALL (BREAKS)); toml / CLI | Render phase bits perf_no_shadows clears in renderer+448 (hex). 0x65E0 = sun cascades (0x61E0, what the game's own 'noshadows' clears) plus the night/blend shadow phase 0x400 that it misses. 0x400 alone isolates the night path. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_no_race_shadows` | bool | `false` | - | toml / CLI | Drop the shadow pass during races only (dev switch 'noraceshadows'). Needs a restart. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_fast_vehicle_shadows` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > SHADOWS > CHEAP CAR SHADOW (label shows RESTART); toml / CLI | Cheap blob shadow under vehicles instead of the real-time one (dev switch 'fastVehShadows'). Use instead of perf_no_shadows to keep world shadows. Needs a restart. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_no_impostors` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > SHADOWS > TREE IMPOSTORS (row shows ON when the value is false); toml / CLI | Do not allocate the foliage impostor render targets (dev switch 'noimpostors'). Distant trees lose their billboards. Applies on the next load of that system. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_no_trees` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > SHADOWS > FOLIAGE (row shows ON when the value is false); toml / CLI | Skip loading the prop/foliage set (dev switch 'notrees'). Last resort. Applies on the next district load. | `src/mc_engine/hooks/render_phases.cpp` |
| `perf_no_fullscreen_blur` | bool | `false` | - | menu: GRAPHICS > PERFORMANCE > RENDERING > SCREEN BLUR (row shows ON when the value is false) (label shows RESTART); toml / CLI | Skip the full-screen blur pass (dev switch 'nofsblur'). Needs a restart. | `src/mc_engine/hooks/render_phases.cpp` |
| `enable_ambient_tuning` | bool | `true` | - | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > CITY AMBIENT CULLING; toml / CLI | Enable ambient traffic and pedestrian density tuning. | `src/mc_engine/hooks/world_tuning.cpp` |
| `traffic_unspawn_dist` | double | `250.0` | 100.0 to 600.0 | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > TRAFFIC RANGE; steps: 100.0, 150.0, 200.0, 250.0, 300.0, 400.0, 500.0, 600.0; toml / CLI | Traffic vehicle unspawn radius in meters (default 400.0, lower = higher FPS in city). | `src/mc_engine/hooks/world_tuning.cpp` |
| `ped_density_scale` | double | `0.5` | 0.0 to 2.0 | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > PEDESTRIANS; steps: 0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0; toml / CLI | Pedestrian density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full). | `src/mc_engine/hooks/world_tuning.cpp` |
| `parked_car_scale` | double | `0.5` | 0.0 to 2.0 | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > PARKED CARS; steps: 0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0; toml / CLI | Parked car density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full). | `src/mc_engine/hooks/world_tuning.cpp` |
| `global_max_draw_distance` | double | `0.0` | 0.0 to 6000.0 | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > PROP DRAW DIST; steps: STOCK, 250.0, 500.0, 1000.0, 1500.0, 2000.0, 3000.0, 4000.0; toml / CLI | fragTuneStruct::GlobalMaxDrawingDistance — draw distance for breakable props (poles, signs, fences). 0 = keep the tune file's value (3000); the engine's own constructor default is 250. | `src/mc_engine/hooks/world_tuning.cpp` |
| `breaking_frame_rate_limit` | double | `0.0` | 0.0 to 120.0 | menu: GRAPHICS > PERFORMANCE > WORLD DENSITY > BREAK FPS FLOOR; steps: STOCK, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0; toml / CLI | fragTuneStruct::BreakingFrameRateLimit — framerate floor under which the engine stops spawning new fragment breaks. 0 = keep the tune file's value (10); the engine's own constructor default is 30. | `src/mc_engine/hooks/world_tuning.cpp` |

### `MCLA/LOD` (2)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `lod_traffic_scale` | double | `1.0` | 0.1 to 10.0 | menu: GRAPHICS > PERFORMANCE > LOD AND FOLIAGE > TRAFFIC LOD; steps: 0.1, 0.5, 1.0, 2.0, 5.0, 10.0; toml / CLI | Level-of-detail scale for traffic. Higher keeps detail at a distance. | `src/mc_engine/hooks/graphics.cpp` |
| `lod_city_scale` | double | `1.0` | 0.1 to 10.0 | menu: GRAPHICS > PERFORMANCE > LOD AND FOLIAGE > CITY LOD; steps: 0.1, 0.5, 1.0, 2.0, 5.0, 10.0; toml / CLI | Level-of-detail scale for the city. Higher keeps detail at a distance. | `src/mc_engine/hooks/graphics.cpp` |

### `Graphics` (1)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `larecomp_texture_folder` | string | `""` | - | toml / CLI | Folder the texture dumps are written to. Empty: textures_dump/ in the mod folder (modloader/, mods/ or models/ beside the executable) | `src/mc_engine/texture_dump.cpp` |

### `MCLA/Video` (1)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `display_gamma` | string | `tv` | one of `tv`, `pc` | menu: RECOMP SETTINGS > DISPLAY GAMMA; steps: tv (CONSOLE), pc; toml / CLI | Display gamma ramp. tv: the console's for an HDTV (BT.709), as the game ships. pc: sRGB, for PC monitors, with brighter shadows. Live. | `src/mc_engine/hooks/display_gamma.cpp` |

### `MCLA/Textures` (2)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `texture_mods` | bool | `true` | - | menu: GRAPHICS > TEXTURE MODS > TEXTURE MODS; toml / CLI | Use the texture mods (`<mods>/<Mod>/textures_replace/`). False shows the game's own textures; changing it re-indexes the mods at the start of the next frame. | `src/gpu/textures.cpp` |
| `texture_reload` | bool | `false` | - | toml / CLI | Set to true to reload the texture mods (`<mods>/<Mod>/textures_replace/`) at the start of the next frame, the same as RELOAD TEXTURE MODS in the pause menu. It clears itself afterwards. | `src/gpu/textures.cpp` |

## Audio

### `MCLA/Audio` (9)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `mcla_audio_output` | string | `auto` | - | toml / CLI | auto: the default device, or silence when none opens. null: always silent. Needs a restart. | `src/mc_engine/audio_output.h` |
| `audio_mixer_priority` | int32 | `-1` | -1 to 3 | toml / CLI | Host priority of the game's two audio threads: the guest thread that mixes each 5.33 ms frame (the game's request for a higher priority is ignored) and the SDK's audio worker that calls the game for each frame: 0 = normal, 1 = above normal, 2 = highest, 3 = highest and registered with the Windows multimedia scheduler ("Pro Audio"), -1 = automatic: 3 on a PC with 5 or fewer logical processors, 0 otherwise. Needs a restart. | `src/mc_engine/hooks/audio_mixer.cpp` |
| `custom_music` | bool | `false` | - | toml / CLI | Old host player: decodes `<exe>/music/*.mp3` outside the game and plays them over it, with the guest radio carrying only the row. Off, because the native route (mc_engine/music) turns the same files into real game objects, real cues and real streaming wave banks -- the radio then behaves exactly as it does for a shipped song. Needs a restart. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `custom_music_announce` | bool | `true` | - | toml / CLI | Print the song name on the HUD info strip (the bar under the speedometer that normally carries the district name) whenever the radio moves to a new track. Uses the game's own message channel, so it looks like any other line the game puts there; applies to the shipped radio as well as custom MP3s. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `custom_music_announce_seconds` | double | `15.0` | 1.0 to 60.0 | toml / CLI | How long the song name stays on the HUD info strip. The game's own uses of that channel are short (3 s for MUSIC OFF, 5 s for a district name), but a track title is worth reading, so this defaults longer. Anything the game itself puts on the strip replaces it before the time is up. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `custom_music_volume` | double | `0.5` | - | toml / CLI | Trim applied on top of the in-game music slider for custom MP3 tracks. The shipped radio goes through the RAGE mixer and its ducking curves, which the host player does not, so 1.0 comes out noticeably louder. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `custom_music_cue_gain_db` | double | `4.0` | 0.0 to 6.0 | toml / CLI | Gain for native custom music in the game's audio mixer, after the bank's 3 dB MP3 decode headroom. Shipped songs keep their original gain. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `music_bank_log` | bool | `false` | - | toml / CLI | Log every streaming wave bank the audio engine opens: the bank id (an index into the sounds.dat string table) and whether the file was found. Use it when a song plays silently -- it separates 'the bank was never asked for' from 'the bank was opened and decoded to nothing'. | `src/mc_engine/mp3custom/mp3custom.cpp` |
| `music_list` | bool | `false` | - | toml / CLI | Log the whole radio once, right after mcMusicManager finishes building it: every genre bucket with each song's hash code and title. This is how you tell whether a song added through the audio metadata (a rebuilt game.dat plus its wave bank) actually reached AddSong, since the game's own log line for that was compiled out. Needs a restart. | `src/mc_engine/mp3custom/mp3custom.cpp` |

## Input (controller, keyboard, wheel)

### `MCLA/Input` (22)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `adaptive_triggers` | bool | `true` | - | menu: RECOMP SETTINGS > DUALSENSE TRIGGERS; toml / CLI | DualSense adaptive triggers: a throttle spring on R2 and brake resistance on L2 while a car is driven, slack everywhere else. Other controllers are not touched. | `src/mc_engine/adaptive_triggers.cpp` |
| `trigger_throttle` | int32 | `3` | 1 to 8 | toml / CLI | R2 resistance at the end of its travel, 1 (light) to 8 (firm). | `src/mc_engine/adaptive_triggers.cpp` |
| `trigger_brake` | int32 | `6` | 1 to 8 | toml / CLI | L2 resistance at the end of its travel, 1 (light) to 8 (firm). | `src/mc_engine/adaptive_triggers.cpp` |
| `hide_mouse_pointer` | bool | `true` | - | menu: RECOMP SETTINGS > HIDE MOUSE POINTER; toml / CLI | Hide the mouse pointer during play. It goes at once when a controller is used and after about 3 seconds without mouse movement, only over the game window while it has focus. Mouse movement, a click, the wheel or losing focus brings it back, and it stays while the mouse is over an overlay. | `src/mc_engine/cursor_hide.cpp` |
| `script_pad_latch_ms` | int32 | `100` | 0 to 500 | toml / CLI | A script that polls a pad button only every few updates (the title screen's Start above 30 FPS) is told about a press it missed, at its next look, if its previous look was at most this many milliseconds before. | `src/mc_engine/hooks/script_input.cpp` |
| `left_stick_deadzone` | double | `0.0` | 0.0 to 50.0 | menu: RECOMP SETTINGS > LEFT STICK DEAD ZONE; steps: OFF, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0; toml / CLI | Dead zone of the left stick, in percent of its range: a stick that rests off centre (drift) reads as centred inside it. 0 = none. | `src/mc_engine/hooks/stick_deadzone.cpp` |
| `right_stick_deadzone` | double | `0.0` | 0.0 to 50.0 | menu: RECOMP SETTINGS > RIGHT STICK DEAD ZONE; steps: OFF, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0; toml / CLI | Dead zone of the right stick, in percent of its range: a stick that rests off centre (drift) reads as centred inside it. 0 = none. | `src/mc_engine/hooks/stick_deadzone.cpp` |
| `controller_vibration` | bool | `true` | - | menu: RECOMP SETTINGS > VIBRATION; toml / CLI | Controller vibration. Off keeps the game's rumble from reaching any controller and stops a motor that is running. | `src/mc_engine/hooks/vibration.cpp` |
| `keyboard_native` | bool | `false` | - | toml / CLI | Native keyboard. Feeds the keyboard straight into the game's own input layer (rage ioMapper), which the SDK's keystroke API never filled: menus and driving get separate key sets, so Enter can be 'accept' in a menu and Pause on the road without ever being both. | `src/mc_engine/keyboard_native.cpp` |
| `keycaps_dir` | string | `""` | - | toml / CLI | Folder of the 64x64 key cap PNGs the keyboard glyphs are painted from, named by lower-case key (return.png, escape.png, a.png, capslock.png ...). Empty = the set embedded in the executable (src/keycaps/). | `src/mc_engine/keyboard_native.cpp` |
| `map_mouse` | bool | `true` | - | toml / CLI | Drive the full map's cursor with the mouse. The pointer position is unprojected onto the map plane, so the cursor lands where you point instead of drifting like a stick would. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_fov` | double | `45.0` | 5.0 to 150.0 | toml / CLI | Vertical field of view, in degrees, of the camera the full map is drawn with. The only number the unprojection cannot read out of the game. Too small and the cursor lags behind the pointer towards the screen edges; too large and it runs ahead. The centre of the screen is correct at any value, so calibrate by pointing at a corner. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_flip_x` | bool | `false` | - | toml / CLI | Mirror the horizontal axis of the map cursor. Needed only if the camera basis rows turn out to be left-handed -- the cursor then moves the wrong way along one axis while the other tracks correctly. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_flip_y` | bool | `false` | - | toml / CLI | Mirror the vertical axis of the map cursor. See map_mouse_flip_x. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_zoom` | bool | `true` | - | toml / CLI | Zoom the full map with the scroll wheel. The camera height is the zoom, so this walks it through the game's own min/max at +1124/+1128. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_zoom_step` | double | `1.15` | 1.01 to 3.0 | toml / CLI | Multiplier applied to the map camera's height per wheel detent. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_zoom_smooth` | double | `0.22` | 0.02 to 1.0 | toml / CLI | How much of the remaining distance to the wheel's target height the camera covers each frame. A detent is a step, and stepping the camera straight to it is what made the zoom read as jerky; this eases into it instead. 1.0 restores the instant jump. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_zoom_to_cursor` | bool | `true` | - | toml / CLI | Zoom around the point under the pointer instead of around the middle of the view. With this off the camera only changes height, which slides the world under a pointer that has not moved and drags the cursor towards the centre on every detent. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_magnet` | bool | `false` | - | toml / CLI | Leave the map's cursor magnet alone. The map snaps the cursor onto the selected icon, both on selecting it and again on every frame it stays selected, which is how a thumbstick is made to feel sticky over waypoints and what welds a pointer to the first one it brushes. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_max_steps` | int32 | `96` | 1 to 512 | toml / CLI | Ceiling on how many stick-sized steps one frame may walk the cursor towards the pointer. The map's boundary sweep is written for the small deltas a stick produces and reads a null neighbour pointer when handed a long chord, so the move is subdivided rather than teleported. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_hide_cursor` | bool | `true` | - | toml / CLI | Hide the OS pointer while the full map is up. The in-game cursor tracks it one to one, so the visible cursor is the game's own. | `src/mc_engine/map_mouse.cpp` |
| `map_mouse_diag` | bool | `false` | - | toml / CLI | Log the map screen pointer, camera basis and the world point the mouse unprojects to, once a second. | `src/mc_engine/map_mouse.cpp` |

### `MCLA/Input/Keyboard - driving` (20)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `kb_drive_steer_left` | string | `A,Left` | - | toml / CLI | Driving: steer left (pairs with the right key at the same position) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_steer_right` | string | `D,Right` | - | toml / CLI | Driving: steer right | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_gas` | string | `W,Up` | - | toml / CLI | Driving: accelerate | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_brake` | string | `S,Down` | - | toml / CLI | Driving: brake / reverse | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_handbrake` | string | `Space` | - | toml / CLI | Driving: handbrake (the pad's A) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_weight` | string | `CapsLock` | - | toml / CLI | Driving: weight transfer, the 'turn in' of the pad's B | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_nitro` | string | `Shift` | - | toml / CLI | Driving: nitro (the pad's X) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_headlights` | string | `H` | - | toml / CLI | Driving: headlights (the pad's Y) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_horn` | string | `G` | - | toml / CLI | Driving: horn | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_camera` | string | `V` | - | toml / CLI | Driving: change camera, one step along the game's cycle (the pad's RB) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_camera_keys` | string | `1,2,3,4,5` | - | toml / CLI | Driving: keys that jump straight to a camera, slot 1..5 (empty = off). The game only has a button that steps through its cameras; the jump presses it for you, as many times as the cycle needs, and stops on the one asked for. | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_camera_views` | string | `4,0,3,2,1` | - | toml / CLI | Driving: which camera each slot above selects, as the game numbers them: 0 bumper, 1 cockpit, 2/3/4 the chase views. Default: slot 1 action(4), 2 bumper(0), 3 classic(3), 4 static(2), 5 cockpit(1). Swap numbers here if 3 and 4 turn out the other way round. | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_lookback` | string | `R` | - | toml / CLI | Driving: look back | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_map` | string | `M` | - | toml / CLI | Driving: map | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_talk` | string | `T` | - | toml / CLI | Driving: talk | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_shift_up` | string | `E` | - | toml / CLI | Driving: gear up (manual transmission) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_shift_down` | string | `Q` | - | toml / CLI | Driving: gear down (manual transmission) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_pause` | string | `""` | - | toml / CLI | Driving: the PAUSE action of the control map. Empty by default: the pause menu is opened by Start in the UI mapper (kb_ui_start), which is where the pad's Start goes as well. | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_popup_select` | string | `""` | - | toml / CLI | Driving: answer a pop-up with A (empty = pad only; Enter would also be Pause) | `src/mc_engine/keyboard_native.cpp` |
| `kb_drive_popup_back` | string | `""` | - | toml / CLI | Driving: answer a pop-up with B (empty = pad only) | `src/mc_engine/keyboard_native.cpp` |

### `MCLA/Input/Keyboard - menus` (14)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `kb_ui_accept` | string | `Return,Space` | - | toml / CLI | Menus: A / accept | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_back` | string | `Escape,Backspace` | - | toml / CLI | Menus: B / back | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_x` | string | `X` | - | toml / CLI | Menus: X | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_y` | string | `Y` | - | toml / CLI | Menus: Y | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_lb` | string | `Q` | - | toml / CLI | Menus: LB (previous tab) | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_rb` | string | `E` | - | toml / CLI | Menus: RB (next tab) | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_lt` | string | `PageUp` | - | toml / CLI | Menus: LT | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_rt` | string | `PageDown` | - | toml / CLI | Menus: RT | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_up` | string | `Up,W` | - | toml / CLI | Menus: up | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_down` | string | `Down,S` | - | toml / CLI | Menus: down | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_left` | string | `Left,A` | - | toml / CLI | Menus: left | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_right` | string | `Right,D` | - | toml / CLI | Menus: right | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_start` | string | `Return,Escape` | - | toml / CLI | Start: opens the pause menu on the road and answers "PRESS START" on the title screen. It is Start only while NO menu is open: with a menu up these keys are accept/back and nothing else, which is what keeps Enter from being accept and Start at once. | `src/mc_engine/keyboard_native.cpp` |
| `kb_ui_select` | string | `Tab` | - | toml / CLI | Menus: the Back/Select button (GPS, map). Start has no key here on purpose. | `src/mc_engine/keyboard_native.cpp` |

### `MCLA/Wheel` (25)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `wheel_fake` | string | `off` | one of `off`, `pad`, `keys` | toml / CLI | Present the game with a steering wheel without owning one. off: nothing changes. pad: the real pad or keyboard-and-mouse state is shown as a wheel (stick X steers, triggers are the pedals, bumpers shift). keys: A/D steer, W throttle, S brake, Q/E shift down/up. Takes effect on the next input poll. | `src/mc_engine/wheel/wheel_fake.cpp` |
| `wheel_ffb_log` | bool | `true` | - | toml / CLI | Log every force-feedback command the game sends while wheel_fake is on: effect id, type, duration and parameters. Capped so a long race cannot fill the log. | `src/mc_engine/wheel/wheel_fake.cpp` |
| `wheel_ffb` | bool | `true` | - | toml / CLI | Force feedback on the steering wheel. Off: the wheel is never given a force. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_strength` | int32 | `70` | - | toml / CLI | Force feedback strength, 0-100: the share of the wheel's full force that the game's strongest effect reaches. The game's own forces are mostly 10-40% of its scale (it was written for a light 360 wheel), so a direct-drive wheel needs this high to feel them; a belt or gear wheel may want it lower. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_boost` | int32 | `200` | - | toml / CLI | Multiplies the forces the GAME asks for (constant, vibration, its spring and damper), 100-500 %, before wheel_ffb_strength. The game's forces are mostly 10-40% of its scale, written for a light 360 wheel, so on a strong base they feel weak: 200 doubles them, with the result capped at the wheel's full force. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_sign` | string | `auto` | one of `auto`, `normal`, `inverted` | toml / CLI | Which way a positive constant force turns the wheel (centring does not use it). auto: measured once per device with a gentle push (keep your hands off the wheel); normal / inverted: skip the test. If the game's constant forces push into the turn instead of against it, set the other one. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_centering` | int32 | `40` | - | toml / CLI | Centring added by this program while the game is not running its own spring (it does not while free-roaming), 0-100: the most force the centring spring can reach, before wheel_ffb_strength. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_autocenter` | bool | `true` | - | toml / CLI | Only used when wheel_ffb_centering is 0: leave the wheel driver's own centring spring on. With centring above 0 the driver's spring is switched off (two springs would fight). Takes effect when the wheel is opened. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_panic` | bool | `false` | - | toml / CLI | Emergency stop for the wheel's force feedback: switching it on stops every force and disarms until wheel_ffb_rearm is switched on. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_rearm` | bool | `false` | - | toml / CLI | Switch on to arm force feedback again after an emergency stop (it re-checks the wheel first). Switch it off again before the next time. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_watchdog_ms` | int32 | `500` | - | toml / CLI | Force goes to zero when the game has not run an input frame for this long (a loading screen, a hang) and comes back by itself when it does. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_selftest` | bool | `false` | - | toml / CLI | When force feedback arms, play a staged test and log it: 0.5 s push one way, 0.5 s the other, 2 s of centring spring (turn the wheel a little and let go), 1 s of damper. Every device call's result and the wheel's position (every 50 ms) go to the log, so a dead force path shows in the log whatever you feel. Needs the wheel to be free. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_ffb_trace` | bool | `false` | - | toml / CLI | Log the force sent to the wheel about four times a second, every device call's result, and every state change. | `src/mc_engine/wheel/wheel_ffb.cpp` |
| `wheel_mode` | string | `auto` | one of `auto`, `wheel`, `gamepad` | toml / CLI | Who drives when a wheel is connected. auto: the wheel until the gamepad is moved, the gamepad until the wheel is. wheel: always the wheel. gamepad: the wheel is ignored and the game sees the pad. Only the driving controls switch; Start, Back, A, B and the D-pad work from both. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_device` | string | `auto` | - | toml / CLI | Which joystick is the wheel. auto: the first one SDL calls a wheel or a known wheel model. off: none. Anything else is matched against the device name (part of it, any case) or written as vendor:product in hex (046d:c24f) and opens that device even if SDL does not call it a wheel. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_device_range` | int32 | `900` | - | toml / CLI | The lock-to-lock rotation the wheel is set to in its own driver, in degrees. The wheel reports -1..1 over that range; this and wheel_game_range turn it into the game's range. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_game_range` | int32 | `540` | - | toml / CLI | The lock-to-lock rotation that reaches full steering lock in the game, in degrees. The game reads steering as 8 bits (255 steps), so a smaller range steers finer: 270 deg = 1.06 deg per step, 540 = 2.1, 900 = 3.5. Full lock is reached before the wheel's own end stop when this is smaller than wheel_device_range. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_steer_deadzone` | double | `0.01` | - | toml / CLI | Fraction of the wheel's range around centre that reads as straight ahead (0.01 = 1%). | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_steer_gamma` | double | `1.0` | - | toml / CLI | Shapes steering after the range scaling: 1 is linear, above 1 softens the centre. The game applies its own curve afterwards, so leave this at 1 unless the car feels twitchy. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_virtual` | string | `off` | one of `off`, `low`, `high` | toml / CLI | Test aid: create a virtual SDL wheel (type wheel, 4 axes, 20 buttons, a hat) driven from the keyboard (A/D steer, W throttle, S brake, Q/E paddles) so the whole SDL path can be run without hardware. off, low (pedals rest at -32768) or high (pedals rest at +32767). | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_learn` | bool | `false` | - | toml / CLI | Run the wheel setup: a console window walks through the steering, the pedals, the paddles and the H-shifter, watches which axes and buttons move, and writes the wheel's section of wheel_profiles.ini. Switches itself off when it is done. Start the game with --wheel_learn=true before playing. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_auto_setup` | bool | `true` | - | toml / CLI | The first time a wheel is found that has no section in wheel_profiles.ini, ask whether to run the setup now (a yes/no box, shown once the game window is up). Answering no writes a marker so it is not asked again; the setup can always be started with wheel_learn. Off: never ask. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_cancel_game_deadzone` | double | `0.05` | - | toml / CLI | The game puts a dead zone of about 5% on its stick and pedal axes (measured: steering comes out as (x - 0.05) / 0.95), written for a thumb stick. A wheel is precise, so the first few degrees would do nothing. This lifts the wheel's output by that amount, so the game's own dead zone cancels and the response is linear. 0 turns it off. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_trace` | bool | `false` | - | toml / CLI | Log, once a second, every axis and pressed button SDL reads from the wheel and what the game is given (who drives, steering, pedals, buttons). For finding out why a wheel behaves oddly: start the game with it, reproduce the problem, send the log. | `src/mc_engine/wheel/wheel_input.cpp` |
| `wheel_pedal_autorange` | bool | `true` | - | toml / CLI | Pedals that never reach the end of their travel read full once they have been pressed to where they do reach (after the first firm press). Off: the pedals use their full nominal range. A pedal with throttle_full / brake_full in the profile is never auto-ranged. | `src/mc_engine/wheel/wheel_input.cpp` |

## Camera and HUD

### `MCLA/Camera` (37)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `cam_freelook` | bool | `false` | - | menu: GAMEPLAY > CAMERA > LOOK AROUND > FREE LOOK; toml / CLI | Free look on the gameplay camera with the mouse. The shipped lookaround is four digital pan actions easing towards clamped limits, which is why the camera settles on a handful of angles instead of following you. While this is on the cursor is captured and the mouse steers the view directly. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_sens` | double | `0.0035` | 0.0001 to 0.05 | menu: GAMEPLAY > CAMERA > LOOK AROUND > LOOK SENS; steps: 0.0015, 0.0025, 0.0035, 0.005, 0.007, 0.01; toml / CLI | Radians of view movement per pixel of mouse movement. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_yaw_limit` | double | `3.14159` | 0.1 to 3.15 | toml / CLI | How far the view may turn left or right, in radians. Pi is all the way round. This is the cap on our own angle, not the game's -- the game's own limits are what this feature overwrites. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_pitch_limit` | double | `0.7` | 0.05 to 1.5 | toml / CLI | How much tilt the mouse may accumulate, in radians. This caps the input, not the result -- where the camera ends up is decided by the two elevation limits below, which are what keep it out of the ground. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_inplace_rigs` | string | `0,1` | - | toml / CLI | Comma-separated camera rig indices that turn in place instead of orbiting: the views mounted on the car. Switch to a view, read the rig number out of the cam_freelook_diag line, add it here. Empty orbits everything. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_min_elev` | double | `-0.08` | -0.5 to 1.0 | toml / CLI | Lowest the camera may go, as an angle in radians above the horizontal through the point it orbits. 0 is dead level with the car; below that it goes under it and into the ground. Negative values are allowed for deliberately low shots, on the understanding that the ground is not consulted. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_max_elev` | double | `1.3` | 0.2 to 1.5 | toml / CLI | Highest the camera may go, same units. Short of pi/2 on purpose: at dead overhead the forward vector is parallel to world up and the yaw stops meaning anything. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_approach` | double | `0.0` | 0.0 to 200.0 | toml / CLI | LookaroundApproachRate while free look is on. 0 leaves the shipped value. The shipped one eases the view towards its target over a good fraction of a second, which reads as lag under a mouse. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_lerp` | double | `0.0` | 0.0 to 1.0 | toml / CLI | LookaroundLerpFactor while free look is on. 0 leaves the shipped value. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_return_delay` | double | `0.35` | 0.0 to 30.0 | menu: GAMEPLAY > CAMERA > LOOK AROUND > RECENTER DELAY; steps: OFF, 0.15, 0.35, 0.6, 1.0, 2.0; toml / CLI | Seconds the mouse has to sit still before the view starts easing back behind the car. The game's own equivalent is LookaroundDelayTime. Set it very high to keep the view wherever it was left. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_return_rate` | double | `4.0` | 0.1 to 40.0 | menu: GAMEPLAY > CAMERA > LOOK AROUND > RECENTER SPEED; steps: 1.0, 2.0, 4.0, 8.0, 16.0; toml / CLI | How quickly the view eases back once the delay is up, as a fraction of the remaining angle per second. The game's own equivalent is LookaroundDecayRate. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_invert_x` | bool | `false` | - | menu: GAMEPLAY > CAMERA > LOOK AROUND > INVERT LOOK X; toml / CLI | Mirror the horizontal axis. Needed if the tune's left/right limits turn out to be signed the other way round from what this assumes. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_invert_y` | bool | `false` | - | menu: GAMEPLAY > CAMERA > LOOK AROUND > INVERT LOOK Y; toml / CLI | Mirror the vertical axis. See cam_freelook_invert_x. | `src/mc_engine/camera_look.cpp` |
| `cam_look_obj` | int32 | `860` | 0 to 4096 | toml / CLI | Byte offset into the camera system holding the pointer to the object the look lives in. 860 is what the field probe found. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_orbit` | double | `7.5` | 1.0 to 200.0 | toml / CLI | Distance from the camera to the point it turns around, used whenever the object's own +156 is not usable. | `src/mc_engine/camera_look.cpp` |
| `cam_look_phase` | int32 | `2` | 0 to 2 | toml / CLI | The camera collision lives on its own switch, cam_freelook_collide, and not on this one -- see there. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_cs_invert` | bool | `false` | - | menu: GAMEPLAY > CAMERA > LOOK AROUND > COLLISION FLIP; toml / CLI | Flip the sign used when feeding the camera state's own two angle channels (cs+776 yaw, cs+772 pitch). If the view turns opposite to the mouse with cam_freelook_collide on, this is the switch. Only affects the collide path. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_collide` | bool | `false` | - | menu: GAMEPLAY > CAMERA > LOOK AROUND > CAMERA COLLISION; toml / CLI | Turn cam_freelook_diag on with it -- the log says whether the hook ran, which camera state it got, and what CollideType that state carries. With this off, cam_look_phase decides everything, exactly as before. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_tune` | bool | `false` | - | toml / CLI | Also pin the LookaroundLimit* tune fields, the first approach. It did not hold on its own and is kept only so the two can be tried together. | `src/mc_engine/camera_look.cpp` |
| `cam_freelook_diag` | bool | `false` | - | toml / CLI | Log the lookaround tune fields as they are found and the angles being written, once a second. | `src/mc_engine/camera_look.cpp` |
| `cam_probe_mark` | bool | `false` | - | toml / CLI | Button: snapshot the gameplay camera object. Flips back off. Do it with the car still and the view centred, then look around, then use cam_probe_dump. | `src/mc_engine/camera_look.cpp` |
| `cam_probe_dump` | bool | `false` | - | toml / CLI | Button: snapshot the camera again, diff it against cam_probe_mark and write every float that moved to `<exe>/cam_fields.txt.` Flips back off. | `src/mc_engine/camera_look.cpp` |
| `cam_probe_run` | bool | `false` | - | toml / CLI | Button: run the whole camera measurement on a timer. Close the overlay when it is armed; the baseline is taken after cam_probe_arm_delay with the camera left alone, and the diff cam_probe_hold_delay after that with the look held. Both samples happen with nothing on screen. Flips back off. | `src/mc_engine/camera_look.cpp` |
| `cam_probe_arm_delay` | double | `4.0` | 1.0 to 30.0 | toml / CLI | Seconds between arming cam_probe_run and the baseline sample. Long enough to close the overlay and let the camera settle. | `src/mc_engine/camera_look.cpp` |
| `cam_probe_hold_delay` | double | `6.0` | 1.0 to 30.0 | toml / CLI | Seconds between the baseline sample and the diff. Hold the look for all of it. | `src/mc_engine/camera_look.cpp` |
| `fov_1p_scale` | double | `1.0` | 0.5 to 5.0 | menu: GAMEPLAY > CAMERA > FOV > 1P FOV; steps: 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0; toml / CLI | FOV scale — 1st person / cockpit (0.5 = narrower, 2.0 = wider) | `src/mc_engine/hooks/camera.cpp` |
| `fov_3p_scale` | double | `1.0` | 0.5 to 5.0 | menu: GAMEPLAY > CAMERA > FOV > 3P FOV; steps: 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0; toml / CLI | FOV scale — 3rd person / chase cam (0.5 = narrower, 2.0 = wider) | `src/mc_engine/hooks/camera.cpp` |
| `smooth_chase_cam` | bool | `true` | - | menu: GAMEPLAY > CAMERA > CHASE CAMERA > SMOOTH CHASE CAM; toml / CLI | Fix: Smooth chase camera boom interpolation at 60 FPS using continuous-time exponential decay. | `src/mc_engine/hooks/camera.cpp` |
| `chase_cam_smoothing_factor` | double | `1.0` | 0.1 to 3.0 | menu: GAMEPLAY > CAMERA > CHASE CAMERA > CAM SMOOTH FACTOR; steps: 0.2, 0.5, 0.8, 1.0, 1.2, 1.5, 2.0; toml / CLI | Chase camera boom smoothing factor multiplier (0.1 - 3.0). | `src/mc_engine/hooks/camera.cpp` |
| `smooth_camera_transitions` | bool | `true` | - | toml / CLI | Fix: Eliminate camera transition jitter and 180-degree wrap flipping when transitioning between orbit, menu, and gameplay cameras. | `src/mc_engine/hooks/camera.cpp` |
| `scale_camera_shake` | bool | `true` | - | toml / CLI | Fix: Scale camera shake intensity across framerates (60+ FPS) to prevent violent noise jitter during drifts, burnouts, and crashes. | `src/mc_engine/hooks/camera.cpp` |
| `camera_shake_scale` | double | `0.6` | 0.0 to 3.0 | toml / CLI | Camera shake intensity multiplier (0.0 = disabled, 0.6 = balanced PC rumble, 1.0 = full console shake). | `src/mc_engine/hooks/camera.cpp` |
| `debug_cam` | string | `off` | one of `off`, `free` | menu: GAMEPLAY > CAMERA > FREECAM > FREECAM; steps: off, free (ON); toml / CLI | Free-fly camera during live gameplay: left stick moves, right stick looks, triggers change speed. Gameplay keeps running underneath (drive, traffic, physics). | `src/mc_engine/hooks/camera.cpp` |
| `debug_cam_speed` | double | `40.0` | 1.0 to 500.0 | menu: GAMEPLAY > CAMERA > FREECAM > CAM SPEED; steps: 10.0, 20.0, 40.0, 80.0, 150.0, 300.0; toml / CLI | Free-fly camera move speed (world units/second). | `src/mc_engine/hooks/camera.cpp` |
| `debug_cam_sens` | double | `2.5` | 0.2 to 10.0 | toml / CLI | Free-fly camera look sensitivity (radians/second at full stick). | `src/mc_engine/hooks/camera.cpp` |
| `debug_cam_mouse` | bool | `true` | - | toml / CLI | Free-fly camera: use the mouse to look (captures the cursor while active). | `src/mc_engine/hooks/camera.cpp` |
| `debug_cam_mouse_sens` | double | `0.0025` | 0.0002 to 0.02 | toml / CLI | Free-fly mouse look sensitivity (radians per mouse pixel). | `src/mc_engine/hooks/camera.cpp` |

### `MCLA/Menu Camera` (8)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `menu_cam_lock` | bool | `false` | - | toml / CLI | Always show the same shot instead of letting the game rotate. Off = the game picks (setMenuCam plus its own visibility probes) and menu_cam_slot is ignored; on = menu_cam_slot every time. This is the on/off switch — the slot number by itself never locks anything. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_slot` | int32 | `0` | - | toml / CLI | Which shot menu_cam_lock pins (0-63). Ignored while menu_cam_lock is off, so parking a number here is harmless. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_custom` | bool | `false` | - | toml / CLI | Place the menu camera at the shot from &lt;exe dir&gt;/menu_cameras.txt matching the current slot. Slots with no line keep the stock camera. The file is re-read when it changes, so you can tune it with the menu on screen. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_only_custom` | bool | `false` | - | toml / CLI | Show only your own shots. Whatever slot the game picks is remapped onto the lines present in menu_cameras.txt, so the stock cameras never appear. The game's rotation still decides which of your shots comes up; turn on menu_cam_lock to pin one. Needs menu_cam_custom. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_cycle` | double | `0.0` | 0.0 to 600.0 | toml / CLI | Seconds between shots, rotating through every line in menu_cameras.txt. 0 = no rotation. The game does not rotate the front-end camera itself — it only calls setMenuCam for the network and garage menus, so without this the boot screen sits on one shot forever. Ignored while menu_cam_lock is on. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_record` | bool | `false` | - | toml / CLI | Capture the game's own menu shots into menu_cameras.txt as 'cam' lines. There is no table of stock camera spots in the XEX — they only exist as runtime state — so this samples the real camera whenever it moves somewhere new (25+ units) and appends it with the next free slot number. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_diag` | bool | `false` | - | toml / CLI | Log the camera the game hands to the renderer (mode id, position, matrix rows) once per mode change, and every shot this places. Turn on if a shot does not show up or points the wrong way. | `src/mc_engine/menu_camera.cpp` |
| `menu_cam_dump` | command | n/a | - | toml / CLI | Append the current spot to &lt;exe dir&gt;/menu_cameras.txt as a ready-to-use 'cam' line. Fly there with debug_cam = free first (the free-fly pose gives both the position and the look direction); without it the player car's position is used. Also records the menu camera currently on screen. | `src/mc_engine/menu_camera.cpp` |

### `MCLA/HUD` (13)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `rearview` | bool | `false` | - | menu: GAMEPLAY > CAMERA > REAR VIEW > REAR VIEW; toml / CLI | Show the rear view mirror at the top right of the screen (a feature the retail game ships switched off). Takes effect immediately. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_position` | string | `stock` | one of `stock`, `left_top`, `middle_top`, `left_down`, `middle_footer`, `right_down` | menu: GAMEPLAY > CAMERA > REAR VIEW > REAR VIEW POS; steps: stock, left_top (LEFT TOP), middle_top (MIDDLE TOP), left_down (LEFT DOWN), middle_footer (MIDDLE FOOTER), right_down (RIGHT DOWN); toml / CLI | Where the mirror sits: stock (under the race position, top right), left_top, middle_top, left_down, middle_footer or right_down. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_frame` | bool | `true` | - | menu: GAMEPLAY > CAMERA > REAR VIEW > REAR VIEW FRAME; toml / CLI | Draw the mirror's frame. The retail game ships the frame meshes without their textures (black_small.dds and rearView.dds are gone), so the frame comes out solid white. Off hides it and leaves just the mirror view. Takes effect immediately. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_cars` | bool | `false` | - | toml / CLI | Show the racers' cars in the mirror. Retail's mirror phase leaves them out, and the game only treats a car as visible when the main camera sees it, so this runs the car's visibility test again with the mirror's camera and draws it with the colour pass. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_traffic` | bool | `false` | - | toml / CLI | Put ambient traffic back in the mirror's phase, where retail has it. It draws nothing useful there (cars show up as black silhouettes, or not at all) and a car close to the mirror's camera blacks the view out, so it is taken out of the phase unless this is on. Needs rearview on. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_diag` | bool | `false` | - | toml / CLI | Log, every couple of seconds, how many objects each render client holds when the mirror's phase begins. For working out why cars and traffic do not draw in it. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_shift_x` | int32 | `0` | - | toml / CLI | Fine-tunes the mirror and its frame horizontally on top of rearview_position, in 1280x720 pixels. Negative is left. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_shift_y` | int32 | `0` | - | toml / CLI | Fine-tunes the mirror and its frame vertically on top of rearview_position, in 1280x720 pixels. Positive is down. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_window_dx` | int32 | `22` | - | toml / CLI | Offset of the scene viewport from the frame window's outer corner, horizontally, in pixels. The window's left and right edges lean, so the viewport rectangle has to start inside the narrowest part or its corner pokes out of the frame. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_window_dy` | int32 | `2` | - | toml / CLI | Offset of the scene viewport from the frame window's outer corner, vertically, in pixels. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_window_w` | int32 | `229` | - | toml / CLI | Width of the scene viewport in pixels. The widest rectangle that fits inside the leaning window is 229 wide. | `src/mc_engine/hooks/rear_view.cpp` |
| `rearview_window_h` | int32 | `44` | - | toml / CLI | Height of the scene viewport in pixels. The widest rectangle that fits inside the leaning window is 44 tall. | `src/mc_engine/hooks/rear_view.cpp` |
| `speed_units` | string | `kmh` | one of `game`, `kmh`, `mph` | menu: INTERFACE > UI > SPEED UNITS; steps: game, kmh, mph; toml / CLI | Speedometer / distance units. | `src/mc_engine/hooks/ui.cpp` |

### `MCLA/UI` (12)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `button_prompts` | string | `xbox` | one of `xbox`, `playstation`, `keyboard` | menu: INTERFACE > UI > BUTTON LAYOUT; steps: xbox, playstation, keyboard; toml / CLI | On-screen button glyphs. xbox = A/B/X/Y + LB/RB/LT/RT, playstation = cross/circle/square/triangle + L1/R1/L2/R2. Both sets already ship inside the game's own UI movies and texture dictionary, so this only flips the 'platform' flag their ActionScript reads - no asset is swapped or reloaded. | `src/mc_engine/hooks/ui.cpp` |
| `ui_movie_pacing` | string | `PHOTOALBUMMOVIE` | - | toml / CLI | Flash movies advanced at the console's 30 frames a second instead of one frame per rendered frame. The game steps every UI movie exactly one frame per update, which on the 360 was 30 times a second; above 30 FPS those movies play faster -- the photo album twice as fast at 60. | `src/mc_engine/hooks/ui.cpp` |
| `ui_countdown_fix` | bool | `true` | - | toml / CLI | Keep the race countdown (3, 2, 1, GO) in time when the HUD's gauge movie steps less often than the game updates (HUDGAUGE or '*' in ui_movie_pacing, or ui_interpolate on). | `src/mc_engine/hooks/ui.cpp` |
| `ui_input_repeat_30hz` | bool | `true` | - | toml / CLI | Menu cursor auto-repeat at the console's pace. The game repeats a held direction or button by counting frames -- the first repeat after 7, then one every 6 -- which on the 360 meant 233 ms and then 200 ms. | `src/mc_engine/hooks/ui.cpp` |
| `ui_advance_on_input` | bool | `true` | - | toml / CLI | Menus answer a key press on the update it arrives in. A menu movie paced to 30 steps a second (ui_movie_pacing) would otherwise show the highlight moving up to 33 ms after the press, 17 ms on average. | `src/mc_engine/hooks/ui.cpp` |
| `ui_interpolate` | bool | `true` | - | menu: GRAPHICS > DISPLAY > SMOOTH UI; toml / CLI | Smooth UI. | `src/mc_engine/hooks/ui_smooth.cpp` |
| `ui_interpolate_camera` | bool | `true` | - | toml / CLI | With ui_interpolate: also draw the pause menu's 3D between the movie's last two steps: the camera that swings in when the menu opens, between its pages and out again, and the place, size and turn of the 3D panels. Off leaves them in 30 Hz steps. | `src/mc_engine/hooks/ui_smooth.cpp` |
| `ui_theme_accent` | string | `""` | - | menu: INTERFACE > UI > MENU BLUE; toml / CLI | Replaces the blue/teal that every menu uses (selection bar, highlights, arrows) with this color, as RRGGBB hex. Empty keeps the game's own. Shades of the original keep their brightness relative to it. | `src/mc_engine/hooks/ui_theme.cpp` |
| `ui_theme_title` | string | `""` | - | menu: INTERFACE > UI > TITLES AND GLOBE; toml / CLI | Replaces the yellow of the menu titles and the spinning globe, as RRGGBB hex. Empty keeps the game's own. | `src/mc_engine/hooks/ui_theme.cpp` |
| `ui_theme_frame` | string | `""` | - | menu: INTERFACE > UI > FRAME GRAY; toml / CLI | Replaces the gray of the menu frame, as RRGGBB hex. Empty keeps the game's own. | `src/mc_engine/hooks/ui_theme.cpp` |
| `ui_theme_skip_movies` | string | `HUDMOVIE` | - | toml / CLI | Comma-separated Flash movie names the theme leaves alone. The HUD movie shares the menus' teal and yellow in its speedometer; empty themes every movie. | `src/mc_engine/hooks/ui_theme.cpp` |
| `ui_color_buttons` | bool | `true` | - | toml / CLI | Show the pause menu's color buttons (UI OPTIONS) as the garage's own PickColor: garage.xsf's color button, placed in the pause movie and driven the way the garage's Fillmenu drives it. | `src/mc_engine/ui_palette.cpp` |

### `MCLA/PhotoMode` (3)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `photo_album_keep_new_preview` | bool | `true` | - | toml / CLI | Fix: while choosing the slot for a new photo, the album shows the photo you just took -- in the big picture and pulsing in the highlighted slot -- instead of the last saved photo the cursor passed over. The game decodes every photo into one shared preview image, and in the slot picker it let the slots overwrite the new one. | `src/mc_engine/hooks/photo_album.cpp` |
| `photo_auto_readback` | bool | `true` | - | toml / CLI | Fix: photo mode previews show an old frame or garbage. The game takes its picture on the CPU, by locking the front buffer and JPEG-encoding it, so it needs the GPU resolve copied back to guest memory. | `src/mc_engine/hooks/photo_mode.cpp` |
| `photo_readback_debug` | bool | `false` | - | toml / CLI | Diagnostics for photo_auto_readback: logs the photo album state machine's phase transitions and the front buffer addresses the readback is armed for. Pair with the GPU-side gpu_log_resolve_readback_misses to see whether the resolve that fills the front buffer actually lands in the armed range. | `src/mc_engine/hooks/photo_mode.cpp` |

## Gameplay and world

### `MCLA/World` (17)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `warp_spot` | string | `""` | - | toml / CLI | Where F9 drops the car: "x y z heading" (heading in degrees, 0 = facing +Z). Empty = the first point a mod names in `models/<mod>/warps.txt.` The car is dropped onto whatever collision is below the point. | `src/mc_engine/map_warp.cpp` |
| `start_garage_last` | string | `hollywood` | - | toml / CLI | The garage nearest the car the last time it was driven (hollywood, beaches or sc); start_garage = last starts the session there. The game keeps it up to date. | `src/mc_engine/map_warp.cpp` |
| `start_garage` | string | `hollywood` | - | menu: EXTRAS > START GARAGE (label shows RESTART); steps: hollywood, beaches, sc (SOUTH CENTRAL), last (LAST USED); toml / CLI | Which district garage the car starts the session at: hollywood (what the game always does), beaches, sc, or last (the garage nearest the car when the game was closed). The car is moved there once, when it first appears at Hollywood's door. | `src/mc_engine/map_warp.cpp` |
| `warp` | command | n/a | - | toml / CLI | Warp the car: 'warp `<point>'` (a mod's warps.txt), 'warp back' (to where it was), 'warp x y z [heading]', or 'warp' to list the points. The car is dropped onto whatever collision is below the point. | `src/mc_engine/map_warp.cpp` |
| `mod_breakables` | bool | `true` | - | toml / CLI | Props a mod baked into the city sectors break off when the car hits them fast enough (`models/<mod>/breakables.bin`); a slow bump still stops the car. | `src/mc_engine/modloader/features/mod_breakables.cpp` |
| `mod_breakable_speed_scale` | double | `1.0` | - | toml / CLI | Multiplier on the speed a mod's breakable prop needs to break. | `src/mc_engine/modloader/features/mod_breakables.cpp` |
| `mod_glows` | bool | `true` | - | toml / CLI | Light glows a mod places in the world (`models/<mod>/glows.bin`), drawn by MCLA's glow renderer. | `src/mc_engine/modloader/features/mod_glows.cpp` |
| `mod_glows_max` | int32 | `64` | - | toml / CLI | Mod glows submitted per frame, nearest first. MCLA draws 96 a frame in all, car lights included. | `src/mc_engine/modloader/features/mod_glows.cpp` |
| `mod_glow_size` | double | `1.8` | - | toml / CLI | Mod glow size multiplier (on top of each glow's own). | `src/mc_engine/modloader/features/mod_glows.cpp` |
| `mod_glow_intensity` | double | `1.0` | - | toml / CLI | Mod glow intensity multiplier. | `src/mc_engine/modloader/features/mod_glows.cpp` |
| `mod_lights` | bool | `true` | - | toml / CLI | Dynamic lights a mod adds to its part of the map (`models/<mod>/lights.bin`): its lamps light the ground and the cars at night, on a light grid of its own. Built at city load. | `src/mc_engine/modloader/features/mod_lights.cpp` |
| `mod_light_intensity` | double | `1.0` | - | toml / CLI | Mod light intensity multiplier, applied when the mod's light grid is built (city load). | `src/mc_engine/modloader/features/mod_lights.cpp` |
| `time_of_day_hold` | bool | `false` | - | menu: GAMEPLAY > TIME OF DAY > HOLD TIME; toml / CLI | Freeze the world at time_of_day instead of letting the day cycle run. This is the on/off switch — the hour by itself changes nothing while this is off, so you can park a value and arm it later. | `src/mc_engine/time_of_day.cpp` |
| `time_of_day` | double | `12.0` | 0.0 to 24.0 | menu: GAMEPLAY > TIME OF DAY > TIME; steps: 0.0, 3.0, 6.0, 9.0, 12.0, 14.0, 16.75, 17.4, 19.0, 21.0, 23.5; toml / CLI | Hour to hold the world at (0-24, fractional), applied while time_of_day_hold is on. Takes effect every frame, so dragging the bar walks the sun across the sky live. Menu presets: 6.0 sunrise, 16.75 afternoon, 17.4 sunset, 23.5 night. | `src/mc_engine/time_of_day.cpp` |
| `time_of_day_realtime` | bool | `false` | - | menu: GAMEPLAY > TIME OF DAY > REAL CLOCK; toml / CLI | Follow your computer's clock: the world sits at the real local hour and keeps up with it. Outranks time_of_day_hold. | `src/mc_engine/time_of_day.cpp` |
| `weather` | string | `game` | one of `game`, `real`, `nice`, `cloudy`, `stormy`, `foggy` | menu: GAMEPLAY > TIME OF DAY > WEATHER; steps: game, real (REAL (LIVE)), nice, cloudy, stormy, foggy; toml / CLI | Force the weather. 'game' leaves it alone, including its own random changes; a named sky pins that one and stops the automatic changes. | `src/mc_engine/time_of_day.cpp` |
| `time_of_day_speed` | double | `1.0` | 0.0 to 100.0 | menu: GAMEPLAY > TIME OF DAY > DAY SPEED; steps: 1.0, 2.0, 3.0, 5.0, 10.0; toml / CLI | Multiplier on the day cycle speed. Only applies while time_of_day_hold is off — a held hour does not advance at all. 1 = stock, 0 = frozen where it is, 10 = ten times faster. The stock rate varies with the hour (about 2 minutes per hour at midday, 3 at night). | `src/mc_engine/time_of_day.cpp` |

### `MCLA/Physics` (3)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `smooth_chassis_depth` | bool | `true` | - | menu: RECOMP SETTINGS > SUSPENSION FIX; toml / CLI | Fix: Smooth vehicle chassis suspension and ground depth damping at 60 FPS. | `src/mc_engine/hooks/vehicle.cpp` |
| `smooth_vehicle_physics` | bool | `true` | - | toml / CLI | Fix: Continuous-time tire slip filter at 60+ FPS. | `src/mc_engine/hooks/vehicle.cpp` |
| `smooth_bike_gyro` | bool | `true` | - | toml / CLI | Fix: Step the motorcycle gyro's smoothing in continuous time, so bikes lean and settle the same at any frame rate. | `src/mc_engine/hooks/vehicle.cpp` |

### `MCLA/Difficulty` (8)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `rubberband_dump` | bool | `false` | - | toml / CLI | Write every RubberBandTune entry to `<exe>/rubberband_dump.txt` and flip back off. Read it before picking the scales below. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_catchup_scale` | double | `1.0` | 0.0 to 2.0 | toml / CLI | Scales how hard the computer-controlled opponents pulls when it is BEHIND: MaxThrottle, BehindMaxThrottle, HomeStretchBehindMaxThrottle, PlayerCloseMaxThrottle. Below 1 = less catch-up. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_holdback_scale` | double | `1.0` | 0.0 to 2.0 | toml / CLI | Scales MinThrottle, the throttle cap while the computer-controlled opponents is AHEAD. Below 1 = the leader backs off harder; above 1 = it runs away more freely. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_level` | int32 | `-1` | -1 to 10 | toml / CLI | Force which RubberBandTune level the computer-controlled opponents races at: -1 leaves the game's own choice, 0 = easiest, 10 = hardest. This is the direct way to make races harder — the tune index the game normally derives from your racer level and the race's base. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_distance_scale` | double | `1.0` | 0.1 to 4.0 | toml / CLI | Scales the distance bands the rubberband reacts within: MinDist, MaxDist, BehindMinDist, BehindMaxDist. Larger = the band engages from further away. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_nitro_start_dist` | double | `-1.0` | -1.0 to 8000.0 | toml / CLI | UseNitroMinDistFromStart in world units: how far into the race the computer-controlled opponents must be before it may touch nitrous. Stock is 100 at every difficulty level, which is why they empty the bottle off the line. -1 keeps stock. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_nitro_finish_scale` | double | `1.0` | 0.0 to 8.0 | toml / CLI | Scales UseNitroMinDistFromFinish, the gate tied to the run-in to the finish. | `src/mc_engine/hooks/rubberband.cpp` |
| `rubberband_nitro_behind_scale` | double | `1.0` | 0.0 to 8.0 | toml / CLI | Scales UseNitroMinDistBehindPlayer and UnlimitedNitroMinDistBehindPlayer — how far behind you the computer-controlled opponents has to be before it spends nitrous, and before it gets unlimited. | `src/mc_engine/hooks/rubberband.cpp` |

### `MCLA/Fun` (6)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `squash_stretch` | bool | `false` | - | toml / CLI | Squash & stretch the player's car like the "Why's This Dealer?" meme. Visual only — physics and collision are untouched. | `src/mc_engine/squash_stretch.cpp` |
| `squash_amplitude` | double | `0.25` | - | toml / CLI | Squash depth, 0..0.9. 0.25 = the body loses a quarter of its height at the bottom of the cycle and gains half of that in width. | `src/mc_engine/squash_stretch.cpp` |
| `squash_hz` | double | `1.5` | - | toml / CLI | Oscillations per second. | `src/mc_engine/squash_stretch.cpp` |
| `squash_pivot` | double | `0.0` | - | toml / CLI | Pivot height in metres along the up axis, in car space. 0 = scale about the car origin (the body sinks and rises). Negative values push the pivot down toward the tyre contact patch so the car keeps its ground contact. | `src/mc_engine/squash_stretch.cpp` |
| `squash_up_axis` | int32 | `2` | - | toml / CLI | Which car-space axis is up: 0 = X, 1 = Y, 2 = Z (RAGE default). The other two axes take the anti-phase widening. | `src/mc_engine/squash_stretch.cpp` |
| `squash_traffic` | bool | `false` | - | toml / CLI | Apply the squash to every car in the world instead of just the player's. | `src/mc_engine/squash_stretch.cpp` |

### `MCLA/Garage` (6)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `extra_vinyl_layers` | bool | `false` | - | menu: EXTRAS > EXTRA VINYL LAYERS (label shows RESTART); toml / CLI | Raise the front/rear BUMPER vinyl caps from 16 to 31 layers each (64/64/64/31/31). Top and side caps stay at 64 because MCLA's vinyl composite pipeline hard-caps a single surface at 64 layers (fixed-64 work-area buffers; >64 overflows and crashes). Needs a restart. | `src/mc_engine/hooks/vinyl.cpp` |
| `export_vinyl` | bool | `false` | - | toml / CLI | Export the current car's vinyl layers to a .vgp file in &lt;exe dir&gt;/vinyls/ (auto-named `vinyl_<car>_<timestamp>.vgp`). Toggle ON while in the garage with a car loaded; it fires once and flips back OFF. Share the file; import later. | `src/mc_engine/hooks/vinyl.cpp` |
| `import_vinyl` | string | `""` | - | toml / CLI | Import a .vgp vinyl package onto the current car. Type the file name (found in &lt;exe dir&gt;/vinyls/, with or without the .vgp extension) and press Enter while in the garage with a car loaded. It overwrites the car's current vinyl layers and re-composites, then clears the field. | `src/mc_engine/hooks/vinyl.cpp` |
| `vinyl_auto_readback` | bool | `true` | - | toml / CLI | Fix: car vinyls only load outside the Vinyl Editor when GPU readback is on (the game builds the decal texture on the CPU from a GPU composite). | `src/mc_engine/hooks/vinyl.cpp` |
| `dump_vinyl_shapes` | bool | `false` | - | toml / CLI | Dump the vinyl shape catalog (every ShapeIdx -> source bitmap filename + texture header) to &lt;exe dir&gt;/vinyls/vinyl_shapes.json. Enter the Vinyl editor once so the shape library loads, then toggle ON; it fires once and flips back OFF. Used to build the image->vinyl decomposer's brush set. | `src/mc_engine/hooks/vinyl_shapes.cpp` |
| `capture_vinyl_shapes` | bool | `false` | - | toml / CLI | Hands-free: force-load every vinyl shape a few at a time, hash each, and build the full ShapeIdx->hash map in vinyls/vinyl_shape_hashes.txt (+ .json) — no manual browsing. Enter the Vinyl editor (main menu, not a picker grid), then toggle ON; it sweeps all ~894 shapes over a few seconds and logs when done. | `src/mc_engine/hooks/vinyl_shapes.cpp` |

### `MCLA/Cutscenes` (14)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `cutscene_replay` | string | `cut_story_district_champs` | - | menu: EXTRAS > CUTSCENES > CUTSCENE; toml / CLI | Which cutscene the pause menu's CUTSCENES tab replays. Names match the scene's animation pack under $/resources/animation/cineanimpacks. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_play_now` | string | `""` | - | toml / CLI | Set to a cutscene name to play it immediately, without going through the menu. Clears itself once the scene starts. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_force_events` | bool | `false` | - | toml / CLI | Leave it off; it is kept only to reproduce that. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_force_camera` | bool | `false` | - | toml / CLI | It is a no-op when the camera is already running. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_menu_autoclose` | bool | `true` | - | toml / CLI | Turn it off to arm the scene and close the menu by hand instead. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_skip_uses_teardown` | bool | `false` | - | toml / CLI | Make the skip button end a REPLAY through our own teardown instead of the game's skip path. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_keep_movies` | bool | `true` | - | toml / CLI | Clear mgr+316 before the scene tears down, so FinishScene does not run its movie teardown. The pause menu is a Flash movie (PAUSEMOVIE), and reopening it after a replay died on exactly that: a vfunc call through an object whose vtable had been zeroed. mgr+316 is set by SetSceneName, so every replay took that branch. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_probe_log` | bool | `false` | - | toml / CLI | Print the per-frame trace of a replay: which characters stream in, every actor launch with its clip and entity, the camera hand-over, and each cine event as it fires with its name from the game's own table. Off by default because a 74 second scene is a few hundred lines. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_run_script` | string | `""` | - | toml / CLI | Start any script object by name, relative to $/script_obj, on the next frame. Clears itself once the script is mounted. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_setup` | bool | `true` | - | toml / CLI | Run the game's own standalone-cutscene setup script before the scene. It slots four racers and nine character sets, which a built cutscene script assumes. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_bind_coordsys` | bool | `false` | - | toml / CLI | Anchor the scene to the player's car before it plays, through the game's own CineScript_SetGameCoordSysBindingCopyRacer. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_pad_racers` | bool | `true` | - | toml / CLI | Repeat the player into the cutscene's remaining car slots when the world has fewer racers than the scene wants. Wrong on screen -- several cars end up being the same one -- but it is what gets a scene past its 'insufficient number of racers slotted' guard in free roam. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_debug` | bool | `false` | - | toml / CLI | Dump the whole script-thread tree and the scrThread array with each cutscene gate report. Off by default: it is ~200 lines a shot. | `src/mc_engine/cutscene_gallery.cpp` |
| `cutscene_stop` | string | `""` | one of `""`, `kill`, `finish` | toml / CLI | Abort the running cutscene and dump which of mcCineScript's gates never opened. 'kill' raises the script's own kill flag; 'finish' runs the teardown directly, which is what gets the HUD back from a wedged scene. | `src/mc_engine/cutscene_gallery.cpp` |

### `MCLA/Carbon` (1)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `carbon_menu_diag` | bool | `false` | - | toml / CLI | Log the garage menu state tree, per-state field dumps and the activate/command stream while the paint screen is open. | `src/mc_engine/carbon_parts.cpp` |

## Online

### `MCLA/Multiplayer` (14)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `leaderboards_enabled` | bool | `true` | - | toml / CLI | Leaderboards: serve the Xbox LIVE stats boards (race wins, battle modes, race times) from our own backend, shared through the Rate My Ride server. Off = stock behaviour (empty boards). | `src/mc_engine/online/leaderboards/lb_client.cpp` |
| `leaderboards_diag` | bool | `false` | - | toml / CLI | Log every leaderboard read and write in detail. | `src/mc_engine/online/leaderboards/lb_client.cpp` |
| `online_race_prop_guard` | bool | `true` | - | toml / CLI | Skip the physics update of a prop whose owner is broken instead of crashing. Starting an online race re-syncs every race prop. | `src/mc_engine/online/race_props.cpp` |
| `online_race_prop_watch` | string | `""` | - | toml / CLI | Diagnostic: guest address (hex) of a prop inst. Its owner's inst word is polled every frame and every change is logged. Empty = off. | `src/mc_engine/online/race_props.cpp` |
| `rmr_enabled` | bool | `true` | - | toml / CLI | Rate My Ride: serve the mode from our own backend instead of Rockstar's dead server. Off = stock behaviour ("server not available"). | `src/mc_engine/online/rate_my_ride/rmr_client.cpp` |
| `rmr_server` | string | `""` | - | toml / CLI | Rate My Ride server as host[:port]. Empty = the online relay host (mcla_online_server) when online is enabled, otherwise a local store shared by the profiles on this PC. "local" forces the local store. | `src/mc_engine/online/rate_my_ride/rmr_client.cpp` |
| `rmr_server_port` | int32 | `rmr::kDefaultPort` | - | toml / CLI | TCP port of the Rate My Ride server when it runs on the relay host (default 31002). A port typed into rmr_server overrides it. | `src/mc_engine/online/rate_my_ride/rmr_client.cpp` |
| `rmr_diag` | bool | `false` | - | toml / CLI | Log every Rate My Ride request and record in detail. | `src/mc_engine/online/rate_my_ride/rmr_client.cpp` |
| `online_master_server` | string | `26.24.254.19:31003` | - | toml / CLI | Server list for the server browser (F6): host or host:port of a larecomp_master_server (port 31003 if omitted). Empty = no list, only server_browser_servers and the current server. | `src/mc_engine/online/server_browser/browser_ui.cpp` |
| `server_browser_servers` | string | `""` | - | toml / CLI | Your own servers for the server browser, comma-separated host:port. | `src/mc_engine/online/server_browser/browser_ui.cpp` |
| `server_browser_show_icons` | bool | `true` | - | toml / CLI | Server browser: show each server's icon. | `src/mc_engine/online/server_browser/browser_ui.cpp` |
| `server_browser_pending` | string | `""` | - | toml / CLI | Set by the server browser when a room needs another server: the room to offer right after the restart (address\|vip\|label). Cleared once used. | `src/mc_engine/online/server_browser/browser_ui.cpp` |
| `online_ignore_content_check` | bool | `true` | - | toml / CLI | Ignore the session DLC/content compatibility check when joining or hosting online. The check is symmetric and fails on any mask difference, so it wrongly demands DLC (South Central) even when nobody owns any. Turn off to restore the stock behaviour. | `src/mc_engine/online/system_link.cpp` |
| `online_diag` | bool | `false` | - | toml / CLI | Log online session content masks and the valid-vehicle count at the join content check. Used to confirm whether the host's session properties transported. Verbose; leave off for normal play. | `src/mc_engine/online/system_link.cpp` |

### `MCLA/Online` (1)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `net_lift_big_session_limits` | bool | `false` | - | menu: EXTRAS > NO 8 PLAYER LIMITS; toml / CLI | Sessions of more than 8 players keep damage, debris, world props and the default session settings that MCLA turns off above 8. Applies to the next session you host. | `src/mc_engine/online/session_limits.cpp` |

### `Discord` (5)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `larecomp_discord_rpc` | bool | `true` | - | toml / CLI | Enable Discord Rich Presence. | `src/discord_rpc/discord_rpc.cpp` |
| `larecomp_discord_rpc_log_state` | bool | `true` | - | toml / CLI | Log Discord RPC state changes with printf. | `src/discord_rpc/discord_rpc.cpp` |
| `larecomp_discord_large_image` | string | `mcla_logo` | - | toml / CLI | Discord RPC large image key. | `src/discord_rpc/discord_rpc.cpp` |
| `larecomp_discord_rpc_language` | string | `auto` | one of `auto`, `en`, `pt`, `es` | toml / CLI | Rich Presence language. 'auto' follows the game (user_language); or force en / pt / es. Discord itself does not translate presence text. | `src/discord_rpc/discord_rpc.cpp` |
| `larecomp_discord_activity` | bool | `true` | - | toml / CLI | Enable Discord Rich Presence activity | `src/discord_rpc/rpc_client.cpp` |

## Mods

### `MCLA/Mods` (45)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `model_mods` | bool | `true` | - | toml / CLI | Load model replacements from `<exe>/modloader/<mod>/<asset>.obj` (or mods/, models/). Each .obj is baked into a native drawable and packed into xarchive_mods.rpf, which is appended to the archive list -- the engine's own last-mounted-wins rule makes it override per file. Game archives are never modified. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_bone` | int32 | `-1` | - | toml / CLI | -1 keeps a skinned mesh's own weights (glTF with JOINTS_0/WEIGHTS_0), matching its joints to the driver's bones by bind-pose position, so the model follows the seated pose. Any value >= 0 ignores the weights and rides the whole mesh rigidly on that one bone, which is all a .obj can do. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_offset_y` | double | `0.0` | - | toml / CLI | Extra height for the replacement model, in metres, on top of the automatic lift onto its bone. Positive raises it. Use this if the model still sits below or above the seat -- it takes a restart but no rebuild. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_offset_x` | double | `0.0` | - | toml / CLI | Sideways nudge for the replacement model, in metres. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_offset_z` | double | `0.0` | - | toml / CLI | Forward/back nudge for the replacement model, in metres. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_yaw` | double | `0.0` | - | toml / CLI | Turns the replacement model about the vertical axis, in degrees. Source models rarely face the same way the game does -- 180 turns a model that sits with its back to the wheel. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_pitch` | double | `0.0` | - | toml / CLI | Tips the replacement model forward or back, in degrees. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_roll` | double | `0.0` | - | toml / CLI | Rolls the replacement model sideways, in degrees. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_scale` | double | `1.0` | - | toml / CLI | Size multiplier on top of the automatic fit, which matches the model's height to the driver's. 1.0 leaves it alone. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_submeshes` | bool | `true` | - | toml / CLI | Spread the replacement mesh across every submesh of the character instead of the largest one alone. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_decimate` | bool | `false` | - | toml / CLI | Whether a replacement mesh larger than the slot it has to live in may be welded down to fit: a character, a rim, or a part replaced on a shipped car. A car built on a donor is always welded to its ceilings (model_mods_car_budget for the body) and ignores this. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_proportions` | double | `0.0` | - | toml / CLI | How much of the replacement model's own build to keep when it is reposed onto the driver's skeleton, 0 to 1. At 0 every joint lands exactly on the bone it was matched to, which is what the game's skinning expects and what a human-shaped model wants. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_textures` | bool | `true` | - | toml / CLI | Use the replacement model's own textures. The images embedded in a .glb (one per material) are packed into a single atlas, each primitive's UVs are remapped into its own cell, and the result is written over the character's diffuse map in place -- same address, same size, same format. Off leaves the mesh wearing the original driver's skin. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_textures` | bool | `true` | - | toml / CLI | Give a replacement car body its own textures. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_autoalign` | bool | `true` | - | toml / CLI | Put a replacement wheel on the game's axle, in two steps. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_keep_badge` | bool | `true` | - | toml / CLI | Leave the wheel's badge quad as it shipped instead of silencing it. It is nine vertices on its own material, and that material is the only one on the wheel that samples a texture -- so silencing it, which is what the geometry rewrite did to every unused submesh, takes the badge off the car and leaves a mod's paint read by nothing. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_shade_profile` | bool | `true` | - | toml / CLI | Rebuild a replaced wheel's baked shading from the template instead of flooding it with one average. A shipped wheel carries its ambient occlusion per vertex, and it is not a small effect: measured across five of them the lane runs the full 0..255 with a deviation above fifty. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_texture_mode` | int32 | `-1` | - | toml / CLI | Where a wheel mod's own image is applied: -1 auto, 0 the hub cap, 1 the whole rim. A shipped wheel keeps no skin. Its metal is the shader and the baked per-vertex shade, and its texture is a decal sheet -- the maker's logo plus a few lug nuts and valve stems, most of it transparent -- sampled by a nine-vertex quad sitting on the hub. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_band_profile` | bool | `false` | - | toml / CLI | Carry the template wheel's material bands onto the replacement instead of flooding one. texcoord1.x is the rim's material selector: xRimMain's VS_Common indexes materialHookups and tintColors with it, and PS_Multi then reflects a dual-paraboloid environment map through whatever material came out. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_shade_occlusion` | bool | `true` | - | toml / CLI | Bake the wheel's shade lane from occlusion measured on the replacement. A shipped wheel carries a baked lane: measured on three of them it runs 0..252 with a deviation near fifty (bbs_ch 0..252 mean 155 dev 52, amer_razor 5..254 mean 186 dev 45, 5zigen_5zr 0..249 mean 144 dev 55). Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_inherit_shade` | bool | `false` | - | toml / CLI | Diagnostic. Copy the shipped wheel's per-vertex shading onto the replacement, vertex by vertex from whichever original vertex is nearest. This is what a character replacement does, and it works there because the mesh has been reposed onto the very skeleton it replaces. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_offset_x` | double | `0.0` | - | toml / CLI | Nudge for a replacement wheel along its axle, where one unit is the wheel's own diameter. Positive pushes it further into the tyre, negative pulls it out. Only needed when the automatic flush above lands slightly off for a particular model. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_yaw` | double | `0.0` | - | toml / CLI | Turns a replacement wheel about the vertical axis, in degrees. Separate from the character angles because a wheel and a driver never want the same correction. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_pitch` | double | `0.0` | - | toml / CLI | Tips a replacement wheel forward or back, in degrees. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_roll` | double | `0.0` | - | toml / CLI | Rolls a replacement wheel sideways, in degrees. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_rim_scale` | double | `1.0` | - | toml / CLI | Size multiplier for a replacement wheel, on top of the automatic fit to the shipped wheel's own bounds. 1.0 leaves it alone. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_yaw` | double | `180.0` | - | toml / CLI | Turns a replacement car about the vertical axis before its parts are placed, in degrees. 180 is the normal value and the default: glTF has a model face +Z and MCLA has it face -Z, which is checked rather than assumed -- the shipped cars keep their headlights at negative Z and their tail lights at positive Z, and an exported car has it the other way round. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_scale` | double | `1.0` | - | toml / CLI | Size multiplier for a replacement car, on top of the automatic fit. The fit matches the length of the mod's body to the length of the body it replaces, and every part of the car is then moved by that one factor -- scaling each part into its own slot instead would leave a hood a size larger than the doors beside it. 1.0 leaves it alone. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_grow` | bool | `false` | - | toml / CLI | Give a replacement wheel or car part buffers of its own instead of welding it down into the ones the shipped resource came with. The resource is made longer and the submesh is pointed at the new space; nothing already in it moves. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_budget` | uint32 | `16777216` | - | toml / CLI | The largest a replaced car body may be grown to, in bytes for LOD 0; the lower LODs get a quarter of it. Anything the mod brings beyond what fits is welded away, whatever model_mods_decimate says. It is a ceiling, not a size: a body that needs less is grown only to what it needs. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_pack_write` | bool | `true` | - | toml / CLI | Write the mod's atlases into the car's material packs. A probe, not a feature. model_mods_car_textures gates TWO things at once: the atlas, which rewrites the mesh's UVs, and the pack rewrite, which unpacks .xtl/.xtp, drops the atlases in and repacks them through BuildRsc5File instead of shipping the donor's bytes as they came. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_share_images` | bool | `false` | - | toml / CLI | Let two materials that name the same picture share one cell of the atlas. A textures/ folder is keyed by material name, so a car whose three lamp materials all draw 750i_taillight ships three identical files and they take three cells. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_cabin_light` | bool | `true` | - | toml / CLI | Give a replacement car's cabin (the car-space part slots, interior0) the donor's per-vertex interior lighting, read off the donor's cabin vertex nearest each new one, instead of one flooded average. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_lod1_copy` | bool | `true` | - | toml / CLI | Ship a replacement car's LOD 1 as a byte copy of the LOD 0 just built, for the body and for every part slot the donor gives a LOD 1. The garage draws LOD 1 close up. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_part_probe` | bool | `true` | - | toml / CLI | TEMPORARY diagnostic: every second, while the player's car is waiting to reattach its parts (car+6467) -- which is when the game draws none of it, wheels included -- log which selected part is not resident yet and the state of its stream (object, resource, handle, flags, state, refcount). See PartProbeLoop. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_glass_band` | bool | `true` | - | toml / CLI | Give a replacement body's glass the pane number of the donor glass nearest each piece of it, instead of the dominant pane of the submesh it is written into. texcoord1.x on CarGlass says which pane a vertex is -- 0 windscreen, 1 rear, 2/3 doors -- and the window film tints 1-3 only, so a body flooded with 0 takes no film at all. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_part_growth` | uint32 | `8` | - | toml / CLI | How much larger than the donor's own a replacement PART may be, as a multiple of the shipped resource. A part is not a body and does not need the body's machinery: the Impala's front bumper is 1731 vertices where its shell is three thousand-odd, and the whole of a car's sixty parts together weigh less than one of its LODs. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_verbatim` | bool | `false` | - | toml / CLI | Diagnostic, cars only. Copies each part's shipped file into the mod archive exactly as it lies in the game's own -- same bytes, same compressed length, same flag -- instead of decompressing it and packing it back. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_passthrough` | bool | `false` | - | toml / CLI | Diagnostic. Repacks the original character resource without touching its geometry, so the .obj is ignored. If the driver still renders correctly with this on, the packing path (LZX, archive, mount, flags) is sound and any breakage belongs to the mesh rewrite; if it breaks, the packing path is at fault. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_anchor_bones` | bool | `true` | - | toml / CLI | Whether the repose plants each of the mod's joints on the bone it was matched to. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_weight_smoothing` | int32 | `4` | - | toml / CLI | How many rounds of neighbour averaging the mod's skin weights get before the model is reposed onto the driver's skeleton. 0 keeps them exactly as authored. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_grow_slack` | uint32 | `65536` | - | toml / CLI | Room left unused at the end of a resource `model_mods_grow` has enlarged. The tail of a grown segment does not arrive intact, and this is what keeps the mesh out of it -- measured on a wheel: buffers ending 16 KB short of the segment's end came back with holes, and ending 82 KB short came back clean. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_car_space_only` | bool | `true` | - | toml / CLI | Write a donor car's replacement mesh only into the models the donor authors in the CAR's frame, and silence the pass's submeshes in the others. A vehicle body is several models and the game draws each with its own part matrix -- a bumper, a lamp, a door window. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_grow_probe` | int32 | `0` | - | toml / CLI | With `model_mods_grow` on: 1 grows the resource and stops there, the segment gets bigger and nothing else changes -- no buffer moves, no pointer is rewritten, no count is touched, and the mesh is decimated into the buffers the template already owned. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |
| `model_mods_diag` | bool | `false` | - | toml / CLI | Write a `.diag` folder beside the mods: one .txt per rebuilt asset holding the template's submesh table, both rigs with the joint-to-bone mapping, and how the mesh was dealt across the submeshes -- plus the mesh itself as .obj before and after the retarget. Needs a restart. | `src/mc_engine/modloader/modloader.cpp` |

## Startup, saves and text

### `MCLA` (3)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `mcla_controller_toast` | bool | `true` | - | toml / CLI | Show the MCLA-styled toast when a controller connects or disconnects. Its corner follows controller_toast_position. | `src/mc_engine/ui/controller_toast.cpp` |
| `profile_auto_select` | bool | `false` | - | toml / CLI | Start with the save named by user_name and never show the save picker, even when several saves exist. The picker's 'Always use this save' box sets it; hold Shift while the game starts to see the picker once anyway. | `src/profile/larecomp_profile_picker.cpp` |
| `skip_save_import` | bool | `false` | - | toml / CLI | Skip the save import wizard shown on first launch and start with no save. The wizard is skipped automatically anyway when no Xenia or RPCS3 save is found on the machine. | `src/saveporter/larecomp_save_porter.cpp` |

### `MCLA/Settings` (2)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `rexglue_settings_in_gameoptions` | bool | `false` | - | toml / CLI | Replace Game Options with ReXGlue Settings overlay (F4) | `src/mc_engine/hooks/ui.cpp` |
| `show_test_movie` | bool | `false` | - | toml / CLI | Show the TestMovie debug button in the pause menu | `src/mc_engine/pause_menu.cpp` |

### `MCLA/Localization` (1)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `language` | string | `auto` | one of `auto`, `en`, `es`, `fr`, `de`, `it`, `pt` | menu: INTERFACE > LANGUAGES > LANGUAGE; steps: auto, en (ENGLISH), es (ESPANOL), fr (FRANCAIS), de (DEUTSCH), it (ITALIANO), pt (PORTUGUES); toml / CLI | UI text language. Japanese is not offered. Measured: hard crash. pt is the empty eleventh slot every shipped .strtbl carries but never fills. It stays blank -- and therefore English -- unless a mod archive supplies files that fill it. Switching in-game reloads the table live. | `src/mc_engine/hooks/ui.cpp` |

### `System/String Replacement` (4)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `string_dump_enabled` | bool | `false` | - | toml / CLI | Capture encountered strings to TOML for translation/modding | `src/mc_engine/string_dumper.cpp` |
| `string_replace_enabled` | bool | `false` | - | toml / CLI | Inject replacement strings from disk when available | `src/mc_engine/string_dumper.cpp` |
| `string_folder` | string | `""` | - | toml / CLI | Override the strings folder (empty = <game data>/strings) Needs a restart. | `src/mc_engine/string_dumper.cpp` |
| `string_language` | string | `""` | - | toml / CLI | Language code for string replacements (e.g. pt_BR, es_ES) | `src/mc_engine/string_dumper.cpp` |

## Debug and diagnostics

### `MCLA/Debug` (3)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `lzx_stats` | bool | `false` | - | toml / CLI | Measure pgStreamer LZX decompression (XMemDecompressStream): per-2s window stats appended to `<exe>/lzx_stats.txt.` For diagnosing streaming stutter (South Central). | `src/mc_engine/hooks/diagnostics.cpp` |
| `mcla_gfx_probe` | bool | `false` | - | toml / CLI | Milestone 0: passive D3D9-Xenon probe. Renders nothing, changes nothing. Writes mcla_gfx_probe_*.csv next to the executable. Needs a restart. | `src/native_probe/mcla_gfx_probe.cpp` |
| `mcla_gfx_probe_frames` | int32 | `900` | 0 to 100000 | toml / CLI | Milestone 0: number of guest frames to capture before the probe dumps its report and goes idle (0 = never stop). | `src/native_probe/mcla_gfx_probe.cpp` |

### `MCLA/Diag` (4)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `guest_profile` | bool | `false` | - | toml / CLI | Sample the busiest threads, and photograph every thread when the game stops. The same thing MCLA_PROFILE=1 turns on, reachable from the config file so a session need not be launched from a shell to get it. Needs a restart. | `src/mc_engine/guest_profiler.cpp` |
| `stream_trace` | string | `""` | - | toml / CLI | Log every resource the streamer is asked for whose path contains this text, and whether the file was found. That pair is the only thing that separates "the game never asked for it" from "the game asked and the archive did not have it", which is the question a car that loads forever comes down to. Empty turns it off. | `src/mc_engine/hooks/diagnostics.cpp` |
| `segment_trace` | string | `""` | - | toml / CLI | Dump the segment table of every resource whose file name contains this text, as the streamer allocates it. So a buffer that starts in one segment and runs past its end is read as if the next segment followed it, and from the boundary on the GPU fetches whatever the heap put there -- a mesh that is perfect on disk and blades on screen. | `src/mc_engine/hooks/diagnostics.cpp` |
| `inflate_trace` | bool | `false` | - | toml / CLI | Log every call into zlibInflater's decode step, for the first few hundred. Both of those follow from one number -- how much input the decoder actually consumed on a call -- and this prints it. | `src/mc_engine/hooks/diagnostics.cpp` |

### `MCLA/Diagnostics` (5)

| Name | Type | Default | Range / values | Where | Effect | Source |
|---|---|---|---|---|---|---|
| `tune_field_probe` | bool | `false` | - | toml / CLI | Log every tune field name as it registers. | `src/mc_engine/hooks/diagnostics.cpp` |
| `photo_ui_trace` | bool | `true` | - | toml / CLI | Log the UI statechart while the photo album scene is up (photo mode, the album, the Rate My Ride snapshots): events posted and dispatched, delayed events, the UI.* and photoAlbum.* commands with the states they touch, and the paced movies' frame counts every 10 seconds. Silent everywhere else. | `src/mc_engine/hooks/photo_album.cpp` |
| `ui_color_log` | bool | `false` | - | toml / CLI | Log every distinct Flash UI color (movie, draw site, ARGB, bound texture) with a running count, to measure the families ui_theme_* recolors. | `src/mc_engine/hooks/ui_theme.cpp` |
| `warp_transition_trace` | bool | `false` | - | toml / CLI | Log every frame the warper spends waiting on a cinematic warp transition: the transition's time, duration, active flag and the frame delta. | `src/mc_engine/hooks/warper.cpp` |
| `warp_transition_test_miss` | bool | `false` | - | toml / CLI | Testing aid: hide the running transition from the warper's own end check, so every cinematic warp ends the way the stuck ones do -- the transition finishes and reports it while the warper is not looking. With warp_transition_fix off this reproduces the stuck garage facade on demand. | `src/mc_engine/hooks/warper.cpp` |
