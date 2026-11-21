// MCLA GPU renderer -- the world frame's probe (CITY-RT package FRAME): one thread a point, what the lamp lookup and the world rows say there. NOT WIRED: no host pass records it yet; it is the reference
// consumer of hlsl/rt_world_frame.hlsli, so that the file is compiled for DXIL and SPIR-V and validated (spirv-val), and the pass a first run can use to read the buffers back through the real
// descriptor set and compare them with the host's own copy (rt_world_frame.h RtWorldFrame::Current(): the same rows and the same lamp words, and rt_world_frame_rules.h LampsNear is the twin of
// the loop below).
//
// Layout (the car scene's set, as city_hit_shade_check_cs.hlsl): set 1 this pass's own (space1: t0 the points, u0 the answers), set 2 the ray tracing set (RT_WORLD_SPACE 2: t10 the world rows and
// t11 the lamps; the pass uses no other register of it), the push block at (b0, space3). Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2.
//
// Point i (t0[i]): xyz a VIEW space position, w the radius in metres (0: the world's default lookup radius). Answer i (u0[i], four words): x the lamps the lookup found, y the first lamp's index
// (0xFFFFFFFF: none), z the float bits of the sum of the intensities of the lamps found, w the float bits of the sum of the lamps' summed colour's luminance weights (r + g + b of every lamp's
// colour row): the host adds the same numbers over LampsNear's answer for the same point. Answer `count` (one extra) is the world's summary: x serial, y flags, z the float bits of the hour,
// w the float bits of the lamp schedule.

#define RT_WORLD_SPACE 2
#include "rt_world_frame.hlsli"

StructuredBuffer<float4> g_points : register(t0, space1);
RWStructuredBuffer<uint4> g_answers : register(u0, space1);

struct RtWorldProbePush
{
    // x the number of points, y .. w reserved
    uint4 control;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtWorldProbePush> g_push;
#else
ConstantBuffer<RtWorldProbePush> g_push : register(b0, space3);
#endif

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint count = g_push.control.x;
    if (id.x > count)
    {
        return;
    }
    const RtWorld w = RtWorldLoad();
    if (id.x == count)
    {
        g_answers[id.x] = uint4(w.serial, w.flags, asuint(w.hour), asuint(w.lampFactor));
        return;
    }
    const float4 probe = g_points[id.x];
    const float radius = probe.w > 0.0 ? probe.w : w.lookupM;
    uint found = 0u;
    uint first = 0xFFFFFFFFu;
    float intensity = 0.0;
    float colour = 0.0;
    RtLampCursor c = RtLampBegin(w, probe.xyz, radius);
    RT_WORLD_LOOP
    while (c.done == 0u)
    {
        if (found == 0u)
        {
            first = c.index;
        }
        found = found + 1u;
        const uint4 a = RtLampRow(w, c.index, 0u);
        const uint4 k = RtLampRow(w, c.index, 2u);
        intensity += asfloat(a.w);
        colour += asfloat(k.x) + asfloat(k.y) + asfloat(k.z);
        c = RtLampStep(w, c);
    }
    g_answers[id.x] = uint4(found, first, asuint(intensity), asuint(colour));
}
