// MCLA GPU renderer -- the probe update's shared HLSL (package G2 of perf/CITY-RT-M3.md; src/gpu/gi_update_rules.h is the C++ twin and says what everything is).
//
// What the update's two passes share (hlsl/gi_update_trace_cs.hlsl traces the rays of the probes the scheduler chose and shades their hits in the library's GI mode, hlsl/gi_update_blend_cs.hlsl
// integrates the rays into the probes' maps with adaptive hysteresis and classifies the probes inside geometry): the hash and the rotation of a probe's ray set, the spherical Fibonacci
// directions, the ray record, the probe word, the octahedral texels, the integration of a texel from the rays, the classification and the blend. src/gpu/gi_update_rules_test.cpp compiles THIS FILE
// as C++ (hlsl_cpp_shim.h) and checks every function against the twin (gi_update_rules.h, and gi_frame.h's IntegrateTexel, the field's definition of the units) on random inputs, and every #define
// against the header's numbers; that is why the text keeps to a subset both languages read: scalar and float3 arithmetic, no out parameters (a function returns a struct), no swizzles of more
// than one component, and only the intrinsics abs sqrt min max saturate floor frac pow sin cos dot cross asfloat asuint f32tof16 f16tof32.
//
// THE RESOURCES. The includer that wants the integration (GiuIntegrateTexel exists only where GIU_RAY_DIR is defined) defines, BEFORE including this file, the accessors of the rays it holds (the blend pass keeps them in group shared memory):
//   GIU_RAY_DIR(i)        float3: the unit direction of ray i (GiuRayDirection of the probe's rotation)
//   GIU_RAY_RADIANCE(i)   float3: what the hit sent back along ray i (zero for a ray that escaped)
//   GIU_RAY_DISTANCE(i)   float: metres to the hit
//   GIU_RAY_ESCAPED(i)    bool: the ray left the city
// Nothing else is read here.

#ifndef MCLA_GI_UPDATE_COMMON_HLSLI
#define MCLA_GI_UPDATE_COMMON_HLSLI

// ---- the numbers (src/gpu/gi_update_rules.h) ----------------------------------------------------------------------------------------------------

#define GIU_FORMAT_VERSION 1

#define GIU_TRACE_GROUP 64u             // the trace pass's threads in a group
#define GIU_GROUPS_X 4096u              // both passes are dispatched (GIU_GROUPS_X, ceil(groups / GIU_GROUPS_X), 1): group g = y * GIU_GROUPS_X + x
#define GIU_RAY_WORDS 4u                // a ray record is one uint4
#define GIU_RAY_BYTES 16u
#define GIU_MAX_RADIANCE 32.0f          // the most a ray carries: a firefly's guard (neon, a sun glint)
#define GIU_TWO_PI 6.283185307179586f
#define GIU_GOLDEN 0.6180339887498949f
#define GIU_MOMENTS_LOBE_POWER 8.0f     // gi_frame.h kMomentsLobePower
#define GIU_RAY_SCALE 4.0f              // gi_frame.h kRayScale

#define GIU_RAY_HIT 1u                  // ray record flags (word 1, bits 16..31)
#define GIU_RAY_BACK 2u
#define GIU_RAY_LOD 4u
#define GIU_RAY_SKIPPED 8u              // record 0 of a probe the trace left alone (an inside probe that is not re-tested this visit): the blend does nothing for it

#define GIU_PROBE_DIRTY 1u              // probe word flags: traced because its data is stale or its light changed (not because it is new): the jump hysteresis
#define GIU_PROBE_WORD_FLAGS_SHIFT 2u
#define GIU_PROBE_WORD_INDEX_SHIFT 6u

#define GIU_INSIDE_BACK_SHARE 0.30f     // M3 section 3.1: more than this share of the rays that hit back faces
#define GIU_INSIDE_NEAREST_SPACINGS 0.1f // ... or a hit nearer than this many spacings
#define GIU_INSIDE_RETEST_MASK 7u       // an inside probe is traced again at one visit in 8 (hash)
#define GIU_JUMP_RELATIVE 0.25f         // the probe's mean changed by more than this (and by more than GIU_JUMP_SIGMAS standard errors of the new mean): the jump hysteresis
#define GIU_JUMP_SIGMAS 4.0f
#define GIU_RAY_FIXED 4096.0f           // the group's sums of the rays' luminance and its square are fixed point this much
#define GIU_JUMP_FLOOR 0.05f            // the least a mean is taken as in a relative change (scene colour units)
#define GIU_CHANGED_STAT 0.02f          // the census counts a probe whose mean changed by more than this
#define GIU_CONVERGED_UPDATES 32u       // the census counts a probe with this many updates blended as converged
#define GIU_FIXED_SCALE 1024.0f         // the group's sums of luminance are fixed point this much
#define GIU_STAT_HIST_BINS 32u

// ---- the hash, the rotation and the rays ----------------------------------------------------------------------------------------------------------

// lowbias32 (Wellons): a good 32 bit mix.
uint GiuHash(uint x)
{
    x = x ^ (x >> 16u);
    x = x * 0x7feb352du;
    x = x ^ (x >> 15u);
    x = x * 0x846ca68bu;
    x = x ^ (x >> 16u);
    return x;
}

// 0 <= result < 1 from the hash's top 24 bits (exact in a float).
float GiuUnit(uint h)
{
    return (float)(h >> 8u) * (1.0f / 16777216.0f);
}

// The seed of one probe's ray set at one update: new every frame, so that successive updates sample new directions (no banding), and the same for the trace and the blend of that update.
uint GiuSeed(uint frame, uint cascade, uint index)
{
    return GiuHash(index * 0x9E3779B1u + cascade * 0x85EBCA6Bu + GiuHash(frame + 0x27d4eb2fu));
}

// The unit direction of ray i of n on a sphere, spherical Fibonacci: z falls evenly, the angle turns by the golden ratio. (The world's y is the axis of the fall.)
float3 GiuFibonacci(uint i, uint n)
{
    float z = 1.0f - (2.0f * (float)i + 1.0f) / (float)n;
    float r = sqrt(max(1.0f - z * z, 0.0f));
    float phi = GIU_TWO_PI * frac((float)i * GIU_GOLDEN);
    return float3(r * cos(phi), z, r * sin(phi));
}

// A uniformly random rotation (Shoemake): a unit quaternion (x, y, z, w) from three numbers.
float4 GiuRotationQuat(uint seed)
{
    float u1 = GiuUnit(GiuHash(seed ^ 0xA511E9B3u));
    float u2 = GiuUnit(GiuHash(seed ^ 0x63D83595u));
    float u3 = GiuUnit(GiuHash(seed ^ 0xB5297A4Du));
    float a = sqrt(1.0f - u1);
    float b = sqrt(u1);
    float t2 = GIU_TWO_PI * u2;
    float t3 = GIU_TWO_PI * u3;
    return float4(a * sin(t2), a * cos(t2), b * sin(t3), b * cos(t3));
}

float3 GiuRotate(float4 q, float3 v)
{
    float3 qv = float3(q.x, q.y, q.z);
    float3 t = cross(qv, v) * 2.0f;
    return v + t * q.w + cross(qv, t);
}

// The direction of ray i of n of a probe whose rotation is q.
float3 GiuRayDirection(uint i, uint n, float4 q)
{
    return GiuRotate(q, GiuFibonacci(i, n));
}

// ---- the probe word (one uint a probe in the probe list) -------------------------------------------------------------------------------------------

uint GiuProbeWord(uint cascade, uint flags, uint index)
{
    return (cascade & 3u) | ((flags & 15u) << GIU_PROBE_WORD_FLAGS_SHIFT) | (index << GIU_PROBE_WORD_INDEX_SHIFT);
}

uint GiuProbeCascade(uint word)
{
    return word & 3u;
}

uint GiuProbeFlags(uint word)
{
    return (word >> GIU_PROBE_WORD_FLAGS_SHIFT) & 15u;
}

uint GiuProbeIndex(uint word)
{
    return word >> GIU_PROBE_WORD_INDEX_SHIFT;
}

// ---- the ray record (uint4: x, y low = rgb as halves, y high = flags, z = the distance as float bits, w = the hit's class) ------------------------------

struct GiuRay
{
    float3 radiance;
    float distance;
    uint flags;
    uint extra;
};

float GiuRadianceChannel(float v)
{
    return min(max(v, 0.0f), GIU_MAX_RADIANCE);
}

uint4 GiuPackRay(float3 radiance, float distance, uint flags, uint extra)
{
    uint r = f32tof16(GiuRadianceChannel(radiance.x));
    uint g = f32tof16(GiuRadianceChannel(radiance.y));
    uint b = f32tof16(GiuRadianceChannel(radiance.z));
    return uint4(r | (g << 16u), b | ((flags & 0xFFFFu) << 16u), asuint(distance), extra);
}

GiuRay GiuUnpackRay(uint4 w)
{
    GiuRay r;
    r.radiance = float3(f16tof32(w.x & 0xFFFFu), f16tof32(w.x >> 16u), f16tof32(w.y & 0xFFFFu));
    r.flags = w.y >> 16u;
    r.distance = asfloat(w.z);
    r.extra = w.w;
    return r;
}

// ---- the octahedral map's texels ------------------------------------------------------------------------------------------------------------------

struct GiuTexelCoord
{
    uint x;
    uint y;
};

// Which texel of a 10 x 10 tile a texel copies (gi_frame.h OctBorderSource): an interior texel itself, a border texel the interior texel across the map's seam.
GiuTexelCoord GiuBorderSource(uint tx, uint ty)
{
    GiuTexelCoord c;
    c.x = tx;
    c.y = ty;
    bool left = tx == 0u;
    bool right = tx == 9u;
    bool top = ty == 0u;
    bool bottom = ty == 9u;
    if (left && top)
    {
        c.x = 8u;
        c.y = 8u;
    }
    else if (right && top)
    {
        c.x = 1u;
        c.y = 8u;
    }
    else if (left && bottom)
    {
        c.x = 8u;
        c.y = 1u;
    }
    else if (right && bottom)
    {
        c.x = 1u;
        c.y = 1u;
    }
    else if (left)
    {
        c.x = 1u;
        c.y = 9u - ty;
    }
    else if (right)
    {
        c.x = 8u;
        c.y = 9u - ty;
    }
    else if (top)
    {
        c.x = 9u - tx;
        c.y = 1u;
    }
    else if (bottom)
    {
        c.x = 9u - tx;
        c.y = 8u;
    }
    return c;
}

// The unit direction of an octahedral coordinate in 0..1 (gi_frame.h OctDecode).
float3 GiuOctDecode(float fx, float fy)
{
    float x = fx * 2.0f - 1.0f;
    float y = fy * 2.0f - 1.0f;
    float z = 1.0f - abs(x) - abs(y);
    float t = saturate(-z);
    x = x + (x >= 0.0f ? -t : t);
    y = y + (y >= 0.0f ? -t : t);
    float il = 1.0f / sqrt(x * x + y * y + z * z);
    return float3(x * il, y * il, z * il);
}

// The direction an INTERIOR texel (tile coordinates 1..8) is the value of: the centre of the texel in the map (gi_frame.h TexelDirection).
float3 GiuTexelDirection(uint tx, uint ty)
{
    return GiuOctDecode(((float)tx - 1.0f + 0.5f) / 8.0f, ((float)ty - 1.0f + 0.5f) / 8.0f);
}

// ---- the integration of a texel from the rays (gi_frame.h IntegrateTexel is the definition; the units are the field's) -------------------------------

struct GiuTexel
{
    float3 irradiance;
    float sky;
    float meanDistance;
    float meanDistanceSq;
};

#ifdef GIU_RAY_DIR
GiuTexel GiuIntegrateTexel(float3 texelDir, uint count, float maxDistance)
{
    float3 e = float3(0.0f, 0.0f, 0.0f);
    float sky = 0.0f;
    float sumW = 0.0f;
    float sumD = 0.0f;
    float sumD2 = 0.0f;
    for (uint i = 0u; i < count; ++i)
    {
        float cosT = max(dot(texelDir, GIU_RAY_DIR(i)), 0.0f);
        float d = 0.0f;
        if (GIU_RAY_ESCAPED(i))
        {
            sky = sky + cosT;
            d = maxDistance;
        }
        else
        {
            e = e + GIU_RAY_RADIANCE(i) * cosT;
            d = min(GIU_RAY_DISTANCE(i), maxDistance);
        }
        float w = pow(cosT, GIU_MOMENTS_LOBE_POWER);
        sumW = sumW + w;
        sumD = sumD + w * d;
        sumD2 = sumD2 + w * d * d;
    }
    float scale = count > 0u ? GIU_RAY_SCALE / (float)count : 0.0f;
    GiuTexel t;
    t.irradiance = e * scale;
    t.sky = sky * scale;
    if (sumW > 1.0e-12f)
    {
        t.meanDistance = sumD / sumW;
        t.meanDistanceSq = sumD2 / sumW;
    }
    else
    {
        t.meanDistance = maxDistance;
        t.meanDistanceSq = maxDistance * maxDistance;
    }
    return t;
}
#endif

// ---- the classification (M3 section 3.1) ------------------------------------------------------------------------------------------------------------

// A probe is INSIDE geometry (switched off: no light, weight 0) when more than GIU_INSIDE_BACK_SHARE of its rays hit back faces, or its nearest hit is closer than a tenth of a spacing.
bool GiuProbeInside(uint backCount, uint rayCount, float nearest, float spacing)
{
    return (float)backCount > GIU_INSIDE_BACK_SHARE * (float)rayCount || nearest < GIU_INSIDE_NEAREST_SPACINGS * spacing;
}

// The share of the rays that hit a back face as the state word's byte (0..255 = 0..100%).
uint GiuBackByte(uint backCount, uint rayCount)
{
    return rayCount > 0u ? min((backCount * 255u + rayCount / 2u) / rayCount, 255u) : 0u;
}

// An inside probe is traced again at one visit in GIU_INSIDE_RETEST_MASK + 1, picked by a hash (the geometry round it may have changed): the others are left alone.
bool GiuSkipInside(bool inside, uint frame, uint probeNumber)
{
    return inside && (GiuHash(frame * 0x9E3779B1u + probeNumber) & GIU_INSIDE_RETEST_MASK) != 0u;
}

// ---- the blend ---------------------------------------------------------------------------------------------------------------------------------------

float GiuLuma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// How much a probe's mean luminance moved, 0..1: |new - old| over the larger of the two and a floor (so that a dark probe's noise is not a change).
float GiuRelativeChange(float oldMean, float newMean)
{
    return abs(newMean - oldMean) / max(max(oldMean, newMean), GIU_JUMP_FLOOR);
}

// The standard error of the mean of the rays' luminance (what the probe's new mean is uncertain by, 64 rays at a time): from the sums of the luminance and of its square.
float GiuStandardError(float sumLuma, float sumLumaSq, uint count)
{
    float n = (float)max(count, 1u);
    float mean = sumLuma / n;
    float variance = max(sumLumaSq / n - mean * mean, 0.0f);
    return sqrt(variance / n);
}

// A probe's mean luminance JUMPED (a lamp on, the sun's colour at dusk): it moved by more than GIU_JUMP_RELATIVE and by more than GIU_JUMP_SIGMAS standard errors. A probe that sees a small bright
// thing (a lamp, a neon sign) with a few of its rays has a new mean that swings from one update to the next: that is noise, not a change, and must not lower the hysteresis.
bool GiuMeanJumped(float oldMean, float newMean, float standardError)
{
    return GiuRelativeChange(oldMean, newMean) > GIU_JUMP_RELATIVE && abs(newMean - oldMean) > GIU_JUMP_SIGMAS * standardError;
}

// The weight of the stored value in the blend. A new probe (never traced, or back from being inside geometry) has no history: 0. Otherwise the running mean while the probe is young
// (n / (n + 1): the first updates average instead of fading), at most `base` (gi_frame.h kHysteresis); a probe traced because its light changed, or whose mean jumped, at most `jump`
// (kHysteresisJump).
float GiuHysteresis(uint updates, bool isNew, bool dirty, bool jumped, float base, float jump)
{
    if (isNew)
    {
        return 0.0f;
    }
    float h = min(base, (float)updates / ((float)updates + 1.0f));
    if (dirty || jumped)
    {
        h = min(h, jump);
    }
    return h;
}

float3 GiuBlend3(float3 oldValue, float3 newValue, float h)
{
    return oldValue * h + newValue * (1.0f - h);
}

float GiuBlend1(float oldValue, float newValue, float h)
{
    return oldValue * h + newValue * (1.0f - h);
}

// The census's histogram bin of a probe's mean irradiance luminance: half octaves from 2^-10 (bin 0) to 2^6 (bin 31).
uint GiuHistBin(float mean)
{
    float v = (log2(max(mean, 1.0e-6f)) + 10.0f) * 2.0f;
    return (uint)min(max(v, 0.0f), 31.0f);
}

#endif
