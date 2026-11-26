// Check of the bounce light's clip box and a-trous passes (ssgi_stable.h). A program of its own: it needs no renderer, no ReXGlue and no
// GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssgi_stable_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssgi_stable_test [<src/gpu dir>]
//
// With the src/gpu folder as its argument it also checks that the shaders' and ao.cpp's constants are the header's (they repeat them).
// With --print it lists the numbers the checks are made on.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "ao_fast.h"
#include "setting_source.h"
#include "ssgi_stable.h"

using namespace mcla::gpu::ssgi_stable;
using mcla::gpu::ao_stable::kReferenceHeight;

namespace {

namespace shader {
using std::abs;
using std::clamp;
using std::max;
using std::sqrt;
#include "hlsl/ssgi_sector.hlsli"
#include "hlsl/ssgi_filter_plane.hlsli"
}

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[240];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}

// ---------------------------------------------------------------------------
// the clip box
// ---------------------------------------------------------------------------

void CheckClip() {
  Check(kFirstClipWidth == 1.25f && kFirstClipHistoryShare == 0.0f, "the first version's box is 1.25 deviations and no share of the history");
  Check(kClipWidth > kFirstClipWidth && kClipHistoryShare > 0.0f, "the box is wider than the first version's");
  // The code the share travels in: 4 bits of 0.05.
  Check(ClipHistoryCode(kClipHistoryShare) == 3u && std::fabs(ClipHistoryOfCode(3u << kClipHistoryShift) - kClipHistoryShare) < 1e-6f, "the share of 0.15 is the code 3");
  bool round_trip = true;
  for (uint32_t code = 0; code <= 15; ++code) {
    round_trip = round_trip && ClipHistoryCode(ClipHistoryOfCode(code << kClipHistoryShift)) == code;
  }
  Check(round_trip, "every code of the share reads back");
  Check(ClipHistoryCode(-1.0f) == 0u && ClipHistoryCode(10.0f) == 15u && ClipHistoryCode(0.0f) == 0u, "the share's code is clamped to 0..15");
  Check((kClipHistoryMask >> kClipHistoryShift) == 0xFu && kClipHistoryShift == 26, "the share is in bits 26-29");
  // The box's half width: deviations of the neighbourhood, the floor share of its mean, and the share of the history.
  const float first = ClipHalfWidth(1.0f, 0.1f, 1.5f, kFirstClipWidth, kFirstClipHistoryShare);
  const float now = ClipHalfWidth(1.0f, 0.1f, 1.5f, kClipWidth, kClipHistoryShare);
  Check(std::fabs(first - (1.25f * 0.1f + 0.05f)) < 1e-6f, "the first version's half width: 1.25 deviations and 5% of the mean");
  Check(std::fabs(now - (kClipWidth * 0.1f + 0.05f + kClipHistoryShare * 1.5f)) < 1e-6f, "the half width now: 2 deviations, 5% of the mean and 15% of the history");
  Check(now > first, "the box is wider");
  // A history a frame's noise away from the mean is kept; far away it is brought to the edge; nothing is clipped to below 0.
  Check(ClipHistory(1.05f, 1.0f, 0.1f, kClipWidth, kClipHistoryShare) == 1.05f, "a history inside the box is kept");
  Check(ClipHistory(3.0f, 1.0f, 0.1f, kClipWidth, kClipHistoryShare) < 3.0f && ClipHistory(3.0f, 1.0f, 0.1f, kClipWidth, kClipHistoryShare) > 1.0f, "a history far outside is brought to the edge");
  Check(ClipHistory(0.0f, 0.0f, 0.0f, kClipWidth, kClipHistoryShare) == 0.0f && ClipHistory(2.0f, 0.0f, 0.0f, kClipWidth, kClipHistoryShare) < 2.0f, "a neighbourhood of nothing clips a history of light");
  Check(ClipHistory(-1.0f, 0.2f, 0.05f, kClipWidth, kClipHistoryShare) >= 0.0f, "a clipped history is never negative");
  // The share keeps a history that the blurred neighbourhood's own noise would have cut: a frame whose mean is 20% off the history, with
  // a neighbourhood whose nine values differ by 3% (what the blur leaves), is inside the new box and outside the first version's.
  const float mean = 1.0f, deviation = 0.03f, history = 1.2f;
  Check(ClipHistory(history, mean, deviation, kFirstClipWidth, kFirstClipHistoryShare) < history, "the first version's box clips a history that is 20% off a tight neighbourhood");
  Check(ClipHistory(history, mean, deviation, kClipWidth, kClipHistoryShare) == history, "the box now keeps it");
  // ... and still catches a light that really changed: a history 3 times the neighbourhood's mean is cut to under 1.7 of it.
  Check(ClipHistory(3.0f, 1.0f, 0.03f, kClipWidth, kClipHistoryShare) < 1.7f, "a light that changed by a factor of 3 is cut to under 1.7 of the new one in one frame");
  // The box is monotone: a wider setting never clips more.
  bool monotone = true;
  for (float w = 0.25f; w < 8.0f; w += 0.25f) {
    monotone = monotone && ClipHistory(2.0f, 1.0f, 0.1f, w + 0.25f, kClipHistoryShare) >= ClipHistory(2.0f, 1.0f, 0.1f, w, kClipHistoryShare);
  }
  Check(monotone, "a wider box clips no more");
}

// ---------------------------------------------------------------------------
// the a-trous passes
// ---------------------------------------------------------------------------

void CheckAtrous() {
  Check(AtrousIterations(2, 1080, true) == 2, "residual bounce can omit the extra pass at 1080 traced lines");
  Check(AtrousIterations(2, 2160, true) == 3, "residual bounce omits exactly one extra pass at full 4K");
  Check(AtrousIterations(2, 720, true) == 2 && AtrousIterations(0, 2160, true) == 0,
        "the short filter retains the user-selected base and honours an explicitly disabled filter");
  const struct {
    int base;
    uint32_t height;
    int passes;
  } kTable[] = {{2, 360, 2},  {2, 540, 2},  {2, 720, 2},  {2, 1018, 2},  {2, 1019, 3}, {2, 1080, 3}, {2, 1440, 3}, {2, 1800, 3},
                {2, 2036, 3}, {2, 2037, 4}, {2, 2160, 4}, {2, 2880, 4}, {2, 4072, 4}, {2, 4073, 5}, {2, 8640, 5}, {0, 2160, 0}, {1, 2160, 3},
                {3, 720, 3}, {3, 2160, 5}, {5, 720, 5},  {2, 0, 2}};
  for (const auto& row : kTable) {
    Check(AtrousIterations(row.base, row.height) == row.passes,
          Num("the a-trous passes for a base of %.0f at %.0f lines", double(row.base), double(row.height)) + Num(" are %.0f", double(row.passes)) +
              Num(" (got %.0f)", double(AtrousIterations(row.base, row.height))));
  }
  bool bounded = true, monotone = true;
  int last = 0;
  for (uint32_t h = 0; h <= 10000; h += 13) {
    const int n = AtrousIterations(2, h);
    bounded = bounded && n >= 2 && n <= kMaxAtrous;
    monotone = monotone && n >= last;
    last = n;
  }
  Check(bounded && monotone, "the passes never fall below the setting, never rise over 5, and never fall as the height grows");
  Check(AtrousReachPixels(0) == 0 && AtrousReachPixels(2) == 6 && AtrousReachPixels(3) == 14 && AtrousReachPixels(4) == 30 && AtrousReachPixels(5) == 62, "the reach of n passes of the 5x5 kernel is 2 + 4 + 8 + ...");
  // What it buys: the reach in the pixels of a 720 line picture. The first version's 2 passes reached 6 pixels at 720 lines and 2 at 2160.
  double lo = 1e9;
  for (uint32_t h = 720; h <= 4320; h += 7) {
    const double reach720 = double(AtrousReachPixels(AtrousIterations(2, h))) * double(kReferenceHeight) / double(h);
    lo = std::min(lo, reach720);
  }
  if (g_print) {
    std::printf("the a-trous filter's reach in 720 line pixels from 720 to 4320 lines: at least %.2f (the first version: 6.00 at 720, 2.00 at 2160)\n", lo);
  }
  Check(lo >= 4.2, Num("from 720 lines up the filter reaches at least 4.2 pixels of a 720 line picture (got %.2f)", lo));
  Check(double(AtrousReachPixels(AtrousIterations(2, 2160))) * double(kReferenceHeight) / 2160.0 > 2.9 * 2.0, "at 4K it reaches three times what the first version's two passes did");
  // The bounce light traced at half the picture's size above 1080 lines (ao_size.h, mcla_gpu_ssgi_half): the passes follow the traced image's
  // height (ao.cpp takes it from plan.ao[1]), and what they reach as a share of the PICTURE stays within 30% of the full size image's.
  for (const uint32_t picture : {1200u, 1440u, 1620u, 2160u, 2880u}) {
    const uint32_t half = (picture + 1) / 2;
    const double full_share = double(AtrousReachPixels(AtrousIterations(2, picture))) / double(picture);
    const double half_share = double(AtrousReachPixels(AtrousIterations(2, half))) / double(half);
    Check(half_share / full_share >= 0.7 && half_share / full_share <= 1.3,
          Num("a picture of %.0f lines: the bounce's a-trous reach traced at half size is within 30% of the full size image's", double(picture)) +
              Num(" (%.3f against ", half_share) + Num("%.3f of the height)", full_share));
  }
  Check(AtrousIterations(2, 1080) == 3 && AtrousIterations(2, 720) == 2, "4K traced at 1080 lines runs 3 passes, 1440p traced at 720 runs 2 (4 and 3 at full size)");
}

void CheckFastMath() {
  constexpr float half_pi = 1.57079632679f;
  std::mt19937 random(0x53534749u);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  for (int i = 0; i < 10000; ++i) {
    const float c = unit(random);
    const float n = unit(random) * half_pi;
    const float side = i & 1 ? 1.0f : -1.0f;
    const float sn = std::sin(n);
    const float cn = std::cos(n);
    for (float angle : {std::acos(c), mcla::gpu::ao_fast::AcosFast(c)}) {
      const float theta = side * angle;
      const float t = n + std::clamp(theta - n, -half_pi, half_pi);
      const float ref = (cn - std::cos(2.0f * t - n) + 2.0f * t * sn) * 0.25f;
      const float got = shader::SsgiFastSectorIntegral(c, side, theta, n, sn, cn, half_pi, t);
      Check(std::abs(got - ref) <= 3.6e-5f, "horizon integral stays within 0.0012 of a sector");
    }

    const float nx = unit(random), ny = unit(random), nz = unit(random);
    const float x = unit(random), y = unit(random);
    const float dx = unit(random) * 0.03f, dy = unit(random) * 0.03f;
    const float centre = 1.0f + 400.0f * std::abs(unit(random));
    const float tap = std::max(0.02f, centre * (1.0f + 0.1f * unit(random)));
    const float centre_ray = nx * x + ny * y + nz;
    const float slow = nx * ((x + dx) * tap - x * centre) + ny * ((y + dy) * tap - y * centre) + nz * (tap - centre);
    const float fast = shader::SsgiFilterPlaneOffset(tap, centre, centre_ray, nx * dx + ny * dy);
    const float sigma = 0.006f * centre + 0.01f;
    const auto weight = [sigma](float offset) { return std::exp(-0.5f * (offset / sigma) * (offset / sigma)); };
    Check(std::abs(weight(fast) - weight(slow)) <= 2.0e-5f, "affine plane reconstruction preserves bilateral weights");
  }
}

// ---------------------------------------------------------------------------
// the shaders and ao.cpp repeat the header's numbers
// ---------------------------------------------------------------------------

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CheckShaders(const std::string& gpu_dir) {
  const std::string temporal = Slurp(gpu_dir + "/hlsl/ao_temporal_ps.hlsl");
  const std::string common = Slurp(gpu_dir + "/hlsl/ao_common.hlsli");
  const std::string main_ps = Slurp(gpu_dir + "/hlsl/ao_main_ps.hlsl");
  const std::string filter = Slurp(gpu_dir + "/hlsl/ao_atrous_ps.hlsl");
  const std::string cpp = Slurp(gpu_dir + "/ao.cpp");
  Check(!temporal.empty() && !common.empty() && !main_ps.empty() && !cpp.empty(), "the shaders and ao.cpp are readable from " + gpu_dir);
  if (temporal.empty() || common.empty() || main_ps.empty() || cpp.empty()) {
    return;
  }
  auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
  // The box in the temporal pass: the floor, the share's bits and unit, the term in the half width.
  Check(has(temporal, Num("static const float kBounceBoxFloor = %.2ff;", double(kClipFloorShare))), "ao_temporal_ps.hlsl: the box's floor is the header's");
  Check(has(temporal, Num("(g_push.slots2.z >> %.0f) & 0xFu) * %.2ff", double(kClipHistoryShift), double(kClipHistoryUnit))), "ao_temporal_ps.hlsl: the share is in bits 26-29 in units of 0.05");
  Check(has(temporal, "g_push.lens.w * spread + kBounceBoxFloor * m1 + AoClipHistoryShare() * history.yzw"), "ao_temporal_ps.hlsl: the half width is deviations + floor + a share of the history");
  Check(has(temporal, "normal, AoWideRadius());") && !has(temporal, "normal, 2);"), "ao_temporal_ps.hlsl: both wide blurs take their radius from AoWideRadius");
  // The blur of the four channels: the same sigma as the occlusion's (ao_stable.h), no first-version constant left.
  Check(has(common, "const float spatial_sigma = AoBlurSigma(radius);") && !has(common, "radius >= 2 ? 1.4f : 0.9f"), "ao_common.hlsli: the blur over four channels takes its sigma from AoBlurSigma");
  // The bounce light's main pass takes its noise from the same shift as the occlusion's.
  Check(!has(main_ps, "frame * float2("), "ao_main_ps.hlsl: the bounce light's main pass keeps no copy of the recurrence");
  const size_t occupy = main_ps.find("occupied |= bits;");
  const size_t black = main_ps.find("if (!any(radiance > 0.0f))");
  const size_t facing = main_ps.find("if (facing_weight > 0.0f)");
  Check(occupy < black && black < facing && facing != std::string::npos, "black samples still cover their sectors before skipping the facing work");
  Check(has(main_ps, "far_ring && (!AoFastGather() || occupied != 0xFFFFFFFFu)") &&
            has(filter, "SsgiFilterPlaneOffset(tq, tc, ray_at_centre,"),
        "skip a fully occluded far ring and use the tested affine filter plane");
  // The setting's default (a REX_SETTING row of src/settings/generated; the REXCVAR_DEFINE of ao.cpp before the conversion), then ao.cpp: the first
  // version's values for the switch, the passes, the share's bits.
  const setting_source::Info clamp = setting_source::Find(gpu_dir, cpp, "mcla_gpu_ssgi_test_history_clamp");
  Check(setting_source::DefaultIs(clamp, double(kClipWidth)), "mcla_gpu_ssgi_test_history_clamp: the setting's default is kClipWidth");
  Check(has(cpp, "ssgi_stable::kFirstClipWidth") && has(cpp, "ssgi_stable::kFirstClipHistoryShare") && has(cpp, "ssgi_stable::kClipHistoryShare"),
        "ao.cpp: the switch and the plan use the header's widths and shares");
  Check(has(cpp, "ssgi_stable::AtrousIterations(int(plan.gi.atrous), plan.ao[1], trim_height_pass)"), "ao.cpp: the a-trous passes follow the image's height");
  Check(has(cpp, "plan.gi.lights_only && plan.render[1] >= 2160 &&") && has(cpp, "mcla_gpu_ssgi_residual_short_filter.Get()"),
        "ao.cpp: only 4K residual local-light bounce can opt into the short filter");
  Check(has(cpp, "ssgi_stable::ClipHistoryCode(plan.gi.clip_history) << ssgi_stable::kClipHistoryShift"), "ao.cpp: the share goes into the temporal pass's flags by the header's code");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      gpu_dir = argv[i];
    }
  }
  CheckClip();
  CheckAtrous();
  CheckFastMath();
  if (!gpu_dir.empty()) {
    CheckShaders(gpu_dir);
  }
  std::printf("ssgi_stable_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
