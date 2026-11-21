// MCLA GPU renderer -- the probe field's shared HLSL: THE FROZEN INTERFACE (package G0 of perf/CITY-RT-M3.md; the layouts, the units and the reasons are src/gpu/gi_frame.h's comment).
//
// One include for every shader that reads the global illumination field (G3's apply, the AO's apply path, the light pass if it ever wants the field, M2's hit shading, G5's per-pixel
// pass, the debug view): the decode of the frame's rows (GpuGiFrame), the lookup (GiLookup: the cascades, the eight probes, the weights), the ambient at a point (GiAmbientAt), the ground
// baseline (GiGroundBaseline, GiNetIrradiance) and the octahedral map. The C++ twin is src/gpu/gi_frame.h; src/gpu/gi_frame_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL
// vector types and of the four resources below) and checks it against the twin on random frames and random points, and compares every #define below with gi_frame.h, so the two cannot
// drift. That is why the text keeps to a subset both languages read: scalar and vector arithmetic (float3 + float3, float3 * float), .x .y .z .w, no swizzles of more than one component,
// no out parameters, no static arrays, no attributes, and only the intrinsics abs, sqrt, min, max, saturate, floor, dot, asfloat, asint.
//
// THE RESOURCES. The includer defines these four macros before including (the pass's own names for its resources; the textures are the three the field publishes, found through the
// bindless slots of the frame's row 1: GiSlot(GI_PRODUCT_*), or bound by the pass directly):
//   GI_FRAME_ROW(i)             the frame's row i (0 .. GI_FRAME_ROWS - 1) as a uint4: the GpuGiFrame the host wrote (gi_frame.h MakeGpuFrame), 20 rows of 16 bytes
//   GI_SAMPLE_IRRADIANCE(uv)    float4: the irradiance atlas (RGBA16F), a bilinear sample of mip 0 with CLAMP addressing at the atlas' 0..1 coordinates: rgb the irradiance, a the sky visibility
//   GI_SAMPLE_MOMENTS(uv)       float2 (or a float4 whose .x and .y are used): the moments atlas (RG16F), the same sampler: x the mean distance, y the mean of its square, metres
//   GI_LOAD_PROBE_WEIGHT(x, y)  float 0..1: the weight map's texel (R8_UNORM), a point load at the integer texel (x, y); no filtering
//
// The names: every one starts with Gi or GI_.

#ifndef MCLA_GI_COMMON_HLSLI
#define MCLA_GI_COMMON_HLSLI

#ifndef GI_FRAME_ROW
#error "define GI_FRAME_ROW(i), GI_SAMPLE_IRRADIANCE(uv), GI_SAMPLE_MOMENTS(uv) and GI_LOAD_PROBE_WEIGHT(x, y) before including gi_common.hlsli"
#endif

// ---- the numbers (src/gpu/gi_frame.h) -------------------------------------------------------------------------------------------------------

#define GI_FORMAT_VERSION 1

#define GI_MAX_CASCADES 4u
#define GI_PROBE_INTERIOR 8u
#define GI_PROBE_BORDER 1u
#define GI_PROBE_TILE 10u
#define GI_NO_SLOT 0xFFFFFFFFu

#define GI_FRAME_ROWS 20u
#define GI_ROW_HEADER 0u
#define GI_ROW_SLOTS 1u
#define GI_ROW_FADE 2u
#define GI_ROW_CAMERA 3u
#define GI_ROW_SUN 4u
#define GI_ROW_AMBIENT_UP 5u
#define GI_ROW_AMBIENT_DOWN 6u
#define GI_ROW_RESERVED 7u
#define GI_ROW_PIXEL 7u
#define GI_ROW_CASCADE0 8u
#define GI_ROWS_PER_CASCADE 3u

#define GI_FLAG_VALID 1u
#define GI_FLAG_REPLACES_SSGI 2u
#define GI_FLAG_PIXEL_PASS 4u

#define GI_PRODUCT_IRRADIANCE 0u
#define GI_PRODUCT_MOMENTS 1u
#define GI_PRODUCT_PROBE_WEIGHT 2u
#define GI_PRODUCT_PIXEL_IRRADIANCE 3u

#define GI_NORMAL_OFFSET_SPACINGS 0.1f
#define GI_BACKFACE_FLOOR 0.0001f
#define GI_CHEBYSHEV_FLOOR 0.0001f
#define GI_RANGE_FADE_SHARE 0.15f
#define GI_MIN_WEIGHT_SUM 1.0e-20f
#define GI_MIN_COVER 1.0e-6f
#define GI_REMAINING_STOP 1.0e-4f

// ---- the octahedral map (the G-buffer's, gbuffer_common.hlsli GbOctEncode / GbOctDecode) -------------------------------------------------------

float GiSignNotZero(float v)
{
    return v >= 0.0f ? 1.0f : -1.0f;
}

// The map's coordinate of a direction, each 0..1. `n` need not be unit length; it must not be zero.
float2 GiOctEncode(float3 n)
{
    float inv = 1.0f / (abs(n.x) + abs(n.y) + abs(n.z));
    float x = n.x * inv;
    float y = n.y * inv;
    float z = n.z * inv;
    float ex = x;
    float ey = y;
    if (z < 0.0f)
    {
        ex = (1.0f - abs(y)) * GiSignNotZero(x);
        ey = (1.0f - abs(x)) * GiSignNotZero(y);
    }
    return float2(ex * 0.5f + 0.5f, ey * 0.5f + 0.5f);
}

// The unit direction of a coordinate in 0..1.
float3 GiOctDecode(float2 f)
{
    float x = f.x * 2.0f - 1.0f;
    float y = f.y * 2.0f - 1.0f;
    float z = 1.0f - abs(x) - abs(y);
    float t = saturate(-z);
    x = x + (x >= 0.0f ? -t : t);
    y = y + (y >= 0.0f ? -t : t);
    float il = 1.0f / sqrt(x * x + y * y + z * z);
    return float3(x * il, y * il, z * il);
}

// ---- the frame's rows (gi_frame.h: THE ROWS THE SHADERS READ) -------------------------------------------------------------------------------

float4 GiRowFloats(uint row)
{
    uint4 r = GI_FRAME_ROW(row);
    return float4(asfloat(r.x), asfloat(r.y), asfloat(r.z), asfloat(r.w));
}

uint GiFlags()
{
    return GI_FRAME_ROW(GI_ROW_HEADER).x;
}

uint GiCascadeCount()
{
    return min(GI_FRAME_ROW(GI_ROW_HEADER).y, GI_MAX_CASCADES);
}

// The bindless slot of a product (GI_PRODUCT_*); GI_NO_SLOT where the frame does not have it.
uint GiSlot(uint product)
{
    uint4 r = GI_FRAME_ROW(GI_ROW_SLOTS);
    return product == 0u ? r.x : (product == 1u ? r.y : (product == 2u ? r.z : r.w));
}

// G5's per-pixel pass (gi_frame.h PixelParams, row 7): whether the frame has the denoised image, its slot, its guides and its size; the image is the traced size divided by the divisor.
bool GiPixelPresent()
{
    return (GiFlags() & GI_FLAG_PIXEL_PASS) != 0u && GiSlot(GI_PRODUCT_PIXEL_IRRADIANCE) != GI_NO_SLOT;
}

struct GiPixelRow
{
    uint depthSlot;
    uint normalSlot;
    uint width;
    uint height;
    uint divisor;
    uint raw;
};

GiPixelRow GiLoadPixelRow()
{
    uint4 r = GI_FRAME_ROW(GI_ROW_PIXEL);
    GiPixelRow p;
    p.depthSlot = r.x & 0xFFFFu;
    p.normalSlot = r.x >> 16u;
    p.width = r.y & 0xFFFFu;
    p.height = r.y >> 16u;
    p.divisor = r.z > 1u ? r.z : 1u;
    p.raw = r.w & 1u;
    return p;
}

bool GiFieldValid()
{
    return (GiFlags() & GI_FLAG_VALID) != 0u;
}

bool GiReplacesSsgi()
{
    return (GiFlags() & GI_FLAG_REPLACES_SSGI) != 0u;
}

float GiFade()
{
    return GiRowFloats(GI_ROW_FADE).x;
}

float GiSafeRange()
{
    return GiRowFloats(GI_ROW_FADE).y;
}

float GiStrength()
{
    return GiRowFloats(GI_ROW_FADE).z;
}

float GiBounceWeight()
{
    return GiRowFloats(GI_ROW_FADE).w;
}

float3 GiCamera()
{
    float4 r = GiRowFloats(GI_ROW_CAMERA);
    return float3(r.x, r.y, r.z);
}

float GiGroundAlbedo()
{
    return GiRowFloats(GI_ROW_CAMERA).w;
}

float3 GiSunColour()
{
    float4 r = GiRowFloats(GI_ROW_SUN);
    return float3(r.x, r.y, r.z);
}

float GiSunSinElevation()
{
    return GiRowFloats(GI_ROW_SUN).w;
}

float3 GiAmbientUp()
{
    float4 r = GiRowFloats(GI_ROW_AMBIENT_UP);
    return float3(r.x, r.y, r.z);
}

float3 GiAmbientDown()
{
    float4 r = GiRowFloats(GI_ROW_AMBIENT_DOWN);
    return float3(r.x, r.y, r.z);
}

struct GiCascade
{
    int originX;
    int originY;
    int originZ;
    uint atlasY;
    uint dimX;
    uint dimY;
    uint dimZ;
    uint weightY;
    float spacing;
    float invSpacing;
    float maxDistance;
    float blendBand;
};

GiCascade GiLoadCascade(uint c)
{
    uint row = GI_ROW_CASCADE0 + GI_ROWS_PER_CASCADE * c;
    uint4 a = GI_FRAME_ROW(row);
    uint4 b = GI_FRAME_ROW(row + 1u);
    float4 d = GiRowFloats(row + 2u);
    GiCascade k;
    k.originX = asint(a.x);
    k.originY = asint(a.y);
    k.originZ = asint(a.z);
    k.atlasY = a.w;
    k.dimX = b.x;
    k.dimY = b.y;
    k.dimZ = b.z;
    k.weightY = b.w;
    k.spacing = d.x;
    k.invSpacing = d.y;
    k.maxDistance = d.z;
    k.blendBand = d.w;
    return k;
}

// ---- the lookup -------------------------------------------------------------------------------------------------------------------------------

struct GiSample
{
    float3 irradiance;     // E_surf: what surfaces send, scene colour units (no sky)
    float skyVisibility;   // the cosine weighted share of the lobe that sees the sky
    float confidence;      // 0..1: how much of the lookup the field holds (the rest is the fallback's)
};

// c mod dim, never negative (the toroidal slot of a lattice coordinate).
uint GiWrap(int c, uint dim)
{
    int d = (int)dim;
    int r = c % d;
    if (r < 0)
    {
        r = r + d;
    }
    return (uint)r;
}

// The atlas coordinates (0..1) of direction `oct` (GiOctEncode of it) in the tile of slot (sx, sy, sz) of a cascade.
float2 GiProbeUv(GiCascade k, float invAtlasW, float invAtlasH, uint sx, uint sy, uint sz, float2 oct)
{
    float px = (float)((sx + k.dimX * sy) * GI_PROBE_TILE) + (float)GI_PROBE_BORDER + oct.x * (float)GI_PROBE_INTERIOR;
    float py = (float)(k.atlasY + sz * GI_PROBE_TILE) + (float)GI_PROBE_BORDER + oct.y * (float)GI_PROBE_INTERIOR;
    return float2(px * invAtlasW, py * invAtlasH);
}

// How far a world point is inside a cascade's usable window, 0..1: 1 deeper than blendBand probes from the edge, 0 at it and outside.
float GiCascadeBlend(GiCascade k, float3 p)
{
    float gx = p.x * k.invSpacing - (float)k.originX;
    float gy = p.y * k.invSpacing - (float)k.originY;
    float gz = p.z * k.invSpacing - (float)k.originZ;
    float edge = min(min(min(gx, (float)(k.dimX - 1u) - gx), min(gy, (float)(k.dimY - 1u) - gy)), min(gz, (float)(k.dimZ - 1u) - gz));
    return k.blendBand > 0.0f ? saturate(edge / k.blendBand) : (edge > 0.0f ? 1.0f : 0.0f);
}

// 1 up to (1 - GI_RANGE_FADE_SHARE) of the safe range from the camera, falling to 0 at it; 0 where no safe range is set.
float GiRangeFade(float distanceM, float safeRangeM)
{
    if (!(safeRangeM > 0.0f))
    {
        return 0.0f;
    }
    float start = safeRangeM * (1.0f - GI_RANGE_FADE_SHARE);
    return saturate((safeRangeM - distanceM) / (safeRangeM - start));
}

// One cascade's lookup at world point p, normal n (unit): the eight probes round the point, weighted by trilinear x validity x the smooth back face term x the Chebyshev visibility.
GiSample GiCascadeLookup(GiCascade k, float invAtlasW, float invAtlasH, float3 p, float3 n)
{
    float offset = GI_NORMAL_OFFSET_SPACINGS * k.spacing;
    float3 q = float3(p.x + n.x * offset, p.y + n.y * offset, p.z + n.z * offset);
    // The lattice position, kept inside the window so that all eight corners are.
    float gx = min(max(q.x * k.invSpacing, (float)k.originX), (float)k.originX + (float)(k.dimX - 1u));
    float gy = min(max(q.y * k.invSpacing, (float)k.originY), (float)k.originY + (float)(k.dimY - 1u));
    float gz = min(max(q.z * k.invSpacing, (float)k.originZ), (float)k.originZ + (float)(k.dimZ - 1u));
    int bx = min((int)floor(gx), k.originX + (int)k.dimX - 2);
    int by = min((int)floor(gy), k.originY + (int)k.dimY - 2);
    int bz = min((int)floor(gz), k.originZ + (int)k.dimZ - 2);
    float fx = gx - (float)bx;
    float fy = gy - (float)by;
    float fz = gz - (float)bz;
    float2 octN = GiOctEncode(n);

    float sumW = 0.0f;
    float sumT = 0.0f;
    float sky = 0.0f;
    float3 acc = float3(0.0f, 0.0f, 0.0f);
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
            float c = variance / (variance + diff * diff);
            wc = max(c * c * c, GI_CHEBYSHEV_FLOOR);
        }
        float w = wt * wv * bf * wc;
        float2 uv = GiProbeUv(k, invAtlasW, invAtlasH, sx, sy, sz, octN);
        float4 e = GI_SAMPLE_IRRADIANCE(uv);
        acc = acc + float3(e.x, e.y, e.z) * w;
        sky = sky + e.w * w;
        sumW = sumW + w;
        sumT = sumT + wt * wv;
    }
    GiSample s;
    s.irradiance = float3(0.0f, 0.0f, 0.0f);
    s.skyVisibility = 0.0f;
    if (sumW > GI_MIN_WEIGHT_SUM)
    {
        float rcp = 1.0f / sumW;
        s.irradiance = acc * rcp;
        s.skyVisibility = sky * rcp;
    }
    s.confidence = sumT;
    return s;
}

// The field's lookup at world point p with unit normal n: the finest cascade that holds the point, blended into the coarser ones over the band at its edge, each cascade counted by how much
// of its probes are valid there (a cascade that is still being built hands the point to the next one), and the whole faded to 0 toward the safe range.
GiSample GiLookup(float3 p, float3 n)
{
    uint4 header = GI_FRAME_ROW(GI_ROW_HEADER);
    uint count = min(header.y, GI_MAX_CASCADES);
    float invAtlasW = 1.0f / (float)header.z;
    float invAtlasH = 1.0f / (float)header.w;
    float remaining = 1.0f;
    float sky = 0.0f;
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    for (uint c = 0u; c < count; ++c)
    {
        GiCascade k = GiLoadCascade(c);
        float w = GiCascadeBlend(k, p);
        if (!(w > 0.0f))
        {
            continue;
        }
        GiSample s = GiCascadeLookup(k, invAtlasW, invAtlasH, p, n);
        float cw = remaining * (w * s.confidence);
        acc = acc + s.irradiance * cw;
        sky = sky + s.skyVisibility * cw;
        remaining = remaining - cw;
        if (remaining <= GI_REMAINING_STOP)
        {
            break;
        }
    }
    float cover = 1.0f - remaining;
    GiSample outS;
    outS.irradiance = float3(0.0f, 0.0f, 0.0f);
    outS.skyVisibility = 0.0f;
    if (cover > GI_MIN_COVER)
    {
        float rcp = 1.0f / cover;
        outS.irradiance = acc * rcp;
        outS.skyVisibility = sky * rcp;
    }
    float3 cam = GiCamera();
    float3 dc = float3(p.x - cam.x, p.y - cam.y, p.z - cam.z);
    outS.confidence = cover * GiRangeFade(sqrt(dot(dc, dc)), GiSafeRange());
    return outS;
}

// ---- the ambient, the baseline, the share -----------------------------------------------------------------------------------------------------

// The game's flat hemisphere ambient for a normal: the down colour at the bottom, the up colour at the top.
float3 GiAmbientHemisphere(float3 n)
{
    float t = 0.5f + 0.5f * n.y;
    float3 up = GiAmbientUp();
    float3 down = GiAmbientDown();
    return down + (up - down) * t;
}

// The ambient light at a point (M3 section 4.3: M2's hit shading): where the field stands, its irradiance (surfaces) + the sky's ambient x the sky visibility; where it does not, the flat
// hemisphere; blended by confidence x fade. The probe update's own hit shading does NOT use this: it takes GiLookup's surfaces only, times GiBounceWeight().
float3 GiAmbientAt(float3 p, float3 n)
{
    float3 hemisphere = GiAmbientHemisphere(n);
    GiSample s = GiLookup(p, n);
    float k = saturate(s.confidence * GiFade());
    float3 up = GiAmbientUp();
    float3 field = s.irradiance + up * s.skyVisibility;
    return hemisphere + (field - hemisphere) * k;
}

// What an unobstructed plane of albedo GiGroundAlbedo() sends to a surface with normal n: the cosine weighted share of its hemisphere that is ground, (1 - n.y) / 2, times the ground's
// radiance, albedo x (the sun's colour x sin(elevation) + the up ambient). At night the sun term is 0 and the ambient stays.
float3 GiGroundBaseline(float3 n)
{
    float share = 0.5f * (1.0f - n.y) * GiGroundAlbedo();
    float sun = max(GiSunSinElevation(), 0.0f);
    float3 s = GiSunColour();
    float3 a = GiAmbientUp();
    return float3((s.x * sun + a.x) * share, (s.y * sun + a.y) * share, (s.z * sun + a.z) * share);
}

// E_net = max(E_surf - E_base, 0), per channel.
float3 GiNetIrradiance(float3 eSurf, float3 eBase)
{
    return float3(max(eSurf.x - eBase.x, 0.0f), max(eSurf.y - eBase.y, 0.0f), max(eSurf.z - eBase.z, 0.0f));
}

// The share of a pixel the screen-space bounce light keeps, 0..1: all of it where the field has nothing, none where it stands.
float GiSsgiShare(float confidence, float fade)
{
    return 1.0f - saturate(confidence * fade);
}

#endif
