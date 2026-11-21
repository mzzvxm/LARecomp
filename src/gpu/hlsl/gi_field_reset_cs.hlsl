// MCLA GPU renderer -- the probe field owner's reset pass (gi_field.h, package G1 of perf/CITY-RT-M3.md).
//
// A window of a cascade moved with the camera: the coordinates that entered it are stored in the slots of the coordinates that left it (toroidal addressing, gi_frame.h), so the
// slot holds another probe's light. This clears them, ONE probe a thread group (10 x 10 threads: the probe's tile, border included):
//   irradiance tile  0 (no light, no sky: the apply reads nothing from a probe whose weight is 0 anyway)
//   moments tile     the cascade's maximum distance and its square (open)
//   weight texel     0 (not trusted: the lookup ignores the probe)
//   state word       kStateAfterReset: never traced, new, not inside
// The list of probes is gi_field_rules.h's ResetList (the host fills it each frame it has exposures): the group's number is the index into the boxes it holds in order. The cascade's
// rows are the frame's (gi_frame.h GpuGiFrame), the same buffer the weights pass reads.
//
// Layout (gi_field.cpp, the compute layout): set 0 (space0) the field's set: t0 the frame rows (ByteAddressBuffer, 20 rows of 16 bytes), t1 the reset list, u2 the irradiance atlas
// (RGBA16F), u3 the moments atlas (RG16F), u4 the weight map (R8_UNORM), u5 the state buffer (RWByteAddressBuffer); no push constants. The formats are given to SPIR-V explicitly (no
// shaderStorageImageWriteWithoutFormat needed). Dispatch (kResetGroupsX, ceil(total / kResetGroupsX), 1) groups.

ByteAddressBuffer g_rows : register(t0, space0);
ByteAddressBuffer g_list : register(t1, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_irradiance : register(u2, space0);
[[vk::image_format("rg16f")]] RWTexture2D<float2> g_moments : register(u3, space0);
[[vk::image_format("r8")]] RWTexture2D<float> g_weight : register(u4, space0);
#else
RWTexture2D<float4> g_irradiance : register(u2, space0);
RWTexture2D<float2> g_moments : register(u3, space0);
RWTexture2D<float> g_weight : register(u4, space0);
#endif
RWByteAddressBuffer g_state : register(u5, space0);

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) float4(0.0f, 0.0f, 0.0f, 0.0f)
#define GI_SAMPLE_MOMENTS(uv) float2(0.0f, 0.0f)
#define GI_LOAD_PROBE_WEIGHT(x, y) 0.0f
#include "gi_common.hlsli"
#define GIF_RESET_WORD(i) g_list.Load((i) * 4u)
#include "gi_field_common.hlsli"

// The first word of cascade `c` in the state buffer: the probes of the cascades before it.
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

[numthreads(10, 10, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint g = group.y * GIF_RESET_GROUPS_X + group.x;
    GifResetProbe p = GifResetProbeOf(g);
    if (!p.valid || p.cascade >= GiCascadeCount())
    {
        return;
    }
    GiCascade k = GiLoadCascade(p.cascade);
    uint sx = GiWrap(p.x, k.dimX);
    uint sy = GiWrap(p.y, k.dimY);
    uint sz = GiWrap(p.z, k.dimZ);
    uint2 texel = uint2((sx + k.dimX * sy) * GI_PROBE_TILE + thread.x, k.atlasY + sz * GI_PROBE_TILE + thread.y);
    g_irradiance[texel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_moments[texel] = float2(k.maxDistance, k.maxDistance * k.maxDistance);
    if (thread.x == 0u && thread.y == 0u)
    {
        g_weight[uint2(sx + k.dimX * sy, k.weightY + sz)] = 0.0f;
        uint probe = (sz * k.dimY + sy) * k.dimX + sx;
        g_state.Store((StateBase(p.cascade) + probe) * 4u, GIF_STATE_AFTER_RESET);
    }
}
