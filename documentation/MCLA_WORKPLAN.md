# LARecomp Work Plan

Running record of what has been fixed, what was measured, what was ruled out, and what is still open.

Engine-level detail lives in [`TECHNICAL_NOTES.md`](TECHNICAL_NOTES.md). This file is the decision log: it exists so the same ground does not get covered twice.

**Conventions.** A claim without a measurement behind it is marked as a guess. Anything disproven is kept rather than deleted, in the "Ruled out" section at the bottom.

---

## Current state

The game boots, plays, and holds 60 FPS. Physics, camera and traffic behave the same at 30, 60, 120 and 144 FPS.

| Area | State |
|---|---|
| Frame timing above 30 FPS | Solved. Two fixed-timestep paths hooked, accumulators intact. |
| Frame pacing | Solved. The 15.625 ms grid was Windows timer granularity. |
| Camera and suspension at high FPS | Solved. Continuous-time decay against the 30 FPS reference curve. |
| Ambient density tuning | Working since the hook moved to the constructor epilogue. It never fired before that. |
| Texture cache | Closed. `texture_cache_misses` sits at zero. |
| Long-session frame rate collapse | Root cause identified in the SDK. Workaround in place; upstream fix pending. |
| Audio jitter in dense areas | Open. Four causes eliminated by measurement. |
| Dithered alpha on shadows and foliage | Open, and the only rendering issue left. Car reflections and HUD glitches were fixed in the custom ReXGlue build. |

---

## Solved

### Frame timing above 30 FPS

The engine substitutes a fixed 30 Hz timestep in two places, and patching one leaves the game at double speed. `MCLAUseRealDelta` at `0x821BDB58` skips the fixed-step overwrite; `MCLAFixedStepPath` at `0x821BDB90` replaces the loaded fixed step with the measured delta so the accumulators keep advancing.

The `[r3+56]` reset guard is preserved, unlike in the Xenia patch. Skipping it feeds a garbage delta on reset frames. With the hitch clamp also removed, that produced a loud audio blowout.

Verified rate-invariant: the summed substep delta reads 2.00x at both a 30 and a 60 FPS cap. The 2.00 is an artifact of three passes per frame; what matters is that it does not move with frame rate.

### Frame pacing

Frame times sat on a 15.625 ms grid with empty buckets between clusters. Ruled out the display, vsync at 60 Hz, and the guest vblank rate by sweeping `video_mode_refresh_rate` across 30, 60, 120 and 144. It was Windows' default timer resolution.

Fixing it needs `timeBeginPeriod(1)` **and** vsync off. Either alone leaves the grid. Together: 95% of frames on-grid drops to 34%, and throughput rises from 37.2 to 48.4 FPS.

Because vsync off removes the only throttle, the limiter sleeps to a wall-clock deadline. A vblank-based limiter would put the grid back.

### Camera, chassis and ground depth at high frame rates

Chase camera position and look-at (`0x82320468`, `0x823204F4`) and the chassis ground depth filter (`0x82563720`) all stepped a per-frame constant, which changes behaviour with frame rate. Replaced with continuous-time exponential decay:

```
S(dt) = 1 - (1 - 0.5 * S_raw) ^ (30 * dt * scale)
a(dt) = 1 - 0.90 ^ (30 * dt)
```

Rate-invariant by construction, because decay compounds.

### Ambient density tuning

The hook sat at `0x826F4E3C`, after the `density_tuning.xml` parse, and **never executed**. A three minute gameplay session logged zero firings while the midnightclub fork, which hooks the constructor, reported real values every run.

Cross-references settled it: `sub_826F4CB8` is reached only from `sub_826D89F0`, never from the zone builder `sub_826D8E70`, and `sub_826F5CB0` contains no parse call at all. Moved to `0x826F5CA0` with `r3`. Verified: 32 zones apply, stride 0x1730, values matching the reference fork.

### Settings that were disabling each other

`MCLAFrameDelta` gated `EnforceFrameLimit()` and `UpdateCityLODMemory()` behind `real_frame_delta`. Turning REAL FRAME DELTA off therefore also killed the FPS LIMIT row and the CITY LOD slider without saying so. Both now run unconditionally, as does the hitch clamp.

The option itself was called "60 FPS" and sat directly above "FPS LIMIT". Renamed to `real_frame_delta` / **REAL FRAME DELTA**, since it controls delta injection and is not a frame rate cap.

### Pause menu crash on SAVE SETTINGS

`sub_82661210` dereferences `[list+0xC0]` at `+0xC` without a null check, and the custom row installer deliberately clears that field. Guarded at `0x8266126C`, jumping to the function's own `li r3, 0` early-out at `0x82661290`.

Only the save row triggered it, because it was the one row that relabelled itself and forced a re-render. The row is gone: settings now persist when you leave a submenu, and only when a value actually changed. Writing on every change was rejected because `SaveConfig` rewrites the whole file and a slider moves several times a second.

### Log noise

`EnsureStringTableText` creates an entry when the patch misses, so a miss is the normal path, but it logged at warning level. That produced about 180 warnings per session and buried everything real. Downgraded to debug; a null table still warns.

Warnings per session went from 179 to 10. The first thing that surfaced afterwards was a real one about the experimental FSR path.

### Frame-time instrumentation

`MCLA_TIMING_LOG=1` writes per-second frame stats, a GPU time breakdown, cache hit and miss counts, interrupt split, accumulator state, and a 1 ms histogram every 30 seconds. It rides existing hooks, so no codegen change was needed.

Two methodology notes worth keeping:

- Perf counters are zeroed every frame, so reading one at the report boundary samples a single arbitrary frame. That printed `draws=0` on healthy 61 FPS seconds. They are now accumulated per frame.
- The sampling point sits in `MCLAFrameDelta`, and where the runtime's own reset lands relative to that is not pinned down. Trust relative change across an event, not absolute totals.

### Build and packaging

- `enable_language(RC)` selected Microsoft `rc.exe`, which cannot parse clang's `# <line> "file"` markers and failed any clean build with `RC2019`. `CMakeLists.txt` now prefers `llvm-rc` from the configured clang's directory.
- `SetFlag` records `ok` or `FAIL` per attempt into `logs/effective_config.txt`. Discarding that return value is how a GPU configuration can run entirely unapplied while looking fine.
- The env dump listed 22 variables that were never passed to `getenv()`. It now lists only what the build reads, with cvar-only settings in a separate section.
- `MCLA_MAX_FRAME_MS` was advertised in the dump but hardcoded at 125 ms. Now read, clamped to 16-1000 ms.
- `MCLA_GAME_DATA` added, ordered behind `--game_data_root` and ahead of auto-detection. An invalid value warns to stderr rather than being ignored.

### Dead code removed

- `Hook_IntroHalfRate` skipped every other SWF advance, which is only correct at exactly 60 FPS. With a configurable `fps_limit` it was wrong at every setting but one.
- `Patch_60FPS_Jump` had no header declaration and no active hook entry.
- The `kSave` item kind and its 13 call sites, after the save row was replaced by auto-save.

---

## The long-session frame rate collapse

Symptom: after 7 to 30 minutes the frame rate falls under 10 FPS and stays there. Traffic cars bounce and flip, because the engine pins `dt` to its own 0.1 s ceiling and physics integrates at 100 ms steps.

**Root cause is in ReXGlue 0.10.0, not this repo.** The vsync worker's catch-up loop compares `current_time - last_frame_time` as unsigned. One backwards step in the guest tick clock makes that wrap to about 1.8e19, the exit condition can never be met again, and the loop dispatches guest interrupts continuously under the global critical region lock.

Measured across the transition:

| | healthy | collapsed |
|---|---|---|
| vblank interrupts/s | ~1,010 | 6,042,116 rising to 11,766,658 |
| CP interrupts/s | ~140 | 21 to 39 |
| draw calls/s | 77,125 | 503 |
| `0x0BADF00D` poison hits | 0 | 0 |

**Workaround, in place now:**

```powershell
Start-Process larecomp.exe -ArgumentList "--clock_no_scaling=true"
```

That branch returns the guest tick count as a plain function of the host clock with no re-basing, so it stays monotonic. A 33 minute run peaked at 4,037 vblanks/s, spent zero seconds above 1M, and never dropped below 24 FPS. The run it replaced collapsed at 715 s.

**Upstream fix**, for whoever picks up the SDK: clamp the backwards step and cap the catch-up per pass so a backlog is dropped rather than drained a millisecond at a time. That removes the need for the flag entirely.

**Open question: does the flag cost frame rate?** Reported as feeling less smooth. Not confirmed either way. What is measured: interrupt load is unchanged, at 1,014 vblanks per second under the flag against a 1,010 baseline, so that mechanism is out. Two things it does change: `ScaleHostToGuestTicks` runs on the full host tick count rather than a small delta, and `QueryGuestSystemTime` returns host time while `SetGuestSystemTime` becomes a no-op. Settling it needs the same route run twice with `MCLA_TIMING_LOG=1` and the histograms compared. The older logs were deleted, so there is no baseline to compare against retroactively.

Also worth noting: with `vsync=false` the vblank interval is 1 ms, not 16.67 ms, giving roughly 17x an Xbox 360's rate. It does not cause the bug but it widens the window.

---

## Open work

### Audio jitter in dense areas

Worst in the cockpit camera and on tyre skid loops. Four causes eliminated by measurement:

| candidate | result |
|---|---|
| Memory ordering in the cache-flush bypass | Fence present; removing it makes crackle worse, not better |
| General CPU starvation | No correlation with frame time |
| Output buffer depth | Tested at 8, 16 and 32. No change. |
| Output underrun | `buffer_queue_depth` stays pinned at maximum and never drains |

What is left is per-voice DSP or mixing inside the guest audio engine. Needs IDA work on the audio path. Cheap first step: check whether Xenia shows the same crackle on the same scene, which would place it in the game rather than the recompilation.

### Traffic and NPC smoothing sweep

Vehicles and pedestrians other than the player car have their own per-frame interpolation constants that have not been swept the way the camera and chassis ones were. Same treatment likely applies. Not blocking anything.

### `PM4_DRAW_INDX_2` backend failure

The GPU backend rejects this packet type under `tess_mode=1, edram_mode=6`. Logged, not currently causing a visible defect.

### Symbol resolver has no call sites

`mcla_symbol_resolver.h` compiles in and maps RAGE Jenkins hashes back to asset names, but nothing calls it. It is header-only, so unused inlines cost nothing in the binary. Either wire it into a diagnostic or delete it. Until then `MCLA_RESOLVE_SYMBOLS` and `MCLA_STRINGS_FILE` do nothing, which is why neither appears in the env dump.

---

## Ruled out

Kept so none of it gets re-tested.

| Hypothesis | What actually happened |
|---|---|
| The collapse is our ambient density hook | The collapse reproduced in runs where no setting was touched, and nothing about density writes can produce an interrupt storm |
| The collapse is a corrupt command buffer | The game's own `0x0BADF00D` check never fired once; CP interrupts never exceeded 2,586/s |
| The collapse is GPU-bound | `fencewait` stayed at 0 throughout |
| The collapse is streaming or shader thrash | `texture_cache_misses` and `pipeline_cache_misses` both stayed at 0 |
| The collapse is the FSR2 upscaler | Reproduced with `present_effect = bilinear` |
| The collapse is a stale limiter deadline after UNCAPPED | The catch-up clause recovers it in one frame. Reset anyway so the stale value is not load-bearing. |
| `engine_dt` pinning at 100 ms means the timer broke | It is the engine's own delta ceiling saturating because frames are genuinely that slow |
| Dense-area slowdown is draw-call submission | About 70 draws per frame. The cost is in recompiled guest code. |
| Texture cache eviction causes city slowdowns | Misses are at zero after the limit increase. Avenue closed. |
| `MCLA_SUBSTEPS=0` is a valid setting | It leaves the player car with no wheels and undriveable. The substep passes are where vehicle setup happens. |
| `clear_memory_page_state=false` is a harmless typo fix | It breaks the render-to-texture minimap, which turns into a white box |
| Dummy `.loc` files remove streaming overhead | Leftover `test_` prefixed dev assets. Seven warnings once at startup, no per-frame cost. |
| Capping the frame rate fixes the fast intro movies | Identical speed at 30, 45 and 60 FPS caps |
| Grepping for `0xF0(rN)` proves a field is unused | Fields passed by address never appear as a displacement |

---

## Audit history

**Port audit against the midnightclub fork.** Compared hook inventories, addresses, registers and implementations. Core timing, camera and chassis hooks matched exactly, including `0x827D7508` as the frame delta address and `+0x58` as the unscaled delta offset. `MCLAPresentInterval` was not missing, it is `Patch_60FPS_Byte` at the same `0x82419AA0`, and its `jump_address_on_true` form is cleaner than the overwrite. Texture cache limits matched at 1536/2048/64.

Two real defects came out of it: the dead ambient hook, and the frame limiter and city LOD being gated behind `real_frame_delta`.

**Documentation audit.** `RUNNING.md` documented `MCLA_LOD_CITY_SCALE`, `MCLA_CAMERA_SMOOTH_SCALE`, `MCLA_TIMING_LOG` and `crash_stack.txt`. None existed at the time. It also listed a 240 FPS option that is not in `kFpsCapVals`, and the pre-move ambient hook address. All corrected. `MCLA_TIMING_LOG` and its log file are real now.

**Mistakes worth remembering.** Two claims in this project were made from a grep rather than from the disassembly, and both were wrong: that the DoF field at `+0xF0` was unused, and that the ambient parse path was the live one. Cross-references settle questions like these; absence of a displacement match does not.
