# Glossary

Terms as this project uses them. Each entry names where the meaning is stated.

| Term | Meaning | Source |
|---|---|---|
| MCLA | Midnight Club: Los Angeles (Complete Edition), the game. Also the prefix of many cvars, environment variables and log tags. | README |
| LARecomp | This project: a static recompilation of MCLA for Windows x86-64. | README |
| Static recompilation | Translating the Xbox 360 PowerPC code in `default.xex` into native C++ at build time. No emulator or interpreter runs the game code. | README |
| ReXGlue SDK | The toolkit and runtime LARecomp is built on (file I/O, GPU commands, audio, threading). Version 0.10.0 per the README. | README |
| Guest, host | Guest is the recompiled game and its memory; host is the PC process around it. | `src/larecomp_app.h` and hook comments |
| `default.xex` | The game's executable. Must be in the game data folder. | `src/larecomp_app.h` |
| Game data folder (`game_data_root`) | Folder holding `default.xex` and the `xarchive_*.rpf` archives. | [Getting Started](Getting-Started) |
| User data root (`user_data_root`) | Folder for saves, shader cache and related user files. | `src/larecomp_app.h` |
| cvar | A named setting registered by code, settable in the pause menu, on the command line (`--name=value`) or in `larecomp.toml`. | README, [Settings Reference](Settings-Reference) |
| `larecomp.toml` | The saved settings file. Flat `key = value` lines; the game edits only the lines it changes. | `src/mc_engine/config_save.cpp` |
| Hot reload / restart-only | A cvar's lifecycle: hot-reloadable cvars change while the game runs; restart-only cvars take effect on the next launch. | cvar registrations |
| Pause menu | In-game menu: Options, then the Settings tab, with the hubs RECOMP SETTINGS, GRAPHICS, GAMEPLAY, INTERFACE and EXTRAS. | `src/mc_engine/pause_menu.cpp` |
| Real frame delta | `real_frame_delta`: feed the simulation the measured frame time instead of the fixed 30 Hz timestep. Not a frame rate cap. | README |
| FPS limit | `fps_limit`: the wall-clock frame cap. 0 is uncapped. | README, `src/mc_engine/hooks/frame_timing.cpp` |
| Hitch clamp | Cap on how far one frame can advance the clock (`MCLA_MAX_FRAME_MS`, default 125 ms, 16 to 1000). | README |
| vblank | The display refresh interrupt of the guest. A broken vblank timer in the SDK is behind the long-session frame rate collapse. | README |
| Xenia renderer | The pause menu's name (XENIA) for the SDK's own GPU path. | `src/mc_engine/pause_menu.cpp` |
| Plume renderer (NATIVE) | LARecomp's own renderer in `src/gpu`, built on the Plume rendering library; the menu calls it NATIVE. Texture mods require it. | `CMakeLists.txt`, `src/gpu/` |
| Legacy native runtime | The older `src/native_gfx` renderer. No longer offered in the menu. | `src/mc_engine/pause_menu.cpp` |
| EDRAM | The console's embedded GPU memory; the renderers emulate it. `single_tile` renders in one predicated-tiling tile instead of two. | `src/mc_engine/hooks/graphics.cpp` |
| MSAA, FXAA, SMAA | Anti-aliasing modes. `disable_msaa` turns MSAA off; `mcla_gpu_aa` selects `off`, `fxaa` or `smaa` on the Plume renderer. | `src/mc_engine/pause_menu.cpp` |
| FSR, CAS | FidelityFX upscaling and sharpening, selected by `present_effect` (Xenia renderer) or `mcla_gpu_fsr` (Plume renderer). | `src/mc_engine/pause_menu.cpp` |
| LOD | Level of detail. `lod_city_scale` and `lod_traffic_scale` scale it. | `src/mc_engine/hooks/graphics.cpp` |
| Impostor | A flat stand-in drawn for distant trees. `perf_no_impostors` and `disable_imposter_shadows` relate to them. | `src/mc_engine/hooks/graphics.cpp`, `render_phases.cpp` |
| Rubberband | The game's system that makes computer-controlled opponents catch up or hold back. | `src/mc_engine/hooks/rubberband.cpp` |
| Mid-asm hook | An engine patch declared as `[[midasm_hook]]` in `larecomp_config.toml` and implemented in `src/mc_engine/hooks/`. | README |
| Codegen | `rexglue codegen larecomp_manifest.toml`, which writes the recompiled sources. Never edit its output. | README |
| Cvar category | The group string in a cvar's registration, for example `MCLA/Camera`; [Settings Reference](Settings-Reference) groups by it. | cvar registrations |
| Mod folder | `modloader/`, `mods/` or `models/` beside the executable; the first present is read. | `src/mc_engine/modloader/modloader.h` |
| Mod | A subfolder of the mod folder. Its name is used in logs and by the mod browser. | [Mod Loader](Mod-Loader) |
| `xarchive_mods.rpf` | The archive the loader builds from mods and mounts last, so its files win. | [Mod Loader](Mod-Loader) |
| `xarchive_cache.rpf` | The shipped archive the loader reads as the template for meshes. | `src/mc_engine/modloader/modloader.cpp` |
| RPF | The game's archive container, extension `.rpf`. Mounted in a list at one mount point; the last mounted wins per file. | `modloader.h` |
| Resource / RSC5 | The game's packed resource format (header plus virtual and physical segments). A mod file starting with that header is packed as a resource. | `src/mc_engine/modloader/rsc5.h` |
| Drawable | A mesh resource the game draws. Mesh mods are baked into drawables. | `modloader.h` |
| Donor | A shipped asset a new car or wheel is built from. Named in `parts.txt` (`donor`) or in a `.donor` sidecar. | [Car Mods](Car-Mods), [Wheel Mods](Wheel-Mods) |
| Slot | One of the 55 part kinds of a car, with an index 0 to 9 and three LODs. | [Car Mods](Car-Mods) |
| `parts.txt` | The file in a car folder mapping slots to model groups and carrying reserved directives. | [Car Mods](Car-Mods) |
| Rim | A wheel. `rims/` holds wheel mods. | [Wheel Mods](Wheel-Mods) |
| `all` | The reserved asset name that stands in for every character or every wheel. | [Mod Loader](Mod-Loader) |
| Sidecar | A small file next to a model that adds information (`.donor`, `textures/`). | [Mod Loader](Mod-Loader) |
| Raw file | A file under `<mod>/files/`, copied into the archive under the same relative path. | [Mod Loader](Mod-Loader) |
| Content hash | XXH3-64 of a texture's base level bytes; the 16 hex digits at the end of a replacement file name. | [Texture Modding](Texture-Modding) |
| Dump | Texture files written to `textures_dump/`. | [Texture Modding](Texture-Modding) |
| Sector | A streamed chunk of the city. Breakable props are tied to a sector by name. | [Map Mods](Map-Mods) |
| Glow | A light flare drawn by the game's glow renderer. | [Map Mods](Map-Mods) |
| Light grid | The per-pixel night lighting structure over a rectangle of the city; mods add a second one. | [Map Mods](Map-Mods) |
| Warp point | A named location in a mod's `warps.txt`. | [Map Mods](Map-Mods) |
| Menu camera | The camera shot behind the front-end menus; slots 0 to 63. | [Menu Cameras](Menu-Cameras) |
| Vinyl | A decal layer on a car. `.vgp` is the export format. | [Vinyls](Vinyls) |
| Wave bank | The streaming audio container for a song. Custom MP3s are wrapped into banks. | [Custom Music](Custom-Music) |
| Cue | The sound object that plays a bank. | `src/mc_engine/music/custom_music.h` |
| Radio manager | `MUSIC_0_MANAGER`, the game object listing radio songs in seven genre groups. | `src/mc_engine/music/custom_music.cpp` |
| Debug option | One of the game's developer command-line options, reactivated through `debug_options.txt`. | [Debug Options](Debug-Options) |
| Effective config | `logs/effective_config.txt`, the cvar values that took effect at launch. | README |
| Relay, server browser | Online features: a relay server hosts sessions; the browser (F6) lists servers. | `src/mc_engine/online/` |
