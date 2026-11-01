# Car Mods

A car mod replaces a vehicle the game ships, or adds a new one built on a shipped "donor" car. Source: `src/mc_engine/modloader/modloader.cpp` (the `vehicles/` scan, `parts.txt` reader, `BuildVehicleMods`, `BuildDonorCar`) and `src/mc_engine/hooks/mods.cpp`. Read [Mod Loader](Mod-Loader) first for the folder rules and reload behavior.

A car is not one asset. A player car ships as many resources (the code comment gives "around a hundred and seventy"), one per part, so a mod states which of its own parts fills which slot. That statement is `parts.txt`.

## Folder layout

```text
<mod folder>/<mod>/
  vehicles/
    <car>/                  name of the car; see "Replacing" and "Adding"
      <model>.glb           the first .glb, .gltf or .obj found in the folder
      parts.txt             required
      textures/             optional loose images, one per material name
```

- Each folder directly under `vehicles/` is one car. Plain files there are ignored.
- The mesh is the first regular file with extension `.obj`, `.gltf` or `.glb` found in the car folder. Keep one mesh per car folder. (With `verbatim = 1`, no mesh is needed.)
- A missing or empty `parts.txt` is logged as `parts.txt maps no slots` (or `cannot open parts.txt`) and the car is skipped.
- `textures/` works as for characters: one file per material name, accepted extensions in priority order `.dds`, `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`. It only fills materials that have no image.

## `parts.txt` grammar

One `key = value` per line (`ReadPartsFile`).

- A comment starts at a `#` at the start of a line or after a space or tab. A `#` inside a token is kept, because `InteriorTrim#3` names the fourth drawn `InteriorTrim`.
- Lines without `=` are ignored.
- The value is split on commas into groups; spaces around groups are trimmed; empty groups are dropped. A line with no groups is dropped.
- A key that is not a reserved key (below) is a part slot: `slot = group[, group...]`. The groups name glTF nodes, `.obj` objects (`o`), or materials of the model. Naming materials is how a car exported with its shell collapsed into one node can still be split (for example `interior0 = MAT_1, MAT_2`).

### Slot names

A slot is a name from the table below followed by an index `0` to `9`, for example `hood0`, `door_l0`, `widebody0`. The game asks for `<slot><index>_lod_<lod>` for lod 0 to 2.

```text
conv_top    conv_topup  hood        bumper_r    trunk_swap
intercooler bumper_f    fenders     skirts      spoiler
spoiler_b   headlight   taillight   taillight_b frontgrill
wheeliebar  blower      bike_tail   bike_tank   bike_cowl
door_l      door_r      conv_topdown exh_pipe   exh_side
armfl       armfr       armrl       armrr       shockfl
shockfr     shockrl     shockrr     guiderl     guiderr
axle        subbumf     subbumr     widebody    spoiler_wide
hood_wide   door_l_wide door_r_wide trunk_wide  interior
seats_f     seats_p     door_l_int  door_r_int  stereo
stereo_l    stereo_r    steer_whl   post_gauge  occluder_
```

A slot with no line keeps the shipped part. That allows a partial swap. The loader notes, for example, that one shipped car has five spoiler slots and another has no spoiler at all.

### Reserved keys

Listed with the example values from the loader's comments. These keys are consumed by the loader and are not slots.

| Key | Value | Meaning |
|---|---|---|
| `donor` | car name | Marks the folder as a new car built from this shipped car. See "Adding a car". |
| `driver` | car name | Whose driver animations the car uses. Default: the donor. |
| `verbatim` | `1` or `0` | `1`: clone the donor and rewrite nothing (a control). |
| `scale` | number | Scale of the model. Also marks the placement as given. |
| `offset` | `x y z` | Offset of the model, three numbers on one line. |
| `yaw`, `pitch`, `roll` | degrees | Rotation of the model. Example in the comments: `pitch = -90` for a Z-up file. |
| `silence` | slots | Slots left empty. |
| `keep` | effect names | Shader effects left exactly as the donor drew them. |
| `flat_uv` | effect names | Effects given flat UVs. |
| `weight.<effect>` | number | Scales the share of the body size budget given to the shader running that effect. Default 1.0. Example: `weight.CarPaintCustomizable = 2.5`. |
| `shader.<material>` | effect name, optionally `#N` | Which of the game's car shaders a material is drawn with. `shader.*` is a catch-all. `shader.<slot>.<material>` applies to one part only. `#N` picks the Nth shader running that effect (or `<effect>#N` where effect names are ambiguous). |
| `frame.<slot>` | `tx ty tz rx ry rz [scale] [rigid]` | Rest pose of the bone that draws a slot. `rigid` means a body panel with no centring. |
| `license.<slot>` | 12 numbers | The four corners (top-left, top-right, bottom-left, bottom-right, each x y z) of a licence plate quad. |
| `plate` | `x y z` | Where the donor's plate moves to. |
| `exhaust.<slot>` | `x y z` | Exhaust position. |
| `body_m<N>` | groups | Groups written into model N of the body drawable; needs a rigid `frame.body_m<N>`. |
| `lamp_box`, `lamp_from` | see source | Lamp sampling controls. Format is read in `ReadVehiclePlan`; further meaning is not documented outside the source. |
| `band.<name>`, `dial.<clip>` | numbers | Band weight, and a dial's pose and sweep (`tx ty tz rx ry rz, frame degrees, ...`). A malformed dial logs `parts.txt: dial.<clip> wants 'tx ty tz rx ry rz, frame degrees, ...'`. |
| `tune` | `rename` | Rewrite the tune's embedded car name. Requires a new name no longer than the donor's. |
| `tune.<field>` | value | Change a field of the car tune after renaming. Fields named in the code comments: `cFrontBumperMovement` and the other bumper fields (`NoMovement`, `AllowLoose`, `AllowBroken`), hood and boot movement (`NoMovement`, `NormalMovement`, `ReverseMovement`). An unknown field logs `tune.<field> is not a field this can change`. |
| `paint_shade` | hex colour word | One colour word for all paint, for example `F4000000`. |
| `atlas_cell` | 256, 512 or 1024 | Texel size kept per textured shader. Other values are ignored. |

Reserved keys are matched case-insensitively. Do not use them as slot names.

## Replacing a shipped car

Name the folder after the shipped vehicle, for example `vehicles/vp_nsn_skyline_99/`. Rules:

- `parts.txt` must have a `widebody0` line. Without it the car is skipped: `parts.txt has no 'widebody0' line, and without it there is nothing to size the car against`. The body groups set one scale for the whole car, measured along its length, so every part is resized by the same factor.
- Each slot line names the model's groups that fill that slot of the game's car. Parts are placed where the slot they replace sits, turned to face the way the game does, and scaled by the car's factor.
- Materials routed to a part slot leave the body; otherwise the body would be drawn twice.
- The car is turned about the vertical axis by `model_mods_car_yaw` (default `180.0`) before parts are placed.

Example skeleton, using the form shown in the loader's comments:

```text
widebody0 = bbme38_dno
hood0     = bbme38_hood_rest
door_l0   = bbme38_door_FL
```

## Adding a car

A `parts.txt` that names a `donor` builds a car the game does not ship, from a car it does. The roster is data: a `tune/vehicle/vehicleNNNN.lst` entry naming a car makes the showroom ask for resources under `resources/vehicle/<name>/`. A mod ships that list file as a raw file:

```text
<mod>/files/tune/vehicle/vehicle0003.lst     copied to tune/vehicle/vehicle0003.lst
```

The loader's notes on the roster: it is read from `tune/vehicle/vehicle.lst` plus `vehicle0001.lst` to `vehicle0008.lst`, and "only 0001 and 0002 are taken". The format of these lists is not documented in the source.

What the game asks for per car: three LOD drawables (`body_lod_0` to `body_lod_2`), two material packs (`<name>.xtl`, `<name>.xtp`), a tune (`<name>.xct`), a part manifest (`<name>.xspm`), and the slot drawables, each behind an existence check. A file the mod does not ship is simply not drawn. The donor supplies the non-geometry files (tune, packs, bound, manifest); the mod's mesh goes into the drawables. The whole model is placed with one transform measured from the donor's axles and the mod's wheels.

Further behavior:

- The new name must not be longer than the donor's when `tune = rename` is used; otherwise the loader logs `the name is longer than the donor's (N)`.
- Driver animations: the game files them under the car's name without `vp_`. The loader aliases the new car's name to the donor's (or `driver`) while the cars are built, through hooks on the three name formatters. The log line is `driver animations from <car>`.
- A car config (`.xcc`) shipped as a raw file whose rim byte names a rim is resolved to the rim catalog index when archives are written, so the garage shows the wheels. The log line names `rim byte ... resolved to catalog index N`.
- A raw file that is an exact copy of another archive entry (header `LARC`) is supported for entries that cannot be rebuilt, such as a per-car config. The layout of that header is in `src/mc_engine/modloader/modloader.cpp`; it is not described further here.

## Textures

- Embedded glTF images, or a `textures/` folder, are grouped by the shader their materials map to, packed into one atlas per shader, and written over that shader's colour map in the car's material packs (`.xtl`, `.xtp`). Paint, glass, matte and chrome effects read no texture, so those materials keep their UVs. Controlled by `model_mods_car_textures` (default `true`). Off ships the donor's packs exactly.
- A shader that names the same picture as another shares one cell only when `model_mods_share_images` is on (default `false`).

## Related options

All restart-only unless stated; full rows in [Settings Reference](Settings-Reference#mclamods-45).

| Option | Default | Effect |
|---|---|---|
| `model_mods_car_yaw` | `180.0` | Turn applied before parts are placed. |
| `model_mods_car_scale` | `1.0` | Size multiplier on top of the automatic fit. |
| `model_mods_car_budget` | `16777216` | Largest a replaced body may be grown to, bytes for LOD 0. |
| `model_mods_car_textures` | `true` | Give the body its own textures. |
| `model_mods_car_space_only` | `true` | Write a donor car's mesh only into the models the donor authors in car space. |
| `model_mods_car_lod1_copy` | `true` | Ship LOD 1 as a copy of LOD 0 for the body and parts. |
| `model_mods_cabin_light` | `true` | Give the cabin the donor's per-vertex interior lighting. |
| `model_mods_glass_band` | `true` | Give body glass the donor's pane numbers. |
| `model_mods_part_growth` | `8` | How much larger than the donor's a part may be, as a multiple. |
| `model_mods_grow` | `false` | Give a part buffers of its own instead of welding it down. |
| `model_mods_car_verbatim` | `false` | Diagnostic: copy each part's shipped file exactly. |
| `model_mods_diag` | `false` | Write the `.diag` folder. |

## Failure modes

Messages carry the prefix `[mods] <mod>/vehicles/<car>`. See also [Troubleshooting](Troubleshooting).

| Message (abridged) | Meaning |
|---|---|
| `no .glb, .gltf or .obj in the folder` | No mesh in the car folder. |
| `parts.txt maps no slots` / `cannot open parts.txt` | Missing or empty `parts.txt`. |
| `parts.txt has no 'widebody0' line` | Replacement without a body line. |
| `slot <slot> names no geometry in the model` / `<slot> names no geometry in the model` | The groups on that line match no node, object or material. |
| `the 'body' line names no geometry in the model` | Same for a donor car's body. |
| `donor <car> ships no <file> to build on` | The donor lacks a file the new car needs. |
| `cannot read the donor's material pack` / `cannot list the donor's folder` | Donor resources could not be read. |
| `'<name>' is not one of the game's car ...` | A `shader.` rule names an effect the game does not have. |
| `'<name>' does not end in a shader number` | A `#N` suffix is malformed. |
| `the donor has N shaders, so '<rule>' has ...` | The shader index is out of range. |
| `the donor draws nothing with <effect>` / `the body draws NONE of them` | A rule would write into a shader that has no geometry. Pick a shader the body renders. |
| `<slot> needs a rigid frame.<slot>` | A slot that needs a bone frame has none. |
| `tune.<field> = <value>: expected ...` | Bad tune value. |
| `body_lod_N took no geometry at all` | Nothing from the model fit the body. |
| `cannot measure the car body` | The body bounds could not be measured. |

## Not documented in the source

- Which shipped vehicle names exist beyond the examples in comments.
- The format of the roster `.lst` files.
- Effect names a `shader.` rule may use beyond those quoted in comments (`CarPaintCustomizable`, `CarGlass`, `BlackMatte`, `Chrome`, `CarLight`, `BumpSpecAlpha`, `InteriorTrim`).
