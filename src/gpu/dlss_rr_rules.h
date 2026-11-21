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
//   FrameGate              why a frame cannot be given to RR even when it is the plan (no G-buffer planes: pause world, garage, map, cutscene): that frame is SR's.
//   JitterPhases           RR wants at least 32 positions in the jitter sequence (SR's 8 x ratio^2 is not enough).
//   WantRrPlugin           whether Streamline is started with the RR plugin (plugins cannot be added after slInit).
//
// The rules the numbers come from: Streamline 2.14.16 docs/ProgrammingGuideDLSS_RR.md, the DLSS-RR Integration Guide (August 2026) chapter 3, and NVIDIA's RR
// integration checklist in the same package (plugins/streamline-agent-skills/skills/integrate-dlss-rr).
// ===========================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

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

// --- who does the denoising ------------------------------------------------------------------------------------------------------------------------------

// The row DENOISER (perf/DLSS-RR.md section 5).
enum class Denoiser : uint8_t { kAuto, kRr, kOurs };
// The debug cvar mcla_gpu_dlss_rr: off = nothing of RR exists (the plugin is not asked for), probe = the plugin loads and its support and sizes are logged but no
// frame uses it, on = RR is eligible.
enum class DebugSwitch : uint8_t { kOff, kProbe, kOn };

inline bool ParseDenoiser(std::string_view text, Denoiser* out) {
  if (text == "auto") { *out = Denoiser::kAuto; return true; }
  if (text == "rr") { *out = Denoiser::kRr; return true; }
  if (text == "ours") { *out = Denoiser::kOurs; return true; }
  return false;
}
inline bool ParseDebugSwitch(std::string_view text, DebugSwitch* out) {
  if (text == "off") { *out = DebugSwitch::kOff; return true; }
  if (text == "probe") { *out = DebugSwitch::kProbe; return true; }
  if (text == "on") { *out = DebugSwitch::kOn; return true; }
  return false;
}
constexpr const char* DenoiserName(Denoiser d) { return d == Denoiser::kAuto ? "auto" : (d == Denoiser::kRr ? "rr" : "ours"); }
constexpr const char* DebugSwitchName(DebugSwitch s) { return s == DebugSwitch::kOff ? "off" : (s == DebugSwitch::kProbe ? "probe" : "on"); }

// What the PC and the build can do (Streamline's answers, published by the DLSS backend).
struct Facts {
  bool build_has_rr = false;         // LARECOMP_HAS_DLSS_RR: the headers and the two files were fetched
  bool files_ok = false;             // sl.dlss_d.dll and nvngx_dlssd.dll are next to the exe, the plugin is signed
  bool streamline_running = false;   // slInit and the device are done (a start still going on is not "not supported")
  bool supported = false;            // slIsFeatureSupported(kFeatureDLSS_RR) said yes for this adapter
};

struct PlanInputs {
  Denoiser setting = Denoiser::kAuto;
  DebugSwitch debug = DebugSwitch::kOff;
  Facts facts;
  bool upscaler_is_dlss = false;     // RR includes the upscale: only where DLSS is the upscaler
  bool consumers_active = false;     // some effect that wants RR (SSR, city RT reflections, RTGI, a raw mode of the car effects) is on
  bool guides_ready = false;         // the frame's guide images exist (RR-2)
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
    case Why::kNoConsumer: return "AUTO, and no effect that wants RR is on";
    case Why::kGuidesNotReady: return "the guide images are not made yet (the guide pass is package RR-2)";
  }
  return "?";
}

struct Plan {
  bool use_rr = false;
  Why why = Why::kSwitchOff;
};

// RR or our own denoisers; the first reason that applies is the answer.
inline Plan SelectDenoiser(const PlanInputs& in) {
  const auto no = [](Why why) { return Plan{false, why}; };
  if (in.debug == DebugSwitch::kOff) return no(Why::kSwitchOff);
  if (in.setting == Denoiser::kOurs) return no(Why::kSettingOurs);
  if (!in.facts.build_has_rr) return no(Why::kBuildHasNoRr);
  if (!in.facts.files_ok) return no(Why::kFilesMissing);
  if (!in.facts.streamline_running) return no(Why::kStreamlineNotRunning);
  if (!in.facts.supported) return no(Why::kNotSupported);
  if (!in.upscaler_is_dlss) return no(Why::kUpscalerNotDlss);
  if (in.debug == DebugSwitch::kProbe) return no(Why::kProbeOnly);
  if (in.setting == Denoiser::kAuto && !in.consumers_active) return no(Why::kNoConsumer);
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

// --- the plugin -------------------------------------------------------------------------------------------------------------------------------------------

// The files RR needs next to the exe (cmake/FetchStreamline.cmake copies them): the plugin, checked for NVIDIA's signature before anything is loaded, and its network,
// which the plugin loads and checks itself.
inline constexpr std::array<const char*, 2> kFiles = {"sl.dlss_d.dll", "nvngx_dlssd.dll"};
inline constexpr std::array<const char*, 1> kSignedFiles = {"sl.dlss_d.dll"};

// Whether Streamline is started with the RR plugin (slInit's featuresToLoad: plugins cannot be added later, like frame generation's).
struct PluginInputs {
  DebugSwitch debug = DebugSwitch::kOff;
  bool build_has_rr = false;
  bool files_ok = false;
  bool looks_rtx = false;            // the adapter's name says RTX (the support query is the final word)
  bool d3d12 = false;                // D3D12 starts Streamline after the device and may switch to DLSS live; Vulkan does not
  bool dlss_saved_at_start = false;  // Vulkan: DLSS is the saved upscaler at the start (the device is made through Streamline then)
};
constexpr bool WantRrPlugin(const PluginInputs& in) {
  if (in.debug == DebugSwitch::kOff || !in.build_has_rr || !in.files_ok || !in.looks_rtx) return false;
  return in.d3d12 || in.dlss_saved_at_start;
}

}  // namespace mcla::gpu::rr
