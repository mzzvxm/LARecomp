#pragma once
// ===========================================================================
// MCLA GPU renderer -- DLSS Ray Reconstruction: the pure rules (perf/DLSS-RR.md)
// ===========================================================================
// What needs no Streamline, no Plume, no log and no state, in one header so that a program of its own checks it (dlss_rr_rules_test.cpp, target
// larecomp_dlss_rr_rules_test). temporal_dlss.cpp turns these answers into Streamline calls; nothing here includes a Streamline or a Plume header, and the
// Streamline constants below are pinned by the test against the values of sl_core_types.h (SDK 2.14.16) and by static_asserts in temporal_dlss.cpp against the
// real ones.
//
//   Buffer / PlanTags      which images of a frame are tagged for RR, under which Streamline buffer type, at which extent; whether the roughness rides in the
//                          normals' alpha; whether the reflection's motion comes from its own image, from a hit distance (with the camera matrices) or from the
//                          primary motion standing in; and why a frame's set is refused (missing, wrong size, wrong format).
//   MakeOptions            the options of a viewport (DLSSDOptions' content) and the camera matrices RR's hit distance path needs.
//   SelectDenoiser         DENOISER AUTO / RR / OURS and the debug switch against what the PC and the frame have: RR or our own denoisers, and why.
//   Consumers              (RR-5) the effects that hand RR their raw noisy radiance (SSR, the car ray traced reflections, the city's ray traced reflections, ray traced bounce light): which have a raw mode
//                          built, which of them AUTO trusts RR with (the A/B said RR is better than ours there), and what "a consumer is on" means for AUTO and for the row's own RR value.
//   FrameGate              why a frame cannot be given to RR even when it is the plan (no G-buffer planes: pause world, garage, map, cutscene): that frame is SR's.
//   JitterPhases           RR wants at least 32 positions in the jitter sequence (SR's 8 x ratio^2 is not enough).
//   WantRrPlugin           whether Streamline is started with the RR plugin (plugins cannot be added after slInit).
//   ChooseDispatch         (RR-3) which context a frame's dispatch goes to: RR when the plan says RR, its context exists and this frame has its guides; else Super Resolution,
//                          whose context is kept beside RR (a missing guide is an SR frame, never "DLSS off for the session").
//   ContextKeeper          (RR-3) how long the RR context is kept after the plan stops wanting it (a quick switch back finds it).
//   ContextNeedBytes       (RR-3) the video memory the RR context asks for, the check before it is made (NVIDIA's table, or what the first context measured).
//   FailureBook            (RR-3) what a failed or refused RR evaluation means: a refusal is an SR frame, three errors in a row drop RR for the session (never DLSS).
//
// The rules the numbers come from: Streamline 2.14.16 docs/ProgrammingGuideDLSS_RR.md, the DLSS-RR Integration Guide (August 2026) chapter 3, and NVIDIA's RR
// integration checklist in the same package (plugins/streamline-agent-skills/skills/integrate-dlss-rr).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "vram_room.h"

namespace mcla::gpu::rr {

// --- Streamline's ids (sl_core_types.h; temporal_dlss.cpp static_asserts them against sl::kBufferType* and sl::kFeatureDLSS_RR) ----------------------------

inline constexpr uint32_t kSlFeatureDlssRr = 1001;
inline constexpr uint32_t kSlBufferDepth = 0;
inline constexpr uint32_t kSlBufferMotionVectors = 1;
inline constexpr uint32_t kSlBufferScalingInputColor = 3;
inline constexpr uint32_t kSlBufferScalingOutputColor = 4;
inline constexpr uint32_t kSlBufferNormals = 5;
inline constexpr uint32_t kSlBufferRoughness = 6;
inline constexpr uint32_t kSlBufferAlbedo = 7;
inline constexpr uint32_t kSlBufferSpecularAlbedo = 8;
inline constexpr uint32_t kSlBufferSpecularMotionVectors = 10;
inline constexpr uint32_t kSlBufferNormalRoughness = 14;
inline constexpr uint32_t kSlBufferColorBeforeTransparency = 40;
inline constexpr uint32_t kSlBufferSpecularHitDistance = 42;
inline constexpr uint32_t kSlBufferResponsivityMask = 68;

// What the guides hold where there is no surface (the integration guide, 3.4): the sky's defaults.
inline constexpr float kSkyDiffuseAlbedo = 0.5f;
inline constexpr float kSkySpecularAlbedo = 0.0f;
inline constexpr float kSkyRoughness = 0.0f;
inline constexpr float kSkyHitDistance = 65504.0f;  // FP16_MAX

// The linear roughness RR asks for from the G-buffer's smoothness byte (gbuffer_rules.h: alpha_GGX = (1 - smoothness)^2, so 1 - smoothness is the linear one).
inline float LinearRoughnessOfSmoothness(float smoothness) {
  const float s = smoothness < 0.0f ? 0.0f : (smoothness > 1.0f ? 1.0f : smoothness);
  return 1.0f - s;
}

// --- the images of a frame -------------------------------------------------------------------------------------------------------------------------------

enum class Buffer : uint8_t {
  kColorIn,
  kColorOut,
  kDepth,
  kMotion,
  kDiffuseAlbedo,
  kSpecularAlbedo,
  kNormalRoughness,  // the normals with the roughness in alpha (DLSSDNormalRoughnessMode::ePacked)
  kNormals,
  kRoughness,
  kSpecularMotion,
  kSpecularHitDistance,
  kResponsivity,
  kColorBeforeTransparency,
  kCount,
};

// The formats the rules tell apart (the renderer's own names map onto them in temporal_dlss.cpp; anything else is kUnknown and is refused).
enum class Format : uint8_t {
  kUnknown,
  kR8Unorm,
  kR8Snorm,
  kR16Float,
  kR32Float,
  kD32Float,
  kRg16Float,
  kRg32Float,
  kRgba8Unorm,
  kRgba8Snorm,
  kRgba8Srgb,
  kRgb10A2Unorm,
  kRgba16Float,
  kRgba32Float,
};

constexpr uint32_t Channels(Format f) {
  switch (f) {
    case Format::kR8Unorm:
    case Format::kR8Snorm:
    case Format::kR16Float:
    case Format::kR32Float:
    case Format::kD32Float: return 1;
    case Format::kRg16Float:
    case Format::kRg32Float: return 2;
    case Format::kRgba8Unorm:
    case Format::kRgba8Snorm:
    case Format::kRgba8Srgb:
    case Format::kRgb10A2Unorm:
    case Format::kRgba16Float:
    case Format::kRgba32Float: return 4;
    case Format::kUnknown: break;
  }
  return 0;
}
constexpr bool IsFloat(Format f) {
  return f == Format::kR16Float || f == Format::kR32Float || f == Format::kD32Float || f == Format::kRg16Float || f == Format::kRg32Float ||
         f == Format::kRgba16Float || f == Format::kRgba32Float;
}

// What an image is, for the plan: whether it is there, its size and its format.
struct Image {
  bool present = false;
  uint32_t width = 0;
  uint32_t height = 0;
  Format format = Format::kUnknown;
};

struct BufferInfo {
  const char* name;
  uint32_t sl_type;      // Streamline's buffer type
  bool at_output_size;   // the extent is the output's (the colour out), else the render size's
};
constexpr BufferInfo InfoOf(Buffer b) {
  switch (b) {
    case Buffer::kColorIn: return {"colour in", kSlBufferScalingInputColor, false};
    case Buffer::kColorOut: return {"colour out", kSlBufferScalingOutputColor, true};
    case Buffer::kDepth: return {"depth", kSlBufferDepth, false};
    case Buffer::kMotion: return {"motion vectors", kSlBufferMotionVectors, false};
    case Buffer::kDiffuseAlbedo: return {"diffuse albedo", kSlBufferAlbedo, false};
    case Buffer::kSpecularAlbedo: return {"specular albedo", kSlBufferSpecularAlbedo, false};
    case Buffer::kNormalRoughness: return {"normals + roughness", kSlBufferNormalRoughness, false};
    case Buffer::kNormals: return {"normals", kSlBufferNormals, false};
    case Buffer::kRoughness: return {"roughness", kSlBufferRoughness, false};
    case Buffer::kSpecularMotion: return {"specular motion vectors", kSlBufferSpecularMotionVectors, false};
    case Buffer::kSpecularHitDistance: return {"specular hit distance", kSlBufferSpecularHitDistance, false};
    case Buffer::kResponsivity: return {"responsivity mask", kSlBufferResponsivityMask, false};
    case Buffer::kColorBeforeTransparency: return {"colour before transparency", kSlBufferColorBeforeTransparency, false};
    case Buffer::kCount: break;
  }
  return {"?", 0, false};
}

// The formats the integration guide accepts for each buffer (3.4): the colour in HDR float; albedos 3 or more channels, linear (no sRGB); the normals a float or
// signed normalised 4 channel format; a roughness one channel; the vectors two floats; the hit distance one float; the depth one float channel.
constexpr bool FormatOk(Buffer b, Format f) {
  switch (b) {
    case Buffer::kColorIn:
    case Buffer::kColorBeforeTransparency: return f == Format::kRgba16Float || f == Format::kRgba32Float;
    case Buffer::kColorOut:
      return f == Format::kRgba16Float || f == Format::kRgba32Float || f == Format::kRgba8Unorm || f == Format::kRgb10A2Unorm;
    case Buffer::kDepth: return f == Format::kR32Float || f == Format::kD32Float;
    case Buffer::kMotion:
    case Buffer::kSpecularMotion: return f == Format::kRg16Float || f == Format::kRg32Float;
    case Buffer::kDiffuseAlbedo:
    case Buffer::kSpecularAlbedo:
      return f == Format::kRgba8Unorm || f == Format::kRgb10A2Unorm || f == Format::kRgba16Float || f == Format::kRgba32Float;
    case Buffer::kNormalRoughness:
    case Buffer::kNormals: return f == Format::kRgba16Float || f == Format::kRgba32Float || f == Format::kRgba8Snorm;
    case Buffer::kRoughness: return f == Format::kR8Unorm || f == Format::kR16Float || f == Format::kR32Float;
    case Buffer::kSpecularHitDistance: return f == Format::kR16Float || f == Format::kR32Float;
    case Buffer::kResponsivity: return f == Format::kR8Snorm || f == Format::kR16Float;
    case Buffer::kCount: break;
  }
  return false;
}

// --- the tag plan ----------------------------------------------------------------------------------------------------------------------------------------

// What a frame has to give RR. Sizes: every image but the output is at the render size, the output at the output size.
struct Inputs {
  Image color, depth, motion, output;
  Image diffuse_albedo, specular_albedo;
  Image normal_roughness;                 // packed: normals with the linear roughness in alpha
  Image normals, roughness;               // or separate
  Image specular_motion, specular_hit_distance;
  Image responsivity, color_before_transparency;  // optional
  uint32_t render_size[2] = {};
  uint32_t output_size[2] = {};
  bool have_view_matrices = false;        // the options carry worldToCameraView / cameraViewToWorld (the hit distance needs them)
  bool allow_primary_motion_as_specular = true;  // neither specular input exists: the primary motion image is tagged for it too
};

enum class Verdict : uint8_t {
  kOk,
  kMissing,             // `culprit`: a required image is not there
  kNeedsViewMatrices,   // the hit distance is the reflection's motion and the options have no camera matrices
  kBadSize,             // `culprit`: its size is not the render size (the output's not the output size, or smaller than the render size)
  kBadFormat,           // `culprit`: a format the guide does not take
};

enum class NormalRoughnessMode : uint8_t { kUnpacked, kPacked };
enum class SpecularSource : uint8_t {
  kSpecularMotion,   // the frame's own image of the reflection's motion
  kHitDistance,      // the hit distance (+ the camera matrices)
  kPrimaryMotion,    // the primary motion image, tagged under the specular type too (the reflection moves with its surface)
};

struct Tag {
  Buffer buffer = Buffer::kCount;  // what it is tagged as (its Streamline type is InfoOf(buffer).sl_type)
  Buffer source = Buffer::kCount;  // which image of Inputs supplies it (differs from `buffer` for the primary motion standing in)
  bool output_extent = false;
};

inline constexpr uint32_t kMaxTags = 12;

struct TagPlan {
  Verdict verdict = Verdict::kMissing;
  Buffer culprit = Buffer::kCount;
  std::array<Tag, kMaxTags> tags{};
  uint32_t count = 0;
  NormalRoughnessMode normal_roughness = NormalRoughnessMode::kUnpacked;
  SpecularSource specular = SpecularSource::kPrimaryMotion;
  bool needs_view_matrices = false;
};

namespace detail {
constexpr const Image& ImageOf(const Inputs& in, Buffer b) {
  switch (b) {
    case Buffer::kColorIn: return in.color;
    case Buffer::kColorOut: return in.output;
    case Buffer::kDepth: return in.depth;
    case Buffer::kMotion: return in.motion;
    case Buffer::kDiffuseAlbedo: return in.diffuse_albedo;
    case Buffer::kSpecularAlbedo: return in.specular_albedo;
    case Buffer::kNormalRoughness: return in.normal_roughness;
    case Buffer::kNormals: return in.normals;
    case Buffer::kRoughness: return in.roughness;
    case Buffer::kSpecularMotion: return in.specular_motion;
    case Buffer::kSpecularHitDistance: return in.specular_hit_distance;
    case Buffer::kResponsivity: return in.responsivity;
    case Buffer::kColorBeforeTransparency: return in.color_before_transparency;
    case Buffer::kCount: break;
  }
  return in.color;
}
}  // namespace detail

// The tags of a frame, in the order Streamline's call is given them: colour, output, depth, motion, the albedos, the normals (and roughness), the reflection's
// motion or hit distance, then the optional guides that are there. Required: colour, output, depth, motion, both albedos, the normals with a roughness (packed or
// separate), and a reflection's motion or hit distance (or the primary motion standing in, `allow_primary_motion_as_specular`).
inline TagPlan PlanTags(const Inputs& in) {
  TagPlan plan;
  const auto refuse = [&plan](Verdict v, Buffer culprit) {
    plan.verdict = v;
    plan.culprit = culprit;
    plan.count = 0;
    return plan;
  };
  const auto add = [&plan](Buffer buffer, Buffer source) {
    Tag& t = plan.tags[plan.count++];
    t.buffer = buffer;
    t.source = source;
    t.output_extent = InfoOf(buffer).at_output_size;
  };
  // Missing images first (the log names the first).
  const Buffer always[] = {Buffer::kColorIn, Buffer::kColorOut, Buffer::kDepth, Buffer::kMotion, Buffer::kDiffuseAlbedo, Buffer::kSpecularAlbedo};
  for (Buffer b : always) {
    if (!detail::ImageOf(in, b).present) return refuse(Verdict::kMissing, b);
  }
  for (Buffer b : always) {
    add(b, b);
  }
  if (in.normal_roughness.present) {
    plan.normal_roughness = NormalRoughnessMode::kPacked;
    add(Buffer::kNormalRoughness, Buffer::kNormalRoughness);
  } else {
    if (!in.normals.present) return refuse(Verdict::kMissing, Buffer::kNormals);
    if (!in.roughness.present) return refuse(Verdict::kMissing, Buffer::kRoughness);
    plan.normal_roughness = NormalRoughnessMode::kUnpacked;
    add(Buffer::kNormals, Buffer::kNormals);
    add(Buffer::kRoughness, Buffer::kRoughness);
  }
  if (in.specular_motion.present) {
    plan.specular = SpecularSource::kSpecularMotion;
    add(Buffer::kSpecularMotion, Buffer::kSpecularMotion);
  } else if (in.specular_hit_distance.present) {
    plan.specular = SpecularSource::kHitDistance;
    plan.needs_view_matrices = true;
    if (!in.have_view_matrices) return refuse(Verdict::kNeedsViewMatrices, Buffer::kSpecularHitDistance);
    add(Buffer::kSpecularHitDistance, Buffer::kSpecularHitDistance);
  } else if (in.allow_primary_motion_as_specular) {
    plan.specular = SpecularSource::kPrimaryMotion;
    add(Buffer::kSpecularMotion, Buffer::kMotion);
  } else {
    return refuse(Verdict::kMissing, Buffer::kSpecularMotion);
  }
  if (in.responsivity.present) add(Buffer::kResponsivity, Buffer::kResponsivity);
  if (in.color_before_transparency.present) add(Buffer::kColorBeforeTransparency, Buffer::kColorBeforeTransparency);

  // Sizes and formats of every tagged image (a source image is checked once per tag: the aliased motion is checked as the motion).
  for (uint32_t i = 0; i < plan.count; ++i) {
    const Tag& t = plan.tags[i];
    const Image& image = detail::ImageOf(in, t.source);
    const uint32_t want_w = t.output_extent ? in.output_size[0] : in.render_size[0];
    const uint32_t want_h = t.output_extent ? in.output_size[1] : in.render_size[1];
    if (want_w == 0 || want_h == 0 || image.width != want_w || image.height != want_h) return refuse(Verdict::kBadSize, t.buffer);
  }
  if (in.output_size[0] < in.render_size[0] || in.output_size[1] < in.render_size[1]) return refuse(Verdict::kBadSize, Buffer::kColorOut);
  for (uint32_t i = 0; i < plan.count; ++i) {
    const Tag& t = plan.tags[i];
    // The aliased motion is a motion image whatever it is tagged as.
    const Buffer as = (t.source == Buffer::kMotion) ? Buffer::kMotion : t.buffer;
    if (!FormatOk(as, detail::ImageOf(in, t.source).format)) return refuse(Verdict::kBadFormat, t.buffer);
  }
  plan.verdict = Verdict::kOk;
  plan.culprit = Buffer::kCount;
  return plan;
}

constexpr const char* VerdictText(Verdict v) {
  switch (v) {
    case Verdict::kOk: return "ok";
    case Verdict::kMissing: return "a required image is missing";
    case Verdict::kNeedsViewMatrices: return "the specular hit distance needs the camera matrices in the options";
    case Verdict::kBadSize: return "an image is not the size RR is made for";
    case Verdict::kBadFormat: return "an image has a format RR does not take";
  }
  return "?";
}

// --- the options -----------------------------------------------------------------------------------------------------------------------------------------

// Row-major 4x4, row vectors (v' = v M): Streamline's own convention.
struct Matrix4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// The camera, in world space: position and the three unit axes (Streamline's cameraPos / cameraRight / cameraUp / cameraFwd).
struct Camera {
  float position[3] = {0, 0, 0};
  float right[3] = {1, 0, 0};
  float up[3] = {0, 1, 0};
  float forward[3] = {0, 0, 1};
};

// View space to world space: the rows are the axes and the position (sl_matrix_helpers.h recalculateCameraMatrices builds it the same way).
inline Matrix4 CameraViewToWorld(const Camera& c) {
  Matrix4 out;
  const float* axes[3] = {c.right, c.up, c.forward};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) out.m[r * 4 + k] = axes[r][k];
    out.m[r * 4 + 3] = 0.0f;
  }
  out.m[12] = c.position[0];
  out.m[13] = c.position[1];
  out.m[14] = c.position[2];
  out.m[15] = 1.0f;
  return out;
}

// World space to view space: the inverse of the above for orthonormal axes (the transpose of the rotation, and the position carried through it and negated).
inline Matrix4 WorldToCameraView(const Camera& c) {
  Matrix4 out;
  const float* axes[3] = {c.right, c.up, c.forward};
  for (int col = 0; col < 3; ++col) {
    // Element (row k, column col) of the transpose is component k of axis `col`.
    for (int k = 0; k < 3; ++k) out.m[k * 4 + col] = axes[col][k];
    out.m[12 + col] = -(c.position[0] * axes[col][0] + c.position[1] * axes[col][1] + c.position[2] * axes[col][2]);
  }
  out.m[3] = out.m[7] = out.m[11] = 0.0f;
  out.m[15] = 1.0f;
  return out;
}

// The modes RR takes are DLSS's five, at DLSS's ratios (the integration guide 3.2).
enum class Mode : uint8_t { kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance, kCount };
constexpr double RatioOf(Mode m) {
  switch (m) {
    case Mode::kDlaa: return 1.0;
    case Mode::kQuality: return 1.0 / 0.66;
    case Mode::kBalanced: return 1.0 / 0.58;
    case Mode::kPerformance: return 2.0;
    case Mode::kUltraPerformance: return 3.0;
    case Mode::kCount: break;
  }
  return 1.0;
}

// DLSSDOptions' content that is ours to decide: NVIDIA's default presets (leave them eDefault: the latest transformer model), HDR colour, no alpha upscale; the
// sharpness, the exposure and the auto exposure are ignored by RR.
struct Options {
  Mode mode = Mode::kDlaa;
  uint32_t output_size[2] = {};
  NormalRoughnessMode normal_roughness = NormalRoughnessMode::kUnpacked;
  bool colour_hdr = true;
  bool alpha_upscaling = false;
  bool default_presets = true;
  Matrix4 world_to_view;
  Matrix4 view_to_world;
};
inline Options MakeOptions(Mode mode, uint32_t output_width, uint32_t output_height, const TagPlan& plan, const Camera& camera) {
  Options o;
  o.mode = mode;
  o.output_size[0] = output_width;
  o.output_size[1] = output_height;
  o.normal_roughness = plan.normal_roughness;
  o.world_to_view = WorldToCameraView(camera);
  o.view_to_world = CameraViewToWorld(camera);
  return o;
}

// The jitter sequence's length: NVIDIA asks for "at least 32" positions for RR; SR's 8 x ratio^2 (8 at 1, 18 at 1.5, 32 at 2, 72 at 3) is kept where it is longer.
inline uint32_t JitterPhases(double ratio) {
  const double r = ratio < 1.0 ? 1.0 : ratio;
  const uint32_t sr = uint32_t(std::floor(8.0 * r * r));
  return sr < 32u ? 32u : sr;
}
// What the temporal system hands upscaler_math.h JitterPhases as the runtime's own count: the longer of the context's (0: none) and RR's while RR may run this frame (the plan is RR and
// its context is ready: a frame that turns out to be Super Resolution's keeps the longer sequence, which Super Resolution takes as well), else the runtime's untouched.
inline uint32_t RuntimePhaseCount(uint32_t from_runtime, double ratio, bool rr_may_run) {
  return rr_may_run ? std::max(from_runtime, JitterPhases(ratio)) : from_runtime;
}

// --- who does the denoising ------------------------------------------------------------------------------------------------------------------------------

// The row DENOISER (perf/DLSS-RR.md section 5).
enum class Denoiser : uint8_t { kAuto, kRr, kOurs };
// The debug cvar mcla_gpu_dlss_rr (restart): auto (the default; the row DENOISER decides), off = nothing of RR exists (the plugin is not asked for, whatever the row says), probe = the plugin loads and
// its support and sizes are logged but no frame uses it, on = RR is chosen as the row's RR value would choose it AND the row's consumer rule is lifted (RR on a frame with nothing to denoise: the A/B runs).
// The numbers are the order the switch had before RR-5 (off, probe, on); the new default is last.
enum class DebugSwitch : uint8_t { kOff, kProbe, kOn, kRow };

inline bool ParseDenoiser(std::string_view text, Denoiser* out) {
  if (text == "auto") { *out = Denoiser::kAuto; return true; }
  if (text == "rr") { *out = Denoiser::kRr; return true; }
  if (text == "ours") { *out = Denoiser::kOurs; return true; }
  return false;
}
inline bool ParseDebugSwitch(std::string_view text, DebugSwitch* out) {
  if (text == "auto") { *out = DebugSwitch::kRow; return true; }
  if (text == "off") { *out = DebugSwitch::kOff; return true; }
  if (text == "probe") { *out = DebugSwitch::kProbe; return true; }
  if (text == "on") { *out = DebugSwitch::kOn; return true; }
  return false;
}
constexpr const char* DenoiserName(Denoiser d) { return d == Denoiser::kAuto ? "auto" : (d == Denoiser::kRr ? "rr" : "ours"); }
constexpr const char* DebugSwitchName(DebugSwitch s) {
  return s == DebugSwitch::kRow ? "auto" : (s == DebugSwitch::kOff ? "off" : (s == DebugSwitch::kProbe ? "probe" : "on"));
}

// What the PC and the build can do (Streamline's answers, published by the DLSS backend).
struct Facts {
  bool build_has_rr = false;         // LARECOMP_HAS_DLSS_RR: the headers and the two files were fetched
  bool files_ok = false;             // no known problem with sl.dlss_d.dll and nvngx_dlssd.dll (next to the exe, the plugin signed): false only where a check ran and said no
  bool streamline_running = false;   // slInit and the device are done (a start still going on is not "not supported")
  bool supported = false;            // slIsFeatureSupported(kFeatureDLSS_RR) said yes for this adapter
  bool failed = false;               // (RR-3) RR was dropped for this session: its feature could not be made, or its evaluation failed FailureBook::kMaxErrors times in a row
  // (RR-5, after the others so that the first five can still be written as a list) The plugin: Streamline was started WITH it in this session (plugins cannot be added later), and where it was not, whether
  // it is only because of the row (OURS) or the switch: an RTX card, the files fine, a build with it, an effect with a raw mode. A start with another row would load it: a restart.
  bool asked = false;
  bool loadable = false;
};

struct PlanInputs {
  Denoiser setting = Denoiser::kAuto;
  DebugSwitch debug = DebugSwitch::kRow;
  Facts facts;
  bool upscaler_is_dlss = false;     // RR includes the upscale: only where DLSS is the upscaler
  bool consumers_active = false;     // AUTO's: some effect that RR cleans better than ours (SSR, city RT reflections, RTGI ...: the A/B said so, see Consumers below) is on
  bool consumers_any = false;        // (RR-5) the row's RR value's: some effect that has a raw mode built is on (a superset of the one above: the rules treat `active` as implying it)
  bool guides_ready = false;         // the frame's guide images exist and were handed over (TemporalSystem::NoteRrGuides, RR-3; the pass that makes them is RR-2's rr_guides.h)
  bool room_ok = true;               // (RR-3) video memory has room for the RR context (ContextNeedBytes); true where it is not asked (the context exists, or the sizes are not known yet)
};

enum class Why : uint8_t {
  kRrOn,
  kSwitchOff,           // mcla_gpu_dlss_rr is off
  kSettingOurs,         // the row says OURS
  kBuildHasNoRr,
  kFilesMissing,
  kStreamlineNotRunning,
  kNotSupported,
  kUpscalerNotDlss,
  kProbeOnly,           // mcla_gpu_dlss_rr is probe
  kNoConsumer,          // AUTO and nothing that wants RR is on
  kGuidesNotReady,
  kRrFailed,            // (RR-3) RR failed in this session and stays off until the game is started again; Super Resolution goes on
  kNoRoom,              // (RR-3) video memory has no room for the RR context; Super Resolution goes on, the context is asked again after a wait
  kNotLoaded,           // (RR-5) Streamline was started without the RR plugin (the row said OURS then): it can load only at the next start
};
constexpr const char* WhyText(Why w) {
  switch (w) {
    case Why::kRrOn: return "RR is on";
    case Why::kSwitchOff: return "mcla_gpu_dlss_rr is off";
    case Why::kSettingOurs: return "the setting is OURS";
    case Why::kBuildHasNoRr: return "this build has no DLSS Ray Reconstruction";
    case Why::kFilesMissing: return "sl.dlss_d.dll or nvngx_dlssd.dll is not next to the exe, or the plugin is not signed";
    case Why::kStreamlineNotRunning: return "Streamline has not finished starting";
    case Why::kNotSupported: return "Streamline says this graphics card or driver cannot run RR";
    case Why::kUpscalerNotDlss: return "the upscaler is not DLSS (RR includes the upscale)";
    case Why::kProbeOnly: return "mcla_gpu_dlss_rr is probe: RR is loaded and asked, no frame uses it";
    case Why::kNoConsumer: return "no effect that RR denoises is on (AUTO needs one RR cleans better than ours, DLSS RR any with a raw mode)";
    case Why::kGuidesNotReady: return "the guide images are not handed to the upscaler yet (the guide pass makes them: rr_guides.h)";
    case Why::kRrFailed: return "Ray Reconstruction failed in this session (the log says why) and stays off until the game is started again; Super Resolution goes on";
    case Why::kNoRoom: return "there is no room in video memory for the Ray Reconstruction context; Super Resolution goes on";
    case Why::kNotLoaded: return "Streamline was started without the Ray Reconstruction plugin (the row said OURS then): it loads at the next start";
  }
  return "?";
}

struct Plan {
  bool use_rr = false;
  Why why = Why::kSwitchOff;
};

// --- the consumers (RR-5) -------------------------------------------------------------------------------------------------------------------------------

// The effects that make noisy radiance RR can clean. An effect is a CONSUMER when it has a raw mode (RR-4: it hands RR its 1 spp radiance and the hit distance, and skips its own filter while the plan is
// RR); two flags each, both measured and decided per effect, never by the row:
//   raw_mode_built   the raw mode exists. Until it does, the effect is not a consumer (it keeps its own denoiser whatever the row says) and its "on" does not count.
//   auto_uses_rr     AUTO trusts RR with it: the A/B pictures and the cost said RR beats ours there (perf/DLSS-RR.md section 3: "keep ours where RR is not better"). The row's own RR value takes every
//                    consumer with a raw mode; AUTO only these.
// So AUTO is "RR where it is known to be better", the RR value is "RR wherever it can run", and OURS is "never". RR-4's packages flip these two tables' cells as each raw mode lands and each A/B is
// read; the test compares them with the table's own words. THE CELLS ARE FALSE BECAUSE NO RAW MODE EXISTS YET: with them false nothing is a consumer, AUTO is OURS everywhere, the row is grey, and the
// plugin is not asked for (WantRrPlugin), so a player's start is exactly what it was before the row.
enum class Consumer : uint8_t {
  kSsr,                 // the screen-space reflections (REFLECTIONS ON SURFACES): stochastic, additive radiance with a hit distance (the first real candidate)
  kCarRtReflections,    // the car's ray traced self-reflections and the chrome of other cars (car_rt_refl.h): an A/B, expected no gain
  kCityRtReflections,   // the city's ray traced reflections (track 8): designed noisy, RR's reason to exist here
  kRtGi,                // ray traced bounce light (RT BOUNCE LIGHT, the probe field): designed noisy
  kCount,
};
inline constexpr size_t kConsumerCount = size_t(Consumer::kCount);
struct ConsumerInfo {
  const char* name;
  bool raw_mode_built;
  bool auto_uses_rr;
};
using ConsumerTable = std::array<ConsumerInfo, kConsumerCount>;
inline constexpr ConsumerTable kConsumers = {{
    {"screen-space reflections", false, false},
    {"ray traced car reflections", false, false},
    {"ray traced city reflections", false, false},
    {"ray traced bounce light", false, false},
}};
constexpr const ConsumerInfo& InfoOf(Consumer c) { return kConsumers[size_t(c)]; }
constexpr uint32_t ConsumerBit(Consumer c) { return 1u << uint32_t(c); }
inline constexpr uint32_t kAllConsumers = (1u << uint32_t(kConsumerCount)) - 1u;

// Whether some effect has a raw mode (the plugin is worth loading at all), and the two questions of the plan for a set of effects that are on now (bit i = Consumer i; the effect owners say it each frame,
// denoiser_setting.h NoteConsumer): any consumer, and any consumer AUTO trusts RR with. The table is a parameter (the shipped one by default) so that the test can ask them of a table with cells flipped.
constexpr bool AnyRawModeBuilt(const ConsumerTable& table = kConsumers) {
  for (const ConsumerInfo& c : table) {
    if (c.raw_mode_built) return true;
  }
  return false;
}
constexpr bool AnyConsumerOn(uint32_t on_mask, const ConsumerTable& table = kConsumers) {
  for (size_t i = 0; i < kConsumerCount; ++i) {
    if (((on_mask >> i) & 1u) != 0 && table[i].raw_mode_built) return true;
  }
  return false;
}
constexpr bool AnyAutoConsumerOn(uint32_t on_mask, const ConsumerTable& table = kConsumers) {
  for (size_t i = 0; i < kConsumerCount; ++i) {
    if (((on_mask >> i) & 1u) != 0 && table[i].raw_mode_built && table[i].auto_uses_rr) return true;
  }
  return false;
}

// RR or our own denoisers; the first reason that applies is the answer.
//   - The debug switch: off = nothing of RR; on stands for the row's RR value and, like probe, lifts the consumer rule (RR even over a frame with nothing to denoise: the A/B runs and the cost runs).
//   - OURS never. With the row deciding (the switch on auto): AUTO needs a consumer AUTO trusts RR with, RR needs a consumer with a raw mode (NVIDIA: RR is for ray traced content, "disable when ray tracing
//     is not enabled": the row is grey then). This rule comes first, so that a PC that cannot run RR, or a build without it, answers "no effect that wants it" and says nothing more while nothing wants it.
//   - Then what RR needs of the PC (denoiser_setting.h GateOf asks this function again with a consumer made up, to name why the row is grey whatever the setting).
//   - The plugin must have been asked for at Streamline's start (plugins cannot be added later): a start with the row on OURS leaves it out, and the answer is kNotLoaded (a restart) where the PC
//     could have loaded it (Facts::loadable), kNotSupported where it could not (not an RTX card, Vulkan without DLSS saved ...).
inline Plan SelectDenoiser(const PlanInputs& in) {
  const auto no = [](Why why) { return Plan{false, why}; };
  if (in.debug == DebugSwitch::kOff) return no(Why::kSwitchOff);
  const Denoiser setting = in.debug == DebugSwitch::kOn ? Denoiser::kRr : in.setting;
  if (setting == Denoiser::kOurs) return no(Why::kSettingOurs);
  if (in.debug == DebugSwitch::kRow) {
    if (setting == Denoiser::kAuto && !in.consumers_active) return no(Why::kNoConsumer);
    if (setting == Denoiser::kRr && !in.consumers_active && !in.consumers_any) return no(Why::kNoConsumer);
  }
  if (!in.facts.build_has_rr) return no(Why::kBuildHasNoRr);
  if (!in.facts.files_ok) return no(Why::kFilesMissing);
  if (!in.facts.streamline_running) return no(Why::kStreamlineNotRunning);
  if (!in.facts.asked) return no(in.facts.loadable ? Why::kNotLoaded : Why::kNotSupported);
  if (!in.facts.supported) return no(Why::kNotSupported);
  if (in.facts.failed) return no(Why::kRrFailed);
  if (!in.upscaler_is_dlss) return no(Why::kUpscalerNotDlss);
  if (in.debug == DebugSwitch::kProbe) return no(Why::kProbeOnly);
  if (!in.room_ok) return no(Why::kNoRoom);
  if (!in.guides_ready) return no(Why::kGuidesNotReady);
  return Plan{true, Why::kRrOn};
}

// --- a frame RR cannot take --------------------------------------------------------------------------------------------------------------------------------

// What the frame says about its G-buffer (gbuffer.h frame(), published at the mid-scene depth copy; the pause world's 4x views, the garage, the map and a cutscene's
// frames never carry the sidecar).
struct FrameFacts {
  bool gbuffer_published = false;
  uint32_t planes_valid = 0;          // gbuffer::PlaneBit mask (bit 0 normal, 1 albedo, 2 material)
  uint32_t gbuffer_size[2] = {};
  uint32_t render_size[2] = {};
  bool cutscene = false;
  bool pause_view = false;            // the pause menu's, the garage's or the map's world
};
inline constexpr uint32_t kNeededPlanes = 7;  // normal, albedo and material

enum class Gate : uint8_t { kOpen, kNoGBuffer, kPlanesMissing, kSizeMismatch, kCutscene, kPauseView };
constexpr const char* GateText(Gate g) {
  switch (g) {
    case Gate::kOpen: return "RR can take this frame";
    case Gate::kNoGBuffer: return "the frame has no G-buffer";
    case Gate::kPlanesMissing: return "the G-buffer lacks one of the normal, albedo and material planes";
    case Gate::kSizeMismatch: return "the G-buffer is not the render size";
    case Gate::kCutscene: return "a cutscene frame";
    case Gate::kPauseView: return "the pause, garage or map view";
  }
  return "?";
}
inline Gate FrameGate(const FrameFacts& f) {
  if (f.cutscene) return Gate::kCutscene;
  if (f.pause_view) return Gate::kPauseView;
  if (!f.gbuffer_published) return Gate::kNoGBuffer;
  if ((f.planes_valid & kNeededPlanes) != kNeededPlanes) return Gate::kPlanesMissing;
  if (f.gbuffer_size[0] != f.render_size[0] || f.gbuffer_size[1] != f.render_size[1]) return Gate::kSizeMismatch;
  return Gate::kOpen;
}

// --- RR-3: which context a frame goes to, how long RR's context is kept, what it costs, what a failure means -------------------------------------------------

// Two contexts live side by side while RR is wanted: the Super Resolution one the upscaler always has (the pause world, the garage, the map, a cutscene, the first frames and every
// frame whose guides are missing use it) and the RR one (perf/DLSS-RR.md section 4). A dispatch goes to exactly one of them.
enum class Dispatch : uint8_t { kSuperResolution, kRayReconstruction };
enum class DispatchWhy : uint8_t {
  kRr,                 // RR runs this frame
  kPlanOurs,           // the plan is not RR (the setting, the card, the upscaler, no consumer, no room ...): Super Resolution is the upscaler
  kRrFailed,           // RR was dropped for the session
  kNoGuides,           // this frame has no guide images (no G-buffer: pause, garage, map, cutscene, the first frame; or the guide pass was not ready)
  kContextNotReady,    // the RR context is being made (its feature is built in the background): Super Resolution goes on meanwhile
  kCount,
};
constexpr const char* DispatchWhyText(DispatchWhy w) {
  switch (w) {
    case DispatchWhy::kRr: return "Ray Reconstruction";
    case DispatchWhy::kPlanOurs: return "the plan is not Ray Reconstruction";
    case DispatchWhy::kRrFailed: return "Ray Reconstruction failed in this session";
    case DispatchWhy::kNoGuides: return "the frame has no guide images";
    case DispatchWhy::kContextNotReady: return "the Ray Reconstruction context is not made yet";
    case DispatchWhy::kCount: break;
  }
  return "?";
}

struct DispatchInputs {
  bool plan_rr = false;          // this frame's plan (SelectDenoiser) says RR
  bool rr_failed = false;        // the backend dropped RR for the session
  bool guides_now = false;       // THIS frame's guide pass ran (its images are in SHADER_READ at the compute stage)
  bool context_ready = false;    // the RR context exists, its feature made
};
struct DispatchChoice {
  Dispatch kind = Dispatch::kSuperResolution;
  DispatchWhy why = DispatchWhy::kPlanOurs;
};
// The first reason that applies is the answer. RR needs all of: the plan, no failure, this frame's guides, its context. Anything missing is a Super Resolution frame: the upscale
// itself is never lost to RR (a missing guide is an SR frame, never "DLSS off for the session").
constexpr DispatchChoice ChooseDispatch(const DispatchInputs& in) {
  if (!in.plan_rr) return {Dispatch::kSuperResolution, DispatchWhy::kPlanOurs};
  if (in.rr_failed) return {Dispatch::kSuperResolution, DispatchWhy::kRrFailed};
  if (!in.guides_now) return {Dispatch::kSuperResolution, DispatchWhy::kNoGuides};
  if (!in.context_ready) return {Dispatch::kSuperResolution, DispatchWhy::kContextNotReady};
  return {Dispatch::kRayReconstruction, DispatchWhy::kRr};
}

// Whether the plan's reason says the RR context is worth having: RR is the choice (kRrOn) or would be as soon as the guides are there (kGuidesNotReady): the same words that make the
// guide pass wanted (rr_guides_rules.h PassWanted), so the context is being made while the guides are, and a switch is instant. Every other reason (off, OURS, no consumer, no room,
// failed ...) wants none.
constexpr bool ContextWanted(Why why) { return why == Why::kRrOn || why == Why::kGuidesNotReady; }

// The RR context is kept for kContextHoldFrames frames after the plan stops wanting it (a pause, a consumer toggled off and on, a frame without an anchor): a quick switch back finds it,
// and an RR that is really off gives its video memory back after that.
inline constexpr uint32_t kContextHoldFrames = 600;
class ContextKeeper {
 public:
  // One call per frame: whether the context should exist this frame.
  bool Update(bool wanted) {
    if (wanted) {
      keep_ = true;
      unwanted_ = 0;
    } else if (keep_ && ++unwanted_ > kContextHoldFrames) {
      keep_ = false;
      unwanted_ = 0;
    }
    return keep_;
  }
  bool keep() const { return keep_; }
  void Reset() { *this = ContextKeeper{}; }

 private:
  bool keep_ = false;
  uint32_t unwanted_ = 0;
};

// What the RR context asks for in video memory. The network (Default / preset F, the integration guide 2.2): 154, 268 and 579 MB for an output of 1080p, 1440p and 4K, taken between
// those points by the output's pixels (above 4K on the last slope, under 1080p in proportion). The runtime's own figure for a context it made (`measured_bytes` for
// `measured_pixels` of output) replaces the table, scaled by the pixels. The guide images (28 bytes a render pixel, rr_guides_rules.h BytesOf) are asked for too where the pass has not made
// them yet (`guide_bytes`); the Super Resolution context kept beside it is counted already (it exists).
inline constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t NetworkBytes(uint32_t out_w, uint32_t out_h) {
  const uint64_t px = uint64_t(out_w) * out_h;
  constexpr uint64_t p1 = 1920ull * 1080ull, p2 = 2560ull * 1440ull, p3 = 3840ull * 2160ull;
  constexpr uint64_t b1 = 154 * kMiB, b2 = 268 * kMiB, b3 = 579 * kMiB;
  if (px == 0) return 0;
  if (px <= p1) return b1 * px / p1;
  if (px <= p2) return b1 + (b2 - b1) * (px - p1) / (p2 - p1);
  return b2 + (b3 - b2) * (px - p2) / (p3 - p2);
}
constexpr uint64_t ContextNeedBytes(uint32_t out_w, uint32_t out_h, uint64_t measured_bytes, uint64_t measured_pixels, uint64_t guide_bytes) {
  const uint64_t scaled = vram_room::ScaledEstimate(measured_bytes, measured_pixels, uint64_t(out_w) * out_h);
  return (scaled != 0 ? scaled : NetworkBytes(out_w, out_h)) + guide_bytes;
}
// vram_room.h's line: the use plus the need stays under kHighPercent of the budget (true where the adapter reports none).
constexpr bool HasRoomForContext(uint64_t used, uint64_t budget, uint64_t need) { return vram_room::HasRoom(used, budget, need); }

// What one RR evaluation came to, and what it means (render thread). A REFUSAL (the frame's guides are not right: nothing was recorded) is an SR frame and never a count against RR: the
// guides may be right next frame. An ERROR (Streamline said no after the tags were accepted) makes the frame Super Resolution's (never a plain one: three jittered frames in a row that are
// not upscaled turn the upscaler off for the session); kMaxErrors in a row drop RR for the session, with Super Resolution going on (the backend is NOT failed: "never DLSS off for the
// session"). A run resets the count; a refusal does not.
class FailureBook {
 public:
  enum class Event : uint8_t { kRan, kRefused, kError };
  static constexpr uint32_t kMaxErrors = 3;
  // True when this event dropped RR for the session (the first time only).
  bool Note(Event event) {
    switch (event) {
      case Event::kRan:
        ++ran_;
        errors_in_row_ = 0;
        break;
      case Event::kRefused:
        ++refused_;
        break;
      case Event::kError:
        ++errors_;
        if (++errors_in_row_ >= kMaxErrors && !dropped_) {
          dropped_ = true;
          return true;
        }
        break;
    }
    return false;
  }
  // The feature could not be made (the background build): RR is dropped at once. True the first time.
  bool Drop() {
    const bool first = !dropped_;
    dropped_ = true;
    return first;
  }
  bool dropped() const { return dropped_; }
  uint64_t ran() const { return ran_; }
  uint64_t refused() const { return refused_; }
  uint64_t errors() const { return errors_; }
  uint32_t errors_in_row() const { return errors_in_row_; }

 private:
  uint64_t ran_ = 0;
  uint64_t refused_ = 0;
  uint64_t errors_ = 0;
  uint32_t errors_in_row_ = 0;
  bool dropped_ = false;
};

// The debug lever mcla_gpu_dlss_rr_test (RR-3): the frames Ray Reconstruction cannot take, made on purpose, so that a run can see Super Resolution take them and the switch back need no
// rebuild. 1: the guide images are withheld from the dispatch for kTestPhaseFrames frames in every 2 x kTestPhaseFrames (the frame has no guides); 2: in the same frames the dispatch carries
// no diffuse albedo, so the rules refuse it and the backend gives the frame to Super Resolution in the same call; 3: Ray Reconstruction's evaluation fails FailureBook::kMaxErrors frames in
// a row, once (it is dropped for the session). Only while Ray Reconstruction is the plan.
enum class TestLever : uint8_t { kOff = 0, kWithholdGuides = 1, kRefuseGuides = 2, kInjectErrors = 3 };
constexpr TestLever TestLeverOf(int value) { return value >= 1 && value <= 3 ? TestLever(value) : TestLever::kOff; }
inline constexpr uint64_t kTestPhaseFrames = 30;
// The frames the lever acts in: the second half of each 60 frame cycle (the first 30 frames of the run are untouched, so the switch to RR has settled before the first one).
constexpr bool TestPhaseActive(uint64_t frame) { return (frame / kTestPhaseFrames) % 2 == 1; }
inline constexpr uint32_t kTestErrorFrames = FailureBook::kMaxErrors;

// --- the plugin -------------------------------------------------------------------------------------------------------------------------------------------

// The files RR needs next to the exe (cmake/FetchStreamline.cmake copies them): the plugin, checked for NVIDIA's signature before anything is loaded, and its network,
// which the plugin loads and checks itself.
inline constexpr std::array<const char*, 2> kFiles = {"sl.dlss_d.dll", "nvngx_dlssd.dll"};
inline constexpr std::array<const char*, 1> kSignedFiles = {"sl.dlss_d.dll"};

// Whether Streamline is started with the RR plugin (slInit's featuresToLoad: plugins cannot be added later, like frame generation's). The row decides what the player wants (anything but OURS), the
// debug switch can force it (probe and on load it whatever the row says: a test of RR itself; off never), and there must be something to use it: no effect has a raw mode yet (kConsumers), so by default
// no player's start loads it. The rest is what the PC can do.
struct PluginInputs {
  DebugSwitch debug = DebugSwitch::kRow;
  Denoiser setting = Denoiser::kAuto;  // the row at the start
  bool raw_mode_built = false;         // AnyRawModeBuilt()
  bool build_has_rr = false;
  bool files_ok = false;
  bool looks_rtx = false;            // the adapter's name says RTX (the support query is the final word)
  bool d3d12 = false;                // D3D12 starts Streamline after the device and may switch to DLSS live; Vulkan does not
  bool dlss_saved_at_start = false;  // Vulkan: DLSS is the saved upscaler at the start (the device is made through Streamline then)
};
// What the row and the switch ask for, before the PC's say: whether a start would load the plugin on a PC that can (also the file check's question: the files are looked at only when this is true).
constexpr bool PluginWantedByChoice(const PluginInputs& in) {
  if (in.debug == DebugSwitch::kOff) return false;
  if (in.debug == DebugSwitch::kProbe || in.debug == DebugSwitch::kOn) return true;
  return in.setting != Denoiser::kOurs && in.raw_mode_built;
}
constexpr bool WantRrPlugin(const PluginInputs& in) {
  if (!PluginWantedByChoice(in) || !in.build_has_rr || !in.files_ok || !in.looks_rtx) return false;
  return in.d3d12 || in.dlss_saved_at_start;
}

}  // namespace mcla::gpu::rr
