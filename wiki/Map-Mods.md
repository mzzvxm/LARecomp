# Map Mods

Data files a mod can ship for ground the road network does not reach: warp points, light glows, dynamic lights and breakable props. These files sit directly in the mod folder, not under `files/`, so the mod loader does not pack them into the archive. They are read by the game process. Source: `src/mc_engine/map_warp.cpp` and `src/mc_engine/modloader/features/`.

All four are read once on first use. Restart the game after changing them. A mod folder that does not contain the file contributes nothing.

```text
<mod folder>/<mod>/
  warps.txt
  glows.bin
  lights.bin
  breakables.bin
```

Mods are visited in sorted folder-name order (`FindModDataFiles`).

## `warps.txt`

Named landing points. One per line:

```text
name x y z [heading]
```

- `heading` is degrees, `0` = facing +Z. It is optional.
- `#` starts a comment. Numbers may be separated by spaces, tabs or commas.
- A line that does not parse is logged: `[warp] <file>: '<line>' needs name x y z [heading]`.

Use:

| Control | Effect |
|---|---|
| F9 (`bind_map_warp`) | Take the car to the first warp point, or to `warp_spot` when it is set, and back to where it was. |
| Console command `warp <point>` | Warp to a named point. |
| `warp back` | Return to where the car was. |
| `warp x y z [heading]` | Warp to any point. |
| `warp` | List the points. |
| `warp_spot` | String `"x y z heading"`; empty means the first mod point. |

The car is dropped onto whatever collision is below the point. The warp runs on the game thread; the key and the command only file a request.

## `glows.bin`

Light glows placed in the world, drawn by the game's own glow renderer. Needed because lights baked into a mod's city sectors have no prop behind them. Glows near the camera are submitted every frame, depth-tested and distance-faded, and lit only in the lamp hours. A glow may follow a breakable prop and go out while it lies knocked over.

Format, little-endian (from the file's header comment):

| Part | Layout |
|---|---|
| Header | `'MGLW'`, u32 version (`1`), u32 count, u32 record size (`32`) |
| Record | f32 x y z; f32 size multiplier, f32 intensity multiplier; u32 colour `0x00RRGGBB`; u16 flags (`1` = flashing); u16 phase; u32 id of the breakable it follows (`0xFFFFFFFF` = none) |

Options:

| Option | Default | Effect |
|---|---|---|
| `mod_glows` | `true` | Enable mod glows. |
| `mod_glows_max` | `64` | Glows submitted per frame, nearest first. The game draws 96 a frame in all, car lights included. |
| `mod_glow_size` | `1.8` | Size multiplier on top of each glow's own. |
| `mod_glow_intensity` | `1.0` | Intensity multiplier. |

## `lights.bin`

Dynamic lights for a mod's part of the map. At night the game lights the city and cars per pixel from a light grid over a hard-coded rectangle around the city, so ground outside it gets no lamp light. The loader builds a second grid over the mod's rectangle at city load and switches to it while the camera is inside. The city's grid is not touched.

Format, little-endian:

| Part | Layout |
|---|---|
| Header (32 bytes) | `'MLGT'`, u32 version (`1`), u32 count, u32 record size (`56`), f32 grid rectangle min x, min z, max x, max z (all `0` means the lights' extent plus 100 m) |
| Record (56 bytes) | f32 position x y z; f32 colour r g b (0 to 1); f32 direction x y z; f32 intensity; f32 cone angle (degrees, `360` = omni); f32 cone spread (degrees); u32 id; u8 type (`0` omni, `1` spot); u8 flags (`1` = on); u16 reserved |

Intensity is in the game's units: `20` is a Los Angeles street lamp, reaching about 44.7 m. At most `128 * 256 - 1` lights fit the light textures.

| Option | Default | Effect |
|---|---|---|
| `mod_lights` | `true` | Enable mod lights. |
| `mod_light_intensity` | `1.0` | Intensity multiplier applied when the grid is built, at city load. |

## `breakables.bin`

Props baked into a mod's city sectors that break off when the car hits them fast enough. Geometry a mod adds to the sectors is static and its collision sits in the city bound, so without this a pole stops the car dead. Behavior:

- A slow bump changes nothing; the collision box stops the car.
- When the player's car reaches a prop faster than its break speed, the box's vertices are lifted out of the collision structure, the upright models are culled, and the knocked-over copy on the side the car was travelling to is shown. The car keeps a fraction of its speed.
- Glows linked to a broken prop are skipped.
- A broken prop stands up again once the car is 350 m away.
- A sector that streams out and back in comes back with the upright models; the patch is applied again.
- Nothing breaks below 4 m/s.

Format, little-endian (header and record layout as stated in the file's header comment):

```text
'MBRK' u32 version(1) u32 count f32 headroom
per prop: u32 id, f32 base[3], f32 fall[3], f32 radius, f32 height,
          f32 break speed (m/s), f32 speed kept (0..1), char sector[16],
          u8 n_up, u8 n_fallen, u8 n_cells, u8 0, u32 up_hash[n_up],
          2 x n_fallen x { u32 hash, f32 centre[3], f32 radius, f32 min[3], f32 max[3] }
          (fallen towards +fall, then -fall),
          n_cells x { u16 cell, u16 n, u16 vertex[n] } padded to 4 bytes
```

| Option | Default | Effect |
|---|---|---|
| `mod_breakables` | `true` | Enable breakable mod props. |
| `mod_breakable_speed_scale` | `1.0` | Multiplier on the speed a prop needs to break. |

## Garage start

`start_garage` (menu: EXTRAS > START GARAGE) chooses the district garage the car starts at: `hollywood`, `beaches`, `sc` (SOUTH CENTRAL) or `last`. `start_garage_last` is kept up to date by the game. These are listed in [Settings Reference](Settings-Reference#mclaworld-17) and are not mod-specific.

## Not documented in the source

- Tools or editors for authoring these files. The formats above are the only description in the repository.
- Meaning of `id` fields beyond cross-linking glows to breakable props.
