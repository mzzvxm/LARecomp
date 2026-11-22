// Check of the weekly tournament's prize settling (tournament_awards.cpp) and of the slot-to-view rule it depends on
// (StatsViewForSlot in tournament_schedule.h). A program of its own: no game, no ReXGlue, no network and no server. It runs the
// real Awards class against the real leaderboard store (leaderboards/lb_store.cpp) in a temporary folder, writing times the way
// the game does (the rating column is the negated time, aggregated by max; attribute 2 is the time, aggregated by min), and exits
// 0 when every check holds.
//
// What it pins (Mz's tournament server: a week whose slot is 351..381 never produced winners, because the game keeps a slot's
// times in its stats view, slot >= 351 ? slot + 31 : slot, and the server looked in the slot's own board):
//   - StatsViewForSlot for every slot the game accepts (13..39, 351..381): one view per slot, none shared;
//   - a week settles on the game's view for both kinds of slot, and rows in the slot's own board (351..381) win nothing;
//   - a week an older server settled (value 1 in tournament_weeks.txt) on the wrong board, slot from 351, is settled once more on
//     the right one; value 1 on a slot below 351 (the right board) and value 2 (this code's) stay settled;
//   - the weeks file is written with 2 for a settled week, and a settled week is never settled again;
//   - prizes only add up: what an older server gave stays;
//   - serving the next week settles the weeks that ended first, then empties the slot's view (not the slot's own board).
//
//   cmake --build <build dir> --target larecomp_tournament_awards_test     (CMakeLists.txt)
//   <build dir>/larecomp_tournament_awards_test
//
// or, with no build directory at all (from the folder of this file):
//   clang++ -std=c++23 -D_CRT_SECURE_NO_WARNINGS tournament_awards_test.cpp tournament_awards.cpp tournament_schedule.cpp
//           leaderboards/lb_store.cpp leaderboards/lb_protocol.cpp rate_my_ride/rmr_protocol.cpp -o tournament_awards_test.exe

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "leaderboards/lb_protocol.h"
#include "leaderboards/lb_store.h"
#include "rate_my_ride/rmr_protocol.h"
#include "tournament_awards.h"
#include "tournament_schedule.h"

namespace fs = std::filesystem;
using namespace tournament;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckEq(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s (got %llu, want %llu)\n", what.c_str(), static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
  }
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A player is its token; the store derives the owner key from it, as the server does.
rmr::Token TokenOf(uint8_t id) {
  rmr::Token t{};
  t[0] = id;
  t[15] = 0x5A;
  return t;
}
uint64_t OwnerOf(uint8_t id) { return rmr::OwnerKeyFromToken(TokenOf(id)); }

// One finished tournament run as the game writes it (sub_8229D2E0): the negated time is the rating (max), the time itself is
// attribute 2 (min). The store stamps the row with the real clock.
void WriteTime(lb::Store& boards, uint8_t player, uint32_t view, int64_t time_ms) {
  lb::Request req;
  req.op = lb::Op::kWrite;
  req.token = TokenOf(player);
  req.gamertag = "player" + std::to_string(player);
  req.view = view;
  lb::Value rating;
  rating.attr = lb::kAttrRating;
  rating.type = lb::kTypeInt64;
  rating.agg = lb::kAggMax;
  rating.bits = -time_ms;
  lb::Value time;
  time.attr = 2;
  time.type = lb::kTypeInt64;
  time.agg = lb::kAggMin;
  time.bits = time_ms;
  req.columns = {rating, time};
  const lb::Response resp = boards.Handle(req);
  Check(resp.err == lb::kOk, "write of a time to view " + std::to_string(view));
}

uint32_t RowsIn(lb::Store& boards, uint32_t view) {
  lb::Request req;
  req.op = lb::Op::kSize;
  req.token = TokenOf(200);
  req.view = view;
  return boards.Handle(req).total;
}

// The week as the server serves it, around the real clock: the rows the store stamps with the real time fall inside it.
Definition WeekAround(uint32_t week, uint32_t slot, int64_t now) {
  Definition d;
  d.week = week;
  d.table_id = slot;
  d.starts = now - 3600;
  d.ends = now + 3600;
  d.prize_first = kRewardAudiR8;
  d.prize_podium = kRewardTisRims;
  d.podium = 3;
  return d;
}

struct Scratch {
  fs::path root;
  explicit Scratch(const std::string& name)
      : root(fs::temp_directory_path() / ("larecomp_tournament_awards_test_" + name)) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "rmr", ec);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path rmr() const { return root / "rmr"; }
  fs::path lb() const { return root / "lb"; }
};

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void WriteFile(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// The settled flag of every line of tournament_weeks.txt, by week.
std::vector<std::pair<uint32_t, int>> WeeksFile(const fs::path& dir) {
  std::vector<std::pair<uint32_t, int>> out;
  std::istringstream in(ReadFile(dir / "tournament_weeks.txt"));
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    uint32_t week = 0, slot = 0, prize_first = 0, prize_podium = 0, podium = 0;
    int64_t starts = 0, ends = 0;
    int settled = -1;
    if (ss >> week >> slot >> starts >> ends >> prize_first >> prize_podium >> podium >> settled)
      out.emplace_back(week, settled);
  }
  return out;
}

int SettledOf(const std::vector<std::pair<uint32_t, int>>& weeks, uint32_t week) {
  for (const auto& [w, s] : weeks)
    if (w == week) return s;
  return -1;
}

// ------------------------------------------------------------------------------------------------

void TestStatsViewForSlot() {
  CheckEq(StatsViewForSlot(13), 13, "slot 13 is view 13");
  CheckEq(StatsViewForSlot(39), 39, "slot 39 is view 39");
  CheckEq(StatsViewForSlot(350), 350, "slot 350 is below the change: view 350");
  CheckEq(StatsViewForSlot(351), 382, "slot 351 is view 382");
  CheckEq(StatsViewForSlot(352), 383, "slot 352 is view 383");
  CheckEq(StatsViewForSlot(381), 412, "slot 381 is view 412");

  std::set<uint32_t> views;
  for (uint32_t slot = 13; slot <= 39; ++slot) views.insert(StatsViewForSlot(slot));
  for (uint32_t slot = 351; slot <= 381; ++slot) views.insert(StatsViewForSlot(slot));
  CheckEq(views.size(), 27 + 31, "the 58 tournament slots have 58 different views");
  Check(*views.begin() == 13 && *views.rbegin() == 412, "the views span 13..412");
  for (uint32_t v : views) Check((v >= 13 && v <= 39) || (v >= 382 && v <= 412), "view " + std::to_string(v) + " is one the game writes");

  // The rotation: every week's slot is one the game accepts, and over two cycles it uses both ranges (the weeks that were affected).
  constexpr int64_t kWeek = 7 * 24 * 3600;
  constexpr int64_t kFirstMonday = 4 * 24 * 3600;
  int high = 0, low = 0;
  for (uint32_t week = 0; week < 116; ++week) {
    const Definition d = Current(kFirstMonday + static_cast<int64_t>(week) * kWeek + 3600, fs::path());
    CheckEq(d.week, week, "Current's week number");
    const bool is_low = d.table_id >= 13 && d.table_id <= 39;
    const bool is_high = d.table_id >= 351 && d.table_id <= 381;
    Check(is_low || is_high, "the rotation's slot " + std::to_string(d.table_id) + " is one the game accepts");
    (is_high ? high : low) += 1;
    const uint32_t view = StatsViewForSlot(d.table_id);
    Check((view >= 13 && view <= 39) || (view >= 382 && view <= 412), "the rotation's view " + std::to_string(view));
  }
  CheckEq(low, 54, "two cycles of the rotation: 27 weeks each on a slot below 351");
  CheckEq(high, 62, "two cycles of the rotation: 31 weeks each on a slot from 351");
}

// A week settles on the game's view, whatever the slot.
void TestSettleOnTheGamesView() {
  const int64_t now = Now();
  const uint32_t kSlots[] = {13, 39, 351, 366, 381};
  for (uint32_t slot : kSlots) {
    const std::string what = "slot " + std::to_string(slot) + ": ";
    Scratch s("view" + std::to_string(slot));
    lb::Store boards(s.lb());
    std::string error;
    Check(boards.Open(&error), what + "store opens");
    Awards awards(s.rmr());
    awards.Attach(&boards);
    const Definition d = WeekAround(100 + slot, slot, now);
    awards.NoteServed(d);

    const uint32_t view = StatsViewForSlot(slot);
    // The game's writes: the fastest is player 1, then 2, 3; player 4 is fourth.
    WriteTime(boards, 3, view, 62000);
    WriteTime(boards, 1, view, 60000);
    WriteTime(boards, 4, view, 63000);
    WriteTime(boards, 2, view, 61000);
    // Rows in the board named by the slot (what the server used to read from slot 351 on): faster, and worth nothing. For a
    // slot below 351 that board is the view itself, so there is nothing to put elsewhere.
    if (view != slot) {
      WriteTime(boards, 5, slot, 50000);
      WriteTime(boards, 6, slot, 51000);
    }

    std::string log;
    CheckEq(awards.RewardsFor(OwnerOf(1), d.ends - 1, &log), 0, what + "nothing is settled before the week ends");
    log.clear();
    CheckEq(awards.RewardsFor(OwnerOf(1), d.ends, &log), kRewardAudiR8, what + "first place gets the first prize");
    CheckEq(awards.RewardsFor(OwnerOf(2), d.ends, nullptr), kRewardTisRims, what + "second place gets the podium prize");
    CheckEq(awards.RewardsFor(OwnerOf(3), d.ends, nullptr), kRewardTisRims, what + "third place gets the podium prize");
    CheckEq(awards.RewardsFor(OwnerOf(4), d.ends, nullptr), 0, what + "fourth place gets nothing");
    CheckEq(awards.RewardsFor(OwnerOf(5), d.ends, nullptr), 0, what + "a row in the slot's own board wins nothing");
    CheckEq(awards.RewardsFor(OwnerOf(6), d.ends, nullptr), 0, what + "a second row in the slot's own board wins nothing");
    Check(log.find("3 winner(s)") != std::string::npos, what + "the log says three winners: " + log);
    CheckEq(SettledOf(WeeksFile(s.rmr()), d.week), 2, what + "the week is written as settled with 2");
  }
}

// What the file of an older server looks like, read by this code.
void TestOlderServerFile() {
  const int64_t now = Now();
  Scratch s("older");
  lb::Store boards(s.lb());
  std::string error;
  Check(boards.Open(&error), "older: store opens");

  // One line per week: week slot starts ends prize_first prize_podium podium settled. Each week has its own first prize and its
  // own winner (player 11 + its place in the list), so a prize can be traced to the week that gave it.
  struct W {
    uint32_t week, slot;
    int settled;
    uint32_t prize;
  };
  const W weeks[] = {
      {500, 351, 1, kRewardAudiR8},      // an older server settled it on board 351: nobody won there; settles again on view 382
      {501, 13, 1, kRewardTisRims},      // settled on the right board (slot below 351): stays settled
      {502, 352, 0, kRewardHydraulics},  // open: settles on view 383
      {503, 353, 2, kRewardAudiR8},      // settled by this code: stays settled
      {504, 39, 1, kRewardAudiR8},       // the highest slot below 351: stays settled
      {505, 381, 1, kRewardAudiR8},      // the highest slot: settles again on view 412
      {506, 350, 1, kRewardAudiR8},      // not a slot the game accepts, but below 351: its view is the slot itself, stays settled
  };
  std::string file;
  for (const W& w : weeks) {
    file += std::to_string(w.week) + " " + std::to_string(w.slot) + " " + std::to_string(now - 3600) + " " +
            std::to_string(now + 3600) + " " + std::to_string(w.prize) + " 0 1 " + std::to_string(w.settled) + '\n';
  }
  WriteFile(s.rmr() / "tournament_weeks.txt", file);
  // The rows are on the game's views, written by the player of that week.
  uint8_t player = 11;
  for (const W& w : weeks) WriteTime(boards, player++, StatsViewForSlot(w.slot), 60000);
  // An owner an older server paid wrongly (or rightly) keeps the prize; a winner's earlier prize stays beside the new one.
  WriteFile(s.rmr() / "tournament_awards.txt", [&] {
    char buf[64];
    std::string t;
    std::snprintf(buf, sizeof(buf), "%016llX 7\n", static_cast<unsigned long long>(OwnerOf(90)));
    t += buf;
    std::snprintf(buf, sizeof(buf), "%016llX 4\n", static_cast<unsigned long long>(OwnerOf(11)));
    t += buf;
    return t;
  }());

  Awards awards(s.rmr());
  awards.Attach(&boards);
  const int64_t after = now + 3600;
  std::string log;
  CheckEq(awards.RewardsFor(OwnerOf(11), after, &log), kRewardHydraulics | kRewardAudiR8, "older: week 500 (slot 351, value 1) settles again; the earlier prize stays");
  CheckEq(awards.RewardsFor(OwnerOf(12), after, nullptr), 0, "older: week 501 (slot 13, value 1) stays settled");
  CheckEq(awards.RewardsFor(OwnerOf(13), after, nullptr), kRewardHydraulics, "older: week 502 (slot 352, value 0) settles");
  CheckEq(awards.RewardsFor(OwnerOf(14), after, nullptr), 0, "older: week 503 (slot 353, value 2) stays settled");
  CheckEq(awards.RewardsFor(OwnerOf(15), after, nullptr), 0, "older: week 504 (slot 39, value 1) stays settled");
  CheckEq(awards.RewardsFor(OwnerOf(16), after, nullptr), kRewardAudiR8, "older: week 505 (slot 381, value 1) settles again");
  CheckEq(awards.RewardsFor(OwnerOf(17), after, nullptr), 0, "older: week 506 (slot 350, value 1) stays settled");
  CheckEq(awards.RewardsFor(OwnerOf(90), after, nullptr), 7, "older: a prize an older server gave is never taken back");
  Check(log.find("settled week 500") != std::string::npos, "older: the log names week 500: " + log);
  Check(log.find("settled week 502") != std::string::npos, "older: the log names week 502: " + log);
  Check(log.find("settled week 505") != std::string::npos, "older: the log names week 505: " + log);
  Check(log.find("settled week 501") == std::string::npos && log.find("settled week 503") == std::string::npos &&
            log.find("settled week 504") == std::string::npos && log.find("settled week 506") == std::string::npos,
        "older: the log names no week that was already settled: " + log);

  // The file now says 2 for every settled week; nothing is written as 1 any more.
  const auto after_file = WeeksFile(s.rmr());
  CheckEq(after_file.size(), 7, "older: the weeks file keeps its seven lines");
  for (const W& w : weeks) CheckEq(SettledOf(after_file, w.week), 2, "older: week " + std::to_string(w.week) + " is written as 2");

  // Once settled on the right board, a week is not settled again, whatever is written to the board afterwards.
  WriteTime(boards, 40, StatsViewForSlot(351), 1000);
  Awards again(s.rmr());
  again.Attach(&boards);
  CheckEq(again.RewardsFor(OwnerOf(40), after, nullptr), 0, "older: a faster time after the week settled wins nothing");
  CheckEq(again.RewardsFor(OwnerOf(11), after, nullptr), kRewardHydraulics | kRewardAudiR8, "older: a new Awards reads the 2 and keeps the prizes");
  CheckEq(again.RewardsFor(OwnerOf(16), after, nullptr), kRewardAudiR8, "older: a new Awards reads the 2: the prizes did not double");
}

// A week the server never settled wrongly (a new week, no file yet): serving the next week settles the one that ended.
void TestNextWeekSettlesFirst() {
  const int64_t now = Now();
  Scratch s("next");
  lb::Store boards(s.lb());
  std::string error;
  Check(boards.Open(&error), "next: store opens");
  Awards awards(s.rmr());
  awards.Attach(&boards);

  const uint32_t kSlot = 351;
  const uint32_t view = StatsViewForSlot(kSlot);
  const Definition week_a = WeekAround(300, kSlot, now);
  awards.NoteServed(week_a);
  WriteTime(boards, 1, view, 60000);
  WriteTime(boards, 2, view, 61000);
  WriteTime(boards, 7, kSlot, 50000);  // the slot's own board: not the game's, not touched
  CheckEq(RowsIn(boards, view), 2, "next: two rows on the game's view");
  CheckEq(RowsIn(boards, kSlot), 1, "next: one row on the slot's own board");

  // The next week of the same slot begins where the first ended: its definition is served before anyone asks for rewards.
  Definition week_b = WeekAround(301, kSlot, now);
  week_b.starts = week_a.ends;
  week_b.ends = week_a.ends + 7 * 24 * 3600;
  awards.NoteServed(week_b);
  CheckEq(RowsIn(boards, view), 0, "next: the game's view is emptied for the new week");
  CheckEq(RowsIn(boards, kSlot), 1, "next: the slot's own board is not touched");
  CheckEq(awards.RewardsFor(OwnerOf(1), week_b.starts, nullptr), kRewardAudiR8, "next: the week that ended was settled before its rows went (first)");
  CheckEq(awards.RewardsFor(OwnerOf(2), week_b.starts, nullptr), kRewardTisRims, "next: the week that ended was settled before its rows went (second)");
  CheckEq(awards.RewardsFor(OwnerOf(7), week_b.starts, nullptr), 0, "next: the slot's own board won nothing");
  const auto weeks = WeeksFile(s.rmr());
  CheckEq(SettledOf(weeks, 300), 2, "next: week 300 is written as settled");
  CheckEq(SettledOf(weeks, 301), 0, "next: week 301 is open");

  // The same definition served again changes nothing.
  awards.NoteServed(week_b);
  CheckEq(SettledOf(WeeksFile(s.rmr()), 301), 0, "next: week 301 is still open after it is served again");

  // A slot below 351: the view is the slot, and the drop is on it.
  const Definition low = WeekAround(310, 20, now);
  awards.NoteServed(low);
  WriteTime(boards, 3, 20, 60000);
  WriteTime(boards, 3, 51, 60000);  // slot 20 + 31: not its view
  Definition low_next = WeekAround(311, 20, now);
  low_next.starts = low.ends;
  low_next.ends = low.ends + 7 * 24 * 3600;
  awards.NoteServed(low_next);
  CheckEq(RowsIn(boards, 20), 0, "next: a slot below 351 is emptied on the slot itself");
  CheckEq(RowsIn(boards, 51), 1, "next: and not on slot + 31");
  CheckEq(awards.RewardsFor(OwnerOf(3), low_next.starts, nullptr), kRewardAudiR8, "next: the low slot's winner was settled before the drop");
}

// A week with nobody on its board settles with no winners, and a week that is not over yet is left alone.
void TestEmptyAndOpenWeeks() {
  const int64_t now = Now();
  Scratch s("empty");
  lb::Store boards(s.lb());
  std::string error;
  Check(boards.Open(&error), "empty: store opens");
  Awards awards(s.rmr());
  awards.Attach(&boards);
  const Definition d = WeekAround(400, 360, now);
  awards.NoteServed(d);
  std::string log;
  CheckEq(awards.RewardsFor(OwnerOf(1), d.ends, &log), 0, "empty: nobody wins an empty board");
  Check(log.find("0 winner(s)") != std::string::npos, "empty: the log says no winners: " + log);
  CheckEq(SettledOf(WeeksFile(s.rmr()), 400), 2, "empty: the week is settled all the same");

  // A row older than the week's start is not part of the week even on the right view.
  const Definition later = WeekAround(401, 360, now + 10 * 24 * 3600);
  awards.NoteServed(later);
  WriteTime(boards, 2, StatsViewForSlot(360), 60000);  // stamped with now, before `later` began
  CheckEq(awards.RewardsFor(OwnerOf(2), later.ends, nullptr), 0, "empty: a row written before the week began does not count");
}

}  // namespace

int main() {
  TestStatsViewForSlot();
  TestSettleOnTheGamesView();
  TestOlderServerFile();
  TestNextWeekSettlesFirst();
  TestEmptyAndOpenWeeks();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
