// MCLA GPU renderer -- the probe update's blend pass (gi_update.h, CITY-RT M3 package G2; the rules and the reasons are gi_update_rules.h's comment, the shared maths gi_update_common.hlsli).
//
// One thread group of 10 x 10 threads a probe of the chunk: the probe's tile of the two atlases, border included. The group reads the probe's ray records (the trace pass's) into group shared
// memory with each ray's direction (the same rotation as the trace's: the seed is a hash of the frame, the cascade and the probe), then
//   1. CLASSIFIES the probe: inside geometry (switched off: no light, weight 0) when more than 30% of its rays hit back faces or its nearest hit is nearer than a tenth of a spacing;
//   2. INTEGRATES every texel of the tile (gi_frame.h IntegrateTexel: the irradiance by the cosine lobe, the sky visibility, the distance moments), a border texel as the interior texel it copies
//      (gi_frame.h OctBorderSource), and reads what the tile held;
//   3. BLENDS with ADAPTIVE HYSTERESIS: the stored value's weight is the running mean while the probe is young, at most 0.97 (about 33 updates); 0.85 for a probe that was traced because its light
//      changed (the scheduler's dirty flag: a sector built, an hour jump) or whose mean luminance JUMPED (more than 25% and more than four standard errors of the rays' mean: a lamp on, the sun's
//      colour at dusk, not the noise of a probe that sees a small bright thing with a few rays); none for a probe that has no history (new, or back from being inside geometry);
//   4. writes the tile (not for an inside probe: its data stays what it was, the weight is 0) and the probe's STATE WORD (updates, the inside bit, the back face share, the frame), and the census.
// A probe whose record 0 says SKIPPED (an inside probe the trace did not look at) is left alone entirely.
//
// Layout (gi_update.cpp, the blend layout): set 0 (space0) this pass's own set: t0 the frame's rows, t1 the probe list, u2 the ray records, u3 the irradiance atlas (RGBA16F), u4 the moments atlas
// (RG16F), u5 the state words, u6 the counters; the push block at (b0, space1), the trace pass's. Dispatch (GIU_GROUPS_X, ceil(probes / GIU_GROUPS_X), 1). Shader Model 6.0.

ByteAddressBuffer g_rows : register(t0, space0);
ByteAddressBuffer g_probes : register(t1, space0);
RWByteAddressBuffer g_rays : register(u2, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_irradiance : register(u3, space0);
[[vk::image_format("rg16f")]] RWTexture2D<float2> g_moments : register(u4, space0);
#else
RWTexture2D<float4> g_irradiance : register(u3, space0);
RWTexture2D<float2> g_moments : register(u4, space0);
#endif
RWByteAddressBuffer g_state : register(u5, space0);
RWByteAddressBuffer g_stats : register(u6, space0);

struct GiuPush
{
    uint4 control;  // x the probes in this chunk, y the rays a probe, z the chunk's first probe in the list, w the frame's serial
    uint4 flags;    // x the stats flag
    float4 tune;    // x the bounce weight (unused here), y the rays' reach (unused here), z the hysteresis, w the jump hysteresis
    float4 tune2;   // unused here
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GiuPush> g_push;
#else
ConstantBuffer<GiuPush> g_push : register(b0, space1);
#endif

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) float4(0.0f, 0.0f, 0.0f, 0.0f)
#define GI_SAMPLE_MOMENTS(uv) float2(0.0f, 0.0f)
#define GI_LOAD_PROBE_WEIGHT(x, y) 0.0f
#include "gi_common.hlsli"
#define GIF_RESET_WORD(i) 0u
#include "gi_field_common.hlsli"
// The probe's rays, the directions among them, in group shared memory (the integration's accessors).
groupshared float4 gsDir[64];       // xyz the unit direction, w 1 where the ray escaped
groupshared float4 gsRadiance[64];  // xyz what the hit sent back, w the distance
// 0 back faces, 1 hits, 2 the nearest hit's distance (float bits, positive: an unsigned minimum), 3 lod hits, 4 the old mean (fixed), 5 the new mean (fixed), 6 the sky (fixed),
// 7 the sum of the rays' luminance and 8 of its square (GIU_RAY_FIXED)
groupshared uint gsCount[9];

#define GIU_RAY_DIR(i) float3(gsDir[(i)].x, gsDir[(i)].y, gsDir[(i)].z)
#define GIU_RAY_RADIANCE(i) float3(gsRadiance[(i)].x, gsRadiance[(i)].y, gsRadiance[(i)].z)
#define GIU_RAY_DISTANCE(i) gsRadiance[(i)].w
#define GIU_RAY_ESCAPED(i) (gsDir[(i)].w > 0.5f)

#include "gi_update_common.hlsli"

uint StateBase(uint c)
{
    uint base = 0u;
    for (uint i = 0u; i < c; ++i)
    {
        GiCascade k = GiLoadCascade(i);
        base += k.dimX * k.dimY * k.dimZ;
    }
    return base;
}

void Count(uint word, uint value)
{
    uint ignored = 0u;
    g_stats.InterlockedAdd(word * 4u, value, ignored);
}

[numthreads(10, 10, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint t = thread.y * 10u + thread.x;
    const uint rays = min(max(g_push.control.y, 1u), 64u);
    const uint slot = group.y * GIU_GROUPS_X + group.x;
    if (slot >= g_push.control.x) return;
    const uint4 first = g_rays.Load4((slot * rays) * GIU_RAY_BYTES);
    const uint skipped = (first.y >> 16u) & (GIU_RAY_SKIPPED | GIU_RAY_SLEEPING);
    // All lanes in this group work on the same probe. Skips need no tile work
    // or group barriers.
    if (skipped != 0u)
    {
        if (t == 0u && g_push.flags.x != 0u) Count(skipped == GIU_RAY_SLEEPING ? 17u : 16u, 1u);
        return;
    }
    const uint word = g_probes.Load((g_push.control.z + slot) * 4u);
    const uint cascade = GiuProbeCascade(word);
    const uint index = GiuProbeIndex(word);
    const uint frame = g_push.control.w;
    const GiCascade k = GiLoadCascade(cascade);
    const uint stateAt = StateBase(cascade) + index;
    const uint state = g_state.Load(stateAt * 4u);

    if (t < 9u)
    {
        gsCount[t] = t == 2u ? asuint(1.0e30f) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // the rays: a thread a ray (up to 64 of the 100)
    if (t < rays)
    {
        const GiuRay r = GiuUnpackRay(g_rays.Load4((slot * rays + t) * GIU_RAY_BYTES));
        const bool hit = (r.flags & GIU_RAY_HIT) != 0u;
        const float4 q = GiuRotationQuat(GiuSeed(frame, cascade, index));
        const float3 d = GiuRayDirection(t, rays, q);
        gsDir[t] = float4(d.x, d.y, d.z, hit ? 0.0f : 1.0f);
        gsRadiance[t] = float4(hit ? r.radiance.x : 0.0f, hit ? r.radiance.y : 0.0f, hit ? r.radiance.z : 0.0f, r.distance);
        const float luma = hit ? GiuLuma(r.radiance) : 0.0f;
        InterlockedAdd(gsCount[7], (uint)(luma * GIU_RAY_FIXED + 0.5f));
        InterlockedAdd(gsCount[8], (uint)(luma * luma * GIU_RAY_FIXED + 0.5f));
        if (hit)
        {
            InterlockedAdd(gsCount[1], 1u);
            InterlockedMin(gsCount[2], asuint(r.distance));
            if ((r.flags & GIU_RAY_BACK) != 0u)
            {
                InterlockedAdd(gsCount[0], 1u);
            }
            if ((r.flags & GIU_RAY_LOD) != 0u)
            {
                InterlockedAdd(gsCount[3], 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const bool inside = GiuProbeInside(gsCount[0], rays, asfloat(gsCount[2]), k.spacing);
    // the tile's texel this thread is the value of: an interior texel itself, a border texel the interior texel it copies
    const GiuTexelCoord src = GiuBorderSource(thread.x, thread.y);
    const bool interior = thread.x >= 1u && thread.x <= 8u && thread.y >= 1u && thread.y <= 8u;
    const uint sx = index % k.dimX;
    const uint sy = (index / k.dimX) % k.dimY;
    const uint sz = index / (k.dimX * k.dimY);
    const uint2 tile = uint2((sx + k.dimX * sy) * GI_PROBE_TILE, k.atlasY + sz * GI_PROBE_TILE);
    const uint2 own = tile + uint2(thread.x, thread.y);
    const uint2 from = tile + uint2(src.x, src.y);
    const float4 oldIrradiance = g_irradiance[from];
    const float2 oldMoments = g_moments[from];
    const GiuTexel texel = GiuIntegrateTexel(GiuTexelDirection(src.x, src.y), rays, k.maxDistance);
    if (interior)
    {
        InterlockedAdd(gsCount[4], (uint)(GiuLuma(float3(oldIrradiance.x, oldIrradiance.y, oldIrradiance.z)) * GIU_FIXED_SCALE + 0.5f));
        InterlockedAdd(gsCount[5], (uint)(GiuLuma(texel.irradiance) * GIU_FIXED_SCALE + 0.5f));
        InterlockedAdd(gsCount[6], (uint)(texel.sky * GIU_FIXED_SCALE + 0.5f));
    }
    GroupMemoryBarrierWithGroupSync();

    // the probe's old and new mean luminance (64 interior texels), what changed, and the hysteresis
    const float oldMean = (float)gsCount[4] / (GIU_FIXED_SCALE * 64.0f);
    const float newMean = (float)gsCount[5] / (GIU_FIXED_SCALE * 64.0f);
    const float skyMean = (float)gsCount[6] / (GIU_FIXED_SCALE * 64.0f);
    const float change = GiuRelativeChange(oldMean, newMean);
    const float standardError = GiuStandardError((float)gsCount[7] / GIU_RAY_FIXED, (float)gsCount[8] / GIU_RAY_FIXED, rays);
    const bool jumped = GiuMeanJumped(oldMean, newMean, standardError);
    const uint updates = GifStateUpdates(state);
    const bool isNew = GifStateNew(state) || updates == 0u || GifStateInside(state);
    const bool dirty = (GiuProbeFlags(word) & GIU_PROBE_DIRTY) != 0u;
    const float h = GiuHysteresis(updates, isNew, dirty, jumped, g_push.tune.z, g_push.tune.w);
    if (!inside)
    {
        const float3 blended = GiuBlend3(float3(oldIrradiance.x, oldIrradiance.y, oldIrradiance.z), texel.irradiance, h);
        const float sky = GiuBlend1(oldIrradiance.w, texel.sky, h);
        const float2 moments = float2(min(GiuBlend1(oldMoments.x, texel.meanDistance, h), k.maxDistance), min(GiuBlend1(oldMoments.y, texel.meanDistanceSq, h), k.maxDistance * k.maxDistance));
        g_irradiance[own] = float4(blended.x, blended.y, blended.z, sky);
        g_moments[own] = moments;
    }
    if (t == 0u)
    {
        // The count rises for a blended probe. Store the latest classification,
        // update frame and consecutive low-variability visits.
        const uint back = GiuBackByte(gsCount[0], rays);
        const uint kept = state & ~(GIF_STATE_INSIDE | GIF_STATE_BACK_MASK);
        uint next = inside ? kept : GifStateAfterUpdate(kept);
        next = next | (inside ? GIF_STATE_INSIDE : 0u) | (back << GIF_STATE_BACK_SHIFT);
        const float rayMean = (float)gsCount[7] / (GIU_RAY_FIXED * (float)rays);
        const bool reset = g_push.flags.y == 0u || inside || isNew || dirty;
        const uint stable = GiuNextStableVisits(state, reset, change, standardError, rayMean);
        next = (next & 0x3FFFFu) | ((frame & 0x3FFu) << 18u) | (stable << GIU_STABLE_SHIFT);
        g_state.Store(stateAt * 4u, next);
        if (g_push.flags.x != 0u)
        {
            Count(cascade, 1u);
            Count(9u, inside ? 1u : 0u);
            Count(10u, (!inside && !isNew && change > GIU_CHANGED_STAT) ? 1u : 0u);
            Count(11u, dirty ? 1u : 0u);
            if (!inside)
            {
                Count(12u, GifStateUpdates(next) >= GIU_CONVERGED_UPDATES ? 1u : 0u);
                Count(13u, 1u);
                Count(14u, (uint)(newMean * GIU_FIXED_SCALE + 0.5f));
                Count(15u, (uint)(skyMean * GIU_FIXED_SCALE + 0.5f));
                Count(18u + GiuHistBin(newMean), 1u);
            }
        }
    }
}
