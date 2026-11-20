#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's rules: the C++ twin of hlsl/gbuffer_common.hlsli, and the host's pure decisions
// ===========================================================================
// Part 1 (the twin, FROZEN with the .hlsli: package IF of perf/GBUFFER-SURVEY.md 7.2): the encodings of the three planes and of the material word,
// the derivative normal's arithmetic, in plain C++ so that a test can run them and so that a CPU consumer (a unit test, the debug statistics) reads a
// plane the way a shader does. gbuffer_rules_test.cpp checks the twin on its own (round trips, the 16 bit error, the "no data" code), compiles the
// .hlsli as C++ and compares it with this, and compares every GB_* define with gbuffer_types.h.
//
// Part 2 (the host's rules, the spike's: gbuffer.cpp and Scene call them): which draws of the frame get the sidecar and which of them write it, the
// frame window the sidecar lives in (cleared with the guest's clear of the main colour, closed at the mid-scene depth copy), the trigger, and the
// setting's words. Pure: no Plume, no renderer, so they are tested alone.
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

// The setting mcla_gpu_gbuffer (debug-only): `off` nothing, `normals` RT1 only, `full` all three planes, `auto` what the consumers on ask for (no
// consumer reads the planes yet: auto is off until GB-E and the later packages say otherwise). A word that is none of them counts as auto.
enum class Setting : uint8_t { kAuto, kOff, kNormals, kFull };
inline Setting SettingOfWord(std::string_view word) {
  const auto lower_eq = [&](std::string_view want) {
    if (word.size() != want.size()) return false;
    for (size_t i = 0; i < word.size(); ++i) {
      const char c = word[i];
      if ((c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) != want[i]) return false;
    }
    return true;
  };
  if (lower_eq("off")) return Setting::kOff;
  if (lower_eq("normals")) return Setting::kNormals;
  if (lower_eq("full")) return Setting::kFull;
  return Setting::kAuto;
}
// The planes the frame makes: `consumer_planes` is what the effects that are on would read (auto's answer). Phase 1 knows the normal plane only; a `full`
// asks for all three and the host makes what it can (a plane no variant writes yet is left out of GBufferFrame::valid).
inline uint32_t PlanesFor(Setting setting, uint32_t consumer_planes) {
  switch (setting) {
    case Setting::kOff: return 0;
    case Setting::kNormals: return PlaneBit(kPlaneNormal);
    case Setting::kFull: return kAllPlanes;
    case Setting::kAuto: break;
  }
  return consumer_planes & kAllPlanes;
}

// The debug view's number from the setting (an out of range value is off).
inline View ViewOf(int value) { return (value > 0 && value < int(View::kCount)) ? View(value) : View::kOff; }

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
// samples) has no sidecar in phase 1, so it never triggers.
inline bool IsTrigger(const TriggerFacts& f) {
  return f.depth_copy && (f.main_depth_samples == 1 || f.main_depth_samples == 2) && f.dest_address == kTriggerDestination && !f.clears_depth &&
         !f.inside_end_tiling && f.first_this_frame;
}

// Whether a draw that does NOT write the sidecar (blended, no variant yet, another variant, no colour) still binds it with write mask 0, as the survey's design has
// it (one framebuffer for the whole window, no pass switch). That needs a per colour target write mask: Vulkan has it (Plume reads the device's features and
// fills colorWriteMask per attachment), but Plume's D3D12 back end builds the pipeline state from RenderTarget[0] alone (IndependentBlendEnable is never set,
// plume_d3d12.cpp), so there RT1 would get RT0's mask and such a draw, whose pixel shader writes no SV_Target1, would put undefined values into the normal plane. Until
// Plume sets it (a one line change, its own PR) a draw that does not write the sidecar binds the guest's own framebuffer, and only the draws of the variant bind the
// sidecar: the framebuffer changes between the two kinds (a pass switch on Vulkan, a binding change on D3D12). A variant draw takes RT0's blend state for RT1 on
// D3D12: opaque, so no blend, and the write mask of a lit draw has red and green in it.
inline constexpr bool kMaskedDrawsAttach = false;

// What a draw of the main view's colour (the guest has one colour target, RT0, and a depth target) does with the sidecar.
enum class Why : uint8_t {
  kWrites,           // attached, and the pixel shader's variant writes the planes
  kMaskedBlended,    // the draw blends (glass, decals, particles) and does not define the surface (write mask 0 when kMaskedDrawsAttach)
  kMaskedNoVariant,  // the pixel shader has no G-buffer variant (yet)
  kMaskedOtherVariant,  // the draw took another variant (a shader has one)
  kMaskedNoColor,    // the draw writes no colour (RT0's write mask is 0)
  kOff,              // the G-buffer is off this frame
  kNotMainView,      // not the main view's colour (a cascade, the UI, a reflection view)
  kPauseView,        // the pause menu's / the map's world (4 samples): no sidecar in phase 1
  kNotOneColor,      // the guest draw has more than one colour target (or none)
  kNoDepth,
  kWindowClosed,     // after the mid-scene depth copy
  kCount,
};
inline constexpr bool Attached(Why why) { return why == Why::kWrites || (kMaskedDrawsAttach && why <= Why::kMaskedNoColor); }

struct DrawFacts {
  bool active = false;               // the G-buffer is on this frame
  bool window_open = false;          // Window::Gate() said kOpen (or the clear was just made)
  uint32_t main_samples = 0;         // temporal::MatchMainView: 1, 2 or 4 samples of the main view's colour, 0 not the main view
  uint32_t guest_color_count = 0;
  bool has_depth = false;
  bool rt0_blended = false;          // RT0's blend is enabled
  bool rt0_writes = true;            // RT0's write mask is not 0
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
  bool attach() const { return Attached(why); }
};
inline DrawDecision Decide(const DrawFacts& f) {
  DrawDecision d;
  if (!f.active) { d.why = Why::kOff; return d; }
  if (f.main_samples == 0) { d.why = Why::kNotMainView; return d; }
  if (f.main_samples == 4) { d.why = Why::kPauseView; return d; }
  if (f.guest_color_count != 1) { d.why = Why::kNotOneColor; return d; }
  if (!f.has_depth) { d.why = Why::kNoDepth; return d; }
  if (!f.window_open) { d.why = Why::kWindowClosed; return d; }
  if (!f.rt0_writes) { d.why = Why::kMaskedNoColor; return d; }
  if (f.rt0_blended) { d.why = Why::kMaskedBlended; return d; }
  if (f.variant_word == 0) {
    // No word: either the draw already has a feature the G-buffer's stage does not compose with on this shader (the headlight's does), or the shader has no stage.
    d.why = (f.ps_variant_before >> 1) != 0 ? Why::kMaskedOtherVariant : Why::kMaskedNoVariant;
    return d;
  }
  d.why = Why::kWrites;
  d.variant = f.variant_word;
  return d;
}

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
