# Running and debugging LARecomp

Setup and build instructions are in the [README](../README.md). This file covers running the game and reading what it writes.

On Linux and the Steam Deck a game installed with the launcher needs no particular C++ library: the launcher's build kit brings its own (libc++). A build made with the system's compiler (by hand, or by the launcher where its kit cannot start) needs libstdc++ 13 or newer (SteamOS 3.5+, Ubuntu 24.04+, Fedora 39+, Arch); without it that build does not start and the terminal shows `version 'GLIBCXX_3.4.xx' not found`. Details in the README's [Linux and the Steam Deck](../README.md#linux-and-the-steam-deck).

---

## Quick start

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; Start-Process larecomp.exe
```

**Set `REX_LOG_LEVEL=warn`.** Non-Release builds default to `trace`, which is about 7,500 log lines and 1.4 MB of synchronous disk writes per second during gameplay. That is enough to cause stutter by itself.

Launching from Visual Studio does not set it, so those runs log at trace level.

On first launch, an ISO install wizard appears if the game data is missing, and there is an option to import saves from Xenia or RPCS3.

### If the frame rate collapses after a long session

Add `--clock_no_scaling=true`:

```powershell
$env:REX_LOG_LEVEL="warn"; Start-Process larecomp.exe -ArgumentList "--clock_no_scaling=true"
```

This works around an unsigned underflow in the SDK's vblank timer that dispatches guest interrupts continuously once triggered. A 33 minute test run with the flag never dropped below 24 FPS; the run before it collapsed to 6 FPS after 715 seconds. Details in [`TECHNICAL_NOTES.md`](TECHNICAL_NOTES.md#4-the-long-session-frame-rate-collapse).

---

## Settings

Defaults out of the box are `real_frame_delta = true`, `fps_limit = 60`, `vsync = false`.

Set anything from the pause menu, on the command line as `--name=value`, or in `larecomp.toml` next to the executable. Menu changes apply immediately and save when you leave the submenu. A flag the game cannot use (a typo, an old name, a setting of a graphics or input driver this run does not load, a value the setting refuses) does not stop it: the game starts and the log says `[cli] --name=value ignored: ` and why.

The pause menu holds the settings a player is meant to change. The troubleshooting and developer switches are not in it: they are launch arguments, rows of the launcher's Advanced tab and Troubleshooting section, and, for the ones that work while the game runs, pages of the hidden DEBUG menu (below).

Two settings sound similar and are not (REAL FRAME DELTA is on the DEBUG menu's FIXES page, not in the normal menus):

| Setting | What it does |
|---|---|
| **REAL FRAME DELTA** (`real_frame_delta`) | Feeds the simulation the measured frame time instead of the fixed 30 Hz timestep, and unlocks presentation from every-other-vblank. This is what makes physics, camera and traffic correct above 30 FPS. It is not a frame rate cap. |
| **FPS LIMIT** (`fps_limit`) | The cap itself: 30, 60, 120, 144 or UNCAPPED. A wall-clock limiter, so frame times stay evenly spaced. |

Turning REAL FRAME DELTA off puts the engine back on its original 30 Hz timestep. The hitch clamp, the FPS cap and the city LOD scale keep working either way; earlier builds gated all three behind it, so switching it off silently disabled the cap and the LOD slider too.

### Pause menu

Press Start or Escape and open the **SETTINGS** tab. Buttons, and what each holds:

| Button | Contents |
|---|---|
| RECOMP SETTINGS | Hide mouse pointer, original intro speed, skip intro videos (both intro rows take hold at the next start) |
| GRAPHICS | **PRESET** (NATIVE) applies VANILLA, ENHANCED or ENHANCED RT to the four effect pages. VANILLA is the first-start default. Editing a covered setting switches to CUSTOM. ENHANCED RT is offered where the device can trace rays. **DISPLAY** includes resolution, aspect ratio, HDR, gamma, frame limit, latency reduction, anti-aliasing, upscaling, the vendor denoiser and frame generation where available. **IMAGE QUALITY** includes texture filtering, depth of field and motion blur. **LIGHTING AND SHADOWS** includes sun, contact, foliage and car shadows, ambient occlusion, bounce light, sun shafts and local lighting. **REFLECTIONS** includes road, surface and car reflections and side mirrors. **WORLD DETAIL** includes draw distance, traffic and city LOD, pedestrians and parked cars. Each effect level uses the same rendering parameters on supported GPUs. |
| GAMEPLAY | **CAMERA**: CHASE CAMERA (smoothing factor, camera shake), FOV, REAR VIEW (mirror, position, frame, cars in the mirror), LOOK AROUND (free look). **TIME OF DAY**: real clock, hold time, time, day speed, weather. **CARBON FIBER**: per-car carbon part toggles. **DIFFICULTY**: AI difficulty and the rubberband scales. **MAP MOUSE**: mouse on the full map, scroll-wheel zoom, hide the pointer, cursor magnet |
| CONTROLLER | Vibration, DualSense and Xbox trigger effects (DualSense throttle and brake strength included), stick dead zones, button layout |
| INTERFACE | **UI**: menu colors, speed units, HUD width. **LANGUAGES** |
| EXTRAS | Cutscenes, texture mods (NATIVE only), extra vinyl layers, no 8 player limits, start garage |
| QUIT | Quit or restart |

Not in these menus: GRAPHICS API (the launcher's Graphics API row sets it; `--graphics_backend=auto|d3d12|vulkan` works on the command line), the renderer, and the developer switches (below).

**Ray denoising.** The vendor switch in GRAPHICS > DISPLAY defaults to OFF, which uses LARecomp's denoisers. ON uses DLSS Ray Reconstruction on RTX with DLSS upscaling, or AMD FSR Ray Regeneration on RX 9000 with Windows 11 and Direct3D 12. The game reads an older DLSS RR or FSR RAY REGEN choice as ON and an older AUTO or OURS choice as OFF. A saved value of the new toggle takes precedence.

**The compatibility renderer.** The game runs on its own (NATIVE) renderer, and the launcher and these menus offer no other. For a PC where it will not start, the program keeps a second one (the XENIA renderer, the old Xenia GPU emulation), which runs on more graphics cards (Direct3D 12 with shader model 5.1 or binding tier 1, where the native renderer asks for 6.0 and tier 2) and has fewer options: no ambient occlusion, HDR, upscalers or frame generation, and a lower frame rate. It is hidden on purpose. The box that says the game could not start on this PC's graphics card has a button, **Use the compatibility renderer**, which saves the choice and starts the game again; the shader build's failure dialog has the same button, and after two starts in a row that never got going the next start offers it. By hand: put `gpu.driver = "d3d12"` (`"vulkan"` on Linux) in `larecomp.toml`, or start with `--gpu.driver=d3d12`; `gpu.driver = "larecomp"` is the native renderer again. With the debug menu unlocked, RENDERING > RENDERER does the same from inside the game (NATIVE, XENIA D3D12, XENIA VULKAN; it applies at the next start, and leaving the menu offers RESTART NOW). A `--gpu.driver` on the command line or `REX_GPU_DRIVER` in the environment beats the file, and then the row is grey.

**DEBUG menu.** Start with `--debug_menu` (or `REX_DEBUG_MENU=1`, or `debug_menu = true` in `larecomp.toml`) and the SETTINGS tab gets a DEBUG button before QUIT. Its pages: FIXES, PERFORMANCE, RENDERING, CAMERA, TOOLS, LOGGING, INFO. Changes made there last for the session and are never written to `larecomp.toml`. Switches that are only read at start (tree impostors, screen blur, the diagnostics) are launch arguments, for example `larecomp.exe --perf_no_impostors=true`.

**Function keys.** They work for everyone, with or without the debug menu: **F2** dumps the textures of the next frame (Plume renderer), **F3** shows the telemetry card, **F4** opens the settings panel, **F9** warps the car to a mod's warp point and back, **F10** writes a raw trace of the next frame (Plume renderer), **F11** captures the next frame with RenderDoc. Only the pause menu is cleaned up; these keys are as they always were.

**F4** lists every setting, key bindings included. Settings that apply after a restart are yellow. A setting tagged debug (the ones the debug menu holds) can be changed there and applies for the session, but it is never written to `larecomp.toml`: Ctrl+S says how many it left out. To keep one, use a launch argument or a line in `larecomp.toml`. The denoiser A/B chooser uses launch arguments or the environment for persistent overrides because the old config key is reserved for migration.

### Environment variables

Only these are read. Everything else is a cvar.

| Variable | Default | Purpose |
|---|---|---|
| `REX_LOG_LEVEL` | `trace` on non-Release | Set to `warn`. |
| `MCLA_GAME_DATA` | auto-detect | Folder holding `default.xex`. An invalid path warns and falls back to auto-detection. |
| `MCLA_FPS_CAP` | unset | Overrides the `fps_limit` cvar. |
| `MCLA_MAX_FRAME_MS` | unset | A fixed cap, in ms, on how far one frame can advance the clock; wins over the `frame_step_cap` cvar (default `adaptive`: about 34 ms at 60 FPS, 50 ms at 30 FPS, at most 125 ms). Clamped to 16-1000 ms; the cap cannot be turned off. |
| `MCLA_TIMING_LOG` | off | `1` writes the timing log described below. |
| `MCLA_TEX_SOFT` / `MCLA_TEX_HARD` / `MCLA_TEX_RTT` | `1536` / `2048` / `64` | Texture cache limits in MB. |
| `MCLA_VSYNC` | `false` | On costs about 30% throughput. |
| `MCLA_REFRESH_RATE` | `60` | Guest video mode refresh rate. |
| `MCLA_ALLOW_INVALID_FETCH` | `true` | Set `false` if HUD or minimap glitches appear. |
| `LARECOMP_LOG_FILE` | auto | Log file path. |
| `LARECOMP_NO_CRASH_DIALOG` | unset | `1` skips the "crashed" message box: a crash writes its report to the log and the game ends at once (exit status `0xC4A50001` for an access violation), for test runs nobody watches. |

`REX_LOG_LEVEL` has to be an environment variable. Setting the level from `OnPostInitLogging()` does not work, because the runtime has already printed its startup banner.

---

## What the game writes

| File | Contents |
|---|---|
| `logs/larecomp_NNN.log` | Main log, rolling, up to 10 files of 20 MB. A flag the game cannot use is logged as `[cli] --name=value ignored: ` and why, and the debug settings that are not at their default are listed as `[settings] name = value (command line)`. **Check it first when a setting appears to do nothing.** |
| `logs/timing_<date>_<time>_cap<N>.log` | Frame timing, when `MCLA_TIMING_LOG=1`. |
| `logs/online_tables.json` | The game's own online tables, written once per run a few seconds in (and again after 90 s only if more of the table filled in; a file with the same text is not touched): the stats view ids with their columns (from the title's XDBF resource) and the session property names and values (what a room's GameMode and the like mean). A launcher (or a bug report) reads it to show names without the game running; it holds no player data and nothing is shipped with LARecomp (the game writes it from its own memory). `src/mc_engine/online/online_tables_core.h` documents the format. |
| `larecomp.toml` | Saved settings, written when you leave a pause submenu with a changed value. |
| `logs/join_server.txt` | Only in a run started with `--join_server`: `state=` (waiting, connecting, searching, joining, done, failed), `reason=` and `message=` for a failure, `updated=`. Rewritten on every change; `done` and `failed` are final. |

**Joining a server from the command line.** `larecomp.exe --join_server=host:port` puts the game online on that relay for this run (the same as `--mcla_online_enable=true --mcla_online_server=host:port`, nothing is written to `larecomp.toml`) and, once the city is up and the pause menu is closed, joins by itself: Cruise on that server (a session on it, or one it hosts for the next player). `,race` does the same for a Race session, `,room:<gamertag>` joins the room that player hosts. There is no offer to accept and no time limit but the city loading (20 minutes). The log has `[join_server]` lines for every step, and `logs/join_server.txt` holds the state for the launcher; a failure (server down, room not found or full, a required mod missing) opens the server browser with the sentence in it.

Crash diagnostics go into the main log, not a separate file. The SEH and SIGABRT handler in `crash_handler.cpp` resolves and writes the call stack there.

---

## Launcher contract

The [schemas and examples](contract/) describe the JSON files the game exports for the launcher.

- `machine.json` is written on startup and by `--probe_machine[=<file>]`. `features.frame_generation.dlss_card` applies the game's NVIDIA adapter name rule. It is independent of the graphics API, driver support, installed files, and runtime failures. It is `null` when the adapter cannot be identified. The headless probe can still identify a card by enumeration after a device open fails.
- `api.by_api.d3d12` and `api.by_api.vulkan` each contain `car_rt`, `rt_tier`, and `ray_regen`. The current API repeats `api.car_rt`, `gpu.rt_tier`, and `features.denoiser.ray_regen`. An unopened API leaves device-dependent values `null`. A backend absent from the build reports `false` and `"none"` for its RT facts. Ray Regeneration reports `"unsupported"` for an unopened Vulkan backend or a build without the D3D12 provider.
- `install-manifest.json` provides `launch.game_root` and `launch.debug_menu` as individual arguments. Substitute the game data directory for `{game_root}` and pass the debug argument when the user enables it. `game.size` gives the byte length of the supported `default.xex` identified by `game.xex_sha256`.
- For each enabled setup action, read `actions[].timeout_min` as its wall-clock limit in minutes and `actions[].estimated_disk_mib` as its additional free-space estimate, including temporary files. Driver-managed caches and logs are excluded. Use legacy defaults when either field is absent. For `precache_pipelines`, use the top-level `pipeline_key_settings` list to detect changed saved settings and rerun the action. The game's stamp decides whether to rebuild. Use the legacy key list for manifests that omit the field.
- `cmake/deps.json` groups of the `streamline` kit carry `gpu_vendor` and `gpu_name`, a vendor word and a name pattern for the cards the group is for, so the first setup can choose the Streamline files before the game exists (nothing of the game can run before they are fetched; `when` stays about the PC). A group applies to a graphics adapter of that vendor whose name matches the pattern (searched anywhere, ignoring case). `core` is the cards that use the kit at all (machine.json's `gpu.rtx`); `frame_generation` is read together with `core`, for the same adapter, and is the cards DLSS frame generation is for (`features.frame_generation.dlss_card`). The machine.json of an earlier start, when it describes that adapter, still has the last word. Use the launcher's own name rule for a `deps.json` whose group has no `gpu_name`. A group's `when` and `gpu_vendor` name the same vendor (the tests check it). `larecomp_dlss_card_table_test` and `cmake/tests/install_manifest_test.py` check the patterns against the name tests in the code on `contract/adapter-name-vectors.json`; change the code and the patterns together, with a vector for the new name.
- `settings-ui.json` from `--dump_settings_ui` provides `presets.legacy_file_preset` for a `larecomp.toml` that contains any key in `presets.covers` and omits `presets.setting`. The current value is `"custom"`, which preserves the saved values. Use `presets.first_start` if the file omits both the preset key and all covered keys.

## City data actions (for the launcher's setup)

The ray tracing effects read files built on the player's PC from their own game files, in `<shader pack folder>\city`: the city pack (`mcla_city.v1.pack`), and the hit data beside it, the hit file (`mcla_city_hit.v1.pack`) and the texture file (`mcla_city_tex.v1.pack`) with one stamp (`mcla_city_hit.stamp`). The release ships `larecomp.exe` only, so it builds and checks all of them. Each action runs without a window and ends the process; the game builds the same files itself in the background when they are missing and an effect wants them, so none of this is required.

| Action | Does | Exit code |
|---|---|---|
| `--build_city_pack` | the city pack (and its lamps) | 0 built / up to date / not needed, 1 otherwise |
| `--build_city_hit` | the textures, then the hit file, over the pack in place; "no pack" when there is none | the same |
| `--check_city_hit` | stat only: is the hit data in place and current? | 0 up to date, 1 not current |
| `--check_city_hit --check_city_hit_deep=true` | reads the city pack, the hit file and every picture of the texture file back (seconds) | 0 whole, 1 problems |

`--build_city_hit_force=true` builds even when the stamp is current. Several actions on one command line run in this order and stop at the first failure: `--build_city_pack`, `--build_city_hit`, `--check_city_hit`. The launcher runs one process a step.

Output is `@citypack|` lines on stdout, the same grammar as `--build_shader_packs`' `@shaderpack|` lines (fields separated by `|`, the free text last):

```
@citypack|folder|<where the files go>
@citypack|phase|<steps, 0 = unknown>|<name>
@citypack|step|<done>|<steps>|<item>          at most five a second
@citypack|log|<info|warn|error>|<text>
@citypack|result|<status>|<text>              once, last (once per action when several run)
```

Statuses: `ok` (built), `up to date`, `not needed`, `cancelled`, `failed`, `no archive`, plus `no pack` (the hit data needs the city pack) and `not current` (`--check_city_hit`: missing or built from other files). Phases of the build: `far level of detail`, `lamps`, `city sectors` (the pack); `city textures`, `city texture files`, `far level of detail`, `city hit sectors` (the hit data). Phases of the deep check: `checking the city pack`, `checking the city hit file`, `checking the city textures` (one step each). Ending the process cancels and is safe at any moment: files are written under temporary names and the stamp last. Logs: `logs/city_pack_build.txt`, `logs/city_hit_build.txt`, `logs/city_hit_check.txt`. The line format and the order rules are in `src/bootprogress/city_pack_rules.h`; `tools/citypack_tool` (`build-hit`, `check-hit`, `check-tex`) runs the same code.

---

## Frame timing instrumentation

```powershell
$env:MCLA_TIMING_LOG="1"; Start-Process larecomp.exe
```

Off by default, costing one already-resolved bool test per frame.

Once per second it records:

- Measured against engine frame time. A `ratio` far from 1.00 means the simulation and the wall clock disagree.
- Spike counts at 20, 33, 50 and 100 ms.
- A GPU time breakdown: submit, draw, fence wait, resolve, pipeline creation, texture upload.
- Texture and pipeline cache hits and misses, draw calls, vertices.
- Interrupt counts split into `vblank`, `cpu` and `poison`.
- The engine's accumulated-time totals, with `<-- FROZEN` if they stop advancing.

Every 30 seconds it writes a 1 ms histogram with empty buckets omitted. A healthy 60 FPS run looks like one dominant bucket with thin tails:

```
--- frame-time histogram | window 0.0s..30.0s | 1720 frames | mean 17.44 ms ---
   16 ms |   1625 ################################################
   17 ms |     32 #
```

### Reading it

`substep r24` is a rate-invariance check. It must not change with `fps_limit`.

`dt clamp=[0.0001..0.1000]` is the engine's own delta floor and ceiling. An `engine_dt` pinned at exactly 100.00 ms means frames really are that slow and the ceiling saturated, not that the timer broke.

The `interrupts` split is what diagnoses a frame rate collapse:

| Pattern | Meaning |
|---|---|
| `vblank` in the millions | The SDK vblank underflow. Use `--clock_no_scaling=true`. |
| `cpu` climbing, `poison` above 0 | Corrupt command buffer, by the game's own check. |
| Everything flat while fps falls | Guest CPU. Compare `dispatched` against the GPU counters. |

The GPU counters are accumulated per frame and reported per second. The sampling point sits in `MCLAFrameDelta`, and where the runtime's own per-frame reset lands relative to it is not pinned down, so trust relative change across an event rather than absolute totals.

---

## Regenerating code

```powershell
E:\MCLA\rexglue-sdk-0.11.0\out\install\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo
```

Pass the manifest, not the config: the manifest includes `larecomp_config.toml` and sets up the module dependency graph.

**Nothing under `generated/` is hand-edited.** Codegen rewrites the whole directory. Engine patches live as `[[midasm_hook]]` entries in the config with implementations in `src/mc_engine/hooks/`, so they survive regeneration.

---

## Optional developer checks

`tools/static_analysis/` runs LLVM 23's lifetime, memory and thread-safety analysis over the game's own C++ sources. It is optional: the normal build does not need LLVM 23. What it checks, how to run it and how to review its findings: [STATIC-ANALYSIS.md](STATIC-ANALYSIS.md).
