# Rate My Ride Server

The shared server for Rate My Ride, leaderboards and the [Online Tournament](Online-Tournament). Players rate and buy each other's rides, share boards and run the weekly tournament through it. Without it, Rate My Ride uses a local store on each PC and the boards are local. Source: `src/mc_engine/online/rate_my_ride/rmr_server_main.cpp`. Player settings are in [Online Play](Online-Play#rate-my-ride-and-leaderboards).

## Build and run

CMake target `larecomp_rmr_server` (standalone, no SDK; sources are the Rate My Ride and leaderboard protocol and store files).

```text
larecomp_rmr_server [--port 31002] [--data <dir>]
```

| Option | Default | Meaning |
|---|---|---|
| `--port`, `-p` | `31002` | TCP port. An invalid value prints `invalid port '<x>'`. |
| `--data`, `-d` | `rmr_data` next to the executable | Data folder. It survives restarts. |
| `--help`, `-h` | | Print usage. An unknown argument prints `unknown argument '<x>' (try --help)`. |

Run it on the same machine as the [Relay Server](Relay-Server). Players with online enabled reach it at the relay's address on TCP 31002 (`rmr_server_port`), or at another host named in `rmr_server`. Open **TCP 31002** inbound on the host's firewall, like UDP 31000 for the relay.

## Data

| Path | Contents |
|---|---|
| `<dir>/rmr.db` | Rate My Ride records. |
| `<dir>/` | The uploaded files. |
| `<dir>/leaderboards/leaderboards.db` | Leaderboards. |
| `<dir>/tournament.txt` | Optional overrides for the running tournament, written by the operator. See [Online Tournament](Online-Tournament#tournamenttxt-optional-written-by-you). |
| `<dir>/tournament_weeks.txt` | Tournaments served and whether each week has been settled. |
| `<dir>/tournament_awards.txt` | Tournament prizes won, per player. |

Back up the whole folder.

## HTTP endpoints

| Request | Meaning |
|---|---|
| `GET /rmr/v1` | Plain-text status, handy from a browser. |
| `POST /rmr/v1` | One game request (binary; see `rmr_protocol.h`). |
| `GET /lb/v1` | Leaderboards status. |
| `POST /lb/v1` | One leaderboard request (binary; see `leaderboards/lb_protocol.h`). |

## Log

Each request is logged with a timestamp, the peer address and a one-line summary (`[<time>] <peer>  <line>`, leaderboard requests prefixed `lb`). Startup logs `Rate My Ride server on TCP <port>, data in <dir>` and summaries of the store and the boards. On stop it logs `stopped (<summaries>)`; the server stops on a termination signal.

## Not documented in the source

- Any limit on record counts or upload size beyond what the protocol headers enforce.
- Authentication. Players are identified by a token kept with their identity ([Online Play](Online-Play#nick-check-and-identity-f8)); the server-side rules are in the store sources, not described here.
