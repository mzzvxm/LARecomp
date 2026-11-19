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
//   alpha 1               the player's car, nothing of the car in the reflection's reach: the environment stays
//   alpha 1 + w           the reflection hits parts the camera sees, rgb their colour in the history; w the weight, the share of the cone's rays
//   alpha -(1 + w)        the reflection hits parts the camera does not see (or off the picture), rgb the surface's own colour in shade
//
// THE PRIMARY RAY goes from the camera through the texel's render pixel with the direction (ndc.x tanX, ndc.y tanY, 1), so a distance
// along it is the view distance t that the history holds. It looks only in a window around the history's distance (the tolerance of
// car_rt_refl_rules.h) at the opaque rigid parts: a hit there is the player's car at this pixel, and gives the point and the unit
// normal (CarRtHitNormal: the mesh's vertex normals, interpolated: the same normal the paint shades with). No hit, or a hit that is not
// at the history's distance once the slope is known: no data.
//
// THE CONE: from the point (put off the surface along the normal) kConeRays rays leave in directions spread over a cone of
// g_push.tune.w (the tangent of its half angle: the paint's gloss; 0 = a mirror and one ray) around reflect(view, normal), against
// body, glass and tyres (the push's parts). The pattern is two rings of equal area and four directions that add up to the mirror's, turned
// by an angle that is different at every texel (and, with a temporal upscaler running, at every frame): car_rt_refl_rules.h ConeDisc
// and ConeRotation. A ray that hits is taken into the SAME frame's history image: if the texels around its point show a surface at its
// distance it is a part the camera sees, and the hit's colour is theirs (an edge-aware bilinear mean: only the texels that are that surface
// count); if they do not (the point is on the back of a part, or hidden by another, or off the picture) the colour is the surface's own
// mean colour around the pixel in shade (kShade) and the weight is a little less (kHiddenWeight). The weight of a ray falls to nothing from
// 0.6 of its length to its end. A ray that hits the panel it left (the grazing self-hit: the ray runs under its own triangle's plane,
// which the mesh's smooth normal does not know) is a miss. The texel's weight is the sum of the rays' over the number of rays, its colour
// their weighted mean. After the first two rays (the outer pair) that found nothing the inner two are not cast.
//
// A SECOND OUTPUT, for the denoise that follows (car_rt_refl_denoise_cs.hlsl, car_rt_refl_rules.h "The denoise"): g_aux (RGBA8_UNORM, the same
// size) holds, for a texel with data, x the hit distance (sqrt of the mean over the rays that hit / the ray's length; 0 where none did), y the
// instance of the primary ray's hit / 255, z N.V of the surface, w 1; all 0 where the texel has no data.
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0:
// the history), set 1 the outputs (u0 the image, u1 the aux, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2), the push
// block (b0, space 3).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"
#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the image is R16G16B16A16_FLOAT, the aux
// image R8G8B8A8_UNORM (car_rt_refl.cpp EnsureOutput).
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);
#ifdef __spirv__
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> g_aux : register(u1, space1);

struct CarRtReflPush
{
    // x the history image's bindless slot, y the reflection ray's parts (CAR_RT_MASK_*), z the primary ray's parts, w the pattern's phase
    // (0, or 0 .. 7 by the frame where a temporal upscaler runs)
    uint4 slots;
    // xy the image's size in texels, zw the render size in pixels
    float4 image;
    // x, y tan of half the horizontal and the vertical field of view (the history's lens), z the image's divisor (1 or 2), w the
    // reflection ray's length in metres
    float4 lens;
    // x the shade (the surface's own colour in shade, 0.4), y the weight of a hit the history cannot show (0.85), z the share of the ray's
    // length where the fade starts (0.6), w the tangent of the cone's half angle (0: a mirror, one ray)
    float4 tune;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtReflPush> g_push;
#else
ConstantBuffer<CarRtReflPush> g_push : register(b0, space3);
#endif

static const float kMinNv = 0.03;
static const float kMinDepth = 0.05;
static const float kVisibleFullShare = 0.5;
static const float kMaxColour = 60000.0;
static const float kNoData = 0.0;
static const float kMinAbove = 0.03;         // a cone ray is kept at least this far (as a cosine) above the point's tangent plane
static const float kSelfCos = 0.85;          // a hit on the primary ray's instance that faces within 32 degrees of the point's normal is the panel itself
static const float kSurfaceSpacing = 2.0;    // texels between the taps of the surface's mean colour
static const float kTwoPi = 6.283185307179586;

// ---------------------------------------------------------------------------------------------------------------------
// the aux image (EncodeHitDistance, EncodeInstance of car_rt_refl_rules.h)
// ---------------------------------------------------------------------------------------------------------------------

// sqrt of the hit distance's share of the reach: more steps near the panel, where the cone is narrow; a hit never rounds to 0 (no hit).
float EncodeHitDistance(float distance, float reach)
{
    return distance > 0.0 ? max(sqrt(min(distance / max(reach, 1e-3), 1.0)), 1.0 / 255.0) : 0.0;
}

float EncodeInstance(uint instance)
{
    return float(min(instance, 255u)) / 255.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// the cone (ConeRadius, ConeOrder, ConeDisc, ConeRotation, ConeDirection of car_rt_refl_rules.h)
// ---------------------------------------------------------------------------------------------------------------------

static const float kConeRadius[4] = {0.5, 0.8660254037844386, 0.5, 0.8660254037844386};
static const int kConeOrder[4] = {1, 3, 0, 2};

// The angle the texel's pattern is turned by, in turns.
float ConeRotation(uint2 texel, uint phase)
{
    const float noise = frac(52.9829189 * frac(0.06711056 * float(texel.x) + 0.00583715 * float(texel.y)));
    return frac(noise + 0.6180339887 * float(phase));
}

// Ray `ray` of the cone around the mirror direction (both unit vectors in, a unit vector out), lifted to kMinAbove above the tangent plane.
float3 ConeDirection(float3 mirror, float3 normal, float tanHalf, int ray, float turns)
{
    const float3 up = abs(mirror.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(up, mirror));
    const float3 b = cross(mirror, t);
    const float angle = kTwoPi * (turns + 0.25 * float(ray));
    const float2 disc = kConeRadius[ray] * float2(cos(angle), sin(angle));
    float3 d = normalize(mirror + (t * disc.x + b * disc.y) * tanHalf);
    const float above = dot(d, normal);
    if (above < kMinAbove)
    {
        d = normalize(d + (kMinAbove - above) * normal);
    }
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// a ray's hit, taken into this frame's history
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

// The mean colour of the pixel's own surface around it: a 3x3 of history texels kSurfaceSpacing apart, those within the distance tolerance,
// tent weighted (SurfaceMean). The fallback colour of a hit the history cannot show is this times the shade.
float3 SurfaceMean(Texture2D<float4> history, uint2 texel, float t, float nv)
{
    const float texelMeters = PixelMeters(t, g_push.lens.y, g_push.image.y);
    const float tolerance = Tolerance(t, texelMeters, kSurfaceSpacing, nv);
    const int2 last = int2(g_push.image.xy) - 1;
    float total = 0.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 tapOffset = int2(k % 3, k / 3) - 1;
        const int2 at = clamp(int2(texel) + tapOffset * int(kSurfaceSpacing), int2(0, 0), last);
        const float4 h = history.Load(int3(at, 0));
        const float tent = (tapOffset.x == 0 ? 1.0 : 0.5) * (tapOffset.y == 0 ? 1.0 : 0.5);
        const float w = tent * MatchWeight(h.a, t, tolerance);
        total += w;
        sum += w * h.rgb;
    }
    return total > 1e-6 ? sum / total : history.Load(int3(int2(texel), 0)).rgb;
}

// One ray of the cone. False: a miss (nothing of the car in reach, a hit beyond the weight's fade, or the panel the ray left). True: `fade`
// the fade of its distance, `visible` how much of its hit the history shows (0..1), `seen` the colour the history shows there, `hitDistance`
// the distance along the ray (for the aux image).
bool TraceRay(Texture2D<float4> history, float3 origin, float3 direction, float reach, uint originInstance, uint originPrimitive,
              float3 originNormal, out float fade, out float visible, out float3 seen, out float hitDistance)
{
    fade = 0.0;
    visible = 0.0;
    seen = float3(0.0, 0.0, 0.0);
    hitDistance = 0.0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.y, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;  // nothing of the car in the way: the environment stays
    }
    const float distance = query.CommittedRayT();
    const uint instance = query.CommittedInstanceIndex();
    const uint primitive = query.CommittedPrimitiveIndex();
    const float3 hitNormal = CarRtHitNormal(instance, primitive, query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), direction);
    const bool usable = dot(hitNormal, hitNormal) > 0.25;
    if (usable && instance == originInstance && (primitive == originPrimitive || dot(hitNormal, originNormal) > kSelfCos))
    {
        return false;  // the panel itself: the ray ran under its own surface (IsSelfHit)
    }
    const float faded = 1.0 - smoothstep(g_push.tune.z * reach, reach, distance);
    if (faded < 1.0 / 512.0)
    {
        return false;  // beyond the reach: a miss
    }
    const float3 hit = origin + direction * distance;
    const float hitNv = usable ? abs(dot(hitNormal, normalize(hit))) : 1.0;
    const float coverage = VisibleAt(history, hit, hitNv, seen);
    fade = faded;
    visible = saturate(coverage / kVisibleFullShare);
    hitDistance = distance;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// one texel
// ---------------------------------------------------------------------------------------------------------------------

float4 TraceTexel(Texture2D<float4> history, uint2 texel, float4 own, out float4 aux)
{
    aux = float4(0.0, 0.0, 0.0, 0.0);
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
    const uint primaryInstance = first.CommittedInstanceIndex();
    const uint primaryPrimitive = first.CommittedPrimitiveIndex();
    const float3 viewDirection = normalize(direction);
    const float3 normal = CarRtHitNormal(primaryInstance, primaryPrimitive, first.CommittedTriangleBarycentrics(),
                                         first.CommittedObjectToWorld3x4(), viewDirection);
    if (!(dot(normal, normal) > 0.25))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // no usable normal: the march
    }
    const float nv = abs(dot(normal, viewDirection));
    if (abs(hitT - t) > Tolerance(t, pixelMeters, 0.75, nv))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // a surface of the car, but not the one the picture shows here
    }
    // the texel has data from here on: what the denoise needs to know about it (its hit distance below, where a ray hit)
    aux = float4(0.0, EncodeInstance(primaryInstance), saturate(nv), 1.0);
    if (nv < kMinNv)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // grazing: the paint does not trace it either (a miss: nothing changes)
    }

    // 2. the cone of reflection rays, off the surface along the normal
    const float3 mirror = reflect(viewDirection, normal);
    const float reach = g_push.lens.w;
    const float tanHalf = g_push.tune.w;
    const float rays = tanHalf > 0.0 ? 4.0 : 1.0;
    const float3 origin = direction * hitT + normal * (0.004 + 0.002 * hitT);
    const float turns = ConeRotation(texel, g_push.slots.w);
    float fade[4] = {0.0, 0.0, 0.0, 0.0};      // 0: the ray found nothing (or was not cast)
    float visible[4] = {0.0, 0.0, 0.0, 0.0};
    float3 seen[4] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    float distances[4] = {0.0, 0.0, 0.0, 0.0};
    bool found = false;
    [unroll]
    for (int slot = 0; slot < 4; ++slot)
    {
        if (slot >= 1 && !(tanHalf > 0.0))
        {
            break;  // a mirror: one ray
        }
        if (slot == 2 && !found)
        {
            break;  // the outer pair found nothing: the inner two are not cast
        }
        const int ray = kConeOrder[slot];
        const float3 direction2 = ConeDirection(mirror, normal, tanHalf, ray, turns);
        float f = 0.0;
        float v = 0.0;
        float3 c = float3(0.0, 0.0, 0.0);
        float d = 0.0;
        if (TraceRay(history, origin, direction2, reach, primaryInstance, primaryPrimitive, normal, f, v, c, d))
        {
            fade[ray] = f;
            visible[ray] = v;
            seen[ray] = c;
            distances[ray] = d;
            found = true;
        }
    }
    if (!found)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // nothing of the car in the reflection: the environment stays
    }

    // 3. what the hits show: the history's colour where the camera sees them, the surface's own colour in shade where it does not
    bool needShade = false;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        needShade = needShade || (fade[k] > 0.0 && visible[k] < 1.0);
    }
    float3 shade = float3(0.0, 0.0, 0.0);
    [branch]
    if (needShade)
    {
        shade = SurfaceMean(history, texel, t, nv) * g_push.tune.x;
    }
    float weightSum = 0.0;
    float visibleSum = 0.0;
    float3 colourSum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        const float w = fade[j] * (g_push.tune.y + (1.0 - g_push.tune.y) * visible[j]);
        weightSum += w;
        visibleSum += w * visible[j];
        colourSum += w * (shade + (seen[j] - shade) * visible[j]);
    }
    const float weight = min(weightSum / rays, 1.0);
    if (weight < 1.0 / 512.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float3 colour = min(max(colourSum / weightSum, 0.0), kMaxColour);
    // the mean distance of the rays that hit: how wide the cone is where it meets the part (the denoise's strength)
    float distanceSum = 0.0;
    float hitCount = 0.0;
    [unroll]
    for (int m = 0; m < 4; ++m)
    {
        distanceSum += fade[m] > 0.0 ? distances[m] : 0.0;
        hitCount += fade[m] > 0.0 ? 1.0 : 0.0;
    }
    aux.x = EncodeHitDistance(distanceSum / max(hitCount, 1.0), reach);
    return float4(colour, visibleSum >= 0.5 * weightSum ? 1.0 + weight : -(1.0 + weight));
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
    float4 aux = float4(0.0, 0.0, 0.0, 0.0);
    // Most of the image is no vehicle (alpha 0): one read and two writes. A wave with no vehicle texel in it never reaches the rays.
    if (own.a > 0.0 && own.a < 60000.0)
    {
        result = TraceTexel(history, id.xy, own, aux);
    }
    g_output[id.xy] = result;
    g_aux[id.xy] = aux;
}
