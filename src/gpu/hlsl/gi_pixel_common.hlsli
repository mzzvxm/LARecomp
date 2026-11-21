// MCLA GPU renderer -- the per-pixel detail pass's maths (CITY-RT M3 package G5; src/gpu/gi_pixel_rules.h is the specification and THE TWIN of every function here).
//
// Included by hlsl/gi_pixel_trace_cs.hlsl (the ray and its radiance) and by hlsl/gi_apply_ps.hlsl (the mix of the denoised term with the field's). src/gpu/gi_pixel_test.cpp compiles THIS FILE as C++ (through
// hlsl_cpp_shim.h) and compares each function with the twin on random inputs, and every #define with the header's number: that is why the text keeps to the subset both languages read: scalar and vector
// arithmetic (float3 + float3, float3 * float), .x .y .z, no out parameters, no attributes, and only the intrinsics abs sqrt min max saturate floor frac cos sin dot lerp.
//
// The names: every one starts with Gip or GIP_.

#ifndef MCLA_GI_PIXEL_COMMON_HLSLI
#define MCLA_GI_PIXEL_COMMON_HLSLI

#define GIP_FORMAT_VERSION 1

#define GIP_REACH_M 12.0f
#define GIP_MAX_RADIANCE 32.0f
#define GIP_CAR_ALBEDO 0.12f
#define GIP_MISS_SHARE 1.0f
#define GIP_FULL_LENGTH 4.0f
#define GIP_MAX_WEIGHT 1.0f
#define GIP_MISS_DISTANCE 4096.0f
#define GIP_COSINE_FLOOR 0.02f
#define GIP_R2_A1 0.7548776662466927f
#define GIP_R2_A2 0.5698402909980532f
#define GIP_TWO_PI 6.283185307179586f
#define GIP_LIGHTS_MAX 6u
#define GIP_FLAG_STATS 1u
#define GIP_MIN_TAP_WEIGHT 1.0e-4f

// The trace's own rows come after the apply's buffer (the field's 20 rows and the apply's 9: gi_pixel_rules.h): row GIP_ROW_BASE + i.
#define GIP_ROW_BASE 29u
#define GIP_STAT_PIXELS 0u
#define GIP_STAT_HITS 1u
#define GIP_STAT_CAR_HITS 2u
#define GIP_STAT_MISSES 3u
#define GIP_STAT_SKIPPED 4u
#define GIP_STAT_NO_DATA 5u

// lowbias32 (Wellons), the probes' hash.
uint GipHash(uint x)
{
    x = x ^ (x >> 16u);
    x = x * 0x7feb352du;
    x = x ^ (x >> 15u);
    x = x * 0x846ca68bu;
    x = x ^ (x >> 16u);
    return x;
}

// 0 <= result < 1 from the hash's top 24 bits (exact in a float).
float GipUnit(uint h)
{
    return (float)(h >> 8u) * (1.0f / 16777216.0f);
}

// The pixel's two numbers in 0..1 at a frame: a per-pixel offset (a hash of the pixel alone) advanced by the R2 sequence each frame, so that the frames of one pixel are a low discrepancy set.
float2 GipNoise(uint x, uint y, uint frame)
{
    uint h = GipHash(x * 0x9E3779B1u + y * 0x85EBCA6Bu + 0x27d4eb2fu);
    float a = GipUnit(h);
    float b = GipUnit(GipHash(h ^ 0x68E31DA4u));
    float f = (float)(frame & 0xFFFFu);
    return float2(frac(a + f * GIP_R2_A1), frac(b + f * GIP_R2_A2));
}

struct GipBasis
{
    float3 t;
    float3 b;
};

// The orthonormal frame about a unit vector (Duff et al. 2017: the library's CityHitMakeBasis).
GipBasis GipMakeBasis(float3 n)
{
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float c = n.x * n.y * a;
    GipBasis r;
    r.t = float3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
    r.b = float3(c, s + n.y * n.y * a, -n.y);
    return r;
}

// A cosine weighted direction about the unit normal n from two numbers in 0..1 (Malley's: the disc lifted to the hemisphere), kept at least GIP_COSINE_FLOOR above the tangent plane.
float3 GipCosineDirection(float3 n, float u1, float u2)
{
    float r = sqrt(min(u1, 0.9999f));
    float phi = GIP_TWO_PI * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(1.0f - x * x - y * y, 0.0f));
    GipBasis bs = GipMakeBasis(n);
    float3 d = bs.t * x + bs.b * y + n * z;
    float above = dot(d, n);
    if (above < GIP_COSINE_FLOOR)
    {
        d = d + n * (GIP_COSINE_FLOOR - above);
    }
    return d * (1.0f / sqrt(dot(d, d)));
}

// (the GPU's max returns the other operand for a NaN: a NaN and a negative value are 0)
float GipClampChannel(float v)
{
    return v >= 0.0f ? min(v, GIP_MAX_RADIANCE) : 0.0f;
}

float3 GipClampRadiance(float3 c)
{
    return float3(GipClampChannel(c.x), GipClampChannel(c.y), GipClampChannel(c.z));
}

// A ray with no hit within the reach carries the field's irradiance at the pixel.
float3 GipMissRadiance(float3 eField)
{
    return GipClampRadiance(eField * GIP_MISS_SHARE);
}

// A car's hit: a neutral diffuse in the frame's ambient and the sun's N.L.
float3 GipCarRadiance(float3 ambient, float3 sun, float nDotL)
{
    return GipClampRadiance((ambient + sun * saturate(nDotL)) * GIP_CAR_ALBEDO);
}

// The raw mode's hit distance (dlss_rr_rules.h RawHitDistance's rule).
float GipHitDistance(bool traced, bool hit, float length)
{
    if (!traced)
    {
        return 0.0f;
    }
    if (!hit)
    {
        return GIP_MISS_DISTANCE;
    }
    return length > 0.0f ? (length < GIP_MISS_DISTANCE ? length : GIP_MISS_DISTANCE) : 0.0f;
}

// The ray's origin is lifted off the surface along its normal (the library's CityHitLift).
float GipOriginLift(float viewDistance)
{
    return min(0.006f + 0.003f * viewDistance, 0.4f);
}

// The pixel term's weight against the field's, 0..1: the denoiser's history length (1: fresh: 0; GIP_FULL_LENGTH: all); a pixel the pass did not trace (a depth guide of 0) has none.
float GipWeight(float length, float depthGuide)
{
    if (!(depthGuide > 0.0f))
    {
        return 0.0f;
    }
    return saturate((length - 1.0f) / (GIP_FULL_LENGTH - 1.0f)) * GIP_MAX_WEIGHT;
}

// One tap of a reduced image's read: the bilinear weight times the depth and the normal agreement (the apply's own: gi_apply_common.hlsli GiaDepthWeight, GiaNormalWeight, given as numbers here), floored
// as the apply's upsample floors it; 0 for a tap the pass did not trace (a depth guide of 0).
float GipTapWeight(float bilinear, float tapDepth, float depthWeight, float normalWeight)
{
    return tapDepth > 0.0f ? bilinear * max(depthWeight * normalWeight, GIP_MIN_TAP_WEIGHT) : 0.0f;
}

float3 GipMix(float3 eField, float3 ePixel, float w)
{
    if (!(w > 0.0f))
    {
        return eField;
    }
    return float3(eField.x + (ePixel.x - eField.x) * w, eField.y + (ePixel.y - eField.y) * w, eField.z + (ePixel.z - eField.z) * w);
}

#endif
