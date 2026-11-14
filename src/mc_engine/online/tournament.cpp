#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Online Tournament (mcOnlineTournament) on our own backend.
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
// 351..381) and a car restriction (layout in tournament_schedule.h). Start runs
// that race in game mode 25 with the tournament flag set; at the finish
// (sub_822D0438) the game posts the time to the slot's stats view through its
// Stats Worker and XSessionWriteStats, which the leaderboards module serves.
//
// Here: the read asks rmr::client (the shared server, or the local store) for
// this week's definition and answers with one record whose file id carries the
// week; the download hands over the definition that came with it. Replies are
// written at the top of the state machine's next tick, on the game thread.
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

constexpr char kTable[] = "tournament";

// rlUpload record, 152 bytes (layout in rate_my_ride.cpp). The state machine
// only checks the count and reads the file id; the rest makes it a record any
// other reader of the table would accept.
constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kRecHandleId = 8;
constexpr uint32_t kRecGamertag = 16;
constexpr uint32_t kRecId = 32;
constexpr uint32_t kRecFileCount = 36;
constexpr uint32_t kRecFileIds = 40;
constexpr uint32_t kRecFileSizes = 52;
constexpr char kOwnerTag[] = "LARecomp";

enum TaskState : uint32_t { kPending = 1, kFailed = 2, kSucceeded = 3 };

struct Pending {
  uint32_t records = 0;
  uint32_t count_out = 0;
  uint32_t max_records = 0;
  uint32_t status = 0;
};

struct Completed {
  Pending pending;
  rmr::Response resp;
};

std::mutex g_mutex;
std::deque<Completed> g_done;            // replies waiting for the game thread
std::vector<uint8_t> g_definition;       // game thread only: the last definition
uint32_t g_definition_id = 0;            // its file id
uint32_t g_status = 0;                   // the state machine's status object

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

void Apply(Memory* mem, const Completed& c) {
  const Pending& p = c.pending;
  if (c.resp.err != rmr::kOk || c.resp.data.size() != tournament::kDefinitionSize) {
    LARECOMP_APP_WARN("[tourney] definition request failed (code {}, {} bytes)", c.resp.err,
                      c.resp.data.size());
    GuestWrite32(mem, p.count_out, 0);
    SetStatus(mem, p.status, kFailed, c.resp.err != rmr::kOk ? c.resp.err : rmr::kUnknown);
    return;
  }
  g_definition = c.resp.data;
  g_definition_id = tournament::kFileIdTag | (c.resp.count & 0x00FFFFFF);

  uint8_t rec[kRecordSize] = {};
  rec[0] = 1;
  Put32(rec + kRecHandleId + 4, g_definition_id);
  std::memcpy(rec + kRecGamertag, kOwnerTag, sizeof(kOwnerTag) - 1);
  Put32(rec + kRecId, g_definition_id);
  Put32(rec + kRecFileCount, 1);
  Put32(rec + kRecFileIds, g_definition_id);
  Put32(rec + kRecFileSizes, tournament::kDefinitionSize);
  std::memcpy(mem->TranslateVirtual<uint8_t*>(p.records), rec, kRecordSize);
  GuestWrite32(mem, p.count_out, 1);
  SetStatus(mem, p.status, kSucceeded, 0);

  const char* name = reinterpret_cast<const char*>(g_definition.data() + 23);
  LARECOMP_APP_INFO("[tourney] week {}: '{}' race {:08X} slot {} restriction {}/{:08X} via {}",
                    c.resp.count, std::string(name, strnlen(name, 253)),
                    Get32(g_definition.data() + 16), Get32(g_definition.data() + 4),
                    Get32(g_definition.data() + 8), Get32(g_definition.data() + 12),
                    rmr::client::DescribeTarget());
}

}  // namespace

namespace tournament::game {

bool OnReadRecords(uint32_t table, uint32_t max_records, uint32_t records, uint32_t count_out,
                   uint32_t status) {
  if (!rmr::client::Enabled()) return false;
  Memory* mem = Mem();
  if (!mem || !IsGuestPtr(table) || GuestCStr(mem, table, 16) != kTable) return false;
  if (max_records == 0 || !IsGuestPtr(records) || !IsGuestPtr(count_out)) return false;

  Pending p;
  p.records = records;
  p.count_out = count_out;
  p.max_records = max_records;
  p.status = status;
  g_status = status;
  GuestWrite32(mem, count_out, 0);
  SetStatus(mem, status, kPending, 0);

  rmr::Request req;
  req.op = rmr::Op::kTournament;
  rmr::client::Submit(std::move(req), [p](rmr::Response&& resp) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_done.push_back(Completed{p, std::move(resp)});
  });
  return true;
}

bool OnDownload(uint32_t file_id, uint32_t buffer, uint32_t buffer_size, uint32_t size_out,
                uint32_t progress) {
  if (!tournament::IsTournamentFileId(file_id)) return false;
  Memory* mem = Mem();
  if (!mem || !IsGuestPtr(buffer) || !IsGuestPtr(size_out)) return false;
  // The state machine's status object (the read's), which state 9 polls.
  if (file_id != g_definition_id || g_definition.empty() ||
      buffer_size < g_definition.size()) {
    LARECOMP_APP_WARN("[tourney] download of {:08X}: no such definition", file_id);
    GuestWrite32(mem, size_out, 0);
    SetStatus(mem, g_status, kFailed, rmr::kFileNotFound);
    return true;
  }
  std::memcpy(mem->TranslateVirtual<uint8_t*>(buffer), g_definition.data(), g_definition.size());
  GuestWrite32(mem, size_out, static_cast<uint32_t>(g_definition.size()));
  if (IsGuestPtr(progress)) GuestWrite32(mem, progress, 0x3F800000);  // 1.0f
  SetStatus(mem, g_status, kSucceeded, 0);
  return true;
}

}  // namespace tournament::game

// sub_82740958 entry (r3 = the state object): replies that arrived since the
// last tick are applied before the state machine looks at its status.
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
