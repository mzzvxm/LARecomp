// LARecomp shader pack builder -- variant "hdr_scene": the HDR variant of the game's tone map (see
// hdr_scene.h). Port of native/tools/build_hdr_variants.py.

#include "shaderpack/variants/hdr_scene.h"

#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/hash.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
namespace {
// hdr_tonemap_shoulder.hlsli, turned into bytes by CMake (src/shaderpack/CMakeLists.txt), with a
// closing 0 that is not part of the file (so that even an empty file makes an array).
const unsigned char kEmbeddedCurve[] = {
#include "hdr_hlsli_bytes.inc"
};
}  // namespace
#endif

namespace larecomp::shaderpack {

using namespace textedit;

VariantTransformResult TransformToneMapHlsl(std::string_view hlsl, std::string_view include_text) {
  VariantTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string include = NormalizeNewlines(include_text);
  while (!include.empty() && IsSpace(include.back())) include.pop_back();  // include_text.rstrip()

  // 1. g_HdrScene in the SPIR-V shared constants (header of every generated shader).
  static constexpr Piece kSpirvDeclaration[] = {
      Lit("#define g_InvResolutionScale"), Ws(1),
      Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  // 2. and in the DXIL shared constants block.
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  // 3. The curve and the finish, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  // 4. The game's own code: the luminance curve (Ld = Ls (1 + Ls / W) / (1 + Ls), made in r0.z from r1.x = Ls)...
  static constexpr Piece kLuminanceCurve[] = {
      Lit("\tr3.z = r1.x * r0.z;"),
      Ws(0),
      Lit("\tps = clamp(rcp(r0.x), FLT_MIN, FLT_MAX);"),
      Ws(0),
      Lit("\tr0.x = ps;"),
      Ws(0),
      Lit("\tr0.xzw = r3.xzy * r0.yxy;")};
  // 5. ... and the last assignment (the game's encode).
  static constexpr Piece kEncode[] = {Lit("\toC0.xyz = "),
                                      Lit("select(r2.xyzz == 0.0, r1.xyzz, r0.xyzz)"),
                                      Lit(";")};

  const auto fail = [&](std::string error) {
    result.ok = false;
    result.hlsl.clear();
    result.error = std::move(error);
    return result;
  };
  std::string error;
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n// HDR scene variant (src/gpu/constants.h HdrSceneConstants): 16 bytes after the 608.\n"
                            "#define g_HdrScene vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [](std::string_view group) {
                     // the group without its closing ';', then the line continuation and the new member
                     return std::string(group.substr(0, group.size() - 1)) +
                            "; \\\n    float4 g_HdrScene : packoffset(c38);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [&](std::string_view group) { return include + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kLuminanceCurve, 0, 6, "Ld = Ls (1 + Ls / W) / (1 + Ls)",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tr0.z = HdrToneLd(r1.x, White.x, g_HdrScene);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kEncode, 1, 1, "oC0.xyz",
                   [](std::string_view group) { return "\toC0.xyz = HdrFinish(" + std::string(group) + ".xyz);"; },
                   &error)) {
    return fail(std::move(error));
  }

  for (const std::string_view needle : {std::string_view("HdrToneLd(r1.x"), std::string_view("HdrFinish(select"),
                                        std::string_view("g_HdrScene : packoffset(c38)"),
                                        std::string_view("SharedConstants + 608")}) {
    const size_t count = CountOccurrences(text, needle);
    if (count != 1) return fail(std::format("the transformed HLSL has '{}' {} times", needle, count));
  }

  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

// ---------------------------------------------------------------------------
// The variant
// ---------------------------------------------------------------------------

namespace {

// The tone map and the vertex shaders it is drawn with: they make its uv and the adapted luminance (the
// second also the screen position the fog variants use). Which one the game pairs it with is told by
// the renderer's own check; the runtime requires one of these. The order is the pack's: slot 0, 1.
constexpr VariantShader kPixelTarget = {"xrage_postfx__PSToneMap", 0xBC5F0EED0A746DEAull};
constexpr VariantShader kVertexTargets[] = {
    {"xrage_postfx__VS_ragePassThroughNoXform4TexOnly", 0xF097B3E9236CB05Aull},
    {"xrage_postfx__VS_ragePassThroughNoXform4TexAndScrPos", 0xF25FA84098BACE43ull},
};
static_assert(std::size(kVertexTargets) <= kVariantVertexShaders);

// The embedded curve, or empty text when this build has none.
std::string EmbeddedCurve() {
#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
  return std::string(reinterpret_cast<const char*>(kEmbeddedCurve), sizeof(kEmbeddedCurve) - 1);
#else
  return std::string();
#endif
}

}  // namespace

VariantDef MakeHdrSceneVariantFromText(std::string_view hlsli_text) {
  VariantDef def;
  def.id = kVariantHdrScene;
  def.name = "hdr_scene";
  def.version = kHdrSceneVariantVersion;
  // The fingerprint is of the curve as the transform reads it (TransformToneMapHlsl: CRLF and lone CR are LF), not of
  // the file's bytes. CMake embeds the file as the tree has it, so a Windows tree with CRLF files (git autocrlf) and a
  // tree with LF files (a worktree, Linux) would give the pack tool and the game different values for the same curve,
  // and the game would find a tool-made stamp stale and rebuild the variant pack. For an LF file the value is the same
  // as before, so stamps made from LF trees stay valid.
  def.input_xxh3 = Xxh3(NormalizeNewlines(hlsli_text));
  def.pixel_shaders = {kPixelTarget};
  def.vertex_shaders.assign(std::begin(kVertexTargets), std::end(kVertexTargets));
  def.transform = [curve = std::string(hlsli_text)](const VariantShader&, std::string_view hlsl) {
    return TransformToneMapHlsl(hlsl, curve);
  };
  return def;
}

bool MakeHdrSceneVariant(VariantDef* out, std::string* why) {
  const std::string curve = EmbeddedCurve();
  if (curve.empty()) {
#ifdef LARECOMP_HDR_HLSLI_EMBEDDED
    *why = "the embedded HDR curve file is empty";
#else
    *why = "this build has no HDR curve embedded (src/gpu/hlsl/hdr_tonemap_shoulder.hlsli was not in the "
           "source tree when CMake configured; LARECOMP_HDR_HLSLI can name another file)";
#endif
    return false;
  }
  *out = MakeHdrSceneVariantFromText(curve);
  return true;
}

}  // namespace larecomp::shaderpack
