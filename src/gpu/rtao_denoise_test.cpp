#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hlsl_shim.h"
#include "ray_regen_guides_rules.h"
#include "rt_denoise_rules.h"
#include "rtao_denoise_rules.h"
#include "rtao_rules.h"

namespace hlsl_shim {
struct float4 { float x, y, z, w; };
using std::exp;
using std::exp2;
using std::log2;
using std::pow;
#include "hlsl/blue_noise_frame.hlsli"
#include "hlsl/rtao_noise.hlsli"
#define MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
#include "hlsl/rt_denoise_common.hlsli"
#undef MCLA_RT_DENOISE_AO_CONSUMER_HLSLI
#include "hlsl/rtao_denoise_math.hlsli"
float Term(float open, float coverage, float screen, float power, float floor, bool blend) {
  return RtAoCoverage(RtAoVisibility(open, power, floor), coverage, screen, blend);
}
}  // namespace hlsl_shim

namespace {
namespace shader = hlsl_shim;
namespace ao = mcla::gpu::rtao_denoise;
namespace rtao = mcla::gpu::rtao;
namespace dn = mcla::gpu::rt_denoise;
namespace rgd = mcla::gpu::rayregen::guides;

int checks = 0, failures = 0;
void Check(bool passed, const char* description) {
  ++checks;
  if (!passed) {
    ++failures;
    if (failures <= 20) std::printf("FAIL %s\n", description);
  }
}
bool Near(float a, float b, float tolerance = 1.0e-5f) { return std::abs(a - b) <= tolerance; }

void CheckMath() {
  Check(ao::HistoryCap(-1) == 1 && ao::HistoryCap(15) == 15 && ao::HistoryCap(100) == 64, "history cvar bounds");
  for (uint32_t slot : {1u, 255u, 256u, 32767u}) {
    const uint32_t bits = 0x800022u;
    const auto word = ao::VehicleWord(slot, bits);
    Check(word >> 8u == bits && ao::VehicleSlot(word, slot >> 8u) == slot, "all 24 ID bits and the whole bindless slot survive packing");
  }
  Check(ao::CanReadHistory(true, true, 8, 7, 16, 16), "consecutive compatible frames");
  Check(!ao::CanReadHistory(false, true, 8, 7, 16, 16), "cut, raw frame or changed view");
  Check(!ao::CanReadHistory(true, false, 8, 7, 16, 16), "no previous frame");
  Check(!ao::CanReadHistory(true, true, 9, 7, 16, 16), "gap");
  Check(!ao::CanReadHistory(true, true, 8, 7, 16, 32), "live cap change");
  Check(!ao::CanReadHistory(true, true, 0, ~uint64_t(0), 16, 16), "frame number wrap");
  Check(shader::RtAoAcceptTap(1.059f, 1, true, 0.8f), "a close, parallel tap passes");
  Check(!shader::RtAoAcceptTap(1.061f, 1, true, 1), "depth rejection at the near plane");
  Check(!shader::RtAoAcceptTap(10, 10, false, 1), "vehicle bit rejection");
  // The host rule (the one the simulated scenes and the shader's twin are held to) rejects a tap of the other vehicle bit the same way, on a flat parallel tap that would pass otherwise.
  Check(ao::AcceptTap(10, 10, true, 1) && !ao::AcceptTap(10, 10, false, 1) && !ao::AcceptTap(10, 10, false, 0.8f), "host tap rule: the vehicle bit rejection");
  Check(!shader::RtAoAcceptTap(10, 10, true, 0.79f), "normal rejection at the threshold");
  Check(Near(shader::RtAoMotionCap(0, 64), 32) && Near(shader::RtAoMotionCap(1, 32), 32 / 2.2f) &&
            Near(shader::RtAoMotionCap(9, 32), 4) && Near(shader::RtAoMotionCap(0, 1), 1), "motion and user cap");
  Check(Near(shader::RtAoHistoryLength(4, 0.249f, 32), 0) && Near(shader::RtAoHistoryLength(4, 0.25f, 32), 8) &&
            Near(shader::RtAoHistoryLength(4, 0.75f, 2), 2), "fractional footprint threshold, confidence and cap");
  Check(Near(shader::RtAoClip(0.2f, 1, 0, 1, 1), 1), "uniform open neighbourhood clips immediately");
  Check(Near(shader::RtAoClip(0, 0.5f, 0.1f, 0, 1), 0.3f), "variance clip floor");
  Check(Near(shader::RtAoClip(0, 0.5f, 0.2f, 0, 1), 0.25f), "variance clip scale");
  Check(Near(shader::RtAoClip(0, 0.5f, 0.4f, 0.45f, 0.55f), 0.44f), "variance clip extent margin");
  Check(Near(shader::RtAoBlend(0.5f, 1, 3), 0.625f) && Near(ao::Blend(0.5f, 1, 3), 0.625f), "one new sample blends with n history samples");
  Check(shader::RtAoNextLength(0, 4) == 1 && shader::RtAoNextLength(4, 4) == 4 && ao::NextLength(4, 4) == 4, "history length starts at one and stays capped");
  const auto profile = dn::Profile::kAmbient;
  Check(Near(dn::Radius(profile, 1, 1, 1, 1, 1), 5) && Near(dn::Radius(profile, 1, 1, 1, 1, 12), 2), "kernel shrinks with maturity");
  Check(dn::AmbientStep(4.0f, 4, 3.99f, false, false) && !dn::AmbientStep(2, 2, 12, false, false) &&
            dn::AmbientStep(2, 1, 12, false, false), "young pixels take three stages, mature pixels take one by default");
  Check(shader::RtDnAmbientStep(2, 2, 32, true, false), "spatial resolve filters mature world pixels at step two");
  Check(!shader::RtDnAmbientStep(2, 2, 32, false, false) && !shader::RtDnAmbientStep(2, 2, 32, true, true),
        "step two still passes through mature vehicles and the default path");
  Check(!shader::RtDnAmbientStep(5, 4, 32, true, false) && !shader::RtDnAmbientStep(1.49f, 2, 32, true, false),
        "spatial resolve adds only step two within the existing radius rule");
  Check(Near(shader::RtAoResolveEdgeWeight(0.5f, 0.5f), 1) && Near(shader::RtAoResolveEdgeWeight(0.5f, 0.375f), 0.5f) &&
            Near(shader::RtAoResolveEdgeWeight(0.375f, 0.5f), 0.5f) && Near(shader::RtAoResolveEdgeWeight(0.25f, 0.8f), 0),
        "extra taps keep small variation and stop across strong shadow edges on the same surface");
  Check(Near(dn::DepthSigma(profile), 0.03f) && Near(dn::NormalPower(profile), 16), "AO-specific plane and normal weights");
  Check(Near(shader::RtDnDepthSigma(RT_DN_PROFILE_AMBIENT), dn::DepthSigma(profile)) &&
            Near(shader::RtDnNormalPower(RT_DN_PROFILE_AMBIENT), dn::NormalPower(profile)), "AO weight shader twins");
  for (int i = 0; i < 1024; ++i) {
    const float a = float(i) / 1024.0f;
    const float b = float((i * 337) % 1024) / 1024.0f;
    const float depth = 0.01f + 80.0f * a;
    const float tap = depth * (0.94f + b * 0.12f);
    for (const bool same_vehicle : {true, false})
      Check(ao::AcceptTap(tap, depth, same_vehicle, b) == shader::RtAoAcceptTap(tap, depth, same_vehicle, b), "tap shader twin");
    Check(Near(ao::MotionCap(a * 12, 16), shader::RtAoMotionCap(a * 12, 16)), "motion cap shader twin");
    Check(Near(ao::HistoryLength(15 * a, a, 12), shader::RtAoHistoryLength(15 * a, a, 12)), "length shader twin");
    Check(Near(ao::Clip(a, 0.6f, b, 0.2f, 0.75f), shader::RtAoClip(a, 0.6f, b, 0.2f, 0.75f)), "clip shader twin");
    Check(Near(ao::ResolveEdgeWeight(a, b), shader::RtAoResolveEdgeWeight(a, b)), "resolve weight shader twin");
    Check(Near(ao::UpsampleWeight(a, tap, depth, b > 0.4f), shader::RtAoUpsampleWeight(a, tap, depth, b > 0.4f)), "upsampling shader twin");
    Check(Near(ao::Term(a, b, 0.8f, 3.6f, 0.1f, i % 2), shader::Term(a, b, 0.8f, 3.6f, 0.1f, i % 2)), "compose shader twin");
    const float length = a * 32.0f;
    const float radius = dn::Radius(profile, 1, 0, 10, 0.01f, length);
    Check(Near(radius, shader::RtDnRadius(RT_DN_PROFILE_AMBIENT, 1, 0, 10, 0.01f, length)), "radius shader twin");
    for (const uint32_t step : {1u, 2u, 4u})
      for (const bool resolve : {false, true})
        for (const bool vehicle : {false, true})
          Check(dn::AmbientStep(radius, step, length, resolve, vehicle) == shader::RtDnAmbientStep(radius, step, length, resolve, vehicle),
                "iteration shader twin");
  }
}

struct Tap { float open, depth, judged; uint32_t surface; };
float Compose(const std::array<Tap, 4>& taps, float z, uint32_t surface, float fx, float fy, float screen = 1.0f) {
  const float bilinear[] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
  float weights = 0, judged = 0, opens = 0;
  for (size_t i = 0; i < taps.size(); ++i) {
    const auto& t = taps[i];
    const float w = shader::RtAoUpsampleWeight(bilinear[i], t.depth, z, surface == t.surface);
    weights += w;
    judged += w * t.judged;
    opens += w * t.judged * t.open;
  }
  return shader::Term(judged > 1.0e-8f ? opens / judged : 1.0f, weights > 1.0e-8f ? judged / weights : 0.0f, screen, 3.6f, 0.1f, false);
}
void CheckCompose() {
  Check(Near(shader::Term(0.5f, 0, 0.8f, 2, 0.1f, false), 0.8f), "uncovered uses screen space");
  Check(Near(shader::Term(0.5f, 0.5f, 0.8f, 2, 0.1f, false), 0.525f), "coverage mixes powered visibility");
  Check(Near(shader::Term(0, 1, 1, 3.6f, 0.1f, false), 0.1f), "visibility floor");
  Check(Near(shader::Term(0.9f, 1, 0.3f, 2, 0.1f, true), 0.3f), "screen blend takes the darker covered term");
  const uint32_t world = rtao::ScreenBlendFlags(rtao::Screen::kWorld, false);
  const uint32_t all = rtao::ScreenBlendFlags(rtao::Screen::kOn, false);
  const uint32_t history = 1u << 1;
  Check(shader::RtAoBlendScreen(world | history, false) && !shader::RtAoBlendScreen(world | history, true),
        "world blending affects world receivers only");
  Check(shader::RtAoBlendScreen(all, false) && shader::RtAoBlendScreen(all, true),
        "the existing all-receiver mode still blends vehicle AO");
  Check(!shader::RtAoBlendScreen(history, false) && !shader::RtAoBlendScreen(history, true),
        "history presence alone cannot enable blending");
  const auto world_term = [&](float open, float coverage, float screen, bool vehicle) {
    return shader::Term(open, coverage, screen, 2.0f, 0.1f, shader::RtAoBlendScreen(world, vehicle));
  };
  Check(Near(world_term(1, 1, 0.65f, false), 0.65f) && Near(world_term(1, 1, 0.65f, true), 1),
        "a covered world ray miss keeps the deck's raster occlusion while the vehicle stays traced");
  Check(Near(world_term(0.5f, 1, 0.65f, false), 0.25f), "stronger ray occlusion survives");
  Check(Near(world_term(1, 0, 1, false), 1) && Near(world_term(1, 1, 1, false), 1),
        "sky and an open world surface remain open");
  Check(Near(world_term(0, 0, 0.65f, true), 0.65f) && Near(world_term(0.5f, 0.5f, 0.65f, false), 0.45f),
        "cabin fallback and partial coverage still use the screen term");
  const auto bounce = shader::RtAoWithBounce(0.6f, {0.2f, 0.1f, -0.4f, 3.5f});
  Check(bounce.x == 0.6f && bounce.y == 0.1f && bounce.z == -0.4f && bounce.w == 3.5f, "composition preserves the three bounce channels");
  const std::array<Tap, 4> pole = {{{0, 2, 1, 0}, {1, 10, 1, 0}, {1, 10, 1, 0}, {1, 10, 1, 0}}};
  Check(Near(Compose(pole, 2, 0, 0.3f, 0.4f), 0.1f) && Near(Compose(pole, 10, 0, 0.3f, 0.4f), 1, 0.02f), "thin pole keeps its width");
  auto car = pole;
  car[0] = {0, 10, 1, 3};
  Check(Near(Compose(car, 10, 0, 0.3f, 0.4f), 1) && Near(Compose(car, 10, 4, 0.3f, 0.4f), 1), "separate vehicles and world never mix");
  const std::array<Tap, 4> taps = {{{0.2f, 4.00f, 1, 0}, {1, 4.05f, 0, 0}, {0.65f, 3.94f, 1, 0}, {0.8f, 4.12f, 1, 0}}};
  for (int i = 0; i < 32; ++i) {
    const double x = double(i) / 31, y = 1 - x;
    double all = 0, covered = 0, open = 0;
    for (int j = 0; j < 4; ++j) {
      const double d = (double(taps[j].depth) - 4.0) / 0.08;
      const double weight = std::exp(-d * d) * ((j & 1) ? x : 1 - x) * ((j & 2) ? y : 1 - y);
      all += weight;
      covered += weight * taps[j].judged;
      open += weight * taps[j].judged * taps[j].open;
    }
    const double expected = covered ? 1 - (1 - std::max(std::pow(open / covered, 3.6), 0.1)) * covered / all : 1;
    Check(Near(Compose(taps, 4, 0, float(x), float(y)), float(expected)), "depth-aware four-tap compose against double precision");
  }
  using Image = rgd::Image;
  using Format = mcla::gpu::rr::Format;
  Check(rgd::AoSpecOf(Image::kSignal).format == Format::kR16Float && rgd::AoSpecOf(Image::kDenoised).bytes_per_texel == 2 &&
            rgd::AoSpecOf(Image::kDepth).format == Format::kR32Float && rgd::AoSpecOf(Image::kMotion).format == Format::kRgba16Float,
        "AMD AO signal and output are scalar and retain the shared guide formats");
}

constexpr int kExtent = 128;
constexpr int kRacerWarmup = 12;
struct Pixel {
  float truth = 0.55f, raw = 1.0f, depth = 10.0f, normal = 0.0f, x_prev = 0.0f, y_prev = 0.0f;
  uint32_t surface = 0;
  bool judged = true;
};
struct History { float open = 1, length = 0; };
bool Inside(int x, int y) { return x >= 0 && x < kExtent && y >= 0 && y < kExtent; }
size_t At(int x, int y) { return size_t(y) * kExtent + x; }
using Field = std::vector<Pixel>;
using HistoryImage = std::vector<History>;

HistoryImage Temporal(const Field& current, const Field& previous, const HistoryImage& history, bool valid) {
  HistoryImage out(current.size());
  for (int y = 0; y < kExtent; ++y) {
    for (int x = 0; x < kExtent; ++x) {
      const size_t i = At(x, y);
      const auto& c = current[i];
      if (!c.judged) continue;
      out[i] = {c.raw, 1};
      if (!valid) continue;
      const int base_x = int(std::floor(c.x_prev)), base_y = int(std::floor(c.y_prev));
      const float fx = c.x_prev - base_x, fy = c.y_prev - base_y;
      float weights = 0, opens = 0, lengths = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          if (!Inside(base_x + dx, base_y + dy)) continue;
          const size_t j = At(base_x + dx, base_y + dy);
          const auto& q = previous[j];
          if (history[j].length <= 0 || !ao::AcceptTap(q.depth, c.depth, (c.surface == 0) == (q.surface == 0), std::cos(q.normal - c.normal))) continue;
          const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * dn::DepthWeight(std::abs(q.depth - c.depth), c.depth, 0.03f);
          weights += w;
          opens += history[j].open * w;
          lengths += history[j].length * w;
        }
      }
      const float cap = ao::MotionCap(std::hypot(c.x_prev - x, c.y_prev - y), 32);
      const float n = ao::HistoryLength(lengths, weights, cap);
      if (n <= 0) continue;
      float lo = c.raw, hi = c.raw, sum = 0, sum2 = 0, count = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const auto& q = current[At(std::clamp(x + dx, 0, kExtent - 1), std::clamp(y + dy, 0, kExtent - 1))];
          if (!q.judged) continue;
          lo = std::min(lo, q.raw);
          hi = std::max(hi, q.raw);
          sum += q.raw;
          sum2 += q.raw * q.raw;
          count += 1;
        }
      }
      const float clipped = ao::Clip(opens / weights, sum / count, std::sqrt(dn::Variance(sum, sum2, count)), lo, hi);
      out[i] = {ao::Blend(clipped, c.raw, n), ao::NextLength(n, cap)};
    }
  }
  return out;
}

HistoryImage Spatial(const Field& guides, HistoryImage input, bool resolve) {
  for (int step : {1, 2, 4}) {
    HistoryImage out = input;
    for (int y = 0; y < kExtent; ++y) {
      for (int x = 0; x < kExtent; ++x) {
        const size_t i = At(x, y);
        const auto& c = guides[i];
        const float len = input[i].length;
        const float radius = dn::Radius(dn::Profile::kAmbient, 1, 0, c.depth, 0.01f, len);
        if (!c.judged || !dn::AmbientStep(radius, step, len, resolve, c.surface != 0)) continue;
        float sum = 0, sum2 = 0, count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const size_t j = At(std::clamp(x + dx * step, 0, kExtent - 1), std::clamp(y + dy * step, 0, kExtent - 1));
            if (!guides[j].judged) continue;
            sum += input[j].open;
            sum2 += input[j].open * input[j].open;
            count += 1;
          }
        }
        const float variance = dn::Variance(sum, sum2, count);
        float total = 0, weights = 0;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            if (!Inside(x + dx * step, y + dy * step)) continue;
            const size_t j = At(x + dx * step, y + dy * step);
            const auto& q = guides[j];
            if (!q.judged || c.surface != q.surface) continue;
            float w = dn::Kernel1(dx) * dn::Kernel1(dy) * dn::DepthWeight(std::abs(q.depth - c.depth), c.depth, 0.03f) *
                      dn::NormalWeight(std::cos(c.normal - q.normal), 16.0f) * dn::LumaWeight(input[i].open, input[j].open, variance, 4);
            if (step == 2 && len >= dn::kMatureLength) w *= ao::ResolveEdgeWeight(input[i].open, input[j].open);
            weights += w;
            total += input[j].open * w;
          }
        }
        out[i].open = total / weights;
      }
    }
    input.swap(out);
  }
  return input;
}

enum class Scene { kStill, kRacer, kPan, kOrbit, kDisocclusion, kScreenEdge };
Field MakeField(Scene scene, int frame) {
  Field field(kExtent * kExtent);
  for (int y = 0; y < kExtent; ++y) {
    for (int x = 0; x < kExtent; ++x) {
      auto& p = field[At(x, y)];
      p.x_prev = float(x);
      p.y_prev = float(y);
      if (scene == Scene::kRacer) {
        const int edge = 16 + std::max(0, frame - kRacerWarmup) * 5;
        p.truth = x >= edge && x < edge + 10 ? 0.1f : 1.0f;
      } else if (scene == Scene::kPan) {
        const int edge = 16 + frame * 4;
        p.truth = x < edge ? 0.0f : 1.0f;
        p.depth = x < edge ? 3.0f : 20.0f;
        p.x_prev -= 4.0f;
      } else if (scene == Scene::kOrbit) {
        const float t = 0.035f * frame;
        const float u = float(x) - 63.5f, v = float(y) - 63.5f;
        p.truth = u * std::cos(t) + v * std::sin(t) < 0 ? 0.16f : 0.88f;
        const bool near_side = p.truth < 0.5f;
        p.depth = near_side ? 4.0f : 14.0f;
        p.normal = near_side ? t : t + 1.0f;
        p.x_prev = 63.5f + u * std::cos(0.035f) + v * std::sin(0.035f);
        p.y_prev = 63.5f - u * std::sin(0.035f) + v * std::cos(0.035f);
      } else if (scene == Scene::kDisocclusion) {
        p.truth = frame < 12 ? 0.2f : 0.57f;
        p.depth = frame < 12 ? 10.0f : 2.0f;
      } else if (scene == Scene::kScreenEdge) {
        p.truth = 0.65f;
        p.x_prev -= 2.5f;
      }
      const auto noise = shader::BlueNoiseFrame2(x, y, uint32_t(frame * 8 + 1));
      p.raw = noise.y < p.truth ? 1.0f : 0.0f;
    }
  }
  return field;
}

void CheckScenes(bool resolve) {
  for (Scene scene : {Scene::kStill, Scene::kRacer, Scene::kPan, Scene::kOrbit, Scene::kDisocclusion, Scene::kScreenEdge}) {
    Field previous;
    HistoryImage history;
    float trail = 0;
    const int frames = scene == Scene::kStill ? 32 : (scene == Scene::kDisocclusion || scene == Scene::kRacer ? 24 : 16);
    for (int f = 0; f < frames; ++f) {
      Field current = MakeField(scene, f);
      history = Temporal(current, previous, history, f != 0);
      const HistoryImage filtered = Spatial(current, history, resolve);
      previous = std::move(current);
      if (scene == Scene::kRacer && f > kRacerWarmup) {
        const int edge = 16 + (f - kRacerWarmup) * 5;
        float sum = 0;
        for (int y = 8; y < kExtent - 8; ++y) sum += 1 - filtered[At(edge - 3, y)].open;
        trail = std::max(trail, sum / (kExtent - 16));
      }
      if (f + 1 != frames) continue;
      double sum2 = 0;
      for (size_t i = 0; i < filtered.size(); ++i) sum2 += std::pow(filtered[i].open - previous[i].truth, 2);
      const double rms = std::sqrt(sum2 / filtered.size());
      std::printf("resolve %d, scene %d rms %.5f\n", int(resolve), int(scene), rms);
      if (scene == Scene::kStill) Check(rms < 0.03, "STILL: below .03 RMS after 32 frames");
      if (scene == Scene::kRacer) {
        std::printf("racer trail %.5f\n", double(trail));
        Check(trail < 0.05f, "RACER: trail beyond two pixels stays under .05");
      }
      if (scene == Scene::kDisocclusion) Check(rms < 0.05, "disocclusion: below .05 RMS after twelve new frames");
      if (scene == Scene::kOrbit) Check(rms < 0.10, "ORBIT: smooth both rotated surfaces without a visible trail");
      if (scene == Scene::kPan) {
        const int edge = 16 + f * 4;
        int edge_width = 0;
        for (int x = edge - 8; x < edge + 8; ++x) {
          float mean = 0;
          for (int y = 0; y < kExtent; ++y) mean += filtered[At(x, y)].open / kExtent;
          edge_width += mean > 0.1f && mean < 0.9f;
        }
        Check(edge_width <= 1, "PAN: at most 1.5 times the one-pixel unfiltered edge");
      }
      if (scene == Scene::kScreenEdge) {
        double near = 0, center = 0;
        for (int y = 0; y < kExtent; ++y) {
          for (int x = 0; x < 8; ++x) {
            near += filtered[At(x, y)].open;
            center += filtered[At(x + 60, y)].open;
          }
        }
        const double delta = std::abs(near - center) / (kExtent * 8);
        std::printf("screen-edge band delta %.5f\n", delta);
        Check(delta < 0.02, "screen-edge entry: no dark or light band");
      }
    }
  }
  Field guides(kExtent * kExtent);
  HistoryImage history(guides.size());
  for (int y = 0; y < kExtent; ++y) {
    for (int x = 0; x < kExtent; ++x) {
      guides[At(x, y)].surface = x < 64 ? 0 : (x < 72 ? 2 : 3);
      history[At(x, y)] = {x < 64 ? 1.0f : (x < 72 ? 0.0f : 0.5f), 32.0f};
    }
  }
  const auto filtered = Spatial(guides, history, resolve);
  Check(Near(filtered[At(63, 63)].open, 1) && Near(filtered[At(64, 63)].open, 0) &&
            Near(filtered[At(71, 63)].open, 0) && Near(filtered[At(72, 63)].open, 0.5f),
        "spatial taps stop at the world and vehicle boundaries and between IDs");
}

void CheckContactFringe() {
  Field previous;
  HistoryImage history;
  for (int frame = 0; frame < 32; ++frame) {
    Field current(kExtent * kExtent);
    for (int y = 0; y < kExtent; ++y) {
      for (int x = 0; x < kExtent; ++x) {
        auto& pixel = current[At(x, y)];
        pixel.x_prev = float(x);
        pixel.y_prev = float(y);
        pixel.truth = std::clamp(0.75f + (float(y) - 64.0f) / 80.0f, 0.3f, 1.0f);
        pixel.raw = shader::RtAoRayNoise(x, y, uint32_t(frame), 0).x < pixel.truth ? 1.0f : 0.0f;
      }
    }
    history = Temporal(current, previous, history, frame != 0);
    previous = std::move(current);
  }
  const auto original = Spatial(previous, history, false);
  const auto resolved = Spatial(previous, history, true);
  const auto metrics = [](const HistoryImage& filtered) {
    float energy = 0.0f, mean = 0.0f;
    for (int y = 52; y < 76; ++y) {
      for (int x = 8; x < kExtent - 8; ++x) {
        const float open = filtered[At(x, y)].open;
        const float hp = open - 0.25f * (filtered[At(x - 2, y)].open + filtered[At(x + 2, y)].open +
                                         filtered[At(x, y - 2)].open + filtered[At(x, y + 2)].open);
        energy += hp * hp;
        mean += open;
      }
    }
    constexpr int pixels = 24 * (kExtent - 16);
    return std::array{std::sqrt(energy / pixels), mean / pixels};
  };
  const auto [original_rms, original_mean] = metrics(original);
  const auto [resolved_rms, resolved_mean] = metrics(resolved);
  std::printf("contact fringe: high-pass RMS %.5f -> %.5f, mean %.5f -> %.5f\n",
              original_rms, resolved_rms, original_mean, resolved_mean);
  Check(resolved_rms < original_rms * 0.75f, "mature world resolve reduces high-frequency contact grain");
  Check(Near(original_mean, resolved_mean, 0.01f), "mature world resolve keeps the mean contact visibility");
}
}  // namespace

int main() {
  CheckMath();
  CheckCompose();
  CheckScenes(false);
  CheckScenes(true);
  CheckContactFringe();
  std::printf("rtao_denoise_test: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
