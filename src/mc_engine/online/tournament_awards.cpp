#include "tournament_awards.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "leaderboards/lb_store.h"

namespace tournament {
namespace {

namespace fs = std::filesystem;

constexpr char kWeeksFile[] = "tournament_weeks.txt";
constexpr char kAwardsFile[] = "tournament_awards.txt";

// Write to a temporary name, then swap it in, so a crash never leaves half a file.
bool WriteAtomic(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
  return !ec;
}

std::string Hex64(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016" PRIX64, v);
  return buf;
}

}  // namespace

Awards::Awards(fs::path dir) : dir_(std::move(dir)) {}

void Awards::Load() {
  if (loaded_) return;
  loaded_ = true;
  {
    std::ifstream in(dir_ / kWeeksFile);
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream ss(line);
      Week w;
      int settled = 0;
      if (ss >> w.week >> w.slot >> w.starts >> w.ends >> w.prize_first >> w.prize_podium >>
          w.podium >> settled) {
        w.settled = settled != 0;
        weeks_[w.week] = w;
      }
    }
  }
  {
    std::ifstream in(dir_ / kAwardsFile);
    std::string owner_hex;
    uint32_t mask = 0;
    while (in >> owner_hex >> mask) {
      const uint64_t owner = std::strtoull(owner_hex.c_str(), nullptr, 16);
      if (owner) awards_[owner] |= mask & kRewardsKnown;
    }
  }
}

bool Awards::SaveWeeks() const {
  std::string text;
  for (const auto& [n, w] : weeks_) {
    text += std::to_string(w.week) + " " + std::to_string(w.slot) + " " +
            std::to_string(w.starts) + " " + std::to_string(w.ends) + " " +
            std::to_string(w.prize_first) + " " + std::to_string(w.prize_podium) + " " +
            std::to_string(w.podium) + " " + (w.settled ? "1" : "0") + "\n";
  }
  return WriteAtomic(dir_ / kWeeksFile, text);
}

bool Awards::SaveAwards() const {
  std::string text;
  for (const auto& [owner, mask] : awards_) text += Hex64(owner) + " " + std::to_string(mask) + "\n";
  return WriteAtomic(dir_ / kAwardsFile, text);
}

void Awards::NoteServed(const Definition& d) {
  Load();
  auto it = weeks_.find(d.week);
  if (it != weeks_.end() && !it->second.settled && it->second.slot == d.table_id &&
      it->second.prize_first == d.prize_first && it->second.prize_podium == d.prize_podium &&
      it->second.podium == d.podium)
    return;
  if (it != weeks_.end() && it->second.settled) return;  // a closed week never changes
  Week w;
  w.week = d.week;
  w.slot = d.table_id;
  w.starts = d.starts;
  w.ends = d.ends;
  w.prize_first = d.prize_first;
  w.prize_podium = d.prize_podium;
  w.podium = d.podium;
  weeks_[d.week] = w;
  SaveWeeks();
  // Times left in this slot from an earlier week are not part of this one.
  if (boards_) boards_->DropWrittenBefore(d.table_id, d.starts);
}

void Awards::Settle(int64_t now, std::string* log) {
  if (!boards_) return;
  bool weeks_changed = false, awards_changed = false;
  for (auto& [n, w] : weeks_) {
    if (w.settled || now < w.ends) continue;
    const std::vector<uint64_t> top =
        boards_->TopWrittenBetween(w.slot, w.podium ? w.podium : 1, w.starts, w.ends);
    for (size_t i = 0; i < top.size(); ++i) {
      const uint32_t prize = i == 0 ? w.prize_first : w.prize_podium;
      if (!prize) continue;
      uint32_t& mask = awards_[top[i]];
      if ((mask | prize) != mask) awards_changed = true;
      mask |= prize;
    }
    w.settled = true;
    weeks_changed = true;
    if (log) {
      *log += " settled week " + std::to_string(w.week) + " (slot " + std::to_string(w.slot) +
              ", " + std::to_string(top.size()) + " winner(s))";
    }
  }
  if (weeks_changed) SaveWeeks();
  if (awards_changed) SaveAwards();
}

uint32_t Awards::RewardsFor(uint64_t owner, int64_t now, std::string* log) {
  Load();
  Settle(now, log);
  auto it = awards_.find(owner);
  return it == awards_.end() ? 0 : it->second;
}

}  // namespace tournament
