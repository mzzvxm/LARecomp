# larecomp_online_tool identity

The player's online identity from the command line, for the launcher: see the gamertag and the key's fingerprint, ask a relay and its nick server what they say about the nick, make a backup file, put a backup back, handle the recovery code, move a nick with its code, stop someone else moving it, and use another gamertag. Everything else about the tool (running it, exit codes, `servers`) is in [README.md](README.md).

There is no account and no password. A profile has a **key** (made when the game first starts with it); the first player to use a gamertag on a server's nick server **owns** it there; the **recovery code** the nick server hands out once, or a **backup file**, is how a nick follows a player to another PC. See `launcher-plan\ONLINE.md` for the whole picture.

The commands are built from the game's own identity code, not a copy of it: the key file, its seal, the ticket cache and the backup file are `identity/identity_core.cpp` (the game's `identity_store.cpp` is a thin layer around it), the relay and nick server exchange is `identity/identity_net.cpp` (the game's `identity_client.cpp` calls the same code), and the gamertag rules are `identity/identity_rename.cpp` (the game's F8 window calls them too). A key written here is the game's key and the other way round.

## Rules the tool keeps

1. **No secret in a command line, a log or a document.** The key (the seed), the Rate My Ride token, a backup password and a recovery code never appear in an argument, an environment variable or the log. Passwords and recovery codes come in on **stdin** (`--password-stdin`, `--code-stdin`; the first line, UTF-8, with or without a byte order mark, at most 1024 bytes). `--password` and `--code` are refused. The tool prints no secret except the recovery code that `recovery show` was asked for, and, as a last resort, a code the nick server handed out that could not be stored (`recovery_code_not_saved`, so that it is not lost). `show` says only *whether* there is a token and a code.
2. **The game must be closed to change anything.** The game reads its key file once and rewrites the whole file on every change it makes, so a second writer while it runs can lose a change; it also rewrites `larecomp.toml` when it exits. Every command that changes a file needs `--game-exe <path of larecomp.exe>` (the exact path the launcher starts the game by), checks that the game does not run when it starts and **again right before each write**, and refuses with `game_running` otherwise. It looks for the marker a running game holds (the launcher's own way: a named mutex on Windows, a lock file on Linux) and, for a game that has none, for a process of that program file. A process of the same name that cannot be inspected counts as running (`unsure`). Reading commands (`show`, `backup`, `recovery show`, `recovery check`, `check` without `--claim`/`--refresh`, `recover`) do not need `--game-exe`.
3. **The nick server is asked only for what the command was told to ask.** Nothing registers a nick unless `--claim` is given (or `rename` is told to claim), and nothing refreshes one unless `--refresh` is. The nick server's limits are 30 requests a minute and 3 **new** nicks a day per IP, and 10 recovery tries an hour per IP and 5 per nick: a command makes one request, never retries, and a recovery code of the wrong shape is not even sent. Call the identity commands only when the player pressed a button, never on a timer.
4. **A recovery code is never overwritten.** `check --claim`, `check --refresh` and `rename --claim` refuse (`recovery_code_unconfirmed`) while a code is waiting to be confirmed, because a registration hands out a new code that would replace it. The key file is rewritten once before the nick server is asked, so a key file that cannot be written is found out *before* a code is given out.
5. **A key is never destroyed.** `restore` over a different key needs `--replace` and keeps the old key file beside the new one (`<id>.key.replaced-<time>`); a key that cannot be read is moved aside (`<id>.key.unreadable`), as the game does. The tool never deletes a key.
6. **Hostile text is text.** Everything a server says (a nick's owner, the master's address) is cleaned like the rest of the tool's text (valid UTF-8, no control characters) and clipped. Show it as text.

## Naming the profile

Every command names a profile one of two ways (and `--identity-dir D` to use another identity folder than the default, `%LOCALAPPDATA%\larecomp\identity` on Windows, `$XDG_DATA_HOME/larecomp/identity` or `~/.local/share/larecomp/identity` elsewhere):

| Way | Meaning |
|---|---|
| `--saves <folder> --gamertag <name>` | The saves folder (`user_data_root`, the folder of `<gamertag>\...`) and the profile's gamertag. The profile's id is read from `<saves>\<gamertag>\profile.xuid`. This is what the launcher knows. |
| `--profile <id>` (with `--gamertag <name>` when a command needs the nick) | The id itself: 16 hex digits, or `name_<gamertag>` for a profile that still has the shared legacy XUID. |

`check`, `recover`, `cancel` and `rename` need the gamertag (the nick that is proven or changed). A key that an older SDK filed by name (`name_<gamertag>.key`) is found through the gamertag (`profile.filed_by` is `"name"`); the game moves it to the profile's id at its next start. An id or gamertag that could name another file is refused (`bad_profile`, `bad_gamertag`).

## Commands

```
larecomp_online_tool identity show                     <profile>
larecomp_online_tool identity check    --server H[:P]  <profile> [--claim | --refresh] [--game-exe EXE]
larecomp_online_tool identity backup   --file F --password-stdin [--overwrite]  <profile>
larecomp_online_tool identity restore  --file F --password-stdin [--replace] --game-exe EXE  <profile>
larecomp_online_tool identity recovery show            <profile>
larecomp_online_tool identity recovery check --code-stdin  <profile>
larecomp_online_tool identity recovery confirm [--code-stdin] --game-exe EXE  <profile>
larecomp_online_tool identity recovery remind (--days N | --never) --game-exe EXE  <profile>
larecomp_online_tool identity recover  --server H[:P] --code-stdin  <profile>
larecomp_online_tool identity cancel   --server H[:P] --game-exe EXE  <profile>
larecomp_online_tool identity rename   --nick NEW --config larecomp.toml --game-exe EXE  <profile with --saves>  [--claim --server H[:P]]
```

`<profile>` is the way of naming it above plus the optional `--identity-dir D` and `--pretty` (indented output). `--server` is a **relay** (`host[:port]`, port 31000 when absent), as in `larecomp.toml`'s `mcla_online_server`: the relay names the nick server its players use, so the tool asks the same one the game would. `--timeout-ms N` (one datagram try, 50 to 10000, default 1000, three tries) and `--master-timeout-ms N` (the nick server's answer, 200 to 60000, default 8000) apply to the commands that talk to a server.

### show

Local and instant; safe while the game runs. The gamertag is whatever `--gamertag` said.

### check

Asks the relay for its challenge (does it check nicks, which nick server does it use) and answers what the game's F8 window shows as "Nick verified", "Nick taken", ...

* No flag: **never asks the nick server and writes nothing.** With a good ticket for this nick already in hand the nick is proven to the relay (`verified`); with none the state is `needs_claim`.
* `--claim`: asks the nick server for a ticket when there is no good one. **This registers the nick if it is free** (a new nick counts against the 3 a day) and stores the ticket and the recovery code. It is the explicit "claim my nick" button; ask the player first.
* `--refresh`: asks the nick server even when the ticket in hand is still good, but only when a ticket for this nick is already on record (so it never registers anything new). This is how "someone is moving your nick" (`status.incoming_until`) is noticed. `needs_claim` when there is none on record.

`--claim` and `--refresh` need `--game-exe` and a closed game (they write the ticket and the code). They refuse with `recovery_code_unconfirmed` while a recovery code waits.

### backup and restore

`backup` writes the key and the Rate My Ride token into a file sealed with the password (Argon2id, 64 MiB, 3 passes, then XChaCha20-Poly1305: the `.lrid` format of the game's F8 window, which opens it and the other way round). The recovery code is **never** in a backup. The file is opened again and compared before it is trusted (`verified`). It does not overwrite a file unless `--overwrite`, and refuses a place inside the identity folder. It only reads, so it works while the game runs. The password (8 bytes at least) is the first line of stdin. A backup is the player's to keep, off this PC.

`restore` puts a backup's key and token in the profile's key file, and the game must be started again to use it (`restart_needed`). If the profile already has a *different* key, nothing happens until `--replace` is given (the error `would_replace_key` carries `current_fingerprint` and `backup_fingerprint` for the confirmation dialog); the old key file is then kept as `previous_kept_as`, the tickets of the old key are forgotten (they name it) and a waiting recovery code is dropped (it belonged to the old key). Restoring the key that is already there changes nothing but the token, and keeps its recovery code and tickets. A backup that has no token does not remove the token the profile has. If the new key file cannot be written the old one is put back.

### recovery

`recovery show` prints the waiting code (the one secret the tool prints; call it only when the player asked to see it). `recovery check --code-stdin` tells whether a typed code has the right shape (`valid_format`: 20 letters and digits, the way the nick server reads it; case, dashes and spaces do not matter; O and I are read as 0 and 1) and whether it matches the waiting one (`matches_pending`, `null` when none waits): "did I write it down right?". `recovery confirm` is the "I wrote it down" button: it forgets the waiting code, which this PC cannot show again; give `--code-stdin` to make the player type it back first (`code_mismatch` leaves everything as it was). `recovery remind` sets when the game's identity window may open by itself again for the code: `--days N`, or `--never` (only from F8).

### recover and cancel

`recover` moves a nick that another key owns to this profile's key with its recovery code (on stdin). The nick server holds the move for a while (the master's default is 168 hours) and the owner can cancel it; the answer is `result: "pending"` and `until` (unix time). After that time a `check --claim` from this profile completes the move (`claim.transferred: true`, a new recovery code is kept). `cancel` is the owner's side: it stops a move someone started with the owner's code; the old code is void and the nick server hands out a new one, which is kept (`recovery_code_pending`).

### rename

"Use another gamertag", with the game's own rules (`identity_rename.cpp`: letters, digits and spaces, up to 15, not a reserved name, not the current one, no save of that name already, the current save folder there). It writes the two things the game's F8 window writes: the save marker (`<saves>\.profile`) and `user_name` in `larecomp.toml` (`--config`); the SDK renames the save folder at the next start and the key stays with the profile id (a key still filed by the gamertag is renamed with it: `key_moved`). The new nick needs its own claim: with `--claim --server H` the tool registers it on that relay's nick server **first** and changes nothing if the nick server says no (`nick_taken`, `rate_limited`, `relay_does_not_check_nicks`, ...); without `--claim` the new nick is claimed when the game first goes online with it.

## The documents

Every document starts with `ok`, `command` (`"identity"`), `action` and `schema` (1). Exit code: **0** `ok` is true, **1** the command could not be done (`ok` is false and `error.code` says why), **2** the command line was wrong (`error.code` is `"usage"`; this document has no `action`). Fields can be added without a schema change: ignore the ones you do not know. All of them below come after those four. A `profile` block (`id`, `gamertag` or `null`, `identity_dir`, `key_file`, `filed_by` `"id"` or `"name"`) is in every document but the errors.

| Action | Fields |
|---|---|
| `show` | `key` {`state` `"ok"`, `"missing"` or `"unreadable"`, `protection` `"windows_dpapi"` (readable by this Windows user only) or `"plain_file"` (mode 0600), `fingerprint` (5 bytes of the public key's hash, hex), `xuid` (the XUID the key gives), `rmr_token` (whether there is one), `unreadable_copy`}, `recovery` {`pending`, `remind_after`: 0 = the game may remind now, -1 = never, else a unix time}, `tickets` [{`master` (fingerprint), `nick`, `expires`, `valid`, `for_this_key`}] |
| `check` | `server` {`relay`, `master` (or `null`)}, `status` {`state`: `verified` `not_checked` (the relay checks no nicks) `no_answer` `taken` `claim_pending` `master_down` `failed` `needs_claim`; `detail` (a sentence); `master_status` (the nick server's own word: `ok`, `taken`, `rate_limited`, `bad_clock`, `down`, ...; empty when it was not asked); `owner_nick`; `claim_until`; `incoming_until` (someone is moving the nick at this unix time; 0 = no)}, `claim` {`mode` `never`/`refresh`/`always`, `asked_master`, `ticket_stored`, `transferred`, `recovery_code_pending`} |
| `backup` | `file`, `bytes`, `verified`, `fingerprint`, `includes` {`key`, `rmr_token`, `recovery_code` (always false)} |
| `restore` | `file`, `fingerprint`, `xuid`, `previous_fingerprint`, `previous_kept_as` (both `null` when nothing was replaced), `restart_needed` |
| `recovery_show` | `recovery` {`pending`, `code` (or `null`), `remind_after`} |
| `recovery_check` | `recovery` {`valid_format`, `matches_pending`} |
| `recovery_confirm` | `confirmed` |
| `recovery_remind` | `remind_after` |
| `recover` | `master`, `result` (`"pending"`), `until` |
| `cancel` | `master`, `result` (`"cancelled"`), `recovery_code_pending` |
| `rename` | `from`, `to`, `claimed`, `key_moved` (`null` unless a key filed by name was renamed with it), `restart_needed` |

Errors: `"error": {"code": ..., "message": ...}` (plus the fields named below). The message is a plain sentence a launcher may show.

| `error.code` | When |
|---|---|
| `usage` | The command line (exit 2). |
| `bad_profile`, `bad_gamertag`, `bad_server` | An id, gamertag or server that cannot be used. |
| `profile_not_found` | No `profile.xuid` in the gamertag's save folder: the game has not made the profile yet. |
| `no_key`, `key_unreadable` | The profile has no key yet; or its key file cannot be read by this user on this PC, or is damaged (restore a backup). |
| `game_running` | A writing command while the game runs (`running`, `unsure`, `how` `"marker"` or `"process"`). |
| `secret_missing`, `secret_too_long` | Nothing, or more than 1024 bytes, on stdin. |
| `password_too_short`, `wrong_password`, `not_a_backup`, `bad_file`, `file_exists`, `cannot_write`, `backup_verify_failed`, `cannot_keep_old_key` | Backup and restore. |
| `would_replace_key` | `restore` over another key without `--replace` (`current_fingerprint`, `backup_fingerprint`). |
| `no_pending_code`, `code_mismatch`, `bad_code_format` | Recovery codes. |
| `recovery_code_unconfirmed` | A claim that would replace a code that was never confirmed. |
| `store_not_writable` | The key file cannot be written; the nick server was not asked. |
| `recovery_code_not_saved` | The nick server handed out a recovery code and it could not be stored (the game started meanwhile, or the disk refused): the document carries `recovery_code`. **Show it to the player to write down at once**: it is not given out again. |
| `relay_unresolved`, `relay_no_answer`, `relay_does_not_check_nicks` | The relay could not be found, does not answer nick checks, or checks no nicks (so there is no nick server to ask). |
| `master_unreachable`, `master_refused`, `bad_clock`, `rate_limited` | The nick server does not answer; answered something the tool does not know; the PC's clock is more than 10 minutes off (`message` says by how much); or it limits this PC for now (`master_status`). |
| `bad_code`, `unknown_nick`, `already_yours`, `not_owner`, `nothing_pending`, `nick_taken`, `claim_pending`, `bad_nick` | The nick server's answers to `recover`, `cancel` and the claim of `rename` (`master`, `master_status`). |
| `nick_not_allowed` | `rename`: the game's sentence for why the name cannot be used. |
| `marker_not_written`, `config_not_written` | `rename` could not write the save marker or `larecomp.toml`. |

## For the launcher

* Start it with `ProcessStartInfo.ArgumentList`, `RedirectStandardInput = true` and `StandardInputEncoding = new UTF8Encoding(false)`; write the one line, close stdin, read stdout to the end. A call that does not talk to a server is instant; one that does takes under 10 seconds unless a server hangs (give the process 30 seconds).
* The account page's secret handling: keep a password or a recovery code in memory only for the dialog, never in a log or in the diagnostics text (the identity documents hold no secret, but `recovery show` and `recovery_code_not_saved` carry a code).
* The closed-game rule is checked by the tool again, but the launcher should disable the writing buttons while the game runs ("nothing changes while the game runs"), as on the Mods page.
* On Linux the key is a plain file with mode 0600 and the game's marker is a lock file; online play itself is Windows only today, but the commands run.
* `--clock-offset-sec N` shifts the time a request to the nick server carries. It exists for the checks of the nick server's clock window; a launcher never passes it.

## The checks

`cmake --build <build dir> --target larecomp_online_tool_identity_test` and run it (exit 0 = passed). It needs no game and no real server: keys, backups and profiles are made in scratch folders; the only network is 127.0.0.1, where the **real** nick registry (`master_server/identity_registry.cpp`) answers behind a small listener and a fake relay does the nick check like `larecomp_relay_server`. It pins the key file and backup file formats (including a golden backup made by the code as it was before the identity code was split), the closed-game rule, that no document holds a secret, and every command: claim, taken, recover, cancel, the hold and the move, rename, the 3-new-nicks-a-day limit, the recovery tries limit, a clock that is wrong, a nick server that is down, a relay that checks no nicks, a game that starts while the nick server answers.

`--target larecomp_identity_adapter_test` compiles the game's own `identity_store.cpp` and `identity_client.cpp` without the SDK (against stand-ins in `identity/test/stubs`) and exercises them through their public API; `identity/test/identity_adapter_compare.ps1` runs it against the code before the split and after and compares what each prints and logs: this is the check that the game behaves as it did.
