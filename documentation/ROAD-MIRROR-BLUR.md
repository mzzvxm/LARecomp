# Wet-road planar reflection

`mcReflectionPass` redraws the LOD city in phase `0x2` with the main camera. The vertex shaders mirror the geometry about the plane in `gReflectionParams.z`. The pass resolves its 2× MSAA colour target (EDRAM tile 128, pitch 320, 2:10:10:10 float) into a 256×256, single-level texture. The census counted 43–257 draws in the reflection view.

| Stage | Shader | Hash |
| --- | --- | --- |
| Side view VS | `xCityLOD::VSCityLODReflect` | `790CBAB21806E539` |
| Side view VS | `xCityLOD::VSCityLODReflectNight` | `32083F74A3089E75` |
| Side view VS | `xCityDefaultLOD::VSCityDefaultLODReflect` | `FCB95F4D8D6FD852` |
| Side view VS | `xCityGroundLOD::VSCityGroundLODReflect` | `8FC83E3377051D77` |
| Side view VS | `xCityWindowLOD::VSCityLODReflect` | `62D5D2B83744D3F9` |
| Side view PS | Four LOD city shaders observed in the census | `15953A3E036B16DB`, `A50FD69C0F0739C1`, `35F41762995C91B9`, `37BB7CE96694769E` |
| Road PS, day | `xCityRoad::PSCityRoad` | `BD23D361E8AB343A` |
| Road PS, night | `xCityRoad::PSMultiLight` | `A59C1BB99CEA3F7B` |
| Tunnel road PS, day | `xCityTunnelRoad::PSCityTunnelRoad` | `8EC549775CE702DF` |
| Tunnel road PS, night | `xCityTunnelRoad::PSMultiLight` | `922697AD3493F601` |

The road pixel shader divides the projected UV by W, adds the ripple, samples `ReflectionSampler`, and blends the texel over the lit road using the game's wetness and puddle terms. Windows and water sample the same resolve. `side_view_reflection.cpp` identifies the pass and implements ROAD REFLECTIONS. GAME keeps the 256² view and two samples. HIGH and ULTRA draw it one sample per pixel at 512² and 768² respectively. The resolve is read directly at that size. The rain showcases show hard building edges at both resolutions.

## Filter

`mcla_gpu_road_mirror_blur` is a live switch under **DEBUG → PICTURE FIXES → ROAD MIRROR BLUR**, off by default. When enabled, a pixel shader variant softens the four road shaders above. It runs a separable 3×3 Gaussian at the existing lookup, using mip 0 of the already resolved side view. This keeps the change local to asphalt, and works independently of SSR.

The wetness register supplies a roughness proxy and ramps the change in from 0.18 to 0.8. The observed dry value of 0.095 uses the original lookup. For saturated rain the kernel's vertical tap spacing grows from 0.65 to 2.5 base-view texels over 4–32 metres of view depth. The horizontal spacing is 5/8 of that in UV to account for the wider screen. The footprint uses the same UV scale at every ROAD REFLECTIONS setting. The existing reflection blend fades back to the lit road over 10–48 metres of view depth. Puddle darkening and ripples still come from the guest shader.

The road stage composes with the forward headlight and G-buffer stages. On night roads the G-buffer captures the faded blend weight for deferred lights. If a new pipeline is pending or fails, the renderer can fall back to drawing the road without this filter. The variant's version and helper fingerprint invalidate the variant stamp at setup. The pack layout stays at v3.

## Capture and validation

Build a showcase-capable test ref containing this branch and use its matching pack. The commands below assume the coordinator names the ref `test/road-mirror-blur-opt` and the executable set `roadblur`. Run the build under the coordinator's lease.

```powershell
$perfScripts = 'C:\mcv\larecomp\perf\scripts'
$roadPack = 'C:\mcv\larecomp\ports\scratch\packs-roadblur'
& "$perfScripts\build-set-011.ps1" -Name roadblur -Ref test/road-mirror-blur-opt -Sdk setm -ExtraTargets larecomp_road_mirror_blur_test
& 'C:\mcv\larecomp\ports\exe\roadblur\larecomp_road_mirror_blur_test.exe'
& "$perfScripts\make-pack-011.ps1" -Name roadblur -Exe roadblur -Ref test/road-mirror-blur-opt -PacksDir $roadPack
& "$perfScripts\chain-showcase.ps1" -Stage shots,post -Weathers rain -ExeName roadblur -TestRef test/road-mirror-blur-opt -UltraExtra 'mcla_gpu_road_mirror_blur:off,mcla_gpu_ssr:off,mcla_gpu_ssr_rt:off' -OutTag roadblur_off
& "$perfScripts\chain-showcase.ps1" -Stage shots,post -Weathers rain -ExeName roadblur -TestRef test/road-mirror-blur-opt -UltraExtra 'mcla_gpu_road_mirror_blur:on,mcla_gpu_ssr:off,mcla_gpu_ssr_rt:off' -OutTag roadblur_on
```

Compare the ULTRA images of the two runs at the grazing cameras, including the reflected building edge, nearby light streaks and the road toward the horizon. Check the live toggle, Vulkan and D3D12, and GPU timing with ROAD REFLECTIONS held at the same value.

`chain-ssr-motion.ps1` currently accepts only integ8/integ9 and has no extra-cvar parameter. To use its existing pinned and dolly scenes for this A/B, allow the new executable set in both the PowerShell and `ssr_motion.py` arguments. Forward a `-RoadMirrorBlur on|off` parameter as `--mcla_gpu_road_mirror_blur=<value>` in `opt_run.py`'s `--extra` argument list. Use separate tags and start with `-Smoke`. Compare the two runs' base clips, which keep SSR and its RT fallback off. Those scenes use stormy weather, so run the rain showcase for the still comparison.

`larecomp_road_mirror_blur_test` checks the filter math and transform with a built-in fixture. It also takes three optional arguments together, the local translated `city` shader folder, `src/gpu/hlsl/gbuffer_common.hlsli` and a scratch directory for the transformed compositions. Compile those with the shader pack's DXC flags (`ps_6_0`, `main`, `XENOS_SPEC_CONSTANTS=0` and `2`), then validate the SPIR-V with `spirv-val --target-env vulkan1.2`. Translated shaders and blobs stay in scratch.
