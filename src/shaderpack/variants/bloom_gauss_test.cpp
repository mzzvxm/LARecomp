// Check of the bloom fix's variant (bloom_gauss.h): the text transform on small fixtures, and the proof that at S = 3 the two
// bilinear fetches of the helper give the same weighted sum as the three sub-taps they replace. A program of its own: it needs no
// renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_bloom_gauss_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_bloom_gauss_test [<folder of translated HLSL> [<folder for the results>]]
//
// THE EQUALITY. The helper (bloom_gauss.cpp kHelper) is modelled here line by line, twice: `fast` false is the helper as it was
// (n = ceil(S - 0.01) sub-taps, each a bilinear fetch of the sampler, 1 / n of the weight) and `fast` true is the helper now (at
// S = 3 within 0.001: two fetches at i - 1 + 1 / (2 - f) and i + 1 + f / (1 + f), weights (2 - f) / 3 and (1 + f) / 3). The
// sampler is a bilinear sampler with clamp-to-edge: a one-channel image, the fetch at a position in texels (texel i's centre is
// i + 0.5) takes the texels i and i + 1 of floor(position - 0.5) weighted by the fraction, a texel outside the image is the
// nearest one in it. The two models are compared
//   * on random images, on both axes, at S = 2 and S = 3, with the game's offsets -4..4, at every texel centre and at many
//     fractional positions, inside the image and up to four texels beyond its edge (so the clamp is in play), in double (the
//     identity itself: the difference is a rounding error of 1e-15) and in float (what the shader computes);
//   * on impulse images: the weights the two fetches put on the texels are exactly (1 - f, 1, 1, f) / 3;
//   * on a model of the hardware's filter, whose fraction has 8 bits: at f = 0 (a pixel-aligned tap, which is what the bloom's
//     passes make at 4K) both are exact and equal; at other f the two forms may differ by the rounding of the fractions.
// At S = 2 the helper makes two sub-taps both before and now (the same two fetches, the same number: nothing to gain); every other
// scale takes its own n sub-taps, unchanged.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/bloom_gauss.h"

namespace sp = larecomp::shaderpack;

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

size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// ---------------------------------------------------------------------------
// The model of the sampler and of the helper
// ---------------------------------------------------------------------------

struct Image {
  int w = 0;
  int h = 0;
  std::vector<double> v;
  double At(long x, long y) const {
    x = std::clamp<long>(x, 0, w - 1);  // the sampler's clamp-to-edge: the nearest texel of the image
    y = std::clamp<long>(y, 0, h - 1);
    return v[size_t(y) * size_t(w) + size_t(x)];
  }
};

uint32_t g_seed = 12345;
double Random() {  // 0 to 1
  g_seed = g_seed * 1664525u + 1013904223u;
  return double(g_seed >> 8) / double(1u << 24);
}

Image MakeImage(int w, int h) {
  Image image;
  image.w = w;
  image.h = h;
  image.v.resize(size_t(w) * size_t(h));
  for (double& value : image.v) value = Random();
  return image;
}

long g_fetches = 0;

// One bilinear fetch at the position (px, py) in texels. `bits` 0: exact weights; else the fractions are rounded to 1 / 2^bits
// (the hardware's filter).
template <class R>
R Bilinear(const Image& image, R px, R py, int bits) {
  ++g_fetches;
  const R fx = px - R(0.5);
  const R fy = py - R(0.5);
  const R ix = std::floor(fx);
  const R iy = std::floor(fy);
  R ax = fx - ix;
  R ay = fy - iy;
  if (bits > 0) {
    const R q = R(1u << bits);
    ax = std::floor(ax * q + R(0.5)) / q;
    ay = std::floor(ay * q + R(0.5)) / q;
  }
  const long x0 = long(ix);
  const long y0 = long(iy);
  const R t00 = R(image.At(x0, y0));
  const R t10 = R(image.At(x0 + 1, y0));
  const R t01 = R(image.At(x0, y0 + 1));
  const R t11 = R(image.At(x0 + 1, y0 + 1));
  return (t00 * (R(1) - ax) + t10 * ax) * (R(1) - ay) + (t01 * (R(1) - ax) + t11 * ax) * ay;
}

// bloom_gauss.cpp's GaussTap, for the tap of the game's offset `off` along `axis` (0: x, 1: y) of the pixel whose texCoord * size
// is (bx, by). `fast` false: the helper as it was; true: as it is now (the HLSL's text is checked below to hold these formulas).
template <class R>
R Tap(const Image& image, int axis, R bx, R by, R off, R scale, bool fast, int bits) {
  auto fetch = [&](R along) {
    R x = bx;
    R y = by;
    (axis == 0 ? x : y) += along;
    return Bilinear<R>(image, x, y, bits);
  };
  if (scale > R(0.99) && scale < R(1.01)) {
    return fetch(off);
  }
  const int count = std::clamp(int(std::ceil(scale - R(0.01))), 1, 8);
  if (count == 1) {
    return fetch(off * scale);
  }
  if (fast && count == 3 && std::fabs(scale - R(3.0)) < R(0.001)) {
    const R at = (axis == 0 ? bx : by) + off * scale;     // getWeights2D(.., texCoord, offset * scale): frac(position - 0.5)
    const R phase = (at - R(0.5)) - std::floor(at - R(0.5));
    const R first = fetch(off * scale + (R(1.0) / (R(2.0) - phase) - phase - R(1.0)));
    const R second = fetch(off * scale + (phase / (R(1.0) + phase) - phase + R(1.0)));
    return first * ((R(2.0) - phase) * (R(1.0) / R(3.0))) + second * ((R(1.0) + phase) * (R(1.0) / R(3.0)));
  }
  const R spacing = scale / R(count);
  R sum = R(0);
  for (int j = 0; j < count; ++j) {
    const R along = (R(j) - R(0.5) * R(count - 1)) * spacing;
    sum += fetch(off * scale + along);
  }
  return sum * (R(1.0) / R(count));
}

struct Stats {
  double max_diff = 0.0;
  long taps = 0;
  long same_bits = 0;
  long old_fetches = 0;
  long new_fetches = 0;
};

// Old against new over images, axes, the game's offsets and a grid of positions that runs four texels beyond the edges.
template <class R>
Stats Compare(double scale, int bits, bool aligned_only) {
  Stats stats;
  const int kSizes[][2] = {{1, 1}, {1, 7}, {2, 2}, {3, 5}, {12, 9}, {40, 23}, {7, 1}};
  for (const auto& size : kSizes) {
    const Image image = MakeImage(size[0], size[1]);
    for (int axis = 0; axis < 2; ++axis) {
      const int along_size = axis == 0 ? image.w : image.h;
      const int other_size = axis == 0 ? image.h : image.w;
      std::vector<double> alongs;
      for (int i = -4; i < along_size + 4; ++i) alongs.push_back(double(i) + 0.5);  // texel centres, edges and beyond
      if (!aligned_only) {
        for (int k = 0; k < 160; ++k) alongs.push_back(-4.0 + (along_size + 8.0) * Random());
        for (double f : {0.001, 0.25, 0.5, 0.999}) alongs.push_back(1.0 + f);
      }
      std::vector<double> others = {0.5, double(other_size) - 0.5, 0.5 + 0.5 * other_size};
      if (!aligned_only) {
        others.push_back(-2.3);
        others.push_back(other_size + 2.7);
        others.push_back(Random() * other_size);
      }
      for (double along : alongs) {
        for (double other : others) {
          for (int off = -4; off <= 4; ++off) {
            const R bx = R(axis == 0 ? along : other);
            const R by = R(axis == 0 ? other : along);
            g_fetches = 0;
            const R before = Tap<R>(image, axis, bx, by, R(off), R(scale), false, bits);
            stats.old_fetches += g_fetches;
            g_fetches = 0;
            const R now = Tap<R>(image, axis, bx, by, R(off), R(scale), true, bits);
            stats.new_fetches += g_fetches;
            ++stats.taps;
            stats.max_diff = std::max(stats.max_diff, std::fabs(double(before) - double(now)));
            stats.same_bits += before == now;
          }
        }
      }
    }
  }
  return stats;
}

// ---------------------------------------------------------------------------
// The text of the variant
// ---------------------------------------------------------------------------

const sp::VariantShader kShaderX = sp::kBloomGaussX;
const sp::VariantShader kShaderY = sp::kBloomGaussY;

// The nine taps of a Gaussian as the translator writes them, along one axis, and what the transform looks at around them.
std::string Fixture(bool x_axis, bool wrong_axis = false, bool with_main = true) {
  std::string t;
  t += "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n";
  t += "float4 tfetch2D(uint a, uint b, float2 c, float2 d);\nfloat4 tfetch2DLod(uint a, uint b, float2 c, float2 d, float e);\n\n";
  if (with_main) {
    t += "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iTexCoord0 : TEXCOORD0,\n"
         "\tout float4 oC0 : SV_Target0\n)\n{\n\tfloat4 r0 = iTexCoord0;\n\tfloat4 r1 = 0.0;\n";
  } else {
    t += "void notmain()\n{\n";
  }
  const int offsets[9] = {0, -4, -3, -2, -1, 1, 2, 3, 4};
  for (int i = 0; i < 9; ++i) {
    const bool swap = x_axis == wrong_axis;  // the offsets run along y in the X shader (or the other way)
    t += "\tr" + std::to_string(i + 1) +
         ".xyzw = tfetch2D(RenderMapBilinearSampler_Texture2DDescriptorIndex, RenderMapBilinearSampler_SamplerDescriptorIndex, "
         "r0.xy, float2(" +
         (swap ? "0, " + std::to_string(offsets[i]) : std::to_string(offsets[i]) + ", 0") + ")).xyzw;\n";
  }
  t += "\toC0.xyzw = r1.xyzw + r2.xyzw;\n\treturn;\n}\n";
  return t;
}

void CheckText() {
  for (const bool x_axis : {true, false}) {
    const sp::VariantShader& shader = x_axis ? kShaderX : kShaderY;
    const std::string what = std::string(x_axis ? "PSGaussX" : "PSGaussY") + ": ";
    const sp::VariantTransformResult result = sp::TransformBloomGaussHlsl(shader, Fixture(x_axis));
    Check(result.ok, what + "the transform succeeds (" + result.error + ")");
    if (!result.ok) continue;
    const std::string& out = result.hlsl;
    Check(Count(out, "GaussTap(") == 10 && Count(out, "= tfetch2D(Render") == 0, what + "nine calls and the definition, none of the game's taps");
    Check(Contains(out, std::string("GaussTap(") + (x_axis ? "float2(1.0, 0.0)" : "float2(0.0, 1.0)") + ", RenderMap"),
          what + "each call names its axis");
    Check(out.find("float4 GaussTap(") < out.find("void main("), what + "the helper is before main");
    // The helper's formulas: the model above is these.
    Check(Contains(out, "if (count == 3 && abs(scale - 3.0) < 0.001 && (resourceDescriptorIndex & TFETCH_GAMMA_BIT) == 0)"),
          what + "the two-fetch branch is for S = 3 within 0.001 and a texture without the gamma bit");
    Check(Contains(out, "float phase = dot(axis, getWeights2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset * scale));"),
          what + "f is the fraction of the position along the axis");
    Check(Contains(out, "offset * scale + axis * (1.0 / (2.0 - phase) - phase - 1.0), 0.0);") &&
              Contains(out, "offset * scale + axis * (phase / (1.0 + phase) - phase + 1.0), 0.0);"),
          what + "the two fetches' offsets are the model's");
    Check(Contains(out, "return first * ((2.0 - phase) * (1.0 / 3.0)) + second * ((1.0 + phase) * (1.0 / 3.0));"),
          what + "the weights are (2 - f) / 3 and (1 + f) / 3");
    Check(Count(out, "tfetch2DLod(resourceDescriptorIndex") == 3,
          what + "three explicit fetch calls in the helper (the two of S = 3 and the loop's)");
    // The branches that were there stay.
    Check(Contains(out, "if (scale > 0.99 && scale < 1.01)") && Contains(out, "int count = clamp(int(ceil(scale - 0.01)), 1, 8);") &&
              Contains(out, "float spacing = scale / float(count);") && Contains(out, "return sum * (1.0 / float(count));"),
          what + "the S = 1 call, the count, the loop and its weight are as they were");
    const sp::VariantTransformResult again = sp::TransformBloomGaussHlsl(shader, Fixture(x_axis));
    Check(again.ok && again.hlsl == out, what + "the transform is deterministic");
    // A Gaussian whose taps run along the other axis, and a text with no main, are refused.
    const sp::VariantTransformResult wrong = sp::TransformBloomGaussHlsl(shader, Fixture(x_axis, true));
    Check(!wrong.ok && Contains(wrong.error, "does not run along"), what + "taps along the other axis are refused (" + wrong.error + ")");
    const sp::VariantTransformResult no_main = sp::TransformBloomGaussHlsl(shader, Fixture(x_axis, false, false));
    Check(!no_main.ok && Contains(no_main.error, "pixel shader main"), what + "a text without main is refused");
  }
  const sp::VariantTransformResult other = sp::TransformBloomGaussHlsl(sp::VariantShader{"xrage_postfx__PSBlur", 1}, Fixture(true));
  Check(!other.ok, "another shader is refused");
  const sp::VariantDef def = sp::MakeBloomGaussVariant();
  Check(def.id == sp::kVariantBloomGauss && def.name == "bloom_gauss" && def.pixel_shaders.size() == 2 && def.transform,
        "the definition: id, name, two shaders, a transform");
  // The version is bumped with the helper (the stamp line variants_bloom_gauss_version): 1 was the sub-taps, 2 the two fetches.
  Check(def.version == sp::kBloomGaussVariantVersion && def.version == 2, "the definition carries version 2");
}

// ---------------------------------------------------------------------------
// The equality
// ---------------------------------------------------------------------------

void CheckEquality() {
  // Scales that take the old path in both models: the same bits, the same fetches.
  for (const double scale : {1.0, 1.005, 1.5, 2.0, 2.5, 3.002, 3.5, 4.0, 5.0, 8.0}) {
    const Stats s = Compare<double>(scale, 0, false);
    Check(s.same_bits == s.taps && s.old_fetches == s.new_fetches,
          "S = " + std::to_string(scale) + ": the two models are the same (" + std::to_string(s.taps) + " taps)");
  }
  // S = 2: two sub-taps, two fetches, before and now (nothing to reduce, exactly the same).
  {
    const Stats s = Compare<double>(2.0, 0, false);
    Check(s.old_fetches == 2 * s.taps && s.new_fetches == 2 * s.taps, "S = 2: two fetches a tap before and now");
    Check(s.max_diff == 0.0, "S = 2: the difference is 0");
    std::printf("S = 2: %ld taps, 2 fetches before and now, max |difference| %g\n", s.taps, s.max_diff);
  }
  // S = 3: three fetches become two, the same sum.
  {
    const Stats s = Compare<double>(3.0, 0, false);
    Check(s.old_fetches == 3 * s.taps && s.new_fetches == 2 * s.taps, "S = 3: three fetches a tap become two");
    Check(s.max_diff < 1e-12, "S = 3 in double: the same weighted sum to 1e-12 (" + std::to_string(s.max_diff) + ")");
    std::printf("S = 3 (double): %ld taps incl. 4 texels beyond the edges, 3 fetches -> 2, max |difference| %.3g\n", s.taps, s.max_diff);
    const Stats f = Compare<float>(3.0, 0, false);
    Check(f.max_diff < 1e-5, "S = 3 in float: the same sum to float rounding (" + std::to_string(f.max_diff) + ")");
    std::printf("S = 3 (float):  %ld taps, max |difference| %.3g\n", f.taps, f.max_diff);
    // Pixel aligned taps (f = 0, every pixel of the bloom's passes at 4K): both exact, in float too.
    const Stats a = Compare<double>(3.0, 0, true);
    Check(a.max_diff < 1e-12, "S = 3 aligned: equal (" + std::to_string(a.max_diff) + ")");
    // The hardware's filter has 8 bits of fraction: at f = 0 nothing changes, elsewhere the two forms round differently.
    const Stats h0 = Compare<double>(3.0, 8, true);
    Check(h0.max_diff < 1e-12, "S = 3, 8-bit filter, aligned taps: equal (" + std::to_string(h0.max_diff) + ")");
    const Stats h = Compare<double>(3.0, 8, false);
    Check(h.max_diff < 1.0 / 128.0, "S = 3, 8-bit filter, any tap: within 1/128 of the image's range (" + std::to_string(h.max_diff) + ")");
    std::printf("S = 3 (8-bit hardware weights): aligned max |difference| %.3g, any position %.3g of a range of 1\n", h0.max_diff,
                h.max_diff);
  }
  // Just off 3: within 0.001 the two fetches (the spacing is 1 within 0.03%), beyond it the loop.
  for (const double scale : {2.9995, 3.0005}) {
    const Stats s = Compare<double>(scale, 0, false);
    Check(s.new_fetches == 2 * s.taps && s.max_diff < 1e-3, "S = " + std::to_string(scale) + ": two fetches, within 1e-3 of the sub-taps (" +
                                                                std::to_string(s.max_diff) + ")");
  }

  // The weights themselves: an image of impulses along a strip, the response of the two fetches is (1 - f, 1, 1, f) / 3 on the
  // texels i - 1 .. i + 2 and 0 elsewhere (i = floor(position - 0.5)), for f from 0 to 1, with the strip's ends in reach.
  {
    bool ok = true;
    double worst = 0.0;
    const int n = 24;
    for (int axis = 0; axis < 2; ++axis) {
      for (const double f : {0.0, 0.125, 0.3, 0.5, 0.77, 0.999}) {
        for (const int i : {-3, 0, 1, 5, 11, 20, 22, 23, 26}) {
          // the position's texel (i + f) in texels - 0.5, at the offset 0: position = i + f + 0.5
          const double position = double(i) + f + 0.5;
          double total = 0.0;
          for (int k = 0; k < n; ++k) {
            // an impulse on the texel k of the strip (texels beyond its ends read as the end texels, the clamp)
            Image strip;
            strip.w = axis == 0 ? n : 1;
            strip.h = axis == 0 ? 1 : n;
            strip.v.assign(size_t(n), 0.0);
            strip.v[size_t(k)] = 1.0;
            const double got = Tap<double>(strip, axis, axis == 0 ? position : 0.5, axis == 0 ? 0.5 : position, 0.0, 3.0, true, 0);
            // expected: the weight of the texel k, with the clamp (the indices i - 1 .. i + 2 map to the nearest texel in the strip)
            double want = 0.0;
            const double weights[4] = {(1.0 - f) / 3.0, 1.0 / 3.0, 1.0 / 3.0, f / 3.0};
            for (int j = 0; j < 4; ++j) {
              const int texel = std::clamp(i - 1 + j, 0, n - 1);
              if (texel == k) want += weights[j];
            }
            worst = std::max(worst, std::fabs(got - want));
            total += got;
          }
          ok = ok && std::fabs(total - 1.0) < 1e-12;  // the weights add up to 1 (all four texels clamp into the strip)
        }
      }
    }
    Check(ok, "the impulse responses add up to 1");
    Check(worst < 1e-12, "the two fetches' weights are (1 - f, 1, 1, f) / 3 on the four texels, clamped at the strip's ends (" +
                             std::to_string(worst) + ")");
    std::printf("impulse weights at S = 3: max |weight - (1-f, 1, 1, f)/3| %.3g\n", worst);
  }
}

// ---------------------------------------------------------------------------
// The game's own files (optional)
// ---------------------------------------------------------------------------

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void CheckGameFiles(const std::filesystem::path& folder, const std::filesystem::path& results) {
  for (const sp::VariantShader* shader : {&kShaderX, &kShaderY}) {
    const std::filesystem::path file = folder / (std::string(shader->name) + ".hlsl");
    if (!std::filesystem::exists(file)) {
      Check(false, "game file " + file.string() + " is not there");
      continue;
    }
    const sp::VariantTransformResult result = sp::TransformBloomGaussHlsl(*shader, ReadFile(file));
    Check(result.ok, "game file " + std::string(shader->name) + ": " + result.error);
    if (!result.ok) continue;
    Check(Count(result.hlsl, "GaussTap(") == 10, std::string(shader->name) + ": nine calls and the helper");
    if (!results.empty()) {
      std::filesystem::create_directories(results);
      std::ofstream(results / (std::string(shader->name) + ".hlsl"), std::ios::binary) << result.hlsl;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckText();
  CheckEquality();
  if (argc > 1) {
    CheckGameFiles(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  if (g_failed) {
    std::printf("bloom_gauss_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("bloom_gauss_test: %d checks passed\n", g_checks);
  return 0;
}
