# Custom Music

Put MP3 files in a folder and they appear on the in-game radio. Each file is turned into a real game song: a game object, a cue and a streaming wave bank, built the way the shipped songs are. Source: `src/mc_engine/music/` (native route) and `src/mc_engine/mp3custom/` (older host player).

No cvar is needed for this. The native route described here is always active: it runs at startup whenever it finds `.mp3` files in the folders below. Do not set `custom_music = true`; that switches on the older host player (see Options), which plays over the game instead of through the radio and is off by default.

## Quick start

1. Create a `music/` folder beside the executable and drop `.mp3` files in it.
2. Name each file `Artist - Title.mp3`.
3. Start the game. Banks are built at startup; the first start with new files takes longer.

A second folder is also scanned: `Rockstar Games/LARecomp/User Music` under the user's Documents folder (`MusicFolders` in `src/mc_engine/music/custom_music.cpp`). Folders that do not exist are skipped.

The first time a scanned folder is found without a manifest, a `manifest.txt` is written into it whose lines are all comments, so it changes nothing until edited.

## Format and limits

| Item | Rule | Source |
|---|---|---|
| Container | Plain `.mp3`, extension compared case-insensitively. Other files in the folder are ignored by this route. | `custom_music.cpp` |
| Codec | MPEG-1 Layer III only, at 32, 44.1 or 48 kHz. Frames are copied untouched; no re-encode. MPEG-2 and 2.5 and other rates are refused with `only MPEG-1 at 32, 44.1 or 48 kHz plays; re-encode the file`. | `wave_bank.cpp` |
| Not an MP3 | `no MPEG Layer III frames (only plain MP3 is supported)`. | `wave_bank.cpp` |
| Length | Capped by a wave-bank header budget, not by file size. A track that does not fit is skipped with `track is N s but only about M s of it fit in a header budget of B bytes at this bitrate`. The written manifest template says roughly 5 minutes at 128 kbps or 2.5 minutes at 320 kbps. The budget is raised above the stock value only when the wave slot table can be patched within a measured memory pool; otherwise the stock budget stays and `[music] ... tracks stay inside the stock N byte header` is logged. | `wave_bank.cpp`, `custom_music.cpp` |
| Bank name | Derived from the file name. Two tracks cannot share one; the second is skipped with `another track already claims the bank name`. Set `bank` in the manifest to change it. | `custom_music.cpp` |
| Songs per genre list | A group holds at most 255 songs (a count stored in one byte). Overflow is listed in another genre's group and still plays under its own genre. | `custom_music.cpp` |
| Total songs | 7 genres x 255 = 1785. Beyond that: `the radio is full at N songs -- M track(s) skipped`. | `custom_music.cpp` |
| Archives | Banks go into `xarchive_music_NN.rpf`, at most four archives. | `modloader.cpp` |

## Naming and manifest

The file name is read as `Artist - Title`. With no ` - ` the whole name is the title. The radio shows the file name (without extension) unless `display` is set.

`manifest.txt` (or `music.txt`) in the same folder overrides names. Format, from `ReadManifest`:

- `key = value` lines. Blank lines and lines starting with `#` or `;` are comments. A `#` or `;` inside a value starts an inline comment.
- Lines before the first section are defaults for every file in the folder.
- A section `[File Name.mp3]` (matched case-insensitively on the file name) overrides the defaults for that file.
- Every key except `display` is a single word; text after the first space is ignored.

| Key | Meaning |
|---|---|
| `genre` | One of `ECLECTIC`, `ELECTRONIC`, `HARDROCK`, `HIPHOP`, `ROCK`, `TECHNO`, `WEST_RAP`. Default `ECLECTIC`. An unknown name falls back to `ECLECTIC`. |
| `display` | Text shown by the radio. |
| `name` | Internal name of the radio entry. Default `<GENRE>_<Artist>_<Title>`, sanitised. |
| `bank` | File name of the bank inside the archive. Default derived from artist and title, lower-cased. |
| `title` | String-table key behind `display`. Default `Music_<Genre>_<Artist>_<Title>`. |

```text
# defaults for every file here
genre = ROCK

[Some Band - A Song.mp3]
genre   = HIPHOP
display = Some Band - A Song
```

## Radio integration

- Songs join the radio manager's list (`MUSIC_0_MANAGER`) in their genre. They are selectable like shipped songs.
- A song's display name is a string-table key; a key missing from the table would render as `!!Key!!`. Native tracks register their titles automatically (`[mp3custom] N title(s) declared for metadata songs` in the log).
- `custom_music_announce` (default `true`) prints the song name on the HUD info strip when the radio changes track; `custom_music_announce_seconds` (default `15.0`, range 1 to 60) sets how long. These apply to the shipped radio too.
- `custom_music_cue_gain_db` (default `4.0`, range 0 to 6) is extra gain in the game mixer for native custom tracks, applied after the bank's 3 dB MP3 decode headroom.

For a hand-made mod that adds songs through game metadata instead, a `music_titles.txt` in the mod folder maps string-table keys to text, one `Key = Text` line each (`src/mc_engine/mp3custom/mp3custom.cpp`). Titles longer than 100 characters are cut.

## Cache

Built banks live in `<mod folder>/.custommusic/`, keyed by each source file's path, size and write time (`cache.txt`). A start where nothing changed rebuilds nothing. Delete the folder to force a full rebuild. Original game files are not modified; the banks ride in the mod archive set (see [Mod Loader](Mod-Loader)).

## Options

| Option | Default | Notes |
|---|---|---|
| `custom_music` | `false` | Not needed for custom music. Older, second-class host player: decodes `music/*.mp3` outside the game and plays over it. Off by default. It only sees files the native route could not take. Restart. |
| `custom_music_volume` | `0.5` | Trim for the host player on top of the in-game music slider. |
| `custom_music_announce` | `true` | See above. |
| `custom_music_announce_seconds` | `15.0` | See above. |
| `custom_music_cue_gain_db` | `4.0` | See above. |
| `music_bank_log` | `false` | Logs each streaming wave bank the audio engine opens, with whether it was found. Use when a song plays silently. |
| `music_list` | `false` | Logs the whole radio once after the manager builds it. |
| `mcla_audio_output` | `auto` | `auto` uses the default device (silence if none opens); `null` is always silent. |
| `audio_mixer_priority` | `-1` | Host priority of the game's two audio threads. Range -1 to 3. |

Full rows in [Settings Reference](Settings-Reference#mclaaudio-9).

## Troubleshooting

| Symptom | Cause and log line |
|---|---|
| Track missing from the radio | Look for `[music] <file>: <reason>`. Reasons are in the table above. |
| `[music] N track(s) could not be built and are NOT on the radio` | One or more tracks failed; earlier lines say which. |
| `[music] the shipped game.dat has no MUSIC_0_MANAGER` or `does not walk as seven groups, radio left alone` | The shipped metadata is not what the builder expects; custom music is not installed. |
| `[music] cannot raise all four music slots within the verified audio pool budget` | Long tracks are refused; stock budget stays. |
| Song plays silently | Enable `music_bank_log` and check whether the bank was opened and found. |
| `[music] more than 4 music archives are required; custom music will not be mounted` | Library too large for four archives. |

## Not documented in the source

- An upper bound on the number of files per folder other than the 1785 song limit.
- Behavior with MP3 files carrying variable bit rate beyond the checks above.
