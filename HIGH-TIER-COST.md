# RTX 5080 at 4K

The last steady 4K-internal night bucket spent **1.198 ms on SSGI, 0.977 ms on RT GI, 0.609 ms on deferred local lights, and 0.510 ms on RT AO trace and denoise**, out of 7.422 GPU ms. Five independent, default-off savings have a planning range of **0.41–0.82 ms per frame** before interactions. The modeled 1.81 ms gap leaves **0.99–1.40 ms** to find. The requested 2.5 ms saving leaves **1.68–2.09 ms** to find. The archive assigns 3.294 ms to these four effects, so saving 2.5 ms here would remove about 76% of their measured work.

The night bucket is the last 3,600-present pass summary in the base archive. The 5080 ran a 4K internal render, a 720p swap chain, 2× MSAA, High Auto, and forward headlights. The run produced about 121 FPS with interval p99 of 8.88–10.68 ms. The 144 FPS frame allowance is 6.94 ms and the 120 FPS 1% low allowance is 8.33 ms. The 8.75 ms stack estimate from the tier plan is a mixed-archive budget. A separate native 4K DLAA rain showcase used Custom settings and recorded 7.108 and 6.596 GPU ms in its last two intervals. The exact ENHANCED RT ULTRA scene still needs a measurement. [Night pass summary](../../ports/run/gca-rh4kd-night-base-1/larecomp.log#L4128) · [4K DLAA rain capture](../../ports/run/integ8-show-rain/larecomp.log#L1540) · [Tier budget](../../perf/PERF-TIERS.md)

## Per-effect cuts

Estimates scale the archived pass times by the work each change could remove. The scene, graphics API and final-resolution display can change those shares.

| Effect and archived breakdown | Implemented experiments | Further cuts and risks |
| --- | --- | --- |
| **RT GI, 0.977 ms.** Probe trace 0.408, blend 0.063, weights 0.007, screen lookup 0.279, full-size apply 0.220. The archive used the standard probe field. The preset also selects RT GI High, which uses the field without the per-pixel trace. | **0.07–0.12 ms** with <code>mcla_gpu_rt_gi_reuse_coarse_probes</code>. Halves steady round-robin refresh for coarser cascades after the governor's budget. The finest cascade, warm-up, and new or stale probes keep their quotas. At governor scale 1.5, the standard layout goes from 8,704 to 6,400 scheduled probes in an otherwise clean frame. Check lighting lag at dusk and street-lamp transition boundaries. | Reduce only the screen lookup to quarter size, with depth and normal guidance and the AO contribution kept at its existing resolution. **0.12–0.20 ms** after upsample overhead. Medium risk of light leaking across thin railings and car edges. |
| **SSGI, 1.198 ms.** Visibility and gather 0.339, spatial a-trous 0.263, denoise 0.071, history 0.066, apply 0.146, colour chain and before/after light captures 0.313. RT GI can carry the static bounce while SSGI still carries residual moving lights. | **0.06–0.10 ms** with <code>mcla_gpu_ssgi_residual_short_filter</code>. At 2160 internal lines and above the residual-only chain omits one resolution-dependent spatial iteration. At 4K with half-size SSGI this usually changes three passes to two. Capture neon shimmer and the trailing edges of moving headlights. | Restrict the paired residual captures to the union of affected light rectangles, including the downsample footprint, with zero contribution outside. **0.06–0.14 ms** if the light coverage is small and the clear is cheap. Low risk in the difference math, with boundary and live-resize testing needed. The archived chase view's lights cover almost the entire target, so use the depth-bound experiment there first. |
| **Deferred local lights, 0.609 ms.** The log groups depth and material reads, lighting, neon contact and blending. The final 600 frames reported 1,130 draws and 9,581 MPixels scissored, almost the entire padded 3840 × 2208 target per draw. These draws carried neon. | **0.05–0.20 ms** with <code>mcla_gpu_local_lights_depth_bound</code>. Computes a conservative far depth before the draw and rejects distant surfaces before world reconstruction and reach checks. **0–0.04 ms** in this proxy with <code>mcla_gpu_local_lights_sphere_scissor</code>. Projects the actual light bounds instead of the enclosing cube. In-sphere chase views still need a full scissor. Check moving lamp edges, vehicle vents and neon contact. | For overlapping full-screen draws, measure a tiled light list with one G-buffer fetch per covered pixel. **0.10–0.25 ms** potential, overlapping with the two culling cuts. Medium risk from blend rounding and preserving the separate before/after light-capture partitions. |
| **RT AO, 0.510 ms.** Trace 0.171 and temporal plus spatial denoise 0.339. Compose and AO apply are separate. The archive's governor stayed at half-size trace. | **0.23–0.36 ms** with <code>mcla_gpu_rtao_quarter_at_4k</code>. Exterior 4K automatic strong-RT frames trace and filter at 960 × 540 using the existing guided compose. Manual divisors, cockpit and raw Ray Reconstruction frames keep their paths. Check tyre contacts, wall bases, poles and silhouettes on a moving camera, including the history reset crossing the size boundary. | Keep half-size rays and shorten only the spatial denoiser in established history, **0.05–0.13 ms**. Medium risk from visible noise under rapid disocclusion. This overlaps the quarter-size saving. |

Strong RT GI refresh remains on the graphics queue because it publishes the probes that the frame's lookup and hit shading read. Moving it to a worker queue requires double-buffered publication and a timing study. The archives give no overlap credit. The earlier paint, history, corner-tap and half-AO candidates in [GFX-COST-LEFT](../../perf/GFX-COST-LEFT.md) have separate estimates.

## DLAA and DLSS Quality

DLAA renders 8.29 million scene pixels at 4K. DLSS Quality uses a nominal 2560 × 1440 input, 3.69 million pixels (44.4%), and reconstructs 4K. A 55.6% reduction in pixel-proportional work covers 1.81 ms only if at least 3.26 ms of the baseline scales that way, before any change in DLSS cost. Probe updates, much of the acceleration build and 4K output work are distinct from that input pixel count. The 2160-line RT AO and SSGI experiments are inactive at the Quality input size. The older Quality race confirms the ratio at 2880 × 1620 output, with 1920 × 1080 input. It supplies no paired 4K frame-time delta. DLAA remains the HIGH acceptance run. Measure Quality as a separate arm and compare thin detail, foliage, specular stability and motion against it. [Mode table](src/gpu/upscale_modes.h#L40) · [Quality race](../../ports/run/sr3v-high-race-dlss/larecomp.log#L402)

## Coordinator commands

The worktree is based on <code>origin/main</code> at <code>72bc8059</code>. Review and commit the code locally, then merge the reviewed branch and <code>test/integration14-1125-opt</code> into <code>test/high-tier-cost-opt</code> for the scene and capture harness. Run from <code>C:\mcv\larecomp</code> with <code>$Lockrun</code> set to the current coordinator lock script. Commands use the desktop's 150% DPI setting.

~~~powershell
$targets = 'larecomp_gi_update_rules_test,larecomp_ssgi_stable_test,larecomp_rtao_rules_test,larecomp_rtao_integration_test,larecomp_local_lights_test'
& .\perf\scripts\build-set-011.ps1 -Name high-tier-cost -Ref test/high-tier-cost-opt -Sdk rsync -Patches none -ExtraTargets $targets
& .\perf\scripts\make-pack-011.ps1 -Name high-tier-cost -Exe high-tier-cost -Ref test/high-tier-cost-opt

$exeDir = 'C:\mcv\larecomp\ports\exe\high-tier-cost'
$packDir = 'C:\mcv\larecomp\ports\scratch\packs-high-tier-cost'
$arms = [ordered]@{
    base = @()
    gi = @('--mcla_gpu_rt_gi_reuse_coarse_probes=true')
    ssgi = @('--mcla_gpu_ssgi_residual_short_filter=true')
    ao = @('--mcla_gpu_rtao_quarter_at_4k=true')
    scissor = @('--mcla_gpu_local_lights_sphere_scissor=true')
    depth = @('--mcla_gpu_local_lights_depth_bound=true')
}
$arms['combined'] = @($arms['gi'] + $arms['ssgi'] + $arms['ao'] + $arms['scissor'] + $arms['depth'])
$arms['quality'] = @()
foreach ($arm in $arms.GetEnumerator()) {
    $runName = 'high-tier-cost-' + $arm.Key
    $recipe = python -I -B .\perf\scripts\perf_tiers.py plan HIGH $runName $exeDir $packDir --dpi-scale 1.5 | ConvertFrom-Json
    $runArgs = @($recipe.argv) + @($arm.Value)
    if ($arm.Key -eq 'quality') {
        $runArgs = @($runArgs -replace '^--mcla_gpu_dlss_mode=dlaa$', '--mcla_gpu_dlss_mode=quality')
    }
    python -I -B $Lockrun --lock --keep-display --game --max-min 17 --log ".\perf\logs\$runName.log" -- python @runArgs
    if ($LASTEXITCODE) { throw "Run failed for $runName" }
    if ($arm.Key -ne 'quality') {
        python -I -B .\perf\scripts\perf_tiers.py report HIGH ".\ports\run\$runName"
    } else {
        python -I -B .\perf\scripts\perf_tiers.py tester ".\ports\run\$runName\larecomp.log"
    }
}
~~~

For the separate Quality arm, use the same QPC window if comparing pacing. The HIGH judge explicitly requires DLAA. The tester report gives Quality's pass windows and interval pacing proxies. Keep the full <code>frame.csv</code> for computing the exact slowest-1% mean.

Use same-exe, same-preset DLAA captures for <code>accept.py</code>. In the generated TOP and PLAYER commands set both windows to <code>2560x1440</code> at 150% DPI, both presets to <code>enhanced_rt_ultra</code>, both RT tiers to <code>strong</code>, both upscalers to <code>dlss</code>, and add <code>--mcla_gpu_dlss_mode=dlaa</code> to both. Apply one experiment's flags to the PLAYER commands. Keep the captures independent and match camera, scene clock and weather. Compare dock walls and street-lamp pools for RT GI, moving headlights and neon for SSGI and local lights, and cockpit/exterior transitions, poles and contacts for RT AO. Use the per-region FLIP and motion metrics. Wet-road reflection acceptance also needs the reflection radiance and hit-class streams expected by the judge.

~~~powershell
python -I -B .\perf\scripts\vismet\accept.py --prepare high-tier-cost-dx --exe high-tier-cost --lockrun $Lockrun --backend d3d12
python -I -B .\perf\scripts\vismet\accept.py --prepare high-tier-cost-vk --exe high-tier-cost --lockrun $Lockrun --backend vulkan
python -I -B .\perf\scripts\vismet\accept.py $TopCaptures $PlayerCaptures --name high-tier-cost-combined
~~~

**UNPROVEN**. All in-game timings, look comparisons, live toggles, the existing HIGH and Look Accept routes on this build, and the 144/120 FPS verdict.

