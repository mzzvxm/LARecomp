#pragma once
// ===========================================================================
// MCLA -- the traffic shadow hooks' gate counters (one log line tells which test turns the cars away)
// ===========================================================================
// The first run of the TRAFFIC SHADOWS hook logged nothing, and nothing said which of its tests had stopped every
// car. These counters answer that for the next run: every test a call passes or fails has a counter, bumped with
// one relaxed atomic add (no per-call logging), and `[traffic-shadows] gates:` prints them all as ONE line after
// the first 2000 calls and then about every 30 seconds.
//
// Two routes (traffic_shadow.h): the car-model hook (the player's and the racers' cars: sub_82360518 at 0x8236066C)
// and the traffic's own, the ambient car client's mask test in the phase loop (0x822E6870) and its item draw
// (sub_82793360). Pure: no ReXGlue, no renderer; traffic_shadow_test.cpp checks the counting and the line.
// ===========================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

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
  kCameraPtr,    // [0x82839704] is not a plausible pointer
  kDistance,     // the distance is not a number
  kDecided,      // reached Selector::Decide
  kCast,         // Decide said cast (r11 raised to 1)
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
  kNotVisible,   // item +557 + buffer without bit 4
  kLod,          // item +28 negative
  kNoBody,       // item +556 without bit 0x80
  kCameraPtr,    // [0x82839704] is not a plausible pointer
  kPosition,     // the car's world position is not a position
  kDistance,     // the distance is not a number
  kDecided,      // reached Selector::Decide
  kRemapped,     // Decide said cast (r4 changed to the body phase)
  kCount,
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
    s += ", mode off " + n(Get(C::kModeOff));
    s += ", no memory " + n(Get(C::kNoMemory));
    s += ", vehicle ptr " + n(Get(C::kVehiclePtr));
    s += ", ineligible " + n(Get(C::kIneligible)) + " [+593 clear " + n(Get(C::kSideViews)) + ", +6478 set " +
         n(Get(C::kHidden)) + ", +6467 set " + n(Get(C::kDisabled)) + ", +123 set " + n(Get(C::kGhost)) +
         ", +116 zero " + n(Get(C::kNotVisible)) + ", +120 zero " + n(Get(C::kNotEnabled)) + "]";
    s += ", entity ptr " + n(Get(C::kEntityPtr)) + ", camera ptr " + n(Get(C::kCameraPtr));
    s += ", distance not finite " + n(Get(C::kDistance));
    s += ", reached Decide " + n(Get(C::kDecided)) + ", cast " + n(Get(C::kCast));
    s += "; traffic route (ambient client): phase loop " + n(Get(A::kLoopCalls)) + " cascade meetings (off " +
         n(Get(A::kLoopOff)) + ", mask widened " + n(Get(A::kLoopWidened)) + ")";
    s += ", item draw calls " + n(Get(A::kLeafCalls)) + " (cascade " + n(Get(A::kLeafCascade)) + ", other phases " +
         leaf_phases_.Text() + ")";
    s += ", mode off " + n(Get(A::kModeOff)) + ", no memory " + n(Get(A::kNoMemory)) + ", item ptr " +
         n(Get(A::kItemPtr)) + ", car ptr " + n(Get(A::kCarPtr)) + ", buffer " + n(Get(A::kBuffer));
    s += ", ineligible " + n(Get(A::kIneligible)) + " [+36 zero " + n(Get(A::kEnabled)) + ", car +500 " +
         n(Get(A::kCarFlags)) + ", models<3 " + n(Get(A::kModels)) + ", not visible " + n(Get(A::kNotVisible)) +
         ", LOD<0 " + n(Get(A::kLod)) + ", no body " + n(Get(A::kNoBody)) + "]";
    s += ", camera ptr " + n(Get(A::kCameraPtr)) + ", position " + n(Get(A::kPosition)) + ", distance not finite " +
         n(Get(A::kDistance));
    s += ", reached Decide " + n(Get(A::kDecided)) + ", cast " + n(Get(A::kRemapped));
    return s;
  }

 private:
  std::array<std::atomic<uint64_t>, static_cast<size_t>(CarGate::kCount)> car_{};
  std::array<std::atomic<uint64_t>, static_cast<size_t>(AmbientGate::kCount)> ambient_{};
  ValueCounts car_phases_;
  ValueCounts leaf_phases_;
  std::atomic<uint64_t> calls_{0};
};

// When the line is printed: once the calls reach kFirstAfterCalls, then every kEveryNs. Single threaded (the caller
// holds a lock); `now_ns` is any monotonic clock.
struct LogSchedule {
  static constexpr uint64_t kFirstAfterCalls = 2000;
  static constexpr int64_t kEveryNs = 30'000'000'000;

  bool first_done = false;
  int64_t last_ns = 0;

  bool Due(uint64_t calls, int64_t now_ns) {
    if (!first_done) {
      if (calls < kFirstAfterCalls) {
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
