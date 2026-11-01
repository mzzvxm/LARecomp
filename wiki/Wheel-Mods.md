# Wheel Mods

Replace a shipped wheel, or add a wheel the game does not ship. Source: `src/mc_engine/modloader/modloader.cpp` (`ScanFolder`, `BuildRimMods`), `src/mc_engine/modloader/rim_names.h`. Read [Mod Loader](Mod-Loader) first.

## Layout

```text
<mod folder>/<mod>/
  rims/
    <wheel name>.glb            replaces the shipped wheel with that name
    <wheel name>.donor          optional, one line: the shipped wheel to build from
    all.glb                     stands in for every wheel
  textures/                     optional loose images, by material name
```

- Wheels live in `rims/`, not beside characters, because a name alone cannot say whether a mesh is a character or a wheel. Mesh extensions: `.obj`, `.gltf`, `.glb`.
- The file name without extension is the wheel name, for example `whl_am_volk_te37` or `whl_am_bbs_lm`.
- A wheel is two resources. The mesh goes into `resources/rims/<name>/body_lod_0.xrsc`; any image the mod brought goes into the texture pack beside it (`<name>.xtp`). Replacing only the first leaves the shipped paint.
- `all` stands in for every wheel in the built-in catalog (`kRimAssets`, about 175 names read off the retail archive). A mesh named after one wheel still wins for that wheel. Each catalog entry is checked against the archive before it is built, so an entry missing in another build costs nothing. The log reads `standing in for N wheel(s)`.

## Adding a wheel the game does not ship

A new car's stock wheel is looked up by a name derived from the car's (the game strips `vp_` and asks for `whl_stk_<rest>`), so a new car needs a wheel with that name, and no template exists under it. A one-line sidecar names the shipped wheel to use as the template:

```text
rims/whl_stk_bmw_740i_98.glb
rims/whl_stk_bmw_740i_98.donor      contains: whl_am_bbs_ch
```

The sidecar has the same base name as the mesh and extension `.donor`. Leading and trailing whitespace is trimmed; only the first line is read. The wheel lands under the new name, built from the donor's resources. A donor wheel's paint is written under the new name too.

## Orientation, size and texture

Applied automatically, with restart-only cvars to adjust. Full rows in [Settings Reference](Settings-Reference#mclamods-45).

| Option | Default | Effect |
|---|---|---|
| `model_mods_rim_autoalign` | `true` | Turn the thinnest axis of the model onto the axle, then push the face flush with the shipped wheel's. Off places the wheel entirely by hand. |
| `model_mods_rim_yaw`, `model_mods_rim_pitch`, `model_mods_rim_roll` | `0.0` | Manual rotation in degrees, applied on top of auto-align. |
| `model_mods_rim_offset_x` | `0.0` | Nudge along the axle; one unit is the wheel's own diameter. |
| `model_mods_rim_scale` | `1.0` | Size multiplier on top of the automatic fit. |
| `model_mods_rim_texture_mode` | `-1` | Where the mod's image goes: `-1` auto, `0` the hub cap, `1` the whole rim. Auto decides on the image's alpha: an image with real transparency is a badge and goes on the hub cap quad; an opaque one is a rim skin. |
| `model_mods_rim_keep_badge` | `true` | Keep the shipped badge quad. The first thing to try when a textured wheel mod misbehaves. |
| `model_mods_rim_shade_profile` | `true` | Rebuild baked shading from the template instead of one flat average. |
| `model_mods_rim_shade_occlusion` | `true` | Bake the shade lane from occlusion measured on the replacement. |
| `model_mods_rim_band_profile` | `false` | Carry the template's material bands onto the replacement. |
| `model_mods_rim_inherit_shade` | `false` | Diagnostic: copy the shipped shading vertex by vertex. |
| `model_mods_grow` | `false` | Give the wheel buffers of its own instead of welding it down. |

## Failure modes

| Log text | Meaning |
|---|---|
| `<mod>/rims/<wheel>: <error>` | Mesh unreadable, or the template wheel is not in the archive. |
| `<mod>/rims/<wheel>: no texture pack: <reason>` | The texture pack beside the wheel could not be read. |
| `<mod>/rims/<wheel>: paint not replaced: <reason>` / `paint not packed: <reason>` | The mod's image could not be written. |
| `<mod>/rims/<wheel>: <reason> -- keeping the original paint` | The image was ignored. |

## Not documented in the source

- The complete list of valid wheel names beyond the examples above; it is in `rim_names.h`.
- Size limits of a wheel mesh. A mesh too large for its slot is welded down only when `model_mods_decimate` is on; otherwise it is refused with the reason in the log.
