# Online Play

How the game's System Link and Xbox LIVE menus work online in LARecomp: the relay, the server browser, the nick check, Rate My Ride and leaderboards. Operators should read [Relay Server](Relay-Server), [Master Server](Master-Server) and [Rate My Ride Server](Rate-My-Ride-Server). Source: `src/mc_engine/online/`.

## How it works

The game's own System Link session layer is used. LAN broadcast cannot cross processes or the internet, so the online client sends game packets to a **relay** over UDP. The relay forwards them between players and gives each player a virtual IP (vip) as identity. A **master server** lists relays for the in-game server browser and, optionally, keeps a registry of gamertags (nicks). Rate My Ride and leaderboards use a separate TCP server (see [Rate My Ride Server](Rate-My-Ride-Server)).

| Piece | Default port | Role |
|---|---|---|
| Relay | UDP 31000 | Matchmaker and packet relay. |
| Relay HTTP service | TCP relay port + 1 (31001) | Mod downloads and the operator dashboard. |
| Rate My Ride and leaderboards server | TCP 31002 | Shared storage for rides and boards. |
| Master server | UDP and TCP 31003 | Server list, nick registry. |

## Going online

- The ONLINE checkbox in the SYSTEM LINK menu switches online on and off in a running game. The SDK gives up if the server does not answer within `mcla_online_connect_timeout_ms`, and online then stays off.
- The SDK cvars `mcla_online_enable`, `mcla_online_server` (`host` or `host:port`), `mcla_online_port` and `mcla_online_protocol` are read by the online client. They are registered by the SDK, so type and default are not documented in this repository.
- Rate My Ride and leaderboards reach the same host as the relay on TCP 31002, unless `rmr_server` names another one.

## Server browser (F6)

Opens with F6 (`bind_server_browser`) or the SERVER BROWSER row in the SYSTEM LINK and XBOX LIVE menus. It lists every relay the master lists plus your own, with icon, name, description, players and ping, and each relay's rooms with what they are playing. You can join one. While it is open the game gets no input; keyboard, mouse and a pad drive the window.

Joining uses the game's Quick Match: the browser names the host by its vip, the game's search keeps only that host, and the join script does the rest. "Play on this server" joins any session of that mode on the relay or hosts one for the next player. If the host is gone, the join is turned into the game's own "networking problem" error instead of making you host.

If a room belongs to a relay other than the one you are on, the browser stores the room in `server_browser_pending` and restarts to switch servers.

| Cvar | Default | Effect |
|---|---|---|
| `online_master_server` | see [Settings Reference](Settings-Reference#online) | Server list: `host` or `host:port` of a master (port 31003 if omitted). Empty means no list, only your own servers and the current one. |
| `server_browser_servers` | `""` | Your own relays, comma-separated `host:port`. |
| `server_browser_show_icons` | `true` | Show each server's icon. |

## Nick check and identity (F8)

A relay may check gamertags. When it does, a nick only plays from a PC that proves it owns the nick on the master the relay names. Until the proof lands the relay does not answer the register, so the game finds no server and keeps retrying. A new game session gets a grace period (default 20 seconds, set by the relay) before its nick must be proved.

F8 (`bind_identity`) opens the identity window. It opens by itself when something needs you. It shows who you are online, whether the server admits the nick, the recovery code to write down, a transfer to cancel, a nick to recover, and a password-protected backup.

Status texts of the window and what they mean:

| Status | Meaning |
|---|---|
| Online is off. | Online is not enabled. |
| Checking... | Asking the relay and master right now. |
| No nick check from the server | The relay does not answer (an old relay, or it is down). |
| Server does not check nicks | The relay has no `identity_master_key`; anyone plays under any nick. |
| Nick verified | The relay admits this nick from here. |
| Nick taken | Someone else owns the nick on the relay's master. |
| Nick coming to you | Your recovery code moves the nick to you at a stated time. |
| Nick server unreachable | The master the relay names does not answer. |
| Nick check failed | Anything else; the window shows the detail. |

Facts from the code:

- The first player to claim a nick on a master owns it. A recovery code or a backup moves it to another PC.
- The identity is kept outside the save folder, so sharing a save shares no identity. The same save on another PC starts a new identity (new XUID); the nick needs a backup or its recovery code to follow.
- A recovery code is shown once. The window keeps it until you confirm you wrote it down.
- If someone uses your recovery code, the nick moves to them after a hold unless you cancel from the window. The hold is set by the master (default 168 hours).
- A nick unused for the master's expiry (default 90 days) is free again.

## Rate My Ride and leaderboards

The mode runs in the game; the storage that died with the original servers is replaced.

| Cvar | Default | Effect |
|---|---|---|
| `rmr_enabled` | `true` | Serve Rate My Ride from our backend. Off gives the stock "server not available". |
| `rmr_server` | `""` | `host[:port]`. Empty uses the relay host when online is on, otherwise a local store shared by the profiles on this PC. `local` forces the local store. |
| `rmr_server_port` | 31002 | TCP port when the server runs on the relay host. A port typed into `rmr_server` overrides it. |
| `rmr_diag` | `false` | Log every request and record. |
| `leaderboards_enabled` | `true` | Serve the Xbox LIVE stats boards (race wins, battle modes, race times) from the same server. Off gives empty boards. |
| `leaderboards_diag` | `false` | Log every leaderboard read and write. |

Without a server, Rate My Ride falls back to a local store on your PC.

## Other online cvars

| Cvar | Default | Effect |
|---|---|---|
| `online_ignore_content_check` | `true` | Ignore the session DLC compatibility check when joining or hosting. The stock check wrongly demands South Central DLC even when nobody owns any. |
| `net_lift_big_session_limits` | `false` | Sessions of more than 8 players keep damage, debris, world props and default session settings. Applies to the next session you host. Menu: EXTRAS > NO 8 PLAYER LIMITS. |
| `online_race_prop_guard` | `true` | Skip the physics update of a prop whose owner is broken instead of crashing. |
| `online_diag` | `false` | Log session content masks at the join check. Verbose. |
| `online_race_prop_watch` | `""` | Diagnostic: guest address of a prop to watch. |

Discord Rich Presence has its own cvars (`larecomp_discord_rpc` and related); see [Settings Reference](Settings-Reference#online).

Full rows with sources: [Settings Reference](Settings-Reference#online).

## Known limits

- The Online Tournament is a work in progress. The tournament data path was a dead server; the code builds the tournament locally. The notes in `src/mc_engine/online/tournament.cpp` say it appears and Start passes eligibility, but launching the race still hits a null brain.
- A relay older than 16 October 2026 does not answer the info queries the browser uses (`src/mc_engine/online/relay_info.h`).

## Troubleshooting

| Symptom | Check |
|---|---|
| Game finds no server | The relay may be refusing your nick (log: `REFUSED gt=... nick not verified on this relay`). Open F8 and read the status. |
| Browser shows no relays | `online_master_server` empty or unreachable; add your relay to `server_browser_servers`. |
| Join fails with "networking problem" | The host left before the join finished. |
| Rate My Ride says "server not available" | `rmr_enabled` is off, or `rmr_server` points at a server that is down. |
| Boards empty | `leaderboards_enabled` off, or the server on TCP 31002 is not reachable. |
