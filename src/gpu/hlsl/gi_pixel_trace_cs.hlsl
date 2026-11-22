// MCLA GPU renderer -- the probe field's per-pixel detail pass: the trace (gi_pixel.h, CITY-RT M3 package G5; the rules and the reasons are gi_pixel_rules.h's comment, the shared maths gi_pixel_common.hlsli).
//
// One thread a pixel of the pass's image (the occlusion's traced size, or half of it: the divisor in the rows). The pixel is read exactly as the apply's lookup reads it (hlsl/gi_apply_ps.hlsl GIA_PASS 0:
// the G-buffer's RT1 normal and RT2 albedo and the main depth at the picture's pixel the traced texel stands for): a pixel with no normal, no albedo, a lit weight of 0, no geometry, or a vehicle's mark in
// the occlusion's linear depth is not signal (guides 0). For the others it sends ONE cosine weighted ray (the R2 sequence per pixel and frame) at most GIP_REACH_M metres into the top level, the city (mask
// 0x80) and the cars' parts, and the result is
//   a hit on the city   the library's GI mode (hlsl/city_hit_shade.hlsli CityHitShadeHit: the SAME call the probes' rays make): the average colour times the ambient, the sun with a shadow ray (the disc
//                       jittered, the temporal stage averages it), ONE of the lamps of the world frame's grid with a shadow ray (R7: CITY_HIT_LAMP_GRID), the glow of what glows, the HEADLIGHTS of the frame's
//                       surface lights (CITY_HIT_AMBIENT: no shadow ray) and the field one frame old as the second bounce (CITY_HIT_FEEDBACK);
//   a hit on a car      a neutral diffuse in the frame's ambient and the sun (an occluder: CHECK G5-d);
//   nothing in reach    the field's irradiance at the pixel (the long range is the field's: gi_pixel_rules.h MISS).
// in the paint's units (params.units = 1), the field's. The signal image takes the sample's radiance, the guides the denoiser (hlsl/rt_denoise_plain_consumer.hlsli) wants: the depth along the view axis, the view
// space normal facing the camera (roughness 1: diffuse), and the hit distance with the share 1.
//
// Layout (gi_pixel.cpp): set 0 (space0) the bindless Texture2D<float4>[], set 1 (space1) the same as Texture2DMS<float4>[] (GIP_MS 1: the planes at the host's MSAA level), set 2 (space2) the samplers
// (0 = linear clamp), set 3 (space3) this pass's own: t0 the rows (the apply's buffer then the pixel rows), u1 the signal, u2 the depth guide, u3 the normal guide, u4 the hit image, u5 the counters; set 4
// (space4) the ray tracing set (the car scene's t0..t5, the hit data's t6..t9, the world frame's t10, t11: CAR_RT_SPACE 4); the push block at (b0, space5). Shader Model 6.5, SPIR-V 1.5 for Vulkan 1.2.

#define CAR_RT_SPACE 4
#define RT_WORLD_SPACE 4
#define CITY_HIT_SPACE 4
#include "car_rt_common.hlsli"

#ifndef GIP_MS
#define GIP_MS 0
#endif

Texture2D<float4> g_textures[] : register(t0, space0);
Texture2DMS<float4> g_textures_ms[] : register(t0, space1);
SamplerState g_samplers[] : register(s0, space2);
ByteAddressBuffer g_rows : register(t0, space3);
#ifdef __spirv__
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_signal : register(u1, space3);
[[vk::image_format("r32f")]] RWTexture2D<float> g_depth : register(u2, space3);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_normal : register(u3, space3);
[[vk::image_format("rg16f")]] RWTexture2D<float2> g_hit : register(u4, space3);
#else
RWTexture2D<float4> g_signal : register(u1, space3);
RWTexture2D<float> g_depth : register(u2, space3);
RWTexture2D<float4> g_normal : register(u3, space3);
RWTexture2D<float2> g_hit : register(u4, space3);
#endif
RWByteAddressBuffer g_stats : register(u5, space3);

struct GipPush
{
    // x the main depth plane's slot, y RT1's, z RT2's, w the occlusion's linear depth (traced size: the vehicle mark)
    uint4 slots;
    // z the host sample that is the guest's sample 0
    uint4 info;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GipPush> g_push;
#else
ConstantBuffer<GipPush> g_push : register(b0, space5);
#endif

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) g_textures[GiSlot(GI_PRODUCT_IRRADIANCE)].SampleLevel(g_samplers[0], (uv), 0.0f)
#define GI_SAMPLE_MOMENTS(uv) g_textures[GiSlot(GI_PRODUCT_MOMENTS)].SampleLevel(g_samplers[0], (uv), 0.0f).xy
#define GI_LOAD_PROBE_WEIGHT(px, py) g_textures[GiSlot(GI_PRODUCT_PROBE_WEIGHT)].Load(int3((int)(px), (int)(py), 0)).x
#include "gbuffer_common.hlsli"
#include "gi_common.hlsli"
#include "gi_apply_common.hlsli"
#include "gi_pixel_common.hlsli"
#include "light_laws.hlsli"
#include "rt_world_frame.hlsli"

#define GIP_ROW(i) GI_FRAME_ROW(GIP_ROW_BASE + (i))

static RtWorld g_world;
static uint g_lightCount;
static float g_lightGain;

// The field's irradiance at a hit (view space point and unit normal), the hit's bounce: the surfaces' light only, times the confidence (the probes' feedback, gi_update_trace_cs.hlsl GiuFeedback).
float3 GipFeedback(float3 viewPoint, float3 viewNormal)
{
    float3 pw = RtWorldPoint(g_world, viewPoint);
    float3 nw = float3(dot(viewNormal, g_world.axisX), dot(viewNormal, g_world.axisY), dot(viewNormal, g_world.axisZ));
    GiSample s = GiLookup(pw, nw);
    return s.irradiance * s.confidence;
}

// The frame's surface lights (the headlights) at a hit: the sum of their terms in the world, by their laws (hlsl/light_laws.hlsli), no shadow ray, times the gain. The rows hold them as GpuLight rows.
float3 GipLights(float3 viewPoint, float3 viewNormal)
{
    if (g_lightCount == 0u)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    float3 pw = RtWorldPoint(g_world, viewPoint);
    float3 nw = float3(dot(viewNormal, g_world.axisX), dot(viewNormal, g_world.axisY), dot(viewNormal, g_world.axisZ));
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < g_lightCount; ++i)
    {
        uint4 a = GIP_ROW(3u + 4u * i);
        uint4 b = GIP_ROW(4u + 4u * i);
        uint4 c = GIP_ROW(5u + 4u * i);
        uint4 d = GIP_ROW(6u + 4u * i);
        LlGpuLight g;
        g.row0 = float4(asfloat(a.x), asfloat(a.y), asfloat(a.z), asfloat(a.w));
        g.row1 = float4(asfloat(b.x), asfloat(b.y), asfloat(b.z), asfloat(b.w));
        g.row2 = float4(asfloat(c.x), asfloat(c.y), asfloat(c.z), asfloat(c.w));
        g.packed0 = d.x;
        g.law_a = asfloat(d.y);
        g.packed1 = d.z;
        g.shadow_weight = asfloat(d.w);
        sum += LlLightTerm(LlUnpack(g), pw, nw, 1.0f);
    }
    return sum * g_lightGain;
}

#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_LAMP_GRID
#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)
#define CITY_HIT_FEEDBACK(P, N) GipFeedback((P), (N))
#define CITY_HIT_AMBIENT(P, N, ambient) ((ambient) + GipLights((P), (N)))
#include "city_hit_shade.hlsli"

float4 GipLoadPlane(uint slot, int2 p)
{
#if GIP_MS
    return g_textures_ms[slot].Load(p, g_push.info.z);
#else
    return g_textures[slot].Load(int3(p, 0));
#endif
}

void GipWriteNone(int2 p)
{
    g_signal[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_depth[p] = 0.0f;
    g_normal[p] = float4(0.0f, 0.0f, -1.0f, 1.0f);
    g_hit[p] = float2(0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint4 r0 = GIP_ROW(0u);
    if (id.x >= r0.x || id.y >= r0.y)
    {
        return;
    }
    const int2 p = int2(id.xy);
    const uint4 r1 = GIP_ROW(1u);
    const uint4 r2u = GIP_ROW(2u);
    const float reach = asfloat(r2u.x);
    const float bounce = asfloat(r2u.y);
    g_lightGain = asfloat(r2u.z);
    const float pixelAngle = asfloat(r2u.w);
    g_lightCount = min(r1.z, GIP_LIGHTS_MAX);
    const bool stats = (r1.y & GIP_FLAG_STATS) != 0u;

    const GiaFrame fr = GiaLoadFrame();
    const int2 traced = int2(fr.traced);
    const int2 last = int2(fr.render) - 1;
    const int2 tex = min(p * int(r0.z), traced - 1);
    const int2 r = min(tex * int(r0.w), last);

    // The pixel: as the apply's lookup reads it. Anything the apply would not light is not signal.
    uint traced_count = 0u;
    uint hit_count = 0u;
    uint car_count = 0u;
    uint miss_count = 0u;
    uint skipped = 0u;
    uint no_data = 0u;
    g_world = RtWorldLoad();
    bool signal = (g_world.flags & RT_WORLD_VALID) != 0u;
    float2 rt1 = float2(0.0f, 0.0f);
    float4 rt2 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float t = GIA_SKY;
    if (signal)
    {
        rt1 = GipLoadPlane(g_push.slots.y, r).xy;
        rt2 = GipLoadPlane(g_push.slots.z, r);
        signal = GbRt1Valid(rt1) && any(rt2 > 0.0f) && rt2.w > 0.0f;
    }
    if (signal)
    {
        float stored = GipLoadPlane(g_push.slots.x, r).r;
        t = GiaLinearDepth(stored, fr.lin);
        signal = t < GIA_SKY;
    }
    if (signal)
    {
        // a vehicle's pixel (the depth pass marks it in the lowest bit of the traced linear depth) takes the field's value: no ray (a car's own surface is in the top level)
        signal = (asuint(g_textures[g_push.slots.w].Load(int3(tex, 0)).x) & 1u) == 0u;
    }
    if (!signal)
    {
        GipWriteNone(p);
        skipped = 1u;
    }
    else
    {
        const float3 n = GbRt1Decode(rt1);
        const float3 pos = GiaWorldPosition(fr, (float)r.x + 0.5f, (float)r.y + 0.5f, fr.render.x, fr.render.y, t);
        const RtHitFrame f = RtWorldHitFrame();
        // the ray: the pixel's place and the cosine direction, in the top level's view space (the probes' conversion: the world's axes in view space)
        const float2 noise = GipNoise(uint(p.x), uint(p.y), r1.x);
        const float3 dirWorld = GipCosineDirection(n, noise.x, noise.y);
        const float3 rel = pos - g_world.camera;
        const float3 posView = f.axisX * rel.x + f.axisY * rel.y + f.axisZ * rel.z;
        const float3 nView = f.axisX * n.x + f.axisY * n.y + f.axisZ * n.z;
        const float3 direction = f.axisX * dirWorld.x + f.axisY * dirWorld.y + f.axisZ * dirWorld.z;
        const float3 origin = posView + nView * GipOriginLift(t);
        const float footprint0 = pixelAngle * t;
        const float tanHalf = 0.5f * pixelAngle;

        const CityHitTraceResult hit = CityHitTrace(origin, direction, 0.0f, reach, 0x80u | CAR_RT_MASK_ALL, footprint0, tanHalf, 0.5f);
        float3 radiance = float3(0.0f, 0.0f, 0.0f);
        bool counted = false;
        bool anyHit = hit.hit != 0u;
        traced_count = 1u;
        if (hit.hit != 0u && CityHitIsCity(hit.inst.instanceId))
        {
            const CityHit decoded = CityHitDecode(hit.inst.instanceId, hit.inst.geometryIndex, hit.inst.primitiveIndex, hit.inst.barycentrics);
            if (decoded.valid != 0u)
            {
                CityHitParams params;
                params.mode = CITY_HIT_MODE_GI;
                params.flags = CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE;
                params.units = 1.0f;
                params.sunReach = 0.0f;
                params.sunTan = 0.01388f;   // tan of three times the sun's disc, as the city's shadow pass: the temporal stage averages the jitter
                params.lampFactor = g_world.lampFactor;
                params.wet = g_world.wet;
                params.aoMix = 0.5f;
                params.lodBias = 0.0f;
                params.bounce = bounce;
                params.lampFirst = 0u;
                params.lampCount = 0u;
                if (g_world.lampFactor > 0.0f && (g_world.flags & RT_WORLD_LAMPS) != 0u)
                {
                    params.flags = params.flags | CITY_HIT_P_LAMPS | CITY_HIT_P_LAMP_SHADOW;
                }
                const uint h = GipHash(uint(p.x) * 0x9E3779B1u + uint(p.y) * 0x85EBCA6Bu + r1.x * 0x27d4eb2fu);
                params.noise = float3(GipUnit(h), GipUnit(GipHash(h)), GipUnit(GipHash(h ^ 0x68E31DA4u)));
                CityHitRay ray;
                ray.origin = origin;
                ray.direction = direction;
                ray.t = hit.t;
                ray.footprint = footprint0 + 2.0f * tanHalf * hit.t;
                const CityHitShaded s = CityHitShadeHit(params, f, ray, hit.inst, decoded);
                if (s.valid != 0u)
                {
                    radiance = GipClampRadiance(s.colour);
                    counted = true;
                    hit_count = 1u;
                }
            }
            if (!counted)
            {
                no_data = 1u;
                anyHit = false;   // a surface with no data in the pool: the field stands in
            }
        }
        else if (hit.hit != 0u)
        {
            const float3x4 m = float3x4(float4(hit.inst.row0, 0.0f), float4(hit.inst.row1, 0.0f), float4(hit.inst.row2, 0.0f));
            const float3 cn = CarRtHitNormal(hit.instanceIndex, hit.inst.primitiveIndex, hit.inst.barycentrics, m, direction);
            radiance = GipCarRadiance(RtHitAmbientLight(f, RtHitHemisphere(f, cn)), f.sun, dot(cn, f.toSun));
            counted = true;
            car_count = 1u;
        }
        if (!counted)
        {
            const GiSample s = GiLookup(pos, n);
            radiance = GipMissRadiance(s.irradiance);
            miss_count = hit.hit == 0u ? 1u : 0u;
        }

        // the denoiser's guides: the view space normal facing the camera, the depth along the view axis, the hit distance and the share
        float3 nv = float3(dot(n, fr.right), dot(n, fr.up), dot(n, fr.forward));
        const float3 pv = float3((((float)r.x + 0.5f) / fr.render.x * 2.0f - 1.0f) * fr.tanX * t, (1.0f - ((float)r.y + 0.5f) / fr.render.y * 2.0f) * fr.tanY * t, t);
        if (dot(nv, pv) > 0.0f)
        {
            nv = -nv;
        }
        g_signal[p] = float4(radiance, 0.0f);
        g_depth[p] = t;
        g_normal[p] = float4(nv, 1.0f);
        g_hit[p] = float2(GipHitDistance(true, anyHit, hit.t), 1.0f);
    }

    // the census: one atomic a counter a wave
    if (stats)
    {
        const uint tracedSum = WaveActiveSum(traced_count);
        const uint hitSum = WaveActiveSum(hit_count);
        const uint carSum = WaveActiveSum(car_count);
        const uint missSum = WaveActiveSum(miss_count);
        const uint skipSum = WaveActiveSum(skipped);
        const uint dataSum = WaveActiveSum(no_data);
        if (WaveIsFirstLane())
        {
            uint ignored = 0u;
            g_stats.InterlockedAdd(GIP_STAT_PIXELS * 4u, tracedSum, ignored);
            g_stats.InterlockedAdd(GIP_STAT_HITS * 4u, hitSum, ignored);
            g_stats.InterlockedAdd(GIP_STAT_CAR_HITS * 4u, carSum, ignored);
            g_stats.InterlockedAdd(GIP_STAT_MISSES * 4u, missSum, ignored);
            g_stats.InterlockedAdd(GIP_STAT_SKIPPED * 4u, skipSum, ignored);
            g_stats.InterlockedAdd(GIP_STAT_NO_DATA * 4u, dataSum, ignored);
        }
    }
}
