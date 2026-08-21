# LARecomp Technical Notes

Findings from running Midnight Club: Los Angeles (Xbox 360, Title ID `545407F8`) as a static recompilation on Windows x86-64 via **ReXGlue 0.10.0**.

Everything here was verified by measurement or by reading the disassembly. Where something is inferred rather than proven, it says so. The investigation log, including the hypotheses that turned out wrong, is in [`MCLA_WORKPLAN.md`](MCLA_WORKPLAN.md).

---

## 1. The engine frame timer

`sub_821BDA90` owns the timer object, which lives at a fixed address, so its field offsets are absolute addresses:

| offset | address | meaning |
|--------|---------|---------|
| +0x00 | `0x827D7500` | vtable |
| +0x08 | `0x827D7508` | frame delta, time-scaled. What the engine consumes. |
| +0x0C | `0x827D750C` | `1.0 / delta` |
| +0x14 | `0x827D7514` | accumulated time A |
| +0x18 | `0x827D7518` | accumulated time B |
| +0x24 | `0x827D7524` | delta ceiling, `0.1` |
| +0x28 | `0x827D7528` | delta floor, `0.0001` |
| +0x38 | `0x827D7538` | one-shot "timer was reset" flag |
| +0x54 | `0x827D7554` | time scale, `1.0` |
| +0x58 | `0x827D7558` | frame delta, unscaled |

The guest timebase is **49,875,000 Hz**, not 50 MHz. It follows from `flt_82011110` = 2.00504e-8, whose reciprocal is 49,874,300. Call `Clock::guest_tick_frequency()` rather than hardcoding either figure.

### The delta clamp at the end of the function

`0x821BDC78` and `0x821BDC88` apply a floor and a ceiling to both `[r3+8]` and `[r3+88]` using `fsel` pairs, reading the bounds from `[r3+40]` and `[r3+36]`. Measured at runtime those are `0.0001` and `0.1`.

This matters when reading logs: an `engine_dt` pinned at exactly 100.00 ms does not mean the timer broke. It means real frames took 100 ms or longer and the engine's own ceiling saturated. It is a symptom of slow frames, never a cause. Physics integrating at 100 ms steps is also why traffic cars bounce and flip during a frame rate collapse.

---

## 2. Why the Xenia 60 FPS patch is not enough

The community Xenia patch writes a blanket jump at `0x821BDB08` and a `1` at `0x82419AA3`.

Two corrections to write-ups that circulate with it:

- `0x821BDB08` is `bne cr6, 0x821BDBC8`. It is not `fcmpu`. Misreading it put the patch at `0x821BDB68` in at least one older document, which leaves the main fixed-timestep path live.
- `0x82419AA0` (`li r11, 2`) is not a delta-time value. `sub_824199B0` builds GPU command packets and shifts `r11` into a PM4 present-interval field at `0x82419AB4`. It sets how many vblanks to wait between presents. Two means 30 Hz.

There are **two** fixed-timestep paths, and patching one leaves the game at double speed:

- `loc_821BDB58` compares the measured delta against the fixed step and overwrites `[r3+8]` and `[r3+88]`.
- `loc_821BDB90` is taken when `[r3+3A]` or `[r3+3C]` is set. Measured: taken on **every** frame during gameplay, not occasionally. It overwrites `[r3+8]` with the fixed 33.3 ms step regardless of elapsed time.

The blanket jump also skips the block that advances `[r3+20]` and `[r3+24]`, the engine's accumulated-time totals. Measured: they advanced once and then sat frozen at 0.0333 for a whole session.

### What LARecomp does instead

| hook | address | purpose |
|---|---|---|
| `MCLAUseRealDelta` | `0x821BDB58` | Jumps to `loc_821BDC34`, skipping only the fixed-step overwrite. The `[r3+56]` reset guard at `0x821BDB08` still runs. |
| `MCLAFixedStepPath` | `0x821BDB90` | Replaces the freshly loaded fixed step in `f11` with the measured unscaled delta from `[r3+0x58]`. Everything downstream then does the right thing on its own, and the accumulators keep advancing. |
| `MCLAFrameDelta` | `0x821BDAB0` | Clamps how far one frame can advance the clock, so a streaming stall cannot feed an unbounded delta into physics and audio. |
| `Patch_60FPS_Byte` | `0x82419AA0` | Present interval 2 to 1. |

Keeping the `[r3+56]` reset guard matters. On a reset frame the last-tick field is stale, so the measured delta is garbage. Skipping the guard the way the Xenia patch does, with the clamp also removed, produced a loud audio blowout.

The hitch clamp runs unconditionally, ahead of the `real_frame_delta` check. Gating it meant turning that option off also removed crash protection, and the clamp is correct at 30 Hz anyway, since the guest's fixed timestep never exceeds it.

**Verification method.** Sum the delta actually used across every substep-loop pass and compare against real time. It reads 2.00x at a 30 FPS cap and 2.00x at 60. The 2.00 is an artifact of three passes per frame, two of `dt/2` plus one full `dt`. What matters is that the number does not change with frame rate.

---

## 3. Frame pacing: the stutter was Windows timer granularity

Frame times were quantized to a **15.625 ms grid**, which is 1/64 s. The buckets between clusters were empty, not sparse.

Not the display, which at 170 Hz is 5.88 ms. Not 60 Hz vsync at 16.67 ms. Not the guest vblank rate either: sweeping `video_mode_refresh_rate` across 30, 60, 120 and 144 did not move the grid.

| timer resolution | vsync | frames on grid | fps |
|---|---|---|---|
| default, ~15.6 ms | on | 95% | 37.2 |
| 1 ms | on | 62% | 40.6 |
| default | off | 93% | 40.7 |
| **1 ms** | **off** | **34%** | **48.4** |

Both changes are needed. `timeBeginPeriod(1)` on its own hands pacing to real vsync; disabling vsync on its own leaves the timer grid intact. Together the distribution goes continuous and throughput rises about 30%.

Quantization costs frame rate, not only smoothness. A frame that overruns its quantum by 1 ms waits another full 15.6 ms.

Taking vsync away also removes the only throttle, so the frame limiter sleeps to a wall-clock deadline. A vblank-based limiter would put the grid straight back. The guest's present-interval field cannot do this job, because it only means anything to DXGI when vsync is on.

---

## 4. The long-session frame rate collapse

After 7 to 30 minutes of play the frame rate could fall under 10 FPS and never recover. Three runs reproduced it with the same signature.

What it is **not**, from the instrumentation:

| candidate | evidence against |
|---|---|
| GPU-bound | `fencewait` stayed at 0 across the collapse |
| Streaming thrash | `texture_cache_misses` stayed at 0 |
| Shader recompilation | `pipeline_cache_misses` stayed at 0 |
| Lock contention or thread leak | contentions 0, thread count flat at 40 |
| Corrupt command buffer | the game's own `0x0BADF00D` poison check never fired, and CP interrupts never exceeded 2,586/s |
| Our ambient, LOD or camera hooks | none of them can produce an interrupt storm; the collapse also happened in runs where no setting was touched |

What it is: a **vblank interrupt storm**.

| | healthy | collapsed |
|---|---|---|
| vblank interrupts/s | ~1,010 | 6,042,116 rising to 11,766,658 |
| CP interrupts/s | ~140 | 21 to 39 |
| draw calls/s | 77,125 | 503 |

The guest CPU spends the entire frame servicing interrupts, so nothing gets submitted. Draw calls collapse as a consequence, not a cause. `FunctionDispatcher::ExecuteInterrupt` takes the global critical region lock on every dispatch, which serialises the whole runtime.

The loop, in the SDK's `src/graphics/graphics_system.cpp`:

```cpp
uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
while (vsync_worker_running_) {
    uint64_t current_time = chrono::Clock::QueryGuestTickCount();
    while (current_time - last_frame_time >= interval_ticks) {   // unsigned
        MarkVblank();
        last_frame_time += interval_ticks;
    }
```

Every value is `uint64_t`. If `last_frame_time` ever exceeds `current_time`, the subtraction wraps to about 1.8e19, which is always `>= interval_ticks`. The loop then never exits. Escaping would need `last_frame_time` to close 1.8e19 ticks at 50,000 per iteration, roughly 3.7e14 iterations, which at the observed 11M/s is on the order of a thousand years. That is why it never recovers.

The backwards step comes from `QueryGuestClockFast()`, which returns `guest_base + scale(host_now - host_base)` from a snapshot that gets re-based on every update. The generation counter guarantees each read sees a consistent snapshot, but not that two successive reads are monotonic; a re-base with rounding in `ScaleHostToGuestTicks` can publish a baseline that makes the next read come back lower. One tick is enough.

**Workaround.** `clock_no_scaling=true` takes the branch that returns `ScaleHostToGuestTicks(host_tick_count, ...)`, a plain function of the host clock with no re-basing, so it is monotonic while QPC is. A 33 minute run with it set peaked at 4,037 vblanks/s, spent zero seconds above 1M, and never dropped below 24 FPS.

**Proper fix**, for the SDK rather than this repo: clamp a backwards step with `if (current_time < last_frame_time) last_frame_time = current_time;` and cap the catch-up per pass so a large backlog is dropped instead of drained one millisecond at a time.

One detail worth knowing regardless: with `vsync=false` the interval is `guest_tick_frequency / 1000`, a **1 ms** vblank period. That is the ~1,010/s baseline, roughly 17x an Xbox 360's 60 Hz, and every one of them takes the global lock.

---

## 5. ReXGlue 0.10.0 gotchas

- **GPU cvars cannot be set from `OnPreSetup`.** Everything in `rex/graphics/flags.h` lives in the GPU plugin DLL, which `Runtime::Setup()` loads after `OnPreSetup` returns. Those calls report `FAIL` there and `ok` from `OnPostSetup`. `SetFlagByName` returns `false`, so a mistimed call looks exactly like a typo. `larecomp_app.h` logs every attempt with an `ok` or `FAIL` line for this reason.
- **Log level defaults to `trace` on non-Release builds**, about 7,500 lines and 1.4 MB/s of synchronous disk writes during gameplay. Set `REX_LOG_LEVEL` as an environment variable; calling `SetAllLevels()` from `OnPostInitLogging()` does not work, because the runtime has already emitted its startup banner.
- **`anisotropic_override` is an enum index**, not a multiplier: `-1` none, `0` off, `1` 1x, `2` 2x, `3` 4x, `4` 8x, **`5` 16x**.
- **`clear_memory_page_state=false`** breaks memory coherency and makes the render-to-texture minimap flicker white. Leave it alone.
- **`resolution_scale=2` is broken for this title.** It corrupts the projection aspect feeding the frustum-cull plane normals, so race-start showcase cameras pick opponents behind the camera, and grid spawn transforms come out with garbage rotation, spawning cars tilted and floating. The window is upscaled anyway.
- **Mid-asm hook signatures** take only the registers named in the hook's `registers` list, by reference, with ordinary C++ linkage. Not `extern "C"`, not `(ctx, base)`. Float registers arrive as `PPCRegister&` with the value in `.f64`. Returning `bool` drives `jump_address_on_true`.
- **`llvm-rc` is required.** `enable_language(RC)` picks Microsoft's `rc.exe` by default, which cannot parse the `# <line> "file"` markers clang emits and fails with `RC2019`. `CMakeLists.txt` prefers `llvm-rc` from the configured clang's directory before `enable_language(RC)` runs.
- **Perf counters** in `rex/perf/counter.h` are exported and callable, and 0.10.0 carries a much wider set than 0.9.0 did, including per-stage GPU timing, pipeline creation, fence waits and interrupt dispatch. No build define is needed. Counters are frame accumulators zeroed on each flip unless `kIsGauge` is set for them, so reading one at a report boundary samples a single arbitrary frame. Accumulate every frame if you want per-second totals.

---

## 6. Performance findings

- **`mc_FlushDataCache` (`0x821D5510`)** is a `dcbf`/`dcbst` loop walking a buffer 128 bytes at a time. Host caches are coherent on x86_64, so the hook returns immediately. Measured over an 8.5 minute session: median **4,976 calls/sec**, peak 9,936, and **33.5 GB** of flush range in total, which is roughly 540,000 avoided 128-byte line operations per second and peaks near 1.9 million. Keep the memory fence: the loop is also a publication point, and the XMA decoder and audio worker run on their own host threads. Removing the fence has been linked to audio crackle.
- **Texture cache**: raising limits to 1536 MB soft, 2048 MB hard and 64 MB render-to-texture takes `texture_cache_misses` to zero. That avenue is closed.
- **Draw calls run about 70 per frame** at ~50 FPS. Dense-area slowdowns are not draw-call submission; the cost is in the recompiled guest code doing traffic AI, physics and streaming.
- The startup stub sweep registers about 1.7 million unmapped addresses in 400 to 500 ms. `stubs.txt` stays empty in practice, but the net turns an unmapped indirect call from a crash into a logged no-op, so it stays on.

---

## 7. Ambient density: the hook has to be on the constructor

`mcAmbientDensityTuning` is not a separate object. `sub_826F5CB0` calls its constructor as `sub_826F5B18(a1)` with its own `this`, so the tuning fields sit at the base of the roughly 5,936-byte ambient zone. `sub_826D8E70` builds the zones by calling `sub_826F5CB0` per zone.

An earlier version of this hook sat at `0x826F4E3C`, just after the `density_tuning.xml` parse in `sub_826F4CB8`, on the theory that the parse overwrites whatever the constructor set. The cross-references disprove it: `sub_826F4CB8` is reached only from `sub_826D89F0`, never from `sub_826D8E70`, and `sub_826F5CB0` contains no parse call at all. That hook site never executed. A three minute gameplay session logged zero firings.

The working site is `0x826F5CA0`, in the constructor epilogue. `mr r3, r31` at `0x826F5C80` puts the object in `r3`, the last field write is `stfs f0, 0x98(r31)` at `0x826F5C90`, and `__restfpr_25` only touches float registers, so `r3` still holds the instance. Verified: 32 zones apply, with a 0x1730 stride matching the documented zone size.

Field offsets, static asserted in `mcla_rage_types.h`:

| offset | field | stock value |
|---|---|---|
| +0x08 | spawn_max | 180.0 |
| +0x10 | unspawn_max | 400.0 |
| +0x14 | cull_max | 700.0 |
| +0x60 | ped_density | 15.0 |
| +0x98 | parked_factor | 0.25 |

`parked_factor` is at +0x98, not +0x9C. The padding arithmetic in an earlier version put it one field too far, so the scale silently did nothing.

---

## 8. Pause menu: the list adapter null

`sub_82661210` loads the list's table-source pointer from `[list+0xC0]` and dereferences it at `+0xC` with no null check.

`InstallNativeRows` clears both higher-priority row sources so the renderer falls through to the widget array holding the custom rows, which leaves `+0xC0` legitimately null while a submenu is open. The per-frame menu update path then crashed on it, reported as `read of guest 0x0000000C`.

`MCLA_MenuListNullTableGuard` at `0x8266126C` returns true when the pointer is null, jumping to `0x82661290`. That is the function's own `li r3, 0` early-out for "nothing selected", not an invented exit, and the stack frame is intact because the prologue has run.

Only the SAVE SETTINGS row triggered it, because it was the one row that relabelled itself after being pressed and forced a list re-render. That row is gone now; settings save when you leave a submenu.

---

## 9. Known limitations

- **Intro movies play fast.** Inherent to the frame rate unlock. Capping to 30, 45 or 60 FPS gives identical speed, so a cap does not help. Use `skip_intro` or press A.
- **Dithered alpha on shadows and foliage.** A dither pattern on shadow edges and on vegetation leaves. Two candidate causes, neither confirmed: the GPU plugin's handling of the dither pattern, or the game's own shaders dithering at a pattern scale that assumes the 1280x720 console output and does not hold at higher output resolutions.

  This is the only rendering issue left. Car body reflections and the occasional HUD glitches were both fixed in the custom ReXGlue build LARecomp is developed against, which is not published yet. Older notes in this project attributed all three to the stock `xenos` plugin lacking fixes that [xenia-edge](https://github.com/has207/xenia-edge) carries, and treated them as unfixable without plugin source. Two thirds of that turned out to be wrong.
- **Audio jitter in dense areas**, worst in the cockpit camera and on tyre skid loops. Four causes eliminated by measurement: memory ordering, general CPU starvation, output buffer depth at 8, 16 and 32, and output underrun, since `buffer_queue_depth` stays pinned at maximum and never drains. What is left is per-voice DSP or mixing inside the guest audio engine.

---

## 10. Plausible-sounding things that are wrong

Recorded so nobody re-tests them.

| claim | reality |
|---|---|
| The 60 FPS patch belongs at `0x821BDB68` | It belongs at `0x821BDB08` and `0x821BDB58`. The earlier address came from reading `bne` as `fcmpu`. |
| `0x82419AA0` is a delta-time patch | It is a PM4 present-interval field. |
| The guest timebase is 50 MHz | 49.875 MHz. |
| The stutter is vsync | Windows timer granularity. Vsync off on its own still leaves 93% of frames on the grid. |
| `engine_dt` pinned at 100 ms means the timer broke | It is the engine's own delta ceiling at `[r3+36]` saturating because frames really are that slow. |
| The frame rate collapse is a corrupt command buffer | The game's own `0x0BADF00D` check never fires. It is a vblank interrupt storm from an unsigned underflow in the SDK. |
| Ambient tuning belongs after the `density_tuning.xml` parse | That path never executes. Hook the constructor epilogue at `0x826F5CA0`. |
| Dummy `.loc` files remove streaming overhead | They are leftover `test_` prefixed dev assets. Seven warnings fire once at startup and never recur, so there is no per-frame cost to remove. Fabricating empty files risks the parser accepting garbage instead of failing cleanly. |
| Physics substeps are fixed per frame | `sub_821BD910` divides `dt` by the substep count correctly. |
| Camera smoothing can be fixed at the shared lerp `sub_8231D3A8` | That lerp is shared with cockpit view, wheel animation, speedometer and HUD. Hooking it breaks all of them. Hook the specific caller. |
| Grepping for `0xF0(rN)` proves a field is unused | Fields passed **by address** to a helper never appear as a displacement. `mcDofObject::coc_vector` is uploaded via `addi r29, r31, 0xF0` into a shader parameter setter. |
| Reading a perf counter once per second gives a per-second total | Counters are zeroed each frame, so one read samples one arbitrary frame. Healthy 61 FPS seconds printed `draws=0` this way. |
