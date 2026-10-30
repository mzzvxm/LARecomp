# Getting Started

Setup, build and credits live in the project [README](../README.md). This page only adds what the source says about first launch and where settings live. Run notes are in `documentation/RUNNING.md`.

## Requirements

From the README:

| Item | Requirement |
|---|---|
| SDK | ReXGlue SDK 0.10.0 |
| Build tools | CMake 3.25 or newer, Clang (LLVM or Visual Studio), Ninja. MSVC does not build the project. |
| Game data | Your own copy of the game. The repository ships none. |
| Platform | Windows x86-64 is the target. `LARECOMP_GPU` (the Plume renderer) defaults to ON on Windows and OFF elsewhere (`CMakeLists.txt`). |

## Game data folder

LARecomp needs a folder holding `default.xex` and at least one of `xarchive_cache.rpf`, `xarchive_audio.rpf`, `xarchive_audlo.rpf` (`src/larecomp_app.h`). A half-finished install is rejected.

Lookup order at startup:

1. `--game_data_root` on the command line, if it is valid.
2. The `MCLA_GAME_DATA` environment variable, if it is valid.
3. The folder remembered from a previous ISO install.
4. Walking up from the executable folder. At each level it checks `MCLA_Game_Files/`, `game/`, `assets/`, then the level itself.

An invalid `--game_data_root` or `MCLA_GAME_DATA` value is reported and ignored; auto-detection continues. If nothing is found, the install wizard runs (see below). The fallback location is `assets/` beside the executable.

## First launch

Several dialogs can appear before the game starts. Each is skipped when it has nothing to offer.

| Dialog | When it appears | How to skip or script it | Source |
|---|---|---|---|
| ISO install wizard | No valid game data folder was found. Validates that the ISO is an Xbox 360 image whose `default.xex` carries the expected title, then installs into the game data folder. | `LARECOMP_INSTALL_ISO` environment variable runs an unattended install. | `src/isoinstaller/larecomp_iso_installer.cpp` |
| Save import wizard | No usable `mc4.sav` exists in the content store yet. Imports a save from Xenia (`mc4.sav`, copied as is) or RPCS3 (`RAGE.SAV`, build tag patched). | `skip_save_import` cvar. Skipping is not an error; the game creates a new save. | `src/saveporter/larecomp_save_porter.cpp` |
| Save picker | Several saves exist on disk and the choice is not fixed. Lists existing saves, can start a new one, can remember the choice. | `profile_auto_select` with `user_name` set. Holding Shift while the game starts shows the picker once anyway. | `src/profile/larecomp_profile_picker.cpp` |

The ISO install wizard and the other dialogs are ImGui windows drawn before the runtime exists.

## Where things are written

| Path | Contents | Source |
|---|---|---|
| `larecomp.toml` | Saved settings. Flat `key = value` lines. | `src/mc_engine/config_save.cpp`, README |
| `logs/larecomp_NNN.log` | Rolling main log. | `documentation/RUNNING.md` |
| `logs/effective_config.txt` | Cvar values that took effect, rewritten each launch. Check it first when a setting appears to do nothing. | `src/larecomp_app.h`, README |
| `stubs.txt` | Unregistered guest addresses hit at runtime. | `documentation/RUNNING.md` |
| User data root | Saves, shader cache, achievements. Defaults to a `larecomp` folder under the user's Documents. If that cannot be written, data moves to a `larecomp` folder under the local application data folder, existing saves are copied, and a `.moved_from_documents` marker keeps it there. Setting `user_data_root` overrides the choice. | `src/larecomp_app.h` |
| `<mod folder>/` | `modloader/`, `mods/` or `models/` beside the executable. See [Mod Loader](Mod-Loader). | `src/mc_engine/modloader/modloader.h` |
| `debug_options.txt`, `menu_cameras.txt` | Read from the working directory. See [Debug Options](Debug-Options) and [Menu Cameras](Menu-Cameras). | `src/mc_engine/hooks/dev_options.cpp`, `src/mc_engine/menu_camera.cpp` |

## Settings: how they are set

Most settings are cvars. A cvar can be set three ways:

- In the pause menu: Options, then the Settings tab. Rows are listed in [Settings Reference](Settings-Reference).
- On the command line as `--name=value`.
- In `larecomp.toml` beside the executable.

Every user-facing cvar is listed in [Settings Reference](Settings-Reference), with the menu row when one exists.

### When the game rewrites `larecomp.toml`

Settings changed in the pause menu apply at once and are saved when the submenu is left (`src/mc_engine/pause_menu.cpp`). Rules from `src/mc_engine/config_save.cpp` and `src/profile/larecomp_profile_store.h`:

- Only cvars changed at runtime (by a menu, the F4 settings panel or the console) are written. Values that came from the command line, the environment or the file itself are not.
- Only the affected lines are rewritten. Other lines, comments and keys the build does not know stay as they are.
- A changed value that returned to its default only rewrites a line the file already has.
- Keys are looked up above the first `[table]`; new keys are added there.
- The file is written to a temporary file and moved over the original, so a crash mid-write does not leave half a config.
- A value that cannot be written as valid TOML is skipped with a warning in the log.

Restart-only rows are different: the menu holds the new value and writes it to the file when the submenu saves, but the live value does not change until the next launch. The rows affected are renderer, graphics API, native MSAA and anisotropy, Plume render resolution and FSR1 preset (`kDeferredCvars` in `src/mc_engine/pause_menu.cpp`).

### Environment variables

The README lists the variables the build reads (`REX_LOG_LEVEL`, `MCLA_GAME_DATA`, `MCLA_FPS_CAP`, `MCLA_MAX_FRAME_MS`, `MCLA_TIMING_LOG`, `MCLA_TEX_SOFT`, `MCLA_TEX_HARD`, `MCLA_TEX_RTT`, `MCLA_VSYNC`, `MCLA_REFRESH_RATE`, `MCLA_ALLOW_INVALID_FETCH`, `MCLA_NO_STUB_SWEEP`, `LARECOMP_LOG_FILE`). Defaults and effects are in [Settings Reference](Settings-Reference#environment-variables).

### Log level

Non-Release builds log at `trace` by default, which the README describes as enough synchronous disk writing to cause stutter. Set `REX_LOG_LEVEL=warn`. It has to be an environment variable (`documentation/RUNNING.md`).

## Overlay and hotkeys

| Key (cvar) | Action | Source |
|---|---|---|
| F4 (`bind_mcla_settings`) | Toggle the settings panel. The key is taken from the SDK's `bind_settings` when that is set, otherwise F4. | `src/larecomp_app.h`, `src/mc_engine/ui/settings.cpp` |
| F3 (`bind_mcla_telemetry`) | Toggle the telemetry overlay. The key is taken from the SDK's `bind_debug_overlay` when that is set, otherwise F3. | `src/larecomp_app.h`, `src/mc_engine/ui/debug_overlay.cpp` |
| F5 (`bind_mod_browser`) | Mod browser. See [Mod Browser](Mod-Browser). | `src/mc_engine/modloader/modsbrowser/mod_browser_ui.cpp` |
| F6 (`bind_server_browser`) | Server browser. | `src/mc_engine/online/server_browser/browser_ui.cpp` |
| F8 (`bind_identity`) | Online identity window. | `src/mc_engine/online/identity/identity_ui.cpp` |
| F9 (`bind_map_warp`) | Warp to a mod's warp point and back. See [Map Mods](Map-Mods). | `src/mc_engine/map_warp.cpp` |
| F2 (`bind_gpu_texture_dump`) | Dump the textures of the next frame (Plume renderer). See [Texture Modding](Texture-Modding). | `src/gpu/renderer.cpp` |
| F10 (`bind_gpu_trace_next`) | Write a raw trace of the next guest frame (Plume renderer). | `src/gpu/renderer.cpp` |
| F11 (`bind_gpu_renderdoc_capture`) | Capture the next frame with RenderDoc (Plume renderer). | `src/gpu/renderer.cpp` |

The README and `documentation/RUNNING.md` describe the pause menu as Start or Escape, then Options. The tab list in those files predates the current menu; use [Settings Reference](Settings-Reference#pause-menu-layout) for the layout in the source.

## Known issues

See the README's "Known issues" table and [Troubleshooting](Troubleshooting).
