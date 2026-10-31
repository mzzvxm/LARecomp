# Texture Modding

Replace or dump game textures. This page covers the Plume renderer's texture mods (`src/gpu/texture_mods.*`, `src/gpu/textures.cpp`) and the dump helper (`src/mc_engine/texture_dump.*`). Model textures (images inside a mesh mod) are a different path; see [Mod Loader](Mod-Loader).

Texture mods need the NATIVE renderer (the Plume renderer). The pause menu row TEXTURE MODS is shown only for it (`NativeOnly` in `src/mc_engine/pause_menu.cpp`).

## Options

| Option | Where | Default | Effect | Source |
|---|---|---|---|---|
| `texture_mods` | menu: GRAPHICS > TEXTURE MODS > TEXTURE MODS; toml / CLI | `true` | Use replacements. Setting it false shows the game's own textures; a change re-indexes the folders at the start of the next frame. | `src/gpu/textures.cpp` |
| `texture_reload` | toml / CLI; same as the RELOAD TEXTURE MODS menu row | `false` | Set true to re-index the replacement folders at the start of the next frame. It resets itself afterwards. | `src/gpu/textures.cpp` |
| `mcla_gpu_texture_dump` | toml / CLI | `false` | Write every texture the renderer uploads, once per texture per session. | `src/gpu/textures.cpp` |
| `bind_gpu_texture_dump` | key F2 | F2 | Dump the textures of the next frame only. | `src/gpu/renderer.cpp` |
| `larecomp_texture_folder` | toml / CLI | `""` | Folder for the helper dump in `src/mc_engine/texture_dump.cpp`. Empty means `textures_dump/` in the mod folder. A relative path is taken from the executable folder. The old value `textures` is treated as empty. | `src/mc_engine/texture_dump.cpp` |

The menu also has two action rows: DUMP SCENE TEXTURES dumps every texture the renderer currently holds, not only the next frame's (`src/mc_engine/pause_menu.cpp`), and RELOAD TEXTURE MODS re-indexes the replacement folders.

## Folders

`<mods>` is the mod folder beside the executable: `modloader/`, `mods/` or `models/` (first one present).

| Folder | Role |
|---|---|
| `<mods>/textures_replace/` | Loose replacements. Win over every mod. |
| `<mods>/<Mod>/textures_replace/` | A mod's replacements. Any depth below the folder. |
| `<mods>/<Mod>/files/textures_replace/` | Same, for mods that keep everything under `files/`. The mod loader ships `files/` as raw game files, which does nothing for a texture; the texture system reads this folder directly. |
| `<mods>/textures_dump/<dictionary>/` | Where dumps from the Plume renderer land, named `<name>__<hash>.dds`. |

Only `.dds` files (extension compared case-insensitively) are read.

## Naming: the hash

A texture is replaced by its content hash, which must be the end of the file name. The hash is XXH3-64 of the bytes of the texture's base level as the game holds them. It is written as 16 hexadecimal digits. Accepted file names (`HashFromFileName`):

| Pattern | Example |
|---|---|
| `<anything>__<16 hex>.dds` | `capo__3f2a91c04d7e55b1.dds` |
| `<16 hex>.dds` | `3f2a91c04d7e55b1.dds` |
| `0x<16 hex>.dds` | `0x3f2a91c04d7e55b1.dds` |
| `<16 hex>_<anything>.dds` | `3f2a91c04d7e55b1_512x512_k_DXT1.dds` |

The last form is the name the helper dump in `src/mc_engine/texture_dump.cpp` writes (`<hash16>_<w>x<h>_<fmt>.dds`), so its files can be reused as they are. Hex digits are case-insensitive.

A file whose name carries no hash is ignored and logged:

```text
[gpu] texture mods: <path> carries no texture hash in its name (expected <name>__<16 hex>.dds); ignored
```

When two files carry the same hash, the loose folder wins, then mod folders in name order. The loser is logged as shadowed.

## Supported formats

From `TextureMods::LoadDds`:

| Accepted | Notes |
|---|---|
| BC1 to BC5: FourCC `DXT1`, `DXT3`, `DXT5`, `ATI1`, `ATI2`, `BC4U`, `BC5U`, or a DX10 header with the matching DXGI format | Uploaded compressed. DXGI formats accepted: BC1, BC2, BC3 (Unorm and sRGB), BC4 Unorm, BC5 Unorm. |
| Uncompressed 32-bit RGBA, BGRA, BGRX (including DX10 `R8G8B8A8`, `B8G8R8A8`, `B8G8R8X8`) | Uploaded as 8-bit RGBA. |

Rules:

- Every mip level present in the file is used. No mip levels are created. A file with only a base level has no mips.
- Only 2D textures. Cube maps, volumes and arrays are refused.
- A block-compressed base size must be a multiple of 4.
- A refused file is logged once with the reason, and the game's texture stays.

Refusal reasons seen in the code: `cannot be opened`, `not a DDS file`, `a cube map or volume (only 2D textures are replaced)`, `truncated DX10 header`, `a DX10 header that is not a single 2D texture`, `DXGI format N (BC1-BC5 or 8-bit RGBA/BGRA only)`, `FourCC ... (DXT1/3/5, ATI1/2, BC4U/BC5U or DX10 only)`, `32-bit RGB with masks other than RGBA/BGRA`, `block-compressed WxH, not a multiple of 4`, `no complete level in the file`.

## Workflow

1. Turn on a dump: set `mcla_gpu_texture_dump = true` in `larecomp.toml`, or press F2 for one frame, or use the DUMP SCENE TEXTURES row. Files appear under `<mods>/textures_dump/`.
2. Edit a copy of the texture and save it as DDS in one of the supported formats, keeping the hash in the file name.
3. Put it in `<mods>/<Mod>/textures_replace/`.
4. Reload with the RELOAD TEXTURE MODS row or `texture_reload = true`. No restart is needed.

Dumps are the game's own art taken from your own copy. They are not part of this repository and should not be redistributed; share only your edited replacements.

## Log lines

```text
[gpu] texture mods: N replacement(s) from <folder>
[gpu] texture mods: N replacement(s) from M folder(s) (K names without a hash, S shadowed)
[gpu] texture mods: switched off; no replacements
[gpu] texture mods: no <mod folder> folder; no replacements
```

## Limits

- Replacement is by exact content hash. A texture the game builds at runtime, or changes after load, will not match a hash taken earlier.
- The code states no maximum image size.
- Not documented in the source: behavior with the legacy native runtime or the Xenia renderer. The texture mod code lives in the Plume renderer only.

## Related

- [Mod Loader](Mod-Loader) for the mod folder rules.
- [Settings Reference](Settings-Reference): `texture_mods`, `texture_reload`, `larecomp_texture_folder`.
