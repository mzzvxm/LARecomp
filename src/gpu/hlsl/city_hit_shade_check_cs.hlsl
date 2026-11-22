// MCLA GPU renderer -- the city hit library's reference consumer: a ray per pixel into the city, resolved as a reflection's hit (CITY-RT M2 package H4; the library is hlsl/city_hit_shade.hlsli).
//
// What the library does for R1 to R4 and G2 in one small pass, so that the library is compiled (DXIL, SPIR-V, spirv-val) and so that the first run of the hit shading does not need a car:
// for every pixel of an image a primary ray goes from the camera into the top level (the city's structures, instance mask 0x80, and the cars' parts), the closest hit goes through
// CityHitTrace (the cut-outs' exact alpha in the candidate loop) and CityHitResolve (the picture where the hit's point is seen, the model where it is not), and the colour is written. With the
// view flag the output shows what the resolve decided instead of the colour: the source (screen green, shaded red, blend yellow, none black), the class by a ramp, the sun's visibility, the
// glow. NOT WIRED: no host pass records it (a debug pass would, once the car scene's frame rows exist for a frame with no car: CITY-RT-H4 "needs"); the chain's unit stage compiles it.
//
// Layout (the car scene's set, as city_rt_shadow_cs.hlsl): set 0 the bindless texture set (t0, space0), set 1 this pass's own (space1: t0 the frame's rows, t1 the lamps, t2 the class gains,
// u0 the output image, u1 the calibration's counters), set 2 the car RT set (CAR_RT_SPACE 2: the top level, the car's tables, the hit data's t6..t9), the push block at (b0, space3).
// Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
StructuredBuffer<uint4> g_frame : register(t0, space1);       // the frame's rows (rt_hit_frame.h): the sun, the ambient, the environment map, the cascades
StructuredBuffer<uint4> g_lamps : register(t1, space1);       // three rows a lamp (city_hit_shade_rules.h MakeGpuLamp)
StructuredBuffer<float> g_gains : register(t2, space1);       // the class gains (rt_hit_calibration.h ClassGains::Table)
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);
RWByteAddressBuffer g_stats : register(u1, space1);           // rt_hit_calibration.h kStatsWords: per class count, the picture's summed log, the model's

struct CityHitCheckPush
{
    // x the depth image's bindless slot, y the picture's (colour) slot, z, w the output's width and height
    uint4 slots;
    // x, y tan of half the horizontal and the vertical field of view, z the picture's width, w its height
    float4 lens;
    // x the units (2^exp_bias), y the lamp schedule, z the wetness, w the cone's half angle (tangent)
    float4 tune;
    // x the first lamp, y the number of lamps, z the library's flags (CITY_HIT_P_*), w the view (0 the colour, 1 the source, 2 the class, 3 the sun, 4 the glow)
    uint4 control;
    // x the sun disc's tangent, y the sun ray's reach (0: the library's), z the frame's serial (the noise), w the screen's depth tolerance (m)
    float4 sun;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CityHitCheckPush> g_push;
#else
ConstantBuffer<CityHitCheckPush> g_push : register(b0, space3);
#endif

#define CITY_HIT_SPACE 2
#define RT_HIT_FRAME_ROW(i) g_frame[i]
#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_SCREEN_DEPTH(texel) g_textures[g_push.slots.x].Load(int3(texel, 0)).x
#define CITY_HIT_SCREEN_COLOUR(uv, mip) g_textures[g_push.slots.y].Load(int3(int2(uv * g_push.lens.zw), int(mip))).rgb
#define CITY_HIT_GAIN(cls) g_gains[cls]
#define CITY_HIT_STAT_ADD(word, value) g_stats.InterlockedAdd((word) * 4u, (value))
#define CITY_HIT_LAMP_LOAD(i) CityHitLampFromRows(g_lamps[(i) * 3u], g_lamps[(i) * 3u + 1u], g_lamps[(i) * 3u + 2u])
#include "city_hit_shade.hlsli"

float Ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float3 Ramp(float v)
{
    return float3(saturate(1.5 - abs(v * 4.0 - 3.0)), saturate(1.5 - abs(v * 4.0 - 2.0)), saturate(1.5 - abs(v * 4.0 - 1.0)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_push.slots.z || id.y >= g_push.slots.w)
    {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_push.slots.zw);
    const float3 direction = normalize(float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0));
    const float tanHalf = g_push.tune.w;
    const float pixelWidth = 2.0 * g_push.lens.y / float(g_push.slots.w);
    const CityHitTraceResult hit = CityHitTrace(float3(0.0, 0.0, 0.0), direction, 0.0, 2000.0, CAR_RT_MASK_ALL | 0x80u, pixelWidth, tanHalf, 0.5);
    if (hit.hit == 0u || !CityHitIsCity(hit.inst.instanceId))
    {
        g_output[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    CityHitParams p;
    p.mode = CITY_HIT_MODE_REFLECT;
    p.flags = g_push.control.z;
    p.units = g_push.tune.x;
    p.sunReach = g_push.sun.y;
    p.sunTan = g_push.sun.x;
    p.lampFactor = g_push.tune.y;
    p.wet = g_push.tune.z;
    p.aoMix = 0.5;
    p.lodBias = 0.0;
    p.bounce = 0.0;
    p.lampFirst = g_push.control.x;
    p.lampCount = g_push.control.y;
    const float2 noise = frac(float2(Ign(float2(id.xy)), Ign(float2(id.yx) + float2(31.0, 11.0))) + float(g_push.sun.z) * float2(0.75487766624669, 0.56984029099806));
    p.noise = float3(noise.x, noise.y, frac(noise.x + noise.y * 1.618));
    CityHitScreen s;
    s.lens = g_push.lens;
    s.tolerance = float4(g_push.sun.w, 0.01, 0.04, 0.05);
    s.cap = 0.0;
    CityHitRay ray;
    ray.origin = float3(0.0, 0.0, 0.0);
    ray.direction = direction;
    ray.t = hit.t;
    ray.footprint = pixelWidth * hit.t + 2.0 * tanHalf * hit.t;
    const RtHitFrame f = RtHitLoadFrame();
    const CityHitResolved r = CityHitResolve(p, f, s, ray, hit.inst);
    float3 colour = r.colour;
    const uint view = g_push.control.w;
    if (view == 1u)
    {
        colour = r.source == CITY_HIT_SOURCE_SCREEN ? float3(0.0, 1.0, 0.0) : (r.source == CITY_HIT_SOURCE_SHADED ? float3(1.0, 0.0, 0.0) : (r.source == CITY_HIT_SOURCE_BLEND ? float3(1.0, 1.0, 0.0) : float3(0.0, 0.0, 0.0)));
    }
    else if (view == 2u)
    {
        colour = Ramp(float(r.cls) / 15.0);
    }
    else if (view == 3u)
    {
        colour = float3(r.ndl, r.visible, r.ndl * r.visible);
    }
    else if (view == 4u)
    {
        colour = float3(r.glow, r.glow, r.glow);
    }
    g_output[id.xy] = float4(colour, r.source == CITY_HIT_SOURCE_NONE ? 0.0 : 1.0);
}
