#pragma once
// LT-6 routes car lights to virtual bounce. City lights retain the SSGI
// and probe-field crossfade. Reflection hit shading has its own light list.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

#include "rt_emitters_rules.h"

namespace mcla::gpu::gi_bounce {

namespace lg = ::larecomp::gpu;
namespace re = ::mcla::gpu::rt_emitters;

enum class Carrier : uint8_t {
  kVirtual,    // the surface struck by a car light (LT-6)
  kScreen,     // the colour chain's lights' share (the screen-space bounce)
  kField,      // the probe field (the lamp grid and the static emitters)
  kNone,       // nobody: the light does not bounce (its group's switch is off, or it is too dark to light anything)
};

constexpr const char* CarrierName(Carrier c) {
  return c == Carrier::kVirtual ? "virtual" : c == Carrier::kScreen ? "screen" : c == Carrier::kField ? "field" : "none";
}

// What the field's emitters list (RtWorldFrame, rt_emitters_rules.h Build) says about a light of the light pass, by its id.
enum class Listed : uint8_t { kNo, kStatic, kMoving };

// What the frame says.
struct Frame {
  uint32_t groups = re::kGroupAll;  // the four switches' mask (rt_emitters::MaskOf)
  bool field_stands = false;        // the probe field replaces the screen-space bounce this frame (SsgiSkipped(share): fade over)
  bool virtual_on = true;           // LIGHT BOUNCE
};

// The world's own lamps: a street lamp or a window the game does not draw as a light volume (the lamp grid and the glow materials of the ray traced hit shading light the bounce of these).
inline bool IsWorldLamp(const lg::Light& l) {
  return l.source == lg::LightSource::kLamp;
}

// A light is worth a bounce when it adds something a picture could show: the world frame's own test (rt_emitters_rules.h kMinColour), by the pass's units (a game lamp's law is times the night factor, the
// headlight law's colour holds its own; the gain is the pass's). By day almost every light fails it, so a frame with no lit lamp has no bracket and no capture.
inline bool Bounces(const lg::Light& l, float night, float gain) {
  if (!(l.range > 0.0f)) {
    return false;
  }
  const float n = std::isfinite(night) ? std::min(std::max(night, 0.0f), 1.0f) : 0.0f;
  const float g = std::isfinite(gain) ? std::max(gain, 0.0f) : 0.0f;
  const float scale = g * (l.law == lg::LightLaw::kHeadlight ? 1.0f : n);
  float brightest = 0.0f;
  for (int k = 0; k < 3; ++k) {
    const float v = l.color[k] * scale;
    if (std::isfinite(v) && v > brightest) {
      brightest = v;
    }
  }
  return brightest >= re::kMinColour;
}

// The ONE rule: which path carries `l`. `listed` is the field's list's word about it (kNo where the field is not standing or the light has no id).
inline Carrier CarrierOf(const Frame& f, const lg::Light& l, Listed listed) {
  if (IsWorldLamp(l)) {
    return f.field_stands ? Carrier::kField : Carrier::kScreen;
  }
  if ((f.groups & re::GroupOf(l)) == 0) {
    return Carrier::kNone;
  }
  if (f.field_stands && listed == Listed::kStatic) return Carrier::kField;
  if (re::IsVehicleLight(l)) return f.virtual_on ? Carrier::kVirtual : Carrier::kNone;
  return f.field_stands ? Carrier::kNone : Carrier::kScreen;
}

// During a field fade, an already listed parked light has one total share.
constexpr float VirtualShare(bool listed_static, float field_share) {
  return listed_static ? 1.0f - field_share : 1.0f;
}

// The light's place in the light pass: in the screen-space pair (drawn between the two captures) or outside it.
inline bool InPair(const Frame& f, const lg::Light& l, Listed listed, float night, float gain) {
  return Bounces(l, night, gain) && CarrierOf(f, l, listed) == Carrier::kScreen;
}

// The virtual pass can stay idle when all car-light groups are off.
constexpr bool AnyGroup(uint32_t groups) {
  return (groups & re::kGroupAll) != 0;
}

// What the light pass is handed (Scene, once a frame, only while the AO wants the lights' pair): the frame's facts for the rule, the field's list and the capture to make between the pass's two runs of draws.
struct Pair {
  Frame frame;
  std::function<bool()> capture_before;    // makes the "before" capture (screen_->CaptureLightsBefore); true when it recorded one
};

// ---------------------------------------------------------------------------
// the partition of a light pass's draws (local_lights.cpp OnResolve)
// ---------------------------------------------------------------------------

// The order of `count` lights (at most kMax) with the ones outside the pair first and the pair's last, each side in its own order: order[slot] = the source index. Returns how many are outside. Neon tubes of
// one car stay together when they share a side (they are adjacent in the pass's selection and the partition is stable: PlanDraws merges consecutive lights of a car).
template <size_t kMax>
inline uint32_t PartitionOrder(const std::array<bool, kMax>& in_pair, uint32_t count, std::array<uint32_t, kMax>* order) {
  count = std::min<uint32_t>(count, uint32_t(kMax));
  uint32_t at = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!in_pair[i]) {
      (*order)[at++] = i;
    }
  }
  const uint32_t outside = at;
  for (uint32_t i = 0; i < count; ++i) {
    if (in_pair[i]) {
      (*order)[at++] = i;
    }
  }
  return outside;
}

}  // namespace mcla::gpu::gi_bounce
