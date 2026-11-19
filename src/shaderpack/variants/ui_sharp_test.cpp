// Check of the UI sharpening variant's text transform and of the arithmetic of its helper (ui_sharp.h). A program of its own: it
// needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ui_sharp_variant_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_ui_sharp_variant_test [<translated xrage_im__PS_Textured.hlsl> [<folder for the transformed text>]]
//
// Without arguments it runs on a small text made here (the definition of tfetch2D, the two declarations of the shared constants,
// main and one call) and on texts it must refuse or skip, and on a model of the helper's arithmetic (the same formulas as its HLSL):
// the Catmull-Rom weights, and the alpha edge. With the game's translated HLSL it also transforms that and, given a folder, writes
// the result for a compile with dxc (-T ps_6_0 -E main -HV 2021 -DXENOS_SPEC_CONSTANTS=0, and with -spirv -fvk-use-dx-layout for SPIR-V).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "shaderpack/variants/ui_sharp.h"

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

void CheckNear(double got, double want, double tolerance, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tolerance)) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g (+-%g)\n", what.c_str(), got, want, tolerance);
  }
}

size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

// ---------------------------------------------------------------------------
// The text
// ---------------------------------------------------------------------------

// What every translated shader holds around the call, as the translator writes it (shader_common.h pasted in), cut to what the edits read.
std::string Fixture(int calls, int definitions = 1, bool with_main = true, bool with_declarations = true) {
  std::string text = "#define g_InvResolutionScale      vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n";
  if (!with_declarations) text.clear();
  text += "#else\n#define DEFINE_SHARED_CONSTANTS() \\\n    float4 g_PosOffset : packoffset(c36); \\\n"
          "    float2 g_InvResolutionScale : packoffset(c37.x);\n";
  if (!with_declarations) text = "#else\n";
  for (int i = 0; i < definitions; ++i) {
    text += "float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)\n{\n"
            "    return 0.0;\n}\n\n";
  }
  text += "float4 tfetch2DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)\n{\n"
          "    return 0.0;\n}\n\n";
  // The calls are in main (the helper goes before it); without main they are in a function of their own.
  std::string calls_text;
  for (int i = 0; i < calls; ++i) {
    calls_text += "\tr" + std::to_string(i % 2) + ".xyzw = tfetch2D(TextureSampler_Texture2DDescriptorIndex, "
                  "TextureSampler_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyzw;\n";
  }
  if (with_main) {
    text += "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iPos : SV_Position\n)\n{\n"
            "\tfloat4 r0 = iTexCoord0;\n\tfloat4 r1 = 0.0;\n" + calls_text +
            "\toC0.xyzw = r0.xyzw * r1.xyzw;\n\treturn;\n}\n";
  } else {
    text += "void body(inout float4 r0, inout float4 r1)\n{\n" + calls_text + "}\n";
  }
  return text;
}

void TestTransform() {
  const sp::VariantShader shader = sp::kUiSharpTextured;
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(1));
    Check(r.ok && !r.skip, "one call: " + r.error);
    if (r.ok) {
      Check(Count(r.hlsl, "tfetch2DUiSharp(") == 2, "one call: its definition and the renamed call");
      Check(Count(r.hlsl, "tfetch2D(") == 2, "one call: the game's definition and the helper's own call stay");
      Check(Count(r.hlsl, "float4 tfetch2D(uint resourceDescriptorIndex") == 1, "the game's definition is still there, once");
      Check(Count(r.hlsl, "tfetch2DLod(") == 1, "the explicit level fetch is not touched");
      Check(Count(r.hlsl, "SharedConstants + 608") == 1, "SPIR-V: g_UiSharp declared once");
      Check(Count(r.hlsl, "g_UiSharp : packoffset(c38)") == 1, "DXIL: g_UiSharp declared once");
      Check(r.hlsl.find('\x01') == std::string::npos, "no sentinel left");
      const size_t helper = r.hlsl.find("float4 tfetch2DUiSharp(");
      const size_t main_at = r.hlsl.find("void main(");
      const size_t call = r.hlsl.find("= tfetch2DUiSharp(");
      Check(helper != std::string::npos && main_at != std::string::npos && helper < main_at, "the helper comes before main");
      Check(call != std::string::npos && call > main_at, "the renamed call is in main, after the helper");
    }
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(3));
    Check(r.ok && Count(r.hlsl, "tfetch2DUiSharp(") == 4, "three calls: all renamed, plus the definition");
  }
  {
    // CRLF text (the translator's files on Windows): the same result.
    std::string crlf;
    for (const char c : Fixture(2)) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    const sp::VariantTransformResult a = sp::TransformUiSharpHlsl(shader, crlf);
    const sp::VariantTransformResult b = sp::TransformUiSharpHlsl(shader, Fixture(2));
    Check(a.ok && b.ok && a.hlsl == b.hlsl, "CRLF input gives the same text");
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(0));
    Check(!r.ok && r.skip, "no call: skipped, not an error");
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(1, 2));
    Check(!r.ok && !r.skip && !r.error.empty(), "two definitions of tfetch2D: refused");
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(1, 0));
    Check(!r.ok && !r.skip, "no definition of tfetch2D: refused");
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(1, 1, /*with_main=*/false));
    Check(!r.ok && !r.skip, "no main: refused (nothing to put the helper before)");
  }
  {
    const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(shader, Fixture(1, 1, true, /*with_declarations=*/false));
    Check(!r.ok && !r.skip, "no shared constants declaration: refused");
  }
  {
    const sp::VariantDef def = sp::MakeUiSharpVariant();
    Check(def.id == sp::kVariantUiSharp && def.id >= sp::kFirstPackedVariant && def.id < sp::kVariantSlots, "the id is a variant pack slot");
    Check(def.name == "ui_sharp" && def.version == sp::kUiSharpVariantVersion, "name and version");
    Check(def.pixel_shaders.size() == 1 && def.pixel_shaders[0].key == 0x5CA2EECD341441FFull &&
              def.pixel_shaders[0].name == "xrage_im__PS_Textured",
          "xrage_im PS_Textured by its verified identity");
    Check(def.pixel_name_suffixes.size() == std::size(sp::kUiSharpSuffixes) && def.vertex_shaders.empty() && bool(def.transform),
          "the others by name, no vertex shader, a transform");
    Check(sp::kVariantUiSharp == 10, "the id never changes: a pipeline cache holds it");
  }
}

// ---------------------------------------------------------------------------
// The helper's arithmetic (the same formulas as its HLSL)
// ---------------------------------------------------------------------------

double Saturate(double v) { return std::min(1.0, std::max(0.0, v)); }
double Smoothstep(double a, double b, double x) {
  const double t = Saturate((x - a) / (b - a));
  return t * t * (3.0 - 2.0 * t);
}

// The five-tap weights along one axis for the fraction f (UiSharpCatmullRom): w0 and w3 the outer taps, w12 the pair a bilinear tap reads.
struct Weights {
  double w0, w12, w3;
};
Weights CatmullRom(double f) {
  const double f2 = f * f, f3 = f2 * f;
  const double w0 = f2 - 0.5 * (f3 + f);
  const double w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
  const double w3 = 0.5 * (f3 - f2);
  const double w2 = 1.0 - w0 - w1 - w3;
  return {w0, w1 + w2, w3};
}

// UiSharpFetch's alpha steps: `a` is the filtered alpha, lo and hi the extremes of the four texels around the sample.
double AlphaEdge(double a, double lo, double hi, double magnification, double y) {
  const double range = hi - lo;
  const double edge = Smoothstep(0.30, 0.55, range);
  const double t = Saturate((a - lo) / std::max(range, 1.0e-3));
  const double gain = std::max(1.0, magnification * y);
  const double steep = Saturate((t - 0.5) * gain + 0.5);
  return a + (lo + range * steep - a) * edge;
}

void TestArithmetic() {
  // Catmull-Rom: the weights of an axis sum to 1, w12 + w0 + w3 = 1 (before the corner taps are dropped, the renormalisation divides by
  // the five taps' weights), and a fraction of 0 or 1 reads one texel.
  for (int i = 0; i <= 20; ++i) {
    const double f = i / 20.0;
    const Weights w = CatmullRom(f);
    CheckNear(w.w0 + w.w12 + w.w3, 1.0, 1e-12, "weights sum to 1 at " + std::to_string(f));
  }
  const Weights at0 = CatmullRom(0.0);
  CheckNear(at0.w0, 0.0, 1e-12, "fraction 0: outer weight 0");
  CheckNear(at0.w12, 1.0, 1e-12, "fraction 0: the pair is all the weight");
  const Weights half = CatmullRom(0.5);
  CheckNear(half.w0, -0.0625, 1e-12, "fraction 0.5: the first outer lobe is negative (the sharpening)");
  CheckNear(half.w3, -0.0625, 1e-12, "fraction 0.5: and so is the last");

  // The alpha edge. A glyph's edge ramps 0 -> 1 over one texel; magnified 3x the bilinear ramp is 3 pixels: the steepened one is 1 pixel.
  const double m = 3.0;
  auto ramp_width = [&](double y) {
    // across the texel, a = x (bilinear, x 0..1 in texels, lo = 0, hi = 1); width of the 10%..90% part of the result in pixels
    double x10 = -1.0, x90 = -1.0;
    for (int i = 0; i <= 3000; ++i) {
      const double x = i / 3000.0;
      const double a = AlphaEdge(x, 0.0, 1.0, m, y);
      if (x10 < 0.0 && a >= 0.1) x10 = x;
      if (x90 < 0.0 && a >= 0.9) x90 = x;
    }
    return (x90 - x10) * m;
  };
  const double plain = ramp_width(0.0);  // y = 0 -> gain 1: the filtered alpha as it is
  const double sharp = ramp_width(1.0);
  CheckNear(plain, 0.8 * m, 0.02, "the bilinear ramp's 10-90% width is 0.8 texels = 2.4 pixels at 3x");
  Check(sharp < 0.4 * plain && sharp > 0.2 * plain, "gain = magnification: the 10-90% ramp is about a third of it (" + std::to_string(sharp) + " pixels)");
  CheckNear(AlphaEdge(0.5, 0.0, 1.0, m, 1.0), 0.5, 1e-12, "the middle of the ramp stays where it was");
  // A thin stroke: the texels around the sample are 0 and 0.6, so the peak and the floor are kept.
  CheckNear(AlphaEdge(0.6, 0.0, 0.6, m, 1.0), 0.6, 1e-12, "a stroke's peak alpha is kept");
  CheckNear(AlphaEdge(0.0, 0.0, 0.6, m, 1.0), 0.0, 1e-12, "and its floor");
  Check(AlphaEdge(0.45, 0.0, 0.6, m, 1.0) > 0.45, "a stroke's shoulder moves towards the peak");
  // A fade or a glow (the four texels differ by less than 0.3): left as the game has it. At 0.30 the edge weight is still 0.
  for (const double range : {0.0, 0.05, 0.1, 0.2, 0.3}) {
    CheckNear(AlphaEdge(0.4 * range + 0.2, 0.2, 0.2 + range, m, 1.0), 0.4 * range + 0.2, 1e-12,
              "a range of " + std::to_string(range) + " is not an edge");
  }
  // A flat region (all four texels equal): no division by zero, no change.
  CheckNear(AlphaEdge(0.7, 0.7, 0.7, m, 1.0), 0.7, 1e-12, "flat alpha unchanged");
  // Strength 2 doubles the gain, a harder edge.
  Check(AlphaEdge(0.6, 0.0, 1.0, m, 2.0) > AlphaEdge(0.6, 0.0, 1.0, m, 1.0), "strength 2 steepens more than 1");
  // Magnification below 1 (a minified texel) never softens: gain is at least 1.
  CheckNear(AlphaEdge(0.6, 0.0, 1.0, 0.5, 1.0), 0.6, 1e-12, "gain never goes below 1");
}

}  // namespace

int main(int argc, char** argv) {
  TestTransform();
  TestArithmetic();

  if (argc >= 2) {
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
      std::printf("FAILED: cannot read %s\n", argv[1]);
      ++g_failed;
    } else {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const sp::VariantTransformResult r = sp::TransformUiSharpHlsl(sp::kUiSharpTextured, text);
      Check(r.ok, std::string("the game's translated text transforms: ") + r.error);
      if (r.ok) {
        Check(Count(r.hlsl, "= tfetch2DUiSharp(") >= 1, "the game's call is renamed");
        if (argc >= 3) {
          std::error_code ec;
          std::filesystem::create_directories(argv[2], ec);
          const std::filesystem::path out = std::filesystem::path(argv[2]) / std::filesystem::path(argv[1]).filename();
          std::ofstream file(out, std::ios::binary);
          file << r.hlsl;
          std::printf("wrote %s (%zu bytes)\n", out.string().c_str(), r.hlsl.size());
        }
      }
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
