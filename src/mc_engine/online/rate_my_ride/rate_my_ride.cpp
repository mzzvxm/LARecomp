#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Rate My Ride (mcRateMyRide) on our own backend.
//
// The mode itself still runs entirely in the game: the garage scene, the
// scxml menus (tune/ui/garage/rmr/), the photo-mode snapshots, purchases and
// rewards. What died with Rockstar's servers is the storage it
// talks to: rage::rlUpload, a GameSpy Sake table called "RateMyRide". Every
// request goes through one of nine functions that queue an rlUpload task, and
// the mode polls the task's status object ({state, code}: 1 pending, 2 failed,
// 3 done) once per frame in sub_826B2328.
//
// We take over those nine functions for the "RateMyRide" table (the other
// tables - ProfileStats, Rewards, PhotoAlbum, tournament - keep the stock
// path), send the request to rmr::client (local store or the shared server)
// and hand the reply back in the exact shape the rlUpload worker would have.
//
// Replies are applied on the game thread at the top of the mode's next poll,
// never as soon as they arrive: RefreshCurrentRecord (sub_826B1BB8) clears
// its count output right AFTER queueing the read, which only works because the
// real reply always came later. Applying early would make the purchase flow
// see no record.
//
// Registered as midasm hooks in larecomp_config.toml:
//   sub_824F6DE8  Hook_RmrGetTableSize       GetTableSizeCmd
//   sub_824F6EA0  Hook_RmrReadByGamer        ReadRecordsByGamerCmd
//   sub_824F6FA0  Hook_RmrReadRandom         ReadRandomRecordCmd
//   sub_824F70A8  Hook_RmrReadByRank         ReadRecordsByRankCmd
//   sub_824F7188  Hook_RmrRateRecord         RateRecordCmd
//   sub_824F7308  Hook_RmrUploadFile         UploadFileCmd
//   sub_824F7468  Hook_RmrOverwriteFile      OverwriteFileCmd
//   sub_824F7578  Hook_RmrDownloadFile       DownloadFileCmd
//   sub_824F7638  Hook_RmrOverwriteUserData  OverwriteUserDataCmd
//   sub_826B2328  Hook_RmrPump               mcRateMyRide status poll
//   sub_826B15B8  Hook_RmrEnter              "[rmr] Entering Rate My Ride"
//

#include "../online_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <rex/ppc/function.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>

#include "rmr_client.h"
#include "rmr_protocol.h"

namespace {

using rex::memory::Memory;

constexpr char kTable[] = "RateMyRide";

// rlUpload record, 152 bytes big-endian (serializer sub_824F30D8).
constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kRecHandleId = 8;      // rlGamerHandle: u8 type @0, u64 id @8
constexpr uint32_t kRecGamertag = 16;     // char[16]
constexpr uint32_t kRecId = 32;
constexpr uint32_t kRecFileCount = 36;
constexpr uint32_t kRecFileIds = 40;      // [3]
constexpr uint32_t kRecFileSizes = 52;    // [3]
constexpr uint32_t kRecNumRatings = 64;
constexpr uint32_t kRecAverage = 68;      // f32
constexpr uint32_t kRecRank = 72;
constexpr uint32_t kRecUserData = 76;     // [64]
constexpr uint32_t kRecUserDataSize = 140;
constexpr uint32_t kRecContent = 144;

// The game's record array (0x82908E58) has room for exactly ten records.
constexpr uint32_t kMaxRecordsOut = 10;
// Random reads may return a few candidates so we can skip rides whose vehicle
// this player does not have (mods, DLC); the game itself takes one.
constexpr uint32_t kRandomCandidates = 16;

// Guest globals used to find the local player's gamer handle, the same chain
// the mode uses: sub_82388580(0) -> sub_82270170(netmgr, idx) -> +16 rlGamerInfo,
// handle at +56 (sub_824E7110), gamertag at +76 (sub_824E7118).
constexpr uint32_t kLocalPlayersPtr = 0x8288E5AC;
constexpr uint32_t kNetMgrPtr = 0x8286D800;
constexpr uint32_t kNetMgrGamers = 4284;
constexpr uint32_t kGamerStride = 10832;
constexpr uint32_t kGamerHandle = 16 + 56;
constexpr uint32_t kGamerTag = 16 + 76;

// Vehicle id -> catalog index hash table (lookup sub_82396F58).
constexpr uint32_t kVehicleBucketCount = 0x828CD064;
constexpr uint32_t kVehicleBuckets = 0x828CD06C;

// What sub_826B0D50 calls to put an error text in the "RMR_Failure_Menu"
// dialog: find the UI state, dynamic_cast it to UIMessageBox, set the text by
// string-table key (resolved at the call, so the key can be a temporary).
constexpr uint32_t kGuestNewFn = 0x82130528;        // operator new
constexpr uint32_t kFindUiStateFn = 0x821FA230;     // UI state by name
constexpr uint32_t kDynamicCastFn = 0x823DEDD8;     // __RTDynamicCast
constexpr uint32_t kSetMessageTextFn = 0x82637DD8;  // UIMessageBox text = key
constexpr uint32_t kRttiVhsmState = 0x827DA9A4;
constexpr uint32_t kRttiUIMessageBox = 0x827DBA20;
constexpr uint32_t kStrFailureMenu = 0x820BAAF4;    // "RMR_Failure_Menu"
constexpr uint32_t kStrDevError = 0x820BAB90;       // "RMR_DEV_ERROR", its stock text

enum TaskState : uint32_t { kPending = 1, kFailed = 2, kSucceeded = 3 };

std::atomic<uint32_t> g_generation{1};
bool g_failure_text_swapped = false;  // game thread only

// ----------------------------------------------------------- guest i/o ----

Memory* Mem() {
  auto* rt = rex::Runtime::instance();
  return rt ? rt->memory() : nullptr;
}

uint32_t U32(PPCRegister& r) { return static_cast<uint32_t>(r.u64); }

void ReadBytes(Memory* mem, uint32_t addr, void* dst, size_t n) {
  std::memcpy(dst, mem->TranslateVirtual<const uint8_t*>(addr), n);
}

void WriteBytes(Memory* mem, uint32_t addr, const void* src, size_t n) {
  std::memcpy(mem->TranslateVirtual<uint8_t*>(addr), src, n);
}

uint8_t GuestRead8(Memory* mem, uint32_t addr) {
  return *mem->TranslateVirtual<const uint8_t*>(addr);
}

uint64_t LoadBe64(const uint8_t* p) {
  return (uint64_t(rmr::LoadBe32(p)) << 32) | rmr::LoadBe32(p + 4);
}

void StoreBe64(uint8_t* p, uint64_t v) {
  rmr::StoreBe32(p, static_cast<uint32_t>(v >> 32));
  rmr::StoreBe32(p + 4, static_cast<uint32_t>(v));
}

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

void GuestWriteFloat(Memory* mem, uint32_t addr, float f) { GuestWrite32(mem, addr, FloatBits(f)); }

bool IsRmrTable(Memory* mem, uint32_t table) {
  return IsGuestPtr(table) && GuestCStr(mem, table, 32) == kTable;
}

template <typename... Args>
uint32_t CallGuest(uint32_t fn_addr, Args... args) {
  auto* rt = rex::Runtime::instance();
  auto* dispatcher = rt ? rt->function_dispatcher() : nullptr;
  PPCFunc* fn = dispatcher ? dispatcher->GetFunction(fn_addr) : nullptr;
  if (!fn) return 0;
  return rex::ppc::GuestToHostFunction<uint32_t>(fn, static_cast<uint32_t>(args)...);
}

// Sets the text of the "RMR_Failure_Menu" dialog, like sub_826B0D50 does for
// error codes. `key` is a guest string; returns false if the dialog is missing.
bool SetFailureDialogKey(uint32_t key) {
  const uint32_t state = CallGuest(kFindUiStateFn, kStrFailureMenu);
  if (!IsGuestPtr(state)) return false;
  const uint32_t box = CallGuest(kDynamicCastFn, state, 0, kRttiVhsmState, kRttiUIMessageBox, 0);
  if (!IsGuestPtr(box)) return false;
  CallGuest(kSetMessageTextFn, box, key);
  return true;
}

// "Rate rides" with nothing to rate fails into the same dialog as a dead
// server ("RMR_DEV_ERROR": the server is not available). That was rare against
// Rockstar's full server and is common on a new or local one, so say what
// happened instead: "RMR_NoRecordsDesc", the game's own "server is empty".
void ShowEmptyServerOnFailure(Memory* mem) {
  static uint32_t key = 0;  // game-heap copy of the key, allocated once
  if (!key) {
    static constexpr char kKey[] = "RMR_NoRecordsDesc";
    key = CallGuest(kGuestNewFn, sizeof(kKey));
    if (!IsGuestPtr(key)) {
      key = 0;
      return;
    }
    WriteBytes(mem, key, kKey, sizeof(kKey));
  }
  g_failure_text_swapped = SetFailureDialogKey(key);
}

void RestoreFailureText() {
  if (g_failure_text_swapped) {
    SetFailureDialogKey(kStrDevError);
    g_failure_text_swapped = false;
  }
}

// ------------------------------------------------------------- identity ---

struct Me {
  std::array<uint8_t, 16> handle{};
  bool valid = false;
  uint64_t key = 0;  // our owner key for this profile
  std::string gamertag;
};

bool HandleValid(const uint8_t* h) {
  return h[0] >= 1 && h[0] <= 3 && rmr::LoadBe32(h + 12) != 0;
}

// Address of the local player's gamer record, or 0.
uint32_t LocalGamer(Memory* mem) {
  // sub_82388580(0): *(*(*(0x8288E5AC)) + 0)
  const uint32_t holder = GuestRead32(mem, kLocalPlayersPtr);
  const uint32_t players = IsGuestPtr(holder) ? GuestRead32(mem, holder) : 0;
  const uint32_t lp = IsGuestPtr(players) ? GuestRead32(mem, players) : 0;
  const uint32_t netmgr = GuestRead32(mem, kNetMgrPtr);
  if (!IsGuestPtr(lp) || !IsGuestPtr(netmgr)) return 0;
  const uint32_t idx = GuestRead32(mem, lp + 4);
  const uint32_t gamers = GuestRead32(mem, netmgr + kNetMgrGamers);
  if (idx >= 64 || !IsGuestPtr(gamers)) return 0;
  return gamers + kGamerStride * idx;
}

Me LocalPlayer(Memory* mem) {
  Me me;
  me.key = rmr::client::PlayerOwnerKey();
  if (const uint32_t gamer = LocalGamer(mem)) {
    ReadBytes(mem, gamer + kGamerHandle, me.handle.data(), me.handle.size());
    me.valid = HandleValid(me.handle.data());
    me.gamertag = GuestCStr(mem, gamer + kGamerTag, 16);
  }
  if (!me.valid) {
    // The game's own handle is {1, profile XUID} (kSharedGuestXuid on SDKs
    // without per-profile XUIDs); the chain above only confirms it.
    auto* ks = rex::system::kernel_state();
    const auto* profile = ks ? ks->user_profile() : nullptr;
    me.handle.fill(0);
    me.handle[0] = 1;
    StoreBe64(me.handle.data() + kRecHandleId,
              profile ? profile->xuid() : rmr::kSharedGuestXuid);
    me.valid = true;
  }
  return me;
}

// A handle the game hands us back: ours (our XUID, or the legacy shared one) or
// one we minted for another player, whose id is that player's owner key.
uint64_t OwnerOf(const uint8_t* h, const Me& me) {
  if (!HandleValid(h)) return 0;
  const uint64_t id = LoadBe64(h + kRecHandleId);
  if ((me.valid && id == LoadBe64(me.handle.data() + kRecHandleId)) ||
      id == rmr::kSharedGuestXuid)
    return me.key;
  return id;
}

uint64_t OwnerOfGuest(Memory* mem, uint32_t handle_addr, const Me& me) {
  uint8_t h[16];
  ReadBytes(mem, handle_addr, h, sizeof(h));
  return OwnerOf(h, me);
}

// ------------------------------------------------------------- records ----

void EncodeGuestRecord(const rmr::Record& r, const Me& me, uint8_t* g) {
  std::memset(g, 0, kRecordSize);
  if (r.owner == me.key && me.valid) {
    std::memcpy(g, me.handle.data(), me.handle.size());
  } else {
    g[0] = 1;
    StoreBe64(g + kRecHandleId, r.owner);
  }
  std::memcpy(g + kRecGamertag, r.gamertag.data(), std::min<size_t>(r.gamertag.size(), 15));
  rmr::StoreBe32(g + kRecId, r.id);
  rmr::StoreBe32(g + kRecFileCount, r.file_count);
  for (uint32_t i = 0; i < rmr::kMaxFiles; ++i) {
    rmr::StoreBe32(g + kRecFileIds + 4 * i, r.file_ids[i]);
    rmr::StoreBe32(g + kRecFileSizes + 4 * i, r.file_sizes[i]);
  }
  rmr::StoreBe32(g + kRecNumRatings, r.num_ratings);
  rmr::StoreBe32(g + kRecAverage, FloatBits(r.average));
  rmr::StoreBe32(g + kRecRank, r.rank);
  const size_t ud = std::min<size_t>(r.userdata.size(), rmr::kUserDataMax);
  std::memcpy(g + kRecUserData, r.userdata.data(), ud);
  rmr::StoreBe32(g + kRecUserDataSize, static_cast<uint32_t>(ud));
  g[kRecContent] = r.content;
}

// sub_824F2380: what the game accepts as a record for rating/overwriting.
bool GuestRecordValid(const uint8_t* g) {
  if (!HandleValid(g) || g[kRecGamertag] == 0 || rmr::LoadBe32(g + kRecId) == 0) return false;
  const uint32_t n = rmr::LoadBe32(g + kRecFileCount);
  if (n == 0 || n > rmr::kMaxFiles) return false;
  for (uint32_t i = 0; i < n; ++i)
    if (!rmr::LoadBe32(g + kRecFileIds + 4 * i) || !rmr::LoadBe32(g + kRecFileSizes + 4 * i))
      return false;
  return true;
}

// Whether this player's game knows the ride's vehicle (sub_82396F58 >= 0).
// Rides built on a mod car the player does not have would only fail with
// "you must buy the downloadable content".
bool VehicleKnown(Memory* mem, const rmr::Record& r) {
  if (r.userdata.size() < rmr::kUdUsedBytes) return true;
  const uint32_t key = rmr::LoadBe32(r.userdata.data() + rmr::kUdVehicle);
  const uint32_t count = GuestRead32(mem, kVehicleBucketCount);
  const uint32_t buckets = GuestRead32(mem, kVehicleBuckets);
  if (count == 0 || (count & (count - 1)) != 0 || !IsGuestPtr(buckets)) return true;
  uint32_t e = GuestRead32(mem, buckets + 4 * ((count - 1) & key));
  for (int guard = 0; IsGuestPtr(e) && guard < 4096; ++guard) {
    if (GuestRead32(mem, e) == key) return true;
    e = GuestRead32(mem, e + 12);
  }
  return false;
}

// Length of a JPEG stream inside a fixed-size buffer (the snapshots are
// uploaded at 204799 bytes, most of it padding), or 0 if it is not one.
size_t JpegLength(const uint8_t* d, size_t n) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
  size_t i = 2;
  while (i + 4 <= n) {
    if (d[i] != 0xFF) return 0;
    const uint8_t m = d[i + 1];
    if (m == 0xD9) return i + 2;
    if (m == 0xFF) {
      ++i;
      continue;
    }
    if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      i += 2;
      continue;
    }
    const size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
    if (len < 2) return 0;
    i += 2 + len;
    if (m == 0xDA) {
      // Entropy-coded data runs to the next marker that is not a stuffed 0xFF00
      // or a restart marker.
      while (i + 1 < n) {
        if (d[i] == 0xFF && d[i + 1] != 0x00 && !(d[i + 1] >= 0xD0 && d[i + 1] <= 0xD7)) break;
        ++i;
      }
    }
  }
  return 0;
}

// The upload's file list: the serialized car, then the two snapshots. A
// snapshot that is not a well-formed JPEG is dropped (the car alone is a
// complete ride); everything is copied now, since the photo buffers are
// reused as soon as the menu moves on.
bool ReadUploadFiles(Memory* mem, uint32_t ptrs, uint32_t sizes, uint32_t nfiles,
                     std::vector<std::vector<uint8_t>>& out) {
  out.clear();
  for (uint32_t i = 0; i < nfiles && i < rmr::kMaxFiles; ++i) {
    const uint32_t p = GuestRead32(mem, ptrs + 4 * i);
    const uint32_t s = GuestRead32(mem, sizes + 4 * i);
    if (i == 0) {
      if (!IsGuestPtr(p) || s == 0 || s > rmr::kMaxCarBytes) return false;
      out.emplace_back(s);
      ReadBytes(mem, p, out.back().data(), s);
      continue;
    }
    if (!IsGuestPtr(p) || s == 0) break;
    std::vector<uint8_t> buf(std::min<uint32_t>(s, rmr::kMaxPhotoBytes));
    ReadBytes(mem, p, buf.data(), buf.size());
    const size_t len = JpegLength(buf.data(), buf.size());
    if (len == 0) break;
    buf.resize(len);
    out.push_back(std::move(buf));
  }
  return !out.empty();
}

// ---------------------------------------------------------- task plumbing --

// Where a reply lands, captured when the game queues the task.
struct Pending {
  rmr::Op op = rmr::Op::kTableSize;
  uint32_t generation = 0;
  uint32_t status = 0;     // guest {state, code}
  uint32_t out = 0;        // record array / record / download buffer
  uint32_t count_out = 0;  // record count / downloaded size
  uint32_t max_out = 0;    // records the array takes / buffer size
  uint32_t progress = 0;   // f32, upload/download progress
  uint32_t max_ratings = 0;  // random: 0 = the last, unfiltered attempt
  std::array<uint8_t, kRecordSize> in_record{};  // rate: the record as sent
  Me me;
};

struct Completed {
  Pending pending;
  rmr::Response resp;
};

std::mutex g_done_mutex;
std::deque<Completed> g_done;

void Begin(Memory* mem, rmr::Request&& req, Pending&& p) {
  RestoreFailureText();
  // Same as the task constructor + sub_824F6D60: the task is pending until its
  // reply is applied.
  if (IsGuestPtr(p.status)) {
    GuestWrite32(mem, p.status, kPending);
    GuestWrite32(mem, p.status + 4, 0);
  }
  p.generation = g_generation.load();
  if (rmr::client::Diag()) {
    LARECOMP_APP_INFO("[rmr] {} queued -> {}", rmr::OpName(req.op),
                      rmr::client::DescribeTarget());
  }
  rmr::client::Submit(std::move(req), [p = std::move(p)](rmr::Response&& resp) {
    std::lock_guard<std::mutex> lock(g_done_mutex);
    g_done.push_back(Completed{p, std::move(resp)});
  });
}

void Apply(Memory* mem, const Pending& p, const rmr::Response& resp) {
  const char* op = rmr::OpName(p.op);
  if (resp.err != rmr::kOk) {
    LARECOMP_APP_WARN("[rmr] {} failed (code {})", op, resp.err);
    if (IsGuestPtr(p.status)) {
      GuestWrite32(mem, p.status + 4, resp.err);
      GuestWrite32(mem, p.status, kFailed);
    }
    return;
  }

  uint8_t g[kRecordSize];
  switch (p.op) {
    case rmr::Op::kTableSize:
      GuestWrite32(mem, p.count_out, resp.count);
      LARECOMP_APP_INFO("[rmr] {}: {} ranked rides", op, resp.count);
      break;

    case rmr::Op::kByOwner:
    case rmr::Op::kByRank: {
      const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(resp.records.size()), p.max_out);
      for (uint32_t i = 0; i < n; ++i) {
        EncodeGuestRecord(resp.records[i], p.me, g);
        WriteBytes(mem, p.out + kRecordSize * i, g, kRecordSize);
      }
      GuestWrite32(mem, p.count_out, n);
      LARECOMP_APP_INFO("[rmr] {}: {} record(s)", op, n);
      break;
    }

    case rmr::Op::kRandom: {
      const rmr::Record* pick = nullptr;
      for (const auto& r : resp.records) {
        if (VehicleKnown(mem, r)) {
          pick = &r;
          break;
        }
      }
      if (pick) {
        EncodeGuestRecord(*pick, p.me, g);
        WriteBytes(mem, p.out, g, kRecordSize);
      }
      GuestWrite32(mem, p.count_out, pick ? 1 : 0);
      // ReadRandomRecord(5), (10), then (0): the game gives up after the last.
      if (!pick && p.max_ratings == 0) ShowEmptyServerOnFailure(mem);
      LARECOMP_APP_INFO("[rmr] {}: {} candidate(s), picked {}", op, resp.records.size(),
                        pick ? ("'" + pick->gamertag + "'") : std::string("none"));
      break;
    }

    case rmr::Op::kRate: {
      // RateRecordCmd's completion: the record as sent, with the new totals.
      std::memcpy(g, p.in_record.data(), kRecordSize);
      rmr::StoreBe32(g + kRecNumRatings, resp.num_ratings);
      rmr::StoreBe32(g + kRecAverage, FloatBits(resp.average));
      rmr::StoreBe32(g + kRecRank, 0);
      if (IsGuestPtr(p.out)) WriteBytes(mem, p.out, g, kRecordSize);
      LARECOMP_APP_INFO("[rmr] {}: now {:.2f} over {} rating(s)", op, resp.average,
                        resp.num_ratings);
      break;
    }

    case rmr::Op::kUpload:
    case rmr::Op::kOverwrite:
    case rmr::Op::kUserData:
      if (!resp.records.empty() && IsGuestPtr(p.out)) {
        EncodeGuestRecord(resp.records[0], p.me, g);
        WriteBytes(mem, p.out, g, kRecordSize);
      }
      if (IsGuestPtr(p.progress)) GuestWriteFloat(mem, p.progress, 1.0f);
      if (!resp.records.empty()) {
        const auto& r = resp.records[0];
        LARECOMP_APP_INFO("[rmr] {}: ride {:08X} '{}' files={} ratings={}", op, r.id, r.gamertag,
                          r.file_count, r.num_ratings);
      }
      break;

    case rmr::Op::kDownload: {
      const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(resp.data.size()), p.max_out);
      if (n) WriteBytes(mem, p.out, resp.data.data(), n);
      GuestWrite32(mem, p.count_out, n);
      if (IsGuestPtr(p.progress)) GuestWriteFloat(mem, p.progress, 1.0f);
      LARECOMP_APP_INFO("[rmr] {}: {} bytes", op, n);
      break;
    }
  }

  if (IsGuestPtr(p.status)) {
    GuestWrite32(mem, p.status + 4, 0);
    GuestWrite32(mem, p.status, kSucceeded);
  }
}

// Common entry checks: feature on, guest memory up, and the RateMyRide table.
Memory* ForRmr(uint32_t table) {
  if (!rmr::client::Enabled()) return nullptr;
  Memory* mem = Mem();
  if (!mem || !IsRmrTable(mem, table)) return nullptr;
  return mem;
}

}  // namespace

// ------------------------------------------------------------------ hooks --
//
// Each hook sits on the first instruction of the task-creating function, so
// r1 is still the caller's stack pointer (stack arguments at r1+0x54 and up)
// and returning true is the function's blr with r3 = 1 ("task queued"). Bad
// arguments fall through to the original, which rejects them the same way
// without queueing anything.

// sub_824F6DE8(client, table, minRatings, ?, u32* count, status*)
bool Hook_RmrGetTableSize(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                          PPCRegister& r7, PPCRegister& r8) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r7))) return false;
  rmr::Request req;
  req.op = rmr::Op::kTableSize;
  req.min_ratings = U32(r5);
  Pending p;
  p.op = req.op;
  p.count_out = U32(r7);
  p.status = U32(r8);
  p.me = LocalPlayer(mem);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F6EA0(client, table, handle*, maxRecords, records*, u32* count, status*)
bool Hook_RmrReadByGamer(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                         PPCRegister& r7, PPCRegister& r8, PPCRegister& r9) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r5)) || U32(r6) == 0 || !IsGuestPtr(U32(r7)) ||
      !IsGuestPtr(U32(r8)))
    return false;
  Pending p;
  p.me = LocalPlayer(mem);
  rmr::Request req;
  req.op = rmr::Op::kByOwner;
  req.owner = OwnerOfGuest(mem, U32(r5), p.me);
  req.count = std::min<uint32_t>(U32(r6), kMaxRecordsOut);
  p.op = req.op;
  p.out = U32(r7);
  p.count_out = U32(r8);
  p.max_out = req.count;
  p.status = U32(r9);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F6FA0(client, table, handle*, minRatings, maxRatings, contentMask,
//              records*, u32* count, [r1+0x54] status*)
bool Hook_RmrReadRandom(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                        PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                        PPCRegister& r10) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r5)) || !IsGuestPtr(U32(r9)) || !IsGuestPtr(U32(r10))) return false;
  rmr::Request req;
  req.op = rmr::Op::kRandom;
  req.min_ratings = U32(r6);
  req.max_ratings = U32(r7);
  req.content_mask = U32(r8);
  req.count = kRandomCandidates;
  Pending p;
  p.op = req.op;
  p.out = U32(r9);
  p.count_out = U32(r10);
  p.max_out = 1;
  p.max_ratings = req.max_ratings;
  p.status = GuestRead32(mem, U32(r1) + 0x54);
  p.me = LocalPlayer(mem);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F70A8(client, table, firstRank, minRatings, count, records*, u32* count, status*)
bool Hook_RmrReadByRank(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                        PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || U32(r5) == 0 || U32(r7) == 0 || !IsGuestPtr(U32(r8)) || !IsGuestPtr(U32(r9)))
    return false;
  rmr::Request req;
  req.op = rmr::Op::kByRank;
  req.start = U32(r5);
  req.min_ratings = U32(r6);
  req.count = std::min<uint32_t>(U32(r7), kMaxRecordsOut);
  Pending p;
  p.op = req.op;
  p.out = U32(r8);
  p.count_out = U32(r9);
  p.max_out = req.count;
  p.status = U32(r10);
  p.me = LocalPlayer(mem);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F7188(client, table, record*, u8 rating, record* out, status*)
bool Hook_RmrRateRecord(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                        PPCRegister& r7, PPCRegister& r8) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r5))) return false;
  Pending p;
  ReadBytes(mem, U32(r5), p.in_record.data(), kRecordSize);
  if (!GuestRecordValid(p.in_record.data())) return false;
  p.me = LocalPlayer(mem);
  rmr::Request req;
  req.op = rmr::Op::kRate;
  req.record_id = rmr::LoadBe32(p.in_record.data() + kRecId);
  req.owner = OwnerOf(p.in_record.data(), p.me);
  req.rating = static_cast<uint8_t>(U32(r6) & 0xFF);
  p.op = req.op;
  p.out = U32(r7);
  p.status = U32(r8);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F7308(client, table, 1, handle*, gamertag, nFiles, ptrs[], sizes[],
//              [r1+0x54] userdata, [+0x5C] udSize, [+0x67] u8 content,
//              [+0x6C] record* out, [+0x74] f32* progress, [+0x7C] status*)
bool Hook_RmrUploadFile(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                        PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                        PPCRegister& r10) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem) return false;
  const uint32_t sp = U32(r1);
  const uint32_t userdata = GuestRead32(mem, sp + 0x54);
  const uint32_t ud_size = GuestRead32(mem, sp + 0x5C);
  const uint8_t content = GuestRead8(mem, sp + 0x67);
  const uint32_t out = GuestRead32(mem, sp + 0x6C);
  const uint32_t progress = GuestRead32(mem, sp + 0x74);
  if (!IsGuestPtr(U32(r6)) || !IsGuestPtr(U32(r7)) || U32(r8) == 0 || !IsGuestPtr(U32(r9)) ||
      !IsGuestPtr(U32(r10)) || !IsGuestPtr(out) || !IsGuestPtr(progress) || content >= 0x20)
    return false;

  rmr::Request req;
  req.op = rmr::Op::kUpload;
  if (!ReadUploadFiles(mem, U32(r9), U32(r10), U32(r8), req.files)) return false;
  req.gamertag = GuestCStr(mem, U32(r7), 16);
  req.content = content;
  if (IsGuestPtr(userdata) && ud_size) {
    req.userdata.resize(std::min<uint32_t>(ud_size, rmr::kUserDataMax));
    ReadBytes(mem, userdata, req.userdata.data(), req.userdata.size());
  }
  Pending p;
  p.op = req.op;
  p.out = out;
  p.progress = progress;
  p.status = GuestRead32(mem, sp + 0x7C);
  p.me = LocalPlayer(mem);
  GuestWriteFloat(mem, progress, 0.0f);
  LARECOMP_APP_INFO("[rmr] uploading '{}': car {} bytes, {} photo(s)", req.gamertag,
                    req.files[0].size(), req.files.size() - 1);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F7468(client, table, record*, nFiles, ptrs[], sizes[], userdata, udSize,
//              [r1+0x57] u8 content, [+0x5C] record* out, [+0x64] f32* progress,
//              [+0x6C] status*)
bool Hook_RmrOverwriteFile(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                           PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9,
                           PPCRegister& r10) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r5))) return false;
  const uint32_t sp = U32(r1);
  const uint8_t content = GuestRead8(mem, sp + 0x57);
  const uint32_t out = GuestRead32(mem, sp + 0x5C);
  const uint32_t progress = GuestRead32(mem, sp + 0x64);
  uint8_t record[kRecordSize];
  ReadBytes(mem, U32(r5), record, kRecordSize);
  if (!GuestRecordValid(record) || U32(r6) == 0 || !IsGuestPtr(U32(r7)) || !IsGuestPtr(U32(r8)) ||
      !IsGuestPtr(out) || !IsGuestPtr(progress) || content >= 0x20)
    return false;

  Pending p;
  p.me = LocalPlayer(mem);
  rmr::Request req;
  req.op = rmr::Op::kOverwrite;
  if (!ReadUploadFiles(mem, U32(r7), U32(r8), U32(r6), req.files)) return false;
  req.record_id = rmr::LoadBe32(record + kRecId);
  req.owner = OwnerOf(record, p.me);
  req.gamertag = !p.me.gamertag.empty()
                     ? p.me.gamertag
                     : std::string(reinterpret_cast<const char*>(record + kRecGamertag),
                                   strnlen(reinterpret_cast<const char*>(record + kRecGamertag), 16));
  req.content = content;
  if (IsGuestPtr(U32(r9)) && U32(r10)) {
    req.userdata.resize(std::min<uint32_t>(U32(r10), rmr::kUserDataMax));
    ReadBytes(mem, U32(r9), req.userdata.data(), req.userdata.size());
  }
  p.op = req.op;
  p.out = out;
  p.progress = progress;
  p.status = GuestRead32(mem, sp + 0x6C);
  GuestWriteFloat(mem, progress, 0.0f);
  LARECOMP_APP_INFO("[rmr] replacing ride {:08X} of '{}': car {} bytes, {} photo(s)",
                    req.record_id, req.gamertag, req.files[0].size(), req.files.size() - 1);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F7578(client, fileId, buffer, bufferSize, u32* size, f32* progress, status*).
// No table here: only our ids (kIdTag) are ours to serve.
bool Hook_RmrDownloadFile(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                          PPCRegister& r7, PPCRegister& r8, PPCRegister& r9) {
  if (!rmr::client::Enabled() || !rmr::IsRmrId(U32(r4))) return false;
  Memory* mem = Mem();
  if (!mem || !IsGuestPtr(U32(r5)) || U32(r6) == 0 || !IsGuestPtr(U32(r7)) ||
      !IsGuestPtr(U32(r8)))
    return false;
  rmr::Request req;
  req.op = rmr::Op::kDownload;
  req.file_id = U32(r4);
  req.max_bytes = U32(r6);
  Pending p;
  p.op = req.op;
  p.out = U32(r5);
  p.max_out = U32(r6);
  p.count_out = U32(r7);
  p.progress = U32(r8);
  p.status = U32(r9);
  p.me = LocalPlayer(mem);
  GuestWrite32(mem, p.count_out, 0);
  GuestWriteFloat(mem, p.progress, 0.0f);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_824F7638(client, table, record*, userdata, udSize, record* out, status*)
bool Hook_RmrOverwriteUserData(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                               PPCRegister& r7, PPCRegister& r8, PPCRegister& r9) {
  Memory* mem = ForRmr(U32(r4));
  if (!mem || !IsGuestPtr(U32(r5)) || !IsGuestPtr(U32(r6)) || U32(r7) == 0 ||
      !IsGuestPtr(U32(r8)))
    return false;
  uint8_t record[kRecordSize];
  ReadBytes(mem, U32(r5), record, kRecordSize);
  Pending p;
  p.me = LocalPlayer(mem);
  rmr::Request req;
  req.op = rmr::Op::kUserData;
  req.record_id = rmr::LoadBe32(record + kRecId);
  req.owner = OwnerOf(record, p.me);
  req.userdata.resize(std::min<uint32_t>(U32(r7), rmr::kUserDataMax));
  ReadBytes(mem, U32(r6), req.userdata.data(), req.userdata.size());
  p.op = req.op;
  p.out = U32(r8);
  p.status = U32(r9);
  Begin(mem, std::move(req), std::move(p));
  r3.u64 = 1;
  return true;
}

// sub_826B2328 entry: mcRateMyRide polls its task status right after this, so
// replies applied here are seen in the same frame, and always after the code
// that queued the task has finished touching its outputs.
void Hook_RmrPump(PPCRegister& r3) {
  std::deque<Completed> batch;
  {
    std::lock_guard<std::mutex> lock(g_done_mutex);
    if (g_done.empty()) return;
    batch.swap(g_done);
  }
  Memory* mem = Mem();
  if (!mem) return;
  const uint32_t gen = g_generation.load();
  for (const Completed& c : batch) {
    if (c.pending.generation != gen) {
      LARECOMP_APP_INFO("[rmr] dropped a {} reply from an earlier visit",
                        rmr::OpName(c.pending.op));
      continue;
    }
    Apply(mem, c.pending, c.resp);
  }
}

// sub_826B15B8: "[rmr] Entering Rate My Ride". Replies still in flight belong
// to the previous visit, whose state this function is about to reset.
void Hook_RmrEnter(PPCRegister& r3) {
  g_generation.fetch_add(1);
  RestoreFailureText();
  if (!rmr::client::Enabled()) return;
  Memory* mem = Mem();
  const Me me = mem ? LocalPlayer(mem) : Me{};
  LARECOMP_APP_INFO("[rmr] entering Rate My Ride as '{}' (owner {:016X}, handle {}) via {}",
                    me.gamertag, me.key, me.valid ? "ok" : "unresolved",
                    rmr::client::DescribeTarget());
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif  // REXGLUE_HAS_XEO3_TARGET
