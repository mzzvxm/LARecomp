# Troubleshooting

Built from log and error strings in the source and from the README's known issues. If a symptom is not here, the repository does not describe it. Always check `logs/effective_config.txt` first when a setting seems to do nothing (see [Getting Started](Getting-Started#where-things-are-written)).

## Known issues (README)

| Issue | Detail |
|---|---|
| Dithered alpha on shadows and foliage | A dither pattern on shadow edges and vegetation. The README says the cause is not pinned down: either the GPU plugin's handling of the dither pattern, or the game's shaders dithering at a pattern scale that assumes 1280x720 output. It is the only rendering issue the README lists as left. |
| Intro movies play fast | Pacing follows the frame rate unlock; a frame cap does not help. Press A to skip, or turn on `skip_intro`. `intro_original_speed` (menu: RECOMP SETTINGS > ORIGINAL INTRO SPEED, default `true`) presents the start-up legal screens and logos at 30 per second. |
| Frame rate collapses below 10 FPS after a long session | The README traces it to an unsigned underflow in the SDK's vblank timer. Workaround: start with `--clock_no_scaling=true`. Whether the flag costs frame rate is stated as not settled. |

## Startup and game data

| Symptom or message | Cause and fix |
|---|---|
| Install wizard appears although you installed | No valid game data folder was found. A folder counts only with `default.xex` and at least one of `xarchive_cache.rpf`, `xarchive_audio.rpf`, `xarchive_audlo.rpf`. See [Getting Started](Getting-Started#game-data-folder). |
| `game_data_root is set to '...' but that directory has no default.xex plus xarchive_*.rpf; ignoring it and auto-detecting instead` | The folder passed with `--game_data_root` is not valid. Logged at startup under `Game data:`. |
| `MCLA_GAME_DATA is set to '...' but that directory has no default.xex plus xarchive_*.rpf - ignoring it and auto-detecting instead.` | Same for the environment variable. Printed to stderr, because logging is not up yet. |
| `no game data found from the executable directory (...); falling back to '...'` | Auto-detect found nothing; the wizard runs against that fallback. |
| `Game files are missing and there is no UI to install them from` | The install wizard cannot be shown. |
| `Embedded achievements.toml resource is empty` / `not found` | The achievements metadata resource is missing from the build. |
| Message box: `LARecomp cannot save to ... Your saves are kept in ... instead` | The default user folder is not writable. The code mentions Controlled Folder Access as a cause. Saves, cache and the rest moved to a `larecomp` folder under the local application data folder and existing saves were copied. |
| `cannot write to '...' nor to '...'; saves and the shader cache will fail` | Neither location is writable. Set `user_data_root` to a writable folder. |

## ISO install wizard

Error texts shown by the wizard (`src/isoinstaller/larecomp_iso_installer.cpp`):

| Text | Meaning |
|---|---|
| `Unable to open the selected ISO.` | File cannot be opened. |
| `The selected file is not a recognized Xbox 360 game ISO.` | Not an Xbox 360 disc image. |
| `The ISO root directory could not be read.` / `is invalid.` | Damaged image. |
| `The ISO does not contain default.xex.` | Wrong image. |
| `The selected ISO is not MCLA: CE. Found Title ID: <id>` | Wrong game. |
| `Title ID could not be found in the ISO's default.xex.` / `default.xex is not a valid Xbox 360 executable (Missing XEX2 magic).` / `Failed to read default.xex header.` | The executable in the image could not be validated. |
| `Unable to create the game directory: ...` / `Unable to create an install subdirectory: ...` / `Unable to create <path>.` / `Failed to write <path>.` | Target is not writable or has no space. |
| `Failed to read <path> from the ISO.` | Read error in the image. |
| `The ISO contains an unsafe file path.` | The image holds a path outside the install folder; the install refuses it. |
| `The ISO directory tree is unexpectedly large.` / `Failed to read an ISO directory entry.` / `...name.` / `The ISO contains an invalid directory entry name.` | Image structure problems. |
| `Installation completed, but game files are missing from the install directory (see the log for which).` | Check the log. |

## Settings and saving

| Symptom or message | Cause and fix |
|---|---|
| `Config: could not write <path>; the file is as it was` | The settings file could not be replaced. It is written to a temporary file and moved over, so the old file is intact. |
| `Config: <name> not saved, its value cannot be written as TOML` | The value is not valid TOML (for example a string that is not UTF-8). The setting is not saved. |
| `Config: no path to save to` | No config path known. |
| A change in the menu did nothing | Restart-only rows (renderer, graphics API, native MSAA and anisotropy, Plume render resolution, FSR1 preset) are held until the next launch. The label carries a `(RESTART)` suffix. Check the toml was written: `[pause-menu] N restart-only setting(s) written to ...`. |
| `[pause-menu] could not write the restart-only settings to <path>` | The toml could not be written. |
| `[pause-menu] renderer '<x>' is not one this build offers; nothing changed` | The build has no Plume renderer, or the value is unknown. |
| A command-line value is not kept | By design: values from the command line or environment are never written back (see [Getting Started](Getting-Started#when-the-game-rewrites-larecomptoml)). |
| Rows of the other renderer are missing | The menu lists only the rows of the renderer running at launch. |

## Performance

- Stutter in a non-Release build with logging on: set `REX_LOG_LEVEL=warn`. The README says `trace` logs about 7,500 lines (1.4 MB) per second during gameplay.
- Frame pacing: use `fps_limit` (30, 60, 120, 144, uncapped). `real_frame_delta` is not a cap; it feeds the simulation the measured frame time. Turning it off returns to the 30 Hz timestep; the cap, hitch clamp and city LOD keep working.
- `MCLA_TIMING_LOG=1` writes a frame timing log. From `documentation/RUNNING.md`: `vblank` in the millions in the interrupt split is the SDK vblank underflow (use `--clock_no_scaling=true`); `cpu` climbing with `poison` above 0 is a corrupt command buffer by the game's own check; everything flat while fps falls points at guest CPU.
- HUD or minimap glitches: the README suggests `MCLA_ALLOW_INVALID_FETCH=false`.
- Crashes: the crash handler writes the resolved call stack into the main log, not a separate file.

## Mods

See the full table in [Mod Loader](Mod-Loader#common-failures). Quick checks:

| Symptom | Check |
|---|---|
| Mod not loading | Is it in the folder the loader reads? Only the first of `modloader/`, `mods/`, `models/` is read; the log warns about the others (`<dir>/ is ignored`). |
| Mod loads nothing, old mod still active | `nothing built, model replacement disabled` removes the old archive; otherwise fix the first `[mods] ... :` error line. |
| Character stays the original driver | The asset name must match a shipped one. `<mod>/<asset>: not found in xarchive_cache.rpf` means it does not. The game can switch to a variant; a mesh named `all` replaces every character. |
| Mesh refused as too big | Turn on `model_mods_decimate`, or reduce the mesh yourself. |
| Mod textures wrong on a wheel | Try `model_mods_rim_keep_badge`, and see `model_mods_rim_texture_mode` in [Wheel Mods](Wheel-Mods). |
| Car skipped | See the table in [Car Mods](Car-Mods#failure-modes). |
| `already shipped by another mod, skipped` | Two mods ship the same file path. Remove one. |
| Game crashes at boot after adding many archives | The loader records a crash once nine archives are mounted; music is capped at four archives. |

## Texture mods

| Symptom | Cause |
|---|---|
| Replacement ignored | The file name has no 16-digit hex hash (`carries no texture hash in its name`), or the format is refused (reason logged once per file), or the hash is shadowed by another file (`which wins`). |
| Replacement not used after editing | Reload with the menu row or `texture_reload`. |
| Nothing changes at all | `texture_mods` is false, or the renderer is not the Plume renderer. |

## Custom music

See the troubleshooting table in [Custom Music](Custom-Music#troubleshooting). Common causes: not MPEG-1 at 32, 44.1 or 48 kHz, track too long for the header budget, two files with the same bank name, more than 1785 songs.

## Vinyls and photo mode

- Vinyl decals missing outside the editor: leave `vinyl_auto_readback` on.
- Photo preview stale or garbage: leave `photo_auto_readback` on.
- `Import: '<name>' not found in <dir>`: put the `.vgp` in `vinyls/` in the working directory.

## FAQ

**Where is my config file?** `larecomp.toml` beside the executable, per the README. It is created and edited by the game; see [Getting Started](Getting-Started).

**Can I set an option without the menu?** Yes, every cvar can be set in `larecomp.toml` or as `--name=value`; see [Settings Reference](Settings-Reference).

**Why is a menu toggle inverted?** Rows backed by `disable_*` cvars show ON when the cvar is false.

**Does a mod change the game's files?** No. Mods are packed into `xarchive_mods.rpf` (and music archives) in the game data folder, mounted last. Shipped archives are never modified.

**How do I turn off all mods?** `model_mods = false` for meshes and files, `texture_mods = false` for textures; or switch single mods off in the [Mod Browser](Mod-Browser).

**Which options need a restart?** Rows with a `(RESTART)` suffix in the menu, and cvars marked "Needs a restart" in [Settings Reference](Settings-Reference).
