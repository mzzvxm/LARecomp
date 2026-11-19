// LARecomp shader pack builder -- variant "ui_sharp": the UI's textured pixel shaders, sharper where the console's bitmaps are
// magnified (see ui_sharp.h).

#include "shaderpack/variants/ui_sharp.h"

#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The game's definition of the call (shader_common.h, inside every translated shader): the one `tfetch2D(` that is not a call.
constexpr std::string_view kDefinition = "float4 tfetch2D(uint resourceDescriptorIndex";
constexpr std::string_view kCall = "tfetch2D(";
constexpr std::string_view kSentinel = "\x01TFETCH2D_DEFINITION\x01";
// The macro that names a texture's descriptor in the SPIR-V block of a translated shader, and what follows its name.
constexpr std::string_view kDescriptorSuffix = "_Texture2DDescriptorIndex";
constexpr std::string_view kDescriptorDefine = "#define ";
constexpr std::string_view kDescriptorBody = " vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + ";
// Fetch slots a pixel shader has (the descriptor block holds 32 uints per texture kind: Texture3D starts at byte 128).
constexpr uint32_t kMaxSlots = 32;

// The helper, before main. g_UiSharp: x = the share of Catmull-Rom in the colour (0..1), y = the alpha edge's gain scale (0 leaves
// the alpha alone), z = the magnification below which the game's own fetch is made, w = the fetch slots the draw sharpens, one bit each
// (an integer below 2^16 held in a float: exact). The game's fetch is made first, whatever the branches: it takes a gradient, which is only
// defined in uniform flow (the branches below are uniform for the draw, or quad uniform with coarse derivatives, but the compiler cannot
// know that), and it is the bilinear tap the mix needs. The reconstruction itself uses explicit levels (level 0: the texels are
// magnified, so level 0 is what the sampler would pick as well) and GatherAlpha, which has none.
constexpr std::string_view kHelper =
    R"HLSL(// LARecomp variant ui_sharp (src/shaderpack/variants/ui_sharp.cpp): the UI's bitmaps, reconstructed sharper where the console's
// 1280x720 art is magnified. Catmull-Rom (five bilinear taps, the corners left out and the weights renormalised) in place of the
// bilinear fetch, and the alpha across an edge of the bitmap steepened to about a pixel again. Below the magnification the constant z
// names (1.25), and for a texture the draw did not name in the constant w (a depth, an environment map...) it is the game's own fetch.
float4 UiSharpCatmullRom(Texture2D<float4> tex, SamplerState smp, float2 uv, float2 size)
{
    float2 position = uv * size;
    float2 centre = floor(position - 0.5) + 0.5;
    float2 f = position - centre;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;
    float2 w12 = w1 + w2;
    float2 inv = 1.0 / size;
    float2 tc0 = (centre - 1.0) * inv;
    float2 tc3 = (centre + 2.0) * inv;
    float2 tc12 = (centre + w2 / w12) * inv;
    float4 sum = tex.SampleLevel(smp, float2(tc12.x, tc0.y), 0.0) * (w12.x * w0.y)
               + tex.SampleLevel(smp, float2(tc0.x, tc12.y), 0.0) * (w0.x * w12.y)
               + tex.SampleLevel(smp, tc12, 0.0) * (w12.x * w12.y)
               + tex.SampleLevel(smp, float2(tc3.x, tc12.y), 0.0) * (w3.x * w12.y)
               + tex.SampleLevel(smp, float2(tc12.x, tc3.y), 0.0) * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return sum / weight;
}

float4 tfetch2DUiSharp(uint slot, uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    float4 plain = tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
    float4 settings = g_UiSharp;
    if ((settings.x <= 0.0 && settings.y <= 0.0) || ((uint(settings.w + 0.5) >> slot) & 1u) == 0u)
    {
        return plain;
    }
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[TFETCH_INDEX(resourceDescriptorIndex)];
    float2 size = float2(getTexture2DDimensions(tex));
    float2 uv = texCoord + offset / size;
    float2 texel = uv * size;
    float2 gx = ddx_coarse(texel);
    float2 gy = ddy_coarse(texel);
    float footprint = sqrt(max(dot(gx, gx), dot(gy, gy)));
    float magnification = footprint > 1.0e-5 ? 1.0 / footprint : 1.0;
    if (magnification < settings.z)
    {
        return plain;
    }
    SamplerState smp = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    float4 result = plain;
    if (settings.x > 0.0)
    {
        float4 sharp = applyTfetchGamma(saturate(UiSharpCatmullRom(tex, smp, uv, size)), resourceDescriptorIndex);
        result = lerp(plain, sharp, saturate(settings.x));
    }
    if (settings.y > 0.0)
    {
        float4 ga = tex.GatherAlpha(smp, uv);
        float lo = min(min(ga.x, ga.y), min(ga.z, ga.w));
        float hi = max(max(ga.x, ga.y), max(ga.z, ga.w));
        float range = hi - lo;
        float edge = smoothstep(0.30, 0.55, range);
        float t = saturate((result.a - lo) / max(range, 1.0e-3));
        float gain = max(1.0, magnification * settings.y);
        float steep = saturate((t - 0.5) * gain + 0.5);
        result.a = lerp(result.a, lo + range * steep, edge);
    }
    return result;
}
)HLSL";

bool IsBitmapSampler(std::string_view argument) {
  if (!argument.ends_with(kDescriptorSuffix)) return false;
  const std::string_view name = argument.substr(0, argument.size() - kDescriptorSuffix.size());
  for (const std::string_view sampler : kUiSharpBitmapSamplers) {
    if (name == sampler) return true;
  }
  return false;
}

// The fetch slot of a texture's descriptor macro: `#define <macro> vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + N)`, the slot is N / 4.
// The macro has to be defined once, with a multiple of 4 below 4 x kMaxSlots; false with *error otherwise.
bool SlotOf(std::string_view text, std::string_view macro, uint32_t* slot, std::string* error) {
  const std::string head = std::string(kDescriptorDefine) + std::string(macro) + std::string(kDescriptorBody);
  const size_t found = CountOccurrences(text, head);
  if (found != 1) {
    *error = std::format("the descriptor macro {} is defined {} times in the SPIR-V block (need 1): not the translator's HLSL this was written for",
                         macro, found);
    return false;
  }
  size_t at = text.find(head) + head.size();
  uint32_t value = 0;
  size_t digits = 0;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 6) {
    value = value * 10 + uint32_t(text[at] - '0');
    ++at;
    ++digits;
  }
  if (digits == 0 || at >= text.size() || text[at] != ')' || value % 4 != 0 || value / 4 >= kMaxSlots) {
    *error = std::format("the descriptor macro {} does not end in a byte offset of a fetch slot (a multiple of 4 below {})", macro, 4 * kMaxSlots);
    return false;
  }
  *slot = value / 4;
  return true;
}

}  // namespace

VariantTransformResult TransformUiSharpHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string error;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.skip = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(message);
    return result;
  };

  // 1. The calls of tfetch2D(: all but the definition, which must be there once.
  const size_t definitions = CountOccurrences(text, kDefinition);
  const size_t total = CountOccurrences(text, kCall);
  if (definitions != 1 || total < 1) {
    return fail(std::format("the text has {} definitions of tfetch2D (need 1): not the translator's HLSL this was written for",
                            definitions));
  }
  const size_t calls = total - 1;
  if (calls == 0) {
    result.skip = true;
    result.error = "no fetch to sharpen";
    return result;
  }
  // The definition out of the way (a sentinel), the calls on a colour bitmap renamed with their slot in front, the definition back.
  if (!ReplaceAllExactly(text, kDefinition, kSentinel, 1, "the definition of tfetch2D", &error)) return fail(std::move(error));
  size_t sharpened = 0;
  {
    std::map<std::string, uint32_t> slots;  // by the argument's text
    std::string out;
    out.reserve(text.size() + 64 * calls);
    size_t from = 0;
    for (size_t at = text.find(kCall); at != std::string::npos; at = text.find(kCall, from)) {
      const size_t argument_begin = at + kCall.size();
      const size_t comma = text.find(',', argument_begin);
      if (comma == std::string::npos) return fail("a call of tfetch2D( without a second argument");
      std::string_view argument = std::string_view(text).substr(argument_begin, comma - argument_begin);
      while (!argument.empty() && IsSpace(argument.front())) argument.remove_prefix(1);
      while (!argument.empty() && IsSpace(argument.back())) argument.remove_suffix(1);
      out.append(text, from, at - from);
      if (IsBitmapSampler(argument)) {
        const std::string key(argument);
        auto slot = slots.find(key);
        if (slot == slots.end()) {
          uint32_t value = 0;
          if (!SlotOf(text, argument, &value, &error)) return fail(std::move(error));
          slot = slots.emplace(key, value).first;
        }
        out += std::format("tfetch2DUiSharp({}u, ", slot->second);
        ++sharpened;
      } else {
        out.append(kCall);
      }
      from = argument_begin;
    }
    out.append(text, from, std::string::npos);
    text = std::move(out);
  }
  if (!ReplaceAllExactly(text, kSentinel, kDefinition, 1, "the definition's sentinel", &error)) return fail(std::move(error));
  if (sharpened == 0) {
    result.skip = true;
    result.error = "no fetch of a colour bitmap to sharpen";
    return result;
  }

  // 2. g_UiSharp in the SPIR-V shared constants, 3. and in the DXIL block (as the HDR scene's variant puts g_HdrScene there).
  static constexpr Piece kSpirvDeclaration[] = {
      Lit("#define g_InvResolutionScale"), Ws(1),
      Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  // 4. The helper, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n// UI sharpening variant (src/gpu/ui_sharp.h UiSharpConstants): 16 bytes after the 608.\n"
                            "#define g_UiSharp vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group.substr(0, group.size() - 1)) +
                            "; \\\n    float4 g_UiSharp : packoffset(c38);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }

  // What the text must hold now: the helper's definition, one g_UiSharp declaration in each backend and one read.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"float4 tfetch2DUiSharp(", 1},
      {"float4 UiSharpCatmullRom(", 1},
      {"SharedConstants + 608", 1},
      {"g_UiSharp : packoffset(c38)", 1},
      {"float4 settings = g_UiSharp;", 1},
      {"float4 tfetch2D(uint resourceDescriptorIndex", 1},
      {"\x01", 0},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return fail(std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count, check.want));
    }
  }
  // tfetch2DUiSharp( is the helper's own definition and the renamed calls; the game's tfetch2D( is its definition, the helper's one call and
  // the calls that were not on a colour bitmap.
  const size_t renamed = CountOccurrences(text, "tfetch2DUiSharp(");
  const size_t game_calls = CountOccurrences(text, kCall);
  if (renamed != sharpened + 1 || game_calls != 2 + (calls - sharpened)) {
    return fail(std::format("the transformed HLSL has {} tfetch2DUiSharp( (need {}) and {} tfetch2D( (need {})", renamed, sharpened + 1,
                            game_calls, 2 + (calls - sharpened)));
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeUiSharpVariant() {
  VariantDef def;
  def.id = kVariantUiSharp;
  def.alpha_test_id = kVariantUiSharpAlphaTest;
  def.name = "ui_sharp";
  def.version = kUiSharpVariantVersion;
  def.pixel_shaders = {kUiSharpTextured};
  for (const std::string_view suffix : kUiSharpSuffixes) def.pixel_name_suffixes.push_back(suffix);
  def.transform = [](const VariantShader& shader, std::string_view hlsl) { return TransformUiSharpHlsl(shader, hlsl); };
  return def;
}

}  // namespace larecomp::shaderpack
