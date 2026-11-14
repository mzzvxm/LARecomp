#pragma once
//
// Online Tournament: which tournament is running, and the definition file the
// game downloads for it.
//
// Shared by the game (local mode) and larecomp_rmr_server, so nothing here may
// depend on the rexglue SDK.
//
// A tournament in MCLA is one of the city's regular races, run against the
// clock with an optional car restriction; each finished run posts its time to a
// leaderboard slot reserved for tournaments. The game fetches the definition
// from the rlUpload table "tournament" (ReadRecordsByGamer, then DownloadFile of
// the record's file) and keeps the 1556-byte file at the menu object +48:
//
//   +0   u32  version
//   +4   u32  table id: the tournament leaderboard slot, 13..39 or 351..381
//             (the game maps 351.. to stats views 382..412)
//   +8   u32  restriction type: 0 car model, 1 the car's +4 field, 2 car class
//   +12  u32  restriction data (0xFFFFFFFF = any car)
//   +16  u32  race key: the race's entry in the city's race map (race+148)
//   +20  char description[6][256], "xx:name" per language; the menu falls back
//             to the first entry past its "xx:" prefix
//
// All big-endian, as the game reads it.
//

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tournament {

inline constexpr uint32_t kDefinitionSize = 1556;
inline constexpr uint32_t kDescriptionSize = 1536;
inline constexpr uint32_t kAnyCar = 0xFFFFFFFFu;

// File ids handed to the game carry 'T' (tournament definition) or 'W' (the
// player's rewards file) in the top byte.
inline constexpr uint32_t kFileIdTag = 0x54000000;
inline constexpr uint32_t kRewardsFileIdTag = 0x57000000;
inline constexpr uint32_t kFileIdTagMask = 0xFF000000;
inline bool IsTournamentFileId(uint32_t id) { return (id & kFileIdTagMask) == kFileIdTag; }
inline bool IsRewardsFileId(uint32_t id) { return (id & kFileIdTagMask) == kRewardsFileIdTag; }

// Prizes. The game's rlUpload table "Rewards" holds, per player, a 38-byte
// file: "001\0" (the u32 0x30303100 sub_822D4E00 checks) then eight big-endian
// u32; each non-zero one sets the career
// stat tREW[i] (sub_822D1260), which the game reads as:
//   tREW[0]  the 2008 Audi R8 (vp_aud_r8_08, PortalRewardIdx 0 in vehicle.lst)
//   tREW[1]  the TIS rims in the garage (rim_tis_m, sub_826A49F8)
//   tREW[2]  the hydraulics menu in the garage without the South Central
//            download (sub_826AB520)
// tREW[3..7] are read nowhere. Rewards are bits of a mask, bit i = tREW[i].
inline constexpr uint32_t kRewardsFileSize = 38;
inline constexpr uint32_t kRewardAudiR8 = 1u << 0;
inline constexpr uint32_t kRewardTisRims = 1u << 1;
inline constexpr uint32_t kRewardHydraulics = 1u << 2;
inline constexpr uint32_t kRewardsKnown = kRewardAudiR8 | kRewardTisRims | kRewardHydraulics;

// Who wins what when a week closes: first place, and second to `podium`.
inline constexpr uint32_t kDefaultPrizeFirst = kRewardAudiR8 | kRewardTisRims | kRewardHydraulics;
inline constexpr uint32_t kDefaultPrizePodium = kRewardTisRims | kRewardHydraulics;
inline constexpr uint32_t kDefaultPodium = 3;

struct Definition {
  uint32_t week = 0;          // tournament number (weeks since 1970-01-05, UTC)
  uint32_t version = 1;
  uint32_t table_id = 13;
  uint32_t restriction_type = 2;
  uint32_t restriction_data = kAnyCar;
  uint32_t race_key = 0;
  std::string name;           // the race
  std::string description;    // what the menu shows; built from the rest if empty
  uint32_t prize_first = kDefaultPrizeFirst;
  uint32_t prize_podium = kDefaultPrizePodium;
  uint32_t podium = kDefaultPodium;
  int64_t starts = 0;         // unix seconds the week began
  int64_t ends = 0;           // unix seconds the week rolls over
};

// The tournament for `now`: the next race of the rotation and the next
// leaderboard slot each week. A file `tournament.txt` in `dir` (one key=value
// per line: race, slot, restriction_type, restriction_data, name, description,
// prize_first, prize_podium, podium; '#' starts a comment) overrides any field
// for whoever hosts the server.
Definition Current(int64_t now, const std::filesystem::path& dir);

// The 1556-byte file as the game reads it.
std::vector<uint8_t> EncodeGuest(const Definition& d);

// "Audi R8 + TIS rims + hydraulics" for a reward mask ("" for none).
std::string RewardNames(uint32_t mask);

// The player's rewards file for `mask` (38 bytes).
std::vector<uint8_t> EncodeRewardsFile(uint32_t mask);

}  // namespace tournament
