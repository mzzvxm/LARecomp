// LARecomp shader pack builder -- variant "car_selfrefl": the player's car paint reflects its own car (see car_selfrefl.h).

#include "shaderpack/variants/car_selfrefl.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The two environment reads of the paint and the last write of its colour, as the translator writes them.
constexpr std::string_view kSharpRead =
    "r10.xyz = tfetch2DGrad(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r1.yw, "
    "float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyz;";
constexpr std::string_view kBlurredRead =
    "r13.xyz = tfetch2D(dpSpecFrontSampler_Texture2DDescriptorIndex, dpSpecFrontSampler_SamplerDescriptorIndex, r0.xy, "
    "float2(0, 0)).xyz;";
constexpr std::string_view kFinalWrite = "oC0.xyz = r3.www * -r0.xzy + r0.xzy;";

// What follows each of them. The trace runs once, after the sharp read: the world position (iTexCoord1), the normal
// (iTexCoord2) and the view vector (iTexCoord6, with N.V in .w) are the vertex shader's outputs in the order the pixel
// shader receives them (the VS writes oTexCoord1.xyz = (x, y, z) of the world position, oTexCoord2 the world normal of
// the car's matrices, oTexCoord6 = (V, N.V) with V towards the camera: xCarPaintCustomizable__VS_CarPaint.hlsl).
constexpr std::string_view kAfterSharpRead =
    "\n\tcarRefl = CarReflTrace(iTexCoord1.xyz, iTexCoord2.xyz, iTexCoord6.xyz, iPos.xy);"
    "\n\tr10.xyz = CarReflApply(r10.xyz, carRefl, 1.0);";
constexpr std::string_view kAfterBlurredRead = "\n\tr13.xyz = CarReflApply(r13.xyz, carRefl, 0.5);";
// The debug view: replaces the colour; iTexCoord4.w is the baked vertex AO (r4.w of the game's lines).
constexpr std::string_view kAfterFinalWrite =
    "\n\t[branch] if (g_CarReflB.y > 0.5) { oC0.xyz = CarReflDebug(carRefl, iTexCoord4.w); }";

// The helper and its types, before main. The constants are declared by the two edits below it.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant car_selfrefl (src/shaderpack/variants/car_selfrefl.cpp): the paint's two environment reads occluded by,
// or replaced with, the car's own colour of the previous frame. The history image (g_CarReflC.x): rgb the frame's scene colour,
// a the view distance of the pixels of a vehicle (0 for everything else). g_CarReflM0..M3 take a world position of this frame
// into the history frame's clip space (the vehicle's own motion is in them). g_CarReflA: x mode (0 off, 1 darken, 2 replace),
// y ray length (m), z thickness (m), w strength. g_CarReflB: x steps, y debug view, z colour gain, w noise frame.
struct CarReflResult
{
    float3 colour;  // what the ray found, in the paint's own units
    float hit;      // 0..1: the share of the reflection the hit takes over
    float dist;     // the hit's distance as a share of the ray length
    float state;    // debug: 0 off, 1 the shaded point is not in the history, 2 the ray left the picture, 3 no hit, 4 hit
};

// Interleaved gradient noise of a host pixel, plus the R2 shift of the frame (as the shadow filter's).
float CarReflNoise(float2 pixel, float frame)
{
    float gradient = frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715))));
    return frac(gradient + frame * 0.75487766624669);
}

float4 CarReflProject(float4 m0, float4 m1, float4 m2, float4 m3, float3 p)
{
    return p.x * m0 + p.y * m1 + p.z * m2 + m3;
}

float2 CarReflUv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float4 CarReflLoad(Texture2D<float4> history, float2 size, float2 uv)
{
    return history.Load(int3(int2(clamp(uv * size, float2(0.0, 0.0), size - 1.0)), 0));
}

// The ray point of view distance `depth` is just behind a vehicle pixel whose distance is `stored`.
bool CarReflHit(float depth, float stored, float thickness)
{
    float diff = depth - stored;
    return stored > 0.0 && diff > 0.01 + 0.004 * depth && diff < thickness;
}

CarReflResult CarReflTrace(float3 world, float3 normal, float3 viewVector, float2 pixel)
{
    CarReflResult result = (CarReflResult)0;
    const float4 settings = g_CarReflA;
    if (settings.x < 0.5)
    {
        return result;
    }
    const float4 more = g_CarReflB;
    const uint4 slots = g_CarReflC;
    const float4 m0 = g_CarReflM0;
    const float4 m1 = g_CarReflM1;
    const float4 m2 = g_CarReflM2;
    const float4 m3 = g_CarReflM3;
    const float normalSquare = dot(normal, normal);
    const float viewSquare = dot(viewVector, viewVector);
    if (normalSquare < 1e-8 || viewSquare < 1e-8)
    {
        return result;
    }
    const float3 n = normal * rsqrt(normalSquare);
    const float3 v = viewVector * rsqrt(viewSquare);
    const float nv = dot(n, v);
    if (nv < 0.03)
    {
        return result;
    }
    const float3 r = 2.0 * nv * n - v;  // the view ray reflected: where the reflection looks

    Texture2D<float4> history = g_Texture2DDescriptorHeap[TFETCH_INDEX(slots.x)];
    const float2 size = float2(getTexture2DDimensions(history));

    // 1. Is the shaded point in the history, at the distance it has now? A disocclusion, a wrong guess of the car's
    // motion or a cut puts it elsewhere: the whole effect fades with the disagreement.
    const float4 start = CarReflProject(m0, m1, m2, m3, world);
    result.state = 1.0;
    if (start.w < 0.1)
    {
        return result;
    }
    const float2 startUv = CarReflUv(start);
    if (any(startUv < float2(0.0, 0.0)) || any(startUv > float2(1.0, 1.0)))
    {
        return result;
    }
    const float storedStart = CarReflLoad(history, size, startUv).a;
    const float tolerance = 0.06 + 0.02 * start.w;
    const float agreement = 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(storedStart - start.w));
    if (!(storedStart > 0.0) || agreement <= 0.0)
    {
        return result;
    }

    // 2. The march: `steps` points along the ray, the first a fraction of a step from the surface (jittered by the noise).
    const float rayLength = max(settings.y, 0.05);
    const int steps = clamp(int(more.x + 0.5), 4, 24);
    const float stepLength = rayLength / float(steps);
    const float thickness = max(settings.z, 1.2 * stepLength);
    const float jitter = CarReflNoise(pixel, more.w);
    float hitT = -1.0;
    result.state = 3.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float t = (float(i) + 0.25 + 0.75 * jitter) * stepLength;
        const float4 c = CarReflProject(m0, m1, m2, m3, world + r * t);
        const float2 uv = CarReflUv(c);
        if (c.w < 0.1 || any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
        {
            result.state = 2.0;
            break;
        }
        if (CarReflHit(c.w, CarReflLoad(history, size, uv).a, thickness))
        {
            hitT = t;
            break;
        }
    }

    // 3. The hit: three halvings of the last step, then its colour and the weight.
    if (hitT > 0.0)
    {
        float lo = max(hitT - stepLength, 0.0);
        float hi = hitT;
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            const float mid = 0.5 * (lo + hi);
            const float4 c = CarReflProject(m0, m1, m2, m3, world + r * mid);
            if (c.w >= 0.1 && CarReflHit(c.w, CarReflLoad(history, size, CarReflUv(c)).a, thickness))
            {
                hi = mid;
            }
            else
            {
                lo = mid;
            }
        }
        const float4 c = CarReflProject(m0, m1, m2, m3, world + r * hi);
        const float2 uv = CarReflUv(c);
        const float share = hi / rayLength;
        const float edge = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 12.5);
        const float rayFade = 1.0 - smoothstep(0.55, 1.0, share);
        const float3 colour = history.SampleLevel(g_SamplerDescriptorHeap[slots.y], uv, 0.0).rgb;
        result.colour = min(max(colour, 0.0) * more.z, 64.0);
        result.dist = share;
        result.hit = agreement * edge * rayFade;
        result.state = 4.0;
    }
    return result;
}

// The reflection `env` (the sharp read, share 1; the blurred one, share 0.5) with the hit applied.
float3 CarReflApply(float3 env, CarReflResult hit, float share)
{
    if (hit.hit <= 0.0)
    {
        return env;
    }
    const float4 settings = g_CarReflA;
    const float weight = saturate(hit.hit * settings.w * share);
    if (settings.x > 1.5)
    {
        return lerp(env, hit.colour, weight);
    }
    return env * (1.0 - 0.85 * weight);
}

// The debug views of g_CarReflB.y: 1 the hit weight, 2 the colour the ray found, 3 the paint's baked vertex AO, 4 the
// state of the trace (red: the point is not in the history, blue: the ray left the picture, green: no hit, yellow: a hit,
// brighter the nearer). The write of the colour is scaled by the exponent bias of the target, so they are in its units.
float3 CarReflDebug(CarReflResult hit, float bakedAo)
{
    const uint view = uint(g_CarReflB.y + 0.5);
    const float unit = gInvColorExpBias.x;
    if (view == 1u)
    {
        return float3(hit.hit, hit.hit, hit.hit) * unit;
    }
    if (view == 2u)
    {
        return hit.colour;
    }
    if (view == 3u)
    {
        return float3(bakedAo, bakedAo, bakedAo) * unit;
    }
    float3 colour = float3(0.0, 0.0, 0.0);
    if (hit.state > 3.5)
    {
        colour = float3(1.0, 1.0, 0.0) * (1.0 - 0.7 * hit.dist);
    }
    else if (hit.state > 2.5)
    {
        colour = float3(0.0, 0.5, 0.0);
    }
    else if (hit.state > 1.5)
    {
        colour = float3(0.0, 0.0, 1.0);
    }
    else if (hit.state > 0.5)
    {
        colour = float3(1.0, 0.0, 0.0);
    }
    return colour * unit;
}
)HLSL";

}  // namespace

VariantTransformResult TransformCarSelfReflHlsl(const VariantShader& shader, std::string_view hlsl) {
  VariantTransformResult result;
  if (shader.name != kCarPaint.name) {
    result.error = std::format("{} is not the car paint this variant was written for", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string error;

  // 1. The three lines the edits follow: each exactly once.
  if (!ReplaceAllExactly(text, kSharpRead, std::string(kSharpRead) + std::string(kAfterSharpRead), 1,
                         "the sharp environment read", &error) ||
      !ReplaceAllExactly(text, kBlurredRead, std::string(kBlurredRead) + std::string(kAfterBlurredRead), 1,
                         "the blurred environment read", &error) ||
      !ReplaceAllExactly(text, kFinalWrite, std::string(kFinalWrite) + std::string(kAfterFinalWrite), 1,
                         "the last write of the colour", &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // 2. The anchored edits: the local result, the constants of both backends and the helper.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kSpirvDeclaration[] = {
      Lit("#define g_InvResolutionScale"), Ws(1),
      Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tCarReflResult carRefl = (CarReflResult)0;";
                   },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n// Car self-reflection variant (src/gpu/constants.h CarSelfReflConstants): 112 bytes after the 608.\n"
                            "#define g_CarReflM0 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)\n"
                            "#define g_CarReflM1 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 624, 0x10)\n"
                            "#define g_CarReflM2 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 640, 0x10)\n"
                            "#define g_CarReflM3 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 656, 0x10)\n"
                            "#define g_CarReflA vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 672, 0x10)\n"
                            "#define g_CarReflB vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 688, 0x10)\n"
                            "#define g_CarReflC vk::RawBufferLoad<uint4>(g_PushConstants.SharedConstants + 704, 0x10)";
                   },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group.substr(0, group.size() - 1)) +
                            "; \\\n    float4 g_CarReflM0 : packoffset(c38); \\\n"
                            "    float4 g_CarReflM1 : packoffset(c39); \\\n"
                            "    float4 g_CarReflM2 : packoffset(c40); \\\n"
                            "    float4 g_CarReflM3 : packoffset(c41); \\\n"
                            "    float4 g_CarReflA : packoffset(c42); \\\n"
                            "    float4 g_CarReflB : packoffset(c43); \\\n"
                            "    uint4 g_CarReflC : packoffset(c44);";
                   },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // What the text must hold now: the helper's definitions and their calls, one declaration of each constant in
  // each backend's declaration, and the game's three lines nowhere without their edit.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"CarReflResult CarReflTrace(", 1}, {"= CarReflTrace(", 1},
      {"float3 CarReflApply(", 1},        {"CarReflApply(r10.xyz, carRefl, 1.0)", 1},
      {"CarReflApply(r13.xyz, carRefl, 0.5)", 1},
      {"float3 CarReflDebug(", 1},        {"CarReflDebug(carRefl, iTexCoord4.w)", 1},
      {"CarReflResult carRefl = (CarReflResult)0;", 1},
      {"SharedConstants + 608", 1},       {"SharedConstants + 704", 1},
      {"g_CarReflM0 : packoffset(c38)", 1}, {"g_CarReflC : packoffset(c44)", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      result.error = std::format("{}: the transformed HLSL has '{}' {} times (need {})", shader.name, check.needle,
                                 count, check.want);
      return result;
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeCarSelfReflVariant() {
  VariantDef def;
  def.id = kVariantCarSelfRefl;
  def.name = "car_selfrefl";
  def.version = kCarSelfReflVariantVersion;
  def.needs_rpf = true;  // xCarPaintCustomizable lives in xarchive_cache.rpf (cars)
  def.pixel_shaders = {kCarPaint};
  def.transform = [](const VariantShader& shader, std::string_view hlsl) {
    return TransformCarSelfReflHlsl(shader, hlsl);
  };
  return def;
}

}  // namespace larecomp::shaderpack
