// Check of the screen-space reflections (ssr_rules.h, hlsl/ssr_common.hlsli, hlsl/ssr_trace.hlsli, ssr.h / ssr.cpp's use of them). A program of its own: it needs no renderer, no ReXGlue and no
// GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssr_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssr_test [<src/gpu dir>] [--print]
//
// GB-I2 adds: the wetness in seconds and the ramp of the term over the smoothness, the share of the game's road view, the hit packed into the trace's alpha (a vehicle's ID in the half float's mantissa), the
// image of a reflected MOVING vehicle against the analytic mirror, the history's variance clip, border and guard, the road view's rules and word, and the sources' use of all of it.
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
#include "blue_noise.h"
#include "blue_noise_spectrum.h"
#include "gbuffer_rules.h"
#include "gbuffer_types.h"
#include "screen_frame.h"
#include "screen_rules.h"
#include "dlss_rr_rules.h"
#include "frame_order.h"
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
#include "hlsl/blue_noise.hlsli"
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

// GB-I3: what the effect asks of the shared owners at each level, and the cost arms the chains set
void TestWantsAndArms() {
  const uint32_t hiz_and_post = sc::ProductBit(sc::kHizMin) | sc::ProductBit(sc::kColourPost);
  Check(sr::WantedProducts(sr::Level::kOff) == 0 && sr::WantedPlanes(sr::Level::kOff) == 0 && !sr::WantsCamera(sr::Level::kOff), "off asks for nothing: no Hi-Z, no colour_post, no planes, no camera");
  for (sr::Level level : {sr::Level::kHigh, sr::Level::kUltra}) {
    Check(sr::WantedProducts(level) == hiz_and_post && sr::WantedProducts(level) == sr::kWantedProducts, std::string(sr::LevelName(level)) + " wants the Hi-Z and colour_post, and no other product (not the history)");
    Check(sr::WantedPlanes(level) == gb::kAllPlanes && sr::WantedPlanes(level) == gb::ConsumerPlanes(gb::Consumer::kReflections), std::string(sr::LevelName(level)) + " wants all three planes (the auto policy's table)");
    Check(sr::WantsCamera(level), std::string(sr::LevelName(level)) + " wants the frame's camera");
  }
  Check(sr::WantedProducts(sr::Level::kHigh) == sr::WantedProducts(sr::Level::kUltra) && sr::WantedPlanes(sr::Level::kHigh) == sr::WantedPlanes(sr::Level::kUltra), "high and ultra read the same things (they differ in the trace's rays and steps)");
  // the G-buffer's auto policy: the planes exist only while a consumer that is on wants one
  gb::ConsumerStates states;
  Check(gb::AutoPlanes(states) == 0, "no consumer on: no planes (the sidecar is not made, no draw changes)");
  states.reflections = true;
  Check(gb::AutoPlanes(states) == sr::WantedPlanes(sr::Level::kHigh), "the reflections on: the auto policy asks for the planes the effect wants");
  // through the owners' wants, as the frame's plan sees them
  gb::Wants gwants;
  gwants.Set(gb::Consumer::kReflections, sr::WantedPlanes(sr::Level::kOff));
  Check(gwants.Union() == 0 && gwants.Of(gb::Consumer::kReflections) == 0, "off withdraws the planes' want");
  gwants.Set(gb::Consumer::kReflections, sr::WantedPlanes(sr::Level::kUltra));
  Check(gwants.Union() == gb::kAllPlanes, "ultra wants the three planes");
  sc::Wants swants;
  sc::Plan plan = sc::PlanFrame(swants, sc::View::kOff, 0);
  Check(plan.make == 0 && !plan.NeedsDepthChain(), "nothing wanted: the owner makes nothing and the AO runs no depth frame for it");
  swants.Set(sc::Consumer::kReflections, sr::WantedProducts(sr::Level::kOff));
  plan = sc::PlanFrame(swants, sc::View::kOff, 0);
  Check(plan.make == 0 && !plan.NeedsDepthChain(), "off asks for no product: no Hi-Z pass, no capture, no depth-only frame (the cost of the off arm is nothing)");
  swants.Set(sc::Consumer::kReflections, sr::WantedProducts(sr::Level::kHigh));
  plan = sc::PlanFrame(swants, sc::View::kOff, 0);
  Check(plan.make == hiz_and_post && plan.NeedsDepthChain(), "high makes the Hi-Z and the capture and needs the depth chain");

  // the arms of the cost runs (frame_order.h cost::kReflectionsArms): the words are the setting's levels, the off arm runs nothing, the others the same set
  using mcla::gpu::frame_order::cost::kReflectionsArms;
  Check(kReflectionsArms.size() == sr::kLevelCount, "one arm for each level");
  for (size_t i = 0; i < kReflectionsArms.size(); ++i) {
    const sr::Level level = sr::LevelFromText(kReflectionsArms[i].word);
    Check(size_t(level) == i && sr::LevelName(level) == kReflectionsArms[i].word, std::string("arm ") + kReflectionsArms[i].word + " is level " + std::to_string(i));
    Check(kReflectionsArms[i].runs == (sr::NumbersOf(level).rays > 0) && kReflectionsArms[i].runs == (sr::WantedProducts(level) != 0) && kReflectionsArms[i].runs == (sr::WantedPlanes(level) != 0),
          std::string("arm ") + kReflectionsArms[i].word + ": it runs exactly when it has rays and asks for the products and the planes");
  }
  Check(std::string(mcla::gpu::frame_order::cost::kReflectionsSetting) == "mcla_gpu_ssr", "the cost arms set mcla_gpu_ssr");
  // cost: the arms in order of cost (the trace's work): off < high < ultra
  const sr::LevelNumbers off = sr::NumbersOf(sr::Level::kOff), high = sr::NumbersOf(sr::Level::kHigh), ultra = sr::NumbersOf(sr::Level::kUltra);
  Check(off.rays * off.max_iterations < high.rays * high.max_iterations && high.rays * high.max_iterations < ultra.rays * ultra.max_iterations, "the trace's work grows from off to high to ultra (rays x steps)");
  // memory: the images the effect holds exist only while it ran lately; the sizes the cost run reads
  Check(sr::HeldBytes(1920, 1080) < sr::HeldBytes(3840, 2160) && sr::HeldBytes(0, 0) == 0, "the held images grow with the picture and are none for none");
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
  double seconds = 0.0;
  while (wet < 0.95f && seconds < 10.0) {
    wet = sr::SmoothWetDt(wet, 1.0f, 1.0f / 60.0f);
    seconds += 1.0 / 60.0;
  }
  Check(seconds > 0.8 && seconds < 1.6, Num("the start of rain ramps over a second or so (%.2f s to 95%%)", seconds));
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
  Check(p::kTemporalFirstResolve == SSR_TEMPORAL_FIRST_RESOLVE && p::kTemporalUpscaler == SSR_TEMPORAL_UPSCALER &&
            p::kTemporalSparkleResolve == SSR_TEMPORAL_SPARKLE_RESOLVE, "the temporal options match the shader");
  Check(sr::PackTemporalWord(17, true, true, true) == sr::PackSlots(17, 6) &&
            sr::PackTemporalWord(sc::kNoSlot, true, true, false) == sr::PackSlots(sc::kNoSlot, 2) &&
            sr::PackTemporalWord(17, false, true, true) == sr::PackSlots(17, 3) &&
            sr::PackTemporalWord(17, true, false, true) == sr::PackSlots(17, 0),
        "the sparkle option requires the spatial resolve, a temporal upscaler and the debug switch");
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
  Check(sr::ViewFromSetting(0) == sr::View::kOff && sr::ViewFromSetting(4) == sr::View::kHit && sr::ViewFromSetting(7) == sr::View::kOff && sr::ViewFromSetting(-1) == sr::View::kOff, "the debug views");
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
  constexpr uint32_t kLegacy = 0x80000000u;
  double sum = 0.0, sum2 = 0.0;
  int n = 0;
  float lo = 1.0f, hi = 0.0f;
  for (uint32_t frame = 0; frame < 16; ++frame) {
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        const float a = hs::SsrNoise1(x, y, frame, 0, 1);
        const float b = hs::SsrNoise2(x, y, frame, 0, 1);
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
  Check(Near(mean, 0.5, 0.01) && Near(var, 1.0 / 12.0, 0.005) && lo >= 0.0f && hi < 1.0f, Num("the noise is uniform on 0..1 (mean %.3f, variance %.4f)", mean, var));
  // the temporal sequence of one pixel: the R2 sequence is low discrepancy in the SQUARE (the lobe's two numbers together; each number alone is not the golden ratio's: 4 steps of its first
  // increment come back to 0.02): 8 frames fall in 4 x 4 cells nearly all apart (random pairs give about 6.4 of 8), 16 frames in at least 11 of 16
  int cells8 = 0, cells16 = 0;
  for (int y = 0; y < 16; ++y) {
    bool seen8[16] = {}, seen16[16] = {};
    for (uint32_t frame = 0; frame < 16; ++frame) {
      const int a = std::min(3, int(hs::SsrNoise1(9, y, frame, 0, 1) * 4.0f)), b = std::min(3, int(hs::SsrNoise2(9, y, frame, 0, 1) * 4.0f));
      seen16[a * 4 + b] = true;
      if (frame < 8) seen8[a * 4 + b] = true;
    }
    for (bool c : seen8) cells8 += c ? 1 : 0;
    for (bool c : seen16) cells16 += c ? 1 : 0;
  }
  if (g_print) std::printf("a pixel's frames in 4 x 4 cells of the square: %.2f of 8, %.2f of 16\n", cells8 / 16.0, cells16 / 16.0);
  Check(cells8 / 16.0 > 6.8, Num("one pixel's eight frames fall in %.2f different cells of the 4 x 4 grid on average (random pairs: 6.4)", cells8 / 16.0));
  Check(cells16 / 16.0 >= 11.0, Num("one pixel's sixteen frames cover %.1f of 16 cells of the square on average", cells16 / 16.0));
  // the rays of a pixel are consecutive numbers of one sequence: the second ray of frame f with two rays is the sample 2 f + 1 of the one-ray sequence
  bool same_sequence = true;
  for (uint32_t frame = 0; frame < 20; ++frame) {
    same_sequence = same_sequence && hs::SsrNoise1(10, 10, frame, 1, 2) == hs::SsrNoise1(10, 10, 2 * frame + 1, 0, 1) && hs::SsrNoise2(10, 10, frame, 0, 2) == hs::SsrNoise2(10, 10, 2 * frame, 0, 1);
  }
  Check(same_sequence, "the rays of a pixel are consecutive samples of one sequence (the second ray of frame f with two rays is sample 2 f + 1)");
  Check(hs::SsrNoise1(10, 10, 3, 0, 2) != hs::SsrNoise1(10, 10, 3, 1, 2) && hs::SsrNoise2(10, 10, 3, 0, 2) != hs::SsrNoise2(10, 10, 3, 1, 2), "the second ray of a pixel has its own numbers");
  // neighbours differ (the noise is not a smooth ramp: a blue noise's mean step is 1/3)
  double diff = 0.0;
  for (int x = 0; x < 63; ++x) diff += std::fabs(hs::SsrNoise1(x, 7, 2, 0, 1) - hs::SsrNoise1(x + 1, 7, 2, 0, 1));
  Check(diff / 63.0 > 0.2, Num("neighbouring pixels differ (mean step %.2f)", diff / 63.0));
  // THE PATTERN STAYS WHERE IT IS: the step a pixel's number takes from one frame to the next is the same for every pixel (the frame's step is added to the tile), so the picture of the noise is the
  // tile moved round the circle and nothing slides; with the first version's noise the step differs from pixel to pixel
  const auto spread = [&](uint32_t flag) {
    double worst = 0.0;
    for (uint32_t frame = 0; frame < 4; ++frame) {
      double lo_d = 2.0, hi_d = -1.0;
      for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
          double d = double(hs::SsrNoise1(x, y, (frame + 1) | flag, 0, 1)) - double(hs::SsrNoise1(x, y, frame | flag, 0, 1));
          d -= std::floor(d);
          lo_d = std::min(lo_d, d);
          hi_d = std::max(hi_d, d);
        }
      }
      // the differences sit on the circle: take the shortest arc that holds them all (the spread of a value just under 1 and one just over 0 is small)
      double spread_arc = hi_d - lo_d;
      if (spread_arc > 0.5) {
        double lo2 = 2.0, hi2 = -1.0;
        for (int y = 0; y < 48; ++y) {
          for (int x = 0; x < 48; ++x) {
            double d = double(hs::SsrNoise1(x, y, (frame + 1) | flag, 0, 1)) - double(hs::SsrNoise1(x, y, frame | flag, 0, 1));
            d -= std::floor(d);
            if (d < 0.5) d += 1.0;
            lo2 = std::min(lo2, d);
            hi2 = std::max(hi2, d);
          }
        }
        spread_arc = std::min(spread_arc, hi2 - lo2);
      }
      worst = std::max(worst, spread_arc);
    }
    return worst;
  };
  const double now = spread(0u), then = spread(kLegacy);
  Check(now < 1e-5, Num("the step from frame to frame is the same at every pixel (spread %.2g): the pattern stays where it is", now));
  Check(then > 0.01, Num("... which the first version's noise (bit 31) did not do (spread %.3f)", then));
  // the first version's switch: bit 31 of the frame word gives exactly the old numbers
  bool legacy_same = true;
  for (uint32_t frame = 0; frame < 6; ++frame) {
    for (uint32_t ray = 0; ray < 2; ++ray) {
      const float f = float(frame & 255u) + float(ray) * 97.0f;
      legacy_same = legacy_same && hs::SsrNoise1(13, 29, frame | kLegacy, ray, 2) == hs::SsrIgn(13.0f + 5.588238f * f, 29.0f + 5.588238f * f) &&
                    hs::SsrNoise2(13, 29, frame | kLegacy, ray, 2) == hs::SsrIgn(29.0f + 4.4f * f + 31.0f, 13.0f + 6.2f * f + 17.0f);
    }
  }
  Check(legacy_same, "the frame word's bit 31 gives the first version's interleaved gradient noise, number for number");
  Check(SSR_FRAME_LEGACY_NOISE == kLegacy && sr::push::kFrameLegacyNoise == kLegacy, "the bit is the same in the shader and in ssr_rules.h");
  // The frame number's bit 31 is not part of the sequence: a frame number with it clear and the same number without are the same, and a frame number of 2^30 and more still steps along the sequence
  Check(hs::SsrNoise1(3, 4, 7u, 0, 1) == hs::SsrNoise1(3, 4, 7u, 0, 1) && hs::SsrNoise1(3, 4, 7u + (1u << 30), 0, 1) != hs::SsrNoise1(3, 4, 7u, 0, 1) + 100.0f, "the numbers of a high frame count are finite and in range");
}

// What the accumulation leaves of the noise, and whether it moves. The response of a pixel to its two numbers is a smooth periodic function (a reflection's brightness against the lobe's sample), the
// temporal pass keeps 0.9 of the history (an exponential mean of about ten frames: the high level's), and the picture of what is left is read the way the eye reads a pattern: its spectrum (how much of it is in
// ONE frequency, how unevenly it is spread over the directions) and its MOTION (the shift of the picture between two frames that gives the best match: a pattern that crawls shows as a shift).
void TestNoiseResolve() {
  constexpr uint32_t kLegacy = 0x80000000u;
  const int N = 128;
  const int frames_of_history = 60;
  const double weight = 0.9;
  const auto response = [](float u1, float u2) { return double(std::cos(6.2831853f * u1 + 0.7f)) + 0.6 * double(std::cos(6.2831853f * u2)); };
  // The error image of frame `t`: the exponential mean of the response over the frames before it, the picture's mean taken out.
  const auto image = [&](uint32_t t, uint32_t flag) {
    std::vector<double> img(size_t(N) * size_t(N), 0.0);
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) {
        double acc = 0.0, w = 1.0 - weight, norm = 0.0;
        for (int k = 0; k < frames_of_history; ++k) {
          const uint32_t f = (t - uint32_t(k)) | flag;
          acc += w * response(hs::SsrNoise1(x, y, f, 0, 1), hs::SsrNoise2(x, y, f, 0, 1));
          norm += w;
          w *= weight;
        }
        img[size_t(y) * size_t(N) + size_t(x)] = acc / norm;
      }
    }
    return img;
  };
  struct Result {
    blue_noise_spectrum::Shape shape;
    double zero_corr = 0.0;
    double best_corr = 0.0;
    int best_dx = 0, best_dy = 0;
    double rms = 0.0;
  };
  const auto measure = [&](uint32_t flag) {
    const uint32_t t = 100;
    const std::vector<double> a = image(t, flag), b = image(t + 1, flag);
    Result r;
    r.shape = blue_noise_spectrum::Analyse(blue_noise_spectrum::Power(a, N, true), N);
    double mean = 0.0;
    for (double v : a) mean += v;
    mean /= double(a.size());
    for (double v : a) r.rms += (v - mean) * (v - mean);
    r.rms = std::sqrt(r.rms / double(a.size()));
    // the best match of frame t + 1 with frame t shifted by (dx, dy), over the middle of the picture (|corr|: a pattern that comes back inverted is the same pattern)
    const int m = 12;
    for (int dy = -8; dy <= 8; ++dy) {
      for (int dx = -8; dx <= 8; ++dx) {
        double sab = 0.0, saa = 0.0, sbb = 0.0, ma = 0.0, mb = 0.0;
        int cnt = 0;
        for (int y = m; y < N - m; ++y) {
          for (int x = m; x < N - m; ++x) {
            ma += a[size_t(y) * size_t(N) + size_t(x)];
            mb += b[size_t(y + dy) * size_t(N) + size_t(x + dx)];
            ++cnt;
          }
        }
        ma /= cnt;
        mb /= cnt;
        for (int y = m; y < N - m; ++y) {
          for (int x = m; x < N - m; ++x) {
            const double va = a[size_t(y) * size_t(N) + size_t(x)] - ma, vb = b[size_t(y + dy) * size_t(N) + size_t(x + dx)] - mb;
            sab += va * vb;
            saa += va * va;
            sbb += vb * vb;
          }
        }
        const double c = std::fabs(sab / std::sqrt(std::max(saa * sbb, 1e-30)));
        if (dx == 0 && dy == 0) r.zero_corr = c;
        if (c > r.best_corr) {
          r.best_corr = c;
          r.best_dx = dx;
          r.best_dy = dy;
        }
      }
    }
    return r;
  };
  const Result legacy = measure(kLegacy), blue = measure(0u);
  if (g_print) {
    std::printf("the resolved error: first version rms %.4f, one frequency %.4f of it, strongest direction %.2f x the mean, best match with the next frame at shift (%d, %d): %.2f (at 0: %.2f)\n", legacy.rms,
                legacy.shape.peak_share, legacy.shape.wedge_ratio, legacy.best_dx, legacy.best_dy, legacy.best_corr, legacy.zero_corr);
    std::printf("                    blue noise    rms %.4f, one frequency %.4f of it, strongest direction %.2f x the mean, best match with the next frame at shift (%d, %d): %.2f (at 0: %.2f)\n", blue.rms,
                blue.shape.peak_share, blue.shape.wedge_ratio, blue.best_dx, blue.best_dy, blue.best_corr, blue.zero_corr);
  }
  Check(legacy.shape.peak_share > 0.02 && legacy.shape.wedge_ratio > 2.5, Num("the first version's error is a lattice: %.3f of it in one frequency (over 0.02) and a direction %.1f times the mean (over 2.5)", legacy.shape.peak_share, legacy.shape.wedge_ratio));
  Check(blue.shape.peak_share < 0.012 && blue.shape.wedge_ratio < 2.4, Num("the blue noise's has no frequency that stands out (%.4f, under 0.012) and no direction (%.2f, under 2.4)", blue.shape.peak_share, blue.shape.wedge_ratio));
  // it crawls: the next frame's picture is the same picture moved, at a shift of a few pixels, and better than in place
  Check((legacy.best_dx != 0 || legacy.best_dy != 0) && legacy.best_corr > 0.9 && legacy.zero_corr < 0.85 * legacy.best_corr,
        Num("the first version's error crawls: frame t + 1 matches frame t moved by (%.0f, %.0f) at %.2f, and in place at only %.2f", double(legacy.best_dx), double(legacy.best_dy), legacy.best_corr, legacy.zero_corr));
  // it does not: no shift of the picture matches the next frame (the response moves along the circle from frame to frame, so the picture flickers; it never drifts)
  Check(blue.best_corr < 0.35, Num("the blue noise's error does not crawl: no shift up to 8 pixels makes the next frame's picture match the last (the best is %.2f, under 0.35)", blue.best_corr));
  // and it is not bigger
  Check(blue.rms < 1.3 * legacy.rms, Num("the blue noise's error rms %.4f is within 1.3 times the first version's (%.4f)", blue.rms, legacy.rms));
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
  // (the history's clip: TestClip)
}


// ---------------------------------------------------------------------------
// GB-I2: the wetness in seconds, the ramp, the share of the road view
// ---------------------------------------------------------------------------

void TestWetI2() {
  // the votes carry the game's reflection strength (VS c43.y): the strength of the commonest wetness
  sr::WetVotes votes;
  float x = -1.0f, y = -1.0f;
  for (int i = 0; i < 30; ++i) votes.Add(1.17f, 1.19f);
  for (int i = 0; i < 10; ++i) votes.Add(0.095f, 0.095f);
  Check(votes.Pick(&x, &y) && Near(x, 1.2, 1e-6) && Near(y, 1.19, 1e-5), Num("the commonest wetness %.2f with its strength %.3f", x, y));
  votes.Reset();
  votes.Add(0.095f, 0.10f);
  votes.Add(0.095f, 0.09f);
  votes.Add(0.095f, std::nanf(""));
  Check(votes.Pick(&x, &y) && Near(y, (0.10 + 0.09 + sr::WetVotes::kDefaultGameStrength) / 3.0, 1e-5), "a strength that is no number counts as the default (a draw gave none)");
  votes.Reset();
  votes.Add(0.5f);
  Check(votes.Pick(&x, &y) && Near(y, sr::WetVotes::kDefaultGameStrength, 1e-6), "a vote without a strength carries the default one");
  Check(votes.Pick(&x) && Near(x, 0.5, 1e-6), "and the strength is optional to ask for");

  // the smoothing is in seconds: the same ramp at 30, 60 and 144 frames a second
  const auto ramp = [](double fps, double seconds) {
    float wet = 0.0f;
    const int n = int(seconds * fps + 0.5);
    for (int i = 0; i < n; ++i) wet = sr::SmoothWetDt(wet, 1.0f, float(1.0 / fps));
    return double(wet);
  };
  const double a30 = ramp(30, 0.5), a60 = ramp(60, 0.5), a144 = ramp(144, 0.5);
  Check(Near(a30, a60, 2e-3) && Near(a60, a144, 2e-3), Num("half a second of rain is %.4f at 30 FPS, %.4f at 60, %.4f at 144: the frame rate does not change the ramp", a30, a60, a144));
  Check(ramp(60, 0.5) < 0.75 && ramp(60, 1.25) > 0.94, Num("the start of rain is a ramp of about a second (%.2f after 0.5 s, %.2f after 1.25 s)", ramp(60, 0.5), ramp(60, 1.25)));
  Check(sr::SmoothWetDt(0.0f, 1.0f, 10.0f) <= 1.0f - std::exp(-sr::kWetMaxDt / sr::kWetTauSeconds) + 1e-6f && sr::SmoothWetDt(0.2f, 1.0f, std::nanf("")) == 0.2f && sr::SmoothWetDt(0.2f, 1.0f, -1.0f) == 0.2f && sr::SmoothWetDt(0.2f, 1.0f, 0.0f) == 0.2f,
        "a pause spell is a step of a quarter of a second at most; no time, a negative time and a NaN move nothing");
  Check(sr::SmoothWetDt(1.0f, 0.0f, 0.05f) < 1.0f && sr::SmoothWetDt(1.0f, 0.0f, 0.05f) > 0.0f, "and the rain stopping ramps down the same way");

  // FallbackShare: never more than the game shows, never more than the setting
  Check(sr::FallbackShare(1.0f, 1.19f) == 1.0f && Near(sr::FallbackShare(1.0f, 0.095f), 0.095, 1e-6) && sr::FallbackShare(0.0f, 1.0f) == 0.0f && Near(sr::FallbackShare(0.5f, 0.8f), 0.4, 1e-6), "the share taken out: setting x the game's strength");
  Check(sr::FallbackShare(2.0f, 1.0f) == 1.0f && sr::FallbackShare(-1.0f, 1.0f) == 0.0f && sr::FallbackShare(std::nanf(""), 1.0f) == 0.0f && sr::FallbackShare(1.0f, std::nanf("")) == 0.0f && sr::FallbackShare(1.0f, -3.0f) == 0.0f,
        "out of range or no number: clamped, or none");

  // the ramp of the term over the smoothness
  const float floor_smooth = sr::kDefaultMinSmooth;
  Check(hs::SsrSmoothRamp(0.30f, floor_smooth) == 0.0f && hs::SsrSmoothRamp(floor_smooth, floor_smooth) == 0.0f && hs::SsrSmoothRamp(0.0f, floor_smooth) == 0.0f, "a dry road (0.30) and everything under the floor take nothing");
  Check(Near(hs::SsrSmoothRamp(floor_smooth + hs::kSsrRampSpan, floor_smooth), 1.0, 1e-6) && hs::SsrSmoothRamp(0.92f, floor_smooth) == 1.0f && hs::SsrSmoothRamp(1.0f, floor_smooth) == 1.0f, "a wet road (0.92) takes the whole term");
  bool monotone = true;
  float last = 0.0f;
  for (int i = 0; i <= 200; ++i) {
    const float g = hs::SsrSmoothRamp(float(i) / 200.0f, floor_smooth);
    monotone = monotone && g >= last - 1e-6f && g >= 0.0f && g <= 1.0f;
    last = g;
  }
  Check(monotone, "the ramp rises monotonically from 0 to 1");
  // the rain's ramp through the floor: from a dry road to a wet one the term takes in over a stretch of wetness, with no jump
  float prev = 0.0f, worst_step = 0.0f, first_on = -1.0f, full_at = -1.0f;
  for (int i = 0; i <= 200; ++i) {
    const float wet = float(i) / 200.0f;
    const float g = hs::SsrSmoothRamp(hs::SsrWetSmooth(uint(GB_CLASS_ROAD), 0.30f, wet), floor_smooth);
    if (i > 0) worst_step = std::max(worst_step, g - prev);
    if (first_on < 0.0f && g > 0.0f) first_on = wet;
    if (full_at < 0.0f && g >= 1.0f - 1e-6f) full_at = wet;
    prev = g;
  }
  Check(first_on > 0.1f && full_at < 0.6f && full_at > first_on && worst_step < 0.05f, Num("the wet road takes the reflection in from wetness %.2f to %.2f, by at most %.3f in a step of 0.005 (no jump at the floor)", first_on, full_at, worst_step));
  // a debug floor just under the wet road's smoothness still gives the wet road its whole term (the run plan's trick)
  Check(Near(hs::SsrSmoothRamp(0.92f, 0.9f), 1.0, 1e-6) && hs::SsrSmoothRamp(0.85f, 0.9f) == 0.0f, "a floor of 0.9: the wet road (0.92) reflects, the stub's 0.85 does not");
  Check(Near(hs::SsrSmoothRamp(0.5f, 0.95f), 0.0, 1e-6) && hs::SsrSmoothRamp(1.0f, 0.95f) == 1.0f, "a floor above the wet road's smoothness: the ramp is short (0.01) and stays defined");
}

// ---------------------------------------------------------------------------
// GB-I2: what the trace says of a hit (the half float's mantissa)
// ---------------------------------------------------------------------------

uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, sizeof(x));
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t exp = int32_t((x >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = x & 0x7FFFFFu;
  if (exp <= 0) return uint16_t(sign);          // the tests stay in the normal range (the shader clamps a distance to 0.02 and up)
  if (exp >= 31) return uint16_t(sign | 0x7C00u);
  uint32_t half = (uint32_t(exp) << 10) | (mant >> 13);
  const uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;   // round to nearest even, what the ROP does
  return uint16_t(sign | half);
}
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 31u;
  const uint32_t mant = h & 1023u;
  uint32_t bits;
  if (exp == 0) {
    return (sign ? -1.0f : 1.0f) * float(mant) * std::ldexp(1.0f, -24);
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
float ThroughHalf(float f) { return HalfToFloat(FloatToHalf(f)); }

void TestHitPacking() {
  // a hit that is no vehicle's: the distance as it is (the half's own rounding), no sign, no ID
  Check(!hs::SsrHitIsVehicle(hs::SsrPackHit(12.5f, false, 0)) && hs::SsrHitVehicleId(hs::SsrPackHit(12.5f, false, 7)) == 0 && hs::SsrPackHit(12.5f, false, 7) == 12.5f, "no vehicle: the distance, no flag, no ID");
  Check(Near(hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(12.34f, false, 0))), 12.34, 12.34 * 0.001), "a plain hit keeps its ten bits (0.1%)");
  // a vehicle's: every ID, over the whole range of distances, through the half float the image stores
  bool ids_ok = true, flag_ok = true, nonzero_ok = true, finite_ok = true;
  double worst = 0.0;
  Lcg rng;
  for (uint id = 0; id <= 31; ++id) {
    for (int k = 0; k < 400; ++k) {
      const float d = std::exp(rng.Range(std::log(0.02f), std::log(60000.0f)));
      const float stored = ThroughHalf(hs::SsrPackHit(d, true, id));
      ids_ok = ids_ok && hs::SsrHitVehicleId(stored) == id;
      flag_ok = flag_ok && hs::SsrHitIsVehicle(stored);
      nonzero_ok = nonzero_ok && stored != 0.0f && stored < 0.0f;
      const float back = hs::SsrHitDistance(stored);
      finite_ok = finite_ok && std::isfinite(back) && back > 0.0f;
      worst = std::max(worst, std::fabs(double(back) - double(d)) / double(d));
    }
  }
  Check(ids_ok && flag_ok, "a vehicle's ID (0 to 31, 1 to 23 are real) and flag survive the half float exactly");
  Check(nonzero_ok && finite_ok, "the alpha is never 0 (0 means 'not traced') and the distance back is positive and finite");
  if (g_print) std::printf("[hit packing] worst distance error of a vehicle's hit %.2f%%\n", worst * 100.0);
  Check(worst < 0.032, Num("the distance of a vehicle's hit is within %.2f%% of the true one (the ID took five of the ten mantissa bits)", worst * 100.0));
  // the extremes
  Check(hs::SsrHitVehicleId(ThroughHalf(hs::SsrPackHit(60000.0f, true, 23))) == 23 && std::isfinite(hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(60000.0f, true, 23)))), "60000 metres and ID 23");
  Check(hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(0.02f, true, 1))) > 0.0f && hs::SsrHitVehicleId(ThroughHalf(hs::SsrPackHit(0.02f, true, 1))) == 1, "2 cm and ID 1");
  Check(Near(hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(sr::kDefaultMaxDistance, false, 0))), 150.0, 0.2) && hs::SsrHitDistance(ThroughHalf(hs::kSsrMissLength)) == hs::kSsrMissLength && !hs::SsrHitIsVehicle(hs::kSsrMissLength),
        "a miss (4096) is stored whole and is no vehicle");
  // an ID wider than five bits cannot leak into the exponent or the sign
  const float wide = ThroughHalf(hs::SsrPackHit(100.0f, true, 0xFFu));
  Check(hs::SsrHitVehicleId(wide) == 31 && Near(hs::SsrHitDistance(wide), 100.0, 3.5), "an ID is masked to its five bits");
}

// ---------------------------------------------------------------------------
// GB-I2: the image of a reflected moving vehicle, against the analytic mirror
// ---------------------------------------------------------------------------

struct Rows3x4 {
  hs::float4 r[3];
};
// The 3x4 that maps a point of `now`'s view space to `before`'s view space of the point that was `shift` earlier in the world (the camera's motion alone for a zero shift, the vehicle's own for a moving one).
Rows3x4 MotionRows(const Pose& now, const Pose& before, Vec shift) {
  const auto map = [&](Vec p) {
    Vec world = ToWorld(now, p);
    world = Sub(world, shift);
    return ToView(before, world);
  };
  const Vec o = map({{0.0, 0.0, 0.0}});
  const Vec ex = Sub(map({{1.0, 0.0, 0.0}}), o), ey = Sub(map({{0.0, 1.0, 0.0}}), o), ez = Sub(map({{0.0, 0.0, 1.0}}), o);
  Rows3x4 rows;
  for (int k = 0; k < 3; ++k) rows.r[k] = hs::float4(float(ex.v[k]), float(ey.v[k]), float(ez.v[k]), float(o.v[k]));
  return rows;
}

void TestVehicleImage() {
  const double tan_x = 0.9, tan_y = 0.5;
  struct Result {
    double du, dv, dz, static_miss_px;
    bool ok;
  };
  // one configuration: a camera that moves, a vehicle's point H that moves by `vel` a frame, the road y = 0
  const auto run = [&](const Pose& before, const Pose& now, Vec H, Vec vel) {
    Result res{1e9, 1e9, 1e9, 0.0, false};
    const Vec H_prev = Sub(H, vel);
    const Vec Hmirror{{H.v[0], -H.v[1], H.v[2]}};
    const Vec cam{{now.pos[0], now.pos[1], now.pos[2]}};
    const Vec dir = Sub(Hmirror, cam);
    if (std::fabs(dir.v[1]) < 1e-6) return res;
    const double t = (0.0 - cam.v[1]) / dir.v[1];
    if (t <= 0.0 || t >= 1.0) return res;
    const Vec P_world{{cam.v[0] + dir.v[0] * t, 0.0, cam.v[2] + dir.v[2] * t}};
    const Vec P_view = ToView(now, P_world);
    const Vec H_view = ToView(now, H);
    if (P_view.v[2] < 1.0 || H_view.v[2] < 1.0) return res;
    const double length = std::sqrt(DotV(Sub(H_view, P_view), Sub(H_view, P_view)));
    // the receiver's normal in this frame's view space: the world's up axis
    Vec r, u, f;
    Axes(now, &r, &u, &f);
    const hs::float3 n(float(r.v[1]), float(u.v[1]), float(f.v[1]));
    const Rows3x4 cam_rows = MotionRows(now, before, {{0.0, 0.0, 0.0}});
    const Rows3x4 veh_rows = MotionRows(now, before, vel);
    const hs::float3 P(float(P_view.v[0]), float(P_view.v[1]), float(P_view.v[2]));
    const hs::SsrPrevious img = hs::SsrVehicleImage(cam_rows.r[0], cam_rows.r[1], cam_rows.r[2], veh_rows.r[0], veh_rows.r[1], veh_rows.r[2], P, n, float(length), float(tan_x), float(tan_y));
    // where the image of the vehicle's point was in the last frame: the mirror of H_prev seen from the last camera
    const Vec Hm_old = ToView(before, {{H_prev.v[0], -H_prev.v[1], H_prev.v[2]}});
    if (Hm_old.v[2] < 1.0) return res;
    const double u_old = 0.5 + 0.5 * Hm_old.v[0] / (tan_x * Hm_old.v[2]);
    const double v_old = 0.5 - 0.5 * Hm_old.v[1] / (tan_y * Hm_old.v[2]);
    if (u_old < 0.0 || u_old > 1.0 || v_old < 0.0 || v_old > 1.0) return res;
    res.ok = img.ok;
    res.du = img.u - u_old;
    res.dv = img.v - v_old;
    res.dz = img.z - Hm_old.v[2];
    // what the world's reprojection (the virtual image with the camera's motion alone) would have said
    const hs::SsrPrevious world = hs::SsrToPrevious(cam_rows.r[0], cam_rows.r[1], cam_rows.r[2], hs::SsrVirtualPoint(P, float(length)), float(tan_x), float(tan_y));
    res.static_miss_px = world.ok ? std::hypot((world.u - u_old) * 1920.0, (world.v - v_old) * 1080.0) : 1e9;
    return res;
  };
  const Pose before{{0.0, 1.5, 0.0}, 0.00, 0.02};
  const Pose now{{0.4, 1.5, 1.3}, 0.03, 0.02};
  const Vec H{{3.0, 1.2, 12.0}};   // a car's roof light over the road, 12 m ahead
  // a car driving away at 15 m a second at 60 FPS (0.25 m a frame), across, and fast
  const Vec velocities[] = {{{0.0, 0.0, 0.25}}, {{0.20, 0.0, 0.0}}, {{-0.3, 0.0, 0.6}}};
  bool exact = true, worse_static = true;
  double worst = 0.0, smallest_static = 1e9;
  for (const Vec& vel : velocities) {
    const Result r = run(before, now, H, vel);
    exact = exact && r.ok && std::fabs(r.du) < 2e-4 && std::fabs(r.dv) < 2e-4 && std::fabs(r.dz) < 5e-3;
    worst = std::max(worst, std::max(std::fabs(r.du), std::fabs(r.dv)));
    smallest_static = std::min(smallest_static, r.static_miss_px);
    worse_static = worse_static && r.static_miss_px > 2.0;
  }
  Check(exact, Num("a reflected moving vehicle's image is found in the last frame to within %.1e of the picture (analytic mirror, three speeds)", worst));
  Check(worse_static, Num("taken as the world's it would have missed by at least %.1f pixels (that is the smear)", smallest_static));
  // a parked vehicle: its row is the camera's, and the image is the world's virtual image
  const Result parked = run(before, now, H, {{0.0, 0.0, 0.0}});
  Check(parked.ok && std::fabs(parked.du) < 2e-4 && std::fabs(parked.dv) < 2e-4 && parked.static_miss_px < 0.5, "a parked vehicle: the vehicle's row is the camera's, and the image is the world's (the same answer)");
  // random cameras, heights and velocities: the analytic mirror holds, every time
  Lcg rng;
  int cases = 0, bad = 0;
  double worst_random = 0.0;
  for (int i = 0; i < 400; ++i) {
    const Pose b{{rng.Range(-5, 5), rng.Range(1.0f, 3.0f), rng.Range(-5, 5)}, rng.Range(-0.3f, 0.3f), rng.Range(-0.1f, 0.1f)};
    const Pose n{{b.pos[0] + rng.Range(-1, 1), b.pos[1] + rng.Range(-0.05f, 0.05f), b.pos[2] + rng.Range(0, 2)}, b.yaw + rng.Range(-0.05f, 0.05f), b.pitch + rng.Range(-0.02f, 0.02f)};
    const Vec h{{n.pos[0] + rng.Range(-15, 15), rng.Range(0.3f, 6.0f), n.pos[2] + rng.Range(8, 80)}};
    const Vec v{{rng.Range(-0.5f, 0.5f), 0.0, rng.Range(-0.5f, 0.5f)}};
    const Result r = run(b, n, h, v);
    if (r.du > 1e8) continue;   // not a valid configuration (off the picture, behind a camera)
    ++cases;
    const double e = std::max(std::fabs(r.du), std::fabs(r.dv));
    worst_random = std::max(worst_random, e);
    if (!r.ok || e > 3e-4 || std::fabs(r.dz) > 2e-2) ++bad;
  }
  if (g_print) std::printf("[vehicle image] %d random configurations, worst error %.2e of the picture; the three speeds: worst %.2e, the world's reprojection misses by at least %.1f px\n", cases, worst_random, worst, smallest_static);
  Check(cases > 100 && bad == 0, Num("%.0f random configurations (cameras, heights, speeds): every reprojection is on the analytic mirror (worst %.1e of the picture)", cases, worst_random));
  // a vehicle that was not there (a row that maps behind the last camera) has no history
  const Rows3x4 cam_rows = MotionRows(now, before, {{0.0, 0.0, 0.0}});
  const hs::float4 behind0(1.0f, 0.0f, 0.0f, 0.0f), behind1(0.0f, 1.0f, 0.0f, 0.0f), behind2(0.0f, 0.0f, 1.0f, -100.0f);
  const hs::SsrPrevious gone = hs::SsrVehicleImage(cam_rows.r[0], cam_rows.r[1], cam_rows.r[2], behind0, behind1, behind2, hs::float3(0.0f, -1.5f, 10.0f), hs::float3(0.0f, 1.0f, 0.0f), 5.0f, float(tan_x), float(tan_y));
  Check(!gone.ok, "a point the row puts behind the last camera has no history");
  // the host's weights for the three kinds of hit
  Check(sr::kVehicleRowWeight == hs::kSsrVehicleRowWeight && sr::kVehicleNoRowWeight == hs::kSsrVehicleNoRowWeight, "the host and the shaders agree on the vehicle weights");
  Check(sr::VehicleHistoryScale(false, false) == 1.0f && sr::VehicleHistoryScale(false, true) == 1.0f && sr::VehicleHistoryScale(true, true) > sr::VehicleHistoryScale(true, false) && sr::VehicleHistoryScale(true, true) < 1.0f &&
            sr::VehicleHistoryScale(true, false) > 0.0f,
        "the world keeps all of its history, a vehicle with a row most of it, one without a row little");
}

// ---------------------------------------------------------------------------
// GB-I2: the history's clip, the border, the guard
// ---------------------------------------------------------------------------

void TestClip() {
  const hs::float3 lo(0.0f, 0.0f, 0.0f), hi(1.0f, 2.0f, 4.0f);
  const hs::float3 inside(0.5f, 1.0f, 2.0f);
  const hs::float3 same = hs::SsrClipToBox(inside, lo, hi);
  Check(same.x == inside.x && same.y == inside.y && same.z == inside.z, "a history inside the box is kept as it is");
  const hs::float3 centre(0.5f, 1.0f, 2.0f);
  Lcg rng;
  bool on_box = true, along_line = true, idempotent = true;
  for (int i = 0; i < 500; ++i) {
    const hs::float3 h(rng.Range(-5, 6), rng.Range(-5, 7), rng.Range(-5, 9));
    const hs::float3 c = hs::SsrClipToBox(h, lo, hi);
    const bool was_outside = h.x < lo.x || h.x > hi.x || h.y < lo.y || h.y > hi.y || h.z < lo.z || h.z > hi.z;
    on_box = on_box && c.x >= lo.x - 1e-4f && c.x <= hi.x + 1e-4f && c.y >= lo.y - 1e-4f && c.y <= hi.y + 1e-4f && c.z >= lo.z - 1e-4f && c.z <= hi.z + 1e-4f;
    if (was_outside) {
      // collinear with the box's centre and the history: the colour's hue is kept (a clamp would turn it)
      const hs::float3 a = h - centre, b = c - centre;
      const hs::float3 cr = hs::cross(a, b);
      const double len = std::sqrt(double(hs::dot(a, a)) * double(hs::dot(b, b)));
      along_line = along_line && std::sqrt(double(hs::dot(cr, cr))) < 1e-4 * std::max(len, 1.0);
    }
    const hs::float3 c2 = hs::SsrClipToBox(c, lo, hi);
    idempotent = idempotent && std::fabs(c2.x - c.x) < 1e-4f && std::fabs(c2.y - c.y) < 1e-4f && std::fabs(c2.z - c.z) < 1e-4f;
  }
  Check(on_box, "the clip puts any history into the box");
  Check(along_line, "on the line from the box's centre to the history (the hue is kept, a clamp turns it)");
  Check(idempotent, "and clipping twice is clipping once");
  // a flat box (a neighbourhood of one colour) pulls the history to it
  const hs::float3 flat = hs::SsrClipToBox(hs::float3(5.0f, 5.0f, 5.0f), hs::float3(1.0f, 1.0f, 1.0f), hs::float3(1.0f, 1.0f, 1.0f));
  Check(std::fabs(flat.x - 1.0f) < 2e-3f && std::fabs(flat.y - 1.0f) < 2e-3f && std::fabs(flat.z - 1.0f) < 2e-3f, "a box of one colour pulls the history to that colour");
  // the box: ordered, around the mean, never wider than the plain clamp's, and narrower than it where the neighbourhood is steady
  bool ordered = true, within_clamp = true;
  for (int i = 0; i < 500; ++i) {
    const float a = rng.Range(0, 5), b = rng.Range(0, 5);
    const float vlo = std::min(a, b), vhi = std::max(a, b);
    const float mean = rng.Range(vlo, vhi), sigma = rng.Range(0.0f, 2.0f) * (vhi - vlo);
    const float bl = hs::SsrBoxLoChannel(mean, sigma, vlo, vhi), bh = hs::SsrBoxHiChannel(mean, sigma, vlo, vhi);
    ordered = ordered && bl <= mean + 1e-6f && bh >= mean - 1e-6f;
    within_clamp = within_clamp && bl >= vlo - hs::kSsrClipMargin * (vhi - vlo) - 1e-5f && bh <= vhi + hs::kSsrClipMargin * (vhi - vlo) + 1e-5f;
  }
  Check(ordered && within_clamp, "the box holds the mean and is never wider than the min / max widened by a tenth (the old clamp)");
  Check(hs::SsrBoxHiChannel(1.0f, 0.05f, 0.0f, 3.0f) < 1.1f && hs::SsrBoxLoChannel(1.0f, 0.05f, 0.0f, 3.0f) > 0.9f && hs::SsrBoxHiChannel(1.0f, 5.0f, 0.0f, 3.0f) > 3.2f, "a steady neighbourhood gives a narrow box, a noisy one the plain clamp's");
  // a noisy single-ray neighbourhood and a ghost: the clip takes the ghost in to the noise's edge, and leaves a plausible history alone
  float samples[9];
  double sum = 0.0, sum2 = 0.0;
  float smin = 1e9f, smax = -1e9f;
  for (float& s : samples) {
    s = 1.0f + rng.Range(-0.4f, 0.4f);
    sum += s;
    sum2 += double(s) * s;
    smin = std::min(smin, s);
    smax = std::max(smax, s);
  }
  const float mean = float(sum / 9.0), sigma = float(std::sqrt(std::max(sum2 / 9.0 - double(mean) * mean, 0.0)));
  const float bl = hs::SsrBoxLoChannel(mean, sigma, smin, smax), bh = hs::SsrBoxHiChannel(mean, sigma, smin, smax);
  const hs::float3 ghost = hs::SsrClipToBox(hs::float3(3.0f, 3.0f, 3.0f), hs::float3(bl, bl, bl), hs::float3(bh, bh, bh));
  const hs::float3 plausible = hs::SsrClipToBox(hs::float3(mean, mean, mean), hs::float3(bl, bl, bl), hs::float3(bh, bh, bh));
  Check(ghost.x <= bh + 1e-4f && ghost.x < 1.6f && plausible.x == mean, Num("a ghost of 3.0 over a neighbourhood around 1.0 is taken to %.2f; a history at the mean is kept", ghost.x));

  // the picture's border
  Check(hs::SsrHistoryInside(0.5f, 0.5f, 960.0f, 540.0f) && !hs::SsrHistoryInside(0.0f, 0.5f, 960.0f, 540.0f) && !hs::SsrHistoryInside(1.0f, 0.5f, 960.0f, 540.0f) && !hs::SsrHistoryInside(0.5f, 0.0005f, 960.0f, 540.0f) &&
            hs::SsrHistoryInside(0.5f, 0.01f, 960.0f, 540.0f),
        "a history position within a texel of the edge is not read");

  // the guard
  const hs::float3 g = hs::SsrGuardTerm(hs::float3(-10.0f, -0.1f, 2.0f), hs::float3(1.0f, 1.0f, 1.0f));
  Check(Near(g.x, -0.8, 1e-6) && Near(g.y, -0.1, 1e-6) && g.z == 2.0f, "the guard keeps a fifth of the colour at least, leaves a small negative alone and a positive whole");
  const hs::float3 g0 = hs::SsrGuardTerm(hs::float3(-1.0f, -1.0f, -1.0f), hs::float3(0.0f, -2.0f, 0.0f));
  Check(g0.x == 0.0f && g0.y == 0.0f && g0.z == 0.0f, "a pixel with no colour (or a negative one) has nothing taken away");
}

// ---------------------------------------------------------------------------
// the spatial resolve and the clip's floor (SSR-STRIPES-2, 2026-11-22): what is left of the noise on a SPARSE bright reflection
// ---------------------------------------------------------------------------
// The scene: a small bright thing (a lamp, a tail light) of radius `lamp` pixels whose image the lobe's jitter (the shader's own numbers: the blue noise tile stepped along the R2 sequence, a GGX tail
// radius R * sqrt(u / (1 - u)), the angle 2 pi u) reaches from the pixels round it: a pixel's ray finds it, or finds the road, a frame at a time. The pipeline is the temporal pass's own
// arithmetic on a scalar image (hlsl/ssr_temporal_ps.hlsl: the tent over the 3x3, the variance clip's box of the same 3x3, the history's keep, 0.9 of the history): `resolve` false is the first version's
// (the centre pixel alone, the clip whole). A still camera, the world still: everything that is left is the noise. The truth is the lobe's mean (a dense Monte Carlo of the same distribution).
namespace {
constexpr int kResolveN = 48;
constexpr float kResolveHit = 100.0f, kResolveBg = 1.0f;
struct ResolveScene {
  float lobe = 2.0f;    // R: the jitter's scale in pixels (alpha times the distance)
  float lamp = 4.0f;    // the thing's radius in pixels
};
float ResolveSample(const ResolveScene& sc, int x, int y, uint32_t frame, bool lamp_on) {
  const float u1 = hs::SsrNoise1(x, y, frame, 0, 1), u2 = hs::SsrNoise2(x, y, frame, 0, 1);
  const float m = sc.lobe * std::sqrt(u1 / std::max(1.0f - u1, 1e-6f));
  const float phi = 6.2831853f * u2;
  const float px = float(x) + m * std::cos(phi) - float(kResolveN / 2), py = float(y) + m * std::sin(phi) - float(kResolveN / 2);
  return (lamp_on && px * px + py * py < sc.lamp * sc.lamp) ? kResolveHit : kResolveBg;
}
std::vector<float> ResolveTruth(const ResolveScene& sc) {
  std::vector<float> t(size_t(kResolveN) * kResolveN, kResolveBg);
  Lcg rng;
  const int samples = 1200;
  for (int y = 0; y < kResolveN; ++y) {
    for (int x = 0; x < kResolveN; ++x) {
      double acc = 0.0;
      for (int k = 0; k < samples; ++k) {
        const float u1 = rng.Next() * 0.9999f, u2 = rng.Next();
        const float m = sc.lobe * std::sqrt(u1 / (1.0f - u1));
        const float phi = 6.2831853f * u2;
        const float px = float(x) + m * std::cos(phi) - float(kResolveN / 2), py = float(y) + m * std::sin(phi) - float(kResolveN / 2);
        acc += (px * px + py * py < sc.lamp * sc.lamp) ? kResolveHit : kResolveBg;
      }
      t[size_t(y) * kResolveN + size_t(x)] = float(acc / samples);
    }
  }
  return t;
}
// The images of `frames` frames of the pass; the lamp is on until frame `off_at` (a number past `frames`: never off).
std::vector<std::vector<float>> ResolveRun(const ResolveScene& sc, bool resolve, int frames, int off_at, float keep) {
  const int N = kResolveN;
  std::vector<float> trace(size_t(N) * N), history(size_t(N) * N, 0.0f), out(size_t(N) * N, 0.0f);
  std::vector<std::vector<float>> all;
  for (int f = 0; f < frames; ++f) {
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) trace[size_t(y) * N + size_t(x)] = ResolveSample(sc, x, y, uint32_t(f), f < off_at);
    }
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) {
        const float centre = trace[size_t(y) * N + size_t(x)];
        float lo = centre, hi = centre, sum = 0.0f, sum2 = 0.0f, count = 0.0f, tent = 0.0f, tent_weight = 0.0f;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const float t = trace[size_t(std::clamp(y + dy, 0, N - 1)) * N + size_t(std::clamp(x + dx, 0, N - 1))];
            lo = std::min(lo, t);
            hi = std::max(hi, t);
            sum += t;
            sum2 += t * t;
            count += 1.0f;
            if ((dx == 0 && dy == 0) || (resolve && hs::SsrTapSameSurface(10.0f, 10.0f))) {
              const float k = hs::SsrTapKernel(dx, dy);
              tent += t * k;
              tent_weight += k;
            }
          }
        }
        const float value = tent / tent_weight;
        float o = value;
        if (f > 0) {
          const float h = history[size_t(y) * N + size_t(x)];
          const float mean = sum / count, sigma = std::sqrt(std::max(sum2 / count - mean * mean, 0.0f));
          const float bl = hs::SsrBoxLoChannel(mean, sigma, lo, hi), bh = hs::SsrBoxHiChannel(mean, sigma, lo, hi);
          const hs::float3 clipped = hs::SsrClipToBox(hs::float3(h, h, h), hs::float3(bl, bl, bl), hs::float3(bh, bh, bh));
          const float kept = hs::SsrKeepClip(hs::float3(h, h, h), clipped, keep).x;
          o = value * (1.0f - 0.9f) + kept * 0.9f;
        }
        out[size_t(y) * N + size_t(x)] = o;
      }
    }
    history = out;
    all.push_back(out);
  }
  return all;
}
struct ResolveNumbers {
  double flicker = 0.0;   // the temporal std over the lamp's pixels, as a share of the truth there
  double frame_delta = 0.0; // mean absolute change between consecutive frames over the lamp's pixels
  double energy = 0.0;    // the time mean of the output over the truth, over the lamp's pixels (1 = the right brightness)
  int fade_half = 0;      // frames after the lamp goes out until what is left is below half / a tenth of what the lamp added
  int fade_tenth = 0;
};
ResolveNumbers ResolveMeasure(const ResolveScene& sc, bool resolve, bool upscaler = false, bool sparkle_resolve = false) {
  const std::vector<float> truth = ResolveTruth(sc);
  const float keep = sparkle_resolve ? hs::SsrSparkleClipKeep(0.0f, 0.0f) : hs::SsrClipKeepFor(resolve, upscaler);
  const std::vector<std::vector<float>> run = ResolveRun(sc, resolve, 240, 1000, keep);
  ResolveNumbers r;
  double flicker = 0.0, out_sum = 0.0, truth_sum = 0.0;
  int n = 0;
  for (size_t i = 0; i < truth.size(); ++i) {
    if (truth[i] < kResolveBg * 1.5f) continue;
    double m = 0.0, m2 = 0.0;
    for (int f = 120; f < 240; ++f) {
      m += run[size_t(f)][i];
      m2 += double(run[size_t(f)][i]) * run[size_t(f)][i];
      r.frame_delta += std::abs(run[size_t(f)][i] - run[size_t(f - 1)][i]);
    }
    m /= 120.0;
    m2 /= 120.0;
    flicker += std::sqrt(std::max(m2 - m * m, 0.0)) / truth[i];
    out_sum += m;
    truth_sum += truth[i];
    ++n;
  }
  r.flicker = flicker / std::max(n, 1);
  r.frame_delta /= 120.0 * std::max(n, 1);
  r.energy = out_sum / std::max(truth_sum, 1e-9);
  const std::vector<std::vector<float>> gone = ResolveRun(sc, resolve, 200, 100, keep);
  const auto added = [&](int f) {
    double a = 0.0;
    for (size_t i = 0; i < truth.size(); ++i) {
      if (truth[i] >= kResolveBg * 1.5f) a += double(gone[size_t(f)][i]) - kResolveBg;
    }
    return a;
  };
  const double start = added(99);
  r.fade_half = r.fade_tenth = 999;
  for (int f = 100; f < 200; ++f) {
    if (r.fade_half == 999 && added(f) < 0.5 * start) r.fade_half = f - 99;
    if (r.fade_tenth == 999 && added(f) < 0.1 * start) r.fade_tenth = f - 99;
  }
  return r;
}
}  // namespace

void TestResolve() {
  // the kernel: a tent of 16, the centre 4 times a corner
  float total = 0.0f;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) total += hs::SsrTapKernel(dx, dy);
  }
  Check(total == 16.0f && hs::SsrTapKernel(0, 0) == 4.0f && hs::SsrTapKernel(1, 0) == 2.0f && hs::SsrTapKernel(0, -1) == 2.0f && hs::SsrTapKernel(1, 1) == 1.0f && hs::SsrTapKernel(-1, 1) == 1.0f,
        "the resolve's kernel is a tent: 4 in the centre, 2 on the edges, 1 in the corners (sum 16)");
  // the same surface: a few per cent and a few centimetres of receiver depth (a road is flat; a kerb, a car, a pole are not)
  Check(hs::SsrTapSameSurface(20.0f, 20.0f) && hs::SsrTapSameSurface(20.0f, 20.8f) && !hs::SsrTapSameSurface(20.0f, 22.0f) && hs::SsrTapSameSurface(0.5f, 0.54f) && !hs::SsrTapSameSurface(0.5f, 0.6f) &&
            !hs::SsrTapSameSurface(5.0f, 5.5f),
        "a neighbour joins the average only within 8% and 5 centimetres of the centre's receiver depth (the history's own rule)");
  {
    bool same_rule = true;
    for (float z : {0.5f, 5.0f, 20.0f, 60.0f}) {
      for (float k : {0.8f, 0.9f, 0.95f, 1.0f, 1.05f, 1.1f, 1.2f}) same_rule = same_rule && hs::SsrTapSameSurface(z * k, z) == hs::SsrSameSurface(z * k, z);
    }
    Check(same_rule, "and it is the history's rule, not a tighter one (a road at a grazing angle runs away by metres a row at 50 metres)");
  }
  // the clip's floor: a positive history is not taken below the keep; a raise, a negative history and a history inside the box are the clip's
  const hs::float3 down = hs::SsrKeepClip(hs::float3(10.0f, 10.0f, 10.0f), hs::float3(1.0f, 9.0f, 5.0f), hs::kSsrClipKeep);
  Check(Near(down.x, 10.0 * hs::kSsrClipKeep, 1e-5) && down.y == 9.0f && Near(down.z, 10.0 * hs::kSsrClipKeep, 1e-5), "the clip takes at most 1 - keep of a positive history away in a frame, and its small steps whole");
  const hs::float3 raised = hs::SsrKeepClip(hs::float3(1.0f, 0.0f, 3.0f), hs::float3(4.0f, 2.0f, 3.0f), hs::kSsrClipKeep);
  Check(raised.x == 4.0f && raised.y == 2.0f && raised.z == 3.0f, "a history the clip raises or leaves is the clip's");
  const hs::float3 negative = hs::SsrKeepClip(hs::float3(-5.0f, -5.0f, -5.0f), hs::float3(-9.0f, -1.0f, -5.0f), hs::kSsrClipKeep);
  Check(negative.x == -9.0f && negative.y == -1.0f && negative.z == -5.0f, "a negative history (the road's term can be) is the clip's, whole");
  Check(hs::kSsrClipKeep > 0.5f && hs::kSsrClipKeep < 0.95f, "the keep is a real floor, not a switch");
  // NOISE-FOLLOW: the floor only where our resolve is the only temporal filter: with a temporal upscaler, or the first version's resolve, there is none (the clip is whole)
  {
    const hs::float3 history(10.0f, 10.0f, 10.0f), clipped(1.0f, 9.0f, -4.0f);
    const hs::float3 floored = hs::SsrKeepClip(history, clipped, hs::SsrClipKeepFor(true, false));
    const hs::float3 upscaled = hs::SsrKeepClip(history, clipped, hs::SsrClipKeepFor(true, true));
    const hs::float3 first = hs::SsrKeepClip(history, clipped, hs::SsrClipKeepFor(false, false));
    Check(hs::SsrClipKeepFor(true, false) == hs::kSsrClipKeep && hs::SsrClipKeepFor(true, true) == hs::kSsrClipKeepUpscaler && hs::kSsrClipKeepUpscaler == 0.0f && hs::SsrClipKeepFor(false, false) == 0.0f && hs::SsrClipKeepFor(false, true) == 0.0f,
          "the floor is kSsrClipKeep with the spatial resolve and no temporal upscaler, kSsrClipKeepUpscaler (0) with an upscaler, and none with the first version's resolve");
    Check(Near(floored.x, 10.0 * hs::kSsrClipKeep, 1e-5) && floored.y == 9.0f && Near(floored.z, 10.0 * hs::kSsrClipKeep, 1e-5) && upscaled.x == 1.0f && upscaled.y == 9.0f && upscaled.z == -4.0f && first.x == 1.0f && first.z == -4.0f,
          "with an upscaler the clip is whole (also where it goes negative: no floor to lift it), without one it is floored");
  }
  Check(hs::SsrSparkleClipKeep(0.0f, 0.0f) == 0.75f && hs::SsrSparkleClipKeep(-0.5f, 1.0f) == 0.375f &&
            hs::SsrSparkleClipKeep(-1.0f, 0.5f) == 0.375f && hs::SsrSparkleClipKeep(2.0f, 0.0f) == 0.0f &&
            hs::SsrSparkleClipKeep(0.0f, -4.0f) == 0.0f, "the positive-history floor tapers with the largest reprojection displacement in SSR texels");
  const hs::float3 dark(0.0f, 0.0f, 0.0f);
  const hs::float3 last_bright(12.0f, 8.0f, -3.0f);
  const hs::float3 after_miss = hs::SsrKeepClip(last_bright, dark, hs::SsrSparkleClipKeep(0.0f, 0.0f));
  Check(after_miss.x == 9.0f && after_miss.y == 6.0f && after_miss.z == 0.0f, "an empty clip box retains some bright history and clips the signed road subtraction");
  // the reprojection's distance: the pixel's own when its ray found something, else the nearest hit round it
  Check(hs::SsrReprojectDistance(30.0f, 12.0f) == 30.0f && hs::SsrReprojectDistance(hs::kSsrMissLength, 40.0f) == 40.0f && hs::SsrReprojectDistance(hs::kSsrMissLength, hs::kSsrMissLength) == hs::kSsrMissLength &&
            hs::SsrReprojectDistance(hs::kSsrMissLength, 5.0f) == 5.0f,
        "where the ray found nothing the nearest hit of the same-surface neighbours gives the reprojection's distance; a pixel that hit keeps its own");
  // and why it matters: with the first version's hit or miss a pixel round a lamp's image looked its history up in two places (the lamp's parallax, the sky's), a few pixels apart on a moving camera
  {
    const Vec H{{3.0, 6.0, 40.0}};
    const Vec Hmirror{{H.v[0], -H.v[1], H.v[2]}};
    const Pose before{{0.0, 1.5, 0.0}, 0.0, 0.02};
    const Pose now{{0.0, 1.5, 0.55}, 0.0, 0.02};   // 0.55 m a frame at 60 Hz is 120 km/h
    const double tan_x = 0.9, tan_y = 0.5;
    const Vec cam{{now.pos[0], now.pos[1], now.pos[2]}};
    const Vec dir = Sub(Hmirror, cam);
    const double t = (0.0 - cam.v[1]) / dir.v[1];
    const Vec P_view = ToView(now, {{cam.v[0] + dir.v[0] * t, 0.0, cam.v[2] + dir.v[2] * t}});
    const Vec H_view = ToView(now, H);
    const double length_to_hit = std::sqrt(DotV(Sub(H_view, P_view), Sub(H_view, P_view)));
    hs::float4 rows[3];
    {
      Vec r, u, f, r0, u0, f0;
      Axes(now, &r, &u, &f);
      Axes(before, &r0, &u0, &f0);
      const Vec dp = Sub({{now.pos[0], now.pos[1], now.pos[2]}}, {{before.pos[0], before.pos[1], before.pos[2]}});
      const Vec axes_old[3] = {r0, u0, f0};
      for (int k = 0; k < 3; ++k) rows[k] = hs::float4(float(DotV(r, axes_old[k])), float(DotV(u, axes_old[k])), float(DotV(f, axes_old[k])), float(DotV(dp, axes_old[k])));
    }
    const hs::float3 P(float(P_view.v[0]), float(P_view.v[1]), float(P_view.v[2]));
    const auto where = [&](float distance) {
      const hs::SsrPrevious p = hs::SsrToPrevious(rows[0], rows[1], rows[2], hs::SsrVirtualPoint(P, distance), float(tan_x), float(tan_y));
      return std::array<double, 2>{p.u * 960.0, p.v * 540.0};   // the SSR images of a 1920 x 1080 picture
    };
    const auto hit = where(float(length_to_hit)), miss = where(hs::kSsrMissLength), stable = where(hs::SsrReprojectDistance(hs::kSsrMissLength, float(length_to_hit)));
    const double apart = std::hypot(hit[0] - miss[0], hit[1] - miss[1]), after = std::hypot(hit[0] - stable[0], hit[1] - stable[1]);
    if (g_print) std::printf("a pixel round a lamp's image on a camera at 120 km/h: hit and miss look the history up %.2f SSR pixels apart; with the nearest hit %.2f\n", apart, after);
    Check(apart > 0.4 && after < 1e-6, Num("a miss next to a hit looked its history up %.2f pixels from the hit's place; with the nearest hit it is the same place (%.4f)", apart, after));
  }
  // THE NOISE THAT IS LEFT (the pipeline above on three small bright things)
  struct Case {
    const char* name;
    ResolveScene scene;
  };
  const Case cases[] = {{"a lamp 4 pixels wide, a lobe of 2", {2.0f, 4.0f}}, {"a tail light 2 pixels wide, a lobe of 3", {3.0f, 2.0f}}, {"a lamp 3 pixels wide, a lobe of 1", {1.0f, 3.0f}}};
  for (const Case& c : cases) {
    const ResolveNumbers then = ResolveMeasure(c.scene, false), now = ResolveMeasure(c.scene, true);
    if (g_print) {
      std::printf("%s: first resolve flicker %.3f energy %.3f fade %d / %d frames; resolve flicker %.3f energy %.3f fade %d / %d frames\n", c.name, then.flicker, then.energy, then.fade_half, then.fade_tenth, now.flicker, now.energy,
                  now.fade_half, now.fade_tenth);
    }
    Check(now.flicker < 0.70 * then.flicker, std::string(c.name) + Num(": the flicker on the lamp's pixels is %.0f%% of what the first version left", 100.0 * now.flicker / then.flicker) + Num(" (%.2f of the truth against %.2f)", now.flicker, then.flicker));
    Check(now.energy > then.energy - 1e-3 && now.energy > 0.45, Num("... and it keeps the reflection's brightness at least as well: %.2f of the lobe's mean against %.2f", now.energy, then.energy));
    Check(now.fade_half <= 2 && now.fade_tenth <= 6 && then.fade_half <= 2, Num("... and a lamp that goes out leaves half of its reflection after %.0f frames (at most 2, the comment's own: the first version took 1 or 2)", double(now.fade_half)) + Num(", a tenth after %.0f (at most 6)", double(now.fade_tenth)));
    // NOISE-FOLLOW: with a temporal upscaler the floor is off: no trail of our own on top of the upscaler's (the first version's fade, a frame or two), and the tent average alone still takes the flicker down
    const ResolveNumbers up = ResolveMeasure(c.scene, true, true);
    if (g_print) std::printf("%s: with an upscaler (no floor): flicker %.3f energy %.3f fade %d / %d frames\n", c.name, up.flicker, up.energy, up.fade_half, up.fade_tenth);
    Check(up.fade_half <= 2 && up.fade_tenth <= 3 && up.fade_tenth < now.fade_tenth, std::string(c.name) + Num(": with a temporal upscaler a lamp that goes out leaves half after %.0f frames and a tenth after %.0f (at most 2 and 3: no trail of ours; %.0f with the floor)", double(up.fade_half), double(up.fade_tenth), double(now.fade_tenth)));
    Check(up.flicker < 0.70 * then.flicker && up.energy >= then.energy - 0.04 && up.energy <= now.energy + 1e-3,
          std::string(c.name) + Num(": ... and its flicker is still %.0f%% of the first version's (the tent average alone; the upscaler averages the rest) at the first version's brightness within 0.04 (%.2f against %.2f; the floor adds %.2f on top, which is the price of the trail)", 100.0 * up.flicker / then.flicker, up.energy, then.energy, now.energy - up.energy));
    const ResolveNumbers fixed = ResolveMeasure(c.scene, true, true, true);
    if (g_print) {
      std::printf("%s: upscaler mean frame delta %.3f before / %.3f after, energy %.3f after, fade %d / %d frames\n",
                  c.name, up.frame_delta, fixed.frame_delta, fixed.energy, fixed.fade_half, fixed.fade_tenth);
    }
    Check(fixed.frame_delta < 0.92 * up.frame_delta, std::string(c.name) + ": the stationary floor reduces frame-to-frame changes with the same blue noise");
    Check(fixed.fade_half <= 2 && fixed.fade_tenth <= 6, std::string(c.name) + ": the stationary floor bounds how long an extinguished highlight persists");
  }
}

// ---------------------------------------------------------------------------
// GB-I2: the road view's rules and its push word
// ---------------------------------------------------------------------------

void TestRoadViewRules() {
  sr::RoadViewFacts f;
  f.share = 1.0f;
  f.has_target = true;
  f.is_reflection_class = true;
  f.drawn_this_frame = true;
  f.slot_ok = true;
  f.side = 256;
  f.host_samples = 2;
  Check(sr::DecideRoadView(f) == sr::RoadViewWhy::kUsed, "a drawn road view with a slot is used");
  sr::RoadViewFacts g = f;
  g.share = 0.0f;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kOff, "off by the setting (or by the game showing none of it)");
  g = f;
  g.has_target = false;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kNoTarget, "no target");
  g = f;
  g.is_reflection_class = false;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kNotClassified, "another use of the identity");
  g = f;
  g.drawn_this_frame = false;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kNotDrawn, "another frame's picture is not read");
  g = f;
  g.slot_ok = false;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kNoSlot, "no shader view");
  for (const uint32_t side : {0u, 1024u, 5000u}) {
    g = f;
    g.side = side;
    Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kBadSize, Num("a side of %.0f does not fit the word", side));
  }
  g = f;
  g.host_samples = 9;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kBadSize, "nine samples do not fit");
  g = f;
  g.side = 768;
  g.host_samples = 1;
  Check(sr::DecideRoadView(g) == sr::RoadViewWhy::kUsed, "the 3x view (768, one sample) fits");
  bool named = true;
  for (uint32_t i = 0; i < uint32_t(sr::RoadViewWhy::kCount); ++i) named = named && std::string(sr::RoadViewWhyName(sr::RoadViewWhy(i))) != "?";
  Check(named && std::string(sr::RoadViewWhyName(sr::RoadViewWhy::kCount)) == "?", "every reason has a name");
  // the word
  bool word_ok = true;
  for (const uint32_t slot : {0u, 5u, 1234u, 32767u}) {
    for (const uint32_t samples : {1u, 2u, 4u, 8u}) {
      for (const uint32_t side : {256u, 512u, 768u, 1023u}) {
        const uint32_t w = sr::PackRoadView(slot, samples, side);
        word_ok = word_ok && hs::SsrRoadViewSlot(w) == slot && hs::SsrRoadViewSamples(w) == samples && hs::SsrRoadViewSide(w) == side && hs::SsrHasSlot(hs::SsrRoadViewSlot(w));
      }
    }
  }
  Check(word_ok, "the road view's word: slot, host samples and side come back");
  Check(!hs::SsrHasSlot(hs::SsrRoadViewSlot(sr::kNoRoadView)) && hs::SsrRoadViewSide(sr::kNoRoadView) == 0 && !hs::SsrHasSlot(hs::SsrRoadViewSlot(sr::PackRoadView(0xFFFFFFFFu, 2, 256))), "no view: no slot, and a none slot stays none");
  Check(sr::PackRoadView(7, 0, 256) == sr::PackRoadView(7, 1, 256), "no samples is one");
  // the push block's words of the three new uses
  namespace p = sr::push;
  Check(SSR_W_ROAD_VIEW == int(p::kRoadView) && SSR_W_FALLBACK == int(p::kFallback) && SSR_W_VEH_TABLE == int(p::kVehTable) && SSR_W_STENCIL == int(p::kStencil) && SSR_W_ROAD_VIEW_APPLY == int(p::kRoadViewApply),
        "the new words are the same in the .hlsli and the host");
  Check(p::kRoadView == p::kSrc && p::kFallback == p::kStrength && p::kStencil == p::kLin0 && p::kVehTable == p::kRt2Rt3 && p::kRoadViewApply == p::kHiz01, "and each sits in a word its pass does not read for its first meaning");
  // the debug views
  Check(sr::ViewFromSetting(5) == sr::View::kRoadView && sr::ViewFromSetting(6) == sr::View::kDiff && sr::ViewFromSetting(7) == sr::View::kOff && int(sr::View::kCount) == 7 && int(sr::View::kCount) <= 16, "views 5 (the road view) and 6 (the term's sign)");
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
// RR-4a: the raw mode's rules
// ---------------------------------------------------------------------------

void TestRaw() {
  namespace rr = mcla::gpu::rr;
  // DecideRaw over every combination of its facts, against a second statement of the order of the reasons
  bool agrees = true, possible_agrees = true, only_raw_has_all = true;
  int raw_cells = 0;
  for (int asked = 0; asked < 2; ++asked) {
    for (int view = 0; view < 2; ++view) {
      for (int stub = 0; stub < 2; ++stub) {
        for (uint32_t samples : {1u, 2u, 4u, 8u}) {
          for (int ready = 0; ready < 2; ++ready) {
            sr::RawFacts f;
            f.asked = asked != 0;
            f.debug_view = view != 0;
            f.stub = stub != 0;
            f.host_samples = samples;
            f.pipelines_ready = ready != 0;
            const sr::RawWhy got = sr::DecideRaw(f);
            const sr::RawWhy want = !asked ? sr::RawWhy::kNotAsked : view ? sr::RawWhy::kDebugView : stub ? sr::RawWhy::kStub : samples != 1 ? sr::RawWhy::kMultisampled : !ready ? sr::RawWhy::kPipelines : sr::RawWhy::kRaw;
            agrees = agrees && got == want;
            raw_cells += got == sr::RawWhy::kRaw ? 1 : 0;
            only_raw_has_all = only_raw_has_all && ((got == sr::RawWhy::kRaw) == (asked && !view && !stub && samples == 1 && ready));
            // the pipelines are asked for only for a frame that could take them: everything but the pipelines
            possible_agrees = possible_agrees && sr::RawPossible(f) == (asked && !view && !stub && samples == 1);
          }
        }
      }
    }
  }
  Check(agrees, "RR-4a: DecideRaw: not asked, a debug view, the stub, a multisampled colour, pipelines not made: in that order, else raw (2 x 2 x 2 x 4 x 2 facts)");
  Check(raw_cells == 1 && only_raw_has_all, "RR-4a: exactly one cell of the table is raw: asked, no view, RT3 there, one sample, the pipelines made");
  Check(possible_agrees, "RR-4a: RawPossible is everything but the pipelines (the raw pipelines are listed with the precache only for such a frame: with the plan not RR none ever is)");
  {
    sr::RawFacts f;
    Check(sr::DecideRaw(f) == sr::RawWhy::kNotAsked && !sr::RawPossible(f), "RR-4a: the default frame (the plan not RR) is not asked: the plain passes");
    for (int w = 0; w < int(sr::RawWhy::kCount); ++w) {
      const std::string name = sr::RawWhyName(sr::RawWhy(w));
      Check(!name.empty() && name != "?", "RR-4a: raw reason " + std::to_string(w) + " has a name");
    }
  }
  // the hit distance image and the held bytes
  Check(sr::HitImageBytes(1920, 1080) == 1920ull * 1080ull * 2ull && sr::HitImageBytes(3840, 2160) == 3840ull * 2160ull * 2ull && sr::kBytesR16f == 2, "RR-4a: the hit distance image is R16_FLOAT at the picture's size: 4.0 MiB at 1080p, 15.8 MiB at 4K");
  Check(sr::RawHeldBytes(960, 540, 1920, 1080) == sr::ImageBytes(960, 540) + sr::HitImageBytes(1920, 1080) && sr::RawHeldBytes(960, 540, 1920, 1080) < sr::HeldBytes(960, 540),
        "RR-4a: a raw frame holds the trace and the hit image, less than the plain three (the accumulation's two are not needed)");
  Check(sr::kRawReleaseAfter == 120, "RR-4a: the images the mode does not need go back after 120 frames of the other mode");
  // the miss distance is the same number as the guide side's (dlss_rr_rules.h kRawMissDistance) and as the trace's alpha holds for a ray that found nothing
  Check(hs::kSsrMissLength == rr::kRawMissDistance && rr::RawHitDistance(true, false, 0.0f) == hs::kSsrMissLength, "RR-4a: a ray that found nothing is 4096 on both sides (the guide's raw rule and the trace's miss length)");
  // the distance the hit pass reads back from the trace's alpha is the one the trace packed (a vehicle's, rounded to the 3% its ID took)
  Check(hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(12.5f, false, 0))) == 12.5f && hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(12.5f, true, 7))) > 12.0f && hs::SsrHitDistance(ThroughHalf(hs::SsrPackHit(12.5f, true, 7))) < 13.0f,
        "RR-4a: the hit pass reads the trace's alpha through SsrHitDistance: the distance, within 3% for a vehicle's hit");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

// The text of a shader without its RR-4a raw compilation's block: from `#if SSR_RAW` to `#else`, the `#endif` dropped (what the plain compilations contain).
std::string PlainOf(const std::string& text) {
  const size_t a = text.find("#if SSR_RAW\n");
  if (a == std::string::npos) return text;
  const size_t e = text.find("#else\n", a);
  const size_t f = text.find("#endif\n", a);
  if (e == std::string::npos || f == std::string::npos || e > f) return text;
  return text.substr(0, a) + text.substr(e + 6, f - (e + 6)) + text.substr(f + 7);
}

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

// RR-4a: the raw mode's sources: the shaders, their registration, the host's use of the rules (nothing raw unless asked: the plain frame is the old one), the wiring and the debug switch.
void TestRawSources(const std::string& dir, const std::string& rules, const std::string& header, const std::string& cpp, const std::string& scene, const std::string& cmake, const std::string& shaders_h,
                    const std::string& pass, const std::string& apply_ps, const std::string& settings, const std::string& manifest, const std::string& meta) {
  const std::string hit_ps = Read(dir, "hlsl/ssr_hit_ps.hlsl"), trace_ps = Read(dir, "hlsl/ssr_trace_ps.hlsl"), temporal = Read(dir, "temporal.cpp"), temporal_h = Read(dir, "temporal.h");
  const std::string trigger = FunctionText(cpp, "bool ScreenReflections::OnTrigger(");
  // the shaders: the raw compilation of the apply and the hit pass
  Check(Has(cmake, "larecomp_gpu_host_shader(ssr_apply_raw_ps ps_6_0 SOURCE ssr_apply_ps DEFINES SSR_MS=0 SSR_RAW=1)") && Has(cmake, "larecomp_gpu_host_shader(ssr_hit_ps ps_6_0 DEFINES SSR_MS=0)"),
        "RR-4a: CMake builds the raw resolve (the apply with SSR_RAW) and the hit pass, over plain planes");
  Check(Has(shaders_h, "case 7: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_raw_ps);") && Has(shaders_h, "default: return MCLA_GPU_HOST_SHADER_BLOB(ssr_hit_ps);") && Has(shaders_h, "case 4: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_ms_ps);") &&
            Has(shaders_h, "case 0: return MCLA_GPU_HOST_SHADER_BLOB(ssr_trace_ps);") && Has(shaders_h, "case 3: return MCLA_GPU_HOST_SHADER_BLOB(ssr_apply_ps);"),
        "RR-4a: ScreenReflectionsPixelShader numbers them 7 and 8 (after the old five and R4's two) and the old ones keep their numbers");
  Check(Has(header, "enum Shader : uint32_t { kShaderTrace, kShaderTraceMs, kShaderTemporal, kShaderApply, kShaderApplyMs, kShaderTraceRt, kShaderTraceRtMs, kShaderApplyRaw, kShaderHit, kShaderCount };") &&
            Has(header, "enum Variant : uint32_t { kVTrace, kVTraceMs, kVTemporal, kVApplyBase, kVTraceRt = kVApplyBase + 8, kVTraceRtMs, kVApplyRaw, kVHit, kVariantCount };"),
        "RR-4a: the shader and variant enums add the two after the old ones (the old numbering is untouched)");
  // the apply: the raw block only in the raw compilation, the plain branch exactly what it was
  Check(Has(apply_ps, "#ifndef SSR_RAW\n#define SSR_RAW 0\n#endif") && Has(apply_ps, "#if SSR_RAW\n    if (t.w != 0.0f) {\n      const float dz = (SsrHizAt(SsrHz(), q.x, q.y) - z) / sigma;") &&
            Has(apply_ps, "#else\n    if (t.w > 0.0f) {\n      const float dz = (t.w - z) / sigma;\n      w = wx * wy / (1.0f + dz * dz);\n    }\n#endif") && Count(apply_ps, "#if SSR_RAW") == 1,
        "RR-4a: the apply's raw compilation weighs the trace's texels by the Hi-Z's receiver depth (the trace's alpha is the hit), and the plain branch is the one it always was; one block only");
  Check(!Has(PlainOf(apply_ps), "SsrHizAt(") && Has(PlainOf(apply_ps), "if (t.w > 0.0f) {"), "RR-4a: with the raw block out the apply is the old text (no Hi-Z read, the accumulation's alpha test)");
  // the hit pass: the apply's gates and weights, the trace's alpha through SsrHitDistance, nothing for a vehicle
  Check(Has(hit_ps, "SsrClassGain(surface.cls, surface.flags)") && Has(hit_ps, "SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET))") && Has(hit_ps, "SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH))") &&
            Has(hit_ps, "SsrViewNormalAt(p, pc).w < 0.5f") && Has(hit_ps, "const float sigma = 0.03f * z + 0.03f;") && Has(hit_ps, "w = wx * wy / (1.0f + dz * dz);") && Has(hit_ps, "(SsrHizAt(SsrHz(), q.x, q.y) - z) / sigma") &&
            Has(hit_ps, "return float4(SsrHitDistance(best_hit), 0.0f, 0.0f, 0.0f);") && Has(hit_ps, "const SsrSurface surface = SsrSurfaceAt(p, false);"),
        "RR-4a: the hit pass has the apply's gates (class, smoothness, ramp, normal) and the apply's weights, and writes the heaviest trace texel's distance in x");
  Check(Has(apply_ps, "const float sigma = 0.03f * z + 0.03f;") && Has(apply_ps, "const float2 uv_ssr = (float2(p) + 0.5f) * 0.5f - 0.5f;") && Has(hit_ps, "const float2 uv_ssr = (float2(p) + 0.5f) * 0.5f - 0.5f;") &&
            Has(apply_ps, "const int2 q = clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), ssr - 1);") && Has(hit_ps, "const int2 q = clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), ssr - 1);"),
        "RR-4a: the hit pass takes the same four texels of the same place as the apply (the two are one rule: change one, change the other)");
  Check(!Has(hit_ps, "SSR_W_ROAD_VIEW") && !Has(hit_ps, "SSR_W_STRENGTH") && !Has(hit_ps, "SSR_W_FRAME") && Has(hit_ps, "SsrLo(SsrW(SSR_W_SRC))") && Has(hit_ps, "SsrHizAt(SsrHz(), q.x, q.y)"),
        "RR-4a: the hit pass reads no word the other passes borrowed (the road view, the strength, the frame): the trace slot in the source word's low half and the Hi-Z");
  Check(Has(pass, "float SsrHizAt(uint level, int cx, int cy) {"), "RR-4a: the Hi-Z reader the raw compilation and the hit pass use is the pass header's one");
  // the host: nothing raw unless asked; the pipelines listed only for a frame that could take them; the passes; the order
  Check(Has(cpp, "constexpr plume::RenderFormat kHitFormat = plume::RenderFormat::R16_FLOAT;") && Has(cpp, "case kVHit: return make(kShaderHit, copy, 1, kHitFormat);") && Has(cpp, "case kVApplyRaw: return make(kShaderApplyRaw, apply_blend(false), 1);"),
        "RR-4a: the hit image is R16_FLOAT and the pipelines are the raw resolve (adding, one sample) and the hit pass (copy)");
  const size_t possible_at = trigger.find("if (ssr::RawPossible(raw_facts)) {");
  const size_t apply_pipe_at = trigger.find("Pipeline(kVApplyRaw)");
  const size_t hit_pipe_at = trigger.find("Pipeline(kVHit)");
  Check(possible_at != std::string::npos && apply_pipe_at != std::string::npos && hit_pipe_at != std::string::npos && possible_at < apply_pipe_at && possible_at < hit_pipe_at && Count(cpp, "Pipeline(kVApplyRaw)") == 2 &&   // the raw mode's, and AMD FSR Ray Regeneration's (RG-3: the same resolve over the denoised image, asked inside rgd::RegenPossible)
            
            Count(cpp, "Pipeline(kVHit)") == 1,
        "RR-4a: the raw pipelines are asked for in one place, inside the rules' RawPossible: a frame that is not asked never lists them with the host precache");
  Check(Has(trigger, "raw_facts.asked = raw_requested;") && Has(trigger, "raw_facts.debug_view = debug_view;") && Has(trigger, "raw_facts.stub = decision.stub;") && Has(trigger, "raw_facts.host_samples = depth.host_samples;") &&
            Has(trigger, "bool raw = raw_why == ssr::RawWhy::kRaw;") && Has(trigger, "raw_why = ssr::RawWhy::kImages;") && Has(header, "bool OnTrigger(const ResolveCache::Summary& summary, plume::RenderCommandList* list, bool raw_requested = false, bool upscaler_running = false, const RegenAsk& regen = RegenAsk{});"),
        "RR-4a: the frame is raw by the rules' decision on what the caller asked (the default is plain), a hit image that cannot be made keeps the frame plain");
  Check(Has(trigger, "if (raw_ever_asked_ && uint32_t(raw_why) != raw_why_logged_) {") && Has(trigger, "if (raw_requested) {\n    raw_ever_asked_ = true;\n  }"), "RR-4a: nothing is logged until the mode was asked for once: the log of a run with the plan not RR is what it was");
  Check(Has(trigger, "  if (!raw && !regen_ok) {\n    // The accumulation.") && Has(trigger, "pass_timing_->Mark(list, \"ssr temporal\");\n    DrawPass(list, accum_[write_], temporal_pipeline, temporal);\n  }"),
        "RR-4a: the accumulation runs only in a plain frame");
  Check(Has(trigger, "if (raw) {\n    // RR-4a: the resolve reads the trace itself") && Has(trigger, "apply.Set(pk::kSrc, ssr::PackSlots(trace_.slot, trace_.slot));") &&
            Has(trigger, "  } else {\n    apply.Set(pk::kSrc, ssr::PackSlots(resolve_source->slot, trace_.slot));   // the accumulation, or its filtered image (RtDenoise)\n    apply.Set(pk::kRoadViewApply, road_word);\n  }") &&
            Has(trigger, "list->setPipeline(raw ? raw_apply_pipeline : (regen_ok ? regen_apply_pipeline : apply_pipeline));") && Has(trigger, "pass_timing_->Mark(list, \"ssr apply raw\");"),
        "RR-4a: the raw resolve reads the trace in both halves of the source word and keeps the Hi-Z words (the road view's borrowing is the plain apply's)");
  const size_t apply_mark = trigger.find("Mark(list, \"ssr apply\")"), hit_mark = trigger.find("Mark(list, \"ssr hit\")"), after_mark = trigger.find("Mark(list, \"after ssr\")"), hit_draw = trigger.find("DrawPass(list, hit_, hit_pipeline, hit_push);");
  const size_t apply_draw = trigger.find("list->drawInstanced(3, 1, 0, 0);");
  Check(apply_mark != std::string::npos && hit_mark != std::string::npos && after_mark != std::string::npos && apply_draw != std::string::npos && hit_draw != std::string::npos && apply_draw < hit_mark && hit_mark < hit_draw && hit_draw < after_mark &&
            Has(trigger, "hit_push.Set(pk::kSrc, ssr::PackSlots(trace_.slot, trace_.slot));") && Has(trigger, "hit_ready_ = true;") && Count(cpp, "DrawPass(list, hit_,") == 1,
        "RR-4a: in a raw frame the hit pass follows the resolve's draw and comes before the closing mark, reading the trace; the image is announced only then");
  Check(Has(trigger, "  if (!raw && !regen_ok) {\n    last_key_ = now;") && Has(trigger, "  } else {\n    last_key_.valid = false;\n  }") && Has(trigger, "plain_streak_") && Has(trigger, "raw_streak_ >= ssr::kRawReleaseAfter"),
        "RR-4a: a raw frame leaves no history (the frame after it starts again), and the images of the mode not in use go back after a spell");
  Check(Has(FunctionText(cpp, "void ScreenReflections::BeginFrame()"), "hit_ready_ = false;") && Has(header, "const HostImage* RawHitImage() const { return hit_ready_ ? &hit_ : nullptr; }") &&
            Has(FunctionText(cpp, "void ScreenReflections::ReleaseImages()"), "&hit_") && Has(FunctionText(cpp, "uint64_t ScreenReflections::HeldBytes() const"), "&hit_"),
        "RR-4a: the hit image is announced for the frame that wrote it only, released with the others and counted in the memory line");
  // the wiring: the caller asks the temporal system, which says false with the plan not RR; the image goes to the guide pass
  Check(Has(scene, "const bool raw_ssr = temporal_ && temporal_->RawModes().Ssr();") && Has(scene, "temporal_->NoteRrHit(ssr_ ? ssr_->RawHitImage() : nullptr,") && Has(temporal_h, "rr::RawModes RawModes();") &&
            Has(temporal_h, "void NoteRrHit(const HostImage* ssr, const HostImage* car_refl)"),
        "RR-4a: Scene asks TemporalSystem::RawModes for the effect and hands the hit image over after the trigger's passes");
  Check(Has(temporal, "in.hit[0] = rr_hit_[0];") && Has(temporal, "in.hit[1] = rr_hit_[1];") && Has(temporal, "rr_hit_[0] = rr_hit_[1] = nullptr;"), "RR-4a: the guide pass gets this frame's hit images; the frame boundary forgets last frame's");
  const std::string raw_modes = FunctionText(temporal, "rr::RawModes TemporalSystem::RawModes() {");
  Check(Has(raw_modes, "f.play_view = frame_.samples != 0 && frame_.view == temporal::View::kPlay;") && Has(raw_modes, "rr::RawModesOf(f, rr::RawSwitchOn(") && Has(raw_modes, "if (f.plan_rr || rr_raw_logged_) {"),
        "RR-4a: RawModes adds the play view to what the boundary knew, takes the two switches, and logs/counts only once the plan has been RR");
  Check(Has(temporal, "frame_.raw.plan_rr = rr_plan_.use_rr && effective_ == Upscaler::kDlss;") && Has(temporal, "if (frame_.raw.plan_rr) {\n    frame_.raw.failed = dlss::GetRayReconstructionFacts().failed;") &&
            Has(temporal, "frame_.raw.context_ready = ready && dlss_ && rr_context_asked_ && dlss_->CompanionReady(rr_request_);") && Has(temporal, "frame_.raw.armed = frame_.armed;"),
        "RR-4a: the boundary's facts (plan RR on DLSS, no failure, the context made, the frame armed) are read only while the plan is RR");
  // the debug switch: debug-only, live, default 1, in the three generated files
  const std::string rex_setting = std::string("REX_") + "SETTING(";
  bool in_all = true;
  for (const char* name : {"mcla_gpu_dlss_rr_ssr", "mcla_gpu_dlss_rr_car_refl"}) {
    const std::string n = name;
    const size_t decl = settings.find(rex_setting + "i32, " + n + ", 1,");
    const size_t end = decl == std::string::npos ? std::string::npos : settings.find(");\n", decl);
    const bool tagged = decl != std::string::npos && end != std::string::npos && settings.substr(decl, end - decl).find("rex::kDebugOnly, rex::Range{0, 1}") != std::string::npos;
    const bool restart = decl != std::string::npos && end != std::string::npos && settings.substr(decl, end - decl).find("kRestart") != std::string::npos;
    in_all = in_all && tagged && !restart && Has(settings, "inline auto& " + n + " = g_gpu." + n + ";") && Has(manifest, "\"" + n + "\": {") && Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},");
  }
  Check(in_all, "RR-4: the two switches (mcla_gpu_dlss_rr_ssr, mcla_gpu_dlss_rr_car_refl) are debug-only, live, 0 or 1, on by default, in the settings header, its aliases, the manifest and the meta list");
  Check(Has(rules, "kRawReleaseAfter = 120") && Has(rules, "enum class RawWhy : uint8_t {"), "RR-4a: the rules header carries the raw rules");
  (void)trace_ps;
}

void TestSources(const std::string& dir) {
  if (dir.empty()) return;
  const std::string rules = Read(dir, "ssr_rules.h"), header = Read(dir, "ssr.h"), cpp = Read(dir, "ssr.cpp"), scene = Read(dir, "scene.cpp"), scene_h = Read(dir, "scene.h"), renderer = Read(dir, "renderer.cpp"),
                    renderer_h = Read(dir, "renderer.h"), cmake = Read(dir, "CMakeLists.txt"), shaders_h = Read(dir, "host_shaders.h"), precache = Read(dir, "host_precache.h"), ao_h = Read(dir, "ao.h"), ao_cpp = Read(dir, "ao.cpp"),
                    common = Read(dir, "hlsl/ssr_common.hlsli"), pass = Read(dir, "hlsl/ssr_pass.hlsli"), trace_ps = Read(dir, "hlsl/ssr_trace_ps.hlsl"), temporal_ps = Read(dir, "hlsl/ssr_temporal_ps.hlsl"),
                    apply_ps = Read(dir, "hlsl/ssr_apply_ps.hlsl"), trace_h = Read(dir, "hlsl/ssr_trace.hlsli");
  const std::string settings = Read(dir, "../settings/generated/settings_gpu.h"), manifest = Read(dir, "../settings/generated/manifest.json"), meta = Read(dir, "../settings/generated/settings_meta.inc");

  // the setting: off by default (`auto` whose cells are all off), the row's mcla_gpu_ssr a player setting, the other eight debug-only, in the three generated files
  // (the declaration's text is built here, not written out: tools\check_settings_exposure.py reads every REX_SETTING( in the sources, this file's too)
  const std::string rex_setting = std::string("REX_") + "SETTING(";
  Check(Has(settings, rex_setting + "std::string, mcla_gpu_ssr, \"auto\"") && Has(settings, "rex::Allowed(\"auto\", \"off\", \"high\", \"ultra\"));"), "GB-J: mcla_gpu_ssr is auto by default (the class cell, off) and allows auto, off, high, ultra");
  bool every = true;
  for (const char* name : {"mcla_gpu_ssr", "mcla_gpu_ssr_view", "mcla_gpu_ssr_strength", "mcla_gpu_ssr_min_smooth", "mcla_gpu_ssr_stub", "mcla_gpu_ssr_wet", "mcla_gpu_ssr_history", "mcla_gpu_ssr_fallback", "mcla_gpu_ssr_vehicle_rows"}) {
    const std::string n = name;
    const size_t decl = settings.find(", " + n + ", ");
    const size_t end = decl == std::string::npos ? std::string::npos : settings.find(");\n", decl);
    const bool tagged = decl != std::string::npos && end != std::string::npos && settings.substr(decl, end - decl).find("rex::kDebugOnly") != std::string::npos;
    const bool debug = (n == "mcla_gpu_ssr") ? !tagged : tagged;   // the row's setting is a player setting, every other one is debug-only
    const bool in_alias = Has(settings, "inline auto& " + n + " = g_gpu." + n + ";");
    const bool in_manifest = Has(manifest, "\"" + n + "\": {");
    const bool in_meta = Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},");
    if (!(debug && in_alias && in_manifest && in_meta)) std::printf("setting %s: debug %d alias %d manifest %d meta %d\n", name, debug, in_alias, in_manifest, in_meta);
    every = every && debug && in_alias && in_manifest && in_meta;
  }
  Check(every, "all nine settings are in the settings header, its aliases, the manifest and the meta list; the eight beside the row's are debug-only, the row's is not");
  const size_t view_decl = settings.find(", mcla_gpu_ssr_view, ");
  const size_t view_end = view_decl == std::string::npos ? std::string::npos : settings.find(");\n", view_decl);
  Check(Has(settings, rex_setting + "f64, mcla_gpu_ssr_fallback, 1.0,") && Has(settings, rex_setting + "i32, mcla_gpu_ssr_vehicle_rows, 1,") && view_end != std::string::npos && settings.substr(view_decl, view_end - view_decl).find("rex::Range{0, 6}") != std::string::npos,
        "GB-I2: the fallback is on by default (1) and the vehicle rows too, the view setting goes to 6");

  // the blue noise: the setting, the frame word's flag, the trace's call, the debug row
  {
    const std::string debug_pages_blue = Read(dir, "../mc_engine/menus/debug_menu_pages.cpp");
    const std::string decl = std::string("REX_") + "SETTING(bool, mcla_gpu_ssr_blue_noise, true,";
    const size_t at = settings.find(decl);
    const size_t end = at == std::string::npos ? std::string::npos : settings.find(");\n", at);
    Check(at != std::string::npos && end != std::string::npos && settings.substr(at, end - at).find("rex::kDebugOnly") != std::string::npos && Has(settings, "inline auto& mcla_gpu_ssr_blue_noise = g_gpu.mcla_gpu_ssr_blue_noise;") &&
              Has(manifest, "\"mcla_gpu_ssr_blue_noise\": {") && Has(meta, "{\"mcla_gpu_ssr_blue_noise\", \"MCLA/GPU\"},"),
          "mcla_gpu_ssr_blue_noise: a live debug setting, on by default, in the settings header, its alias, the manifest and the meta list");
    Check(Has(cpp, "push.Set(pk::kFrame, (uint32_t(frames_) & ~pk::kFrameLegacyNoise) | (larecomp::settings::mcla_gpu_ssr_blue_noise.Get() ? 0u : pk::kFrameLegacyNoise));"),
          "ssr.cpp: the frame word carries the first version's flag when mcla_gpu_ssr_blue_noise is off");
    Check(Has(common, "SsrNoise1(px, py, frame, ray, rays), SsrNoise2(px, py, frame, ray, rays)") && Has(trace_ps, "SsrSampleRay(n, to_camera, alpha, pix.x, pix.y, frame, ray, rays)"), "the trace pass gives the noise the ray's number and the rays a pixel (through SsrSampleRay)");
    Check(Has(common, "#include \"blue_noise.hlsli\"") && Has(common, "static const uint kSsrR2StepA = 3242174889u;") && Has(common, "static const uint kSsrR2StepB = 2447445413u;") &&
              uint32_t(0.7548776662466927 * 4294967296.0) == 3242174889u && uint32_t(0.5698402909980532 * 4294967296.0) == 2447445413u,
          "ssr_common.hlsli: the frame's steps are the R2 sequence's, as 32 bit fractions");
    Check(Has(debug_pages_blue, "\"mcla_gpu_ssr_blue_noise\""), "the debug menu's FIXES page has the A/B row");
  }

  // GB-J: the row, the class cell, the debug row of the light pass, the hidden G-buffer setting
  {
    const std::string gpu_class_h = Read(dir, "gpu_class.h"), gpu_class_test = Read(dir, "gpu_class_test.cpp"), pause = Read(dir, "../mc_engine/menus/pause_menu.cpp"),
                      debug_pages = Read(dir, "../mc_engine/menus/debug_menu_pages.cpp");
    Check(Has(gpu_class_h, "kSsr,                   // mcla_gpu_ssr") && Has(gpu_class_h, "inline constexpr int kEffectCount = 17;") && Has(gpu_class_h, "{\"mcla_gpu_ssr\", \"off\", \"off\", \"off\"},"),
          "GB-J: the class table has the effect, with a cell of off on every class until the owner has seen the pictures");
    Check(Has(gpu_class_test, "{Effect::kSsr, \"mcla_gpu_ssr\", \"off\", \"off\", \"off\", \"AUTO (OFF)\", \"AUTO (OFF)\", \"AUTO (OFF)\"},"), "GB-J: the class test pins the cells");
    Check(Has(cpp, "static LiveText text(\"mcla_gpu_ssr\");") && Has(cpp, "ssr::LevelFromText(gpu_class::Resolve(gpu_class::Effect::kSsr, Setting().Get()))") && !Has(cpp, "mcla_gpu_ssr.Get()"),
          "GB-J: the level is the setting through LiveText with auto resolved by the class (the cvar's own string is never read by the render thread)");
    // an `auto` that was not resolved would be off, never a level
    Check(sr::LevelFromText("auto") == sr::Level::kOff && sr::LevelFromText("HIGH") == sr::Level::kHigh && sr::LevelFromText("Ultra") == sr::Level::kUltra && sr::LevelFromText("") == sr::Level::kOff,
          "GB-J: a word that is no level (auto before it is resolved, a hand-edited file) is off");
    const size_t items = pause.find("const ItemDef kReflectionItems[] = {");
    const size_t items_end = items == std::string::npos ? std::string::npos : pause.find("\n};", items);
    const std::string reflection_page = items == std::string::npos || items_end == std::string::npos ? "" : pause.substr(items, items_end - items);
    Check(Has(reflection_page, "Str(\"PM_RxGpuSsr\", \"mcla_gpu_ssr\", \"REFLECTIONS ON SURFACES: \",") && Has(reflection_page, "kNativeSsrVals, kNativeSsrNames, 4") && Has(pause, "const char* const kNativeSsrVals[]  = {\"auto\", \"off\", \"high\", \"ultra\"};") &&
              Has(Read(dir, "../settings/ui_rules.toml"), "[rows.PM_RxGpuSsr.value.\"auto\"]\nauto_label = true"),
          "GB-J: the row REFLECTIONS ON SURFACES is on the GRAPHICS > REFLECTIONS page: AUTO (what the class says), OFF, HIGH, ULTRA (nothing called low)");
    Check(!Has(pause, "\"mcla_gpu_local_lights\"") && !Has(pause, "LOCAL LIGHTS") && Has(debug_pages, "Bool(\"PM_RxDbgLocalLights\", \"mcla_gpu_local_lights\", \"LOCAL LIGHT PASS: \")") &&
              Has(debug_pages, "\"mcla_gpu_local_lights_sources\", \"LIGHT PASS SOURCES: \""),
          "GB-J: the light pass has NO row for players (no LOCAL LIGHTS row, owner Q3) and a switch and a sources row on the hidden DEBUG menu's RENDERING page");
    const size_t gb_decl = settings.find(", mcla_gpu_gbuffer, ");
    const size_t gb_end = gb_decl == std::string::npos ? std::string::npos : settings.find(");\n", gb_decl);
    const std::string gb_text = gb_decl == std::string::npos || gb_end == std::string::npos ? "" : settings.substr(gb_decl, gb_end - gb_decl);
    Check(Has(gb_text, "\"auto\"") && Has(gb_text, "rex::kDebugOnly") && !Has(pause, "\"mcla_gpu_gbuffer\"") && !Has(debug_pages, "\"mcla_gpu_gbuffer\""),
          "GB-J: mcla_gpu_gbuffer stays `auto` and hidden (debug-only: no pause menu row, no debug menu row, no launcher row): the planes exist only while a consumer wants them");
  }

  // what the effect asks of the owners, and that it asks for nothing while it is off
  const std::string begin = FunctionText(cpp, "void ScreenReflections::BeginFrame()");
  Check(Has(begin, "screen_->Want(screen::Consumer::kReflections, ssr::WantedProducts(level_))") && Has(begin, "screen_->WantCamera(screen::Consumer::kReflections, ssr::WantsCamera(level_))") &&
            Has(begin, "gbuffer_->Want(gbuffer::Consumer::kReflections, ssr::WantedPlanes(level_))") && !Has(begin, "on ? ") && !Has(begin, "const bool on"),
        "BeginFrame asks for the Hi-Z, colour_post, the camera and the G-buffer's planes through the rules (ssr_rules.h WantedProducts / WantsCamera / WantedPlanes: only while on, 0 withdraws) and by the RESOLVED level (GB-I3)");
  Check(Has(rules, "constexpr uint32_t WantedPlanes(Level level) { return level == Level::kOff ? 0u : gbuffer::ConsumerPlanes(gbuffer::Consumer::kReflections); }") &&
            Has(rules, "inline constexpr uint32_t kWantedProducts = screen::ProductBit(screen::kHizMin) | screen::ProductBit(screen::kColourPost);"),
        "the rules' wants are the planes of the auto policy's table and the Hi-Z with colour_post");
  Check(Has(scene, "ssr_->BeginFrame();") && scene.find("ssr_->BeginFrame()") < scene.find("gbuffer_->BeginFrame(frame_serial_)") && scene.find("ssr_->BeginFrame()") < scene.find("screen_->BeginFrame(frame_serial_)") &&
            scene.find("local_lights_->BeginFrame()") < scene.find("ssr_->BeginFrame()"),
        "GB-I3: Scene::BeginFrame declares the reflections' wants right after the light pass's and before the G-buffer and the screen owner read them (lag 0)");
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
  const size_t ssr_at = scene.find("ssr_->OnTrigger(summary, list, raw_ssr, UpscalerRunning(), regen_ask);");
  const size_t car_at = scene.find("car_refl_->OnResolve(record, summary, list);");
  Check(capture_at != std::string::npos && ssr_at != std::string::npos && car_at != std::string::npos && capture_at < ssr_at && ssr_at < car_at, "scene.cpp: the colour capture, then the reflections, then the car self-reflection's history");
  Check(Has(scene, "if (colour_captured && ssr_) {") && Has(scene, "if (ssr_ && ssr_->wants_draws()) {\n    ssr_->OnMainDraw(draw.match, banks);") && Has(scene, "ssr_->BeginFrame();") && Has(scene, "ssr_->OnPresent();"),
        "scene.cpp: the reflections run only in a frame that made the capture, hear the draws only while on, begin and end the frame");
  Check(Has(scene_h, "void SetScreenReflections(ScreenReflections* ssr)") && Has(scene_h, "ScreenReflections* ssr_ = nullptr;"), "scene.h holds the pointer");
  const size_t init_at = renderer.find("ssr_.Initialize(device_, heap_, passes_, edram_, pass_timing_, &frames_);");
  const size_t screen_init = renderer.find("screen_.Initialize(device_, heap_, passes_, edram_, pass_timing_);");  // SS-2 gave the owner the device and the heap (its extraction's layout)
  Check(init_at != std::string::npos && screen_init != std::string::npos && screen_init < init_at && Has(renderer, "ssr_.SetScreenShared(&screen_);") && Has(renderer, "ssr_.SetGBuffer(&gbuffer_);") &&
            Has(renderer, "ssr_.SetAmbientOcclusion(&ao_);") && Has(renderer, "scene_.SetScreenReflections(&ssr_);"),
        "renderer.cpp: made after the shared products, wired to the owners and the scene");
  size_t shutdowns = 0;
  bool before_ao = true;
  for (size_t at = renderer.find("ssr_.Shutdown();"); at != std::string::npos; at = renderer.find("ssr_.Shutdown();", at + 1)) {
    ++shutdowns;
    const size_t ao_at = renderer.find("ao_.Shutdown();", at);
    before_ao = before_ao && ao_at != std::string::npos && ao_at - at < 1000;  // the light pass's Shutdown (GB-H1) and the probe field's three (CITY-RT M3 G4, each with its comment) sit between them in the merged branches
  }
  Check(shutdowns == 2 && before_ao, "renderer.cpp: shut down before the AO it holds a pointer to, in both Shutdown paths");
  Check(Has(renderer, "ssr_.HeldBytes()") && Has(renderer_h, "ScreenReflections ssr_;") && Has(renderer_h, "#include \"ssr.h\""), "renderer.h / renderer.cpp: the memory line and the member");

  // the build: the sources, the shaders, the test, the registration of the blobs, the precache owner
  Check(Has(cmake, "    ssr.cpp\n") && Has(cmake, "add_executable(larecomp_ssr_test EXCLUDE_FROM_ALL ssr_test.cpp)"), "CMake: the source and the test");
  for (const char* stem : {"ssr_trace_ps", "ssr_trace_ms_ps", "ssr_temporal_ps", "ssr_apply_ps", "ssr_apply_ms_ps", "ssr_apply_raw_ps", "ssr_hit_ps"}) {
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
      {"SSR_W_PREV_TAN_Y", "kPrevTanY"}, {"SSR_W_HISTORY_WEIGHT", "kHistoryWeight"},
      {"SSR_W_ROAD_VIEW", "kRoadView"}, {"SSR_W_FALLBACK", "kFallback"}, {"SSR_W_VEH_TABLE", "kVehTable"}, {"SSR_W_STENCIL", "kStencil"}, {"SSR_W_ROAD_VIEW_APPLY", "kRoadViewApply"}};
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
  Check(Has(rules, "kStagePlanes") == false && Has(rules, "gbuffer::ConsumerPlanes(gbuffer::Consumer::kReflections)") && gb::ConsumerPlanes(gb::Consumer::kReflections) == gb::kAllPlanes, "all three planes are asked for, through the auto policy's table (GB-J; RT3 may not be written yet: the frame then has two)");
  // the pass timing (GB-I3): a mark before each of the three passes and a closing mark after the apply, each once
  for (const char* label : {"ssr trace", "ssr temporal", "ssr apply", "after ssr"}) {
    Check(Count(cpp, std::string("pass_timing_->Mark(list, \"") + label + "\");") == 1, std::string("ssr.cpp marks '") + label + "' once");
  }
  {
    const std::string trigger_text = FunctionText(cpp, "bool ScreenReflections::OnTrigger(");
    const size_t t = trigger_text.find("Mark(list, \"ssr trace\")"), p = trigger_text.find("Mark(list, \"ssr temporal\")"), a = trigger_text.find("Mark(list, \"ssr apply\")"), c = trigger_text.find("Mark(list, \"after ssr\")");
    Check(t != std::string::npos && t < p && p < a && a < c, "the marks come in the passes' order, the closing one last (a mark's time runs to the next mark)");
    const size_t draw = trigger_text.find("list->drawInstanced(3, 1, 0, 0);");
    Check(draw != std::string::npos && a < draw && draw < c, "the closing mark follows the apply's draw (outside the render pass the apply closed)");
  }

  // the AO's accessor is a read and nothing else
  const std::string accessor = FunctionText(ao_cpp, "bool AmbientOcclusion::FrameCameraNow(FrameCameraOut* out) const {");
  Check(!accessor.empty() && Has(ao_h, "bool FrameCameraNow(FrameCameraOut* out) const;") && Has(accessor, "FrameCameraRows(&rows)") && Has(accessor, "FrameMotion(&motion, &pose, &cut)") && Has(accessor, "((k == 2) != (r == 2)) ? -1.0 : 1.0"),
        "ao.h / ao.cpp: the camera accessor is const, uses the AO's own camera and motion, and turns the motion into this renderer's view space as FillPush does");
  // GB-I2: the AO's vehicle table accessor is a read and nothing else, and the effect asks for it only for its vehicles
  const std::string table_accessor = FunctionText(ao_cpp, "bool AmbientOcclusion::VehicleTableNow(VehicleTableOut* out) const {");
  Check(!table_accessor.empty() && Has(ao_h, "bool VehicleTableNow(VehicleTableOut* out) const;") && Has(table_accessor, "vehicle_table_frame_ != frame_") && !Has(table_accessor, "list->") && Has(ao_cpp, "  vehicle_table_frame_ = frame_;\n  vehicle_table_ids_ = mask;"),
        "ao.h / ao.cpp: the vehicle table accessor is const, answers only for a table drawn in this frame and records nothing");
  Check(Has(trigger, "vehicle_rows_ && ao_") && Has(trigger, "ao_->VehicleTableNow(&table)") &&
            Has(trigger, "temporal.Set(pk::kVehTable, ssr::PackTemporalWord(vehicle_table_slot, larecomp::settings::mcla_gpu_ssr_resolve.Get(), upscaler_running,") &&
            Has(trigger, "larecomp::settings::mcla_gpu_ssr_sparkle_resolve.Get()));") &&
            Has(trigger, "push.Set(pk::kLin0, ssr::PackSlots(sf.lin_depth_slot[0], stencil_slot));"),
        "ssr.cpp: the vehicle table goes to the temporal pass, the stencil plane to the trace");
  // GB-I2: the road view: found by its identity, classified, drawn this frame, moved to a shader read before the trace, asked for only while the share is above 0; its numbers are the side view's own
  const std::string reflection_rule = Read(dir, "side_view_reflection.cpp");
  Check(Has(reflection_rule, "constexpr uint16_t kPitch = " + std::to_string(sr::roadview::kPitch) + ";") && Has(reflection_rule, "constexpr uint8_t kSamples = " + std::to_string(sr::roadview::kGuestSamples) + ";") &&
            Has(reflection_rule, "constexpr uint16_t kColorBase = " + std::to_string(sr::roadview::kColorBase) + ";") && Has(reflection_rule, "constexpr uint32_t kDeclared = " + std::to_string(sr::roadview::kDeclared) + ";") &&
            Has(reflection_rule, "key.format == uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)") && Has(trigger, "const EdramKey road_key = RoadViewKey();") &&
            Has(FunctionText(cpp, "EdramKey RoadViewKey()"), "key.format = uint8_t(rex::xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);"),
        "ssr_rules.h roadview:: is the road reflection side view's identity (tile, pitch, samples, declared size, format)");
  const size_t road_find = trigger.find("edram_->Find(road_key)");
  const size_t road_transition = trigger.find("edram_->Transition(list, *road_target, plume::RenderTextureLayout::SHADER_READ)");
  const size_t trace_draw = trigger.find("DrawPass(list, trace_, trace_pipeline, trace_push);");
  const size_t road_share = trigger.find("if (road.share > 0.0f) {");
  Check(road_find != std::string::npos && road_share != std::string::npos && road_share < road_find && road_transition != std::string::npos && road_transition < trace_draw && trace_draw != std::string::npos,
        "ssr.cpp: the road view is looked for only while the share is above 0, and is a shader read before the trace");
  Check(Has(trigger, "road_found->side_view == SideView::kReflection") && Has(trigger, "uint32_t(road_found->drawn_frame) == sf.frame_serial") && Has(trigger, "road_found->scale.Edge(ssr::roadview::kDeclared)") &&
            Has(trigger, "ssr::DecideRoadView(road)") && Has(trigger, "road_word = ssr::PackRoadView(road_slot, road.host_samples, road.side);") && Has(trigger, "trace_push.Set(pk::kRoadView, road_word);") &&
            Has(trigger, "apply.Set(pk::kRoadViewApply, road_word);"),
        "ssr.cpp: classified by the side view rule, drawn this frame, its host side from the scale, decided by the rules and handed to the trace and the debug view");
  Check(Has(trigger, "ssr::FallbackShare(fallback_setting_, wet_strength_)") && Has(trigger, "ssr::SmoothWetDt(wet_, target, dt)") && Has(trigger, "std::chrono::steady_clock::now()") && !Has(cpp, "SmoothWet("),
        "ssr.cpp: the wetness is smoothed in seconds and the share is the setting times the game's strength");
  Check(Has(FunctionText(cpp, "void ScreenReflections::OnMainDraw("), "wet_votes_.Add(BankFloat(banks.vs.data(), kVsReflectionParams, 0), BankFloat(banks.vs.data(), kVsReflectionParams, 1))"), "the votes carry VS c43.x and c43.y");
  // the aliased words: a pass never reads a word for its first meaning that another use of it has taken
  Check(!Has(trace_ps, "SSR_W_SRC") && !Has(trace_ps, "SSR_W_STRENGTH") && Has(trace_ps, "SsrF(SSR_W_FALLBACK)") && Has(trace_ps, "SsrW(SSR_W_ROAD_VIEW)"), "the trace reads the road view and the share where the other passes read the source images and the strength");
  Check(!Has(temporal_ps, "SSR_W_RT2_RT3") && !Has(temporal_ps, "SsrRt3At(") && !Has(temporal_ps, "SsrRt1At(") && Has(temporal_ps, "SsrW(SSR_W_VEH_TABLE)"), "the temporal pass reads the vehicle table where the others read the planes' word");
  Check(!Has(PlainOf(apply_ps), "SsrHizAt(") && !Has(apply_ps, "SSR_W_HIZ") && Has(apply_ps, "SsrW(SSR_W_ROAD_VIEW_APPLY)"), "the apply reads no Hi-Z (outside the RR-4a raw compilation, which a debug view never is), so its first word carries the road view for the debug view");
  Check(Has(pass, "SsrHi(SsrW(SSR_W_STENCIL))") && Has(pass, "SsrLo(SsrW(SSR_W_LIN0))") && Has(trace_ps, "SsrVehicleIdAt(hx, hy)") && Has(trace_ps, "SsrPackHit(distance_out, vehicle_hit, vehicle_id)") && Has(temporal_ps, "SsrVehicleImage(m0, m1, m2, v0, v1, v2,"),
        "the stencil plane rides in the high half of the lin depth word; the trace packs the vehicle, the temporal pass moves it");
  Check(Has(trace_ps, "radiance - fallback * conf") && Has(trace_ps, "fallback = min(NgFinite3(SsrRoadViewAt(road_view, screen_uv)), float3(cap, cap, cap)) * share;") && Has(apply_ps, "term = SsrGuardTerm(term,") && Has(apply_ps, "SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH))"),
        "the trace brings the difference to the road view, the apply guards the term and ramps it by the smoothness");
  Check(Has(temporal_ps, "const float3 clipped = SsrClipToBox(history, box_lo, box_hi);") && Has(temporal_ps, "history = SsrKeepClip(history, clipped, keep);") && Has(temporal_ps, "SsrHistoryInside(image.u, image.v") && !Has(temporal_ps, "SsrClampToBox"), "the temporal pass clips its history and does not read at the edge");
  const std::string validated_history = FunctionText(temporal_ps, "if (stored > 0.0f && SsrSameSurface(stored, plain.z)) {");
  Check(Has(temporal_ps, "float keep = SsrClipKeepFor(resolve, upscaler);") &&
            Has(validated_history, "(temporal_options & SSR_TEMPORAL_SPARKLE_RESOLVE) != 0u && vehicle_scale != kSsrVehicleNoRowWeight") &&
            Has(validated_history, "float2(image.u, image.v) * float2(ssr) - (float2(pix) + 0.5f)") &&
            Has(validated_history, "keep = SsrSparkleClipKeep(motion.x, motion.y);"),
        "the sparkle floor is opt-in, requires valid history and excludes vehicle hits without motion rows");
  const std::string sparkle_setting = std::string("REX_") + "SETTING(bool, mcla_gpu_ssr_sparkle_resolve, false,";
  const std::string debug_pages = Read(dir, "../mc_engine/menus/debug_menu_pages.cpp");
  const size_t sparkle_at = settings.find(sparkle_setting);
  Check(sparkle_at != std::string::npos && Has(settings.substr(sparkle_at, settings.find('\n', sparkle_at) - sparkle_at), "rex::kDebugOnly") &&
            Has(debug_pages, "\"mcla_gpu_ssr_sparkle_resolve\"") && Has(manifest, "\"mcla_gpu_ssr_sparkle_resolve\": {") &&
            Has(meta, "{\"mcla_gpu_ssr_sparkle_resolve\", \"MCLA/GPU\"}"),
        "the sparkle resolve defaults off and is available as a debug setting");

  TestRawSources(dir, rules, header, cpp, scene, cmake, shaders_h, pass, apply_ps, settings, manifest, meta);

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
  TestWantsAndArms();
  TestSizes();
  TestDecision();
  TestHistory();
  TestWet();
  TestPush();
  TestCamera();
  TestGate();
  TestBrdf();
  TestNoise();
  TestNoiseResolve();
  TestGgx();
  TestFades();
  TestVirtualImage();
  TestWetI2();
  TestHitPacking();
  TestRaw();
  TestVehicleImage();
  TestClip();
  TestResolve();
  TestRoadViewRules();
  TestMarchAnalytic();
  TestMarchBrute();
  TestMarchEdges();
  TestLensFunctions();
  TestSources(src_dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  (void)Count;
  return g_failed == 0 ? 0 : 1;
}
