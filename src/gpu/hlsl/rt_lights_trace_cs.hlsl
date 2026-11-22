// MCLA GPU renderer -- the ray traced visibility of the top K lights: the trace (rt_lights.h, rt_lights_rules.h; perf/RT-DIRECT-SURVEY.md section 4 O1; RT-LIGHTS-D1 part 1).
//
// One thread is one pixel of the occlusion's TRACED size (its linear depth image, R32F, the vehicle mark in its lowest mantissa bit). For every light of the frame's top K (the rows: a 64 byte header and
// up to nine 64 byte lights) that can light the pixel (inside its range and cone, facing it: hlsl/rt_lights_common.hlsli RtvReaches) the thread sends `rays` rays to points of the light's disc against the top
// level (the city's structures, mask 0x80, and the cars' parts: the first hit ends the search, a non opaque triangle blocks by a coin of the blue noise) and writes the share of them that arrived:
//   the signal images   three RGBA16F images (a light's CHANNEL = image x 3 + component, rgb; the alpha is RtDenoise's): the visibility 0 .. 1, 1 where the light does not reach the pixel or the pixel
//                       is not a surface to judge (the sky, a vehicle: its light is its own pass's; or beyond the range);
//   the guides          the denoiser's (hlsl/rt_denoise_plain_consumer.hlsli): the depth along the view axis (R32F, 0 where not signal), the view space normal facing the camera and the roughness 1 (RGBA16F),
//                       the hit distance 0 and the share 1 where some light of the frame reaches the pixel (RG16F).
// THE IDENTITY. With mcla_gpu_rt_lights_check 1 (kFlagCheckVisible) the rays are still traced and every visibility is written as 1; with 2 (kFlagCheckBlocked) as 0 where the light reaches the pixel.
// THE NOISE (AGENT-RULE-NOISE): the shared per-frame blue noise at RtvSampleIndex(frame, light, slot): nothing is a fixed lattice. The first versions' gradient noise is the debug comparison (kFlagBlue off).
//
// Layout (rt_lights.cpp): set 0 (space0) the bindless Texture2D<float4>[] (the linear depth image by its slot), set 1 (space1) this pass's own: t0 the rows, u1 u2 u3 the signal images, u4 the depth guide,
// u5 the normal guide, u6 the hit / share guide, u7 the counters; set 2 (space2) the ray tracing set (the car scene's t0..t3, the world frame's t10 and t11: CAR_RT_SPACE 2); the push block at (b0, space3).
// Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query). Wave intrinsics (SM 6.0) add the counters a wave at a time.

#define CAR_RT_SPACE 2
#define RT_WORLD_SPACE 2
#include "car_rt_common.hlsli"

#define RTV_CITY_MASK 0x80   // city_rt_rules.h kMaskCity: the one bit of the instance mask the car scene does not use
#define RTV_INSTANCE_CITY 0x800000u   // city_rt_rules.h kInstanceIdFlag: the InstanceID of a city structure has it

Texture2D<float4> g_textures[] : register(t0, space0);
ByteAddressBuffer g_rows : register(t0, space1);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (rt_lights.cpp EnsureImages makes them these).
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_signal0 : register(u1, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_signal1 : register(u2, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_signal2 : register(u3, space1);
[[vk::image_format("r32f")]] RWTexture2D<float> g_depth : register(u4, space1);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_normal : register(u5, space1);
[[vk::image_format("rg16f")]] RWTexture2D<float2> g_hit : register(u6, space1);
#else
RWTexture2D<float4> g_signal0 : register(u1, space1);
RWTexture2D<float4> g_signal1 : register(u2, space1);
RWTexture2D<float4> g_signal2 : register(u3, space1);
RWTexture2D<float> g_depth : register(u4, space1);
RWTexture2D<float4> g_normal : register(u5, space1);
RWTexture2D<float2> g_hit : register(u6, space1);
#endif
RWByteAddressBuffer g_stats : register(u7, space1);

#include "rt_world_frame.hlsli"
#include "blue_noise_frame.hlsli"
#include "rt_lights_common.hlsli"

struct RtvPush
{
    // x the occlusion's linear depth image's bindless slot, y the number of signal images (groups of three lights) this frame writes
    uint4 slots;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtvPush> g_push;
#else
ConstantBuffer<RtvPush> g_push : register(b0, space3);
#endif

// rt_lights_rules.h: the numbers and the flags, in step with it (rt_lights_test.cpp reads this file and compares).
static const float kSky = 60000.0;                 // what the depth chain holds for the sky (ao_common.hlsli kAoSky)
static const uint kFlagStats = 1u << 0;
static const uint kFlagCheckVisible = 1u << 1;
static const uint kFlagCheckBlocked = 1u << 2;
static const uint kFlagBlue = 1u << 3;
static const uint kStatPixels = 0;                  // pixels of the image
static const uint kStatWorld = 1;                   // ... that are a surface to judge (not the sky, not a vehicle, within the range, the world frame's rows valid)
static const uint kStatCovered = 2;                 // ... that some light of the frame reaches
static const uint kStatPairs = 3;                   // (pixel, light) pairs that reach
static const uint kStatRays = 4;                    // rays traced
static const uint kStatBlockedCity = 5;             // ... that the city's structures blocked
static const uint kStatBlockedCar = 6;              // ... a car
static const uint kStatBlockedPairs = 7;            // pairs whose visibility is under one half
static const uint kStatWords = 8;

// ---- noise ------------------------------------------------------------------------------------------------------------------------------------

// Interleaved gradient noise (ao_common.hlsli AoNoise): only the first versions' path (kFlagBlue off), for the comparison.
float Ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// The pixel's two numbers at a sample: the blue noise tile stepped along the R2 sequence (blue_noise_frame.hlsli), or the first versions' noise moved by the R2 step of the sample.
float2 PixelNoise(uint2 pixel, uint sampleIndex, uint flags)
{
    if ((flags & kFlagBlue) != 0u)
    {
        return BlueNoiseFrame2(int(pixel.x), int(pixel.y), sampleIndex);
    }
    const float2 spatial = float2(Ign(float2(pixel)), Ign(float2(pixel).yx + float2(31.0, 11.0)));
    return frac(spatial + float(sampleIndex & 1023u) * float2(0.75487766624669, 0.56984029099806));
}

// ---- the picture --------------------------------------------------------------------------------------------------------------------------------

float LoadDepthRaw(int2 p)
{
    return g_textures[g_push.slots.x].Load(int3(p, 0)).x;
}

float LoadDepth(int2 p)
{
    return asfloat(asuint(LoadDepthRaw(p)) & ~1u);
}

// The view-space position of the pixel centre `centre` (in pixels of an image `size` wide and high) at distance t (ao_common.hlsli AoViewPos).
float3 ViewPos(float2 centre, float2 size, float t, float2 tanHalf)
{
    const float2 uv = centre / size;
    return float3((uv.x * 2.0 - 1.0) * tanHalf.x * t, (1.0 - uv.y * 2.0) * tanHalf.y * t, t);
}

// The view-space normal from the depth image (ao_common.hlsli AoNormalFromImage: the neighbour on each axis whose depth is nearer to the pixel's).
float3 NormalFromDepth(int2 p, int2 size, float tc, float3 pc, float2 tanHalf)
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
    const float3 h = abs(tl - tc) < abs(tr - tc) ? pc - ViewPos(float2(pl) + 0.5, fs, tl, tanHalf) : ViewPos(float2(pr) + 0.5, fs, tr, tanHalf) - pc;
    const float3 v = abs(tt - tc) < abs(tb - tc) ? ViewPos(float2(pt) + 0.5, fs, tt, tanHalf) - pc : pc - ViewPos(float2(pb) + 0.5, fs, tb, tanHalf);
    const float3 n = cross(v, h);
    const float len = length(n);
    return len > 1e-20 ? n / len : float3(0.0, 0.0, -1.0);
}

// ---- the rays -----------------------------------------------------------------------------------------------------------------------------------

// What the top level holds between the ray's origin and its end: 0 nothing, 1 the city's shell, 2 a car. The first hit ends the search; a non opaque candidate blocks when its coin (the blue noise
// number plus the primitive's offset) is under one half.
uint TraceKind(RtvRay ray, float coinNoise)
{
    if (ray.tMax <= 0.0)
    {
        return 0u;
    }
    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = ray.direction;
    desc.TMin = 0.0;
    desc.TMax = ray.tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, RTV_CITY_MASK | CAR_RT_MASK_ALL, desc);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (RtvCoin(coinNoise, query.CandidatePrimitiveIndex()) < 0.5)
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return 0u;
    }
    return (query.CommittedInstanceID() & RTV_INSTANCE_CITY) != 0u ? 1u : 2u;
}

// ---- the counters ---------------------------------------------------------------------------------------------------------------------------------

// A wave's sum of a value, added once by its first lane (the words are never cleared: the host reads the difference of two reads).
void AddWaveSum(uint word, uint value)
{
    const uint sum = WaveActiveSum(value);
    if (sum != 0u && WaveIsFirstLane())
    {
        uint ignored;
        g_stats.InterlockedAdd(word * 4u, sum, ignored);
    }
}

// ---- the pixel ----------------------------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint4 h0 = g_rows.Load4(0);                 // width, height, frame, flags
    const uint4 h1 = g_rows.Load4(16);                // light count, rays, (unused ray mask), unused
    const float4 h2 = asfloat(g_rows.Load4(32));      // tan half fov x and y, the farthest pixel judged
    if (id.x >= h0.x || id.y >= h0.y)
    {
        return;
    }
    const int2 p = int2(id.xy);
    const uint2 size = uint2(h0.x, h0.y);
    const uint frame = h0.z;
    const uint flags = h0.w;
    const uint lightCount = min(h1.x, kRtvMaxLights);
    const uint rays = clamp(h1.y, 1u, kRtvMaxRays);
    const float2 tanHalf = float2(h2.x, h2.y);

    const float raw = LoadDepthRaw(p);
    const bool vehicle = (asuint(raw) & 1u) != 0u;
    const float t = asfloat(asuint(raw) & ~1u);
    const bool sky = !(t > 0.0) || t >= kSky * 0.5;
    const RtWorld w = RtWorldLoad();
    const bool worldOk = (w.flags & RT_WORLD_VALID) != 0u && (w.flags & RT_WORLD_CAMERA_KNOWN) != 0u;
    const bool world = !sky && !vehicle && t < h2.z && worldOk;

    float vis[9] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    float3 normal = float3(0.0, 0.0, -1.0);
    uint coveredLights = 0u;
    uint raysTraced = 0u;
    uint blockedCity = 0u;
    uint blockedCar = 0u;
    uint blockedPairs = 0u;

    if (world)
    {
        const float3 P = ViewPos(float2(id.xy) + 0.5, float2(size), t, tanHalf);
        float3 N = NormalFromDepth(p, int2(size), t, P, tanHalf);
        if (dot(N, P) > 0.0)
        {
            N = -N;   // facing the camera
        }
        normal = N;
        const float footprintUnit = 2.0 * tanHalf.y / float(size.y);
        const float lift = RtvLift(footprintUnit, t);
        [loop]
        for (uint i = 0u; i < lightCount; ++i)
        {
            const uint at = 64u + i * 64u;
            const float4 r0 = asfloat(g_rows.Load4(at));
            const float4 r1 = asfloat(g_rows.Load4(at + 16u));
            const float4 r2 = asfloat(g_rows.Load4(at + 32u));
            const uint4 r3 = g_rows.Load4(at + 48u);
            const float stopShort = asfloat(r3.x);
            const uint channel = r3.y;
            const bool spot = (r3.z & kRtvFlagSpot) != 0u;
            const bool pair = (r3.z & kRtvFlagPair) != 0u;
            if (channel >= 9u)
            {
                continue;
            }
            const float3 lampPos = RtvWorldToView(r0.xyz, w.camera, w.axisX, w.axisY, w.axisZ);
            const float3 lampDir = RtvDirToView(r1.xyz, w.axisX, w.axisY, w.axisZ);
            const RtvReach reach = RtvReaches(P, N, lampPos, lampDir, r1.w, r0.w, spot);
            if (!reach.covered)
            {
                continue;
            }
            ++coveredLights;
            float3 pairOffset = float3(0.0, 0.0, 0.0);
            if (pair)
            {
                pairOffset = RtvDirToView(r2.xyz, w.axisX, w.axisY, w.axisZ);
            }
            const float pick = pair ? PixelNoise(id.xy, RtvSampleIndex(frame, i, kRtvSlotPair), flags).x : 0.0;
            const float3 lampPoint = RtvLampPoint(lampPos, pairOffset, pair, pick);
            const float coinNoise = PixelNoise(id.xy, RtvSampleIndex(frame, i, kRtvSlotCoin), flags).x;
            uint blocked = 0u;
            [loop]
            for (uint k = 0u; k < rays; ++k)
            {
                const float2 noise = PixelNoise(id.xy, RtvSampleIndex(frame, i, k), flags);
                const float2 disc = RtvDiscSample(k, rays, noise.x, noise.y);
                const RtvRay ray = RtvMakeRay(P, N, lift, lampPoint, r2.w, disc, stopShort);
                const uint kind = TraceKind(ray, coinNoise);
                blocked += kind != 0u ? 1u : 0u;
                blockedCity += kind == 1u ? 1u : 0u;
                blockedCar += kind == 2u ? 1u : 0u;
                ++raysTraced;
            }
            float v = 1.0 - float(blocked) / float(rays);
            if ((flags & kFlagCheckVisible) != 0u)
            {
                v = 1.0;
            }
            else if ((flags & kFlagCheckBlocked) != 0u)
            {
                v = 0.0;
            }
            vis[channel] = v;
            blockedPairs += v < 0.5 ? 1u : 0u;
        }
    }

    const bool signal = world && coveredLights != 0u;
    const uint groups = min(g_push.slots.y, 3u);
    g_signal0[p] = float4(vis[0], vis[1], vis[2], 1.0);
    if (groups > 1u)
    {
        g_signal1[p] = float4(vis[3], vis[4], vis[5], 1.0);
    }
    if (groups > 2u)
    {
        g_signal2[p] = float4(vis[6], vis[7], vis[8], 1.0);
    }
    g_depth[p] = world ? t : 0.0;
    g_normal[p] = float4(normal, 1.0);
    g_hit[p] = float2(0.0, signal ? 1.0 : 0.0);

    if ((flags & kFlagStats) != 0u)
    {
        AddWaveSum(kStatPixels, 1u);
        AddWaveSum(kStatWorld, world ? 1u : 0u);
        AddWaveSum(kStatCovered, signal ? 1u : 0u);
        AddWaveSum(kStatPairs, coveredLights);
        AddWaveSum(kStatRays, raysTraced);
        AddWaveSum(kStatBlockedCity, blockedCity);
        AddWaveSum(kStatBlockedCar, blockedCar);
        AddWaveSum(kStatBlockedPairs, blockedPairs);
    }
}
