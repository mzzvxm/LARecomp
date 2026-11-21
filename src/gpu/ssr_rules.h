#pragma once
// ===========================================================================
// MCLA GPU renderer -- screen-space reflections: the host's pure rules (no GPU), package GB-I1 of perf/LIGHT-UNIFY.md 11.2
// ===========================================================================
// ssr.cpp asks here what a frame does (whether the reflections run and why not), how big the images are and what they cost, which numbers the passes get, what the road's wetness is,
// how the push block is filled; ssr_test.cpp checks it all and compiles hlsl/ssr_common.hlsli and hlsl/ssr_trace.hlsli as C++ against analytic scenes. The numbers of the push block and
// of the flags are the ones of hlsl/ssr_common.hlsli (the test compares them).
//
// THE EFFECT (ssr.h has the long text): a Hi-Z ray march of the shared depth at HALF the picture's size, one GGX-sampled ray a pixel (two at ultra), the colour of what it hits read from
// the shared colour_post with a mip chosen by the lobe's width, a temporal accumulation by the VIRTUAL IMAGE's reprojection, then an additive resolve into the main colour at the picture's
// size, per pixel by the G-buffer's class and smoothness. Cars' paint and chrome keep their own reflections; what the rays miss (and what is near the picture's edge) is the game's own.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "screen_frame.h"

namespace mcla::gpu::ssr {

// ---------------------------------------------------------------------------
// the levels (mcla_gpu_ssr)
// ---------------------------------------------------------------------------

enum class Level : uint8_t { kOff, kHigh, kUltra };
inline constexpr uint32_t kLevelCount = 3;

constexpr std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kOff: return "off";
    case Level::kHigh: return "high";
    case Level::kUltra: return "ultra";
  }
  return "?";
}
// A text that is none of the words (a hand-edited file) is off: the effect is new and off by default.
inline Level LevelFromText(std::string_view text) {
  std::string lower;
  for (char c : text) lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "high") return Level::kHigh;
  if (lower == "ultra") return Level::kUltra;
  return Level::kOff;
}

// What a level is: rays a pixel, the Hi-Z walk's iterations, and the weight the history keeps (the share of the last frames in the picture; 0.9 is a mean of about ten).
struct LevelNumbers {
  uint32_t rays;
  uint32_t max_iterations;
  float history_weight;
};
inline constexpr LevelNumbers NumbersOf(Level level) {
  switch (level) {
    case Level::kHigh: return {1, 40, 0.90f};
    case Level::kUltra: return {2, 64, 0.92f};
    case Level::kOff: break;
  }
  return {0, 0, 0.0f};
}
inline constexpr uint32_t kMaxRays = 2;
inline constexpr uint32_t kMaxIterations = 255;   // the push's 8 bits

// The numbers no setting names (hlsl/ssr_common.hlsli and ssr_trace.hlsli have the rest as constants); the debug settings replace the ones that have one.
inline constexpr float kDefaultMaxDistance = 150.0f;   // the longest ray, world units (metres)
inline constexpr float kDefaultThickness = 0.04f;      // a surface's thickness as a share of its depth (and never under 0.3 m: the shader's)
inline constexpr float kDefaultMinSmooth = 0.40f;      // pixels rougher than this are not reflected: a dry road (0.30) is not, a wet one (0.92) is
inline constexpr float kDefaultStubSmooth = 0.85f;
inline constexpr float kDefaultStrength = 1.0f;
// The brightest a reflection may carry, in multiples of the exposure factor of the main colour target (2^exp_bias: 16 in the game): the colour is in the target's own units, a scene white
// is about that factor, so this many times a white wall. A neon or a lamp seen in a mirror is bright but the tone map clips at one white.
inline constexpr float kCapSceneWhites = 4.0f;
// The colour target's exposure factor when the camera's is not known.
inline constexpr float kDefaultExposure = 16.0f;

// ---------------------------------------------------------------------------
// sizes and memory
// ---------------------------------------------------------------------------

// The traced size (the Hi-Z's level 0) is the picture's size or half of it (rounded up) (ao_size.h); the SSR images are always half of the picture's: the Hi-Z level that has that size is 0 when
// the traced size is the half already and 1 when it is the whole picture.
struct SsrSize {
  bool valid = false;
  uint32_t size[2] = {0, 0};   // the SSR images' size
  uint32_t hz = 0;             // the Hi-Z level whose size that is (0 or 1)
};
constexpr uint32_t HalfUp(uint32_t n) { return (n + 1) / 2; }
constexpr SsrSize SizeFor(uint32_t render_w, uint32_t render_h, uint32_t traced_w, uint32_t traced_h) {
  SsrSize s;
  if (render_w == 0 || render_h == 0) {
    return s;
  }
  const uint32_t hw = HalfUp(render_w);
  const uint32_t hh = HalfUp(render_h);
  if (traced_w == render_w && traced_h == render_h) {
    s.hz = 1;
  } else if (traced_w == hw && traced_h == hh) {
    s.hz = 0;
  } else {
    return s;   // a traced size that is neither: not a size this pass knows (the AO's rule changed)
  }
  s.valid = true;
  s.size[0] = hw;
  s.size[1] = hh;
  return s;
}
// colour_post's size by the height rule (screen_rules.h ColourPostIsHalf): the picture's up to 1080 lines, half above.
constexpr bool ColourIsHalf(uint32_t render_h) { return render_h > 1080; }

inline constexpr uint32_t kBytesRgba16f = 8;
// The images the effect holds: the trace and the two accumulation images (the history ping-pongs), RGBA16F at the SSR size.
constexpr uint64_t ImageBytes(uint32_t w, uint32_t h) { return uint64_t(w) * h * kBytesRgba16f; }
constexpr uint64_t HeldBytes(uint32_t w, uint32_t h) { return 3 * ImageBytes(w, h); }

// How many Hi-Z levels the walk reads (the push has six slots) and how many colour levels (four slots).
inline constexpr uint32_t kHizSlots = 6;
inline constexpr uint32_t kColourSlots = 4;
constexpr uint32_t HizLevelsUsed(uint32_t hiz_levels) { return hiz_levels < kHizSlots ? hiz_levels : kHizSlots; }
constexpr uint32_t ColourLevelsUsed(uint32_t colour_levels) { return colour_levels < kColourSlots ? colour_levels : kColourSlots; }

// ---------------------------------------------------------------------------
// the wetness
// ---------------------------------------------------------------------------

// The road's wetness as the game says it: VS c43.x (gReflectionParams.x), 0.095 dry, 1.17 in rain (ao.cpp, census 6A.2). The wet number of the passes is 0 up to kWetDry and 1 from
// kWetDry + kWetSpan (the AO's contact gate has the same two).
inline constexpr float kWetDry = 0.2f;
inline constexpr float kWetSpan = 0.5f;
inline float Wet01(float c43x) {
  if (!std::isfinite(c43x)) return 0.0f;
  return std::clamp((c43x - kWetDry) / kWetSpan, 0.0f, 1.0f);
}

// The frame's wetness: the commonest value of VS c43.x among the main view's colour draws before the trigger, in bins of 0.1 up to 4.0 (the AO's PickWet), and a smoothing across frames so
// that the start of rain is a ramp of about a second and not a step.
class WetVotes {
 public:
  void Reset() {
    bins_.fill(0);
    draws_ = 0;
  }
  void Add(float c43x) {
    if (std::isfinite(c43x) && c43x >= 0.0f && c43x <= 4.0f) {
      ++bins_[uint32_t(c43x * 10.0f + 0.5f)];
      ++draws_;
    }
  }
  bool Pick(float* value) const {
    if (draws_ == 0) return false;
    uint32_t best = 0;
    for (uint32_t i = 1; i < kBins; ++i) {
      if (bins_[i] > bins_[best]) best = i;
    }
    *value = 0.1f * float(best);
    return true;
  }
  uint32_t draws() const { return draws_; }
  static constexpr uint32_t kBins = 41;

 private:
  std::array<uint16_t, kBins> bins_{};
  uint32_t draws_ = 0;
};
inline constexpr float kWetSmoothing = 0.08f;   // the share of the way to the frame's value taken each frame
inline float SmoothWet(float previous, float target) { return previous + (target - previous) * kWetSmoothing; }

// ---------------------------------------------------------------------------
// the frame's decision
// ---------------------------------------------------------------------------

// Why the reflections do or do not run in a frame (the log says it when it changes).
enum class Why : uint8_t {
  kUsed,          // runs
  kOff,           // the setting is off
  kNoShaders,     // a shader or a pipeline is not made (yet: a frame never waits for a compile) or could not be
  kPause,         // the pause menu's, the garage's or the map's world: no sidecar, no history
  kNoHiz,         // the frame has no Hi-Z (the depth chain did not run, its pipeline is still being made)
  kNoColour,      // the frame has no colour_post with its mips
  kNoNormals,     // the G-buffer's normal plane is not in the frame (a cutscene, the first frame, the sidecar over the memory cap)
  kNoLens,        // the frame has no camera lens (the shared frame's camera is empty: the first frames, a cut)
  kSizes,         // the traced size is neither the picture's nor half of it, or the planes' size is not the picture's
  kSamples,       // the planes' host samples differ from the depth's (a frame that mixes two targets)
  kNoDepth,       // the guest's depth plane could not be read as a texture
  kCount,
};
constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kUsed: return "runs";
    case Why::kOff: return "off";
    case Why::kNoShaders: return "a shader or pipeline of the effect is not ready";
    case Why::kPause: return "the pause menu's, the garage's or the map's world";
    case Why::kNoHiz: return "the frame has no Hi-Z";
    case Why::kNoColour: return "the frame has no colour_post with its mip levels";
    case Why::kNoNormals: return "the G-buffer's normal plane is not in the frame";
    case Why::kNoLens: return "the frame has no camera lens";
    case Why::kSizes: return "the sizes do not fit (traced size, planes' size)";
    case Why::kSamples: return "the planes' host samples differ from the depth's";
    case Why::kNoDepth: return "the depth plane could not be read";
    case Why::kCount: break;
  }
  return "?";
}

struct FrameFacts {
  Level level = Level::kOff;
  bool shaders_ready = false;
  bool pause_view = false;
  bool has_lens = false;
  bool has_hiz = false;
  uint32_t hiz_levels = 0;
  bool has_colour = false;
  uint32_t colour_levels = 0;
  bool has_rt1 = false;
  bool has_rt2 = false;
  bool has_rt3 = false;
  bool depth_slot_ok = false;
  uint32_t render[2] = {0, 0};
  uint32_t traced[2] = {0, 0};
  uint32_t plane_size[2] = {0, 0};     // the G-buffer's planes (the colour target's size, at or above the picture's)
  uint32_t depth_samples = 1;
  uint32_t plane_samples = 1;
  uint32_t depth_sample0 = 0;
  uint32_t plane_sample0 = 0;
};
struct Decision {
  Why why = Why::kOff;
  SsrSize size;
  bool stub = false;            // no RT3: the stub class (hlsl SsrStubGain)
};
inline Decision Decide(const FrameFacts& f) {
  Decision d;
  if (f.level == Level::kOff) {
    d.why = Why::kOff;
    return d;
  }
  if (!f.shaders_ready) {
    d.why = Why::kNoShaders;
    return d;
  }
  if (f.pause_view) {
    d.why = Why::kPause;
    return d;
  }
  if (!f.has_rt1) {
    d.why = Why::kNoNormals;
    return d;
  }
  if (!f.has_lens) {
    d.why = Why::kNoLens;
    return d;
  }
  if (!f.has_hiz || f.hiz_levels == 0) {
    d.why = Why::kNoHiz;
    return d;
  }
  if (!f.has_colour || f.colour_levels == 0) {
    d.why = Why::kNoColour;
    return d;
  }
  d.size = SizeFor(f.render[0], f.render[1], f.traced[0], f.traced[1]);
  if (!d.size.valid || f.plane_size[0] < f.render[0] || f.plane_size[1] < f.render[1]) {
    d.why = Why::kSizes;
    return d;
  }
  if (f.plane_samples != f.depth_samples || f.plane_sample0 != f.depth_sample0) {
    d.why = Why::kSamples;
    return d;
  }
  if (!f.depth_slot_ok) {
    d.why = Why::kNoDepth;
    return d;
  }
  d.why = Why::kUsed;
  d.stub = !f.has_rt3;
  return d;
}

// The history of the temporal pass is usable when the last frame ran the effect at the frame before, in the same kind of view and at the same size, and the camera did not cut.
struct HistoryKey {
  bool valid = false;
  uint64_t frame = 0;
  uint32_t size[2] = {0, 0};
  uint32_t generation = 0;   // the products' generation (screen_frame.h): the Hi-Z, the colour and the planes are the same images as at the last frame
};
constexpr bool HistoryUsable(const HistoryKey& last, const HistoryKey& now, bool camera_motion_known) {
  return camera_motion_known && last.valid && now.valid && last.frame + 1 == now.frame && last.size[0] == now.size[0] && last.size[1] == now.size[1];
}

// ---------------------------------------------------------------------------
// the push block (hlsl/ssr_common.hlsli SSR_W_*)
// ---------------------------------------------------------------------------

namespace push {
inline constexpr uint32_t kWords = 32;
inline constexpr uint32_t kDepthRt1 = 0;
inline constexpr uint32_t kRt2Rt3 = 1;
inline constexpr uint32_t kSrc = 2;
inline constexpr uint32_t kHiz01 = 3;
inline constexpr uint32_t kHiz23 = 4;
inline constexpr uint32_t kHiz45 = 5;
inline constexpr uint32_t kCol01 = 6;
inline constexpr uint32_t kCol23 = 7;
inline constexpr uint32_t kRender = 8;
inline constexpr uint32_t kFlags = 9;
inline constexpr uint32_t kFrame = 10;
inline constexpr uint32_t kLin0 = 11;
inline constexpr uint32_t kTanX = 12;
inline constexpr uint32_t kTanY = 13;
inline constexpr uint32_t kLinX = 14;
inline constexpr uint32_t kLinY = 15;
inline constexpr uint32_t kLinZ = 16;
inline constexpr uint32_t kLinW = 17;
inline constexpr uint32_t kNear = 18;
inline constexpr uint32_t kMaxDist = 19;
inline constexpr uint32_t kRight = 20;
inline constexpr uint32_t kUp = 23;
inline constexpr uint32_t kStrength = 26;
inline constexpr uint32_t kMinSmooth = 27;
inline constexpr uint32_t kStubSmooth = 28;
inline constexpr uint32_t kWet = 29;
inline constexpr uint32_t kThick = 30;
inline constexpr uint32_t kCap = 31;
// The temporal pass's own use of words: the last frame's lens in the near and distance words, the history's weight in the frame word, and the motion's three rows (12 floats) from the
// right axis on.
inline constexpr uint32_t kPrevTanX = kNear;
inline constexpr uint32_t kPrevTanY = kMaxDist;
inline constexpr uint32_t kHistoryWeight = kFrame;
inline constexpr uint32_t kMotion = 20;

// The flag bits of the flags word, above the 12 low bits (sample0, the Hi-Z's and the colour's levels).
inline constexpr uint32_t kFlagMs = 1;
inline constexpr uint32_t kFlagHz = 2;
inline constexpr uint32_t kFlagColourHalf = 4;
inline constexpr uint32_t kFlagStub = 8;
inline constexpr uint32_t kFlagHistory = 16;
inline constexpr uint32_t kFlagCamera = 32;
inline constexpr uint32_t kFlagShift = 12;
inline constexpr uint32_t kRaysShift = 18;
inline constexpr uint32_t kViewShift = 20;
inline constexpr uint32_t kIterShift = 24;
inline constexpr uint32_t kNoSlot16 = 0xFFFFu;
}  // namespace push

struct Push {
  std::array<uint32_t, push::kWords> w{};
  void Set(uint32_t word, uint32_t value) { w[word] = value; }
  void SetFloat(uint32_t word, float value) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    w[word] = bits;
  }
  float Float(uint32_t word) const {
    float value;
    std::memcpy(&value, &w[word], sizeof(value));
    return value;
  }
};
static_assert(sizeof(Push) == 128, "the passes' push range is 128 bytes");

// A bindless slot in 16 bits: kNoSlot (screen_frame.h, 0xFFFFFFFF) and anything that does not fit are 0xFFFF (none).
constexpr uint32_t Slot16(uint32_t slot) { return slot < push::kNoSlot16 ? slot : push::kNoSlot16; }
constexpr uint32_t PackSlots(uint32_t lo, uint32_t hi) { return Slot16(lo) | (Slot16(hi) << 16); }
// A size (a picture's width and height, below 65536) in a word, the same two halves.
constexpr uint32_t PackSize(uint32_t width, uint32_t height) { return (width & 0xFFFFu) | (height << 16); }
constexpr uint32_t UnpackLo(uint32_t word) { return word & 0xFFFFu; }
constexpr uint32_t UnpackHi(uint32_t word) { return word >> 16; }

struct FlagsIn {
  uint32_t sample0 = 0;
  uint32_t hiz_levels = 0;
  uint32_t colour_levels = 0;
  bool ms = false;
  bool hz1 = false;
  bool colour_half = false;
  bool stub = false;
  bool history = false;
  bool camera = false;
  uint32_t rays = 1;
  uint32_t view = 0;
  uint32_t max_iterations = 0;
};
constexpr uint32_t PackFlags(const FlagsIn& f) {
  uint32_t bits = (f.ms ? push::kFlagMs : 0u) | (f.hz1 ? push::kFlagHz : 0u) | (f.colour_half ? push::kFlagColourHalf : 0u) | (f.stub ? push::kFlagStub : 0u) |
                  (f.history ? push::kFlagHistory : 0u) | (f.camera ? push::kFlagCamera : 0u);
  const uint32_t rays = f.rays < 1 ? 1 : (f.rays > kMaxRays ? kMaxRays : f.rays);
  return (f.sample0 & 15u) | ((HizLevelsUsed(f.hiz_levels) & 15u) << 4) | ((ColourLevelsUsed(f.colour_levels) & 15u) << 8) | (bits << push::kFlagShift) | ((rays - 1) << push::kRaysShift) |
         ((f.view & 15u) << push::kViewShift) | ((f.max_iterations > kMaxIterations ? kMaxIterations : f.max_iterations) << push::kIterShift);
}

// ---------------------------------------------------------------------------
// the camera's rows (turning the G-buffer's WORLD normal into view space)
// ---------------------------------------------------------------------------

// The right and up axes of the camera in world space. The third (the forward one) is not pushed: forward = up x right for the game's right handed world (the view space of the game looks down -z,
// right x up = back), which the host checks (RowsOk). `ao_gbuffer::Rows` x, y, z are right, up and forward (hlsl/ao_gbuffer_normal.hlsli).
struct CameraAxes {
  bool ok = false;
  float right[3] = {1, 0, 0};
  float up[3] = {0, 1, 0};
};
inline CameraAxes AxesFromRows(const float x[3], const float y[3], const float z[3]) {
  CameraAxes a;
  for (int i = 0; i < 3; ++i) {
    a.right[i] = x[i];
    a.up[i] = y[i];
  }
  // up x right must be the forward row (a right handed camera): the test of a matrix that is a rotation and not a mirror.
  const float fx = y[1] * x[2] - y[2] * x[1];
  const float fy = y[2] * x[0] - y[0] * x[2];
  const float fz = y[0] * x[1] - y[1] * x[0];
  const float dot = fx * z[0] + fy * z[1] + fz * z[2];
  const auto len = [](const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
  a.ok = dot > 0.98f && len(x) > 0.98f && len(x) < 1.02f && len(y) > 0.98f && len(y) < 1.02f && len(z) > 0.98f && len(z) < 1.02f;
  return a;
}

// Whether two cameras (the 4x4 view-projections of screen_frame.h, 16 floats) are the same: the largest difference within `tolerance` of the larger entry (a camera that does not move).
inline bool SameCamera(const float a[16], const float b[16], float tolerance = 1.0e-4f) {
  float largest = 1.0f;
  float worst = 0.0f;
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false;
    largest = std::max(largest, std::max(std::fabs(a[i]), std::fabs(b[i])));
    worst = std::max(worst, std::fabs(a[i] - b[i]));
  }
  return worst <= tolerance * largest;
}

// The cap a reflection's colour is held to, in the target's own units.
constexpr float CapFor(float exposure_factor) { return kCapSceneWhites * (exposure_factor > 0.0f ? exposure_factor : kDefaultExposure); }

// The debug views of mcla_gpu_ssr_view (the apply pass draws one in place of its add): 0 off.
enum class View : uint8_t {
  kOff = 0,
  kTerm = 1,       // the colour the effect adds, alone on black
  kGate = 2,       // the class gate: green (a pixel the effect reflects), red (its class keeps its own reflection), blue (stub pixel), black (no data), brighter with the share
  kTrace = 3,      // the raw trace at the SSR size, before the history (premultiplied radiance)
  kHit = 4,        // the trace's alpha: hit distance in blue to white (a log scale), red for a miss, black where nothing was traced
  kCount = 5,
};
inline View ViewFromSetting(int value) { return value > 0 && value < int(View::kCount) ? View(value) : View::kOff; }

}  // namespace mcla::gpu::ssr
