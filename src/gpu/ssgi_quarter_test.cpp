#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "ao_size.h"
#include "ssgi_level.h"
#include "ssgi_stable.h"

namespace shader {
using uint = uint32_t;
using std::abs;
using std::max;
using std::min;
struct int2 {
  int x, y;
};
int2 operator*(int2 p, int n) { return {p.x * n, p.y * n}; }
int2 operator+(int2 p, int n) { return {p.x + n, p.y + n}; }
int2 operator+(int2 p, int2 q) { return {p.x + q.x, p.y + q.y}; }
int2 min(int2 p, int2 q) { return {min(p.x, q.x), min(p.y, q.y)}; }
uint asuint(float x) { return std::bit_cast<uint>(x); }
float asfloat(uint x) { return std::bit_cast<float>(x); }
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
#include "hlsl/ssgi_quarter.hlsli"
}  // namespace shader

namespace {
using namespace mcla::gpu;
int checks = 0;
int failures = 0;

void Check(bool ok, const char* name) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", name);
  }
}

void CheckSizes() {
  for (uint32_t n = 1; n <= 4097; ++n) {
    Check(ao_size::Quarter(n) == (n + 3) / 4, "quarter dimensions round up");
  }
  Check(ao_size::Quarter(1920) == 480 && ao_size::Quarter(1080) == 270, "1080p trace dimensions");
  Check(ao_size::Quarter(3840) == 960 && ao_size::Quarter(2160) == 540, "4K trace dimensions");
  for (uint32_t h : {720u, 1080u, 2160u}) {
    for (bool high : {false, true}) {
      for (bool contact : {false, true}) {
        const ao_size::Inputs ao{.render_height = h, .occlusion = true, .occlusion_ultra = !high,
                                 .contact = contact, .contact_ultra = contact};
        ao_size::Inputs gi = ao;
        gi.bounce = true;
        gi.bounce_ultra = true;
        gi.bounce_half = true;
        gi.half_above_bounce = false;
        gi.bounce_quarter = true;
        const auto a = ao_size::Decide(ao);
        const auto b = ao_size::Decide(gi);
        Check(a.low == b.low && a.by_height == b.by_height && a.contact_full == b.contact_full,
              "independent GI preserves AO and contact dimensions");
      }
    }
  }
  const ao_size::Frame frame{.render = {1920, 1080}, .bounce = true, .bounce_quarter = true};
  const auto bytes = ao_size::Bytes(frame);
  Check(bytes.history == 2ull * 1920 * 1080 * 4, "AO retains its own RG16F history");
  Check(bytes.bounce == 480ull * 270 * 48 + 240ull * 135 * 10 + 120ull * 68 * 10,
        "GI image bytes include both depth guides and both mips");
}

void CheckHistoryAndNoise() {
  const uint32_t render[2] = {1920, 1080};
  const uint32_t different_width[2] = {1919, 1080};
  const uint32_t different_height[2] = {1920, 1079};
  Check(ssgi_stable::QuarterHistoryMatches(true, 8, 9, render, render), "adjacent matching history");
  Check(!ssgi_stable::QuarterHistoryMatches(false, 8, 9, render, render), "camera cut invalidates history");
  Check(!ssgi_stable::QuarterHistoryMatches(true, 8, 10, render, render), "gap invalidates history");
  Check(!ssgi_stable::QuarterHistoryMatches(true, UINT64_MAX, 0, render, render), "wrapping frame is a discontinuity");
  Check(!ssgi_stable::QuarterHistoryMatches(true, 8, 9, render, different_width), "resize to same rounded width invalidates history");
  Check(!ssgi_stable::QuarterHistoryMatches(true, 8, 9, render, different_height), "resize to same rounded height invalidates history");
  Check(shader::SsgiQuarterLength(16, 0, 16) == 1, "disocclusion starts at one sample");
  Check(shader::SsgiQuarterLength(4, 0.5f, 16) == 3, "partial confidence reduces sample count");
  Check(shader::SsgiQuarterLength(32, 1, 16) == 16, "mature history caps at the selected span");
  Check(shader::SsgiQuarterLength(1, 1, 16) == 2, "one new sample is added each frame");
  Check(shader::SsgiQuarterStep(3.9f) == 2 && shader::SsgiQuarterStep(4) == 1, "new pixels filter farther than mature pixels");
  std::array<bool, 1024> phases{};
  for (uint64_t frame = 0; frame < 128; ++frame) {
    const auto noise = ao_stable::NoiseCode(frame, false, true);
    const uint32_t phase = noise & 1023u;
    Check((noise & ao_stable::kBlueNoiseFlag) != 0 && !phases[phase],
          "blue noise advances with no repeat within the history window");
    phases[phase] = true;
  }
}

void CheckGuides() {
  Check(shader::kSsgiQuarterFlag == ssgi_level::kQuarterFlag, "shader flag matches host");
  Check(shader::kSsgiQuarterApply == ssgi_level::kQuarterApplyFlag, "apply flag matches host");
  Check(ssgi_level::kQuarterHighSlices * ssgi_level::kQuarterHighSteps * 2 == 16, "High trace takes 16 near taps");
  for (uint32_t source = 0; source < 4; ++source) {
    const auto centre = shader::SsgiQuarterSource({2, 3}, source, {100, 100});
    Check(centre.x == 9 + int(source & 1u) && centre.y == 13 + int(source >> 1u), "four centre positions");
    const auto edge = shader::SsgiQuarterSource({2, 3}, source, {8, 12});
    Check(edge.x == 8 && edge.y == 12, "odd edge clamps the picked source");
  }
  for (uint32_t id = 0; id <= 23; ++id) {
    const float depth = shader::asfloat(shader::asuint(20.0f) | 15u);
    const float marked = shader::SsgiQuarterMarkVehicle(depth, id);
    Check(shader::SsgiQuarterVehicle(marked) == id, "quarter guide preserves vehicle ID");
    Check((shader::asuint(marked) & 15u) == 15u, "quarter ID leaves the other marks intact");
    Check(std::abs(marked - depth) < 0.001f, "quarter ID leaves depth intact");
  }
  Check(shader::SsgiQuarterWeight(10, 10, 0) == 1, "same surface keeps full weight");
  Check(shader::SsgiQuarterWeight(8.0f, 10, 0) == 0, "foreground silhouette contributes no radiance");
  Check(shader::SsgiQuarterWeight(10, 10, 0.20f) == 0, "a wall across the receiver plane contributes no radiance");
  const float half_depth = shader::SsgiQuarterWeight(10.275f, 10, 0);
  const float half_plane = shader::SsgiQuarterWeight(10, 10, -0.0675f);
  Check(std::abs(half_depth - 0.25f) < 1e-4f && std::abs(half_plane - 0.25f) < 1e-4f,
        "depth and plane rejection use the quadratic falloff");
}

std::string Read(const std::filesystem::path& file) {
  std::ifstream input(file);
  return {std::istreambuf_iterator<char>(input), {}};
}

void CheckWiring(const std::filesystem::path& src) {
  const auto ao = Read(src / "ao.cpp");
  Check(ao.contains("plan.occlusion && plan.rtao.screen_pass") && ao.contains("if (plan.SharedGi())"),
        "quarter trace keeps the ordinary screen AO path");
  Check(ao.contains("ao_stable::NoiseCode(scene_noise::NoiseIndex(frame_), false, true)"),
        "quarter trace uses the frame index and blue tile");
  Check(ao.contains("DrawPass(list, gi_hist_[out], temporal, accum);") &&
        ao.contains("DrawPass(list, gi_dn_, spatial, filter);"), "one spatial pass preserves unfiltered history");
  Check(ao.contains("quarter_history_.valid = false;") && ao.contains("quarter_history_.world != trigger_world_") &&
        ao.contains("quarter_history_.sample_key != plan.gi.sample_key"), "history resets on cut, view change or MSAA changes");
  const auto apply = Read(src / "hlsl/ao_apply_ps.hlsl");
  Check(apply.contains("SsgiQuarterVehicle(td) != vehicle") && apply.contains("w <= 0.0f"),
        "upsample rejects silhouettes before fetching bounce");
  Check(apply.contains("bounce = sum / max(weights, 0.05f);") && apply.contains("const float w = bilinear * SsgiQuarterWeight"),
        "upsample fades missing coverage and weights four taps");
  const auto temporal = Read(src / "hlsl/ao_temporal_ps.hlsl");
  Check(temporal.contains("SsgiQuarterVehicle(texel_depth) != vehicle") &&
        temporal.contains("SsgiQuarterLength(history.x, confidence, g_push.lens.z)"),
        "temporal rejects vehicle mismatches and drives spatial radius with per-pixel evidence");
  Check(Read(src / "hlsl/ao_atrous_ps.hlsl").contains("SsgiQuarterStep(centre.x)"), "spatial pass uses per-pixel history length");
}
}  // namespace

int main(int argc, char** argv) {
  CheckSizes();
  CheckHistoryAndNoise();
  CheckGuides();
  CheckWiring(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path());
  std::printf("ssgi_quarter_test: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
