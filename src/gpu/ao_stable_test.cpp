// Check of the occlusion's blur radius, its wide blur, the noise sequence and the history weight (ao_stable.h). A program of its
// own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ao_stable_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_stable_test [<src/gpu dir>]
//
// With the src/gpu folder as its argument it also checks that the shaders' and ao.cpp's constants are the header's (they repeat them).
// With --print it lists the numbers the checks are made on (the ones ao_stable.h quotes).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_stable.h"

using namespace mcla::gpu::ao_stable;

namespace {

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
// the blur's radius
// ---------------------------------------------------------------------------

void CheckRadius() {
  // The table: 720 lines and under as the first version had it (3x3 and 5x5 wide), the radius growing with the height.
  const struct {
    uint32_t height;
    int radius;
  } kTable[] = {{0, 1},    {1, 1},    {360, 1},  {540, 1},  {720, 1},  {1000, 1}, {1079, 1}, {1080, 2},
                {1440, 2}, {1800, 3}, {2159, 3}, {2160, 3}, {2880, 3}, {4320, 3}, {8640, 3}};
  for (const auto& row : kTable) {
    Check(DenoiseRadius(row.height) == row.radius,
          Num("the blur's radius at %.0f lines is %.0f", double(row.height), double(row.radius)) + Num(" (got %.0f)", double(DenoiseRadius(row.height))));
  }
  // Never smaller than the first version's 3x3, never larger than kMaxRadius, and never shrinking as the picture grows.
  bool monotone = true;
  bool bounded = true;
  int last = 0;
  for (uint32_t h = 0; h <= 10000; h += 7) {
    const int r = DenoiseRadius(h);
    bounded = bounded && r >= kMinRadius && r <= kMaxRadius;
    monotone = monotone && r >= last;
    last = r;
  }
  Check(bounded && monotone, "the radius stays between 1 and 3 and never falls as the height grows");

  // What it buys: the blur's reach in the pixels of a 720 line picture. At 720 lines it is 1 (as it was); a picture of 540 lines and
  // up keeps it between 0.5 and 1.4, where the first version's 3x3 reached 1 at 720 lines and 0.33 at 2160. (360 lines, the low
  // quality's image of a 720 line picture: 2, unchanged.)
  Check(std::fabs(ReachIn720Pixels(720) - 1.0) < 1e-9, "at 720 lines the blur reaches what it did");
  Check(std::fabs(ReachIn720Pixels(360) - 2.0) < 1e-9, "at 360 lines (the low quality's image) the blur is the first version's");
  double lo = 1e9, hi = 0.0;
  for (uint32_t h = 540; h <= 4320; h += 3) {
    const double reach = ReachIn720Pixels(h);
    lo = std::min(lo, reach);
    hi = std::max(hi, reach);
  }
  if (g_print) {
    std::printf("the blur's reach in 720 line pixels over 540 to 4320 lines: %.2f to %.2f (the first version's 3x3: 2.00 at 360 lines, 1.00 at 720, 0.33 at 2160)\n", lo, hi);
  }
  Check(lo >= 0.5 - 1e-9 && hi <= 1.4 + 1e-9, Num("the reach stays between 0.5 and 1.4 (got %.3f to %.3f)", lo, hi));
  Check(ReachIn720Pixels(2160) > 2.9 * (double(kReferenceHeight) / 2160.0), "at 4K the blur reaches three times what the first version's 3x3 did");

  // The occlusion traced at half the picture's size above 1080 lines (ao_size.h, mcla_gpu_ao_half_above_1080): the radius is the traced
  // image's (ao.cpp FillPush takes it from plan.ao[1]), so the blur reaches what the picture's height says either way.
  {
    const struct {
      uint32_t picture;  // the render height
      int full_radius;   // the blur of the image at the picture's own size
      int half_radius;   // ... of the half size image
    } kHalf[] = {{1200, 2, 1}, {1440, 2, 1}, {1620, 2, 1}, {2160, 3, 2}, {2880, 3, 2}};
    for (const auto& row : kHalf) {
      const uint32_t half = (row.picture + 1) / 2;
      Check(DenoiseRadius(row.picture) == row.full_radius && DenoiseRadius(half) == row.half_radius,
            Num("a picture of %.0f lines: the blur is ", double(row.picture)) + Num("%.0f at the picture's size and ", double(row.full_radius)) +
                Num("%.0f traced at half size", double(row.half_radius)) + Num(" (got %.0f)", double(DenoiseRadius(half))));
      if (g_print) {
        std::printf("a picture of %u lines: blur %dx%d (reach %.2f of the 720 line picture's pixel) at full size, %dx%d (reach %.2f) traced at %u lines\n",
                    row.picture, 2 * DenoiseRadius(row.picture) + 1, 2 * DenoiseRadius(row.picture) + 1, ReachIn720Pixels(row.picture),
                    2 * DenoiseRadius(half) + 1, 2 * DenoiseRadius(half) + 1, ReachIn720Pixels(half), half);
      }
    }
    // 1440p traced at 720 lines has exactly the reach of the 1440p image's own blur; 4K's is a third wider (5x5 of 1080 lines against 7x7 of 2160).
    Check(std::fabs(ReachIn720Pixels(720) - ReachIn720Pixels(1440)) < 1e-9, "1440p: traced at half size the blur reaches what it did at full size");
    Check(std::fabs(ReachIn720Pixels(1080) / ReachIn720Pixels(2160) - 4.0 / 3.0) < 1e-9, "4K: traced at 1080 lines the blur reaches a third more than at 2160");
    // The wide blur (the pixels that have no history) follows: 7x7 at 1080 lines, 5x5 at 720.
    Check(WideRadius(DenoiseRadius(1080)) == 3 && WideRadius(DenoiseRadius(720)) == 2, "the wide blur of the half size images: 7x7 (4K) and 5x5 (1440p)");
  }

  // The wide blur: 2 at 720 lines (the first version's constant), one wider than the blur, never over kMaxWideRadius.
  Check(WideRadius(1) == 2 && WideRadius(2) == 3 && WideRadius(3) == 4, "the wide blur is one wider than the blur");
  Check(WideRadius(4) == kMaxWideRadius && WideRadius(0) == 2 && WideRadius(7) == kMaxWideRadius, "the wide blur is never over 4 and never under 2");
  for (uint32_t h = 100; h < 5000; h += 11) {
    if (WideRadius(DenoiseRadius(h)) <= DenoiseRadius(h)) {
      Check(false, Num("the wide blur is wider than the blur at %.0f lines", double(h)));
      break;
    }
  }
}

void CheckSigma() {
  // The first version's two sigmas are kept: the 3x3 and the 5x5 of a picture of 720 lines come out as they did.
  Check(BlurSigma(1) == 0.9f && BlurSigma(2) == 1.4f, "the 3x3's and 5x5's sigma are the first version's");
  Check(BlurSigma(1) < BlurSigma(2) && BlurSigma(2) < BlurSigma(3) && BlurSigma(3) < BlurSigma(4), "the sigma grows with the radius");
  // A gaussian cut off by the kernel's edge is a box: the weight at the edge (dx = r) against the centre stays between 0.15 and 0.6
  // (the 3x3's 0.54 and the 5x5's 0.36 as they were).
  for (int r = 1; r <= 4; ++r) {
    const double edge = std::exp(-0.5 * double(r * r) / double(BlurSigma(r) * BlurSigma(r)));
    if (g_print) {
      std::printf("radius %d: sigma %.2f, the weight at the kernel's edge %.2f of the centre's\n", r, double(BlurSigma(r)), edge);
    }
    Check(edge >= 0.15 && edge <= 0.6, Num("radius %.0f: the weight at the edge is %.2f, between 0.15 and 0.6", double(r), edge));
  }
}

// ---------------------------------------------------------------------------
// the noise and the history
// ---------------------------------------------------------------------------

// The widest gap of the circle [0, 1) between the sorted values (wrapping round).
double WidestGap(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  double gap = v.front() + 1.0 - v.back();
  for (size_t i = 1; i < v.size(); ++i) {
    gap = std::max(gap, v[i] - v[i - 1]);
  }
  return gap;
}

// The farthest any point of the unit square (on a 16x16 grid, the square a torus) is from one of `points`.
double FarthestPoint(const std::vector<Shift>& points) {
  double worst = 0.0;
  for (int gx = 0; gx < 16; ++gx) {
    for (int gy = 0; gy < 16; ++gy) {
      const double px = (gx + 0.5) / 16.0, py = (gy + 0.5) / 16.0;
      double nearest = 1e9;
      for (const Shift& s : points) {
        double dx = std::fabs(px - double(s.x));
        double dy = std::fabs(py - double(s.y));
        dx = std::min(dx, 1.0 - dx);
        dy = std::min(dy, 1.0 - dy);
        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
      }
      worst = std::max(worst, nearest);
    }
  }
  return worst;
}

void CheckNoise() {
  // The first version's increments are 1/g and 1/g^2 of the plastic number g (the real root of x^3 = x + 1).
  double g = 1.32471795724475;
  for (int i = 0; i < 100; ++i) {
    g = std::cbrt(g + 1.0);
  }
  Check(std::fabs(double(kR2A) - 1.0 / g) < 1e-7 && std::fabs(double(kR2B) - 1.0 / (g * g)) < 1e-7, "the first version's increments are the R2 sequence's");
  Check(std::fabs(double(kStepGolden) - (std::sqrt(5.0) - 1.0) / 2.0) < 1e-6, "the step offsets' increment is the golden ratio's");
  Check(kFrameWrap == 1023, "the frame number wraps at 1024");

  // The code the main pass gets: the phase with the flag, or the frame number.
  Check(NoiseCode(0, true) == (kLatticeFlag | 0u) && NoiseCode(13, true) == (kLatticeFlag | 0u) && NoiseCode(27, true) == (kLatticeFlag | 1u),
        "the lattice's code is the flag and the frame mod 13");
  Check(NoiseCode(5, false) == 5u && NoiseCode(1024 + 5, false) == 5u && (NoiseCode(1000000, false) & kLatticeFlag) == 0u,
        "the first version's code is the frame number mod 1024, without the flag");
  Check(NoiseCode(~0ull, true) <= (kLatticeFlag | kLatticePhaseMask) && (kLatticePeriod - 1) <= kLatticePhaseMask, "the phase fits its 12 bits");

  // The lattice: 13 distinct, evenly spaced values on each axis, every one of 13 consecutive frames once, the pairs the Fibonacci lattice.
  std::vector<double> xs;
  std::vector<double> ys;
  std::vector<Shift> all;
  bool in_range = true;
  for (uint32_t f = 0; f < kLatticePeriod; ++f) {
    const Shift s = DecodeShift(NoiseCode(f, true));
    xs.push_back(double(s.x));
    ys.push_back(double(s.y));
    all.push_back(s);
    in_range = in_range && s.x >= 0.0f && s.x < 1.0f && s.y >= 0.0f && s.y < 1.0f;
  }
  Check(in_range, "the shifts are in [0, 1)");
  std::vector<double> sx = xs, sy = ys;
  std::sort(sx.begin(), sx.end());
  std::sort(sy.begin(), sy.end());
  bool even = true;
  for (uint32_t i = 0; i < kLatticePeriod; ++i) {
    even = even && std::fabs(sx[i] - double(i) / double(kLatticePeriod)) < 1e-6 && std::fabs(sy[i] - double(i) / double(kLatticePeriod)) < 1e-6;
  }
  Check(even, "the 13 rotations and the 13 step offsets are 13 distinct, evenly spaced values (every k/13 once)");
  bool every_window = true;
  for (uint32_t start = 0; start < 200; start += 3) {
    std::vector<double> wx;
    for (uint32_t i = 0; i < kLatticePeriod; ++i) {
      wx.push_back(double(DecodeShift(NoiseCode(start + i, true)).x));
    }
    std::sort(wx.begin(), wx.end());
    for (uint32_t i = 0; i < kLatticePeriod; ++i) {
      every_window = every_window && std::fabs(wx[i] - double(i) / 13.0) < 1e-6;
    }
  }
  Check(every_window, "any 13 consecutive frames visit every rotation once");
  // The pairs: the Fibonacci lattice (n/13, 8n/13): the farthest point of the square from a pair is the best a lattice of 13 points gets.
  const double farthest = FarthestPoint(all);
  Check(farthest <= 0.19, Num("the 13 (rotation, step offset) pairs leave no point of the square farther than 0.19 (got %.3f)", farthest));

  // The history: with any starting frame, `n` consecutive frames of the shift leave no gap in the rotation wider than a share of
  // it, against the first version's recurrence (which the check also prints). The history averages 12 frames
  // (mcla_gpu_ao_history), the bounce light's 16. A window of 4 or 8 frames is not even for the lattice (the rotation goes
  // m/13, (m+1)/13, ...: a contiguous arc, 0.77 of a gap in 4 frames), and it still converges faster than the recurrence in the twin (the
  // error of the mean of the first 4, 8 and 12 frames after a cut, over five places: 0.5 to 0.7, 0.5 to 0.6 and 0.4 to 0.7 of the
  // recurrence's, ao_stable_noise_test.cpp): the pixels next to it have other offsets of the interleaved gradient noise and fill the
  // rotation in, and the lattice's pairs are the best of the 13-point lattices tried (search in ao_stable.h). So only the windows
  // from 12 frames on are held to a bound.
  const uint32_t windows[] = {4, 8, 12, 16};
  const double lattice_bounds[] = {1.0, 1.0, 0.16, 0.08};
  for (int w = 0; w < 4; ++w) {
    double worst_lattice = 0.0, worst_first = 0.0, worst_square_lattice = 0.0, worst_square_first = 0.0;
    for (uint32_t start = 0; start + windows[w] <= kFrameWrap + 1; start += 3) {
      std::vector<double> lx, fx;
      std::vector<Shift> lp, fp;
      for (uint32_t i = 0; i < windows[w]; ++i) {
        const Shift l = DecodeShift(NoiseCode(start + i, true));
        const Shift f = DecodeShift(NoiseCode(start + i, false));
        lx.push_back(double(l.x));
        fx.push_back(double(f.x));
        lp.push_back(l);
        fp.push_back(f);
      }
      worst_lattice = std::max(worst_lattice, WidestGap(lx));
      worst_first = std::max(worst_first, WidestGap(fx));
      if (start % 24 == 0) {
        worst_square_lattice = std::max(worst_square_lattice, FarthestPoint(lp));
        worst_square_first = std::max(worst_square_first, FarthestPoint(fp));
      }
    }
    if (g_print) {
      std::printf("%2u consecutive frames: widest gap of the slice rotation %.3f (first version %.3f), farthest point of the square from a pair %.3f (first version %.3f)\n",
                  windows[w], worst_lattice, worst_first, worst_square_lattice, worst_square_first);
    }
    Check(worst_lattice <= lattice_bounds[w] + 1e-9, Num("%.0f consecutive frames leave no gap over", double(windows[w])) + Num(" %.2f of the rotation (got %.3f)", lattice_bounds[w], worst_lattice));
    if (windows[w] >= 12) {
      Check(worst_lattice < 0.75 * worst_first, Num("%.0f frames: the lattice's widest gap is under 0.75 of the first version's", double(windows[w])) + Num(" (%.3f against %.3f)", worst_lattice, worst_first));
    }
  }
}

void CheckHistory() {
  // The history weight: the plain mean of the frames so far (1/2, 2/3, 3/4, ...), then 1 - 1/cap.
  Check(std::fabs(HistoryWeight(0, 12) - 0.5f) < 1e-6f && std::fabs(HistoryWeight(1, 12) - 2.0f / 3.0f) < 1e-6f &&
            std::fabs(HistoryWeight(2, 12) - 0.75f) < 1e-6f,
        "the first history frames are the plain mean");
  Check(std::fabs(HistoryWeight(10, 12) - (1.0f - 1.0f / 12.0f)) < 1e-6f && std::fabs(HistoryWeight(500, 12) - (1.0f - 1.0f / 12.0f)) < 1e-6f,
        "from the 12th frame on the weight is 1 - 1/12");
  bool monotone = true;
  for (uint32_t age = 0; age < 40; ++age) {
    monotone = monotone && HistoryWeight(age + 1, 16) >= HistoryWeight(age, 16) && HistoryWeight(age, 16) < 1.0f;
  }
  Check(monotone, "the weight never falls with the age and is under 1");
  Check(HistoryWeight(5, 1) == 0.0f && HistoryWeight(0, 0) == 0.0f, "a cap of 1 (or none) means no history");
  // The mean of the frames so far is what the weight makes: the cumulative mean of n frames, whatever the values.
  float mean = 0.0f;
  float sum = 0.0f;
  const float values[] = {0.3f, 0.9f, 0.5f, 0.7f, 0.1f, 0.6f, 0.8f, 0.4f};
  for (uint32_t i = 0; i < 8; ++i) {
    mean = i == 0 ? values[0] : values[i] + (mean - values[i]) * HistoryWeight(i - 1, 64);
    sum += values[i];
  }
  Check(std::fabs(mean - sum / 8.0f) < 1e-5f, "the weights make the cumulative mean while the age is under the cap");
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
  const std::string common = Slurp(gpu_dir + "/hlsl/ao_common.hlsli");
  const std::string main_ps = Slurp(gpu_dir + "/hlsl/ao_main_ps.hlsl");
  const std::string temporal = Slurp(gpu_dir + "/hlsl/ao_temporal_ps.hlsl");
  const std::string denoise = Slurp(gpu_dir + "/hlsl/ao_denoise_ps.hlsl");
  const std::string cpp = Slurp(gpu_dir + "/ao.cpp");
  Check(!common.empty() && !main_ps.empty() && !temporal.empty() && !denoise.empty() && !cpp.empty(), "the shaders and ao.cpp are readable from " + gpu_dir);
  if (common.empty() || main_ps.empty() || temporal.empty() || denoise.empty() || cpp.empty()) {
    return;
  }
  auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
  // The sigma: the one function, with the header's four numbers.
  char sigma_text[200];
  std::snprintf(sigma_text, sizeof(sigma_text), "return radius >= 4 ? %.1ff : (radius == 3 ? %.1ff : (radius == 2 ? %.1ff : %.1ff));", double(BlurSigma(4)),
                double(BlurSigma(3)), double(BlurSigma(2)), double(BlurSigma(1)));
  const std::string sigma = sigma_text;
  Check(has(common, sigma), "ao_common.hlsli: AoBlurSigma is the header's BlurSigma");
  Check(has(common, "const float spatial_sigma = AoBlurSigma(radius);"), "ao_common.hlsli: the blur takes its sigma from AoBlurSigma");
  // The wide blur: one wider than the denoise radius, at most 4.
  Check(has(common, Num("int AoWideRadius() { return min(max(AoDenoiseRadius(), %.0f) + 1, %.0f); }", double(kMinRadius), double(kMaxWideRadius))),
        "ao_common.hlsli: AoWideRadius is the header's WideRadius");
  Check(has(temporal, "normal, AoWideRadius());"), "ao_temporal_ps.hlsl: the wide blur takes its radius from AoWideRadius");
  // The denoise pass reads the radius the push block carries; the radius is in bits 17-19, 3 bits: the largest the header gives fits.
  Check(has(denoise, "AoDenoiseRadius());") && has(common, "(g_push.slots2.z >> 17) & 0x7u") && kMaxRadius <= 7, "the denoise radius travels in the push block's bits 17-19");
  // The noise's shift: one function, the header's lattice and the first version's recurrence, used by both main passes.
  Check(has(common, "static const uint kAoNoiseLatticeFlag = 1u << 30;") && kLatticeFlag == (1u << 30), "ao_common.hlsli: the lattice's flag is bit 30");
  Check(has(common, "const uint m = code & 0xFFFu;") && kLatticePhaseMask == 0xFFFu, "ao_common.hlsli: the phase is in the low 12 bits");
  Check(has(common, "return float2(float(m), float((m * 8u) % 13u)) * (1.0f / 13.0f);") && kLatticePeriod == 13 && kLatticeStepY == 8,
        "ao_common.hlsli: the lattice is (m / 13, (8 m mod 13) / 13)");
  Check(has(common, "return float(code & 1023u) * float2(0.75487766624669f, 0.56984029099806f);") && kFrameWrap == 1023, "ao_common.hlsli: the first version's recurrence is the header's R2 increments");
  Check(has(main_ps, "const float2 noise = frac(spatial + AoNoiseShift());"), "ao_main_ps.hlsl: the noise takes its shift from AoNoiseShift");
  Check(has(main_ps, "0.6180339887f"), "ao_main_ps.hlsl: the step offsets' increment is the header's golden ratio");
  // ao.cpp: the radius from the header, the switch, the frame number.
  Check(has(cpp, "ao_stable::DenoiseRadius(plan.ao[1])") && has(cpp, "mcla_gpu_ao_stable"), "ao.cpp: the denoise radius comes from ao_stable.h, switchable by mcla_gpu_ao_stable");
  Check(has(cpp, "ao_stable::NoiseCode(frame_, REXCVAR_GET(mcla_gpu_ao_stable))"), "ao.cpp: the main pass's noise code comes from ao_stable.h, switchable by mcla_gpu_ao_stable");
  Check(has(cpp, "push->slots2[1] = uint32_t(frame_ & 1023);"), "ao.cpp: the other passes keep the frame number in slots2.y (the contact shadows' noise)");
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
  CheckRadius();
  CheckSigma();
  CheckNoise();
  CheckHistory();
  if (!gpu_dir.empty()) {
    CheckShaders(gpu_dir);
  }
  std::printf("ao_stable_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
