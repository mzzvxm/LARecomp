#pragma once
// ===========================================================================
// MCLA -- traffic and AI cars in the sun's shadow cascades (the TRAFFIC SHADOWS setting)
// ===========================================================================
// Owner, 2026-11-13: everything under a TRAFFIC car should be as dark as it is under the player's car.
//
// Why it is not [guest code, read from the recompiled functions; CHECK in a run]: Vehicle::Draw (0x82360518,
// the draw of every vehicle: world, garage, reflection) draws a vehicle into the four sun shadow cascades
// (render phases 0x20, 0x40, 0x80, 0x100; 0x1000 is the shadow collector that reads the atlas) only when
// the vehicle's byte +594 is set (0x82360668: `lbz r11,594(r30)`, `cmplwi cr6,r11,0`, `beq` over the call of
// sub_8235FA90, the routine that draws a car at the forced low LOD into the current phase). The constructor
// (0x8235AF90) writes the two bytes from its 4th argument (r6, the vehicle's slot):
//     +593 = (slot != -1)      the car takes part in the side views and the shadow phases at all
//     +594 = (slot ==  0)      the car casts into the sun shadow map
// so the player's car (slot 0) is the one vehicle the game ever draws into the cascades; traffic and the
// opponents (+593 = 1, +594 = 0) get only the game's soft quad decal under them. Nothing else in the draw
// reads +594 on the cascade path (sub_8235FA90 and everything it calls do not), the car materials have the
// cascade technique (VS_CarShadowDepthLocal, 27 car groups, no pixel shader), and sub_8235FA90 is already
// called for EVERY vehicle in the rain phase 0x10, so the routine copes with any car's model.
//
// What the hook does (hooks/traffic_shadow.cpp, 0x8236066C): for a vehicle that the game would leave out
// of a cascade (r11 = 0 where the player's car has 1) it answers 1, when the car is one this file picks.
// Nothing is written into the vehicle, so the +594 readers elsewhere (the vehicle update, the colour
// pass) see what they always saw, and the setting is live: the next cascade draw obeys it.
//
// Which cars (Selector): the cars within `radius` metres of the camera and, of those, the nearest `cars`.
// The census of the cars comes from the cascade draw itself: in a frame's first cascade phase every
// candidate car passes the hook once, so the distances of that frame give the cutoff the NEXT frame casts
// by (a car casts when its distance is at most the cutoff, which sits halfway between the Nth and the
// (N+1)th nearest, or at the radius when there are no more than N). One frame late is invisible: cars move
// a few metres a second, and a car that is new has no history but a distance. Only cars the main view
// sees are candidates (the game's own visibility word +116, which sub_8235FA90 tests anyway: a car it
// would not draw must not take a place), so the cars behind the camera never use up the count.
//
// What it costs [I, to be measured: chain-trafficcascade.ps1]: every casting car is drawn once per cascade
// phase (4) at the forced LOD (a body and four wheels at the low model, tens of draws, not the 210 of the
// player's car at full detail), with the guest's own CPU cost of a car draw on the render thread and the
// host's draw capture. ExtraDraws() is the rule of thumb the log line prints.
//
// What the renderer does with it (ao.cpp, ao_contact_ps.hlsl): the contact shadows' ground-under-a-car term
// (contact_trace.h UnderVehicle, the screen-space approximation made when only the player's car cast) stands
// aside for a car that now casts: the hook publishes the cutoff and the term leaves out the car whose shell
// is nearer than it (Publish / CastDistance, UnderCastWeight). Far cars, beyond the cutoff, keep it.
//
// Everything that decides something is here, pure (no ReXGlue, no renderer): traffic_shadow_test.cpp checks it.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "gpu_class.h"

namespace mcla::gpu::traffic_shadow {

enum class Mode : uint8_t {
  kOff,   // the game's own picture: only the player's car casts into the sun shadow map
  kNear,  // the nearest few cars within a short radius
  kAll,   // every car within a long radius, up to a larger count
};

constexpr std::string_view ModeName(Mode mode) {
  switch (mode) {
    case Mode::kNear:
      return "near";
    case Mode::kAll:
      return "all";
    default:
      return "off";
  }
}

// The setting's text after `auto` has been resolved (gpu_class.h): near and all; anything else is off.
inline Mode ParseMode(std::string_view resolved) {
  if (resolved == "near") {
    return Mode::kNear;
  }
  if (resolved == "all") {
    return Mode::kAll;
  }
  return Mode::kOff;
}

// The mode for a GPU class and the setting's own text (`auto` is the class's cell: off on Low, near on Mid and High).
inline Mode ModeFor(gpu_class::GpuClass cls, std::string_view setting_text) {
  return ParseMode(gpu_class::ResolveFor(cls, gpu_class::Effect::kTrafficShadows, setting_text));
}

// --- the render phases --------------------------------------------------------------------------------------

// The sun shadow cascades, nearest first [guest: sub_823120C8 / the renderer's phase list; the order is CHECK].
inline constexpr uint32_t kCascadePhases[4] = {0x20u, 0x40u, 0x80u, 0x100u};

// 1 to 4 for a cascade phase, 0 for anything else (the collector 0x1000 included: a car is not added to it).
constexpr int CascadeIndex(uint32_t phase) {
  for (int i = 0; i < 4; ++i) {
    if (phase == kCascadePhases[i]) {
      return i + 1;
    }
  }
  return 0;
}

// --- which cars ---------------------------------------------------------------------------------------------

inline constexpr float kNearRadius = 40.0f;  // m: `near` reaches this far from the camera ...
inline constexpr uint32_t kNearCars = 4;     // ... and takes at most this many cars
inline constexpr float kAllRadius = 150.0f;  // m: `all`
inline constexpr uint32_t kAllCars = 16;
inline constexpr uint32_t kMaxCars = 24;     // the most any setting may take (the census table is bigger: 32)

struct Limits {
  float radius = 0.0f;     // m from the camera; 0 or less: no car casts
  uint32_t cars = 0;       // the most cars that cast at once
  uint32_t cascades = 4;   // the cars are drawn into the first this many cascades (4: all)
};

// The limits of a mode with the debug overrides applied (0 or less keeps the mode's own value).
inline Limits LimitsFor(Mode mode, double radius_override, int cars_override, int cascades_override) {
  Limits limits;
  if (mode == Mode::kOff) {
    return limits;
  }
  limits.radius = mode == Mode::kNear ? kNearRadius : kAllRadius;
  limits.cars = mode == Mode::kNear ? kNearCars : kAllCars;
  if (radius_override > 0.0) {
    limits.radius = float(std::min(radius_override, 1000.0));
  }
  if (cars_override > 0) {
    limits.cars = uint32_t(std::min<int>(cars_override, int(kMaxCars)));
  }
  if (cascades_override >= 1 && cascades_override <= 4) {
    limits.cascades = uint32_t(cascades_override);
  }
  return limits;
}

// The guest's bytes of a vehicle that decide whether it is a candidate (offsets in hooks/traffic_shadow.cpp).
struct VehicleFlags {
  uint8_t casts_in_game = 0;  // +594: the game draws it into the cascades already (the player's car)
  uint8_t side_views = 0;     // +593: the vehicle takes part in the shadow phases at all
  uint8_t hidden = 0;         // +6478: the game's own "do not draw" byte, tested in the same condition
  uint8_t disabled = 0;       // +6467: sub_8235FA90 draws nothing for it
  uint8_t ghost = 0;          // +123: likewise
  uint16_t visible = 0;       // +116: seen by the main view (the visibility pass of this or the last frame)
  uint16_t enabled = 0;       // +120: likewise
};

// A car this file may add: the game leaves it out (+594 clear) although it is a shadow-casting kind
// (+593 set), and sub_8235FA90 would draw it (the tests it makes at its start, so that a car it would
// skip does not take one of the places).
constexpr bool Eligible(const VehicleFlags& f) {
  return f.casts_in_game == 0 && f.side_views != 0 && f.hidden == 0 && f.disabled == 0 && f.ghost == 0 &&
         f.visible != 0 && f.enabled != 0;
}

inline float Distance(const float camera[3], const float position[3]) {
  const float dx = camera[0] - position[0];
  const float dy = camera[1] - position[1];
  const float dz = camera[2] - position[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The distance up to which a car casts, from the distances of the frame's candidates (any order; NaN counts as
// far away): the radius when no more than `cars` of them are within reach, else halfway between the Nth and
// the (N+1)th nearest (or the radius if that is nearer): the nearest N cast and the rest do not, whatever
// their order of arrival.
inline float CutoffFor(const float* distances, uint32_t count, const Limits& limits) {
  if (!(limits.radius > 0.0f) || limits.cars == 0) {
    return 0.0f;
  }
  constexpr uint32_t kCapacity = 32;
  float sorted[kCapacity];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < kCapacity; ++i) {
    const float d = distances[i];
    // Only the cars within the radius compete for the places.
    if (std::isfinite(d) && d <= limits.radius) {
      sorted[n++] = d;
    }
  }
  if (n <= limits.cars) {
    return limits.radius;
  }
  std::sort(sorted, sorted + n);
  const float cut = 0.5f * (sorted[limits.cars - 1] + sorted[limits.cars]);
  return std::min(limits.radius, cut);
}

// What the frame did, for the log.
struct FrameStats {
  uint32_t candidates = 0;       // eligible cars the first cascade phase saw
  uint32_t casting = 0;          // of them the ones that cast
  float nearest = 0.0f;          // m, the nearest candidate (0 with none)
  float farthest_cast = 0.0f;    // m, the farthest car that cast (0 with none)
  float cutoff = 0.0f;           // m, the cutoff those cars were chosen by
  uint64_t frame = 0;            // frames counted since the start (boundaries seen)
};

// The census and the decision. Single threaded (the guest's render thread, the one that draws the phases).
// Decide() is called once per candidate car in every cascade phase of a frame, nearest cascade first; a frame
// ends (and the next cutoff is made) when a phase comes that is not above the last one's, or when the
// frame's census phase sees a car it has seen already (a frame that has one phase only).
class Selector {
 public:
  static constexpr uint32_t kCapacity = 32;

  // Whether the car (any id that is stable and different per car: its guest address) casts in `phase` now.
  // `distance` is its distance from the camera in metres. A phase that is not a cascade does not cast.
  bool Decide(uint32_t id, uint32_t phase, float distance, const Limits& limits) {
    const int cascade = CascadeIndex(phase);
    if (cascade == 0) {
      return false;
    }
    if (phase < last_phase_ || (phase == census_phase_ && Seen(id))) {
      EndFrame(limits);
    }
    last_phase_ = phase;
    if (census_phase_ == 0) {
      census_phase_ = phase;
    }
    const float reach = cutoff_ < 0.0f ? limits.radius : std::min(cutoff_, limits.radius);
    const bool cast = limits.radius > 0.0f && limits.cars > 0 && cascade <= int(limits.cascades) &&
                      std::isfinite(distance) && distance <= reach;
    if (phase == census_phase_) {
      if (count_ < kCapacity) {
        ids_[count_] = id;
        distances_[count_] = distance;
        ++count_;
      }
      ++candidates_;
      nearest_ = candidates_ == 1 ? distance : std::min(nearest_, distance);
      if (cast) {
        ++casting_;
        farthest_cast_ = std::max(farthest_cast_, distance);
      }
    }
    return cast;
  }

  // Closes the frame in progress (the next one casts by its census). Public for the caller that knows a frame
  // ended (a mode change or the game leaving the world: everything is forgotten).
  void EndFrame(const Limits& limits) {
    cutoff_ = CutoffFor(distances_, count_, limits);
    stats_.candidates = candidates_;
    stats_.casting = casting_;
    stats_.nearest = candidates_ ? nearest_ : 0.0f;
    stats_.farthest_cast = farthest_cast_;
    stats_.cutoff = cutoff_;
    ++stats_.frame;
    count_ = 0;
    candidates_ = 0;
    casting_ = 0;
    nearest_ = 0.0f;
    farthest_cast_ = 0.0f;
    census_phase_ = 0;
    last_phase_ = 0;
  }

  // Forget everything (the setting went off, or came on after a pause).
  void Reset() { *this = Selector(); }

  // The cutoff in force for this frame, in metres: negative before the first frame has been counted.
  float cutoff() const { return cutoff_; }
  const FrameStats& last() const { return stats_; }

 private:
  bool Seen(uint32_t id) const {
    for (uint32_t i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        return true;
      }
    }
    return false;
  }

  uint32_t ids_[kCapacity] = {};
  float distances_[kCapacity] = {};
  uint32_t count_ = 0;
  uint32_t candidates_ = 0;
  uint32_t casting_ = 0;
  float nearest_ = 0.0f;
  float farthest_cast_ = 0.0f;
  uint32_t last_phase_ = 0;
  uint32_t census_phase_ = 0;  // the phase whose cars are counted: the first one of the frame
  float cutoff_ = -1.0f;
  FrameStats stats_;
};

// --- the cost rule of thumb ---------------------------------------------------------------------------------

// Cascade draws a casting car adds a frame: every cascade it is drawn into times the draws of the car at the
// forced low LOD. kDrawsPerCarPerCascade is a first guess [I] for the body and wheels at LOD 1 (the player's
// car has about 210 draws in the main view at full detail, the whole cascade pass about 1,340 for the world and
// the player's car): the chain counts the real number (cascade draws a frame with the setting off and on,
// divided by the cars that cast).
inline constexpr uint32_t kDrawsPerCarPerCascade = 40;
constexpr uint32_t ExtraDraws(uint32_t casting_cars, uint32_t cascades) {
  return casting_cars * cascades * kDrawsPerCarPerCascade;
}

// --- what the renderer is told ------------------------------------------------------------------------------

// The cutoff, published once a frame by the hook (render thread) and read when the contact pass is recorded.
// 0 while nothing is published or the last publication is older than kStaleNs (the game's cascades stopped:
// night, rain, a menu; the hook stopped: the setting is off): the screen-space term then treats every car as
// one that does not cast.
inline constexpr int64_t kStaleNs = 300'000'000;

struct Published {
  std::atomic<float> cutoff{0.0f};
  std::atomic<int64_t> stamp_ns{0};
};

inline Published& State() {
  static Published state;
  return state;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void Publish(float cutoff_m, int64_t now_ns) {
  Published& s = State();
  s.cutoff.store(cutoff_m > 0.0f && std::isfinite(cutoff_m) ? cutoff_m : 0.0f, std::memory_order_relaxed);
  s.stamp_ns.store(now_ns, std::memory_order_release);
}

// Pure form of CastDistance: the published cutoff if it is fresh, else 0.
constexpr float FreshCutoff(float cutoff_m, int64_t stamp_ns, int64_t now_ns) {
  return (stamp_ns != 0 && now_ns - stamp_ns <= kStaleNs && now_ns >= stamp_ns) ? cutoff_m : 0.0f;
}

// The distance (metres from the camera) within which cars cast into the sun shadow map this frame; 0 for none.
inline float CastDistance() {
  const Published& s = State();
  const int64_t stamp = s.stamp_ns.load(std::memory_order_acquire);
  return FreshCutoff(s.cutoff.load(std::memory_order_relaxed), stamp, NowNs());
}

}  // namespace mcla::gpu::traffic_shadow
