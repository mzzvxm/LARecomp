# Running and debugging LARecomp

Setup and build instructions are in the [README](../README.md). This file covers running the game and reading what it writes.

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

Set anything from the pause menu, on the command line as `--name=value`, or in `larecomp.toml` next to the executable. Menu changes apply immediately and save when you leave the submenu.

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
| RECOMP SETTINGS | Hide mouse pointer, original intro speed |
| GRAPHICS | **DISPLAY**: display mode, vsync, resolution, HDR (page), display gamma, FPS limit, smooth UI, then the picture rows (NATIVE renderer: upscaling, render scale, sharpness, MSAA, post AA, anisotropic, ambient occlusion, AO intensity, frame generation; XENIA renderer: render scale, MSAA, anisotropic). **PERFORMANCE**: RENDERING (shadows, depth of field, NVIDIA Reflex or AMD Anti-Lag 2), WORLD DENSITY (traffic range, pedestrians, parked cars, prop draw distance), LOD AND FOLIAGE (foliage shadows, traffic LOD, city LOD: AUTO, 0.5X, 1X, 1.5X or 2X; AUTO is 2X on a mid-range or fast graphics card and 1X on integrated graphics). **TEXTURE MODS** (NATIVE only): texture mods on or off |
| GAMEPLAY | **CAMERA**: motion blur, CHASE CAMERA (smoothing factor), FOV, REAR VIEW, LOOK AROUND (free look). **TIME OF DAY**: real clock, hold time, time, day speed, weather. **CARBON FIBER**: per-car carbon part toggles. **DIFFICULTY**: AI difficulty and the rubberband scales |
| CONTROLLER | Vibration, DualSense and Xbox trigger effects, stick dead zones, button layout |
| INTERFACE | **UI**: menu colors, speed units, HUD width. **LANGUAGES** |
| EXTRAS | Cutscenes, extra vinyl layers, no 8 player limits, start garage |
| QUIT | Quit or restart |

Not in these menus: RENDERER and GRAPHICS API (the launcher's Troubleshooting section sets them; `--graphics_backend=auto|d3d12|vulkan` works on the command line, and the renderer is four cvars the launcher writes together), and the developer switches (below).

**DEBUG menu.** Start with `--debug_menu` (or `REX_DEBUG_MENU=1`, or `debug_menu = true` in `larecomp.toml`) and the SETTINGS tab gets a DEBUG button before QUIT. Its pages: FIXES, PERFORMANCE, RENDERING, CAMERA, TOOLS, LOGGING, INFO. Changes made there last for the session and are never written to `larecomp.toml`. Switches that are only read at start (cheap car shadow, tree impostors, screen blur, the diagnostics) are launch arguments, for example `larecomp.exe --perf_no_impostors=true`.

**Function keys.** They work for everyone, with or without the debug menu: **F2** dumps the textures of the next frame (Plume renderer), **F3** shows the telemetry card, **F4** opens the settings panel, **F9** warps the car to a mod's warp point and back, **F10** writes a raw trace of the next frame (Plume renderer), **F11** captures the next frame with RenderDoc. Only the pause menu is cleaned up; these keys are as they always were.

**F4** lists every setting, key bindings included. Settings that apply after a restart are yellow. A setting tagged debug (the ones the debug menu holds) can be changed there and applies for the session, but it is never written to `larecomp.toml`: Ctrl+S says how many it left out. To keep one, use a launch argument or a line in `larecomp.toml`.

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
| `MCLA_NO_STUB_SWEEP` | `0` | `1` skips the startup stub sweep, saving about 400 ms. Only safe while `stubs.txt` stays empty. |
| `LARECOMP_LOG_FILE` | auto | Log file path. |

`REX_LOG_LEVEL` has to be an environment variable. Setting the level from `OnPostInitLogging()` does not work, because the runtime has already printed its startup banner.

---

## What the game writes

| File | Contents |
|---|---|
| `logs/larecomp_NNN.log` | Main log, rolling, up to 10 files of 20 MB. |
| `logs/effective_config.txt` | Written every launch: active cvar values, an `ok` or `FAIL` line per GPU flag, the env vars this build reads, and the cvar-only settings listed separately. **Check this first when a setting appears to do nothing.** |
| `logs/timing_<date>_<time>_cap<N>.log` | Frame timing, when `MCLA_TIMING_LOG=1`. |
| `stubs.txt` | Unregistered guest addresses hit at runtime. Empty means nothing was missed. |
| `larecomp.toml` | Saved settings, written when you leave a pause submenu with a changed value. |

Crash diagnostics go into the main log, not a separate file. The SEH and SIGABRT handler in `crash_handler.cpp` resolves and writes the call stack there.

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
E:\MCLA\rexglue-sdk-0.10.0\out\install\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo
```

Pass the manifest, not the config: the manifest includes `larecomp_config.toml` and sets up the module dependency graph.

**Nothing under `generated/` is hand-edited.** Codegen rewrites the whole directory. Engine patches live as `[[midasm_hook]]` entries in the config with implementations in `src/mc_engine/hooks/`, so they survive regeneration.
