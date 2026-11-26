# POST-5 proof

Run from `C:\mcv\larecomp` after committing `perf/upscaler-inputs`. These runs need the
scene and timing harness. Create a fresh test worktree off the opt harness at `af6dad50`.

```powershell
git -C src worktree add -b test/post5-opt C:\mcv\larecomp\h11-work\post5-opt af6dad50
git -C h11-work/post5-opt merge --no-edit perf/upscaler-inputs
& .\perf\scripts\build-set-011.ps1 -Name post5 -Ref test/post5-opt -Sdk rsync -Patches C:\mcv\larecomp\perf\pgt\patches\pass-timing-all.patch
& .\perf\scripts\make-pack-011.ps1 -Name packs-post5 -Exe post5 -Ref test/post5-opt -Confirm
```

## Timing on the main PC

The iGPU FSR Quality pair is the POST-5 timing gate. The RTX 5080 pair measures DLSS
at 4K output scale in a small window. Run wet and dry pairs with frame
generation off. The no-upscaler control should show zero upscaler input work.

```powershell
$lockrunPath = (Select-String -Path .\perf\pgt\scripts\pgt_chain.py -Pattern '^LOCKRUN = r"(.+)"$').Matches[0].Groups[1].Value
$packPath = 'C:\mcv\larecomp\ports\scratch\packs-post5'
foreach ($config in @(
    @{ Name='igpu'; Adapter='AMD Radeon(TM) Graphics'; Scale='1.5'; Upscaler='fsr31' },
    @{ Name='5080'; Adapter='NVIDIA GeForce RTX 5080'; Scale='3'; Upscaler='dlss' }
)) {
    foreach ($weatherName in 'stormy','nice') {
        foreach ($lean in 'false','true') {
            $runName = "post5-$($config.Name)-$weatherName-$lean"
            python -I $lockrunPath --lock --game --max-min 8 -- python -I .\ports\scripts\opt_run.py --name $runName --exe-dir ports/exe/post5 --backend d3d12 --adapter-name $config.Adapter --window 1280x720 --scale $config.Scale --msaa off --fps-limit 0 --save mzlate --time-of-day 19 --weather $weatherName --roam-s 30 --frame-log --log-level info --extra "--shader_pack_dir=$packPath" "--mcla_gpu_upscaler=$($config.Upscaler)" --mcla_gpu_fsr31_mode=quality --mcla_gpu_dlss_mode=quality --mcla_gpu_dlss_rr=off --mcla_gpu_frame_generation=off --mcla_gpu_hdr=off --mcla_gpu_pass_timing=true "--mcla_gpu_upscaler_inputs_lean=$lean"
            python -I .\perf\pgt\scripts\pgt_report.py "ports/run/$runName" --top 150 --json "perf/out/$runName.json"
        }
    }
}
python -I $lockrunPath --lock --game --max-min 8 -- python -I .\ports\scripts\opt_run.py --name post5-native --exe-dir ports/exe/post5 --backend d3d12 --window 1280x720 --scale 1.5 --msaa 2 --save mzlate --weather stormy --roam-s 15 --frame-log --log-level info --extra "--shader_pack_dir=$packPath" --mcla_gpu_upscaler=off --mcla_gpu_frame_generation=off --mcla_gpu_pass_timing=true
python -I .\perf\pgt\scripts\pgt_report.py ports/run/post5-native --top 150
```

Confirm the adapter, render and output extents, and active upscaler in `larecomp.log`.
Sum `temporal depth copy`, `temporal motion vectors`, `temporal depth + motion vectors`,
`reactive mask`, `transparency mask`, `upscaler masks resolve`, `upscaler mask draws`
and `upscaler rain mask draws`, treating absent labels as zero. Compare the trimmed
window means. POST-5 targets at least 30% input-time reduction on the iGPU. Report
the dedicated rain shader label separately, plus wet minus dry for all mask draws
to account for weather particles that use shared particle shaders.

## Vulkan and laptop timing on the ROG lane

```powershell
$jobs = foreach ($adapter in @(
    @{ Tag='intel'; Name='Intel(R) UHD Graphics'; Upscalers=@('fsr31') },
    @{ Tag='rtx'; Name='NVIDIA GeForce RTX'; Upscalers=@('fsr31','dlss') }
)) {
    foreach ($upscaler in $adapter.Upscalers) {
        foreach ($lean in 'false','true') {
            @{
                name = "post5-vk-$($adapter.Tag)-$upscaler-$lean"
                exe = 'post5'
                packs = 'packs-post5'
                backend = 'vulkan'
                args = @('--adapter-name', $adapter.Name, '--window', '1280x720', '--scale', '1.5',
                    '--msaa', 'off', '--fps-limit', '0', '--save', 'mzlate', '--time-of-day', '19',
                    '--weather', 'stormy', '--roam-s', '30', '--frame-log', '--log-level', 'info',
                    '--extra', "--mcla_gpu_upscaler=$upscaler", '--mcla_gpu_fsr31_mode=quality',
                    '--mcla_gpu_dlss_mode=quality', '--mcla_gpu_dlss_rr=off', '--mcla_gpu_frame_generation=off',
                    '--mcla_gpu_hdr=off', '--mcla_gpu_pass_timing=true', "--mcla_gpu_upscaler_inputs_lean=$lean")
            }
        }
    }
}
$queuePath = Join-Path $PWD 'perf/out/post5-rog.json'
ConvertTo-Json -InputObject @($jobs) -Depth 5 | Set-Content -Encoding ascii $queuePath
python -I .\perf\scripts\laptop\laptop_lane.py $queuePath --plan
python -I .\perf\scripts\laptop\laptop_lane.py $queuePath
foreach ($job in $jobs) {
    python -I .\perf\pgt\scripts\pgt_report.py "ports/run/$($job.name)-rog" --top 150
}
```

Use the same label sum. Confirm the selected adapter in each log. Use a separate
validation session with the Vulkan validation layer enabled and repeat the RTX
capture for both FSR and DLSS.

## Byte checks and look

Launch the test executable under RenderDoc with the same backend/upscaler settings plus
`--mcla_gpu_upscaler_inputs_verify=true --mcla_gpu_object_names=true`. Request a full
frame with F11 after the temporal log says the upscaler is ready. The verify setting
records the separate and combined draws on the same source data, in that order.
In RenderDoc's Python shell select the event immediately after each draw, then save
`ReplayController.GetTextureData(resourceId, renderdoc.Subresource(0,0,0))` for mip 0
of each named target. Compare the files with `fc.exe /b old.bin fused.bin` per target
and API. Check the R32_FLOAT depth, RG16_FLOAT motion, and the two resolved masks
(R8_UNORM on D3D12, R16_FLOAT on Vulkan). Repeat for a cut/reset, 1x and 4x samples
(`--mcla_gpu_msaa_with_upscaler=true --mcla_gpu_msaa=4`), live resize and switch off.
For depth/stencil and coverage around the first mask draw, check the clear targets
in the event browser. A single-layer frame should have exactly one mask resolve.
For a separate D3D12 validation run set `--mcla_gpu_debug_layer=true` and
`--mcla_gpu_gpu_based_validation=true`.

Use the existing rain and clear scenes for a repeat-floor comparison. The
`ghost-chain2` and `ghost_archive` presets pin the upscaler off, so these commands
use the custom VisMet arms. For the independent DLSS mask A/B, hold the lean path on and vary only
`mcla_gpu_upscaler_masks=true/false` with RR off. Check ghosting around glass, spray and
moving car shadows against the repeat floor before changing which hints DLSS gets.

```powershell
$look = 'mcla_gpu_upscaler=fsr31,mcla_gpu_fsr31_mode=quality,mcla_gpu_dlss_rr=off,mcla_gpu_frame_generation=off'
& .\perf\scripts\chain-vismet.ps1 -Preset custom -Scene 'showcase-rain@8','showcase-clear@8' -Tag post5-fsr-dx -Backend d3d12 -ExeA post5 -ExeB post5 -Repeats 3 -CvarsA "$look,mcla_gpu_upscaler_inputs_lean=false" -CvarsB "$look,mcla_gpu_upscaler_inputs_lean=true"
$look = 'mcla_gpu_upscaler=dlss,mcla_gpu_dlss_mode=quality,mcla_gpu_dlss_rr=off,mcla_gpu_frame_generation=off'
& .\perf\scripts\chain-vismet.ps1 -Preset custom -Scene 'showcase-rain@8','showcase-clear@8' -Tag post5-dlss-dx -Backend d3d12 -ExeA post5 -ExeB post5 -Repeats 3 -CvarsA "$look,mcla_gpu_upscaler_inputs_lean=false" -CvarsB "$look,mcla_gpu_upscaler_inputs_lean=true"
& .\perf\scripts\chain-vismet.ps1 -Preset custom -Scene 'showcase-rain@8','showcase-clear@8' -Tag post5-dlss-masks-dx -Backend d3d12 -ExeA post5 -ExeB post5 -Repeats 3 -CvarsA "$look,mcla_gpu_upscaler_masks=true" -CvarsB "$look,mcla_gpu_upscaler_masks=false"
```
