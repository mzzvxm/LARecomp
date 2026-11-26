#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "env_hold_link.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class EdramTargets;
class ResolveCache;

namespace env_fallback {

// The whole host handoff is one push word: the 2D bindless slot in the low
// half, the host width of one hemisphere in the high half. Zero means the
// map cannot take any of the reflection weight this frame.
uint32_t MapWord(ResolveCache& resolves, const EdramTargets& edram, Device& device,
                 BindlessHeap& heap, uint64_t written);
bool Enabled();

// Shader handoff (hlsl/env_fallback.hlsli): EnvFallbackSample(word,
// probeToReceiverWorld, reflectedWorld, perceptualRoughness, groundReceiver).
// The ray is unit length and both vectors are in the game's world axes (+Y
// up). The first vector is relative to the capture eye, in metres; for the
// camera-centered capture it is the rotated view-space position plus the
// current view eye's displacement from the eye that rendered the map.
// The result is unexposed map radiance. Apply the scene's exposure once
// before blending it with exposure-scaled SSR/RT radiance.

inline constexpr uint32_t kLevels = 4;
inline constexpr float kProxyRadius = 40.0f;
inline constexpr float kProxyFadeStart = 0.75f;
inline constexpr float kSkyFadeEnd = 0.35f;

constexpr uint32_t PackMapWord(uint32_t slot, uint32_t hemisphere_size) {
  if (slot > 0xFFFFu || hemisphere_size == 0 || hemisphere_size > 0xFFFFu) {
    return 0;
  }
  return (hemisphere_size << 16) | slot;
}

inline float Mip(float perceptual_roughness) {
  return std::clamp(perceptual_roughness, 0.0f, 1.0f) * float(kLevels - 1);
}

struct Vec3 {
  float x, y, z;
};

struct Vec2 {
  float x, y;
};

constexpr bool LevelMatches(uint64_t written, uint64_t level_write, bool complete) {
  return written != 0 && level_write == written && complete;
}

// A hold can reuse only the complete capture whose origin we recorded. Each
// frame still needs its own marker, so views without an env pass cannot
// inherit a probe from the previous view.
class ProbeFrame {
 public:
  void EndFrame() { sampled_ = false; }

  void Note(uint32_t renderer, uint64_t written, bool refreshed, const env_hold::Eye& eye) {
    if (refreshed) {
      renderer_ = renderer;
      written_ = written;
      origin_ = eye;
    }
    sampled_ = renderer != 0 && renderer == renderer_ && LevelMatches(written_, written, true);
    view_ = eye;
  }

  uint64_t written() const { return sampled_ ? written_ : 0; }
  bool has_origin() const { return sampled_ && origin_.valid && view_.valid; }
  std::array<float, 3> CameraDelta() const {
    if (!has_origin()) return {};
    return {view_.position[0] - origin_.position[0], view_.position[1] - origin_.position[1],
            view_.position[2] - origin_.position[2]};
  }

 private:
  uint32_t renderer_ = 0;
  uint64_t written_ = 0;
  bool sampled_ = false;
  env_hold::Eye origin_;
  env_hold::Eye view_;
};

inline float Smooth(float lo, float hi, float value) {
  const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Inside the sphere, take the forward exit. Fade back to a direction at
// infinity for distant receivers and for reflected rays towards the sky.
inline Vec3 Direction(Vec3 probe_to_receiver, Vec3 ray, bool ground_receiver) {
  if (!ground_receiver || ray.y >= kSkyFadeEnd) {
    return ray;
  }
  const Vec3 q{probe_to_receiver.x / kProxyRadius, probe_to_receiver.y / kProxyRadius,
               probe_to_receiver.z / kProxyRadius};
  const float d2 = Dot(q, q);
  if (!(d2 < 1.0f)) {
    return ray;
  }
  const float b = Dot(q, ray);
  const float exit = std::sqrt(b * b + (1.0f - d2)) - b;
  const float weight = (1.0f - Smooth(kProxyFadeStart * kProxyFadeStart, 1.0f, d2)) *
                       (1.0f - Smooth(0.0f, kSkyFadeEnd, ray.y));
  const float along_ray = 1.0f + weight * (exit - 1.0f);
  const Vec3 corrected{q.x * weight + ray.x * along_ray, q.y * weight + ray.y * along_ray,
                       q.z * weight + ray.z * along_ray};
  const float length = std::sqrt(Dot(corrected, corrected));
  return {corrected.x / length, corrected.y / length, corrected.z / length};
}

inline float RimInset(float mip, float hemisphere_size) {
  return 4.0f * std::exp2(mip) / hemisphere_size;
}

inline Vec2 AtlasUv(Vec3 ray, bool left, float inset) {
  const float scale = 1.0f / (1.0f + std::abs(ray.z));
  const float radius = std::hypot(ray.x, ray.y) * scale;
  const float limited_scale = radius > 1.0f - inset ? scale * (1.0f - inset) / radius : scale;
  return {left ? 0.25f + 0.25f * ray.x * limited_scale : 0.75f - 0.25f * ray.x * limited_scale,
          0.5f - 0.5f * ray.y * limited_scale};
}

inline float RightWeight(float z, float inset) { return Smooth(-inset, inset, z); }

}  // namespace env_fallback
}  // namespace mcla::gpu
