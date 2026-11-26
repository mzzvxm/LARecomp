#include "rtao_rules.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>

namespace rtao = mcla::gpu::rtao;
namespace car = mcla::gpu::car_rt_ao;
using mcla::gpu::gpu_class::RtTier;

namespace {

int checks = 0, failed = 0;

void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failed;
    std::printf("FAIL: %s\n", what);
  }
}

bool Near(double a, double b, double tolerance = 1e-6) {
  return std::abs(a - b) <= tolerance;
}

void PlanChecks() {
  using enum rtao::Fallback;
  rtao::PlanInputs f;
  f.render[0] = 1921;
  f.render[1] = 1081;
  f.budget = rtao::BudgetFor(RtTier::kStrong);
  f.range = 120.0f;
  Check(rtao::Decide(f).why == kNotWanted && rtao::Decide(f).screen_pass, "identity precedes all fallback reasons");
  f.wanted = true;
  Check(rtao::Decide(f).why == kNotPlayView, "non-play view precedes pipelines");
  f.play_view = true;
  Check(rtao::Decide(f).why == kPipelineNotReady, "pipeline precedes TLAS");
  f.pipelines = true;
  Check(rtao::Decide(f).why == kNoTopLevel, "TLAS precedes city");
  f.top_level = true;
  Check(rtao::Decide(f).why == kNoCity, "city precedes images");
  f.city = true;
  Check(rtao::Decide(f).why == kImagesFailed, "images precede trace");
  f.screen = rtao::Screen::kOff;
  Check(!rtao::Decide(f).screen_pass, "screen off leaves a fallback receiver open");
  f.images = true;
  f.screen = rtao::Screen::kAuto;
  const auto plan = rtao::Decide(f);
  Check(plan.on && plan.why == kNone && !plan.screen_pass && !plan.raw, "ready play frame traces only");
  Check(plan.traced[0] == 961 && plan.traced[1] == 541 && plan.rays == 1 && Near(plan.reach, 1.5) && Near(plan.range, 90),
        "half size rounds up and the strong rung has one ray with a 90 m range limit");
  f.cabin_screen = true;
  Check(rtao::Decide(f).screen_pass, "uncovered cabin asks for screen AO");
  f.screen = rtao::Screen::kOff;
  Check(!rtao::Decide(f).screen_pass, "screen off wins over cabin");
  f.bounce_light = true;
  Check(rtao::Decide(f).screen_pass, "bounce light still needs visibility");
  f.bounce_light = f.cabin_screen = false;
  f.screen = rtao::Screen::kOn;
  f.scale = 4;
  f.rays = 4;
  Check(rtao::Decide(f).screen_pass && rtao::Decide(f).traced[0] == 481 && rtao::Decide(f).rays == 4,
        "blend, quarter size and four-ray debug arms");
  f.screen = rtao::Screen::kWorld;
  Check(rtao::Decide(f).on && rtao::Decide(f).screen_pass, "world blending produces a screen term even without bounce light");
  f.raw = true;
  f.scale = 1;
  Check(rtao::Decide(f).rays == 2 && rtao::Decide(f).raw && rtao::Decide(f).traced[0] == 1921, "raw uses two rays at its requested extent");
  using S = rtao::Screen;
  Check(rtao::ParseScreen("world") == S::kWorld, "world is a live screen mode");
  Check(rtao::ParseScreen("auto") == S::kAuto && rtao::ParseScreen("") == S::kAuto &&
            rtao::ParseScreen("off") == S::kOff && rtao::ParseScreen("on") == S::kOn,
        "existing screen modes and the unknown-value fallback");
  Check(rtao::ScreenBlendFlags(S::kAuto, false) == 0u && rtao::ScreenBlendFlags(S::kOff, false) == 0u &&
            rtao::ScreenBlendFlags(S::kOn, false) == rtao::kBlendScreenAll &&
            rtao::ScreenBlendFlags(S::kWorld, false) == rtao::kBlendScreenWorld,
        "only the two blend modes set compose flags");
  Check(rtao::ScreenBlendFlags(S::kWorld, true) == 0u && rtao::ScreenBlendFlags(S::kOn, true) == 0u,
        "the force-open identity test bypasses blending");
  for (unsigned bits = 0; bits < 32; ++bits) {
    Check(rtao::KeepHistory(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16) == (bits == 15),
          "history needs a consecutive ours-frame of the same size and a usable reprojection");
  }
}

void RuleChecks() {
  for (uint32_t bits = 0; bits < 16; ++bits) {
    Check(rtao::UseQuarterAt4k(bits & 1, 2160, bits & 2, bits & 4, bits & 8) == (bits == 3),
          "quarter-size experiment needs the switch and automatic scale in an exterior non-raw frame");
  }
  Check(!rtao::UseQuarterAt4k(true, 2159, true, false, false) && rtao::UseQuarterAt4k(true, 2161, true, false, false),
        "quarter-size experiment starts at 2160 render lines");
  for (uint32_t n = 0; n < 8193; ++n) {
    Check(rtao::TracedExtent(n) == (n + 1) / 2, "traced size rounds up");
  }
  Check(rtao::TracedExtent(UINT32_MAX) == 0x80000000u, "traced size cannot overflow");
  Check(rtao::UpsampleNeeded(1080, 540) && !rtao::UpsampleNeeded(1080, 1080), "upsample only when the term is taller");
  const auto strong = rtao::BudgetFor(RtTier::kStrong);
  const auto weak = rtao::BudgetFor(RtTier::kWeak);
  Check(strong.rays == 1 && strong.boost && Near(strong.reach_share, 0.50) && strong.history_cap == 32 && strong.exact_alpha, "strong budget");
  Check(weak.rays == 1 && weak.boost && Near(weak.reach_share, 0.50) && weak.history_cap == 32 && weak.exact_alpha, "the weak and strong tiers use the same budget");
  Check(rtao::BudgetFor(RtTier::kNone).rays == 0, "no hardware rays");
  Check(rtao::kRayMask == 0x9D && rtao::kCabinMask == 3 && rtao::kBoostBelow == 4, "trace, cabin and history constants");
  Check(Near(rtao::Reach(3, 0.5, 0), 1.5) && Near(rtao::Reach(3, 0.4, 10), 2.5) && Near(rtao::Reach(10, 0.5, 0.1), 0.75),
        "look and debug reach use the same clamp");
  Check(Near(rtao::RayLength(2, 0.25f), 1) && Near(rtao::RayLength(2, 0), 2) && Near(rtao::RayLength(2, 1), 0), "quadratic reach falloff");
  Check(Near(rtao::Lift(5, 0.01f), 0.041) && Near(rtao::CarReach(1.5f, 0.4f), 0.6), "lift and shorter vehicle reach");
  Check(Near(rtao::OpenShare(1, 4), 0.75) && Near(rtao::VisibilityOf(0.5, 2, 0.1), 0.25) &&
        Near(rtao::VisibilityOf(0, 2, 0.1), 0.1), "open share, power and floor");
  for (uint32_t i = 0; i <= 64; ++i) {
    Check(rtao::CabinClosed(i, 64) == (i >= 62), "closed cabin threshold");
  }
  Check(!rtao::CabinClosed(62, 63) && !rtao::CabinClosed(65, 64), "incomplete or invalid probe");
  using I = rtao::Interior;
  Check(rtao::ScreenCabin(I::kAuto, true, false) && !rtao::ScreenCabin(I::kAuto, true, true) &&
        rtao::ScreenCabin(I::kScreen, true, true) && !rtao::ScreenCabin(I::kRt, true, false) &&
        !rtao::ScreenCabin(I::kScreen, false, false), "interior policies and exterior");
  car::Box box;
  box.valid = true;
  for (int i = 0; i < 3; ++i) {
    box.min[i] = -1.0f;
    box.max[i] = 1.0f;
  }
  const float edge[3] = {1.05f, 0, 0};
  const auto rect = rtao::BoxPixelRect(box, 0.1f, 1, 1, 1920, 1080);
  Check(rtao::InsideBox(edge, box, 0.1f) && !rtao::InsideBox(edge, box, 0.01f) &&
        rect.width() == 1920 && rect.height() == 1080, "cabin geometry and projection");
  Check(rtao::ImageBytes(960, 540) == 31'104'000 && rtao::TermBytes(1920, 1080, false) == 4'147'200 &&
        rtao::TermBytes(1920, 1080, true) == 16'588'800 && Near(rtao::EstimatedRays(960, 540, 1), 570'240),
        "image memory and ray model");
  Check(sizeof(rtao::RtAoPush) == 128 && offsetof(rtao::RtAoPush, trace) == 32 &&
        offsetof(rtao::RtAoPush, lens) == 48 && offsetof(rtao::RtAoPush, box_max) == 96 && offsetof(rtao::RtAoPush, rate) == 112, "128-byte push layout");
  for (unsigned b = 0; b < 8; ++b)
    Check(car::OcclusionOn(b & 1, b & 2, b & 4) == (b == 1 || b == 5 || b == 7), "car term is folded except in the blend arm");
}

// A top-down viewer over a flat receiver. A vertical obstacle is seen at a
// radial distance d and height h. Integrate its cosine-weighted solid angle
// with the trace's length law and the Ultra horizon's distance weight.
struct Obstacle {
  double wall_x, wall_y, pole_radius, height;
};

double Distance(const Obstacle& o, double x, double y) {
  double d = 1e6;
  if (x > 0.0 && o.wall_x > 0.0) d = o.wall_x / x;
  if (y > 0.0 && o.wall_y > 0.0) d = std::min(d, o.wall_y / y);
  if (o.pole_radius > 0.0 && x > 0.0) {
    const double discriminant = o.pole_radius * o.pole_radius - 0.25 * y * y;
    if (discriminant >= 0.0) d = std::min(d, 0.5 * x - std::sqrt(discriminant));
  }
  return d;
}

void Calibration() {
  // A 10 cm kerb at four offsets, a low retaining corner, and a bollard.
  constexpr Obstacle scenes[] = {{0, 0, 0, 0}, {0.25, 0, 0, 0.10}, {0.50, 0, 0, 0.10},
                                 {1.0, 0, 0, 0.10}, {2.0, 0, 0, 0.10}, {0.5, 0.5, 0, 0.18},
                                 {0, 0, 0.09, 0.6}};
  constexpr float radii[] = {2.5f, 3.0f, 3.5f, 3.75f};
  for (float radius : radii) {
    double rt_sum = 0, gtao_sum = 0, last_wall = 0;
    for (size_t i = 0; i < std::size(scenes); ++i) {
      const auto& scene = scenes[i];
      const float reach = rtao::Reach(radius, rtao::BudgetFor(RtTier::kStrong).reach_share, 0);
      double rt_blocked = 0, screen_blocked = 0;
      constexpr int azimuths = 512, strata = 128;
      for (int a = 0; a < azimuths; ++a) {
        const double angle = (a + 0.5) * (2 * std::numbers::pi / azimuths);
        const double d = Distance(scene, std::cos(angle), std::sin(angle));
        double horizon = 0;
        for (int k = 0; k < strata; ++k) {
          const double z = scene.height * (k + 0.5) / strata;
          const double t = std::hypot(d, z);
          horizon = std::max(horizon, (z / t) * std::clamp((radius - t) / (0.615 * radius), 0.0, 1.0));
          const double r2 = (k + 0.5) / strata;
          const double n = std::max(double(rtao::kMinAbove), std::sqrt(1.0 - r2));
          const double radial = std::sqrt(1.0 - n * n);
          const double at = d / radial;
          if (d * n <= scene.height * radial && at < reach) {
            const double open_probability = 1.0 - at / reach;
            rt_blocked += open_probability * open_probability / strata;
          }
        }
        screen_blocked += horizon * horizon;
      }
      const float rt = rtao::VisibilityOf(float(1.0 - rt_blocked / azimuths), 3.6f, 0.1f);
      const float gtao = rtao::VisibilityOf(float(1.0 - screen_blocked / azimuths), 3.6f, 0.1f);
      if (i == 0) Check(Near(rt, 1) && Near(gtao, 1), "open plane");
      if (i >= 1 && i <= 4) {
        Check(rt >= last_wall && rt >= 0.1f, "monotone wall distance and floor");
        last_wall = rt;
      }
      rt_sum += rt;
      gtao_sum += gtao;
    }
    Check(Near(rt_sum / std::size(scenes), gtao_sum / std::size(scenes), radius == 3 ? 0.04 : 0.06),
          "mean traced and GTAO Ultra visibility agree within the look tolerance");
  }
}

std::string Read(const std::filesystem::path& file) {
  std::ifstream stream(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

std::string Function(const std::string& text, const std::string& start) {
  const size_t first = text.find(start);
  if (first == std::string::npos) return {};
  return text.substr(first, text.find("\n}", first) + 2 - first);
}

void ShaderPins(const std::filesystem::path& gpu) {
  const std::string common = Read(gpu / "hlsl/rtao_common.hlsli");
  const std::string trace = Read(gpu / "hlsl/rtao_trace_cs.hlsl");
  const std::string surface = Read(gpu / "hlsl/rtao_surface.hlsli");
  const std::string car_shader = Read(gpu / "hlsl/car_rt_ao_cs.hlsl");
  const std::string host = Read(gpu / "rtao.cpp");
  for (const char* pin : {"kFlagRaysShift = 0", "kFlagAlphaShift = 4", "kFlagAlphaRefShift = 12", "kFlagMs = 1u << 6",
                          "kFlagRaw = 1u << 7", "kFlagCheckOpen = 1u << 8", "kFlagCheckClosed = 1u << 9",
                          "kFlagInterior = 1u << 10", "kFlagBlue = 1u << 11", "kFlagStats = 1u << 28", "kMinAbove = 0.08",
                          "uint4 slots;\n  uint4 sizes;\n  uint4 trace;\n  float4 lens;\n  float4 look;\n  float4 box_min;\n  float4 box_max;"}) {
    Check(common.find(pin) != std::string::npos, pin);
  }
  for (const char* pin : {"kCabinMask = 0x03u", "kCabinRays = 64u", "kCityMask = 0x80u",
                          "ray.TMax = 3.0", "reach * (1.0 - sqrt(u))", "kHalfSurfaceShare = 0.04",
                          "const float limit = shorten_car ? min(ray_length, car_reach) : ray_length",
                          "Blocked(origin, direction, limit, ray_length, kCityMask", "float(rays) / 4.0"}) {
    Check(trace.find(pin) != std::string::npos, pin);
  }
  Check(surface.find("0.006 + 3.0 * footprint + 0.001 * depth") != std::string::npos, "trace lift");
  Check(surface.find("kSelfCos = 0.85") != std::string::npos && surface.find("dot(hit_normal, own_normal) > kSelfCos") != std::string::npos,
        "same-instance self hit cosine");
  Check(surface.find("kSnapFootprint = 0.75;") != std::string::npos, "vehicle snap uses the car pass's footprint");
  for (const char* start : {"float3 ViewPos(", "float3 NormalFromDepth(", "void Frame(", "bool Snap("}) {
    std::string expected = Function(car_shader, start);
    if (const auto offset = expected.find("g_push.noise.y"); offset != std::string::npos)
      expected.replace(offset, std::string("g_push.noise.y").size(), "g_push.trace.y");
    std::string actual = Function(surface, start);
    if (std::string_view(start) == "bool Snap(") {
      const std::string city_guard =
          "    // The city is in this query's mask but has no row in the car geometry table.\n"
          "    if ((first.CommittedInstanceID() & kInstanceCity) != 0u)\n"
          "    {\n"
          "        return false;\n"
          "    }\n";
      const auto guard = actual.find(city_guard);
      Check(guard != std::string::npos, "the world snap excludes city geometry from the car normal read");
      if (guard != std::string::npos) actual.erase(guard, city_guard.size());
    }
    Check(!expected.empty() && expected == actual, start);
  }
  Check(host.find("uint32_t(in.noise_index & 0xFFFFF)") != std::string::npos, "the trace takes a 20-bit frame term");
}

}  // namespace

int main(int argc, char** argv) {
  RuleChecks();
  PlanChecks();
  Calibration();
  ShaderPins(argc > 1 && argv[1][0] != '-' ? std::filesystem::path(argv[1])
                                          : std::filesystem::path(__FILE__).parent_path());
  std::printf("rtao_rules_test: %d checks, %d failed\n", checks, failed);
  return failed != 0;
}
