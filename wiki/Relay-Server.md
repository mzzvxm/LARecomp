# Relay Server

Host an online relay for LARecomp players. The relay is the matchmaker and packet relay for System Link over the internet. Source: `src/mc_engine/online/relay_server/relay_server.cpp` and `server.toml.example` in the same folder. Player side: [Online Play](Online-Play). The server list is described in [Master Server](Master-Server).

## What a relay does

- **Session directory:** hosts register their advertised game; clients query it.
- **Relay:** forwards emulated LAN broadcast and unicast game packets between peers. Each peer gets a unique virtual IP (vip) as its identity.
- **Rooms:** a room is the set of peers that exchanged game data (unicast on any port but the discovery port 1001) in the last 20 seconds. It splits by itself when that traffic stops. Rooms scope in-game broadcasts and group the dashboard.
- **Server browser:** the relay describes itself and its rooms to the in-game browser and announces itself to a master server so it is listed.
- **Mods:** it serves a folder of mods over HTTP so players can download them (see below).
- **Nick check:** optionally admits a gamertag only from a PC that proves it owns it.

Identity details from the code: a peer is keyed by the session id its client sends, not by its IP and port, so a NAT or VPN port change keeps the same vip. Fresh vips come from a counter saved in `relay_state.txt` next to the executable, so a vip handed out before a restart is never given to anyone else after it. Relay packets from an address that has not identified itself are dropped until its next heartbeat (at most about 5 seconds away).

## Build

CMake target `larecomp_relay_server` (defined in `CMakeLists.txt`; standalone, no SDK). The source comments also give a standalone build:

```bash
clang++ -O2 -std=c++17 src/mc_engine/online/relay_server/relay_server.cpp -x c++ thirdparty/monocypher/monocypher.c -o larecomp-server.exe
```

On Windows it links Winsock2 through a pragma; elsewhere it uses POSIX sockets.

## Run

```text
larecomp_relay_server [bind_ip] [port]
```

| Argument | Default | Meaning |
|---|---|---|
| `bind_ip` | `0.0.0.0` | Local address to listen on. `0.0.0.0`, `any`, `all` or `*` mean every interface, which is normal and what works for a public IP, a tunnel such as playit.gg, or Hamachi. Give a specific IP only to restrict the relay to one adapter. An invalid address logs a warning and falls back to all interfaces. |
| `port` | `31000` | UDP port. Players, or the tunnel, must use this port. |

Open **UDP 31000** (or your port) inbound on the host's firewall. The HTTP service listens on **TCP port + 1** (31001 by default). A relay bound to a loopback address keeps its HTTP service on loopback too.

If the bind fails the relay logs `bind <ip>:<port> failed (err N) -- port in use, or that IP is not a local adapter?` and exits.

## Files next to the executable

| File | Role |
|---|---|
| `server.toml` | Server description and options. Re-read every few seconds; edits show up without a restart. |
| `relay_icon.png` | Default icon (see `icon`). |
| `relay_name.txt` | Older way to set the name; `server.toml` wins when it has a name. The dashboard can rewrite it. |
| `relay_state.txt` | The vip counter. Created by the relay. Keep it, so vips are never reused. |
| `mods/` | Mods offered for download (see `mods_dir`). |

## `server.toml`

Copy `server.toml.example` next to the relay executable as `server.toml`. The parser accepts a subset of TOML: one `key = value` per line, values in double quotes (escapes `\"`, `\\`, `\n`, `\t`) or bare, `#` comments, an optional UTF-8 byte order mark. Tables are not used. Strings are UTF-8; control characters are dropped.

| Key | Default | Limit | Meaning |
|---|---|---|---|
| `name` | none | 47 bytes | Server name shown in the browser. |
| `description` | `""` | 255 bytes | Description. `\n` makes a line break. |
| `region` | `""` | 23 bytes | Region label, for example `BR`. |
| `tags` | `""` | 95 bytes | Comma-separated tags. |
| `icon` | `relay_icon.png` | PNG, 64 KB, 512x512 at most | File name next to the executable. Must be a plain name; a name with a path separator, a colon or `..` falls back to `relay_icon.png`. |
| `public_address` | `""` | 63 characters for the host | `host[:port]` where players reach the relay when it is not the machine's own public IP and the relay port (a tunnel, a DNS name, a forwarded port). |
| `master` | `""` | | `host[:port]` of the master server to be listed on. Empty means not listed; players can still add the relay by address. Port defaults to 31003. |
| `mods_dir` | `mods` | plain folder name | Folder of mods. A name with a path separator, a colon or `..` falls back to `mods`. |
| `mods_required` | `""` | 1023 characters | Comma-separated mod names players must have, or `*` for every mod in `mods_dir`. Every other mod is optional. |
| `public_http_port` | `0` | 1 to 65535 | TCP port players reach the HTTP service on when a tunnel maps it elsewhere. `0` means relay port + 1. |
| `identity_master_key` | `""` | 64 hex digits | The master's public key. Setting it turns the nick check on. Empty means no check. A value that is not 64 hex digits logs `WARNING identity_master_key is not 64 hex digits: nicks are NOT checked`. |
| `identity_master` | `""` | | `host[:port]` where players get their proof. Empty means the `master` above. |
| `identity_names` | `""` | 1023 characters | Comma-separated `host:port` names players use to reach the relay, besides `public_address` and the machine's own IPs. A proof is only accepted for a name the relay knows as its own. |
| `identity_grace_sec` | `20` | 0 to 120 | Seconds a new game session plays before its gamertag must be proved. |

The example file ships with sample text in Portuguese ("Meu Servidor"); replace it.

Example, taken from the shipped example with values changed:

```toml
name = "My Relay"
description = "Races every night.\nRules: no ramming on purpose."
region = "BR"
tags = "drift, JDM"
icon = "relay_icon.png"
public_address = ""
master = "master.example:31003"
mods_dir = "mods"
mods_required = ""
public_http_port = 0
identity_master_key = ""
identity_master = ""
identity_names = ""
identity_grace_sec = 20
```

At startup the relay logs what it read:

```text
server name: <name> | icon: <...>
server browser: region "<r>" | tags "<t>" | N-byte description | <...>
nick check: ON (master key <...>, proofs from <host>:<port>) | names: ...
nick check: off (no identity_master_key in server.toml)
```

## Being listed

With `master` set, the relay announces itself to the master every 30 seconds over UDP, using the details in `server.toml`. The master does not list an address on the relay's word: it queries the announced address and lists it only when the answer carries a valid proof. A relay that stops answering drops off after about 100 seconds. Log lines: `master announce: sending to <host>:<port> as <host>:<port>`, or `master announce: <error>`, or `master announce: socket failed (err N); this relay will not be listed`.

If the machine's public address is not reachable at the relay port, set `public_address`; otherwise the master may probe an address players cannot use.

## Hosting mods

Put one sub-folder per mod inside `mods_dir`, with the same layout as the game's mod folder (`mods/<mod>/...`). The relay builds a manifest, hashes the files, and serves them to players over HTTP; the in-game [Mod Browser](Mod-Browser) and the server browser use it.

Rules from the code:

- Folders and files whose names start with `.` are ignored.
- Programs and scripts (`.exe`, `.dll`, `.bat` and similar) are never served. The log says `skipped <dir>/<mod>/<file>: programs and scripts are never served`.
- Names the manifest cannot hold are skipped: `name not allowed in the manifest`.
- Limits: 64 mods, 20,000 files, 4 GiB in total. At most 16 downloads run at once.
- Files whose size and write time did not change keep their hash, so rebuilds are quick. The first manifest is built at startup (hashing a large set takes a second or two); later rebuilds run on their own thread.
- Each mod may carry a `mod.toml` shown in the browsers (see [Mod Browser](Mod-Browser)).

HTTP paths, public (no key): `GET /mods/manifest` and `GET /mods/file/<mod>/<path>`.

Log lines: `mods: N required, M optional, K files, <size> (crc ........)`, `mods: none (<dir>/ next to the exe)`, `mods: required mod "<name>" is not in <dir>/`, `mods: required mod "<name>" listed, but <dir>/ does not exist`, `mods: could not read <path>; skipped`.

## Nick check

To make gamertags exclusive on your relay:

1. Run a [Master Server](Master-Server). On start it prints its public key, which is also served at `http://<master>:31003/identity/v1/pubkey`.
2. Put that 64-digit key in `identity_master_key`.
3. If players should get their proof from a different master than the one listing the relay, set `identity_master`.
4. If players reach the relay by names other than `public_address` and the machine's own IPs, list them in `identity_names`.

Behavior: a register with a gamertag is only answered when its source IP proved it owns the nick. The relay logs `nick "<n>" verified from <ip> (key <...>)`, `nick proof for "<n>" from <ip> refused (status N, signed for "<name>")` and `REFUSED gt="<n>" from <ip>: nick not verified on this relay`. Each new game session gets `identity_grace_sec` seconds first, once per session, with at most 6 new sessions per IP per minute. Without `identity_master_key`, anyone plays under any gamertag.

## Admin dashboard

The HTTP service on TCP port + 1 also serves a web dashboard. At startup the relay prints:

```text
admin dashboard: http://127.0.0.1:<port+1>/?key=<token>
mods (public): http://127.0.0.1:<port+1>/mods/manifest
```

The token is eight hex digits chosen at random on every start. Everything except the dashboard page itself and the mod downloads needs `?key=<token>`:

| Path | Effect |
|---|---|
| `/` | The dashboard page. It reads the key from its URL. |
| `/api/state` | JSON snapshot, republished every 500 ms. |
| `/api/kick?ip=<ip>&key=...` | Drop a peer with a short cooldown. |
| `/api/ban?ip=<ip>&key=...` | Block an IP. |
| `/api/unban?ip=<ip>&key=...` | Remove a ban. |
| `/api/name?name=<text>&key=...` | Rewrite `relay_name.txt`; a name in `server.toml` takes precedence. |

The token changes on every restart. The HTTP service listens on all interfaces unless the relay is bound to loopback, so anyone who learns the token can kick and ban; do not publish the dashboard URL. Bans are held in memory and are not saved.

## Reading the log

| Line | Meaning |
|---|---|
| `matchmaker+relay listening on <ip>:<port>` | Up. |
| `vip allocator: next <vip> (resumed from <file>)` or `new state file` | The vip counter. |
| `new peer <addr> vip=<vip>` | A client registered. |
| `peer <vip> gt="<n>" moved <a> -> <b> (same game, same vip)` | NAT or port change; identity kept. |
| `peer <vip> gt="<n>" from <addr>: re-adopted its vip` | A client returned after a relay restart. |
| `WARNING peer gt="<n>" from <addr> claims vip <vip>, which is ...; giving it a new one` | A vip claim that could not be honored. |
| `REGISTER vip=<vip> gt="<n>" ... host=<0/1> <a>/<b>` | A session was registered. |
| `relay from unidentified <addr> dropped, waiting for its heartbeat` | A packet from an unknown source. |
| `admin KICK <ip>` / `admin BAN <ip>` | Dashboard action. |
| `peers=N hosts=N (active=N) \| rx=... relayed=... sendfail=... dropped=...` | Periodic counters. |
| `FLOW <vip> -> BCAST ...` / `FLOW <vip> -> <vip> port=...` | Traffic flows, logged once per unique flow. |

## Checklist

1. Open UDP 31000 and TCP 31001 (relay port and port + 1) on the host.
2. If behind a tunnel, set `public_address` (and `public_http_port` if the HTTP port is mapped elsewhere).
3. Copy `server.toml.example` to `server.toml` and edit.
4. Set `master` to be listed. Players can always add the relay by address in `server_browser_servers`.
5. Optional: run a master, copy its key into `identity_master_key`.
6. Optional: run the [Rate My Ride Server](Rate-My-Ride-Server) on the same machine, TCP 31002.

## Not documented in the source

- Maximum number of players per relay.
- The wire protocol beyond the constants in `src/mc_engine/online/protocol/`. The relay's protocol version is 5.
