# Guest thread cost

The RTX 5080 PGT run averaged 10.76 ms (92.9 FPS) at 4K ENHANCED ULTRA. Pass timers sum to 5.68 ms on the GPU and the Plume thread spent 5.01 ms per frame waiting for guest work. VANILLA averaged 130.9 FPS (7.64 ms). Reaching 144 FPS takes 6.94 ms per frame, a 3.82 ms reduction from that ULTRA run. The 120 FPS 1% low target corresponds to 8.33 ms. PGT used a 4K window and Independent Flip, so use it as context for the new 720p-window A/B. Sources are PERF-GROUND-TRUTH section 2b and the saved PGT reports `main-enhu-5080` and `main-van-5080`.

## Reading the stopwatch

Turn on **DEBUG > LOGGING > GUEST TIMING** or pass `--mcla_guest_timing=true`. Each guest thread reports one `[guest timing]` line per 120 completed brackets. Sections show elapsed mean and nearest-rank p95 in milliseconds per frame and calls per frame. Frames that skip a section contribute zero. The update bracket starts after the clock hook's limiter and ends as the game update returns, before the hand-over wait. The submit bracket starts at the first world phase loop, ends on entry to `grcDevice::EndFrame`, and reports after Swap. `present_wait` separately times the wait for Plume to execute the present, and `readback_wait` times synchronous guest texture reads. The update and submit lanes run in parallel. Line ranges are local to each lane.

The phase columns bracket requested guest phases, including captured draws. `cascade_car_draws` includes the ambient caster gate and the guest item callback it opens. `hud_map_capture` is a subset of `capture` identified by the renderer's UI map target keys. It cannot time the map-specific game work that ran before the draws. `hud` brackets the guest HUD wrapper that calls the rear-view widget. Hook columns inside `housekeeping`, phase columns, and the submit and update totals overlap with their children. Compare each column on its own. These are wall intervals on the actual guest threads and include scheduler stalls. Use `frame.csv` and C7 where available to tell guest work from Plume wait and render-thread work.

## Expected guest costs at `2131b906`

These are estimates per frame. For draw-driven rows, the rough conversion is 0.5-2 microseconds per added draw, bounded by the capture-heavy 0.576 ms for 290 draws in the LAMP-BSPIKE report. The PGT comparison did not isolate these rows, and these estimates overlap.

| Area | Actual ENHANCED ULTRA setting and guest effect | Expected guest cost |
|---|---|---:|
| City and traffic LOD | City 2x, traffic 3x. The hooks themselves write a single value or multiply a distance. More visible work comes from the game's high-detail city sectors and vehicle models. EFFECT-OPT-VIEWS reports about 450 added LOD draws for its city arm. | City 0.23-0.90 ms for those draws. Traffic incremental cost depends on the model and distance mix. |
| Traffic, pedestrian and parked density | 400 m and 1x/1x match VANILLA's preset cells. Traffic density scale is separate. The hook still reconciles 31 zones and 217 fields per update. | 0.002-0.020 ms for hook bookkeeping. Increment versus VANILLA from these preset cells is zero. |
| Prop draw distance | Default override is 0 in every preset. The fragment tune hook still checks the override. | Under 0.01 ms expected with stock distance. |
| Traffic shadows | Preset NEAR, up to four cars at 40 m across four cascades. ALL is a separate test arm, up to 16 at 150 m. The code's estimate is 40 draws per caster per cascade. | 0-1.28 ms for NEAR and 0-5.12 ms for ALL at the configured maxima. Count actual casts and draws before using either ceiling. |
| Mirror and cars | ULTRA side mirrors can request the shared rear phase even if the HUD stamp is hidden. The saved mirror survey reports 511 view draws and 125-860 extra car draws in measured arms. | 0.26-1.02 ms for a 511-draw view, plus 0.06-1.72 ms for cars in those arms. Zero replay when the phase stays off. |
| Planar road view | HIGH reflection resolution changes the host target size. The guest still submits the road pass, typically 61 draws (53-277 in EFFECT-OPT-VIEWS). Only the old debug car hook is present here and defaults off. The production walker with its own time log is on the planar-car branch. | 0.03-0.12 ms at 61 draws. Measure the production walker as a separate arm after integration. |
| Environment | HIGH environment resolution changes host work. The game's twin phases still submit about 240 draws during a refresh. The environment hold is on `perf/env-hold` (`dc9ec9eb`). | 0.12-0.48 ms per refresh. Measure hold after integration with the matching phase mask applied before timing. |
| Car frame and RT | The explicit RT reflection, paint and mirror rows are off in this preset. RT auto consumers and the real RT configuration need separate verification from the effective-mode log. The car model build/walk runs on Plume. The guest taps vehicle IDs and waits at the stream boundaries. | Under 0.03 ms for taps expected. Attribute model walk CPU to C7 and guest-visible waits to `present_wait` or `readback_wait`. |
| Light gathers | The enhanced headlights, light and neon paths read guest points, lamp lists and neon state when consumed. | 0.02-0.15 ms estimated, with night traffic toward the high end. |
| Capture, HUD map and census | Plume captures every guest draw and relevant state changes. HUD maps are stock guest work. Full census diagnostics are opt-in. | Report capture and map capture separately. About 0.05-0.29 ms of guest work for 105-146 stock HUD-view draws at the same draw rate. |

## Implemented cuts

All three use live, default-on **DEBUG > FIXES** switches. The same data and picture rules apply on both paths.

| Switch | Work saved | Estimated saving per frame |
|---|---|---:|
| `mcla_fix_guest_density_work` | Keep stable pointers to the 31 canonical zone states for the active manager and use one settings snapshot for the spawn gates and the zones. Each frame still checks the game's fields for tune reloads. A manager change invalidates the pointers. | 1-5 us per update. |
| `mcla_fix_guest_mirror_work` | Skip empty visibility slots when restoring a car and find the lowest free slot by its bit. Preserve the first saved value, the existing capacity and the restore and LOD order. | 1-15 us per submit frame, depending on vehicle draws and pending restores. |
| `mcla_fix_guest_shadow_work` | Read the current viewport only for a census or when the main viewport cannot supply the eye. Select the two distances that straddle the cutoff. | 2-15 us per submit frame with traffic, depending on rejected and eligible items. |

These cuts target repeated host bookkeeping in guest hooks. The larger opportunities need the first timings. The existing RT walk already caches geometry using a model signature and age, and reads active poses from the current frame. A longer-lived cache must invalidate on pointer reuse, model/LOD changes and retunes. A worker can process copied immutable snapshots, but reading live guest pointers after the render buffer advances risks mixing frames. For the same reason, the density write and mirror visibility restore stay on their current threads. Existing mirror-consumer gates and the independent environment and planar branches cover the view work that can be skipped after validating product lifetime. Changing LOD, cadence or which cars cast would require a separate look check.

## Coordinator A/B

Use one instrumented build so both arms have identical timing overhead. From `C:\mcv\larecomp`, after committing the worktree, merge it into a test worktree based on `127c9479` (`test/land-1125c-opt`). That ref has the tour, route and frame-log harness over the requested base. Review the harness hooks in `frame_timing.cpp`, `hooks.cpp` and `guest_capture.cpp` when merging. The build and pack scripts acquire their build leases and the game commands take the exclusive lock.

```powershell
. .\perf\scripts\lockrun-lease.ps1
$lockrun = Find-LockrunPy
$testTree = Join-Path $PWD.Path 'h11-work\guest-thread-cost-opt'
git -C src worktree add -b test/guest-thread-cost-opt $testTree 127c9479
git -C $testTree merge --no-edit perf/guest-thread-cost
& .\perf\scripts\build-set-011.ps1 -Name gtc -Ref test/guest-thread-cost-opt -Sdk rsync
& .\perf\scripts\make-pack-011.ps1 -Name gtc -Exe gtc -Ref test/guest-thread-cost-opt
$common = @('--exe-dir','gtc','--backend','d3d12','--adapter-name','NVIDIA GeForce RTX 5080',
            '--window','1280x720','--scale','3','--msaa','4','--fps-limit','0','--log-level','info',
            '--save','mzlate','--time-of-day','12','--weather','nice','--no-police','--frame-log')
foreach ($arm in 'before','after') {
    $on = if ($arm -eq 'after') { 'true' } else { 'false' }
    $extra = @('--extra','--mcla_gpu_preset=enhanced_ultra','--mcla_guest_timing=true',
               "--shader_pack_dir=$PWD\ports\scratch\packs-gtc",'--mcla_gpu_upscaler=off',
               '--mcla_gpu_frame_generation=off','--mcla_gpu_hdr=off','--mcla_gpu_vendor_denoiser=false',
               '--mcla_gpu_pass_timing=true',"--mcla_fix_guest_density_work=$on",
               "--mcla_fix_guest_mirror_work=$on","--mcla_fix_guest_shadow_work=$on")
    python -I $lockrun --lock --keep-display --game --max-min 15 -- python -I ports/scripts/opt_run.py @common --name "gtc-$arm-roam" --drive-route sunset-and-vine.route --roam-s 90 @extra
    python -I $lockrun --lock --keep-display --game --max-min 15 -- python -I ports/scripts/opt_run.py @common --name "gtc-$arm-race" --tour c11-race --tour-var RACE_S=90 --no-tour-shots @extra
}
```

- Smoke-check that the effective preset and all switches are accepted, the internal image is 3840x2160, VSync is off, and both lanes report 120-frame windows. Confirm race and roam markers and compare intervals wholly inside their timed segments after discarding three warm-up windows. Keep camera, route, time and weather matched between arms. If the race uses its own weather or time, check the effective values in both logs.
- Compare guest update, submit and present-wait means and p95 with draw counts, Plume idle, GPU time, PresentMon average and 1% low. Check the cutoffs, density recapture log, HUD map counts, mirror phase and the same representative pictures. Repeat with timing off to measure observer cost before interpreting a small FPS delta.
- Isolate the expensive rows after the first run. For an ALL shadow arm override `--mcla_gpu_traffic_shadows=all` on both sides. For the HIGH acceptance test rerun with its ENHANCED RT ULTRA and default upscaler recipe from TEST-PLAN-1125. The ENHANCED ULTRA A/B and the HIGH acceptance recipe answer different questions.

Runtime checks are pending, including the existing route and tour with these launch arguments. Verify the stopwatch against guest thread names and frame counts, and flip each switch during play before relying on its A/B result.
