// MCLA GPU renderer -- the EMITTERS of the ray traced bounce light, the shader side (GI-BOUNCE-RT; the list, the groups and the layout are src/gpu/rt_emitters_rules.h, the use is the city hit library's
// CityHitEmitters in hlsl/city_hit_shade.hlsli).
//
// What it gives a pass that shades a city hit (the probe field's rays, the per-pixel pass, the reflections' city hits): the two functions the library's emitter hooks call, over the world frame's emitter
// rows (hlsl/rt_world_frame.hlsli: RtWorld::emitterCount, emitterStatic, RtEmitterRow). An emitter is a GpuLight (light_frame.h), WORLD space, the colour already times the night factor and the pass's gain, so
// the light of one at a point is hlsl/light_laws.hlsli's LlLightTerm: the very function the deferred light pass lights the picture with (a bounce and a direct term agree).
//
// THE INCLUDER defines, before this file: `RT_WORLD_SPACE` and includes hlsl/light_laws.hlsli and hlsl/rt_world_frame.hlsli; and after it (before hlsl/city_hit_shade.hlsli) the three hooks over its own RtWorld:
//     #define CITY_HIT_EMITTER_COUNT (g_world.emitterStatic)                       // the probe field: the static emitters only (a moving light would smear in world-fixed probes); the per-pixel pass: emitterCount
//     #define CITY_HIT_EMITTER_RADIANCE(i, P, N) RtEmitterRadiance(g_world, (i), (P), (N))
//     #define CITY_HIT_EMITTER_TO(i, P) RtEmitterTo(g_world, (i), (P))
// src/gpu/rt_emitters_rules_test.cpp compiles THIS FILE as C++ (hlsl_cpp_shim.h) and checks both functions against light_laws.h's LightTerm and the world rows' decode on random lights and points; that is why the
// text keeps to the subset both languages read: scalar and float3 arithmetic, no out parameters, dot.
//
// The names: every one starts with RtEmitter.

#ifndef MCLA_RT_EMITTERS_HLSLI
#define MCLA_RT_EMITTERS_HLSLI

// How many emitters the frame uploaded (row 44 of the world rows, one load): a pass that loads the whole RtWorld only for a hit that has emitters asks this first.
uint RtEmittersUploaded()
{
    return gRtWorld[RT_WORLD_ROW_EMITTERS].x;
}

// Emitter `i` as the light pass reads a row set (LlGpuLight: row0 position and range, row1 direction and the outer cosine, row2 colour and the inner cosine, then packed0, law_a, packed1, shadow_weight).
LlGpuLight RtEmitterGpuLight(RtWorld w, uint i)
{
    const uint4 a = RtEmitterRow(w, i, 0u);
    const uint4 b = RtEmitterRow(w, i, 1u);
    const uint4 c = RtEmitterRow(w, i, 2u);
    const uint4 d = RtEmitterRow(w, i, 3u);
    LlGpuLight g;
    g.row0 = float4(asfloat(a.x), asfloat(a.y), asfloat(a.z), asfloat(a.w));
    g.row1 = float4(asfloat(b.x), asfloat(b.y), asfloat(b.z), asfloat(b.w));
    g.row2 = float4(asfloat(c.x), asfloat(c.y), asfloat(c.z), asfloat(c.w));
    g.packed0 = d.x;
    g.law_a = asfloat(d.y);
    g.packed1 = d.z;
    g.shadow_weight = asfloat(d.w);
    return g;
}

// The unshadowed light emitter `i` sends to a VIEW space point with a unit normal (facing the viewer): the law's term (colour x its scalar part: LlLightTerm, visibility 1) at the point in the game's world. The
// first load is row 0 alone (the position and the range): an emitter out of its range (nearly every one, for nearly every hit) costs that one load and a dot product; the other three rows are read only for the
// ones that may reach.
float3 RtEmitterRadiance(RtWorld w, uint i, float3 viewPoint, float3 viewNormal)
{
    const uint4 a = RtEmitterRow(w, i, 0u);
    const float3 pw = RtWorldPoint(w, viewPoint);
    const float3 d = float3(asfloat(a.x), asfloat(a.y), asfloat(a.z)) - pw;
    const float range = asfloat(a.w);
    if (!(dot(d, d) < range * range))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 nw = float3(dot(viewNormal, w.axisX), dot(viewNormal, w.axisY), dot(viewNormal, w.axisZ));
    return LlLightTerm(LlUnpack(RtEmitterGpuLight(w, i)), pw, nw, 1.0);
}

// The vector from a VIEW space point to emitter `i`, in view space (its length is the distance): the world delta along the world's axes in view space (the inverse of RtWorldPoint's).
float3 RtEmitterTo(RtWorld w, uint i, float3 viewPoint)
{
    const uint4 a = RtEmitterRow(w, i, 0u);
    const float3 d = float3(asfloat(a.x), asfloat(a.y), asfloat(a.z)) - RtWorldPoint(w, viewPoint);
    return w.axisX * d.x + w.axisY * d.y + w.axisZ * d.z;
}

#endif  // MCLA_RT_EMITTERS_HLSLI
