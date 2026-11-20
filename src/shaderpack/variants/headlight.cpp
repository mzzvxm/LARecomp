// LARecomp shader pack builder -- variant "headlight": the lit shaders add the cars' headlights as spot lights (see headlight.h).

#include "shaderpack/variants/headlight.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
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

// The helper block, before main. The `kHl...` numbers are src/gpu/headlight_rig_rules.h's (LampScale is the CPU twin, headlight_test.cpp compares them).
// The constants are declared by the edits below it: g_HlHeader x = the light count, y = 0 (reserved), z = 0, w = 0; g_HlCamera xyz = the camera's world
// position; g_HlBoundsLo / g_HlBoundsHi = the box every light reaches; g_HlLight(3 i + 0) = position xyz and reach, (3 i + 1) = the axis xyz and the
// cosine of the outer half angle, (3 i + 2) = the colour rgb (times the strength and the fade) and the cosine of the inner half angle;
// g_HlShadowOf = the shadow view of each of the four lights (0xFFFFFFFF: none); g_HlAtlas.x / .y = the atlas texture's and its sampler's bindless slots
// (0xFFFFFFFF: none, every light is lit); g_HlShadow(6 v + 0..3) = the view's world_to_shadow rows, + 4 = its atlas rectangle (u0 v0 u1 v1), + 5 = the
// depth bias and the texel size as a share of the tile (src/gpu/local_shadow_types.h LocalShadowView).
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant headlight (src/shaderpack/variants/headlight.cpp): the cars' headlights as spot lights, one more term of the sum the shader multiplies
// by the surface. 672 bytes after the shared constants (src/gpu/headlight_rig.h HeadlightConstants).
static const float kHlPeak = 2.0;
static const float kHlFalloffDistance = 26.0;
static const float kHlSurfaceWrap = 0.45;
static const float kHlCutLow = 0.10;
static const float kHlCutHigh = 0.38;
static const float kHlNearStart = 0.8;
static const float kHlNearSpan = 1.5;
static const uint kHlNoView = 0xFFFFFFFFu;
#ifndef __spirv__
#define g_HlLight(INDEX) g_HlLights[INDEX]
#define g_HlShadow(INDEX) g_HlShadowArr[INDEX]
#endif

// The share of a light the shadow views let through at a world point: 4 compares in the atlas (the game's `>=`, the depth moved toward the lamp by the
// view's bias). A point outside the view's rectangle or depth range is lit.
float HeadlightShadow(uint view, float3 world)
{
    float4 row0 = g_HlShadow(view * 6u + 0u);
    float4 row1 = g_HlShadow(view * 6u + 1u);
    float4 row2 = g_HlShadow(view * 6u + 2u);
    float4 row3 = g_HlShadow(view * 6u + 3u);
    float4 rect = g_HlShadow(view * 6u + 4u);
    float4 shadowParams = g_HlShadow(view * 6u + 5u);
    float4 p = float4(world, 1.0);
    float4 clip = float4(dot(row0, p), dot(row1, p), dot(row2, p), dot(row3, p));
    if (clip.w <= 0.0)
        return 1.0;
    float3 s = clip.xyz / clip.w;
    if (s.x < rect.x || s.y < rect.y || s.x > rect.z || s.y > rect.w || s.z < 0.0 || s.z > 1.0)
        return 1.0;
    float depth = s.z - shadowParams.x;
    uint4 atlas = g_HlAtlas;
    float2 t = (rect.zw - rect.xy) * shadowParams.y;  // one texel of the tile in atlas uv, on each axis (the atlas is two tiles wide, not two tall)
    float lit = 0.0;
    lit += tfetch2DLod(atlas.x, atlas.y, s.xy + float2(-0.5, -0.5) * t, float2(0.0, 0.0), 0.0).x >= depth ? 1.0 : 0.0;
    lit += tfetch2DLod(atlas.x, atlas.y, s.xy + float2(0.5, -0.5) * t, float2(0.0, 0.0), 0.0).x >= depth ? 1.0 : 0.0;
    lit += tfetch2DLod(atlas.x, atlas.y, s.xy + float2(-0.5, 0.5) * t, float2(0.0, 0.0), 0.0).x >= depth ? 1.0 : 0.0;
    lit += tfetch2DLod(atlas.x, atlas.y, s.xy + float2(0.5, 0.5) * t, float2(0.0, 0.0), 0.0).x >= depth ? 1.0 : 0.0;
    return lit * 0.25;
}

// The headlights' light at a world point, rgb (the same units as a lamp's term: the shader adds it to the lamps and the ambient). Called at the start of
// main, where every lane is in step (the derivatives of the world position are taken before the early-out).
float4 HeadlightLight(float3 world)
{
    float3 dx = ddx(world);
    float3 dy = ddy(world);
    uint count = min(uint(g_HlHeader.x + 0.5), 4u);
    if (count == 0u || any(world < g_HlBoundsLo.xyz) || any(world > g_HlBoundsHi.xyz))
        return float4(0.0, 0.0, 0.0, 0.0);
    // The surface's own (geometric) normal, turned to the camera.
    float3 normal = cross(dy, dx);
    float normalLength2 = dot(normal, normal);
    normal = normalLength2 > 1.0e-14 ? normal * rsqrt(normalLength2) : float3(0.0, 1.0, 0.0);
    if (dot(normal, g_HlCamera.xyz - world) < 0.0)
        normal = -normal;
    uint4 shadowOf = g_HlShadowOf;
    uint4 atlas = g_HlAtlas;
    float3 sum = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        float4 a = g_HlLight(i * 3u);       // position, reach
        float4 b = g_HlLight(i * 3u + 1u);  // axis, cosine of the outer half angle
        float4 c = g_HlLight(i * 3u + 2u);  // colour, cosine of the inner half angle
        float3 toLight = a.xyz - world;
        float dist2 = dot(toLight, toLight);
        float range2 = a.w * a.w;
        if (dist2 >= range2 || dist2 < 1.0e-6)
            continue;
        float invDist = rsqrt(dist2);
        float3 l = toLight * invDist;           // from the surface to the lamp
        float cosine = -dot(l, b.xyz);          // the angle off the beam's axis
        if (cosine <= b.w)
            continue;
        float cone = smoothstep(b.w, c.w, cosine);
        float dist = dist2 * invDist;
        float falloff = 1.0 / (1.0 + dist2 * (1.0 / (kHlFalloffDistance * kHlFalloffDistance)));
        float window = saturate(1.0 - dist2 / range2);
        window *= window;
        float surface = saturate(kHlSurfaceWrap + (1.0 - kHlSurfaceWrap) * dot(normal, l));
        float cutoff = 1.0 - smoothstep(kHlCutLow, kHlCutHigh, -l.y);  // a low beam: nothing a little above the lamp
        float nearFade = saturate((dist - kHlNearStart) * (1.0 / kHlNearSpan));
        float scale = kHlPeak * cone * falloff * window * surface * cutoff * nearFade;
        uint view = shadowOf[i];
        if (view != kHlNoView && atlas.x != 0xFFFFFFFFu)
            scale *= HeadlightShadow(view, world);
        sum += c.rgb * scale;
    }
    return float4(sum, 0.0);
}
)HLSL";

// ---------------------------------------------------------------------------
// Reading the translator's lines
// ---------------------------------------------------------------------------

struct Line {
  size_t begin = 0;       // offset of the line's first character in the text
  size_t end = 0;         // offset of the '\n' after it (or the end of the text)
  size_t indent = 0;      // tabs and spaces before the code
  std::string_view code;  // the line without its indentation and trailing white space
};

std::vector<Line> LinesFrom(const std::string& text, size_t from) {
  std::vector<Line> lines;
  size_t at = from;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    Line line;
    line.begin = at;
    line.end = end;
    size_t first = at;
    while (first < end && (text[first] == '\t' || text[first] == ' ')) ++first;
    size_t last = end;
    while (last > first && IsSpace(text[last - 1])) --last;
    line.indent = first - at;
    line.code = std::string_view(text).substr(first, last - first);
    lines.push_back(line);
    at = end + 1;
  }
  return lines;
}

bool IsRegisterName(std::string_view name) {
  if (name.size() < 2 || name[0] != 'r') return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

bool AllIn(std::string_view text, std::string_view letters) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [&](char c) { return letters.find(c) != std::string_view::npos; });
}

// "r12.xz" -> "r12", "xz".
bool SplitOperand(std::string_view operand, std::string_view* reg, std::string_view* swizzle) {
  const size_t dot = operand.find('.');
  if (dot == std::string_view::npos) return false;
  *reg = operand.substr(0, dot);
  *swizzle = operand.substr(dot + 1);
  return IsRegisterName(*reg) && AllIn(*swizzle, "xyzw") && swizzle->size() <= 4;
}

// The destination of an assignment statement: the text before " = " when it is a register with a swizzle ("r5.xyzw"), else empty.
std::string_view DestinationOf(std::string_view code) {
  const size_t equals = code.find(" = ");
  if (equals == std::string_view::npos) return {};
  std::string_view reg, swizzle;
  const std::string_view dest = code.substr(0, equals);
  return SplitOperand(dest, &reg, &swizzle) ? dest : std::string_view();
}

bool WritesLane(std::string_view code, std::string_view reg, char comp) {
  const std::string_view dest = DestinationOf(code);
  if (dest.empty()) return false;
  std::string_view dreg, dswizzle;
  SplitOperand(dest, &dreg, &dswizzle);
  return dreg == reg && dswizzle.find(comp) != std::string_view::npos;
}

// `max(rS.c, rS.c)` (the translator's move through the scalar unit) -> rS.c.
bool ParseMaxCopy(std::string_view expr, std::string_view* reg, char* comp) {
  if (!expr.starts_with("max(") || !expr.ends_with(")")) return false;
  expr.remove_prefix(4);
  expr.remove_suffix(1);
  const size_t comma = expr.find(", ");
  if (comma == std::string_view::npos) return false;
  const std::string_view first = expr.substr(0, comma), second = expr.substr(comma + 2);
  std::string_view reg1, sw1;
  if (first != second || !SplitOperand(first, &reg1, &sw1) || sw1.size() != 1) return false;
  *reg = reg1;
  *comp = sw1[0];
  return true;
}

// Line `i` is a copy of one lane of another register into the lane `comp` of `reg`: `reg.comp = max(rS.c, rS.c);`, or `reg.comp = ps;` after
// `ps = max(rS.c, rS.c);`.
bool CopySource(const std::vector<Line>& lines, size_t i, std::string_view* source_reg, char* source_comp) {
  const std::string_view code = lines[i].code;
  const size_t equals = code.find(" = ");
  if (equals == std::string_view::npos || !code.ends_with(";")) return false;
  const std::string_view dest = code.substr(0, equals);
  std::string_view reg, swizzle;
  if (!SplitOperand(dest, &reg, &swizzle) || swizzle.size() != 1) return false;
  std::string_view rhs = code.substr(equals + 3);
  rhs.remove_suffix(1);
  if (rhs == "ps") {
    if (i == 0 || !lines[i - 1].code.starts_with("ps = ") || !lines[i - 1].code.ends_with(";")) return false;
    rhs = lines[i - 1].code.substr(5);
    rhs.remove_suffix(1);
  }
  return ParseMaxCopy(rhs, source_reg, source_comp);
}

struct Interpolant {
  int index = -1;  // iTexCoord<index>
  char comp = 0;
};

// Where the lane `comp` of `reg` comes from, at line `before`: an interpolant when nothing wrote the lane before (`float4 rS = iTexCoordK;`), or the
// lane the copies that wrote it take it from (every copy the same, the last write a copy).
bool ResolveLane(const std::vector<Line>& lines, const std::map<std::string, int>& interpolants, const std::string& reg, char comp, size_t before,
                 int depth, Interpolant* out) {
  std::vector<size_t> writes;
  for (size_t i = 0; i < before; ++i) {
    if (WritesLane(lines[i].code, reg, comp)) writes.push_back(i);
  }
  if (writes.empty()) {
    const auto found = interpolants.find(reg);
    if (found == interpolants.end()) return false;
    out->index = found->second;
    out->comp = comp;
    return true;
  }
  if (depth >= 2) return false;
  std::string_view source_reg;
  char source_comp = 0;
  if (!CopySource(lines, writes.back(), &source_reg, &source_comp)) return false;
  for (const size_t i : writes) {
    std::string_view r;
    char c = 0;
    if (!CopySource(lines, i, &r, &c)) continue;  // an earlier write that is not a copy (the colour's, in a branch): the last write decides
    if (r != source_reg || c != source_comp) return false;
  }
  return ResolveLane(lines, interpolants, std::string(source_reg), source_comp, writes.back(), depth + 1, out);
}

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

struct Insertion {
  size_t at = 0;
  size_t remove = 0;
  std::string text;
};

}  // namespace

std::string_view HeadlightHelperText() { return kHelper; }

VariantTransformResult TransformHeadlightHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  std::string text = NormalizeNewlines(hlsl);

  // 0. The body of main, line by line.
  const size_t main_at = text.find("\nvoid main(");
  if (main_at == std::string::npos) return Skipped(kHeadlightSkipNoGrid);
  const std::vector<Line> lines = LinesFrom(text, main_at + 1);

  // 1. The light grid lookup: `rD.zw = rS.xz * LightSortScaleBias.xy + LightSortScaleBias.zw;` exactly once.
  size_t grid_line = lines.size();
  size_t grids = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].code.find("LightSortScaleBias.") != std::string_view::npos) {
      ++grids;
      grid_line = i;
    }
  }
  if (grids != 1) return Skipped(kHeadlightSkipNoGrid);
  std::string grid_source;
  std::string grid_swizzle, grid_mul;
  {
    const std::string_view code = lines[grid_line].code;
    const size_t equals = code.find(" = ");
    const size_t times = code.find(" * LightSortScaleBias.");
    const size_t plus = code.find(" + LightSortScaleBias.");
    if (equals == std::string_view::npos || times == std::string_view::npos || plus == std::string_view::npos || times < equals || plus < times) {
      return Skipped(kHeadlightSkipNoGrid);
    }
    std::string_view reg, swizzle;
    if (!SplitOperand(code.substr(equals + 3, times - equals - 3), &reg, &swizzle) || swizzle.size() != 2) return Skipped(kHeadlightSkipNoGrid);
    const std::string_view mul = code.substr(times + std::string_view(" * LightSortScaleBias.").size(),
                                             plus - times - std::string_view(" * LightSortScaleBias.").size());
    if (mul.size() != 2 || !AllIn(mul, "xy")) return Skipped(kHeadlightSkipNoGrid);
    grid_source = std::string(reg);
    grid_swizzle = std::string(swizzle);
    grid_mul = std::string(mul);
  }

  // 2. The world position: the grid's x and z lanes lead to one interpolant; its remaining component is the height.
  char x_lane = 0, z_lane = 0;
  for (size_t i = 0; i < 2; ++i) {
    if (grid_mul[i] == 'x') x_lane = grid_swizzle[i];
    if (grid_mul[i] == 'y') z_lane = grid_swizzle[i];
  }
  if (x_lane == 0 || z_lane == 0) return Skipped(kHeadlightSkipPosition);
  std::map<std::string, int> interpolants;
  for (size_t i = 0; i < grid_line; ++i) {
    const std::string_view code = lines[i].code;
    if (!code.starts_with("float4 r") || !code.ends_with(";")) continue;
    const size_t equals = code.find(" = iTexCoord");
    if (equals == std::string_view::npos) continue;
    const std::string_view reg = code.substr(7, equals - 7);
    std::string_view digits = code.substr(equals + std::string_view(" = iTexCoord").size());
    digits.remove_suffix(1);
    if (!IsRegisterName(reg) || digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
    interpolants[std::string(reg)] = std::stoi(std::string(digits));
  }
  Interpolant x_source, z_source;
  if (!ResolveLane(lines, interpolants, grid_source, x_lane, grid_line, 0, &x_source) ||
      !ResolveLane(lines, interpolants, grid_source, z_lane, grid_line, 0, &z_source) || x_source.index != z_source.index ||
      x_source.comp == z_source.comp || !AllIn(std::string_view(&x_source.comp, 1), "xyz") || !AllIn(std::string_view(&z_source.comp, 1), "xyz")) {
    return Skipped(kHeadlightSkipPosition);
  }
  char y_comp = 0;
  for (const char c : std::string_view("xyz")) {
    if (c != x_source.comp && c != z_source.comp) y_comp = c;
  }
  const std::string position = std::format("iTexCoord{}.{}{}{}", x_source.index, x_source.comp, y_comp, z_source.comp);

  // 3. The statements that add the ambient colour: each at one tab of indentation, `dest = ... gLightAmbient.<swizzle> ...;`, the channels of all of
  // them x, y and z once each. A statement about the alpha (gLightAmbient.w alone) is not one.
  struct AmbientEdit {
    size_t line = 0;
    std::string swizzle;
  };
  std::vector<AmbientEdit> ambient;
  std::string channels;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string_view code = lines[i].code;
    const size_t at = code.find("gLightAmbient.");
    if (at == std::string_view::npos) continue;
    size_t end = at + std::string_view("gLightAmbient.").size();
    size_t length = 0;
    while (end + length < code.size() && std::string_view("xyzw").find(code[end + length]) != std::string_view::npos) ++length;
    const std::string swizzle(code.substr(end, length));
    if (swizzle == "w") continue;
    if (swizzle.empty() || code.find("gLightAmbient.", at + 1) != std::string_view::npos || code.starts_with("oC0") ||
        code.find("select(") != std::string_view::npos || code.find("max(gLightAmbient") != std::string_view::npos || lines[i].indent != 1 ||
        !code.ends_with(";")) {
      return Skipped(kHeadlightSkipAmbient);
    }
    const size_t equals = code.find(" = ");
    if (equals == std::string_view::npos || equals > at) return Skipped(kHeadlightSkipAmbient);
    const std::string_view dest = code.substr(0, equals);
    if (dest == "ps") {
      if (swizzle.size() != 1) return Skipped(kHeadlightSkipAmbient);
    } else {
      std::string_view reg, lanes;
      if (!SplitOperand(dest, &reg, &lanes) || lanes.size() != swizzle.size()) return Skipped(kHeadlightSkipAmbient);
    }
    for (const char c : swizzle) {
      if (c == 'w' || channels.find(c) != std::string::npos) return Skipped(kHeadlightSkipAmbient);
      channels += c;
    }
    ambient.push_back({i, swizzle});
  }
  if (channels.size() != 3) return Skipped(kHeadlightSkipAmbient);

  // 4. The edits at offsets, in decreasing order: each statement of the ambient gets `+ hlLight.<swizzle>` before its semicolon.
  std::vector<Insertion> edits;
  for (const AmbientEdit& edit : ambient) {
    const Line& line = lines[edit.line];
    const size_t semicolon = line.begin + line.indent + line.code.size() - 1;
    edits.push_back({semicolon, 1, std::format(" + hlLight.{};", edit.swizzle)});
  }
  std::sort(edits.begin(), edits.end(), [](const Insertion& a, const Insertion& b) { return a.at > b.at; });
  for (const Insertion& edit : edits) text.replace(edit.at, edit.remove, edit.text);

  // 5. The anchored edits: the local the light goes to, the constants and the helper.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  std::string error;
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [&](std::string_view group) {
                     return std::string(group) + "\n\tfloat4 hlLight = HeadlightLight(" + position + ");";
                   },
                   &error)) {
    return Failed(shader, std::move(error));
  }
  // The block of constants (672 bytes: src/gpu/headlight_rig.h HeadlightConstants), declared at the place the chain gives it: byte 608 alone, after the blocks of
  // the stages before it in a composition (the headlight is applied before the G-buffer's: variant_chain.h).
  static constexpr ConstantMember kMembers[] = {
      {"float4", "g_HlHeader"}, {"float4", "g_HlCamera"}, {"float4", "g_HlBoundsLo"}, {"float4", "g_HlBoundsHi"},
      {"float4", "g_HlLights", 12, "g_HlLight(INDEX)"},
      {"uint4", "g_HlShadowOf"}, {"uint4", "g_HlAtlas"},
      {"float4", "g_HlShadowArr", 24, "g_HlShadow(INDEX)"}};  // 64 + 192 + 32 + 384 = 672, the table's constants_bytes
  if (!DeclareConstants(text, ctx, "Headlight variant (src/gpu/headlight_rig.h HeadlightConstants): 672 bytes after the 608.", kMembers, &error)) {
    return Failed(shader, std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    return Failed(shader, std::move(error));
  }

  // What the text must hold now: the helper's definitions and the one call, the declarations once in each backend, one local, and the term in as many
  // statements as the ambient had (and in no other).
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"float4 HeadlightLight(", 1},      {"float HeadlightShadow(", 1},
      {" = HeadlightLight(", 1},          {"float4 hlLight = ", 1},
      {"hlLight.", ambient.size()},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      return Failed(shader, std::format("the transformed HLSL has '{}' {} times (need {})", check.needle, count, check.want));
    }
  }
  VariantTransformResult result;
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeHeadlightVariant() {
  VariantDef def;
  def.feature = kFeatureHeadlight;
  def.name = "headlight";
  def.version = kHeadlightVariantVersion;
  for (const std::string_view suffix : kHeadlightSuffixes) def.pixel_name_suffixes.push_back(suffix);
  def.needs_rpf = true;
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) { return TransformHeadlightHlsl(shader, hlsl, ctx); };
  return def;
}

}  // namespace larecomp::shaderpack
