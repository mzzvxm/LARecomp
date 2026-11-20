#pragma once
// ===========================================================================
// MCLA GPU renderer -- the game's own headlight points, from its light submit (the headlight rig's best source)
// ===========================================================================
// The night probe (perf/out/NIGHT-PROBE.txt, C12 and C13) read the game's vehicle light submit (sub_8237AC90, one light point at 0x8237AFB4):
// every vehicle has its light points by kind (3 Headlight x2, 4 HiBeam x2, 0 Brakelight, 1 Taillight, 6 TurnSignal ...), each with a WORLD position
// (a racer's and the player's points are stored with it; the traffic's are made by sub_8237A698: position only), for a racer's a direction (it is the
// car's BACK axis: the lamps lie along minus it: the player's pair at +1.95 m along -dir, 1.2 m apart across it), and an intensity (the kind's table entry
// times the object's value) that is exactly the lights' on state: the player's and the racers' Headlights read 2.00 at night and 0.00 by day (the guest's
// own class flag and status bytes read the same at both: they are not the state). The ownerless traffic points read 3.4 at night.
//
// This is the table the guest hook (src/mc_engine/hooks/headlight_points.cpp, a midasm hook at 0x8237AFB4, Headlight points only, only while the rig wants
// them) writes on the game thread and the rig reads on the render thread: the latest point of each (vehicle, index) or, for a point with no owner, of each
// place, with the time it was seen. Header only (inline functions and variables: one table for every file that includes it); pure, it includes nothing of the
// renderer or the SDK, headlight_rig_test.cpp checks it.
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

namespace mcla::gpu::headlight_points {

// The kind of a Headlight point in the light object's sets (night_probe_rules.h LightKindName: 0 Brakelight, 1 Taillight, 3 Headlight, 4 HiBeam ...).
inline constexpr uint32_t kHeadlightKind = 3;
// The intensity at and below which a light is off, and the one the player's lamps read at night (the light's full strength).
inline constexpr float kOffIntensity = 0.05f;
inline constexpr float kFullIntensity = 2.0f;

struct Point {
  float pos[3] = {};
  float dir[3] = {};      // a racer's and the player's: the car's back axis (the beam goes along minus it); zero for a point with no owner
  bool has_dir = false;
  float intensity = 0.0f;
  bool player = false;    // the player's vehicle
  uint32_t vehicle = 0;   // the vehicle's guest address; 0 for a point with no owner
  uint32_t index = 0;     // the point's place in its set
};

// Whether the hook is to do anything: the rig sets it while the headlights are on (one relaxed load in the hook otherwise).
inline std::atomic<bool> g_wanted{false};
inline bool Wanted() { return g_wanted.load(std::memory_order_relaxed); }
inline void SetWanted(bool wanted) { g_wanted.store(wanted, std::memory_order_relaxed); }

class Table {
 public:
  static constexpr size_t kCapacity = 256;
  using Clock = std::chrono::steady_clock;

  // Game thread: a point seen now (or at `at`, for tests).
  void Note(const Point& point, Clock::time_point at = Clock::now()) {
    const uint64_t key = KeyOf(point);
    std::lock_guard<std::mutex> guard(lock_);
    size_t found = kCapacity, free_slot = kCapacity, oldest = 0;
    for (size_t i = 0; i < kCapacity; ++i) {
      const Entry& e = entries_[i];
      if (!e.used) {
        if (free_slot == kCapacity) free_slot = i;
        continue;
      }
      if (e.key == key) {
        found = i;
        break;
      }
      if (e.at < entries_[oldest].at || !entries_[oldest].used) oldest = i;
    }
    // The point's own entry, else a free one, else (the table is full) the oldest goes.
    const size_t slot = found != kCapacity ? found : (free_slot != kCapacity ? free_slot : oldest);
    entries_[slot] = {true, key, at, point};
    ++notes_;
  }

  // Render thread: the points seen within `max_age`, in no particular order.
  void Recent(std::vector<Point>* out, std::chrono::milliseconds max_age, Clock::time_point now = Clock::now()) const {
    out->clear();
    std::lock_guard<std::mutex> guard(lock_);
    for (const Entry& e : entries_) {
      if (e.used && now - e.at <= max_age) out->push_back(e.point);
    }
  }

  uint64_t notes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return notes_;
  }
  void Clear() {
    std::lock_guard<std::mutex> guard(lock_);
    entries_ = {};
    notes_ = 0;
  }

 private:
  struct Entry {
    bool used = false;
    uint64_t key = 0;
    Clock::time_point at{};
    Point point;
  };

  // An owned point is its vehicle and index; one with no owner is its place to a quarter of a metre (a moving car's points make new keys, the old ones age out).
  static uint64_t KeyOf(const Point& p) {
    if (p.vehicle != 0) return (uint64_t(p.vehicle) << 8) ^ uint64_t(p.index & 0xFFu) ^ 1ull;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int c = 0; c < 3; ++c) {
      const int64_t q = int64_t(std::floor(double(p.pos[c]) * 4.0));
      h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return (h << 1) | 0ull;
  }

  mutable std::mutex lock_;
  std::array<Entry, kCapacity> entries_{};
  uint64_t notes_ = 0;
};

inline Table& Global() {
  static Table table;
  return table;
}

}  // namespace mcla::gpu::headlight_points
