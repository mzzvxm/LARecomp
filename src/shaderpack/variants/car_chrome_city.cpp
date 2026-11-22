// LARecomp shader pack builder -- variant "car_chrome_city": the car's chrome reflects the city (see car_chrome_city.h).

#include "shaderpack/variants/car_chrome_city.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The line the chrome's stage (car_chrome.cpp kAfterEnvRead) writes after the environment read, and its replacement.
constexpr std::string_view kEnvCall = "r1.xyz = CarChromeEnv(r1.xyz, carChrome);";
constexpr std::string_view kCityCall = "r1.xyz = CarChromeCityEnv(r1.xyz, carChrome);";

// The helper, before main (after the chrome's own, whose types and functions it uses: CarChrState, CarChromeEnv, kCarChrDark, the heap and TFETCH_INDEX).
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant car_chrome_city (src/shaderpack/variants/car_chrome_city.cpp, CITY-RT M2 R3a): the chrome's reflection of the city and of the cars beside the player, read from the ray traced image
// (src/gpu/car_rt_refl.h) as the car paint's variant reads it (colour mean of the cone's hits, weight the share of the cone, the sign of the alpha "mostly on another car or the city"). The
// numbers are the rules header's (src/gpu/car_chrome_rt_rules.h ReadForChromeCity, ChromeCityEnv, kCityMaxColour): keep the two in step.
static const float kCarChrCityMaxColour = 64.0;   // the paint's cap on a reflected colour, in the paint's units
static const float kCarChrCityModeMin = 1.5;      // g_CarReflA.x from which the self-reflection replaces colour (the paint's mode 2); under it the chrome only occludes

struct CarChrCityRead
{
    float use;       // 1: the image answers here with the player's own texels (at least half of the footprint, as the chrome's read)
    float weight;    // 0..1: the share of the cone that hit anything (the chrome's own read of it)
    float other;     // 0..1: the share of that weight whose alpha has the other-car sign: the city, or a car beside the player
    float3 colour;   // the mean colour of the hits, in the history's units
};

// The chrome's 3x3 B-spline read of the image (CarChrRtRead, the player's kind), with the colour and the sign kept.
CarChrCityRead CarChrCityLookup(Texture2D<float4> history, float2 size, float2 uv, float depth, float tolerance, uint slot)
{
    CarChrCityRead r = (CarChrCityRead)0;
    Texture2D<float4> image = g_Texture2DDescriptorHeap[TFETCH_INDEX(slot)];
    const float2 f = uv * size - 0.5;
    const float2 nearest = floor(f + 0.5);
    const float2 d = f - nearest;
    const float3 wx = float3(0.5 * (0.5 - d.x) * (0.5 - d.x), 0.75 - d.x * d.x, 0.5 * (0.5 + d.x) * (0.5 + d.x));
    const float3 wy = float3(0.5 * (0.5 - d.y) * (0.5 - d.y), 0.75 - d.y * d.y, 0.5 * (0.5 + d.y) * (0.5 + d.y));
    const int2 last = int2(size) - 1;
    float geometry = 0.0;
    float known = 0.0;
    float weight = 0.0;
    float other = 0.0;
    float3 colour = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 offset = int2(k % 3, k / 3) - 1;
        const int2 texel = clamp(int2(nearest) + offset, int2(0, 0), last);
        const float wb = wx[k % 3] * wy[k / 3];
        const float4 h = history.Load(int3(texel, 0));
        const float agree = (h.a > 0.0) ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(h.a - depth)) : 0.0;
        const float wg = wb * agree;
        geometry += wg;
        const float4 t = image.Load(int3(texel, 0));
        const float code = abs(t.a);
        if (code >= 0.5 && !(t.r < -0.5))
        {
            const float wt = saturate(code - 1.0);
            known += wg;
            weight += wg * wt;
            other += (t.a < 0.0) ? wg * wt : 0.0;
            colour += wg * wt * t.rgb;
        }
    }
    if (geometry > 1e-4 && known >= 0.5 * geometry)
    {
        r.use = 1.0;
        r.weight = weight / known;
        r.other = (weight > 1e-5) ? saturate(other / weight) : 0.0;
        r.colour = (weight > 1e-5) ? colour / weight : float3(0.0, 0.0, 0.0);
    }
    return r;
}

// `env` (the sharp environment read) for the chrome: the car's own parts occlude it as the chrome variant does, the city and the cars beside replace it with what they look like. Anything the
// image does not answer for the player's own texel keeps the chrome variant's own line.
float3 CarChromeCityEnv(float3 env, CarChrState s)
{
    if (s.state < 4.5 || s.state > 5.5 || g_CarReflA.x < kCarChrCityModeMin)
    {
        return CarChromeEnv(env, s);
    }
    Texture2D<float4> history = g_Texture2DDescriptorHeap[TFETCH_INDEX(g_CarReflC.x)];
    const CarChrCityRead c = CarChrCityLookup(history, s.ray.size, s.ray.startUv, s.ray.depth, 2.0 * (0.004 + 0.0015 * s.ray.depth + s.ray.slack), g_CarReflC.z);
    if (c.use < 0.5 || c.other <= 0.0)
    {
        return CarChromeEnv(env, s);
    }
    const float strength = g_CarReflA.w;
    const float whole = c.weight * s.agreement;
    const float city = whole * c.other;
    const float own = whole - city;
    const float3 base = env * (1.0 - kCarChrDark * saturate(own * strength));
    const float3 seen = min(max(c.colour, 0.0) * g_CarReflB.z, kCarChrCityMaxColour);
    return lerp(base, seen, saturate(city * strength));
}
)HLSL";

}  // namespace

VariantTransformResult TransformCarChromeCityHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  VariantTransformResult result;
  if (shader.name != kChrome.name) {
    result.error = std::format("{} is not the chrome this variant was written for", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  // Without the chrome's stage in the text there is nothing to rewrite: the game's own text (the pack's entry for this stage alone, which the compositions are built from).
  if ((ctx.features & chain::FeatureBit(kFeatureCarChrome)) == 0) {
    result.ok = true;
    result.hlsl = std::move(text);
    return result;
  }
  std::string error;
  // 1. The line the chrome's stage wrote for `env`, exactly once.
  if (!ReplaceAllExactly(text, kEnvCall, kCityCall, 1, "the chrome stage's line for `env`", &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  // 2. The helper before main (after the helpers the earlier stages put there).
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0), Lit("#endif"), Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main", [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  // What the text must hold now: the helper's definitions and calls once, the chrome stage's call nowhere, the old arithmetic of the chrome's line only inside the helper's fallbacks.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"float3 CarChromeCityEnv(", 1},
      {"CarChromeCityEnv(r1.xyz, carChrome)", 1},
      {"CarChromeEnv(r1.xyz, carChrome)", 0},
      {"CarChrCityRead CarChrCityLookup(", 1},
      {"= CarChrCityLookup(", 1},
      {"return CarChromeEnv(env, s);", 2},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      result.error = std::format("{}: the transformed HLSL has '{}' {} times (need {})", shader.name, check.needle, count, check.want);
      return result;
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeCarChromeCityVariant() {
  VariantDef def;
  def.feature = kFeatureCarChromeCity;
  def.name = "car_chrome_city";
  def.version = kCarChromeCityVariantVersion;
  def.needs_rpf = true;  // xChrome lives in xarchive_cache.rpf (cars), as the chrome variant's
  def.pixel_shaders = {kChrome};
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformCarChromeCityHlsl(shader, hlsl, ctx);
  };
  return def;
}

}  // namespace larecomp::shaderpack
