#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class AudioSettings final : public rex::SettingOwner {
 public:
  AudioSettings() : SettingOwner("larecomp") {}

  REX_SETTING(i32, audio_mixer_priority, -1, "Host priority of the game's two audio threads: the guest thread that mixes each 5.33 ms "
    "frame (the game's request for a higher priority is ignored) and the SDK's audio worker "
    "that calls the game for each frame: 0 = normal, 1 = above normal, 2 = highest, 3 = highest "
    "and registered with the Windows multimedia scheduler (\"Pro Audio\"), -1 = automatic: 3 on "
    "a PC with 6 or fewer logical processors, 0 otherwise. On Linux 1 to 3 raise the threads' "
    "nice value if the system allows it (it usually does not for an ordinary user). Takes "
    "effect at the next start.", rex::Range{-1, 3}, rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, custom_music, false, "Old host player: decodes <exe>/music/*.mp3 outside the game and plays them "
    "over it, with the guest radio carrying only the row. Off, because the "
    "native route (mc_engine/music) turns the same files into real game "
    "objects, real cues and real streaming wave banks -- the radio then behaves "
    "exactly as it does for a shipped song. This only ever sees a file the "
    "native route could not take, and playing that one a second-class way hides "
    "the reason instead of showing it.", rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, custom_music_announce, true, "Print the song name on the HUD info strip (the bar under the speedometer "
    "that normally carries the district name) whenever the radio moves to a new "
    "track. Uses the game's own message channel, so it looks like any other line "
    "the game puts there; applies to the shipped radio as well as custom MP3s.");

  REX_SETTING(f64, custom_music_announce_seconds, 15.0, "How long the song name stays on the HUD info strip. The game's own uses of "
    "that channel are short (3 s for MUSIC OFF, 5 s for a district name), but a "
    "track title is worth reading, so this defaults longer. Anything the game "
    "itself puts on the strip replaces it before the time is up.", rex::Range{1.0, 60.0});

  REX_SETTING(f64, custom_music_cue_gain_db, 4.0, "Gain for native custom music in the game's audio mixer, after the bank's "
    "3 dB MP3 decode headroom. Shipped songs keep their original gain.", rex::Range{0.0, 6.0});

  REX_SETTING(f64, custom_music_volume, 0.5, "Trim applied on top of the in-game music slider for custom MP3 tracks. "
    "The shipped radio goes through the RAGE mixer and its ducking curves, "
    "which the host player does not, so 1.0 comes out noticeably louder.", rex::kDebugOnly);

  REX_SETTING(std::string, mcla_audio_output, "auto", "auto: the default device, or silence when none opens. null: always silent.", rex::kDebugOnly, rex::kRequiresRestart);

  REX_SETTING(bool, music_bank_log, false, "Log every streaming wave bank the audio engine opens: the bank id (an index "
    "into the sounds.dat string table) and whether the file was found. Use it "
    "when a song plays silently -- it separates 'the bank was never asked for' "
    "from 'the bank was opened and decoded to nothing'.", rex::kDebugOnly);

  REX_SETTING(bool, music_list, false, "Log the whole radio once, right after mcMusicManager finishes building it: "
    "every genre bucket with each song's hash code and title. This is how you "
    "tell whether a song added through the audio metadata (a rebuilt game.dat "
    "plus its wave bank) actually reached AddSong, since the game's own log "
    "line for that was compiled out.", rex::kRequiresRestart, rex::kDebugOnly);

};

inline AudioSettings g_audio;

inline auto& audio_mixer_priority = g_audio.audio_mixer_priority;
inline auto& custom_music = g_audio.custom_music;
inline auto& custom_music_announce = g_audio.custom_music_announce;
inline auto& custom_music_announce_seconds = g_audio.custom_music_announce_seconds;
inline auto& custom_music_cue_gain_db = g_audio.custom_music_cue_gain_db;
inline auto& custom_music_volume = g_audio.custom_music_volume;
inline auto& mcla_audio_output = g_audio.mcla_audio_output;
inline auto& music_bank_log = g_audio.music_bank_log;
inline auto& music_list = g_audio.music_list;

}  // namespace larecomp::settings
