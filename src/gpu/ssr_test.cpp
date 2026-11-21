// Check of the screen-space reflections (ssr_rules.h, hlsl/ssr_common.hlsli, hlsl/ssr_trace.hlsli, ssr.h / ssr.cpp's use of them). A program of its own: it needs no renderer, no ReXGlue and no
// GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssr_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssr_test [<src/gpu dir>] [--print]
//
// It checks: the levels, sizes, memory, the frame's decision and its reasons, the history's rule, the wetness (the votes, the ramp), the push block's words against the .hlsli's #defines
// and the packing; then it compiles hlsl/ssr_common.hlsli and hlsl/ssr_trace.hlsli ITSELF as C++ (through the shim of the HLSL types below) and checks the class gate (every class and flag), the
// wet road, the environment BRDF, the noise, the GGX visible-normal sampling, the fades and the cone's mip, the reprojection of the virtual image against the analytic mirror (a flat road, a
// moving camera), and the Hi-Z ray march: against analytic scenes (a road, a wall, a thin pole, the sky) and against a brute force march of the full size depth on random block scenes. Given
// the src/gpu folder it also reads the sources: the shaders' use of the push words, the registration, the setting, the order at the trigger, and that nothing is recorded without a want.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_gbuffer.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "screen_frame.h"
#include "screen_rules.h"
#include "ssr_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: the shim of what the files use (gbuffer_common.hlsli and ao_gbuffer_normal.hlsli first, as the shaders include them)
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float2 {
  float x = 0, y = 0;
  float2() = default;
  float2(float a, float b) : x(a), y(b) {}
};
struct float3 {
  float x = 0, y = 0, z = 0;
  float3() = default;
  float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float2 operator*(float2 a, float s) { return float2(a.x * s, a.y * s); }
inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float abs(float v) { return std::fabs(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
inline float3 sqrt(float3 v) { return float3(std::sqrt(v.x), std::sqrt(v.y), std::sqrt(v.z)); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float3 normalize(float3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline int clamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline float floor(float v) { return std::floor(v); }
inline float frac(float v) { return v - std::floor(v); }
inline float exp2(float v) { return std::exp2(v); }
inline float log2(float v) { return std::log2(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
// the Hi-Z the march reads (set by the test)
struct HizSource {
  int levels = 0;
  int w[8] = {}, h[8] = {};
  const float* data[8] = {};
};
HizSource g_hiz;
float SsrHizAt(uint level, int cx, int cy) {
  const int l = int(level);
  cx = std::min(std::max(cx, 0), g_hiz.w[l] - 1);
  cy = std::min(std::max(cy, 0), g_hiz.h[l] - 1);
  return g_hiz.data[l][cy * g_hiz.w[l] + cx];
}
#include "hlsl/gbuffer_common.hlsli"
#include "hlsl/ao_gbuffer_normal.hlsli"
#include "hlsl/ssr_common.hlsli"
#include "hlsl/ssr_trace.hlsli"
}  // namespace hlsl_shim

namespace sr = mcla::gpu::ssr;
namespace gb = mcla::gpu::gbuffer;
namespace sc = mcla::gpu::screen;
namespace hs = hlsl_shim;

namespace {

using uint = unsigned int;
int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[400];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

struct Lcg {
  uint32_t state = 20261121u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

// ---------------------------------------------------------------------------
// the levels, sizes, decisions, history, wetness
// ---------------------------------------------------------------------------

void TestLevels() {
  Check(sr::LevelFromText("off") == sr::Level::kOff && sr::LevelFromText("high") == sr::Level::kHigh && sr::LevelFromText("ultra") == sr::Level::kUltra, "the level words");
  Check(sr::LevelFromText("HIGH") == sr::Level::kHigh && sr::LevelFromText("") == sr::Level::kOff && sr::LevelFromText("low") == sr::Level::kOff && sr::LevelFromText("auto") == sr::Level::kOff,
        "a word that is none of them is off (the effect is new and off by default)");
  Check(sr::LevelName(sr::Level::kUltra) == "ultra" && sr::LevelName(sr::Level::kOff) == "off", "the names");
  const sr::LevelNumbers high = sr::NumbersOf(sr::Level::kHigh);
  const sr::LevelNumbers ultra = sr::NumbersOf(sr::Level::kUltra);
  Check(high.rays == 1 && ultra.rays == 2 && high.max_iterations < ultra.max_iterations && high.history_weight < ultra.history_weight, "ultra costs more: two rays, more steps, a longer history");
  Check(sr::NumbersOf(sr::Level::kOff).rays == 0, "off has no rays");
  Check(ultra.rays <= sr::kMaxRays && ultra.max_iterations <= sr::kMaxIterations && high.history_weight < 1.0f && ultra.history_weight < 1.0f, "within the push's fields");
}

void TestSizes() {
  // 1080p, the traced size is the picture's (the AO at ultra): the SSR images are half and the Hi-Z level 1 has that size
  sr::SsrSize s = sr::SizeFor(1920, 1080, 1920, 1080);
  Check(s.valid && s.size[0] == 960 && s.size[1] == 540 && s.hz == 1, "1080p traced whole: 960x540, Hi-Z level 1");
  // 1080p, the AO's high (half size)
  s = sr::SizeFor(1920, 1080, 960, 540);
  Check(s.valid && s.size[0] == 960 && s.size[1] == 540 && s.hz == 0, "1080p traced at half: 960x540, Hi-Z level 0");
  // 4K above 1080 lines: the traced size is half whatever the AO says
  s = sr::SizeFor(3840, 2160, 1920, 1080);
  Check(s.valid && s.size[0] == 1920 && s.size[1] == 1080 && s.hz == 0, "4K traced at half: 1920x1080, level 0");
  // odd sizes round up on both
  s = sr::SizeFor(1281, 721, 641, 361);
  Check(s.valid && s.size[0] == 641 && s.size[1] == 361 && s.hz == 0, "odd picture: half rounded up");
  s = sr::SizeFor(1281, 721, 1281, 721);
  Check(s.valid && s.size[0] == 641 && s.size[1] == 361 && s.hz == 1, "odd picture traced whole: level 1 has the size (rounded up, as the Hi-Z)");
  {
    // the Hi-Z level 1 of a whole traced size is exactly the SSR size, level 0 of a half one too (the chain halves rounding up)
    bool all = true;
    for (uint32_t w : {640u, 1279u, 1280u, 1281u, 1920u, 2561u}) {
      for (uint32_t h : {360u, 719u, 720u, 721u, 1080u, 1441u}) {
        const sr::SsrSize whole = sr::SizeFor(w, h, w, h);
        all = all && whole.valid && whole.size[0] == sc::HizLevelSize(w, 1) && whole.size[1] == sc::HizLevelSize(h, 1);
        const sr::SsrSize half = sr::SizeFor(w, h, sr::HalfUp(w), sr::HalfUp(h));
        all = all && half.valid && half.size[0] == sc::HizLevelSize(sr::HalfUp(w), 0) && half.hz == 0;
      }
    }
    Check(all, "the SSR size is the Hi-Z's own size at level 1 (whole) and level 0 (half) for every size");
  }
  Check(!sr::SizeFor(1920, 1080, 480, 270).valid && !sr::SizeFor(0, 0, 0, 0).valid && !sr::SizeFor(1920, 1080, 1920, 540).valid, "a traced size that is neither the picture's nor half of it is not known");
  Check(sr::ColourIsHalf(1081) && !sr::ColourIsHalf(1080) && sr::ColourIsHalf(2160) == sc::ColourPostIsHalf(2160) && sr::ColourIsHalf(1080) == sc::ColourPostIsHalf(1080), "the colour's size rule is the owner's");
  // memory: three RGBA16F images of the SSR size
  Check(sr::HeldBytes(960, 540) == 3ull * 960 * 540 * 8 && sr::HeldBytes(960, 540) < 13ull * 1024 * 1024, "1080p holds 11.9 MiB");
  Check(sr::HeldBytes(1920, 1080) == 3ull * 1920 * 1080 * 8 && sr::HeldBytes(1920, 1080) < 48ull * 1024 * 1024, "4K holds 47.5 MiB");
  Check(sr::HizLevelsUsed(7) == 6 && sr::HizLevelsUsed(3) == 3 && sr::ColourLevelsUsed(5) == 4 && sr::ColourLevelsUsed(1) == 1, "the push's slots cap the levels read");
}

sr::FrameFacts GoodFacts() {
  sr::FrameFacts f;
  f.level = sr::Level::kHigh;
  f.shaders_ready = true;
  f.has_lens = true;
  f.has_hiz = true;
  f.hiz_levels = 7;
  f.has_colour = true;
  f.colour_levels = 5;
  f.has_rt1 = f.has_rt2 = f.has_rt3 = true;
  f.depth_slot_ok = true;
  f.render[0] = 1920;
  f.render[1] = 1080;
  f.traced[0] = 960;
  f.traced[1] = 540;
  f.plane_size[0] = 1920;
  f.plane_size[1] = 1080;
  f.depth_samples = f.plane_samples = 2;
  f.depth_sample0 = f.plane_sample0 = 0;
  return f;
}

void TestDecision() {
  sr::FrameFacts f = GoodFacts();
  sr::Decision d = sr::Decide(f);
  Check(d.why == sr::Why::kUsed && !d.stub && d.size.valid && d.size.size[0] == 960, "a good frame runs");
  f.has_rt3 = false;
  d = sr::Decide(f);
  Check(d.why == sr::Why::kUsed && d.stub, "no RT3: it runs with the stub class");
  f = GoodFacts();
  f.level = sr::Level::kOff;
  Check(sr::Decide(f).why == sr::Why::kOff, "off");
  f = GoodFacts();
  f.shaders_ready = false;
  Check(sr::Decide(f).why == sr::Why::kNoShaders, "a pipeline still being made: no frame waits for it");
  f = GoodFacts();
  f.pause_view = true;
  Check(sr::Decide(f).why == sr::Why::kPause, "the pause world has no sidecar and no history");
  f = GoodFacts();
  f.has_rt1 = false;
  Check(sr::Decide(f).why == sr::Why::kNoNormals, "no normal plane");
  f = GoodFacts();
  f.has_lens = false;
  Check(sr::Decide(f).why == sr::Why::kNoLens, "no lens");
  f = GoodFacts();
  f.has_hiz = false;
  Check(sr::Decide(f).why == sr::Why::kNoHiz, "no Hi-Z");
  f = GoodFacts();
  f.has_colour = false;
  Check(sr::Decide(f).why == sr::Why::kNoColour, "no colour_post");
  f = GoodFacts();
  f.traced[0] = 700;
  Check(sr::Decide(f).why == sr::Why::kSizes, "a traced size the rule does not know");
  f = GoodFacts();
  f.plane_size[1] = 1000;
  Check(sr::Decide(f).why == sr::Why::kSizes, "planes smaller than the picture");
  f = GoodFacts();
  f.plane_size[0] = 1920;
  f.plane_size[1] = 1104;  // the target is taller than the picture (1.5x render scale: 1920x1104): fine
  Check(sr::Decide(f).why == sr::Why::kUsed, "planes larger than the picture are fine (the picture is their top left corner)");
  f = GoodFacts();
  f.plane_samples = 4;
  Check(sr::Decide(f).why == sr::Why::kSamples, "other host samples");
  f = GoodFacts();
  f.plane_sample0 = 1;
  Check(sr::Decide(f).why == sr::Why::kSamples, "another sample 0");
  f = GoodFacts();
  f.depth_slot_ok = false;
  Check(sr::Decide(f).why == sr::Why::kNoDepth, "the depth plane could not be read");
  // every reason has a distinct text
  bool distinct = true;
  for (uint32_t a = 0; a < uint32_t(sr::Why::kCount); ++a) {
    for (uint32_t b = a + 1; b < uint32_t(sr::Why::kCount); ++b) {
      distinct = distinct && std::string(sr::WhyName(sr::Why(a))) != std::string(sr::WhyName(sr::Why(b)));
    }
  }
  Check(distinct && std::string(sr::WhyName(sr::Why::kCount)) == "?", "the reasons have names");
}

void TestHistory() {
  sr::HistoryKey last;
  last.valid = true;
  last.frame = 100;
  last.size[0] = 960;
  last.size[1] = 540;
  sr::HistoryKey now = last;
  now.frame = 101;
  Check(sr::HistoryUsable(last, now, true), "the frame before, the same size: usable");
  Check(!sr::HistoryUsable(last, now, false), "no camera motion: not usable");
  now.frame = 102;
  Check(!sr::HistoryUsable(last, now, true), "a gap");
  now.frame = 101;
  now.size[0] = 961;
  Check(!sr::HistoryUsable(last, now, true), "another size");
  now.size[0] = 960;
  last.valid = false;
  Check(!sr::HistoryUsable(last, now, true), "no last frame");
}

void TestWet() {
  Check(sr::Wet01(0.095f) == 0.0f && sr::Wet01(0.2f) == 0.0f && sr::Wet01(1.17f) == 1.0f && Near(sr::Wet01(0.45f), 0.5, 1e-6) && sr::Wet01(std::nanf("")) == 0.0f, "the wet number: dry 0, rain 1, a ramp between");
  sr::WetVotes votes;
  float v = -1.0f;
  Check(!votes.Pick(&v), "no vote");
  for (int i = 0; i < 30; ++i) votes.Add(0.095f);
  for (int i = 0; i < 10; ++i) votes.Add(1.17f);
  votes.Add(9.0f);
  votes.Add(std::nanf(""));
  votes.Add(-1.0f);
  Check(votes.Pick(&v) && Near(v, 0.1, 1e-6) && votes.draws() == 40, "the commonest value in bins of 0.1; a value out of range or a NaN is not a vote");
  votes.Reset();
  for (int i = 0; i < 5; ++i) votes.Add(1.17f);
  Check(votes.Pick(&v) && Near(v, 1.2, 1e-6) && votes.draws() == 5, "after a reset: rain");
  float wet = 0.0f;
  int frames = 0;
  while (wet < 0.95f && frames < 500) {
    wet = sr::SmoothWet(wet, 1.0f);
    ++frames;
  }
  Check(frames > 20 && frames < 90, Num("the start of rain ramps over a second or so (%.0f frames to 95%%)", frames));
  // the shaders' use of it
  Check(hs::SsrWetSmooth(uint(GB_CLASS_ROAD), 0.30f, 0.0f) == 0.30f && Near(hs::SsrWetSmooth(uint(GB_CLASS_ROAD), 0.30f, 1.0f), 0.92, 1e-6) && Near(hs::SsrWetSmooth(uint(GB_CLASS_ROAD), 0.30f, 0.5f), 0.61, 1e-6),
        "a road's smoothness rises with the wetness");
  Check(hs::SsrWetSmooth(uint(GB_CLASS_ROAD), 0.97f, 1.0f) == 0.97f, "and never falls");
  Check(Near(hs::SsrWetSmooth(uint(GB_CLASS_SIDEWALK), 0.2f, 1.0f), 0.80, 1e-6) && hs::SsrWetSmooth(uint(GB_CLASS_METAL), 0.7f, 1.0f) == 0.7f && hs::SsrWetSmooth(uint(GB_CLASS_GLASS), 0.95f, 1.0f) == 0.95f,
        "a sidewalk too; metal and glass do not care about rain");
}

// ---------------------------------------------------------------------------
// the push block
// ---------------------------------------------------------------------------

void TestPush() {
  namespace p = sr::push;
  Check(SSR_W_DEPTH_RT1 == int(p::kDepthRt1) && SSR_W_RT2_RT3 == int(p::kRt2Rt3) && SSR_W_SRC == int(p::kSrc) && SSR_W_HIZ01 == int(p::kHiz01) && SSR_W_HIZ23 == int(p::kHiz23) && SSR_W_HIZ45 == int(p::kHiz45) &&
            SSR_W_COL01 == int(p::kCol01) && SSR_W_COL23 == int(p::kCol23) && SSR_W_RENDER == int(p::kRender) && SSR_W_FLAGS == int(p::kFlags) && SSR_W_FRAME == int(p::kFrame) && SSR_W_LIN0 == int(p::kLin0),
        "the slot and size words are the .hlsli's");
  Check(SSR_W_TAN_X == int(p::kTanX) && SSR_W_TAN_Y == int(p::kTanY) && SSR_W_LIN_X == int(p::kLinX) && SSR_W_LIN_Y == int(p::kLinY) && SSR_W_LIN_Z == int(p::kLinZ) && SSR_W_LIN_W == int(p::kLinW) &&
            SSR_W_NEAR == int(p::kNear) && SSR_W_MAX_DIST == int(p::kMaxDist) && SSR_W_RIGHT == int(p::kRight) && SSR_W_UP == int(p::kUp) && SSR_W_STRENGTH == int(p::kStrength) &&
            SSR_W_MIN_SMOOTH == int(p::kMinSmooth) && SSR_W_STUB_SMOOTH == int(p::kStubSmooth) && SSR_W_WET == int(p::kWet) && SSR_W_THICK == int(p::kThick) && SSR_W_CAP == int(p::kCap) &&
            SSR_PUSH_WORDS == int(p::kWords),
        "the lens, camera and tunable words are the .hlsli's");
  Check(int(SSR_FLAG_MS) == int(p::kFlagMs) && int(SSR_FLAG_HZ) == int(p::kFlagHz) && int(SSR_FLAG_COL_HALF) == int(p::kFlagColourHalf) && int(SSR_FLAG_STUB) == int(p::kFlagStub) &&
            int(SSR_FLAG_HISTORY) == int(p::kFlagHistory) && int(SSR_FLAG_CAMERA) == int(p::kFlagCamera) && int(SSR_FLAG_SHIFT) == int(p::kFlagShift) && int(SSR_RAYS_SHIFT) == int(p::kRaysShift) &&
            int(SSR_VIEW_SHIFT) == int(p::kViewShift) && int(SSR_ITER_SHIFT) == int(p::kIterShift),
        "the flag bits and shifts are the .hlsli's");
  Check(p::kMotion == p::kRight && p::kPrevTanX == p::kNear && p::kPrevTanY == p::kMaxDist && p::kHistoryWeight == p::kFrame, "the temporal pass's overlay");
  Check(SSR_W_MOTION == int(p::kMotion) && SSR_W_PREV_TAN_X == int(p::kPrevTanX) && SSR_W_PREV_TAN_Y == int(p::kPrevTanY) && SSR_W_HISTORY_WEIGHT == int(p::kHistoryWeight), "the .hlsli names the same overlay");
  Check(p::kMotion + 12 == p::kWords, "the motion's twelve floats end the block");
  // words that the three passes need at once do not overlap: the camera rows (the apply, the trace) and the motion (the temporal) are different passes, the rest never share
  Check(p::kUp + 3 == p::kStrength && p::kRight + 3 == p::kUp, "the camera's two axes sit before the tunables");
  // packing
  Check(sr::Slot16(0xFFFFFFFFu) == 0xFFFFu && sr::Slot16(5) == 5 && sr::Slot16(0xFFFF) == 0xFFFFu && sr::Slot16(70000) == 0xFFFFu, "a slot in 16 bits: none stays none");
  const uint32_t w = sr::PackSlots(123, 4567);
  Check(sr::UnpackLo(w) == 123 && sr::UnpackHi(w) == 4567 && hs::SsrLo(w) == 123 && hs::SsrHi(w) == 4567, "two slots to a word");
  const uint32_t none = sr::PackSlots(sc::kNoSlot, 9);
  Check(sr::UnpackLo(none) == 0xFFFFu && !hs::SsrHasSlot(hs::SsrLo(none)) && hs::SsrHasSlot(hs::SsrHi(none)), "none survives the pack");
  sr::FlagsIn f;
  f.sample0 = 3;
  f.hiz_levels = 7;
  f.colour_levels = 5;
  f.ms = true;
  f.hz1 = true;
  f.stub = true;
  f.rays = 2;
  f.view = 3;
  f.max_iterations = 64;
  const uint32_t word = sr::PackFlags(f);
  Check((word & 15u) == 3 && ((word >> 4) & 15u) == 6 && ((word >> 8) & 15u) == 4, "the flags word: sample0, the Hi-Z's levels (6 at most), the colour's (4 at most)");
  Check(((word >> SSR_FLAG_SHIFT) & (SSR_FLAG_MS | SSR_FLAG_HZ | SSR_FLAG_STUB)) == (SSR_FLAG_MS | SSR_FLAG_HZ | SSR_FLAG_STUB) && ((word >> SSR_FLAG_SHIFT) & (SSR_FLAG_COL_HALF | SSR_FLAG_HISTORY | SSR_FLAG_CAMERA)) == 0,
        "the flag bits");
  Check(((word >> SSR_RAYS_SHIFT) & 3u) == 1 && ((word >> SSR_VIEW_SHIFT) & 15u) == 3 && (word >> SSR_ITER_SHIFT) == 64, "rays - 1, the view, the iterations");
  f.max_iterations = 9999;
  f.rays = 9;
  const uint32_t clamped = sr::PackFlags(f);
  Check((clamped >> SSR_ITER_SHIFT) == 255 && ((clamped >> SSR_RAYS_SHIFT) & 3u) == 1, "the iterations and the rays are clamped to their fields");
  Check(((SSR_FLAG_COL_HALF | SSR_FLAG_STUB | SSR_FLAG_HISTORY | SSR_FLAG_CAMERA | SSR_FLAG_MS | SSR_FLAG_HZ) << SSR_FLAG_SHIFT) < (1u << SSR_RAYS_SHIFT), "the flag bits end below the rays");
  sr::Push push;
  push.SetFloat(p::kTanX, 0.575f);
  push.Set(p::kFrame, 77);
  Check(push.Float(p::kTanX) == 0.575f && hs::asfloat(push.w[p::kTanX]) == 0.575f && push.w[p::kFrame] == 77, "floats ride in the words");
  {
    float a[16], b[16];
    for (int i = 0; i < 16; ++i) a[i] = b[i] = float(i) * 0.37f - 2.0f;
    Check(sr::SameCamera(a, b), "the same camera");
    b[12] += 0.0002f;
    Check(sr::SameCamera(a, b), "a camera within the tolerance (a hair of the position) is the same");
    b[12] += 0.5f;
    Check(!sr::SameCamera(a, b), "a camera that moved half a unit is not");
    b[12] = std::nanf("");
    Check(!sr::SameCamera(a, b), "a NaN is not a camera");
  }
  Check(sr::CapFor(16.0f) == 64.0f && sr::CapFor(0.0f) == sr::kCapSceneWhites * sr::kDefaultExposure, "the cap is a number of white walls in the target's units");
  Check(sr::ViewFromSetting(0) == sr::View::kOff && sr::ViewFromSetting(4) == sr::View::kHit && sr::ViewFromSetting(5) == sr::View::kOff && sr::ViewFromSetting(-1) == sr::View::kOff, "the debug views");
}

// ---------------------------------------------------------------------------
// the camera's axes
// ---------------------------------------------------------------------------

void TestCamera() {
  // the game's view inverse (c12-c15 of the vertex constants: row vectors, row 0 the camera's right in world space, row 1 its up, row 2 its BACK): a camera turned 30 degrees about the world's up
  // axis (y) and pitched 10 degrees. ao_gbuffer.h RowsFromViewInverse is the AO's way; the axes the push carries are its x and y rows, and up x right is its z row.
  const double yaw = 30.0 * 3.14159265358979 / 180.0, pitch = 10.0 * 3.14159265358979 / 180.0;
  const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  // forward (the game's -z) = (sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch))... any rotation will do: build right, up, back from yaw and pitch
  const double back[3] = {-sy * cp, -sp, cy * cp};
  const double right[3] = {cy, 0.0, sy};
  const double up[3] = {back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0]};  // back x right = up
  double m[4][4] = {{right[0], right[1], right[2], 0}, {up[0], up[1], up[2], 0}, {back[0], back[1], back[2], 0}, {5, 2, 7, 1}};
  mcla::gpu::ao_gbuffer::Rows rows;
  const bool made = mcla::gpu::ao_gbuffer::RowsFromViewInverse(m, &rows);
  Check(made, "the rows of a rotation are accepted by the AO's function");
  const sr::CameraAxes axes = sr::AxesFromRows(rows.x, rows.y, rows.z);
  Check(axes.ok, "a right handed camera's axes are usable");
  // turning a world direction into view space with the axes only (the .hlsli's rule: forward = up x right) equals the AO's rows
  Lcg rng;
  float worst = 0.0f;
  for (int i = 0; i < 200; ++i) {
    hs::float3 n(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
    n = hs::normalize(n);
    const hs::float3 r(axes.right[0], axes.right[1], axes.right[2]);
    const hs::float3 u(axes.up[0], axes.up[1], axes.up[2]);
    const hs::float3 f = hs::cross(u, r);
    const hs::float3 viaAxes(hs::dot(n, r), hs::dot(n, u), hs::dot(n, f));
    const hs::float3 viaRows = hs::AoGbToView(n, hs::float3(rows.x[0], rows.x[1], rows.x[2]), hs::float3(rows.y[0], rows.y[1], rows.y[2]), hs::float3(rows.z[0], rows.z[1], rows.z[2]));
    worst = std::max(worst, std::fabs(viaAxes.x - viaRows.x) + std::fabs(viaAxes.y - viaRows.y) + std::fabs(viaAxes.z - viaRows.z));
  }
  Check(worst < 1e-5f, Num("forward = up x right gives the AO's third row (worst %.2e)", worst));
  // a mirror (a left handed matrix) and a non unit row are refused
  // the right handed world (x right, y up) looks down -z: forward = up x right = (0, 0, -1)
  float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1};
  Check(sr::AxesFromRows(x, y, z).ok == false, "a mirrored basis (forward = -(up x right)) is refused: the depth's normal is used instead");
  float z2[3] = {0, 0, -1};
  Check(sr::AxesFromRows(x, y, z2).ok, "the default camera of the right handed world: forward -z");
  float x3[3] = {2, 0, 0};
  Check(!sr::AxesFromRows(x3, y, z2).ok, "a scaled row is refused");
}

// ---------------------------------------------------------------------------
// the class gate
// ---------------------------------------------------------------------------

void TestGate() {
  const uint32_t ssr = GB_FLAG_SSR, veh = GB_FLAG_VEHICLE;
  struct Row {
    uint32_t cls;
    float expected;
    const char* name;
  };
  const Row rows[] = {{GB_CLASS_NONE, 0.0f, "none"},      {GB_CLASS_WORLD, 0.6f, "world"},     {GB_CLASS_ROAD, 1.0f, "road"},      {GB_CLASS_WET_ROAD, 1.0f, "wet road"},
                      {GB_CLASS_SIDEWALK, 0.8f, "sidewalk"}, {GB_CLASS_TERRAIN, 0.0f, "terrain"}, {GB_CLASS_LOD, 0.0f, "lod"},        {GB_CLASS_FOLIAGE, 0.0f, "foliage"},
                      {GB_CLASS_PAINT, 0.0f, "paint"},    {GB_CLASS_CHROME, 0.0f, "chrome"},   {GB_CLASS_METAL, 0.8f, "metal"},    {GB_CLASS_GLASS, 0.7f, "glass"},
                      {GB_CLASS_SKIN, 0.0f, "skin"},      {GB_CLASS_EMISSIVE, 0.0f, "emissive"}, {GB_CLASS_INTERIOR, 0.0f, "interior"}, {GB_CLASS_WATER, 1.0f, "water"}};
  bool all = true;
  for (const Row& row : rows) {
    const float with = hs::SsrClassGain(row.cls, ssr);
    const float without = hs::SsrClassGain(row.cls, 0);
    const float vehicle = hs::SsrClassGain(row.cls, ssr | veh);
    const bool ok = with == row.expected && without == 0.0f && vehicle == 0.0f;
    if (!ok) std::printf("class %s: with %.2f without %.2f vehicle %.2f (expected %.2f)\n", row.name, with, without, vehicle, row.expected);
    all = all && ok;
  }
  Check(all, "every class: the gain with the SSR flag, none without it, none on a vehicle");
  // paint and chrome keep their own reflections whatever the flags say
  Check(hs::SsrClassGain(GB_CLASS_PAINT, ssr) == 0.0f && hs::SsrClassGain(GB_CLASS_CHROME, ssr) == 0.0f && hs::SsrClassGain(GB_CLASS_PAINT, ssr | 2) == 0.0f, "paint and chrome never take the effect, even flagged");
  // the numbers are the G-buffer's
  Check(GB_CLASS_ROAD == int(gb::SurfaceClass::kRoad) && GB_CLASS_METAL == int(gb::SurfaceClass::kMetal) && GB_CLASS_PAINT == int(gb::SurfaceClass::kPaint) && GB_FLAG_SSR == int(gb::kMaterialFlagSsr) &&
            GB_FLAG_VEHICLE == int(gb::kMaterialFlagVehicle),
        "the class and flag numbers are the interface's");
  // the RT3 texel's decode into the gate (r smoothness, g class | flags << 5)
  const uint32_t word = (200u) | (uint32_t(gb::SurfaceClass::kRoad) << 8) | (uint32_t(gb::kMaterialFlagSsr) << 13);
  const hs::float2 rt3 = hs::GbMaterialRt3(word);
  Check(hs::GbRt3Class(rt3) == uint32_t(gb::SurfaceClass::kRoad) && (hs::GbRt3Flags(rt3) & 1u) != 0 && Near(hs::GbRt3Smoothness(rt3), 200.0 / 255.0, 1e-5), "RT3 of a road: the class, the flag and the smoothness");
  // the stub: a world normal up is a road, a wall is nothing
  Check(hs::SsrStubClass(1.0f) == uint32_t(GB_CLASS_ROAD) && hs::SsrStubClass(0.86f) == uint32_t(GB_CLASS_ROAD) && hs::SsrStubClass(0.84f) == uint32_t(GB_CLASS_NONE) && hs::SsrStubClass(-1.0f) == uint32_t(GB_CLASS_NONE),
        "the stub makes a road of a surface that points up");
  Check(Near(hs::SsrAlphaOfSmooth(0.9f), 0.01, 1e-6) && Near(hs::SsrAlphaOfSmooth(0.0f), 1.0, 1e-6) && hs::SsrAlphaOfSmooth(1.0f) == hs::kSsrMinAlpha, "alpha = (1 - s)^2, never below the floor");
  Check(Near(hs::SsrAlphaOfSmooth(0.85f), double(gb::AlphaOfSmoothness(0.85f)), 1e-6), "the same alpha as the G-buffer's rule");
}

// ---------------------------------------------------------------------------
// the environment BRDF, the noise, the GGX sampling, the fades
// ---------------------------------------------------------------------------

void TestBrdf() {
  // a smooth dielectric (F0 0.04): the usual Fresnel curve: about 0.04 face on and 0.5 and more at grazing
  const auto reflect = [](float f0, float rough, float nov) {
    const hs::float2 ab = hs::SsrEnvBrdf(rough, nov);
    return f0 * ab.x + ab.y;
  };
  Check(Near(reflect(0.04f, 0.0f, 1.0f), 0.0456, 0.003), Num("face on, smooth: %.4f (0.04 plus the table's rounding)", reflect(0.04f, 0.0f, 1.0f)));
  Check(reflect(0.04f, 0.0f, 0.1f) > 0.45f && reflect(0.04f, 0.0f, 0.1f) < 0.7f, Num("grazing, smooth: %.3f", reflect(0.04f, 0.0f, 0.1f)));
  bool monotone = true;
  float last = 2.0f;
  for (int i = 1; i <= 100; ++i) {
    const float v = reflect(0.04f, 0.1f, float(i) / 100.0f);
    monotone = monotone && v <= last + 1e-5f;
    last = v;
  }
  Check(monotone, "the reflectance falls from grazing to face on");
  Check(reflect(0.04f, 1.0f, 0.1f) < reflect(0.04f, 0.0f, 0.1f), "a rough surface reflects less at grazing than a smooth one");
  Check(reflect(1.0f, 0.5f, 0.7f) > 0.5f && reflect(1.0f, 0.5f, 0.7f) < 1.2f, "a metal (F0 1) reflects most of it");
  bool bounded = true;
  float lowest = 1.0f, highest = 0.0f;
  for (int r = 0; r <= 10; ++r) {
    for (int n = 0; n <= 20; ++n) {
      const float v = reflect(0.04f, float(r) / 10.0f, float(n) / 20.0f);
      lowest = std::min(lowest, v);
      highest = std::max(highest, v);
    }
  }
  bounded = lowest > -0.005f && highest <= 1.05f;  // the table overshoots to 1.04 at exactly grazing; the apply pass saturates it
  Check(bounded, Num("a dielectric's reflectance stays between 0 and 1 over roughness and angle (lowest %.4f, highest %.4f)", lowest, highest));
}

void TestNoise() {
  double sum = 0.0, sum2 = 0.0;
  int n = 0;
  float lo = 1.0f, hi = 0.0f;
  for (uint32_t frame = 0; frame < 16; ++frame) {
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        const float a = hs::SsrNoise1(x, y, frame, 0);
        const float b = hs::SsrNoise2(x, y, frame, 0);
        sum += a + b;
        sum2 += double(a) * a + double(b) * b;
        n += 2;
        lo = std::min(lo, std::min(a, b));
        hi = std::max(hi, std::max(a, b));
      }
    }
  }
  const double mean = sum / n;
  const double var = sum2 / n - mean * mean;
  Check(Near(mean, 0.5, 0.02) && Near(var, 1.0 / 12.0, 0.01) && lo >= 0.0f && hi < 1.0f, Num("the noise is uniform on 0..1 (mean %.3f, variance %.4f)", mean, var));
  // the temporal sequence of one pixel: 8 frames fall in different eighths most of the time
  int covered = 0;
  for (int y = 0; y < 16; ++y) {
    bool seen[8] = {};
    for (uint32_t frame = 0; frame < 8; ++frame) {
      seen[std::min(7, int(hs::SsrNoise1(5, y, frame, 0) * 8.0f))] = true;
    }
    int bins = 0;
    for (bool b : seen) bins += b ? 1 : 0;
    covered += bins;
  }
  Check(covered / 16.0 > 4.5, Num("one pixel's eight frames cover %.1f of 8 bins on average", covered / 16.0));
  // the two rays of a pixel are not the same
  Check(hs::SsrNoise1(10, 10, 3, 0) != hs::SsrNoise1(10, 10, 3, 1) && hs::SsrNoise2(10, 10, 3, 0) != hs::SsrNoise2(10, 10, 3, 1), "the second ray of a pixel has its own numbers");
  // neighbours differ (the noise is not a smooth ramp)
  double diff = 0.0;
  for (int x = 0; x < 63; ++x) diff += std::fabs(hs::SsrNoise1(x, 7, 2, 0) - hs::SsrNoise1(x + 1, 7, 2, 0));
  Check(diff / 63.0 > 0.1, "neighbouring pixels differ");
}

void TestGgx() {
  Lcg rng;
  // a unit normal and a view direction at 37 degrees
  const hs::float3 n(0.0f, 0.0f, 1.0f);
  const float ang = 37.0f * 3.14159265f / 180.0f;
  const hs::float3 v(std::sin(ang), 0.0f, std::cos(ang));
  const hs::float3 mirror = hs::SsrReflect(n, v);
  Check(Near(mirror.x, -std::sin(ang), 1e-5) && Near(mirror.z, std::cos(ang), 1e-5), "the mirror direction");
  const auto spread = [&](float alpha, int* below, double* unit_err) {
    double sum = 0.0;
    int cnt = 0;
    *below = 0;
    *unit_err = 0.0;
    for (int i = 0; i < 4000; ++i) {
      const hs::float3 r = hs::SsrSampleReflection(n, v, alpha, rng.Next(), rng.Next());
      *unit_err = std::max<double>(*unit_err, std::fabs(hs::dot(r, r) - 1.0f));
      if (r.z <= 0.0f) {
        ++(*below);
        continue;
      }
      const float c = std::min(1.0f, std::max(-1.0f, hs::dot(r, mirror)));
      sum += std::acos(c);
      ++cnt;
    }
    return cnt ? sum / cnt : 0.0;
  };
  int below = 0;
  double unit_err = 0.0;
  const double s_mirror = spread(0.005f, &below, &unit_err);
  Check(s_mirror < 0.02 && below == 0, Num("alpha 0.005: the samples stay within %.4f rad of the mirror", s_mirror));
  const double s_smooth = spread(0.05f, &below, &unit_err);
  const double s_mid = spread(0.2f, &below, &unit_err);
  const double s_rough = spread(0.5f, &below, &unit_err);
  Check(s_smooth < s_mid && s_mid < s_rough, Num("the spread grows with alpha (%.3f, %.3f, %.3f rad)", s_smooth, s_mid, s_rough));
  Check(s_mid > 0.1 && s_mid < 0.5, Num("alpha 0.2 spreads the reflection by about %.2f rad (a few times alpha)", s_mid));
  Check(unit_err < 1e-4, Num("the sampled directions are unit length (worst %.2e)", unit_err));
  // grazing: more of the samples point below the surface, and the caller keeps the mirror for them
  const hs::float3 grazing(std::sin(1.45f), 0.0f, std::cos(1.45f));
  int below_grazing = 0;
  for (int i = 0; i < 4000; ++i) {
    const hs::float3 r = hs::SsrSampleReflection(n, grazing, 0.3f, rng.Next(), rng.Next());
    below_grazing += r.z <= 0.0f ? 1 : 0;
  }
  int below_face = 0;
  for (int i = 0; i < 4000; ++i) {
    const hs::float3 r = hs::SsrSampleReflection(n, hs::float3(0, 0, 1), 0.3f, rng.Next(), rng.Next());
    below_face += r.z <= 0.0f ? 1 : 0;
  }
  // GGX has a heavy tail: the share of the half vectors tilted past 45 degrees is alpha^2 / (alpha^2 + 1) (8.3% at alpha 0.3), which at normal incidence is the share of reflections below the surface
  Check(Near(below_face / 4000.0, 0.3 * 0.3 / (0.3 * 0.3 + 1.0), 0.02) && below_grazing < 600, Num("below the surface: face on %.0f of 4000 (the GGX tail says 330), grazing %.0f", below_face, below_grazing));
  // a tilted normal: the basis works for every normal (no NaN anywhere on the sphere)
  bool finite = true;
  for (int i = 0; i < 2000; ++i) {
    hs::float3 nn(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
    nn = hs::normalize(nn);
    hs::float3 vv(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
    vv = hs::normalize(vv);
    if (hs::dot(nn, vv) < 0.05f) vv = hs::normalize(vv + nn * 1.5f);
    const hs::float3 r = hs::SsrSampleReflection(nn, vv, rng.Range(0.01f, 0.8f), rng.Next(), rng.Next());
    finite = finite && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z);
  }
  Check(finite, "every normal and view direction gives a finite direction");
  // the corner of the basis: n pointing down z (the Duff basis' singular side) and exactly along -z
  const hs::float3 down = hs::SsrSampleReflection(hs::float3(0, 0, -1), hs::float3(0, 0, -1), 0.1f, 0.3f, 0.6f);
  Check(std::isfinite(down.x) && std::isfinite(down.y) && std::isfinite(down.z), "the normal (0, 0, -1) is no singular case");
}

void TestFades() {
  Check(hs::SsrEdgeFade(0.5f, 0.5f) == 1.0f && hs::SsrEdgeFade(0.0f, 0.5f) == 0.0f && hs::SsrEdgeFade(0.5f, 1.0f) == 0.0f && hs::SsrEdgeFade(1.0f, 0.5f) == 0.0f, "the edge fade: whole in the middle, none at the border");
  Check(hs::SsrEdgeFade(0.04f, 0.5f) > 0.4f && hs::SsrEdgeFade(0.04f, 0.5f) < 0.6f && hs::SsrEdgeFade(0.08f, 0.5f) == 1.0f && hs::SsrEdgeFade(0.92f, 0.5f) == 1.0f, "half way over the border it is half");
  bool symmetric = true;
  for (int i = 0; i <= 20; ++i) {
    const float u = float(i) / 20.0f;
    symmetric = symmetric && Near(hs::SsrEdgeFade(u, 0.5f), hs::SsrEdgeFade(1.0f - u, 0.5f), 1e-6) && Near(hs::SsrEdgeFade(0.5f, u), hs::SsrEdgeFade(u, 0.5f), 1e-6);
  }
  Check(symmetric, "the fade is the same on all four sides");
  Check(hs::SsrFacingFade(1.0f) == 1.0f && hs::SsrFacingFade(0.15f) == 1.0f && hs::SsrFacingFade(-0.15f) == 0.0f && hs::SsrFacingFade(-1.0f) == 0.0f && Near(hs::SsrFacingFade(0.0f), 0.5, 1e-6),
        "a ray that points back at the camera has no reflection");
  // the colour's mip: sharper for a smooth surface and a near hit, blurrier for a rough one and a far hit, never past the chain
  const float l_smooth = hs::SsrConeLod(0.01f, 10.0f, 10.0f, 0.58f, 1920.0f, 3.0f);
  const float l_rough = hs::SsrConeLod(0.2f, 10.0f, 10.0f, 0.58f, 1920.0f, 3.0f);
  const float l_far = hs::SsrConeLod(0.05f, 80.0f, 80.0f, 0.58f, 1920.0f, 3.0f);
  const float l_near = hs::SsrConeLod(0.05f, 2.0f, 80.0f, 0.58f, 1920.0f, 3.0f);
  Check(l_smooth < l_rough && l_rough <= 3.0f && l_smooth >= 0.0f, Num("the mip grows with the roughness (%.2f -> %.2f)", l_smooth, l_rough));
  Check(l_near <= l_far + 1e-6f, "and with the distance travelled for the same surface");
  Check(hs::SsrConeLod(1.0f, 500.0f, 1.0f, 0.58f, 1920.0f, 3.0f) == 3.0f && hs::SsrConeLod(0.0f, 0.0f, 10.0f, 0.58f, 1920.0f, 3.0f) == 0.0f, "clamped to the chain");
}

// ---------------------------------------------------------------------------
// the virtual image's reprojection: a flat mirror and a moving camera
// ---------------------------------------------------------------------------

struct Pose {
  // a camera in the game's world: position, yaw about world y, pitch; view space of this renderer (x right, y up, z forward)
  double pos[3];
  double yaw;
  double pitch;
};
struct Vec {
  double v[3];
};
Vec Sub(Vec a, Vec b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]}}; }
double DotV(Vec a, Vec b) { return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]; }
Vec CrossV(Vec a, Vec b) { return {{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0]}}; }
// the axes of a pose in the renderer's view space convention: right, up, forward
void Axes(const Pose& p, Vec* right, Vec* up, Vec* fwd) {
  const double cy = std::cos(p.yaw), sy = std::sin(p.yaw), cp = std::cos(p.pitch), sp = std::sin(p.pitch);
  *fwd = {{sy * cp, sp, cy * cp}};
  *right = {{cy, 0.0, -sy}};
  *up = CrossV(*fwd, *right);  // forward x right = up in this left handed view space... pick the sign so that up has a positive y
  if (up->v[1] < 0.0) *up = {{-up->v[0], -up->v[1], -up->v[2]}};
}
Vec ToView(const Pose& p, Vec world) {
  Vec r, u, f;
  Axes(p, &r, &u, &f);
  const Vec d = Sub(world, {{p.pos[0], p.pos[1], p.pos[2]}});
  return {{DotV(d, r), DotV(d, u), DotV(d, f)}};
}
[[maybe_unused]] Vec ToWorld(const Pose& p, Vec view) {
  Vec r, u, f;
  Axes(p, &r, &u, &f);
  return {{p.pos[0] + view.v[0] * r.v[0] + view.v[1] * u.v[0] + view.v[2] * f.v[0], p.pos[1] + view.v[0] * r.v[1] + view.v[1] * u.v[1] + view.v[2] * f.v[1],
           p.pos[2] + view.v[0] * r.v[2] + view.v[1] * u.v[2] + view.v[2] * f.v[2]}};
}

void TestVirtualImage() {
  // the road is the plane y = 0 of the world; a lamp's head H stands over it. The camera is 1.5 above the road. The image of H in the road seen from the camera is at the mirror point H* = (Hx, -Hy, Hz).
  const Vec H{{3.0, 6.0, 40.0}};
  const Vec Hmirror{{H.v[0], -H.v[1], H.v[2]}};
  const Pose before{{0.0, 1.5, 0.0}, 0.00, 0.02};
  const Pose now{{0.4, 1.5, 1.3}, 0.03, 0.02};
  const double tan_x = 0.9, tan_y = 0.5;
  // the receiver: where the line from the camera to H* crosses the road plane, in the current frame
  const Vec cam{{now.pos[0], now.pos[1], now.pos[2]}};
  const Vec dir = Sub(Hmirror, cam);
  const double t = (0.0 - cam.v[1]) / dir.v[1];
  const Vec P_world{{cam.v[0] + dir.v[0] * t, 0.0, cam.v[2] + dir.v[2] * t}};
  const Vec P_view = ToView(now, P_world);
  const Vec H_view = ToView(now, H);
  const double length_to_hit = std::sqrt(DotV(Sub(H_view, P_view), Sub(H_view, P_view)));
  // the virtual point is the mirror image of H in the current view space
  const hs::float3 P(float(P_view.v[0]), float(P_view.v[1]), float(P_view.v[2]));
  const hs::float3 V = hs::SsrVirtualPoint(P, float(length_to_hit));
  const Vec Hm_view = ToView(now, Hmirror);
  Check(Near(V.x, Hm_view.v[0], 2e-3) && Near(V.y, Hm_view.v[1], 2e-3) && Near(V.z, Hm_view.v[2], 2e-3),
        Num("the virtual point is the mirror image of the hit: (%.3f, %.3f, %.3f)", V.x, V.y, V.z) + Num(" against (%.3f, %.3f, %.3f)", Hm_view.v[0], Hm_view.v[1], Hm_view.v[2]));
  // the motion from this frame's view space into the last frame's (the 3x4 rows)
  hs::float4 rows[3];
  {
    Vec r, u, f, r0, u0, f0;
    Axes(now, &r, &u, &f);
    Axes(before, &r0, &u0, &f0);
    // a point q of this view space: world = pos + q.x r + q.y u + q.z f; last view = (world - pos0) . (r0, u0, f0)
    const Vec dp = Sub({{now.pos[0], now.pos[1], now.pos[2]}}, {{before.pos[0], before.pos[1], before.pos[2]}});
    const Vec axes_old[3] = {r0, u0, f0};
    for (int k = 0; k < 3; ++k) {
      rows[k] = hs::float4(float(DotV(r, axes_old[k])), float(DotV(u, axes_old[k])), float(DotV(f, axes_old[k])), float(DotV(dp, axes_old[k])));
    }
  }
  const hs::SsrPrevious prev = hs::SsrToPrevious(rows[0], rows[1], rows[2], V, float(tan_x), float(tan_y));
  // where the image of H was in the last frame: the mirror point seen from the last camera
  const Vec Hm_old = ToView(before, Hmirror);
  const double u_old = 0.5 + 0.5 * Hm_old.v[0] / (tan_x * Hm_old.v[2]);
  const double v_old = 0.5 - 0.5 * Hm_old.v[1] / (tan_y * Hm_old.v[2]);
  Check(prev.ok && Near(prev.u, u_old, 2e-4) && Near(prev.v, v_old, 2e-4) && Near(prev.z, Hm_old.v[2], 5e-3),
        Num("the reflection of the lamp was at (%.5f, %.5f) in the last frame", u_old, v_old) + Num(", the reprojection says (%.5f, %.5f)", prev.u, prev.v));
  // and the plain receiver reprojection (length 0) is NOT there: it differs by pixels, which is why the virtual image matters
  const hs::SsrPrevious plain = hs::SsrToPrevious(rows[0], rows[1], rows[2], P, float(tan_x), float(tan_y));
  const double miss_px = std::hypot((plain.u - u_old) * 1920.0, (plain.v - v_old) * 1080.0);
  Check(plain.ok && miss_px > 5.0, Num("without the virtual image the history would be %.0f pixels off", miss_px));
  // a miss (a hit very far away) reprojects like a rotation
  const hs::float3 far_point = hs::SsrVirtualPoint(P, sr::kDefaultMaxDistance * 1000.0f);
  const hs::SsrPrevious far = hs::SsrToPrevious(rows[0], rows[1], rows[2], far_point, float(tan_x), float(tan_y));
  Check(far.ok, "a ray that found nothing reprojects to somewhere on the picture");
  // the same surface test
  Check(hs::SsrSameSurface(20.0f, 20.0f) && hs::SsrSameSurface(20.0f, 21.0f) && !hs::SsrSameSurface(20.0f, 25.0f) && hs::SsrSameSurface(0.5f, 0.54f) && !hs::SsrSameSurface(0.5f, 0.7f), "the same surface: within 8% and a few centimetres");
  Check(!hs::SsrToPrevious(rows[0], rows[1], rows[2], hs::float3(0.0f, 0.0f, -5.0f), 0.9f, 0.5f).ok, "a point behind the last camera has no history");
  Check(hs::SsrClampToBox(5.0f, 0.0f, 1.0f) == 1.1f && hs::SsrClampToBox(-3.0f, 0.0f, 1.0f) == -0.1f && hs::SsrClampToBox(0.5f, 0.0f, 1.0f) == 0.5f, "the history is clamped to the current neighbourhood's box");
}


// ---------------------------------------------------------------------------
// the Hi-Z march
// ---------------------------------------------------------------------------

struct Lens {
  double tan_x = 0.9, tan_y = 0.5, n = 0.1, f = 1000.0;
  double a() const { return f / (f - n); }
};
hs::SsrTraceParams MakeParams(const Lens& lens, int w, int h, int levels, uint max_iter) {
  hs::SsrTraceParams p;
  p.width = float(w);
  p.height = float(h);
  p.width_i = uint(w);
  p.height_i = uint(h);
  p.tan_x = float(lens.tan_x);
  p.tan_y = float(lens.tan_y);
  // d = a (1 - n / z) = (a z - a n) / (z): lin.x = a, lin.y = 1, lin.z = a n, lin.w = 0 (ao_common.hlsli AoLinearDepth is its inverse)
  p.lin_x = float(lens.a());
  p.lin_y = 1.0f;
  p.lin_z = float(lens.a() * lens.n);
  p.lin_w = 0.0f;
  p.near_plane = float(lens.n);
  p.max_distance = 150.0f;
  p.thick_rel = 0.04f;
  p.levels = uint(levels);
  p.max_iter = max_iter;
  return p;
}

// A depth image and its Hi-Z (the min chain of screen_rules.h: the nearest of the 2x2 below, an odd edge clamped).
struct Pyramid {
  std::vector<std::vector<float>> level;
  int w[8] = {}, h[8] = {};
  int levels = 0;
  void Build(const std::vector<float>& depth, int w0, int h0) {
    level.clear();
    level.push_back(depth);
    w[0] = w0;
    h[0] = h0;
    levels = 1;
    while (levels < 7 && std::min(w[levels - 1], h[levels - 1]) > 1) {
      const int pw = w[levels - 1], ph = h[levels - 1];
      const int nw = (pw + 1) / 2, nh = (ph + 1) / 2;
      std::vector<float> next(size_t(nw) * nh);
      for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
          float m = 1e30f;
          for (int k = 0; k < 4; ++k) {
            const int sx = std::min(x * 2 + (k & 1), pw - 1);
            const int sy = std::min(y * 2 + (k >> 1), ph - 1);
            m = std::min(m, level[levels - 1][size_t(sy) * pw + sx]);
          }
          next[size_t(y) * nw + x] = m;
        }
      }
      level.push_back(next);
      w[levels] = nw;
      h[levels] = nh;
      ++levels;
    }
  }
  void Install() const {
    hs::g_hiz.levels = levels;
    for (int l = 0; l < levels; ++l) {
      hs::g_hiz.w[l] = w[l];
      hs::g_hiz.h[l] = h[l];
      hs::g_hiz.data[l] = level[l].data();
    }
  }
};

struct Box {
  double lo[3], hi[3];
};
// the nearest entry of the ray (origin o, direction d) into a box: s and the face's normal; false when none (s > 0)
bool HitBox(const Box& b, const double o[3], const double d[3], double* s_out, double normal[3]) {
  double t0 = 1e-6, t1 = 1e30;
  int axis = -1;
  double sign = 0.0;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(d[a]) < 1e-12) {
      if (o[a] < b.lo[a] || o[a] > b.hi[a]) return false;
      continue;
    }
    double ta = (b.lo[a] - o[a]) / d[a], tb = (b.hi[a] - o[a]) / d[a];
    double face_sign = -1.0;  // entering through the low face: its normal points to -axis
    if (ta > tb) {
      std::swap(ta, tb);
      face_sign = 1.0;
    }
    if (ta > t0) {
      t0 = ta;
      axis = a;
      sign = face_sign;
    }
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  if (axis < 0) return false;
  *s_out = t0;
  normal[0] = normal[1] = normal[2] = 0.0;
  normal[axis] = sign;
  return true;
}

// The depth (z) of the nearest box along the pixel's ray from the camera at the origin, kSky for none.
struct World {
  std::vector<Box> boxes;
  float DepthAt(double x_prime, double y_prime, double normal[3] = nullptr) const {
    const double o[3] = {0, 0, 0};
    const double d[3] = {x_prime, y_prime, 1.0};
    double best = 1e30;
    for (const Box& b : boxes) {
      double s, n[3];
      if (HitBox(b, o, d, &s, n) && s < best) {
        best = s;
        if (normal) {
          normal[0] = n[0];
          normal[1] = n[1];
          normal[2] = n[2];
        }
      }
    }
    return best < 1e29 ? float(best) : hs::kSsrSky;  // d.z is 1, so s is the distance along z
  }
};

std::vector<float> Render(const World& world, const Lens& lens, int w, int h) {
  std::vector<float> depth(size_t(w) * h);
  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      const double xp = (2.0 * (i + 0.5) / w - 1.0) * lens.tan_x;
      const double yp = (1.0 - 2.0 * (j + 0.5) / h) * lens.tan_y;
      depth[size_t(j) * w + i] = world.DepthAt(xp, yp);
    }
  }
  return depth;
}

// the texel of a view space point
void Project(const Lens& lens, int w, int h, double x, double y, double z, double* px, double* py) {
  *px = (0.5 + 0.5 * x / (lens.tan_x * z)) * w;
  *py = (0.5 - 0.5 * y / (lens.tan_y * z)) * h;
}

// A dense march of the full resolution depth: the reference the Hi-Z walk is checked against. It walks the same ray (the same start, the same end, the same surface test) in small screen space
// steps and returns the first texel where the ray is behind the surface by no more than its thickness.
struct BruteHit {
  bool hit = false;
  double px = 0, py = 0;
};
BruteHit BruteTrace(const hs::SsrTraceParams& p, const std::vector<float>& depth, double ox, double oy, double oz, double rx, double ry, double rz) {
  BruteHit out;
  const double z_clip = std::max<double>(p.near_plane, 0.05) * 1.02;
  double len = p.max_distance;
  if (rz < 0.0) len = std::min(len, (z_clip - oz) / rz);
  if (!(len > 1e-3)) return out;
  const double ex = ox + rx * len, ey = oy + ry * len, ez = oz + rz * len;
  const auto pix_x = [&](double x, double z) { return (0.5 + 0.5 * x / (p.tan_x * z)) * p.width; };
  const auto pix_y = [&](double y, double z) { return (0.5 - 0.5 * y / (p.tan_y * z)) * p.height; };
  const auto dof = [&](double z) { return (p.lin_x * z - p.lin_z) / (p.lin_y * z - p.lin_w); };
  const double px0 = pix_x(ox, oz), py0 = pix_y(oy, oz), d0 = dof(oz);
  const double dx = pix_x(ex, ez) - px0, dy = pix_y(ey, ez) - py0, dd = dof(ez) - d0;
  const double major = std::max(std::fabs(dx), std::fabs(dy));
  if (!(major > 1e-6)) return out;
  const double step = 0.2 / major;
  for (double tau = 1.5 / major; tau <= 1.0; tau += step) {
    const double px = px0 + dx * tau, py = py0 + dy * tau, d = d0 + dd * tau;
    if (px < 0 || py < 0 || px >= p.width || py >= p.height) return out;
    const double zs = depth[size_t(int(py)) * size_t(p.width_i) + size_t(int(px))];
    if (zs >= hs::kSsrSky * 0.5) continue;
    const double ds = dof(zs);
    if (d >= ds) {
      const double zr = (p.lin_z - d * p.lin_w) / (p.lin_x - d * p.lin_y);
      const double thick = std::max<double>(hs::kSsrThickAbs, p.thick_rel * zs);
      if (zr - zs <= thick) {
        out.hit = true;
        out.px = px;
        out.py = py;
        return out;
      }
    }
  }
  return out;
}

void TestMarchAnalytic() {
  // a road (y = -1.5), a wall at z = 25 (x -7..7, up to 9), a pole at (3, 12) 0.3 wide up to 3 m: boxes stand for them
  const Lens lens;
  const int W = 480, H = 270;
  World world;
  world.boxes.push_back({{-1000, -3.0, 0.0}, {1000, -1.5, 2000}});          // the road: a thick slab under y = -1.5
  world.boxes.push_back({{-7.0, -3.0, 25.0}, {7.0, 9.0, 26.0}});             // the wall's front face at z = 25
  world.boxes.push_back({{2.85, -3.0, 11.85}, {3.15, 3.0, 12.15}});          // the pole
  const std::vector<float> depth = Render(world, lens, W, H);
  Pyramid pyr;
  pyr.Build(depth, W, H);
  pyr.Install();
  Check(pyr.levels == 7, "the test's Hi-Z has seven levels");
  const hs::SsrTraceParams params = MakeParams(lens, W, H, int(pyr.levels), 64);
  int rays = 0, expected_hits = 0, agree = 0, wrong_pos = 0, false_hit = 0, missed = 0;
  int sky_rays = 0, sky_inside = 0;
  long iterations = 0, iterations_sky = 0;
  for (int j = H / 2 + 6; j < H; j += 3) {
    for (int i = 4; i < W - 4; i += 5) {
      const double xp = (2.0 * (i + 0.5) / W - 1.0) * lens.tan_x;
      const double yp = (1.0 - 2.0 * (j + 0.5) / H) * lens.tan_y;
      if (yp >= -1e-3) continue;
      const double z = -1.5 / yp;  // the road's depth along z (y = -1.5 at the pixel)
      if (z > 60.0) continue;
      const double P[3] = {xp * z, yp * z, z};
      const double len = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
      const double R[3] = {P[0] / len, -P[1] / len, P[2] / len};  // the mirror about the road's plane (normal +y)
      const double bias = hs::kSsrNormalBias + hs::kSsrNormalBiasShare * z;
      const double O[3] = {P[0], P[1] + bias, P[2]};
      // the analytic hit: the first of the wall and the pole along the ray
      double best = 1e30;
      for (size_t b = 1; b < world.boxes.size(); ++b) {
        double s, n[3];
        if (HitBox(world.boxes[b], O, R, &s, n)) best = std::min(best, s);
      }
      bool expect_hit = best < 1e29;
      double epx = 0, epy = 0;
      if (expect_hit) {
        const double H3[3] = {O[0] + R[0] * best, O[1] + R[1] * best, O[2] + R[2] * best};
        if (H3[2] > 0.2 && best < params.max_distance) {
          Project(lens, W, H, H3[0], H3[1], H3[2], &epx, &epy);
          if (epx < 0 || epy < 0 || epx >= W || epy >= H) expect_hit = false;  // past the picture's edge: the screen has nothing of it
        } else {
          expect_hit = false;
        }
      }
      const hs::SsrHit hit = hs::SsrTrace(params, float(O[0]), float(O[1]), float(O[2]), float(R[0]), float(R[1]), float(R[2]));
      ++rays;
      iterations += hit.iterations;
      if (!expect_hit) {
        if (hit.hit) {
          ++false_hit;
        } else {
          ++agree;
          ++sky_rays;
          iterations_sky += hit.iterations;
          sky_inside += hit.inside_end ? 1 : 0;
        }
        continue;
      }
      ++expected_hits;
      if (!hit.hit) {
        ++missed;
        continue;
      }
      if (std::fabs(hit.px - epx) <= 2.5 && std::fabs(hit.py - epy) <= 2.5) {
        ++agree;
      } else {
        ++wrong_pos;
        if (g_print && wrong_pos < 8) std::printf("  wrong: pixel (%d,%d) expected (%.1f,%.1f) got (%.1f,%.1f)\n", i, j, epx, epy, hit.px, hit.py);
      }
    }
  }
  if (g_print) {
    std::printf("[march analytic] %d rays, %d expected hits: %d agree, %d wrong place, %d missed, %d false hits; sky rays %d (inside end %d); mean iterations %.1f (sky %.1f)\n", rays, expected_hits, agree,
                wrong_pos, missed, false_hit, sky_rays, sky_inside, double(iterations) / rays, sky_rays ? double(iterations_sky) / sky_rays : 0.0);
  }
  Check(rays > 1500 && expected_hits > 300, Num("the scene has rays (%.0f) and hits (%.0f)", rays, expected_hits));
  Check(missed <= expected_hits / 33, Num("rays that should hit the wall or the pole find it: %.0f missed of %.0f", missed, expected_hits));
  Check(wrong_pos <= expected_hits / 50, Num("and at the right texel (within 2.5): %.0f wrong of %.0f", wrong_pos, expected_hits));
  Check(false_hit <= rays / 50, Num("rays that should find nothing find nothing: %.0f false hits of %.0f", false_hit, rays));
  Check(double(iterations) / rays < 30.0, Num("the walk is short: %.1f iterations a ray on average (a dense march needs hundreds)", double(iterations) / rays));
}

void TestMarchBrute() {
  // random block scenes: a ground slab and thirty boxes; random rays from random surface points into the hemisphere; the Hi-Z walk against the dense march of the same depth image
  Lens lens;
  lens.tan_x = 0.75;
  lens.tan_y = 0.42;
  const int W = 320, H = 180;
  Lcg rng;
  int rays = 0, both_hit = 0, both_miss = 0, hiz_only = 0, brute_only = 0, close = 0;
  long iterations = 0;
  for (int scene = 0; scene < 6; ++scene) {
    World world;
    world.boxes.push_back({{-1000, -3.0, 0.0}, {1000, -1.5, 2000}});
    for (int b = 0; b < 30; ++b) {
      const double cx = rng.Range(-30, 30), cz = rng.Range(6, 70), sx = rng.Range(0.3, 5), sz = rng.Range(0.3, 5), hy = rng.Range(0.5, 9);
      world.boxes.push_back({{cx - sx, -3.0, cz - sz}, {cx + sx, -1.5 + hy, cz + sz}});
    }
    const std::vector<float> depth = Render(world, lens, W, H);
    Pyramid pyr;
    pyr.Build(depth, W, H);
    pyr.Install();
    const hs::SsrTraceParams params = MakeParams(lens, W, H, pyr.levels, 80);
    for (int k = 0; k < 700; ++k) {
      const int i = int(rng.Range(8, W - 8)), j = int(rng.Range(H * 0.3f, H - 3.0f));
      const double xp = (2.0 * (i + 0.5) / W - 1.0) * lens.tan_x;
      const double yp = (1.0 - 2.0 * (j + 0.5) / H) * lens.tan_y;
      double n[3] = {0, 0, 0};
      const float z = world.DepthAt(xp, yp, n);
      if (z > 90.0f) continue;
      const double P[3] = {xp * z, yp * z, double(z)};
      // a random direction in the hemisphere of the surface's normal, rejecting the ones that point very flat
      double R[3];
      do {
        R[0] = rng.Range(-1, 1);
        R[1] = rng.Range(-1, 1);
        R[2] = rng.Range(-1, 1);
        const double l = std::sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
        if (l < 0.2) continue;
        R[0] /= l;
        R[1] /= l;
        R[2] /= l;
      } while (R[0] * n[0] + R[1] * n[1] + R[2] * n[2] < 0.15);
      const double bias = hs::kSsrNormalBias + hs::kSsrNormalBiasShare * z;
      const double O[3] = {P[0] + n[0] * bias, P[1] + n[1] * bias, P[2] + n[2] * bias};
      if (O[2] <= 0.3) continue;
      const hs::SsrHit hit = hs::SsrTrace(params, float(O[0]), float(O[1]), float(O[2]), float(R[0]), float(R[1]), float(R[2]));
      const BruteHit ref = BruteTrace(params, depth, O[0], O[1], O[2], R[0], R[1], R[2]);
      ++rays;
      iterations += hit.iterations;
      if (hit.hit && ref.hit) {
        ++both_hit;
        if (std::fabs(hit.px - ref.px) <= 3.0 && std::fabs(hit.py - ref.py) <= 3.0) ++close;
      } else if (!hit.hit && !ref.hit) {
        ++both_miss;
      } else if (hit.hit) {
        ++hiz_only;
      } else {
        ++brute_only;
        if (g_print && brute_only <= 12) {
          std::printf("  dense only: pixel (%d,%d) z %.2f origin (%.2f,%.2f,%.2f) dir (%.3f,%.3f,%.3f) dense hit at (%.1f,%.1f) iterations %u inside_end %d\n", i, j, z, O[0], O[1], O[2], R[0], R[1], R[2], ref.px, ref.py, hit.iterations,
                      hit.inside_end ? 1 : 0);
        }
      }
    }
  }
  if (g_print) {
    std::printf("[march brute] %d rays: both hit %d (%d within 3 texels), both miss %d, Hi-Z only %d, dense only %d, mean iterations %.1f\n", rays, both_hit, close, both_miss, hiz_only, brute_only,
                double(iterations) / rays);
  }
  Check(rays > 3000 && both_hit > 600 && both_miss > 600, Num("the random scenes give hits (%.0f) and misses (%.0f)", both_hit, both_miss));
  Check(double(both_hit + both_miss) / rays > 0.95, Num("the Hi-Z walk and the dense march agree on hit or miss in %.1f%% of the rays", 100.0 * (both_hit + both_miss) / rays));
  Check(double(close) / std::max(1, both_hit) > 0.93, Num("and on the texel (within 3) in %.1f%% of the hits", 100.0 * close / std::max(1, both_hit)));
  Check(double(iterations) / rays < 45.0, Num("mean iterations %.1f", double(iterations) / rays));
}

void TestMarchEdges() {
  const Lens lens;
  const int W = 320, H = 180;
  World world;
  world.boxes.push_back({{-1000, -3.0, 0.0}, {1000, -1.5, 2000}});
  const std::vector<float> depth = Render(world, lens, W, H);
  Pyramid pyr;
  pyr.Build(depth, W, H);
  pyr.Install();
  const hs::SsrTraceParams params = MakeParams(lens, W, H, pyr.levels, 64);
  // a road alone: a mirror ray to the sky ends on the picture (the vanishing point of its direction) when that is on the picture
  const double z = 12.0, yp = -1.5 / z, xp = 0.2;
  const double P[3] = {xp * z, yp * z, z};
  const double len = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
  const double R[3] = {P[0] / len, -P[1] / len, P[2] / len};
  const hs::SsrHit sky = hs::SsrTrace(params, float(P[0]), float(P[1] + 0.07), float(P[2]), float(R[0]), float(R[1]), float(R[2]));
  Check(!sky.hit && sky.inside_end, "a mirror ray up into the sky on a bare road misses and ends on the picture");
  // where it ends: the far end of the ray (150 m): the projection of the point at the longest length
  const double L = params.max_distance;
  double epx, epy;
  Project(lens, W, H, P[0] + R[0] * L, P[1] + 0.07 + R[1] * L, P[2] + R[2] * L, &epx, &epy);
  Check(Near(sky.px, epx, 0.5) && Near(sky.py, epy, 0.5), Num("the end is the projection of the ray's far end (%.2f, ", sky.px) + Num("%.2f) against ", sky.py) + Num("(%.2f, ", epx) + Num("%.2f)", epy));
  // a ray that leaves the picture sideways is a miss and does not end inside
  const hs::SsrHit side = hs::SsrTrace(params, float(P[0]), float(P[1] + 0.07), float(P[2]), 0.9f, 0.2f, 0.3f);
  Check(!side.hit && !side.inside_end, "a ray that leaves the picture ends outside it");
  // a ray pointing back at the camera ends before the near plane and has no hit on a bare road
  const hs::SsrHit back = hs::SsrTrace(params, float(P[0]), float(P[1] + 0.07), float(P[2]), 0.1f, 0.3f, -0.9f);
  Check(!back.hit, "a ray back at the camera finds nothing on a bare road");
  // a degenerate ray along the view direction does not hit itself or loop
  const hs::SsrHit along = hs::SsrTrace(params, float(P[0]), float(P[1] + 0.07), float(P[2]), float(P[0] / len), float(P[1] / len), float(P[2] / len));
  Check(!along.hit && along.iterations < 8, "a ray along the view direction has no screen movement: no hit, no loop");
  // an origin off the picture: no trace
  const hs::SsrHit off = hs::SsrTrace(params, 50.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f);
  Check(!off.hit && off.iterations == 0, "an origin off the picture is not traced");
  // the iteration cap ends the walk
  hs::SsrTraceParams capped = params;
  capped.max_iter = 3;
  const hs::SsrHit few = hs::SsrTrace(capped, float(P[0]), float(P[1] + 0.07), float(P[2]), float(R[0]), float(R[1]), float(R[2]));
  Check(few.iterations <= 3 && !few.inside_end, "the iteration cap ends the walk and says it did not reach the end");
  // the hit's facing: a flat wall facing the camera has a normal facing the ray's origin
  World wall;
  wall.boxes.push_back({{-1000, -3.0, 0.0}, {1000, -1.5, 2000}});
  wall.boxes.push_back({{-50, -3.0, 30.0}, {50, 40.0, 31.0}});
  const std::vector<float> dw = Render(wall, lens, W, H);
  Pyramid pw;
  pw.Build(dw, W, H);
  pw.Install();
  const hs::SsrTraceParams pr = MakeParams(lens, W, H, pw.levels, 64);
  const hs::SsrHit wh = hs::SsrTrace(pr, float(P[0]), float(P[1] + 0.07), float(P[2]), float(R[0]), float(R[1]), float(R[2]));
  Check(wh.hit && Near(wh.surface_z, 30.0, 0.3), Num("a wall ahead is hit at its depth (%.2f)", wh.surface_z));
  if (wh.hit) {
    const float facing = hs::SsrHitFacing(pr, int(wh.px), int(wh.py), wh.surface_z, float(R[0]), float(R[1]), float(R[2]));
    Check(facing < -0.3f, Num("the wall faces the ray's origin: facing %.2f", facing));
    // the sign test: a ray that leaves the camera's side of the wall along the wall's own normal away from the camera has a positive product (a back face the screen does not show)
    const float away = hs::SsrHitFacing(pr, int(wh.px), int(wh.py), wh.surface_z, -float(R[0]), -float(R[1]), -float(R[2]));
    Check(away > 0.3f, "a ray that points away from the camera along the wall's own normal has a positive facing");
  }
}


// ---------------------------------------------------------------------------
// the small pure functions the shaders' bodies use (the lens, the block pick, the positions)
// ---------------------------------------------------------------------------

void TestLensFunctions() {
  const Lens lens;
  const hs::SsrTraceParams p = MakeParams(lens, 480, 270, 7, 64);
  // the stored depth (1 near, 0 far) of a distance and back: SsrLinearDepth is ao_common.hlsli AoLinearDepth, SsrDOfZ its inverse in the ndc coordinate the march uses
  bool round = true;
  for (double z : {0.2, 1.0, 5.0, 25.0, 120.0, 900.0}) {
    const float d = hs::SsrDOfZ(p, float(z));
    const float stored = 1.0f - d;
    const float back = hs::SsrLinearDepth(stored, p.lin_x, p.lin_y, p.lin_z, p.lin_w);
    const float zz = hs::SsrZOfD(p, d);
    round = round && Near(back, z, z * 2e-3) && Near(zz, z, z * 2e-3);
  }
  Check(round, "the distance, its depth coordinate and the stored depth are one another's inverse");
  Check(hs::SsrLinearDepth(0.0f, p.lin_x, p.lin_y, p.lin_z, p.lin_w) == hs::kSsrSky && hs::SsrLinearDepth(-1.0f, p.lin_x, p.lin_y, p.lin_z, p.lin_w) == hs::kSsrSky, "the far plane's stored 0 is the sky");
  bool monotone = true;
  float last = -1.0f;
  for (int i = 1; i < 200; ++i) {
    const float d = hs::SsrDOfZ(p, 0.1f + 0.5f * float(i));
    monotone = monotone && d > last;
    last = d;
  }
  Check(monotone, "the depth coordinate grows with the distance");
  // the position of a pixel centre and the pixel of a position are inverse
  bool pos_ok = true;
  for (int i = 0; i < 20; ++i) {
    const float cx = 10.5f + 23.0f * float(i), cy = 7.5f + 11.0f * float(i % 7);
    const float z = 3.0f + float(i);
    const hs::float3 v = hs::SsrViewPos(cx, cy, 480.0f, 270.0f, z, p.tan_x, p.tan_y);
    pos_ok = pos_ok && Near(v.z, z, 1e-5) && Near(hs::SsrPixelX(p, v.x, v.z), cx, 1e-3) && Near(hs::SsrPixelY(p, v.y, v.z), cy, 1e-3);
  }
  Check(pos_ok, "a pixel's view position and the texel of a position are inverse (the AO's AoViewPos)");
  // the block's pick: the pixel nearest the Hi-Z's depth, ties to the first
  Check(hs::SsrPickInBlock(10.0f, 10.0f, 10.0f, 10.0f, 10.0f) == 0u && hs::SsrPickInBlock(30.0f, 10.2f, 31.0f, 29.0f, 10.0f) == 1u && hs::SsrPickInBlock(30.0f, 31.0f, 10.0f, 29.0f, 10.0f) == 2u &&
            hs::SsrPickInBlock(30.0f, 31.0f, 29.0f, 10.0f, 10.0f) == 3u && hs::SsrPickInBlock(5.0f, 40.0f, 41.0f, 42.0f, 5.0f) == 0u,
        "the receiver is the pixel of the 2x2 block whose depth is the Hi-Z's");
  Check(hs::SsrLevelSize(1080, 0) == sc::HizLevelSize(1080, 0) && hs::SsrLevelSize(1080, 3) == sc::HizLevelSize(1080, 3) && hs::SsrLevelSize(1081, 5) == sc::HizLevelSize(1081, 5) && hs::SsrLevelSize(7, 2) == sc::HizLevelSize(7, 2) &&
            hs::SsrLevelSize(1, 4) == sc::HizLevelSize(1, 4),
        "the walk's level sizes are the Hi-Z chain's (round up)");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

std::string Read(const std::string& dir, const std::string& rel) {
  std::string text;
  if (!Slurp(dir + "/" + rel, &text)) {
    Check(false, "the source " + rel + " can be read");
  }
  return text;
}
// the text of a function from its first line to the next function at column 0 (a rough but stable cut)
std::string FunctionText(const std::string& text, const std::string& head) {
  const size_t at = text.find(head);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
// "SSR_W_" macro names used by a shader text
std::vector<std::string> WordsUsed(const std::string& text) {
  std::vector<std::string> out;
  for (size_t at = text.find("SSR_W_"); at != std::string::npos; at = text.find("SSR_W_", at + 6)) {
    size_t end = at + 6;
    while (end < text.size() && (std::isupper(static_cast<unsigned char>(text[end])) || std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
    const std::string name = text.substr(at, end - at);
    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
  }
  return out;
}

void TestSources(const std::string& dir) {
  if (dir.empty()) return;
  const std::string rules = Read(dir, "ssr_rules.h"), header = Read(dir, "ssr.h"), cpp = Read(dir, "ssr.cpp"), scene = Read(dir, "scene.cpp"), scene_h = Read(dir, "scene.h"), renderer = Read(dir, "renderer.cpp"),
                    renderer_h = Read(dir, "renderer.h"), cmake = Read(dir, "CMakeLists.txt"), shaders_h = Read(dir, "host_shaders.h"), precache = Read(dir, "host_precache.h"), ao_h = Read(dir, "ao.h"), ao_cpp = Read(dir, "ao.cpp"),
                    common = Read(dir, "hlsl/ssr_common.hlsli"), pass = Read(dir, "hlsl/ssr_pass.hlsli"), trace_ps = Read(dir, "hlsl/ssr_trace_ps.hlsl"), temporal_ps = Read(dir, "hlsl/ssr_temporal_ps.hlsl"),
                    apply_ps = Read(dir, "hlsl/ssr_apply_ps.hlsl"), trace_h = Read(dir, "hlsl/ssr_trace.hlsli");
  const std::string settings = Read(dir, "../settings/generated/settings_gpu.h"), manifest = Read(dir, "../settings/generated/manifest.json"), meta = Read(dir, "../settings/generated/settings_meta.inc");

  // the setting: off by default, every one debug-only, in the three generated files
  // (the declaration's text is built here, not written out: tools\check_settings_exposure.py reads every REX_SETTING( in the sources, this file's too)
  const std::string rex_setting = std::string("REX_") + "SETTING(";
  Check(Has(settings, rex_setting + "std::string, mcla_gpu_ssr, \"off\"") && Has(settings, "rex::kDebugOnly, rex::Allowed(\"off\", \"high\", \"ultra\")"), "mcla_gpu_ssr is off by default and debug-only");
  bool every = true;
  for (const char* name : {"mcla_gpu_ssr", "mcla_gpu_ssr_view", "mcla_gpu_ssr_strength", "mcla_gpu_ssr_min_smooth", "mcla_gpu_ssr_stub", "mcla_gpu_ssr_wet", "mcla_gpu_ssr_history"}) {
    const std::string n = name;
    const size_t decl = settings.find(", " + n + ", ");
    const size_t end = decl == std::string::npos ? std::string::npos : settings.find(");\n", decl);
    const bool debug = decl != std::string::npos && end != std::string::npos && settings.substr(decl, end - decl).find("rex::kDebugOnly") != std::string::npos;
    const bool in_alias = Has(settings, "inline auto& " + n + " = g_gpu." + n + ";");
    const bool in_manifest = Has(manifest, "\"" + n + "\": {");
    const bool in_meta = Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},");
    if (!(debug && in_alias && in_manifest && in_meta)) std::printf("setting %s: debug %d alias %d manifest %d meta %d\n", name, debug, in_alias, in_manifest, in_meta);
    every = every && debug && in_alias && in_manifest && in_meta;
  }
  Check(every, "all seven settings are debug-only and in the settings header, its aliases, the manifest and the meta list");

  // what the effect asks of the owners, and that it asks for nothing while it is off
  const std::string begin = FunctionText(cpp, "void ScreenReflections::BeginFrame()");
  Check(Has(begin, "screen_->Want(screen::Consumer::kReflections, on ? (screen::ProductBit(screen::kHizMin) | screen::ProductBit(screen::kColourPost)) : 0u)") && Has(begin, "screen_->WantCamera(screen::Consumer::kReflections, on)") &&
            Has(begin, "gbuffer_->Want(gbuffer::Consumer::kReflections, on ? gbuffer::kAllPlanes : 0u)"),
        "BeginFrame asks for the Hi-Z, colour_post, the camera and the G-buffer's planes only while on (0 withdraws)");
  Check(!Has(begin, "kHistory"), "the owner's history product is not asked for (the accumulation keeps its own images)");
  const std::string trigger = FunctionText(cpp, "bool ScreenReflections::OnTrigger(");
  const size_t first_command = std::min({trigger.find("DrawPass("), trigger.find("list->"), trigger.find("edram_->Transition")});
  const size_t off_return = trigger.find("level_ == ssr::Level::kOff");
  Check(off_return != std::string::npos && off_return < first_command, "OnTrigger returns before recording anything while the effect is off");
  Check(Has(FunctionText(cpp, "void ScreenReflections::OnMainDraw("), "level_ == ssr::Level::kOff") && Has(cpp, "bool wants_draws() const") == false && Has(header, "bool wants_draws() const { return level_ != ssr::Level::kOff; }"),
        "the wetness hook does nothing while off");
  Check(Has(trigger, "trigger_done_ = true;") && Has(trigger, "ssr::Decide(facts)") && Has(trigger, "ssr::HistoryUsable(") && Has(trigger, "ao_->FrameCameraNow(&cam)"), "once a frame, by the rules' decision, with the history's rule and the AO's camera");
  Check(Has(trigger, "facts.has_rt3 = gbuffer_current && gf.Has(gbuffer::kPlaneMaterial) && !force_stub_"), "RT3 is used where the frame has it; the stub where not (or by the debug setting)");

  // the order at the trigger: the capture, then the reflections, then the car's history
  const size_t capture_at = scene.find("colour_captured = screen_->CapturePost(record, summary, list);");
  const size_t ssr_at = scene.find("ssr_->OnTrigger(summary, list);");
  const size_t car_at = scene.find("car_refl_->OnResolve(record, summary, list);");
  Check(capture_at != std::string::npos && ssr_at != std::string::npos && car_at != std::string::npos && capture_at < ssr_at && ssr_at < car_at, "scene.cpp: the colour capture, then the reflections, then the car self-reflection's history");
  Check(Has(scene, "if (colour_captured && ssr_) {") && Has(scene, "if (ssr_ && ssr_->wants_draws()) {\n    ssr_->OnMainDraw(draw.match, banks);") && Has(scene, "ssr_->BeginFrame();") && Has(scene, "ssr_->OnPresent();"),
        "scene.cpp: the reflections run only in a frame that made the capture, hear the draws only while on, begin and end the frame");
  Check(Has(scene_h, "void SetScreenReflections(ScreenReflections* ssr)") && Has(scene_h, "ScreenReflections* ssr_ = nullptr;"), "scene.h holds the pointer");
  const size_t init_at = renderer.find("ssr_.Initialize(device_, heap_, passes_, edram_, pass_timing_);");
  const size_t screen_init = renderer.find("screen_.Initialize(passes_, edram_, pass_timing_);");
  Check(init_at != std::string::npos && screen_init != std::string::npos && screen_init < init_at && Has(renderer, "ssr_.SetScreenShared(&screen_);") && Has(renderer, "ssr_.SetGBuffer(&gbuffer_);") &&
            Has(renderer, "ssr_.SetAmbientOcclusion(&ao_);") && Has(renderer, "scene_.SetScreenReflections(&ssr_);"),
        "renderer.cpp: made after the shared products, wired to the owners and the scene");
  size_t shutdowns = 0;
  bool before_ao = true;
  for (size_t at = renderer.find("ssr_.Shutdown();"); at != std::string::npos; at = renderer.find("ssr_.Shutdown();", at + 1)) {
    ++shutdowns;
    const size_t ao_at = renderer.find("ao_.Shutdown();", at);
    before_ao = before_ao && ao_at != std::string::npos && ao_at - at < 400;  // the light pass's Shutdown (GB-H1) sits between them in the merged phase 2 branch
  }
  Check(shutdowns == 2 && before_ao, "renderer.cpp: shut down before the AO it holds a pointer to, in both Shutdown paths");
  Check(Has(renderer, "ssr_.HeldBytes()") && Has(renderer_h, "ScreenReflections ssr_;") && Has(renderer_h, "#include \"ssr.h\""), "renderer.h / renderer.cpp: the memory line and the member");

  // the build: the sources, the shaders, the test, the registration of the blobs, the precache owner
  Check(Has(cmake, "    ssr.cpp\n") && Has(cmake, "add_executable(larecomp_ssr_test EXCLUDE_FROM_ALL ssr_test.cpp)"), "CMake: the source and the test");
  for (const char* stem : {"ssr_trace_ps", "ssr_trace_ms_ps", "ssr_temporal_ps", "ssr_apply_ps", "ssr_apply_ms_ps"}) {
    const std::string s = stem;
    Check(Has(cmake, "larecomp_gpu_host_shader(" + s + " ps_6_0") && Has(shaders_h, "#include \"" + s + ".hlsl.dxil.h\"") && Has(shaders_h, "#include \"" + s + ".hlsl.spirv.h\"") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(" + s + ")"),
          "the shader " + s + " is built, included for both backends and returned by ScreenReflectionsPixelShader");
  }
  Check(Has(cmake, "SOURCE ssr_trace_ps DEFINES SSR_MS=1") && Has(cmake, "SOURCE ssr_apply_ps DEFINES SSR_MS=1") && Has(cmake, "ssr_trace_ps ps_6_0 DEFINES SSR_MS=0") && Has(cmake, "ssr_apply_ps ps_6_0 DEFINES SSR_MS=0"),
        "the plain and multisampled compilations of the trace and the apply");
  Check(Has(precache, "kScreenReflections = 24") && Has(cpp, "HostPrecache::kScreenReflections"), "its own owner number in the host precache");

  // the passes' words: every word a shader reads is one the macros define and the host fills
  const std::pair<const char*, const char*> kNames[] = {
      {"SSR_W_DEPTH_RT1", "kDepthRt1"}, {"SSR_W_RT2_RT3", "kRt2Rt3"}, {"SSR_W_SRC", "kSrc"},   {"SSR_W_HIZ01", "kHiz01"},   {"SSR_W_HIZ23", "kHiz23"}, {"SSR_W_HIZ45", "kHiz45"},
      {"SSR_W_COL01", "kCol01"},        {"SSR_W_COL23", "kCol23"},   {"SSR_W_RENDER", "kRender"}, {"SSR_W_FLAGS", "kFlags"}, {"SSR_W_FRAME", "kFrame"},   {"SSR_W_LIN0", "kLin0"},
      {"SSR_W_TAN_X", "kTanX"},         {"SSR_W_TAN_Y", "kTanY"},    {"SSR_W_LIN_X", "kLinX"},    {"SSR_W_LIN_Y", "kLinY"},   {"SSR_W_LIN_Z", "kLinZ"},   {"SSR_W_LIN_W", "kLinW"},
      {"SSR_W_NEAR", "kNear"},          {"SSR_W_MAX_DIST", "kMaxDist"}, {"SSR_W_RIGHT", "kRight"}, {"SSR_W_UP", "kUp"},        {"SSR_W_STRENGTH", "kStrength"}, {"SSR_W_MIN_SMOOTH", "kMinSmooth"},
      {"SSR_W_STUB_SMOOTH", "kStubSmooth"}, {"SSR_W_WET", "kWet"},   {"SSR_W_THICK", "kThick"},   {"SSR_W_CAP", "kCap"},      {"SSR_W_MOTION", "kMotion"}, {"SSR_W_PREV_TAN_X", "kPrevTanX"},
      {"SSR_W_PREV_TAN_Y", "kPrevTanY"}, {"SSR_W_HISTORY_WEIGHT", "kHistoryWeight"}};
  bool defined = true, filled = true;
  for (const std::string* text : {&pass, &trace_ps, &temporal_ps, &apply_ps}) {
    for (const std::string& word : WordsUsed(*text)) {
      bool known = false;
      for (const auto& kn : kNames) {
        if (word == kn.first) {
          known = true;
          const bool in_common = Has(common, "#define " + word + " ");
          const bool set_by_host = Has(cpp, std::string("pk::") + kn.second);
          if (!in_common) std::printf("word %s: not defined in ssr_common.hlsli\n", word.c_str());
          if (!set_by_host) std::printf("word %s: not set by ssr.cpp\n", word.c_str());
          defined = defined && in_common;
          filled = filled && set_by_host;
        }
      }
      if (!known) {
        std::printf("word %s: not in the test's table\n", word.c_str());
        defined = false;
      }
    }
  }
  Check(defined, "every SSR_W word the shaders read is defined in ssr_common.hlsli (and known to this test)");
  Check(filled, "and every one is set by ssr.cpp (pk::k...)");
  Check(Has(pass, "struct SsrPush {\n  uint4 r[8];") && Has(cpp, "builder.addPushConstant(0, 4, sizeof(ssr::Push)"), "the shaders' push block is 8 x uint4 = 128 bytes, the layout's push range the host struct's");
  Check(!Has(pass, "NonUniformResourceIndex(") && !Has(trace_ps, "NonUniformResourceIndex(") && !Has(temporal_ps, "NonUniformResourceIndex(") && !Has(apply_ps, "NonUniformResourceIndex("),
        "no shader needs the device's non uniform descriptor indexing: every slot read is a value of the push block");
  Check(Has(pass, "float SsrHizAt(uint level, int cx, int cy) {\n  if (level == 0u) {\n    return g_textures[SsrLo(SsrW(SSR_W_HIZ01))]"), "the Hi-Z is read through a chain of branches on the level, one push-block slot each");
  Check(Has(trace_ps, "#include \"ssr_trace.hlsli\"") && Has(pass, "#define SSR_LOOP [loop]") && Has(trace_h, "SSR_LOOP for (; i < p.max_iter; ++i)"), "the march's loop is [loop] in the shader and plain C++ in the test");
  // the pass's inputs: the trace reads the class gate before any walk
  const size_t gate_at = trace_ps.find("SsrClassGain(surface.cls, surface.flags)");
  const size_t walk_at = trace_ps.find("SsrTrace(tp,");
  Check(gate_at != std::string::npos && walk_at != std::string::npos && gate_at < walk_at && Has(trace_ps, "if (!(gain > 0.0f) || smoothness < SsrF(SSR_W_MIN_SMOOTH)) {\n    return float4(0.0f, 0.0f, 0.0f, 0.0f);"),
        "the trace gates by class and smoothness before it walks (a pixel that reflects nothing costs the gate's reads)");
  Check(Has(apply_ps, "if (!reflects) {\n    return float4(0.0f, 0.0f, 0.0f, 0.0f);") && Has(apply_ps, "return float4(term, 0.0f);"), "the apply adds 0 where the class keeps its own reflection and writes no alpha");
  Check(Has(cpp, "blend.srcBlend = plume::RenderBlend::ONE;") && Has(cpp, "blend.dstBlend = plume::RenderBlend::ONE;") && Has(cpp, "blend.renderTargetWriteMask = kWriteRgb;"), "the apply pipeline adds and leaves alpha alone");
  Check(Has(rules, "kStagePlanes") == false && Has(cpp, "gbuffer::kAllPlanes"), "all three planes are asked for (RT3 may not be written yet: the frame then has two)");

  // the AO's accessor is a read and nothing else
  const std::string accessor = FunctionText(ao_cpp, "bool AmbientOcclusion::FrameCameraNow(FrameCameraOut* out) const {");
  Check(!accessor.empty() && Has(ao_h, "bool FrameCameraNow(FrameCameraOut* out) const;") && Has(accessor, "FrameCameraRows(&rows)") && Has(accessor, "FrameMotion(&motion, &pose, &cut)") && Has(accessor, "((k == 2) != (r == 2)) ? -1.0 : 1.0"),
        "ao.h / ao.cpp: the camera accessor is const, uses the AO's own camera and motion, and turns the motion into this renderer's view space as FillPush does");
  // the pause world and the effect's own gate are in the rules
  Check(Has(rules, "d.why = Why::kPause;") && Has(trigger, "facts.pause_view = (sf.flags & screen::kFramePause) != 0;"), "the pause menu's, the garage's and the map's world are not reflected");
  // the effect does not touch the colour capture or the history of the owner
  Check(!Has(cpp, "screen_->CapturePost(") && !Has(cpp, "screen::kHistory") && !Has(cpp, "screen_->Image("), "the effect only reads what the owner made (frame slots), it makes none of it");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--print") {
      g_print = true;
    } else if (a[0] != '-') {
      src_dir = a;
    }
  }
  TestLevels();
  TestSizes();
  TestDecision();
  TestHistory();
  TestWet();
  TestPush();
  TestCamera();
  TestGate();
  TestBrdf();
  TestNoise();
  TestGgx();
  TestFades();
  TestVirtualImage();
  TestMarchAnalytic();
  TestMarchBrute();
  TestMarchEdges();
  TestLensFunctions();
  TestSources(src_dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  (void)Count;
  return g_failed == 0 ? 0 : 1;
}
