# Vinyls

Vinyl layers on a car can be exported to a file, shared and imported. Source: `src/mc_engine/hooks/vinyl.cpp`. Both actions work only in the garage with a car loaded.

## Export

Set `export_vinyl` to `true` while in the garage. It fires once and resets to `false`. The file is written to `vinyls/` in the working directory, named `vinyl_<car>_<timestamp>.vgp`.

## Import

Set `import_vinyl` to the file name, found in `vinyls/`, with or without `.vgp`, and press Enter. The car's current vinyl layers are overwritten and re-composited, then the field is cleared. Layers beyond the active cap of a surface are dropped.

| Cvar | Type | Default | Menu |
|---|---|---|---|
| `export_vinyl` | bool | `false` | no |
| `import_vinyl` | string | `""` | no |
| `extra_vinyl_layers` | bool | `false` | EXTRAS > EXTRA VINYL LAYERS (restart) |
| `vinyl_auto_readback` | bool | `true` | no |

`extra_vinyl_layers` raises the front and rear bumper caps from 16 to 31 layers each (caps then 64/64/64/31/31). Top and side stay at 64 because the game's composite pipeline hard-caps one surface at 64 layers; more overflows and crashes. Existing garage and online cars stay compatible because bumpers keep their 5-bit save field.

`vinyl_auto_readback` (leave on) briefly enables the GPU resolve readback for about 1.5 seconds around each vinyl composite, so decals appear outside the Vinyl Editor, and keeps it off while racing.

## `.vgp` format

Header fields are little-endian 32-bit values. Layer bytes are copied verbatim from game memory (big-endian inside), so a round trip is lossless.

```text
u32 magic   u32 version
u32 name length, then the car name bytes
u32 surface count
per surface:
  u32 cap            capacity of that surface
  u32 used           number of used layers
  per used layer:    u32 local index, then 20 raw layer bytes
```

Import checks:

| Log message | Cause |
|---|---|
| `Import: '<name>' not found in <dir>` (followed by `available:` lines) | No such file in `vinyls/`. |
| `Import: <file> is not a RexGlue .vgp (bad magic)` | Not a vinyl package. |
| `Import: file format vN (importer is vM); reading anyway` | Version differs; the import continues. |
| `Import: <file> is truncated` / `truncated surface N` | Damaged file. |
| `Import: unexpected surface count (N)` | Header does not match. |
| `Export: invalid vinyl block` | No car loaded. |
| `Export: cannot open <file> for writing` | `vinyls/` is not writable. |

## Other vinyl cvars

`dump_vinyl_shapes` and `capture_vinyl_shapes` are development tools that write shape catalogs to `vinyls/`. Their rows are in [Settings Reference](Settings-Reference#mclagarage-6). They are not documented further here.
