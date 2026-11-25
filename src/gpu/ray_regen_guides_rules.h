#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD FSR Ray Regeneration's guide conversion and the screen-space reflections' signal: the pure rules and the C++ twin (perf/RAY-REGEN.md, packages RG-2 and RG-3)
// ===========================================================================
// What needs no Plume, no AMD header and no state, in one header so that a program of its own checks it (ray_regen_guides_test.cpp, target larecomp_ray_regen_guides_test):
//
//   the images    the six images a Ray Regeneration frame of the reflections holds (the signal, the four guides, the denoised signal): formats, bytes a texel, the memory at a size.
//   the frame     DecideRegen: whether a frame of the reflections is Ray Regeneration's (the first reason that does not apply is the answer, as ssr_rules.h DecideRaw), and the outcome that matters for
//                 "never black": a frame that is not, or whose dispatch is refused, runs the effect's own accumulation and resolve.
//   the camera    BuildCamera: the matrices of the dispatch (view, unjittered projection), its position delta and its depth bounds, from what the reflections have at the trigger (the camera's own
//                 axes and motion, the lens of the screen frame); the mirrored convention of perf/RAY-REGEN.md 4.3 as one constant (kLinearDepthPositiveAhead in ray_regen_rules.h).
//   the push      the 16 words after the reflections' 32 (hlsl/ray_regen_ssr_cs.hlsl RG_X_*): the camera's motion rows, the jitter, the depth sign, the flags.
//   the twin      the functions of hlsl/ray_regen_common.hlsli in C++, in the same order of arithmetic, so that the test can run both on the same texels: signal, depth, motion, normals, specular albedo.
//
// Nothing here includes Plume or AMD's headers. Threading: pure.
//
// CHECK: nothing here was run against AMD's runtime (no RDNA 4 card on this PC). The conventions are the ones of ray_regen_rules.h (CONVENTIONS); the dispatch's view matrix has NO translation (the
// pass has the camera's rotation and its motion at the trigger, not its place in the world; the position delta travels separately, which is what the API's own pair of inputs is for): RAY-REGEN.md
// section 8 C16.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ray_regen_rules.h"
#include "ssr_rules.h"

namespace mcla::gpu::rayregen::guides {

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------------------------------------------------------------------------------

// In the order of the pass's UAVs (u0 .. u4 of hlsl/ray_regen_ssr_cs.hlsl), then the denoised signal the runtime writes.
enum class Image : uint8_t {
  kSignal,    // RGBA16F: the trace's radiance, the hit distance in alpha
  kDepth,     // R32F: the signed view-space z
  kMotion,    // RGBA16F: PreviousUV - CurrentUV, the signed linear depth delta
  kNormals,   // RGBA16F: octahedral world normal, linear roughness, material type
  kAlbedo,    // RGBA16F: linear specular or diffuse albedo, depending on the site
  kDenoised,  // RGBA16F: the runtime's output (rgb denoised radiance, alpha the hit distance preserved)
  kCount,
};
inline constexpr size_t kImageCount = size_t(Image::kCount);
inline constexpr size_t kPassImageCount = size_t(Image::kDenoised);  // the five the compute pass writes
struct ImageSpec {
  const char* name;
  rr::Format format;
  uint32_t bytes_per_texel;
};
constexpr ImageSpec SpecOf(Image image) {
  switch (image) {
    case Image::kSignal: return {"ray regen signal", rr::Format::kRgba16Float, 8};
    case Image::kDepth: return {"ray regen linear depth", rr::Format::kR32Float, 4};
    case Image::kMotion: return {"ray regen motion", rr::Format::kRgba16Float, 8};
    case Image::kNormals: return {"ray regen normals", rr::Format::kRgba16Float, 8};
    case Image::kAlbedo: return {"ray regen albedo", rr::Format::kRgba16Float, 8};
    case Image::kDenoised: return {"ray regen denoised", rr::Format::kRgba16Float, 8};
    case Image::kCount: break;
  }
  return {"?", rr::Format::kUnknown, 0};
}
constexpr uint32_t BytesPerTexel() {
  uint32_t bytes = 0;
  for (size_t i = 0; i < kImageCount; ++i) bytes += SpecOf(Image(i)).bytes_per_texel;
  return bytes;
}
// The video memory the six images hold at a traced size (the context's own is the runtime's: ray_regen_rules.h EstimateBytes).
constexpr uint64_t BytesOf(uint32_t width, uint32_t height) { return uint64_t(width) * height * BytesPerTexel(); }

constexpr ImageSpec AoSpecOf(Image image) {
  if (image == Image::kSignal) return {"ray regen AO signal", rr::Format::kR16Float, 2};
  if (image == Image::kDenoised) return {"ray regen denoised AO", rr::Format::kR16Float, 2};
  return SpecOf(image);
}

// The image of a guide as the dispatch plan (ray_regen_rules.h PlanDispatch) sees it.
constexpr rr::Image PlanImage(Image image, uint32_t width, uint32_t height) { return rr::Image{true, width, height, SpecOf(image).format}; }

// Images the compute pass leaves alone are given back after this many frames that were not Ray Regeneration's (a quick switch back finds them).
inline constexpr uint32_t kReleaseAfterFrames = 120;

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------------------------------------------------------------------------------

// The reflections' frame is Ray Regeneration's when the caller asks (TemporalSystem::RegenModes: the plan, the row, the context, the play view), nothing in the frame keeps the passes out (a debug view
// of the effect, the stub class, a multisampled colour: the same rules as the raw mode of DLSS Ray Reconstruction, ssr_rules.h DecideRaw) and the pass's pipeline, the resolve's and the guide images
// are there. A frame that is not Ray Regeneration's is the effect's own accumulation and resolve, as ever.
enum class RegenWhy : uint8_t {
  kRegen,         // Ray Regeneration takes the frame
  kNotAsked,      // the plan is not Ray Regeneration, or the effect's switch is off, or its context is not ready
  kDebugView,     // an effect debug view is on
  kStub,          // the G-buffer's material plane is not in the frame
  kMultisampled,  // the main colour has host samples above 1
  kPipelines,     // the guide pass's or the resolve's pipeline is being made
  kImages,        // an image could not be made
  kCount,
};
constexpr const char* RegenWhyName(RegenWhy w) {
  switch (w) {
    case RegenWhy::kRegen: return "AMD FSR Ray Regeneration takes the frame and cleans the effect's noise";
    case RegenWhy::kNotAsked: return "the frame is not Ray Regeneration's (the plan, the effect's switch or the context)";
    case RegenWhy::kDebugView: return "a debug view of the effect is on";
    case RegenWhy::kStub: return "the stub class stands in for the material plane";
    case RegenWhy::kMultisampled: return "the main colour is multisampled (turn the host's MSAA off, or let an upscaler do the anti-aliasing)";
    case RegenWhy::kPipelines: return "the guide pass's pipeline is not made yet";
    case RegenWhy::kImages: return "the guide images could not be made";
    case RegenWhy::kCount: break;
  }
  return "?";
}
struct RegenFacts {
  bool asked = false;            // TemporalSystem::RegenModes().Ssr(): the plan is Ray Regeneration's, the context is ready, the frame is the play view
  bool debug_view = false;
  bool stub = false;
  uint32_t host_samples = 1;
  bool pipelines_ready = false;  // the compute pass's pipeline and the resolve that reads the denoised image
  bool images_ok = true;
};
// Whether the frame would take the passes but for the pipelines: the pipelines are listed with the host precache only for such a frame (a frame that is not asked never makes one).
constexpr bool RegenPossible(const RegenFacts& f) { return f.asked && !f.debug_view && !f.stub && f.host_samples == 1; }
constexpr RegenWhy DecideRegen(const RegenFacts& f) {
  if (!f.asked) return RegenWhy::kNotAsked;
  if (f.debug_view) return RegenWhy::kDebugView;
  if (f.stub) return RegenWhy::kStub;
  if (f.host_samples != 1) return RegenWhy::kMultisampled;
  if (!f.pipelines_ready) return RegenWhy::kPipelines;
  if (!f.images_ok) return RegenWhy::kImages;
  return RegenWhy::kRegen;
}

// What a frame that began as Ray Regeneration's ends as: the dispatch recorded (the resolve reads the denoised image), or it was refused (the plan of the dispatch, the runtime, a missing image) and
// the resolve reads the effect's OWN accumulated image that frame, which the effect makes then (never the denoiser's output, never nothing).
enum class Outcome : uint8_t { kDenoised, kOwnFilter };
constexpr Outcome OutcomeOf(bool dispatch_recorded) { return dispatch_recorded ? Outcome::kDenoised : Outcome::kOwnFilter; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the camera at the trigger
// ---------------------------------------------------------------------------------------------------------------------------------------------------

// What the reflections have of the frame's camera at the trigger: the occlusion's own camera of THIS frame (its axes in the world, and the 3x4 that takes this frame's view space into the last frame's:
// the view space of the occlusion looks along +z, x right, y up, in the game's units: ssr_common.hlsli SsrToPrevious), and the lens of the screen frame (the projection's depth rows P22 P23 P32 P33
// and the tangents of half the field of view). The game's real camera exists only at the anchor, after the trigger.
struct CameraInputs {
  float right[3] = {1, 0, 0};
  float up[3] = {0, 1, 0};
  float forward[3] = {0, 0, -1};     // the game's view looks along -z of its own view space; the world axes are what these are
  float position[3] = {};            // the trigger supplies this where it has the world-space pose
  float motion[12] = {};             // three rows of the 3x4: previous view space = rows . (x, y, z, 1)
  bool have_motion = false;
  float lin[4] = {0, -1, 0, 0};      // P22 P23 P32 P33
  float tan_half_fov[2] = {1, 1};
  float near_plane = 5.0f;
  float far_plane = 10000.0f;
};
struct RegenCamera {
  float view[16] = {};               // world to view, row-major, row vectors
  float projection[16] = {};         // view to clip, unjittered
  float position_delta[3] = {};      // previous minus current, world space (0 when there is no motion)
  Bounds bounds;
  bool have_previous = false;
};

// The world-space vector from the camera to where it was a frame ago: the previous camera's origin in this frame's view space is -R^T t of the 3x4's rotation R and translation t (the rows of a rigid
// motion are orthonormal), turned into the world by the camera's own axes.
inline void PositionDelta(const CameraInputs& in, float out[3]) {
  float o[3];
  for (int axis = 0; axis < 3; ++axis) {
    // R^T t, axis `axis`: the dot of column `axis` of R (the rows' entries at index axis) with t (the rows' 4th entries)
    o[axis] = -(in.motion[0 * 4 + axis] * in.motion[0 * 4 + 3] + in.motion[1 * 4 + axis] * in.motion[1 * 4 + 3] + in.motion[2 * 4 + axis] * in.motion[2 * 4 + 3]);
  }
  for (int i = 0; i < 3; ++i) {
    out[i] = in.right[i] * o[0] + in.up[i] * o[1] + in.forward[i] * o[2];
  }
}

// The matrices of the dispatch. The view matrix takes a world vector (row vector) to the game's view space: columns right, up, BACKWARD (a right handed view looks along -z). The projection is the
// lens as a matrix: x and y scaled by the tangents, the depth rows as the screen frame has them. With the mirrored convention (kLinearDepthPositiveAhead) the third axis of both is flipped:
// V' = V diag(1,1,-1,1), P' = diag(1,1,-1,1) P, which makes a point ahead of the camera sit at positive z with positive clip w (perf/RAY-REGEN.md 4.3).
inline RegenCamera BuildCamera(const CameraInputs& in, bool mirrored = kLinearDepthPositiveAhead) {
  RegenCamera cam;
  const float back[3] = {-in.forward[0], -in.forward[1], -in.forward[2]};
  const float tx = in.tan_half_fov[0] > 0.0f ? in.tan_half_fov[0] : 1.0f;
  const float ty = in.tan_half_fov[1] > 0.0f ? in.tan_half_fov[1] : 1.0f;
  // view: row i is the world axis i's components in the view's (right, up, back)
  for (int row = 0; row < 3; ++row) {
    cam.view[row * 4 + 0] = in.right[row];
    cam.view[row * 4 + 1] = in.up[row];
    cam.view[row * 4 + 2] = back[row];
    cam.view[row * 4 + 3] = 0.0f;
    cam.view[12] -= in.position[row] * in.right[row];
    cam.view[13] -= in.position[row] * in.up[row];
    cam.view[14] -= in.position[row] * back[row];
  }
  cam.view[15] = 1.0f;
  cam.projection[0] = 1.0f / tx;
  cam.projection[5] = 1.0f / ty;
  cam.projection[10] = in.lin[0];
  cam.projection[11] = in.lin[1];
  cam.projection[14] = in.lin[2];
  cam.projection[15] = in.lin[3];
  if (mirrored) {
    for (int row = 0; row < 4; ++row) cam.view[row * 4 + 2] = -cam.view[row * 4 + 2];   // V diag(1,1,-1,1): the third column
    for (int col = 0; col < 4; ++col) cam.projection[2 * 4 + col] = -cam.projection[2 * 4 + col];   // diag(1,1,-1,1) P: the third row
  }
  cam.bounds = LinearDepthBounds(in.near_plane, in.far_plane);
  cam.have_previous = in.have_motion;
  if (in.have_motion) {
    PositionDelta(in, cam.position_delta);
  }
  return cam;
}

// The jitter of the frame (render pixels, +y down) at the dispatch's size: the dispatch is at the trace's size, a share of the render size.
constexpr float JitterAtSize(float jitter_render, uint32_t render, uint32_t traced) { return render != 0 ? jitter_render * float(traced) / float(render) : 0.0f; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the push block (hlsl/ray_regen_ssr_cs.hlsl)
// ---------------------------------------------------------------------------------------------------------------------------------------------------

namespace push {
inline constexpr uint32_t kExtraWords = 16;
inline constexpr uint32_t kMotion = 0;     // 12 floats
inline constexpr uint32_t kJitterX = 12;
inline constexpr uint32_t kJitterY = 13;
inline constexpr uint32_t kDepthSign = 14;
inline constexpr uint32_t kFlags = 15;
inline constexpr uint32_t kFlagHaveMotion = 1;
}  // namespace push

struct Push {
  ssr::Push base;                                   // the reflections' words (the apply's, with the trace's slot in the low half of kSrc)
  std::array<uint32_t, push::kExtraWords> x{};
  void SetFloat(uint32_t word, float value) { std::memcpy(&x[word], &value, sizeof(value)); }
};

struct BouncePush {
  uint32_t signal, depth, hit, normal;
  uint32_t albedo, material, width, height;
  uint32_t render_width, render_height, step;
  float depth_sign;
  float tan_x, tan_y, prev_tan_x, prev_tan_y;
  float motion[12];
  float jitter[2];
  uint32_t have_motion, padding;
};
static_assert(sizeof(BouncePush) == 128);

struct BounceHistoryKey {
  uint64_t frame = 0;
  uint32_t width = 0, height = 0;
  uint32_t render_width = 0, render_height = 0;
  uint32_t step = 0;
};

constexpr bool KeepBounceHistory(const BounceHistoryKey& prev, const BounceHistoryKey& now, bool cut) {
  return !cut && prev.step != 0 && prev.frame + 1 == now.frame && prev.width == now.width && prev.height == now.height &&
         prev.render_width == now.render_width && prev.render_height == now.render_height && prev.step == now.step;
}
static_assert(sizeof(Push) == 192, "48 words: the reflections' 32 and the 16 of the pass (hlsl/ray_regen_ssr_cs.hlsl SsrPush with SSR_REGEN_EXTRA); a Direct3D 12 pass, 48 of the 64 root dwords");

constexpr float DepthSign(bool mirrored = kLinearDepthPositiveAhead) { return mirrored ? 1.0f : -1.0f; }

constexpr uint32_t Groups(uint32_t extent) { return (extent + 7u) / 8u; }  // the pass's thread groups are 8 x 8

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// the twin of hlsl/ray_regen_common.hlsli
// ---------------------------------------------------------------------------------------------------------------------------------------------------

inline constexpr float kNoDepth = 60000.0f;
inline constexpr float kMinHit = 0.02f;

struct Float4 {
  float x = 0, y = 0, z = 0, w = 0;
};

// RgDepthTexel.
inline float DepthTexel(bool traced, float t, float sign) {
  const bool valid = traced && t > 0.0f && t < kNoDepth;
  return (valid ? t : kNoDepth) * sign;
}
// RgMotionTexel.
inline Float4 MotionTexel(float cur_u, float cur_v, float prev_u, float prev_v, float prev_z, float t, bool valid, float sign) {
  if (!valid) return {};
  return {prev_u - cur_u, prev_v - cur_v, sign * prev_z - sign * t, 0.0f};
}
// RgSpecularAlbedo (per channel): saturate(F0 * scale + bias).
inline float SpecularAlbedoChannel(float f0, float scale, float bias) { return std::min(std::max(f0 * scale + bias, 0.0f), 1.0f); }
// RgSignalTexel. The max keeps the other value where one is NaN, as HLSL's does (a NaN radiance is 0).
inline float NonNegative(float v) { return v > 0.0f ? v : 0.0f; }
inline Float4 SignalTexel(float r, float g, float b, float hit_distance, bool traced) {
  if (!traced) return {};
  return {NonNegative(r), NonNegative(g), NonNegative(b), hit_distance > kMinHit ? hit_distance : kMinHit};
}

}  // namespace mcla::gpu::rayregen::guides
