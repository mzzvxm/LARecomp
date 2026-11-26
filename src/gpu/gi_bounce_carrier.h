#pragma once
// ===========================================================================
// MCLA GPU renderer -- WHO CARRIES A LIGHT'S BOUNCE (GI-BOUNCE-JOIN, perf/GI-BOUNCE.md section JOIN): the one rule that joins the screen-space path (ssgi_lights.h) and the ray traced path
// (rt_emitters_rules.h, the probe field and the per-pixel pass) so that every light bounces exactly once, at every tier. Pure: no renderer, no guest, no SDK. gi_bounce_carrier_test.cpp holds it.
// ===========================================================================
// THE OWNER'S RULE (2026-11-22): every light (neon, headlights, street lamps, tail lamps, decorative lights) bounces: screen-space when ray tracing is off or unavailable, ray traced when it is on.
// Two halves were written apart: the screen-space path carries the light PASS's difference (the colour before and after the deferred light pass, last frame's, reprojected into the colour chain), the ray traced
// path lights what its rays hit with an emitters list (the probe field the STATIC ones, the per-pixel pass all of them; the world's own lamps are the lamp grid's). Where the field stands the screen-space bounce
// was skipped whole, so a MOVING car's lights bounced through neither path at HIGH. This rule closes it: a light is carried by exactly one path.
//
//   the light                                              RT off       field alone (HIGH)                           field + per-pixel pass (ULTRA)
//   a street lamp, the world's lamps (source kLamp)        screen       FIELD (the lamp grid)                          FIELD
//   a listed STATIC emitter (a parked car's lights)        screen       FIELD (the probes)                             FIELD
//   a listed MOVING emitter (a car on the road)            screen       SCREEN (lights-only chain)                     PIXEL PASS
//   a light the list does not hold (too far, over budget)  screen       screen                                         screen
//   an emitter of a group whose switch is off              none         none                                           none
//   a light too dark to bounce (day)                       none         none                                           none
// "screen" is the colour chain's lights' share; where the field stands it runs LIGHTS-ONLY (the chain's radiance is the lights' share alone: the field already carries everything else, ao_color_ps.hlsl
// AoLightsOnly). The screen-space bounce takes a light from the light pass by DRAWING it between the two captures: the lights it does not carry are drawn first, outside the pair (local_lights.cpp), so the pair's
// difference is the carried lights' light and nothing else, and nothing is counted twice.
//
// THE FIELD STANDS when the probe field replaces the screen-space bounce completely this frame (gi_apply_rules.h SsgiSkipped: fade over). While it fades in or out (0 < fade < 1: 0.4 s after a cut, 0.5 s going
// away) the old crossfade holds: the screen-space bounce carries EVERY light at (1 - fade) and the field its own at fade, so nothing is lost in between; only a MOVING light at HIGH is at (1 - fade) until the
// field has stood (the screen-space strength is the crossfade's, as it always was).
//
// THE SWITCHES. One coherent story, the four SHARED debug switches (mcla_gpu_gi_bounce_headlights / _neon / _rear / _other) govern the emitters' groups on BOTH paths: a group that is off is not gathered by the
// ray traced list and is not in the screen-space pair; all four off is no lights' share at all (the colour chain is the old one, ao.cpp PlanGi); mcla_gpu_ssgi_lights (false: the colour chain as it was) is the
// screen-space half's master and mcla_gpu_city_refl_lamps the reflections'. The world's lamps are in no group: they follow the screen path while no field stands and the field when it does.
// CHECK: confirm capture transitions and history rejection on D3D12 and Vulkan, moving-light bounce at RT High, and daylight pixel equality with no contributing lights.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

#include "rt_emitters_rules.h"
#include "ssgi_lights.h"

namespace mcla::gpu::gi_bounce {

namespace lg = ::larecomp::gpu;
namespace re = ::mcla::gpu::rt_emitters;

enum class Carrier : uint8_t {
  kScreen,     // the colour chain's lights' share (the screen-space bounce)
  kField,      // the probe field (the lamp grid and the static emitters)
  kPixelPass,  // the per-pixel pass (the moving emitters at ULTRA on a strong card)
  kNone,       // nobody: the light does not bounce (its group's switch is off, or it is too dark to light anything)
};

constexpr const char* CarrierName(Carrier c) {
  return c == Carrier::kScreen ? "screen" : (c == Carrier::kField ? "field" : (c == Carrier::kPixelPass ? "pixel pass" : "none"));
}

// What the field's emitters list (RtWorldFrame, rt_emitters_rules.h Build) says about a light of the light pass, by its id.
enum class Listed : uint8_t { kNo, kStatic, kMoving };

// What the frame says.
struct Frame {
  uint32_t groups = re::kGroupAll;  // the four switches' mask (rt_emitters::MaskOf)
  bool field_stands = false;        // the probe field replaces the screen-space bounce this frame (SsgiSkipped(share): fade over)
  bool pixel_pass = false;          // the per-pixel pass ran in this frame (it carries the listed moving emitters)
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
  if (!f.field_stands) {
    return Carrier::kScreen;
  }
  switch (listed) {
    case Listed::kStatic: return Carrier::kField;
    case Listed::kMoving: return f.pixel_pass ? Carrier::kPixelPass : Carrier::kScreen;
    case Listed::kNo: break;
  }
  return Carrier::kScreen;
}

// The light's place in the light pass: in the screen-space pair (drawn between the two captures) or outside it.
inline bool InPair(const Frame& f, const lg::Light& l, Listed listed, float night, float gain) {
  return Bounces(l, night, gain) && CarrierOf(f, l, listed) == Carrier::kScreen;
}

// "All four off = the lights' share off" (the one line both halves described): the screen-space half is off when no group is on.
constexpr bool AnyGroup(uint32_t groups) {
  return (groups & re::kGroupAll) != 0;
}

// What the light pass is handed (Scene, once a frame, only while the AO wants the lights' pair): the frame's facts for the rule, the field's list and the capture to make between the pass's two runs of draws.
struct Pair {
  Frame frame;
  std::function<Listed(uint64_t)> listed;  // the world frame's emitters list by light id (null: nothing is listed); asked only where frame.field_stands
  std::function<bool()> capture_before;    // makes the "before" capture (screen_->CaptureLightsBefore); true when it recorded one
};

// ---------------------------------------------------------------------------
// the AO's plan (ao.cpp PlanGi)
// ---------------------------------------------------------------------------

// What the bounce light's plan says about the lights this frame. `residual`: the light pass makes the RESIDUAL pair (the field stands: only the lights nobody else carries are between the captures);
// `only`: the colour chain runs LIGHTS-ONLY, on the residual pair the last frame left. Everywhere else (no field, or a field still fading in or out: the crossfade) both are false and the pair, if the lights' share
// is wanted, is the whole pass's, as it was.
struct JoinPlan {
  bool residual = false;
  bool only = false;
};
// `wants`: the lights' share is wanted (the setting, a group on, the owner extracting); `stands`: the field replaces the screen-space bounce completely this frame (gi_apply_rules.h SsgiSkipped(share));
// `ready`: the last frame left a residual pair (ScreenShared::LightsResidualReady); `hold`: the lights-only chain's hold (stepped once a frame the plan is made).
inline JoinPlan PlanFor(bool wants, bool stands, bool ready, ssgi_lights::OnlyHold* hold) {
  JoinPlan plan;
  if (!stands) {
    hold->Step(false, false);
    return plan;
  }
  plan.residual = wants;
  plan.only = hold->Step(wants, wants && ready);
  return plan;
}

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
