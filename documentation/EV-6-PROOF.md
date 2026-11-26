# EV-6 cabin mirror proof

The coordinator runs this after committing `perf/interior-mirror`. The Opt build and both capture paths below are unproven. Use D3D12 on the main PC and Vulkan on the ROG.

## Build the Opt executable

From `C:\mcv\larecomp` on the main PC, using the existing test harness and SDK:

```powershell
git -C src worktree add -b test/ev6-opt C:/mcv/larecomp/h11-work/ev6-opt test/land-1126-opt
git -C h11-work/ev6-opt merge --no-edit perf/interior-mirror
& .\perf\scripts\build-set-011.ps1 -Name ev6 -Ref test/ev6-opt -Sdk rsync
& .\perf\scripts\make-pack-011.ps1 -Name ev6 -Exe ev6 -Ref test/ev6-opt -PacksDir C:\mcv\larecomp\ports\scratch\packs-ev6
```

Run the build and pack scripts through the coordinator's build lease. Stage the same executable, packs, harness and test save on the ROG.

## Census and pictures

Choose 12-16 verified garage slots from the scratch save. Cover the R34, another tuner, muscle, exotic, an SUV/truck, a classic, a convertible in both roof states where available, and a bike. Keep a slot-to-model list. The tour uses the existing stock camera cycle. Set `CAM_RB` from the save's starting rig and accept a cockpit shot only if the log confirms rig 1 after that slot loads.

```powershell
$EvRoot = 'C:\mcv\larecomp'
$EvProof = "$EvRoot\perf\out\ev6"
$EvSlots = 0..13   # Replace with the verified slots from the scratch save.
$EvLock = 'C:\Users\renan\AppData\Local\Temp\claude\C--Games-Midnight-Club-Los-Angeles\a2893308-fcd9-49e0-ac7b-39325fffb1ef\scratchpad\tools\lockrun.py'
New-Item -ItemType Directory -Path $EvProof -Force | Out-Null

@'
#@title EV-6 cabin mirror census
#@duration 180s
#@default SCENE=ev6.scene
#@default CAM_RB=a,b,c
wait_until roam 60s
wait 12s
scene {SCENE}
wait_until scene_running timeout=60s
wait_until scene_t >= 5 timeout=60s
scene_at 5
wait_until f32 0x827E0E34 >= 570 timeout=60s optional
wait 12s
@foreach {CAM_RB}: press rb 4f ; wait 1200ms
wait 5s
shot cabin_on
set mcla_gpu_car_dump 1
wait 2s
set rearview_hide_with_interior false
wait 3s
shot both
set rearview_hide_with_interior true
set rearview_interior false
wait 3s
shot cabin_off
set rearview_interior true
wait 3s
press rb 4f
wait 3s
shot bumper
scene off
'@ | Set-Content -LiteralPath "$EvProof\census.tour" -Encoding ascii

foreach ($EvSlot in $EvSlots) {
    $EvScene = "$EvProof\slot-$EvSlot.scene"
    @"
at 0 car player profile $EvSlot
at 1 car player at 1033.50,15.27,-572.88 heading 88.4
at 1.5 car marker spawn vp_chv_camaross_69 at 1024.63,15.14,-577.82 heading 88.4 paint 2
end 20
"@ | Set-Content -LiteralPath $EvScene -Encoding ascii
    python -I "$EvRoot\ports\scripts\scene_check.py" --target director $EvScene
    if ($LASTEXITCODE) { throw "Scene lint failed for slot $EvSlot" }

    $EvRun = "ev6-d3d12-slot$EvSlot"
    $EvArgs = @('-I', "$EvRoot\ports\scripts\opt_run.py", '--name', $EvRun,
        '--exe-dir', "$EvRoot\ports\exe\ev6", '--backend', 'd3d12',
        '--save', 'mzlate', '--starter', "$EvRoot\ports\scratch\starter\merged-1.bin",
        '--window', '1920x1080', '--scale', '0', '--msaa', '2', '--log-level', 'info',
        '--time-of-day', '12', '--weather', 'nice', '--tour', "$EvProof\census.tour",
        '--tour-var', "SCENE=$EvScene", '--tour-timeout-s', '240', '--tour-shots-full',
        '--extra', "--shader_pack_dir=$EvRoot\ports\scratch\packs-ev6",
        '--mcla_gpu_side_mirrors=on', '--rearview=true')
    python -I $EvLock --lock --untimed --game --max-min 12 -- python @EvArgs
    if ($LASTEXITCODE) { throw "Game run failed for slot $EvSlot" }
    rg -n 'camera rig |\[cabin mirror\]|\[cabin mask\]|\[car dump\]' "$EvRoot\ports\run\$EvRun" -g larecomp.log
}
```

On the ROG, stage the generated tour and scenes to `C:\larecomp-test\proof\ev6`, the car census Opt executable to `exe\ev6`, the packs to `packs\ev6`, and its starter to `starter\ev6-mcla_pipelines.bin`. Run the same verified slots through the existing ROG harness in its game lane.

```powershell
$EvRog = 'C:\larecomp-test'
foreach ($EvSlot in $EvSlots) {
    $EvRun = "ev6-vulkan-slot$EvSlot"
    & "$EvRog\python\python.exe" -I "$EvRog\scripts\opt_run_laptop.py" --name $EvRun `
        --exe-dir "$EvRog\exe\ev6" --backend vulkan --save mzlate --window 1920x1080 --scale 0 --msaa 2 `
        --log-level info --time-of-day 12 --weather nice --starter "$EvRog\starter\ev6-mcla_pipelines.bin" `
        --tour "$EvRog\proof\ev6\census.tour" --tour-var "SCENE=$EvRog\proof\ev6\slot-$EvSlot.scene" `
        --tour-timeout-s 240 --tour-shots-full --extra "--shader_pack_dir=$EvRog\packs\ev6" `
        --mcla_gpu_side_mirrors=on --rearview=true
    if ($LASTEXITCODE) { throw "Game run failed for slot $EvSlot" }
    rg -n 'camera rig |\[cabin mirror\]|\[cabin mask\]|\[car dump\]' "$EvRog\runs\$EvRun" -g larecomp.log
}
```

Record the final key, draw, area and width for each car. Check the shots for the selected glass, rear window opening, pillars and headrests, and for the marker behind-left on the left side of the reflection. A bike should keep the HUD view and have no cabin overlay. Compare `cabin_on`, `both`, `cabin_off` and `bumper` for the live switches. Check chase, pause, photo and garage separately.

Rerun the R34 at night in rain and in a tunnel. For performance, run the same scene with the Opt timing harness, `--mcla_gpu_pass_timing=true` and `--mcla_gpu_pass_timing_interval=1200`, and hold the cockpit long enough to record several intervals. Read `mirror view`, `cabin mask`, `mirror overlay` and the mask refresh lines. Use the D3D12 debug layer on a separate diagnostic run and a Vulkan validation build on the ROG. Confirm the queued colour-write to shader-read transition and the absence of new validation errors.
