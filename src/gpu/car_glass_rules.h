#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's car windows reflecting the city: the rules (CITY-RT M2 package R3b)
// ===========================================================================
// WHAT WAS THERE. The glass of the player's car is drawn after the world, blended, and writes no depth: the picture under a window is the interior, and the only reflection on it is the game's own
// environment map read in the glass's pixel shader (every lamp a blob, and the street never in it). The ray traced reflections of the paint (city_refl.h, R1) cannot help: the paint's pass finds
// the car's pixel with a primary ray in a window around the HISTORY's depth, and the history is made of vehicle pixels the paint's shader drew; a window is no such pixel.
//
// WHAT THIS ADDS (CITY-RT-M2.md section 6, "R3 chrome and glass of the player's car", (b)): an ADDITIVE pass at the history trigger, after the screen-space reflections (the last pass that adds to the
// colour there) and the car history's own pass (so the history holds no glass reflection: a reflection is not a reflection of a reflection), in two steps:
//   1. a compute dispatch (hlsl/car_rt_refl_glass.hlsli, the glass variant of car_rt_refl_cs.hlsl: the same file, so TraceRay and the city hit library are the paint's) over the rectangle of the
//      player's box in the history's grid: a primary ray from the camera against the player's GLASS instances alone (instance mask 0x02), cast as far as the picture's own depth at the pixel (lin_depth,
//      the screen shortcut's image) plus a slack, finds the pane that is the nearest thing at that pixel (nothing in front of it: not the dashboard, not a wall), with its point and mesh normal; ONE mirror
//      ray from the pane through the cars' structures and the city's (the clear-coat ray of the paint's pass without the cone); its hit made a colour by the library (the picture's own colour where the hit
//      is on the screen, shaded from its material where not); times the glass's Fresnel (Schlick, F0 0.04, power 5, at most 0.6: Fresnel below) and the hit's weight: the colour to ADD, in the main colour
//      target's own units. The texel is written as rgb that colour, alpha 1 (a pane; 0 for none). A ray that finds nothing (the sky) adds nothing: the game's own reflection of the environment map is
//      already in the colour and stays.
//   2. a full-screen additive draw into the main colour (hlsl/car_glass_apply_ps.hlsl, blend ONE ONE on the colour channels): the image read at each pixel with its four nearest texels weighted by their
//      bilinear share times their alpha (the mean of the panes' colour, no black from the texels beside the glass), fading in over the first half of the coverage, taps and pixels kept inside the
//      rectangle the trace wrote (the texels outside hold the frames before's: ApplyPixel below is the same arithmetic, tested).
//
// WHERE IT RUNS: wherever the paint's ray traced pass runs with the city on (CarRtRefl::OnHistory: the structures of the frame, the city's rows, a level above off), the glass is served at every level
// (city_refl_rules.h Serves), and mcla_gpu_city_refl_glass (debug-only, true) is its A/B. It needs lin_depth (the screen owner's product the city's rows carry: without it nothing tells a pane from what is
// in front of it, so no pass), a valid box of the player's car, a main colour target at the depth's samples and scale, and its two pipelines (made in the background where they were not listed at the
// start; a frame never waits for them).
//
// COST (an estimate, nothing was run): the rectangle is the car's box in the history's grid, a quarter to a third of the picture on a chase camera (half the height of the picture at 4K, 1080 rows at 1080p),
// one primary ray against a handful of glass instances and, for a texel on a pane (a few percent of the rectangle) one mirror ray with the library's hit: 0.05 to 0.15 ms on an RTX 5080 class card; the apply
// is one full-screen triangle that returns at once outside the rectangle (0.02 ms). Memory: one image of the history's size (R16G16B16A16_FLOAT, 8 bytes a texel: 16 MiB at 4K with the half size history),
// released after kReleaseAfterFrames frames without the pass. CHECK on the first run: the pass timing rows "car glass refl" and "car glass apply".
//
// This header includes no renderer: car_glass_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "car_rt_opp_refl_rules.h"
#include "car_rt_refl_rules.h"
#include "car_rt_scene_rules.h"
#include "city_refl_rules.h"

namespace mcla::gpu::car_glass {

namespace opp = car_rt_opp_refl;
namespace rt = car_rt_refl;
namespace rr = car_rt_rules;
namespace cr = city_refl;

// ---------------------------------------------------------------------------------------------------------------------
// the parts and the numbers (hlsl/car_rt_refl_glass.hlsli keeps its own copy: change one, change the other; the test checks it)
// ---------------------------------------------------------------------------------------------------------------------
// The primary ray finds the player's glass, and nothing else.
inline constexpr uint32_t kPrimaryParts = rr::kMaskGlass;
static_assert(kPrimaryParts == 0x02 && (kPrimaryParts & (rr::kMaskBody | rr::kMaskTyres | rr::kMaskDriver | rr::kMaskOthers | rr::kMaskOtherChrome | rr::kMaskOtherPaint | cr::kPartCity)) == 0,
              "the glass pass's primary ray finds the player's glass instances alone");

inline constexpr float kF0 = 0.04f;           // the reflectance of glass facing the camera (the G-buffer class glass / the paint's clear coat: car_rt_hit_rules.h kCoatF0)
inline constexpr float kPower = 5.0f;         // Schlick's exponent
inline constexpr float kMaxFresnel = 0.6f;    // the most of the reflection a pane takes (a grazing view)
inline constexpr float kStrength = 1.0f;      // the pass's gain on the whole term
inline constexpr float kDepthSlack = 0.02f;   // m: the pane may be this far behind the picture's depth ...
inline constexpr float kDepthSlackRel = 0.01f;  // ... plus this share of it (the depth is read at a texel and stored in a float)
inline constexpr float kFarDepth = 10000.0f;  // m: the primary ray never looks further (the sky's depth is far)
inline constexpr float kCapUnits = 64.0f;     // the paint's cap on a reflected colour, in the paint's units (the history's: times 2^exp_bias)
inline constexpr float kApplyCoverageFrom = 0.5f;  // the share of the four taps that are pane under which a pixel adds nothing, and from which the term fades in
inline constexpr int32_t kRectMargin = 4;     // texels added round the box's rectangle (the pane's edge, the history's one texel lag)
inline constexpr uint32_t kReleaseAfterFrames = 120;  // frames without the pass after which the image goes back
inline constexpr uint32_t kGroup = 8;         // the shader's thread group is 8 x 8

// Schlick's Fresnel of the glass at N.V (the cosine between the view and the pane's normal), capped.
inline float Fresnel(float nv) {
  const float one_minus = std::clamp(1.0f - nv, 0.0f, 1.0f);
  return std::min(kF0 + (1.0f - kF0) * std::pow(one_minus, kPower), kMaxFresnel);
}

// How far the primary ray is cast for a pixel whose picture depth is `depth` (view space z in metres): a pane at the depth or nearer is the nearest thing there.
inline float PrimaryReach(float depth) { return std::min(depth * (1.0f + kDepthSlackRel) + kDepthSlack, kFarDepth); }

// The colour a pane adds for a hit: the colour the hit shows times the Fresnel times the hit's weight times the strength, held to the cap (kCapUnits in the paint's units; `units` the history's over
// the paint's, 2^exp_bias: the colour is in the history's).
inline void AddedColour(const float colour[3], float nv, float weight, float strength, float units, float out[3]) {
  const float scale = Fresnel(nv) * weight * strength;
  const float cap = kCapUnits * units;
  for (int c = 0; c < 3; ++c) {
    out[c] = std::min(std::max(colour[c], 0.0f) * scale, cap);
  }
}

// The colour a pane shows for a TraceRay answer: another car's hit takes its colour where the history shows it and its stand-in where not (the paint's blend) with the hidden weight; any other hit (the
// city's, which has visible 1; the player's own parts) takes the history's colour where the camera sees it, by the visible share.
inline void HitColour(bool other, float fade, float visible, const float seen[3], const float hidden[3], float hidden_weight, float colour[3], float* weight) {
  if (other) {
    for (int c = 0; c < 3; ++c) {
      colour[c] = hidden[c] + (seen[c] - hidden[c]) * visible;
    }
    *weight = fade * (hidden_weight + (1.0f - hidden_weight) * visible);
    return;
  }
  for (int c = 0; c < 3; ++c) {
    colour[c] = seen[c];
  }
  *weight = fade * visible;
}

// ---------------------------------------------------------------------------------------------------------------------
// the dispatch's rectangle
// ---------------------------------------------------------------------------------------------------------------------
// The texels of the history's grid the car's box covers, kRectMargin larger (opp::BoxRect: the box in the frame's view space, the history's lens and divisor); empty where the box is off the picture
// or invalid. A box that crosses the camera's plane has no finite rectangle: the whole image (the pane may be anywhere).
inline opp::TexelRect RectFor(const car_rt_instances::ViewBox& box, double tan_x, double tan_y, double render_w, double render_h, double divisor, int32_t width, int32_t height) {
  return opp::BoxRect(box, tan_x, tan_y, render_w, render_h, divisor, width, height, kRectMargin);
}

// ---------------------------------------------------------------------------------------------------------------------
// the decision of a frame
// ---------------------------------------------------------------------------------------------------------------------
enum class Why : uint8_t {
  kUsed = 0,
  kSettingOff,   // mcla_gpu_city_refl_glass is off, or the level does not serve the glass (the city is not in the reflections)
  kNoCity,       // the city's rows are not on this frame (the pass's own gate)
  kNoScreen,     // no lin_depth in the city's rows: nothing tells a pane from what is in front of it
  kNoBox,        // the player's box is not valid, or is off the picture
  kNoMask,       // the frame's top level has no glass instance (the glass was not told from the body: car_rt_masks, GlassRuleInformative)
  kNotReady,     // the pipelines are not made yet
  kNoImage,      // the image could not be made
  kNoTarget,     // the main colour target is not there at the depth's samples and scale
};

inline const char* WhyName(Why why) {
  switch (why) {
    case Why::kUsed: return "runs";
    case Why::kSettingOff: return "the glass is off (mcla_gpu_city_refl_glass, or the level)";
    case Why::kNoCity: return "the city is not in this frame's reflections";
    case Why::kNoScreen: return "the frame has no lin_depth (the screen owner's product)";
    case Why::kNoBox: return "the player's car box is not on the picture";
    case Why::kNoMask: return "no instance of the car is glass in this frame";
    case Why::kNotReady: return "the pipelines are not made yet";
    case Why::kNoImage: return "the image could not be made";
    case Why::kNoTarget: return "the main colour target is not there";
  }
  return "?";
}

struct Facts {
  bool wanted = false;       // CityRefl::glass_wanted(): the level serves the glass and the debug switch is on
  bool city_on = false;      // the city's rows of this frame are on
  bool screen = false;       // the rows carry lin_depth
  bool box = false;          // a non-empty rectangle
  bool glass_instance = true;  // the top level holds an instance with the glass bit (true where the host cannot tell)
  bool ready = false;        // both pipelines
};

inline Why Decide(const Facts& f) {
  if (!f.wanted) return Why::kSettingOff;
  if (!f.city_on) return Why::kNoCity;
  if (!f.screen) return Why::kNoScreen;
  if (!f.box) return Why::kNoBox;
  if (!f.glass_instance) return Why::kNoMask;
  if (!f.ready) return Why::kNotReady;
  return Why::kUsed;
}

// ---------------------------------------------------------------------------------------------------------------------
// the apply pass's push block (hlsl/car_glass_apply_ps.hlsl GlassApplyPush) and its arithmetic
// ---------------------------------------------------------------------------------------------------------------------
struct ApplyPush {
  uint32_t image[4];  // the glass image's bindless slot, its width and height in texels, the divisor
  uint32_t rect[4];   // x0, y0, x1, y1 of the texels the trace wrote
  float sizes[4];     // the picture's width and height in pixels, two reserved
};
static_assert(sizeof(ApplyPush) == 48 && sizeof(ApplyPush) <= 64, "the host passes' push range is 64 bytes");

// What one texel of the image holds: the colour to add (zero where there is no pane), the pane's alpha (0 or 1).
struct Texel {
  float rgb[3] = {0.0f, 0.0f, 0.0f};
  float alpha = 0.0f;
};

// The pixel's place in texels of the image: texel i stands for the render pixel floor((i + 0.5) divisor), centred at i * divisor + floor(divisor / 2) + 0.5, so the pixel p is at
// (p - floor(divisor / 2)) / divisor in texel units; `base` the texel below it and `frac` the share towards the next.
inline void TapPlace(int pixel, int divisor, int* base, float* frac) {
  const float f = float(pixel - divisor / 2) / float(divisor);
  const float lo = std::floor(f);
  *base = int(lo);
  *frac = f - lo;
}

// The apply's term for pixel (px, py): `at(x, y)` the image's texel. The same arithmetic as the pixel shader: nothing outside the rectangle, the four taps clamped into it, weighted by bilinear share
// times alpha, the mean colour times the fade-in of the coverage.
template <class At>
inline void ApplyPixel(int px, int py, int divisor, const int rect[4], At&& at, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  divisor = std::max(divisor, 1);
  const int own_x = int(std::floor((float(px) + 0.5f) / float(divisor)));
  const int own_y = int(std::floor((float(py) + 0.5f) / float(divisor)));
  if (own_x < rect[0] || own_y < rect[1] || own_x > rect[2] - 1 || own_y > rect[3] - 1) {
    return;
  }
  int bx, by;
  float fx, fy;
  TapPlace(px, divisor, &bx, &fx);
  TapPlace(py, divisor, &by, &fy);
  float sum[4] = {0, 0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    const int ox = k & 1, oy = k >> 1;
    const int tx = std::clamp(bx + ox, rect[0], rect[2] - 1);
    const int ty = std::clamp(by + oy, rect[1], rect[3] - 1);
    const float w = (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy);
    const Texel t = at(tx, ty);
    sum[0] += w * t.rgb[0];
    sum[1] += w * t.rgb[1];
    sum[2] += w * t.rgb[2];
    sum[3] += w * t.alpha;
  }
  if (sum[3] < kApplyCoverageFrom) {
    return;
  }
  const float fade = std::clamp((sum[3] - kApplyCoverageFrom) / (1.0f - kApplyCoverageFrom), 0.0f, 1.0f);
  for (int c = 0; c < 3; ++c) {
    out[c] = sum[c] / sum[3] * fade;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------------------------------------------------
inline uint64_t HeldBytes(uint32_t width, uint32_t height) { return uint64_t(width) * uint64_t(height) * 8u; }

// ---------------------------------------------------------------------------------------------------------------------
// the log
// ---------------------------------------------------------------------------------------------------------------------
// One window of the log: every kLogEveryPasses passes.
inline constexpr uint32_t kLogEveryPasses = 300;
struct Window {
  uint64_t first_frame = 0;
  uint32_t passes = 0;
  uint64_t texels = 0;      // the rectangles' areas summed
  uint64_t not_run[9] = {};  // frames the pass was wanted and did not run, by Why
};

inline std::string WindowText(const Window& w, uint64_t last_frame) {
  char buf[400];
  const double n = double(std::max<uint32_t>(w.passes, 1));
  std::snprintf(buf, sizeof(buf),
                "[car glass] frames %llu..%llu: %u passes, the rectangle %.0f texels on average; frames not run: no city %llu, no lin_depth %llu, no box %llu, no glass instance %llu, pipelines not ready %llu, no target %llu",
                static_cast<unsigned long long>(w.first_frame), static_cast<unsigned long long>(last_frame), w.passes, double(w.texels) / n,
                static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNoCity)]), static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNoScreen)]),
                static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNoBox)]), static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNoMask)]),
                static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNotReady)]), static_cast<unsigned long long>(w.not_run[uint32_t(Why::kNoTarget)]));
  return buf;
}

}  // namespace mcla::gpu::car_glass
