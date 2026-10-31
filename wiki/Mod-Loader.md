# Mod Loader

How mods are found, what they may contain, and when they take effect. Everything here comes from `src/mc_engine/modloader/` and `src/mc_engine/hooks/mods.cpp`. Cvar details are in [Settings Reference](Settings-Reference#mclamods-45).

## Overview

At startup the loader scans the mod folder, converts meshes to the game's drawable format, copies loose files as they are, and packs the result into one archive, `xarchive_mods.rpf`, written into the game data folder. That archive is appended to the list of archives the game mounts, last. The game searches a mount's archives from the last one back and falls through when a file is missing, so a mod file wins over the shipped file of the same path and everything else still comes from the shipped archives. The shipped archives are never modified (`modloader.h`).

The loader reads the shipped `xarchive_cache.rpf` as the template for meshes. A mod that only ships loose files does not need it; if it is missing, mesh mods are disabled and the log says so.

Building runs once per launch, before guest code runs (`Init`). Changes to the mod folder take effect on the next launch. The pack is rewritten only when its contents changed.

## Mod folder

The mod folder sits beside the executable. Three names are accepted and the first one that exists wins: `modloader/`, `mods/`, `models/`. If none exists the loader uses `modloader/`. Mods placed in one of the other two names are ignored and a warning is logged:

```text
[mods] mods/ is ignored: mods are read from modloader/ only; move its folders there
```

Each direct subfolder is one mod. The folder name is the mod name used in logs. Folders that are not mods are skipped:

| Folder name | Why it is skipped |
|---|---|
| starts with `.` | Caches and diagnostics (`.cache`, `.diag`, `.custommusic`, `.disabled_<name>`, `.old_<name>_<stamp>`). |
| `textures_dump` | Texture dumps. See [Texture Modding](Texture-Modding). |
| `textures_replace` | Loose texture replacements. |
| `music` | Reserved name. |

`model_mods` (default `true`, restart) turns the mesh and file scan off. It does not affect texture mods or custom music, which have their own switches.

## What a mod folder can contain

| Path inside `<mod>/` | Purpose | Page |
|---|---|---|
| `<name>.obj`, `<name>.glb`, `<name>.gltf` | Character replacement. The file name (without extension) is the asset it replaces. | below |
| `<name>.donor` | Optional sidecar naming the shipped asset a new asset is built from. | [Wheel Mods](Wheel-Mods) |
| `rims/<wheel>.glb` | Wheel replacement or new wheel. | [Wheel Mods](Wheel-Mods) |
| `vehicles/<car>/` | Car replacement or new car, with a `parts.txt`. | [Car Mods](Car-Mods) |
| `textures/` | Loose images, one per material name, next to a model. | below |
| `files/<path>` | Files copied into the archive under the same relative path. | below |
| `textures_replace/` or `files/textures_replace/` | Texture replacements for the Plume renderer. | [Texture Modding](Texture-Modding) |
| `warps.txt`, `glows.bin`, `lights.bin`, `breakables.bin` | Map mod data read by the game process. | [Map Mods](Map-Mods) |
| `music_titles.txt` | Titles for songs added through game metadata. | [Custom Music](Custom-Music) |
| `mod.toml` (+ image) | Description shown by the mod browser. | [Mod Browser](Mod-Browser) |

Mesh files are recognised by extension, case-insensitively: `.obj`, `.gltf`, `.glb`.

### Characters

`<mod>/<asset>.glb` replaces the character resource `resources/character/<asset>/<asset>.xrsc`. Shipped asset names follow `drv_<two letters>_<number>_set`, for example `drv_mp_01_set` (`modloader.h`).

- The game uses several resources built from one mesh: the base set, the `_h` variant, and, for names ending in `_set`, low-detail `_lod02_set` and `_lod02_set_h` variants. The loader writes every variant that exists in the shipped archive. Only the base asset is required; a missing variant is skipped quietly.
- A mesh named `all` stands in for every character the loader can find. A mesh named after one character still wins for that character. The log reports `standing in for N character(s)`.
- A `.obj` carries no skin weights and no images. It rides rigidly on one bone (`model_mods_bone`, `-1` picks the bone automatically).
- A glTF with `JOINTS_0` and `WEIGHTS_0` is skinned to the driver's skeleton by matching bind-pose positions. If the glTF has a skeleton but no weights, the log notes `no per-vertex weights, riding one bone rigidly`.
- Embedded glTF images are packed into one atlas and written over the character's diffuse map in place (`model_mods_textures`, default `true`).
- Size and placement cvars (`model_mods_scale`, `model_mods_offset_x`/`_y`/`_z`, `model_mods_yaw`, `model_mods_pitch`, `model_mods_roll`, `model_mods_proportions`, `model_mods_submeshes`, `model_mods_decimate`) are described in [Settings Reference](Settings-Reference#mclamods-45). All are restart-only.
- A mesh larger than the slot it must fit is refused with the reason in the log unless `model_mods_decimate` is on, in which case it is welded down. Default is off.

### Loose textures beside a model

A `textures/` folder next to a model supplies images for materials that have none. One file per material name, matched case-insensitively on the file stem. Accepted extensions, in priority order when two files share a stem: `.dds`, `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`. A material that already carries an image keeps it.

### Raw files

Everything under `<mod>/files/` is copied into the archive byte for byte, keeping its relative path as the archive path:

```text
modloader/<mod>/files/tune/vehicle/vehicle0003.lst  ->  tune/vehicle/vehicle0003.lst
```

Rules from `ScanRawFiles` and `Init`:

- Empty files are skipped (`empty, skipped`).
- Names starting with `.` are skipped.
- Archive paths are compared case-insensitively. A second mod shipping the same path is dropped with `already shipped by another mod, skipped`; the other mods are unaffected.
- A path the game has never seen is added as a new file.
- A file starting with the resource magic is treated as a resource. The `files/` path accepts two resource shapes, described in the code comments of `Init`; an uncompressed resource with the wrong flag or segment size is refused with a message naming the expected sizes. Details for resource authors are not documented outside the source.
- The `textures_replace/` folder under `files/` is not shipped to the archive as game files; the texture system reads it directly.

## Load order

- Mods are scanned in directory order of the operating system. Within a mod: characters, then `rims/`, then `files/`, then `vehicles/`.
- For duplicate paths among raw files the first one wins and later ones are logged and dropped.
- For texture replacements the order is defined: the loose folder first, then mod folders in name order. See [Texture Modding](Texture-Modding).
- `xarchive_mods.rpf` is mounted after the shipped archives. Custom music archives (`xarchive_music_NN.rpf`) are mounted before it, per the loader's comments. Each music archive holds up to 3 GiB of payload.

The number of archives the game can mount is limited. The loader's comments record a crash when nine archives are mounted, so music is capped at four archives. If more are needed the log reads `more than N music archives are required; custom music will not be mounted`.

## Reload behavior

| Item | Reload |
|---|---|
| Meshes, cars, wheels, raw files, music | Restart. Rebuilt at launch; unchanged archives are not rewritten. |
| Texture replacements | Live. `texture_reload` or the TEXTURE MODS menu row. |
| `menu_cameras.txt` | Re-read when its modification time changes. See [Menu Cameras](Menu-Cameras). |
| Mod browser changes | The browser restarts the game itself. |
| `glows.bin`, `lights.bin`, `breakables.bin`, `warps.txt` | Read once on first use; restart to change. |

## Caches and diagnostics

Created inside the mod folder, skipped by the mod scan because their names start with a dot:

| Folder | Contents |
|---|---|
| `.cache/` | Decompressed templates of shipped resources (`<variant>.tpl`), and per-car data. |
| `.diag/` | Per-asset diagnostic files: `<mod>__<variant>.txt` plus `_before.obj`, `_after.obj`, `_template.obj` when the rewrite produced them. |
| `.custommusic/` | Built music banks and metadata. See [Custom Music](Custom-Music). |

Deleting a cache folder forces a rebuild.

## Minimal worked example

A character replacement, a wheel, a loose file and a texture folder in one mod. The names are examples; the rules are those above.

```text
<exe folder>/
  larecomp.exe
  modloader/
    my_mod/
      mod.toml                       optional, shown in the mod browser
      drv_mp_01_set.glb              replaces that character (and its _h / lod02 variants)
      textures/
        mat_7.png                    image for the material named MAT_7, if the glb has none
      rims/
        whl_am_volk_te37.glb         replaces that wheel
      files/
        tune/vehicle/vehicle0003.lst copied to the archive path tune/vehicle/vehicle0003.lst
```

`mod.toml`, per `src/mc_engine/modloader/modsbrowser/mod_download.h`:

```toml
title = "My Mod"
description = "What it does."
author = "Someone"
version = "1.0.0"
image = "preview.png"
```

Expected log lines on a good build (formats from `modloader.cpp`):

```text
[mods] my_mod -> drv_mp_01_set (<n> tris, <n> variant(s))
[mods] my_mod: tune/vehicle/vehicle0003.lst (<n> bytes, verbatim)
[mods] xarchive_mods.rpf with <n> replacement(s) and 0 music archive(s), mounted last
```

## Common failures

Messages are logged with the `[mods]` prefix. See also [Troubleshooting](Troubleshooting).

| Log text (abridged) | Cause |
|---|---|
| `<dir>/ is ignored: mods are read from <dir>/ only` | Two mod folders exist; only the first of `modloader`, `mods`, `models` is read. |
| `<archive> not found, model replacement disabled` | `xarchive_cache.rpf` missing from the game data folder. |
| `cannot open <archive>` | The shipped archive could not be opened. |
| `<mod>/<asset>: <error>` | The mesh file could not be read or parsed. |
| `<mod>/<asset>: not found in xarchive_cache.rpf` | The asset name does not exist in the shipped archive. Check the file name. |
| `<mod>/<asset>: texture not replaced: <reason>` | The atlas could not be written over the diffuse map. |
| `<mod>/<asset>: <reason> -- keeping the original textures` | The atlas could not be built. |
| `<mod>/files/<path>: empty, skipped` | Zero-byte file. |
| `<mod>/files/<path>: already shipped by another mod, skipped` | Duplicate archive path. |
| `<mod>/files/<path>: cannot read` | Unreadable file, or size above 0x3FFFFFFF bytes. |
| `<mod>/files/<path>: flag ... wants N bytes of segments, the file carries M` | Resource file does not match its own header. |
| `nothing built, model replacement disabled` | No mod produced an archive entry. The old mod archive is removed so a stale mod cannot stay active. |
| `cannot write <path>` | The archive could not be written (permissions or disk). |
| `archive list is full, N custom archive(s) will not be mounted` | Too many archives for the mount list. |

If no mod and no music is present, any stale `xarchive_mods.rpf` or music archive in the game data folder is deleted.

## Related

- [Car Mods](Car-Mods), [Wheel Mods](Wheel-Mods), [Map Mods](Map-Mods)
- [Texture Modding](Texture-Modding), [Custom Music](Custom-Music)
- [Mod Browser](Mod-Browser)
