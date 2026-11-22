// MCLA GPU renderer -- the probe field owner's weights pass (gi_field.h, package G1 of perf/CITY-RT-M3.md).
//
// The weight map (R8_UNORM, one texel a probe: gi_frame.h) has ONE writer, this pass: once a frame, after the probe update (G2) has written the probes' state words and before any
// reader looks the field up, every probe's weight is made from its state word and its place (gi_field_rules.h ProbeWeight):
//   0   inside geometry (the state's inside bit), never traced (no update since the reset), or farther from the camera than the safe range (M1's SafeRange: the structures are not
//       all built beyond it; a safe range of 0 is no field)
//   a ramp over the first updates up to 1 (the first trace has no history and 64 rays)
// One thread a probe, the cascades' probes in one run in the order of the state buffer (cascade 0's first). The frame's rows give the cascades (origin, dims, rows of the maps), the
// camera and the safe range.
//
// Layout (gi_field.cpp, the compute layout): set 0 (space0) the field's set, as the reset pass's (t0 the frame rows, t1 the reset list (unused here), u2 .. u4 the textures (unused:
// only the weight map is written), u5 the state buffer); no push constants. Dispatch ceil(total probes / 64) groups.

ByteAddressBuffer g_rows : register(t0, space0);
#ifdef __spirv__
[[vk::image_format("r8")]] RWTexture2D<float> g_weight : register(u4, space0);
#else
RWTexture2D<float> g_weight : register(u4, space0);
#endif
RWByteAddressBuffer g_state : register(u5, space0);

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) float4(0.0f, 0.0f, 0.0f, 0.0f)
#define GI_SAMPLE_MOMENTS(uv) float2(0.0f, 0.0f)
#define GI_LOAD_PROBE_WEIGHT(x, y) 0.0f
#include "gi_common.hlsli"
#define GIF_RESET_WORD(i) 0u
#include "gi_field_common.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint count = GiCascadeCount();
    uint p = id.x;
    uint c = 0u;
    uint base = 0u;
    // which cascade this probe is in
    for (c = 0u; c < count; ++c)
    {
        GiCascade k = GiLoadCascade(c);
        uint n = k.dimX * k.dimY * k.dimZ;
        if (p < base + n)
        {
            break;
        }
        base += n;
    }
    if (c >= count)
    {
        return;
    }
    GiCascade k = GiLoadCascade(c);
    uint index = p - base;
    uint sx = index % k.dimX;
    uint sy = (index / k.dimX) % k.dimY;
    uint sz = index / (k.dimX * k.dimY);
    GifCoord coord = GifCoordOfSlot(k.originX, k.originY, k.originZ, k.dimX, k.dimY, k.dimZ, sx, sy, sz);
    float3 world = float3((float)coord.x * k.spacing, (float)coord.y * k.spacing, (float)coord.z * k.spacing);
    float3 cam = GiCamera();
    float3 d = world - cam;
    float safeRange = GiSafeRange();
    bool inRange = safeRange > 0.0f && sqrt(dot(d, d)) <= safeRange;
    uint state = g_state.Load(p * 4u);
    g_weight[uint2(sx + k.dimX * sy, k.weightY + sz)] = GifProbeWeight(state, inRange);
}
