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

// File ids handed to the game carry 'T' in the top byte, the week number below.
inline constexpr uint32_t kFileIdTag = 0x54000000;
inline constexpr uint32_t kFileIdTagMask = 0xFF000000;
inline bool IsTournamentFileId(uint32_t id) { return (id & kFileIdTagMask) == kFileIdTag; }

struct Definition {
  uint32_t week = 0;          // tournament number (weeks since 1970-01-05, UTC)
  uint32_t version = 1;
  uint32_t table_id = 13;
  uint32_t restriction_type = 2;
  uint32_t restriction_data = kAnyCar;
  uint32_t race_key = 0;
  std::string name;
  int64_t ends = 0;           // unix seconds the week rolls over
};

// The tournament for `now`: the next race of the rotation and the next
// leaderboard slot each week. A file `tournament.txt` in `dir` (one key=value
// per line: race, slot, restriction_type, restriction_data, name; '#' starts a
// comment) overrides any field for whoever hosts the server.
Definition Current(int64_t now, const std::filesystem::path& dir);

// The 1556-byte file as the game reads it.
std::vector<uint8_t> EncodeGuest(const Definition& d);

}  // namespace tournament
