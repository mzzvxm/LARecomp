#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Xbox LIVE leaderboards, served by our own backend (lb_client).
//
// The game's stats layer is rage::rlStats. Each leaderboard screen queues
// commands on its manager; the manager's update (sub_824FED28) starts the next
// one and, once its status leaves "pending", parses the result:
//
//   type 0 GetLeaderboardSize  sub_824FDF00  XUserCreateStatsEnumeratorByRank,
//                                            then XEnumerate -> total row count
//   type 1 ReadByRank          sub_824FE058  rows from rank *(cmd+48)
//   type 2 ReadByGamer         sub_824FE240  XUserReadStats for the handles at
//                                            *(cmd+48), count *(cmd+52)
//   type 3 ReadAroundGamer     sub_824FE4A8  ByXuid around the handle at
//                                            cmd+48, 2 * *(cmd+64) + 1 rows
//
// Command layout: +4 type, +12 status {u32 state 1 pending / 2 failed /
// 3 done, i32 code}, +28 allocator (vtable +8 alloc(this, size, 0, 0), +12
// free), +32 result buffer, +36 stats spec, +40 enumerator handle, +44 the
// leaderboard (types 1-3: its vtable +4 returns the info) or its info (type 0).
// Info vtable: +16 view id, +20 column count, +28(i) column attribute id.
// The command's destructor frees +32/+36 with the allocator and closes +40.
//
// The SDK's stats calls hand back an empty enumerator and leave the result
// buffer as allocated (0xCD fill): the boards show -842150451 for everything.
// Here each starter is replaced: it allocates the result buffer itself, marks
// the command pending and asks the backend; the manager's update, on the game
// thread, writes the XUSER_STATS_READ_RESULTS the game expects once the answer
// is in and flips the status. Nothing reaches XAM.
//
// Writes: at the end of a LIVE session (never System Link, sub_824E1050) the
// game flushes each gamer's queued leaderboard updates through
// XSessionWriteStats (sub_8252C658 -> sub_8274BDA0). Goal Attack race times
// open a short solo session for it (sub_8229CE40). The hook at the call reads
// the XSESSION_VIEW_PROPERTIES and, when the gamer is the local player, sends
// them to the backend with each column's aggregation rule from the title's SPA
// (XVC2 in the XDBF at 0x82950000). Every machine of an arbitrated (ranked)
// session writes every player; only the local player's own write counts here,
// so nothing is counted twice. Players are told apart by the rlGamerInfo gamer
// id (abEnet ^ gamertag hash), not the XUID, which older SDKs gave every
// profile of the recomp as the same constant.
//
// Result rows carry the local profile's XUID for the local player (so the game
// recognizes "you") and a made-up XUID for everyone else, remembered so a later
// read by gamer can name them again.
//

#include "../online_common.h"
#include "lb_client.h"
#include "lb_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REX_EXTERN(__imp__rex_sub_824FDF00);
REX_EXTERN(__imp__rex_sub_824FE058);
REX_EXTERN(__imp__rex_sub_824FE240);
REX_EXTERN(__imp__rex_sub_824FE4A8);
REX_EXTERN(__imp__rex_sub_824FED28);

namespace {

constexpr uint32_t kSpaAddress = 0x82950000;   // the XEX's XDBF resource
constexpr uint32_t kNetMgrPtr = 0x8286D800;
constexpr uint32_t kLocalUserPtr = 0x8288E5AC;
constexpr uint32_t kGamersAt = 4284;           // netmgr: gamer array
constexpr uint32_t kGamerStride = 10832;
constexpr uint32_t kGamerIdAt = 16 + 48;       // gamer: rlGamerInfo (+16), its gamer id (+48)
constexpr uint32_t kGamertagAt = 92;
constexpr uint64_t kLegacyLocalXuid = 0xB13EBABEBABEBABEull;  // SDKs without per-profile XUIDs
constexpr uint64_t kOtherXuidTag = 0x0009000000000000ull;
constexpr uint16_t kAttrHidden = 0xFFFA;       // 0x10008011, internal to LIVE

constexpr uint32_t kStatusPending = 1;
constexpr uint32_t kStatusFailed = 2;
constexpr uint32_t kStatusDone = 3;

constexpr uint32_t kResultsSize = 8;   // XUSER_STATS_READ_RESULTS
constexpr uint32_t kViewSize = 16;     // XUSER_STATS_VIEW
constexpr uint32_t kRowSize = 48;      // XUSER_STATS_ROW
constexpr uint32_t kColumnSize = 24;   // XUSER_STATS_COLUMN

// ── guest memory ───────────────────────────────────────────────────────────
uint8_t* Base() {
  auto* rt = rex::Runtime::instance();
  return rt ? rt->virtual_membase() : nullptr;
}
uint32_t R32(uint8_t* b, uint32_t a) {
  const uint8_t* p = b + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t R16(uint8_t* b, uint32_t a) { return uint16_t((b[a] << 8) | b[a + 1]); }
uint64_t R64(uint8_t* b, uint32_t a) { return (uint64_t(R32(b, a)) << 32) | R32(b, a + 4); }
void W32(uint8_t* b, uint32_t a, uint32_t v) {
  b[a] = uint8_t(v >> 24); b[a + 1] = uint8_t(v >> 16); b[a + 2] = uint8_t(v >> 8); b[a + 3] = uint8_t(v);
}
void W16(uint8_t* b, uint32_t a, uint16_t v) { b[a] = uint8_t(v >> 8); b[a + 1] = uint8_t(v); }
void W64(uint8_t* b, uint32_t a, uint64_t v) { W32(b, a, uint32_t(v >> 32)); W32(b, a + 4, uint32_t(v)); }

// Calls the guest function at `fn` (an address) with up to four integer args.
uint32_t CallGuest(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
  auto* rt = rex::Runtime::instance();
  PPCFunc* host = rt && rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(fn) : nullptr;
  if (!host) return 0;
  return rex::ppc::GuestToHostFunction<uint32_t>(host, a0, a1, a2, a3);
}

uint32_t VCall(uint8_t* b, uint32_t obj, uint32_t slot, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
  if (!IsGuestPtr(obj)) return 0;
  const uint32_t vt = R32(b, obj);
  if (!IsGuestPtr(vt)) return 0;
  return CallGuest(R32(b, vt + slot), obj, a1, a2, a3);
}

// ── the title's stats views (XVC2 in the SPA) ──────────────────────────────
struct ColumnDef {
  uint32_t prop = 0;
  uint16_t attr = 0;
  uint8_t agg = 0;
};
struct ViewDef {
  std::vector<ColumnDef> columns;
};

std::once_flag g_schema_once;
std::unordered_map<uint32_t, ViewDef> g_views;

void LoadSchema() {
  uint8_t* b = Base();
  if (!b || std::memcmp(b + kSpaAddress, "XDBF", 4) != 0) {
    LARECOMP_APP_WARN("[leaderboards] no XDBF at {:08X}; writes cannot be mapped", kSpaAddress);
    return;
  }
  const uint32_t etl = R32(b, kSpaAddress + 8);
  const uint32_t ec = R32(b, kSpaAddress + 12);
  const uint32_t fstl = R32(b, kSpaAddress + 16);
  const uint32_t data = kSpaAddress + 0x18 + etl * 18 + fstl * 8;
  uint32_t xvc2 = 0, xvc2_len = 0;
  for (uint32_t i = 0; i < ec && i < etl; ++i) {
    const uint32_t e = kSpaAddress + 0x18 + i * 18;
    if (R16(b, e) == 1 && R64(b, e + 2) == 0x58564332ull) {  // section 1, "XVC2"
      xvc2 = data + R32(b, e + 10);
      xvc2_len = R32(b, e + 14);
    }
  }
  if (!xvc2 || std::memcmp(b + xvc2, "XVC2", 4) != 0) {
    LARECOMP_APP_WARN("[leaderboards] the SPA has no XVC2 section");
    return;
  }
  const uint32_t end = xvc2 + xvc2_len;
  uint32_t p = xvc2 + 12;
  const uint16_t nshared = R16(b, p);
  p += 2;
  std::vector<std::vector<ColumnDef>> shared;
  for (uint16_t s = 0; s < nshared && p < end; ++s) {
    const uint16_t ncol = R16(b, p), nrow = R16(b, p + 2);
    p += 12;
    std::vector<ColumnDef> cols;
    for (uint16_t c = 0; c < ncol; ++c, p += 32) {
      ColumnDef d;
      d.prop = R32(b, p + 4);
      d.attr = R16(b, p + 12);
      d.agg = static_cast<uint8_t>(R16(b, p + 16) & 0xFF);
      cols.push_back(d);
    }
    p += 32u * nrow;
    if (std::memcmp(b + p, "XPBM", 4) != 0) {
      LARECOMP_APP_WARN("[leaderboards] XVC2 shared view {} is not followed by XPBM", s);
      return;
    }
    p += 20 + 4 * R32(b, p + 16);
    shared.push_back(std::move(cols));
  }
  const uint16_t nviews = R16(b, p);
  p += 2;
  for (uint16_t v = 0; v < nviews && p + 16 <= end; ++v, p += 16) {
    const uint32_t id = R32(b, p);
    const uint16_t si = R16(b, p + 8);
    if (si < shared.size()) g_views[id].columns = shared[si];
  }
  LARECOMP_APP_INFO("[leaderboards] {} stats views from the SPA ({} column sets), backend: {}",
                    g_views.size(), shared.size(), lb::client::DescribeTarget());
}

const ViewDef* FindView(uint32_t id) {
  std::call_once(g_schema_once, LoadSchema);
  auto it = g_views.find(id);
  return it == g_views.end() ? nullptr : &it->second;
}

uint8_t TypeOfAttr(const ViewDef* def, uint16_t attr) {
  if (def) {
    for (const ColumnDef& c : def->columns) {
      if (c.attr == attr) return static_cast<uint8_t>(c.prop >> 28);
    }
  }
  return lb::kTypeInt32;
}

// ── the local player ───────────────────────────────────────────────────────
uint32_t LocalGamer(uint8_t* b) {
  const uint32_t netmgr = R32(b, kNetMgrPtr);
  const uint32_t p0 = R32(b, kLocalUserPtr);
  const uint32_t p1 = IsGuestPtr(p0) ? R32(b, p0) : 0;
  const uint32_t user = IsGuestPtr(p1) ? R32(b, p1) : 0;
  if (!IsGuestPtr(netmgr) || !IsGuestPtr(user)) return 0;
  const uint32_t gamers = R32(b, netmgr + kGamersAt);
  if (!IsGuestPtr(gamers)) return 0;
  return gamers + kGamerStride * R32(b, user + 4);
}

std::string LocalGamertag(uint8_t* b) {
  const uint32_t g = LocalGamer(b);
  if (!g) return {};
  std::string tag;
  for (uint32_t i = 0; i < 16; ++i) {
    const char c = static_cast<char>(b[g + kGamertagAt + i]);
    if (!c) break;
    tag.push_back(c);
  }
  return tag;
}

// ── XUIDs handed to the game ───────────────────────────────────────────────
std::mutex g_xuid_mutex;
std::unordered_map<uint64_t, uint64_t> g_owner_by_xuid;  // made-up XUID -> owner

// The XUID the game itself signed in with.
uint64_t LocalXuid() {
  auto* ks = rex::system::kernel_state();
  const auto* profile = ks ? ks->user_profile() : nullptr;
  return profile ? profile->xuid() : kLegacyLocalXuid;
}

uint64_t XuidForOwner(uint64_t owner) {
  if (owner == lb::client::PlayerOwnerKey()) return LocalXuid();
  const uint64_t xuid = kOtherXuidTag | (owner & 0x0000FFFFFFFFFFFFull);
  std::lock_guard<std::mutex> lock(g_xuid_mutex);
  g_owner_by_xuid[xuid] = owner;
  return xuid;
}

bool OwnerForXuid(uint64_t xuid, uint64_t& owner) {
  if (xuid == LocalXuid() || xuid == kLegacyLocalXuid) {
    owner = lb::client::PlayerOwnerKey();
    return true;
  }
  std::lock_guard<std::mutex> lock(g_xuid_mutex);
  auto it = g_owner_by_xuid.find(xuid);
  if (it == g_owner_by_xuid.end()) return false;
  owner = it->second;
  return true;
}

// ── reads in flight ────────────────────────────────────────────────────────
struct Pending {
  uint32_t mgr = 0;
  uint32_t cmd = 0;
  uint32_t buffer = 0;
  uint32_t type = 0;
  uint32_t view = 0;
  uint32_t capacity = 0;            // rows the buffer holds
  std::vector<uint16_t> attrs;      // columns of each row, rating excluded
  bool answered = false;
  lb::Response resp;
};

std::mutex g_pending_mutex;
std::unordered_map<uint32_t, Pending> g_pending;  // by command

uint32_t BufferSize(uint32_t rows, size_t columns) {
  return kResultsSize + kViewSize + rows * (kRowSize + static_cast<uint32_t>(columns) * kColumnSize);
}

// Allocates the command's result buffer with its own allocator, so its
// destructor frees it the way it frees the stock one.
uint32_t AllocResults(uint8_t* b, uint32_t cmd, uint32_t size) {
  const uint32_t alloc = R32(b, cmd + 28);
  const uint32_t buf = VCall(b, alloc, 8, size, 0, 0);
  if (buf) std::memset(b + buf, 0, size);
  return buf;
}

void WriteResults(uint8_t* b, const Pending& p) {
  const ViewDef* def = FindView(p.view);
  const uint32_t view = p.buffer + kResultsSize;
  const uint32_t rows = std::min<uint32_t>(static_cast<uint32_t>(p.resp.rows.size()), p.capacity);
  const uint32_t row0 = view + kViewSize;
  const uint32_t col0 = row0 + p.capacity * kRowSize;
  W32(b, p.buffer + 0, 1);
  W32(b, p.buffer + 4, view);
  W32(b, view + 0, p.view);
  W32(b, view + 4, p.resp.total);
  W32(b, view + 8, rows);
  W32(b, view + 12, rows ? row0 : 0);
  for (uint32_t i = 0; i < rows; ++i) {
    const lb::Row& r = p.resp.rows[i];
    const uint32_t row = row0 + i * kRowSize;
    const uint32_t cols = col0 + i * static_cast<uint32_t>(p.attrs.size()) * kColumnSize;
    W64(b, row + 0, XuidForOwner(r.owner));
    W32(b, row + 8, r.rank);
    W64(b, row + 16, static_cast<uint64_t>(r.rating));
    std::memset(b + row + 24, 0, 16);
    std::memcpy(b + row + 24, r.gamertag.data(), std::min<size_t>(r.gamertag.size(), 15));
    W32(b, row + 40, static_cast<uint32_t>(p.attrs.size()));
    W32(b, row + 44, p.attrs.empty() ? 0 : cols);
    for (size_t k = 0; k < p.attrs.size(); ++k) {
      const uint32_t c = cols + static_cast<uint32_t>(k) * kColumnSize;
      const uint16_t attr = p.attrs[k];
      uint8_t type = TypeOfAttr(def, attr);
      int64_t bits = 0;
      if (attr == lb::kAttrRank) {
        type = lb::kTypeInt32;
        bits = r.rank;
      } else {
        for (const lb::Value& v : r.columns) {
          if (v.attr == attr) {
            type = v.type;
            bits = v.bits;
            break;
          }
        }
      }
      W16(b, c + 0, attr);
      b[c + 8] = type;
      if (type == lb::kTypeInt64 || type == lb::kTypeDouble) {
        W64(b, c + 16, static_cast<uint64_t>(bits));
      } else {
        W32(b, c + 16, static_cast<uint32_t>(bits));
      }
    }
  }
}

// Shared tail of the four starters: buffer, pending status, request.
bool StartRead(uint8_t* b, uint32_t mgr, uint32_t cmd, uint32_t type, uint32_t view,
               uint32_t capacity, std::vector<uint16_t> attrs, lb::Request&& req) {
  const uint32_t size = BufferSize(capacity, attrs.size());
  const uint32_t buf = AllocResults(b, cmd, size);
  if (!buf) {
    LARECOMP_APP_WARN("[leaderboards] {} view {}: cannot allocate {} bytes", lb::OpName(req.op),
                      view, size);
    return false;
  }
  W32(b, cmd + 32, buf);
  W32(b, cmd + 12, kStatusPending);
  W32(b, cmd + 16, 0);
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    Pending& p = g_pending[cmd];
    p = Pending{};
    p.mgr = mgr;
    p.cmd = cmd;
    p.buffer = buf;
    p.type = type;
    p.view = view;
    p.capacity = capacity;
    p.attrs = std::move(attrs);
  }
  if (lb::client::Diag()) {
    LARECOMP_APP_INFO("[leaderboards] {} view {} start {} count {} (cmd {:08X})", lb::OpName(req.op),
                      view, req.start, req.count, cmd);
  }
  req.view = view;
  lb::client::Submit(std::move(req), [cmd, buf](lb::Response&& resp) {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    auto it = g_pending.find(cmd);
    if (it == g_pending.end() || it->second.buffer != buf) return;
    it->second.resp = std::move(resp);
    it->second.answered = true;
  });
  return true;
}

// The leaderboard's view id and the columns its rows carry (rating excluded,
// it travels in the row itself).
uint32_t ReadLayout(uint8_t* b, uint32_t leaderboard, std::vector<uint16_t>& attrs) {
  const uint32_t info = VCall(b, leaderboard, 4);
  if (!IsGuestPtr(info)) return 0;
  const uint32_t view = VCall(b, info, 16);
  const uint32_t ncols = std::min<uint32_t>(VCall(b, info, 20), lb::kMaxColumns);
  for (uint32_t i = 0; i < ncols; ++i) {
    const uint16_t attr = static_cast<uint16_t>(VCall(b, info, 28, i));
    if (attr != lb::kAttrRating) attrs.push_back(attr);
  }
  return view;
}

std::atomic<bool> g_logged_first_read{false};

void LogFirstRead() {
  if (!g_logged_first_read.exchange(true)) {
    std::call_once(g_schema_once, LoadSchema);
  }
}

}  // namespace

// rlStats GetLeaderboardSize: *(cmd+44) is the leaderboard info.
extern "C" REX_FUNC(rex_sub_824FDF00) {
  uint8_t* b = Base();
  if (!lb::client::Enabled() || !b) {
    __imp__rex_sub_824FDF00(ctx, base);
    return;
  }
  LogFirstRead();
  const uint32_t mgr = ctx.r3.u32, cmd = ctx.r4.u32;
  const uint32_t view = VCall(b, R32(b, cmd + 44), 16);
  lb::Request req;
  req.op = lb::Op::kSize;
  ctx.r3.u64 = StartRead(b, mgr, cmd, 0, view, 0, {}, std::move(req)) ? 1 : 0;
}

// rlStats ReadByRank: `capacity` rows (*(leaderboard+8)) from rank *(cmd+48).
extern "C" REX_FUNC(rex_sub_824FE058) {
  uint8_t* b = Base();
  if (!lb::client::Enabled() || !b) {
    __imp__rex_sub_824FE058(ctx, base);
    return;
  }
  LogFirstRead();
  const uint32_t mgr = ctx.r3.u32, cmd = ctx.r4.u32;
  const uint32_t leaderboard = R32(b, cmd + 44);
  std::vector<uint16_t> attrs;
  const uint32_t view = ReadLayout(b, leaderboard, attrs);
  const uint32_t capacity = std::min<uint32_t>(R32(b, leaderboard + 8), lb::kMaxRows);
  lb::Request req;
  req.op = lb::Op::kByRank;
  req.start = std::max<uint32_t>(R32(b, cmd + 48), 1);
  req.count = capacity;
  ctx.r3.u64 = StartRead(b, mgr, cmd, 1, view, capacity, std::move(attrs), std::move(req)) ? 1 : 0;
}

// rlStats ReadByGamer (XUserReadStats): the gamer handles at *(cmd+48), 16
// bytes each with the XUID at +8, count *(cmd+52).
extern "C" REX_FUNC(rex_sub_824FE240) {
  uint8_t* b = Base();
  if (!lb::client::Enabled() || !b) {
    __imp__rex_sub_824FE240(ctx, base);
    return;
  }
  LogFirstRead();
  const uint32_t mgr = ctx.r3.u32, cmd = ctx.r4.u32;
  std::vector<uint16_t> attrs;
  const uint32_t view = ReadLayout(b, R32(b, cmd + 44), attrs);
  const uint32_t handles = R32(b, cmd + 48);
  const uint32_t count = std::min<uint32_t>(R32(b, cmd + 52), lb::kMaxOwners);
  lb::Request req;
  req.op = lb::Op::kByOwners;
  for (uint32_t i = 0; IsGuestPtr(handles) && i < count; ++i) {
    uint64_t owner;
    if (OwnerForXuid(R64(b, handles + 16 * i + 8), owner)) req.owners.push_back(owner);
  }
  req.count = count;
  ctx.r3.u64 = StartRead(b, mgr, cmd, 2, view, std::max<uint32_t>(count, 1), std::move(attrs),
                         std::move(req))
                   ? 1
                   : 0;
}

// rlStats read around a gamer (ByXuid): the handle at cmd+48 (XUID at +8),
// 2 * *(cmd+64) + 1 rows.
extern "C" REX_FUNC(rex_sub_824FE4A8) {
  uint8_t* b = Base();
  if (!lb::client::Enabled() || !b) {
    __imp__rex_sub_824FE4A8(ctx, base);
    return;
  }
  LogFirstRead();
  const uint32_t mgr = ctx.r3.u32, cmd = ctx.r4.u32;
  std::vector<uint16_t> attrs;
  const uint32_t view = ReadLayout(b, R32(b, cmd + 44), attrs);
  const uint32_t rows = std::min<uint32_t>(2 * R32(b, cmd + 64) + 1, lb::kMaxRows);
  lb::Request req;
  req.op = lb::Op::kByPivot;
  req.count = rows;
  uint64_t owner = 0;
  if (OwnerForXuid(R64(b, cmd + 56), owner)) req.pivot = owner;
  ctx.r3.u64 = StartRead(b, mgr, cmd, 3, view, rows, std::move(attrs), std::move(req)) ? 1 : 0;
}

// rlStats manager update: lands the answers before it looks at the status.
extern "C" REX_FUNC(rex_sub_824FED28) {
  uint8_t* b = Base();
  const uint32_t mgr = ctx.r3.u32;
  if (b) {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      Pending& p = it->second;
      if (p.mgr != mgr || !p.answered) {
        ++it;
        continue;
      }
      // Only while the command is still the current one, still pending, and
      // still holds our buffer: a command freed meanwhile is left alone.
      const bool live = R32(b, mgr + 48) == p.cmd && R32(b, p.cmd + 32) == p.buffer &&
                        R32(b, p.cmd + 12) == kStatusPending;
      if (live) {
        const bool ok = p.resp.err == lb::kOk;
        if (ok) WriteResults(b, p);
        W32(b, p.cmd + 16, ok ? 0u : 0xFFFFFFFFu);
        W32(b, p.cmd + 12, ok ? kStatusDone : kStatusFailed);
        if (lb::client::Diag() || !ok) {
          LARECOMP_APP_INFO("[leaderboards] view {} type {}: {} of {} row(s){}", p.view, p.type,
                            p.resp.rows.size(), p.resp.total,
                            ok ? "" : fmt::format(" FAILED (err {})", p.resp.err));
        }
      }
      it = g_pending.erase(it);
    }
  }
  __imp__rex_sub_824FED28(ctx, base);
}

// sub_8252C658 at `bl XSessionWriteStats` (0x8252C828): r23 = the write task,
// r25 = the gamer's session record (gamer id at +8), r5 = view count, r6 =
// XSESSION_VIEW_PROPERTIES[] {view id, property count, XUSER_PROPERTY*} with
// XUSER_PROPERTY {id, (type at +8), value at +16}.
void Hook_LeaderboardWrite(PPCRegister& r25, PPCRegister& r5, PPCRegister& r6) {
  uint8_t* b = Base();
  if (!lb::client::Enabled() || !b) return;
  const uint32_t record = r25.u32;
  const uint32_t local = LocalGamer(b);
  if (!IsGuestPtr(record) || !local) return;
  const uint64_t gamer_id = R64(b, record + 8);
  const uint64_t local_id = R64(b, local + kGamerIdAt);
  if (gamer_id != local_id) {
    if (lb::client::Diag()) {
      LARECOMP_APP_INFO("[leaderboards] write for gamer {:016X} skipped (local is {:016X})",
                        gamer_id, local_id);
    }
    return;
  }
  const std::string tag = LocalGamertag(b);
  const uint32_t nviews = std::min<uint32_t>(r5.u32, 64);
  const uint32_t views = r6.u32;
  for (uint32_t v = 0; IsGuestPtr(views) && v < nviews; ++v) {
    const uint32_t entry = views + 12 * v;
    const uint32_t view_id = R32(b, entry);
    const uint32_t nprops = std::min<uint32_t>(R32(b, entry + 4), lb::kMaxColumns);
    const uint32_t props = R32(b, entry + 8);
    const ViewDef* def = FindView(view_id);
    if (!def || !IsGuestPtr(props)) {
      LARECOMP_APP_WARN("[leaderboards] write to unknown view {} ignored", view_id);
      continue;
    }
    lb::Request req;
    req.op = lb::Op::kWrite;
    req.view = view_id;
    req.gamertag = tag;
    std::string detail;
    for (uint32_t i = 0; i < nprops; ++i) {
      const uint32_t p = props + 24 * i;
      const uint32_t prop = R32(b, p);
      const uint8_t type = b[p + 8];
      const int64_t bits = (type == lb::kTypeInt64 || type == lb::kTypeDouble)
                               ? static_cast<int64_t>(R64(b, p + 16))
                               : (type == lb::kTypeInt32 ? static_cast<int64_t>(static_cast<int32_t>(R32(b, p + 16)))
                                                         : static_cast<int64_t>(R32(b, p + 16)));
      for (const ColumnDef& c : def->columns) {
        if (c.prop != prop || c.attr == lb::kAttrRank || c.attr == lb::kAttrGamerName ||
            c.attr == kAttrHidden) {
          continue;
        }
        lb::Value val;
        val.attr = c.attr;
        val.type = type;
        val.agg = c.agg;
        val.bits = bits;
        req.columns.push_back(val);
        detail += fmt::format(" {:X}={}", c.attr, bits);
      }
    }
    if (req.columns.empty()) continue;
    LARECOMP_APP_INFO("[leaderboards] {} posts view {}:{}", tag, view_id, detail);
    lb::client::Submit(std::move(req), [view_id](lb::Response&& resp) {
      if (resp.err != lb::kOk) {
        LARECOMP_APP_WARN("[leaderboards] write to view {} failed (err {})", view_id, resp.err);
      }
    });
  }
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_LeaderboardWrite(PPCRegister& r25, PPCRegister& r5, PPCRegister& r6) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
