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

## How a car gets its wheels

Wheels are not part of a car's model. A car's wheels are a separate resource found through its car config (`<car>.xcc`), which holds one byte (offset `0x844`) naming a rim:

- The byte is an index into the rim catalog: the `rims` block of `tune/vehicle/wheel_models.list`, then those of `wheel_models0001.list` to `wheel_models0007.list`, in that order.- A byte past the end of the catalog (new cars use `0xFF`) means "the car's own wheel". The game then builds a name from the car's and looks that wheel up: drop the first three characters (`vp_`) and prefix `whl_stk_`, so `vp_bmw_740i_98` asks for `whl_stk_bmw_740i_98`. A car name with `vpd_` drops four and prefixes `whld_stk_`; a name containing `DUB` gets `whl_am_tis_tis07`.
- That lookup only happens once the car is driven. The loader resolves it earlier: a car config shipped as a raw file (`files/...xcc`) that names its rim is written with the catalog index already filled in, so the garage shows the wheels at once. Log: `rim byte ... resolved to catalog index N`. Without it a bought car stands in the garage with no wheels until it has been on the street.

So a car mod has two ways to get its own wheels:

1. Ship a wheel named for the car (`rims/whl_stk_<name>.glb`, with a `.donor` sidecar as above). The car's own-wheel lookup finds it.
2. Ship a car config whose rim byte names a shipped wheel, or one you added to the catalog. A raw file `files/tune/vehicle/wheel_models000N.list` shipped by a mod is read by the loader in place of the archive's when it counts the catalog.

Wheel mods and car mods do not touch each other's files. A wheel mod changes what a named wheel looks like for every car that uses it; a car mod only picks which wheel its car asks for.

### TIRE OFFSET

`garage_tire_offset` (default on) adds a TIRE OFFSET row to the garage's wheels > dimensions screen. It edits the per-car `TireOffset0/1` bytes that the retail game parses and never uses, and moves the wheel sideways: `tire_offset_step` metres per notch, 17 notches from -8 to +8 (about 10 cm each way at the default 0.012). `tire_offset_targets` is a bitmask of the five per-wheel matrix blocks it moves (31 = all). It adjusts how far a wheel sits out of the body on one car; it does not change the wheel mesh or its fit on the axle, which is what `model_mods_rim_*` does.

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
