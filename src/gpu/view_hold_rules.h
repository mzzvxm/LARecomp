#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::view_hold {

using Vec3 = std::array<float, 3>;

struct Camera {
  Vec3 position;
  Vec3 forward;
  Vec3 up;
};

struct Sample {
  Camera camera{};
  uint64_t time_ms = 0;
  uint32_t world = 0;
  uint32_t clock_jumps = 0;
  int32_t weather = -1;
  float brightness = -1.0f;
  bool cut = false;
};

struct Limits {
  uint64_t max_age_ms = 66;
  float travel_metres = 8.0f;
  float turn_cosine = 0.93969262f;  // 20 degrees
  float still_metres = 0.05f;
  float still_cosine = 0.99996192f;  // 0.5 degrees
  float brightness_ratio = 1.5f;
};

inline float Dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline bool Positioned(const Camera& camera) {
  for (float v : camera.position) {
    if (!(std::abs(v) <= 1.0e6f)) return false;
  }
  const float forward = Dot(camera.forward, camera.forward);
  const float up = Dot(camera.up, camera.up);
  return forward >= 0.9f && forward <= 1.1f && up >= 0.9f && up <= 1.1f &&
         std::abs(Dot(camera.forward, camera.up)) < 0.1f;
}

inline bool Turned(const Vec3& a, const Vec3& b, float cosine) {
  return Dot(a, b) < cosine * std::sqrt(Dot(a, a) * Dot(b, b));
}

enum class Decision : uint8_t { kHold, kMissing, kCut, kWorld, kLight, kTravel, kTurn, kAge };

// Compare against the camera and lighting that made the product. Committing a
// refresh rebases all thresholds together. Stationary noise inside the two
// deadbands cannot start an age-driven refresh, even in a long pause.
inline Decision Decide(const Limits& limits, const Sample& capture, const Sample& now,
                       bool have_product) {
  if (!have_product || !Positioned(capture.camera) || !Positioned(now.camera)) return Decision::kMissing;
  if (now.cut || now.time_ms < capture.time_ms) return Decision::kCut;
  if (now.world != capture.world) return Decision::kWorld;
  if (now.clock_jumps != capture.clock_jumps || now.weather != capture.weather) return Decision::kLight;
  if (capture.brightness >= 0.0f && now.brightness >= 0.0f) {
    // The floor prevents tiny lighting differences in near-black scenes from
    // making either side of a ratio test unstable.
    const float old_light = capture.brightness + 0.01f;
    const float new_light = now.brightness + 0.01f;
    if (new_light > old_light * limits.brightness_ratio ||
        old_light > new_light * limits.brightness_ratio) return Decision::kLight;
  }
  const Vec3 delta = {now.camera.position[0] - capture.camera.position[0],
                      now.camera.position[1] - capture.camera.position[1],
                      now.camera.position[2] - capture.camera.position[2]};
  const float travel = Dot(delta, delta);
  if (travel > limits.travel_metres * limits.travel_metres) return Decision::kTravel;
  if (Turned(capture.camera.forward, now.camera.forward, limits.turn_cosine) ||
      Turned(capture.camera.up, now.camera.up, limits.turn_cosine)) return Decision::kTurn;
  if (now.time_ms - capture.time_ms >= limits.max_age_ms &&
      (travel > limits.still_metres * limits.still_metres ||
       Turned(capture.camera.forward, now.camera.forward, limits.still_cosine) ||
       Turned(capture.camera.up, now.camera.up, limits.still_cosine))) return Decision::kAge;
  return Decision::kHold;
}

struct MapSample {
  Camera car{};
  Vec3 angles{};
  Vec3 zoom{};
  Vec3 origin{};
  int64_t time_ns = 0;
  uint32_t owner = 0;
  uint32_t world = 0;
  uint32_t clock_epoch = 0;
  bool has_transform = false;
  bool cut = false;
};

inline constexpr int64_t kMapPeriodNs = 33'333'333;

inline bool ValidMapSample(const MapSample& sample) {
  if (!sample.owner || sample.time_ns < 0 || !Positioned(sample.car)) return false;
  const auto& forward = sample.car.forward;
  if (forward[0] * forward[0] + forward[2] * forward[2] < 0.01f) return false;
  if (sample.has_transform) {
    for (float v : sample.angles) {
      if (!std::isfinite(v)) return false;
    }
    for (float v : sample.zoom) {
      if (!std::isfinite(v)) return false;
    }
    for (float v : sample.origin) {
      if (!std::isfinite(v)) return false;
    }
  }
  return true;
}

inline Decision DecideMap(const MapSample& capture, const MapSample& now,
                          int64_t deadline_ns, bool have_product) {
  if (!have_product || !ValidMapSample(capture) || !ValidMapSample(now)) return Decision::kMissing;
  if (now.cut || now.clock_epoch != capture.clock_epoch || now.time_ns < capture.time_ns) return Decision::kCut;
  if (now.owner != capture.owner || now.world != capture.world ||
      now.has_transform != capture.has_transform) return Decision::kWorld;
  if (now.has_transform) {
    for (size_t i = 0; i < now.zoom.size(); ++i) {
      if (std::abs(now.zoom[i] - capture.zoom[i]) > 0.0001f) return Decision::kWorld;
    }
  }
  const Vec3 delta = {now.car.position[0] - capture.car.position[0],
                      now.car.position[1] - capture.car.position[1],
                      now.car.position[2] - capture.car.position[2]};
  if (Dot(delta, delta) > 4.0f ||
      (now.has_transform && (std::abs(now.origin[0] - capture.origin[0]) > 2.0f ||
                             std::abs(now.origin[2] - capture.origin[2]) > 2.0f))) {
    return Decision::kTravel;
  }
  // Compare headings in the road plane. Pitch and roll on a ramp do not
  // rotate the radar, and a wrap at north is a small change.
  const Vec3 old_heading = {capture.car.forward[0], 0.0f, capture.car.forward[2]};
  const Vec3 new_heading = {now.car.forward[0], 0.0f, now.car.forward[2]};
  constexpr float kTurnCosine = 0.99862953f;  // 3 degrees
  if (Turned(old_heading, new_heading, kTurnCosine)) return Decision::kTurn;
  if (now.has_transform) {
    for (size_t i = 0; i < now.angles.size(); ++i) {
      if (std::cos(now.angles[i] - capture.angles[i]) < kTurnCosine) return Decision::kTurn;
    }
  }
  // Moving blips and routes still update on a parked car.
  return now.time_ns >= deadline_ns ? Decision::kAge : Decision::kHold;
}

inline int64_t MapDeadline(const MapSample& now, int64_t previous, Decision decision) {
  if (decision == Decision::kAge && now.time_ns - previous < kMapPeriodNs) {
    return previous + kMapPeriodNs;
  }
  return now.time_ns + kMapPeriodNs;
}

}  // namespace mcla::gpu::view_hold
