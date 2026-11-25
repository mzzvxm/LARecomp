#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Rigged car configs: broken vinyl layers and tuning values the game cannot make.
//
// Saves passed around to "brick" online lobbies carry cars built by editing the
// save. Two kinds of damage, both sent to everyone in the session:
//
// 1. Vinyl layers that are garbage: half-floats that are NaN or infinite, and shape ids that are
// not in the game's shape catalog (41398, 35253, 44544...). The player who
// owns such a car sends it to everyone in the session, and the game takes it
// as it comes: mcCarConfig::Import (sub_82390940) reads the bitstream
// (sub_82395A50) into a CarConfig with no range check at all -- the shape id
// is 16 bits (sub_823926C0), scale and position are the raw 32 bits of two
// halves each, skew the raw 16 bits of one.
//
// A layer is 20 bytes; the CarConfig keeps 288 of them from +0x904 (the same
// offset in the save's garage block and in the network import's object):
//   +0 half scale x, +2 scale y, +4 pos x, +6 pos y, +8 u32 color,
//   +12 half skew, +14 half rotation, +16 s16 shape id (-1 = empty),
//   +18 u8 negate alpha, +19 u8 is-vector.
// The five surfaces use the slots the tables at 0x820510B0 (capacity) and
// 0x827E9770 (first slot) give, and the game keeps each surface's used slots
// first.
//
// The shape catalog (*0x828CD0F8, built by sub_8239A5D8 from
// tune/vehicle/vinyl_shapes.list): 23 groups at +96, 8 bytes each {names*,
// u16 count, u16 capacity}. A shape id is group * 1000 + index.
//
// A layer whose shape is not in the catalog, or whose scale, position, skew or
// rotation is not a finite number, is cleared the way sub_8236E2E8 clears one
// (shape -1, color -1), and the surface is packed again.
//
// 2. Tuning values the game can never make ("brick", "drift", "unflippable",
// "sticky" cars). Only values that break something are touched; every bound
// below is the game's own, checked against the 686 stock configs in
// resources/vehicle/configs (all of them inside it). The physics setup
// (sub_82361988, a3 = the CarConfig) blends every value between the stock car
// and the fully upgraded one by a percentage, and multiplies some directly:
//   +28..+40   part states: EngineState0-6, TransState0-1, SuspensionState0-1,
//              BrakeState0, TireState0 (named by sub_82398390). Each one picks a
//              percentage out of its part's 32-byte record in the mods tune
//              (sub_823849C0: record + 4 + 2 * state). Every part of every car in
//              tune/garage/*_performance.gtun has two upgrades for sale, so a state
//              is 0 (stock) .. 2 -- the garage's levels 1 .. 3, which is how the
//              notice shows them; 201
//              reads 400 bytes past the record and the blend runs far beyond
//              the upgraded car.
//   +59        EstimatedModValue (mcRacer::GetEsitmatedModValue): 0..100 once the
//              garage worked it out, -1 before that (the constructor
//              sub_82398118 and every stock config).
//   +61/62/63  signed byte / 100 multiplying the tires' grip and one more
//              factor (sub_82361988's tail). Only the constructor writes them,
//              with 100; -1, -112 or 36 come from a save editor (negative grip =
//              "sticky"/"spazz", low rear = drift).
//   +0x830     the wheel block (named by sub_82394388), per axle. Only the values
//              that index the game's float tables are bounded, to the tables'
//              length: RideHeight 0..11 (0x820511F4, 12 entries), TireWidth 0..16
//              (0x827E98B0, 17), RimSize 12..28 (flt_827E9A50[size - 12], 17),
//              TireProfile 0..13 (0x8205110C, 14) -- sub_82392F08 turns them into
//              the tire radius, and a RimSize of 1 reads the rim radius 11 floats
//              before the table: the "brick" wheel. Camber0/1 are floats and only
//              a NaN, an infinity or more than 45 degrees is put back. TireOffset
//              is left alone: the stock traffic cars carry 7 and 8.
//   +0x660     PaintType of the six paint layers, an index into the game's six
//              paint kinds (Gloss .. Enamel, sub_82399618); the bitstream allows 7.
// A value out of range is put back to the nearest one the game allows (the
// grip factors to 100). Values in range are never touched.
//
// Both checks run
//   - inside mcCarConfig::Import, right after the bitstream read (0x82390984):
//     every car that comes from outside -- another player's (sub_8229B4F0),
//     a Rate My Ride ride (sub_826B1C88) -- passes there before the game uses it;
//   - on the garage when the save is read (vehicle_records.cpp).
//
// The import check is the one that protects a session, and it is always on: it
// runs on the machine that RECEIVES the car, so a sender who builds the game
// without it still cannot get a broken car into anyone else's game. The garage
// check only spares the player their own crashes and tells them why.
//
// The player is told through one of the game's own message boxes,
// "UnsupportedGamerChars" (tune/ui/net/dialog.sc.xml: title msg_ol_warning,
// description msg_ol_unsupportedgamerchars, OK closes it), with what was changed
// listed under the warning ("rim size: 92 -> 28"). A garage car's notice waits
// until that car is the one being driven: fixing the 15th car of the garage says
// nothing to a player who drives the car they just bought. Its description entry
// points at our text while it is up and back at the game's after.
//
// Registered as midasm hooks in larecomp_config.toml.
//

#include "car_guard.h"
#include "online_common.h"
#include "mc_engine/hooks/owned_cars.h"
#include "mc_engine/keyboard_native.h"  // MenuScreenUp

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/memory/endian.h>
#include <rex/kernel/heap.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>

namespace larecomp::car_guard {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kLayersAt = 0x904;
constexpr uint32_t kLayerSize = 20;
constexpr uint32_t kLayerSlots = 288;
constexpr uint32_t kSurfaceCaps = 0x820510B0;
constexpr uint32_t kSurfaceFirst = 0x827E9770;
constexpr int kSurfaces = 5;

constexpr uint32_t kCatalogPtr = 0x828CD0F8;
constexpr uint32_t kCatalogGroups = 96;
constexpr uint32_t kCatalogGroupCount = 23;

constexpr uint32_t kProfile = 0x8288E920;      // LocalOptions
constexpr uint32_t kOwnedCount = kProfile + 1340;  // s32
constexpr uint32_t kCurrentSlot = kProfile + 1344;  // s32
constexpr uint32_t kCarVehicleName = 0x1FAD;   // char[32], "vp_aud_r8_08"
constexpr uint32_t kCarDisplayName = 0x1FCD;   // char[34], "83 GOLF/GTI"
constexpr uint32_t kCarPlate = 64 + 2055;      // char[7]

// Tuning (offsets from the CarConfig, see the top of the file).
constexpr uint32_t kPartStates = 28;
constexpr uint8_t kPartStateMax = 2;
constexpr uint32_t kEstimatedModValue = 59;
constexpr uint32_t kGripFactors = 61;  // 61, 62, 63
constexpr uint8_t kGripDefault = 100;
constexpr uint32_t kWheels = 64 + 2032;
constexpr uint32_t kPaintLayers = 64 + 1568;
constexpr uint32_t kPaintLayerSize = 48;
constexpr uint32_t kPaintType = 32;
constexpr uint32_t kPaintKinds = 6;

// sub_82398390's part states, by what they upgrade.
constexpr const char* kPartLabel[13] = {
    "engine upgrade", "engine upgrade", "engine upgrade", "engine upgrade",
    "engine upgrade", "engine upgrade", "engine upgrade", "transmission upgrade",
    "transmission upgrade", "suspension upgrade", "suspension upgrade", "brake upgrade",
    "tire upgrade"};
// sub_82399618's paint kinds.
constexpr const char* kPaintName[kPaintKinds] = {"Gloss", "Metallic", "ColorShift",
                                                 "Pearlescent", "Matte", "Enamel"};

// UI (see browser_join.cpp / photo_export.cpp).
constexpr uint32_t kUiManagerPtr = 0x8286D804;
constexpr uint32_t kFindStateFn = 0x82691650;      // (hsm, name) -> state
// UI.ActivateAndFocus(state) (sub_821F8A58, command 14) is three calls on the
// state: vtable +108 (activate), +64 with 1 (focus), +96 with 1 (show).
constexpr uint32_t kStateActivate = 108;
constexpr uint32_t kStateFocus = 64;
constexpr uint32_t kStateShow = 96;
constexpr uint32_t kGuestNewFn = 0x82130528;
constexpr char kDialogState[] = "UnsupportedGamerChars";
constexpr char kDialogKey[] = "msg_ol_unsupportedgamerchars";

// The string manager (*0x8286D7FC). A lookup (sub_82218268) hands back the
// table entry's text itself (entry+8), however long, unless the key also has a
// buffer in the display cache at +16 (an atMap {node** buckets, u16 count},
// node {hash, char* buffer, next}): the cache only gets keys that were missing
// from the table when asked for (sub_82217CA0), and its buffers are 132 bytes
// that the refresh (sub_822180B0) fills with no bound. So the notice may be as
// long as it likes while the key has no cache buffer, and 127 bytes when it has.
constexpr uint32_t kStringTableMgr = 0x8286D7FC;
constexpr uint32_t kMgrTable = 4;
constexpr uint32_t kMgrCache = 16;
constexpr uint32_t kEntryText = 8;
constexpr size_t kCachedTextMax = 127;
constexpr size_t kTextMax = 600;
constexpr size_t kMaxLines = 6;

// FlashNavigator (*mcUIManager + 52): its FIFO of modal message boxes, count 0..3
// (photo_album.cpp, keyboard_native.cpp). A UIMessageBox that opened sits there.
constexpr uint32_t kNavLayerCount = 263992;

// What a scrub changed, one line per kind of value ("rim size: 92 -> 28"). A
// kind is listed once, with the first value that was out of range.
struct Report {
  std::vector<std::string> lines;
  int count = 0;  // values changed, vinyl layers included
  void Add(const std::string& label, const std::string& from, const std::string& to) {
    ++count;
    const std::string head = label + ": ";
    for (const std::string& l : lines)
      if (l.compare(0, head.size(), head) == 0) return;
    lines.push_back(head + from + " -> " + to);
  }
  explicit operator bool() const { return count != 0; }
  std::string Joined() const {
    std::string s;
    for (const std::string& l : lines) s += (s.empty() ? "" : ", ") + l;
    return s;
  }
};

// A garage car fixed when the save was read, told about once it is driven.
// The slot can move (a car sold before it), so the car is known by its names
// and plate too.
struct Pending {
  uint32_t slot = 0;
  std::string vehicle, display, plate;
  Report report;
};

std::mutex g_mutex;
std::vector<Pending> g_garage;      // under g_mutex
bool g_theirs_pending = false;      // under g_mutex
uint32_t g_theirs_cfg = 0;          // under g_mutex: the imported CarConfig
Report g_theirs;                    // under g_mutex
using GuestMemory = rex::kernel::GuestAddressSpace;

int g_theirs_more = 0;              // under g_mutex: rigged cars after the first
Clock::time_point g_last_theirs{};  // under g_mutex

GuestMemory* Mem() {
  return larecomp::guest::AddressSpace();
}

template <typename... Args>
uint32_t CallGuest(uint32_t fn_addr, Args... args) {
  auto* rt = REX_RUNTIME();
  PPCFunc* fn = rt ? rt->FindFunction(fn_addr).value_or(nullptr) : nullptr;
  if (!fn) return 0;
  return larecomp::guest::Call<uint32_t>(fn, static_cast<uint32_t>(args)...);
}

// ---------------------------------------------------------------- vinyl ---

bool HalfFinite(uint16_t h) { return (h & 0x7C00) != 0x7C00; }

bool ShapeKnown(GuestMemory* mem, uint16_t raw) {
  const int16_t id = static_cast<int16_t>(raw);
  if (id < 0) return false;
  const uint32_t group = static_cast<uint32_t>(id) / 1000;
  const uint32_t index = static_cast<uint32_t>(id) % 1000;
  if (group >= kCatalogGroupCount) return false;
  const uint32_t catalog = GuestRead32(mem, kCatalogPtr);
  if (!IsGuestPtr(catalog)) return true;  // not built yet: the group is all we can check
  return index < GuestRead16(mem, catalog + kCatalogGroups + 8 * group + 4);
}

bool LayerBroken(GuestMemory* mem, uint32_t layer) {
  const uint16_t shape = GuestRead16(mem, layer + 16);
  if (shape == 0xFFFF) return false;
  static constexpr uint32_t kHalves[] = {0, 2, 4, 6, 12, 14};
  for (uint32_t at : kHalves)
    if (!HalfFinite(GuestRead16(mem, layer + at))) return true;
  return !ShapeKnown(mem, shape);
}

// sub_8236E2E8's empty layer.
void ClearLayer(GuestMemory* mem, uint32_t layer) {
  for (uint32_t at = 0; at < 8; at += 2) GuestWrite16(mem, layer + at, 0);
  GuestWrite32(mem, layer + 8, 0xFFFFFFFFu);
  GuestWrite16(mem, layer + 12, 0);
  GuestWrite16(mem, layer + 14, 0);
  GuestWrite16(mem, layer + 16, 0xFFFF);
  GuestWrite8(mem, layer + 18, 0);
  GuestWrite8(mem, layer + 19, 0);
}

// Moves a surface's used layers back to its first slots, in order.
void PackSurface(GuestMemory* mem, uint32_t layers, uint32_t first, uint32_t cap) {
  uint8_t* host = mem->TranslateVirtual<uint8_t*>(layers);
  uint32_t out = first;
  for (uint32_t k = first; k < first + cap; ++k) {
    uint8_t* src = host + k * kLayerSize;
    if (src[16] == 0xFF && src[17] == 0xFF) continue;
    if (out != k) {
      std::memcpy(host + out * kLayerSize, src, kLayerSize);
      ClearLayer(mem, layers + k * kLayerSize);
    }
    ++out;
  }
}

// Clears the broken layers of the CarConfig at `cfg`.
void ScrubVinyl(GuestMemory* mem, uint32_t cfg, Report& r) {
  const uint32_t layers = cfg + kLayersAt;
  int cleared = 0;
  for (uint32_t k = 0; k < kLayerSlots; ++k) {
    const uint32_t layer = layers + k * kLayerSize;
    if (LayerBroken(mem, layer)) {
      ClearLayer(mem, layer);
      ++cleared;
    }
  }
  if (!cleared) return;
  for (int s = 0; s < kSurfaces; ++s) {
    const uint32_t cap = GuestRead32(mem, kSurfaceCaps + 4 * s);
    const uint32_t first = GuestRead32(mem, kSurfaceFirst + 4 * s);
    if (first < kLayerSlots && cap <= kLayerSlots - first) PackSurface(mem, layers, first, cap);
  }
  r.count += cleared;
  r.lines.push_back("vinyls: " + std::to_string(cleared) + " broken layer" +
                    (cleared == 1 ? "" : "s") + " removed");
}

// --------------------------------------------------------------- tuning ---

std::string Num(int v) { return std::to_string(v); }

std::string Num(float f) {
  if (std::isnan(f)) return "NaN";
  if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
  char buf[32];
  std::snprintf(buf, sizeof(buf), std::fabs(f) >= 1e6f ? "%.3g" : "%.1f", f);
  return buf;
}

void ClampByte(GuestMemory* mem, uint32_t addr, uint8_t lo, uint8_t hi, const char* label,
               Report& r) {
  uint8_t* p = mem->TranslateVirtual<uint8_t*>(addr);
  const uint8_t v = *p;
  const uint8_t fixed = v < lo ? lo : (v > hi ? hi : v);
  if (fixed == v) return;
  *p = fixed;
  r.Add(label, Num(int(v)), Num(int(fixed)));
}

void FixCamber(GuestMemory* mem, uint32_t addr, float fallback, Report& r) {
  const uint32_t bits = GuestRead32(mem, addr);
  float f;
  std::memcpy(&f, &bits, 4);
  if (std::isfinite(f) && std::fabs(f) <= 45.0f) return;
  uint32_t out;
  std::memcpy(&out, &fallback, 4);
  GuestWrite32(mem, addr, out);
  r.Add("camber", Num(f), Num(fallback));
}

// Puts the tuning values of the CarConfig at `cfg` that the game cannot make
// back in range.
void ScrubTuning(GuestMemory* mem, uint32_t cfg, Report& r) {
  // Shown the way the garage counts them: stock is level 1, fully upgraded 3.
  for (int k = 0; k < 13; ++k) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(cfg + kPartStates + k);
    if (*p <= kPartStateMax) continue;
    r.Add(kPartLabel[k], Num(int(*p) + 1), Num(int(kPartStateMax) + 1));
    *p = kPartStateMax;
  }
  {
    // Signed in the game (lbz + extsb).
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(cfg + kEstimatedModValue);
    const int8_t v = static_cast<int8_t>(*p);
    if (v < -1 || v > 100) {
      const int8_t fixed = v < 0 ? -1 : 100;
      *p = static_cast<uint8_t>(fixed);
      r.Add("mod value", Num(int(v)), Num(int(fixed)));
    }
  }
  for (uint32_t k = 0; k < 3; ++k) {
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(cfg + kGripFactors + k);
    if (*p == kGripDefault) continue;
    r.Add("grip", Num(int(static_cast<int8_t>(*p))) + "%", Num(int(kGripDefault)) + "%");
    *p = kGripDefault;
  }

  const uint32_t w = cfg + kWheels;
  FixCamber(mem, w + 0, -2.5f, r);  // Camber0
  FixCamber(mem, w + 4, -1.0f, r);  // Camber1
  for (uint32_t axle = 0; axle < 2; ++axle) {
    ClampByte(mem, w + 10 + axle, 0, 11, "ride height", r);
    ClampByte(mem, w + 12 + axle, 0, 16, "tire width", r);
    ClampByte(mem, w + 14 + axle, 12, 28, "rim size", r);
    ClampByte(mem, w + 16 + axle, 0, 13, "tire profile", r);
  }

  for (uint32_t k = 0; k < 6; ++k) {
    const uint32_t at = cfg + kPaintLayers + kPaintLayerSize * k + kPaintType;
    const uint32_t v = GuestRead32(mem, at);
    if (v < kPaintKinds) continue;
    GuestWrite32(mem, at, 0);
    r.Add("paint type", Num(int(v)), kPaintName[0]);
  }
}

Report Scrub(GuestMemory* mem, uint32_t cfg) {
  Report r;
  if (!mem || !IsGuestPtr(cfg)) return r;
  ScrubTuning(mem, cfg, r);
  ScrubVinyl(mem, cfg, r);
  return r;
}

// Printable ASCII of a guest string, for the message box.
std::string Printable(GuestMemory* mem, uint32_t addr, uint32_t maxlen) {
  std::string s;
  for (char ch : GuestCStr(mem, addr, maxlen))
    if (ch >= 0x20 && ch < 0x7F && !(ch == ' ' && (s.empty() || s.back() == ' '))) s += ch;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// The name the player knows the car by: the config's display name ("83 GOLF/GTI"),
// or the vehicle name when that is a missing string key ("!!..._shrt!!").
std::string CarName(GuestMemory* mem, uint32_t cfg) {
  std::string name = Printable(mem, cfg + kCarDisplayName, 34);
  if (name.empty() || name.rfind("!!", 0) == 0) name = Printable(mem, cfg + kCarVehicleName, 32);
  return name;
}

// ------------------------------------------------------------ the dialog ---

uint32_t GuestCopy(GuestMemory* mem, std::string_view s) {
  const uint32_t p = CallGuest(kGuestNewFn, static_cast<uint32_t>(s.size() + 1));
  if (!IsGuestPtr(p)) return 0;
  uint8_t* host = mem->TranslateVirtual<uint8_t*>(p);
  std::memcpy(host, s.data(), s.size());
  host[s.size()] = 0;
  return p;
}

uint32_t GuestName(GuestMemory* mem) {
  static uint32_t name = 0;
  if (!name) name = GuestCopy(mem, kDialogState);
  return name;
}

uint32_t Hsm(GuestMemory* mem) {
  const uint32_t ui = GuestRead32(mem, kUiManagerPtr);
  if (!IsGuestPtr(ui)) return 0;
  const uint32_t root = GuestRead32(mem, ui + 52);
  return IsGuestPtr(root) ? root + 4 : 0;
}

// Modal message boxes FlashNavigator has up right now.
uint32_t OpenMessageBoxes(GuestMemory* mem) {
  const uint32_t ui = GuestRead32(mem, kUiManagerPtr);
  const uint32_t nav = IsGuestPtr(ui) ? GuestRead32(mem, ui + 52) : 0;
  if (!IsGuestPtr(nav)) return 0;
  const uint32_t n = GuestRead32(mem, nav + kNavLayerCount);
  return n <= 3 ? n : 0;
}

uint32_t KeyHash(std::string_view s) {
  uint32_t h = 0;
  for (char ch : s) {
    uint8_t c = static_cast<uint8_t>(ch);
    if (c >= 'A' && c <= 'Z') c = uint8_t(c + 32);
    h += c;
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

uint32_t TableEntry(GuestMemory* mem, uint32_t hash) {
  const uint32_t mgr = GuestRead32(mem, kStringTableMgr);
  const uint32_t map = IsGuestPtr(mgr) ? GuestRead32(mem, mgr + kMgrTable) : 0;
  if (!IsGuestPtr(map)) return 0;
  const uint32_t buckets = GuestRead32(mem, map);
  const uint32_t array = GuestRead32(mem, map + 4);
  if (!buckets || !IsGuestPtr(array)) return 0;
  uint32_t node = GuestRead32(mem, array + 4 * (hash % buckets));
  for (int guard = 0; IsGuestPtr(node) && guard < 4096; ++guard, node = GuestRead32(mem, node + 8))
    if (GuestRead32(mem, node) == hash) return GuestRead32(mem, node + 4);
  return 0;
}

// True when the display cache holds a buffer for the key (sub_826BDDB0).
bool KeyCached(GuestMemory* mem, uint32_t hash) {
  const uint32_t mgr = GuestRead32(mem, kStringTableMgr);
  if (!IsGuestPtr(mgr)) return true;
  const uint32_t array = GuestRead32(mem, mgr + kMgrCache);
  const uint32_t buckets = GuestRead16(mem, mgr + kMgrCache + 4);
  if (!buckets || !IsGuestPtr(array)) return false;
  uint32_t node = GuestRead32(mem, array + 4 * (hash % buckets));
  for (int guard = 0; IsGuestPtr(node) && guard < 4096; ++guard, node = GuestRead32(mem, node + 8))
    if (GuestRead32(mem, node) == hash) return GuestRead32(mem, node + 4) != 0;
  return false;
}

// The message box's text field breaks lines on a carriage return (Flash's
// paragraph break); a line feed shows as a space.
constexpr char kBreak = '\r';

// The warning, then what was changed, one line each, inside `room` bytes.
std::string Compose(std::string_view head, const Report& r, int more_cars, size_t room) {
  std::string text(head);
  if (text.size() > room) text.resize(room);
  const std::string tail_cars =
      more_cars > 0 ? kBreak + ("+" + std::to_string(more_cars)) + " more rigged car" +
                          (more_cars == 1 ? "" : "s")
                    : "";
  size_t shown = 0;
  for (const std::string& line : r.lines) {
    if (shown == kMaxLines) break;
    const size_t rest = r.lines.size() - shown - 1;
    // Keep room for a "+N more" line after this one when lines are left.
    const size_t reserve = (rest ? 12 : 0) + tail_cars.size();
    if (text.size() + 1 + line.size() + reserve > room) break;
    text += kBreak + line;
    ++shown;
  }
  if (shown < r.lines.size()) {
    const std::string more = kBreak + ("+" + std::to_string(r.lines.size() - shown)) + " more";
    if (text.size() + more.size() <= room) text += more;
  }
  if (text.size() + tail_cars.size() <= room) text += tail_cars;
  return text;
}

struct Showing {
  bool active = false;
  bool seen = false;  // the message box was in FlashNavigator's queue at least once
  uint32_t entry = 0;
  uint32_t original = 0;
  uint32_t mine = 0;
  Clock::time_point since{};
};
Showing g_show;

void RestoreText(GuestMemory* mem) {
  if (g_show.entry && GuestRead32(mem, g_show.entry + kEntryText) == g_show.mine)
    GuestWrite32(mem, g_show.entry + kEntryText, g_show.original);
  g_show = Showing{};
}

// Opens the message box with `head` and the report under it. False when the
// dialog is not there to open.
bool Show(GuestMemory* mem, std::string_view head, const Report& r, int more_cars) {
  const uint32_t hsm = Hsm(mem);
  const uint32_t name = GuestName(mem);
  const uint32_t state = (hsm && name) ? CallGuest(kFindStateFn, hsm, name) : 0;
  const uint32_t hash = KeyHash(kDialogKey);
  const uint32_t entry = TableEntry(mem, hash);
  if (!IsGuestPtr(state) || !IsGuestPtr(entry)) {
    LARECOMP_APP_WARN("[car-guard] no '{}' dialog to show the notice in", kDialogState);
    return false;
  }
  const size_t room = KeyCached(mem, hash) ? kCachedTextMax : kTextMax;
  const std::string text = Compose(head, r, more_cars, room);
  const uint32_t mine = GuestCopy(mem, text);
  if (!mine) return false;
  g_show.original = GuestRead32(mem, entry + kEntryText);
  g_show.entry = entry;
  g_show.mine = mine;
  g_show.active = true;
  g_show.seen = false;
  g_show.since = Clock::now();
  GuestWrite32(mem, entry + kEntryText, mine);
  const uint32_t vtbl = GuestRead32(mem, state);
  CallGuest(GuestRead32(mem, vtbl + kStateActivate), state);
  CallGuest(GuestRead32(mem, vtbl + kStateFocus), state, 1u);
  CallGuest(GuestRead32(mem, vtbl + kStateShow), state, 1u);
  LARECOMP_APP_INFO("[car-guard] notice asked for ({} bytes, room {})", text.size(), room);
  return true;
}

bool SameCar(GuestMemory* mem, uint32_t cfg, const Pending& p) {
  return IsGuestPtr(cfg) && GuestCStr(mem, cfg + kCarVehicleName, 32) == p.vehicle &&
         GuestCStr(mem, cfg + kCarDisplayName, 34) == p.display &&
         GuestCStr(mem, cfg + kCarPlate, 7) == p.plate;
}

uint32_t OwnedSlots(GuestMemory* mem) {
  const int32_t count = static_cast<int32_t>(GuestRead32(mem, kOwnedCount));
  const uint32_t cap = owned_cars::Cap();
  return count <= 0 ? 0 : (static_cast<uint32_t>(count) < cap ? static_cast<uint32_t>(count) : cap);
}

// The report of the garage car being driven, if it has one; taken out of the
// list. Reports whose car is gone from the garage are dropped.
bool TakeCurrentCarReport(GuestMemory* mem, Pending& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_garage.empty()) return false;
  const int32_t cur = static_cast<int32_t>(GuestRead32(mem, kCurrentSlot));
  const uint32_t slots = OwnedSlots(mem);
  if (cur < 0 || static_cast<uint32_t>(cur) >= slots) return false;
  const uint32_t cfg = owned_cars::CarAddress(static_cast<uint32_t>(cur));
  for (auto it = g_garage.begin(); it != g_garage.end();) {
    if (SameCar(mem, cfg, *it)) {
      out = std::move(*it);
      g_garage.erase(it);
      return true;
    }
    // Still in the garage, maybe at another slot?
    bool owned = false;
    for (uint32_t s = 0; s < slots && !owned; ++s)
      owned = SameCar(mem, owned_cars::CarAddress(s), *it);
    if (!owned) {
      LARECOMP_APP_INFO("[car-guard] garage car '{}' is gone, its notice dropped", it->vehicle);
      it = g_garage.erase(it);
    } else {
      ++it;
    }
  }
  return false;
}

}  // namespace

void AfterSaveLoad(uint8_t* /*base*/) {
  auto* mem = Mem();
  if (!mem) return;
  std::vector<Pending> found;
  const uint32_t slots = owned_cars::Cap();
  for (uint32_t i = 0; i < slots; ++i) {
    const uint32_t car = owned_cars::CarAddress(i);
    if (!IsGuestPtr(car)) continue;
    if (!*mem->TranslateVirtual<const uint8_t*>(car + kCarVehicleName)) continue;  // empty slot
    Report r = Scrub(mem, car);
    if (!r) continue;
    LARECOMP_APP_WARN("[car-guard] garage car {} ({}): {}", i,
                      GuestCStr(mem, car + kCarVehicleName, 32), r.Joined());
    Pending p;
    p.slot = i;
    p.vehicle = GuestCStr(mem, car + kCarVehicleName, 32);
    p.display = GuestCStr(mem, car + kCarDisplayName, 34);
    p.plate = GuestCStr(mem, car + kCarPlate, 7);
    p.report = std::move(r);
    found.push_back(std::move(p));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_garage = std::move(found);  // a save read again replaces what the last one found
}

void Tick() {
  static int frames = 0;
  static Clock::time_point road_since{};
  static bool on_road = false;
  if (++frames < 15) return;
  frames = 0;
  auto* mem = Mem();
  if (!mem) return;
  if (g_show.active) {
    const bool up = OpenMessageBoxes(mem) != 0;
    if (up && !g_show.seen) {
      g_show.seen = true;
      LARECOMP_APP_INFO("[car-guard] notice is up");
    }
    // Closed (or never opened, after a few seconds): the game's text goes back.
    if (g_show.seen && !up) {
      RestoreText(mem);
      on_road = false;  // the next notice, if any, after another few seconds
    } else if (!g_show.seen && Clock::now() - g_show.since > std::chrono::seconds(5)) {
      LARECOMP_APP_WARN("[car-guard] the notice did not open");
      RestoreText(mem);
      on_road = false;
    }
    return;
  }
  // Only on the road (no menu, no loading, the game entered) for a few seconds,
  // so the message box opens over the game and not into a screen that is going
  // away.
  const bool road = !larecomp::keyboard::MenuScreenUp() && OpenMessageBoxes(mem) == 0;
  if (!road) {
    on_road = false;
    return;
  }
  if (!on_road) {
    on_road = true;
    road_since = Clock::now();
  }
  if (Clock::now() - road_since < std::chrono::seconds(3)) return;
  if (!larecomp::settings::online_car_guard_notice.Get()) return;

  // Another player's car first: it is in the session right now.
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_theirs_pending) {
      g_theirs_pending = false;
      const Report r = g_theirs;
      const int more = g_theirs_more;
      const uint32_t cfg = g_theirs_cfg;
      g_theirs = Report{};
      g_theirs_more = 0;
      lock.unlock();
      // The import read the vehicle's names after our check, so they are read now.
      const std::string name = CarName(mem, cfg);
      LARECOMP_APP_INFO("[car-guard] the rigged car was '{}'", name);
      const std::string head =
          "Someone rolled in with a rigged " + (name.empty() ? std::string("car") : name) +
          ". Fixed before it got to you:";
      Show(mem, head, r, more);
      return;
    }
  }
  Pending p;
  if (TakeCurrentCarReport(mem, p)) {
    const std::string name = CarName(mem, owned_cars::CarAddress(p.slot));
    const std::string head = "Sus ride, huh? Your " + (name.empty() ? std::string("car") : name) +
                             " had values the game can't make. Back to street legal:";
    Show(mem, head, p.report, 0);
  }
}

}  // namespace larecomp::car_guard

using namespace larecomp::car_guard;

// mcCarConfig::Import (sub_82390940), right after the bitstream read
// (sub_82395A50) returned: r30 = the CarConfig. Every car from outside the
// machine comes through here. The vehicle's names come later in the stream.
void Hook_CarGuardImport(PPCRegister& r30) {
  auto* mem = larecomp::car_guard::Mem();
  auto r = larecomp::car_guard::Scrub(mem, r30.u32);
  if (!r) return;
  LARECOMP_APP_WARN("[car-guard] imported car: {}", r.Joined());
  // One notice per half minute, however many cars a session hands over.
  std::lock_guard<std::mutex> lock(larecomp::car_guard::g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (larecomp::car_guard::g_theirs_pending) {
    ++larecomp::car_guard::g_theirs_more;
  } else if (now - larecomp::car_guard::g_last_theirs > std::chrono::seconds(30)) {
    larecomp::car_guard::g_last_theirs = now;
    larecomp::car_guard::g_theirs_pending = true;
    larecomp::car_guard::g_theirs_cfg = r30.u32;
    larecomp::car_guard::g_theirs = std::move(r);
    larecomp::car_guard::g_theirs_more = 0;
  }
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <cstdint>
#include <rex/ppc/context.h>
#include "car_guard.h"

namespace larecomp::car_guard {
void AfterSaveLoad(uint8_t*) {}
void Tick() {}
}  // namespace larecomp::car_guard
void Hook_CarGuardImport(PPCRegister& r30) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
