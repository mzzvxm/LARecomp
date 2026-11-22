// MCLA GPU renderer -- the probe update's trace pass (gi_update.h, CITY-RT M3 package G2; the rules and the reasons are gi_update_rules.h's comment, the shared maths gi_update_common.hlsli).
//
// One thread a ray of a probe the scheduler chose (64 rays a probe: a group of 64 threads is one probe; 32 rays a probe: two probes). The probe's rays are a spherical Fibonacci set turned by a random
// rotation that is new every update. A ray goes from the probe through the top level (the city only: mask 0x80, cars are not occluders of the field's rays, they RECEIVE its light), and its closest hit
// is shaded by the city hit library in GI MODE (hlsl/city_hit_shade.hlsli, CityHitShadeHit): the surface's AVERAGE colour (no texture) times the light there, which is
//   the game's ambient (the hemisphere term), the sun by N.L with a shadow ray against the whole top level, the lamps of the world frame at night (the library picks ONE of the nearest eight by their
//   unshadowed brightness and traces a shadow ray to it), the glow of emissive surfaces (neon, lamp lenses, lit windows by the lamp schedule), and the FIELD ITSELF one frame old (CITY_HIT_FEEDBACK:
//   GiLookup times the confidence, times the bounce weight of the params: the second bounce and the ones after it for free; 0 = strictly one bounce).
// The result is a RAY RECORD (gi_update_common.hlsli GiuPackRay): the radiance as halves, the flags (hit, back face, lod), the distance. A ray that leaves the city is a record with no hit: it adds
// nothing to the irradiance (the sky is in the game's ambient already) and lifts the probe's sky visibility. The records are the blend pass's input.
// The units are the field's: the paint's units (the frame's colours c39 and c26: params.units = 1), NOT the picture's (x 2^exp_bias).
//
// An INSIDE probe (the state word's bit: the blend found most of its rays on back faces) is not traced at 7 of 8 visits: its first thread marks record 0 SKIPPED and the group does nothing else.
//
// Layout (gi_update.cpp, the trace layout): set 0 (space0) the bindless Texture2D<float4>[] (the field's three textures are read through their slots in the frame's rows), set 1 (space1) the samplers
// (0 = linear clamp), set 2 (space2) this pass's own set: t0 the frame's rows (the field's 20 rows, 320 bytes), t1 the probe list (a uint a probe), u2 the ray records, u5 the state words, u6 the
// counters (u3 and u4, the blend's two textures, are in the same set and not used here); set 3 (space3) the ray tracing set (the car scene's t0..t5, the hit data's t6..t9, the world frame's t10, t11:
// CAR_RT_SPACE 3); the push block at (b0, space4). Shader Model 6.5, SPIR-V 1.5 for Vulkan 1.2.

#define CAR_RT_SPACE 3
#define RT_WORLD_SPACE 3
#define CITY_HIT_SPACE 3
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[] : register(s0, space1);
ByteAddressBuffer g_rows : register(t0, space2);
ByteAddressBuffer g_probes : register(t1, space2);
RWByteAddressBuffer g_rays : register(u2, space2);
RWByteAddressBuffer g_state : register(u5, space2);
RWByteAddressBuffer g_stats : register(u6, space2);

struct GiuPush
{
    // x the probes in this chunk, y the rays a probe, z the chunk's first probe in the list, w the frame's serial (the rotations' noise)
    uint4 control;
    // x the stats flag, yzw 0
    uint4 flags;
    // x the bounce weight (the feedback's share), y the rays' reach in metres, z the hysteresis, w the jump hysteresis (the blend's)
    float4 tune;
    // x the tangent of the half angle of a ray's cone (the cut-outs' alpha level), yzw 0
    float4 tune2;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GiuPush> g_push;
#else
ConstantBuffer<GiuPush> g_push : register(b0, space4);
#endif

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) g_textures[GiSlot(GI_PRODUCT_IRRADIANCE)].SampleLevel(g_samplers[0], (uv), 0.0f)
#define GI_SAMPLE_MOMENTS(uv) g_textures[GiSlot(GI_PRODUCT_MOMENTS)].SampleLevel(g_samplers[0], (uv), 0.0f).xy
#define GI_LOAD_PROBE_WEIGHT(px, py) g_textures[GiSlot(GI_PRODUCT_PROBE_WEIGHT)].Load(int3((int)(px), (int)(py), 0)).x
#include "gi_common.hlsli"
#define GIF_RESET_WORD(i) 0u
#include "gi_field_common.hlsli"
#include "gi_update_common.hlsli"
#include "rt_world_frame.hlsli"

// This thread's world rows, and the lamps the hit being shaded is lit by (the nearest eight by their unshadowed brightness at it, picked from the world frame's grid; the library's lamp loops
// read them through CITY_HIT_LAMP_LOAD).
#define GIU_LAMPS 8u
static RtWorld g_world;
static uint g_lampIndex[GIU_LAMPS];

// The field's irradiance at a hit (view space point and unit normal), for the hit's bounce: the surfaces' light only (the sky is not in the field) where the field stands.
float3 GiuFeedback(float3 viewPoint, float3 viewNormal)
{
    float3 pw = RtWorldPoint(g_world, viewPoint);
    float3 nw = float3(dot(viewNormal, g_world.axisX), dot(viewNormal, g_world.axisY), dot(viewNormal, g_world.axisZ));
    GiSample s = GiLookup(pw, nw);
    return s.irradiance * s.confidence;
}

#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_FEEDBACK(P, N) GiuFeedback((P), (N))
#define CITY_HIT_LAMP_LOAD(i) CityHitLampFromRows(RtLampRow(g_world, g_lampIndex[(i)], 0u), RtLampRow(g_world, g_lampIndex[(i)], 1u), RtLampRow(g_world, g_lampIndex[(i)], 2u))
#include "city_hit_shade.hlsli"

// The first probe of cascade c in the state buffer (the probes of the cascades before it).
uint GiuStateBase(uint c)
{
    uint base = 0u;
    for (uint i = 0u; i < c; ++i)
    {
        GiCascade k = GiLoadCascade(i);
        base += k.dimX * k.dimY * k.dimZ;
    }
    return base;
}

// The up to GIU_LAMPS lamps of the grid round the hit that light it most, strongest first (the unshadowed brightness the library picks by), into g_lampIndex; the number found.
uint GiuPickLamps(CityHitParams p, float3 hitView, float3 normal)
{
    float best[GIU_LAMPS];
    uint count = 0u;
    for (uint z = 0u; z < GIU_LAMPS; ++z)
    {
        best[z] = 0.0f;
        g_lampIndex[z] = 0u;
    }
    RtLampCursor c = RtLampBegin(g_world, hitView, 64.0f);
    [loop]
    while (c.done == 0u)
    {
        const CityHitLamp lamp = CityHitLampFromRows(RtLampRow(g_world, c.index, 0u), RtLampRow(g_world, c.index, 1u), RtLampRow(g_world, c.index, 2u));
        const float luma = CityHitLuma(CityHitLampAt(p, lamp, hitView, normal).radiance);
        if (luma > best[GIU_LAMPS - 1u])
        {
            uint at = GIU_LAMPS - 1u;
            [loop]
            while (at > 0u && luma > best[at - 1u])
            {
                best[at] = best[at - 1u];
                g_lampIndex[at] = g_lampIndex[at - 1u];
                at = at - 1u;
            }
            best[at] = luma;
            g_lampIndex[at] = c.index;
            count = min(count + 1u, GIU_LAMPS);
        }
        c = RtLampStep(g_world, c);
    }
    return count;
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint rays = max(g_push.control.y, 1u);
    const uint perGroup = max(GIU_TRACE_GROUP / rays, 1u);
    const uint g = group.y * GIU_GROUPS_X + group.x;
    const uint slot = g * perGroup + tid / rays;
    const uint ray = tid % rays;
    const bool active = slot < g_push.control.x && tid < perGroup * rays;
    uint hits = 0u;
    uint escapes = 0u;
    uint backs = 0u;
    uint lods = 0u;
    uint traced = 0u;
    if (active)
    {
        const uint word = g_probes.Load((g_push.control.z + slot) * 4u);
        const uint cascade = GiuProbeCascade(word);
        const uint index = GiuProbeIndex(word);
        const uint frame = g_push.control.w;
        const GiCascade k = GiLoadCascade(cascade);
        const uint stateAt = GiuStateBase(cascade) + index;
        const uint state = g_state.Load(stateAt * 4u);
        if (GiuSkipInside(GifStateInside(state), frame, stateAt))
        {
            // an inside probe that is not looked at this visit: record 0 says so (the blend reads it) and nothing else is done
            if (ray == 0u)
            {
                g_rays.Store4((slot * rays) * GIU_RAY_BYTES, GiuPackRay(float3(0.0f, 0.0f, 0.0f), 0.0f, GIU_RAY_SKIPPED, 0u));
            }
        }
        else
        {
            g_world = RtWorldLoad();
            const RtHitFrame f = RtWorldHitFrame();
            // the probe's place in the world, and in the top level's view space (the camera plus the axes: rt_world_frame.hlsli RtWorldPoint, the inverse)
            const uint sx = index % k.dimX;
            const uint sy = (index / k.dimX) % k.dimY;
            const uint sz = index / (k.dimX * k.dimY);
            const GifCoord c = GifCoordOfSlot(k.originX, k.originY, k.originZ, k.dimX, k.dimY, k.dimZ, sx, sy, sz);
            const float3 probeWorld = float3((float)c.x * k.spacing, (float)c.y * k.spacing, (float)c.z * k.spacing);
            const float3 rel = probeWorld - g_world.camera;
            const float3 origin = f.axisX * rel.x + f.axisY * rel.y + f.axisZ * rel.z;
            const float4 q = GiuRotationQuat(GiuSeed(frame, cascade, index));
            const float3 dirWorld = GiuRayDirection(ray, rays, q);
            const float3 direction = f.axisX * dirWorld.x + f.axisY * dirWorld.y + f.axisZ * dirWorld.z;

            const CityHitTraceResult hit = CityHitTrace(origin, direction, 0.0f, g_push.tune.y, 0x80u, 0.0f, g_push.tune2.x, 0.5f);
            float3 radiance = float3(0.0f, 0.0f, 0.0f);
            float distance = g_push.tune.y;
            uint flags = 0u;
            uint cls = 0u;
            if (hit.hit != 0u)
            {
                const CityHit decoded = CityHitDecode(hit.inst.instanceId, hit.inst.geometryIndex, hit.inst.primitiveIndex, hit.inst.barycentrics);
                // A hit with no data (a geometry-only sector: its hit data blob is bad or missing) is a MISS, as the reflections and the pixel pass take it: no hit flag, the ray's reach as the distance, no light
                // (gi_update_rules.h HitRecord). It is not a black occluder: that adds no light AND no sky to its direction (full shadow) and darkens the probes beside every such sector.
                if (decoded.valid != 0u)
                {
                    flags = GIU_RAY_HIT;
                    distance = hit.t;
                    lods = (hit.inst.instanceId & 1u) != 0u ? 1u : 0u;
                    flags |= lods != 0u ? GIU_RAY_LOD : 0u;
                    // the geometric normal in view space, NOT turned toward the ray: a ray that meets the back of the triangle is the classification's signal (CHECK C1: the pack's winding)
                    float3 n = float3(dot(hit.inst.row0, decoded.normal), dot(hit.inst.row1, decoded.normal), dot(hit.inst.row2, decoded.normal));
                    const float l2 = dot(n, n);
                    n = l2 > 1.0e-30f ? n * (1.0f / sqrt(l2)) : float3(0.0f, 1.0f, 0.0f);
                    const bool back = dot(n, direction) > 0.0f;
                    flags |= back ? GIU_RAY_BACK : 0u;
                    const float3 facing = back ? -n : n;
                    CityHitParams p;
                    p.mode = CITY_HIT_MODE_GI;
                    p.flags = CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE;
                    p.units = 1.0f;
                    p.sunReach = 0.0f;
                    p.sunTan = 0.0f;
                    p.lampFactor = g_world.lampFactor;
                    p.wet = g_world.wet;
                    p.aoMix = 0.5f;
                    p.lodBias = 0.0f;
                    p.bounce = g_push.tune.x;
                    p.lampFirst = 0u;
                    p.lampCount = 0u;
                    const uint h = GiuHash(GiuSeed(frame, cascade, index) + ray * 0x9E3779B1u);
                    p.noise = float3(GiuUnit(h), GiuUnit(GiuHash(h)), GiuUnit(GiuHash(h ^ 0x68E31DA4u)));
                    const float3 hitView = origin + direction * hit.t;
                    if (g_world.lampFactor > 0.0f && (g_world.flags & RT_WORLD_LAMPS) != 0u)
                    {
                        p.flags |= CITY_HIT_P_LAMPS | CITY_HIT_P_LAMP_SHADOW;
                        p.lampCount = GiuPickLamps(p, hitView, facing);
                    }
                    CityHitRay r;
                    r.origin = origin;
                    r.direction = direction;
                    r.t = hit.t;
                    r.footprint = 2.0f * g_push.tune2.x * hit.t;
                    const CityHitShaded s = CityHitShadeHit(p, f, r, hit.inst, decoded);
                    radiance = s.valid != 0u ? s.colour : float3(0.0f, 0.0f, 0.0f);
                    cls = s.cls;
                }
            }
            g_rays.Store4((slot * rays + ray) * GIU_RAY_BYTES, GiuPackRay(radiance, distance, flags, cls));
            traced = 1u;
            hits = (flags & GIU_RAY_HIT) != 0u ? 1u : 0u;
            escapes = 1u - hits;
            backs = (flags & GIU_RAY_BACK) != 0u ? 1u : 0u;
        }
    }
    // the census: the rays of the wave, one atomic a counter (the blend counts the probes and the rest)
    const uint tracedSum = WaveActiveSum(traced);
    const uint hitSum = WaveActiveSum(hits);
    const uint escapeSum = WaveActiveSum(escapes);
    const uint backSum = WaveActiveSum(backs);
    const uint lodSum = WaveActiveSum(lods);
    if (WaveIsFirstLane() && g_push.flags.x != 0u && tracedSum != 0u)
    {
        uint ignored = 0u;
        g_stats.InterlockedAdd(4u * 4u, tracedSum, ignored);
        g_stats.InterlockedAdd(5u * 4u, hitSum, ignored);
        g_stats.InterlockedAdd(6u * 4u, escapeSum, ignored);
        g_stats.InterlockedAdd(7u * 4u, backSum, ignored);
        g_stats.InterlockedAdd(8u * 4u, lodSum, ignored);
    }
}
