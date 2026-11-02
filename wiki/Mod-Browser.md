# Mod Browser

An in-game window (F5, `bind_mod_browser`) for the mods installed in the mod folder and for mods the known servers offer. Source: `src/mc_engine/modloader/modsbrowser/`. Rows open and close with the key; while the window is open the game receives no input, and it can be driven with a controller read through XInput.

## What it does

| Action | How it works |
|---|---|
| List installed mods | Every folder in the mod folder that the mod loader would treat as a mod, plus disabled and backup copies. Shows each mod's size, file count and its `mod.toml` information. |
| Switch a mod off or on | Renames its folder. Off adds the prefix `.disabled_`; on removes it. A folder starting with `.` is skipped by the mod loader, so the mod stops loading. |
| Clear old copies | Removes `.old_<mod>_<time>` backup folders that the browsers keep. |
| Update a mod | Installs a newer version a server offers. |
| Install a mod | Downloads a mod a server offers into the mod folder. |
| Restart | Changes take a restart; the window restarts the game with the same command line. |

Downloads go to `.download_<mod>/` first. Only when every file has arrived and matched its hash does the old folder move aside as `.old_<mod>_<time>` and the new one take the mod's name. File hashes use FNV-1a 64-bit. A file entry the client refuses is logged as `server mod file rejected`; the source does not list the rejection rules.

A server's mod list is fetched over the relay's HTTP service. A mod is installed, missing or outdated; versions are compared as dotted numbers piece by piece (`1.10` is newer than `1.9`), and non-numeric parts compare as text.

## `mod.toml`

Optional file at the root of a mod folder. The mod loader reads meshes only, so it never sees this file. Fields:

```toml
title = "Nissan Silvia S15"
description = "..."
author = "..."
version = "1.2.0"
image = "preview.png"
```

`image` is a PNG or JPG in the same folder, 1 MB at most.

## Log lines

| Message | Meaning |
|---|---|
| `[browser] server mod file rejected: <mod>/<path>` | A server listed a file path the client will not accept. |
| `[browser] mod download failed: <error>` | The download failed; the old copy stays. |
| `[browser] could not start the restart helper (<code>)` | The automatic restart could not be set up; restart manually. |
| window message `Cannot rename <folder> (<reason>)` | The folder could not be renamed (for example because a file is in use). |

## Related

- [Mod Loader](Mod-Loader) for what `.`-prefixed folders mean.
- Server and relay features are outside this wiki; their cvars are listed under Online in [Settings Reference](Settings-Reference#online).
