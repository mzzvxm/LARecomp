#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "car_rt_ao_rules.h"
#include "city_rt_rules.h"
#include "rt_tier.h"

namespace mcla::gpu::rtao {

constexpr uint32_t TracedExtent(uint32_t render) { return render / 2 + render % 2; }
constexpr bool UpsampleNeeded(uint32_t plan_ao_height, uint32_t traced_height) { return plan_ao_height > traced_height; }

constexpr bool UseQuarterAt4k(bool enabled, uint32_t render_height, bool auto_governed, bool raw, bool interior) {
  return enabled && render_height >= 2160 && auto_governed && !raw && !interior;
}

struct Budget {
  uint32_t rays;
  bool boost;
  float reach_share;
  uint32_t history_cap;
  bool exact_alpha;
};

constexpr Budget BudgetFor(gpu_class::RtTier rung) {
  return rung == gpu_class::RtTier::kNone ? Budget{} : Budget{1, true, 0.50f, 32, true};
}

constexpr Budget kSharedBudget = {1, false, 0.50f, 32, true};
constexpr uint32_t kBoostBelow = 4;
constexpr uint32_t kRawFrameRays = 2;
constexpr uint32_t kRayMask = 0x9D;
static_assert(kRayMask == (car_rt_rules::kMaskBody | car_rt_rules::kMaskTyres | car_rt_rules::kMaskDriver |
                          car_rt_rules::kMaskOthers | city_rt::kMaskCity));
constexpr float kReachMin = 0.75f, kReachMax = 2.5f, kMaxRange = 90.0f;
constexpr float kMinAbove = 0.08f;
constexpr float kCabinPad = 0.1f;
constexpr uint32_t kCabinRays = 64;
constexpr uint32_t kCabinMask = car_rt_rules::kMaskBody | car_rt_rules::kMaskGlass;
constexpr uint64_t kReleaseAfterFrames = 120;

inline float Reach(float look_radius, float reach_share, float override_metres) {
  return std::clamp(override_metres > 0.0f ? override_metres : reach_share * look_radius, kReachMin, kReachMax);
}
inline float RayLength(float reach, float u) { return car_rt_ao::RayLength(reach, u); }
inline float Lift(float depth, float footprint) { return 0.006f + 3.0f * footprint + 0.001f * depth; }
inline float CarReach(float reach, float car_reach_share) { return reach * std::clamp(car_reach_share, 0.0f, 1.0f); }
inline float OpenShare(uint32_t hits, uint32_t rays) { return rays ? 1.0f - float(hits) / float(rays) : 1.0f; }
inline float VisibilityOf(float open, float power, float floor_) { return std::max(std::pow(open, power), floor_); }

constexpr bool CabinClosed(uint32_t hits, uint32_t rays) {
  return rays == kCabinRays && hits >= 62 && hits <= rays;
}

template <typename Box>
constexpr bool InsideBox(const float (&point)[3], const Box& box, float margin) {
  return box.valid && point[0] >= box.min[0] - margin && point[0] <= box.max[0] + margin &&
         point[1] >= box.min[1] - margin && point[1] <= box.max[1] + margin &&
         point[2] >= box.min[2] - margin && point[2] <= box.max[2] + margin;
}

inline car_rt_ao::Rect BoxPixelRect(const car_rt_ao::Box& box, float margin, float tan_x, float tan_y,
                                   uint32_t width, uint32_t height) {
  const float sun[3] = {};
  return car_rt_ao::RegionRect(&box, 1, margin, sun, 0.0f, tan_x, tan_y, width, height);
}

enum class Fallback : uint8_t {
  kNone, kNotWanted, kNotPlayView, kPipelineNotReady, kNoTopLevel, kNoCity, kImagesFailed,
};

constexpr const char* FallbackText(Fallback why) {
  switch (why) {
    case Fallback::kNone: return "none";
    case Fallback::kNotWanted: return "not wanted";
    case Fallback::kNotPlayView: return "not the play view";
    case Fallback::kPipelineNotReady: return "pipeline not ready";
    case Fallback::kNoTopLevel: return "no top level";
    case Fallback::kNoCity: return "no city";
    case Fallback::kImagesFailed: return "images failed";
  }
  return "unknown";
}

enum class Screen : uint8_t { kAuto, kOff, kOn, kWorld };

constexpr Screen ParseScreen(std::string_view text) {
  if (text == "off") return Screen::kOff;
  if (text == "on") return Screen::kOn;
  if (text == "world") return Screen::kWorld;
  return Screen::kAuto;
}

constexpr bool ScreenPassInPlay(Screen screen, bool tracing, bool cabin_screen) {
  return screen != Screen::kOff && (!tracing || cabin_screen || screen == Screen::kOn || screen == Screen::kWorld);
}

// Shared with hlsl/rtao_denoise_math.hlsli. Bit 1 belongs to the history view.
constexpr uint32_t kBlendScreenAll = 1u << 0;
constexpr uint32_t kBlendScreenWorld = 1u << 2;

constexpr uint32_t ScreenBlendFlags(Screen screen, bool force_open) {
  if (force_open) return 0u;
  if (screen == Screen::kOn) return kBlendScreenAll;
  if (screen == Screen::kWorld) return kBlendScreenWorld;
  return 0u;
}

enum class Interior : uint8_t { kAuto, kScreen, kRt };

constexpr bool ScreenCabin(Interior policy, bool interior, bool closed) {
  return interior && (policy == Interior::kScreen || (policy == Interior::kAuto && !closed));
}
constexpr bool KeepHistory(bool previous_ours, bool consecutive, bool same_size, bool camera_valid, bool cut) {
  return previous_ours && consecutive && same_size && camera_valid && !cut;
}

struct PlanInputs {
  bool wanted = false, play_view = false, pipelines = false, top_level = false, city = false, images = false;
  bool cabin_screen = false, raw = false, bounce_light = false;
  Screen screen = Screen::kAuto;
  Budget budget = {};
  uint32_t render[2] = {}, rays = 0, scale = 2;
  float look_radius = 3.0f, reach_override = 0.0f, range = 0.0f;
};

struct Plan {
  bool on = false;
  Fallback why = Fallback::kNotWanted;
  bool screen_pass = true;
  bool raw = false;
  uint32_t rays = 0;
  uint32_t traced[2] = {};
  float reach = 0, range = 0;
};

inline Plan Decide(const PlanInputs& in) {
  Plan plan;
  if (!in.wanted) plan.why = Fallback::kNotWanted;
  else if (!in.play_view) plan.why = Fallback::kNotPlayView;
  else if (!in.pipelines) plan.why = Fallback::kPipelineNotReady;
  else if (!in.top_level) plan.why = Fallback::kNoTopLevel;
  else if (!in.city) plan.why = Fallback::kNoCity;
  else if (!in.images) plan.why = Fallback::kImagesFailed;
  else plan.why = Fallback::kNone;
  plan.on = plan.why == Fallback::kNone;
  plan.screen_pass = in.bounce_light || plan.why == Fallback::kNotWanted || plan.why == Fallback::kNotPlayView ||
                     ScreenPassInPlay(in.screen, plan.on, in.cabin_screen);
  if (plan.on) {
    plan.raw = in.raw;
    plan.rays = in.raw ? kRawFrameRays : (in.rays ? in.rays : in.budget.rays);
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const uint32_t scale = in.scale == 1 || in.scale == 4 ? in.scale : 2;
      plan.traced[axis] = scale == 2 ? TracedExtent(in.render[axis])
                                     : in.render[axis] / scale + (in.render[axis] % scale != 0);
    }
    plan.reach = Reach(in.look_radius, in.budget.reach_share, in.reach_override);
    plan.range = std::clamp(in.range, 0.0f, kMaxRange);
  }
  return plan;
}

constexpr uint32_t kBytesPerTracedPixel = 60;
constexpr uint64_t ImageBytes(uint32_t traced_w, uint32_t traced_h) { return uint64_t(traced_w) * traced_h * kBytesPerTracedPixel; }
constexpr uint64_t TermBytes(uint32_t ao_w, uint32_t ao_h, bool bounce_light) { return uint64_t(ao_w) * ao_h * (bounce_light ? 8 : 2); }
inline double EstimatedRays(uint32_t traced_w, uint32_t traced_h, uint32_t rays, double boost_share = 0.1) {
  return double(traced_w) * traced_h * (rays + boost_share);
}

constexpr uint32_t kFlagRaysShift = 0;
constexpr uint32_t kFlagBoost = 1u << 3;
constexpr uint32_t kFlagAlphaShift = 4;
constexpr uint32_t kFlagAlphaRefShift = 12;
constexpr uint32_t kFlagMs = 1u << 6;
constexpr uint32_t kFlagRaw = 1u << 7;
constexpr uint32_t kFlagCheckOpen = 1u << 8;
constexpr uint32_t kFlagCheckClosed = 1u << 9;
constexpr uint32_t kFlagInterior = 1u << 10;
constexpr uint32_t kFlagBlue = 1u << 11;
constexpr uint32_t kFlagProbe = 1u << 20;
constexpr uint32_t kFlagDepthHalf = 1u << 21;
constexpr uint32_t kFlagRate = 1u << 23;
constexpr uint32_t kFlagStats = 1u << 28;

struct RtAoPush {
  uint32_t slots[4], sizes[4], trace[4];
  float lens[4], look[4], box_min[4], box_max[4];
  uint32_t rate[4];  // history, motion table, previous lens x and y as float bits
};
static_assert(sizeof(RtAoPush) == 128);

}  // namespace mcla::gpu::rtao
