# Atmosphere validation

Use the same executable on both sides of a comparison. These runs are unproven
on this branch. Smoke one short capture on each API before running the pairs.

## Build and packs

Merge the changes into the coordinator's test integration based on
test/land-1125e-opt, which includes the scene harness, VISMET capture and
feat/refl-hit-class. Use the resulting test/atmosphere-cheap-opt ref below.
The host shaders are built into the executable.

```powershell
Set-Location C:\mcv\larecomp
$atmRef = 'test/atmosphere-cheap-opt'
$atmExe = 'atmosphere-cheap'
. .\perf\scripts\lockrun-lease.ps1
$atmLock = Find-LockrunPy
& .\perf\scripts\build-set-011.ps1 -Name $atmExe -Ref $atmRef -Sdk setm -ExtraTargets larecomp_atmosphere_test
& .\perf\scripts\make-pack-011.ps1 -Name $atmExe -Exe $atmExe -Ref $atmRef
```

The build script takes the build lease. Each timing run takes the exclusive
game lease.

## POST-0 look and menu presence

Prepare the seven established look views at 1080p and 4K on D3D12 and Vulkan.
BASE uses Ultra, history on, and a 0.5 m near plane. CHANGE uses Medium,
history on, and 2 m. Both disable shafts to isolate the volume. The stock look
set includes storm, cockpit, tunnel, and the three menu views. For fog and
dry night, also run volumetrics-views with the fog scene and its default
open-mid scene.

```powershell
foreach ($atmConfig in 'PLAYER','TOP') {
    foreach ($atmApi in 'd3d12','vulkan') {
        $atmTag = "vol-equiv-$atmConfig-$atmApi"
        python -B .\perf\scripts\vismet\accept.py --prepare $atmTag --ab --config $atmConfig --exe $atmExe --lockrun $atmLock --backend $atmApi --set mcla_gpu_volumetrics=medium --set mcla_gpu_vol_temporal=true --set mcla_gpu_vol_history_medium=true --set mcla_gpu_vol_near=2.0 --set mcla_gpu_sun_shafts=off
        $atmFolder = "C:\mcv\larecomp\perf\out\look-$atmTag"
        $atmAdapter = if ($atmConfig -eq 'PLAYER') { 'AMD Radeon(TM) Graphics' } else { 'NVIDIA GeForce RTX 5080' }
        Get-Content "$atmFolder\capture-plan.txt" | ForEach-Object {
            if ($_ -match 'opt_run\.py') {
                $atmCommand = $_ -replace " '--backend'", " '--adapter-name' '$atmAdapter' '--backend'"
                if ($atmCommand -match "look-$atmTag[\\/]BASE[\\/]") {
                    $atmCommand += " '--mcla_gpu_volumetrics=ultra' '--mcla_gpu_vol_temporal=true' '--mcla_gpu_vol_history_medium=false' '--mcla_gpu_vol_near=0.5' '--mcla_gpu_sun_shafts=off'"
                }
                $atmCommand
            }
        }
        "python -B .\perf\scripts\vismet\accept.py --ab '$atmFolder\BASE' '$atmFolder\CHANGE' --name $atmTag --run-tag $atmTag"
    }
}
```

Run the printed captures and scoring commands. In the garage, pause world,
and photo mode, confirm the expected UI movie and one volume composite per
eligible frame. Use mcla_gpu_vol_census=120 to log its input lights. Check
the full map, a photo pan, menu entry and exit, and resize. Use mcla_gpu_vol_view=1 on both sides of a second capture to compare
the isolated volume signal. Score ordinary final colour for sanity and FLIP.

Use the limits in EFFECT-OPT-POST. Require whole-frame mean FLIP at most 0.15
and p95 at most 0.50, region limits 0.15/0.45, and dry and rain temporal
limits of 0.003/0.05 and 0.008/0.30. Compare volume-only mean energy in
lamp-pool and headlight crops against Ultra within 5 percent. Report cockpit
near-field differences. Keep existing quality levels until the look passes.

## Pass cost and dusk A/B

Use 2x MSAA and matching effect settings on both adapters. Compare steady
3600-present windows. Extend the hold if the iGPU produces fewer active
frames.

```powershell
$atmExeDir = "C:\mcv\larecomp\ports\exe\$atmExe"
$atmPack = "C:\mcv\larecomp\ports\scratch\packs-$atmExe"
$atmStarter = 'C:\mcv\larecomp\ports\scratch\starter\merged-1.bin'
foreach ($atmApi in 'd3d12','vulkan') {
    foreach ($atmTier in 'igpu1080','rtx5080-4k') {
        $atmAdapter = if ($atmTier -eq 'igpu1080') { 'AMD Radeon(TM) Graphics' } else { 'NVIDIA GeForce RTX 5080' }
        $atmScale = if ($atmTier -eq 'igpu1080') { '1.5' } else { '3' }
        $atmCommon = @('--exe-dir',$atmExeDir,'--backend',$atmApi,'--adapter-name',$atmAdapter,'--window','1280x720','--scale',$atmScale,'--msaa','2','--fps-limit','0','--log-level','info','--save','mzlate','--starter',$atmStarter,'--no-pm','--max-run-s','850','--tour-timeout-s','730')
        $atmFlags = @("--shader_pack_dir=$atmPack",'--mcla_gpu_preset=custom','--mcla_gpu_hdr=off','--mcla_gpu_upscaler=off','--mcla_gpu_headlights=on','--mcla_gpu_light_shadows=medium','--mcla_gpu_pass_timing=true','--mcla_gpu_scene_profile=true')
        foreach ($atmArm in 'ultra','medium') {
            $atmName = "vol-equiv-$atmTier-$atmApi-$atmArm"
            $atmNear = if ($atmArm -eq 'medium') { '2.0' } else { '0.5' }
            python -I $atmLock --lock --keep-display --game --max-min 18 --log "perf\logs\$atmName.log" -- python -I ports\scripts\opt_run.py --name $atmName @atmCommon --time-of-day 23 --weather stormy --tour volumetrics-views --tour-var "LEVELS=$atmArm" --tour-var 'SPLIT=0' --tour-var 'SWITCH_SETTLE=480s' --no-tour-shots --extra @atmFlags "--mcla_gpu_volumetrics=$atmArm" '--mcla_gpu_vol_temporal=true' '--mcla_gpu_vol_history_medium=true' "--mcla_gpu_vol_near=$atmNear" '--mcla_gpu_sun_shafts=off'
        }
        foreach ($atmArm in 'legacy','two-draw','merged') {
            $atmName = "dusk-atmosphere-$atmTier-$atmApi-$atmArm"
            $atmTaps = if ($atmArm -ne 'legacy') { '0' } elseif ($atmTier -eq 'igpu1080') { '32' } else { '64' }
            $atmMerge = if ($atmArm -eq 'merged') { 'true' } else { 'false' }
            python -I $atmLock --lock --keep-display --game --max-min 18 --log "perf\logs\$atmName.log" -- python -I ports\scripts\opt_run.py --name $atmName @atmCommon --time-of-day 17.7 --weather nice --tour w1-shaftlevels --tour-var 'LEVELS=default' --tour-var 'SETTLE_S=480' --no-tour-shots --extra @atmFlags '--mcla_gpu_volumetrics=ultra' '--mcla_gpu_sun_shafts=default' "--mcla_gpu_sun_shafts_steps=$atmTaps" "--mcla_gpu_atmosphere_merge=$atmMerge"
        }
    }
}
```

Verify the active render dimensions in each log. Compare vol inject, vol
integrate, and vol apply separately. Compare shafts sky plus shafts blur in
legacy vs two-draw (32 legacy taps on iGPU, 64 on RTX 5080). In two-draw vs merged, both effects must be active and
atmosphere apply must replace vol apply plus shafts add. Repeat two-draw to
determine the visual repeat floor for the merge. At the same dusk pose, take
matched full-resolution shots with the diagnostic paths and sweep the camera
past the sun. Check the fade and temporal trail on both APIs. Validate
single-effect frames and MSAA with the graphics debug layers.

## Integration

When merging feat/pause-photo-fx and feat/garage-fx, keep their G-buffer and
light replay changes alongside this branch's shared view classifier, lens
rules and garage lamp-list tag. Use the atmosphere-specific gate for both
atmosphere passes. Reconcile the overlapping scene and frame-order hunks.
With PRESETS-SAME-LOOK, carry over its final preset and Auto mappings and keep
the removal of class-specific shaft tap counts.
