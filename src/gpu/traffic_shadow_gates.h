#pragma once
// ===========================================================================
// MCLA -- the traffic shadow hooks' gate counters (one log line tells which test turns the cars away)
// ===========================================================================
// The first run of the TRAFFIC SHADOWS hook logged nothing, and nothing said which of its tests had stopped every
// car. These counters answer that for the next run: every test a call passes or fails has a counter, bumped with
// one relaxed atomic add (no per-call logging), and `[traffic-shadows] gates:` prints them all as ONE line, 20 s after
// the 2000th call (the first 2000 calls come in the world's first half second, when no item has a LOD yet: the second
// run's first line was read as the steady state) and then about every 30 seconds. Three lines come together:
//   gates:   the totals since the start (every test of both routes)
//   window:  the same for the traffic route since the last line (what the cars did lately)
//   census:  where the eye is (the main viewport's against the current one: in a cascade they are different views), how
//            far the items of the first cascade are by distance band and why the others were turned away, and whether
//            the distance agrees with the item's own view depth
//
// Two routes (traffic_shadow.h): the car-model hook (the player's and the racers' cars: sub_82360518 at 0x8236066C)
// and the traffic's own, the ambient car client's mask test in the phase loop (0x822E6870) and its item draw
// (sub_82793360). Pure: no ReXGlue, no renderer; traffic_shadow_test.cpp checks the counting and the line.
// ===========================================================================

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "traffic_shadow.h"

namespace mcla::gpu::traffic_shadow {

// The distinct values of a small set (the phases a test turned away), each with how often it was seen; more distinct
// values than slots go to `other`. Lock-free: a slot is claimed by compare-exchange.
class ValueCounts {
 public:
  static constexpr size_t kSlots = 12;

  void Add(uint32_t value) {
    const uint64_t tag = (uint64_t{1} << 32) | value;  // never 0, so 0 is "slot free"
    for (size_t i = 0; i < kSlots; ++i) {
      uint64_t key = keys_[i].load(std::memory_order_relaxed);
      if (key == 0) {
        uint64_t expected = 0;
        key = keys_[i].compare_exchange_strong(expected, tag, std::memory_order_relaxed) ? tag : expected;
      }
      if (key == tag) {
        counts_[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    other_.fetch_add(1, std::memory_order_relaxed);
  }

  // "{0x10 x3, 0x200 x7, other x1}" in the order the values were first seen; "{}" when nothing was added.
  std::string Text() const {
    std::string out = "{";
    bool first = true;
    auto append = [&](const std::string& item) {
      out += first ? "" : ", ";
      out += item;
      first = false;
    };
    for (size_t i = 0; i < kSlots; ++i) {
      const uint64_t key = keys_[i].load(std::memory_order_relaxed);
      if (key == 0) {
        break;
      }
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "0x%X x%llu", static_cast<unsigned>(key & 0xFFFFFFFFu),
                    static_cast<unsigned long long>(counts_[i].load(std::memory_order_relaxed)));
      append(buffer);
    }
    const uint64_t other = other_.load(std::memory_order_relaxed);
    if (other != 0) {
      append("other x" + std::to_string(other));
    }
    return out + "}";
  }

 private:
  std::array<std::atomic<uint64_t>, kSlots> keys_{};
  std::array<std::atomic<uint64_t>, kSlots> counts_{};
  std::atomic<uint64_t> other_{0};
};

// The car-model hook (Patch_TrafficCascadeGate), in the order of its tests.
enum class CarGate : size_t {
  kCalls,        // every call
  kGameCaster,   // r11 != 0: the game draws this car into the cascade itself (the player's)
  kOtherPhase,   // r28 is not one of the four cascade phases (the values are in the histogram)
  kModeOff,      // the setting says off (or `auto` on a card not known yet)
  kNoMemory,     // the guest memory base is not there
  kVehiclePtr,   // r30 is not a plausible pointer
  kIneligible,   // the vehicle's flags turn it away (each flag below counts too)
  kSideViews,    // +593 clear
  kHidden,       // +6478 set
  kDisabled,     // +6467 set
  kGhost,        // +123 set
  kNotVisible,   // +116 zero
  kNotEnabled,   // +120 zero
  kEntityPtr,    // [vehicle+20] is not a plausible pointer
  kEyeMain,      // the eye is the main viewport's
  kEyeFallback,  // the main viewport could not be read: the eye is the current viewport's (a cascade's own: far from the cars)
  kCameraPtr,    // neither viewport gave an eye
  kDistance,     // the distance is not a number
  kDecided,      // reached Selector::Decide
  kReach,        // selected caster misses this cascade
  kCast,         // Decide said cast (r11 raised to 1)
  kPlayerLeftOut,  // r11 != 0 in a cascade phase and the car shadow map casts the player's car's shadow: r11 lowered to 0
  kCount,
};

// The traffic route (Patch_AmbientCascadeClient in the phase loop, Patch_AmbientCascadeDraw in the item draw).
enum class AmbientGate : size_t {
  kLoopCalls,    // the phase loop met the ambient client in a cascade phase
  kLoopOff,      // ... and the setting says off
  kLoopWidened,  // ... and the client's mask was widened by the phase
  kLeafCalls,    // every call of the item draw
  kLeafCascade,  // ... in a cascade phase (the other phases are in the histogram)
  kModeOff,      // a cascade call with the setting off
  kNoMemory,     // the guest memory base is not there
  kItemPtr,      // r3 is not a plausible pointer
  kCarPtr,       // [item+16] is not a plausible pointer
  kBuffer,       // [0x827E5640] is not a buffer index
  kIneligible,   // the item draw would not draw it (each test below counts too)
  kEnabled,      // item +36 zero
  kCarFlags,     // [item+16] +500 not as the draw wants
  kModels,       // [[item+24]+32] below 3 (or [item+24] not a pointer)
  kLod,          // item +28 negative: outside the main view
  kLodCoarse,    // item +28 = 2: the body phase returns for the coarsest LOD
  kEyeMain,      // the eye is the main viewport's
  kEyeFallback,  // the main viewport could not be read: the eye is the current viewport's
  kRenderer,     // [0x8287E064] (the renderer) is not a plausible pointer
  kViewport,     // the main viewport pointer is not a plausible pointer
  kCameraPtr,    // neither viewport gave an eye
  kPosition,     // the car's world position is not a position
  kDistance,     // the distance is not a number
  kDepthConsistent,  // a candidate's distance agrees with its own view depth (item +32 less the radius)
  kDepthCloser,      // ... is shorter than its depth: the eye or the position is wrong
  kDepthFar,         // ... is longer than the view allows: the eye or the position is wrong
  kDepthUnknown,     // no usable depth
  kDecided,      // reached Selector::Decide
  kReach,        // selected caster misses this cascade
  kRemapped,     // Decide said cast (r4 changed to the body phase)
  kCount,
};

// Why the items of the first cascade phase were turned away or kept, for the census by distance band.
enum class BandKind : size_t {
  kCandidate,  // passed every test
  kLodOutside, // LOD < 0
  kLodCoarse,  // LOD 2
  kOther,      // +36 / car +500 / models
  kCount,
};
constexpr const char* BandKindName(BandKind kind) {
  switch (kind) {
    case BandKind::kCandidate: return "candidates";
    case BandKind::kLodOutside: return "LOD<0";
    case BandKind::kLodCoarse: return "LOD 2";
    default: return "other";
  }
}

// The eyes the census saw last: the main viewport's and the current viewport's (the cascade's own), with the viewports'
// addresses; written with relaxed stores from the render thread, read by the logger (a torn read only blurs a log line).
struct EyeSnapshot {
  uint32_t main_viewport = 0;
  uint32_t current_viewport = 0;
  float main_eye[3] = {0, 0, 0};
  float current_eye[3] = {0, 0, 0};
  bool valid = false;
};

// The counters and the log schedule. One global instance in the hook file; any thread.
class Gates {
 public:
  void Add(CarGate gate) { car_[static_cast<size_t>(gate)].fetch_add(1, std::memory_order_relaxed); }
  void Add(AmbientGate gate) { ambient_[static_cast<size_t>(gate)].fetch_add(1, std::memory_order_relaxed); }
  uint64_t Get(CarGate gate) const { return car_[static_cast<size_t>(gate)].load(std::memory_order_relaxed); }
  uint64_t Get(AmbientGate gate) const { return ambient_[static_cast<size_t>(gate)].load(std::memory_order_relaxed); }

  ValueCounts& car_phases() { return car_phases_; }              // the r28 values the car-model hook turned away
  ValueCounts& leaf_phases() { return leaf_phases_; }            // the phases the item draw saw that are not cascades
  const ValueCounts& car_phases() const { return car_phases_; }
  const ValueCounts& leaf_phases() const { return leaf_phases_; }

  // Calls of the three hooks that count toward the schedule: the car-model hook's, the item draw's and the phase
  // loop's for the ambient client in a cascade.
  uint64_t BumpCalls() { return calls_.fetch_add(1, std::memory_order_relaxed) + 1; }
  uint64_t Calls() const { return calls_.load(std::memory_order_relaxed); }

  // One item of the first cascade phase: its kind and its distance band (metres from the main viewport's eye).
  void AddBand(BandKind kind, float distance) {
    bands_[static_cast<size_t>(kind)][static_cast<size_t>(traffic_shadow::DistanceBand(distance))].fetch_add(
        1, std::memory_order_relaxed);
  }
  uint64_t Band(BandKind kind, int band) const {
    return bands_[static_cast<size_t>(kind)][static_cast<size_t>(band)].load(std::memory_order_relaxed);
  }

  // The eyes of the item just measured (see EyeSnapshot).
  void NoteEyes(uint32_t main_viewport, const float main_eye[3], uint32_t current_viewport, const float current_eye[3]) {
    eye_words_[0].store(main_viewport, std::memory_order_relaxed);
    eye_words_[1].store(current_viewport, std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
      eye_words_[2 + i].store(std::bit_cast<uint32_t>(main_eye[i]), std::memory_order_relaxed);
      eye_words_[5 + i].store(std::bit_cast<uint32_t>(current_eye[i]), std::memory_order_relaxed);
    }
    eye_words_[8].store(1, std::memory_order_relaxed);
  }
  EyeSnapshot Eyes() const {
    EyeSnapshot e;
    e.valid = eye_words_[8].load(std::memory_order_relaxed) != 0;
    e.main_viewport = eye_words_[0].load(std::memory_order_relaxed);
    e.current_viewport = eye_words_[1].load(std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
      e.main_eye[i] = std::bit_cast<float>(eye_words_[2 + i].load(std::memory_order_relaxed));
      e.current_eye[i] = std::bit_cast<float>(eye_words_[5 + i].load(std::memory_order_relaxed));
    }
    return e;
  }

  // The one line (no newline). `mode` is the setting's resolved name ("off", "near", "all").
  std::string Line(const char* mode) const {
    auto n = [](uint64_t v) { return std::to_string(v); };
    using C = CarGate;
    using A = AmbientGate;
    std::string s = "[traffic-shadows] gates: mode ";
    s += mode;
    s += "; car-model hook (player's and racers' cars, 0x8236066C): calls " + n(Get(C::kCalls));
    s += ", r11 set (the game's own caster) " + n(Get(C::kGameCaster));
    s += ", phase rejected " + n(Get(C::kOtherPhase)) + " r28 " + car_phases_.Text();
    s += ", player's car left to the car shadow map " + n(Get(C::kPlayerLeftOut));
    s += ", mode off " + n(Get(C::kModeOff));
    s += ", no memory " + n(Get(C::kNoMemory));
    s += ", vehicle ptr " + n(Get(C::kVehiclePtr));
    s += ", ineligible " + n(Get(C::kIneligible)) + " [+593 clear " + n(Get(C::kSideViews)) + ", +6478 set " +
         n(Get(C::kHidden)) + ", +6467 set " + n(Get(C::kDisabled)) + ", +123 set " + n(Get(C::kGhost)) +
         ", +116 zero " + n(Get(C::kNotVisible)) + ", +120 zero " + n(Get(C::kNotEnabled)) + "]";
    s += ", entity ptr " + n(Get(C::kEntityPtr)) + ", eye main viewport " + n(Get(C::kEyeMain)) + " / current viewport " +
         n(Get(C::kEyeFallback)) + ", camera ptr " + n(Get(C::kCameraPtr));
    s += ", distance not finite " + n(Get(C::kDistance));
    s += ", reached Decide " + n(Get(C::kDecided)) + ", cast " + n(Get(C::kCast));
    s += "; traffic route (ambient client): phase loop " + n(Get(A::kLoopCalls)) + " cascade meetings (off " +
         n(Get(A::kLoopOff)) + ", mask widened " + n(Get(A::kLoopWidened)) + ")";
    s += ", item draw calls " + n(Get(A::kLeafCalls)) + " (cascade " + n(Get(A::kLeafCascade)) + ", other phases " +
         leaf_phases_.Text() + ")";
    s += ", mode off " + n(Get(A::kModeOff)) + ", no memory " + n(Get(A::kNoMemory)) + ", item ptr " +
         n(Get(A::kItemPtr)) + ", car ptr " + n(Get(A::kCarPtr)) + ", buffer " + n(Get(A::kBuffer));
    s += ", ineligible " + n(Get(A::kIneligible)) + " [+36 zero " + n(Get(A::kEnabled)) + ", car +500 " +
         n(Get(A::kCarFlags)) + ", models<3 " + n(Get(A::kModels)) + ", LOD<0 " + n(Get(A::kLod)) + ", LOD 2 " +
         n(Get(A::kLodCoarse)) + "]";
    s += ", eye main viewport " + n(Get(A::kEyeMain)) + " / current viewport " + n(Get(A::kEyeFallback)) + " (renderer ptr " +
         n(Get(A::kRenderer)) + ", viewport ptr " + n(Get(A::kViewport)) + "), camera ptr " + n(Get(A::kCameraPtr)) +
         ", position " + n(Get(A::kPosition)) + ", distance not finite " + n(Get(A::kDistance));
    s += ", reached Decide " + n(Get(A::kDecided)) + ", cast " + n(Get(A::kRemapped));
    s += "; beyond cascade reach: car-model " + n(Get(C::kReach)) + ", ambient " + n(Get(A::kReach));
    return s;
  }

  // The same for the traffic route since the last call (the first call: since the start). Not const: it remembers the
  // totals. One thread (the logger holds a lock).
  std::string WindowLine() {
    using A = AmbientGate;
    auto d = [&](A gate) {
      const size_t i = static_cast<size_t>(gate);
      const uint64_t now = ambient_[i].load(std::memory_order_relaxed);
      const uint64_t delta = now - window_base_[i];
      window_base_[i] = now;
      return std::to_string(delta);
    };
    std::string s = "[traffic-shadows] window: traffic route since the last line: item draws in cascades " + d(A::kLeafCascade);
    s += ", turned away " + d(A::kIneligible) + " [+36 zero " + d(A::kEnabled) + ", car +500 " + d(A::kCarFlags) +
         ", models<3 " + d(A::kModels) + ", LOD<0 " + d(A::kLod) + ", LOD 2 " + d(A::kLodCoarse) + "]";
    s += ", reached Decide " + d(A::kDecided) + ", cast " + d(A::kRemapped);
    s += ", eye main viewport " + d(A::kEyeMain) + " / current viewport " + d(A::kEyeFallback);
    s += ", candidates' distance against the item's own view depth: consistent " + d(A::kDepthConsistent) +
         ", closer than the depth " + d(A::kDepthCloser) + ", beyond the view " + d(A::kDepthFar) + ", unknown " +
         d(A::kDepthUnknown) + ", beyond cascade reach " + d(A::kReach);
    return s;
  }

  // The census: the eyes and the items of the first cascade by distance band (totals since the start).
  std::string CensusLine() const {
    auto n = [](uint64_t v) { return std::to_string(v); };
    auto f = [](float v) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.1f", double(v));
      return std::string(buffer);
    };
    std::string s = "[traffic-shadows] census: ";
    const EyeSnapshot e = Eyes();
    if (e.valid) {
      char head[96];
      std::snprintf(head, sizeof(head), "main viewport 0x%08X, current viewport 0x%08X", e.main_viewport, e.current_viewport);
      s += head;
      s += " (in a cascade the current one is the cascade's); eye of the main viewport " + f(e.main_eye[0]) + " " +
           f(e.main_eye[1]) + " " + f(e.main_eye[2]) + ", of the current viewport " + f(e.current_eye[0]) + " " +
           f(e.current_eye[1]) + " " + f(e.current_eye[2]);
      const float dx = e.main_eye[0] - e.current_eye[0];
      const float dy = e.main_eye[1] - e.current_eye[1];
      const float dz = e.main_eye[2] - e.current_eye[2];
      s += ", " + f(std::sqrt(dx * dx + dy * dy + dz * dz)) + " m apart";
    } else {
      s += "no eye seen yet";
    }
    s += "; items of the first cascade by distance from the main viewport's eye";
    for (int band = 0; band < traffic_shadow::kDistanceBands; ++band) {
      s += std::string(band == 0 ? ": " : ", ") + traffic_shadow::DistanceBandName(band) + " [";
      for (size_t kind = 0; kind < static_cast<size_t>(BandKind::kCount); ++kind) {
        s += std::string(kind == 0 ? "" : ", ") + BandKindName(static_cast<BandKind>(kind)) + " " +
             n(Band(static_cast<BandKind>(kind), band));
      }
      s += "]";
    }
    return s;
  }

 private:
  std::array<std::atomic<uint64_t>, static_cast<size_t>(CarGate::kCount)> car_{};
  std::array<std::atomic<uint64_t>, static_cast<size_t>(AmbientGate::kCount)> ambient_{};
  std::array<std::array<std::atomic<uint64_t>, traffic_shadow::kDistanceBands>, static_cast<size_t>(BandKind::kCount)> bands_{};
  std::array<std::atomic<uint32_t>, 9> eye_words_{};
  std::array<uint64_t, static_cast<size_t>(AmbientGate::kCount)> window_base_{};
  ValueCounts car_phases_;
  ValueCounts leaf_phases_;
  std::atomic<uint64_t> calls_{0};
};

// When the lines are printed: kFirstDelayNs after the first time Due() is asked once the calls have reached
// kFirstAfterCalls, then every kEveryNs. Single threaded (the caller holds a lock); `now_ns` is any monotonic clock.
// The first 2000 calls come within the first half second of the world (the second run: 37 frames), when no item has a
// LOD or a body yet; the delay makes the first line a steady-state one.
struct LogSchedule {
  static constexpr uint64_t kFirstAfterCalls = 2000;
  static constexpr int64_t kFirstDelayNs = 20'000'000'000;
  static constexpr int64_t kEveryNs = 30'000'000'000;

  bool armed = false;
  bool first_done = false;
  int64_t armed_ns = 0;
  int64_t last_ns = 0;

  bool Due(uint64_t calls, int64_t now_ns) {
    if (calls < kFirstAfterCalls) {
      return false;
    }
    if (!armed) {
      armed = true;
      armed_ns = now_ns;
      return false;
    }
    if (!first_done) {
      if (now_ns - armed_ns < kFirstDelayNs) {
        return false;
      }
      first_done = true;
      last_ns = now_ns;
      return true;
    }
    if (now_ns - last_ns >= kEveryNs) {
      last_ns = now_ns;
      return true;
    }
    return false;
  }
};

}  // namespace mcla::gpu::traffic_shadow
