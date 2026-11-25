// MCLA GPU renderer -- the probe field's apply: the per-pixel maths (CITY-RT M3 package G3; the rules and the reasons are src/gpu/gi_apply_rules.h's comment, of which this file is THE TWIN).
//
// Included after hlsl/gi_common.hlsli (GiLookup, GiGroundBaseline, the frame's rows) by hlsl/gi_apply_ps.hlsl. Everything that is a function of numbers only is here so that
// src/gpu/gi_apply_test.cpp can compile THIS FILE as C++ (through a shim of the HLSL vector types, gi_common.hlsli's resources and the intrinsics below) and compare each function with
// gi_apply_rules.h on random inputs. That is why the text keeps to the subset both languages read: scalar and vector arithmetic (float3 + float3, float3 * float, float3 * float3), .x .y .z .w,
// no swizzles of more than one component, no out parameters, no attributes, and only the intrinsics abs, sqrt, exp, pow, min, max, saturate, floor, dot, asfloat, asuint.
//
// THE RESOURCE. The includer defines GI_FRAME_ROW(i) (gi_common.hlsli's: the buffer's row i, 16 bytes each) BEFORE gi_common.hlsli; the apply's own rows follow the field's 20 (GIA_ROW(i)).
//
// The names: every one starts with Gia or GIA_.

#ifndef MCLA_GI_APPLY_COMMON_HLSLI
#define MCLA_GI_APPLY_COMMON_HLSLI

#ifndef GI_FRAME_ROW
#error "include gi_common.hlsli (with GI_FRAME_ROW and the three resources) before gi_apply_common.hlsli"
#endif

#define GIA_FORMAT_VERSION 1

#define GIA_ROWS 9u
#define GIA_SKY 60000.0f
#define GIA_CAP_FLOOR 0.5f
#define GIA_KNEE 0.5f
#define GIA_VEHICLE_SHARE 0.35f
#define GIA_VEHICLE_MAX 0.8f
#define GIA_DEPTH_TOLERANCE 0.02f
#define GIA_DEPTH_TOLERANCE_FLOOR 0.05f
#define GIA_MIN_UPSAMPLE_WEIGHT 1.0e-4f
#define GIA_PAINT_FROM 0.5f
#define GIA_VIEW_SCALE 0.3f

#define GIA_FLAG_LOW_TRACED 1u
#define GIA_FLAG_VISIBILITY 2u
#define GIA_NO_SLOT 0xFFFFFFFFu

// The apply's rows come after the field's GI_FRAME_ROWS (20).
#define GIA_ROW(i) GI_FRAME_ROW(GI_FRAME_ROWS + (i))

float4 GiaRowFloats(uint row)
{
    uint4 r = GIA_ROW(row);
    return float4(asfloat(r.x), asfloat(r.y), asfloat(r.z), asfloat(r.w));
}

struct GiaFrame
{
    float3 right;
    float tanX;
    float3 up;
    float tanY;
    float3 forward;
    float fieldShare;
    float3 position;
    float strength;
    float4 lin;
    float2 render;
    float2 traced;
    float3 sunDir;
    float ambient;
    float sunLuma;
    float cap;
    float aoStrength;
    float aoFadeEnd;
    float deadBand;
    float paintPower;
    float sceneUnits;
    float daylightRecovery;
};

GiaFrame GiaLoadFrame()
{
    float4 a = GiaRowFloats(0u);
    float4 b = GiaRowFloats(1u);
    float4 c = GiaRowFloats(2u);
    float4 d = GiaRowFloats(3u);
    float4 e = GiaRowFloats(4u);
    float4 f = GiaRowFloats(5u);
    float4 g = GiaRowFloats(6u);
    float4 h = GiaRowFloats(7u);
    float4 i = GiaRowFloats(8u);
    GiaFrame fr;
    fr.right = float3(a.x, a.y, a.z);
    fr.tanX = a.w;
    fr.up = float3(b.x, b.y, b.z);
    fr.tanY = b.w;
    fr.forward = float3(c.x, c.y, c.z);
    fr.fieldShare = c.w;
    fr.position = float3(d.x, d.y, d.z);
    fr.strength = d.w;
    fr.lin = e;
    fr.render = float2(f.x, f.y);
    fr.traced = float2(f.z, f.w);
    fr.sunDir = float3(g.x, g.y, g.z);
    fr.ambient = g.w;
    fr.sunLuma = h.x;
    fr.cap = h.y;
    fr.aoStrength = h.z;
    fr.aoFadeEnd = h.w;
    fr.deadBand = i.x;
    fr.paintPower = i.y;
    fr.sceneUnits = i.z;
    fr.daylightRecovery = i.w;
    return fr;
}

// ---- the camera --------------------------------------------------------------------------------------------------------------------------------

// The distance along the view direction of a stored depth (1 near, 0 far): hlsl/ao_common.hlsli AoLinearDepth with the projection's rows in `lin`; GIA_SKY for the sky.
float GiaLinearDepth(float stored, float4 lin)
{
    float ndc = 1.0f - saturate(stored);
    float num = lin.z - ndc * lin.w;
    float den = lin.x - ndc * lin.y;
    float t = num / den;
    return (stored > 0.0f && den != 0.0f && t > 0.0f && t < GIA_SKY) ? t : GIA_SKY;
}

// The world position of the pixel centre (cx, cy) of an image w x h at the view distance t.
float3 GiaWorldPosition(GiaFrame fr, float cx, float cy, float w, float h, float t)
{
    float u = cx / w;
    float v = cy / h;
    float x = (u * 2.0f - 1.0f) * fr.tanX * t;
    float y = (1.0f - v * 2.0f) * fr.tanY * t;
    return fr.position + fr.right * x + fr.up * y + fr.forward * t;
}

// ---- the bounce light's rules (ssgi_common.hlsli SsgiSoftClip and the cap, the ceiling, the vehicle share) -------------------------------------------

float GiaLuma(float3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

float GiaSoftClip(float x, float limit)
{
    if (!(limit > 0.0f))
    {
        return 0.0f;
    }
    float knee = GIA_KNEE * limit;
    if (!(x > knee))
    {
        return x;
    }
    float span = limit - knee;
    return knee + span * (1.0f - exp(-(x - knee) / span));
}

// The cap of a radiance's luma, and the ceiling the bounce may fill a pixel up to: the cap multiple of the frame's light, never under GIA_CAP_FLOOR.
float GiaRadianceCap(float capMult, float ambient, float sunLuma)
{
    return max(capMult * (ambient + sunLuma), GIA_CAP_FLOOR);
}

float3 GiaCapRadiance(float3 c, float cap)
{
    float l = GiaLuma(c);
    if (!(l > 1.0e-6f))
    {
        return c;
    }
    return c * (GiaSoftClip(l, cap) / l);
}

float GiaStrengthFor(float strength, float vehicle)
{
    float v = saturate(vehicle);
    float onCar = min(strength * GIA_VEHICLE_SHARE, GIA_VEHICLE_MAX);
    return strength + (onCar - strength) * v;
}

// ---- the lookup's result ----------------------------------------------------------------------------------------------------------------------

// E_net = max(E_surf - E_base - band x ground, 0), per channel; `ground` is the open plane's radiance (the baseline at the normal pointing down).
float3 GiaNetIrradiance(float3 eSurf, float3 n, float deadBand)
{
    float3 base = GiGroundBaseline(n);
    float3 ground = GiGroundBaseline(float3(0.0f, -1.0f, 0.0f));
    return float3(max(eSurf.x - base.x - deadBand * ground.x, 0.0f), max(eSurf.y - base.y - deadBand * ground.y, 0.0f), max(eSurf.z - base.z - deadBand * ground.z, 0.0f));
}

// The lookup image's texel: E_net x the lookup's confidence.
float3 GiaLookupNet(GiSample s, float3 n, GiaFrame fr)
{
    const float3 net = GiaNetIrradiance(s.irradiance, n, fr.deadBand);
    // Compare with the sky share a flat, open plane would have for the same normal. A downward normal has no open sky in its lobe.
    // CHECK: the sky deficit ramp and recovery fraction need the open lawn, canyon, and portal leak captures on both APIs.
    const float deficit = 0.5f * (1.0f + n.y) - s.skyVisibility;
    const float x = saturate((deficit - 0.15f) / 0.30f);
    const float shade = x * x * (3.0f - 2.0f * x);
    const float daylight = saturate((GiSunSinElevation() - 0.10f) * 5.0f);
    const float weight = shade * daylight;
    if (fr.daylightRecovery < 0.0f || !(weight > 0.0f))
    {
        return net * s.confidence;
    }
    const float3 band = GiGroundBaseline(float3(0.0f, -1.0f, 0.0f)) * fr.deadBand;
    const float3 recovered = GiNetIrradiance(s.irradiance, band) - net;
    const float3 extra = net * (fr.sceneUnits - 1.0f) + recovered * (fr.sceneUnits * fr.daylightRecovery);
    return (net + extra * weight) * s.confidence;
}

// ---- the occlusion's term, the pixel's add ------------------------------------------------------------------------------------------------------

float GiaAoTerm(float ao, float vehicle, float tc, float strength, float fadeEnd, float paintPower)
{
    float onCar = saturate((vehicle - GIA_PAINT_FROM) * 2.0f);
    float seen = ao;
    if (onCar > 0.0f)
    {
        seen = ao + (pow(ao, paintPower) - ao) * onCar;
    }
    float halfEnd = 0.5f * fadeEnd;
    float t = halfEnd > 0.0f ? saturate((tc - halfEnd) / (fadeEnd - halfEnd)) : 1.0f;
    float fade = 1.0f - t * t * (3.0f - 2.0f * t);
    return 1.0f + (seen - 1.0f) * saturate(strength * fade);
}

// The light the pass adds (gi_apply_rules.h BounceAdd): albedo x CapRadiance(E_net) x strength x field share x lit x AO term, then SoftClip to the room under the ceiling.
float3 GiaBounceAdd(float3 albedo, float lit, float3 eNet, float strength, float fieldShare, float vehicle, float aoTerm, float ambient, float sunLuma, float nDotL, float cap)
{
    float capLuma = GiaRadianceCap(cap, ambient, sunLuma);
    float3 e = GiaCapRadiance(eNet, capLuma);
    float s = GiaStrengthFor(strength, vehicle);
    float3 add = albedo * e * (s * fieldShare * saturate(lit) * aoTerm);
    float t = max(ambient + sunLuma * nDotL, 1.0e-3f);
    float own = GiaLuma(albedo) * t;
    float room = max(capLuma - own, 0.0f);
    float added = GiaLuma(add);
    if (added > 1.0e-6f)
    {
        add = add * (GiaSoftClip(added, room) / added);
    }
    return add;
}

// ---- the upsample ------------------------------------------------------------------------------------------------------------------------------

float GiaDepthWeight(float tc, float td)
{
    float dz = abs(td - tc) / max(GIA_DEPTH_TOLERANCE * tc, GIA_DEPTH_TOLERANCE_FLOOR);
    return max(exp(-dz * dz), GIA_MIN_UPSAMPLE_WEIGHT);
}

float GiaNormalWeight(float3 n, float3 m)
{
    float d = saturate(dot(n, m));
    float d2 = d * d;
    return d2 * d2;
}

// ---- the leak test (gi_apply_rules.h GuardShare): the share of the eight probes' weight (trilinear x validity x back face) that the Chebyshev visibility test took away -------------

float GiaGuardShare(float3 p, float3 n)
{
    uint4 header = GI_FRAME_ROW(GI_ROW_HEADER);
    uint count = min(header.y, GI_MAX_CASCADES);
    float invAtlasW = 1.0f / (float)header.z;
    float invAtlasH = 1.0f / (float)header.w;
    for (uint c = 0u; c < count; ++c)
    {
        GiCascade k = GiLoadCascade(c);
        if (!(GiCascadeBlend(k, p) > 0.0f))
        {
            continue;
        }
        float offset = GI_NORMAL_OFFSET_SPACINGS * k.spacing;
        float3 q = float3(p.x + n.x * offset, p.y + n.y * offset, p.z + n.z * offset);
        float gx = min(max(q.x * k.invSpacing, (float)k.originX), (float)k.originX + (float)(k.dimX - 1u));
        float gy = min(max(q.y * k.invSpacing, (float)k.originY), (float)k.originY + (float)(k.dimY - 1u));
        float gz = min(max(q.z * k.invSpacing, (float)k.originZ), (float)k.originZ + (float)(k.dimZ - 1u));
        int bx = min((int)floor(gx), k.originX + (int)k.dimX - 2);
        int by = min((int)floor(gy), k.originY + (int)k.dimY - 2);
        int bz = min((int)floor(gz), k.originZ + (int)k.dimZ - 2);
        float fx = gx - (float)bx;
        float fy = gy - (float)by;
        float fz = gz - (float)bz;
        float sumW = 0.0f;
        float sumB = 0.0f;
        for (uint i = 0u; i < 8u; ++i)
        {
            uint ox = i & 1u;
            uint oy = (i >> 1u) & 1u;
            uint oz = (i >> 2u) & 1u;
            int cx = bx + (int)ox;
            int cy = by + (int)oy;
            int cz = bz + (int)oz;
            float wt = (ox != 0u ? fx : 1.0f - fx) * (oy != 0u ? fy : 1.0f - fy) * (oz != 0u ? fz : 1.0f - fz);
            uint sx = GiWrap(cx, k.dimX);
            uint sy = GiWrap(cy, k.dimY);
            uint sz = GiWrap(cz, k.dimZ);
            float wv = GI_LOAD_PROBE_WEIGHT(sx + k.dimX * sy, k.weightY + sz);
            if (!(wt * wv > 0.0f))
            {
                continue;
            }
            float3 d = float3((float)cx * k.spacing - q.x, (float)cy * k.spacing - q.y, (float)cz * k.spacing - q.z);
            float dist = sqrt(max(dot(d, d), 1.0e-8f));
            float3 toProbe = d * (1.0f / dist);
            float bf = max((dot(toProbe, n) + 1.0f) * 0.5f, GI_BACKFACE_FLOOR);
            bf = bf * bf;
            float2 momentsUv = GiProbeUv(k, invAtlasW, invAtlasH, sx, sy, sz, GiOctEncode(float3(-toProbe.x, -toProbe.y, -toProbe.z)));
            float2 m = GI_SAMPLE_MOMENTS(momentsUv);
            float wc = 1.0f;
            if (dist > m.x)
            {
                float variance = abs(m.y - m.x * m.x);
                float diff = dist - m.x;
                float cheb = variance / (variance + diff * diff);
                wc = max(cheb * cheb * cheb, GI_CHEBYSHEV_FLOOR);
            }
            sumW = sumW + wt * wv * bf * wc;
            sumB = sumB + wt * wv * bf;
        }
        return sumB > 0.0f ? 1.0f - saturate(sumW / sumB) : 0.0f;
    }
    return 0.0f;
}

#endif
