#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class DifficultySettings final : public rex::SettingOwner {
 public:
  DifficultySettings() : SettingOwner("larecomp") {}

  // The pause menu's AI DIFFICULTY row, in percent like Midnight Club 3's: the 11
  // tune levels spread over 0..100, so each 10% is one level. -1 = the game's own
  // choice. rubberband_level (the raw 0..10 index) wins when it is set.
  REX_SETTING(f64, ai_difficulty, -1.0, "AI difficulty in percent: 0 = easiest, 100 = the game's hardest, each 10% one "
    "RubberBandTune level. -1 leaves the game's own choice. "
    "Ignored while rubberband_level is set.", rex::Range{-1.0, 100.0});

  // ── Difficulty preset: the launch build's rules ─────────────────────────────
  //
  // The disc build of December 2008 (title ID 545407F2, no title update) and the
  // Complete Edition larecomp runs ship the same eleven RubberBandTune files and
  // the same AI brain (mcBrainRacing, the throttle in sub_822BCFE0, the nitrous
  // and shortcut tests in sub_822BE670 all match instruction for instruction).
  // What the title update "improved AI balance" changed sits in mcRubberBandMgr,
  // read side by side in both XEXs:
  //
  //  1. LoadTuning (sub_82743A10). The launch build uses the files as parsed. The
  //     Complete Edition then doubles UseNitroMinDistFromFinish (+48) and
  //     HomeStretchDistance (+64), multiplies UnlimitedNitroMinDistBehindPlayer
  //     (+56) by 2.5 and moves BehindDistOffsets[2..7] down into [1..6]. The AI
  //     stops spending nitrous further from the line, is held to
  //     HomeStretchBehindMaxThrottle over twice the run-in, and needs to be 2.5x
  //     further behind before nitrous turns unlimited.
  //  2. The race-result step (sub_82742F28): a win adds points to mgr+64, 18 of
  //     them raise the base level mgr+56 by one, up to a ceiling picked by the
  //     race's tier (0..3); a loss takes points away down to a floor. The launch
  //     ceilings are 5, 6, 8, 10 and the floors 0, 2, 4, 5; the Complete Edition
  //     has 4, 7, 8, 10 and 0, 1, 4, 5.
  //  3. The base level a new career starts from: 3 in the launch constructor,
  //     0 in the Complete Edition's (sub_82742DF8). mgr+56 is stored in mc4.sav,
  //     so a save keeps whatever it had reached.
  //  4. The script native RubberBandMgr_SetDifficulty wrote mgr+56 and mgr+60 in
  //     the launch build and is nullsub_1 in the Complete Edition, whose career
  //     script no longer makes the call that restored the base to 3 after the
  //     hookman race that drops it to 0. Not reproduced: reviving the native with
  //     the newer scripts would leave the base at 0.
  //
  // "original" reproduces 1-3: the raw parsed tune goes back into the table, the
  // race-result step runs with the launch ceilings and floors, and a base below 3
  // is lifted to 3 at the first race set up after the game starts or the preset
  // is switched on (the launch build's starting point; a career that has climbed
  // past 3 is left where it is). "complete" is the game unchanged.
  REX_SETTING(std::string, difficulty_preset, "complete", "Whose AI rules races use: complete = the Complete Edition's, as shipped; original = the "
    "launch build's (December 2008), before the title update that eased it: the AI keeps its "
    "nitrous closer to the finish, the home-stretch hold-back is shorter, and the career "
    "difficulty starts at 3 and climbs and falls within the launch limits.", rex::Allowed("complete", "original"));

  REX_SETTING(f64, rubberband_catchup_scale, 1.0, "Scales how hard the AI pulls when it is BEHIND: MaxThrottle, BehindMaxThrottle, "
    "HomeStretchBehindMaxThrottle, PlayerCloseMaxThrottle. Below 1 = less catch-up.", rex::Range{0.0, 2.0});

  REX_SETTING(f64, rubberband_distance_scale, 1.0, "Scales the distance bands the rubberband reacts within: MinDist, MaxDist, "
    "BehindMinDist, BehindMaxDist. Larger = the band engages from further away.", rex::Range{0.1, 4.0});

  // ── AI rubberband scales ────────────────────────────────────────────────────
  // Applied on top of whatever $/tune/career/RubberBandTune00..10 loaded, to all
  // 11 entries. 1.0 leaves the tune exactly as shipped.
  REX_SETTING(bool, rubberband_dump, false, "Write every RubberBandTune entry to <exe>/rubberband_dump.txt and flip back off. "
    "Read it before picking the scales below.", rex::kDebugOnly);

  REX_SETTING(f64, rubberband_holdback_scale, 1.0, "Scales MinThrottle, the throttle cap while the AI is AHEAD. Below 1 = the leader "
    "backs off harder; above 1 = it runs away more freely.", rex::Range{0.0, 2.0});

  // The 11 RubberBandTune entries are difficulty levels. sub_82743B30
  // (RubberBandMgr_SetupRace) picks one into mgr+60: it starts from the race's
  // base (mgr+56) and then, when the race got clipped down to the racer's own
  // level, subtracts mgr+68 (2) — or adds mgr+72 (3) when the racer is below what
  // the race asked for. The game's own `rubberband` dev switch (node 0x8290AC10)
  // overrides that index outright, clamped 0..10, which is what this reproduces —
  // written every frame so it also survives the restore path at the end of setup.
  REX_SETTING(i32, rubberband_level, -1, "Force which RubberBandTune level the AI races at: -1 leaves the game's own choice, "
    "0 = easiest, 10 = hardest. This is the direct way to make races harder — the tune "
    "index the game normally derives from your racer level and the race's base.", rex::Range{-1, 10}, rex::kDebugOnly);

  REX_SETTING(f64, rubberband_nitro_behind_scale, 1.0, "Scales UseNitroMinDistBehindPlayer and UnlimitedNitroMinDistBehindPlayer — how far "
    "behind you the AI has to be before it spends nitrous, and before it gets unlimited.", rex::Range{0.0, 8.0});

  REX_SETTING(f64, rubberband_nitro_finish_scale, 1.0, "Scales UseNitroMinDistFromFinish, the gate tied to the run-in to the finish.", rex::Range{0.0, 8.0});

  REX_SETTING(f64, rubberband_nitro_start_dist, -1.0, "UseNitroMinDistFromStart in world units: how far into the race the AI must be "
    "before it may touch nitrous. Stock is 100 at every difficulty level, which is why "
    "they empty the bottle off the line. -1 keeps stock.", rex::Range{-1.0, 8000.0});

  // AI nitrous policy. The tune gates nitrous on four distances; raising them
  // makes the AI hold it instead of dumping the bottle off the line.
  //
  // Direction of each comparison is NOT confirmed by RE — the names and offsets
  // are (parser registration sub_823990E0), and LoadTuning rescales two of them
  // (+48 *= 2, +56 *= 2.5) after parsing. Read rubberband_dump.txt for the real
  // numbers and try one knob at a time.
  // Absolute, not a scale: measured across all 11 levels, UseNitroMinDistFromStart
  // is a flat 100 at every one of them — the only field in the struct with no
  // difficulty ramp. Scaling a constant tells you nothing about what you asked
  // for, and 100 is short enough that even x8 lands inside the opening straight.
  // Checked in IDA: the AI brain (sub_822BE670) takes a shorter alt path of the
  // checkpoint list when its distance to the player is below -DistBehindBefore-
  // TakingShortcuts (tune +40, racer+452). So the difficulty level DOES reach the
  // checkpoints, through the shortcuts: a larger scale makes the AI wait until it
  // is further behind before cutting, a smaller one makes it cut earlier.
  REX_SETTING(f64, rubberband_shortcut_scale, 1.0, "Scales DistBehindBeforeTakingShortcuts: how far behind you the AI must fall before "
    "it takes a track's alternate (shortcut) path. 0 = always takes them, above 1 = later.", rex::Range{0.0, 8.0});

};

inline DifficultySettings g_difficulty;

inline auto& ai_difficulty = g_difficulty.ai_difficulty;
inline auto& difficulty_preset = g_difficulty.difficulty_preset;
inline auto& rubberband_catchup_scale = g_difficulty.rubberband_catchup_scale;
inline auto& rubberband_distance_scale = g_difficulty.rubberband_distance_scale;
inline auto& rubberband_dump = g_difficulty.rubberband_dump;
inline auto& rubberband_holdback_scale = g_difficulty.rubberband_holdback_scale;
inline auto& rubberband_level = g_difficulty.rubberband_level;
inline auto& rubberband_nitro_behind_scale = g_difficulty.rubberband_nitro_behind_scale;
inline auto& rubberband_nitro_finish_scale = g_difficulty.rubberband_nitro_finish_scale;
inline auto& rubberband_nitro_start_dist = g_difficulty.rubberband_nitro_start_dist;
inline auto& rubberband_shortcut_scale = g_difficulty.rubberband_shortcut_scale;

}  // namespace larecomp::settings
