# Master Server

The master server does two jobs: it lists relays for the in-game server browser, and it keeps the nick (gamertag) registry that relays can use to check names. Source: `src/mc_engine/online/master_server/master_server_main.cpp`, `identity_registry.h`, and the protocol headers in `src/mc_engine/online/protocol/`. A relay is described in [Relay Server](Relay-Server); the player side is in [Online Play](Online-Play).

## Build and run

CMake target `larecomp_master_server` (standalone, no SDK).

```text
larecomp_master_server [--port 31003] [--transfer-hold-hours 168] [--expire-days 90]
```

| Option | Default | Meaning |
|---|---|---|
| `--port` | `31003` | UDP port for announces and probes, and TCP port for HTTP. The same number is used for both. |
| `--transfer-hold-hours` | `168` | How long a recovery code waits before the nick moves to the new owner, during which the current owner can cancel. Fractions are accepted. |
| `--expire-days` | `90` | A nick unused this long is free again. Fractions are accepted. |

`--transfer-hold-sec` and `--expire-sec` also exist and are marked in the code as for tests. `--help` prints the usage line.

Open **UDP and TCP 31003** inbound on the host's firewall.

Startup log lines: `master server: announces on UDP <port>, list on http://<this host>:<port>/servers`, `nick registry: N nick(s)`, and errors such as `cannot bind UDP <port> (in use?)` and `cannot serve HTTP on TCP <port>: <error>`.

## Files next to the executable

| File | Role |
|---|---|
| `identity_master.key` | The master's signing seed, in hex. Created on first run. Keep it private and back it up: relays pin the matching public key, so a new key means every relay's `identity_master_key` must change and issued tickets stop working. |
| `identity_db.txt` | One nick per line, rewritten on every change. |

## Listing relays

Relays with `master = "<this host>:31003"` in their `server.toml` announce themselves over UDP every 30 seconds. The master never lists an address on the relay's word. It sends a details query to the announced address and lists the relay only when the answer carries a proof made with the secret from the announce, so a listed relay is one players can reach. A relay that stops answering is dropped after about 100 seconds.

Limits that keep one announcer from filling the list or using the master to send probes at someone else:

| Limit | Value |
|---|---|
| Listed relays | 1024 |
| Listed relays per source IP | 16 |
| Announces per source IP per minute | 40 |
| Probe timeout | 3 seconds |
| Pending probes | 4096 |

Log lines: `listed <host:port> "<name>" (announced from <ip>)` and `unlisted <host:port> "<name>" (stopped answering)`.

## HTTP

TCP on the same port:

| Request | Answer |
|---|---|
| `GET /servers` | `text/plain`, one `host:port` per line. This is what the game reads. |
| `POST /servers` | The same (the game's HTTP helper only POSTs). |
| `GET /` | Plain-text status: every listed relay with its name. |
| `POST /identity/v1` | The nick registry. |
| `GET /identity/v1/pubkey` | The master's public key, which relays pin as `identity_master_key`. |

Players point their game at a master with `online_master_server` (`host` or `host:port`).

## Nick registry

Facts from `identity_registry.h` and `identity_protocol.h`:

- A nick maps to the public key that registered it first, plus a recovery code hash per nick. Nicks are up to 15 characters (the Xbox gamertag limit).
- The master signs tickets with its own key. A ticket is valid for 24 hours. Request times must be within 10 minutes of the master's clock. A relay keeps an IP and nick admitted for 10 minutes after a valid proof.
- There is no admin path. A nick changes hands only by its recovery code, after the hold during which the current owner can cancel from the game, or by sitting unused past the expiry.
- Operations: claim a nick, recover a nick with its code, cancel a pending transfer.
- Requests are rate-limited per source IP.

### Setting up a relay to use it

1. Start the master. Copy its public key from the startup output, or fetch `http://<master>:31003/identity/v1/pubkey`.
2. Put the 64 hex digits in the relay's `identity_master_key`. See [Relay Server](Relay-Server#nick-check).

Players see the result in the F8 window ([Online Play](Online-Play#nick-check-and-identity-f8)).

### Command-line tool

`larecomp_identity_tool` (source `src/mc_engine/online/identity/identity_tool.cpp`) performs one step of the player side per call, for testing a master by hand. Its commands are `keygen`, `request`, `proof` and `check`. It is not needed to run a master. The source does not provide a CMake target for it in `CMakeLists.txt`.

## Checklist

1. Open UDP and TCP 31003.
2. Run `larecomp_master_server`.
3. Give players the address for `online_master_server` and relay owners the address for `master`.
4. Back up `identity_master.key` and `identity_db.txt` if nicks matter.
5. For nick checking, give relay owners the public key.

## Not documented in the source

- A maximum number of nicks.
- How the master behaves behind a NAT that does not forward UDP; the port must be reachable inbound.
