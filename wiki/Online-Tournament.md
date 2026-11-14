# Online Tournament

The ONLINE TOURNAMENT mode of the pause menu, served by the [Rate My Ride Server](Rate-My-Ride-Server) (or the local store) instead of the original servers. Source: `src/mc_engine/online/tournament.cpp`, `tournament_schedule.cpp`, `tournament_awards.cpp`.

## How it works

A tournament is one of the city's regular races run against the clock, with a car rule, a leaderboard of its own and prizes. Everyone runs it alone; the best times of the week win.

1. Pause > MODES > ONLINE TOURNAMENT. The menu shows FINDING TOURNAMENT, then the week's tournament: a text box with the race, the car rule and the prizes, the race start on the map, and START TOURNAMENT.
2. START TOURNAMENT loads the race. A car that breaks the rule gets WRONG CAR.
3. At the finish the time is posted to the tournament's leaderboard. LEADERBOARDS (X) in the tournament menu shows it.
4. When the week closes, the best players get the prizes. The game unlocks them by itself the next time it syncs.

Messages the menu can show:

| Message | Meaning |
|---|---|
| NO TOURNAMENT AVAILABLE, "The Midnight Club: Los Angeles server is not available..." | The server did not answer, or it is an older server without tournaments. |
| NO TOURNAMENT AVAILABLE, "No tournament found..." | The server answered with no tournament. |
| WRONG CAR | Your current car does not meet the restriction. |
| WRONG RACE | The tournament's race is not in your city data. |

The settings are the Rate My Ride ones ([Online Play](Online-Play#rate-my-ride-and-leaderboards)): the tournament goes where `rmr_server` points, needs `rmr_enabled`, and its board needs `leaderboards_enabled`.

## Schedule

Without a `tournament.txt` (below), the server runs a rotation:

- A new tournament starts every Monday at 00:00 UTC.
- The race comes from 76 circuit and sprint races of the city (story missions, cutscene races and test tracks are left out). Consecutive weeks step 29 races at a time, so the district changes every week and every race comes up once per cycle.
- Each week uses the next of the 58 leaderboard slots the game accepts for tournaments (13 to 39 and 351 to 381). A slot that comes round again starts empty.
- Any car is allowed.

## Prizes

The game has three unlocks for rewards, and nothing else (no money):

| Bit | Prize |
|---|---|
| 1 | The 2008 Audi R8 (`vp_aud_r8_08` in `tune/vehicle/vehicle.lst`). |
| 2 | TIS Modular rims in the garage, at $0. |
| 4 | The HYDRAULICS/AIR BAGS menu in the garage without the South Central download. |

Default rule: 1st place gets all three (7), 2nd and 3rd get the rims and hydraulics (6). Only times set during the week count. Prizes add up and are kept.

The game asks for your prizes on its own: shortly after start, every 10 minutes after that, and whenever the tournament menu opens. If a prize arrives before the save loads, the next fetch delivers it again.

## For server operators

The server keeps three text files in its data folder (`rmr_data/` beside the executable, or `--data`). In local mode the same files live in `<user data>/ratemyride/`.

### tournament.txt (optional, written by you)

One `key=value` per line; `#` starts a comment. Every key is optional and overrides that field of the running week's tournament. The file is read on every request, so edits apply without a restart, and it keeps applying to the following weeks until you change or delete it.

```text
race=Hollywood Sprint 4
description=Hollywood Sprint 4\nAny car.\n1st: Audi R8.
prize_first=7
prize_podium=6
podium=3
```

| Key | Value |
|---|---|
| `race` | A race name from the rotation (`Hollywood Circuit 1`, `Downtown Sprint 3`, ...; listed in `tournament_schedule.cpp`) or the race key in hex (`0x5C4964ED`). |
| `slot` | Leaderboard slot, 13 to 39 or 351 to 381. Other values are ignored. |
| `name` | The name shown, without changing the race. |
| `description` | The whole text of the menu box. `\n` is a line break. Without it the text is built from the race, the car rule and the prizes. |
| `prize_first` | Prize bits for 1st place (sum of the table above). |
| `prize_podium` | Prize bits for 2nd place down to `podium`. |
| `podium` | How many places win, 1 to 100. Default 3. |
| `restriction_type` | The game's car rule: 0 car model, 1 a car field, 2 car class. |
| `restriction_data` | The value the rule compares; `0xFFFFFFFF` means any car (default). |

The menu box wraps long lines and breaks a line where the text has a line break; the game shows only the first entry of the description (up to 255 characters).

### tournament_weeks.txt (kept by the server)

One line per tournament served:

```text
2966 21 1794182400 1794787200 7 6 3 0
```

Week number, slot, start and end (Unix seconds, UTC), first-place prize, podium prize, podium size, closed (0 or 1). A week closes the first time anyone asks for prizes after it ends: the best players of its slot among times set during the week get the prizes, and the line gets 1. A closed week never changes.

### tournament_awards.txt (kept by the server)

One line per player who won something: the player's owner key (the identity Rate My Ride and the leaderboards use) and the sum of the prize bits won.

```text
DEDDCDEB4DF58075 7
```

Both kept files may be edited by hand to grant or take away prizes, or to reopen a week (set its last field to 0). The server reads them when it starts, so restart it after editing.

## Facts from the code

- The game fetches the tournament through the rlUpload table `tournament` (a 1556-byte definition file) and the prizes through the table `Rewards` (a 38-byte file starting `001` followed by eight flags, one per career stat `tREW[i]`). Only `tREW[0..2]` are read by the game.
- The original menu showed only this description box; the original strings send players to socialclub.rockstargames.com for rules and prizes. The texts used here are the project's own.
- A server older than 13 November 2026 answers the tournament request as malformed, and the menu shows "server is not available".

## Not documented in the source

- Which `restriction_data` values match which car models or classes. Only "any car" is known to work.
- The original tournaments' race choices, rules and prize tables.
