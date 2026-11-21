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
| GRAPHICS | **DISPLAY**: display mode, resolution, aspect ratio (NATIVE only: AUTO fills the window; 16X9, 16X10, 21X9 or 32X9 keep that shape with black bars), HDR (page), display gamma, vsync, FPS limit, then NVIDIA Reflex or AMD Anti-Lag 2 where the graphics card has one. **UPSCALING**: upscaling, its mode (render scale with no upscaler), sharpness, frame generation (NATIVE renderer; XENIA renderer: render scale). **IMAGE QUALITY**: MSAA, post AA (NATIVE only), anisotropic, depth of field, motion blur. **LIGHTING AND SHADOWS**: shadows, foliage shadows (the shadows far trees cast, drawn as flat cards, into the sun's shadows; daylight only), then (NATIVE only) shadow detail (how finely the sun's shadows are drawn: AUTO, GAME, HIGH or ULTRA), shadow filter (AUTO, GAME, SOFT or PCSS: PCSS sharpens a shadow where it touches what casts it), contact shadows (AUTO, OFF, HIGH or ULTRA; AUTO is OFF on integrated graphics, HIGH on a graphics card with under about 6 GB and ULTRA on the rest), traffic shadows (the other cars cast shadows like yours, so the ground under them is dark: AUTO, OFF, NEAR or ALL; foliage shadows, shadow detail, shadow filter and traffic shadows are grey while shadows is off), car shadows (your car casts a shadow of its real shape on itself and on the road: AUTO, OFF, HIGH or ULTRA; AUTO is ULTRA on a graphics card and HIGH on integrated graphics), ambient occlusion (AUTO, OFF, HIGH or ULTRA; AUTO is OFF on integrated graphics, HIGH on a graphics card with under about 6 GB and ULTRA on the rest), AO intensity, bounce light (the colour a lit wall or the ground sends back onto the road and the cars beside it: AUTO, OFF, HIGH or ULTRA; AUTO is HIGH on a fast graphics card and OFF on the rest; needs ambient occlusion), bounce light strength (how strong it looks: LIGHT, DEFAULT, STRONG, STRONGER or STRONGEST; grey while the bounce light is off), sun shafts (how strong the light shafts of a low sun are, at dawn and dusk: OFF is the game's own picture, then LIGHT, DEFAULT, STRONG, STRONGER or STRONGEST; AUTO is DEFAULT on a discrete graphics card and OFF on integrated graphics); AUTO picks by graphics card. **REFLECTIONS** (NATIVE only): road reflections, car reflections (how sharp the world the cars reflect is: AUTO, GAME, HIGH or ULTRA), car self-reflections (your car's paint reflects its own body, the wing in the trunk lid and the mirror in the door: AUTO, OFF, ON or RAY TRACED; RAY TRACED traces the reflection from the car's own shape and is listed only where the graphics card can ray trace (Direct3D 12 with DXR 1.1, or Vulkan with ray queries); AUTO is OFF on integrated graphics and ON on a graphics card, ray traced where it can; works in the game, behind the pause menu and in the garage). **WORLD DETAIL**: traffic range, pedestrians, parked cars, prop draw distance, traffic LOD, city LOD. Traffic range, pedestrians, parked cars, traffic LOD and city LOD start on AUTO, which follows the graphics card: the most on a fast one (traffic range 400 m, the game's own amounts of pedestrians and parked cars, traffic LOD 3X, city LOD 2X), a little less on a mid-range one and the least on integrated graphics (250 m, half the pedestrians and parked cars, traffic LOD 1.5X, city LOD 1X); the row says what AUTO comes to |
| GAMEPLAY | **CAMERA**: CHASE CAMERA (smoothing factor, camera shake), FOV, REAR VIEW (mirror, position, frame, cars in the mirror), LOOK AROUND (free look). **TIME OF DAY**: real clock, hold time, time, day speed, weather. **CARBON FIBER**: per-car carbon part toggles. **DIFFICULTY**: AI difficulty and the rubberband scales. **MAP MOUSE**: mouse on the full map, scroll-wheel zoom, hide the pointer, cursor magnet |
| CONTROLLER | Vibration, DualSense and Xbox trigger effects (DualSense throttle and brake strength included), stick dead zones, button layout |
| INTERFACE | **UI**: menu colors, speed units, HUD width. **LANGUAGES** |
| EXTRAS | Cutscenes, texture mods (NATIVE only), extra vinyl layers, no 8 player limits, start garage |
| QUIT | Quit or restart |

Not in these menus: GRAPHICS API (the launcher's Graphics API row sets it; `--graphics_backend=auto|d3d12|vulkan` works on the command line), the renderer, and the developer switches (below).

**The compatibility renderer.** The game runs on its own (NATIVE) renderer, and the launcher and these menus offer no other. For a PC where it will not start, the program keeps a second one (the XENIA renderer, the old Xenia GPU emulation), which runs on more graphics cards (Direct3D 12 with shader model 5.1 or binding tier 1, where the native renderer asks for 6.0 and tier 2) and has fewer options: no ambient occlusion, HDR, upscalers or frame generation, and a lower frame rate. It is hidden on purpose. The box that says the game could not start on this PC's graphics card has a button, **Use the compatibility renderer**, which saves the choice and starts the game again; the shader build's failure dialog has the same button, and after two starts in a row that never got going the next start offers it. By hand: put `gpu.driver = "d3d12"` (`"vulkan"` on Linux) in `larecomp.toml`, or start with `--gpu.driver=d3d12`; `gpu.driver = "larecomp"` is the native renderer again. With the debug menu unlocked, RENDERING > RENDERER does the same from inside the game (NATIVE, XENIA D3D12, XENIA VULKAN; it applies at the next start, and leaving the menu offers RESTART NOW). A `--gpu.driver` on the command line or `REX_GPU_DRIVER` in the environment beats the file, and then the row is grey.

**DEBUG menu.** Start with `--debug_menu` (or `REX_DEBUG_MENU=1`, or `debug_menu = true` in `larecomp.toml`) and the SETTINGS tab gets a DEBUG button before QUIT. Its pages: FIXES, PERFORMANCE, RENDERING, CAMERA, TOOLS, LOGGING, INFO. Changes made there last for the session and are never written to `larecomp.toml`. Switches that are only read at start (tree impostors, screen blur, the diagnostics) are launch arguments, for example `larecomp.exe --perf_no_impostors=true`.

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
| `LARECOMP_LOG_FILE` | auto | Log file path. |
| `LARECOMP_NO_CRASH_DIALOG` | unset | `1` skips the "crashed" message box: a crash writes its report to the log and the game ends at once (exit status `0xC4A50001` for an access violation), for test runs nobody watches. |

`REX_LOG_LEVEL` has to be an environment variable. Setting the level from `OnPostInitLogging()` does not work, because the runtime has already printed its startup banner.

---

## What the game writes

| File | Contents |
|---|---|
| `logs/larecomp_NNN.log` | Main log, rolling, up to 10 files of 20 MB. A flag the game cannot use is logged as `[cli] --name=value ignored: ` and why, and the debug settings that are not at their default are listed as `[settings] name = value (command line)`. **Check it first when a setting appears to do nothing.** |
| `logs/timing_<date>_<time>_cap<N>.log` | Frame timing, when `MCLA_TIMING_LOG=1`. |
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
E:\MCLA\rexglue-sdk-0.11.0\out\install\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo
```

Pass the manifest, not the config: the manifest includes `larecomp_config.toml` and sets up the module dependency graph.

**Nothing under `generated/` is hand-edited.** Codegen rewrites the whole directory. Engine patches live as `[[midasm_hook]]` entries in the config with implementations in `src/mc_engine/hooks/`, so they survive regeneration.
