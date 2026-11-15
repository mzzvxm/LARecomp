// Check of the luminance scale variant's text transform and of its sampling geometry (luminance_scale.h). A program
// of its own: it needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_luminance_scale_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_luminance_scale_test [<translated xrage_postfx__PSSampleAverageLuminance.hlsl>
//                                                             [<folder for the transformed text>]]
//
// Without arguments it runs on a small text made here (the nine taps of the game's shader and the lines around
// them the transform must leave as they are), on texts it must refuse, and on the geometry: a model of the
// helper's taps (the same arithmetic as its HLSL) over the 128x128 pixels of the pass, counting which texels of the
// image they read at each render scale. With the game's translated HLSL it also transforms that and, given a
// folder, writes the result for a compile with dxc (-T ps_6_0 -E main -HV 2021 -DXENOS_SPEC_CONSTANTS=0, and with
// -spirv -fvk-use-dx-layout for SPIR-V).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/luminance_scale.h"

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
// The text
// ---------------------------------------------------------------------------

constexpr const char* kTapArgs =
    "ToneMapSampler_Texture2DDescriptorIndex, ToneMapSampler_SamplerDescriptorIndex, r0.xy, float2(";

// The nine taps of the game's shader, in its order, with its registers.
const char* const kOffsets[9] = {"1, 1", "0, -1", "-1, 1", "1, 0", "1, -1", "0, 1", "0, 0", "-1, -1", "-1, 0"};
const char* const kDests[9] = {"r3.xyz", "r1.yzw", "r4.xyz", "r5.xyz", "r6.xyz", "r7.xyz", "r8.xyz", "r2.yzw", "r0.xyz"};

std::string Fixture(const std::vector<std::string>& offsets) {
  std::string text = "float4 c255 = asfloat(uint4(0x38D1B717, 0x3DE38E2A, 0x3F317218, 0x0));\n\n"
                     "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iPos : SV_Position\n)\n{\n"
                     "\tfloat4 r0 = iTexCoord0;\n";
  for (size_t i = 0; i < offsets.size(); ++i) {
    text += std::string("\t") + kDests[i % 9] + " = tfetch2D(" + kTapArgs + offsets[i] + ")).xyz;\n";
  }
  text += "\tr2.x = dot(r0.zxy, LUMINANCE.zxy);\n\toC0.xyzw = r0.xxxx * c255.yyyy;\n\treturn;\n}\n";
  return text;
}

std::vector<std::string> Offsets() { return std::vector<std::string>(std::begin(kOffsets), std::end(kOffsets)); }

// The transform undone: the nine calls back to tfetch2D and the helper (everything before the pixel shader's main
// attribute that the transform put there) taken out.
std::string Undo(std::string text) {
  const std::string from = std::string("LuminanceTap(") + kTapArgs;
  const std::string to = std::string("tfetch2D(") + kTapArgs;
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  const size_t helper = text.find("// LARecomp variant luminance_scale");
  const size_t attribute = text.find("#ifndef __spirv__");
  if (helper != std::string::npos && attribute != std::string::npos && helper < attribute) {
    text.erase(helper, attribute - helper);
  }
  return text;
}

void CheckText() {
  const sp::VariantShader shader = sp::kLuminanceSample;
  const std::string fixture = Fixture(Offsets());
  const sp::VariantTransformResult result = sp::TransformLuminanceScaleHlsl(shader, fixture);
  Check(result.ok, "the fixture is transformed: " + result.error);
  if (result.ok) {
    Check(Count(result.hlsl, "LuminanceTap(") == 10, "nine calls and the helper's definition");
    Check(Count(result.hlsl, std::string("tfetch2D(") + kTapArgs) == 0, "no tap of the game's is left");
    Check(Undo(result.hlsl) == fixture, "undoing the nine calls and the helper gives the original text");
    const size_t helper = result.hlsl.find("float4 LuminanceTap(");
    const size_t attribute = result.hlsl.find("#ifndef __spirv__");
    Check(helper != std::string::npos && helper < attribute, "the helper is before the shader's main");
    Check(Contains(result.hlsl, "tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset)"),
          "the helper keeps the game's own call for scale 1");
    Check(Contains(result.hlsl, "tfetch2DLod("), "the helper's sub-taps are explicit level fetches");
    Check(!Contains(result.hlsl, "g_InvResolutionScale"), "it does not read the draw's scale (the target is the guest's)");
  }
  // CRLF text gives the same result as LF text.
  std::string crlf;
  for (const char c : fixture) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  const sp::VariantTransformResult crlf_result = sp::TransformLuminanceScaleHlsl(shader, crlf);
  Check(crlf_result.ok && result.ok && crlf_result.hlsl == result.hlsl, "CRLF text gives the same variant");

  // What it must refuse.
  Check(!sp::TransformLuminanceScaleHlsl({"xrage_postfx__PSGaussX", 0}, fixture).ok, "another shader's name is refused");
  std::vector<std::string> eight = Offsets();
  eight.pop_back();
  Check(!sp::TransformLuminanceScaleHlsl(shader, Fixture(eight)).ok, "eight taps are refused");
  std::vector<std::string> ten = Offsets();
  ten.push_back("0, 0");
  Check(!sp::TransformLuminanceScaleHlsl(shader, Fixture(ten)).ok, "ten taps are refused");
  std::vector<std::string> twice = Offsets();
  twice[8] = "0, 0";  // (-1, 0) is gone, (0, 0) is there twice
  Check(!sp::TransformLuminanceScaleHlsl(shader, Fixture(twice)).ok, "a window with an offset twice is refused");
  std::vector<std::string> wide = Offsets();
  wide[8] = "-2, 0";
  Check(!sp::TransformLuminanceScaleHlsl(shader, Fixture(wide)).ok, "an offset outside the 3 x 3 window is refused");
  std::vector<std::string> half = Offsets();
  half[8] = "-0.5, 0";
  Check(!sp::TransformLuminanceScaleHlsl(shader, Fixture(half)).ok, "an offset that is not whole is refused");
  std::string no_main = fixture;
  no_main.replace(no_main.find("void main("), 10, "void other(");
  Check(!sp::TransformLuminanceScaleHlsl(shader, no_main).ok, "a text without the shader's main is refused");
  const sp::VariantDef def = sp::MakeLuminanceScaleVariant();
  Check(def.id == sp::kVariantLuminanceScale && def.name == "luminance_scale" && def.pixel_shaders.size() == 1 &&
            def.pixel_shaders[0].key == 0x3CEC0BB4CCD2BBBEull,
        "the variant is declared for the shader 3CEC0BB4CCD2BBBE under id 8");
  Check(sp::kVariantLuminanceScale >= sp::kFirstPackedVariant && sp::kVariantLuminanceScale < sp::kVariantSlots,
        "its id is one of the variant pack's");
}

// ---------------------------------------------------------------------------
// The geometry: a model of the helper's taps (the arithmetic of its HLSL) over the pass's pixels
// ---------------------------------------------------------------------------

// The offsets, in texels of an image of `size` texels, that the helper reads for the game's offset `a` along one
// axis at the scale `scale` of that axis (the image's size over the guest's); `game` is the game's own call.
std::vector<double> TapOffsets(double scale, int a, bool game) {
  if (game || scale < 1.01) return {double(a)};
  const int count = std::clamp(int(std::ceil(scale - 0.01)), 1, 4);
  const double spacing = scale / count;
  std::vector<double> out;
  for (int i = 0; i < count; ++i) out.push_back(a * scale + (i - 0.5 * (count - 1)) * spacing);
  return out;
}

// The share of the image's texels along one axis that the pass reads: `pixels` (128) output pixels over `size`
// texels, point samples at floor(x + offset) clamped to the image, for the nine taps' offsets -1, 0, 1.
double Coverage(int pixels, int size, double scale, bool game) {
  std::vector<char> read(size_t(size), 0);
  for (int p = 0; p < pixels; ++p) {
    const double x = (p + 0.5) / pixels * size;
    for (int a = -1; a <= 1; ++a) {
      for (const double offset : TapOffsets(scale, a, game)) {
        const int texel = std::clamp(int(std::floor(x + offset)), 0, size - 1);
        read[size_t(texel)] = 1;
      }
    }
  }
  return double(std::count(read.begin(), read.end(), char(1))) / size;
}

void CheckGeometry() {
  // 1x: 320x180; 1.5x: 480x270; 2x: 640x360; 3x: 960x540.
  const double scales[] = {1.0, 1.5, 2.0, 3.0};
  double game_at_3 = 0.0;
  double game_at_2 = 0.0;
  for (const double scale : scales) {
    const int w = int(std::lround(320 * scale));
    const int h = int(std::lround(180 * scale));
    const double game = Coverage(128, w, scale, true) * Coverage(128, h, scale, true);
    const double variant = Coverage(128, w, scale, false) * Coverage(128, h, scale, false);
    std::printf("scale %.1f (%dx%d): the game's 9 taps read %.1f%% of the image, the variant's %.1f%%\n", scale, w, h,
                100.0 * game, 100.0 * variant);
    if (scale == 1.0) {
      Check(game > 0.999 && variant > 0.999, "1x: complete, as the game has it");
      Check(TapOffsets(1.0, -1, false) == TapOffsets(1.0, -1, true) && TapOffsets(1.0, 1, false).size() == 1,
            "1x: the variant's taps are the game's taps");
    } else {
      Check(variant > 0.999, std::to_string(scale) + "x: the variant reads every texel");
    }
    if (scale == 3.0) game_at_3 = game;
    if (scale == 2.0) game_at_2 = game;
  }
  Check(std::abs(game_at_3 - 0.284) < 0.01, "3x: the game's taps read 28% of the image (the design's number)");
  Check(std::abs(game_at_2 - 0.60) < 0.03, "2x: the game's taps read about 60% of the image");
  // At a whole scale the S x S sub-taps of the three offsets are the 3S x 3S texels in a row, each once.
  std::set<double> window;
  size_t taps = 0;
  for (int a = -1; a <= 1; ++a) {
    for (const double offset : TapOffsets(3.0, a, false)) {
      window.insert(offset);
      ++taps;
    }
  }
  Check(taps == 9 && window.size() == 9 && *window.begin() == -4.0 && *window.rbegin() == 4.0,
        "3x: the nine offsets along an axis are -4 to 4, each once (81 fetches over the two axes)");
  // A scale within 1% of 1, and one below, keep the game's taps.
  Check(TapOffsets(1.005, 1, false) == TapOffsets(1.0, 1, true), "a scale within 1% of 1 keeps the game's tap");
  Check(TapOffsets(0.5, 1, false) == TapOffsets(1.0, 1, true), "a scale below 1 keeps the game's tap");
}

// ---------------------------------------------------------------------------
// The game's own file (optional)
// ---------------------------------------------------------------------------

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void CheckGameFile(const std::filesystem::path& file, const std::filesystem::path& results) {
  const std::string original = ReadFile(file);
  Check(!original.empty(), "the game's HLSL can be read: " + file.string());
  const sp::VariantTransformResult result = sp::TransformLuminanceScaleHlsl(sp::kLuminanceSample, original);
  Check(result.ok, "the game's HLSL is transformed: " + result.error);
  if (!result.ok) return;
  Check(Count(result.hlsl, "LuminanceTap(") == 10, "game file: nine calls and the definition");
  Check(Count(result.hlsl, "tfetch2D(ToneMapSampler") == 0, "game file: no tap of the game's is left");
  std::string normal;
  for (size_t i = 0; i < original.size(); ++i) {
    if (original[i] == '\r') {
      normal += '\n';
      if (i + 1 < original.size() && original[i + 1] == '\n') ++i;
    } else {
      normal += original[i];
    }
  }
  Check(Undo(result.hlsl) == normal, "game file: undoing the edits gives the game's text");
  std::printf("game file: transformed, %zu bytes (the game's %zu)\n", result.hlsl.size(), original.size());
  if (!results.empty()) {
    std::filesystem::create_directories(results);
    std::ofstream(results / "luminance_scale.hlsl", std::ios::binary) << result.hlsl;
    std::printf("the text is in %s\n", (results / "luminance_scale.hlsl").string().c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckText();
  CheckGeometry();
  if (argc > 1) {
    CheckGameFile(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
