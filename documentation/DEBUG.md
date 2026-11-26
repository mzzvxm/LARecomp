# Debug menu and diagnostics

Notes for testers and contributors. The DEBUG menu of the pause menu stays hidden until someone unlocks it on purpose, and the pause menu, the launcher's normal pages and the README do not mention it. The function-key menus (F2, F4, F9, F10, F11) are not part of it: they work for everyone.

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

- The pause menu's SETTINGS tab gets a **DEBUG** button, the last one before QUIT. It opens the eight pages below (seven while the XENIA renderer runs: PICTURE FIXES is the Plume renderer's).
- Nothing else. The function keys do not depend on it: **F2** (texture dump), **F3** (telemetry card), **F4** (the settings panel, every setting), **F9** (warp the car to a mod's warp point and back), **F10** (raw frame trace) and **F11** (RenderDoc capture) work for everyone, locked or not. Only the pause menu is cleaned up.

## Debug changes are never saved

A debug-only setting that is changed while the game runs (a DEBUG page, F4, the console) lasts until the game closes. It is not written to `larecomp.toml`, and a line the file already has for it stays exactly as written. F4's Save says how many it left out. Values from the command line, the environment or the file are honoured as always.

To keep one, use a launch argument, the launcher's **More game options**, or a line in `larecomp.toml`.

**F4 for everyone.** F4 lists every setting, debug-only ones included, whether or not the menu is unlocked. A debug-only setting changed there:

- applies for the session if it is a live one (a restart-only one has no effect until the next start, and the next start does not have it);
- is never written to `larecomp.toml`. Its tooltip says so, the subtitle says debug settings are not saved, and Ctrl+S names how many it left out.

Before the settings clean-up, F4's Save wrote every setting changed in the session, debug-only ones included, so such a change came back at the next start. It no longer does: the never-saved rule holds for every way of changing one (a DEBUG page, F4, the console).

For bug reports, the log lists every debug setting that is not at its default at each start, locked or not, with where the value came from (`command line`, `environment`, `larecomp.toml` or `this session`):

```
[settings] 2 debug setting(s) are not at their default:
[settings] fence_spin_throttle = false (larecomp.toml)
[settings] mcla_gpu_ao_view = 3 (command line)
```

On the DEBUG menu, **INFO > COPY LAUNCH ARGUMENTS** puts `--debug_menu --name=value ...` for every such setting on the clipboard (and in the log). Paste it into a report, or run it to repeat the session. **SHOW CHANGED SETTINGS** writes the list to the log again. **RESET DEBUG SETTINGS** puts the live ones back to their defaults for this session; a restart-only one stays where it is.

One thing is still written: a restart-only choice that a menu holds for the next start (RENDERING > RENDERER), because it cannot work any other way. The row holds it and the next settings save (leaving the menu) writes `gpu.driver` to `larecomp.toml`.

## The pages

Every row is live except RENDERER (below): it takes effect at once and is never saved. A page has at most 24 rows (a page over that is refused whole and does not open; `tools/check_menu_rows.py` counts every page and fails on one over). Rows that only the Plume renderer has show only when it runs.

| Page | What it is for |
|---|---|
| FIXES | Each fix of the recomp's game code with the switch that turns it off (real frame delta, suspension fix, smooth chase cam, the UI fixes and smooth UI, the neon pulse held on the console's 30 Hz clock, the closed checkpoint's smoke fading at the console's rate, the keyboard's own fixes), to see whether a fix caused a bug. RESET DEBUG SETTINGS ends the page. |
| PICTURE FIXES | The Plume renderer's picture fixes with their switches (present dither, alpha to mask, bloom size and exposure sample at a render scale, car chrome, the blue noise of every stochastic effect and the noise resolve). A new fix switch goes to FIXES or PICTURE FIXES by what it fixes. |
| PERFORMANCE | Pacing and speed switches: pace at presentation, fence spin yield, FPS limit spin, single tile, shadow phases, cheap car shadow, foliage, break FPS floor. |
| RENDERING | RENDERER (below), the Plume renderer's debug views (AO, HDR layers), the glow visibility choice (GLOW VISIBILITY: SOFT, the default, fades a street lamp's glow at an edge instead of letting it pop; GAME is the game's one depth test), performance markers, and on the Xenia renderer XENIA VSYNC (it only slows the game's pacing to the vblank; it does not stop tearing). |
| CAMERA | The developer free camera, the free-look collision tests, the camera trace, the menu camera lock. |
| TOOLS | Texture dump and reload for texture mods, buttons for F4 and the telemetry card, the leftover TestMovie button, squash and stretch, HUD ALPHA (the game's own UI faded, see below), FIXED STEP and CAPTURE FPS (capture mode, see below), TRAILER FREE CAMERA and RECORD FLIGHT (the pad flies a free camera and the recorder writes the flight down, see "The free camera and the flight recorder"). |
| LOGGING | One switch per diagnostic that adds lines to the log and can be flipped while the game runs (UI colour, map mouse, wheel, music bank, warp, online and others), and the Plume renderer's periodic logs (GHOST LOG EVERY, MEMORY LOG EVERY, VRAM LOG EVERY). |
| INFO | SHOW CHANGED SETTINGS, COPY LAUNCH ARGUMENTS, RESET DEBUG SETTINGS. |

**FIXES > SKIP GPU TEARDOWN** (`mcla_skip_gpu_teardown`, on by default) is read at quit, so a live change needs no restart. With `mcla_fast_exit` (also on by default) a quit no longer waits for the GPU driver's own device teardown, which took over 8 s on a shared GPU; turn it off to get the full teardown back. The OS reclaims the final GPU objects at process exit after SDK shutdown and the exit flushes. Debug builds, validation runs and device loss use full teardown.

### RENDERER (the way to the compatibility renderer)

The program keeps the XENIA renderer (the SDK's Xenia GPU emulation, with all the fixes it had) as a hidden fallback for a PC where the native (Plume) renderer will not run. Nothing in the launcher or the normal menus offers it; to a player it is "the compatibility renderer" (RUNNING.md). RENDERING > **RENDERER** is the tester's way, in both directions, and it is on the page whichever renderer runs:

- The values are **NATIVE** (`gpu.driver = larecomp`), **XENIA D3D12** (`d3d12`) and **XENIA VULKAN** (`vulkan`). An old file's `any` shows as the API of the platform.
- `gpu.driver` is read at start, so the row does not change the running renderer. It holds the choice; leaving the menu writes `gpu.driver = "..."` to `larecomp.toml` (the one debug row that is kept) and offers RESTART NOW. The label says `(RESTART)`. The log says `[pause-menu] cvar 'gpu.driver' -> 'd3d12' held for the next launch`, `[pause-menu] settings saved to ...` and, after the restart, `[gpu] gpu.driver = d3d12: the SDK's Xenia GPU renders (the compatibility renderer); the Plume renderer stays off`.
- A `--gpu.driver=...` on the command line or `REX_GPU_DRIVER` in the environment beats the file (and a restart keeps the command line), so the row is grey, `RENDERER (LAUNCH OPTION)`, while one is set.
- The XENIA renderer has no shader packs and none of the Plume renderer's effects: the NATIVE-only rows are not listed while it runs, and `[gpu]` lines are not written.

Where else a player can end up on it: the box "LARecomp could not start on this PC's graphics card" has the button **Use the compatibility renderer** (the first button and the default, Enter; **Close** is next to it, Esc; it writes the same line and starts the game again; with a launch option that sets `gpu.driver` it names the option instead; on Windows the box is LARecomp's own, `src/gpu/choice_box.h`, so the long label is not cut); the shader build's failure dialog has **COMPATIBILITY RENDERER** (the compatibility renderer needs no packs); and after two starts in a row of the native renderer that never presented 120 frames or closed by themselves (a crash, a kill: `logs/renderer_start.txt` holds the count, no file means none) the next start asks once. All three log `[renderer] the compatibility renderer is set (gpu.driver = "d3d12" in ...); starting the game again`. To go back: RENDERER > NATIVE, or `gpu.driver = "larecomp"` in `larecomp.toml`.

## Headless present test

Launch with `--debug_menu --mcla_test_present_nowait=true` for a locked or screenless D3D12 A/B. The switch defaults to off and is read at startup. Disable frame generation for these runs. When DXGI reports `DXGI_ERROR_WAS_STILL_DRAWING`, the rendered frame keeps its usual fence and the display skips the image. The log reports `dropped_present_count` and `dropped_present_total` at most once per second, with any pending count flushed at shutdown. Vulkan uses its usual present path because Plume exposes no nonblocking Vulkan present.

The existing `[gpu] scene (interval)` line reports `present_call_ms`, `frame_fence_ms`, `acquire_ms` and `present_slot_ms` as milliseconds per frame alongside the total GPU waits. `present_call_ms` measures the Plume call on the regular path, including its internal Vulkan queue lock, and the DXGI call in D3D12 no-wait mode.

## Diagnostics that write to the log

These only read: they change nothing in the game, they cost nothing while off, and each one is a line or two a tester can send. Switch one on from its row or with a launch argument; the log needs `log_level` info (the default). Why these and not the others of the old local test patches: `feature-parity/T4-DIAGNOSTICS.md`.

| Setting | Where | What it writes |
|---|---|---|
| `mcla_gpu_resolve_memory_check` | launch argument (restart) | Slow: every resolve encode also goes into a second, reference pool at once, and what the game reads is compared with it. `[gpu] resolve memory check: ... readers with a different page ...` every 600 frames and at the end `RESULT PASS` or `RESULT FAIL` (an error line for each difference, the first 16). For wrong or flickering textures that the delayed encode (`mcla_gpu_resolve_memory_encode_delay`, 2 by default) could cause. With the delay at 0 it must pass too (that checks the check). |
| `mcla_gpu_lock_ahead_verify` | launch argument (restart) | A lock whose pages an earlier lock read ahead reads them from the pool as before and compares: `[gpu] lock read-ahead check: ... N pages kept ahead, D different` (D must be 0). The vinyl editor is where it counts. |
| `mcla_gpu_readable_cache_check` | launch argument (restart) | Asks the guest heap again at every range check the per-frame page cache answers. `[gpu] readable cache: ... N cached answers differ from the heap (must be 0)` at the end, and a warning for each of the first 16. |
| `mcla_gpu_shader_memo_check` | launch argument (restart) | No draw takes its shaders from the frame's identity memo: every draw reads the shader objects and compares the memo with the full lookup. `[gpu] shader memo: ... N memo answers differ from the full lookup (must be 0)` at the end, and a warning for each of the first 16 (also for a shader object that changed its microcode within a frame). |
| `mcla_gpu_present_release_check` | launch argument (restart) | Releases the guest at every guest present, also when the GPU is behind, and counts each read or write of guest memory the render thread makes after that: `[gpu] present release: ... N reads or writes of guest memory after the release (must be 0)` at the end. |
| `mcla_gpu_cpu_timing` | LOGGING, CPU TIMING (live) | One line per 600 guest frames with each section's mean and p95 self time in ms/frame, inclusive mean, call count and longest call. Side-view inclusive means cover classified draw preparation and recording. The stopwatch subtracts recorded GPU waits and reports its estimated overhead. The switch applies at the next guest frame. |
| `mcla_gpu_vram_log` | LOGGING, VRAM LOG EVERY (live; frames, 0 off) | `[gpu] video memory (frame N): X MiB in use, budget Y MiB (Z% used) | held by the caches: textures ..., resolve products ..., EDRAM targets ..., resolve memory ... MiB`, at the first check past the interval (checks are 30 frames apart). The existing `[gpu] video memory: short` lines appear only when memory runs short; this one says how near a healthy run is. For stutter or crashes on a card with little video memory. `mcla_gpu_process_memory_log` does the same for the process's system memory. |
| `lowcpu_probe` | LOGGING, LOW CPU PROBE (live) | Every 10 s, `[LowCpuProbe] audio ...`: the audio callback's gaps over 8, 12 and 20 ms and how late the game's 1 ms clock tick was, and `[LowCpuProbe] cpu ...`: CPU time of the audio worker and the timer-queue thread. For crackling sound and stutter on a PC with few cores. The always-on `[Audio]` lines count only the gaps over 20 ms. |

`mcla_gpu_sync_builds` (F4 or a launch argument; live, no row) is the one setting of this section that changes what the renderer does, and it is for recording a trailer: the renderer makes the pipelines of the draws in the background, and a draw whose pipeline is not ready yet is left out for a few frames (`skipped [pipeline pending ...]` in the `[gpu] scene` lines, and the HDR scene's and the upscaler masks' second draws the same way). While the switch is on nothing is left out: the draw waits for its pipeline on the render thread, so no frame misses an object, and the frames that make a new pipeline are slow. The log says `[gpu] capture sync: on`, then the first 24 waits one by one (guest frame, time, shaders) and, beside every scene interval line, `[gpu] capture sync (interval): on | N wait(s) for a pipeline a draw would have been left out for (...), X ms in all, mean, longest | session ...` (that line is absent in a run that never had the switch on). The trailer toolkit's capture mode turns the same switch on while it records.

`mcla_gpu_vram_budget_mb` (a launch argument; live, no row) is the way to see what a small card does on a PC that has plenty: it pretends the video memory budget is that many MiB when the adapter's own is larger (a 4 GB card is about 3900; 0, the default, is the adapter's). Everything that asks the budget sees the smaller number against the real use: the caches' trimming (`[gpu] video memory: short ...` lines), the room checks of frame generation (`NO ROOM` on its row) and of the FidelityFX upscaler's contexts (`[gpu] ffx: no room for the FSR 3.1 context ...`, then the next upscaler down or none runs, and `[gpu] ffx: there is room for the ... context ... now: making it` when the budget is raised again). Run it with `mcla_gpu_vram_log` to read the use.

## Render-thread CPU experiments

`mcla_gpu_target_sig` (F4 or a launch argument, off by default) reuses the draw target's scale, sample count and depth layer on the host render thread when the target signature matches.

`mcla_gpu_layout_two_way` (F4 or a launch argument, off by default) keeps two recent input layouts, two pipeline keys and two successful lookups of ready pipelines on the host render thread.

Both switches apply live at the next guest frame. For an A/B, start with `--mcla_gpu_scene_profile=true` and compare render-thread CPU time alongside the scene profile line's `memo hits` fields. `target sig` is a percentage. `layout 2way`, `pipeline key 2way` and `pipeline get 2way` report hits/tries.

## DLSS Ray Reconstruction and AMD FSR Ray Regeneration (the denoiser row)

GRAPHICS > DISPLAY places the vendor's denoiser after SHARPNESS. An RTX card shows DLSS RAY RECONSTRUCTION, and an RX 9000 on Windows 11 shows AMD FSR RAY REGENERATION. Both use the live bool `mcla_gpu_vendor_denoiser`. ON uses the vendor's denoiser when available, and the default OFF uses LARecomp's denoisers. The toggle is hidden on other GPUs and XENIA. The pause menu uses DLSS RR and RAY REGEN ahead of status suffixes to fit the page's width. DLSS Ray Reconstruction requires DLSS upscaling, and the row shows the reason while unavailable. Streamline loads the RR plugin at startup, so turning the toggle ON after a start without it shows `(RESTART)` and offers the restart prompt. The row shows `(NOT NEEDED)` while all effects that use it are off.

`mcla_gpu_denoiser` (debug-only, live; `row`, `auto`, `rr`, `ours`; `row` by default) is the old three-way chooser, kept for the A/B chains and the live tours: `row` lets the pause menu's row decide; `auto` is Ray Reconstruction where an effect is on that it is known to clean better than ours (none is trusted yet, so `auto` is ours today); `rr` is the row on; `ours` is the row off. Use a launch argument such as `--mcla_gpu_denoiser=rr`, an environment variable or a live debug setting for these overrides. In `larecomp.toml` the old `rr` (both vendor labels) maps to ON, and `auto` and `ours` map to OFF. The new `mcla_gpu_vendor_denoiser` key wins when present. Changing the player toggle saves an explicit new value, including OFF.

`mcla_gpu_dlss_rr` (debug-only, restart; `auto`, `off`, `probe`, `on`) overrides the row: `auto`, the default, leaves the choice to it; `off` takes Ray Reconstruction away whatever the row says (the plugin is not asked for); `probe` loads the plugin and logs whether this card and driver support it and which sizes it takes, with no frame using it; `on` chooses Ray Reconstruction as turning the player toggle ON would, and without the rule that an effect it can clean is on, for the A/B and cost runs. The log's `[gpu] temporal: denoiser: ...` line says each change of the choice and why not (the row's word and the switch are in it); `mcla_gpu_dlss_rr_view` (1 to 6) draws the guide images, and `mcla_gpu_dlss_rr_test` (1 to 3) makes the frames Ray Reconstruction cannot take happen on purpose.

`mcla_gpu_ray_regen` (debug-only, restart; `auto`, `off`, `probe`, `on`): AMD FSR Ray Regeneration (perf/RAY-REGEN.md). It is the AMD half of the denoiser row: the AMD FSR RAY REGENERATION row (on a Radeon RX 9000 series card on Windows 11, Direct3D 12; on Vulkan it is grey with `(D3D12 ONLY)`) lets AMD's denoiser clean the screen-space reflections' noise before the upscaler runs. `auto`, the default, lets the row decide: nothing of AMD's is opened on a card that is not a Radeon, or with the row off, and a row that is turned on opens it live (no restart). `probe` (and `on`) opens AMD's loader and denoiser DLL on a Radeon RX 9000 series card on Direct3D 12 (never on any other PC without the switch), lists the runtime's providers and makes a small trial context; the log's `[gpu] ray regen:` lines say what it found; `probe` uses no frame, `on` is the row on with the rule that an effect it can clean is on lifted (the A/B and cost runs); `off` never.

`mcla_gpu_ray_regen_ssr` (debug-only, live; 0 or 1, default 1) controls the reflection trace while AMD FSR Ray Regeneration is selected. City RT reflections merge their radiance and hit distance into that trace before AMD reads it. Set the switch to 0 to compare against LARecomp's filter for the combined trace.

Per-pixel RT bounce light uses a separate indirect diffuse context at the trace size, including when the governor reduces that size. The trace's coverage and hit distance determine the signal and passthrough texels. The GI apply mixes the denoised irradiance with the probe field and applies receiver albedo. The local filter handles frames without a usable dispatch.

### RG-6 dominant light visibility

The RT-light pass packs up to nine local-light visibility values into three textures. Its trace stops at the first accepted blocker and stores visibility, so those textures cannot recover blocker distance. AMD's dominant-light dispatch takes blocker distance with 65504 for a ray that reaches the light, plus one direction, emission and angular half-angle for the entire dispatch. Local-light directions and angular radii vary per receiver. Keep the packed local-light masks on their current denoiser.

Prototype the city sun first in a dedicated context at the shadow-mask size. Emit nearest blocker distance from an eligible receiver for the sampled sun ray, using 65504 for a miss and an out-of-range depth for uncovered pixels. Supply signed depth, motion with depth delta, world normals, both linear albedos, and the sun's light-to-target direction, emission and sampled cone half-angle. Consume the resulting visibility where the current sun mask is read. Keep the existing mask and filter available on any refused dispatch. Reset for camera cuts, dispatch gaps, footprint changes and material changes to the sun's sampling cone. Verify mask identity for fully open and fully blocked receivers, penumbra width against the sampled cone, and timing and memory on RX 9000 before revisiting local lights.

## The compare keys (F1 and F12, a developer tool)

Two keys for looking at what a graphics effect does, at the same spot: **F1** (`bind_mcla_compare`) flicks the selected effect off and on, **F12** (`bind_mcla_compare_next`) selects the next effect (bounce light, contact shadows, sun shafts, ambient occlusion, shadow filter, self-reflections, and round to the first). A label at the top of the screen says what the picture shows ("BOUNCE LIGHT: OFF" for a flick, "COMPARE: SUN SHAFTS" over ON or OFF for a selection). Both can be rebound in F4's key bindings.

They are not in a release build and not in the pause menu. Configure with `-DLARECOMP_DEV_TOOLS=ON` (CMake option, off by default) and the build has the keys, the `mcla_compare_effect` setting and the code behind them; without it there is none of the three, and the settings' readers ask a flip that is never set (`src/gpu/effect_compare.h`, the inert `State`).

A flick writes no setting, so the saved value and `larecomp.toml` stay as they were. It is a view of the settings' readers, not a setting change: `LiveText::Get()` (ambient occlusion, contact shadows, sun shafts, bounce light, car self-reflections), `shadow_filter::CurrentMode()` and the bloom variant choice in `pipelines.cpp` take the flip into account. It is dropped when the setting it flips is changed or another effect is selected. `[compare]` lines in the log say each press.

`mcla_compare_effect` (debug-only, live) is the selection: F12 writes it, and F4 or the command line can set it too. `mcla_compare_effect=bloom` makes F1 flick the bloom size fix, the same A/B as the debug menu's BLOOM SIZE FIX row, without changing the setting; F12 does not walk to it (the next press goes to the bounce light). `mcla_compare_label_seconds` (debug-only, live, 0.5 to 30, default 1.5) is how long the label stays up; a test run that takes a picture a second after the press sets it higher.

## The words of the graphics settings

The stored word of a graphics setting is the word the pause menu and the launcher show, and nothing is called low. `mcla_gpu_ao`, `mcla_gpu_contact_shadows` and `mcla_gpu_ssgi` take `auto`, `off`, `high` or `ultra`; `mcla_gpu_car_selfrefl` takes `auto`, `off` or `on`. The GPU class (`mcla_gpu_class`) keeps `low`, `mid` and `high`. There is no migration: a word that is no longer a value (the `low` of an older `larecomp.toml`, or the self-reflections' old `high`) is refused by the SDK when it reads the file (`'low' not in allowed values` in the log), so the setting runs as `auto`, and `[settings] larecomp.toml has mcla_gpu_ao = 'low', which is no longer a value of the setting` says so once at the start; the next settings save rewrites that line to `"auto"`. The SDK does not check a value from the command line or the environment, so `--mcla_gpu_ao=low` reaches the renderer as it is, and the renderer counts any word that is no value as `auto` (`src/gpu/host_plan.h`, `not a value of the setting: taken as auto` in the `[gpu] host pipelines` line). The debug cvar `mcla_gpu_ssgi_low` keeps its name (it is the bounce light's half-size choice, 0 to 2, for the HIGH level).

The Steam Deck is class `low` with three cells of its own (`kHandheldCells` in `src/gpu/gpu_class.h`): its GPU is 8 compute units of RDNA 2, so `auto` gives the sun shadow's filter `soft` (not the game's own), and the shadow detail and the reflection detail `high` (Mid's values), where every other `low` class gets `game`. Everything else stays as `low` says (no ambient occlusion, contact shadows, light stack effects, ray tracing; the game's LOD scales). The Deck is found by the name of its GPU (RADV's `VANGOGH`, AMD's `Custom GPU 0405` and `0932`) on an adapter that shares memory; the log's `[gpu] class: low (...)` line says so and the `[gpu] class low (handheld cells): auto is ...` line shows the cells. `mcla_gpu_class=low` gives a Deck the plain `low` cells and `mid` the Mid ones (a forced class is never a handheld), so default against `low` and `mid` with `mcla_gpu_pass_timing` is the measurement that corrects the three cells.

## The graphics presets (GFX-PRESETS)

`mcla_gpu_preset` is one choice for the four effect pages (IMAGE QUALITY, LIGHTING AND SHADOWS, REFLECTIONS, WORLD DETAIL): `vanilla`, `enhanced`, `enhanced_ultra`, `enhanced_rt`, `enhanced_rt_ultra` or `custom`. It sits on the GRAPHICS hub after DISPLAY (the PRESET row, native renderer only), is live, and never touches DISPLAY or UPSCALING (upscaler, render scale, sharpness, frame generation, HDR, the denoiser row). The table is `src/gpu/gfx_preset.h` (`kRows`: one cell per covered row for each of the five named presets, 37 covered rows and one support row); it is the only place the numbers are written. `gpu_class.h`'s `auto` cells (`kEffects`, `kScales`) and `rt_tier.h`'s (`kRtEffects`) are read from it, so an `auto` is the value of the preset its class maps to (Low is VANILLA's column, Mid ENHANCED's, High ENHANCED ULTRA's; the ray traced rows follow the rung: none ENHANCED's, weak ENHANCED RT's, strong ENHANCED RT ULTRA's). The recommended preset of a card is `RecommendedPreset(class, tier)` in `rt_tier.h` (a strong High card ENHANCED RT ULTRA, a card at the weak rung ENHANCED RT, else High ENHANCED ULTRA, Mid ENHANCED, everything else VANILLA); the settings-ui.json export prints the same rule (`presets.class_to_preset`) for the launcher's "Recommended" tag. The first start stays VANILLA.

How a preset reaches the rows (`src/gpu/gfx_preset_apply.cpp`; the decisions are plain functions of `gfx_preset.h`):

- The config keeps the preset's NAME. While a named preset is in force its values are HELD over the covered rows with the same hold the trailer scenes use (`SetCvarForScene`), so a save never writes them to `larecomp.toml`, and the player's own values underneath stay in the file. Picking another preset at run time holds the new values at once; picking `custom` again puts the player's own values back (or, on a fresh install that never had any, keeps what the preset gave, as the player's own).
- Changing a covered row in the pause menu while a named preset is in force turns it into `custom` (`CvarSet` calls `gfx_preset::NoteRowWrite` first): the rows keep the values they have (they are written at the next save) and `mcla_gpu_preset_from` (debug-only) records the preset they came from, so a row added in a later version takes that preset's value. A change made by the F4 panel, the console, a trailer scene or the harness is not the player's choice and does not flip it (the hidden DEBUG menu's rows go through the same `CvarSet`, so a covered row changed there does).
- A setting on the command line or in the environment sits on top of a preset: the row is left alone when the preset is held, the value is never saved, and the preset never turns into `custom` for it. `--mcla_gpu_preset=enhanced_rt_ultra` is the harness's and the trailer's way to start on a preset (also never saved).
- First start (`DecideInitial`, read in `ApplyPlayerConfig`): no `larecomp.toml`, or one with no graphics value in it: `vanilla`. A `larecomp.toml` that has a line for any row of the table and no `mcla_gpu_preset` line (every file written before presets, a zip-era tester's too) starts as `custom`, with `mcla_gpu_preset_from = none` (rows it does not name keep their own defaults, which is what they were), so nothing changes for it.
- The two RT presets need an RT tier: the pause row lists them only where `gpu.rt_tier` is not `none` (and keeps listing the one in force, with `(NO RT GPU)`); the launcher greys them with `needs` and `needs_reason`.

A clean install without a `larecomp.toml` now starts on VANILLA, not on each class's `auto`: a harness run that wants the old look passes the preset it means (`--mcla_gpu_preset=enhanced` or `enhanced_ultra`, `enhanced_rt`, `enhanced_rt_ultra`) or `--mcla_gpu_preset=custom`, which leaves every row at its own value (`auto` included). ENHANCED ULTRA is the High class's `auto`, not literally every row at its top level (`kBelowMax` lists the rows); REFLECTIONS ON SURFACES is off in every class's `auto` and `ultra` in the two ULTRA presets. Five cells of `auto` are NOT read from the table and stay literals, each with its reason in `gpu_class.h` and `car_shadow_map_rules.h`: the Low class's four scale cells (traffic LOD, traffic range, pedestrians, parked cars: below VANILLA's stock numbers since the 2026-11-20 LOD pass), the Steam Deck's three handheld cells, the class cell of RT SHADOWS, the class cell of REFLECTIONS ON SURFACES and the car shadow map's Low cell. `tools/check_gfx_presets.py` (with `--selftest`) reads the table against the four pages, the settings' own lists, the pause row and `ui_rules.toml`; `larecomp_gfx_preset_test` pins the cells against the old `auto` tables and the audit's values.

## The ray tracing tier

The GPU class (`mcla_gpu_class`) says how strong a card is overall; the ray traced upgrades (the car's reflection, occlusion and sun ray, later the city's shadows, reflections and bounce light) also read how well it traces rays, because an RTX 3060 and an RTX 5080 are both `high` and the 5080 traces several times faster. The tier is `none`, `weak` or `strong`, found once when the device opens from the graphics API (can it trace at all), a table of cards by device id and name, and, on the first start with a card, a short benchmark that can move it between `weak` and `strong`. The log says which rule chose it, on one line the launcher reads: `[gpu] ray tracing tier: weak (by name: RTX 3060 -> weak ...)`, then `[gpu] ray tracing tier weak, class high: auto is ...` with what `auto` comes to for each ray traced upgrade.

`mcla_gpu_rt_tier` (`auto`, `none`, `weak`, `strong`; debug-only, live) forces it for a test. While `mcla_gpu_class` forces a class, the tier follows it unless this is set (`high` is `strong`, `mid` is `weak`, `low` is `none`), so a test that says a class still runs that class's cells. A card the graphics API says cannot trace stays `none` under a forced class.

`mcla_gpu_rt_bench` (`auto`, `off`, `force`; debug-only, restart) is the benchmark's switch: `auto` runs it when the card or its driver is new to `rt_bench.txt` (next to `larecomp.toml`), `off` skips it and ignores that file, `force` runs it at every start. It traces a few million rays through a small synthetic city on the card's own ray query path (about 100 to 250 ms once) and logs `[gpu] rt bench: ok (5592 M rays/s, ...)`. A card the table knows moves only when the speed is clearly across the line (a wide band either side), an unknown card goes by the line alone. The line (`kBenchThresholds` in `src/gpu/rt_bench_rules.h`) and which SKU groups are weak or strong (`src/gpu/rt_tier.h`) are first guesses until the graphics cost pass sets them from the measured cost; the log lines of the first runs on other cards are what it needs.

## The ray traced bounce light

The pause menu's RT BOUNCE LIGHT row (`mcla_gpu_rt_gi`: `auto`, `off`, `high`, `ultra`; live) is the screen-space bounce light's light (the BOUNCE LIGHT row, `mcla_gpu_ssgi`) worked out in a field of probes round the camera from the city's own triangles. `auto` is `high` where the ray tracing tier is `strong` and `off` on the rest (a weaker card can switch it on by hand, with fewer probes). Where the field stands it replaces the screen-space bounce light (that row says `(REPLACED)`); BOUNCE LIGHT STRENGTH (`mcla_gpu_ssgi_intensity`) is the strength of both. The field exists only in the play view (not in the pause menu's world, the garage or the map) and where the city's structures stand (`mcla_gpu_rt_shadows` keeps them for now); it fades in over 0.4 s once most of the finest cascade is traced and out over 0.5 s, and the screen-space bounce light crossfades the other way. The log's `[gi]` lines say the setting's decision, `[gi field]` the field and `[gi apply]` the apply; if the field stands but its light cannot be drawn for 8 frames in a row (no G-buffer planes in a cutscene, no pipeline) the field idles for 4 s and the screen-space bounce light takes over.

The screen-space bounce light also takes light from the deferred light pass (neon, headlights, street lamps) in the previous frame. It reprojects the difference between two colour captures taken around the eligible light draws, using the occlusion's camera motion and depth history. It rejects vehicle pixels, disocclusions and the sky. The first frame and camera cuts start with empty light history.

The probe field shades with the world's lamp grid, glow materials and listed static emitters. While the field stands, the screen-space captures carry the remaining deferred lights, including moving emitters at RT High. RT Ultra on a strong GPU blends in per-pixel irradiance that shades with the full emitter list. Those moving emitters leave the screen-space captures when the per-pixel pass publishes its result.

`mcla_gpu_ssgi_lights` (debug only, default on) toggles the screen-space lights' share at BOUNCE LIGHT High and Ultra. `mcla_gpu_ssgi_lights_gain` (default 1) scales it and `mcla_gpu_ssgi_lights_floor` (default 1.3) sets the minimum radiance cap for that share. Set the floor to 0 to use the regular radiance cap. The live debug switches `mcla_gpu_gi_bounce_headlights`, `mcla_gpu_gi_bounce_neon`, `mcla_gpu_gi_bounce_rear` and `mcla_gpu_gi_bounce_other` gate their groups in both paths. See the JOIN section of `perf/GI-BOUNCE.md` for the tier and source details.

`mcla_gpu_rt_gi_view` (debug-only, live) draws one picture in place of the bounce light's add: 1 the irradiance, 2 the confidence, 3 the sky visibility, 5 the ground baseline, 6 the leak test, 9 the light the pass adds, 10 the net irradiance, 11 the occlusion's term (4, 7 and 8, the probes as spheres, are the field owner's and are not wired to a pass yet). `mcla_gpu_rt_gi_bounces` (0 to 0.95, default 0.5; debug-only, live) is how much of the field's own light the probes take back in when they are updated: 0 is strictly one bounce, 0.5 about two. `mcla_gpu_rt_gi_dry_run` (debug-only, live) lets the field run before the probe update is built into the renderer (the row is grey with `(NOT READY)` until then): the textures are made and the reset and weights passes run, nothing traces into the probes, the field never becomes ready and the screen-space bounce light stays the source; it is for proving the field's owner and the order of the frame.

`mcla_gpu_gi_cabin` (debug-only, live, on by default) is the probe-field cabin shield (PICTURE FIXES > CABIN BOUNCE SHIELD). The probes trace the city only. The shield reduces the field's light towards 30% for the driver's skin and interior trim inside the player car's box when the camera meets the box and headroom tests. It currently assumes a closed roof.

### Cockpit effects A/B

The recorded integ4 day split isolated the car shadow map. The left hand, dash and wheel rose from 6.1, 1.9 and 38.5 to 32.2, 9.6 and 48.0 mean crop levels when the map was disabled. Integ7 with the cabin map exemption gave 30.5, 8.3 and 47.8 on D3D12 and 30.5, 8.1 and 47.6 on Vulkan. The valid bounce-only comparisons were within the repeat noise. See `perf\out\interior-cam\cockpit-fix-before-after.png` and the INTERIOR-SHADOW-SPLIT and COCKPIT-FIX-VERIFIED entries in the project log.

The sidecar follows the main-view draw and depth resolve. RT1 supplies normals, RT2 albedo and baked light, and RT3 material and flags. The driver skin and trim groups carry the vehicle flag. The depth resolve derives the vehicle ID from stencil bits 2-7 and can carry the selected half-size sample and the player tag into level zero. These paths reconstruct from the main projection. The code provides no alternate cockpit depth decoder. Check the actual viewport, depth range, and sample-zero stencil of each hand, wheel, dash and windshield in the suite. The motion pass reconstructs from depth and the camera and can substitute a rigid per-vehicle transform. Steering and skinning have no independent motion rows. The RT scene stages eligible rigid player draws, including interior parts observed in the cockpit census, and drops skinned draws. See [G-buffer resolve](../src/gpu/gbuffer.cpp), [material classification](../src/gpu/gbuffer_material.h), [depth resolve](../src/gpu/hlsl/ao_depth_ps.hlsl), [motion pass](../src/gpu/hlsl/temporal_mv_ps.hlsl) and [car RT capture](../src/gpu/car_rt_scene.cpp).

The three new live A/B switches are in DEBUG > CAMERA and start off. Test one switch at a time with same-settings repeats, matching poses, settled exposure, and the same effect tier on both APIs. Use exterior/chase as a control, and take still and steering samples at 1080p and 4K.

| Effect | Cockpit path and known failure or risk | Fix and switch | Capture to review |
| --- | --- | --- | --- |
| GTAO / bitmask AO, car RT AO, RTAO | GTAO reconstructs from shared depth, caps the projected radius and uses depth-derived normals when RT1 is unavailable. Half-size edges and fixed thickness matter at hand distances. Car RT AO can miss the skin mesh when snapping to the BLAS. RTAO used a rotating trace tap with a separately selected guide, so a street or wheel trace could seed the hand's denoiser. The closed-cabin screen fallback protects samples inside the car box when the closure probe agrees. | `mcla_gpu_rtao_cabin_guide` keeps the rotating tap when depth and vehicle ID agree and uses the guide at discontinuities. The built-in RTAO history restarts on a policy change. [RTAO trace](../src/gpu/hlsl/rtao_trace_cs.hlsl) | Stable AO and coverage at hand/wheel and pillar/window edges with raw and filtered RTAO. Check roof closure, open cars, and strong grazing edges. |
| SSGI and captured lights | SSGI paired coarse color averages with depth across hand and window silhouettes. Level zero itself mixed MSAA and half-size color samples. Deferred light captures use previous-world reprojection and reject vehicle texels. Their own reduced-resolution footprint can still include mixed edges. | `mcla_gpu_ssgi_cabin_samples` aligns level-zero scene color to depth for cockpit frames and uses level-zero depth and radiance taps on the player's receivers. It resets AO/SSGI history on entry, exit and ID changes. [Color resolve](../src/gpu/hlsl/ao_color_ps.hlsl), [bounce trace](../src/gpu/hlsl/ao_main_ps.hlsl) | Bright road or lamp through the windscreen, hands and dash, contact detail retained, no new shimmer or GPU-time regression. Check captured lights separately. |
| Probe field and per-pixel RT GI | City-only probes see through the roof. The existing shield selects by material, box and headroom, which can also allow an overlapping alternate camera. The detail ray pass skips vehicle receivers, and rays cannot hit the skinned driver. | `mcla_gpu_gi_cabin_rig` adds the rendered cockpit requirement to the existing shield. [Probe trace](../src/gpu/hlsl/gi_update_trace_cs.hlsl), [apply](../src/gpu/gi_apply.cpp), [detail trace](../src/gpu/hlsl/gi_pixel_trace_cs.hlsl) | Lamp pool, bright wall, tunnel and convertible. Compare the field and detail views. Closed cockpit retains the existing shield in pause. A photo or toolkit camera overlapping the box loses the shield with the new switch on. |
| SSR / RT reflections | SSR uses shared Hi-Z and material smoothness, and excludes vehicle materials as receivers. The near-plane clamp shortens close rays. History uses camera reprojection for the receiver and can use a rigid vehicle row for the reflected hit. Car RT self-reflections trace from eligible glass or vehicle parts and omit the driver. | Existing near-plane, material and RT hit policies. [SSR trace](../src/gpu/hlsl/ssr_trace_ps.hlsl), [SSR history](../src/gpu/hlsl/ssr_temporal_ps.hlsl), [car RT trace](../src/gpu/hlsl/car_rt_refl_cs.hlsl) | Windscreen, dash, mirrors and outside rain reflections during steering. Check edge trails, grazing misses and driver silhouettes across SSR, RT and fallback. |
| Contact, car map and RT shadows | Rigid body and roof can block sunlight at the player receivers. Contact march and RT sun also have explicit source-isolation modes. Local light rays and atlas lookups still follow their own geometry, depth biases and masks. | Existing `mcla_gpu_cabin_sun_shadow` mode 2 car-map exemption on this branch. [Shadow rules](../src/gpu/hlsl/cabin_shadow_rules.hlsli), [RT car AO/sun](../src/gpu/hlsl/car_rt_ao_cs.hlsl) | Day map off/on and vanilla, night lamp, pause and detached views. Confirm the cabin is lit and exterior shadows hold. |
| Volumetrics | Froxel history reprojects in camera/world space. Apply samples the same main-view depth and has an MSAA disagreement path. Closed-cabin air has no separate volume mask, so lighting before the visible surface can include scattering inside the shell. | Existing depth clamp and MSAA coverage resolve. [Injection and history](../src/gpu/hlsl/volumetrics_inject_cs.hlsl), [apply](../src/gpu/hlsl/volumetrics_apply_ps.hlsl) | Night headlights, rain and tunnel. Fog should stop at the dash, pillar and hands while remaining visible through the window. |
| Denoisers, TAA and upscalers | AO/SSGI and contact reproject using depth and available rigid vehicle rows. RT denoisers use per-effect depth, normal and hit guides. TAA/DLSS/FSR/RR inherit the camera or car vectors, so finger and wheel motion can leave residual history. Cuts and depth rejection can limit trails when their inputs agree. | Existing temporal guards, plus the SSGI and RTAO history resets above. [AO temporal](../src/gpu/hlsl/ao_temporal_ps.hlsl), [RTAO denoiser](../src/gpu/hlsl/rtao_denoise_temporal_cs.hlsl), [upscaler inputs](../src/gpu/temporal.cpp) | Motion and history views in still, steering, disocclusion and rig changes. Compare native, DLAA/FSR and raw versus filtered signals where supported. |

## The ray traced city reflections

The pause menu's RT CITY REFLECTIONS row (GRAPHICS > REFLECTIONS, right after CAR SELF-REFLECTIONS; `mcla_gpu_rt_reflections`: `auto`, `off`, `high`, `ultra`; live) puts the real street in the car's paint: the building across the road, the lamp posts and lamp heads, signs and the car beside you, found by rays in the city's own triangles, with the game's environment map kept for the sky. `auto` is `high` where the ray tracing tier is `strong` and `off` on the rest (a weaker card can switch it on by hand and runs a shorter reach: 60 m and two rays at `high`; `high` reaches 150 m and `ultra` 1,000 m on a strong card). It rides on the car's own ray traced pass, so the row is grey with `(NEEDS CAR RT)` unless CAR SELF-REFLECTIONS is on RAY TRACED (or AUTO that comes to it); the other grey states are `(NO RT GPU)` and `(TOO SLOW)` (the integrated class), and `(NOT READY)` when the city's data cannot be had this session (no game archive, a failed build). It needs the city pack and the hit data (the launcher's setup builds both; the game builds them in the background when they are not there, and the row says `(BUILDING DATA)` and stays on meanwhile: the word is kept and the city comes into the paint by itself when the files are done). The row's label is the short `RT CITY` while it carries a reason (the page is as wide as REFLECTIONS ON SURFACES allows). The log's `[city refl]` lines say the decision (`the city in the car paint's reflections: ...`, one line each time it changes) and, while the pass runs, the census (`[city refl] frame N: ...`). `mcla_gpu_city_refl_screen`, `mcla_gpu_city_refl_coat`, `mcla_gpu_city_refl_reach` and `mcla_gpu_city_refl_view` (debug-only, live) are the A/B of the picture's own colour, the mirror's coat layer, the reach in metres and the source/class views.

### The city in the side mirrors (MIRROR-CITY)

`mcla_gpu_car_mirror_rt_world` (`auto`, `env`, `city`; debug-only, live) says what the ray traced side mirrors (the RT SIDE MIRRORS row) show where their ray finds none of the cars. `env` is the game's environment map, the mirrors as they were. `city` is the street behind the car found by rays in the city's triangles and shaded as the car paint's reflections shade them (the same light, lamps and glow; `mcla_gpu_city_refl_lamps` and `mcla_gpu_city_refl_world_light` move both), on any card that can trace, with a reach of 150 m on a strong card and 60 m on the Mid class. `auto` follows the RT CITY REFLECTIONS row: the city where that row comes to on, the environment where it is off. A mirror of a few pixels (the chase cameras) always shows the environment and asks for nothing of the city; a bigger one asks for the city's structures, hit data and textures, which the reflections and the sun shadows already hold where they are on. The mirrors show the environment until the city's data is ready. The log's `[car mirror]` lines say the decision (`the mirror's world ...`, one line each time it changes) and, in the window line, how many passes traced the city.

## The ray traced light visibility (RT-LIGHTS-D1, parts 1 and 2)

`mcla_gpu_rt_lights` (debug-only, live; `off` by default, `auto` follows the RT SHADOWS row, `on` runs it wherever the card traces rays) gives the lights the deferred light pass draws a second shadow beside the shadow atlas: one ray from every pixel to a point of the light's disc, against the city's structures and the cars, filtered by the renderer's own temporal and a-trous filter, read by the light pass. The two shadows multiply, because neither knows every caster (the atlas has the poles and trees the main view drew, the rays have the walls and cars the picture does not show). Which lights: the top K (`mcla_gpu_rt_lights_k`, 0 is 3 and 6 at the largest LIGHT SHADOWS level, up to 9): a car's headlights first, then neon, then the nearest street lamps; a lamp the atlas did not pick is handed to the pass only to get its ray. A street lamp the game lights is only ever taken away from (the pixel loses the shadowed share of the lamp's light), never added, so with every ray free the picture is the game's, bit for bit. It needs the light pass to run (`mcla_gpu_local_lights` or the lamp shadows), the ray tracing structures and, for the lamps, the game's lamp list at night.

`mcla_gpu_rt_lights_radius` (the light's disc in metres, 0: by kind of light), `mcla_gpu_rt_lights_rays` (rays a light and pixel, 1 to 4), `mcla_gpu_rt_lights_view` (1 the filtered visibility drawn instead of each light that has a ray, red where it is blocked, green where it arrives; 2 the raw one) and `mcla_gpu_rt_lights_check` (the identity test: 1 traces and costs but writes every visibility as 1, so the picture must equal the pass off; 2 every visibility 0). The noise is the shared blue noise (`mcla_gpu_noise_blue`). The log's `[rt lights]` lines say when the pass runs, why it does not (the first reason: the setting, the device, no top level, no world rows, no depth, no light, pipelines, images) and, every 600 frames, the counters and the timings (`rt lights`, `rt lights denoise` with `mcla_gpu_pass_timing`). `perf/RT-LIGHTS-D1.md` has the design, the checks and what the first chain measures.

`mcla_gpu_rt_lights_lamp_slot` (debug-only, live; 0 by default, an A/B arm): the pick is headlights, then neon, then street lamps, so three cars with their headlights on use all three channels and no street lamp gets a ray. With 1 and a K of 2 or more, when no street lamp is among the picked lights the last channel goes to the strongest street lamp in the light pass's list; the headlights and neon above it keep theirs.

Part 2 (memory and Ray Reconstruction): the pass keeps its visibility in 8 bits (the signals and the filter's four images a group of three lights RGBA8, the depth guides R16F, the normal and the share one RGBA8 snorm image) and traces at half of the occlusion's traced size where that is taller than 720 lines, so K 3 holds 13.8 MiB at 1080p and at 4K and 24.6 MiB at 1440p (part 1: 119, 119 and 53); the light pass reads the smaller mask back by a depth guided bilinear. `mcla_gpu_rt_lights_scale` (0 auto, 1 the whole traced size, 2 half wherever it fits) is the comparison switch. The 8 bit history is rounded at random (the shared blue noise, a new number every frame) so that the average neither stalls nor drifts. `mcla_gpu_dlss_rr_rt_lights` (1 by default, 0 keeps the filter) hands the unfiltered visibility to the light pass in a frame DLSS Ray Reconstruction takes (the row on DLSS RR; AUTO does not use RR for it until the A/B); the census says how many frames were raw. The `[rt lights]` line `the pass's images` says the mask's size, the step and the MiB held.

## Launch arguments for restart-only diagnostics

A debug setting that the game reads only at start cannot be a row: a change would not take effect, and it could not be saved for the next start. These are launch arguments:

```powershell
larecomp.exe --debug_menu --mcla_gpu_pass_timing=true
```

The same text works in the launcher's **More game options**, and as `mcla_gpu_pass_timing = true` in `larecomp.toml`. A switch takes `true` or `false`. An option the game cannot use (an empty or refused value, a name that is not a setting, a setting of a driver this run does not load) is skipped with a `[cli] ... ignored: ...` line in the log and the game starts. F4 shows each of these with its description (use the search box).

**Where the list comes from.** It is a snapshot of the source: every cvar whose `REXCVAR_DEFINE_*` has `.debug_only()` and a restart lifecycle (`kRequiresRestart` or `kInitOnly`), and that no row of a `kDebug*` table names. The scan used the definition parser of `tools/check_settings_exposure.py`, which reads the chained calls the same way. `perf_no_trees` and `perf_fast_vehicle_shadows` are not here: their lifecycle is live, so they are rows on the PERFORMANCE page. `break_pairwise_collision` is not here either: its hook reads it at every call, so it is live (a setting in F4 and a launch argument that also works while the game runs; it has no row). The SDK's `graphics_backend` (`d3d12` or `vulkan`) is also tagged debug-only at start (`kSdkDebugCvars` in `src/mc_engine/debug_menu.cpp`) and is a launch argument too; it is defined in the SDK, not here, so it is not counted.

The list, 132 names in 15 groups (the number in brackets is the group's size):

**The unlock** (1)
`debug_menu`

**Shader packs, the city data and the pipeline cache (the launcher runs `build_shader_packs`, `build_city_pack`, `build_city_hit`, `check_city_hit` and `precache_pipelines` itself)** (14)
`build_shader_packs`, `build_shader_packs_force`, `build_city_pack`, `build_city_pack_force`, `build_city_hit`, `build_city_hit_force`, `check_city_hit`, `check_city_hit_deep`, `shader_pack_dir`, `shader_pack_threads`, `precache_pipelines`, `precache_pipelines_backend`, `precache_pipelines_force`, `precache_pipelines_threads`

**The legacy D3D12 runtime** (2)
`mcla_native_gfx`, `mcla_native_gfx_continuous`

Which renderer runs is the SDK's `gpu.driver` (`larecomp`, `d3d12` or `vulkan`; the Xenia ones are the hidden compatibility renderer), which the DEBUG menu's RENDERER row holds for the next start (see RENDERER above). `mcla_gpu` and `mcla_native_gfx_nocp` of version 0.10 are not settings any more: a line naming one (in `larecomp.toml`, on the command line) is ignored with a note in the log, and an old `larecomp.toml` that has one set to `false` still picks the Xenia renderer.

**Plume renderer: timing and statistics** (7)
`mcla_gpu_pass_timing`, `mcla_gpu_scene_profile`, `mcla_gpu_ab_interleave`, `mcla_gpu_memory_census`, `mcla_gpu_census_ps`, `mcla_gpu_census_targets`, `mcla_gpu_census_scale`

**Plume renderer: finding picture bugs** (18)
`mcla_gpu_edram_aliasing`, `mcla_gpu_coverage_queries`, `mcla_gpu_edram_capture`, `mcla_gpu_flicker_probe`, `mcla_gpu_flicker_probe_dumps`, `mcla_gpu_flicker_probe_shots`, `mcla_gpu_verify_constants`, `mcla_gpu_validate`, `mcla_gpu_watch_base`, `mcla_gpu_watch_size`, `mcla_gpu_watch_capture`, `mcla_gpu_texture_decode_selftest`, `mcla_gpu_resolve_memory_check`, `mcla_gpu_lock_ahead_verify`, `mcla_gpu_readable_cache_check`, `mcla_gpu_shader_memo_check`, `mcla_gpu_present_release_check`, `mcla_gpu_local_lights_ab`

**Plume renderer: graphics-API debugging** (4)
`mcla_gpu_debug_layer`, `mcla_gpu_gpu_based_validation`, `mcla_gpu_object_names`, `mcla_gpu_force_refusal`

**Plume renderer: tuning and driver workarounds** (10)
`mcla_gpu_batch_draws`, `mcla_gpu_present_wait`, `mcla_gpu_present_release`, `mcla_gpu_pipeline_cache`, `mcla_gpu_readable_cache`, `mcla_gpu_texture_decode_threads`, `mcla_gpu_skip_shaders`, `mcla_gpu_upscaler_lazy_load`, `mcla_gpu_rt_bench`, `mcla_gpu_dlss_rr`

**Legacy native D3D12 runtime (Windows, experimental)** (11)
`mcla_native_gfx_present`, `mcla_native_gfx_aniso`, `mcla_native_gfx_gen_mips`, `mcla_native_gfx_mrt`, `mcla_native_gfx_msaa`, `mcla_native_gfx_own_device`, `mcla_native_gfx_own_heap_kb`, `mcla_native_gfx_own_swapchain`, `mcla_native_gfx_surface_key`, `mcla_native_gfx_upload_mb`, `mcla_gfx_probe`

**Speed experiments read at start** (14)
`perf_no_race_shadows`, `perf_no_impostors`, `perf_no_fullscreen_blur`, `fma_float_fast`, `vsync_fast_poll`, `submit_on_primary_buffer_end`, `lzx_native`, `lzx_native_check_all`, `host_helpers_mask`, `race_line_native_mask`, `race_line_check_calls`, `race_line_twin`, `race_line_twin_mask`, `thread_qos`

`thread_qos` (Windows 10 1709 or later, restart, `0` off by default) is for testing whether Windows power throttling affects the game. `1` requests HighQoS for the process while focused. `-1` (automatic) selects `1`. `2` also tags the render and audio threads and the frame generation pacer for the whole session. On hybrid CPUs, `2` selects Pro Audio when `audio_mixer_priority` is automatic. The `[qos]` log lines say what was applied (see `perf/HYBRID-QOS.md`).

On the plugged-in i9-13980HX in Silent mode, automatic showed no measurable gain (8.311 ms median frame time vs 8.315 ms off, six runs each), and level `2` took about 1% longer per frame in three runs (`perf/HYBRID-QOS-AB.md`).

**Render-thread CPU savings read at start** (1)
`render_cpu_mask`

**Game patches read at start** (3)
`disable_rubberbanding`, `showroom_unlimited`, `vehicle_records_relocate`

**Audio and music** (4)
`audio_mixer_priority`, `mcla_audio_output`, `custom_music`, `music_list`

**Probes and paths** (4)
`guest_profile`, `input_probe`, `dbg_print`, `string_folder`

**Model mod tuning (all `model_mods_*`; the `model_mods` switch itself has a launcher row)** (38)
`model_mods_anchor_bones`, `model_mods_bone`, `model_mods_cabin_light`, `model_mods_car_budget`, `model_mods_car_pack_write`, `model_mods_car_scale`, `model_mods_car_space_only`, `model_mods_car_textures`, `model_mods_car_verbatim`, `model_mods_car_yaw`, `model_mods_decimate`, `model_mods_diag`, `model_mods_glass_band`, `model_mods_grow`, `model_mods_grow_probe`, `model_mods_grow_slack`, `model_mods_offset_x`, `model_mods_offset_y`, `model_mods_offset_z`, `model_mods_part_growth`, `model_mods_passthrough`, `model_mods_pitch`, `model_mods_proportions`, `model_mods_rim_autoalign`, `model_mods_rim_inherit_shade`, `model_mods_rim_offset_x`, `model_mods_rim_pitch`, `model_mods_rim_roll`, `model_mods_rim_scale`, `model_mods_rim_shade_occlusion`, `model_mods_rim_yaw`, `model_mods_roll`, `model_mods_scale`, `model_mods_share_images`, `model_mods_submeshes`, `model_mods_textures`, `model_mods_weight_smoothing`, `model_mods_yaw`

**Crash logger test (Linux)** (1)
`mcla_test_crash`

`mcla_test_crash=MODE[:SECONDS]` makes the game crash on purpose a few seconds after it started, to check the Linux crash logger (needs `--debug_menu`; it is ignored while the menu is locked). Modes: `segv`, `guest` (reads the guest's null page, like the real guest crashes), `stack`, `abort`, `terminate`, `illegal`, `fpe`. The log must end with a `!!! CRASH DETECTED !!!` (or `!!! ABORT DETECTED !!!`) report with a stack trace, and the game must die with the signal's exit status. `ports/scripts/linux/crash-test.sh` runs them all.

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
| `hour <h>`, `hour <a>..<b> over <s>`, `hour free` | `TimeOfDay_SceneHour`, the scene's own request: the hour is pinned (a value over 24 wraps) and outranks `time_of_day_hold` and the clock setting, which carry on when the scene lets go; `free` gives the day cycle back |
| `weather nice\|cloudy\|stormy\|foggy\|game` | `Weather_SceneIndex`, the scene's own request, over the `weather` setting; `game` gives the weather back |
| `traffic`, `peds`, `parked`, `police` `on\|off` | the game's own script natives (`AmbientMgr_Set*Enable`, `PoliceManager_SetAllPoliceActive`); the enable byte is read back and the log says what it became. `traffic off` also holds `traffic_density_scale` at 0 (the zones' traffic density, and the gate that keeps the enable byte at 0 whatever writes it), and calls the native one update after it so the gate saves the game's own byte: `traffic on` and the scene's end give both back |
| `peds <scale>`, `parked <scale>` | the density settings `ped_density_scale` / `parked_car_scale` held for the scene, a number from 0 to 2 (a bare 0 or 1 is a scale here; the switches are the words `on` and `off`); the old value and where it came from come back at the end, like a `set`. A 0 closes that spawner for new spawns; what is on the road stays until it leaves (`peds off` also calls the native). `world peds 0 parked 0.25` works too |
| `timescale <x>`, `timescale <a>..<b> over <s>` | the scale is put into the engine timer itself (a hook in `sub_821BDA90`), so the game's own slow motion and its floor of 0.1 do not matter; the state line's `engine step ratio` is what the engine really did (below about 0.006 at 60 FPS the engine's own floor under the scaled step, 0.1 ms, holds it) |
| `set <setting> <value>` | the setting, live ones only; the old value comes back at the end. Not every setting: see "What a scene cannot set" |
| `label "<text>" [side s] [fade s] [for s]`, `mark <name>` | logged (labels are data for a compositor) |
| `statelog on [dir <folder>]`, `statelog off` | an option of the whole scene (no time): the per-update state log for `take_diff.py` ("The state log and take_diff", below) |
| `split <x>`, `split <a>..<b> over <s> [ease e]`, `split off` | `mcla_gpu_split`: where the before/after split sits, 0 to 1 across the picture (see Before/after sweeps); `off` is no split |
| `capture fps <n> [step 1/<n>] [name <take>] [sink none\|ffmpeg\|png\|raw] [statelog on\|off]`, `capture off` | a take (capture mode, below): the game steps by exactly 1/n a second and every frame is read back; `name` is the take's folder, `sink` where the frames go, `statelog on` asks for `state.csv`; the take ends at `capture off` or the scene's `end` |
| `cam`, `cut` | the camera (below) |
| `car <id> spawn\|delete\|profile ...` | cars in free roam and the player's garage car ("Cars in a scene", below) |
| `car player at <x,y,z> [heading <deg>]`, `car <id> spawn\|ambient ... near <car> <right>,<up>,<forward> [heading <deg>]` | put the player's car at an exact place, and place cars in another car's frame ("The player's place and cars near others", below) |
| `car <id> ambient <model> at <x,y,z> [heading <deg>]` | a real ambient (traffic-class) car, placed through `AmbientMgr_AddAmbientToLayout` and taken back through `AmbientMgr_RemoveAmbientFromLayout` ("Ambient cars in a scene", below) |
| `car <id> path <route file> [speed <x>] [mode brain\|kinematic]`, `car <id> stop` | a scene car drives a route, or stops ("Cars on a route", below) |
| `hud off\|on\|<alpha>`, `hud fade <a>..<b> to <x>`, `hud fade over <s> to <x>` | the game's own UI shown at an alpha ("The game's own UI on and off", below); `<x>` is `off`, `on` or an alpha |
| `start` | kept for the preamble package of the toolkit; read and linted, not applied yet |

The log lines start with `[scene]`: `armed`, `loaded` (with the counts), `start`, one line for every key as its time comes, a `state` line every 2 s that puts the scene's hour, weather and time scale next to what the game reports, `finished`. Natives are called one or two an update, on the game thread.

**What a scene cannot set.** A scene file is data that gets sent around, so `set` is never a way to start a program or write a file. The parser refuses the line (the scene does not load, and the log and the lint say `a scene does not set mcla_capture_ffmpeg: it names a program, a path, a server or arguments, or makes the game write a file of its own (put it on the command line)`) for:

- every `mcla_capture_*` setting (the take's program, its arguments, sink and folder; a scene starts a take with its own `capture` key, and where it goes is chosen on the command line) and `mcla_scene*`;
- a setting that names a program, a path, a folder, a server or arguments: a name ending in `_path`, `_dir`, `_folder`, `_file`, `_root`, `_exe`, `_args`, `_command`, `_csv`, and by name `cl`, `user_name`, `import_vinyl`, `rmr_server`, `online_master_server`, `server_browser_servers`;
- a setting that makes the game write a file of its own in the game's folder: a name with `dump` in it or ending in `_probe`, `_diag`, `_trace`, `_census`, `_telemetry`, and by name `export_vinyl`, `capture_vinyl_shapes`, `lzx_stats`, `menu_cam_record`, `wheel_learn`, `mcla_gpu_trace_frame`, `mcla_native_gfx_capture`, `mcla_state_log` (the update log's file), `mcla_freecam_record` and `mcla_freecam_out` (the flight recorder's) and the like;
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

### The free camera and the flight recorder

A scene's camera is easier to fly than to type. The free camera is a camera the pad flies, detached from the car, and the recorder writes the flight down so that `ports\scripts\scene_cam.py` can turn it into a `cam` track. Nothing changes until it is armed: the camera hook costs a flag test and the pad wrapper one atomic load.

```powershell
larecomp.exe --debug_menu --mcla_freecam=true       # or DEBUG menu > TOOLS > TRAILER FREE CAMERA (close the menu to fly)
```

The camera starts where the game's own camera is. The game gets no pad input while it flies except Start, so the car does not drive and the pause menu still opens; a paused game, the front end's shots and a loading screen leave the game's own camera and pad alone, and the flight goes on from where it was when the game runs again. Turning the camera off gives the game's camera back (a cut, announced to the renderer like the scene camera's).

| Control | What it does |
|---|---|
| left stick | move: forward and back along where the camera looks, left and right level |
| right stick | look: yaw and pitch (right turns right, up looks up) |
| right / left trigger | up / down |
| L3 / R3 (stick clicks, held) | fast (x4) / slow (x1/4) |
| D-pad left / right | the base speed one step (x1.25) per press |
| D-pad up / down (held) | zoom in / out, even in focal length |
| LB / RB (held) | tilt the horizon left / right |
| Y | level the horizon and bring the field of view back to where it started |
| Back + A | start or stop recording |
| Back + X | home: fly again from the game's own camera (a cut) |
| Back + Y | mark a cut in the recording |
| Back + B | add a camera key here (the Director panel's `camera.key.add`; the Director must be on) |
| Back + RB / LB | go to the next / previous camera key of the scene |
| Back + Start | play or pause the scene (Start itself stays the pause menu; with Back held it does not reach the game) |
| Back + D-pad right / up | one frame on (the fixed step) / render the take |

With Back held LB and RB are chords and do not tilt the horizon, and the D-pad does not change the speed.

**The Director panel** (`mcla_director_panel`, DEBUG menu > TOOLS > DIRECTOR PANEL, or Ctrl+F12; it offers to switch the Director on) is a dock over the picture for setting a shot up with the mouse and the keyboard: a timeline with the scene's keys to drag, camera keys by framing with the free camera (W A S D, Q E, Shift fast, Ctrl slow, the right mouse button looks, the wheel steps the speed), rigs with sliders, cars to spawn at the crosshair and to drag, routes to record by driving or to drop as waypoints, the routes and the camera's path drawn in the world, and a button for the take. Every action is a command of the Director (`documentation\DIRECTOR-PROTOCOL.md`) and so a line of the scene file; the dock's footer prints the command of the last action. The dock takes about a third of the window whatever its size (the font and the spacing follow the window, so a small window keeps most of its picture and a 4K one gets a big dock); SIZE in the bar steps small, normal and tall, COLLAPSE leaves the bar alone, a click on a lane's title on the timeline folds that lane to a thin strip, and the tabs scroll with arrows when they do not fit. The keyboard stays the game's too (F2, F4, F9, F10 and F11 work as always); the panel's own keys are listed at the top of `src\mc_engine\ui\director_panel.h`.

The sticks have a dead zone and a bent response (fine control near the middle). Every control runs through an inertia of 0.25 s that is exact in time: the same flight covers the same distance at 60 and at 240 FPS.

**Recording.** `mcla_freecam_record` (row RECORD FLIGHT, or Back + A) writes one sample per game update into a raw track file while the camera flies. `mcla_freecam_out` names the file (an existing one is replaced); empty, it is `freecam\flight-<date>-<time>.cam.csv` next to the game. Rows are flushed twice a second. The file is text:

```
# larecomp freecam v1
# started 2026-11-13 14:00:00 / game frame 48211 / scene none / seed ...
t,scene_t,frame,x,y,z,yaw,pitch,roll,fov,flags
0.000000,-1.000000,48211,819.6120,17.6000,-808.4000,-28.0000,0.0000,0.0000,50.0000,0
```

`t` is seconds since the recording started, summed from the engine timer's frame deltas before the time scale, which is what a scene's clock adds, so a scene whose camera track holds this flight's keys moves the camera the same way from its own 0. `scene_t` is the scene's clock when one runs (else -1): a flight flown against a scene's hour ramp can be keyed to that scene's time. Angles are in the scene file's words (yaw 0 along +Z, pitch + up, roll + lifts the right side), yaw is continuous. The game's world is right-handed (x east, z south), so a growing yaw turns the camera to the left and a turn to the right lowers it; the flight, the scene's camera and `scene_cam.py` share that one basis, so a recorded pose replays exactly (`freecam_test` round-trips the game's own camera rows through the file and a scene key). The camera slots hold the rows right, up and BACK; the flight reads and writes them that way, like the scene camera. `flags` bit 0 starts a new shot (home, Back + Y).

**From a flight to a scene.**

```powershell
python -I ports\scripts\scene_cam.py convert freecam\flight-20261113-140000.cam.csv -o flight.scene
python -I ports\scripts\scene_cam.py compare freecam\flight-20261113-140000.cam.csv --scene flight.scene
```

`convert` cuts the flight into shots (the flagged samples and any jump), smooths each (a zero-phase Gaussian, `--smooth` seconds, the ends mirrored so a path does not shrink), and keeps the fewest keys that stay within a bound of the replay: the replay is the game's own (a centripetal Catmull-Rom through the position keys, a slerp between the aims, linear roll, field of view linear in focal length), so the bound holds against what the game will do. Each channel (position, aim, roll, field of view) is keyed on its own; the defaults are 0.25 m, 0.5 degrees of aim, 0.75 degrees of roll and 0.4 degrees of field of view (`--tol-pos` and the rest; looser gives fewer keys). The first comment lines of the output say what the bounds gave against the smoothed flight and against the raw samples. `--time scene` keys the flight at the scene's own clock, `--start` moves it, `--lines-only` writes only the `cam` and `cut` lines.

The edit helpers touch only `cam` and `cut` lines (comments and every other statement stay): `shift --by S` (`--with-end` moves the `end` too), `speed --factor F` (2 is twice as fast), `trim --from A --to B [--rebase]` (keys inside the window, new ones at both ends, and the keys the ends need to keep the path), `graft base.scene track.scene` (the base scene's world and events with the track's camera) and `info`. `trim` and `compare --scene` replay the track here, so they take a track of world keys (`pos`, `yaw`, `pitch`, `roll`, `fov`) and refuse `attach`, `orbit`, `look`, `follow`, `near` and `far`.

`compare --log larecomp.log` checks a replay in the game: run the scene with `--mcla_cam_log=1` and the tool reads the `[cam] t=...` pose lines and compares each with the flight interpolated at that time (`--max-pos`, `--max-aim`, `--max-roll` and `--max-fov` make it exit 1). `verify track.scene larecomp.log` asks the other question, whether the game plays a scene's camera the way the tool's replay (and so the converter's bound) says: the same log lines against the replay of the same scene, with tight bounds. `python -I ports\scripts\scene_cam.py selftest` runs the tool's own checks, which include numbers printed by the game's rig.

| Setting (debug only, live) | What it does |
|---|---|
| `mcla_freecam` | arms the free camera |
| `mcla_freecam_record` | records the flight (only while the camera flies) |
| `mcla_freecam_out` | the file of the next recording |
| `mcla_freecam_speed` | metres a second at a full stick (12) |
| `mcla_freecam_look` | degrees a second at a full right stick (70) |
| `mcla_freecam_inertia` | seconds the movement takes to settle (0.25; the turn and the tilt take half; 0 is none) |
| `mcla_freecam_log` | log the pose every this many frames |

The log lines start with `[fly]`: on and off, the seed, home, cut marks, `recording to <file>`, `recording ended` with the samples, seconds, metres flown and cut marks, and the converter's command line.

`cmake --build <dir> --target larecomp_freecam_test` builds the checks of the flight, the pad, the chords and the file format (no game needed). `larecomp_freecam_test --synth f.cam.csv` flies a scripted pad flight and writes it, `--raw f.cam.csv` reads a file, and `--compare f.cam.csv f.scene` evaluates a scene's camera track with the game's rig at every sample's time and prints how far it is from the flight: the whole path from a flight to a replay can be tried without the game.

CHECK (nothing here has run in the game yet): that the camera hook runs once per update (the flight steps on the frame counter, so a second run in one update only rewrites the camera); that the XamInputGetState wrapper sees the pad every update and that clearing the guest copy keeps the car still (the car coasts); that the unscaled frame delta at `0x827D7558` is the delta of the update the camera is built in; that the game's own camera in the slots at the hook is a good starting point in every mode; that Start (pause) and the loading screens behave as written above.

### Cars in a scene (spawn, delete, the player's garage car, poses)

The `car` statements of a scene put cars into free roam and dress the player's own. Nothing runs unless a scene has car keys (one flag test an update); they are `mcla_scene` features, so they are debug-only like the rest.

```
at 0 car p1 spawn vp_lam_murcielago_06 at 812.5,17.6,-799.5 heading 62    # a car by the game's vehicle name
at 0 car p2 spawn profile 1 at 818.0,17.6,-797.0 heading 242               # garage slot 1 of the save, dressed as the owner left it
at 0 car player profile 3                                                  # the player's own car becomes garage slot 3
at 20 car p1 delete
```

`at` takes `x,y,z` (or three numbers): a world position, x east, z south, y up. `heading` is where the nose points, in degrees, as the camera's yaw: 0 along +Z, 90 along +X (the game's own `Racer_Teleport` angle is this plus 180; the log shows both). Garage slots count from 0 (`garage:N` and `slot:N` also mean `profile N`). The id is what the camera's `car:<id>` will name; `player` takes `profile` and `at` ("The player's place and cars near others", below). A car is made the way E4 proved in free roam: `OpponentManager_CreateOpponent`, `LoadOpponent` (or `LoadOpponentByCarConfig` with the garage's config), `AddOpponentToWorld`, wait until the load state is 3 and the car is streamed, `Racer_Activate`, `Racer_SetStreamingLockedIn`, `Racer_Teleport`, then `Racer_GetPosition` as a check. That takes about 4 s: a car exists from the log line `car p1 is ready`, so give its spawn key a few seconds before the shot that needs it.

**Safe point.** The scene runner only queues the keys; `TickCars` (one line in `Patch_DeltaTimePre`, after `TickScene`) makes the calls on the game thread, one native an update at most, and only while the player's car is on the road, the ambient and opponent managers exist, the game is not paused and that has held for 10 updates. A racer is looked up in the opponent manager's active list before a call is made on it: if the world was rebuilt under the scene (a race, the front end) the car is dropped from the books with a warning and nothing is called. A name the game's car table does not have, a garage slot that is not there and an empty opponent pool each fail that car with a warning and nothing else; a car that is not streamed in after 20 s is deleted again.

**The end.** Every car the scene spawned is deleted when it ends (also on a restart, a reload and when `mcla_scene` is cleared), after the spawn has run to its end (a half-made opponent is never deleted: E4 only deleted finished ones), and the player's car goes back to the garage slot it wore before the scene changed it. Both take a few seconds after the scene's `finished` line and the log says so (`[cars] ... is deleted`, `[cars] nothing left to do`).

**Poses (for the camera).** `src/mc_engine/cinematic/cars.h`: `GetCarPose("p1", &pose)` and `CarMatrixById("p1", matrix)` give the position, the axes and the heading of the player (`"player"`) and of every scene car, read from the car entity's world matrix (`racer+48 -> entity, +28 -> matrix, +16`: right, up, back, position) at the moment of the call, with the last good pose as the fallback; a registry (`car_registry.h`) holds each car's last pose and the update that read it. `racer+0x470`, the race cars' own position copy, holds a fill value outside a race and is not used. `CarMatrixById` has the shape of the camera package's `CarResolver` and is installed as its car source when that package is in the build. The log writes a `[cars] pose <id> (x, y, z) heading h` line every 2 s for each car that has one, and `[cars] <id> placed:` once after the placement with the matrix, `Racer_GetPosition` and what the scene asked for side by side.

The log lines start with `[cars]`: the scene's keys as they come due, every native with its arguments and result (so the last line before a crash names the call), `ready`, `pose`, `placed`, `is deleted`. `cmake --build <dir> --target larecomp_cars_test` builds the checks of the statements, the pose maths, the registry and the whole spawn / delete / garage script against a pretend game (about 400 checks with the paths below); they need no game.

### The player's place and cars near others (`car player at`, `... near`)

A scene's cars used to stand at world positions, but the player's own car is wherever the save left it, so a test that wanted cars beside him missed by tens of metres. Two statements fix that:

```
at 0   car player at 832.19,16.9,-787.53 heading 62             # the player's car is put there (Racer_Teleport on his racer)
at 0.5 car n1 spawn vp_lam_murcielago_06 near player -3.35,0.3,0   # 3.35 m to his left, 0.3 m up, level with him, the same way as him
at 0.5 car n2 spawn vp_aud_r8_08 near player 3.35,0.3,0 heading 180   # on his right, facing back at him
at 0.5 car n3 spawn vp_chv_camaross_69 near player 0,0.3,-9.5     # 9.5 m behind him
at 2   car a1 ambient va_gmc_yukon_05 near n1 0,0,-8             # ambient cars and cars near a scene car work too
```

*`car player at <x,y,z> [heading <deg>]`.* The one statement that moves the player's car. It makes one `Racer_Teleport` on his racer when the line runs (the native a spawned car is put with: the matrix is set, the speed is 0, there is no ground snap), so give a `y` a little above the road (0.3 to 0.5 m) and the car settles onto it. Without `heading` he keeps the way he faces. It waits for the player's car to be on the road and for the garage dressing (`car player profile N`) to settle (3 s), and it is made before any spawn in the same update. The log has the call (`[cars] player Racer_Teleport racer 0x... at (...)`) and, once his matrix has been read again, `the player's car stands at (...): the call asked for (...)`.

*`near <car> <right>,<up>,<forward>`* (instead of `at`, for `spawn` and `ambient`). The place is in the named car's own frame: metres to its right, up and ahead (the frame is the car's matrix, so a car on a slope has it tilted with it). `<car>` is `player` or the id of a car the scene made earlier. It is worked out when the line runs, from the pose that car has THEN: a line `near player` waits until his `car player at` has been made and his matrix read again (so two lines at the same scene time work in file order), a line near a spawned car waits until that car is ready, near an ambient car until it stands. The wait makes no native call and is logged once (`car n1 (near player) waits for ...`); after a minute the car is dropped with a warning. When it resolves the log says where the other car stood and where this one goes (`car n1 near player: player is at (...) heading 62.0; -3.35 right ...`). `heading` after `near` is RELATIVE to the other car's heading: 0 (the default) is the same way, 180 faces it, 90 is a quarter turn to its left (a scene heading grows counter-clockwise seen from above). A car's width is not known to the scene: two cars 1.5 m apart side by side have their centres half-width plus half-width plus 1.5 m apart (3.35 for two 1.85 m cars). `near` and `at` do not go together; `near` is refused for an id that is not there yet.

*The put-back (what was chosen).* The player's pose before the scene's FIRST `car player at` is kept, and when the scene ends he is put back there with one more `Racer_Teleport`, but only when that is safe: it is the same racer (the world was not rebuilt, the car was not changed), he is still within 5 m of where the scene left him (somebody who drove off is not fetched back; the log says how far he is), the game is on the road and not paused, and the garage dressing is done. If any of that fails the log says `the player's car is not put back: <why>` and he stays. A scene that never moved him puts nothing back. A second scene that begins while the put-back still waits drops it and keeps the origin, so the second scene's end puts him back where he stood before the first.

`cmake --build <dir> --target larecomp_cars_test` has the checks of both statements: the grammar, the frame maths (headings 0 to 331.7, a tilted car, the relative heading), the place and the put-back against a pretend game (a driven-off player, another racer, no pose, a dressing, a scene that ends first), and near cars waiting for a player, a spawned car and an ambient car.

### Ambient (traffic-class) cars in a scene (`car <id> ambient`)

`car <id> spawn` makes a racer (the opponent manager's car: its own shadow shader, its own draw). The game's TRAFFIC is a different kind of car, an ambient car of the manager's pool of 98, drawn by the ambient item draw, with the traffic shadows, the traffic LOD and no driver of ours. A test of anything that treats traffic differently (the traffic sun shadow's rotation, the rim round it, the traffic reflections) needs one of those, and `ambient` makes it:

```
world traffic off police off peds off hour 17 hold weather nice    # an empty road
at 2 car a1 ambient va_gmc_yukon_05 at 817.0,17.2,-797.3 heading 45  # a real traffic-class car, standing there
at 12 car a1 delete                                                  # or it goes when the scene ends
```

`<model>` is one of the ambient manager's types, a lower-case name up to 31 characters (the layout item's name buffer holds 31 and a NUL): the traffic probe's `types:` lines list them (`va_gmc_yukon_05`, `va_mb_eclipse_04`, `va_dge_caravan_07`, `va_golfr32_04`, `va_cad_ctsv_04` stood on the probe's road; a name the manager does not know is refused when the car is placed). `at` and `heading` are the scene's, as for `spawn`: the position is a world point, the heading is where the nose points (0 along +Z, 90 along +X). The car stands still where it is put: no rail, no driver, no route (a `path` or `stop` on it is refused; spawn a racer for a route). The id is the camera's `car:<id>`.

*How it is made* (the traffic probe proved it, `perf\out\TRAFFICPROBE.txt`, 2026-11-19): `AmbientMgr_AddAmbientToLayout` (0x826D6850; the model's name, a position, an angle in degrees) takes a slot from the layout pool (512 slots, class 0) and returns its id. A layout item is a placement: the game's PARKED spawner (`sub_826F23A0`, run for every zone each update) turns it into a real car of class 0 within a second, about 0.5 m from the point, marked as placed so that the unspawn pass leaves it alone. It did not go in 76 s. The layout's angle is the one `Racer_Teleport` takes, so the scene's heading is turned by 180 degrees on the way in (`LayoutHeadingDeg`: the probe asked for 272 and the car stood at heading 92). The log's `stands at` line, written the first time the car's matrix reads (the layout item's bound car, `+76`, and its update matrix at `+128`), says where it really is next to where the scene asked: the car stood 0.4 m higher than the asked y in the probe, so ask for the street's height minus 0.4.

*What it needs.* The parked spawner must be open: `world parked off`, `parked off` or a parked scale of 0 before the car is asked for gives a parser warning (and no car), because the game makes the car through that spawner. Leave `parked` alone or give it a scale above 0; the spawner's own per-zone limit (`max(1..2, factor x count)`) still applies, so many cars at once may not all appear (CHECK: four at once). `traffic off`, `peds 0` and `police off` do not matter to it: the placed car stayed with the traffic spawner closed in the probe's run. The scene key should come a second or more after a `traffic off` (the native's despawn message is queued: CHECK that it does not take a car placed in the same second).

*The guards* are the probe's, with the cap made a scene's: free roam only (the game's Cruising state), the game not paused, the world up for 120 updates, a layout slot free with two to spare, at most 8 placements in a scene, the model in the manager's type list, the point within a kilometre of the player. A guard that can pass (the world is not up yet, a menu is open) makes the car wait, logged once, and it gives up after 90 s; one that cannot (the pool is full, the cap) fails the car at once, logged as a warning, and the rest of the scene runs.

*The clean-up.* `AmbientMgr_RemoveAmbientFromLayout` (0x826D5CD8; the layout item's id) is what the game's own scripts use to take a placement back. Read in the generated code (`sub_826D5CD8` -> `sub_826E1A20(layout, id, 15)`): it finds the item by its id in the layout pool's lists, clears it, sends the ambient manager the despawn message for the bound car (`sub_826D50E8`, reason 15), cancels the spawn ticket of an item whose car is not made yet, and gives the slot back to the pool. One call for each car, made when the scene ends (also on a restart, a reload, when `mcla_scene` is cleared) or on `car <id> delete`, one native an update; the log says `car a1 (ambient) is removed: layout item N given back` and `[cars] nothing left to do`. If the ambient manager was rebuilt under the scene (a new world) the placement went with it and nothing is called. CHECK: the probe never called the remove, so the first run says whether the car really goes (the picture, and the traffic probe's pools: `mcla_traffic_probe 60` shows the layout pool's used/live counts fall).

`cmake --build <dir> --target larecomp_cars_test` has the checks of the verb, the angle (the probe's 272 and 92), the guards, the machine of one car and the director's order with ambient cars, racers and a rebuilt world (about 150 of its 578 checks).

### Cars on a route (path, stop)

```
at 9 car a path m4c-a.route                       # kinematic (the default): the car is put on the route at every update
at 9 car b path routes/lane.route speed x1.5      # the route's own speeds times 1.5
at 9 car c path m4c-brain.route speed 8 mode brain # the game's own AI drives; 8 m/s
at 20 car a stop                                  # the car stops where it is
```

A path is for the cars a scene spawned (`player` is driven by the test harness's `follow`, which writes the pad). It starts when its key comes due and the car is ready (a key before that makes the car start where the route is by then: path time runs from the key). `stop` ends it; so does the route's end (the car stands on the last point). A car can be given a kinematic path again after it ended; a car that had a brain path takes no other.

**The route file.** One point a line, `x,z[,speed[,y]]` (commas or blanks; `#` comments): the files of `ports\scripts\tours\routes` that `follow` reads, with an optional fourth number, the height. x east, z south, speed in m/s (12 when a point has none), at most 20000 points, 1 m long at least. The name is a relative path (no `..`, no drive) looked for beside the scene file, then in a `routes` folder beside it (a name with no extension also tries `.route`). The files are read when the scene starts, so editing one means restarting the scene; a missing or bad route is an error line at the start and its keys are refused. `speed 12` replaces the route's speeds with 12 m/s (0.5 to 120); `speed x1.5` (or `1.5x`) multiplies them (0.05 to 10). The speed between two points is linear, so there is no jump at a point, and a speed under 0.5 m/s is held at 0.5 so that a path always ends.

**Kinematic** (the exact one, what the camera wants). Every update, for each car with a path, one `Racer_Teleport(racer, place, heading, speed)` at the place the route has at the *path clock*: the scene's time with the scene's `timescale` applied (a car follows the slowed world, not the scene's own wall time). The place is a closed-form function of that clock (PathSampler: the time over a stretch of linearly changing speed is `L ln(v1/v0)/(v1-v0)`, and the place at a time is its inverse), so a take at 30 or 60 fps puts the car at the same place at the same time. The heading is the chord of the route from 2 m behind to 2 m ahead (a corner turns over 4 m); the speed is the route's, so the body and the wheels have the velocity of the drive (`Racer_Teleport` gives the car `speed` along its nose: sub_822AB838 hands 822A9930 the back row times minus the speed). The height is the route's if it has one, else the car's own: after a step of physics the car sits on the ground, and the next put keeps that height. CHECK: that holds on a flat street; `Racer_Teleport` sets the yaw only (no pitch, no roll) and a car that keeps its own height does not follow a slope (it floats on a long descent): give the route the fourth column on a hill. There is no native that freezes a racer's physics, and none that gives the ground height; the put each update is what is left. The calls are made after the spawn steps' one native, one for each car that is due, not one an update; the log is not flushed for them (the first and the last of each car are logged and flushed).

**Brain** (the game's own AI; for the look of a real driver, not the exact line). The route is cut into at most 48 destinations, at least 8 m apart, the last the route's end. The program, one native every 100 ms through the same director as the spawn steps: `BrainFactory_CreateBrainCruising(racer)`, `BrainCruising_SetSpeed`, `BrainCruising_SetDestSpeed` (both the route's mean speed, m/s: it is the number the brain hands `Racer_Teleport`), `SetLooping(false)`, `AppendDest` for each destination (the brain's array holds 64 and does not check), then `BrainCruising_DriveToNearestRail`, then `HasReachedGoal` every 0.5 s. A destination's height is the route's, else the car's. `stop` calls `BrainCruising_RemoveFromTraffic` (back to idle, brake held); a car's delete calls `BrainFactory_DeleteBrain` first (E4's order).

*Why the E4 brain stood still* (read from the generated code, CIN-E4-ROUND3.md left it open; CHECK until a run says so). `BrainCruising_AppendDest` (0x827460A8) stores the point at `brain+560+16*count`, counts it (`brain+1584`) and only sets the destination index (`brain+1656`) to 0 if it was -1: it never touches the brain's state, a word at `brain+1708`, which starts at 0. The brain's update (sub_82746340, slot 8 of its vtable at 0x820CEEF4) does nothing for state 0 but write the brake held and no throttle (`brain+1676` = 1.0, `+1680` = 0.0; the names of those outputs are not confirmed), and `HasReachedGoal` (0x82745830) is `state == 6`. What takes the brain out of state 0 is a different native: `BrainCruising_DriveToNearestRail` (0x827469D8 -> sub_82746108), `AttachToNearestRail` (0x82745FE0), `TeleportToNearestRail` (0x827469E8) or `TeleportToNextDestination` (0x82746A40); E4 made none of them. `DriveToNearestRail` needs the ambient manager (0x8286D8E4, which exists in free roam) and a traffic lane near the car (sub_826FD068 -> sub_826FC258 -> sub_82764568 searches the lane graph at 0x828DA3AC with 10.0 and 100.0 from the image's constants); it sets state 1 (a lane path was built) or 2, and state 4 when there is no lane, in which case the update does nothing but try to put the car on the lane nearest its next destination (sub_82745EE0, a `Racer_Teleport`-like move) every update. So the fenced lot and the destinations over the buildings would have kept it at state 4 even with the go; with the lot it stood at state 0 for the plain reason that nothing told it to go. That brains run outside a race: the racer's update (sub_826FCBD0) calls `brain->vtable[8]` whenever `racer+68` is set, with no race test. The brain drives the lanes of the traffic network, so it follows the street, not the polyline: `mode brain` is for a car that looks driven, `mode kinematic` for a car that is where the shot needs it.

The log. `[cars] path <id> starts ...` (the route, its length and time), then every 0.5 s of path time `[cars] path a t=1.50 s=12.3/50.0 m v=7.1 m/s: route (x, y, z) h 62.0, car (x, y, z) h 61.8, off 0.21 m (ahead +0.15, side +0.14), 0.2 deg`: where the route is at the clock, where the car is (read at the start of the update, after the last put and a step of physics), how far apart, split along and across the route (a car that is always `ahead -2 v dt` would mean the speed has the wrong sign), and at the end `path <id> finished ...: N put(s), mean off ..., max ...`. A brain car logs `path <id> t=... brain state S (...) dest D/N; car (x, y, z) h; nearest route point s of L m, d m off` every 0.5 s and when the state changes (a change to 4 is a warning), and a summary with the states seen and the farthest it got. The checks are `larecomp_path_test` (the file, the sampler, the speed profile, the heading, the cut into destinations: 126 checks) and the path cases of `larecomp_cars_test`, which drives the kinematic and brain programs against a pretend game; `larecomp_path_test a.route ...` summarises route files. `scene_check.py` knows the verbs and looks for the route files. Examples: `scenes\cars-path.scene` (two kinematic cars on the street outside the Hollywood garage gate) and `cars-path-brain.scene` (the AI beside a kinematic car), their routes in `scenes\routes`; `perf\scripts\chain-m4c.ps1` runs them.

*A car whose matrix cannot be read.* The pose is the registry's last good read; a car whose matrix is refused keeps it. The path code does not take that for where the car is: a pose read more than 3 updates ago is not compared with the route (the progress line says `the car's matrix has not been read for N updates: no pose to compare`, the summary counts them, the car is put on the route all the same, at the height of its last fresh pose) and a brain's state line says the same instead of printing a stale position. `[cars] <id> the matrix has not been taken for 30 updates (racer, entity, matrix): <why>` names the step that refused (a pointer of the chain, a word that is not finite, a back row that is not 1 long), with the 16 words. The w words of the matrix (`m[3]`, `m[7]`, `m[11]`, `m[15]`) are ignored: `Racer_Teleport`'s wrapper (sub_822AF0F8) copies only x, y, z of the vector it is given to its own stack frame and sub_822AB838 loads the 16 bytes from there, so the position row's w word of a car the host has put is whatever the stack held, and the registry used to refuse a matrix for it (the first M4c run: `placed, but the pose cannot be read` for a put car while `Racer_GetPosition` answered, and a car whose pose stopped updating a second into its path). CHECK: that this is the whole reason (the first run after the change: no `has not been taken` line, and the `a w word ... is not finite` line says whether the words were NaN); a `has not been taken` line with a pointer step says it is something else. The registry also keeps the w words clean (0, 0, 0, 1) for the camera.

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
at 0 capture fps 60 name harbor sink ffmpeg statelog on   # from here: a take at 60 frames a second, into the folder "harbor"
at 0 hour 17.5 hold
end 10                       # the take ends with the scene (or at `capture off`)
```

- **Fixed step.** Every update gives the engine timer exactly `1/n` s in the game's own ticks (60 fps is 831250 ticks), whatever the frame took. The FPS limit, the step cap and frame pacing stand aside. The step is the capture rate (`step 1/<n>` may be written, and must say the same); slow motion is a `timescale` key with a higher capture rate, not another step. The path of a car depends on the step, so a take is tied to its rate.
- **Frame gate.** The game does not start update `k + 1` until frame `k` has been read back (the 1:1 present output, before the overlays, 8-bit SDR, so the F3 card and menus are not in it; the output must not be HDR and frame generation must be off). The frame of an update is told at the hand-over of the frame to the render thread, where that thread is idle, so a take always holds exactly the pictures of its updates: the first is the take's first update and the last is the update before the take's end gate. The log says `[cap] frame count: N frames for N updates (a..b): exact`, or `[cap] FRAME COUNT: ...` with what is off (and `[cap] TAG:` / `frame of update N lost:` for the cause). A wait that runs out is a `[cap] gate timeout`; three in a row switch the waiting off for the take.
- **Scene clock.** During a take the upscaler's history rule and the UI movies' pacing read frames times the step instead of the wall clock, so a slow capture is not a stall to them (`[cap] summary` shows `upscaler: ... history_restarts=N (gap_over_100ms=K)`: K is every frame without the clock). `mcla_capture_virtual_clock` turns it off.
- **Scene time in a take.** A scripted `capture` starts at the first whole step after the key's authored scene time. A debug capture uses the scene time at the request. The Director's `take.render` uses `from` as the exact first frame. In every case, frame `k` of an exact take uses `t0 + k * step` for the scene tracks and `frames.csv`. `take.json` records both in `scene.first_frame_scene_t` and `scene.step_s`, in seconds. The step is `1 / fps` and the guest timer uses the nearest guest tick.
- **No object left out.** While a take runs the renderer does not leave a draw out for a pipeline that is still being made in the background: it waits for the pipeline on the render thread (`mcla_gpu_sync_builds` in "Diagnostics that write to the log" is the same switch by hand; the take turns it on at its start and off at its end). A frame that makes a new pipeline is slow, and none misses a car, a shadow or a lamp's glow. The log says `[gpu] capture sync: on` and counts the waits.
- **The take folder.** Every take is a folder of its own, and a take never overwrites another (a taken name gets `-2`, `-3` ...). It holds everything that says what the take is, so it can be kept with its scene and compared with another take:

  | File | What |
  |---|---|
  | `take.json` | the scene (name, where it was, a hash), the exe (path, the commit the harness's `SET-SOURCE.txt` names, the version), the settings that differ from their defaults, the picture size, the rate (`step_ticks`), the sink and its output, the frame count, the first and last update, `dropped` and `repeated` (0 for a take that did what it should), the gate, the memory it held, the free RAM at the start, lowest and end, how it ended. Written when the take starts (`"state": "running"`, so a crash leaves it) and again when it ends: `complete` (one picture for every update, none dropped or repeated, the output is what the frames say), `inexact` (it ended, but a frame was dropped or repeated) or `failed` (it could not run or stopped: `error` says why). |
  | `video.mkv` | sink `ffmpeg`: the frames as one FFV1 file |
  | `frames/f000000.png` ... | sink `png`: one picture for every frame (ffmpeg writes them; a frame takes as long as ffmpeg needs to compress it, and the take waits) |
  | `video.bgra` | sink `raw`: B8G8R8A8 frames, rows packed, `width x height x 4` bytes each (sizes in `take.json`) |
  | `audio.wav`, `audio.csv` | the take's sound, in step with the frames ("The take's sound" below): 32 bit float, 48 kHz, as long as the video to the sample, and a row for every frame of what the mixer did. Not there when `mcla_capture_audio` is off or the game has no audio render client for the capture driver to go in front of. |
  | `frames.csv` | one row for every frame: its update, its scene time, what it cost (the frame/update map: frame `k` shows update `first_update + k` in an exact take) |
  | `state.csv` | only when the scene's `capture` key says `statelog on` (or `mcla_capture_statelog`): one row for every frame of the game's state as the frame shows it: scene time, hour, time scale, the update's step, the pause flag, the player's car position and a hash of its matrix. A take that matches another matches in these rows to the last bit. It holds no random-number or traffic state. |
  | `scene.scene` | a copy of the scene file the take ran |
  | `ffmpeg.log` | ffmpeg's own output (sinks `ffmpeg` and `png`) |

  EXR pictures are not written: the readback is the 8-bit picture, and a float format needs the HDR scene read back first.
- **Memory is bounded.** A take makes its GPU readback buffers and its host ring once, at the picture's size, from a plan that has to fit `mcla_capture_memory_mb` (2048) and the PC's free memory less `mcla_capture_ram_floor_mb` (1024). The ring is cut first (to 2), then the GPU buffers (to 2 in lock-step); a take that does not fit with 2 + 2 frames does not run, and the log says what it needs and what to change (`[cap] TAKE FAILED: a 7680x4320 take needs at least 506.3 MiB ...`). Nothing grows with the take's length or the writer's speed: a writer slower than the render fills the ring, the render thread waits for a free buffer, and the frame gate holds the game's next update. Only a writer that finishes nothing for the gate's timeout (ffmpeg stuck, a full disk) ends the take, with an error; the free memory is checked every 15 updates and a take stops when it falls below the floor. The buffers are freed when the take ends. At 4K the take holds 8 buffers of 31.6 MiB (253 MiB); at 8K 8 buffers of 126.6 MiB (1012.5 MiB). The only thing that grows with the length of a take is its books, about 130 bytes a frame (an hour at 60 fps is 28 MB). The plan covers the take's own buffers, not the game's render targets at a high render scale (that is what used up the PC's RAM at 8K): the renderer stops at scale 3 and `scene_run.py` never goes above it.

Settings (all debug-only and live; the sink and the files are read when a take starts). A scene cannot set any of them ("What a scene cannot set" above): they come from the command line, F4 or the console.

| Setting | Does |
|---|---|
| `mcla_capture_fps` (row CAPTURE FPS, Plume only), `mcla_fixed_step_fps` (row FIXED STEP) | a take, or the fixed step alone with no gate (for repeatability tests), at this rate; 0 is off. A scene's key wins over the take setting. |
| `mcla_capture_sink` | `none` (default: the readback and its copies are measured and dropped), `ffmpeg` (a pipe, FFV1 in mkv), `png` (ffmpeg writes one PNG a frame) or `raw` (B8G8R8A8 frames). A scene's `sink` wins. |
| `mcla_capture_dir`, `mcla_capture_name` | where the take folders go (`captures`, relative to the game folder) and the folder's name when the scene gives none (`name`; else the scene's own name, else `take`) |
| `mcla_capture_out` | the take folder itself, exactly (new or empty, else the take is refused): what the check chain and `scene_run.py` use |
| `mcla_capture_statelog` | write the take's `state.csv` (a `capture` key's `statelog on` does the same). Not the update log of "The state log and take_diff" (`mcla_state_log`, a scene's own `statelog on` statement) |
| `mcla_capture_ffmpeg`, `mcla_capture_ffmpeg_args` | the program and the output options of the mkv sink (default `-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0`) |
| `mcla_capture_ring`, `mcla_capture_lag` | host buffers between the readback and the writer (2 to 8, as many as the memory plan allows); how many frames the game may run ahead of the readback (0: lock-step) |
| `mcla_capture_memory_mb`, `mcla_capture_ram_floor_mb` | the hard cap on what a take holds; the free memory that has to stay free (0: no floor) |
| `mcla_capture_gate_timeout_ms`, `mcla_capture_delay_ms`, `mcla_capture_log_every` | how long nothing may move at the gate or in the ring before it counts as a timeout; an artificial wait per frame to try a slow capture; a `[cap] f=` line every n frames |

```powershell
larecomp.exe --debug_menu --mcla_scene=take-20s --mcla_capture_out=C:\takes\a --mcla_gpu_hdr=off
```

**One command for a whole take.** The test harness has `ports\scripts\scene_run.py`: it starts the game from a save, runs the tour to free roam, arms the scene (which makes the take), waits for the take to finish, checks `take.json` (state `complete`, frames = updates, nothing dropped or repeated, the output files) and prints the folder:

```powershell
python -I ports\scripts\scene_run.py --exe-dir ports\exe\b-m2b --scene ports\scripts\scenes\take-20s.scene --scale 3 --window 1920x1080
```

`--scale` is the render scale (1 = 1280x720, 3 = 3840x2160: never above 3). It adds a `run` block to `take.json` (the command, the save, the exe's size and SHA-256, the PC's free RAM before and after); `scene_run.py selftest` and `check-json <take.json>` need no game. `cmake --build <dir> --target larecomp_take_test` builds the checks of the take folder (names, the memory plan, the host ring on real threads with a slow and a stuck writer, `take.json` read back, the state log); `larecomp_capture_test` the step maths, the frame books and the clock; `larecomp_scene_test` the `capture` statement. None needs a game.

## The take's sound

A take also records the game's own sound, in step with the pictures. While a take runs the game's audio output is replaced by one that advances the mixer exactly as far as the video has advanced and writes what it mixed to `audio.wav` in the take's folder; when the take ends the real device gets its mixer back. Outside a take nothing changes, and until a take that records sound starts the game's audio is exactly the stock one: `src/mc_engine/audio_output.h` makes the real output (the SDL device, or the silent one) alone, with no wrapper in front of it, no extra thread and no log line. The capture driver is put in front of the first render client's output only when such a take starts (`[cap] audio: client N is the capture driver's: installed in front of the real output as the take starts`, once per game); from then on it passes every frame on untouched outside the take. The swap is one pointer of the SDK's client table written under the SDK's global critical region, which `AudioSystem::SubmitFrame` holds for every mixer frame, so the audio worker sees the real output or the driver and no frame is lost. A game that runs no take, and a take with `mcla_capture_audio` off, never get the driver: nothing for a player to hear, nothing in the log. No setting is needed on the command line for a take's sound: `mcla_capture_audio` is on by default, and it only matters once a take starts.

- **How the mixer is driven.** The game's mixer makes one 256-sample frame (5.33 ms at 48 kHz) for every credit the SDK's audio worker finds on a semaphore, and the real output gives a credit back when it has played the frame, so the mixer normally runs about 43 ms ahead of the speakers. At the start of a take the credits are taken out of the semaphore (the mixer stops) and what it had made ahead is thrown away. At the frame gate of every update the capture releases the credits that cover the video frames so far (3.125 mixer frames a frame at 60 fps), while the picture is read back, then waits for the mixer's frames and writes the frame's samples. The game thread is held at the gate, so the sounds an update asked for are the ones mixed next.
- **No drift.** Video frame `k` owns the samples from `round(k x 48000 / fps)` to `round((k + 1) x 48000 / fps)`, computed from the product, never as a running sum: 60 s at 60 fps is 2,880,000 samples and a rate that does not divide 48000 (59.94) stays within half a sample of the video's length for any length. The mixer's 256-sample frames do not divide a video frame, so the tail of the last one (under 256 samples) opens the next video frame: a sound the game starts in update `k` is in the first mixer frame mixed after it, never before its picture and under 5.33 ms after it. The file is cut to the video's length after every frame (`drift_samples` in the CSV is 0).
- **A stall.** A mixer frame that does not come within `mcla_capture_audio_timeout_ms` is given up on: a frame of silence takes its place, the one that arrives late is thrown away, the length stays exact, and the log says `[cap] audio: video frame N: the mixer gave no frame`. Three such steps in a row stop the waiting (the rest of the WAV is silence).
- **The end.** The credits go back into the semaphore (all of them, as at the start of the game) and the mixer refills the real output's queue by itself; the log says `[cap] audio: the mixer is back on the real output` at the first frame, and `[cap] audio summary ... samples=N expected=N (exact)`.

Files, in the take's folder with the others: `audio.wav` (32 bit float, 48 kHz, the stereo fold the real output plays, or the six channels of the game's 5.1 with `mcla_capture_audio_surround`; both are the SDK's own conversion, so they clip at 1.0 as the real output does) and `audio.csv`, a row for every video frame: the update, the mixer frames released / used / thrown away / silent, the samples written, the running total, the expected total and their difference, the tail of mixed samples not yet written (under 256), the wait for the mixer, the loudest sample, the game's audio clock (it gains 1 s per take second when 48,000 samples are a game second) and the position the emitter system was given as the listener.

| Setting | Does |
|---|---|
| `mcla_capture_audio` | on (default): a take's sound goes to the WAV and the real output is silent during the take; the capture driver is put in front of the audio output as the take starts (not before). Off: nothing is installed, the real output plays in real time during the take and no WAV is made (`[cap] audio: mcla_capture_audio is off`). Read when a take starts. |
| `mcla_capture_audio_surround` | the WAV has the six channels instead of the stereo fold |
| `mcla_capture_audio_timeout_ms` | the wait for the mixer's frames of one video frame (2000) |

What it cannot do: a take whose frames are more than 0.5 s apart can wait up to that for the mixer on each frame (the SDK's audio worker sleeps 500 ms after a wait that found no credit); a sound that times itself with the game's `audioFrameTimer` (it reads the host clock) runs in wall time, not take time; a take needs the game's own audio (`--mcla_audio_output=null` is fine, the capture works behind it). Nothing here has run in the game yet.

`cmake --build <dir> --target larecomp_capture_audio_test` builds the checks of the sample books and the WAV file (no game needed). The test harness has `wav_check.py <take folder> --press 600,1100,1600` for a take with a horn pressed at known take frames: the WAV's length, the CSV, the onsets of the sounds against the presses and against each other.

## The state log and take_diff (the trailer toolkit)

A take is only as good as the proof that the next one matches. The state log writes one CSV row per game update: the player's car (the entity's world matrix and the speed), the scene's cars, the camera block the game publishes, the hour and weather, the engine's clocks, the scene's own clock and the state of the random number generator. `take_diff.py` reads two of them and says *identical*, *identical within tolerance*, or the first row, column and values where they part. Nothing is read or written while no log is open.

Two ways to ask for it, both debug-only and live:

```powershell
larecomp.exe --debug_menu --mcla_scene=statelog-park --mcla_state_log=C:\takes\a.csv
```

- `mcla_state_log`: a `.csv` file, or a folder that gets `state.csv`. Set it and rows start with the next update that has a player car; clear it and the log is closed. A named file is overwritten; a folder that already holds `state.csv` gets `state-2.csv`, `state-3.csv`.
- A scene's `statelog on [dir <folder>]` (no time: it is an option of the whole scene; `statelog off` is the default). The log opens when the scene starts and closes when it ends, and a scene that starts again (`mcla_scene_restart`, a reload) is a new file. It goes to `<folder>\state.csv`, else `statelogs\<scene>-<date>-<time>.csv` next to the game (`statelog::SetTakeFolder` can name a take's folder for a caller that knows it before the scene starts; capture mode does not call it, because a take's folder is made when the take starts, after this log has opened). `mcla_state_log` outranks it. **This is not the take's own `state.csv`** (a row for every frame, written by capture mode when the `capture` key says `statelog on`: see "Capture mode"): that one is a short record of what each frame shows and sits in the take folder; this one is one row a game update with the cars, the camera block and the random number state, for `take_diff.py`. A take may ask for both; name a `dir` that is not the take folder.

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

## Before/after sweeps (the trailer toolkit)

A before/after clip shows one setting off and on in the same picture. Two ways to get it:

**One take, split in the engine.** `mcla_gpu_split` is a position from 0 to 1 across the picture's width (debug only, live; -1 is off and is the default). A per-pixel effect that honours it is applied left of the split and left out right of it, in the same frame, so a world that moves works too. `mcla_gpu_split_effects` (`all`, `ao` or `dither`) says which effects it cuts: ambient occlusion (the apply pass is drawn only left of the split) and the present's dither (the pass that writes the back buffer is drawn in two parts, the dither on the left one only). An effect that is off in its own setting stays off. A scene moves the split with `split <x>`, `split <a>..<b> over <s> [ease e]` and `split off`; the old value comes back when the scene ends. At the default nothing changes and nothing costs: one cvar read and a compare a frame. The edge sits on a whole column, `round(position x width)` of the render target for the occlusion and of the letterboxed picture for the dither; with an upscaler the occlusion's edge is filtered by it. There is no divider in the engine: the compositor draws it.

**Two takes, one compositor.** For a setting that is not per pixel (an upscaler, the render scale) the same parked scene is run twice and `ports\scripts\scene_cut.py` wipes between the takes: straight or tilted, held or moving and eased, a divider line, labels in the AO video's style, a lossless master (`.master.mkv`) and a share copy (`.mp4`). The takes must show the same world (a parked car, a held world: `scenes\sweep-parked.scene`); a driven take does not repeat. Left of the divider is the setting on, right of it off, as in the engine's split.

```powershell
python -I scene_sweep.py --exe-dir ports\exe\b-m5 --setting mcla_gpu_upscaler --on dlss --off off --name dlss      # two takes, then the wipe
python -I scene_sweep.py --exe-dir ports\exe\b-m5 --in-engine ao --name ao                                          # one take with the engine's split (scenes\sweep-split-ao.scene)
python -I scene_cut.py on.mp4 off.mp4 -o wipe --pos 0.15..0.85 --ease inout --label-left "DLSS ON" --label-right "DLSS OFF"
python -I scene_cut.py --one-take clip.mp4 --scene scenes\sweep-split-ao.scene -o ao-wipe                              # divider and labels over an engine-split clip
```

`scene_sweep.py` runs each take as one `opt_run.py` run through the main session's lockrun (`--plan` prints the commands and starts nothing). The take folder of capture mode is merged here, but `scene_sweep.py` still makes each take as `opt_run.py`'s clip of the game window (a take folder instead is a change to that tool); `scene_cut.py` already reads a take folder (its video, or its frames with `take.json`). `larecomp_split_test` (`src/gpu/split.h`: the columns and the two draws) and `larecomp_split_track_test` (the `split` statement and `EvaluateSplit`) check the pure parts without the game; `scene_cut.py selftest` and `scene_sweep.py selftest` check the tools on synthetic frames.

## The Director (agents and the panel)

The Director is one command layer under the trailer toolkit: a panel button, a scene-file line and an agent's request are the same named command. The whole wire format is `documentation/DIRECTOR-PROTOCOL.md`; the design is `feature-parity/DIRECTOR-DESIGN.md`. It is off unless `mcla_director` is on (debug only, live: on starts a listener on 127.0.0.1 only, off stops it).

`mcla_director_info` names a file the game writes `director.json` into while it listens (`{proto, pid, port, token, slot, exe, version, scene, started}`; the port is chosen by the OS, so two games never clash); `mcla_director_root` is the folder commands may write to (stills, takes, routes: relative paths without `..`), default the working folder. A scene cannot set any of them. A client sends `{"cmd":"auth","token":"..."}` first, then one JSON object a line: `{"id":1,"cmd":"transport.seek","args":{"t":3.2},"wait":"settled"}`; `{"cmd":"help"}` lists every command with its arguments. `ports\scripts\director_client.py` is the Python client and a mock server that speaks the whole protocol with no game (`python -I director_client.py selftest`).

What it does in the game (every part is inert while the setting is off):

- **Transport.** A pause or a seek holds the scene clock and freezes the world at the engine's time scale floor; a seek pins the clock at a time, which is exact for what is a function of the scene time (camera, kinematic cars, hour, weather, hud, split) and reports `scrubbable:false` when a brain-mode car, traffic or peds are in the scene. A step runs exact updates at the fixed step (`mcla_fixed_step_fps`, set by `transport.mode`). Before t = 0 the clock waits until the scene's cars are made and the streaming is done (30 s at most).
- **Soft reload.** While the Director runs, a changed scene file (or an edit by the Director) replaces the running scene in place: the clock and the cars stay, the settings of the old scene go back and the due ones of the new scene are taken, the keys already due are not played again, a car whose statements are unchanged is left alone, a car whose path statements changed gets them again, any other changed car is deleted and made again. The camera's rig starts over (one cut). Not while a take runs (the file is then loaded as before: the scene starts over).
- **Edits.** `scene.edit` takes line-addressed ops (insert in time order, replace, delete, move a key's time, param) against a revision; comments and layout stay, a stale revision or `expect` text is refused with the line's actual text, the file is written atomically and every revision is kept in `<scene folder>\.director\<name>\rev-N.scene` (`scene.undo` and `scene.redo` use it).
- **Stills and takes.** `shot` writes one frame as a PNG at the engine's resolution with no overlay or virtual clock. `shot.schedule` takes stills at scheduled scene times during a play-through. `take.render` restarts the scene, waits for it to settle and renders a take starting exactly at `from` and ending before `to`.
- **The free camera.** `camera.set` and `camera.look_at` place the same free camera the pad flies; `camera.mode game` forces the game's own camera.

`larecomp_director_test` (the JSON, the command engine, the console grammar, the line server over real loopback sockets, the projection, the route reducer, the PNG writer) and `larecomp_scene_edit_test` need no game.

## Adding a debug setting

A new cvar is one of three things, and the check below fails until it is:

1. a row a player can reach (pause menu, or the launcher), with no `.debug_only()`;
2. on the exception list in `tools/settings_exposure.toml` (a setting that stays an ordinary setting without a pause-menu or launcher row, such as the key bindings and the online choices; it needs the owner's say);
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
4. a cvar is defined twice and only one definition has the tag.

```powershell
python tools\check_settings_exposure.py        # every cvar
python tools\check_settings_exposure.py --dirs gpu mc_engine/hooks   # only these folders
```

Run it before you push. A short `pending` list in `tools/settings_exposure.toml` names debug-only cvars whose old row is still in a normal table; it shrinks as those rows move.

`tools/check_menu_rows.py` holds every pause-menu page, the DEBUG pages included, to the row cap (`kMaxRowsPerMenu` in `pause_menu.cpp`, 24): it counts the rows of each page from the sources (the `#if` blocks and the NativeOnly / XeniaOnly tags resolved for every build) and exits 1 on a page over the cap, because such a page is refused whole and simply does not open. `tools/check_row_tags.py` runs it too (rule T5). Move rows to a page with room, never raise the cap: nothing past about 14 rows has been looked at in the game.

`tools/check_row_tags.py` keeps the words on the rows honest (the owner wants settings to apply at once, so a word that says RESTART must be true). It exits 1 when a pause-menu row says RESTART for a setting that is not restart-only, or when a row of a restart-only setting says nothing (RESTART or NEXT START, or a dynamic suffix); `--catalog <SettingsSchema.cs>` holds the launcher's Restart tags to the same rule, and `--selftest` checks the rules themselves. A setting that is live but has a part that is read at start (LANGUAGE) is on `PARTLY_LIVE` in the script, with the reason.

```powershell
python tools\check_row_tags.py
python tools\check_row_tags.py --catalog <launcher>\src\LARecomp.Launcher.Core\Settings\SettingsSchema.cs
```

`tools/check_live_readers.py` keeps the other half honest: a setting that is not restart-only promises that a change is seen now, and the tag is only a label. The check looks, for every such setting that a player or a debug row can reach, for a reader that follows it (a `LiveText`, a change callback or watch, a read per frame or per call, a table of names read by a variable) and exits 1 for a setting whose every read is at start (a constructor, a `static` initializer, `std::call_once`): make it follow the setting, or tag it `kRequiresRestart`. A setting that is read when a feature starts by nature (a calibration, a recording) goes in `tools/live_readers.toml` with the reason. `--selftest` checks the rules on a made-up tree; `-v` prints the evidence for each setting.

```powershell
python -I tools\check_live_readers.py
python -I tools\check_live_readers.py --selftest
```
