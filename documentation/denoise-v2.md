# Built-in ray tracing denoiser

`mcla_gpu_rt_denoise_v2` defaults to true and can be changed live in DEBUG → FIXES. False selects the previous paths. The existing per-effect raw-mode routing still selects DLSS Ray Reconstruction and, where available, FSR Ray Regeneration.

| Signal | Previous path and source of residual noise | V2 |
| --- | --- | --- |
| SSR with city RT fallback | One or two specular rays per half-resolution receiver. The hybrid trace provides hit distance and SSR confidence. Legacy temporal reprojection tests depth and clips in RGB, then the fallback share takes up to three spatial iterations. Sparse distant hits and bilinear history crossing geometry can still sparkle. | Reprojects the virtual hit and validates both the receiver and each history tap. Uses reflected vehicle motion where a row is available, signed YCoCg clipping, and a four- or five-iteration filter. SSR confidence controls the spatial blend. The upsample uses the effective sample count. |
| World RT AO | One to four visibility rays per receiver, with depth, normal, vehicle ID and coverage. There is no hit distance in the filter's guides. The old AO path uses geometry and vehicle-aware history, then a radius capped at five traced pixels before history shrinkage. | Keeps vehicle reprojection and uses per-pixel variance to choose the footprint of four spatial iterations. AO compose still receives coverage and history length. |
| Local RT light shadows | One visibility ray per light by default, grouped as three independent channels per texture. No hit distance. The previous 8-bit filter has history and a short kernel, which leaves penumbra grain. | Uses a separate visibility profile with stochastic history rounding and three or four spatial iterations. Light group ownership changes reset history. |
| City sun and nearby-car AO and sun | City uses two or four sun-disc samples. Nearby cars use two or four short AO samples and one sun ray. Both feed visibility into the screen-space AO and contact passes. Few samples can leave stipple or grain. | Uses depth-derived normals, camera motion and three visibility iterations. Tagged car receivers without motion rows use spatial filtering. The city mask filters the combined raw sun visibility once. |
| Per-pixel RT bounce | One diffuse ray per traced receiver on the Strong/Ultra path. The trace provides hit distance for vendor integration. The legacy diffuse filter uses camera motion, a depth test, RGB clipping and a small radius. | Uses normal and plane checks, luminance variance, five spatial iterations, and per-pixel history confidence for the blend into the probe field. |

The world-space irradiance probes keep their probe-space running average. Car paint and traffic paint use a separate packed cone and clear-coat signal with their own filter. Car glass and side mirrors trace sharp reflections with their own coverage and upsampling. These consumers are outside the screen-space signal interface covered here.

The temporal pass stores an unblurred radiance history and a feature texture containing the receiver depth, octahedral normal, surface ID, hit distance, roughness, share and variance. It gathers only geometry-compatible reprojected taps and clips history against the current spatial distribution. The variance combines the temporal first and second central moments and uses current neighbourhood variance while history is young. Neighbour statistics bound isolated HDR samples. The spatial filter propagates variance through a tent à-trous kernel, steps 1, 2, 4, 8 and 16 where the profile, lobe footprint and confidence permit. Each pass checks geometry and signal edges. Visibility channels use their own RGB values rather than a colour transform.

## Cost model and timing

The table assumes the per-pixel passes cover the stated traced extent. The timings are modeled write traffic, calculated from the bytes the shaders store and illustrative throughput of 512 GB/s at 4K and 160 GB/s on a 1080p mid card. Cache and occupancy can move real GPU time in either direction. The 1080p values for per-pixel bounce describe a forced lab configuration, since the standard MID tier does not enable it.

| Pass timing label | Traced size at 4K / 1080p | Extra allocation over previous path | V2 writes at 4K / 1080p | Modeled write time |
| --- | --- | --- | --- | --- |
| `ssr denoise` on Ultra | 1920×1080 / 960×540 | 94.9 / 23.7 MiB | 133 / 33 MB | 0.26 / 0.21 ms |
| `rtao denoise` | 1920×1080 / 960×540 example | 63.3 / 15.8 MiB | 116 / 29 MB | 0.23 / 0.18 ms |
| `rt lights denoise`, per group | 960×540 / 960×540 | 15.8 / 15.8 MiB | 17 / 17 MB | 0.03 / 0.10 ms |
| `city sun denoise` and `car rt ao denoise`, each | 1920×1080 / 1920×1080 | 94.9 / 94.9 MiB | 66 / 66 MB | 0.13 / 0.41 ms |
| `rt gi denoise` | 1920×1080 / 960×540 example | 63.3 / 15.8 MiB | 133 / 33 MB | 0.26 / 0.21 ms |

RT AO and bounce governors can reduce the image area. The current tier budgets allocate 0.42 ms at 4K to shared AO depth, trace, filter and apply together, 0.15 ms to RT sun shadows and 0.55 ms to bounce. Compare measured end-to-end labels against those totals. SSR legacy time is the sum of `ssr temporal` and `ssr denoise`; V2 reports both stages under `ssr denoise`. Measure city and car trace labels as well as their new denoise labels. The grouped light cost scales with active groups. This budget comparison is pending device timing.

## Coordinator verification

After reviewing and committing the worktree, build and prepare the pack under the build lease from `C:\mcv\larecomp`.

```powershell
& .\perf\scripts\build-set-011.ps1 -Name denoise-v2 -Ref feat/denoise-v2 -Sdk rsync -Patches none
& .\perf\scripts\make-pack-011.ps1 -Name denoise-v2 -Exe denoise-v2 -Ref feat/denoise-v2
```

Prepare three 4K takes using the same deterministic Director dolly and DLAA render size. `$Lockrun` is the coordinator's game lock script. These commands write the capture recipes and scenes.

```powershell
$Accept = '.\perf\scripts\vismet\accept.py'
$Compare = @('--exe','denoise-v2','--backend','d3d12','--lockrun',$Lockrun,'--view','night-rain-street',
             '--set','mcla_gpu_ssr_wet=1','--set','mcla_gpu_upscaler=dlss','--set','mcla_gpu_dlss_mode=dlaa',
             '--set','mcla_gpu_pass_timing=true')
python -I $Accept --prepare dn2-old @Compare --set mcla_gpu_denoiser=ours --set mcla_gpu_rt_denoise_v2=false
python -I $Accept --prepare dn2-new @Compare --set mcla_gpu_denoiser=ours --set mcla_gpu_rt_denoise_v2=true
python -I $Accept --prepare dn2-rr @Compare --set mcla_gpu_denoiser=rr --set mcla_gpu_vendor_denoiser=true
```

For each recipe, change `world hour 23 hold weather stormy` to `world hour 23 hold weather nice` in the generated `inputs/look-night-rain-street.scene`, then use only its TOP capture command. Both camera motion and wetness must be visible in the capture and log. Confirm each arm's effective denoiser and the captured frame identities before scoring. Score old vs V2 and RR vs V2 with the same view.

```powershell
$Takes = '.\perf\out'
$Old = "$Takes\look-dn2-old\TOP\night-rain-street"
$New = "$Takes\look-dn2-new\TOP\night-rain-street"
$RR = "$Takes\look-dn2-rr\TOP\night-rain-street"
python -I $Accept --ab $Old $New --view night-rain-street --name dn2-old-new
python -I $Accept --ab $RR $New --view night-rain-street --name dn2-rr-new
```

For timing, use the existing tier plan under the game lock. Run each pair with the same flags and only the V2 flag changed. Set the physical resolution with the host DPI scale from `PERF-TIERS.md`. For the mid-card case use the MID recipe but match a physical 1920×1080 output. Enable SSR Ultra and its RT fallback, RT AO, ray-traced local lights and per-pixel bounce explicitly when profiling each effect. Use a daylight scene for sun visibility. Collect full steady-state buckets with the named labels before judging the budget.

```powershell
$ExeSet = '.\ports\exe\denoise-v2'
$Packs = '.\ports\scratch\packs-denoise-v2'
$Python = (Get-Command python).Source
$Effects = @('--mcla_gpu_ssr=ultra','--mcla_gpu_ssr_rt=ultra','--mcla_gpu_ao=rt','--mcla_gpu_rtao_scale=2',
             '--mcla_gpu_rt_lights=on','--mcla_gpu_rt_gi=ultra','--mcla_gpu_rt_shadows=high')
foreach ($Arm in @('false','true')) {
    $Tag = "dn2-4k-$Arm"
    $Recipe = python -I .\perf\scripts\perf_tiers.py plan HIGH $Tag $ExeSet $Packs --dpi-scale 1.5 | ConvertFrom-Json
    $RunArgs = @($Recipe.argv) + $Effects + "--mcla_gpu_rt_denoise_v2=$Arm"
    & $Python -I $Lockrun --lock --keep-display --game --max-min 17 --log ".\perf\logs\$Tag.log" -- $Python @RunArgs
    if ($LASTEXITCODE -ne 0) { break }
}
```

On the MID host, use the same pair with `MID` in the plan, the host DPI scale and tags `dn2-1080-$Arm`. Before invoking the game lock, set the generated window entry with `$RunArgs[([array]::IndexOf($RunArgs, '--window') + 1)] = '1920x1080'` for a 100% DPI host. A mid card unable to select the extended bounce layout has no per-pixel GI timing. For the daylight pair, set `SCENE` in the returned arguments to a copy of `perf_tiers.scene` with `hour 14 hold weather nice`.

CHECK on both APIs with validation enabled. Exercise the first frame, a camera cut, MSAA, resize, toggling V2 live and switching to and from a vendor denoiser. Inspect wet-road highlights, thin contact shadows, car edges, taillight reflections and the transition from freshly exposed AO into accumulated AO. The modified dry-weather dolly and timing-arm settings need a live smoke.
