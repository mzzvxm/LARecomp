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
    "\n\tcarRefl = CarReflTrace(iTexCoord1.xyz, iTexCoord2.xyz, iTexCoord6.xyz);"
    "\n\tr10.xyz = CarReflApply(r10.xyz, carRefl, 1.0);";
constexpr std::string_view kAfterBlurredRead = "\n\tr13.xyz = CarReflApply(r13.xyz, carRefl, 0.5);";
// The debug view: replaces the colour; iTexCoord4.w is the baked vertex AO (r4.w of the game's lines).
constexpr std::string_view kAfterFinalWrite =
    "\n\t[branch] if (g_CarReflB.y > 0.5) { oC0.xyz = CarReflDebug(carRefl, iTexCoord4.w); }";

// The helper and its types, before main. The constants are declared by the two edits below it.
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant car_selfrefl (src/shaderpack/variants/car_selfrefl.cpp): the paint's two environment reads occluded by,
// or replaced with, the car's own colour of the previous frame. The history image (g_CarReflC.x, the picture's size or half of
// it): a the view distance of the pixels of a vehicle (0 for everything else), rgb the frame's scene colour of those pixels
// (0 elsewhere: the host pass reads the colour of a vehicle's pixels only, hlsl/car_history_ps.hlsl), both read as points
// (no filtering: a filtered read at a vehicle's edge would mix its colour and distance with the zeros of the world's).
// g_CarReflM0..M3 take a world position of this frame
// into the history frame's clip space (the vehicle's own motion is in them). g_CarReflA: x mode (0 off, 1 darken, 2 replace),
// y ray length (m), z thickness (m), w strength. g_CarReflB: x steps, y debug view, z colour gain, w not read (it was the
// noise frame: the trace has no per-pixel noise any more, a pixel's samples are the same in every frame).
// The numbers below are also written as plain functions in src/gpu/car_selfrefl_rules.h (RayDistance, WindowWeight, Approach,
// ThinFactor, SurfaceSlack), which src/gpu/car_selfrefl_rules_test.cpp checks: keep the two in step.
static const float kCarReflStartMin = 0.04;   // m: no sample is nearer the shaded point than this
static const float kCarReflDark = 0.85;       // the most of the reflection a hit takes away (mode 1)
static const float kCarReflBackFloor = 0.5;   // the weight of a hit the ray reached from behind the surface, against 1 from the front
static const float kCarReflThinFloor = 0.6;   // the weight of a hit that one sample or two small ones make, against 1 for a wide one
static const float kCarReflShade = 0.4;       // the paint's own colour in shade, for the face the camera does not see (mode 2)

struct CarReflResult
{
    float3 colour;  // what the ray found, in the paint's own units
    float3 own;     // the shaded point's own colour of the previous frame, in the same units
    float hit;      // 0..1: the share of the reflection the hit takes over
    float face;     // 0..1: 1 the ray met the surface the camera sees from its front, 0 it reached it from behind (a hidden face)
    float dist;     // the hit's distance as a share of the ray length
    float state;    // debug: 0 off, 1 the shaded point is not in the history, 2 the ray left the picture, 3 no hit, 4 hit
};

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

// The distance of the ray's sample `index` (0 to steps - 1): the samples are closer together near the shaded point, where
// the parts that matter are (a wing over a trunk lid, a mirror over a door), and the same for every pixel and every frame.
float CarReflDistance(int index, int steps, float rayLength)
{
    const float u = float(index + 1) / float(steps);
    return max(rayLength * u * sqrt(u), kCarReflStartMin);
}

// How far off the stored distance of a slanted surface can be: the history is read as points, so it holds the surface's
// distance at a texel's centre, up to a texel from where the ray point projects, and a surface seen at a slant changes its
// distance by `slope` metres per metre across the picture. `focal` is the projection's scale (the matrix's y row).
float CarReflSlack(float depth, float historyHeight, float focal, float nv)
{
    const float texel = depth * 2.0 / (historyHeight * max(focal, 0.001));
    const float slope = min(sqrt(max(1.0 - nv * nv, 0.0)) / max(nv, 0.1), 8.0);
    return min(1.5 * texel * slope, 0.05);
}

// 0..1: how much a ray point of view distance `depth` is "just behind" a vehicle pixel whose distance is `diff` less. Not a yes
// or no: it rises from 0 at the bias (the game's depth noise, and the slack of a slanted surface) to 1 over a short ramp, stays
// there, and falls to 0 between half the thickness and the thickness (a ray that is far behind a thin shell has passed it).
float CarReflWindow(float diff, float depth, float thickness, float slack)
{
    const float bias = 0.01 + 0.004 * depth + slack;
    const float ramp = min(0.02 + 0.004 * depth, 0.25 * thickness);
    return smoothstep(bias, bias + ramp, diff) * (1.0 - smoothstep(0.5 * thickness, thickness, diff));
}

// 0..1: how much a ray point has come nearer to the vehicle pixel than the previous point was, when that one was behind it
// (previousDiff > 0), from nothing to a tenth of the thickness. A ray that is coming out of the back of a surface towards its
// front reaches the surface from behind it, at the face the picture does not hold; a ray that goes deeper behind the surface
// went in at the front (0), and so did one that was in front of it at the previous point (the caller passes a negative diff).
float CarReflApproach(float previousDiff, float diff, float thickness)
{
    return (previousDiff > 0.0) ? smoothstep(0.0, 0.1 * thickness, previousDiff - diff) : 0.0;
}

// The weight of the sum of the samples' windows: one sample, or small ones, is a thin part or the edge of one, which the
// samples may have caught or missed; two samples or more are a part the ray goes along.
float CarReflThin(float sum)
{
    return lerp(kCarReflThinFloor, 1.0, saturate((sum - 0.5) / 1.5));
}

CarReflResult CarReflTrace(float3 world, float3 normal, float3 viewVector)
{
    CarReflResult result = (CarReflResult)0;
    result.face = 1.0;
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
    // motion or a cut puts it elsewhere: the whole effect fades with the disagreement. Its colour there is the paint's own.
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
    const float4 own = CarReflLoad(history, size, startUv);
    const float slack = CarReflSlack(start.w, size.y, length(float3(m0.y, m1.y, m2.y)), nv);
    const float tolerance = 0.06 + 0.02 * start.w + slack;
    const float agreement = 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(own.a - start.w));
    if (!(own.a > 0.0) || agreement <= 0.0)
    {
        return result;
    }
    result.own = min(max(own.rgb, 0.0) * more.z, 64.0);

    // 2. The march: `steps` points along the ray, the same for every pixel (a noise per pixel would make neighbours disagree
    // about a part narrower than the steps, a dotted patch; without it a pixel's answer follows the picture). Each point
    // has a weight, how much it is just behind a vehicle pixel (CarReflWindow), fading with the distance along the ray.
    // The hit is the heaviest point. How sure it is of being the face the camera sees: a ray that went in at the front of the
    // surface and deeper (1), or one that is coming back out towards the surface from behind it (0: the surface's hidden
    // side, which the picture does not hold, CarReflApproach). And how wide it is: the sum of the weights, one point alone
    // may be the edge of a thin part.
    const float rayLength = max(settings.y, 0.05);
    const int steps = clamp(int(more.x + 0.5), 4, 24);
    float bestWeight = 0.0;
    float bestT = 0.0;
    float bestFace = 1.0;
    float2 bestUv = float2(0.5, 0.5);
    float3 bestColour = float3(0.0, 0.0, 0.0);
    float sum = 0.0;
    float previousT = 0.0;
    float previousDiff = -1.0;  // how far behind the previous point's vehicle pixel the ray was; negative: in front or no vehicle
    result.state = 3.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float t = CarReflDistance(i, steps, rayLength);
        const float4 c = CarReflProject(m0, m1, m2, m3, world + r * t);
        const float2 uv = CarReflUv(c);
        if (c.w < 0.1 || any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
        {
            result.state = 2.0;
            break;
        }
        const float4 texel = CarReflLoad(history, size, uv);
        const bool onVehicle = texel.a > 0.0;
        const float diff = c.w - texel.a;
        const float thickness = max(settings.z, 1.1 * (t - previousT));
        const float weight = (onVehicle ? CarReflWindow(diff, c.w, thickness, slack) : 0.0) *
                             (1.0 - smoothstep(0.55, 1.0, t / rayLength));
        const float face = 1.0 - CarReflApproach(previousDiff, diff, thickness);
        const float pointWeight = weight * lerp(kCarReflBackFloor, 1.0, face);
        sum += weight;
        if (pointWeight > bestWeight)
        {
            bestWeight = pointWeight;
            bestT = t;
            bestFace = face;
            bestUv = uv;
            bestColour = texel.rgb;  // a point read: the image's rgb is a vehicle's pixel or 0
        }
        previousDiff = onVehicle ? diff : -1.0;
        previousT = t;
        if (sum >= 2.0)
        {
            break;
        }
    }

    // 3. The hit: its colour, and the weight with the self-test and the screen edge.
    if (bestWeight > 0.0)
    {
        const float edge = saturate(min(min(bestUv.x, 1.0 - bestUv.x), min(bestUv.y, 1.0 - bestUv.y)) * 12.5);
        result.colour = min(max(bestColour, 0.0) * more.z, 64.0);
        result.dist = bestT / rayLength;
        result.face = bestFace;
        result.hit = bestWeight * CarReflThin(sum) * agreement * edge;
        if (result.hit > 0.02)
        {
            result.state = 4.0;
        }
    }
    return result;
}

// What the ray sees: the colour of the vehicle pixel it met where that is the face the camera sees, the paint's own colour in
// shade where the ray reached the surface from behind (the hidden face of a body part is usually the same paint, unlit).
float3 CarReflSeen(CarReflResult hit)
{
    return lerp(hit.own * kCarReflShade, hit.colour, hit.face);
}

// The reflection `env` (the sharp read, share 1; the blurred one, share 0.5) with the hit applied. The weight already holds how
// sure the hit is, so a thin or hidden-face hit takes less of the reflection, whatever the mode.
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
        return lerp(env, CarReflSeen(hit), weight);
    }
    return env * (1.0 - kCarReflDark * weight);
}

// The debug views of g_CarReflB.y: 1 the hit weight, 2 the colour the ray sees, 3 the paint's baked vertex AO, 4 the
// state of the trace (red: the point is not in the history, blue: the ray left the picture, green: no hit, yellow: a hit,
// orange where the hit is the hidden face; brighter the heavier). The write of the colour is scaled by the exponent bias of
// the target, so they are in its units.
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
        return CarReflSeen(hit);
    }
    if (view == 3u)
    {
        return float3(bakedAo, bakedAo, bakedAo) * unit;
    }
    float3 colour = float3(0.0, 0.0, 0.0);
    if (hit.state > 3.5)
    {
        colour = float3(1.0, 0.3 + 0.7 * hit.face, 0.0) * (0.25 + 0.75 * saturate(hit.hit));
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
  // each backend's declaration, and the game's three lines nowhere without their edit. The history is read as points
  // (never a filtered sample), with one read of the image per ray point and one for the shaded point.
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
      {"history.SampleLevel(", 0},        {"history.Sample(", 0},
      {"CarReflLoad(history, size, uv)", 1}, {"CarReflLoad(history, size, startUv)", 1},
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
