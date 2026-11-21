# larecomp_online_tool

A small command-line program the launcher runs to ask LARecomp's online servers what the game's own menus ask: the server list, and each relay's name, players, rooms, mods and icon (the server browser, F6); and the Rate My Ride + leaderboards server's rides, photos, stats boards and weekly tournament (the game's Rate My Ride, LEADERBOARDS and ONLINE TOURNAMENT screens). It prints one JSON document on stdout per call and nothing else there.

It is built from the game's own code, not a copy of it: the wire messages (`../protocol/browser_protocol.h`), the relay queries (`../relay_info.cpp`) and the browser's asking and text cleaning (`../server_browser/browser_query.cpp`, which the game's browser client calls too). So the game and the launcher read an answer the same way, and a change to the protocol changes both. It needs no ReXGlue, no game running and no settings file, and it is built with the game (`cmake --build <build dir> --target larecomp_online_tool`, next to `larecomp_relay_server` and `larecomp_master_server`).

The rmr, lb and tournament commands use the game's own Rate My Ride and leaderboards codecs (`../rate_my_ride/rmr_protocol.cpp`, `../leaderboards/lb_protocol.cpp`), its HTTP client and timeouts (`../rate_my_ride/rmr_net.*`) and the tournament schedule (`../tournament_schedule.*`): the same requests to the same server the game sends them to.

The checks are `--target larecomp_online_tool_test` (the server list and relays: `online_tool_test.cpp`) and `--target larecomp_online_tool_remote_test` (rmr, lb, tournament: `online_tool_remote_test.cpp`). Neither needs a game or a real server; they talk only to 127.0.0.1. Never point a check or a test of the launcher at a real server, a real player's token or a real nick.

## For the launcher

### The server list address is a setting, never built in

The tool has no server list address of its own. The launcher reads the player's list address from `larecomp.toml` (`online_master_server`; the game's default is the list at the address in `src/settings/generated/settings_multiplayer.h`) and passes it with `--master`. Without `--master` the tool asks no list and says so (`master.state` is `"not_set"`). A launcher setting for the list address belongs on the Settings page, with the game's default as its default.

### Running it

Start it with `ProcessStartInfo.ArgumentList` (no shell, no quoting: the arguments reach the tool as UTF-16 and it reads them as UTF-8, so a folder with an accent in its name works), read stdout to the end, and give the process a timeout. A call with a few servers takes well under a second (a few seconds when the list does not answer: the connect waits 1.5 s). A list full of silent servers takes longer, by the game's own limits (below): use a timeout of about 30 seconds, and keep what the last good call gave if it is hit.

| Exit code | Meaning |
|---|---|
| 0 | The document on stdout is the answer. A list or a relay that does not answer is still an answer (the document says so). |
| 2 | The command line was wrong. stdout has a document with `"ok": false` and `error.code` `"usage"`. |

Do not run it more often than the game does: every 8 seconds at most, never while the Online page is hidden and never while the game is running (the relay answers each address 120 queries a second; one call asks a relay about 20). The `servers` command writes nothing except the icon files below, and reads or sends no key or token. The rmr, lb and tournament commands are described in their own section below: they write only the photo files they are told to, and only `rmr rate` changes anything on a server.

### Commands

```
larecomp_online_tool --version
larecomp_online_tool --protocol
larecomp_online_tool servers [--master H[:P]] [--servers a,b] [--current H[:P]] [--icons DIR]
                             [--timeout-ms N] [--master-timeout-ms N] [--pretty]
larecomp_online_tool rmr table|top|mine|photo|rate ...        (section "Rate My Ride, leaderboards, tournament")
larecomp_online_tool lb views|size|top|around|owners ...
larecomp_online_tool tournament current|standings|next|rewards ...
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

`--protocol`: `tool`, `version`, `commands` (array of command names the tool has: `["servers","rmr","lb","tournament"]`), `subcommands` ({group: [words]}), `writes` (the commands that change a player's data on a server and need `--confirm`: `["rmr rate"]`), `relay` (`magic` `"MC02"`, `version` 5, `messages` {name: type number}, `bytes` {name: size}: the sizes of the messages on the wire), `limits` (the game's timeouts and caps for the server list, the icon limits, the default ports), `remote` (the Rate My Ride + leaderboards server: `default_port` 31002, the game's `connect_timeout_ms` 3000 and `io_timeout_ms` 15000, `max_requests_per_call`, and per group `rmr`, `lb`, `tournament` its magic, path, page sizes, photo cap, week constants, slot ranges and the prize names).

The two timeouts differ on purpose, and both are the game's own: the browser reads the server list (`POST /servers`) with 1.5 s to connect and 3 s for the answer (`browser_query.h`, and `browser_client.cpp` before that), and the identity cycle's calls to the master's nick registry (`POST /identity/v1`) use 4 s and 8 s (`identity_client.cpp`). The launch plan's "8 s for the master" was the registry's number. `servers` follows the browser; the `identity` commands (a later package) follow the identity cycle.

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

### Rate My Ride, leaderboards, tournament

These commands ask the Rate My Ride + leaderboards server (`larecomp_rmr_server`, TCP 31002; one HTTP POST per request to `/rmr/v1` or `/lb/v1`, binary body) with the game's own codecs. The group comes first on the command line (`larecomp_online_tool rmr top ...`); options are `--name value` or `--name=value`.

```
larecomp_online_tool rmr table  [--min-ratings N]
larecomp_online_tool rmr top    [--start N] [--count N] [--min-ratings N] [--owner HEX] [--photos DIR [--photo-slots 1|2]]
larecomp_online_tool rmr mine   (--owner HEX | --token-stdin) [--photos DIR [--photo-slots 1|2]]
larecomp_online_tool rmr photo  --file ID[,ID] --out DIR
larecomp_online_tool rmr rate   --record ID --rating 1..10 --confirm --token-stdin        (changes the player's rating)

larecomp_online_tool lb views   [--tables FILE]                                           (no server)
larecomp_online_tool lb size    --view N[,N] [--tables FILE]
larecomp_online_tool lb top     --view N [--start N] [--count N] [--owner HEX] [--tables FILE]
larecomp_online_tool lb around  --view N (--owner HEX | --token-stdin) [--span N] [--tables FILE]
larecomp_online_tool lb owners  --view N --owner HEX[,HEX...] [--tables FILE]

larecomp_online_tool tournament current
larecomp_online_tool tournament standings [--start N] [--count N] [--owner HEX | --token-stdin] [--view N] [--tables FILE]
larecomp_online_tool tournament next      [--weeks N]                                     (no server)
larecomp_online_tool tournament rewards   --token-stdin
```

Where the server is (the game's rule, `rmr_client.cpp`'s `ResolveTarget`): `--server H[:P]` is the game's `rmr_server` setting; `--relay H[:P]` is `mcla_online_server`, of which only the host counts (the relay's own UDP port is not the server's); `--port N` is `rmr_server_port` (31002). `--server` wins when both are given. With neither the document says `server.state` `"not_set"` and nothing is asked. The game's local store (no server, online off) is not something the tool reads.

| Option | Meaning |
|---|---|
| `--connect-timeout-ms N` | 200 to 60000, default 3000: the game's connect timeout for this server. |
| `--io-timeout-ms N` | 200 to 120000, default 15000: the game's, for one answer. |
| `--deadline-ms N` | 1000 to 600000, default 60000: no new request starts after this long since the call began (the one running may finish). |
| `--start N`, `--count N` | The first rank (1-based) and how many: rides 1 to 50 (default 10: the game's page), board rows 1 to 100 (default 10, 100 for `tournament standings`). |
| `--min-ratings N` | A ride is ranked from this many ratings; default 3, the number the game sends. |
| `--owner HEX` | An owner key: 16 hexadecimal digits, as the documents print them. It is public (a hash of the player's token, in every record), so it is safe to keep and to pass. `lb owners` takes up to 100 separated by commas. |
| `--token-stdin` | The player's 32-digit Rate My Ride token is on stdin, one line. It is never on a command line. See the secret below. |
| `--confirm` | `rmr rate` only: the player means it. Without it the command is a usage error and nothing is sent. |
| `--photos DIR` | `rmr top`, `rmr mine`: keep the rides' photos in `DIR` (created). `--photo-slots 1` (default) fetches the first photo of each ride, `2` both. |
| `--file ID[,ID]`, `--out DIR` | `rmr photo`: Rate My Ride file ids (8 digits: the `file_id` of a ride's photos) and the folder; up to 16 files. |
| `--record ID`, `--rating N` | `rmr rate`: a ride's id (8 digits) and 1 to 10. |
| `--view N[,N]` | Stats view numbers, 1 to 65535 (`lb size` takes up to 32, the others one). |
| `--span N` | `lb around`: rows each side of the player, 1 to 49 (default 5: 11 rows). |
| `--weeks N` | `tournament next`: 1 to 52 weeks (default 4). |
| `--tables FILE` | The game's `online_tables.json` (the log folder; package G3): which views exist and what their columns are. See below. |
| `--now UNIX` | The clock for the checks (the tournament's "time left"); the PC's clock when absent. |

What each command asks, so the launcher knows the cost:

| Command | Requests |
|---|---|
| `rmr table` | 1: ranked ride count (`kTableSize`). |
| `rmr top` | 2: the count and one page (`kByRank`), plus one download per photo kept (up to 50 rides x 2 photos). |
| `rmr mine` | 1 (`kByOwner`), plus its photos. |
| `rmr photo` | 1 per file. |
| `rmr rate` | 1 (`kRate`). |
| `lb size` | 1 per view (`kSize`). |
| `lb top`, `lb around`, `lb owners` | 1 (`kByRank`, `kByPivot`, `kByOwners`). |
| `tournament current` | 1: the tournament definition (`kTournament`), the request the game's menu sends. |
| `tournament standings` | 2 or 3: the definition, the board (1 or 2 boards, see below), the player's own row. |
| `tournament rewards` | 1 (`kRewards`). |
| `tournament next`, `lb views` | none. |

A call makes at most 40 requests, one after another (no retry), and stops starting any after `--deadline-ms`; `server.state` is then `"limit"` and what was gathered is in the document (photos not reached are `"unavailable"`). A call takes about one round trip per request on a normal link: give the process a timeout of about 90 seconds and the tool `--io-timeout-ms 5000` for a page that should feel quick. The server has no rate limit; do not call more often than the game does (the Online page's own refresh, 8 seconds at most, never while the page is hidden or the game is running).

**The secret.** The game sends the player's Rate My Ride token in every request, in clear, over plain HTTP: whoever is on the path can lift it and rate and post as the player. So the tool sends it only where a request needs it: `rmr rate` and `tournament rewards`. The other commands that take `--token-stdin` (`rmr mine`, `lb around`, `tournament standings`) use it only to know the player's public owner key (`--owner` does the same without the secret) and send an all-zero token. The token is never printed, never in a document, an error message or an exit status; a bad token on stdin is `error.code` `"token"`, exit 2, and the text is not echoed. Send it only to the server the player chose, never to the default list. Reading the token from the identity key store (`--profile`) comes with the identity commands (package G2a); until then the launcher hands it over on stdin.

All documents start with `ok`, `command`, `schema` and `sub` (the word after the command), then `server`:

```json
"server": {
  "address": "host:31002",     // null when none is set
  "state": "ok",               // see below
  "message": "",               // one plain sentence when state is not "ok"
  "detail": "",                // the transport's reason: "connect timed out", "connection refused", ...
  "http_status": 200,          // the HTTP status when the server answered, else 0
  "err": 0,                    // the game's error code when state is "refused"
  "requests": 2,               // requests this call made
  "elapsed_ms": 17
}
```

`server.state`: `"not_set"` (no `--server`/`--relay`), `"ok"`, `"unreachable"` (connect refused, bad HTTP, body over the game's 1 MB cap), `"timed_out"`, `"http_error"` (any HTTP status but 200; `http_status` says which), `"no_service"` (the server answered 404 on `/lb/v1`: an older `larecomp_rmr_server` without leaderboards; the game then uses its local store), `"bad_answer"` (200 but not an answer: wrong magic, truncated, too many records, a tournament file that is not the game's), `"refused"` (the server answered with an error code: `err` is the game's Sake code, 1 invalid or not allowed, 7 not allowed, 10 no such ride or file, 12 unknown, 13 time-out), `"limit"` (the request budget or `--deadline-ms`). Like `servers`, a server that does not answer is still an answer: exit 0 and every other field empty or `null`. Exit code 2 is a wrong command line or a bad token, with `"ok": false` and `error.code` `"usage"` or `"token"`.

`rmr table`: `min_ratings`, `ranked` (the count, `null` when the server did not answer).

`rmr top`: `min_ratings`, `start`, `count` (asked), `total_ranked`, `rides[]`. `rmr mine`: `owner`, `found` (`null` when the server did not answer), `ride` (a ride or `null`: "You have no ride on this server"). A ride:

```json
{ "id": "52000001", "owner": "0c50476a8dc2d715", "mine": false,
  "gamertag": "Alpha", "ratings": 3, "average": 9, "rank": 1, "ranked": true,   // rank 0 = fewer than the minimum of ratings
  "content": 3,                       // the DLC bit of the car
  "created": 1795285900, "updated": 1795285900,    // Unix seconds
  "car_config_bytes": 64,             // the car's configuration file: not downloaded, there is nothing to show
  "sale": { "vehicle": 300, "price": 5000, "purchases": 2, "pending_reward": 1, "for_sale": true },   // null when the ride has no sale data
  "photos": [ { "slot": 1, "file_id": "52000002", "size": 120, "state": "saved", "file": "52000002-120.jpg", "error": "" },
              { "slot": 2, "file_id": "00000000", "size": 0, "state": "none", "file": null, "error": "" } ] }
```

`mine` is true when the ride's owner is the `--owner` (or the token's owner) given. `average` has at most two decimals. `photo.state`: `none` (no photo in that slot), `not_requested` (no `--photos`, or beyond `--photo-slots`), `saved`, `cached` (the file was already there with the announced size and a JPEG header: nothing fetched), `unavailable` (the server did not answer or no longer has it), `invalid` (not a JPEG, or not the size the ride announced), `too_large` (announced over 204,799 bytes: never fetched), `write_failed`. A kept photo is `<8 digit file id>-<size>.jpg` in the folder and is at most 204,799 bytes starting FF D8 FF; a ride that is uploaded again gets new file ids, so its photos get new names. Delete files you no longer need: the tool never deletes any.

`rmr photo`: `photos[]` of `{file_id, size, state, file, error}` (the same states; it always fetches). `rmr rate`: `result` is `"rated"` (then `ratings` and `average` are the ride's new numbers), `"no_such_ride"`, `"refused"` (the player's own ride, which the game never offers either), `"error"` (the server's error code is in `server`), or `"not_sent"` (the server did not answer); also `record` and `rating`. Rating a ride again replaces the player's earlier rating. A rating from outside the garage scene changes how ranks fill: the launcher should ask the player first (launch plan question 4).

`lb views`: `tables` and `views[]`. Every board command has `tables` (`state`: `"not_given"`, `"ok"`, `"not_found"`, `"unreadable"`, `"invalid"`, `"unknown_schema"` or `"stats_unavailable"`, plus `game_version`, `views` (how many) and `error`) and a view object `{id, name, known, kind, time_attr, time_unit, columns[]}` (`kind` below); `lb size` has `boards[]` of `{view, name, known, total}`. `lb top`, `lb around` and `lb owners` have `total` (rows in the view) and `rows[]`; `lb around` also has `found` (the player has a row; when not, `rows` is empty). A row:

```json
{ "owner": "732c9951bf8222d5", "mine": false, "gamertag": "Charlie", "rank": 1,
  "rating": 150, "updated": 1795285900,
  "columns": [ { "attr": 1, "type": "int32", "value": -2 },
               { "attr": 2, "type": "float", "value": 55.125 },
               { "attr": 3, "type": "int64", "value": -7 },
               { "attr": 65534, "type": "int32", "value": 150 } ] }     // 0xFFFE: the rating the server ranks by
```

`type` is `context`, `int32`, `int64`, `float`, `double` or `unknown`; an int32 or context is a signed 32-bit number, a float is the low 32 bits as a float (seven digits), a double is the 64 bits as a double; NaN and infinity are `null`. The rows come in the server's rank order: the rating column (0xFFFE) highest first, which is best first on every board the game has. Attribute meaning is the game's stats views'; the tool shows the numbers, with one interpretation, the race-time boards:

`kind` (of the view; from `--tables`, so `"unknown"` without it) is `"time"` for a race-time board, `"score"` for a board whose rating is a sum or the last value, else `"unknown"`. A time board is the game's own shape (its XVC2, views 13 to 500; `../tournament_schedule.h`, `kTimeBoardTimeAttr`): the rating is the NEGATED time aggregated by max (so highest first is fastest first, the rows need no re-sorting) and the time itself is an int64 column aggregated by min (`time_attr`, 2 in the game). On a time board every row has `"time"`: the best time (the value of that column; for a row written without it the negated rating; `null` when neither is there) in `time_unit` (`"ms"`, see the CHECK). A launcher shows `time`, not the negative rating. Views 1 to 12 are score boards (the rating is a sum). `lb around` and the tournament standings use the same fields.

`--tables FILE`: the game writes `online_tables.json` into its log folder (package G3, `../online_tables_core.h`): the title's stats views and their columns (property id, attribute, aggregation rule) read from the player's own install at run time. With it a board's `view` says whether the game knows the view (`known`) and lists the columns `{attr, prop (8 hex digits), type, agg}`, where `agg` is `last`, `sum`, `min` or `max`. The dump has no view names today (its own CHECK), so `name` is `null` and the launcher shows the numbers; a later dump with a `"name"` on a view is shown as it is. A missing or damaged file is a `tables.state`, never a failure of the command. Nothing of the game is in the repository: the file only exists on a PC that has the game.

`tournament current`: `tournament` (or `null` when the server did not answer):

```json
{ "week": 2967, "version": 1, "slot": 22,                  // the weeks since 1970-01-05 UTC; the leaderboard slot (13..39, 351..381)
  "race_key": "e68bb43f", "race": "Hollywood Sprint 5",    // race: the rotation's name for the key, null when it is not in the rotation
  "restriction": { "type": "car_class", "type_id": 2, "any_car": true, "data": null },    // car_model | car_field | car_class | unknown
  "description": "Hollywood Sprint 5\nAny car. Ends Monday 00:00 UTC.\n1st: Audi R8, TIS rims, hydraulics.\n2nd-3rd: TIS rims, hydraulics.",
  "lines": ["Hollywood Sprint 5", "..."],
  "prizes": [ { "place": "1st", "rewards": ["Audi R8", "TIS rims", "hydraulics"] }, { "place": "2nd-3rd", "rewards": ["TIS rims", "hydraulics"] } ],
  "starts": 1794787200, "ends": 1795392000, "now": 1795089600, "active": true, "seconds_left": 302400 }
```

The game's file carries the week number, the slot, the car rule, the race and the text the menu shows (`../tournament_schedule.h`); the prizes are read back out of that text (lines of the form `1st: Audi R8, TIS rims.`; a line that names no reward the game knows is left out; `lines` is always complete) and the week's start and end are computed from the week number (Monday 00:00 UTC, a week long), since the file has neither. `tournament next` is the default rotation the server follows unless its `tournament.txt` says otherwise (`source` `"default_rotation"`, `server_can_change` true): `weeks[]` of `{week, slot, race_key, race, starts, ends, prizes {first[], podium[], podium_places}}`, no server asked. `tournament rewards`: `rewards` `{mask, items[] of {bit, name, won}, other_bits}`. The prizes are bits of a mask (0 Audi R8, 1 TIS rims, 2 hydraulics); the server settles a week the first time anyone asks for rewards after it ended, as the game's own request does.

`tournament standings`: `tournament` (as above), `tables`, and `board`: `{view, view_basis, candidates[], total, start, rows[], mine}`; each row has `in_week` (written since the week began: the prizes count only those). `mine` is the player's own row wherever it is (`null` without `--owner`/`--token-stdin` or without a time). The slot's times are in one stats view, by the game's own rule (its code writes and reads them with it): the slot itself below 351, slot + 31 from 351 (slots 351 to 381 are views 382 to 412); the server settles the prizes on the same view. `view_basis` is `"slot"`, `"slot_plus_31"` or `"override"` (`--view N`); `candidates` is that one view (a list, because the tool before the rule was settled asked two). A tournament board is always a race-time board (`board.view.kind` `"time"`, each row has `time`), with or without `--tables`.

### CHECKs

- **Slot to stats view: settled.** The game's code (the write sub_8229D2E0, which hands the Stats Worker the view id, and the read of the player's best time sub_822CF070) maps a slot with `slot >= 351 ? slot + 31 : slot`, so slots 351..381 are views 382..412. The server settled prizes on the slot as the view, so a week with a slot from 351 settled with no winners: `tournament_awards.cpp` now settles on `StatsViewForSlot(slot)` (and empties that view when a week begins), and a week an older server settled on the wrong board (`tournament_weeks.txt` value `1`, slot from 351) is settled once more on the right one; prizes only add up, so nobody loses one. The file now writes `2` for a settled week (an older server reads it as settled).
- **Time boards: settled, nothing to re-rank.** `lb_store.cpp` ranks the rating column highest first, and that is right: the game's race-time boards (every view from 13 on) store the negated time in the rating, aggregated by max, and the time itself in attribute 2, aggregated by min (the XVC2's own aggregation words; the game writes `+124` the time and `+136` its negation in sub_8229D2E0). No rating column in the game's views is aggregated by min (the 501 views have only max and sum ratings), so the case the older note feared (a min-aggregated rating ranked slowest first) does not occur. The tool now says which boards are times (`kind`, `time`) so the launcher can show a time instead of a negative number.
- **Time unit.** CHECK: `time_unit` `"ms"` is assumed: the game's code was read for which column holds the time and its sign, not for its unit. One real board (a time and the race it came from) confirms it; the launcher formats minutes, seconds and thousandths from it.
- **Prize text: by design.** The game's definition file has no prize fields (version, slot, car rule, race, text: `../tournament_schedule.h`), and its Rewards file is only the player's own mask, so the text the menu shows is the only prize data the game's own protocol carries; the tool parses it (the server's own wording round-trips for every prize and podium size: tested). A server with its own `description` in `tournament.txt` that does not list `1st: ...` lines shows no prizes in `prizes` (`lines` has the text). `tournament next` has them as data, from the default rotation.
- Not run against the real server (none is known), only against the real store code on loopback (below).

### The checks and what they ran against

`larecomp_online_tool_remote_test` (over 1,200 checks): the command line (every command, every option, about 80 ways to get it wrong); answers recorded once from the real stores (`online_tool_remote_recorded.h`: the real `rmr_store`, `lb_store` and `tournament_schedule` code in a throwaway program, frozen as hex of the wire bytes); hostile answers (garbage, truncation, wrong magic, a server that hangs, HTTP errors and 404, hostile text, wrong sizes, NaN and infinity, a tournament file that is short, with controls or from no language); the tables file in every state; the request budget and the deadline; which commands put the token on the wire; and a live server made of the real `larecomp_rmr_server` store code (`rmr_store.cpp`, `lb_store.cpp`, `tournament_awards.cpp`) behind a listener on 127.0.0.1 only, with rides, photos kept in a folder, ratings, boards of every shape, a tournament with the server's own `tournament.txt`, and a week that ends and settles its prizes. `larecomp_rmr_server` itself binds every interface (`rmr_net.cpp`'s `Serve`), so running it asks Windows Firewall: the check uses its store code and handler logic instead; an optional `--bind` for the servers would let the check run the program itself.

### Local test servers: `--bind`

The three servers listen on every interface by default, as a server for other players must. For a test, give each one `--bind 127.0.0.1` (or set `LARECOMP_BIND=127.0.0.1` once for all three): then nothing reaches them from the network and Windows Firewall has nothing to ask about.

```
larecomp_master_server --bind 127.0.0.1 --port 31003
larecomp_rmr_server    --bind 127.0.0.1 --port 31002
larecomp_relay_server  --bind 127.0.0.1 --port 31000
```

`--bind` takes one local IPv4 address (`localhost` means 127.0.0.1; `0.0.0.0` means every interface). It beats `LARECOMP_BIND`, which beats the default. A wrong value (a name, IPv6, `host:port`, a multicast address) stops the server with a message; it never falls back to every interface. The log says what was bound (`bind: 127.0.0.1 (this machine only), from --bind`). A master bound to one address only lists relays that address can reach (on 127.0.0.1: relays on this machine). The relay still takes its older `[bind_ip] [port]` form, but not mixed with `--bind`/`--port`; its dashboard follows `--bind` only when that is a loopback address. `larecomp_bind_address_test` checks the rules and the listener on loopback; `../server_bind_check.ps1` starts the real servers on 127.0.0.1 and asks Windows what they listen on.

### What the tool does not do (yet)

It does not read or write the identity key or `larecomp.toml`, does not touch the master's nick registry, does not upload or buy rides, does not write leaderboard rows, and does not fetch mods (the relay's mods manifest and files are plain HTTP on `mods.http_port`: `GET /mods/manifest`, `GET /mods/file/<mod>/<path>`; the launcher's Mods page does that). The identity commands are a later package (launch plan `ONLINE.md`, G2a).

## Building it by hand

With a build directory, `cmake --build <dir> --target larecomp_online_tool` (and `larecomp_online_tool_test`, `larecomp_online_tool_remote_test`). With no build directory, on Windows (clang or llvm-mingw, from this folder):

```
clang++ -O2 -std=c++20 online_tool_main.cpp online_tool.cpp online_tool_remote.cpp ../server_browser/browser_query.cpp ../relay_info.cpp ../rate_my_ride/rmr_net.cpp ../rate_my_ride/rmr_protocol.cpp ../leaderboards/lb_protocol.cpp ../tournament_schedule.cpp -lws2_32 -lshell32 -o larecomp_online_tool.exe
```

(On Linux: the same without `-lws2_32 -lshell32`, with `-pthread`. The sockets are plain POSIX there, but the game's online play itself is Windows only.) The remote check adds `online_tool_remote_test.cpp`, `../rate_my_ride/rmr_store.cpp`, `../leaderboards/lb_store.cpp` and `../tournament_awards.cpp` and drops `online_tool_main.cpp` (its header comment has the line); it prints every document it makes when `LARECOMP_ONLINE_TOOL_TEST_DUMP` is set.
