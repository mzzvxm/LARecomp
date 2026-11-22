// Check of the shared per-frame blue noise (hlsl/blue_noise_frame.hlsli: the blue noise tile stepped along the R2 sequence; NOISE-AUDIT, perf\NOISE-AUDIT.md) and of the debug setting that switches the
// other stochastic passes between it and the first versions' gradient noise (mcla_gpu_noise_blue). A program of its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_noise_frame_test, then run src/gpu/larecomp_noise_frame_test [<src/gpu dir>] (a second or two).
//
// It compiles the .hlsli as C++ (hlsl_shim.h) and checks, through noise_resolve_check.h: the numbers are uniform; they change at every pixel every frame; a pixel's numbers over 16 frames leave no
// gap over 0.25 on the circle; the step from one frame to the next is the same for every pixel (the picture is the tile moved round the circle: nothing slides); after the exponential average
// the error has no lattice (and the first version's gradient noise, with the same average, has); a long session (a frame count of 2^31 and over) keeps the numbers uniform; and the setting is in
// the settings header, its alias, the manifest, the meta list and the debug menu's FIXES page.

#include <cstdint>
#include <cstdio>
#include <string>

#include "hlsl_shim.h"
#include "noise_resolve_check.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/blue_noise_frame.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;

namespace {

nc::Checks g_check;

// The first versions' noise: interleaved gradient noise of the pixel, plus the frame's R2 step added to it (the occlusion's, the reflections' cone rotation, the sun rays').
std::pair<float, float> Legacy(int x, int y, uint32_t frame) {
  const float f = float(frame & 1023u);
  const double a = blue_noise_spectrum::Ign(double(x), double(y)), b = blue_noise_spectrum::Ign(double(y) + 31.0, double(x) + 11.0);
  return {float(std::fmod(a + double(f) * 0.75487766624669, 1.0)), float(std::fmod(b + double(f) * 0.56984029099806, 1.0))};
}

std::pair<float, float> Blue(int x, int y, uint32_t frame) {
  const hs::float2 u = hs::BlueNoiseFrame2(x, y, frame);
  return {u.x, u.y};
}

// A smooth periodic response to the two numbers: a brightness against the lobe's sample, an occlusion against the slice's direction.
double Response(float u1, float u2) { return std::cos(6.2831853 * double(u1) + 0.7) + 0.6 * std::cos(6.2831853 * double(u2)); }

void TestPerFrame() {
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(Blue, channel);
    if (g_check.print) {
      nc::Print(channel == 0 ? "blue, number 1" : "blue, number 2", p);
    }
    g_check(p.changed > 0.99, nc::Num("number %d changes at every pixel every frame (%.3f of the pairs differ)", channel + 1, p.changed));
    g_check(p.max_gap < 0.25, nc::Num("number %d: a pixel's 16 frames leave no gap over 0.25 on the circle (%.3f)", channel + 1, p.max_gap));
    g_check(std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004, nc::Num("number %d is uniform on 0..1 (mean %.3f, variance %.4f)", channel + 1, p.mean, p.var));
    g_check(p.step_spread < 1e-4, nc::Num("number %d: every pixel takes the same step from one frame to the next, the picture moves round the circle and nothing slides (spread %.6f)", channel + 1,
                                           p.step_spread));
  }
}

void TestLattice() {
  // the exponential average alone (the reflections' temporal pass, the history of any effect): the first version's error is a lattice, the blue noise's has no frequency that stands out
  const nc::Resolved legacy = nc::Resolve(Legacy, Response, 100, 128, 48, 0.9, false);
  const nc::Resolved blue = nc::Resolve(Blue, Response, 100, 128, 48, 0.9, false);
  if (g_check.print) {
    nc::Print("first version", legacy);
    nc::Print("blue noise", blue);
  }
  g_check(nc::IsLattice(legacy), nc::Num("the first version's error is a lattice (%.3f of it in one frequency, a direction %.1f times the mean)", legacy.shape.peak_share, legacy.shape.wedge_ratio));
  g_check(nc::NoLattice(blue), nc::Num("blue: the error after the average has no frequency that stands out (%.4f) and no direction (%.2f)", blue.shape.peak_share, blue.shape.wedge_ratio));
  g_check(blue.best_corr < 0.35, nc::Num("blue: the error does not crawl (best match with the next frame %.2f)", blue.best_corr));
  g_check(legacy.best_corr > 0.9 && (legacy.best_dx != 0 || legacy.best_dy != 0), nc::Num("(the first version's error crawls: the next frame matches this one moved by (%d, %d) at %.2f)", legacy.best_dx, legacy.best_dy, legacy.best_corr));
  g_check(blue.rms < 1.3 * legacy.rms, nc::Num("blue: the error is within 1.3 times the first version's (%.4f against %.4f)", blue.rms, legacy.rms));
  // after a 3x3 blur too the pattern does not move and the numbers are small (a few thousandths of the response's amplitude of 1.6)
  const nc::Resolved blurred = nc::Resolve(Blue, Response, 100, 128, 48, 0.9, true);
  if (g_check.print) {
    nc::Print("blue noise, 3x3 blur", blurred);
  }
  g_check(blurred.best_corr < 0.35 && blurred.rms < 0.02, nc::Num("blue with the 3x3 blur: does not crawl (%.2f) and the error is small (rms %.4f)", blurred.best_corr, blurred.rms));
}

void TestLongSession() {
  // 2^31 frames and over (a long session, a wrapped counter): the product wraps in 32 bit integers, so the numbers stay uniform and keep stepping
  double sum = 0.0, sum2 = 0.0;
  int n = 0, same = 0;
  for (uint32_t base : {0x7FFFFF00u, 0xFFFFFF00u, 0x40000000u}) {
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 32; ++x) {
        const hs::float2 a = hs::BlueNoiseFrame2(x, y, base + 7u), b = hs::BlueNoiseFrame2(x, y, base + 8u);
        sum += a.x;
        sum2 += double(a.x) * double(a.x);
        ++n;
        same += (a.x == b.x) ? 1 : 0;
        if (a.x < 0.0f || a.x >= 1.0f || a.y < 0.0f || a.y >= 1.0f) {
          ++same;
        }
      }
    }
  }
  const double mean = sum / n, var = sum2 / n - mean * mean;
  g_check(same == 0 && std::fabs(mean - 0.5) < 0.01 && std::fabs(var - 1.0 / 12.0) < 0.004, nc::Num("frame counts over 2^30: uniform on [0, 1) (mean %.3f, variance %.4f) and still stepping", mean, var));
  // the two numbers of a pixel are two blue noises: uncorrelated over the picture
  double sxy = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
  int m = 0;
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      const hs::float2 u = hs::BlueNoiseFrame2(x, y, 5u);
      sx += u.x;
      sy += u.y;
      sxx += double(u.x) * u.x;
      syy += double(u.y) * u.y;
      sxy += double(u.x) * u.y;
      ++m;
    }
  }
  const double c = (sxy / m - sx / m * sy / m) / std::sqrt((sxx / m - sx / m * sx / m) * (syy / m - sy / m * sy / m));
  g_check(std::fabs(c) < 0.1, nc::Num("the two numbers of a pixel are uncorrelated (%.3f)", c));
  g_check(hs::kBlueNoiseR2StepA == uint32_t(0.7548776662466927 * 4294967296.0) && hs::kBlueNoiseR2StepB == uint32_t(0.5698402909980532 * 4294967296.0),
          "the steps are 1/g and 1/g^2 of the plastic number as 32 bit fractions (ssr_common.hlsli's kSsrR2StepA and B)");
}

void TestSetting(const std::string& dir) {
  const std::string settings = nc::ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = nc::ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = nc::ReadFile(dir + "/../settings/generated/settings_meta.inc");
  const std::string pages = nc::ReadFile(dir + "/../mc_engine/menus/debug_menu_pages.cpp");
  if (settings.empty() || manifest.empty() || meta.empty() || pages.empty()) {
    g_check(false, "the settings files could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  const std::string decl = std::string("REX_") + "SETTING(bool, mcla_gpu_noise_blue, true,";
  const size_t at = settings.find(decl);
  const size_t end = at == std::string::npos ? at : settings.find(");", at);
  g_check(at != std::string::npos && end != std::string::npos && settings.substr(at, end - at).find("rex::kDebugOnly") != std::string::npos &&
              nc::Has(settings, "inline auto& mcla_gpu_noise_blue = g_gpu.mcla_gpu_noise_blue;") && nc::Has(manifest, "\"mcla_gpu_noise_blue\": {") && nc::Has(meta, "{\"mcla_gpu_noise_blue\", \"MCLA/GPU\"},"),
          "mcla_gpu_noise_blue: a debug setting, on by default, in the settings header, its alias, the manifest and the meta list");
  g_check(nc::Has(pages, "NativeOnly(Bool(\"PM_RxDbgNoiseBlue\", \"mcla_gpu_noise_blue\""), "the debug menu's FIXES page has its row (live: the passes read it at each draw)");
  const std::string frame = nc::ReadFile(dir + "/hlsl/blue_noise_frame.hlsli");
  g_check(nc::Has(frame, "#include \"blue_noise.hlsli\"") && nc::Has(frame, "float2 BlueNoiseFrame2(int px, int py, uint sample_index)"), "the shared helper is in hlsl/blue_noise_frame.hlsli");
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
  TestPerFrame();
  TestLattice();
  TestLongSession();
  TestSetting(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
