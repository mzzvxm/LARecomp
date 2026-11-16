// Check of the HDR frame's compose in the present (hlsl/hdr_compose.hlsli, present_ps.hlsl's
// kComposeLayers, hdr_scene.h). A program of its own: it needs no renderer, no ReXGlue and no GPU,
// and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_hdr_compose_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_hdr_compose_test
//   (or: clang++ -std=c++20 -O1 hdr_compose_test.cpp -o hdr_compose_test.exe)
//
// What it holds: the present used to read a float16 image the compose pass wrote
// (frame_layers_ps.hlsl, LAYERS_COMPOSE=1) and now, with mcla_gpu_hdr_compose_in_present, takes the
// same sum from the two layers itself. The sum is ONE function (hdr_compose.hlsli, included here as it
// is: its scalar half is plain C++) used by both shaders, and this file is the twin of what is around
// it in each path -- the float16 store and read of the image, the display table's three rules
// (present_ps.hlsl Ramp) and the texel's coordinates with the gap -- so that "the composed result
// equals today's" is checked over every 8-bit blend and a sweep of the float scene:
//   * the two paths give the very same colour wherever the sum is exact in float16 (no UI over the
//     scene, a UI that covers it, the float scene's usual values), and the same to the float16
//     rounding of the image (a half unit in the last place of the sum, carried through the table) for
//     the rest: the partly covered pixels of a UI layer, where the image's rounding of the sum moved
//     the nearest entry of the table by one in the older path;
//   * the sum is finite, never negative and never above 65504 (the table's index), whatever the
//     layers hold (NaN, infinity, negative);
//   * the texels read are the same: the gap clamp of the image path (compose reads at max(position,
//     gap), the present at clamp(at, 0, limit)) and of the layers (clamp(at, gap, limit)) agree at
//     every position, the edges and the neighbours past them included.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

// The HLSL intrinsics the scalar half of hdr_compose.hlsli uses.
using uint = std::uint32_t;
inline std::uint32_t asuint(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float clamp(float x, float lo, float hi) {
  return std::min(std::max(x, lo), hi);
}
#include "hlsl/hdr_compose.hlsli"

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// ---- the float16 image the compose pass writes -------------------------------------------------
// The store into R16G16B16A16_FLOAT: round to nearest even, for a value in [0, 65504].
float RoundToHalf(float f) {
  if (f == 0.0f) {
    return 0.0f;
  }
  int e = 0;
  std::frexp(f, &e);                       // f = m x 2^e, m in [0.5, 1): the value's exponent is e - 1
  const int exponent = std::max(e - 1, -14);   // below 2^-14 the half is subnormal: a step of 2^-24
  const double spacing = std::ldexp(1.0, exponent - 10);
  return float(std::nearbyint(double(f) / spacing) * spacing);
}

// ---- the display's table (present_ps.hlsl Ramp) ------------------------------------------------
// 256 entries a channel, 10 bits over 1023, made as the device's table is (monotone, not even steps).
struct Table {
  float v[256][3];
};
Table MakeTable() {
  Table t{};
  const double gamma[3] = {1.0 / 1.09, 1.0 / 1.12, 1.0 / 1.15};
  for (int i = 0; i < 256; ++i) {
    for (int c = 0; c < 3; ++c) {
      t.v[i][c] = float(std::lround(1023.0 * std::pow(i / 255.0, gamma[c]))) / 1023.0f;
    }
  }
  return t;
}
float MaxStep(const Table& t, int c) {
  float m = 0.0f;
  for (int i = 0; i < 255; ++i) {
    m = std::max(m, t.v[i + 1][c] - t.v[i][c]);
  }
  return m;
}

enum class Codes { kPlain, kExtended, kInterpolated };

// present_ps.hlsl Ramp for one channel, the three rules of hdr_flags (none, kExtendedCodes,
// kExtendedCodes | kInterpolatedCodes).
float Ramp(const Table& t, int c, float color, Codes mode) {
  if (mode == Codes::kInterpolated) {
    const float scaled = std::max(color, 0.0f) * 255.0f;
    const std::uint32_t index = std::uint32_t(std::min(std::floor(scaled), 254.0f));
    const float fraction = scaled - float(index);
    const float low = t.v[index][c];
    const float high = t.v[index + 1][c];
    return low + (high - low) * fraction;
  }
  if (mode == Codes::kExtended) {
    const float scaled = std::max(color, 0.0f) * 255.0f;
    float value = t.v[std::uint32_t(std::min(scaled, 255.0f) + 0.5f)][c];
    const float excess = std::max(scaled - 255.0f, 0.0f);
    if (excess > 0.0f) {
      value += (t.v[255][c] - t.v[254][c]) * excess;
    }
    return value;
  }
  return t.v[std::uint32_t(std::min(std::max(color, 0.0f), 1.0f) * 255.0f + 0.5f)][c];
}

// What the display shows for a texel: the older path reads the float16 image of the sum, the new one
// takes the sum from the layers in the same pass.
float Today(const Table& t, int c, float scene, float ui, float coverage, Codes mode) {
  return Ramp(t, c, RoundToHalf(ComposeCode(scene, ui, coverage)), mode);
}
float Inline(const Table& t, int c, float scene, float ui, float coverage, Codes mode) {
  return Ramp(t, c, ComposeCode(scene, ui, coverage), mode);
}

// ---- the sum ----------------------------------------------------------------------------------
void CheckSum() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // The UI over the scene is the game's blend: premultiplied colour plus the scene times what the
  // coverage lets through.
  Check(ComposeCode(0.5f, 0.0f, 0.0f) == 0.5f, "no UI: the scene as it is");
  Check(ComposeCode(0.5f, 0.25f, 1.0f) == 0.25f, "a covering UI: its own colour");
  Check(ComposeCode(0.5f, 0.125f, 0.5f) == 0.375f, "half covered: ui + scene x (1 - coverage)");
  Check(ComposeCode(4.0f, 0.0f, 0.0f) == 4.0f, "the float scene's light above 1 is kept");
  Check(ComposeCode(4.0f, 0.5f, 0.5f) == 2.5f, "... and shows through the UI by what the coverage lets");
  // Whatever the layers hold, the table's index is finite, not negative and at most 65504.
  const float odd[] = {nan, inf, -inf, -1.0f, -1e30f, 1e30f, 65504.0f, 65505.0f, 0.0f, -0.0f};
  bool all_ok = true;
  for (float s : odd) {
    for (float u : odd) {
      for (float a : {0.0f, 0.5f, 1.0f, nan, inf, -1.0f}) {
        const float code = ComposeCode(s, u, a);
        all_ok &= std::isfinite(code) && code >= 0.0f && code <= 65504.0f;
      }
    }
  }
  Check(all_ok, "NaN, infinity and negative inputs give a finite code in [0, 65504]");
  Check(ComposeCode(nan, 0.0f, 0.0f) == 0.0f, "a NaN scene is none");
  Check(ComposeCode(inf, 0.0f, 0.0f) == 65504.0f, "an infinite scene is the float16 maximum");
  Check(ComposeCode(-2.0f, 0.0f, 0.0f) == 0.0f, "a negative sum is zero");
  // The sum of the compose pass and the present's is the same function: the present's expression
  // per channel against the older pass's code written out (one line of frame_layers_ps.hlsl before
  // the shared function), over every 8-bit UI blend.
  bool same = true;
  for (int s = 0; s < 256 && same; s += 5) {
    for (int a = 0; a < 256 && same; ++a) {
      for (int w = 0; w <= a && same; w += 3) {
        const float scene = float(s) / 255.0f, ui = float(w) / 255.0f, cov = float(a) / 255.0f;
        const float code = ui + scene * (1.0f - cov);
        const std::uint32_t bits = asuint(code) & 0x7FFFFFFFu;
        const float old_code = (bits <= 0x7F800000u) ? std::min(std::max(code, 0.0f), 65504.0f) : 0.0f;
        same &= ComposeCode(scene, ui, cov) == old_code;
      }
    }
  }
  Check(same, "the shared sum is the older compose pass's, bit for bit, over the 8-bit blends");
}

// ---- the colour the display gets: today's path against the new one ------------------------------
void CheckColour() {
  const Table table = MakeTable();
  // (1) The 8-bit scene (HDR stage 2's underlay, the menus' frames) under every 8-bit UI blend:
  // nearest entry of the table. The older path's float16 image moved the sum by at most half a unit
  // of the float16 grid (2^-12 of the value, 0.06 of a code near white): equal wherever the sum is
  // exact, and a neighbouring entry where it lands on the rounding point of the table's index.
  for (int c = 0; c < 3; ++c) {
    const float step = MaxStep(table, c);
    long total = 0, differ = 0, exact_total = 0, exact_differ = 0;
    float worst = 0.0f;
    for (int s = 0; s < 256; ++s) {
      for (int a = 0; a < 256; ++a) {
        for (int w = 0; w <= a; ++w) {
          const float scene = float(s) / 255.0f, ui = float(w) / 255.0f, cov = float(a) / 255.0f;
          const float code = ComposeCode(scene, ui, cov);
          const float d = std::fabs(Today(table, c, scene, ui, cov, Codes::kExtended) -
                                    Inline(table, c, scene, ui, cov, Codes::kExtended));
          ++total;
          differ += d != 0.0f;
          worst = std::max(worst, d);
          if (RoundToHalf(code) == code) {  // the sum is exact in float16: nothing for the image to move
            ++exact_total;
            exact_differ += d != 0.0f;
          }
        }
      }
    }
    Check(exact_differ == 0, "8-bit scene, channel " + std::to_string(c) +
                                 ": a sum exact in float16 gives the same colour in both paths (" +
                                 std::to_string(exact_differ) + " of " + std::to_string(exact_total) +
                                 " differ)");
    Check(worst <= step * 1.0001f, "8-bit scene, channel " + std::to_string(c) +
                                       ": the paths differ by at most one step of the table (worst " +
                                       std::to_string(worst * 1023.0f) + " of " +
                                       std::to_string(step * 1023.0f) + " in 10-bit codes)");
    std::printf("  8-bit scene, channel %d: %ld of %ld blends differ (%.2f %%), worst %.1f 10-bit codes\n", c,
                differ, total, 100.0 * double(differ) / double(total), double(worst) * 1023.0);
    // No UI over the scene (coverage 0, nothing drawn): never a difference, the picture of the 3D
    // scene is the same.
    long no_ui_differ = 0;
    for (int s = 0; s < 256; ++s) {
      const float scene = float(s) / 255.0f;
      no_ui_differ += Today(table, c, scene, 0.0f, 0.0f, Codes::kExtended) !=
                      Inline(table, c, scene, 0.0f, 0.0f, Codes::kExtended);
    }
    Check(no_ui_differ == 0, "8-bit scene, channel " + std::to_string(c) + ": no UI, every scene value the same");
  }
  // (2) The float scene (stage 3: float16 values above 1, read between the table's entries): the
  // image's rounding moves the code by at most 2^-11 of it, so the colour by at most that times the
  // table's slope (a step per code) -- and a scene value that is itself a float16, with no UI over
  // it, comes out the same to the bit.
  for (int c = 0; c < 3; ++c) {
    const float step = MaxStep(table, c);
    float worst_ratio = 0.0f;
    long no_ui_differ = 0, total = 0;
    for (int k = 0; k < 6000; ++k) {
      const float scene = RoundToHalf(float(k) * 0.0035f + 0.0001f * float(k % 7));   // 0 .. 21, in float16
      no_ui_differ += Today(table, c, scene, 0.0f, 0.0f, Codes::kInterpolated) !=
                      Inline(table, c, scene, 0.0f, 0.0f, Codes::kInterpolated);
      for (int a = 0; a < 256; a += 17) {
        for (int w = 0; w <= a; w += 13) {
          const float ui = float(w) / 255.0f, cov = float(a) / 255.0f;
          const float code = ComposeCode(scene, ui, cov);
          const float d = std::fabs(Today(table, c, scene, ui, cov, Codes::kInterpolated) -
                                    Inline(table, c, scene, ui, cov, Codes::kInterpolated));
          // the bound: a half unit of the float16 grid at the code, in codes (x255), times the
          // steepest step of the table
          const float bound = (code * 255.0f) * (1.0f / 2048.0f) * step + 1e-6f;
          worst_ratio = std::max(worst_ratio, d / bound);
          ++total;
        }
      }
    }
    Check(no_ui_differ == 0, "float scene, channel " + std::to_string(c) + ": no UI over a float16 scene is the same to the bit");
    Check(worst_ratio <= 1.0f, "float scene, channel " + std::to_string(c) +
                                   ": the paths differ by at most the float16 rounding of the sum through the table (worst " +
                                   std::to_string(worst_ratio) + " of the bound, " + std::to_string(total) + " blends)");
  }
}

// ---- the texels the two paths read --------------------------------------------------------------
void CheckGap() {
  // The compose pass writes pixel (x, y) from the layers' texel at max((x, y), gap); the present
  // reads that image at clamp(at, 0, limit) (a whole image: gap 0). The inline present reads the
  // layers at clamp(at, gap, limit). The same texel for every position, edges and neighbours past
  // them included.
  bool same = true;
  for (int size : {1, 2, 5, 16}) {
    const int limit = size - 1;
    for (int gap = 0; gap <= std::min(2, limit); ++gap) {
      for (int at = -3; at <= size + 2; ++at) {
        const int image_texel = std::min(std::max(at, 0), limit);         // the present's read of the image
        const int via_image = std::max(image_texel, gap);                 // the compose pass's read of the layers
        const int via_layers = std::min(std::max(at, gap), limit);        // the inline present's read
        same &= via_image == via_layers;
      }
    }
  }
  Check(same, "the layers' texel read is the same through the image and in place, gap and edges included");
}

}  // namespace

int main() {
  std::printf("hdr compose in the present\n");
  CheckSum();
  CheckColour();
  CheckGap();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
