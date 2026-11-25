# Visual comparison captures

These debug settings are read when a take starts. An empty
`mcla_capture_buffers` disables them.

| Setting | Value |
| --- | --- |
| `mcla_capture_buffers` | `all` or a comma-separated list of buffer names below |
| `mcla_capture_buffer_every` | 1 by default. Capture take frame 0 and every Nth frame after it |
| `mcla_capture_buffer_region` | `full` by default. `rects` uses the bounding rectangle of the scene's `rects` option |

Author one or more `x,y,width,height` rectangles in pixels of the unscaled capture
picture. The stream format describes one raster per buffer, so multiple scene
rectangles use their common bounding rectangle. Each target's crop rounds
outward on that target's native grid. The first copy of each stream logs its
source dimensions and crop origin. Use `sink none` to write buffers without a
legacy color capture and `mcla_capture_lag=1` to overlap capture with the next update.

Every available stream goes under `buffers/<name>/`. Its `meta.json` has `w`,
`h`, `channels`, `encoding`, `space` and `frames`. Each index names an
unpadded, top-down, row-major `frame_<n>.bin` with interleaved channels.
Indices count updates from the take's first frame. An unavailable frame is
omitted, and a stream with zero frames has no metadata. A stream that changes
dimensions, crop or source during the take fails visibly.

| Name | Channels, encoding, space | Renderer source |
| --- | --- | --- |
| final | RGB, u8, display-srgb | SDR present at the front buffer's size before the SDK overlay. HDR10 output has no SDR final stream |
| hdr | RGB, f16le, linear-scene | Main scene color resolve before the post chain and tone map. Inherits the game's resolve and exposure scaling |
| ao | Visibility, f16le, linear-scene | AO apply input, including the composed RTAO term when it ran |
| refl | RGB, f16le, linear-scene | SSR apply input, including RT fallback and its selected denoiser. With SSR off, uses the standalone car RT reflection if available. The car's paint consumes that history on the next frame |
| gi | RGB, f16le, linear-scene | Filtered screen-space bounce. When absent, uses the RT probe lookup where it has a half-traced target. The inline RT probe apply has no separate image |
| class | ID, u8, class-id | The G-buffer's material byte at guest sample zero, plus clear scene depth and the matching UI layer's alpha when available |
| motion | XY, f16le, motion-px | Temporal vectors in render pixels, previous minus current, +Y down, with jitter removed. Each vector covers a single update even with subsampling |

Class 0 is clear depth with no material, 1 is dry or wet road, 2 is paint or
vehicle chrome/metal, 3 is vehicle glass, 4 is vehicle interior where tagged,
5 is tagged building interior, 6 is foliage, 7 is matching UI coverage, and
255 is other. The generic world shader shares walls, props and unknowns, and
LOD combines distant buildings with ground. Both stay 255. Cockpit meshes
using generic vehicle shaders, blended glass, effects after the G-buffer
trigger, and UI without a same-grid split can be missing or inherit the class
behind them. The G-buffer's play-view and pack coverage rules still apply.

## Coordinator smoke

After merging the change into the test ref, run the build and pack steps
under the coordinator's leases from `C:\mcv\larecomp`. The smoke uses a new
scene and should be checked on both APIs before relying on its buffers.
The existing `scene_check.py` predates `rects`, so the command sends the scene
straight to the game's parser through the existing `scene-run` tour.

```powershell
$Ref = 'test/integration12-1124-opt'
$Tree = 'C:\mcv\larecomp\h11-work\integration12-1124-opt'
& .\perf\scripts\build-set-011.ps1 -Name vismet -Ref $Ref -Sdk setm -ExtraTargets 'larecomp_capture_visual_test,larecomp_scene_test,larecomp_take_test'
& .\ports\exe\vismet\larecomp_capture_visual_test.exe
& .\ports\exe\vismet\larecomp_scene_test.exe
& .\ports\exe\vismet\larecomp_take_test.exe
& .\perf\scripts\chain-verify-1122.ps1 -Stage pack -ExeName vismet -TestRef $Ref -TestWt $Tree
$Scene = Join-Path $env:TEMP 'vismet-smoke.scene'
@'
scene vismet-smoke fps 30
world hour 23 hold weather stormy
at 2 capture fps 30 sink none rects "160,80,800,560;960,240,160,320"
end 3
'@ | Set-Content -Encoding ascii -LiteralPath $Scene
$Packs = (Resolve-Path .\ports\scratch\packs-vismet).Path
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($Backend in 'd3d12', 'vulkan') {
    $Run = "vismet-smoke-$Backend-$Stamp"
    $Take = "C:\mcv\larecomp\ports\takes\$Run"
    python -I .\ports\scripts\opt_run.py --name $Run --exe-dir .\ports\exe\vismet --backend $Backend --scale 1 --window 1280x720 --log-level info --save mzlate --fps-limit 60 --no-tour-shots --tour-delay-s 0 --tour scene-run --tour-var "SCENE=$Scene" --tour-var 'STEPS=' --tour-var 'TIMEOUT_S=60' --tour-timeout-s 120 --max-run-s 240 --extra "--shader_pack_dir=$Packs" "--mcla_capture_out=$Take" --mcla_gpu_hdr=off --mcla_capture_buffers=all --mcla_capture_buffer_every=4 --mcla_capture_buffer_region=rects --mcla_capture_lag=1 --mcla_gpu_ao=rt --mcla_gpu_ssr=high --mcla_gpu_ssr_rt=high --mcla_gpu_ssgi=high --mcla_gpu_rt_gi=off
}
```

Check frame indices and byte lengths against the metadata, then inspect the last
sample, the HDR channel order, class edges and the sign of motion. Repeat with
`full` and a moving camera to check cross-buffer alignment.
