// MCLA GPU renderer -- the lamp term of a hit through the world frame's grid (CITY-RT package FRAME, R7): the city hit library takes the grid for its lamps with `#define CITY_HIT_LAMP_GRID` (CityHitLamps: the
// cursor's lamps, at most kCityHitLampMax of them, the sum capped), and this pass is its reference consumer: the two files compile together for DXIL and SPIR-V and validate (spirv-val). NOT WIRED: no host
// pass records it; with the probe (rt_world_probe_cs.hlsl) it is the pass a first run can use to read the lamps back through the real descriptor set and compare them with rt_world_frame_rules.h
// LampLightAt (the twin of the library's loop) and LampsNear (the twin of the cursor's walk).
//
// Layout (as the probe): set 0 the bindless textures (space0), set 1 this pass's own (space1: t0 the points, u0 the answers, u1 the lamp term's counters), set 2 the ray tracing set (space2: t0 the top
// level, t6..t9 the hit data, t10 the world rows, t11 the lamps), the push block at (b0, space3). Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query: the library includes the ray queries
// of its sun ray and its lamp ray).
//
// Point i (t0[2 i], t0[2 i + 1]): xyz a view space position and w the radius; xyz a view space UNIT normal. Answer i (u0[i]): xyz the lamps' light (library units: colour x term x cone x N.L, the schedule in
// the colours), w the number of lamps the lookup found. Push control: x the number of points, y the mode (0 the plain sum of the cursor's lamps, as the loop below; 1 the LIBRARY's CityHitLamps: the cap,
// the sum's cap, and with the flag the one lamp's shadow ray), z the library's extra flags (CITY_HIT_P_LAMP_SHADOW, CITY_HIT_P_CALIBRATE: a stats frame counts into u1: words 0..7, CITY_HIT_CENSUS_*).

#define CAR_RT_SPACE 2
#define RT_WORLD_SPACE 2
#define CITY_HIT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
StructuredBuffer<float4> g_points : register(t0, space1);
RWStructuredBuffer<float4> g_answers : register(u0, space1);

RWByteAddressBuffer g_census : register(u1, space1);  // the lamp term's and the glow's counters (library CITY_HIT_CENSUS_*, eight words)

#include "rt_world_frame.hlsli"
#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)
#define CITY_HIT_LAMP_GRID
#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)
#define CITY_HIT_CENSUS_ADD(word, value) g_census.InterlockedAdd((word) * 4u, (value))
#include "city_hit_shade.hlsli"

struct RtWorldLampLightPush
{
    // x the number of points, y the mode (0 the plain sum, 1 the library's CityHitLamps), z the library's extra flags, w a seed for the lamp choice's noise
    uint4 control;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtWorldLampLightPush> g_push;
#else
ConstantBuffer<RtWorldLampLightPush> g_push : register(b0, space3);
#endif

// The sum of the lamps of the grid around `position` (view space) at a surface with the unit `normal`: what CityHitLamps does without the lamp shadow flag, over the cursor instead of the flat range.
float4 RtWorldLampSum(RtWorld w, CityHitParams p, float3 position, float3 normal, float radius)
{
    float3 sum = float3(0.0, 0.0, 0.0);
    float found = 0.0;
    RtLampCursor c = RtLampBegin(w, position, radius);
    RT_WORLD_LOOP
    while (c.done == 0u)
    {
        const CityHitLamp lamp = CityHitLampFromRows(RtLampRow(w, c.index, 0u), RtLampRow(w, c.index, 1u), RtLampRow(w, c.index, 2u));
        sum += CityHitLampAt(p, lamp, position, normal).radiance;
        found += 1.0;
        c = RtLampStep(w, c);
    }
    return float4(sum, found);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_push.control.x)
    {
        return;
    }
    const RtWorld w = RtWorldLoad();
    CityHitParams p;
    p.mode = CITY_HIT_MODE_REFLECT;
    p.flags = CITY_HIT_P_LAMPS | (g_push.control.z & (CITY_HIT_P_LAMP_SHADOW | CITY_HIT_P_CALIBRATE));
    p.units = w.units;
    p.sunReach = 0.0;
    p.sunTan = 0.0;
    p.lampFactor = w.lampFactor;  // the records' colours hold each lamp's activity over this
    p.wet = w.wet;
    p.aoMix = 0.5;
    p.lodBias = 0.0;
    p.bounce = 0.0;
    p.lampFirst = 0u;
    p.lampCount = 0u;
    p.noise = float3(0.5, 0.5, frac(float(id.x * 2654435761u + g_push.control.w) * 2.3283064e-10));
    const float4 a = g_points[id.x * 2u];
    const float4 b = g_points[id.x * 2u + 1u];
    const float radius = a.w > 0.0 ? a.w : w.lookupM;
    float4 answer = float4(0.0, 0.0, 0.0, 0.0);
    if ((w.flags & RT_WORLD_VALID) != 0u && w.lampFactor > 0.0)
    {
        answer = RtWorldLampSum(w, p, a.xyz, b.xyz, radius);
        if (g_push.control.y == 1u)
        {
            answer.xyz = CityHitLamps(p, a.xyz, b.xyz, 0.05);  // the library's: the first kCityHitLampMax lamps, the sum capped, with the flag one lamp and its shadow ray
        }
    }
    g_answers[id.x] = answer;
}
