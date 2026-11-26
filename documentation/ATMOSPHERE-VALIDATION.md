# Atmosphere validation

Use the same executable on both sides of a comparison. These runs are unproven
on this branch. Smoke one short capture on each API before running the pairs.

## POST-ATM isolated light and pass cost

After committing the staged POST-3 merge and the renderer changes, merge the
branch into `test/post-atm-opt` based on `test/land-1126-opt`. Apply the VISMET
patch in the tools workspace.

```powershell
Set-Location C:\mcv\larecomp
git apply --check .\h11-work\atm-cheap\vismet-atmosphere.patch
git apply .\h11-work\atm-cheap\vismet-atmosphere.patch
$postExe = 'post-atm'
$postRef = 'test/post-atm-opt'
& .\perf\scripts\build-set-011.ps1 -Name $postExe -Ref $postRef -Sdk setm -Patches none -ExtraTargets 'larecomp_atmosphere_test,larecomp_volumetrics_amortize_test,larecomp_frame_order_test'
& .\perf\scripts\make-pack-011.ps1 -Name $postExe -Exe $postExe -Ref $postRef
. .\perf\scripts\lockrun-lease.ps1
$postLock = Find-LockrunPy
$postAccept = '.\perf\scripts\vismet\accept.py'
foreach ($postMode in 'presence', 'merge', 'shafts') {
    foreach ($postConfig in 'PLAYER', 'TOP') {
        $postTag = "post-atm-$postMode-$postConfig-dx"
        python -B $postAccept --prepare $postTag --ab --atmosphere $postMode --config $postConfig --exe $postExe --lockrun $postLock --backend d3d12
        Get-Content "perf\out\look-$postTag\capture-plan.txt"
    }
    $postTag = "post-atm-$postMode-rog"
    python -B $postAccept --prepare $postTag --ab --atmosphere $postMode --config PLAYER --exe $postExe --backend vulkan --rog
    Get-Content "perf\out\look-$postTag\capture-plan.txt"
}
```

Run the printed commands for each plan. On the ROG they stage the scenes, print
and launch the lane queue, and judge the pulled takes. Run `presence` first and
use its dry-dusk A/A row as the repeat floor. Read the per-frame presence and
gating rows, isolated FLIP, mean linear-luma ratio, darkest-20% luma, and the
dry-dusk adjacent-frame residual. Confirm the merged arm's final FLIP and
isolated light are at the repeat floor. Compare the cheap shaft arm's sky FLIP
to 0.25 mean and 0.60 p95 and its isolated temporal row to the parked repeat.
The expected dry pose and gates need a live smoke. Check the sun leaving the
frame with a slow camera pan at the dry spot. Open the full map during an
isolated take and require gated indices for both streams. Verify garage,
photo and pause after the UI and camera have settled. Recheck the paired
and single-effect paths with 2x MSAA and backend validation.

Measure pass cost with captures disabled. The dry tour follows the existing
parked pose. The wet tour uses the PERF-TIERS scene. Launch these lanes serially
and require complete steady 3600-present windows.

```powershell
$postWet = (Resolve-Path perf\scripts\perf_tiers.scene).Path
$postWetRog = 'C:/larecomp-test/atmosphere/perf_tiers.scene'
ssh -o BatchMode=yes rog 'cmd /c mkdir C:\larecomp-test\atmosphere'
scp -B $postWet "rog:$postWetRog"
$postCommon = @('--msaa','2','--fps-limit','0','--log-level','info','--save','mzlate','--no-pm','--max-run-s','850','--tour-timeout-s','730','--no-tour-shots')
$postFlags = @('--mcla_gpu_preset=custom','--mcla_gpu_hdr=off','--mcla_gpu_upscaler=off','--mcla_gpu_frame_generation=off','--mcla_gpu_volumetrics=ultra','--mcla_gpu_sun_shafts=default','--mcla_gpu_headlights=on','--mcla_gpu_light_shadows=medium','--mcla_gpu_pass_timing=true','--mcla_gpu_scene_profile=true')
$postRog = @()
foreach ($postArm in 'legacy','separate','merged','wet-oracle','wet-reuse') {
    $postTaps = if ($postArm -eq 'legacy') { '64' } else { '0' }
    $postMerge = if ($postArm -eq 'separate') { 'false' } else { 'true' }
    $postReuse = if ($postArm -eq 'wet-oracle') { 'false' } else { 'true' }
    $postSwitches = @("--mcla_gpu_sun_shafts_steps=$postTaps","--mcla_gpu_atmosphere_merge=$postMerge","--mcla_gpu_vol_amortize=$postReuse")
    $postDryTour = @('--time-of-day','17.7','--weather','nice','--tour','w1-shaftlevels','--tour-var','LEVELS=default','--tour-var','SETTLE_S=480')
    $postWetTour = @('--time-of-day','20','--weather','stormy','--tour','scene-test','--tour-var','SHOTS=','--tour-var','PRELOAD_S=20','--tour-var','TIMEOUT_S=700','--tour-var','AFTER_S=0','--tour-var')
    $postTourDx = if ($postArm -like 'wet-*') { $postWetTour + "SCENE=$postWet" } else { $postDryTour }
    $postTourVk = if ($postArm -like 'wet-*') { $postWetTour + "SCENE=$postWetRog" } else { $postDryTour }
    foreach ($postScale in '1.5', '3') {
        $postRun = "post-atm-dx-$($postScale.Replace('.','_'))-$postArm"
        python -I $postLock --lock --keep-display --game --max-min 18 --log "perf\logs\$postRun.log" -- python -I ports\scripts\opt_run.py --name $postRun --exe-dir "ports\exe\$postExe" --backend d3d12 --adapter-name 'NVIDIA GeForce RTX 5080' --window 1280x720 --scale $postScale @postCommon @postTourDx --extra "--shader_pack_dir=$PWD\ports\scratch\packs-$postExe" @postFlags @postSwitches
    }
    foreach ($postGpu in 'rtx','intel') {
        $postDevice = if ($postGpu -eq 'rtx') { 'NVIDIA GeForce RTX 4080 Laptop GPU' } else { 'Intel(R) RaptorLake-S Mobile Graphics Controller' }
        $postRog += @{name="post-atm-vk-$postGpu-$postArm"; exe=$postExe; packs="packs-$postExe"; backend='vulkan'; title_blind=($postGpu -eq 'intel'); args=@('--adapter-name',$postDevice) + $postCommon + $postTourVk + @('--extra') + $postFlags + $postSwitches}
    }
}
$postQueue = 'perf\out\post-atm-cost-rog.json'
$postRog | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii $postQueue
python -I perf\scripts\laptop\laptop_lane.py $postQueue --plan
python -I perf\scripts\laptop\laptop_lane.py $postQueue
Get-ChildItem ports\run -Directory -Filter 'post-atm-*' | ForEach-Object {
    rg 'pass timing over|first sun shafts pass|first volume|froxel amortization' "$($_.FullName)\larecomp.log"
}
```

For AMD iGPU coverage on the main PC, use the exact adapter name from the
boot log and the 1.5-scale arm, with distinct run names. At each size check
render size and sample count in the effect logs. Compare `shafts sky` plus
`shafts blur` in legacy and merged (target at least 70% less). Compare
`vol apply` plus `shafts add` in separate against `atmosphere apply` in
merged. Verify the wet arms have no shaft work and compare `vol inject` plus
`vol integrate` against `vol cull` plus `vol inject + integrate`. Run the
POST-3 moving-headlight proof below for trail and history behavior.

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

## POST-3 froxel amortization

Merge `perf/vol-amortize` into `test/vol-amortize-opt` with the same scene and
VISMET harness as above. Build and package once on the main PC. Use that
executable on both sides of each comparison. The FIXES row **REUSE VOLUME
SAMPLES** switches the path live. The first volume log reports which path ran.

```powershell
& .\perf\scripts\build-set-011.ps1 -Name vol-amortize -Ref test/vol-amortize-opt -Sdk setm -ExtraTargets larecomp_volumetrics_amortize_test
& .\perf\scripts\make-pack-011.ps1 -Name vol-amortize -Exe vol-amortize -Ref test/vol-amortize-opt
```

Run the following from the harness workspace on each host. Use D3D12 on the
main PC and Vulkan on the ROG. Stage the same exe, packs and scenes on the ROG,
including `perf\out\volumetrics\vol-rain.scene` from the volumetrics chain.
Use the exact adapter name from the host's log if it has more than one GPU.

```powershell
. .\perf\scripts\lockrun-lease.ps1
$volLock = Find-LockrunPy
$volApi = 'd3d12' # main PC. Use 'vulkan' on the ROG.
$volExe = 'vol-amortize'
$volPacks = "$PWD\ports\scratch\packs-$volExe"
$volStarter = "$PWD\ports\scratch\starter\merged-1.bin"
$volShare = @('--exe-dir', "$PWD\ports\exe\$volExe", '--backend', $volApi, '--window', '1280x720', '--scale', '1.5', '--save', 'mzlate', '--starter', $volStarter)
$volSettings = @('--mcla_gpu_preset=custom', '--mcla_gpu_hdr=off', '--mcla_gpu_upscaler=off', '--mcla_gpu_sun_shafts=off', '--mcla_gpu_headlights=on', '--mcla_gpu_light_shadows=ultra', '--mcla_gpu_vol_stable=true')
$volTakes = "ports\takes\post3-$volApi"
foreach ($volMode in 'base', 'change', 'reference') {
    $volReuse = if ($volMode -eq 'change') { 'true' } else { 'false' }
    $volTemporal = if ($volMode -eq 'reference') { 'false' } else { 'true' }
    foreach ($volScene in 'vol-hl-pass', 'vol-stable-chase-fog', 'vol-stable-dolly') {
        if ($volMode -eq 'reference' -and $volScene -ne 'vol-hl-pass') { continue }
        $volInput = @{ 'vol-hl-pass' = 2; 'vol-stable-chase-fog' = 3; 'vol-stable-dolly' = 1 }[$volScene]
        $volView = if ($volScene -eq 'vol-hl-pass') { 1 } else { 2 }
        python -I $volLock --lock --keep-display --game --max-min 15 --log "perf\logs\post3-$volApi-$volScene-$volMode.log" -- python -I .\ports\scripts\scene_run.py @volShare --scene $volScene --out "$volTakes\$volScene-$volMode" --shader-pack-dir $volPacks --extra @volSettings '--mcla_gpu_volumetrics=high' "--mcla_gpu_vol_inputs=$volInput" "--mcla_gpu_vol_view=$volView" "--mcla_gpu_vol_amortize=$volReuse" "--mcla_gpu_vol_temporal=$volTemporal"
    }
}
foreach ($volMode in 'base', 'change') {
    python .\perf\scripts\vol_stable_metric.py trail "$volTakes\vol-hl-pass-$volMode" "$volTakes\vol-hl-pass-reference"
}
foreach ($volScene in 'vol-stable-chase-fog', 'vol-stable-dolly') {
    python .\perf\scripts\vol_stable_metric.py measure "$volTakes\$volScene-base" --json
    python .\perf\scripts\vol_stable_metric.py measure "$volTakes\$volScene-change" --json
}
```

Read the trail percentage (at most 1.76%) and the VOL-STABLE flicker and pop
metrics. Smoke the scenes first. For viewport and cockpit proof, prepare
matched frame captures and run the resulting capture plan. Append
`--mcla_gpu_vol_view=1` to both sides of a second capture for the isolated light.
Use the final-colour pair for the look verdict.

```powershell
$volTag = "post3-look-$volApi"
python -B .\perf\scripts\vismet\accept.py --prepare $volTag --ab --config PLAYER --views night-rain-street,tunnel,cockpit --exe $volExe --lockrun $volLock --backend $volApi --set mcla_gpu_vol_amortize=true --set mcla_gpu_volumetrics=medium --set mcla_gpu_vol_temporal=true --set mcla_gpu_vol_history_medium=true --set mcla_gpu_vol_near=2.0 --set mcla_gpu_sun_shafts=off
Get-Content "perf\out\look-$volTag\capture-plan.txt" | ForEach-Object {
    if ($_ -match 'opt_run\.py' -and $_ -match "look-$volTag[\\/]BASE[\\/]") {
        $_ + " '--mcla_gpu_vol_amortize=false' '--mcla_gpu_volumetrics=medium' '--mcla_gpu_vol_temporal=true' '--mcla_gpu_vol_history_medium=true' '--mcla_gpu_vol_near=2.0' '--mcla_gpu_sun_shafts=off'"
    } else { $_ }
}
python -B .\perf\scripts\vismet\accept.py --ab "perf\out\look-$volTag\BASE" "perf\out\look-$volTag\CHANGE" --name $volTag --run-tag $volTag --views night-rain-street,tunnel,cockpit
```

Use the POST-0 timing tour above with Medium, history on and the 2 m near
plane in both arms. For a wet-night pair with the same harness flags:

```powershell
foreach ($volReuse in 'false', 'true') {
    $volRun = "post3-cost-$volApi-$volReuse"
    python -I $volLock --lock --keep-display --game --max-min 18 --log "perf\logs\$volRun.log" -- python -I .\ports\scripts\opt_run.py --name $volRun @volShare --msaa 2 --fps-limit 0 --log-level info --no-pm --max-run-s 850 --tour-timeout-s 730 --time-of-day 23 --weather stormy --tour volumetrics-views --tour-var "SCENE=$PWD\perf\out\volumetrics\vol-rain.scene" --tour-var 'LEVELS=medium' --tour-var 'SPLIT=0' --tour-var 'SWITCH_SETTLE=480s' --no-tour-shots --extra @volSettings "--shader_pack_dir=$volPacks" '--mcla_gpu_volumetrics=medium' '--mcla_gpu_vol_temporal=true' '--mcla_gpu_vol_history_medium=true' '--mcla_gpu_vol_near=2.0' '--mcla_gpu_pass_timing=true' '--mcla_gpu_scene_profile=true' "--mcla_gpu_vol_amortize=$volReuse"
    rg 'vol cull|vol inject|vol integrate|vol apply|atmosphere apply|first volume' "ports\run\$volRun"
}
```

Compare complete 3600-frame windows. Add `vol cull` to `vol inject + integrate`
on CHANGE and compare to `vol inject` plus `vol integrate` on BASE. The
POST-3 target is at least 55% less compute time on the iGPU and GTX 1650,
with 0.03 ms as the timing noise floor. Check composite timing separately.
Run the short pairs again with D3D12 validation (`--mcla_gpu_debug_layer=true`)
and with Vulkan validation (a Debug Plume build on the ROG). Inspect for
descriptor or barrier warnings and flip the FIXES row during a camera cut,
in cockpit and during a resize. The proof runs and visual verdicts are unproven.

## Integration

When merging feat/pause-photo-fx and feat/garage-fx, keep their G-buffer and
light replay changes alongside this branch's shared view classifier, lens
rules and garage lamp-list tag. Use the atmosphere-specific gate for both
atmosphere passes. Reconcile the overlapping scene and frame-order hunks.
With PRESETS-SAME-LOOK, carry over its final preset and Auto mappings and keep
the removal of class-specific shaft tap counts.
