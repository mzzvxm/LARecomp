# larecomp_online_tool

A small command-line program the launcher runs to ask LARecomp's online servers what the game's own server browser (F6) asks: the server list, and each relay's name, players, rooms, mods and icon. It prints one JSON document on stdout per call and nothing else there.

It is built from the game's own code, not a copy of it: the wire messages (`../protocol/browser_protocol.h`), the relay queries (`../relay_info.cpp`) and the browser's asking and text cleaning (`../server_browser/browser_query.cpp`, which the game's browser client calls too). So the game and the launcher read an answer the same way, and a change to the protocol changes both. It needs no ReXGlue, no game running and no settings file, and it is built with the game (`cmake --build <build dir> --target larecomp_online_tool`, next to `larecomp_relay_server` and `larecomp_master_server`).

The check is `--target larecomp_online_tool_test` (see `online_tool_test.cpp`): it needs no game and no real server.

## For the launcher

### The server list address is a setting, never built in

The tool has no server list address of its own. The launcher reads the player's list address from `larecomp.toml` (`online_master_server`; the game's default is the list at the address in `src/settings/generated/settings_multiplayer.h`) and passes it with `--master`. Without `--master` the tool asks no list and says so (`master.state` is `"not_set"`). A launcher setting for the list address belongs on the Settings page, with the game's default as its default.

### Running it

Start it with `ProcessStartInfo.ArgumentList` (no shell, no quoting: the arguments reach the tool as UTF-16 and it reads them as UTF-8, so a folder with an accent in its name works), read stdout to the end, and give the process a timeout. A call with a few servers takes well under a second (a few seconds when the list does not answer: the connect waits 1.5 s). A list full of silent servers takes longer, by the game's own limits (below): use a timeout of about 30 seconds, and keep what the last good call gave if it is hit.

| Exit code | Meaning |
|---|---|
| 0 | The document on stdout is the answer. A list or a relay that does not answer is still an answer (the document says so). |
| 2 | The command line was wrong. stdout has a document with `"ok": false` and `error.code` `"usage"`. |

Do not run it more often than the game does: every 8 seconds at most, never while the Online page is hidden and never while the game is running (the relay answers each address 120 queries a second; one call asks a relay about 20). The tool never writes anything except the icon files below, never reads a key or token, and never sends one.

### Commands

```
larecomp_online_tool --version
larecomp_online_tool --protocol
larecomp_online_tool servers [--master H[:P]] [--servers a,b] [--current H[:P]] [--icons DIR]
                             [--timeout-ms N] [--master-timeout-ms N] [--pretty]
larecomp_online_tool --help
```

`--version` and `--protocol` are for feature detection. The launcher runs `--version` first, accepts the tool when `schema` is a version it knows, and shows the online controls its `--protocol` `commands` list allows. A missing or older tool means "Update the game for online features here".

`servers` options:

| Option | Meaning |
|---|---|
| `--master H[:P]` | The server list to read (`POST /servers`; port 31003 when absent). Leave it out (or pass an empty value) for no list. |
| `--servers a,b` | The player's own relays, `host[:port]` separated by commas (`server_browser_servers`; port 31000 when absent). An address that is no relay but answers `/servers` is treated as a list: its relays are asked and it is not shown itself, as in the game. |
| `--current H[:P]` | The relay the game is set to (`mcla_online_server`). |
| `--icons DIR` | Keep relay icons in `DIR` as `<crc>.png` (created if missing). Without it no icon is fetched (the document still has the icon's size and checksum). |
| `--timeout-ms N` | One UDP query: details, a page of rooms, an icon chunk. 50 to 10000, default 800 (the game's). |
| `--master-timeout-ms N` | How long the list's HTTP answer may take once connected. 200 to 60000, default 3000 (the game's). The connect itself waits 1500 ms, as in the game. |
| `--pretty` | Indented output. |

At most 128 servers are asked, 8 at a time, with up to 16 pages of 4 rooms each: the game's limits. A server is asked for its details; an older relay (before 16 October 2026) that does not answer that is asked the old way (name, players, sessions, icon, no rooms) and reported with `legacy: true`.

Order of `servers[]`: the list's relays in its order, then the player's own that are not on it, then the current one; an address that is more than one of these is shown once with every flag. Sort and filter in the launcher.

### Documents

Every document starts with `ok` (boolean), `command` (string) and `schema` (integer, now 1). The schema number changes when a field changes meaning or goes; a new field does not change it, so ignore fields you do not know.

`--version`:

```json
{"ok":true,"command":"version","schema":1,"tool":"larecomp_online_tool","version":"0.1.0","game_label":"PATCH 10 - PREVIEW 2"}
```

`--protocol`: `tool`, `version`, `commands` (array of command names the tool has: `["servers"]`), `relay` (`magic` `"MC02"`, `version` 5, `messages` {name: type number}, `bytes` {name: size}: the sizes of the messages on the wire), `limits` (the game's timeouts and caps, the icon limits, the default ports).

`servers`:

```json
{
  "ok": true, "command": "servers", "schema": 1,
  "master": {
    "address": "host:31003",        // as given, null when none
    "state": "ok",                  // "not_set" | "ok" | "unreachable" | "http_error"
    "message": "",                  // the game's one-line status when it failed, e.g. "master unreachable (host:31003)"
    "detail": "",                   // the transport's reason: "connect timed out", "connection refused", ...
    "http_status": 0,               // the HTTP status when it answered
    "listed": 2,                    // servers that are on a list (this one, or a favourite that is a list)
    "via_favorites": 0              // addresses of the player's own that turned out to be lists
  },
  "summary": { "servers": 4, "answered": 3, "players": 17, "rooms": 2, "elapsed_ms": 1234 },
  "servers": [ {
    "address": "host:31000", "host": "host", "port": 31000,
    "listed": true, "favorite": false, "current": true,
    "answered": true,               // false: no answer in time
    "legacy": false,                // true: an older relay, no rooms, no flags
    "ping_ms": 23,                  // null when it did not answer
    "name": "", "description": "Line one\nLine two", "region": "EU",
    "tags": ["drift", "JDM"],
    "players": 3, "rooms": 1,
    "verified_nicks": true,         // null when unknown (legacy or no answer)
    "password": false,              // reserved: no relay sets it yet; null when unknown
    "icon": {
      "crc": "c1edab6d",            // CRC-32 of the PNG as the relay announces it; null when it has no icon
      "size": 2135,                 // its size in bytes (0 = none)
      "state": "saved",             // see below
      "file": "c1edab6d.png",       // inside --icons DIR; null unless state is "saved" or "cached"
      "error": ""                   // a plain reason for "invalid" and "write_failed"
    },
    "mods": { "required": 2, "optional": 1, "http_port": 31001, "crc": "00abcdef" },
    "room_list": [ {
      "host_vip": "10.0.0.2",       // the relay's temporary address for the host: do not keep it
      "host_gamertag": "Host", "name": "Night Race",
      "players": 2, "max_players": 8,
      "host_rtt_ms": 5,             // relay to host, null when unknown
      "active": true,               // the host answered a search lately or has players
      "props": [ { "id": 4660, "value": 7 } ]   // the host's session properties, raw numbers (at most 24)
    } ]
  } ]
}
```

(The comments are for this page; the tool writes plain JSON.) Text fields are valid UTF-8 with control characters removed (`description` keeps its line breaks, as `\n`). They come from other people's servers: show them as text, never as markup, clip them, and never open anything a server gives you.

`icon.state`:

| State | Meaning |
|---|---|
| `none` | The relay has no icon (or did not answer). |
| `not_requested` | It has one, but `--icons` was not given. |
| `saved` | Fetched, checked and written to `<crc>.png`. |
| `cached` | `<crc>.png` was already there and its content still has that checksum: nothing was fetched. |
| `unavailable` | It has one, but the fetch failed or timed out. Try again at the next refresh. |
| `invalid` | What the relay sent is not an acceptable icon (not a PNG, over 512x512 pixels, or not the checksum it announced): nothing was written. |
| `too_large` | It announces more than 64 KB: never fetched. |
| `write_failed` | The icons folder cannot be written. |

An icon file is only ever a PNG of at most 64 KB and 512x512 pixels whose CRC-32 is its file name, so `<crc>.png` can be trusted by name. Delete files you no longer need; the tool never deletes any.

`error` (exit code 2): `{"ok":false,"command":"servers","schema":1,"error":{"code":"usage","message":"unknown option '--bogus'"}}`.

### What the tool does not do (yet)

It does not read or write the identity key, the Rate My Ride token or `larecomp.toml`, does not touch the master's nick registry, and does not fetch mods (the relay's mods manifest and files are plain HTTP on `mods.http_port`: `GET /mods/manifest`, `GET /mods/file/<mod>/<path>`; the launcher's Mods page does that). Rate My Ride, leaderboards, the tournament and the identity commands are later packages (launcher-plan `ONLINE.md`, G2a and G2c).

## Building it by hand

With a build directory, `cmake --build <dir> --target larecomp_online_tool` (and `larecomp_online_tool_test`). With no build directory, on Windows:

```
clang++ -O2 -std=c++20 online_tool_main.cpp online_tool.cpp ../server_browser/browser_query.cpp ../relay_info.cpp ../rate_my_ride/rmr_net.cpp -lws2_32 -lshell32 -o larecomp_online_tool.exe
```

(On Linux: the same without `-lws2_32 -lshell32`, with `-pthread`. The sockets are plain POSIX there, but the game's online play itself is Windows only.)
