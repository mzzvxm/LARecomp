# Graphics showcase trailer

Round 1 asks whether the seven frozen Director shots can carry two clean preset wipes. LAND a shot with three exact 3840×2160, 30 FPS plates, zero lost frames, cross-pass times within 1 μs, player position within 0.10 m per axis and hour within 0.00001 h. Review both wipes at playback speed for a visible change in the named subject and a continuous camera move. DROP a valid shot when the feature change is unreadable. Discard invalid evidence and retry only after fixing its cause. PARK unresolved shots after round 2 with the failed check and what would reopen the shot.

The smoke must confirm RT availability, a settled free-roam scene, a valid camera and a 3840×2160 first still with more than four luma codes between its 2nd and 98th percentiles after downsampling to 32×18. The full render continues only after those checks pass. For the review, use the shot name and preview time, such as `04 Sunset chrome, 00:19.2, Camaro disappears from the door`.

## Shot list

Each row describes the intended look. The preview starts each shot in VANILLA, wipes left to right over 0.6 s at +1.5 s and +3.5 s, and switches the label at each wipe midpoint (+1.8 s and +3.8 s). The same times are authored as preset cuts for live Director playback. All shots hold hour and weather and turn off ambient traffic, parked spawns, pedestrians and police. Any cars staged for the comparison are stationary. The cockpit uses a locked gameplay camera. The other six have authored camera moves.

For live-only review of shot 03, select `camera.rig {"rig":1}` after loading the scene. The render runner does this automatically.

| Shot and preview time | VANILLA | ENHANCED | ENHANCED RT |
| --- | --- | --- | --- |
| **01 Broadway at night** · 00:00–00:06 | Establish the shop fronts, parked hero and stock pools of light. | Watch for better definition around the hero and along the pavement under the lamps. | Watch for off-screen detail in the car's reflections and tighter light occlusion around the curb and car. |
| **02 Wilshire, wet at dusk** · 00:06–00:12 | Establish the dusk sky and broad wet-road highlights. | Look for screen-space lamp and car reflections in the lane nearest camera. | Look for longer, continuous reflections when the reflected source moves outside the frame. |
| **03 Night cockpit** · 00:12–00:17 | Establish the steering wheel, dash, windshield and base cabin brightness. | Look for visible cabin separation and the side-mirror view where supported by the car. | Look for contact detail at wheel, dash and pillars while the windshield and cabin remain readable. |
| **04 Sunset chrome** · 00:17–00:22 | Start on the Skyline's door, trim and stock environment reflections. | Look for sharper reflections and self-reflection around the body contours. | Look for the orange Camaro reflected in the Skyline's flank as the camera slides closer. |
| **05 Sunset Boulevard** · 00:22–00:29 | Frame the sun, palms and the game's shadow edges across the lanes. | Compare shadow filtering, car contact and shafts through the palms. | Compare ray-traced occlusion where the palm shadows cross the road and sidewalk. |
| **06 Selma in rain** · 00:29–00:35 | Frame the oncoming car, wet pavement and streetlights from the curb. | Look for headlight volume, light pools and the car's wet-road reflection. | Look for beam and lamp occlusion behind the car and more complete reflections near frame edges. |
| **07 Sixth Street underpass** · 00:35–00:40 | Establish the covered bay and the light spill below the viaduct. | Look for contact shadows and separation between road and ceiling. | Look for the deck blocking the street lamps above it while the headlight pool stays visible. |

The underpass supplies the enclosed lighting shot within free roam. The known Hollywood garage interior is reached through `GARAGEMOVIE`, while preset plate capture requires a ready free-roam world and a repeatable Director-controlled scene.

## Render and review

From `C:\mcv\larecomp`, use the preset-track build at `82362553` and its matching shader pack and city packs from `director-preset-smoke`. The renderer takes one exclusive game lease on the RTX 5080 with D3D12 and borderless output on the physical 4K desktop. The compositor needs FFmpeg with `xfade`, `drawtext`, FFV1 and libx264. Allow space for 3,600 4K PNGs, up to about 84 GiB before compression, plus the master and compositor intermediates.

```powershell
$kit = 'h11-work/showcase-scenes/perf/scripts/trailer'
$tag = 'trailer-showcase-r1'
& "$kit/render-showcase.ps1" -Plan -Round 1 -RunTag $tag |
    Tee-Object -FilePath "scratch/$tag-plan.txt" |
    python -I -B perf/scripts/runner_preflight.py "$kit/render-showcase.ps1" --plan -
```

Record the plan checksum and the one game arm in the item's proof-round receipt before admission. The prerequisite supplies the build and packs. The live capture is the only game arm in this plan. The first game launch consumes the round even if a later shot fails.

```powershell
$proof = Get-Content -LiteralPath "$kit/showcase.json" -Raw | ConvertFrom-Json
python -I -B perf/scripts/queue_add.py --needs game --gpu 5080 --after director-preset-smoke `
    --question $proof.question --decides $proof.decision --item $proof.item --round 1 --expires $proof.expires `
    $tag -- powershell -NoProfile -File "$kit/render-showcase.ps1" -Round 1 -RunTag $tag
```

Queue dispatch requires the decision metadata options above. The current desktop queue CLI at the time of writing only takes the legacy job fields. Run the command after the coordinator's queue metadata wiring is available and the production preflight passes.

After capture, use the manifest for the finished run to compose the review files. The timestamp table assumes the full 1,200-frame edit. The 4K master uses lossless RGB FFV1 in Matroska. The 1080p review file is H.264 MP4. Both are video-only for the trailer edit.

```powershell
& "$kit/wipe-showcase.ps1" -Manifest "ports/run/$tag/showcase.json"
```

The run folder contains `showcase-4k-smoke.png`, the individual plates, and their frame/state CSVs. The compositor writes `showcase-edit/showcase-preview-1080p.mp4` and `showcase-edit/showcase-master-4k.mkv` next to the manifest. If a launch is refused before the game starts, use a fresh run tag so the previous lockrun log remains available and the receipt can retain both attempts.

**CHECK** Live 4K output, camera clearance and framing in each scene, cockpit rig persistence through all three capture passes, Wilshire road wetness, rain continuity at the Selma wipes and the visual comparison at every cut.
