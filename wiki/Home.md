# LARecomp Wiki

LARecomp is a static recompilation of Midnight Club: Los Angeles (Complete Edition) for Windows x86-64, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It runs the game's own code natively, with a renderer of its own (Plume), an in-game settings menu, a mod loader, online play through community servers, and many fixes for modern hardware.

> [!IMPORTANT]
> You need your own copy of the game. LARecomp ships no game data. See [Getting Started](Getting-Started) for what it asks for and where it goes.

The project overview, build instructions and credits are in the [README](https://github.com/mzzvxm/LARecomp#readme). Everything in this wiki is derived from the repository; where the source does not say, the page says so.

## Where to start

- **First time here:** [Getting Started](Getting-Started), then [Troubleshooting](Troubleshooting) if something does not look right.
- **Want to change a setting:** [Settings Reference](Settings-Reference) lists every setting and where it lives in the menu.
- **Want to play online:** [Online Play](Online-Play).
- **Want to install or make a mod:** [Mod Browser](Mod-Browser) to install one, [Mod Loader](Mod-Loader) to make one.
- **Want to host a server:** [Relay Server](Relay-Server).
- **Met a word you do not know:** [Glossary](Glossary).

## Wiki pages

### Basics

| Page | Contents |
|---|---|
| [Getting Started](Getting-Started) | Requirements, game data folder, first launch, where settings and logs live, when `larecomp.toml` is rewritten, hotkeys. |
| [Troubleshooting](Troubleshooting) | Known issues, log messages and their fixes, FAQ. |
| [Glossary](Glossary) | Project terms. |

### Settings

| Page | Contents |
|---|---|
| [Settings Reference](Settings-Reference) | Every setting with type, default, range, menu location and source file; the pause menu layout; the DEBUG menu; environment variables. |
| [Settings Renderer Advanced](Settings-Renderer-Advanced) | Diagnostic and tuning settings of the two renderers. |
| [Debug Options](Debug-Options) | `debug_options.txt` and the table of the game's developer options. |

### Game features

| Page | Contents |
|---|---|
| [Photo Mode](Photo-Mode) | Photo mode and the photo album: fixes, EXPORT ALBUM and options. |
| [Custom Music](Custom-Music) | Put your own MP3 files on the radio: format, manifest, limits. |
| [Menu Cameras](Menu-Cameras) | Place the camera of the menus with `menu_cameras.txt`. |

### Mods

| Page | Contents |
|---|---|
| [Mod Browser](Mod-Browser) | Install, update and switch mods on or off from inside the game; `mod.toml`. |
| [Mod Loader](Mod-Loader) | Mod folder rules, load order, reload behavior, characters, raw files, a worked example, failures. |

### Content modding

| Page | Contents |
|---|---|
| [Car Mods](Car-Mods) | Replace or add cars, hide the donor's car, add parts to a shipped car, garage lists. |
| [Wheel Mods](Wheel-Mods) | Replace or add wheels; how a car finds its wheels. |
| [Texture Modding](Texture-Modding) | Replace and dump textures; hash naming; formats. |
| [Map Mods](Map-Mods) | `warps.txt`, `glows.bin`, `lights.bin`, `breakables.bin`. |
| [Vinyls](Vinyls) | Export and import vinyls (`.vgp`). |
| [String Replacement](String-Replacement) | Dump and replace game text. |

### Online

| Page | Contents |
|---|---|
| [Online Play](Online-Play) | Relays, server browser, nick check, Rate My Ride, leaderboards, online settings. |
| [Online Tournament](Online-Tournament) | The weekly tournament: how it runs, prizes, and the server's `tournament.txt`. |

### Server hosting

| Page | Contents |
|---|---|
| [Relay Server](Relay-Server) | Host an online relay: build, run, `server.toml`, mod hosting, nick check, limits, dashboard. |
| [Master Server](Master-Server) | Host the server list and the nick registry. |
| [Rate My Ride Server](Rate-My-Ride-Server) | Host the shared Rate My Ride and leaderboards server. |

## Status

The pages describe the `main` branch as of 18 November 2026 (version label PATCH 10 - PREVIEW 2). The project changes quickly: when a page and the game disagree, the game's log and `logs/effective_config.txt` are the truth, and the page is out of date.

## Conventions

- Paths in backticks are relative to the repository, or to the executable folder when they describe runtime files.
- `<mod folder>` means whichever of `modloader/`, `mods/` or `models/` exists beside the executable.
- "Source" columns name the file where the claim was checked.
- A setting is a cvar: it can be set in the pause menu (when a row exists), on the command line as `--name=value`, or in `larecomp.toml`.
