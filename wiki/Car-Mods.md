# Car Mods

A car mod does one of three things:

- **replaces** a vehicle the game ships ("Replacing a shipped car");
- **adds** a new vehicle built on a shipped "donor" car ("Adding a car");
- **adds parts** to a vehicle the game ships, leaving the rest of it as it is ("Adding parts to a shipped car").

Source: `src/mc_engine/modloader/modloader.cpp` (the `vehicles/` scan, `parts.txt` reader, `BuildVehicleMods`, `BuildDonorCar`, `ShipGarageLines`), `src/mc_engine/modloader/rsc5.cpp` (bones, manifest, driver packs) and `src/mc_engine/hooks/mods.cpp`. Read [Mod Loader](Mod-Loader) first for the folder rules and reload behavior.

A car is not one asset. A player car ships as many resources (the code comment gives "around a hundred and seventy"), one per part, so a mod states which of its own parts fills which slot. That statement is `parts.txt`.

## Folder layout

```text
<mod folder>/<mod>/
  vehicles/
    <car>/                  name of the car; see the three sections below
      <model>.glb           the first .glb, .gltf or .obj found in the folder
      parts.txt             required
      textures/             optional loose images, one per material name
  files/                    optional raw files (roster, garage lists, string tables, car config)
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

A slot is a name from the table below followed by an index `0` to `9`, for example `hood0`, `door_l0`, `bumper_f5`. The game asks for `<slot><index>_lod_<lod>` for lod 0 to 2.

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

A slot with no line keeps the shipped part. That allows a partial swap.

The game keeps a car's parts in a table of 55 slots x 10 indices x 3 LODs, and the car's part manifest (`<car>.xspm`) says which of them exist. A car that ships five bumpers lists indices 0 to 4; indices 5 to 9 are free, and `template.` lines (below) fill them. Ten variants per slot is the game's limit.

### Reserved keys

Listed with the example values from the loader's comments. These keys are consumed by the loader and are not slots.

| Key | Value | Meaning |
|---|---|---|
| `donor` | car name | Marks the folder as a new car built from this shipped car. See "Adding a car". |
| `extend` | `1` or `0` | `1`: add parts to the shipped car the folder is named after. See "Adding parts to a shipped car". |
| `template.<slot><i>` | `<slot><j>` | Builds a variant the car does not have (for example `template.bumper_f5 = bumper_f4`) on the car's own file of variant `j`, and switches it on in the part manifest. See "More variants than the car has". |
| `garage.<section>` | `key brand price variant` | A line added to the car's garage list (`Parts.gtun`). See "Garage list". |
| `driver` | car name | Whose driver animations the car uses. Default: the donor. |
| `driver_offset` | `dx dy dz` | Moves the driver, in metres of car space. See "Driver and bones". |
| `bone.<name>` | `tx ty tz rx ry rz` | Rest pose of one bone of the body skeleton, in every LOD. See "Driver and bones". |
| `verbatim` | `1` or `0` | `1`: clone the donor and rewrite nothing (a control). |
| `scale` | number | Scale of the model. Also marks the placement as given. |
| `offset` | `x y z` | Offset of the model, three numbers on one line. |
| `yaw`, `pitch`, `roll` | degrees | Rotation of the model. Example in the comments: `pitch = -90` for a Z-up file. |
| `silence` | slots | Slots left out of the car. |
| `keep` | effect names | Shader effects left exactly as the donor drew them. |
| `flat_uv` | effect names | Effects given flat UVs. |
| `weight.<effect>` | number | Scales the share of the body size budget given to the shader running that effect. Default 1.0. Example: `weight.CarPaintCustomizable = 2.5`. |
| `shader.<material>` | effect name, optionally `#N` | Which of the game's car shaders a material is drawn with. `shader.*` is a catch-all. `shader.<slot>.<material>` applies to one part only. See "Shaders". |
| `frame.<slot>` | `tx ty tz rx ry rz [scale] [rigid]` | Rest pose of the bone that draws a slot. `rigid` means a body panel with no centring. See "Where a part is placed". |
| `license.<slot>` | 12 numbers | The four corners (top-left, top-right, bottom-left, bottom-right, each x y z) of a licence plate quad. |
| `plate` | `x y z` | Where the donor's plate moves to. |
| `exhaust.<slot>` | `x y z [single]` | Where the smoke bones of that (rear bumper) part go, car space: the left pipe's mouth, the right one mirrored. `single` puts both at the one point given, for a car with one tail pipe. Needs a rigid `frame.<slot>`. The bones are found by name: `extPrimary<i>`/`extSecondary<i>` or `exst_primary_<i>`/`exst_secondary_<i>`. |
| `locator.<slot>.<prefix>` | `x y z` | Moves every bone of that part named `<prefix><digits>` to this car-space point, `x` mirrored by the side the bone is on, rotation kept. Lamp parts carry their light sources this way (on the R34: `hl_low_`, `hl_hi_`, `fog_` in each headlight, `brake_` in each taillight, `amber_` in the stock front bumper, `rev_` in each rear bumper); the halo and the beam start at these bones. A slot in its bone's space needs a rigid `frame.<slot>`. See "Lamps". |
| `body_m<N>` | groups | Groups written into model N of the body drawable; needs a rigid `frame.body_m<N>`. |
| `lamp_box` | `<index> <material or *> xmin xmax ymin ymax zmin zmax` | Lamp index for the CarLight triangles of that material inside the box (`x` as absolute value); the first matching line wins. See "Lamps". |
| `lamp_from` | slots | Donor lamp slots whose lamps are sampled for lamp indices. |
| `band.<name>`, `dial.<clip>` | numbers | Band weight, and a dial's pose and sweep (`tx ty tz rx ry rz, frame degrees, ...`). A malformed dial logs `parts.txt: dial.<clip> wants 'tx ty tz rx ry rz, frame degrees, ...'`. |
| `tune` | `rename` | Rewrite the tune's embedded car name. Requires a new name no longer than the donor's. |
| `tune.<field>` | value | Change a field of the car tune after renaming. Fields named in the code comments: `cFrontBumperMovement` and the other bumper fields (`NoMovement`, `AllowLoose`, `AllowBroken`), hood and boot movement (`NoMovement`, `NormalMovement`, `ReverseMovement`). An unknown field logs `tune.<field> is not a field this can change`. |
| `paint_shade` | hex colour word | One colour word for all paint, for example `F4000000`. |
| `atlas_cell` | 256, 512 or 1024 | Texel size kept per textured shader. Other values are ignored. |

Reserved keys are matched case-insensitively. Do not use them as slot names.

## Replacing a shipped car

Name the folder after the shipped vehicle, for example `vehicles/vp_nsn_skyline_99/`, and give no `donor` or `extend` line. Rules:

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

- The new name must not be longer than the donor's; otherwise the loader logs `the name is longer than the donor's (N)`.
- A `body` line names the model's groups that make the shell; without one the whole model is the shell. A body that matches no geometry logs `the 'body' line names no geometry in the model` and the car is not built.
- Driver animations: the game files them under the car's name without `vp_`. The loader aliases the new car's name to the donor's (or `driver`) while the cars are built, through hooks on the three name formatters. The log line is `driver animations from <car>`. With `driver_offset`, the car ships packs of its own instead (see "Driver and bones").
- The garage files of a new car (`tune/garage/<car>.Garage`, `<car>_Parts.gtun`, `<car>_exterior.gtun`, ...) are raw files the mod ships under `files/`, usually copies of the donor's.
- A car config (`.xcc`) shipped as a raw file whose rim byte names a rim is resolved to the rim catalog index when archives are written, so the garage shows the wheels. The log line names `rim byte ... resolved to catalog index N`. How a car finds its wheels, and how to ship wheels of its own, is in [Wheel Mods](Wheel-Mods#how-a-car-gets-its-wheels).
- A raw file that is an exact copy of another archive entry (header `LARC`) is supported for entries that cannot be rebuilt, such as a per-car config. The layout of that header is in `src/mc_engine/modloader/modloader.cpp`; it is not described further here.

## Hiding the donor's car

A new car is built inside the donor's files, so the donor's own geometry is still there. The loader hides it in three layers, and one more switch covers the rest. Knowing which layer a leftover piece belongs to is most of the work.

| What the donor drew | What happens to it | What you do |
|---|---|---|
| The body shell (`body_lod_0` to `body_lod_2`) | The mod's mesh is written into the donor's body drawable, one shader (effect) at a time, over the donor's submeshes of that shader. For every donor shader that none of the mod's materials maps to, the loader silences all the donor's geometry drawn with it. Log: `<effect> silenced (N submesh(es) the mod has no material for)`. | Nothing: the donor's shell does not survive a build. Map the mod's materials with `shader.<material>` (see "Shaders") so they land on the effects you want. |
| Submeshes in bone-local models of the body (a bumper's paint, a lamp, a door window) | With `model_mods_car_space_only` on (the default), the donor's submeshes in models that are not in car space are silenced instead of being written into, because geometry in car space would land wherever that bone is. Log: `N bone-local submesh(es) silenced`. | Nothing. Leave `model_mods_car_space_only` on. |
| A part slot the `parts.txt` lists (`hood0 = my_hood`) | The part file is rewritten with the mod's geometry. Again, any effect the part draws that none of its materials maps to is silenced. Log: `<slot> <effect> silenced (N submesh(es))`. | Give the slot's materials a shader, or accept that the rest of the part is hidden. |
| A part slot the `parts.txt` does not list | The donor's shipped part stays and draws. This is the usual reason a donor's hood, bumper, spoiler, mirror or skirt shows up around the new car. | Map the slot to a group of the model, or hide it with `silence`. |

### `silence`

```text
silence = hood0, skirts0, spoiler0
```

Each name in the list is a slot plus its index, written exactly as it is in the manifest (`hood0`, not `hood`). The part's three LOD files are left out of the car's archive, and the game, which opens every part path and treats a missing one as "nothing to draw", draws nothing. Log: `<file> left out, the mod ships its own`.

- One name hides one variant. A car that ships five bumpers needs `bumper_f0, bumper_f1, bumper_f2, bumper_f3, bumper_f4` to hide all of them, or the donor's bumper appears when the player picks that variant in the garage.
- What the garage menu offers comes from the car's garage list, not from `silence` (see "Garage list"). A variant you silence but leave in the list is offered and draws nothing.
- `silence` is written for a car with a `donor`. Whether it also applies to an `extend` car has not been checked.

### `keep`

`keep = LicensePlate` leaves every submesh the donor draws with that effect exactly as the donor drew it, instead of silencing it. Use it for what a mod does not bring: the donor's plate is a four-vertex quad in the right place on any car of the same shape. `plate = x y z` moves it. Other effect names are in the list under "Shaders".

### Checking what is left

The log is the only reliable view. For each LOD 0 build it states `donor <car> draws with 0=<effect>, 1=<effect>, ...` and which of those the body renders, then one `silenced` line per effect it removed. A donor piece you still see is a slot the `parts.txt` left alone, or an effect on the `keep` line. If the whole car is missing instead, see "Failure modes" (`body_lod_N took no geometry at all`).

## Adding parts to a shipped car

`extend = 1` adds parts to a car the game ships, under its own name. Name the folder after the car, for example `vehicles/vp_nsn_skyline_99/`. Any shipped car works.

The car is its own donor:

- Every file of the car is copied as shipped: body, material packs, textures, tune, part manifest and every part the `parts.txt` does not name.
- Only the slots the `parts.txt` lists are built. A listed slot the car has (`bumper_f0`) is replaced; a variant it lacks (`bumper_f5`) needs a `template.` line.
- Nothing is renamed and no texture is written, so the car looks as it did. A new part draws with the car's own shaders and textures: paint, glass, black, chrome and the other untextured effects need nothing, and a textured effect shows the picture the car already has for it.
- Parts are authored in the car's own space (see "Where a part is placed"). Each one is written in the frame of the bone its slot hangs from, read off the car's skeleton, unless a `frame.` line says otherwise.
- `body_m<N>` lines are refused: the body stays as shipped.
- `bone.`, `dial.`, `plate` and `driver_offset` do nothing on an extended car: the first three change the body, which is copied as shipped, and moved driver packs ship under a name of the car's own. The loader logs `an extended car keeps its body and its name, so these lines do nothing: ...`.
- A `donor` line is ignored (logged).

Example: a sixth front bumper on the game's R34, with its own name in the garage.

```text
extend = 1
template.bumper_f5 = bumper_f4
bumper_f5 = my_bumper
shader.bumper_f5.PAINT = CarPaintCustomizable
shader.bumper_f5.BLACK = BlackMatte
garage.bumper_front = r34_my_bumper 0 1450 5
```

Step by step, for a new part on a shipped car:

1. Name the folder after the car (`vehicles/vp_nsn_skyline_99/`) and put `extend = 1` in `parts.txt`.
2. Pick the slot and a free index. Indices 0 to 9 exist; the ones the car's part manifest does not list need a `template.<slot><i> = <slot><j>` line first, where `<slot><j>` is a variant the car has and whose shaders suit the new part.
3. Write the slot line (`bumper_f5 = my_bumper`) naming the groups of your model.
4. Give the materials a shader of the ones that slot's file draws: `shader.bumper_f5.PAINT = CarPaintCustomizable`. A shader the mould lacks only draws when the vertex layouts match (see "Shaders").
5. Add the `garage.` line whose last field is the index, or the part never appears in the garage menu: `garage.bumper_front = my_key 0 1450 5`.
6. Author the part in the car's own space. If it sits wrongly, add `frame.<slot>` (see "Where a part is placed").
7. Start the game and read the `[mods]` lines for the car before changing anything.

Replacing one of the car's own parts needs steps 1, 3, 4 and 6 only.

Two mods that extend (or replace) the same car do not combine: each writes the car's files, and only one of them takes effect. Keep one mod per car.

## More variants than the car has

`template.<slot><i> = <slot><j>` builds variant `i`, which the car (or donor) does not have, on its file of variant `j`. That file is the mould: its page layout, its shaders and the bone it hangs from. The part is written as `<slot><i>_lod_<l>` and its manifest entry is switched on, or the game would never ask for it. The log line is `<car>.xspm lists N new part file(s)`.

- Pick a mould whose shaders the new part needs: a slot can only draw with the shaders its file has (see "Shaders"). On the R34, `bumper_f4` draws paint, BlackMatte, Underbody and BumpSpec; `spoiler0` draws paint, BumpSpec and BumpSpecAlpha.
- The new variant still has to be listed in the garage to be chosen: add a `garage.` line whose `variant` is `i`.
- Indices go up to 9. `template.` names that are not a slot plus one digit log `template.<name>: not a part name`.

Works the same for a new car (`donor`) and for `extend`.

## Garage list

The garage menus read `tune/garage/<car>_Parts.gtun`. Each menu is a section headed `<!-- name -->` (`bumper_front`, `bumper_rear`, `hood`, `spoiler`, `headlight`, `taillight`, `skirts`, `seats_f`, ...), then the entry count, then four lines per entry:

| Line | Meaning |
|---|---|
| name key | A string-table key; the menu shows that key's text. |
| brand | The manufacturer logo shown on the line (`0` none, `52` stock). |
| price | Price in the garage. |
| variant | The slot index the line selects. It is the variant the game loads, not the number in the name: `skirts_3` with variant `4` loads `skirts4`. |

`garage.<section> = key brand price variant` adds a line to that section (raising its count), or appends the section when the car has none. The text amended is the one a mod already ships under `files/tune/garage/` if there is one, else the car's own from the game's archive, else the donor's (for a new car that ships no list). The log line is `N garage line(s) added to tune/garage/<car>_Parts.gtun`.

### Names

The first field is a key, not the text. The game looks it up in its string tables: the shipped keys `bumper_f_1` to `bumper_f_4` read `1 OF 4` to `4 OF 4`, `spoiler_5` reads `NO SPOILER`. So:

- a line can reuse a shipped key, and shows that key's text; or
- the mod ships its own key with its own text in a string table of its own, `files/ui/mc4stringtable_000N.strtbl`. The game loads `mc4stringtable_0004` to `_0008` besides its own tables, so five such files exist for all mods together, and a mod that ships one owns that number. See [String Replacement](String-Replacement) for the file format.

The loader does not write string tables.

## Where a part is placed

A part file is authored relative to the bone that draws it (a bumper is centred on its bumper bone), except the few slots whose bone sits at the car's origin (cabin, seats and, on most cars, the lamps), which are in car space. The model arrives in car space, so each part is moved into its slot like this:

1. A `frame.<slot>` line: the part is taken into that bone pose (`rigid`: moved and turned only). Use it when you know the bone, or to override the automatic frame.
2. With `extend`, a slot with no `frame.` line gets one from the car's skeleton: the bone its file hangs from (the file's `+0x34`), its rest pose composed down the parent chain. Logged as `<slot> hangs from bone N (<name>) at (x y z), rotation (rx ry rz)`. A slot that hangs from the root gets the identity, so it stays in car space.
3. Otherwise a part whose shipped file straddles its bone on every axis is re-centred onto that file's box; a part whose shipped file is in car space stays where the model put it.

Car space: `x` lateral, `y` up, `z` back (the nose is at `-z`).

## Driver and bones

- `driver_offset = dx dy dz` moves the driver of a new car (`donor`, with a name of its own). The donor's four driver animation packs (player and opponent, male and female) are shipped again under the car's own name, with the root of every seated animation moved by the offset. Animations whose root sits on the car's centre line (facial expression sets, overlays) are left alone. The car then stops borrowing the donor's packs by name. The log line is `<who>/<sex> driver moved by (dx dy dz): N of M animation(s) seated, ...`; a failure logs `driver_offset: <pack>: <reason>` and ships none.
- `bone.<name> = tx ty tz rx ry rz` sets the rest pose of one body skeleton bone in every LOD (car space, Euler radians in the skeleton's convention). Whatever hangs from the bone follows it: `bone.str` moves the steering wheel the game turns, `bone.seats_pass` / `bone.seats` move every seat variant on that side. A bone that is not found, or a bone at the origin whose matrix copies are not where the loader reads them, logs `body_lod_N bone.<name>: the body has no bone of that name, or ...` and stays as shipped.
- A part hanging from a moved bone is written in the new pose with a matching `frame.<slot>` line.

## Shaders

- `shader.<material> = <effect>` maps a material to the first shader the body draws with that effect; `<effect>#N` picks the Nth drawn one, and a bare `#N` picks shader number N. `shader.<slot>.<material>` applies to one part only and beats the unqualified rule.
- A `shader.<slot>.<material>` rule naming an effect that part does not draw is **borrowed**: the material is written into the part's first spare submesh (one no other material of the part uses, in the file's order) and that submesh is repointed at the effect. The effect reads the vertices with the spare submesh's layout, so it only draws when the two layouts match: POSITION NORMAL COLOUR0 TEX0 TANGENT is the layout of paint, BumpSpec, CarbonFiber, Chrome, GrillOpaque and LicensePlate; BrushedMetal adds TEX1, EngineBay drops TANGENT, BlackMatte is POSITION only and never hosts. A mismatch draws nothing and logs nothing. Logged as `<slot> <effect> <- N tris, borrowing the <effect> submesh`. In the borrow, `#N` counts the effect's occurrences in the material pack.
- A shader's texture is one picture for the whole car. Two drawables writing different pictures into one shader log `<file> and an earlier drawable both draw with <effect>, which stores one picture`, and several shaders sharing one texture log `shader N (<effect>) shares <texture> with a shader already written`. Give each material its own shader, or give every writer the same picture.
- Effects the loader knows (`kCarEffects` in `rsc5.cpp`): `WheelRubber`, `WheelBrakeRotor`, `CarbonFiber`, `CarGlass`, `BrushedMetal`, `TexturedPlastic`, `BumpSpec`, `CarLight`, `BlackMatte`, `BumpedGlass`, `InteriorTrim`, `BrakeCaliper`, `BumpSpecSkinned`, `AnisotropicBilletMetal`, `Seam`, `InteriorTrimAlpha`, `InteriorTexturedEmissive`, `InteriorTexturedEmissiveAlpha`, `CarPaintCustomizable`, `RimMain`, `RimBadge`, `Grill`, `Underbody`, `RimNormalMapped`, `Chrome`, `BumpedAlphaChrome`, `LicensePlate`, `CarPaintNormalMapped`, `EngineBay`, `BumperSubframe`, `BumpSpecAlpha`, `GrillOpaque`, `BlackOpaqueGlass`, `BumpedBrushedMetal`, plus generator and effect shaders that are not car surfaces. A car only draws the ones its pack and parts use; the loader logs the list for a donor (`donor <car> draws with 0=..., 1=...`) and which of them its body renders.

## Lamps

Lit surfaces are `CarLight`. CarLight has no colour map: the images given to its materials become the lamp's relief (its normal map) and glow mask, and the lamp's role and colour come from its lamp index (`lamp_box`). Those images are packed once into one atlas for every lamp drawable of the car (`lamps share one ... atlas of N image(s) across every CarLight drawable`). The indices the bundled A80 port uses: `0` brake, `1` tail, `2` turn signal, `3` low beam, `4` high beam, `5` side marker, `6` reversing, `9` reflector. Lenses are `BumpedGlass`. Where the light itself starts (the halo and the beam on the road) is not the lamp's geometry but locator bones in the lamp parts; move them onto the new lamps with `locator.` lines, or the light stays where the donor's lamps were.

## Textures

- Embedded glTF images, or a `textures/` folder, are grouped by the shader their materials map to, packed into one atlas per shader, and written over that shader's colour map in the car's material packs (`.xtl`, `.xtp`). Paint, glass, matte and chrome effects read no texture, so those materials keep their UVs. Controlled by `model_mods_car_textures` (default `true`). Off ships the donor's packs exactly. An `extend` car never writes textures.
- A shader that names the same picture as another shares one cell only when `model_mods_share_images` is on (default `false`).
- A texture written in place that does not fit its slot logs `shader N: no room to write <texture> in place`.

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
| `template.<name>: not a part name` | A `template.` key is not a slot plus one digit. |
| `<car>.xspm: manifest slot table ...` / `no 30-byte presence array` | The part manifest could not be amended; the new variants are not switched on. |
| `extend adds to the car the folder is named after; the 'donor = <car>' line is ignored` | `extend` and `donor` together. |
| `<slot> is a model of the body, which an extended car keeps as shipped` | A `body_m<N>` line on an `extend` car. |
| `an extended car keeps its body and its name, so these lines do nothing: ...` | `bone.`, `dial.`, `plate` or `driver_offset` on an `extend` car. |
| `cannot read the car's skeleton (...)` | `extend` could not read the body; parts without a `frame.` line are centred on their slot instead. |
| `no tune/garage/<car>_Parts.gtun to add garage lines to` | Neither the mod, the car nor the donor has a garage list. |
| `parts.txt: garage.<section> wants 'key brand price variant'` | A `garage.` line without four fields. |
| `parts.txt: bone.<name> wants 'tx ty tz rx ry rz'` / `driver_offset wants 'dx dy dz'` | Malformed pose or offset. |
| `body_lod_N bone.<name>: the body has no bone of that name, or ...` | See "Driver and bones". |
| `<file> locator <prefix>: no bone of that name` / `locators: the slot is in its bone's space and needs a rigid frame` | A `locator.` line names a bone the part does not have, or the slot has no rigid frame. |
| `driver_offset: <pack>: <reason>` | The driver packs could not be moved; the car borrows the donor's. |
| `tune.<field> = <value>: expected ...` | Bad tune value. |
| `body_lod_N took no geometry at all` | Nothing from the model fit the body. |
| `cannot measure the car body` | The body bounds could not be measured. |

## Not documented in the source

- Which shipped vehicle names exist beyond the examples in comments.
- The format of the roster `.lst` files.
- The meaning of every lamp index beyond the ones the A80 port uses.
