// MCLA GPU renderer -- a hit on a car, shaded from the car's own material (RT-HIT-SHADING; src/gpu/car_rt_material_rules.h has the design, the record's layout, the arithmetic's
// reference and the tests, car_rt_scene.cpp writes the rows).
//
// Included by hlsl/car_rt_refl_cs.hlsl after car_rt_common.hlsli (the top level, the instances' rows, the indices, CarRtHitNormal) and after the includer's own
// `Texture2D<float4> g_textures[]` (the bindless textures, space 0): the textures are read with Load, so no sampler set is needed in the pass's layout. The includer defines
// CAR_RT_SPACE (car_rt_common.hlsli says so).
//
// binding 4  gCarRtUvs        every mesh's texture coordinates: one uint a vertex, a half2 (u in the low half), the ranges the normals have
// binding 5  gCarRtMaterials  the frame's rows (kRtHitFrameRows), then kMatRows rows an instance in the instances' order
//
// RT-HIT-SHADING part two (car_rt_hit_rules.h has the design): the sun's visibility at a hit also reads the game's own cascades (RtHitCascadeLit, frame rows 8 to 31: the
// volumetric light's lookup, copied), the clear coat's helpers (CarCoatShare, CarCoatMix) are here so that the player's pass and the opponents' variant share them.
//
// CarHitShade returns false where there is nothing to shade with (hit shading off, no frame light, the instance's class is 0: the shader of that draw is not one the table
// knows) and the caller keeps today's stand-in colour. The colour it makes is in the paint's units times `units` (2^exp_bias): the history's.
//
// The numbers of the classes, the flags and the rows are car_rt_material_rules.h's (car_rt_material_rules_test.cpp checks that this file holds them).
//
// CHECK (nothing here was run): the units of the environment map against the paint's (it is read unscaled and the result times 2^exp_bias); the paraboloid's sign and axis (decoded
// from xChrome::PS_Chrome); the sun's colour at night; the gains. mcla_gpu_car_rt_hit_gain, _ambient and _spec move them live; the log's first lines say what the draws held.

#ifndef CAR_RT_HIT_SHADE_HLSLI
#define CAR_RT_HIT_SHADE_HLSLI

ByteAddressBuffer gCarRtUvs : register(t4, CAR_RT_SPACE_NAME);
StructuredBuffer<uint4> gCarRtMaterials : register(t5, CAR_RT_SPACE_NAME);

// The pieces every hit shading shares are in rt_hit_common.hlsli (CITY-RT M2 H4): the frame's rows and the hit's light (rt_hit_frame.hlsli, included here), the bindless texture reads, the game's
// cascades, the environment map and the sun's ray (rt_hit_sample.hlsli, included below after the car's own records). The frame's rows are the first rows of binding 5 here, and the sun's ray
// looks in the car scene's top level.
#define RT_HIT_FRAME_ROW(i) gCarRtMaterials[i]
#define RT_HIT_TLAS gCarRtTlas
#include "rt_hit_frame.hlsli"
#include "nan_guard.hlsli"

static const float kCarHitMaxColour = 60000.0;   // a hit's colour in the history's units: what a half float keeps with room

#define CAR_MAT_CLASS_NONE 0
#define CAR_MAT_CLASS_PAINT 1
#define CAR_MAT_CLASS_GLASS 2
#define CAR_MAT_CLASS_TYRE 3
#define CAR_MAT_CLASS_CHROME 4
#define CAR_MAT_CLASS_LIGHT 5
#define CAR_MAT_CLASS_INTERIOR 6
#define CAR_MAT_CLASS_OTHER 7

#define CAR_MAT_FLAG_PAINT 1
#define CAR_MAT_FLAG_DECAL 2
#define CAR_MAT_FLAG_UV 4
#define CAR_MAT_FLAG_EMISSIVE 8

static const uint kMatRows = 2;

// What the last CarHitShade call found out, for the census (car_rt_hit_rules.h census::kLit, kShadow, kAway, kGlow): the hit's class, N.L, the sun's visibility and the night factor
// where it glows (0 where it does not).
static uint gCarHitClass = 0u;
static float gCarHitNdl = 0.0;
static float gCarHitVisible = 1.0;
static float gCarHitGlow = 0.0;

// The class's own numbers where the draw's constants say nothing (car_rt_material_rules.h kDefaults: none, paint, glass, tyre, chrome, light, interior, other).
static const float kClassFresnelMin[8] = {0.04, 0.05, 0.04, 0.02, 0.70, 0.04, 0.02, 0.04};
static const float kClassFresnelMax[8] = {0.30, 0.60, 1.00, 0.10, 1.00, 0.90, 0.15, 0.30};
static const float kClassEnvLod[8] = {1.5, 0.5, 0.5, 3.0, 0.7, 0.5, 2.5, 1.5};
static const float kClassSpecGain[8] = {0.5, 1.0, 1.0, 0.3, 1.0, 1.0, 0.3, 0.5};
static const float kClassDiffuseGain[8] = {1.0, 1.0, 1.0, 1.0, 0.1, 0.3, 1.0, 1.0};

struct CarMat
{
    uint cls;
    uint flags;
    float fresnelMin;
    float fresnelMax;
    float fresnelPower;
    uint diffuse;
    float3 tintA;
    float3 tintB;
    float flop;
    float lodBase;
    float emissive;
};

CarMat CarMatLoad(uint instance)
{
    const uint base = kRtHitFrameRows + instance * kMatRows;
    const uint4 r0 = gCarRtMaterials[base];
    const uint4 r1 = gCarRtMaterials[base + 1u];
    CarMat m;
    m.cls = r0.x & 0xFu;
    m.flags = (r0.x >> 4) & 0xFu;
    m.fresnelMin = float((r0.x >> 8) & 0xFFu) / 255.0;
    m.fresnelMax = float((r0.x >> 16) & 0xFFu) / 255.0;
    m.fresnelPower = float((r0.x >> 24) & 0xFFu) / 16.0;
    m.diffuse = r0.y;
    m.tintA = RtHitRgb10(r0.z);
    m.tintB = RtHitRgb10(r0.w);
    m.flop = asfloat(r1.x);
    m.lodBase = asfloat(r1.y);
    m.emissive = asfloat(r1.z);
    return m;
}

// The hit's texture coordinates, interpolated (the vertices as CarRtHitNormal finds them).
float2 CarRtHitUv(uint instance, uint primitive, float2 barycentrics)
{
    const uint4 row = gCarRtInstances[instance];
    const uint tri = (row.x + primitive * 3u) * 4u;
    const uint3 vertex = uint3(gCarRtIndices.Load(tri), gCarRtIndices.Load(tri + 4u), gCarRtIndices.Load(tri + 8u)) + row.y;
    const uint p0 = gCarRtUvs.Load(vertex.x * 4u);
    const uint p1 = gCarRtUvs.Load(vertex.y * 4u);
    const uint p2 = gCarRtUvs.Load(vertex.z * 4u);
    const float2 t0 = float2(f16tof32(p0 & 0xFFFFu), f16tof32(p0 >> 16));
    const float2 t1 = float2(f16tof32(p1 & 0xFFFFu), f16tof32(p1 >> 16));
    const float2 t2 = float2(f16tof32(p2 & 0xFFFFu), f16tof32(p2 >> 16));
    return t0 * (1.0 - barycentrics.x - barycentrics.y) + t1 * barycentrics.x + t2 * barycentrics.y;
}

#include "rt_hit_sample.hlsli"

// The colour of a hit (the history does not show it), in the history's units. `normal` is the unit normal facing the ray, `direction` the ray's unit direction, `hit` the point
// in view space, `footprint` the cone's width in metres at the hit, `tanHalf` the cone's half angle's tangent, `units` 2^exp_bias.
bool CarHitShade(uint instance, uint primitive, float2 barycentrics, float3 normal, float3 direction, float3 hit, float footprint, float tanHalf, float units,
                 out float3 colour)
{
    colour = float3(0.0, 0.0, 0.0);
    const RtHitFrame f = RtHitLoadFrame();
    if ((f.flags & RT_HIT_FRAME_SHADE) == 0u)
    {
        return false;
    }
    const CarMat m = CarMatLoad(instance);
    if (m.cls == CAR_MAT_CLASS_NONE)
    {
        return false;
    }
    const bool paint = (m.flags & CAR_MAT_FLAG_PAINT) != 0u;
    const float nv = saturate(-dot(normal, direction));

    // the albedo: the paint's gradient, the diffuse texture (or the livery over the paint), the tint
    float3 albedo = m.tintA;
    if (paint && m.flop > 0.0)
    {
        albedo = lerp(m.tintB, m.tintA, pow(max(nv, 1e-4), m.flop));
    }
    if (m.diffuse != kRtHitNoSlot && (m.flags & CAR_MAT_FLAG_UV) != 0u)
    {
        const float2 uv = CarRtHitUv(instance, primitive, barycentrics);
        const float lod = log2(max(footprint, 1e-4)) + m.lodBase;
        const float4 texel = RtHitSample(m.diffuse, uv, lod, true);
        if ((m.flags & CAR_MAT_FLAG_DECAL) != 0u)
        {
            albedo = lerp(albedo, texel.rgb, saturate(texel.a));
        }
        else
        {
            albedo *= texel.rgb;
        }
    }

    // the light: the ambient with a hemisphere term, the sun by N.L and its visibility
    const float3 up = f.axisY;
    const float hemisphere = RtHitHemisphere(f, normal);
    const bool sunKnown = (f.flags & RT_HIT_FRAME_SUN) != 0u;
    const float3 toSun = sunKnown ? f.toSun : up;
    const float ndl = saturate(dot(normal, toSun));
    // The sun's visibility: the game's cascades at the hit's world point (the buildings', the trees', the bridges' shadows: the world the reflected car stands in), with the
    // collector's own fade, and one ray towards the sun against the cars (a car's shadow on itself and on its neighbour, which the cascades do not hold). The darker of the two
    // (car_rt_material_rules.h SunVisibility).
    float visible = 1.0;
    [branch]
    if (ndl > 0.0)
    {
        if ((f.flags & RT_HIT_FRAME_CASCADE) != 0u)
        {
            visible = RtHitCascadeVisible(f, hit, normal);
        }
        if (visible > 0.0 && (f.flags & RT_HIT_FRAME_SHADOW) != 0u)
        {
            // the lift follows the magnitude of the view z: a hit behind the camera (a car behind a chase camera, in the paint's reflection) has a negative z, and past 2 m a negative lift puts the ray's
            // origin under the surface, the ray meets the surface it left and the hit is in shadow
            visible = min(visible, RtHitSunVisible(hit + normal * (0.004 + 0.002 * abs(hit.z)), toSun, f.shadowReach, CAR_RT_MASK_ALL));
        }
    }
    const float3 light = RtHitAmbientLight(f, hemisphere) + f.sun * (ndl * visible);
    gCarHitClass = m.cls;
    gCarHitNdl = ndl;
    gCarHitVisible = visible;
    gCarHitGlow = (m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u ? f.night : 0.0;

    // the reflection: the environment map at the reflected direction, by the fresnel
    const float3 reflected = reflect(direction, normal);
    float3 env;
    [branch]
    if ((f.flags & RT_HIT_FRAME_ENV) != 0u)
    {
        const float3 world = RtHitEnvDirection(f, reflected);
        const float lod = kClassEnvLod[m.cls] + log2(1.0 + kRtHitConeLodScale * tanHalf);
        env = RtHitEnvRead(f, world, lod);
    }
    else
    {
        env = f.ambient * (0.6 + 0.4 * dot(reflected, up));
    }
    const float fresnel = m.fresnelMin + (m.fresnelMax - m.fresnelMin) * pow(max(1.0 - nv, 1e-4), max(m.fresnelPower, 0.01));
    const float3 tint = m.cls == CAR_MAT_CLASS_CHROME ? albedo : float3(1.0, 1.0, 1.0);

    float3 result = albedo * light * kClassDiffuseGain[m.cls] + env * (fresnel * kClassSpecGain[m.cls] * f.specGain) * tint;
    if ((m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u)
    {
        result += albedo * (m.emissive * kRtHitEmissiveGain * f.night);
    }
    // not negative, at most what a half float keeps (the reflection pass's kMaxColour), a NaN black: decided by the bits (nan_guard.hlsli), for max(NaN, 0) is 0 on D3D12 only
    colour = NgHeld3(result * (f.gain * units), kCarHitMaxColour);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// the clear coat (car_rt_hit_rules.h: CoatShare, MixCoat, the aux image's w)
// ---------------------------------------------------------------------------------------------------------------------

static const float kCoatNormal = 0.45;
static const float kCoatGrazing = 0.85;
static const float kCoatMax = 0.9;
static const float kCoatMinPower = 1.0;
static const float kAuxSharpScale = 0.49;

// The fresnel exponent of an instance's paint (its record's row 0, bits 24..31: the power times 16; the class's where the draw's constants say none).
float CarMatFresnelPower(uint instance)
{
    return float((gCarRtMaterials[kRtHitFrameRows + instance * kMatRows].x >> 24) & 0xFFu) / 16.0;
}

// The share of a texel's reflection that is the clear coat's mirror (CoatShare).
float CarCoatShare(float nv, float power, float gain)
{
    if (!(gain > 0.0))
    {
        return 0.0;
    }
    const float curve = pow(saturate(1.0 - nv), max(power, kCoatMinPower));
    return clamp(gain * (kCoatNormal + (kCoatGrazing - kCoatNormal) * curve), 0.0, kCoatMax);
}

// RT-HIT-SHADING-4 (car_rt_hit_rules.h MirrorShare, ConeHitCounts): where the clear coat's mirror ray hits another car the mirror is at least kOthersSharpShare of the texel, and a cone ray that
// hits another car where the mirror ray did not (and one was cast) is dropped: a car is as sharp as a mirror, with no haze of its colour past its outline.
static const float kOthersSharpShare = 0.9;

float MirrorShare(float coatShare, bool mirrorOnCar)
{
    return mirrorOnCar ? max(coatShare, kOthersSharpShare) : coatShare;
}

bool ConeHitCounts(bool coneOnCar, bool haveMirror, bool mirrorOnCar)
{
    return !coneOnCar || !haveMirror || mirrorOnCar;
}

// The glossy cone's sums (the sum of the rays' weights, of their weights times their colours, of their weights times the share of the hit that is on another car; over `rays` rays) and the
// coat's ray (its weight, its colour, its weight times that share) added by `share` (MixCoat): the texel's weight; `colour` its mean colour, `carWeight` the part of the weight
// on another car (the code's sign: the paint adds the clear coat's layer for it), `sharp` the part of the weight that is the coat's. With a share of 0 it is the cone alone, as the pass always made it.
float CarCoatMix(float coneWeightSum, float3 coneColourSum, float coneCarSum, float rays, float share, float coatWeight, float3 coatColour, float coatCarWeight,
                 out float3 colour, out float carWeight, out float sharp)
{
    const float a = saturate(share);
    const float coneWeight = min(coneWeightSum / rays, 1.0);
    const float weight = (1.0 - a) * coneWeight + a * coatWeight;
    carWeight = (1.0 - a) * min(coneCarSum / rays, 1.0) + a * coatCarWeight;
    colour = float3(0.0, 0.0, 0.0);
    sharp = 0.0;
    if (weight > 0.0)
    {
        colour = ((1.0 - a) * coneColourSum / rays + a * coatWeight * coatColour) / weight;
        sharp = saturate(a * coatWeight / weight);
    }
    return weight;
}

#endif  // CAR_RT_HIT_SHADE_HLSLI
