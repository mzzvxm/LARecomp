#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's side mirrors show the real view behind, ray traced: the rules that need no renderer (MIRROR-RT)
// ===========================================================================
// Owner, 2026-11-21: the car's side mirrors are fine in the outside cameras and not functional in the first person ones (cockpit, bumper): the glass shows the game's environment map (the
// 512x256 sky and LOD city made around the CAMERA: a plausible blur, not the street behind). perf/MIRRORS-SURVEY.md section 5.2 is the design: a perfect mirror ray per glass pixel through the car
// RT scene (car_rt_scene.h: the player's car and the cars beside it, 18 m on the High class), its hits shaded by the cars' own materials (car_rt_hit_shade.hlsli, RT-HIT-SHADING), what it misses
// shown as the environment map at the mirror direction (the game's own glass shows that already: a miss is the sky and the far street), and, where the city is in the same top level
// (feat/city-rt-all: CityRefl's ResolveHit), the city. THE HOOK for that is hlsl/car_mirror_rt_cs.hlsl MirrorWorld(): today the environment; the city's library goes there.
//
//   FIND THE GLASS   car_mirror_patch_rules.h: a draw of the car's body that is a small flat patch, two of them across the car (a mesh's PatchStats are made when the mesh is read from guest
//                    memory; the pair is chosen in the frame's view space in CarRtInstances::Build). Its table row gets CAR_RT_FLAG_MIRROR (car_rt_scene_rules.h kFlagMirror); the draws of see-through
//                    glass (the windows) get CAR_RT_FLAG_SEE_THROUGH. The frame's pick travels in CarRtView::mirror.
//   TRACE            (car_mirror_rt.cpp, hlsl/car_mirror_rt_cs.hlsl; at the car RT trigger, the game's mid-scene depth copy, right after the structures are built): one compute dispatch for each mirror over
//                    the rectangle its box covers on the picture (at most kImageEdge pixels each way and Limits::max_pixels in all). A pixel casts four primary rays (a rotated grid inside it) against the
//                    car's opaque parts; the share that find a mirror glass first (through the windows) is the pixel's COVERAGE, and the first of them gives the glass's point and its smooth mesh normal (the
//                    slightly convex glass: a wider view, like a real mirror). ONE mirror ray goes off it along reflect(view, normal) against the cars (the player's own flank and rear, the cars beside).
//                    A car hit is shaded from its material and fades into the environment over the last quarter of the cars' reach (the scene holds cars only within that reach: a car is not cut off by
//                    the edge of the scene); the rest is the environment map at the direction. The pixel is written as rgb the colour, alpha the coverage times kStrength (a mirror is not a perfect
//                    reflector: the game's own glass shows through by the rest).
//   APPLY            one draw into the scene colour per mirror, scissored to its rectangle, blend SRC_ALPHA / INV_SRC_ALPHA on the colour channels (car_mirror_apply_ps.hlsl, one pipeline per sample count of
//                    the target, as the glass reflections' and the ambient occlusion's apply). The pixels outside the glass keep the game's.
//   ONLY WHERE IT SHOWS: a frame with no mirror pair in the car's draws (a camera in which the car is not drawn, a car with no such glass), one whose mirror is off the picture, too small to matter
//                    (kMinPixels) or over the cap does nothing at all; the car's structures are asked for in every frame the setting wants the pass (mcla_gpu_car_mirror_rt auto on the High class, on
//                    anywhere that can trace, off never), and the car self-reflections ask for them anyway where they are on.
//
// SETTING mcla_gpu_car_mirror_rt  auto (the default: on where the device can ray trace the car AND it is a strong card, the High class), off, on (any card that can trace and is not of the Low
//   class; the Mid class's smaller cap). Live. A card that cannot ray trace, or of the Low class (shared memory), keeps the game's mirror whatever this says (the shared low-resolution rear
//   view for those cards is a later package of the survey). The player's row is RT SIDE MIRRORS on GRAPHICS > REFLECTIONS (car_mirror_setting.h: the gate, the words, AUTO's label).
// mcla_gpu_car_mirror_rt_pixels (debug) the most pixels one mirror's rectangle may cover, in thousands (0: the class's: High 160, Mid 64); over it that mirror is left to the game.
// mcla_gpu_car_mirror_rt_strength (debug) the share of the game's glass the traced picture replaces (0.92).
//
// COST [E, nothing measured]: a mirror is 8 to 25 k pixels at 4K in the cockpit camera and a few hundred in a chase camera: 4 primary rays a pixel (most pixels of the rectangle's corners are off the glass
// and cost only those), one mirror ray and, for a hit on a car, the hit shading's two more (the sky's openness, the sun's visibility) for the pixels on the glass: about 8 rays a glass pixel,
// 0.03 to 0.1 ms on a 5080 class card for the pair, 3 to 4 times that on a card of the Mid class; the apply is two small draws. The pass is timed as "car mirror rt" and "car mirror apply".
//
// This header includes nothing of the renderer: car_mirror_rt_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "car_mirror_patch_rules.h"
#include "car_rt_hit_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_opp_refl_rules.h"
#include "rt_hit_frame.h"
#include "gpu_class.h"
#include "temporal_math.h"

namespace mcla::gpu::car_mirror {

// ---------------------------------------------------------------------------
// the numbers shared with hlsl/car_mirror_rt_cs.hlsl and hlsl/car_mirror_apply_ps.hlsl (the test checks that they hold them)
// ---------------------------------------------------------------------------

inline constexpr float kStrength = 0.92f;       // the share of the game's glass the traced picture replaces
inline constexpr float kFadeFrom = 0.75f;       // the share of the cars' reach up to which a hit on a car weighs 1 (car_rt_hit_rules.h Level::others_fade_from of the High class)
inline constexpr float kEnvLod = 0.5f;          // the environment map's level for a mirror (the class's of the paint: kClassEnvLod)
inline constexpr float kLift = 0.003f;          // m: the mirror ray starts this far off the glass along the normal ...
inline constexpr float kLiftPerMetre = 0.001f;  // ... plus this a metre of the glass's view distance
inline constexpr float kMinNv = 0.03f;          // a glass seen edge on (N.V under this) is not traced
inline constexpr float kPrimaryReach = 40.0f;   // m: the primary ray never looks further than the car's own size and a camera's distance from it
inline constexpr uint32_t kSeeThroughSteps = 3; // a primary ray goes on through at most this many panes of see-through glass
inline constexpr uint32_t kPrimaryTaps = 4;     // the coverage's rays a pixel
// The instance masks (car_rt_scene_rules.h; the shader's CAR_RT_MASK_*): the primary ray finds the player's parts, the mirror ray the player's parts and the cars beside.
inline constexpr uint32_t kPrimaryMask = 0x0F;      // body, glass, tyres, the driver
inline constexpr uint32_t kReflectionMask = 0x1F;   // ... and another car
// The table's flag bits above the mask and kFlagAfterWorld (car_rt_scene_rules.h).
inline constexpr uint32_t kFlagMirror = 0x200;
inline constexpr uint32_t kFlagSeeThrough = 0x400;

// the image the trace writes and the apply reads: both mirrors' rectangles side by side, each at most kImageEdge square
inline constexpr uint32_t kImageEdge = 512;
inline constexpr uint32_t kImageWidth = 2 * kImageEdge;
inline constexpr uint32_t kImageHeight = kImageEdge;
inline constexpr uint32_t kGroup = 8;  // the shader's thread group is 8 x 8
inline constexpr uint32_t kMaxMirrors = 2;
inline constexpr int32_t kRectMargin = 2;  // pixels added round a mirror's rectangle
inline constexpr uint32_t kReleaseAfterFrames = 120;  // frames without a pass before the image is given back
inline constexpr uint32_t kLogEveryPasses = 300;

inline uint64_t HeldBytes() { return uint64_t(kImageWidth) * kImageHeight * 8u; }  // R16G16B16A16_FLOAT

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

enum class Setting : uint8_t { kAuto, kOff, kOn };

inline Setting SettingOf(std::string_view text) {
  if (text == "off") {
    return Setting::kOff;
  }
  if (text == "on") {
    return Setting::kOn;
  }
  return Setting::kAuto;  // `auto`, and any word that is no value
}

inline const char* SettingName(Setting s) {
  switch (s) {
    case Setting::kOff: return "off";
    case Setting::kOn: return "on";
    case Setting::kAuto: return "auto";
  }
  return "auto";
}

// Whether the pass is wanted: `supported` is the device's answer (the car RT scene is ready and the pass's own layout and shaders were made), `cls` the ray tracing tier's class (rt_tier.h
// CurrentRtClass: a weak card of the High class is Mid, none is Low, as every ray traced effect's budget reads it). auto is on only on the strong
// class (High): the pass costs little, but it keeps the car's structures built in every frame, which a Mid class card only does where the owner asks for it. The Low class (no ray tracing,
// or shared memory) never runs it, whatever the word says: the pause row is grey there (car_mirror_setting.h GateFor), and a grey row's setting does nothing.
inline bool Wanted(Setting setting, bool supported, gpu_class::GpuClass cls) {
  if (!supported || setting == Setting::kOff || cls == gpu_class::GpuClass::kLow) {
    return false;
  }
  return setting == Setting::kOn || cls == gpu_class::GpuClass::kHigh;
}

struct Limits {
  uint32_t max_pixels = 0;  // the most one mirror's rectangle may cover
  uint32_t min_pixels = 0;  // under it the mirror is too small to be worth the pass
  float reach = 0.0f;       // m: how far the mirror ray goes to find another car, and the cars' reach the fade is over
  bool operator==(const Limits&) const = default;
};

inline constexpr uint32_t kMinPixels = 40;  // a rectangle with the margin round it of 6 x 6 pixels or less holds a box under 3 pixels across: nothing to show
inline constexpr uint32_t kMaxPixelsOverride = 262144;

// The class's cell with the debug override (thousands of pixels, 0: the class's). The Low class never runs the pass (Wanted); a test or a tool that asks for its cell gets the Mid class's.
inline Limits LimitsFor(gpu_class::GpuClass cls, int override_thousands) {
  const gpu_class::GpuClass effective = cls == gpu_class::GpuClass::kLow ? gpu_class::GpuClass::kMid : cls;
  Limits limits;
  limits.max_pixels = effective == gpu_class::GpuClass::kHigh ? 160000u : 64000u;
  limits.min_pixels = kMinPixels;
  limits.reach = car_rt_hit::LevelFor(effective).others_reach;
  if (override_thousands > 0) {
    limits.max_pixels = uint32_t(std::min<int64_t>(int64_t(override_thousands) * 1000, kMaxPixelsOverride));
  }
  return limits;
}

// ---------------------------------------------------------------------------
// the lens
// ---------------------------------------------------------------------------

// tan of the half fields of view from the frame's two matrices (car_frame.h Frame::view: world -> the occlusion's view space, view_projection: world -> clip; row vectors): the projection that
// takes the view space to the clip space is inverse(view) x view_projection, its diagonal is 1 / tan. False for a matrix that is not a perspective's (the pass does not run).
inline bool LensOf(const temporal::Mat4& view, const temporal::Mat4& view_projection, double* tan_x, double* tan_y) {
  temporal::Mat4 inverse;
  if (!temporal::Inverse(view, &inverse) || !temporal::Finite(view_projection)) {
    return false;
  }
  const temporal::Mat4 p = temporal::Mul(inverse, view_projection);
  const double px = std::fabs(p.m[0][0]);
  const double py = std::fabs(p.m[1][1]);
  if (!std::isfinite(px) || !std::isfinite(py) || !(px > 1e-6) || !(py > 1e-6)) {
    return false;
  }
  const double tx = 1.0 / px;
  const double ty = 1.0 / py;
  if (!(tx >= 0.05 && tx <= 20.0 && ty >= 0.05 && ty <= 20.0)) {
    return false;
  }
  *tan_x = tx;
  *tan_y = ty;
  return true;
}

// ---------------------------------------------------------------------------
// where a mirror is on the picture
// ---------------------------------------------------------------------------

// Why a frame's pass did not run (for the log's window).
enum class Why : uint8_t {
  kUsed,
  kSettingOff,
  kNoStructures,  // no top level this frame (no car on the screen, or the scene has none yet)
  kNoMirror,      // the car's draws hold no pair of mirror glasses
  kNoLens,
  kOffScreen,
  kTooSmall,
  kOverCap,
  kNotReady,      // a pipeline is not made yet
  kNoImage,       // the image, the set or the colour target could not be had
  kPauseView,     // not the play view (the pause menu's world, the garage)
  kCount
};

inline const char* WhyName(Why w) {
  switch (w) {
    case Why::kUsed: return "ran";
    case Why::kSettingOff: return "setting off";
    case Why::kNoStructures: return "no structures";
    case Why::kNoMirror: return "no mirror pair";
    case Why::kNoLens: return "no lens";
    case Why::kOffScreen: return "off the picture";
    case Why::kTooSmall: return "too small";
    case Why::kOverCap: return "over the cap";
    case Why::kNotReady: return "pipeline not ready";
    case Why::kNoImage: return "no image or target";
    case Why::kPauseView: return "not the play view";
    case Why::kCount: break;
  }
  return "?";
}

struct RectResult {
  car_rt_opp_refl::TexelRect rect;
  Why why = Why::kUsed;
};

// The rectangle of pixels of the `render_w` x `render_h` picture a mirror's box (the frame's view space) covers, kRectMargin larger, and whether the pass may use it. A box that crosses the
// camera's plane has the whole picture for a rectangle (BoxRect) and is over the cap.
inline RectResult RectFor(const car_rt_instances::ViewBox& box, double tan_x, double tan_y, double render_w, double render_h, const Limits& limits) {
  RectResult out;
  if (!(render_w >= 1.0) || !(render_h >= 1.0)) {
    out.why = Why::kOffScreen;
    return out;
  }
  out.rect = car_rt_opp_refl::BoxRect(box, tan_x, tan_y, render_w, render_h, 1.0, int32_t(render_w), int32_t(render_h), kRectMargin);
  if (out.rect.Empty()) {
    out.why = Why::kOffScreen;
    return out;
  }
  const uint64_t area = out.rect.Area();
  if (out.rect.Width() > int32_t(kImageEdge) || out.rect.Height() > int32_t(kImageEdge) || area > limits.max_pixels) {
    out.why = Why::kOverCap;
    return out;
  }
  if (area < limits.min_pixels) {
    out.why = Why::kTooSmall;
    return out;
  }
  return out;
}

// The image's place of mirror `index`: its rectangle's first texel.
inline uint32_t ImageX(uint32_t index) { return index * kImageEdge; }

// ---------------------------------------------------------------------------
// the pixel's arithmetic (the shader's, in C++: the test holds the shader's numbers to these)
// ---------------------------------------------------------------------------

// The mirror direction of a unit view direction off a unit normal.
inline void Reflect(const float d[3], const float n[3], float out[3]) {
  const float k = 2.0f * (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]);
  for (int c = 0; c < 3; ++c) {
    out[c] = d[c] - k * n[c];
  }
}

// The weight of a hit on ANOTHER car at `distance` along the mirror ray: 1 up to kFadeFrom of the reach, a smoothstep down to 0 at the reach (car_rt_hit_rules.h HitFade). A hit on the player's own
// parts weighs 1 whatever its distance (the scene holds the player's car whole).
inline float CarFade(float distance, float reach) { return car_rt_hit::HitFade(distance, reach, kFadeFrom); }

// The pixel's colour: the environment where the ray found nothing or a car's weight is below 1, the car's shaded colour by that weight; the alpha the coverage times the strength.
inline void Compose(const float environment[3], const float shaded[3], bool hit_car, float weight, float coverage, float strength, float out_rgb[3], float* out_alpha) {
  const float w = hit_car ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
  for (int c = 0; c < 3; ++c) {
    out_rgb[c] = environment[c] + (shaded[c] - environment[c]) * w;
  }
  *out_alpha = std::clamp(coverage, 0.0f, 1.0f) * std::clamp(strength, 0.0f, 1.0f);
}

// The coverage of a pixel from its taps: the taps that found a mirror glass first over the number of taps.
inline float Coverage(uint32_t hits, uint32_t taps) { return taps == 0 ? 0.0f : float(std::min(hits, taps)) / float(taps); }

// Whether the frame's rows say anything about the world: the environment map's slot or the shading's rows (the ambient). With neither, the shader's world is black and a mirror made from it would
// be a hole: the pixel then shows only a hit on the player's own car (weight 1) and otherwise leaves the game's glass (alpha 0).
inline constexpr bool WorldKnown(uint32_t frame_flags) { return (frame_flags & (rt_hit::kFrameEnv | rt_hit::kFrameShade)) != 0u; }

// The blend the apply draws with: dst = src * alpha + dst * (1 - alpha) on the colour channels. Where the game's pixel is `dst`.
inline float Blend(float src, float alpha, float dst) { return src * alpha + dst * (1.0f - alpha); }

}  // namespace mcla::gpu::car_mirror
