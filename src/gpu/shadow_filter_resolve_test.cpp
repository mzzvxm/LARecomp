// What the sun shadow filter's disk should do while no temporal upscaler runs (NOISE-RESOLVE, perf\NOISE-AUDIT.md CHECK 1, src/gpu/noise_resolve.h). A program of its own: no renderer, no ReXGlue, no GPU, no game files.
//
//   cmake --build <dir> --target larecomp_shadow_filter_resolve_test, then run src/gpu/larecomp_shadow_filter_resolve_test [<src/gpu dir>] [--print] (a few seconds).
//
// THE QUESTION. The other effects that held one pattern without an upscaler now vary it every frame and resolve it with a history (ao.cpp RecordContactHistory, shafts.cpp RecordTemporal, ...). The shadow
// filter's disk is a Vogel disk of 16 taps (8 on a Low class by hand) turned per pixel by noise ONLY while an upscaler's history averages it (mode bit 4: shadow_filter.h MakeConstants) and not at all otherwise:
// every pixel takes the same fixed turn. The options for the frames without an upscaler, each measured here on the model that the variant's own text defines (a straight shadow edge across the disk, the
// disk's weights 1 - share / 2, taps at the golden angle, point samples: the filter's lit share against the integral of the same weights):
//   S0 fixed turn, 16 taps        what it is now: no grain, the error is a fixed function of the edge's direction and place (bands along an edge)
//   S1 blue tile turn fixed per pixel, 16 taps   the fixed pattern of the first noise fixes: a per pixel grain that nothing averages (the owner's "dithering artifacts" of 2026-11-19)
//   S2 the same with 24 and 32 taps   more taps for less grain: the cost grows with the taps (16 gathers a pixel at the sun's penumbra now)
//   S3 fixed turn, 32 taps        the same cost, no grain
//   S4 the turn changes every frame (blue tile + R2 by the frame) and the mask has a history   what an upscaler gives; the history would be an average over ~9 frames (weight 0.8)
// and the cost of S4's history, which the model cannot show: the lit share is the game's own sun shadow mask (the 1280x720 collector output, resolved mid frame and read by the GAME's lighting
// passes before any pass of ours that could average it: ao.cpp FindMaskSlot reads it for the contact shadows' apply only), so an average over frames would need a pass that rewrites the game's mask
// between the collector and its first reader, with its own reprojection (depth, the camera's and the vehicles' motion) in the middle of the game's frame order, and an image of the mask's size twice.
//
// THE NUMBERS (the test prints them with --print; the checks below pin the premises of the decision):
//   1. By the symmetry of the model the RMS error over all edge directions is the SAME for a fixed turn and for a turn that differs per pixel: what a per pixel turn changes is the structure of the
//      error (bands along an edge against grain), not its size. So S1 trades the owner's "no grain" for nothing in accuracy, and S4's gain is only the average over frames (1 / sqrt(9)).
//   2. The error falls slowly with the taps (about N^-0.75): 32 taps leave 0.6 of 16's, and the 3 times smaller error that an average over about 9 frames gives (weight 0.8) would take about 100 taps.
//   3. The worst edge direction of a fixed turn has 1.2 to 1.3 times the mean's RMS error and 1.7 to 1.8 times the best direction's: the banding along an edge, and what a history would have removed.
// DECISION (noise_resolve.h): the shadow filter's disk is NOT turned per frame without an upscaler. The disk keeps its fixed turn (S0; the owner approved it 2026-11-19, the noise bit stays off), because
// a per frame turn needs the history that the game's mid frame mask cannot have without a pass inside the game's own frame order, and the per pixel turn (S1/S2) is a grain nothing averages that costs
// as much as S3's taps for the same error. With an upscaler the turn changes every frame and the upscaler's history averages it, as before.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blue_noise.h"
#include "noise_resolve_check.h"
#include "shadow_filter.h"

namespace nc = noise_check;
namespace sf = mcla::gpu::shadow_filter;

namespace {

nc::Checks g_check;
constexpr double kPi = 3.14159265358979323846;
constexpr double kGolden = 2.399963229728653;  // the golden angle in radians (the variant's cos/sin constants)
constexpr double kFixedAngle = 2.399963;       // kShadowFilterFixedAngle: the disk's turn without noise

// A tap's compare: a point sample (kSoft 0) or the bilinear weighted compare of four texels the variant's mode bit 1 takes (a ramp across the edge as wide as a texel: the disk's radius is about 3 texels
// of the atlas, so kSoft 0.35 in units of the radius); the model reports both, the checks use ratios that hold for either.
double g_soft = 0.0;
double Lit(double x, double d) {
  if (g_soft <= 0.0) {
    return x < d ? 1.0 : 0.0;
  }
  return std::clamp((d - x) / g_soft + 0.5, 0.0, 1.0);
}

// The weighted share of the disk on the lit side of an edge at offset d (the edge's normal is +x, the disk's radius 1): the weight is 1 - share / 2 with share = r^2, so the area element r dr with the
// weight is the same as the share's: integrate over the share s in (0, 1) and the angle.
double Truth(double d) {
  const int ns = 400, na = 720;
  double lit = 0.0, total = 0.0;
  for (int i = 0; i < ns; ++i) {
    const double s = (double(i) + 0.5) / double(ns);
    const double w = 1.0 - 0.5 * s, r = std::sqrt(s);
    for (int a = 0; a < na; ++a) {
      const double x = r * std::cos((double(a) + 0.5) * 2.0 * kPi / double(na));
      total += w;
      lit += w * Lit(x, d);
    }
  }
  return lit / total;
}

// The variant's loop: `count` taps, tap i at the radius sqrt((i + 0.5) / count) in the direction `angle` turned by the golden angle i times, weight 1 - share / 2; the edge's normal is +x.
double Sample(int count, double angle, double d) {
  double lit = 0.0, total = 0.0;
  for (int i = 0; i < count; ++i) {
    const double s = (double(i) + 0.5) / double(count);
    const double w = 1.0 - 0.5 * s;
    const double x = std::sqrt(s) * std::cos(angle + kGolden * double(i));
    total += w;
    lit += w * Lit(x, d);
  }
  return lit / total;
}

struct Stats {
  double rms = 0.0;        // over the turns and the offsets
  double worst_rms = 0.0;  // the turn (= the edge direction against a fixed turn) with the largest rms over the offsets
  double best_rms = 0.0;
  double worst_peak = 0.0; // the largest error of all
};

Stats Measure(int count) {
  std::vector<double> truth;
  const int nd = 129, nt = 360;
  for (int j = 0; j < nd; ++j) {
    truth.push_back(Truth(-1.0 + 2.0 * double(j) / double(nd - 1)));
  }
  Stats s;
  s.best_rms = 1e9;
  double sum2 = 0.0;
  for (int t = 0; t < nt; ++t) {
    const double angle = (double(t) + 0.5) * 2.0 * kPi / double(nt);
    double row = 0.0;
    for (int j = 0; j < nd; ++j) {
      const double e = Sample(count, angle, -1.0 + 2.0 * double(j) / double(nd - 1)) - truth[size_t(j)];
      row += e * e;
      s.worst_peak = std::max(s.worst_peak, std::fabs(e));
    }
    sum2 += row;
    const double r = std::sqrt(row / double(nd));
    s.worst_rms = std::max(s.worst_rms, r);
    s.best_rms = std::min(s.best_rms, r);
  }
  s.rms = std::sqrt(sum2 / double(nd * nt));
  return s;
}

void TestNumbers(double soft) {
  g_soft = soft;
  const Stats s8 = Measure(8), s16 = Measure(16), s24 = Measure(24), s32 = Measure(32);
  const double codes = 255.0;  // the lit share's error in codes of a pixel the sun's whole light makes
  const char* kind = soft > 0.0 ? "bilinear PCF taps" : "point taps";
  if (g_check.print) {
    for (const auto& [n, s] : {std::pair<int, const Stats*>(8, &s8), {16, &s16}, {24, &s24}, {32, &s32}}) {
      std::printf("  %s, %2d taps: rms %.4f (%.1f codes of the sun's light), the worst direction of a fixed turn %.4f (%.1f codes), the best %.4f, the largest error %.3f\n", kind, n, s->rms, s->rms * codes,
                  s->worst_rms, s->worst_rms * codes, s->best_rms, s->worst_peak);
    }
  }
  // 1. the symmetry: the fixed turn's rms over all edge directions is the per pixel turn's rms (the same average over the same angles).
  double fixed_rms = 0.0;
  {
    const int nd = 129, nt = 360;
    std::vector<double> truth;
    for (int j = 0; j < nd; ++j) {
      truth.push_back(Truth(-1.0 + 2.0 * double(j) / double(nd - 1)));
    }
    double sum2 = 0.0;
    for (int t = 0; t < nt; ++t) {
      const double phi = (double(t) + 0.5) * 2.0 * kPi / double(nt);  // the edge's direction; the turn is fixed
      for (int j = 0; j < nd; ++j) {
        const double d = -1.0 + 2.0 * double(j) / double(nd - 1);
        // an edge turned by phi against a disk turned by the fixed angle is a disk turned by (fixed - phi) against an edge at 0
        const double e = Sample(16, kFixedAngle - phi, d) - truth[size_t(j)];
        sum2 += e * e;
      }
    }
    fixed_rms = std::sqrt(sum2 / double(nd * nt));
  }
  g_check(std::fabs(fixed_rms - s16.rms) < 0.002,
          nc::Num("%s: the fixed turn's rms over all edge directions (%.4f) is the per pixel turn's (%.4f): a per pixel turn changes the error's structure, not its size", kind, fixed_rms, s16.rms));
  g_check(s16.worst_rms > 1.1 * s16.rms && s16.worst_rms > 1.5 * s16.best_rms,
          nc::Num("%s: a fixed turn's worst edge direction has %.2f times the mean's rms (%.4f against %.4f) and %.1f times the best direction's: the bands along an edge", kind, s16.worst_rms / s16.rms,
                  s16.worst_rms, s16.rms, s16.worst_rms / s16.best_rms));
  g_check(s32.rms < 0.75 * s16.rms && s32.rms > 0.4 * s16.rms,
          nc::Num("%s: 32 taps leave %.2f of 16's rms (%.4f against %.4f), 24 taps %.2f: the error falls slowly with the taps and the lookups cost the taps", kind, s32.rms / s16.rms, s32.rms, s16.rms,
                  s24.rms / s16.rms));
  g_check(s8.rms > 1.3 * s16.rms, nc::Num("%s: 8 taps (a Low class given the filter by hand) have %.2f times 16's rms", kind, s8.rms / s16.rms));
  // 2. what S4's history would give: n_eff = (1 + w) / (1 - w) independent frames at weight w; and the taps that the same gain would cost
  const double w = 0.8, n_eff = (1.0 + w) / (1.0 - w);
  const double s4 = s16.rms / std::sqrt(n_eff);
  const double taps_for_s4 = 16.0 * std::pow(s16.rms / s4, 1.0 / 0.75);  // the error falls as N^-0.75: the taps that reach S4's error
  g_check(s4 < 0.4 * s16.rms && taps_for_s4 > 64.0,
          nc::Num("%s: an average over frames (weight %.1f: %.0f frames' worth) would take the 16 tap rms from %.4f to %.4f (%.1f codes); the same error from taps alone would take about %.0f taps (the cost is the taps)", kind, w,
                  n_eff, s16.rms, s4, s4 * codes, taps_for_s4));
  // 3. the grain of a per pixel turn: in codes of the sun's light, nothing averages it
  g_check(s16.rms * codes > 1.0, nc::Num("%s: a per pixel turn at 16 taps is %.1f codes (rms) of the sun's light of grain that nothing averages (the owner's dithering); 32 taps %.1f", kind, s16.rms * codes, s32.rms * codes));
}

// The tile's values, as the variant takes them (a permutation of the 1024 pixels): the per pixel turn of S1 is uniform on the circle, so S1's rms is the model's.
void TestTile() {
  const mcla::gpu::blue_noise::Tile tile = mcla::gpu::blue_noise::MakeTile();
  double mean = 0.0, var = 0.0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      mean += mcla::gpu::blue_noise::Sample(tile, 0, x, y);
    }
  }
  mean /= 1024.0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      const double v = mcla::gpu::blue_noise::Sample(tile, 0, x, y) - mean;
      var += v * v;
    }
  }
  var /= 1024.0;
  g_check(std::fabs(mean - 0.5) < 0.01 && std::fabs(var - 1.0 / 12.0) < 0.003, nc::Num("the tile's turn is uniform on the circle (mean %.3f, variance %.4f): a per pixel turn by it has the model's rms", mean, var));
}

void TestSources(const std::string& dir) {
  const std::string host = nc::ReadFile(dir + "/shadow_filter.h");
  const std::string variant = nc::ReadFile(dir + "/../shaderpack/variants/shadow_filter.cpp");
  const std::string rules = nc::ReadFile(dir + "/noise_resolve.h");
  if (host.empty() || variant.empty() || rules.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  g_check(nc::Has(host, "constants.noise_frame = temporal_frame_valid ? static_cast<float>(frame & 1023u) : 0.0f;") && nc::Has(host, "if (temporal_frame_valid) mode |= kModeNoise | (blue_noise ? kModeBlueNoise : 0u);"),
          "the disk's turn (mode bit 4) and its frame term are set only while a temporal upscaler runs: without one the disk keeps its fixed turn (the decision above)");
  g_check(nc::Has(variant, "kShadowFilterFixedAngle = 2.399963;") && nc::Has(variant, ": kShadowFilterFixedAngle;"), "the variant's fixed turn is the one the model measures (2.399963 radians)");
  g_check(nc::Has(variant, "float weight = 1.0 - 0.5 * share;") && nc::Has(variant, "float share = (float(i) + 0.5) / float(count);") && nc::Has(variant, "kShadowFilterGoldenCos"),
          "the variant's taps are the model's: radius sqrt((i + 0.5) / count), the golden angle turn, weight 1 - share / 2");
  g_check(nc::Has(rules, "the disk keeps its fixed turn"), "noise_resolve.h records the decision");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = nc::DirOf(__FILE__);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--print") {
      g_check.print = true;
    } else {
      dir = a;
    }
  }
  TestNumbers(0.0);
  TestNumbers(0.35);
  TestTile();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
