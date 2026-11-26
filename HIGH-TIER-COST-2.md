# RTX 5080 at 4K, second cost pass

The six switches have a planning range of **0.15–0.56 ms per frame** on top of the first five. That puts both batches at **0.56–1.38 ms** against the mixed-archive **1.81 ms** gap, leaving **0.43–1.25 ms**. The 7.422 ms GPU bucket from the older night archive would need 0.48 ms to fit the GPU budget alone. It presents to a 720p swap chain, and its frame intervals point to a separate pacing gap. No performance, frame-pacing or look result on the acceptance configuration exists yet. [Night archive](../../ports/run/gca-rh4kd-night-base-1/larecomp.log#L4128) · [Tier model](../../perf/PERF-TIERS.md)

| Switch (default on) | Estimate | Basis and picture check |
| --- | ---: | --- |
| `mcla_gpu_ssgi_fast_gather` | 0.04–0.11 ms | The 0.339 ms gather evaluates the horizon integral for each depth sample. Expanding its cosine avoids a trig evaluation per bound, and black samples can omit radiance-facing work after their occlusion mask is set. The ray count, reach and temporal history stay at preset values. Compare moving neon and headlight pools for flicker around quantised sectors. |
| `mcla_gpu_ssgi_fast_filter_plane` | 0.01–0.05 ms | The 0.263 ms a-trous span predates the previous filter cut. Calculate each surviving tap's plane distance with an affine offset. This keeps the filter's taps and history, with floating-point rounding differences. Compare thin edges and grazing road surfaces. |
| `mcla_gpu_rt_gi_short_refresh_rays` | 0.10–0.18 ms | The archive spent 0.408 ms tracing and 0.063 ms blending before the first batch. At scale 1.5 the standard layout's clean-frame plan goes from 8,704 to 6,400 probes with the first batch and from 409,600 to 204,800 planned rays with this cut. Inside probes already skip some visits. Warm-up and new or stale probes keep 64 rays. The 32-ray batch has different sample variance and reads feedback after the priority batch. Check convergence near streaming boundaries, small light sources and dusk. |
| `mcla_gpu_rt_gi_quarter_lookup` | 0–0.13 ms | The 0.279 ms lookup falls from 2,073,600 to 518,400 texels in field-only exterior 4K. The gross linear ceiling is 0.209 ms. Charge the extra guide reads and per-pixel edge fallback against **lookup plus apply** together. This arm could regress on dense geometry. Check curbs, poles, car edges and wall/road lighting transitions. |
| `mcla_gpu_local_lights_reach_mask` | 0–0.07 ms | The archive reported 1,130 grouped draws over the last 600 frames and nearly full scissor coverage. Reuse each grouped light's early range test before loading its full record and evaluating its term. Keep light order, contact donor ranking, blend and capture partition. The first batch's depth bounds already removed some cheap pixels. Measure partly covered tubes and heavily overlapping lights separately. |
| `mcla_gpu_rtao_reuse_trace_validity` | 0–0.02 ms | The trace already writes its judged flag to the signal and the guide. Reading it from the signal removes one guide fetch per variance tap in temporal and later spatial passes. The quarter-size RT AO arm already saves most of the 0.339 ms denoise span. The preset already uses one ray per pixel. Check cabin transitions and newly revealed contact edges. |

## Work that remains

- SSGI's 0.201 ms before/after pair includes scene-target blending and quantisation. A separate additive target changes that subtraction. The observed near-full neon rectangles also limit how much a scissored capture would save. The 0.115 ms post-colour capture occurs at a later point in the frame and serves other consumers. Lowering the SSGI working size again would also change the shared screen AO guides and how the two AO terms are combined. A second timing capture is needed to establish whether these are the biggest remaining costs.
- Local lights already share G-buffer reads within each neon tube. Overlapping draws could share reads through tiled lists, but merging draws changes the order of colour-target blending and must preserve the SSGI capture boundary. The lamp atlas already caches static layers. The night archive shows no lamps using the shadow table and just 0.012 ms in headlight shadows, so no atlas saving is charged here.
- RT GI maintenance, update, AO, lookup and apply use the graphics command list. They share the current top-level scene, probe field and guides. Moving the in-place field update to another queue places a wait before current consumers. True overlap would require independently published field copies and an overlap capture. RT AO has a similar current-frame dependency through compose. No async overlap credit is included.
- RT AO's third spatial dispatch passes mature pixels through, but still filters some disocclusions. Keep it until an A/B can measure the look and the small remaining 4K cost with quarter-size trace enabled.

## Verification

- Twelve standalone suites passed, with 748,113 checks. The tests caught 19 deliberate mutations.
- All five edited renderer translation units passed the syntax sweep with the `sdk-rsync` headers.
- Twenty-three shader variants compiled to DXIL and SPIR-V. All 23 SPIR-V modules passed validation.
- Settings exposure, row tags, menu rows, live readers and descriptor counts passed. The targeted settings exit audit found zero exit-time destructors. The handoff's PowerShell parsed and passed the case lint.

## Coordinator commands

Review and commit from `feat/high-tier-cost-2` (base `a2ca24a1`), then merge with the current capture-capable integration branch into `test/high-tier-cost-2-opt`. Run from `C:\mcv\larecomp`. The build helper takes its own build lease. Set `$Lockrun` to the coordinator's current lock script.

~~~powershell
$targets = 'larecomp_gi_update_rules_test,larecomp_gi_apply_test,larecomp_ssgi_stable_test,larecomp_local_lights_test,larecomp_rtao_denoise_test,larecomp_rtao_integration_test'
& .\perf\scripts\build-set-011.ps1 -Name high-tier-cost-2 -Ref test/high-tier-cost-2-opt -Sdk rsync -Patches none -ExtraTargets $targets
& .\perf\scripts\make-pack-011.ps1 -Name high-tier-cost-2 -Exe high-tier-cost-2 -Ref test/high-tier-cost-2-opt

$exeDir = 'C:\mcv\larecomp\ports\exe\high-tier-cost-2'
$packs = 'C:\mcv\larecomp\ports\scratch\packs-high-tier-cost-2'
$batch1 = @(
    '--mcla_gpu_rt_gi_reuse_coarse_probes=true',
    '--mcla_gpu_ssgi_residual_short_filter=true',
    '--mcla_gpu_rtao_quarter_at_4k=true',
    '--mcla_gpu_local_lights_depth_bound=true',
    '--mcla_gpu_local_lights_sphere_scissor=true'
)
$switches = @(
    'mcla_gpu_ssgi_fast_gather',
    'mcla_gpu_ssgi_fast_filter_plane',
    'mcla_gpu_rt_gi_short_refresh_rays',
    'mcla_gpu_rt_gi_quarter_lookup',
    'mcla_gpu_local_lights_reach_mask',
    'mcla_gpu_rtao_reuse_trace_validity'
)
$arms = @('base') + $switches + @('combined', 'base-repeat')
foreach ($arm in $arms) {
    $runName = 'htc2-' + ($arm -replace '^mcla_gpu_', '')
    $offFlags = @($switches | Where-Object { $arm -ne 'combined' -and $_ -ne $arm } | ForEach-Object { "--$_=false" })
    $recipe = python -I -B .\perf\scripts\perf_tiers.py plan HIGH $runName $exeDir $packs --dpi-scale 1.5 | ConvertFrom-Json
    $runArgs = @($recipe.argv) + $batch1 + $offFlags
    python -I -B $Lockrun --lock --keep-display --game --max-min 17 --log ".\perf\logs\$runName.log" -- python @runArgs
    if ($LASTEXITCODE) { throw "Run failed for $runName" }
    python -I -B .\perf\scripts\perf_tiers.py report HIGH ".\ports\run\$runName"
}
~~~

In the Look Accept A/B plan set both arms to the same 4K DLAA window, preset, RT tier, backend, scene clock, weather and batch-one flags. Start with all six new switches off on BASE, and use an isolated enable or the all-on combination on CHANGE. Repeat the eye-sensitive arms in Vulkan and use the guide and motion metrics with matched camera poses.

~~~powershell
python -I -B .\perf\scripts\vismet\accept.py --prepare htc2-dx --ab --config TOP --exe high-tier-cost-2 --lockrun $Lockrun --backend d3d12
python -I -B .\perf\scripts\vismet\accept.py --prepare htc2-vk --ab --config TOP --exe high-tier-cost-2 --lockrun $Lockrun --backend vulkan
python -I -B .\perf\scripts\vismet\accept.py $BaseCaptures $ChangeCaptures --name htc2-combined
~~~

**UNPROVEN** covers game timing and look for all six switches, 144 FPS average and 120 FPS 1% low, runtime shader validation, the HIGH and Look Accept routes on this build, and the 0.15–0.56 ms planning range.
