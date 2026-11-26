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
| reflhit | Visible source and RT query ID, u8, class-id | Current surface reflection trace on the `refl` grid, or native fallback material tags on the full G-buffer grid with SSR off |
| roadrefl | Combined luma and screen confidence, f16le, linear-scene | Half-resolution `road-reflection-lobe-v1` from SSR's apply input and the renderer's estimate of the native planar radiance |
| vol | RGB, f16le, linear-scene | Unit-gain volumetric in-scattering at the scene render size |
| shafts | RGB, f16le, linear-scene | Tinted, thresholded shaft light after spatial and temporal filtering, sampled at the scene render size |

The `atmosphere-light-v1` metadata lists deliberate zero-light updates in
`empty_frames`. Those updates have no binary payload. An update missing from
both lists lacks capture evidence. A fully gated stream has zero dimensions.

Score `roadrefl` on class 1 receivers. `combined_luma` is nonnegative
scene-linear luma before receiver shading. The native estimate uses the
current road view and game-strength scalar. `screen_confidence` records the
mean screen-ray confidence before RT fallback. The stream requires SSR to
run and uses four bytes per half-resolution pixel.

Class 0 is clear depth with no material, 1 is dry or wet road, 2 is paint or
vehicle chrome/metal, 3 is vehicle glass, 4 is vehicle interior where tagged,
5 is tagged building interior, 6 is foliage, 7 is matching UI coverage, and
255 is other. The generic world shader shares walls, props and unknowns, and
LOD combines distant buildings with ground. Both stay 255. Cockpit meshes
using generic vehicle shaders, blended glass, effects after the G-buffer
trigger, and UI without a same-grid split can be missing or inherit the class
behind them. The G-buffer's play-view and pack coverage rules still apply.

## Reflection hit IDs

`buffers/reflhit/meta.json` uses `domain: reflection-hit-v1` and lists both
channel names and all IDs. The raster stores two bytes per pixel in `visible`,
`rt_query` order. The metadata provides the uncropped grid as `source_w` and
`source_h`, and gives `x` and `y` for a cropped stream.

| IDs | Meaning |
| --- | --- |
| 0 | No attributed source or query |
| 1–6 | SSR vehicle, tagged building interior, road, sky, emissive, other |
| 7, 8 | Native road reflection view, environment map |
| 16–23 | RT city unclassified, world, road, wet road, sidewalk, terrain, LOD, foliage |
| 24–31 | RT city paint, chrome, metal, glass, skin, emissive, interior, water |
| 32-34 | RT query only: discarded car hit, miss, unresolved city hit |
| 35 | Shaded RT car hit |
| 36 | Validated previous-frame screen hit |

For `source: surface-trace`, the visible ID describes the largest current
source weight from current screen hits, previous-frame reuse, RT, and the native fallback. At two rays,
the highest-fade ray represents each traced source. Tags precede temporal or
spatial filtering, so they identify this update's source, including when the
color history still contains an older hit. The RT lane records one query class
per traced pixel. A resolved city hit takes precedence. Without one, the lane
records car, unresolved city, or miss in that order. The city pass supplies no
color for those three outcomes. SSR sky records sky sampled from the picture.
Generic world geometry can include walls, props and distant buildings and is
grouped as SSR other or RT world/LOD. Late lights can lack emissive tags.

For `source: native-material`, the IDs identify the expected fallback from
the receiver material and whether the planar view drew this frame. ID 7 marks
roads, reflective glass or water with that view. ID 8 marks reflective
non-road materials using the environment fallback. Native radiance, including
what geometry the planar view shows, is already in scene color and has no hit
labels here. A missing G-buffer or a wanted SSR pass without a usable trace
produces a missing sample.

For `accept.py`, read `np.uint8` and reshape to `(h, w, 2)`. Join matching
frame indices and nearest-sample `class == 1` on the hit grid using normalized
pixel centers on full captures. Report road coverage (`visible != 0`) and
source mix over covered road pixels. Split known hit classes using the visible
lane, with ID 1 as a vehicle contribution. Keep coverage by native source and
rejected RT car queries as separate measurements. Vehicle share for matched
TOP and PLAYER frames requires attributable hit content on both arms. Mark
native-only vehicle share UNPROVEN. The `refl` manifest identifies
`ssr-resolve-input` or `car-rt-history`. The SSR RGB can be signed because the
renderer subtracts native road-view color before adding the traced result.
Neither RGB source gives total wet-road reflection energy.

With `reflhit` enabled, SSR writes an extra 4-byte target per trace texel
during the take. The present tag controls which updates copy and write it.
With SSR off, a capture-only draw writes the same format on sampled updates
at G-buffer size. Full 1080p samples copy about 2 MiB for SSR or 8 MiB for
native at 1x render scale, and write about 1 MiB or 4 MiB respectively. The
capture pool reserves from the largest grid in the take. Shader and pipeline
objects are created on first use. Normal frames use the regular shaders,
attachments and RT descriptor layout.

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

For the reflection stream, substitute the merged test ref and its worktree in
the build and pack commands. The three arms exercise the native view, SSR and
the RT fallback independently of the presets.

```powershell
$HitRef = 'test/refl-hit-class-opt'
$HitTree = 'C:\mcv\larecomp\h11-work\refl-hit-class-opt'
& .\perf\scripts\build-set-011.ps1 -Name reflhit -Ref $HitRef -Sdk rsync -ExtraTargets 'larecomp_capture_visual_test,larecomp_ssr_test,larecomp_ssr_rt_rules_test'
& .\perf\scripts\chain-verify-1122.ps1 -Stage pack -ExeName reflhit -TestRef $HitRef -TestWt $HitTree
$HitPacks = (Resolve-Path .\ports\scratch\packs-reflhit).Path
$HitScene = Join-Path $env:TEMP 'reflhit-smoke.scene'
@'
scene reflhit-smoke fps 30
world hour 23 hold weather stormy
at 2 capture fps 30 sink none
end 3
'@ | Set-Content -Encoding ascii -LiteralPath $HitScene
$HitStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($HitBackend in 'd3d12', 'vulkan') {
    foreach ($HitSsr in 'off', 'high', 'ultra') {
        $HitRt = if ($HitSsr -eq 'ultra') { 'high' } else { 'off' }
        $HitRun = "reflhit-$HitBackend-$HitSsr-$HitStamp"
        $HitTake = "C:\mcv\larecomp\ports\takes\$HitRun"
        python -I .\ports\scripts\opt_run.py --name $HitRun --exe-dir .\ports\exe\reflhit --backend $HitBackend --scale 1 --window 1280x720 --log-level info --save mzlate --fps-limit 60 --no-tour-shots --tour-delay-s 0 --tour scene-run --tour-var "SCENE=$HitScene" --tour-var 'STEPS=' --tour-var 'TIMEOUT_S=60' --tour-timeout-s 120 --max-run-s 240 --extra "--shader_pack_dir=$HitPacks" "--mcla_capture_out=$HitTake" --mcla_gpu_hdr=off '--mcla_capture_buffers=class,refl,reflhit' --mcla_capture_buffer_region=full --mcla_capture_buffer_every=1 --mcla_capture_lag=1 "--mcla_gpu_ssr=$HitSsr" "--mcla_gpu_ssr_rt=$HitRt"
    }
}
```

Check a parked street with cars in the visible reflections. Compare per-frame
ID maps against the picture and `refl`, check that IDs 32–34 occur only in the
query lane, and verify the native stream and the SSR stream each use their
declared grid. Then repeat with a crop, subsampling and API validation on.
