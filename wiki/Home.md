# LARecomp Wiki

LARecomp is a static recompilation of Midnight Club: Los Angeles (Complete Edition) for Windows x86-64, built on the ReXGlue SDK. This wiki documents using and modding it. The project overview, build instructions and credits are in the [README](../README.md). Everything here is derived from the repository; where the source does not say, the page says so.

## Players

| Page | Contents |
|---|---|
| [Getting Started](Getting-Started) | Requirements, game data folder, first launch dialogs, where settings and logs live, when `larecomp.toml` is rewritten, hotkeys. |
| [Settings Reference](Settings-Reference) | Every cvar with type, default, range, menu location and source file; the pause menu layout; environment variables. |
| [Settings Renderer Advanced](Settings-Renderer-Advanced) | Diagnostic and tuning cvars of the two renderers. |
| [Online Play](Online-Play) | Relays, server browser, nick check, Rate My Ride, leaderboards, online cvars. |
| [Custom Music](Custom-Music) | Put MP3 files on the radio. |
| [Mod Browser](Mod-Browser) | Install, update and switch mods on or off from inside the game. |
| [Photo Mode](Photo-Mode) | Photo mode and album fixes and options. |
| [Troubleshooting](Troubleshooting) | Known issues, log messages and fixes, FAQ. |
| [Glossary](Glossary) | Project terms. |

## Modders

| Page | Contents |
|---|---|
| [Mod Loader](Mod-Loader) | Mod folder rules, load order, reload behavior, characters, raw files, worked example, failures. |
| [Texture Modding](Texture-Modding) | Replace and dump textures; hash naming; formats. |
| [Car Mods](Car-Mods) | Replace or add cars: `vehicles/<car>/`, `parts.txt`, slots, donors. |
| [Wheel Mods](Wheel-Mods) | Replace or add wheels: `rims/`, `.donor` sidecars. |
| [Map Mods](Map-Mods) | `warps.txt`, `glows.bin`, `lights.bin`, `breakables.bin`. |
| [Custom Music](Custom-Music) | Manifest format, limits, radio integration. |
| [Menu Cameras](Menu-Cameras) | `menu_cameras.txt` and its cvars. |
| [Vinyls](Vinyls) | `.vgp` export and import. |
| [String Replacement](String-Replacement) | Dump and replace game text. |
| [Debug Options](Debug-Options) | `debug_options.txt` and the table of developer options. |
| [Mod Browser](Mod-Browser) | `mod.toml` fields. |
| [Relay Server](Relay-Server) | Host an online relay: build, run, `server.toml`, mods hosting, nick check, dashboard. Server operators. |
| [Master Server](Master-Server) | Host the server list and nick registry. Server operators. |
| [Rate My Ride Server](Rate-My-Ride-Server) | Host the shared Rate My Ride and leaderboards server. Server operators. |

## Conventions

- Paths in backticks are relative to the repository, or to the executable folder when they describe runtime files.
- `<mod folder>` means whichever of `modloader/`, `mods/` or `models/` exists beside the executable.
- "Source" columns name the file where the claim was checked.
