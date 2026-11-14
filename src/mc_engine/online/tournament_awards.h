#pragma once
//
// Online Tournament prizes: who won what, kept next to the Rate My Ride data.
//
// Shared by the game (local mode) and larecomp_rmr_server; no SDK.
//
//   tournament_weeks.txt   one line per tournament served:
//                          week slot starts ends prize_first prize_podium podium settled
//   tournament_awards.txt  one line per player: owner key (hex) and reward mask
//
// A week is settled the first time anyone asks for rewards after it ended: the
// best `podium` players of its leaderboard slot, among rows written during that
// week, get the prizes; first place the first-place prize. Rewards only ever
// add up, so a player keeps what they won.
//
// Not thread-safe: the Rate My Ride store calls it under its own lock.
//

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "tournament_schedule.h"

namespace lb {
class Store;
}

namespace tournament {

class Awards {
 public:
  explicit Awards(std::filesystem::path dir);

  void Attach(lb::Store* boards) { boards_ = boards; }

  // The tournament being served: remembered for settling later, and its slot
  // emptied of rows left from an earlier round of the rotation.
  void NoteServed(const Definition& d);

  // Settles the weeks that have ended, then returns `owner`'s reward mask.
  uint32_t RewardsFor(uint64_t owner, int64_t now, std::string* log);

 private:
  struct Week {
    uint32_t week = 0;
    uint32_t slot = 0;
    int64_t starts = 0;
    int64_t ends = 0;
    uint32_t prize_first = 0;
    uint32_t prize_podium = 0;
    uint32_t podium = 0;
    bool settled = false;
  };

  void Load();
  bool SaveWeeks() const;
  bool SaveAwards() const;
  void Settle(int64_t now, std::string* log);

  std::filesystem::path dir_;
  lb::Store* boards_ = nullptr;
  bool loaded_ = false;
  std::map<uint32_t, Week> weeks_;
  std::map<uint64_t, uint32_t> awards_;
};

}  // namespace tournament
