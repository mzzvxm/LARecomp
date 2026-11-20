// LARecomp shader pack builder -- variant "shadow_filter": the sun shadow collector's soft lookup and its
// contact hardening (see shadow_filter.h).

#include "shaderpack/variants/shadow_filter.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The game's lookup of the atlas: a texture fetch at the sampler of the shadow map's slot, and what follows it
// on a line of the translator's text. The four taps of the rotated cross each have exactly this form.
constexpr std::string_view kLookupPrefix =
    "tfetch2D(DepthTextureSampler0_Texture2DDescriptorIndex, DepthTextureSampler0_SamplerDescriptorIndex, ";
constexpr std::string_view kLookupSuffix = ", float2(0, 0)).x;";
// The shaders with a filter of their own (the car paint, the skinned metal, the glass) weigh a 2x2 block.
constexpr std::string_view kOwnFilterMarker = "getWeights2D(DepthTextureSampler0_Texture2DDescriptorIndex";
// The constants that make the four taps a cross of radii 1, 0.75, 0.5, 0.25 x count / 1280: c255.xyzw =
// (0.75, 1, 0.5, 0.25) / 1280 (the same line in every shader that has the cross), and the 0.25 of the average
// (c253.y; the register is named c253 in all of them).
constexpr std::string_view kRadiiLine =
    "float4 c255 = asfloat(uint4(0x3A19999A, 0x3A4CCCCD, 0x39CCCCCD, 0x394CCCCD));";
constexpr std::string_view kQuarterConstant = "0x3E800000";

// The helper and its constants, before main. g_ShadowFilter is declared by the two edits below it.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant shadow_filter (src/shaderpack/variants/shadow_filter.cpp): the sun shadow lookup with N taps
// on a Vogel disk instead of the game's four. g_ShadowFilter (16 bytes after the shared constants): x the number
// of taps, y the radius scale (1: the game's radius), z the noise frame (0: a fixed pattern; a temporal upscaler
// gets the frame number so that its history averages the pattern out), w the mode bits (1: Gather taps, a
// bilinear weighted compare of four texels each, instead of the sampler's own bilinear depth; 2: contact
// hardening, a blocker search first; bits 8 to 15: the sun's size for it, in sixteenths of the real sun's).
static const float kShadowFilterRadiusGain = 1.05;     // the weighted disk is as wide as the game's four taps
static const float kShadowFilterGoldenCos = -0.7373688780783197;  // cos and sin of the golden angle, 137.5 degrees
static const float kShadowFilterGoldenSin = 0.6754902942615238;
static const float kShadowFilterSunTan = 0.0047123890;  // tan of the sun's angular radius, 0.27 degrees
static const float kShadowFilterMinTexels = 0.75;       // contact hardening: the smallest kernel radius, in atlas texels
static const float kShadowFilterMaxRadii = 2.0;         // and the largest, in radii of the soft filter (the search covers it)
static const int kShadowFilterSearchTaps = 8;

// Interleaved gradient noise of a host pixel, plus the R2 shift of the frame (as the ambient occlusion's).
float ShadowFilterNoise(float2 pixel, float frame)
{
    float gradient = frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715))));
    return frac(gradient + frame * 0.75487766624669);
}

// 1 where the shadow map's depth at uv is not nearer than the receiver's (lit), else 0: the game's own compare.
float ShadowFilterLookup(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 uv, float depth, bool gather)
{
    if (gather)
    {
        Texture2D<float4> shadowMap = g_Texture2DDescriptorHeap[TFETCH_INDEX(resourceDescriptorIndex)];
        float2 weight = frac(uv * float2(getTexture2DDimensions(shadowMap)) - 0.5);
        float4 lit = shadowMap.GatherRed(g_SamplerDescriptorHeap[samplerDescriptorIndex], uv) >= depth;
        // Gather returns (x, y, z, w) = the texels (0, 1), (1, 1), (1, 0), (0, 0) from the lower left one.
        return lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
    }
    float lit = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, uv, float2(0.0, 0.0), 0.0).x >= depth;
    return lit;
}

// Contact hardening: the half widths, along the atlas's u and v, of the kernel that a receiver needs when the
// blockers the search found lie `gap` (a difference of atlas depths) nearer to the sun. The sun's disc, seen
// from the receiver through the blockers' plane, is a disc of radius (the distance to the blockers) x tan(the
// sun's angular radius): that is the radius of the PCF's disc in the world. The cascade's light matrix turns it
// into the atlas: its rows base + 0 to 2 hold the gradients of u, v and the depth along the world's x, y and z
// (at right angles to one another, a rotation and a scale), so their lengths are the atlas uv per metre across
// the sun's rays (u and v: the texels are taller than wide) and the depth per metre along them (row 3's w is
// the factor the receiver's depth is multiplied by). The kernel is never narrower than minTexels atlas texels
// (the map cannot show a sharper edge) nor wider than maxRadius (the soft filter's twice).
float2 ShadowFilterPenumbra(int base, float gap, float sunTan, float texel, float maxRadius)
{
    float4 row0 = gLightMatrixArr(0 + base);
    float4 row1 = gLightMatrixArr(1 + base);
    float4 row2 = gLightMatrixArr(2 + base);
    float4 row3 = gLightMatrixArr(3 + base);
    float uPerMetre = length(float3(row0.x, row1.x, row2.x));
    float vPerMetre = length(float3(row0.y, row1.y, row2.y));
    float depthPerMetre = max(length(float3(row0.z, row1.z, row2.z)) * row3.w, 1.0e-9);
    float metres = gap / depthPerMetre * sunTan;
    float lowest = kShadowFilterMinTexels * texel;
    return clamp(metres * float2(uPerMetre, vPerMetre), float2(lowest, lowest), float2(maxRadius, maxRadius));
}

// The average of the game's four taps, replaced. tap1..tap4 are the game's tap coordinates (a cross: tap 1 at
// centre + 1.0 R u, tap 3 at centre - 0.5 R u, tap 2 at centre + 0.75 R v, tap 4 at centre - 0.25 R v, u and v
// at right angles, R = count / 1280): the centre and R come from taps 1 and 3. `cascadeBase` is the game's own
// a0 (4 x the cascade's index: the first row of its light matrix).
// Mode bit 2 (contact hardening) puts a blocker search first: 8 taps over a disc of twice the soft radius, the
// bilinear sampler's depth compared with the receiver's as the game does (nearer to the sun: a blocker). None
// found: lit, all found: in shadow (no more taps for the pixels that are not near an edge). Else the kernel is the
// penumbra of the average blocker (ShadowFilterPenumbra) and the PCF below runs on it, an ellipse along u and v.
float ShadowFilterSoft(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 tap1, float2 tap2,
                       float2 tap3, float2 tap4, float depth, float2 pixel, int cascadeBase)
{
    float4 constants = g_ShadowFilter;
    uint mode = uint(constants.w + 0.5);
    float2 centre = (tap1 + 2.0 * tap3) * (1.0 / 3.0);
    float radius = length(tap1 - tap3) * (1.0 / 1.5) * constants.y * kShadowFilterRadiusGain;
    int count = clamp(int(constants.x + 0.5), 4, 32);
    bool gather = (mode & 1u) != 0u;
    float angle = 6.28318530718 * ShadowFilterNoise(pixel, constants.z);
    float2 direction = float2(cos(angle), sin(angle));
    float2 kernel = float2(radius, radius);
    if ((mode & 2u) != 0u)
    {
        float maxRadius = kShadowFilterMaxRadii * radius;
        float blockerSum = 0.0;
        int blockers = 0;
        [loop]
        for (int j = 0; j < kShadowFilterSearchTaps; ++j)
        {
            float share = (float(j) + 0.5) / float(kShadowFilterSearchTaps);
            float stored = tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex,
                                       centre + direction * (sqrt(share) * maxRadius), float2(0.0, 0.0), 0.0).x;
            if (stored < depth)
            {
                blockerSum += stored;
                blockers += 1;
            }
            direction = float2(direction.x * kShadowFilterGoldenCos - direction.y * kShadowFilterGoldenSin,
                               direction.x * kShadowFilterGoldenSin + direction.y * kShadowFilterGoldenCos);
        }
        if (blockers == 0)
            return 1.0;
        if (blockers == kShadowFilterSearchTaps)
            return 0.0;
        float sunTan = float(mode >> 8u) * (kShadowFilterSunTan * (1.0 / 16.0));
        kernel = ShadowFilterPenumbra(cascadeBase, depth - blockerSum / float(blockers), sunTan,
                                      constants.y * (1.0 / 1280.0), maxRadius);
    }
    float sum = 0.0;
    float total = 0.0;
    [loop]
    for (int i = 0; i < count; ++i)
    {
        float share = (float(i) + 0.5) / float(count);  // r^2: equal areas
        float weight = 1.0 - 0.5 * share;
        sum += weight * ShadowFilterLookup(resourceDescriptorIndex, samplerDescriptorIndex,
                                           centre + direction * (sqrt(share) * kernel), depth, gather);
        total += weight;
        direction = float2(direction.x * kShadowFilterGoldenCos - direction.y * kShadowFilterGoldenSin,
                           direction.x * kShadowFilterGoldenSin + direction.y * kShadowFilterGoldenCos);
    }
    return sum / total;
}
)HLSL";

// ---------------------------------------------------------------------------
// Reading the translator's lines
// ---------------------------------------------------------------------------

// "r12.x": a register and what is picked of it. Nothing else counts (no spaces, no minus, no function).
struct Operand {
  std::string reg;      // "r12"
  std::string swizzle;  // "x", "xw", "xyzw"
};

bool ParseOperand(std::string_view text, Operand* out) {
  const size_t dot = text.find('.');
  if (dot == std::string_view::npos || dot < 2 || dot + 1 >= text.size() || text[0] != 'r') return false;
  for (size_t i = 1; i < dot; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  for (size_t i = dot + 1; i < text.size(); ++i) {
    if (text[i] != 'x' && text[i] != 'y' && text[i] != 'z' && text[i] != 'w') return false;
  }
  out->reg = std::string(text.substr(0, dot));
  out->swizzle = std::string(text.substr(dot + 1));
  return true;
}

struct LineAt {
  size_t begin = 0;  // of the line, at its first tab
  size_t end = 0;    // at the '\n' after it (or the end of the text)
  std::string_view indent;
  std::string_view code;  // after the indentation, up to the line's end
};

LineAt LineOf(const std::string& text, size_t position) {
  LineAt line;
  const size_t newline_before = text.rfind('\n', position == 0 ? 0 : position - 1);
  line.begin = (newline_before == std::string::npos || position == 0) ? 0 : newline_before + 1;
  const size_t newline_after = text.find('\n', position);
  line.end = newline_after == std::string::npos ? text.size() : newline_after;
  const std::string_view all = std::string_view(text).substr(line.begin, line.end - line.begin);
  size_t blanks = 0;
  while (blanks < all.size() && (all[blanks] == '\t' || all[blanks] == ' ')) ++blanks;
  line.indent = all.substr(0, blanks);
  line.code = all.substr(blanks);
  return line;
}

// One of the four fetches: `<dest> = tfetch2D(<atlas>, <sampler>, <coordinate>, float2(0, 0)).x;`
struct Tap {
  LineAt line;
  Operand dest;
  Operand coordinate;
};

bool ParseTap(const LineAt& line, Tap* tap) {
  tap->line = line;
  const std::string_view code = line.code;
  const size_t equals = code.find(" = ");
  if (equals == std::string_view::npos || !ParseOperand(code.substr(0, equals), &tap->dest)) return false;
  std::string_view rest = code.substr(equals + 3);
  if (!rest.starts_with(kLookupPrefix) || !rest.ends_with(kLookupSuffix)) return false;
  rest.remove_prefix(kLookupPrefix.size());
  rest.remove_suffix(kLookupSuffix.size());
  return ParseOperand(rest, &tap->coordinate);
}

// The second of the comma separated words of `float4 <name> = asfloat(uint4(a, b, c, d));`, the line that
// starts at `at`; empty when the line is not that.
std::string_view SecondConstantWord(std::string_view line) {
  const size_t open = line.find("uint4(");
  if (open == std::string_view::npos) return {};
  const size_t first = line.find(',', open);
  if (first == std::string_view::npos) return {};
  const size_t second = line.find(',', first + 1);
  if (second == std::string_view::npos) return {};
  std::string_view word = line.substr(first + 1, second - first - 1);
  while (!word.empty() && IsSpace(word.front())) word.remove_prefix(1);
  while (!word.empty() && IsSpace(word.back())) word.remove_suffix(1);
  return word;
}

// One edit at an offset of the text, made in decreasing offset order so that none moves another.
struct Insertion {
  size_t at = 0;
  size_t remove = 0;  // characters taken out at `at` before `text` goes in
  std::string text;
};

VariantTransformResult Failed(const VariantShader& shader, std::string error) {
  VariantTransformResult result;
  result.error = std::format("{}: {}", shader.name, error);
  return result;
}

VariantTransformResult Skipped(std::string_view why) {
  VariantTransformResult result;
  result.skip = true;
  result.error = std::string(why);
  return result;
}

}  // namespace

VariantTransformResult TransformShadowFilterHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  std::string text = NormalizeNewlines(hlsl);

  // 0. What kind of lookup it has: none, its own, or the rotated cross these edits are for.
  const size_t lookups = CountOccurrences(text, kLookupPrefix);
  if (lookups == 0) return Skipped(kShadowSkipNoLookup);
  if (text.find(kOwnFilterMarker) != std::string::npos) return Skipped(kShadowSkipOwnFilter);
  if (lookups != 4) {
    return Failed(shader, std::format("the atlas is read {} times (the rotated cross has 4 taps): the HLSL tree is "
                                      "not the one this transform was written for", lookups));
  }
  if (CountOccurrences(text, kRadiiLine) != 1) {
    return Failed(shader, "the line of the tap radii (c255) is not there exactly once: the HLSL tree is not the one "
                          "this transform was written for");
  }

  // 0b. What contact hardening reads: the cascade's light matrix, by the game's own address register
  // (`gLightMatrixArr(k + a0)`, k = 0 to 3: the gradients of u, v and the depth, and the offset).
  if (CountOccurrences(text, "int a0 = 0;") != 1) {
    return Failed(shader, "the address register `int a0 = 0;` is not declared exactly once: the HLSL tree is not "
                          "the one this transform was written for");
  }
  for (int row = 0; row < 4; ++row) {
    const std::string needle = std::format("gLightMatrixArr({} + a0)", row);
    if (CountOccurrences(text, needle) < 1) {
      return Failed(shader, std::format("`{}` is not read: the HLSL tree is not the one this transform was written "
                                        "for", needle));
    }
  }

  // 1. The four taps: each on a line of its own, in the order of the cross (coordinates .xw, .yz of one
  // register, .xy, .zw of another), into the four components of one register.
  std::vector<Tap> taps;
  for (size_t at = text.find(kLookupPrefix); at != std::string::npos; at = text.find(kLookupPrefix, at + 1)) {
    Tap tap;
    if (!ParseTap(LineOf(text, at), &tap)) {
      return Failed(shader, "a fetch of the atlas is not `rN.c = tfetch2D(atlas, sampler, rM.ab, float2(0, 0)).x;`: "
                            "the HLSL tree is not the one this transform was written for");
    }
    taps.push_back(std::move(tap));
  }
  static constexpr std::string_view kDestinations[4] = {"x", "y", "z", "w"};
  static constexpr std::string_view kCoordinates[4] = {"xw", "yz", "xy", "zw"};
  for (size_t i = 0; i < 4; ++i) {
    if (taps[i].dest.reg != taps[0].dest.reg || taps[i].dest.swizzle != kDestinations[i] ||
        taps[i].coordinate.swizzle != kCoordinates[i]) {
      return Failed(shader, std::format("tap {} is not the cross's (into {}.{}, from .{}): the HLSL tree is not "
                                        "the one this transform was written for",
                                        i + 1, taps[0].dest.reg, kDestinations[i], kCoordinates[i]));
    }
  }
  const std::string fetched = taps[0].dest.reg;

  // 2. The compare of the four with the receiver's depth, once, after the last tap: `rA.xyzw = rF.xyzw >=
  // rD.xxxx;`, and the average of its result with 0.25, once after that: `rE.x = dot(rA.wzxy, c253.yyyy);`.
  const std::string compare_needle = fetched + ".xyzw >= ";
  const size_t after_taps = taps[3].line.end;
  const std::string_view tail = std::string_view(text).substr(after_taps);
  if (CountOccurrences(tail, compare_needle) != 1) {
    return Failed(shader, std::format("`{}` is not there exactly once after the last tap: the HLSL tree is not the "
                                      "one this transform was written for", compare_needle));
  }
  const size_t compare_at = after_taps + tail.find(compare_needle);
  const LineAt compare_line = LineOf(text, compare_at);
  Operand compare_dest;
  std::string depth_reg;  // the register that holds the receiver's depth in .x
  {
    const std::string_view code = compare_line.code;
    const size_t equals = code.find(" = ");
    const size_t greater = code.find(" >= ");
    std::string_view depth_text;
    if (greater != std::string_view::npos) depth_text = code.substr(greater + 4);
    Operand probe;
    const bool shaped = equals != std::string_view::npos && greater != std::string_view::npos && greater > equals &&
                        depth_text.ends_with(".xxxx;") &&
                        ParseOperand(code.substr(0, equals), &compare_dest) && compare_dest.swizzle == "xyzw" &&
                        code.substr(equals + 3, greater - equals - 3) == fetched + ".xyzw" &&
                        ParseOperand(std::string(depth_text.substr(0, depth_text.size() - 6)) + ".x", &probe);
    if (!shaped) {
      return Failed(shader, "the compare of the four taps is not `rA.xyzw = rF.xyzw >= rD.xxxx;`: the HLSL tree is "
                            "not the one this transform was written for");
    }
    depth_reg = probe.reg;
  }
  const std::string average_needle = "dot(" + compare_dest.reg + ".wzxy, c253.yyyy);";
  const std::string_view after_compare = std::string_view(text).substr(compare_line.end);
  if (CountOccurrences(after_compare, average_needle) != 1) {
    return Failed(shader, std::format("`{}` is not there exactly once after the compare: the HLSL tree is not the "
                                      "one this transform was written for", average_needle));
  }
  const size_t average_at = compare_line.end + after_compare.find(average_needle);
  const LineAt average_line = LineOf(text, average_at);
  Operand average_dest;
  {
    const size_t equals = average_line.code.find(" = ");
    if (equals == std::string_view::npos || !ParseOperand(average_line.code.substr(0, equals), &average_dest) ||
        average_dest.swizzle != "x" || average_line.code.substr(equals + 3) != average_needle) {
      return Failed(shader, "the average of the four taps is not `rE.x = dot(rA.wzxy, c253.yyyy);`: the HLSL tree is "
                            "not the one this transform was written for");
    }
  }

  // 3. c253.y must be the 0.25 the average is made with.
  {
    const size_t at = text.find("float4 c253 = asfloat(uint4(");
    if (at == std::string::npos || SecondConstantWord(LineOf(text, at).code) != kQuarterConstant) {
      return Failed(shader, "c253.y is not 0.25 (0x3E800000): the HLSL tree is not the one this transform was "
                            "written for");
    }
  }

  // 4. The edits at offsets: the average becomes the helper's call, the taps and the depth are copied just
  // before the lines that overwrite or read them. In decreasing order of position.
  std::vector<Insertion> edits;
  {
    std::string call = std::string(average_line.indent) + average_dest.reg + ".x = ShadowFilterSoft(" +
                       "DepthTextureSampler0_Texture2DDescriptorIndex, DepthTextureSampler0_SamplerDescriptorIndex, "
                       "shadowTap1, shadowTap2, shadowTap3, shadowTap4, shadowDepth, iPos.xy, a0);";
    edits.push_back({average_line.begin, average_line.end - average_line.begin, std::move(call)});
    edits.push_back({compare_line.begin, 0,
                     std::string(compare_line.indent) + "shadowDepth = " + depth_reg + ".x;\n"});
    for (size_t i = 0; i < 4; ++i) {
      edits.push_back({taps[i].line.begin, 0,
                       std::format("{}shadowTap{} = {}.{};\n", taps[i].line.indent, i + 1, taps[i].coordinate.reg,
                                   taps[i].coordinate.swizzle)});
    }
  }
  std::sort(edits.begin(), edits.end(), [](const Insertion& a, const Insertion& b) { return a.at > b.at; });
  for (size_t i = 1; i < edits.size(); ++i) {
    if (edits[i].at + edits[i].remove > edits[i - 1].at) {
      return Failed(shader, "two edits of the lookup overlap: the HLSL tree is not the one this transform was "
                            "written for");
    }
  }
  for (const Insertion& edit : edits) text.replace(edit.at, edit.remove, edit.text);

  // 5. The anchored edits: the locals the copies go to, the constants and the helper.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  std::string error;
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n\tfloat2 shadowTap1 = 0.0, shadowTap2 = 0.0, shadowTap3 = 0.0, shadowTap4 = 0.0;"
                            "\n\tfloat shadowDepth = 0.0;";
                   },
                   &error)) {
    return Failed(shader, std::move(error));
  }
  static constexpr ConstantMember kMembers[] = {{"float4", "g_ShadowFilter"}};
  if (!DeclareConstants(text, ctx, "Shadow filter variant (src/gpu/shadow_filter.h ShadowFilterConstants): 16 bytes after the 608.", kMembers,
                        &error)) {
    return Failed(shader, std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    return Failed(shader, std::move(error));
  }

  // What the text must hold now: the helper's definition and its one call, one read of g_ShadowFilter (its declarations are
  // checked by DeclareConstants), and the four copies of the taps and the one of the depth.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"float ShadowFilterSoft(", 1}, {" = ShadowFilterSoft(", 1},
      {"float4 constants = g_ShadowFilter;", 1},
      {"\tshadowTap1 = ", 1},          {"\tshadowTap2 = ", 1},
      {"\tshadowTap3 = ", 1},          {"\tshadowTap4 = ", 1},
      {"\tshadowDepth = ", 1},        {"shadowDepth, iPos.xy, a0);", 1},
      {"gLightMatrixArr(0 + base)", 1}, {"float2 ShadowFilterPenumbra(", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return Failed(shader, std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count,
                                        check.want));
    }
  }
  VariantTransformResult result;
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeShadowFilterVariant() {
  VariantDef def;
  def.feature = kFeatureShadowFilter;
  def.name = "shadow_filter";
  def.version = kShadowFilterVariantVersion;
  def.pixel_name_suffixes = {kShadowBlendSuffix, kShadowBlendAlphaSuffix};
  def.needs_rpf = true;
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformShadowFilterHlsl(shader, hlsl, ctx);
  };
  return def;
}

}  // namespace larecomp::shaderpack
