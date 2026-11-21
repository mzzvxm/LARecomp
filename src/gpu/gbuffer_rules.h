#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's rules: the C++ twin of hlsl/gbuffer_common.hlsli, and the host's pure decisions
// ===========================================================================
// Part 1 (the twin, FROZEN with the .hlsli: package IF of perf/GBUFFER-SURVEY.md 7.2): the encodings of the three planes and of the material word,
// the derivative normal's arithmetic, in plain C++ so that a test can run them and so that a CPU consumer (a unit test, the debug statistics) reads a
// plane the way a shader does. gbuffer_rules_test.cpp checks the twin on its own (round trips, the 16 bit error, the "no data" code), compiles the
// .hlsli as C++ and compares it with this, and compares every GB_* define with gbuffer_types.h.
//
// Part 2 (the host's rules, GB-A's: gbuffer.cpp and Scene call them): which planes a frame makes and which of them a draw carries, which draws of the frame get the
// sidecar and what each does to it (writes it, clears the pixels it covers, leaves them), the frame window the sidecar lives in (cleared with the guest's clear of the
// main colour, closed at the mid-scene depth copy), the trigger, the views that never write (the pause world, a cutscene, a mirror), the memory, the consumers' requests
// and the setting's words. Pure: no Plume, no renderer, so they are tested alone.
//
// The window of a frame (survey 3.2 and 3.4):
//   frame start        closed, not cleared
//   a clear of the main view's colour (the guest clears it at the scene's start, and once more after the sun shadow collector in a day frame)
//                      the sidecar is cleared with the same rectangle (0 = "no data") and the window is open
//   a main view colour draw while the window is closed and the frame has seen no clear at all (a build whose first draw comes before a clear)
//                      the whole sidecar is cleared first, then the draw goes in
//   the mid-scene depth copy of the main depth (the AO's trigger): the window closes, the planes are published; later draws of the main view
//                      (particles, the fog quad, the glows) have no sidecar at all
//
// Threading: pure functions and plain data; the render thread owns the Window.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "gbuffer_types.h"

namespace mcla::gpu::gbuffer {

// ===========================================================================
// Part 1: the twin
// ===========================================================================

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};
struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};
struct Quat {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) { return a * (1.0f / Length(a)); }

inline float SignNotZero(float v) { return v >= 0.0f ? 1.0f : -1.0f; }
inline float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// GbOctEncode: the octahedral code of a direction, 0..1 each. `n` is not zero.
inline Vec2 OctEncode(Vec3 n) {
  const float inv = 1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
  const float x = n.x * inv;
  const float y = n.y * inv;
  const float z = n.z * inv;
  float ex = x;
  float ey = y;
  if (z < 0.0f) {
    ex = (1.0f - std::fabs(y)) * SignNotZero(x);
    ey = (1.0f - std::fabs(x)) * SignNotZero(y);
  }
  return {ex * 0.5f + 0.5f, ey * 0.5f + 0.5f};
}

// GbOctDecode.
inline Vec3 OctDecode(Vec2 f) {
  float x = f.x * 2.0f - 1.0f;
  float y = f.y * 2.0f - 1.0f;
  const float z = 1.0f - std::fabs(x) - std::fabs(y);
  const float t = Saturate(-z);
  x = x + (x >= 0.0f ? -t : t);
  y = y + (y >= 0.0f ? -t : t);
  const float il = 1.0f / std::sqrt(x * x + y * y + z * z);
  return {x * il, y * il, z * il};
}

// GbRt1Encode: the float code a shader outputs; never (0, 0) for a direction, (0, 0) for a non-direction.
inline Vec2 Rt1Encode(Vec3 n) {
  const float len2 = Dot(n, n);
  if (!(len2 > 1.0e-12f)) {
    return {0.0f, 0.0f};
  }
  Vec2 e = OctEncode(n);
  if (e.x < 0.5f / 65535.0f && e.y < 0.5f / 65535.0f) {
    e.x = 1.0f / 65535.0f;
  }
  return e;
}

// What the hardware stores for a float written to an UNORM texel: round to nearest, clamped (a NaN is 0).
inline uint32_t QuantizeUnorm(float v, uint32_t max_value) {
  if (!(v > 0.0f)) {
    return 0;
  }
  if (v >= 1.0f) {
    return max_value;
  }
  return uint32_t(std::floor(v * float(max_value) + 0.5f));
}

struct Rt1Texel {
  uint16_t x = 0;
  uint16_t y = 0;
  bool operator==(const Rt1Texel& o) const { return x == o.x && y == o.y; }
};

// The R16G16_UNORM texel of a normal (what the sidecar holds after a variant wrote it).
inline Rt1Texel Rt1Pack(Vec3 n) {
  const Vec2 e = Rt1Encode(n);
  return {uint16_t(QuantizeUnorm(e.x, 65535)), uint16_t(QuantizeUnorm(e.y, 65535))};
}
// GbRt1Valid on the texel.
inline bool Rt1Valid(Rt1Texel t) { return t.x != 0 || t.y != 0; }
// What a Load or a sample of the texel returns, and GbRt1Decode of it.
inline Vec2 Rt1Load(Rt1Texel t) { return {float(t.x) / 65535.0f, float(t.y) / 65535.0f}; }
inline Vec3 Rt1Decode(Rt1Texel t) { return OctDecode(Rt1Load(t)); }

// GbAlbedoEncode / GbAlbedoDecode, one channel.
inline float AlbedoEncode(float a) { return std::sqrt(Saturate(a)); }
inline float AlbedoDecode(float s) { return s * s; }
inline uint32_t AlbedoPack(float a) { return QuantizeUnorm(AlbedoEncode(a), 255); }
inline float AlbedoUnpack(uint32_t byte) { return AlbedoDecode(float(byte) / 255.0f); }

// The material word and RT3 (gbuffer_types.h).
inline uint32_t MaterialWord(float smoothness, uint32_t cls, uint32_t flags, float lit) {
  const uint32_t s = uint32_t(Saturate(smoothness) * 255.0f + 0.5f);
  const uint32_t l = uint32_t(Saturate(lit) * 255.0f + 0.5f);
  return s | ((cls & kClassMask) << kMaterialClassShift) | ((flags & 7u) << kMaterialFlagsShift) | (l << kMaterialLitShift);
}
inline uint32_t MaterialWord(float smoothness, SurfaceClass cls, uint32_t flags, float lit) {
  return MaterialWord(smoothness, uint32_t(cls), flags, lit);
}
// The two bytes of RT3 for a word.
inline uint32_t MaterialRt3Smoothness(uint32_t word) { return word & 255u; }
inline uint32_t MaterialRt3ClassAndFlags(uint32_t word) { return (word >> kMaterialClassShift) & 255u; }
inline uint32_t MaterialLitByte(uint32_t word) { return (word >> kMaterialLitShift) & 255u; }
// The consumer's side, from the stored bytes.
inline SurfaceClass ClassOfByte(uint32_t class_and_flags) { return SurfaceClass(class_and_flags & kClassMask); }
inline uint32_t FlagsOfByte(uint32_t class_and_flags) { return (class_and_flags >> 5) & 7u; }

// GbQuatRotate.
inline Vec3 QuatRotate(Quat q, Vec3 v) {
  const Vec3 u{q.x, q.y, q.z};
  const Vec3 t = Cross(u, v) * 2.0f;
  return v + t * q.w + Cross(u, t);
}

// GbWorldPoint: `ray` is GBufferConstants::view_ray.
inline Vec3 WorldPoint(const float ray[4], Quat q, float px, float py, float w) {
  const float z = 1.0f / w;
  const Vec3 view{(px * ray[0] + ray[1]) * z, (py * ray[2] + ray[3]) * z, z};
  return QuatRotate(q, view);
}

// GbFaceNormal.
inline Vec3 FaceNormal(Vec3 dpdx, Vec3 dpdy, Vec3 to_camera) {
  Vec3 n = Normalize(Cross(dpdx, dpdy));
  if (Dot(n, to_camera) < 0.0f) {
    n = n * -1.0f;
  }
  return n;
}

// The host's side of GBufferConstants::view_ray: from the projection's half angles (their tangents) and the target's size in host pixels.
inline void MakeViewRay(float tan_half_x, float tan_half_y, float width, float height, float out[4]) {
  out[0] = 2.0f * tan_half_x / width;
  out[1] = -tan_half_x;
  out[2] = -2.0f * tan_half_y / height;
  out[3] = tan_half_y;
}

// The unit quaternion of the rotation whose columns are `right`, `up` and `forward` (the camera's axes in world space): view space to world space.
inline Quat QuatFromBasis(Vec3 right, Vec3 up, Vec3 forward) {
  // the matrix M with M * (1,0,0) = right, M * (0,1,0) = up, M * (0,0,1) = forward
  const float m00 = right.x, m10 = right.y, m20 = right.z;
  const float m01 = up.x, m11 = up.y, m21 = up.z;
  const float m02 = forward.x, m12 = forward.y, m22 = forward.z;
  Quat q;
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  return q;
}

// ===========================================================================
// Part 2: the host's rules
// ===========================================================================

// ---------------------------------------------------------------------------
// The planes: what the pixel stages write, what a frame asks for, what a draw carries
// ---------------------------------------------------------------------------

// The G-buffer's pixel stage (shaderpack/variants/gbuffer*.cpp) declares its outputs once, the same for every family: SV_Target1 is RT1, SV_Target2 RT2 and
// SV_Target3 RT3, as many as kStagePlanes says. A pipeline whose pixel shader is a G-buffer word has to carry exactly those targets (a target the shader does not write
// holds undefined values when its write mask is not 0; an output the pipeline has no target for is a pipeline that does not build), so a draw that carries the sidecar
// carries ALL the stage's planes, and what a frame asks for decides only which of them hold data (GBufferConstants::mode) and which the frame publishes (valid). The
// package that makes the stage declare RT2 and RT3 sets this to 7 in the same commit. GB-W1 made the stage declare RT2 (the albedo and the lit weight: shaderpack/variants/
// gbuffer_albedo.h), so it is RT1 and RT2 (3) now; GB-W2's RT3 makes it 7.
inline constexpr uint32_t kStagePlanes = PlaneBit(kPlaneNormal) | PlaneBit(kPlaneAlbedo);
// The planes of a stage are consecutive from RT1 (the outputs' numbers are the planes'): 1, 3 or 7.
constexpr bool IsPlanePrefix(uint32_t planes) { return planes != 0 && planes <= kAllPlanes && (planes & (planes + 1)) == 0; }
static_assert(IsPlanePrefix(kStagePlanes), "the stage's planes are RT1, then RT2, then RT3");

// The colour slot of a plane in the pipeline key and in the framebuffer: the guest's own colour is slot 0 (a draw that carries the sidecar has exactly one guest
// colour target), the planes follow in their order.
constexpr uint32_t ColourSlotOfPlane(Plane plane) { return 1u + uint32_t(plane); }
inline constexpr uint32_t kMaxColourSlots = 1u + kPlaneCount;  // the guest's colour and every plane: the key's four slots (draw_state.h kColorTargetCount)

constexpr uint32_t PlaneCount(uint32_t planes) { return uint32_t(std::popcount(planes & kAllPlanes)); }

// What a frame's request becomes. `requested`: the planes the setting and the consumers ask for (PlanesFor). Written: the asked planes the stage can write, which
// the frame publishes as valid. Attached: all the stage's planes when anything is written, else none (no sidecar is made).
constexpr uint32_t WrittenPlanes(uint32_t requested, uint32_t stage_planes = kStagePlanes) { return requested & stage_planes & kAllPlanes; }
constexpr uint32_t AttachedPlanes(uint32_t requested, uint32_t stage_planes = kStagePlanes) {
  return WrittenPlanes(requested, stage_planes) != 0 ? (stage_planes & kAllPlanes) : 0u;
}

// The colour channels a plane's format has, as a write mask (R = 1, G = 2, B = 4, A = 8).
constexpr uint32_t ChannelMaskOf(PlaneFormat format) {
  switch (format) {
    case PlaneFormat::kRg16Unorm: return 0x3;
    case PlaneFormat::kRgba8Unorm: return 0xF;
    case PlaneFormat::kRg8Unorm: return 0x3;
  }
  return 0;
}
// The channels a draw's pixel shader writes in the planes of `planes` (the union): where the backend has one blend state for every colour target (Plume's D3D12
// back end today), the draw's own RT0 write mask is the sidecar's too and has to contain them.
constexpr uint32_t ChannelsNeeded(uint32_t planes) {
  uint32_t mask = 0;
  for (uint32_t p = 0; p < kPlaneCount; ++p) {
    if ((planes & (1u << p)) != 0) mask |= ChannelMaskOf(kPlaneFormats[p]);
  }
  return mask;
}

// ---------------------------------------------------------------------------
// MSAA and memory
// ---------------------------------------------------------------------------

// The sidecar takes the main colour target's host samples (a framebuffer cannot mix them): 1, 2, 4 or 8. Under a temporal upscaler the level is 1 (msaa_level.h).
constexpr uint32_t SidecarSamples(uint32_t host_samples) {
  if (host_samples >= 8) return 8;
  if (host_samples >= 4) return 4;
  if (host_samples >= 2) return 2;
  return 1;
}
constexpr uint64_t PlaneBytes(Plane plane, uint32_t width, uint32_t height, uint32_t host_samples) {
  return uint64_t(width) * height * BytesPerSample(kPlaneFormats[plane]) * SidecarSamples(host_samples);
}
constexpr uint64_t SidecarBytes(uint32_t planes, uint32_t width, uint32_t height, uint32_t host_samples) {
  uint64_t bytes = 0;
  for (uint32_t p = 0; p < kPlaneCount; ++p) {
    if ((planes & (1u << p)) != 0) bytes += PlaneBytes(Plane(p), width, height, host_samples);
  }
  return bytes;
}
// The most video memory the sidecar may hold. The survey's figures (3.6) at 4K: normal only 33 / 66 / 133 MB at 1x / 2x / 4x, all three planes 83 / 166 / 332 MB;
// 8x makes 266 MB for the normal alone and 663 MB for all. Past this the frame makes no sidecar at all (the consumers use the depth) and the log says it once.
inline constexpr uint64_t kMaxSidecarBytes = 512ull << 20;
constexpr bool SidecarAffordable(uint32_t planes, uint32_t width, uint32_t height, uint32_t host_samples) {
  return SidecarBytes(planes, width, height, host_samples) <= kMaxSidecarBytes;
}

// ---------------------------------------------------------------------------
// What the backend can do: a blend state and write mask of its own per colour target
// ---------------------------------------------------------------------------

// A draw of the main view that does NOT write the sidecar (a blended draw, one whose shader has no variant yet) can only bind it harmlessly when each colour
// target has a write mask of its own: Vulkan has it (Plume enables every feature the device has, among them independentBlend, and fills colorWriteMask and the blend
// state per attachment). Plume's D3D12 back end builds the pipeline state from RenderTarget[0] alone (IndependentBlendEnable is never set, plume_d3d12.cpp), so there
// RT1 would take RT0's mask and blend, and such a draw, whose pixel shader writes no SV_Target1, would put undefined values into the normal plane. The fix is Plume
// 5d60b70 (fix/d3d12-independent-blend, its own PR to mzzvxm/plume); flip this when the pin has it. Until then on D3D12 only the draws that write the sidecar bind it, and
// the pixels of the others keep what an earlier draw of the window wrote there (see Why and PlaneMode: that is the one inexactness of the writers only way).
// CHECK: Vulkan's per target blend states with the clear (ZERO / ZERO) and the mask (write mask 0) on a pixel shader that writes no output at that slot, and D3D12's with the fix: not run.
inline constexpr bool kD3d12PerTargetBlend = false;
struct Caps {
  bool per_target_blend = false;
};
constexpr Caps CapsFor(bool vulkan) {
  Caps caps;
  caps.per_target_blend = vulkan || kD3d12PerTargetBlend;
  return caps;
}

// The debug-only setting mcla_gpu_gbuffer_attach: `auto` is the backend's capability (above), `writers` binds the sidecar only for the draws that write it (the
// spike's way, any backend), `all` binds it for every draw of the main view's window (the survey's design: no render pass switch, no stale pixels) and trusts the build
// to have per target blend states (D3D12: with the Plume fix). A word that is none of them counts as auto.
enum class AttachSetting : uint8_t { kAuto, kWriters, kAll };
namespace detail {
inline bool LowerEq(std::string_view word, std::string_view want) {
  if (word.size() != want.size()) return false;
  for (size_t i = 0; i < word.size(); ++i) {
    const char c = word[i];
    if ((c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) != want[i]) return false;
  }
  return true;
}
}  // namespace detail
inline AttachSetting AttachOfWord(std::string_view word) {
  if (detail::LowerEq(word, "writers")) return AttachSetting::kWriters;
  if (detail::LowerEq(word, "all")) return AttachSetting::kAll;
  return AttachSetting::kAuto;
}
// Whether the draws that do not write the sidecar bind it too (masked, or clearing what they cover).
constexpr bool MaskedDrawsAttach(AttachSetting setting, Caps caps) {
  switch (setting) {
    case AttachSetting::kAll: return true;
    case AttachSetting::kWriters: return false;
    case AttachSetting::kAuto: break;
  }
  return caps.per_target_blend;
}

// ---------------------------------------------------------------------------
// The setting, the consumers' requests, the frame's plan
// ---------------------------------------------------------------------------

// The setting mcla_gpu_gbuffer (debug-only): `off` nothing, `normals` RT1 only, `full` all three planes, `auto` what the consumers on ask for (the effects
// ask through GBuffer::Want: until one does, auto is off and no sidecar is made). A word that is none of them counts as auto.
enum class Setting : uint8_t { kAuto, kOff, kNormals, kFull };
inline Setting SettingOfWord(std::string_view word) {
  if (detail::LowerEq(word, "off")) return Setting::kOff;
  if (detail::LowerEq(word, "normals")) return Setting::kNormals;
  if (detail::LowerEq(word, "full")) return Setting::kFull;
  return Setting::kAuto;
}
// The planes the frame makes: `consumer_planes` is what the effects that are on would read (auto's answer). A `full` asks for all three and the host makes what
// the stage can write (a plane no stage writes yet is left out of GBufferFrame::valid).
inline uint32_t PlanesFor(Setting setting, uint32_t consumer_planes) {
  switch (setting) {
    case Setting::kOff: return 0;
    case Setting::kNormals: return PlaneBit(kPlaneNormal);
    case Setting::kFull: return kAllPlanes;
    case Setting::kAuto: break;
  }
  return consumer_planes & kAllPlanes;
}

// The effects that read the planes, each with the planes it wants while it is on (the GBuffer::Want calls; a want stays until it is changed: the effects' settings
// are live and the owner calls it when they change).
enum class Consumer : uint8_t {
  kAmbientOcclusion,  // ao.cpp: RT1 (exact normals); GB-E
  kBounceLight,       // the SSGI: RT1 and RT2
  kLocalLights,       // the deferred lights: RT1, RT2, RT3
  kReflections,       // SSR: RT1, RT3 (+ RT2)
  kDecals,
  kCount,
};
constexpr const char* ConsumerName(Consumer consumer) {
  switch (consumer) {
    case Consumer::kAmbientOcclusion: return "ambient occlusion";
    case Consumer::kBounceLight: return "bounce light";
    case Consumer::kLocalLights: return "local lights";
    case Consumer::kReflections: return "reflections";
    case Consumer::kDecals: return "decals";
    case Consumer::kCount: break;
  }
  return "?";
}
class Wants {
 public:
  void Set(Consumer consumer, uint32_t planes) {
    if (consumer < Consumer::kCount) planes_[size_t(consumer)] = planes & kAllPlanes;
  }
  uint32_t Of(Consumer consumer) const { return consumer < Consumer::kCount ? planes_[size_t(consumer)] : 0u; }
  uint32_t Union() const {
    uint32_t all = 0;
    for (uint32_t p : planes_) all |= p;
    return all;
  }

 private:
  std::array<uint32_t, size_t(Consumer::kCount)> planes_{};
};

// One frame's plan, made at the frame's start from the setting, the consumers and the stage.
struct FramePlan {
  uint32_t requested = 0;  // PlanesFor
  uint32_t written = 0;    // WrittenPlanes: the planes the frame will publish as valid
  uint32_t attached = 0;   // AttachedPlanes: the planes every draw that carries the sidecar has (0: no sidecar this frame)
};
inline FramePlan PlanFrame(Setting setting, uint32_t consumer_planes, uint32_t stage_planes = kStagePlanes) {
  FramePlan plan;
  plan.requested = PlanesFor(setting, consumer_planes);
  plan.written = WrittenPlanes(plan.requested, stage_planes);
  plan.attached = AttachedPlanes(plan.requested, stage_planes);
  return plan;
}

// The debug view's number from the setting (an out of range value is off).
inline View ViewOf(int value) { return (value > 0 && value < int(View::kCount)) ? View(value) : View::kOff; }

// ---------------------------------------------------------------------------
// The views that never write
// ---------------------------------------------------------------------------

// The pause menu's, the race pause's, the garage's and the map's world is the main view's identity at 4 guest samples: Decide says kPauseView (the sidecar at 4 samples
// would be 133 MB at 4K for the normal alone, and the consumers have the depth fallback there). The side views (the sun's cascades, the environment map, the road
// reflection, the headlight shadow atlas, the car passes) are other EDRAM identities: kNotMainView. Two kinds of draw land in the main identity and are not the play
// view, and are told apart by what they have:
//   a cutscene: the same keys and the same samples; the cutscene director's camera runs (the hook of the guest's cutscene camera, GBuffer::NoteCutsceneCamera, counts
//               calls); CutsceneGate turns the count into "this frame is a cutscene's" (a frame or two late at the ends, harmless)
//   the mirror: the rear view mirror's phase draws the world again from behind into a small window of the main colour and depth (hooks/rear_view.cpp: 229 x 44 of the
//               1280 x 720 target); the draws' viewport is a small piece of the target. Their pixels are not the play view's: they are told apart by the viewport's width
// CHECK: that the rear view mirror's draws have a guest viewport this narrow (hooks/rear_view.cpp says 229 x 44 of 1280 x 720; not run with the mirror on) and that no draw of the play view does.
inline constexpr uint32_t kSubViewportDivisor = 2;
// A viewport narrower than half the target's pitch (both in host pixels) is a window, not the view.
constexpr bool IsSubViewport(uint32_t viewport_width, uint32_t target_pitch) {
  return target_pitch != 0 && viewport_width * kSubViewportDivisor < target_pitch;
}
class CutsceneGate {
 public:
  static constexpr uint32_t kHoldFrames = 2;  // frames without a call after which the cutscene is over
  // At each frame's start with the hook's counter; true while a cutscene's frames run.
  bool Update(uint64_t ticks) {
    if (ticks != last_) {
      last_ = ticks;
      quiet_ = 0;
    } else if (quiet_ < kHoldFrames) {
      ++quiet_;
    }
    return active();
  }
  bool active() const { return quiet_ < kHoldFrames; }

 private:
  uint64_t last_ = 0;
  uint32_t quiet_ = kHoldFrames;  // no cutscene until the first call
};

// ---------------------------------------------------------------------------
// The trigger
// ---------------------------------------------------------------------------

// The mid-scene depth copy of the main view where the consumers run (the AO's trigger: ao.h kTriggerDestination).
inline constexpr uint32_t kTriggerDestination = 0x078B0000;

struct TriggerFacts {
  bool depth_copy = false;          // the resolve copied an EDRAM depth target
  uint32_t main_depth_samples = 0;  // temporal::MainDepthSamples of its source (1, 2 or 4; 0 not the main depth)
  uint32_t dest_address = 0;
  bool clears_depth = false;        // the call also clears the depth after the copy
  bool inside_end_tiling = false;
  bool first_this_frame = true;     // no trigger before it in the frame
};
// Whether a resolve is the frame's trigger for the play view: the AO's judgement (ao.cpp Judge) without its rectangle rules. The pause world (4
// samples) has no sidecar, so it never triggers.
inline bool IsTrigger(const TriggerFacts& f) {
  return f.depth_copy && (f.main_depth_samples == 1 || f.main_depth_samples == 2) && f.dest_address == kTriggerDestination && !f.clears_depth &&
         !f.inside_end_tiling && f.first_this_frame;
}

// ---------------------------------------------------------------------------
// What a draw does with the sidecar
// ---------------------------------------------------------------------------

// What a draw of the main view's window does to the planes. The sidecar says "this pixel is this surface"; a draw that covers a pixel with another surface, and does
// not write the planes there, would leave the earlier surface's values under it, so the draws that define a surface without writing the planes CLEAR what they cover
// (a blend state of ZERO and ZERO writes 0 whatever the pixel shader outputs) and the draws that do not define one (blended, no colour) leave it:
//   kNone    the draw does not carry the sidecar (it binds the guest's own framebuffer)
//   kWrite   the pixel shader is a G-buffer word and writes the planes: no blend, write mask 0xF
//   kClear   an opaque draw whose shader writes no planes: blend src ZERO, dst ZERO (add), write mask 0xF: the pixels it draws become "no data"
//   kMask    a draw that leaves the planes as they are: write mask 0 (blended glass, decals, particles; draws that write no colour)
enum class PlaneMode : uint8_t { kNone, kWrite, kClear, kMask };

// The blend a plane's colour slot gets for a mode.
struct PlaneBlend {
  bool zero = false;  // blend enabled with source and destination factors ZERO (a clear)
  uint8_t write_mask = 0xF;
};
constexpr PlaneBlend BlendFor(PlaneMode mode) {
  PlaneBlend blend;
  if (mode == PlaneMode::kClear) blend.zero = true;
  if (mode == PlaneMode::kMask) blend.write_mask = 0;
  return blend;
}

// The GUEST's sample count of the pause menu's, the race pause's, the garage's and the map's world (temporal::kPauseSamples). It is the guest's, not the host's: the play view has 1 or 2
// guest samples whatever the host's MSAA level is (mcla_gpu_msaa 4 or 8 gives the same play view that many HOST samples, and the sidecar the same), so a run at host MSAA 4 is the
// play view with a 4 sample sidecar and carries it; only the 4x guest world is the pause world (GB-INTEGRATE-1 read its dx-day-m4 run, host MSAA 4, as the pause world and was wrong).
inline constexpr uint32_t kPauseGuestSamples = 4;
constexpr bool IsPauseWorld(uint32_t guest_samples) { return guest_samples == kPauseGuestSamples; }

// Why a main view draw did what it did (the counts of the log).
enum class Why : uint8_t {
  kWrites,              // carried, and the pixel shader's variant writes the planes
  kMaskedBlended,       // the draw blends (glass, decals, particles) and does not define the surface: kMask
  kMaskedNoVariant,     // the pixel shader has no G-buffer variant (yet): kClear
  kMaskedOtherVariant,  // the draw took another variant and the pack has no composed word with the G-buffer's: kClear
  kMaskedNoColor,       // the draw writes no colour (RT0's write mask is 0): kMask
  kMaskedNoDepth,       // a colour draw with no depth target in the window: kClear
  kMaskedOtherDepth,    // main colour with another depth identity: kClear
  kMaskedSubView,       // the draw's viewport is a window of the target (the rear view mirror): kClear
  kMaskedNarrowMask,    // a writer that cannot write: the backend has one blend state and RT0's write mask lacks the planes' channels: not carried
  kOff,                 // the G-buffer is off this frame
  kNotMainView,         // not the main view's colour (a cascade, the UI, a reflection view)
  kPauseView,           // the pause menu's / the map's / the garage's world (4 samples): no sidecar
  kSuppressed,          // a cutscene's frame
  kNotOneColor,         // the guest draw has more than one colour target (or none)
  kWindowClosed,        // after the mid-scene depth copy, or before the frame's first clear
  kCount,
};
// The rows of the log's family picture: the draws that were in the window and did not write.
constexpr bool IsMasked(Why why) { return why >= Why::kMaskedBlended && why <= Why::kMaskedNarrowMask; }

struct DrawFacts {
  bool active = false;               // the G-buffer is on this frame
  bool suppressed = false;           // a view that never writes (a cutscene's frame)
  bool window_open = false;          // Window::Gate() said kOpen (or the clear was just made)
  uint32_t main_samples = 0;         // temporal::MatchMainView: 1, 2 or 4 samples of the main view's colour, 0 not the main view
  uint32_t guest_color_count = 0;
  bool has_depth = false;
  bool depth_is_main = true;         // the depth target is the main view's depth identity at the colour's samples (a main colour with another depth is not the view)
  bool sub_view = false;             // IsSubViewport: the draw's viewport is a window of the target (the mirror)
  bool rt0_blended = false;          // RT0's blend is enabled
  bool rt0_writes = true;            // RT0's write mask is not 0
  uint32_t rt0_write_mask = 0xF;     // RT0's write mask (for the backends with one blend state: the sidecar's channels have to be in it)
  uint32_t planes = kStagePlanes;    // the planes the frame attaches (FramePlan::attached)
  bool per_target_blend = false;     // Caps::per_target_blend
  bool masked_attach = false;        // MaskedDrawsAttach: the draws that do not write the sidecar bind it too
  uint32_t ps_variant_before = 0;    // the key's variant word before this decision: 0 the game's spec 0, 1 its alpha test one, above that a packed word
                                     // (shaderpack/variant_chain.h: the feature set is the word >> 1, bit 0 is the spec variant)
  uint32_t variant_word = 0;         // the word the draw's pixel shader takes with the G-buffer's stage on top of the features the draw already has, with the draw's
                                     // spec bit (so the alpha test twin for an alpha test draw): Scene::GBufferKey asks ShaderStore::ChooseVariant for the draw's
                                     // features plus the G-buffer's and keeps the answer when it has the G-buffer and every feature the draw had. 0: the pack has no
                                     // such word (the shader has no G-buffer stage, or none that composes with the draw's, or this build has no G-buffer variant)
};
struct DrawDecision {
  Why why = Why::kOff;
  uint32_t variant = 0;  // the key's ps_variant when kWrites
  PlaneMode mode = PlaneMode::kNone;
  bool attach() const { return mode != PlaneMode::kNone; }
  bool writes() const { return mode == PlaneMode::kWrite; }
};
inline DrawDecision Decide(const DrawFacts& f) {
  DrawDecision d;
  const auto not_carried = [&](Why why) {
    d.why = why;
    return d;
  };
  // A draw that writes colour in the window and is not a writer: carried, with `mode`, when the policy has the non writers bind the sidecar.
  const auto masked = [&](Why why, PlaneMode mode) {
    d.why = why;
    d.mode = f.masked_attach ? mode : PlaneMode::kNone;
    return d;
  };
  if (!f.active) return not_carried(Why::kOff);
  if (f.suppressed) return not_carried(Why::kSuppressed);
  if (f.main_samples == 0) return not_carried(Why::kNotMainView);
  if (IsPauseWorld(f.main_samples)) return not_carried(Why::kPauseView);
  if (f.guest_color_count != 1) return not_carried(Why::kNotOneColor);
  if (!f.window_open) return not_carried(Why::kWindowClosed);
  if (!f.rt0_writes) return masked(Why::kMaskedNoColor, PlaneMode::kMask);
  if (f.rt0_blended) return masked(Why::kMaskedBlended, PlaneMode::kMask);
  if (!f.has_depth) return masked(Why::kMaskedNoDepth, PlaneMode::kClear);
  if (!f.depth_is_main) return masked(Why::kMaskedOtherDepth, PlaneMode::kClear);
  if (f.sub_view) return masked(Why::kMaskedSubView, PlaneMode::kClear);
  if (f.variant_word == 0) {
    // No word: either the draw already has a feature the G-buffer's stage does not compose with on this shader, or the shader has no stage.
    return masked((f.ps_variant_before >> 1) != 0 ? Why::kMaskedOtherVariant : Why::kMaskedNoVariant, PlaneMode::kClear);
  }
  if (!f.per_target_blend) {
    const uint32_t need = ChannelsNeeded(f.planes);
    if ((f.rt0_write_mask & need) != need) {
      return not_carried(Why::kMaskedNarrowMask);  // RT0's mask is the sidecar's too here, and lacks channels the shader writes
    }
  }
  d.why = Why::kWrites;
  d.variant = f.variant_word;
  d.mode = PlaneMode::kWrite;
  return d;
}

// The mode bits (gbuffer_types.h kMode*) of a writer's GBufferConstants: the planes it writes, the permission for the derivative normal when the frame has a camera
// (view_ray and camera_to_world are filled: a family whose shader has no normal of its own reads it; without the bit it writes "no data"), and the two sided flag.
constexpr uint32_t ModeBits(uint32_t written_planes, bool camera_valid, bool two_sided) {
  uint32_t mode = 0;
  if ((written_planes & PlaneBit(kPlaneNormal)) != 0) mode |= kModeWriteNormal;
  if ((written_planes & PlaneBit(kPlaneAlbedo)) != 0) mode |= kModeWriteAlbedo;
  if ((written_planes & PlaneBit(kPlaneMaterial)) != 0) mode |= kModeWriteMaterial;
  if (two_sided) mode |= kModeFlipBackface;
  if (camera_valid) mode |= kModeDerivativeNormal;
  return mode;
}
// The camera rows of the constants block (GBufferConstants::view_ray and camera_to_world: what GbWorldPoint reads) from the camera's axes in world space (the rows of its view
// inverse: right, up and forward = the direction it looks along), the tangents of the half field of view and the target's size in host pixels. GbQuatRotate can only turn,
// so the view space it assumes (x right, y up, z forward) has to be a rotation of the world's: that is so when right . (up x forward) is +1 (a left handed world, Direct3D's
// own), and when it is -1 (a right handed world whose camera looks along minus z: the game's view matrices) the x axis is mirrored on both sides, in the rotation (right
// is negated) and in the ray (x and y are), which together give the same world point. False when the axes are not a camera's (not unit length, not square).
inline bool MakeCameraRows(Vec3 right, Vec3 up, Vec3 forward, float tan_half_x, float tan_half_y, float width, float height, float view_ray[4], float camera_to_world[4]) {
  const auto unit = [](Vec3 v) {
    const float l = Length(v);
    return l > 0.99f && l < 1.01f;
  };
  if (!unit(right) || !unit(up) || !unit(forward) || !(tan_half_x > 0.0f) || !(tan_half_y > 0.0f) || !(width > 0.0f) || !(height > 0.0f)) return false;
  const float det = Dot(right, Cross(up, forward));
  if (std::fabs(std::fabs(det) - 1.0f) > 0.02f) return false;
  MakeViewRay(tan_half_x, tan_half_y, width, height, view_ray);
  Vec3 turned_right = right;
  if (det < 0.0f) {
    turned_right = right * -1.0f;
    view_ray[0] = -view_ray[0];
    view_ray[1] = -view_ray[1];
  }
  const Quat q = QuatFromBasis(turned_right, up, forward);
  camera_to_world[0] = q.x;
  camera_to_world[1] = q.y;
  camera_to_world[2] = q.z;
  camera_to_world[3] = q.w;
  return true;
}

// A draw of the play view: the sidecar was asked about it and it is not the pause world's, a side view's or a draw before the view's own clear. (kNotOneColor and kWindowClosed are draws
// of the main view's identity too: Decide returns them after the pause world and the side views were told apart.)
constexpr bool IsPlayViewWhy(Why why) {
  return why == Why::kWrites || IsMasked(why) || why == Why::kNotOneColor || why == Why::kWindowClosed;
}

// One frame's draws of the main view by kind, from the outcomes the Scene reports (GBuffer::NoteDraw): the pause world's draws (4 guest samples) and the play view's. A frame is
// one kind or the other; the tally says which, and whether it was neither (a frame with no 3D view) or, wrongly, both.
struct ViewTally {
  uint32_t play = 0;
  uint32_t pause = 0;
  void Note(Why why) {
    if (why == Why::kPauseView) {
      ++pause;
    } else if (IsPlayViewWhy(why)) {
      ++play;
    }
  }
  bool play_world() const { return play != 0 && pause == 0; }
  bool pause_world() const { return pause != 0 && play == 0; }
  bool mixed() const { return play != 0 && pause != 0; }
};

// The camera for the shaders that have no normal of their own (the derivative normal: store interiors, the traffic signals, the foliage fallback) and for a world point of a
// fragment (GbWorldPoint): the previous frame's, read from the game's motion blur draw (the anchor), whose pixel constants hold the view inverse (right, up, backward), the
// projection and the frame's size (temporal_camera.h). It is read by the G-buffer itself at every anchor of a frame it tracks, so it exists with or without a temporal upscaler (GB-
// INTEGRATE-1's run found the derivative families writing "no data" without one: the rows came from the upscaler's own history). One frame late: a fast pan turns the world normal a
// degree or two at most, a cut shows for one frame. Only a play frame's camera is kept: the pause world's lens differs.
struct CameraAxes {
  Vec3 right;
  Vec3 up;
  Vec3 forward;           // the direction the camera looks along (minus the view inverse's third row)
  float tan_half_x = 0.0f;
  float tan_half_y = 0.0f;
  uint32_t width = 0;     // the frame's size in guest pixels (c62)
  uint32_t height = 0;
};
class CameraKeeper {
 public:
  // The frame's serial at its start (the Scene's guest frame count: one more each frame the renderer runs).
  void BeginFrame(uint64_t serial) { serial_ = serial; }
  // At the frame's anchor: `camera` is what its constants gave (null: nothing sane), `tally` the frame's draws of the main view so far. The camera is kept for the next frame when
  // this frame is the play view's; any other frame (pause world, no main view, both) leaves the next frame with none.
  void OnAnchor(const CameraAxes* camera, const ViewTally& tally) {
    if (camera == nullptr || !tally.play_world()) {
      have_ = false;
      return;
    }
    camera_ = *camera;
    kept_serial_ = serial_;
    have_ = true;
  }
  // The camera the draws of this frame use: the last frame's, when it gave one and this frame follows it directly (a gap of frames, a cutscene's, the pause world's: none).
  const CameraAxes* Previous() const { return have_ && kept_serial_ + 1 == serial_ ? &camera_ : nullptr; }

 private:
  uint64_t serial_ = 0;
  uint64_t kept_serial_ = 0;
  bool have_ = false;
  CameraAxes camera_;
};

// The material word of a draw that has no better one (GB-F's tables come later): world class, smoothness 0, lit by the forward lights.
inline uint32_t DefaultMaterialWord() { return MaterialWord(0.0f, SurfaceClass::kWorld, 0u, 1.0f); }

// The frame's window (see the top of the file). One per renderer; the Scene calls the events in the frame's order.
class Window {
 public:
  enum class Gate : uint8_t { kOpen, kNeedsClear, kClosed };

  void BeginFrame() {
    open_ = false;
    cleared_ = false;
    triggered_ = false;
    clears_ = 0;
    partial_clears_ = 0;
  }
  // The guest cleared the main view's colour: `whole` when the rectangle is the whole target.
  void OnClear(bool whole) {
    if (triggered_) {
      return;  // a clear after the trigger belongs to the next frame's start or to something the consumers are done with: the window stays closed
    }
    open_ = true;
    cleared_ = true;
    ++clears_;
    if (!whole) ++partial_clears_;
  }
  // The sidecar had to be cleared on the first draw (no clear came first): it is whole.
  void OnInitialClear() { OnClear(true); }
  void OnTrigger() {
    open_ = false;
    triggered_ = true;
  }
  // What a main view colour draw finds. kNeedsClear: no clear has opened the window and the trigger has not passed.
  Gate GateForDraw() const {
    if (open_) return Gate::kOpen;
    if (!cleared_ && !triggered_) return Gate::kNeedsClear;
    return Gate::kClosed;
  }
  bool open() const { return open_; }
  bool triggered() const { return triggered_; }
  bool had_clear() const { return cleared_; }
  uint32_t clears() const { return clears_; }
  uint32_t partial_clears() const { return partial_clears_; }

 private:
  bool open_ = false;
  bool cleared_ = false;
  bool triggered_ = false;
  uint32_t clears_ = 0;
  uint32_t partial_clears_ = 0;
};

}  // namespace mcla::gpu::gbuffer
