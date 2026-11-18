// MCLA GPU renderer -- the player's car reflecting itself, ray traced (car_rt_refl.h).
//
// Run at the history trigger (the game's mid-scene depth copy, car_selfrefl.h) right after the history image is made, when this
// frame's acceleration structures of the player's car exist (car_rt_scene.h: the top level is in VIEW space, x right, y up, z forward,
// the AO's: ao_common.hlsli AoViewPos). One thread is one texel of the history image (R16G16B16A16_FLOAT: rgb the scene colour of a
// vehicle's pixel, a its view distance, 0 for everything else; the picture's size over the divisor, 1 or 2) and writes the texel of
// the output image of the same size. What the output holds is in car_rt_refl_rules.h (one source of the numbers: this file keeps its
// own copy of them, change one, change the other):
//
//   alpha 0               no data (not the player's car, or no vehicle): the paint marches the history as before
//   alpha 1               the player's car, the reflection ray hits nothing of the car: the environment stays
//   alpha 1 + w           the ray hits a part the camera sees, rgb its colour in the history; w the weight
//   alpha -(1 + w)        the ray hits a part the camera does not see (or off the picture), rgb the pixel's own colour in shade
//
// THE PRIMARY RAY goes from the camera through the texel's render pixel with the direction (ndc.x tanX, ndc.y tanY, 1), so a distance
// along it is the view distance t that the history holds. It looks only in a window around the history's distance (the tolerance of
// car_rt_refl_rules.h) at the opaque rigid parts: a hit there is the player's car at this pixel, and gives the point and the unit
// normal (CarRtHitNormal: the mesh's vertex normals, interpolated: the same normal the paint shades with). No hit, or a hit that is not
// at the history's distance once the slope is known: no data. THE REFLECTION RAY leaves the point (put off the surface along the
// normal) along reflect(view, normal) against body, glass and tyres (the push's parts). On a hit it is taken into the SAME frame's
// history image: if the texels around it show a surface at its distance it is a part the camera sees, and the hit's colour is theirs
// (an edge-aware bilinear mean: only the texels that are that surface count); if they do not (the point is on the back of a part, or
// hidden by another, or off the picture) the colour is the shaded pixel's own in shade (kShade) at half the weight (kHiddenWeight),
// version 4's hidden-face rule. The weight falls to nothing from 0.6 of the ray's length to its end.
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0:
// the history), set 1 the output (u0, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2), the push block (b0, space 3).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
RWTexture2D<float4> g_output : register(u0, space1);

struct CarRtReflPush
{
    // x the history image's bindless slot, y the reflection ray's parts (CAR_RT_MASK_*), z the primary ray's parts, w unused
    uint4 slots;
    // xy the image's size in texels, zw the render size in pixels
    float4 image;
    // x, y tan of half the horizontal and the vertical field of view (the history's lens), z the image's divisor (1 or 2), w the
    // reflection ray's length in metres
    float4 lens;
    // x the shade (the pixel's own colour in shade, 0.4), y the weight of a hit the history cannot show (0.5), z the share of the ray's
    // length where the fade starts (0.6), w unused
    float4 tune;
};

ConstantBuffer<CarRtReflPush> g_push : register(b0, space3);

static const float kMinNv = 0.03;
static const float kMinDepth = 0.05;
static const float kMaxSlack = 0.05;
static const float kVisibleFullShare = 0.5;
static const float kMaxColour = 60000.0;
static const float kNoData = 0.0;

// ---------------------------------------------------------------------------------------------------------------------
// the numbers of car_rt_refl_rules.h
// ---------------------------------------------------------------------------------------------------------------------

// The spacing of half floats at t.
float HalfStep(float t)
{
    return exp2(floor(log2(max(t, 6.2e-5))) - 10.0);
}

float SurfaceSlope(float nv)
{
    return min(sqrt(max(1.0 - nv * nv, 0.0)) / max(nv, 0.1), 8.0);
}

float Tolerance(float t, float pixelMeters, float footprint, float nv)
{
    const float slack = min(footprint * pixelMeters * SurfaceSlope(nv), kMaxSlack);
    return HalfStep(t) + 0.01 + 0.005 * t + slack;
}

float PixelMeters(float t, float tanY, float edge)
{
    return t * 2.0 * tanY / max(edge, 1.0);
}

float MatchWeight(float texelDistance, float t, float tolerance)
{
    return texelDistance > 0.0 ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(texelDistance - t)) : 0.0;
}

// The alpha of a texel with a trace: a hit visible by `visible` (0..1) at `rayDistance` along a ray of `reach`, with the colour it shows.
float4 HitTexel(float3 visibleColour, float visible, float3 own, float rayDistance, float reach)
{
    const float fade = 1.0 - smoothstep(g_push.tune.z * reach, reach, rayDistance);
    const float vis = saturate(visible / kVisibleFullShare);
    const float weight = fade * (g_push.tune.y + (1.0 - g_push.tune.y) * vis);
    if (weight < 1.0 / 512.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // beyond the reach: a miss
    }
    const float3 shade = own * g_push.tune.x;
    const float3 colour = min(max(shade + (visibleColour - shade) * vis, 0.0), kMaxColour);
    return float4(colour, vis >= 0.5 ? 1.0 + weight : -(1.0 + weight));
}

// ---------------------------------------------------------------------------------------------------------------------
// the reflection ray's hit, taken into this frame's history
// ---------------------------------------------------------------------------------------------------------------------

// How much of the 2x2 footprint around the point q (view space) in the history image is the surface q is on, and the colour of the
// texels that are. `nv` is N.V of the surface at q seen from the camera.
float VisibleAt(Texture2D<float4> history, float3 q, float nv, out float3 colour)
{
    colour = float3(0.0, 0.0, 0.0);
    if (!(q.z > kMinDepth))
    {
        return 0.0;
    }
    const float2 ndc = float2(q.x / (q.z * g_push.lens.x), q.y / (q.z * g_push.lens.y));
    if (any(abs(ndc) > 1.0))
    {
        return 0.0;
    }
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    const float2 place = float2((ndc.x * 0.5 + 0.5) * render.x, (0.5 - ndc.y * 0.5) * render.y);
    // texel i is the render pixel floor((i + 0.5) divisor): centred at i * divisor + floor(divisor / 2) + 0.5
    const float2 f = (place - (floor(divisor * 0.5) + 0.5)) / divisor;
    const float2 base = floor(f);
    const float2 fr = f - base;
    const int2 last = int2(g_push.image.xy) - 1;
    const float texelMeters = PixelMeters(q.z, g_push.lens.y, g_push.image.y);
    const float tolerance = Tolerance(q.z, texelMeters, 1.0, nv);
    float coverage = 0.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const int2 offset = int2(k & 1, k >> 1);
        const int2 texel = clamp(int2(base) + offset, int2(0, 0), last);
        const float wb = (offset.x != 0 ? fr.x : 1.0 - fr.x) * (offset.y != 0 ? fr.y : 1.0 - fr.y);
        const float4 h = history.Load(int3(texel, 0));
        const float w = wb * MatchWeight(h.a, q.z, tolerance);
        coverage += w;
        sum += w * h.rgb;
    }
    if (coverage > 1e-6)
    {
        colour = sum / coverage;
    }
    return coverage;
}

// ---------------------------------------------------------------------------------------------------------------------
// one texel
// ---------------------------------------------------------------------------------------------------------------------

float4 TraceTexel(Texture2D<float4> history, uint2 texel, float4 own)
{
    const float t = own.a;
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    // the render pixel the history pass read for this texel (hlsl/car_history_ps.hlsl: int2(position.xy * divisor), position at the centre)
    const float2 pixel = min(floor((float2(texel) + 0.5) * divisor), render - 1.0);
    const float2 uv = (pixel + 0.5) / render;
    const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
    const float pixelMeters = PixelMeters(t, g_push.lens.y, render.y);

    // 1. the primary ray: the player's car at this pixel, within the window around the history's distance
    const float window = Tolerance(t, pixelMeters, 0.75, 0.0);
    RayDesc primary;
    primary.Origin = float3(0.0, 0.0, 0.0);
    primary.Direction = direction;
    primary.TMin = max(t - window, 0.001);
    primary.TMax = t + window;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> first;
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary);
    while (first.Proceed())
    {
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return float4(0.0, 0.0, 0.0, kNoData);
    }
    const float hitT = first.CommittedRayT();
    const float3 viewDirection = normalize(direction);
    const float3 normal = CarRtHitNormal(first.CommittedInstanceIndex(), first.CommittedPrimitiveIndex(),
                                         first.CommittedTriangleBarycentrics(), first.CommittedObjectToWorld3x4(), viewDirection);
    if (!(dot(normal, normal) > 0.25))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // no usable normal: the march
    }
    const float nv = abs(dot(normal, viewDirection));
    if (abs(hitT - t) > Tolerance(t, pixelMeters, 0.75, nv))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // a surface of the car, but not the one the picture shows here
    }
    if (nv < kMinNv)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // grazing: the paint does not trace it either (a miss: nothing changes)
    }

    // 2. the reflection ray, off the surface along the normal
    const float3 surface = direction * hitT;
    const float3 reflected = reflect(viewDirection, normal);
    const float reach = g_push.lens.w;
    RayDesc reflection;
    reflection.Origin = surface + normal * (0.004 + 0.002 * hitT);
    reflection.Direction = reflected;
    reflection.TMin = 0.0;
    reflection.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> second;
    second.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.y, reflection);
    while (second.Proceed())
    {
    }
    if (second.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // nothing of the car in the way: the environment stays
    }

    // 3. what the hit shows: the history's colour where the camera sees that surface, the fallback where it does not
    const float rayDistance = second.CommittedRayT();
    const float3 hit = reflection.Origin + reflected * rayDistance;
    const float3 hitNormal = CarRtHitNormal(second.CommittedInstanceIndex(), second.CommittedPrimitiveIndex(),
                                            second.CommittedTriangleBarycentrics(), second.CommittedObjectToWorld3x4(), reflected);
    const float hitNv = dot(hitNormal, hitNormal) > 0.25 ? abs(dot(hitNormal, normalize(hit))) : 1.0;
    float3 visibleColour;
    const float visible = VisibleAt(history, hit, hitNv, visibleColour);
    return HitTexel(visibleColour, visible, own.rgb, rayDistance, reach);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(g_push.image.xy);
    if (any(id.xy >= size))
    {
        return;
    }
    Texture2D<float4> history = g_textures[g_push.slots.x];
    const float4 own = history.Load(int3(id.xy, 0));
    float4 result = float4(0.0, 0.0, 0.0, kNoData);
    // Most of the image is no vehicle (alpha 0): one read and one write. A wave with no vehicle texel in it never reaches the rays.
    if (own.a > 0.0 && own.a < 60000.0)
    {
        result = TraceTexel(history, id.xy, own);
    }
    g_output[id.xy] = result;
}
