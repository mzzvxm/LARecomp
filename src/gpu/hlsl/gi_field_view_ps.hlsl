// MCLA GPU renderer -- the probe field's debug views (gi_field.h, mcla_gpu_rt_gi_view; package G1 of perf/CITY-RT-M3.md).
//
// The probes of one cascade drawn as small spheres over the picture (the pass blends over the scene colour: a pixel with no sphere is left as it is, alpha 0). A pixel's ray from the
// camera is walked through the cascade's lattice (gi_field_common.hlsli GifTraceProbes: the cells round the probes, one sphere test a cell) as far as the scene's depth, so the spheres
// are hidden by the geometry in front of them; the nearest sphere is shaded by the view:
//   4  (gi_frame.h View::kProbes)  the probe's validity: magenta = inside geometry (G2's classification), grey = beyond the safe range, yellow = new / never traced, else red (weight 0 .. just
//                                  traced) through to green (converged), shaded with a headlight
//   7  the irradiance spheres: what the probe holds of the light surfaces send, in the direction of the sphere's own normal at each pixel (the octahedral map read back: a probe that
//                                  sees a red wall is red on that side), tone mapped x / (1 + x) after the gain; a probe of weight 0 is shown dark and tinted by its state
//   8  the sky visibility spheres: the cosine weighted share of the lobe that sees the sky (the atlas' alpha), dark blue (closed) to white (open)
// The camera is the push block's (the view's own: the pass is drawn at the trigger with that frame's camera); the depth is the occlusion's linear depth image (R32_FLOAT, the distance
// along the view direction in its z, the vehicle in its lowest bit, the sky a huge value) read at the pixel's place scaled to its size; slot 0xFFFFFFFF reads none (the spheres are
// not hidden). The field's frame and its state come from the buffers below (the frame's rows are gi_frame.h's GpuGiFrame); the atlases through their bindless slots (the rows' slots).
//
// Layout (gi_field.cpp, the view layout): set 0 (space0) the bindless texture set (Texture2D<float4>[]), set 1 (space1) the samplers (SamplerState[]: 0 linear clamp), set 2 (space2) the
// view's set: t0 the frame rows (ByteAddressBuffer), t1 the state buffer (ByteAddressBuffer); push constants at (b0, space3), 96 bytes. Blend: SRC_ALPHA, INV_SRC_ALPHA on the colour.

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[] : register(s0, space1);
ByteAddressBuffer g_rows : register(t0, space2);
ByteAddressBuffer g_state : register(t1, space2);

struct GiFieldViewPush
{
    float4 posTanX;     // the camera's position, tan of the half field of view in x
    float4 rightTanY;   // right, tan of the half field of view in y
    float4 upGain;      // up, the gain of the irradiance view
    float4 forwardMax;  // forward, how far the spheres are drawn (metres)
    uint4 info;         // x the view (gi_field_rules.h kView*), y the cascade, z the depth image's slot (0xFFFFFFFF: none), w its width
    uint4 size;         // x the target's width, y its height, z the depth image's height, w the sphere radius in spacings (float bits)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<GiFieldViewPush> g_push;
#else
ConstantBuffer<GiFieldViewPush> g_push : register(b0, space3);
#endif

#define GI_FRAME_ROW(i) g_rows.Load4((i) * 16u)
#define GI_SAMPLE_IRRADIANCE(uv) g_textures[GiSlot(GI_PRODUCT_IRRADIANCE)].SampleLevel(g_samplers[0], (uv), 0.0f)
#define GI_SAMPLE_MOMENTS(uv) g_textures[GiSlot(GI_PRODUCT_MOMENTS)].SampleLevel(g_samplers[0], (uv), 0.0f).xy
#define GI_LOAD_PROBE_WEIGHT(px, py) g_textures[GiSlot(GI_PRODUCT_PROBE_WEIGHT)].Load(int3((int)(px), (int)(py), 0)).x
#include "gi_common.hlsli"
#define GIF_RESET_WORD(i) 0u
#include "gi_field_common.hlsli"

static const uint kViewProbes = 4u;
static const uint kViewIrradiance = 7u;
static const uint kViewSky = 8u;
static const float kSkyDepth = 30000.0f;  // the depth image holds 60000 for the sky (ao_common.hlsli kAoSky)

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

float4 main(float4 position : SV_Position) : SV_Target
{
    float4 none = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!GiFieldValid() || GiCascadeCount() == 0u)
    {
        return none;
    }
    float2 size = float2((float)g_push.size.x, (float)g_push.size.y);
    float2 ndc = float2(position.x / size.x * 2.0f - 1.0f, 1.0f - position.y / size.y * 2.0f);
    float3 right = float3(g_push.rightTanY.x, g_push.rightTanY.y, g_push.rightTanY.z);
    float3 up = float3(g_push.upGain.x, g_push.upGain.y, g_push.upGain.z);
    float3 forward = float3(g_push.forwardMax.x, g_push.forwardMax.y, g_push.forwardMax.z);
    float3 camera = float3(g_push.posTanX.x, g_push.posTanX.y, g_push.posTanX.z);
    // the ray: forward + right * (x tan_x) + up * (y tan_y), whose component along forward is 1, so a depth z is the distance z x length(ray) along it
    float3 ray = forward + right * (ndc.x * g_push.posTanX.w) + up * (ndc.y * g_push.rightTanY.w);
    float rayLength = sqrt(dot(ray, ray));
    float3 dir = ray * (1.0f / rayLength);
    float limit = g_push.forwardMax.w;
    if (g_push.info.z != 0xFFFFFFFFu)
    {
        float2 depthSize = float2((float)g_push.info.w, (float)g_push.size.z);
        int2 dp = int2(position.xy * (depthSize / size));
        float z = asfloat(asuint(g_textures[g_push.info.z].Load(int3(dp, 0)).x) & 0xFFFFFFFEu);
        if (z > 0.0f && z < kSkyDepth)
        {
            limit = min(limit, z * rayLength);
        }
    }
    uint cascade = min(g_push.info.y, GiCascadeCount() - 1u);
    GiCascade k = GiLoadCascade(cascade);
    float radius = asfloat(g_push.size.w) * k.spacing;
    GifProbeHit h = GifTraceProbes(camera.x, camera.y, camera.z, dir.x, dir.y, dir.z, limit, k.originX, k.originY, k.originZ, k.dimX, k.dimY, k.dimZ, k.spacing, radius);
    if (!h.hit)
    {
        return none;
    }
    float3 centre = float3((float)h.cx * k.spacing, (float)h.cy * k.spacing, (float)h.cz * k.spacing);
    float3 hitPoint = camera + dir * h.t;
    float3 n = (hitPoint - centre) * (1.0f / radius);
    n = n * (1.0f / sqrt(dot(n, n)));
    uint sx = GiWrap(h.cx, k.dimX);
    uint sy = GiWrap(h.cy, k.dimY);
    uint sz = GiWrap(h.cz, k.dimZ);
    uint probe = (sz * k.dimY + sy) * k.dimX + sx;
    uint state = g_state.Load((StateBase(cascade) + probe) * 4u);
    float3 d = centre - camera;
    float safeRange = GiSafeRange();
    bool inRange = safeRange > 0.0f && sqrt(dot(d, d)) <= safeRange;
    float weight = GifProbeWeight(state, inRange);
    float headlight = 0.35f + 0.65f * saturate(dot(n, -dir));
    uint view = g_push.info.x;
    float3 colour = float3(1.0f, 0.0f, 1.0f);
    if (view == kViewProbes)
    {
        if (GifStateInside(state))
        {
            colour = float3(0.9f, 0.1f, 0.9f);
        }
        else if (!inRange)
        {
            colour = float3(0.45f, 0.45f, 0.45f);
        }
        else if (GifStateUpdates(state) == 0u)
        {
            colour = float3(1.0f, 0.85f, 0.1f);
        }
        else
        {
            colour = lerp(float3(0.95f, 0.15f, 0.1f), float3(0.1f, 0.9f, 0.2f), weight);
        }
        colour = colour * headlight;
    }
    else
    {
        uint4 header = GI_FRAME_ROW(GI_ROW_HEADER);
        float2 uv = GiProbeUv(k, 1.0f / (float)header.z, 1.0f / (float)header.w, sx, sy, sz, GiOctEncode(n));
        float4 e = GI_SAMPLE_IRRADIANCE(uv);
        if (view == kViewIrradiance)
        {
            float3 lit = float3(e.x, e.y, e.z) * g_push.upGain.w;
            colour = lit / (float3(1.0f, 1.0f, 1.0f) + lit);
            if (weight <= 0.0f)
            {
                // a probe that is off: dark, tinted by why (inside geometry, new, out of range)
                colour = GifStateInside(state) ? float3(0.25f, 0.0f, 0.25f) : (!inRange ? float3(0.15f, 0.15f, 0.15f) : float3(0.25f, 0.2f, 0.0f));
            }
        }
        else
        {
            colour = lerp(float3(0.02f, 0.05f, 0.35f), float3(1.0f, 1.0f, 1.0f), saturate(e.w));
            if (weight <= 0.0f)
            {
                colour = colour * 0.25f;
            }
        }
    }
    return float4(colour, 1.0f);
}
