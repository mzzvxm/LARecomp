// CPU checks for the atmosphere blend, shaft kernel, and POST-0 grid.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "shafts_blur.h"
#include "shafts_image.h"
#include "volumetrics_types.h"

namespace {

namespace blur = mcla::gpu::shafts_blur;
namespace image = mcla::gpu::shafts_image;
namespace vol = mcla::gpu::volumetrics;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition && failures++ < 12) std::fprintf(stderr, "FAIL %s\n", message);
}

// IEEE half spacing for positive finite scene values.
float HalfSpacing(float x) {
  return std::ldexp(1.0f, std::max(-14, std::ilogb(x)) - 10);
}

float RoundToHalf(float x) {
  const float unit = HalfSpacing(x);
  return std::nearbyint(x / unit) * unit;
}

void CheckBlend() {
  std::mt19937 rng(2131);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  for (unsigned i = 0; i < 10000; ++i) {
    const float dst = RoundToHalf(8.0f * unit(rng));
    const float scatter = unit(rng);
    const float light = unit(rng);
    const float transmittance = unit(rng);
    const float separate = RoundToHalf(RoundToHalf(dst * transmittance + scatter) + light);
    const float combined = RoundToHalf(dst * transmittance + (scatter + light));
    Check(std::abs(combined - separate) <= HalfSpacing(std::max(combined, separate)),
          "merged blend differs from sequential half stores by at most one half ULP");
    Check(RoundToHalf(dst * transmittance + (scatter + 0.0f)) ==
              RoundToHalf(dst * transmittance + scatter), "volume-only blend");
    Check(RoundToHalf(dst * 1.0f + (0.0f + light)) == RoundToHalf(dst + light), "shafts-only blend");
  }
}

struct Point {
  float x;
  float y;
  Point operator+(Point other) const { return {x + other.x, y + other.y}; }
  Point operator-(Point other) const { return {x - other.x, y - other.y}; }
  Point operator*(float scale) const { return {x * scale, y * scale}; }
};

float ScreenReach(Point uv, Point ray) {
  const auto reach = [](float from, float move) {
    if (move >= 0.0f) return (1.0f - from) / std::max(move, 1e-6f);
    return from / std::max(-move, 1e-6f);
  };
  return std::min({1.0f, reach(uv.x, ray.x), reach(uv.y, ray.y)});
}

float Mask(unsigned scene, Point p) {
  if (scene == 0) return p.x >= 0.35f && p.x <= 0.7f && p.y > 0.13f && p.y < 0.57f;  // window
  if (scene == 1) return p.y < 0.45f + 0.08f * std::sin(37.0f * p.x);  // tree line
  return p.y < 0.6f && (p.x < 0.48f || p.x > 0.51f);  // lamp post
}

float WeightedMask(unsigned scene, Point p, Point sun) {
  const Point delta = p - sun;
  return Mask(scene, p) / (1.0f + ((delta.x * 16.0f / 9.0f) * (delta.x * 16.0f / 9.0f) +
                                      delta.y * delta.y) / (1.5f * 1.5f));
}

constexpr unsigned kWidth = 96;
constexpr unsigned kHeight = 54;
using Image = std::array<float, kWidth * kHeight>;

Point Centre(unsigned x, unsigned y) {
  return {(float(x) + 0.5f) / kWidth, (float(y) + 0.5f) / kHeight};
}

float Sample(const Image& input, Point uv) {
  const float x = std::clamp(uv.x * kWidth - 0.5f, 0.0f, float(kWidth - 1));
  const float y = std::clamp(uv.y * kHeight - 0.5f, 0.0f, float(kHeight - 1));
  const unsigned x0 = unsigned(x);
  const unsigned y0 = unsigned(y);
  const unsigned x1 = std::min(x0 + 1, kWidth - 1);
  const unsigned y1 = std::min(y0 + 1, kHeight - 1);
  return std::lerp(std::lerp(input[y0 * kWidth + x0], input[y0 * kWidth + x1], x - x0),
                   std::lerp(input[y1 * kWidth + x0], input[y1 * kWidth + x1], x - x0), y - y0);
}

float March(unsigned scene, Point uv, Point sun, float density, unsigned taps) {
  const Point ray = sun - uv;
  const Point step = ray * (std::min(density, ScreenReach(uv, ray)) / float(taps));
  const float decay = std::pow(0.96f, 64.0f / float(taps));
  // The coarse alternative samples segment centres. Give it the same open-sky
  // gain as the 64-step reference so the test compares kernel shape.
  const float start = taps == 64 ? 1.5f : 0.5f;
  float sum = 0.0f;
  float weight = 1.0f;
  for (unsigned tap = 0; tap < taps; ++tap) {
    sum += weight * WeightedMask(scene, uv + step * (float(tap) + start), sun);
    weight *= decay;
  }
  return sum * ((1.0f - decay) / (64.0f * 0.04f));
}

void CompareKernels(unsigned scene, Point sun, float density) {
  const float ratio = blur::SegmentRatio(density);
  const float extent = 1.0f - std::pow(ratio, float(blur::kTaps));
  Image segments{};
  Image short_march{};
  std::array<float, blur::kTaps> weights{};
  float remaining = 1.0f;
  float mass = 0.0f;
  for (float& weight : weights) {
    const float next = remaining * ratio;
    weight = blur::SegmentWeight(1.0f - remaining, 1.0f - next, extent);
    Check(weight > 0.0f, "segment weights are positive");
    mass += weight;
    remaining = next;
  }
  Check(std::abs(mass - (1.0f - std::pow(0.96f, 64.0f)) / (64.0f * 0.04f)) < 1e-6f,
        "compound kernel keeps the legacy open-sky energy");
  Check(std::abs(extent - std::min(density, 63.0f / 64.0f)) < 1e-6f, "the segments reach the requested extent");
  for (unsigned y = 0; y < kHeight; ++y) {
    for (unsigned x = 0; x < kWidth; ++x) {
      const Point uv = Centre(x, y);
      const Point to_sun = sun - uv;
      const Point step = to_sun * (ScreenReach(uv, to_sun) * (1.0f - ratio) / blur::kTaps);
      float sum = 0.0f;
      for (unsigned tap = 0; tap < blur::kTaps; ++tap) {
        sum += WeightedMask(scene, uv + step * (float(tap) + 0.5f), sun);
      }
      segments[y * kWidth + x] = sum / blur::kTaps;
      short_march[y * kWidth + x] = March(scene, uv, sun, density, 16);
    }
  }
  Image compound{};
  for (unsigned y = 0; y < kHeight; ++y) {
    for (unsigned x = 0; x < kWidth; ++x) {
      const Point uv = Centre(x, y);
      const Point to_sun = sun - uv;
      const Point ray = to_sun * ScreenReach(uv, to_sun);
      float combined = 0.0f;
      float left = 1.0f;
      for (const float weight : weights) {
        combined += weight * Sample(segments, uv + ray * (1.0f - left));
        left *= ratio;
      }
      compound[y * kWidth + x] = combined;
    }
  }
  float peak = 0.0f;
  float error = 0.0f;
  float short_error = 0.0f;
  // Judge both half-size candidates after the bilinear upscale onto the full-size grid.
  for (unsigned y = 0; y < 2 * kHeight; ++y) {
    for (unsigned x = 0; x < 2 * kWidth; ++x) {
      const Point uv = Centre(x, y) * 0.5f;
      const float reference = March(scene, uv, sun, density, 64);
      peak = std::max(peak, reference);
      error += std::abs(Sample(compound, uv) - reference);
      short_error += std::abs(Sample(short_march, uv) - reference);
    }
  }
  const float denominator = float(4 * kWidth * kHeight) * peak;
  const float normalized = error / denominator;
  std::printf("mask %u, sun %.2f/%.2f, density %.2f, mean error / peak: compound %.4f, 16-tap %.4f\n",
              scene, sun.x, sun.y, density, normalized, short_error / denominator);
  // Synthetic regression ceiling. The live FLIP and temporal judges decide visual acceptance.
  Check(normalized < 0.035f, "compound mask error stays below 3.5% of reference peak");
}

void CheckGrids() {
  const auto baseline = vol::ProofGrid(vol::Level::kUltra, false, 0.5f);
  const auto medium = vol::ProofGrid(vol::Level::kMedium, true, 2.0f);
  Check(baseline.valid() && baseline.temporal && baseline.nz == 64 && baseline.near_m == 0.5f, "Ultra reference");
  Check(medium.valid() && medium.temporal && medium.nz == 24 && medium.near_m == 2.0f &&
            medium.voxels() == 124416u, "Medium + history + 2 m arm");
  Check(!vol::ProofGrid(vol::Level::kMedium, false, 0.5f).temporal, "shipped Medium keeps its default history policy");
  Check(vol::ProofGrid(vol::Level::kHigh, false, 2.0f).nz == 40u, "High + 2 m proof arm");
  Check(vol::ProofGrid(vol::Level::kHigh, false, 1000.0f).near_m == 2.0f, "proof near plane stays in range");
  Check(!vol::ProofGrid(vol::Level::kOff, true, 2.0f).valid(), "proof controls cannot turn a volume on");
  Check(image::IteratedSize(1920, 1080, true) == image::Size{480, 270}, "1080p shafts use 480x270");
  Check(image::IteratedSize(3840, 2160, true) == image::Size{480, 270}, "4K shafts use the same cap");
  Check(image::IteratedSize(3840, 2160, false) == image::Size{960, 540}, "uncapped 4K shafts use quarter resolution");
}

std::string Read(const std::string& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void CheckSources(const std::string& gpu) {
  const std::string apply = Read(gpu + "/hlsl/volumetrics_apply_ps.hlsl");
  const std::string segments = Read(gpu + "/hlsl/shafts_blur_ps.hlsl");
  const std::string scene = Read(gpu + "/scene.cpp");
  const std::string blend = Read(gpu + "/volumetrics.cpp");
  const std::string shafts = Read(gpu + "/shafts.cpp");
  const std::string add = Read(gpu + "/hlsl/shafts_add_ps.hlsl");
  const std::string timing = Read(gpu + "/pass_timing.cpp");
  Check(apply.find("result.rgb += SampleShaftLight(uv, g_push.shafts_slot, g_push.shafts_tint);") != std::string::npos, "merged shader adds the shafts after computing transmittance");
  Check(apply.find("return float4(0.0, 0.0, 0.0, 1.0)") != std::string::npos, "near plane returns identity");
  Check(segments.find("static const uint kTaps = 8u;") != std::string::npos && blur::kTaps == 8, "eight taps per stage on both APIs");
  Check(segments.find("remaining *= ratio;") != std::string::npos &&
            segments.find("float(i) + noise") != std::string::npos, "compound advances on the ray with frame-varying segment noise");
  Check(scene.find("shafts_->Composite(list, *shine, merged)") != std::string::npos, "successful merge suppresses the extra add");
  Check(blend.find("blend.dstBlend = plume::RenderBlend::SRC_ALPHA;") != std::string::npos, "volume alpha retains background transmittance");
  Check(blend.find("push.slots[3] = vol::ApplyWord(plan.depth_sample, stable_, 1);") != std::string::npos,
        "volume capture reuses the ordinary depth sample policy at unit gain");
  Check(shafts.find("hist_legacy_steps_ == plan.legacy_steps") != std::string::npos,
        "a change of blur kernel invalidates the history even at the same image size");
  Check(add.find("view == 0u || view == 4u") != std::string::npos && add.find("view == 0u && !any(light > 0.0f)") != std::string::npos,
        "shaft capture uses the apply threshold and writes every pixel");
  Check(timing.find("name.starts_with(\"vol \") || name.starts_with(\"shafts \") || name == \"atmosphere apply\"") != std::string::npos,
        "all atmosphere pass timings remain visible below the top labels");
}

}  // namespace

int main(int argc, char** argv) {
  CheckBlend();
  CheckGrids();
  for (unsigned scene = 0; scene < 3; ++scene) {
    CompareKernels(scene, {0.55f, 0.21f}, 0.9f);
    CompareKernels(scene, {1.10f, 0.16f}, 0.9f);
  }
  if (argc > 1) CheckSources(argv[1]);
  std::printf("atmosphere: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
