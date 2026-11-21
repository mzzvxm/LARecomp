#pragma once
// ===========================================================================
// MCLA GPU renderer -- DLSS Ray Reconstruction's guide images: the pure rules and the C++ twin of the guide pass (perf/DLSS-RR.md, package RR-2)
// ===========================================================================
// What needs no Plume, no Streamline and no state, in one header so that a program of its own checks it (rr_guides_test.cpp, target larecomp_rr_guides_test):
//
//   the images     the five guide images the pass makes at the render size (diffuse albedo, specular albedo, normal + roughness, hit distance, responsivity), their formats, which
//                  dlss_rr_rules.h accepts (the test runs rr::PlanTags over them), and their size in bytes;
//   the views      mcla_gpu_dlss_rr_view 0..6, one per guide;
//   who runs it    PassWanted: the guide pass is Want-driven: it runs while Ray Reconstruction would be the choice if the guides were there (or is), and for a debug view while an upscaler
//                  runs; while the plan is neither RR nor about to be (the row on OURS, no effect that uses RR on, the switch off ...) and no view is on it does nothing and makes nothing;
//   the push       the two compute passes' push blocks, which the HLSL declares in the same order (the test reads the HLSL for it);
//   the decode     the TWIN of hlsl/rr_guides_common.hlsli: the same functions in C++, in the same order of arithmetic, so that the test can run both on the same pixels. The decode is the part
//                  that is "what each guide means": the G-buffer's planes to the guides Ray Reconstruction reads (see the HLSL's header for the table).
//
// Nothing here includes Plume or Streamline. Threading: pure.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "dlss_rr_rules.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"

namespace mcla::gpu::rr_guides {

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------------------------------------------------------------------------------

enum class Guide : uint8_t {
  kDiffuseAlbedo,
  kSpecularAlbedo,
  kNormalRoughness,  // the world normal, the linear roughness in alpha (DLSSDNormalRoughnessMode::ePacked)
  kHitDistance,
  kResponsivity,
  kCount,
};
inline constexpr size_t kGuideCount = size_t(Guide::kCount);

struct GuideSpec {
  const char* name;
  rr::Buffer buffer;      // the buffer type the rules tag it as
  rr::Format format;      // the rules' name of its format
  uint32_t bytes_per_pixel;
};
// CHECK: the two albedos could be R10G10B10A2_UNORM (4 B a pixel instead of 8, the rules accept it): RGBA16F is kept because a typed UAV store of RGB10A2 is not required of every device on both
// backends and the first run should not depend on it; 8 B x 2 is 16 MB at 1080p and 66 MB at 4K more than the 10-bit form. The responsivity and the hit distance are R16F (the rules take both).
constexpr GuideSpec SpecOf(Guide guide) {
  switch (guide) {
    case Guide::kDiffuseAlbedo: return {"diffuse albedo", rr::Buffer::kDiffuseAlbedo, rr::Format::kRgba16Float, 8};
    case Guide::kSpecularAlbedo: return {"specular albedo", rr::Buffer::kSpecularAlbedo, rr::Format::kRgba16Float, 8};
    case Guide::kNormalRoughness: return {"normals + roughness", rr::Buffer::kNormalRoughness, rr::Format::kRgba16Float, 8};
    case Guide::kHitDistance: return {"specular hit distance", rr::Buffer::kSpecularHitDistance, rr::Format::kR16Float, 2};
    case Guide::kResponsivity: return {"responsivity mask", rr::Buffer::kResponsivity, rr::Format::kR16Float, 2};
    case Guide::kCount: break;
  }
  return {"?", rr::Buffer::kCount, rr::Format::kUnknown, 0};
}

constexpr uint32_t BytesPerPixel() {
  uint32_t bytes = 0;
  for (size_t i = 0; i < kGuideCount; ++i) bytes += SpecOf(Guide(i)).bytes_per_pixel;
  return bytes;
}
// The video memory the five images hold at a render size.
constexpr uint64_t BytesOf(uint32_t width, uint32_t height) { return uint64_t(width) * height * BytesPerPixel(); }

// The image of a guide as the rules' tag plan sees it.
constexpr rr::Image PlanImage(Guide guide, uint32_t width, uint32_t height) { return rr::Image{true, width, height, SpecOf(guide).format}; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the views (mcla_gpu_dlss_rr_view)
// ---------------------------------------------------------------------------------------------------------------------------------------------------

enum class View : uint8_t {
  kOff = 0,
  kDiffuseAlbedo = 1,
  kNormal = 2,       // 0.5 + 0.5 n
  kRoughness = 3,    // the linear roughness as a grey
  kSpecularAlbedo = 4,
  kHitDistance = 5,  // log scale: black on a surface, blue in the sky
  kResponsivity = 6,
  kCount = 7,
};
inline constexpr int kViewCount = int(View::kCount);
constexpr View ViewOf(int value) { return (value > 0 && value < kViewCount) ? View(value) : View::kOff; }
constexpr const char* ViewName(View view) {
  switch (view) {
    case View::kOff: return "off";
    case View::kDiffuseAlbedo: return "diffuse albedo";
    case View::kNormal: return "normal";
    case View::kRoughness: return "roughness";
    case View::kSpecularAlbedo: return "specular albedo";
    case View::kHitDistance: return "hit distance";
    case View::kResponsivity: return "responsivity";
    case View::kCount: break;
  }
  return "?";
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// who runs the pass
// ---------------------------------------------------------------------------------------------------------------------------------------------------

// The guide pass is wanted while the plan says Ray Reconstruction would run if its guides were there (the plan's reason is "the guides are not ready") or runs (they are), and, for a debug view,
// while an upscaler runs (the view is drawn over what it made). The planes the pass reads exist only while somebody wants them: the pass asks the G-buffer for all three while it is wanted.
constexpr bool PassWanted(const rr::Plan& plan, View view, bool upscaler_running) {
  return plan.use_rr || plan.why == rr::Why::kGuidesNotReady || (view != View::kOff && upscaler_running);
}
inline constexpr uint32_t kWantedPlanes = gbuffer::kAllPlanes;  // RT1 the normal, RT2 the albedo, RT3 the smoothness and the class

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the push blocks (hlsl/rr_guides_cs.hlsl RrGuidesPush, hlsl/rr_guides_view_cs.hlsl RrGuidesViewPush)
// ---------------------------------------------------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
inline constexpr uint32_t kGroup = 8;  // both shaders' thread groups are 8 x 8
constexpr uint32_t Groups(uint32_t extent) { return (extent + kGroup - 1) / kGroup; }

struct GuidesPush {
  uint32_t slots[4];   // x RT1's bindless slot, y RT2's, z RT3's, w the kept depth's (R32_FLOAT, 1 near / 0 far)
  uint32_t slots2[4];  // x the reactive mask's slot, y the transparency mask's (kNoSlot: none), z a hit distance writer's image, w a second writer's (kNoSlot: none; RR-4: the reflection effects')
  uint32_t info[4];    // x the width, y the height of the render size, z the host sample that is the guest's sample 0, w the bounce light's hit image's slot (kNoSlot: none; CITY-RT G6)
  float right[4];      // xyz the camera's right axis in world space, w tan of half the horizontal field of view
  float up[4];         // xyz the up axis, w tan of half the vertical field of view
  float forward[4];    // xyz the forward axis, w unused
  float lin[4];        // the projection's depth rows P22 P23 P32 P33 (ao.cpp FillPush)
};
static_assert(sizeof(GuidesPush) == 112, "seven rows of 16 bytes: under the 128 bytes Vulkan guarantees");

struct ViewPush {
  uint32_t slots[4];   // x the diffuse albedo's bindless slot, y the normal + roughness image's, z the specular albedo's, w the hit distance's
  uint32_t slots2[4];  // x the responsivity's, y the view (View), zw unused
  uint32_t sizes[4];   // x, y the render size, z, w the output size
};
static_assert(sizeof(ViewPush) == 48, "three rows");

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the decode: the twin of hlsl/rr_guides_common.hlsli
// ---------------------------------------------------------------------------------------------------------------------------------------------------

using gbuffer::Vec3;

inline constexpr float kSky = 60000.0f;
inline constexpr float kSkyDiffuse = rr::kSkyDiffuseAlbedo;
inline constexpr float kSkyHitDistance = rr::kSkyHitDistance;
inline constexpr float kDielectricF0 = 0.04f;
inline constexpr float kMetalF0 = 0.6f;
inline constexpr float kMinAlbedo = 0.02f;
inline constexpr float kUnknownAlbedo = 0.5f;
inline constexpr float kMinTangent = 1.0e-20f;

inline float Sat(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

// RrLinearDepth.
inline float LinearDepth(float stored, float lin_x, float lin_y, float lin_z, float lin_w) {
  const float ndc = 1.0f - Sat(stored);
  const float num = lin_z - ndc * lin_w;
  const float den = lin_x - ndc * lin_y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kSky) ? t : kSky;
}

// RrViewPos.
inline Vec3 ViewPos(float cx, float cy, float width, float height, float t, float tan_x, float tan_y) {
  const float u = cx / width;
  const float v = cy / height;
  return {(u * 2.0f - 1.0f) * tan_x * t, (1.0f - v * 2.0f) * tan_y * t, t};
}

// RrWorldOfView.
inline Vec3 WorldOfView(Vec3 v, Vec3 right, Vec3 up, Vec3 forward) { return Add(Add(Mul(right, v.x), Mul(up, v.y)), Mul(forward, v.z)); }

// RrViewDir.
inline Vec3 ViewDir(float cx, float cy, float width, float height, float tan_x, float tan_y, Vec3 right, Vec3 up, Vec3 forward) {
  return gbuffer::Normalize(WorldOfView(ViewPos(cx, cy, width, height, 1.0f, tan_x, tan_y), right, up, forward));
}

// RrTangent.
inline Vec3 Tangent(Vec3 pc, float zc, Vec3 pm, float zm, Vec3 pp, float zp) {
  const bool m_ok = zm < kSky * 0.5f;
  const bool p_ok = zp < kSky * 0.5f;
  if (m_ok && p_ok) {
    return std::fabs(zp - zc) < std::fabs(zm - zc) ? pp - pc : pc - pm;
  }
  if (p_ok) return pp - pc;
  if (m_ok) return pc - pm;
  return {0.0f, 0.0f, 0.0f};
}

// RrNormalOfTangents.
inline Vec3 NormalOfTangents(Vec3 tx, Vec3 ty) {
  const Vec3 n = gbuffer::Cross(tx, ty);
  const float l2 = gbuffer::Dot(n, n);
  if (!(l2 > kMinTangent)) return {0.0f, 0.0f, 0.0f};
  return Mul(n, 1.0f / std::sqrt(l2));
}

// RrFacing.
inline Vec3 Facing(Vec3 n, Vec3 view_dir) { return gbuffer::Dot(n, view_dir) > 0.0f ? Mul(n, -1.0f) : n; }

// RrLinearRoughness.
inline float LinearRoughness(float smoothness) { return rr::LinearRoughnessOfSmoothness(smoothness); }

// RrClassF0. `cls` is the class number (gbuffer::SurfaceClass).
inline Vec3 ClassF0(uint32_t cls, Vec3 albedo, bool have_albedo) {
  if (cls == uint32_t(gbuffer::SurfaceClass::kChrome) || cls == uint32_t(gbuffer::SurfaceClass::kMetal)) {
    return have_albedo ? albedo : Vec3{kMetalF0, kMetalF0, kMetalF0};
  }
  if (cls == uint32_t(gbuffer::SurfaceClass::kEmissive)) return {0.0f, 0.0f, 0.0f};
  return {kDielectricF0, kDielectricF0, kDielectricF0};
}

// RrEnvBias, RrEnvScale: the guide's matrices multiplied out (the test also multiplies them as the guide's text does).
inline float EnvBias(float nov, float alpha) {
  const float x1 = nov;
  const float x2 = nov * nov;
  const float x3 = nov * x2;
  const float a3 = alpha * alpha * alpha;
  const float num = (0.99044f - 1.28514f * x1) + alpha * (1.29678f - 0.755907f * x1);
  const float den = (1.0f + 2.92338f * x1 + 59.4188f * x3) + alpha * (20.3225f - 27.0302f * x1 + 222.592f * x3) + a3 * (121.563f + 626.13f * x1 + 316.627f * x3);
  return num / den;
}
inline float EnvScale(float nov, float alpha) {
  const float x1 = nov;
  const float x2 = nov * nov;
  const float x3 = nov * x2;
  const float a3 = alpha * alpha * alpha;
  const float num = (0.0365463f + 3.32707f * x1) + alpha * (9.0632f - 9.04756f * x1);
  const float den = (1.0f + 3.59685f * x2 - 1.36772f * x3) + alpha * (9.04401f - 16.3174f * x2 + 9.22949f * x3) + a3 * (5.56589f + 19.7886f * x2 - 20.2123f * x3);
  return num / den;
}

// RrSpecularAlbedo.
inline Vec3 SpecularAlbedo(Vec3 f0, float alpha, float nov_signed) {
  const float nov = std::fabs(nov_signed);
  const float bias = EnvBias(nov, alpha) * Sat(f0.y * 50.0f);
  const float scale = std::max(0.0f, EnvScale(nov, alpha));
  const float b = std::max(0.0f, bias);
  return {f0.x * scale + b, f0.y * scale + b, f0.z * scale + b};
}

// RrHasAlbedo: `rt2` is what a Load of the albedo plane returns (unorm, 0..1).
inline bool HasAlbedo(const float rt2[4]) { return rt2[0] > 0.0f || rt2[1] > 0.0f || rt2[2] > 0.0f || rt2[3] > 0.0f; }

// RrDiffuseAlbedo.
inline Vec3 DiffuseAlbedo(const float rt2[4]) {
  if (!HasAlbedo(rt2)) return {kUnknownAlbedo, kUnknownAlbedo, kUnknownAlbedo};
  return {std::max(gbuffer::AlbedoDecode(rt2[0]), kMinAlbedo), std::max(gbuffer::AlbedoDecode(rt2[1]), kMinAlbedo), std::max(gbuffer::AlbedoDecode(rt2[2]), kMinAlbedo)};
}

// RT3's class of a texel's green (unorm): GbRt3Class.
inline uint32_t Rt3Class(float green) { return uint32_t(green * 255.0f + 0.5f) & uint32_t(gbuffer::kClassMask); }

struct Pixel {
  Vec3 diffuse;
  Vec3 normal;
  float roughness = 0.0f;
  Vec3 specular;
  float hit = 0.0f;
};

// RrSkyPixel.
inline Pixel SkyPixel() {
  Pixel px;
  px.diffuse = {kSkyDiffuse, kSkyDiffuse, kSkyDiffuse};
  px.normal = {0.0f, 0.0f, 0.0f};
  px.roughness = rr::kSkyRoughness;
  px.specular = {rr::kSkySpecularAlbedo, rr::kSkySpecularAlbedo, rr::kSkySpecularAlbedo};
  px.hit = kSkyHitDistance;
  return px;
}

// RrSurfacePixel: `rt1` the plane's texel as a unorm pair, `rt2` its four unorm values, `rt3` the material texel (smoothness, class | flags << 5).
inline Pixel SurfacePixel(const float rt1[2], const float rt2[4], const float rt3[2], Vec3 depth_normal, Vec3 view_dir) {
  Pixel px;
  Vec3 n = Mul(view_dir, -1.0f);
  if (rt1[0] > 0.0f || rt1[1] > 0.0f) {
    n = gbuffer::OctDecode({rt1[0], rt1[1]});
  } else if (gbuffer::Dot(depth_normal, depth_normal) > 0.5f) {
    n = Facing(depth_normal, view_dir);
  }
  const bool have_albedo = HasAlbedo(rt2);
  const Vec3 albedo = {gbuffer::AlbedoDecode(rt2[0]), gbuffer::AlbedoDecode(rt2[1]), gbuffer::AlbedoDecode(rt2[2])};
  const float roughness = LinearRoughness(rt3[0]);
  const Vec3 f0 = ClassF0(Rt3Class(rt3[1]), albedo, have_albedo);
  px.diffuse = DiffuseAlbedo(rt2);
  px.normal = n;
  px.roughness = roughness;
  px.specular = SpecularAlbedo(f0, roughness * roughness, gbuffer::Dot(n, Mul(view_dir, -1.0f)));
  px.hit = 0.0f;
  return px;
}

// RrHitDistance.
inline float HitDistance(bool sky, float source) {
  if (sky) return kSkyHitDistance;
  return (source > 0.0f && source < kSkyHitDistance) ? source : 0.0f;
}

// RR-4: what the pass makes of the two writers' values for a pixel (hlsl/rr_guides_cs.hlsl: source = max(first, second), HLSL's max keeps the other where one is NaN): the larger, so the screen-space
// reflections' and the cars' ray traced ones, which never trace the same pixel, coexist; where neither wrote the value is 0 and HitDistance makes it the surface's.
inline float CombineHitSources(float first, float second) {
  if (std::isnan(first)) return second;
  if (std::isnan(second)) return first;
  return std::max(first, second);
}

// CITY-RT G6, RrWithBounce / RrBounceTexel: the bounce light's per-pixel pass's ray length is the LAST choice of the hit distance (a reflection's own positive distance keeps the pixel; a NaN is not one), read
// from its own image (the traced size over a divisor): the texel under a render pixel, one axis, is the pixel scaled by the two sizes and clamped into the image.
inline float WithBounce(float source, float bounce) { return source > 0.0f ? source : bounce; }
inline uint32_t BounceTexel(uint32_t pixel, uint32_t size, uint32_t hit_size) { return std::min(pixel * hit_size / std::max(size, 1u), std::max(hit_size, 1u) - 1u); }

// RrResponsivity.
inline float Responsivity(float reactive, float transparency) { return Sat(std::max(reactive, transparency)); }

// RrViewColour.
inline Vec3 ViewColour(View view, Vec3 diffuse, const float normal_roughness[4], Vec3 specular, float hit, float responsivity) {
  switch (view) {
    case View::kDiffuseAlbedo: return diffuse;
    case View::kNormal: {
      const Vec3 n = {normal_roughness[0], normal_roughness[1], normal_roughness[2]};
      return gbuffer::Dot(n, n) > 0.0f ? Add(Mul(n, 0.5f), Vec3{0.5f, 0.5f, 0.5f}) : Vec3{0.0f, 0.0f, 0.0f};
    }
    case View::kRoughness: return {normal_roughness[3], normal_roughness[3], normal_roughness[3]};
    case View::kSpecularAlbedo: return specular;
    case View::kHitDistance: {
      if (hit >= kSkyHitDistance * 0.99f) return {0.0f, 0.0f, 1.0f};
      const float g = Sat(std::log2(hit + 1.0f) * (1.0f / 12.0f));
      return {g, g, g};
    }
    case View::kResponsivity: return {responsivity, responsivity, responsivity};
    case View::kOff:
    case View::kCount: break;
  }
  return {0.0f, 0.0f, 0.0f};
}

}  // namespace mcla::gpu::rr_guides
