// MCLA GPU renderer -- the city's ray traced sun shadows (city_rt_shadow.h, city_rt_shadow_rules.h).
//
// Run inside the occlusion's passes (ao.cpp Run), after the car's ray traced occlusion and before the visibility pass and the contact march, when this frame's top
// level structure exists (car_rt_scene.h: view space, x right, y up, z forward, the AO's: ao_common.hlsli AoViewPos) and holds the city's sectors beside the cars
// (city_rt_scene.h: instance mask CITY_RT_MASK_CITY). One thread is one pixel of the occlusion's linear depth image (level 0, R32_FLOAT at the traced size, the vehicle
// bit in its lowest bit) and writes the pixel of the output image of the same size (R8G8B8A8_UNORM), in the layout car_rt_ao_cs.hlsl writes so that the contact march
// reads it as it reads the car's:
//
//   r  1 (the occlusion is not this pass's)
//   g  the sun's open share: the share of the pixel's rays toward the sun that meet nothing, 1 lit .. 0 in shadow
//   b  1 where the pixel was traced, else 0
//   a  1
//
// THE RAYS. From the pixel's surface (the depth's view position, lifted off it along its normal by the rule of city_rt_shadow_rules.h Lift) `rays` rays go toward the sun,
// each to its own place of the sun's disc (DiscSample: the disc's area stratified over the rays, turned by the pixel's noise), as long as RayLength says. Where a ray meets
// a triangle of the city or of a car it is blocked; the visibility is the share that is not. A surface that faces away from the sun (cosine below kSunFacingLow) writes 1: the lit
// shaders' own N.L has darkened it, and the contact march applies this only on the sun's share of the light. Pixels beyond the push's range are 1 too (the host's range:
// where the structures are the hi detail the game drew, city_rt_rules.h SafeReceiverRange).
//
// THE CARS. A vehicle's pixel is not traced here (the car's own passes, car_rt_ao and the car's shadow map, own it): it keeps the car image's g where the host gives the
// car image's slot (kFlagCarImage), else 1. A pixel of the world takes the DARKER of this pass's visibility and the car image's g there (the car's own sun ray, in its region).
// The rays hit the city and the cars' body, tyres and other cars (the push's mask), so the player's car shadows the road here too.
//
// ALPHA TESTED triangles (the pack's second geometry of a sector) come up as candidates: the push's AlphaMode says whether they block (city_rt_shadow_rules.h). In the EXACT mode (the
// default, CITY-RT M1c) a candidate of the city decodes its own UV from the hit data's corner streams (t6..t9 of the ray tracing set, hlsl/city_hit_common.hlsli) and reads the alpha of its
// material's diffuse texture, resident through the bindless set (hlsl/city_hit_alpha.hlsli): the ray is blocked where the alpha reaches the reference (bits 12-19 of the flags), and goes
// through the holes. Where that cannot be read (no hit data, the texture is not resident yet) the candidate follows the stochastic rule: today's behaviour.
//
// THE CHECK (kFlagCheck, mcla_gpu_city_rt_check): instead of the sun rays, one primary ray from the view-space origin through every city pixel of the depth image (not a vehicle,
// not the sky) against the city's instances alone; its hit's distance is compared with the pixel's (the structures' geometry, the camera and the matrices are right when the
// two agree to a centimetre: the same proof the car check makes). The counters are in the stats buffer (city_rt_shadow_rules.h kStat*: pixels, hits, within 1 and 5 percent,
// misses, hits on the lod and on the hi detail, and a histogram of the relative error); with kFlagView the picture's g shows it (1 within 1 percent, 0.5 within 5, 0 else).
//
// Layout (city_rt_shadow.cpp), the car check's shape: set 0 the bindless texture set (t0, space0: the depth image and the car image by their slots), set 1 this pass's own
// (u0 the image, u1 the counters, space1), set 2 the car RT set (hlsl/car_rt_common.hlsli, CAR_RT_SPACE 2), the push range at (b0, space3; a Vulkan push constant block).
// Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

#define CITY_RT_MASK_CITY 0x80  // city_rt_rules.h kMaskCity: the one bit of the instance mask the car scene does not use

Texture2D<float4> g_textures[] : register(t0, space0);

// The hit data (the car scene's set at registers t6..t9: the pool, the sector, material and texture tables) and the cut-outs' alpha through the bindless textures. A slot differs from a
// lane to a lane (each ray meets its own fence), so the index is non uniform.
#define CITY_HIT_SPACE 2
#define CITY_HIT_ALPHA_TEXEL(slot, texel, level) g_textures[NonUniformResourceIndex((slot) & 0x7FFFFFFFu)].Load(int3(texel, int(level))).w
#include "city_hit_alpha.hlsli"
#include "city_rt_shadow_noise.hlsli"
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader: the image is R8G8B8A8_UNORM (city_rt_shadow.cpp EnsureOutput).
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);
RWByteAddressBuffer g_stats : register(u1, space1);

struct CityRtShadowPush
{
    // x the linear depth image's bindless slot, y and z the image's width and height, w the flags (city_rt_shadow_rules.h kFlag*): bits 0-3 the rays a pixel, bits 4-5 the
    // AlphaMode, bit 8 the car image is given, bits 12-19 the exact alpha's reference (unorm8), bit 28 the exact alpha's statistics and picture, bit 29 the check's picture, bit 30 the check
    uint4 slots;
    // x the frame's shift of the noise (low 10 bits; 0: a fixed pattern) and bit 29, the blue noise tile (ao_stable.h kBlueNoiseFlag: city_rt_shadow_noise.hlsli), y the ray mask (CITY_RT_MASK_CITY | the cars' parts), z the frame serial (the alpha hash), w the car image's slot
    uint4 noise;
    // x, y tan of the half field of view (the depth image's lens), z the rays' length in metres, w the range: a pixel beyond it is 1
    float4 lens;
    // xyz the direction to the sun in view space (unit), w the tangent of the sun disc's radius (the real sun's times the setting)
    float4 light;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CityRtShadowPush> g_push;
#else
ConstantBuffer<CityRtShadowPush> g_push : register(b0, space3);
#endif

// city_rt_shadow_rules.h: the numbers, in step with it (the unit test reads this file and compares).
static const float kSky = 60000.0;            // what the depth chain holds for the sky (ao_common.hlsli kAoSky)
static const float kTwoPi = 6.283185307179586;
static const float kGolden = 0.6180339887;
static const float kSunFacingLow = 0.03;
static const float kLiftFloor = 0.006;
static const float kLiftPixels = 3.0;
static const float kLiftCap = 0.4;
static const float kAlphaCoverage = 0.5;
static const float kMinAbove = 0.08;          // a ray is kept at least this far (as a cosine) above the point's tangent plane
static const float kCheckTMin = 0.01;
static const uint kFlagCarImage = 1u << 8;
static const uint kFlagAlphaRefShift = 12;
static const uint kFlagAlphaStats = 1u << 28;
static const uint kFlagView = 1u << 29;
static const uint kFlagCheck = 1u << 30;
static const uint kStatPixels = 0;
static const uint kStatHit = 1;
static const uint kStatWithin1 = 2;
static const uint kStatWithin5 = 3;
static const uint kStatNoHit = 4;
static const uint kStatLod = 5;
static const uint kStatHi = 6;
static const uint kStatHistogram = 8;
static const uint kStatAlphaCandidates = 24;
static const uint kStatAlphaExact = 25;
static const uint kStatAlphaBlocked = 26;
static const uint kStatAlphaPassed = 27;
static const uint kStatAlphaFallback = 28;
static const uint kStatAlphaLevels = 32;
static const uint kAlphaLevelBins = 8;
static const uint kPathCandidate = 1;
static const uint kPathFallback = 2;
static const uint kPathExact = 4;
static const float kViewNoCandidate = 1.0;
static const float kViewExact = 0.7;
static const float kViewFallback = 0.35;
static const uint kInstanceCity = 0x800000;  // city_rt_rules.h kInstanceIdFlag: the InstanceID of a city structure has it
static const uint kHistogramBins = 16;
static const float kHistogramStep = 0.005;

// city_rt_shadow_rules.h Pcg and Hash01.
uint Pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Hash01(uint x, uint y, uint primitive, uint frame)
{
    return float(Pcg(x ^ Pcg(y ^ Pcg(primitive ^ Pcg(frame)))) >> 8) * (1.0 / 16777216.0);
}

float LoadDepth(int2 p)
{
    return g_textures[g_push.slots.x].Load(int3(p, 0)).x;
}

// The view-space position of the pixel centre `centre` (in pixels of an image `size` wide and high) at distance t (ao_common.hlsli AoViewPos).
float3 ViewPos(float2 centre, float2 size, float t)
{
    const float2 uv = centre / size;
    return float3((uv.x * 2.0 - 1.0) * g_push.lens.x * t, (1.0 - uv.y * 2.0) * g_push.lens.y * t, t);
}

// The view-space normal from the depth image (ao_common.hlsli AoNormalFromImage: the neighbour on each axis whose depth is nearer to the pixel's).
float3 NormalFromDepth(int2 p, int2 size, float tc, float3 pc)
{
    const int2 last = size - 1;
    const int2 pl = int2(max(p.x - 1, 0), p.y);
    const int2 pr = int2(min(p.x + 1, last.x), p.y);
    const int2 pt = int2(p.x, max(p.y - 1, 0));
    const int2 pb = int2(p.x, min(p.y + 1, last.y));
    const float tl = LoadDepth(pl);
    const float tr = LoadDepth(pr);
    const float tt = LoadDepth(pt);
    const float tb = LoadDepth(pb);
    const float2 fs = float2(size);
    const float3 h = abs(tl - tc) < abs(tr - tc) ? pc - ViewPos(float2(pl) + 0.5, fs, tl) : ViewPos(float2(pr) + 0.5, fs, tr) - pc;
    const float3 v = abs(tt - tc) < abs(tb - tc) ? ViewPos(float2(pt) + 0.5, fs, tt) - pc : pc - ViewPos(float2(pb) + 0.5, fs, tb);
    const float3 n = cross(v, h);
    const float len = length(n);
    return len > 1e-20 ? n / len : float3(0.0, 0.0, -1.0);
}

// An orthonormal frame about the unit vector n (Duff et al. 2017).
void Frame(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// city_rt_shadow_rules.h DiscSample.
float2 DiscSample(uint k, uint rays, float noise1, float noise2)
{
    const float u = (float(k) + min(noise1, 0.9999)) / float(rays);
    const float r = sqrt(u);
    const float angle = kTwoPi * frac(noise2 + kGolden * float(k));
    return float2(r * cos(angle), r * sin(angle));
}

// city_rt_shadow_rules.h AcceptCandidate: what a cut-out candidate does. Mode 0 never blocks, 1 always, 2 by the hash of the pixel, the triangle and the frame, 3 (exact) by the alpha it
// read against the reference, and by the hash where it could not read one.
bool AcceptCandidate(uint mode, bool known, float alpha, float reference, float hash)
{
    if (mode == 3u && known)
    {
        return CityHitAlphaBlocks(alpha, reference);
    }
    if (mode == 0u)
    {
        return false;
    }
    if (mode == 1u)
    {
        return true;
    }
    return hash < kAlphaCoverage;
}

// One yes or no ray: does anything of the mask lie within t_max of origin along direction? A first hit ends the search. A candidate of an alpha tested geometry (the city's
// second geometry of a structure) is a blocker by the push's AlphaMode (AcceptCandidate). `cone` is the width in metres of the pixel the ray started from; the width the alpha is read at
// is that plus the sun disc's spread over the distance the ray travelled to the candidate. `path` collects what happened for the exact alpha's picture (kPath*).
bool Blocked(float3 origin, float3 direction, float t_max, uint2 pixel, uint salt, float cone, inout uint path)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = t_max;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.noise.y, ray);
    const uint flags = g_push.slots.w;
    const uint mode = (flags >> 4) & 3u;
    const float reference = float((flags >> kFlagAlphaRefShift) & 255u) * (1.0 / 255.0);
    const bool stats = (flags & kFlagAlphaStats) != 0u;
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            bool known = false;
            float alpha = 1.0;
            uint level = 0u;
            const uint instance = query.CandidateInstanceID();
            if (mode == 3u && (instance & kInstanceCity) != 0u)
            {
                const float width = cone + 2.0 * query.CandidateTriangleRayT() * g_push.light.w;
                const CityHitAlpha a = CityHitAlphaOfCandidate(instance, query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), width);
                known = a.known != 0u;
                alpha = a.alpha;
                level = a.level;
            }
            float hash = 0.0;
            if (mode >= 2u && !known)
            {
                hash = Hash01(pixel.x, pixel.y, query.CandidatePrimitiveIndex() + 7919u * salt, g_push.noise.z);
            }
            const bool accept = AcceptCandidate(mode, known, alpha, reference, hash);
            path |= kPathCandidate | (known ? kPathExact : kPathFallback);
            if (stats)
            {
                uint ignored;
                g_stats.InterlockedAdd(kStatAlphaCandidates * 4u, 1u, ignored);
                if (known)
                {
                    g_stats.InterlockedAdd(kStatAlphaExact * 4u, 1u, ignored);
                    g_stats.InterlockedAdd((accept ? kStatAlphaBlocked : kStatAlphaPassed) * 4u, 1u, ignored);
                    g_stats.InterlockedAdd((kStatAlphaLevels + min(level, kAlphaLevelBins - 1u)) * 4u, 1u, ignored);
                }
                else
                {
                    g_stats.InterlockedAdd(kStatAlphaFallback * 4u, 1u, ignored);
                }
            }
            if (accept)
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float4 TraceSun(uint2 pixel, uint2 size, float t, float3 p)
{
    const float4 open = float4(1.0, 1.0, 0.0, 1.0);
    const float3 to_sun = g_push.light.xyz;
    float3 n = NormalFromDepth(int2(pixel), int2(size), t, p);
    if (dot(n, p) > 0.0)
    {
        n = -n;  // facing the camera
    }
    if (dot(n, to_sun) <= kSunFacingLow)
    {
        return open;  // turned from the sun: the lit shader's N.L has it
    }
    const float footprint_unit = 2.0 * g_push.lens.y / float(size.y);
    const float lift = min(kLiftFloor + kLiftPixels * footprint_unit * t, kLiftCap);
    const float3 origin = p + n * lift;
    const uint rays = clamp(g_push.slots.w & 0xFu, 1u, 8u);
    const float2 noise = CityRtShadowNoise(int(pixel.x), int(pixel.y), g_push.noise.x);
    float3 sx;
    float3 sy;
    Frame(to_sun, sx, sy);
    // The width of the pixel the rays start from, in metres: what the exact alpha's level starts at.
    const float cone = footprint_unit * t;
    uint path = 0u;
    float blocked = 0.0;
    [loop]
    for (uint k = 0u; k < rays; ++k)
    {
        const float2 d = DiscSample(k, rays, noise.x, noise.y);
        float3 direction = normalize(to_sun + sx * (d.x * g_push.light.w) + sy * (d.y * g_push.light.w));
        const float above = dot(direction, n);
        if (above < kMinAbove)
        {
            direction = normalize(direction + (kMinAbove - above) * n);
        }
        if (Blocked(origin, direction, g_push.lens.z, pixel, k, cone, path))
        {
            blocked += 1.0;
        }
    }
    // The exact alpha's picture (mcla_gpu_city_rt_alpha_view): which way the pixel's rays went through the cut-outs, in the place of the visibility.
    if ((g_push.slots.w & kFlagAlphaStats) != 0u)
    {
        return float4(1.0, path == 0u ? kViewNoCandidate : ((path & kPathFallback) != 0u ? kViewFallback : kViewExact), 1.0, 1.0);
    }
    return float4(1.0, 1.0 - blocked / float(rays), 1.0, 1.0);
}

// The check's pixel: the primary ray against the city's instances alone. x the error class (0 not looked at, 1 hit, 2 within 1 percent, 3 within 5 percent, 4 no hit),
// y the relative error, z 1 for a hit on a lod structure.
struct CheckResult
{
    uint kind;
    float error;
    bool lod;
};

CheckResult CheckPixel(uint2 pixel, uint2 size, float t)
{
    CheckResult r;
    r.kind = 4u;
    r.error = 1.0;
    r.lod = false;
    const float2 uv = (float2(pixel) + 0.5) / float2(size);
    RayDesc primary;
    primary.Origin = float3(0.0, 0.0, 0.0);
    primary.Direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
    primary.TMin = kCheckTMin;
    primary.TMax = t * 1.5 + 2.0;
    RayQuery<RAY_FLAG_NONE> first;
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, CITY_RT_MASK_CITY, primary);
    while (first.Proceed())
    {
        if (first.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            first.CommitNonOpaqueTriangleHit();  // the check counts the geometry, alpha tested or not
        }
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return r;
    }
    const float error = abs(first.CommittedRayT() - t) / max(t, 0.01);
    r.error = error;
    r.kind = error <= 0.01 ? 2u : (error <= 0.05 ? 3u : 1u);
    r.lod = (first.CommittedInstanceID() & 1u) != 0u;
    return r;
}

void AddWave(uint word, bool condition)
{
    const uint count = WaveActiveCountBits(condition);
    if (count != 0u && WaveIsFirstLane())
    {
        uint ignored;
        g_stats.InterlockedAdd(word * 4u, count, ignored);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(g_push.slots.y, g_push.slots.z);
    if (any(id.xy >= size))
    {
        return;
    }
    const uint flags = g_push.slots.w;
    const float raw = LoadDepth(int2(id.xy));
    const bool vehicle = (asuint(raw) & 1u) != 0u;
    const float t = asfloat(asuint(raw) & ~1u);
    const bool sky = !(t > 0.0) || t >= kSky * 0.5;
    float car_g = 1.0;
    if ((flags & kFlagCarImage) != 0u)
    {
        car_g = g_textures[g_push.noise.w].Load(int3(int2(id.xy), 0)).y;
    }
    float4 result = float4(1.0, car_g, 0.0, 1.0);

    if ((flags & kFlagCheck) != 0u)
    {
        const bool looked = !sky && !vehicle;
        CheckResult c;
        c.kind = 0u;
        c.error = 0.0;
        c.lod = false;
        if (looked)
        {
            c = CheckPixel(id.xy, size, t);
        }
        AddWave(kStatPixels, looked);
        AddWave(kStatHit, looked && c.kind >= 1u && c.kind <= 3u);
        AddWave(kStatWithin1, looked && c.kind == 2u);
        AddWave(kStatWithin5, looked && (c.kind == 2u || c.kind == 3u));
        AddWave(kStatNoHit, looked && c.kind == 4u);
        AddWave(kStatLod, looked && c.kind <= 3u && c.kind >= 1u && c.lod);
        AddWave(kStatHi, looked && c.kind <= 3u && c.kind >= 1u && !c.lod);
        if (looked && c.kind <= 3u && c.kind >= 1u)
        {
            const uint bin = min(uint(c.error / kHistogramStep), kHistogramBins - 1u);
            uint ignored;
            g_stats.InterlockedAdd((kStatHistogram + bin) * 4u, 1u, ignored);
        }
        if ((flags & kFlagView) != 0u && looked)
        {
            result = float4(1.0, c.kind == 2u ? 1.0 : (c.kind == 3u ? 0.5 : 0.0), 1.0, 1.0);
        }
        g_output[id.xy] = result;
        return;
    }

    // The sun's rays: a surface of the world within the range.
    if (!sky && !vehicle && t < g_push.lens.w)
    {
        const float3 p = ViewPos(float2(id.xy) + 0.5, float2(size), t);
        const float4 traced = TraceSun(id.xy, size, t, p);
        result = float4(1.0, min(traced.y, car_g), traced.z, 1.0);
    }
    g_output[id.xy] = result;
}
