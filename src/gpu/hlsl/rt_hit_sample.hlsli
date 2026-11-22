// MCLA GPU renderer -- the reads every hit shading shares: bindless textures by slot, the environment map, the game's sun cascades, the sun's ray (CITY-RT M2 package H4; second half of
// rt_hit_common.hlsli, the first is rt_hit_frame.hlsli; the C++ twin is src/gpu/rt_hit_frame.h).
//
// A bindless texture read by slot (the game's piecewise-linear gamma where the slot's bit 31 asks, bilinear, two levels, wrap or clamp), the environment map's paraboloid, the lookup into the
// game's cascades of the sun's shadow map (the collector's own, copied), the sun's yes/no ray.
//
// THE INCLUDER defines RT_HIT_FRAME_ROW(i) (rt_hit_frame.hlsli), declares `Texture2D<float4> g_textures[]` (the bindless textures, space 0: they are read with Load, so no sampler set is
// needed in the pass's layout) and, unless it defines RT_HIT_NO_RAYQUERY, RT_HIT_TLAS (the top level the sun's ray looks in: the car passes' gCarRtTlas).
// src/gpu/rt_hit_frame_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL types, with RT_HIT_NO_RAYQUERY) and checks its arithmetic against rt_hit_frame.h; that is why the text
// keeps to a subset both languages read (RT_HIT_UNROLL stands for [unroll]; the only out parameters are the texture's GetDimensions).

#ifndef MCLA_RT_HIT_SAMPLE_HLSLI
#define MCLA_RT_HIT_SAMPLE_HLSLI

#include "rt_hit_frame.hlsli"

#ifndef RT_HIT_UNROLL
#define RT_HIT_UNROLL [unroll]
#endif

// The game's piecewise-linear gamma to linear (the translated shaders' pwlGammaToLinearScalar; the fetch's bit 31 asks for it).
float RtHitPwl(float gamma)
{
    gamma = saturate(gamma);
    float scale;
    float offset;
    if (gamma >= 96.0 / 255.0)
    {
        if (gamma >= 192.0 / 255.0)
        {
            scale = 8.0 / 1024.0;
            offset = -1024.0;
        }
        else
        {
            scale = 4.0 / 1024.0;
            offset = -256.0;
        }
    }
    else
    {
        if (gamma >= 64.0 / 255.0)
        {
            scale = 2.0 / 1024.0;
            offset = -64.0;
        }
        else
        {
            scale = 1.0 / 1024.0;
            offset = 0.0;
        }
    }
    float lin = gamma * ((255.0 * 1024.0) * scale) + offset;
    lin += trunc(lin * scale);
    return lin * (1.0 / 1023.0);
}

// One texel of level `mip`, wrapped (the diffuse) or clamped (the environment), the gamma applied where the fetch asks.
float4 RtHitTexel(Texture2D<float4> tex, int2 p, uint mip, int2 size, bool wrap, bool gamma)
{
    int2 q;
    if (wrap)
    {
        q = ((p % size) + size) % size;
    }
    else
    {
        q = clamp(p, int2(0, 0), size - 1);
    }
    float4 t = tex.Load(int3(q, int(mip)));
    if (gamma)
    {
        t.rgb = float3(RtHitPwl(t.r), RtHitPwl(t.g), RtHitPwl(t.b));
    }
    return t;
}

float4 RtHitBilinear(Texture2D<float4> tex, float2 uv, uint mip, int2 size0, bool wrap, bool gamma)
{
    const int2 size = max(size0 >> int(mip), int2(1, 1));
    const float2 p = uv * float2(size) - 0.5;
    const int2 i0 = int2(floor(p));
    const float2 f = p - float2(i0);
    const float4 a = RtHitTexel(tex, i0, mip, size, wrap, gamma);
    const float4 b = RtHitTexel(tex, i0 + int2(1, 0), mip, size, wrap, gamma);
    const float4 c = RtHitTexel(tex, i0 + int2(0, 1), mip, size, wrap, gamma);
    const float4 d = RtHitTexel(tex, i0 + int2(1, 1), mip, size, wrap, gamma);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// A texture read at mip level `lod` (a float: two levels blended), by the bindless slot (bit 31 the gamma bit).
float4 RtHitSample(uint slot, float2 uv, float lod, bool wrap)
{
    Texture2D<float4> tex = g_textures[NonUniformResourceIndex(slot & 0x7FFFFFFFu)];
    uint w;
    uint h;
    uint levels;
    tex.GetDimensions(0, w, h, levels);
    const bool gamma = (slot & 0x80000000u) != 0u;
    const float l = clamp(lod, 0.0, float(max(levels, 1u) - 1u));
    const uint l0 = uint(floor(l));
    const uint l1 = min(l0 + 1u, max(levels, 1u) - 1u);
    const float f = l - float(l0);
    const int2 size0 = int2(int(w), int(h));
    float4 value = RtHitBilinear(tex, uv, l0, size0, wrap, gamma);
    if (l1 != l0 && f > 0.001)
    {
        value = lerp(value, RtHitBilinear(tex, uv, l1, size0, wrap, gamma), f);
    }
    return value;
}

// The game's environment map (a 512x256 dual paraboloid, four levels) at a unit WORLD direction (car_rt_material_rules.h EnvUv: decoded from xChrome::PS_Chrome).
float2 RtHitEnvUv(float3 r)
{
    const float a = -r.y;
    const float b = -r.z;
    const float c = -r.x;
    float2 uv;
    if (b > 0.0)
    {
        uv = float2(0.25 - c / (4.0 + 4.0 * b), 0.5 + a / (2.0 + 2.0 * b));
    }
    else
    {
        uv = float2(0.75 + c / (4.0 - 4.0 * b), 0.5 + a / (2.0 - 2.0 * b));
    }
    return uv;
}

// A depth texel of the cascade atlas, clamped to its size.
float RtHitLoadDepth(Texture2D<float4> atlas, int2 texel, int2 size)
{
    return atlas.Load(int3(clamp(texel, int2(0, 0), size - 1), 0)).x;
}

// The fraction of the sun's light that reaches the WORLD point `p` through the game's cascades: the collector's own lookup, as volumetrics_inject_cs.hlsl CascadeLit has it and
// car_rt_material_rules.h CascadeLit does (the finest box that holds the point, the atlas uv and depth from that cascade's matrix, a bilinear weighted compare: lit where the stored
// depth is at least the receiver's). 1 where the point is in none of the boxes.
float RtHitCascadeLit(float3 p)
{
    const uint4 header = RT_HIT_FRAME_ROW(kRtHitCascadeHeaderRow);
    const uint cascades = min(header.y, kRtHitMaxCascades);
    if (cascades == 0u || header.x == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    const float3 right = asfloat(RT_HIT_FRAME_ROW(kRtHitCascadeRightRow).xyz);
    const float3 top = asfloat(RT_HIT_FRAME_ROW(kRtHitCascadeTopRow).xyz);
    const float r = dot(p, right);
    const float t = dot(p, top);
    uint inside = 0u;
    RT_HIT_UNROLL
    for (uint i = 0u; i < kRtHitMaxCascades; ++i)
    {
        if (i < cascades)
        {
            const float4 box = asfloat(RT_HIT_FRAME_ROW(kRtHitCascadeFirstRow + i * kRtHitCascadeRowsEach + 4u));
            inside += (abs(r + box.x) < box.y && abs(t + box.z) < box.w) ? 1u : 0u;
        }
    }
    if (inside == 0u)
    {
        return 1.0;  // outside every cascade: no shadow data
    }
    const uint base = kRtHitCascadeFirstRow + (cascades - inside) * kRtHitCascadeRowsEach;
    const float4 r0 = asfloat(RT_HIT_FRAME_ROW(base));
    const float4 r1 = asfloat(RT_HIT_FRAME_ROW(base + 1u));
    const float4 r2 = asfloat(RT_HIT_FRAME_ROW(base + 2u));
    const float4 r3 = asfloat(RT_HIT_FRAME_ROW(base + 3u));
    const float u = dot(p, float3(r0.x, r1.x, r2.x)) + r3.x;
    const float v = dot(p, float3(r0.y, r1.y, r2.y)) + r3.y;
    const float depth = (dot(p, float3(r0.z, r1.z, r2.z)) + r3.z) * (dot(p, float3(r0.w, r1.w, r2.w)) + r3.w) + asfloat(header.z);
    Texture2D<float4> atlas = g_textures[header.x];
    uint w;
    uint h;
    uint levels;
    atlas.GetDimensions(0, w, h, levels);
    const int2 size = int2(int(w), int(h));
    const float2 tc = float2(u, v) * float2(size) - 0.5;
    const int2 i0 = int2(floor(tc));
    const float2 fr = tc - float2(i0);
    const float s00 = RtHitLoadDepth(atlas, i0, size) >= depth ? 1.0 : 0.0;
    const float s10 = RtHitLoadDepth(atlas, i0 + int2(1, 0), size) >= depth ? 1.0 : 0.0;
    const float s01 = RtHitLoadDepth(atlas, i0 + int2(0, 1), size) >= depth ? 1.0 : 0.0;
    const float s11 = RtHitLoadDepth(atlas, i0 + int2(1, 1), size) >= depth ? 1.0 : 0.0;
    return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);
}

// Is the point lit by the sun: one ray towards it against the whole scene, a few metres (a car's own shadow on itself and on its neighbour). `mask` is the instance mask the ray looks at
// (the car passes: CAR_RT_MASK_ALL, every part of a car; a pass that holds the city too gives it the city's bit as well).
#ifndef RT_HIT_NO_RAYQUERY
float RtHitSunVisible(float3 origin, float3 toSun, float reach, uint mask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = toSun;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(RT_HIT_TLAS, RAY_FLAG_NONE, mask, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
#endif

// The share of the sun's light that reaches the point `hit` (view space) of a surface with the unit `normal` through the game's cascades, with the collector's own fade: the point is lifted off
// the surface along the normal, turned into the game's world (the camera's place in the frame's rows plus the view vector along the world's axes) and looked up (RtHitCascadeLit). Only
// for a frame that has the cascades (RT_HIT_FRAME_CASCADE): the caller asks.
float RtHitCascadeVisible(RtHitFrame f, float3 hit, float3 normal)
{
    const float3 camera = asfloat(RT_HIT_FRAME_ROW(kRtHitCameraRow).xyz);
    const float3 lifted = hit + normal * kRtHitCascadeNormalOffset;
    const float3 world = camera + float3(dot(lifted, f.axisX), dot(lifted, f.axisY), dot(lifted, f.axisZ));
    const float lit = RtHitCascadeLit(world);
    return lit + (1.0 - lit) * saturate(asfloat(RT_HIT_FRAME_ROW(kRtHitCascadeHeaderRow).w));
}

// The game's environment map is in the WORLD's axes; the passes work in view space: the unit view-space direction `reflected` in the world's axes ...
float3 RtHitEnvDirection(RtHitFrame f, float3 reflected)
{
    return float3(dot(f.axisX, reflected), dot(f.axisY, reflected), dot(f.axisZ, reflected));
}

// ... and the sharp map read in that direction at mip level `lod`.
float3 RtHitEnvRead(RtHitFrame f, float3 world, float lod)
{
    return RtHitSample(f.envSharp, RtHitEnvUv(world), lod, false).rgb;
}

float3 RtHitEnvAt(RtHitFrame f, float3 reflected, float lod)
{
    return RtHitEnvRead(f, RtHitEnvDirection(f, reflected), lod);
}

#endif  // MCLA_RT_HIT_SAMPLE_HLSLI
