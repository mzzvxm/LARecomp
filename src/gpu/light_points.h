#pragma once
// ===========================================================================
// MCLA GPU renderer -- ALL of the game's vehicle light points, from its light submit (perf/LIGHT-UNIFY.md package LU-7 = step U7)
// ===========================================================================
// headlight_points.h holds the Headlight points only, for the headlight rig. This is the same table widened to every kind a vehicle has, with the light OBJECT (the car) and the
// point's whole matrix, for the light manager's sources (car_lights.h: brake, tail, turn, reverse, police, and what a car's lens does at night).
//
// WHAT THE GAME HOLDS (the night probe, test/night-probe 3d3bb584: perf/out/NIGHT-PROBE.txt C12 and C13, the run ports/run/nprobe1-night: 12 825 point lines in 230 logged frames; the
// recompiled sub_8237AC90 and sub_8237A698). sub_8237AC90(r3 = a vehicle's light object, r4 a flag) loops over the object's 19 SETS, one per light KIND (the pointer to set k is at
// object + 16 + 4 k), and over the points of each set (+4 the count, +20 the array of point pointers). A point is a 4x4 matrix in WORLD space: rows right (+0), up (+16), direction (+32)
// and position (+48); sub_8237A698 makes it for a car with no owner (traffic) in a stack buffer whose +48 is r30 at the hook. The lamp is lit by `table x value`: two floats the guest
// multiplies at 0x8237AFB4 (r17 the first, r20 the second: the object's own float of the kind, object + 400 + 4 k). The glow is drawn when that product is above a threshold, times
// how much the lamp faces the camera: the stored direction is the way the lamp is SEEN FROM (the glow is brightest when the eye-to-lamp vector matches it), so a lamp SHINES along
// MINUS it: the headlights' is the car's back axis (the player's pair, probe: (0.47, 0.04, -0.88) with the car heading (-0.47, -0.04, 0.88)), the brake lamps' the car's forward.
//
// The kinds (night_probe_rules.h LightKindName: the enum order of the string table at 0x827E9868, [I] except where a column says the probe saw it):
//   0 Brakelight (2 points: player, racers, traffic; up to 4 on some traffic) | 1 Taillight (2, traffic) | 2 ThirdBrakelight (racers) | 3 Headlight (2) | 4 HiBeam (2) | 5 FogLight (never seen)
//   6 TurnSignal (2: the player's pair sits at the rear centre, 0.36 m apart; traffic's at the rear corners) | 7 Police0 | 8 Police1 (2, near the headlights: the player's reads 1.0, traffic
//   1.8 to 1.9) | 9 Police2 | 10 Interior | 11 ReverseLight (never seen) | 12 None (1, rear) | 13 TurnSignalFL, 14 TurnSignalFR (never seen) | 15 TurnSignalBL, 16 TurnSignalBR (1 each, at the
//   FRONT of a traffic car, 1.3 to 1.6 m apart: left and right as the car's frame says: car_lights_test.cpp holds the recorded layout to it) | 17, 18 (1 each, beside the brake lamps).
// The intensities the probe read at night: the player's Headlights 2.00 and its Police1 1.00 (the lamps' on state), brake 0.075; a racer's Headlights 2.00, brake 0.5, third brake 1.0; a
// TRAFFIC car's are one scalar of the car (0.7 to 0.95, per car, per frame) times the kind's constant: brake 1, taillight 1, police 2, headlight 4 (3.4 to 3.8). By day the Headlights read 0.
//
// WHAT THIS FILE IS. The table the guest hook (src/mc_engine/hooks/headlight_points.cpp: ONE address, 0x8237AFB8, its registers unchanged) writes on the game thread and the render thread
// reads: the latest point of each (light object, kind, index) with the time it was seen and the largest intensity of the last few milliseconds (a car can be submitted twice in a
// frame). The key is the OBJECT, not a place: a moving traffic car's points would otherwise make a new key every quarter of a metre (headlight_points.h does, and ages them out).
// `ReadPoint` is the hook's decoder as a template over a memory reader, so a test lays out a fake guest object and decodes it. The hook costs one relaxed load for a kind nobody wants
// (Want / Wanted); header only, pure (no guest, no renderer, no SDK).
// ===========================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace larecomp::gpu::light_points {

// ---------------------------------------------------------------------------
// the kinds
// ---------------------------------------------------------------------------

inline constexpr uint32_t kKindCount = 19;  // sub_8237AC90's loop count: the object's sets
inline constexpr uint32_t kKindBrake = 0, kKindTail = 1, kKindThirdBrake = 2, kKindHeadlight = 3, kKindHiBeam = 4, kKindFog = 5, kKindTurn = 6, kKindPolice0 = 7, kKindPolice1 = 8,
                          kKindPolice2 = 9, kKindInterior = 10, kKindReverse = 11, kKindNone = 12, kKindTurnFL = 13, kKindTurnFR = 14, kKindTurnBL = 15, kKindTurnBR = 16;
inline constexpr uint32_t kAllKinds = (1u << kKindCount) - 1u;

inline const char* KindName(uint32_t kind) {
  static constexpr const char* kNames[kKindCount] = {"Brakelight", "Taillight",    "ThirdBrakelight", "Headlight",    "HiBeam",       "FogLight", "TurnSignal",
                                                     "Police0",    "Police1",      "Police2",         "Interior",     "ReverseLight", "None",     "TurnSignalFL",
                                                     "TurnSignalFR", "TurnSignalBL", "TurnSignalBR",    "Kind17",       "Kind18"};
  return kind < kKindCount ? kNames[kind] : "unnamed";
}

// What a lamp is FOR (several kinds are one family: both brake kinds, the five turn kinds, the three police kinds).
enum class Family : uint8_t { kHead = 0, kHiBeam, kFog, kBrake, kTail, kTurn, kReverse, kPolice, kOther };
inline constexpr uint32_t kFamilyCount = 9;

inline constexpr Family FamilyOf(uint32_t kind) {
  switch (kind) {
    case kKindBrake: case kKindThirdBrake: return Family::kBrake;
    case kKindTail: return Family::kTail;
    case kKindHeadlight: return Family::kHead;
    case kKindHiBeam: return Family::kHiBeam;
    case kKindFog: return Family::kFog;
    case kKindTurn: case kKindTurnFL: case kKindTurnFR: case kKindTurnBL: case kKindTurnBR: return Family::kTurn;
    case kKindPolice0: case kKindPolice1: case kKindPolice2: return Family::kPolice;
    case kKindReverse: return Family::kReverse;
    default: return Family::kOther;
  }
}
inline const char* FamilyName(Family f) {
  static constexpr const char* kNames[kFamilyCount] = {"head", "hibeam", "fog", "brake", "tail", "turn", "reverse", "police", "other"};
  return kNames[uint32_t(f)];
}

// The intensity at and below which a lamp is off (headlight_points.h kOffIntensity) and the one that counts as full, by family: the headlights read 2.0 when on, every other
// family's reading goes 0 to about 1 (the traffic's headlights and police lamps read more: their level saturates). [I] for the families the probe saw one value of: CHECK.
inline constexpr float kOffIntensity = 0.05f;
inline constexpr float FullIntensity(Family f) {
  return f == Family::kHead || f == Family::kHiBeam || f == Family::kFog ? 2.0f : 1.0f;
}
inline constexpr float Level(Family f, float intensity) {
  const float v = intensity / FullIntensity(f);
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// ---------------------------------------------------------------------------
// the guest's records
// ---------------------------------------------------------------------------

inline constexpr uint32_t kObjectSets = 16;       // the object's sets from here, a pointer each (object + 16 + 4 kind)
inline constexpr uint32_t kOwnerLink = 12;        // [[object] + 12] - 16 is the vehicle
inline constexpr uint32_t kSetPoints = 20;        // the set's array of point pointers
inline constexpr uint32_t kPointRight = 0, kPointUp = 16, kPointDirection = 32, kPointPosition = 48;  // the point's matrix rows
inline constexpr uint32_t kPlayerVehiclePtr = 0x8288DCF8u;  // the vehicle the local player drives

inline constexpr bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

// The kind a set pointer's address says: slot = object + 16 + 4 kind. False for an address that is not one of the object's 19.
inline constexpr bool KindOfSlot(uint32_t object, uint32_t slot, uint32_t* kind) {
  if (slot < object + kObjectSets) return false;
  const uint32_t d = slot - object - kObjectSets;
  if (d % 4 != 0 || d / 4 >= kKindCount) return false;
  *kind = d / 4;
  return true;
}

struct Point {
  uint32_t kind = 0;      // 0..18
  uint32_t index = 0;     // the point's place in its set
  uint32_t object = 0;    // the guest address of the light object: one per car while it lives (the table's key)
  uint32_t vehicle = 0;   // the vehicle's guest address; 0 for a car with no owner (traffic)
  bool owned = false;     // the light object has an owner ([object] != 0): a racer's or the player's; its point matrices are stored in the points
  bool player = false;    // the player's vehicle
  float pos[3] = {};      // world
  float dir[3] = {};      // the matrix row 2 as stored: the way the lamp is SEEN FROM (it shines along minus it); zero when `has_dir` is false
  float right[3] = {};    // rows 0 and 1: [I] the lamp's own frame (CHECK: the car's right and up, where the lamp was authored with the car's)
  float up[3] = {};
  bool has_dir = false;   // owned: read from the point; ownerless: read from the buffer sub_8237A698 made ([I]: the row below the position)
  float intensity = 0.0f; // the latest, `table x value`
  float peak = 0.0f;      // the largest of the submissions of the last kPeakWindow before the latest: what a lamp's state is told by
  float age_ms = 0.0f;    // filled by Recent: how long ago the latest was seen
};

// A reader of guest memory (the hook's wraps the membase; a test's is a map): U32(addr) and F32(addr), big-endian values as the guest sees them.
struct Regs {
  uint32_t object = 0;     // r26
  uint32_t slot = 0;       // r31: the address of the set's pointer
  uint32_t index_x4 = 0;   // r29: 4 times the point's index
  uint32_t pos_ptr = 0;    // r30: the position's pointer when there is no owner
  uint32_t table_ptr = 0;  // r17
  uint32_t value_ptr = 0;  // r20
};

// The hook's decode: the point the registers name, read exactly where the guest reads it (every pointer is one the guest dereferences itself at this place, so no VirtualQuery).
// False when something is not a plausible pointer (the hook then notes nothing).
template <class Mem>
inline bool ReadPoint(const Mem& mem, const Regs& r, uint32_t player_vehicle, Point* out) {
  uint32_t kind = 0;
  if (!PlausiblePtr(r.object) || !PlausiblePtr(r.slot) || !KindOfSlot(r.object, r.slot, &kind)) return false;
  Point p;
  p.kind = kind;
  p.index = r.index_x4 / 4u;
  p.object = r.object;
  const uint32_t owner = mem.U32(r.object);
  p.owned = owner != 0;
  if (owner != 0) {
    const uint32_t set = mem.U32(r.slot);
    if (!PlausiblePtr(set) || !PlausiblePtr(owner)) return false;
    const uint32_t link = mem.U32(owner + kOwnerLink);
    if (link != 0) p.vehicle = link - 16u;
    const uint32_t points = mem.U32(set + kSetPoints);
    if (!PlausiblePtr(points)) return false;
    const uint32_t point = mem.U32(points + r.index_x4);
    if (!PlausiblePtr(point)) return false;
    for (uint32_t i = 0; i < 3; ++i) {
      p.right[i] = mem.F32(point + kPointRight + 4u * i);
      p.up[i] = mem.F32(point + kPointUp + 4u * i);
      p.dir[i] = mem.F32(point + kPointDirection + 4u * i);
      p.pos[i] = mem.F32(point + kPointPosition + 4u * i);
    }
    p.has_dir = true;
  } else {
    const uint32_t at = r.pos_ptr;  // the position sub_8237A698 made: the matrix's +48
    if (!PlausiblePtr(at) || at < kPointPosition) return false;
    for (uint32_t i = 0; i < 3; ++i) {
      p.pos[i] = mem.F32(at + 4u * i);
      p.dir[i] = mem.F32(at - (kPointPosition - kPointDirection) + 4u * i);
      p.up[i] = mem.F32(at - (kPointPosition - kPointUp) + 4u * i);
      p.right[i] = mem.F32(at - (kPointPosition - kPointRight) + 4u * i);
    }
    p.has_dir = true;  // [I]: CHECK in a run (the traffic probe prints it)
  }
  if (!PlausiblePtr(r.table_ptr) || !PlausiblePtr(r.value_ptr)) return false;
  p.intensity = mem.F32(r.table_ptr) * mem.F32(r.value_ptr);
  p.peak = p.intensity;
  p.player = p.vehicle != 0 && p.vehicle == player_vehicle;
  *out = p;
  return true;
}

// The point as the headlight rig's table (headlight_points.h Point) has it, so the hook can feed both from one decode: the rig's own contract (has_dir only for an owned point, which is
// what it matches its lens lamps by). A template over any type with those members, so this header needs not include the old one.
template <class HeadlightPointT>
inline HeadlightPointT ToHeadlightPoint(const Point& p) {
  HeadlightPointT h{};
  for (int i = 0; i < 3; ++i) {
    h.pos[i] = p.pos[i];
    h.dir[i] = p.owned ? p.dir[i] : 0.0f;
  }
  h.has_dir = p.owned;
  h.intensity = p.intensity;
  h.player = p.player;
  h.vehicle = p.vehicle;
  h.index = p.index;
  return h;
}

// ---------------------------------------------------------------------------
// who wants what
// ---------------------------------------------------------------------------

// The consumers that ask for points (each its own kind mask, so that two of them do not switch each other off): the light manager's car lights, ray traced hit shading's lens emissives,
// the debug census. The hook runs for a kind only when some consumer wants it (the rig's own switch is headlight_points.h's SetWanted).
enum class Consumer : uint32_t { kManager = 0, kRt = 1, kDebug = 2 };
inline constexpr uint32_t kConsumerCount = 3;

inline std::array<std::atomic<uint32_t>, kConsumerCount>& ConsumerMasks() {
  static std::array<std::atomic<uint32_t>, kConsumerCount> masks{};
  return masks;
}
inline std::atomic<uint32_t>& WantedKinds() {
  static std::atomic<uint32_t> combined{0};
  return combined;
}
inline void Want(Consumer who, uint32_t kind_mask) {
  auto& masks = ConsumerMasks();
  masks[uint32_t(who)].store(kind_mask & kAllKinds, std::memory_order_relaxed);
  uint32_t all = 0;
  for (auto& m : masks) all |= m.load(std::memory_order_relaxed);
  WantedKinds().store(all, std::memory_order_relaxed);
}
inline bool Wanted(uint32_t kind) { return kind < kKindCount && ((WantedKinds().load(std::memory_order_relaxed) >> kind) & 1u) != 0; }
inline bool AnyWanted() { return WantedKinds().load(std::memory_order_relaxed) != 0; }

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

class Table {
 public:
  static constexpr size_t kCapacity = 2048;  // power of two: 19 sets of at most a few points for 40 cars in sight
  static constexpr size_t kProbe = 16;       // how far a key is looked for from its hash slot
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kPeakWindow{50};

  // Game thread: a point seen now (or at `at`, for tests).
  void Note(const Point& point, Clock::time_point at = Clock::now()) {
    const uint64_t key = KeyOf(point);
    std::lock_guard<std::mutex> guard(lock_);
    const size_t home = size_t(Hash(key)) & (kCapacity - 1);
    size_t found = kCapacity, free_slot = kCapacity, oldest = home;
    for (size_t step = 0; step < kProbe; ++step) {
      const size_t i = (home + step) & (kCapacity - 1);
      const Entry& e = entries_[i];
      if (!e.used) {
        if (free_slot == kCapacity) free_slot = i;
        continue;
      }
      if (e.key == key) {
        found = i;
        break;
      }
      if (!entries_[oldest].used || e.at < entries_[oldest].at) oldest = i;
    }
    const size_t slot = found != kCapacity ? found : (free_slot != kCapacity ? free_slot : oldest);
    Entry& e = entries_[slot];
    Point p = point;
    if (found != kCapacity && at - e.window_start <= kPeakWindow) {
      p.peak = std::max(e.point.peak, point.intensity);
      e = {true, key, at, e.window_start, p};
    } else {
      p.peak = point.intensity;
      e = {true, key, at, at, p};
    }
    ++notes_;
  }

  // Render thread: the points seen within `max_age`, in no particular order, with their age.
  void Recent(std::vector<Point>* out, std::chrono::milliseconds max_age, Clock::time_point now = Clock::now()) const {
    out->clear();
    std::lock_guard<std::mutex> guard(lock_);
    for (const Entry& e : entries_) {
      if (e.used && now - e.at <= max_age) {
        out->push_back(e.point);
        out->back().age_ms = std::chrono::duration<float, std::milli>(now - e.at).count();
      }
    }
  }

  uint64_t notes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return notes_;
  }
  void Clear() {
    std::lock_guard<std::mutex> guard(lock_);
    for (Entry& e : entries_) e = {};
    notes_ = 0;
  }

  // (object, kind, index): the same lamp of the same car, whatever it does. A car's light object is the key because it stays with the car (a position would not).
  static uint64_t KeyOf(const Point& p) {
    return (uint64_t(p.object) << 32) | (uint64_t(p.kind & 0xFFu) << 8) | uint64_t(p.index & 0xFFu);
  }

 private:
  struct Entry {
    bool used = false;
    uint64_t key = 0;
    Clock::time_point at{};
    Clock::time_point window_start{};  // when the current peak window began
    Point point;
  };
  static uint64_t Hash(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    return x ^ (x >> 33);
  }

  mutable std::mutex lock_;
  std::array<Entry, kCapacity> entries_{};
  uint64_t notes_ = 0;
};

inline Table& Global() {
  static Table table;
  return table;
}

}  // namespace larecomp::gpu::light_points
