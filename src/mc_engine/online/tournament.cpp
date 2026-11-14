#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Online Tournament (mcOnlineTournament) and its prizes on our own backend.
//
// The mode runs in the game unchanged. The menu's state machine
// (sub_82740958, ticked by the menu form sub_82642E40) fetches the definition
// of the running tournament from the rlUpload table "tournament":
//   7  ReadRecordsByGamer("tournament")   -> 8 (11 if it could not queue)
//   8  status done, >= 1 record           -> DownloadFile(record +40) -> 9
//      status done, no record             -> 12 "NO TOURNAMENT AVAILABLE"
//      status failed                      -> 11 "server is not available"
//   9  status done                        -> 10, the file sits at the object +48
// The file names one of the city's races, a leaderboard slot (13..39 or
// 351..381), a car restriction and the text the menu shows (layout in
// tournament_schedule.h). Start runs that race in game mode 25 with the
// tournament flag set; at the finish (sub_822D0438) the game posts the time to
// the slot's stats view through its Stats Worker and XSessionWriteStats, which
// the leaderboards module serves.
//
// Prizes come the same way from the table "Rewards": mcStatsManagerGame
// (*0x828CCF68) runs a twin state machine at its +1620 (sub_822D4E00,
// ticked by sub_822D16E0) after each profile-stats sync, every two hours: read
// the player's record, download a 38-byte file, and when it starts "001\0"
// (sub_822D1260) set the career stat tREW[i] for each prize i it carries. The
// server decides who won (tournament_awards.h). Opening the tournament menu
// also starts that fetch, so a prize shows up without waiting for the sync.
//
// Here: the reads ask rmr::client (the shared server, or the local store) and
// answer with one record whose file id carries a tag; the downloads hand over
// what came with that reply. Replies are written at the top of the next tick of
// sub_82740958, which runs every frame from the stats manager as well as from
// the menu.
//
// Registered as midasm hooks in larecomp_config.toml:
//   sub_82740958  Hook_TournamentPump        mcOnlineTournament state machine
// The rlUpload entry points are hooked by rate_my_ride.cpp, which calls
// tournament::game::OnReadRecords / OnDownload first.
//

#include "tournament.h"

#include "online_common.h"
#include "tournament_schedule.h"
#include "rate_my_ride/rmr_client.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <rex/system/xmemory.h>

namespace {

using rex::memory::Memory;

// rlUpload record, 152 bytes (layout in rate_my_ride.cpp). The state machines
// only check the count and read the file id; the rest makes it a record any
// other reader of the table would accept.
constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kRecHandleId = 8;
constexpr uint32_t kRecGamertag = 16;
constexpr uint32_t kRecId = 32;
constexpr uint32_t kRecFileCount = 36;
constexpr uint32_t kRecFileIds = 40;
constexpr uint32_t kRecFileSizes = 52;
constexpr char kOwnerTag[] = "LARecomp";

// mcStatsManagerGame lives at *0x828CCF68 (created by sub_822D0048; the career
// stats tREW is written through it). Its vtable word is one of the class's two
// (0x8203B7B4 / 0x8203B7C4, the second is what a live object shows). Its
// rewards machine is at +1620 ({vtable, state, ...}; state 0 = idle, 7 = start
// the fetch, as sub_822D4CC0 does).
constexpr uint32_t kStatsMgrPtr = 0x828CCF68;
constexpr uint32_t kStatsMgrVtableA = 0x8203B7B4;
constexpr uint32_t kStatsMgrVtableB = 0x8203B7C4;
constexpr uint32_t kStatsMgrRewards = 1620;

enum TaskState : uint32_t { kPending = 1, kFailed = 2, kSucceeded = 3 };

enum Kind { kTournament = 0, kRewards = 1, kKinds = 2 };

struct TableInfo {
  const char* table;
  rmr::Op op;
  uint32_t tag;
  uint32_t file_size;
};
constexpr TableInfo kTables[kKinds] = {
    {"tournament", rmr::Op::kTournament, tournament::kFileIdTag, tournament::kDefinitionSize},
    {"Rewards", rmr::Op::kRewards, tournament::kRewardsFileIdTag, tournament::kRewardsFileSize},
};

struct Pending {
  Kind kind = kTournament;
  uint32_t records = 0;
  uint32_t count_out = 0;
  uint32_t status = 0;
};

struct Completed {
  Pending pending;
  rmr::Response resp;
};

std::mutex g_mutex;
std::deque<Completed> g_done;  // replies waiting for the game thread

// Game thread only: per table, the file of the last reply, its id, and the
// status object its state machine polls (the read's, reused for the download).
struct Served {
  std::vector<uint8_t> file;
  uint32_t file_id = 0;
  uint32_t status = 0;
};
Served g_served[kKinds];

Memory* Mem() {
  auto* rt = rex::Runtime::instance();
  return rt ? rt->memory() : nullptr;
}

void Put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint32_t Get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void SetStatus(Memory* mem, uint32_t status, uint32_t state, uint32_t code) {
  if (!IsGuestPtr(status)) return;
  GuestWrite32(mem, status + 4, code);
  GuestWrite32(mem, status, state);
}

// Starts the stats manager's rewards fetch if it is idle, as the end of a
// profile-stats sync does (sub_822D4CC0).
void KickRewardsFetch(Memory* mem) {
  const uint32_t stats = GuestRead32(mem, kStatsMgrPtr);
  const uint32_t vtable = IsGuestPtr(stats) ? GuestRead32(mem, stats) : 0;
  if (vtable != kStatsMgrVtableA && vtable != kStatsMgrVtableB) {
    LARECOMP_APP_WARN("[tourney] stats manager not found (0x{:08X}); prizes wait for the sync",
                      stats);
    return;
  }
  const uint32_t rewards = GuestRead32(mem, stats + kStatsMgrRewards);
  if (!IsGuestPtr(rewards)) return;
  if (GuestRead32(mem, rewards + 4) == 0) {
    GuestWrite32(mem, rewards + 4, 7);
    LARECOMP_APP_INFO("[tourney] asked for this player's prizes");
  }
}

void Apply(Memory* mem, const Completed& c) {
  const Pending& p = c.pending;
  const TableInfo& t = kTables[p.kind];
  Served& s = g_served[p.kind];

  if (c.resp.err != rmr::kOk ||
      (!c.resp.data.empty() && c.resp.data.size() != t.file_size) ||
      (p.kind == kTournament && c.resp.data.empty())) {
    LARECOMP_APP_WARN("[tourney] {} request failed (code {}, {} bytes)", t.table, c.resp.err,
                      c.resp.data.size());
    GuestWrite32(mem, p.count_out, 0);
    SetStatus(mem, p.status, kFailed, c.resp.err != rmr::kOk ? c.resp.err : rmr::kUnknown);
    return;
  }

  if (c.resp.data.empty()) {
    // No prizes for this player: an empty read, as the table would give.
    GuestWrite32(mem, p.count_out, 0);
    SetStatus(mem, p.status, kSucceeded, 0);
    LARECOMP_APP_INFO("[tourney] no prizes for this player");
    return;
  }

  s.file = c.resp.data;
  s.file_id = t.tag | (c.resp.count & 0x00FFFFFF);

  uint8_t rec[kRecordSize] = {};
  rec[0] = 1;
  Put32(rec + kRecHandleId + 4, s.file_id);
  std::memcpy(rec + kRecGamertag, kOwnerTag, sizeof(kOwnerTag) - 1);
  Put32(rec + kRecId, s.file_id);
  Put32(rec + kRecFileCount, 1);
  Put32(rec + kRecFileIds, s.file_id);
  Put32(rec + kRecFileSizes, t.file_size);
  std::memcpy(mem->TranslateVirtual<uint8_t*>(p.records), rec, kRecordSize);
  GuestWrite32(mem, p.count_out, 1);
  SetStatus(mem, p.status, kSucceeded, 0);

  if (p.kind == kTournament) {
    const char* text = reinterpret_cast<const char*>(s.file.data() + 23);
    LARECOMP_APP_INFO("[tourney] week {}: '{}' race {:08X} slot {} restriction {}/{:08X} via {}",
                      c.resp.count, std::string(text, strnlen(text, 253)),
                      Get32(s.file.data() + 16), Get32(s.file.data() + 4),
                      Get32(s.file.data() + 8), Get32(s.file.data() + 12),
                      rmr::client::DescribeTarget());
    KickRewardsFetch(mem);
  } else {
    LARECOMP_APP_INFO("[tourney] prizes for this player: {} (mask {})",
                      tournament::RewardNames(c.resp.count), c.resp.count);
  }
}

int KindOfTable(Memory* mem, uint32_t table) {
  if (!IsGuestPtr(table)) return -1;
  const std::string name = GuestCStr(mem, table, 16);
  for (int k = 0; k < kKinds; ++k)
    if (name == kTables[k].table) return k;
  return -1;
}

}  // namespace

namespace tournament::game {

bool OnReadRecords(uint32_t table, uint32_t max_records, uint32_t records, uint32_t count_out,
                   uint32_t status) {
  if (!rmr::client::Enabled()) return false;
  Memory* mem = Mem();
  if (!mem) return false;
  const int kind = KindOfTable(mem, table);
  if (kind < 0) return false;
  if (max_records == 0 || !IsGuestPtr(records) || !IsGuestPtr(count_out)) return false;

  Pending p;
  p.kind = static_cast<Kind>(kind);
  p.records = records;
  p.count_out = count_out;
  p.status = status;
  g_served[kind].status = status;
  GuestWrite32(mem, count_out, 0);
  SetStatus(mem, status, kPending, 0);

  rmr::Request req;
  req.op = kTables[kind].op;
  rmr::client::Submit(std::move(req), [p](rmr::Response&& resp) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_done.push_back(Completed{p, std::move(resp)});
  });
  return true;
}

bool OnDownload(uint32_t file_id, uint32_t buffer, uint32_t buffer_size, uint32_t size_out,
                uint32_t progress) {
  int kind = -1;
  if (tournament::IsTournamentFileId(file_id)) kind = kTournament;
  if (tournament::IsRewardsFileId(file_id)) kind = kRewards;
  if (kind < 0) return false;
  Memory* mem = Mem();
  if (!mem || !IsGuestPtr(buffer) || !IsGuestPtr(size_out)) return false;
  Served& s = g_served[kind];
  if (file_id != s.file_id || s.file.empty() || buffer_size < s.file.size()) {
    LARECOMP_APP_WARN("[tourney] download of {:08X}: no such file", file_id);
    GuestWrite32(mem, size_out, 0);
    SetStatus(mem, s.status, kFailed, rmr::kFileNotFound);
    return true;
  }
  std::memcpy(mem->TranslateVirtual<uint8_t*>(buffer), s.file.data(), s.file.size());
  GuestWrite32(mem, size_out, static_cast<uint32_t>(s.file.size()));
  if (IsGuestPtr(progress)) GuestWrite32(mem, progress, 0x3F800000);  // 1.0f
  SetStatus(mem, s.status, kSucceeded, 0);
  return true;
}

}  // namespace tournament::game

// sub_82740958 entry (r3 = a state object): replies that arrived since the
// last tick are applied before the state machines look at their status.
void Hook_TournamentPump(PPCRegister& r3) {
  std::deque<Completed> batch;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_done.empty()) return;
    batch.swap(g_done);
  }
  Memory* mem = Mem();
  if (!mem) return;
  for (const Completed& c : batch) Apply(mem, c);
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_TournamentPump(PPCRegister& r3) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
