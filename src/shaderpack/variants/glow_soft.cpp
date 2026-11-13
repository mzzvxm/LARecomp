// LARecomp shader pack builder -- variant "glow_soft": the glow sprites' visibility as a share of depth taps
// (see glow_soft.h).

#include "shaderpack/variants/glow_soft.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The helper, before main. Two stages: the 8 outer taps of the spiral (the rim, in all directions) and, only
// when they do not agree, the 16 inner ones. A tap is a point fetch of the depth copy at the disc's position
// (the copy is S times the guest's 1280x720; the texel the position falls in), compared as the game does:
// `strictCompare` is the star's (the limit above the scene's depth), the other the round glow's (the depth not
// above the limit). `testOn` is the round glow's `TexCoord0.z > 0` guard: with it off the sprite is not tested
// at all (share 1), as in the game.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant glow_soft (src/shaderpack/variants/glow_soft.cpp): the visibility of a glow sprite as the
// share of 24 depth taps on a disc of radius 2.5 pixels of the game's 1280x720 screen round its projected centre
// that pass the game's compare, in place of the one tap at the centre. The disc is in guest pixels, so it is the
// same part of the picture at every render scale. The sprite shows in full while half of the taps or more pass
// and fades below that, along a smoothstep (a plateau where a lamp mostly sits, the fade in the middle). The outer 8 taps (16..23) are taken first and settle every glow that is not at an edge.
static const float2 kGlowDisc[24] = {
    float2( 0.144338,  0.000000), float2(-0.184342,  0.168873), float2( 0.028217, -0.321513),
    float2( 0.232351,  0.303061), float2(-0.426393, -0.075423), float2( 0.403917, -0.256939),
    float2(-0.135102,  0.502574), float2(-0.257655, -0.496099), float2( 0.559008,  0.204149),
    float2(-0.581555,  0.240057), float2( 0.280348, -0.599087), float2( 0.207170,  0.660490),
    float2(-0.624412, -0.361860), float2( 0.732507, -0.161040), float2(-0.447038,  0.635865),
    float2(-0.103276, -0.796974), float2( 0.634013,  0.534347), float2(-0.853183,  0.035282),
    float2( 0.622332, -0.619303), float2(-0.041636,  0.900426), float2(-0.592151, -0.709594),
    float2( 0.938032,  0.126211), float2(-0.794793,  0.552996), float2( 0.217183, -0.965401)
};

float GlowTapPasses(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 guestPixel, float limit, bool strictCompare)
{
    float depth = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, guestPixel * float2(1.0 / 1280.0, 1.0 / 720.0), float2(0.0, 0.0), 0.0).x;
    return (strictCompare ? (limit > depth) : !(depth > limit)) ? 1.0 : 0.0;
}

float GlowVisibleShare(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 uv, float limit, bool testOn, bool strictCompare)
{
    [branch]
    if (!testOn)
    {
        return 1.0;
    }
    float2 guestPixel = uv * float2(1280.0, 720.0);
    float passed = 0.0;
    [unroll]
    for (int i = 16; i < 24; ++i)
    {
        passed += GlowTapPasses(resourceDescriptorIndex, samplerDescriptorIndex, guestPixel + kGlowDisc[i] * 2.5, limit, strictCompare);
    }
    [branch]
    if (passed < 0.5 || passed > 7.5)
    {
        return passed > 7.5 ? 1.0 : 0.0;
    }
    [unroll]
    for (int j = 0; j < 16; ++j)
    {
        passed += GlowTapPasses(resourceDescriptorIndex, samplerDescriptorIndex, guestPixel + kGlowDisc[j] * 2.5, limit, strictCompare);
    }
    float visible = saturate(passed * (2.0 / 24.0));
    return visible * visible * (3.0 - 2.0 * visible);
}
)HLSL";

// What one of the two shaders is edited at: the tap and what the game compares it with (anchors that must
// each be found once, in this order, so the compare is the one the helper stands for), and the colour write.
struct Shape {
  std::string_view tap;       // the game's one tap, replaced
  std::string_view tap_call;  // its replacement: the helper's call and the "depth" that carries its answer
  std::string_view colour;    // the colour write, multiplied by the share
};

// PSTrailSeedRT: r3.xy = (r1.yx / r1.w), the tap at r3.yx; the compare r3.x > 1 - r0.z is only made when
// r0.z > 0 (r1.y); p0 = no compare passed.
constexpr Shape kTrailShape = {
    "r3.x = tfetch2D(DepthMapSampler_Texture2DDescriptorIndex, DepthMapSampler_SamplerDescriptorIndex, r3.yx, "
    "float2(0, 0)).x;",
    "float glowShare = GlowVisibleShare(DepthMapSampler_Texture2DDescriptorIndex, "
    "DepthMapSampler_SamplerDescriptorIndex, r3.yx, c255.w - r0.z, r0.z > 0.0, false);\n"
    "\tr3.x = glowShare > 0.0 ? -1.0e20 : 1.0e20;",
    "oC0.xyz = saturate(r2.wwww * r2.xyzz + c254.yyyy);",
};

// PSSeedRT1: the tap at r1.xy; the star draws when c255.x - r0.z is greater than the depth (no guard).
constexpr Shape kStarShape = {
    "r0.w = tfetch2D(DepthMapSampler_Texture2DDescriptorIndex, DepthMapSampler_SamplerDescriptorIndex, r1.xy, "
    "float2(0, 0)).x;",
    "float glowShare = GlowVisibleShare(DepthMapSampler_Texture2DDescriptorIndex, "
    "DepthMapSampler_SamplerDescriptorIndex, r1.xy, c255.x - r0.z, true, true);\n"
    "\tr0.w = glowShare > 0.0 ? -1.0e20 : 1.0e20;",
    "oC0.xyz = saturate(r1.zyx * r2.xyz);",
};

// The identity edit that says the context is there exactly once; it changes nothing.
bool FindOnce(std::string& text, std::span<const Piece> pieces, std::string_view what, std::string* error) {
  return ReplaceOnce(text, pieces, 0, pieces.size() - 1, what,
                     [](std::string_view group) { return std::string(group); }, error);
}

// The game's compare of the round glow, from the tap on: the limit 1 - r0.z, the compare, the guard
// r0.z > 0 and p0 = no compare passed.
bool CheckTrailCompare(std::string& text, std::string* error) {
  static constexpr Piece kLimit[] = {
      Lit(kTrailShape.tap),
      Ws(1),
      Lit("r0.w = saturate(-r1.z * c255.y + c255.w);"),
      Ws(1),
      Lit("ps = c255.w - r0.z;"),
      Ws(1),
      Lit("r1.x = ps;"),
      Ws(1),
      Lit("r1.x = r3.x > r1.x;"),
  };
  static constexpr Piece kGuard[] = {
      Lit("ps = r0.z > 0.0;"),
      Ws(1),
      Lit("r1.y = ps;"),
      Ws(1),
      Lit("r1.x = r1.y * r1.x;"),
      Ws(1),
      Lit("p0 = r1.x == 0.0;"),
  };
  return FindOnce(text, kLimit, "the round glow's depth tap and the compare with 1 - TexCoord0.z", error) &&
         FindOnce(text, kGuard, "the round glow's TexCoord0.z > 0 guard and p0", error);
}

// The star's compare, from the tap on: the limit c255.x - r0.z greater than the depth, p0 = it passed.
bool CheckStarCompare(std::string& text, std::string* error) {
  static constexpr Piece kCompare[] = {
      Lit(kStarShape.tap),
      Ws(1),
      Lit("ps = c255.x - r0.z;"),
      Ws(1),
      Lit("r1.x = ps;"),
      Ws(1),
      Lit("r0.w = r1.x > r0.w;"),
      Ws(1),
      Lit("p0 = r0.w != 0.0;"),
  };
  return FindOnce(text, kCompare, "the star's depth tap and the compare with 1 - c255.x", error);
}

}  // namespace

VariantTransformResult TransformGlowSoftHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  const bool trail = shader.name == kGlowSoftTrail.name;
  if (!trail && shader.name != kGlowSoftStar.name) {
    result.error = std::format("{} is not one of the glow sprites' shaders", shader.name);
    return result;
  }
  const Shape& shape = trail ? kTrailShape : kStarShape;
  std::string text = NormalizeNewlines(hlsl);
  std::string error;
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.hlsl.clear();
    result.error = std::string(shader.name) + ": " + std::move(why);
    return result;
  };

  // 1. The compare is the one the helper replaces (anchors only).
  if (!(trail ? CheckTrailCompare(text, &error) : CheckStarCompare(text, &error))) return fail(std::move(error));

  // 2. The tap becomes the call of the helper and the answer in the register the game compares.
  if (!ReplaceAllExactly(text, shape.tap, shape.tap_call, 1, "the glow's one depth tap", &error)) {
    return fail(std::move(error));
  }

  // 3. The colour that is added to the picture times the share (the game's own write, then `* glowShare`).
  const std::string colour(shape.colour);
  const std::string colour_scaled = colour.substr(0, colour.size() - 1) + " * glowShare;";
  if (!ReplaceAllExactly(text, colour, colour_scaled, 1, "the glow's colour write", &error)) {
    return fail(std::move(error));
  }

  // 4. The helper, before main (the same anchor as the other variants').
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }

  // What the text must hold now: the helper's definition and the shader's one call, three uses of the share,
  // and no depth tap of the game's left.
  const size_t calls = CountOccurrences(text, "GlowVisibleShare(");
  const size_t shares = CountOccurrences(text, "glowShare");
  const size_t old_taps = CountOccurrences(text, shape.tap);
  if (calls != 2 || shares != 3 || old_taps != 0) {
    return fail(std::format("the transformed HLSL has {} GlowVisibleShare( (need 2), {} glowShare (need 3) and {} of "
                            "the game's depth taps (need 0)",
                            calls, shares, old_taps));
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeGlowSoftVariant() {
  VariantDef def;
  def.id = kVariantGlowSoft;
  def.name = "glow_soft";
  def.version = kGlowSoftVariantVersion;
  def.pixel_shaders = {kGlowSoftTrail, kGlowSoftStar};
  def.transform = [](const VariantShader& shader, std::string_view hlsl) {
    return TransformGlowSoftHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack
