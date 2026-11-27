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
// RT-HIT-SHADING-5 (car_rt_material_rules.h "the hit's own detail" has the design, the numbers and the reference): the hit's own detail, so that a reflected car is not a flat colour. The sky's
// openness at the hit (one ray towards the sky against the cars of the scene), the contact with the ground (the height over the car's ground, from the caller), the clear coat's image of the
// sun by day and of a street lamp at night (a fresnel by the reflected ray's own view), the class's share of the ambient, the sun's end and a brighter sky at night, the lamps' glow from the
// sun's height. The frame's detail (row 5's w, mcla_gpu_car_rt_hit_detail) scales them: 0 is the shading as it was.
//
// CarHitShade returns false where there is nothing to shade with (hit shading off, no frame light, the instance's class is 0: the shader of that draw is not one the table
// knows) and the caller keeps today's stand-in colour. The colour it makes is in the paint's units times `units` (2^exp_bias): the history's.
//
// PBR LEVEL 1 (task PBR-1B; the frame's RT_HIT_FRAME_PBR, the debug switch mcla_gpu_pbr): with the flag the hit is lit by the SHARED physically based model (hlsl/pbr_brdf.hlsli through hlsl/pbr_rt_hit.hlsli): a PbrSurface from
// the class (chrome a metal, the other classes' roughness, F0 from the draw's fresnel minimum), the ambient through PbrEnvDiffuseWeight, the sun and the street lamp through PbrDirect (their highlights are the BRDF's:
// the clear coat's fake lobes below are not used), the environment through PbrEnvSpecularWeight. The frame's detail (0: the shading as it was) still gates the sun's and the lamp's PbrDirect (off: their diffuse alone as irradiance), the
// openness, the contact and the class's ambient are the car's own and stay. Without the flag every line that is not marked PBR is the model of old. car_rt_material_rules.h ShadeReference is the twin of both.
//
// The numbers of the classes, the flags and the rows are car_rt_material_rules.h's (car_rt_material_rules_test.cpp checks that this file holds them).
//
// CHECK (nothing here was run): the units of the environment map against the paint's (it is read unscaled and the result times 2^exp_bias); the paraboloid's sign and axis (decoded
// from xChrome::PS_Chrome); the sun's colour at night; the gains. mcla_gpu_car_rt_hit_gain, _ambient and _spec move them live; the log's first lines say what the draws held.

#ifndef CAR_RT_HIT_SHADE_HLSLI
#define CAR_RT_HIT_SHADE_HLSLI

#include "car_rt_hit_material.hlsli"
#include "nan_guard.hlsli"
#include "pbr_rt_hit.hlsli"

static const float kCarHitMaxColour = 60000.0;   // a hit's colour in the history's units: what a half float keeps with room

// RT-HIT-SHADING-5 (car_rt_material_rules.h has each number and says what it is)
static const float kNightFillShare = 0.4;
static const float kLampDiffuse = 0.6;  // RT-HIT-SHADING-6 (car_rt_material_rules.h kLampDiffuse): the street lamp's light on a surface that faces up, at night
static const float kCoatF0 = 0.04;
static const float kCoatPower = 5.0;
static const float kSunRadiance = 24.0;
static const float kSunGlintPower = 160.0;
static const float kLampRadiance = 40.0;
static const float kLampGlintPower = 14.0;
static const float kOpenMin = 0.45;
static const float kOpenReach = 2.5;
static const float kOpenJitter = 0.35;
static const float kOpenMinAbove = 0.2;
static const float kOpenHashCells = 100.0;
static const float kContactMin = 0.35;
static const float kContactHeight = 0.5;
static const float kCarCentreRise = 0.6;

// What the last CarHitShade call found out, for the census (car_rt_hit_rules.h census::kLit, kShadow, kAway, kGlow): the hit's class, N.L, the sun's visibility and the night factor
// where it glows (0 where it does not).
static uint gCarHitClass = 0u;
static float gCarHitNdl = 0.0;
static float gCarHitVisible = 1.0;
static float gCarHitGlow = 0.0;

// RT-HIT-SHADING-5 (car_rt_material_rules.h kClassAmbient, kClassGlint)
static const float kClassAmbient[8] = {1.0, 1.0, 1.0, 0.8, 1.0, 1.0, 0.45, 0.85};
static const float kClassGlint[8] = {0.5, 1.0, 1.0, 0.0, 1.0, 0.8, 0.0, 0.3};
// PBR 1B (car_rt_material_rules.h kClassBody, kClassRoughness, kLampRoughness, kF0Min, kF0Max): by class (none, paint, glass, tyre, chrome, light, interior, other). The body's share of the albedo's light (a lamp's lens keeps its old
// diffuse share; chrome is a metal, the model takes its diffuse away), the perceptual roughness (the coat, glass, chrome and a lens take the roughness of the sun's old glint lobe, Blinn-Phong power 160: PbrRoughnessFromBlinnPower
// = 1/3, so the highlight keeps its old width and brightness; a tyre, the interior and the rest are matt), the street lamp's wider lobe (power 14), the range the draw's fresnel minimum is taken as the dielectric F0 in.
static const float kClassBody[8] = {1.0, 1.0, 1.0, 1.0, 1.0, 0.3, 1.0, 1.0};
static const float kClassRoughness[8] = {0.6, 0.3333333, 0.3333333, 0.85, 0.3333333, 0.3333333, 0.8, 0.6};
static const float kLampRoughness = 0.5946036;
static const float kF0Min = 0.02;
static const float kF0Max = 0.1;


// RT-HIT-SHADING-5: a small integer hash (PCG; car_rt_material_rules.h Pcg) and the two numbers in [0, 1) the sky ray's jitter takes from the hit's place (OpenHash).
uint CarMatPcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The sky ray's direction (car_rt_material_rules.h SkyRayDirection, the same arithmetic): normal + up spread over a disc of tangent kOpenJitter by the hit's hash, kept kOpenMinAbove over the
// tangent plane. `normal` is the unit normal facing the ray and `up` the world's up in view space.
float3 CarMatSkyDirection(float3 hit, float3 normal, float3 up)
{
    const int3 cell = int3(floor(hit * kOpenHashCells));
    const uint h = CarMatPcg(asuint(cell.x) + CarMatPcg(asuint(cell.y) + CarMatPcg(asuint(cell.z))));
    const float a = float(h & 0xFFFFu) / 65536.0;
    const float b = float(h >> 16) / 65536.0;
    float3 d = normal + up;
    const float length2 = length(d);
    d = length2 > 0.3 ? d / length2 : up;
    const float3 helper = abs(d.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = cross(helper, d) / max(length(cross(helper, d)), 1e-12);
    const float3 bt = cross(d, t);
    const float angle = 6.283185307179586 * a;
    float3 e = normalize(d + (t * cos(angle) + bt * sin(angle)) * (kOpenJitter * sqrt(b)));
    const float above = dot(e, normal);
    if (above < kOpenMinAbove)
    {
        e = normalize(e + (kOpenMinAbove - above) * normal);
    }
    return e;
}

// The sky's openness at a hit: one ray from the hit along CarMatSkyDirection, kOpenReach metres, against every car of the scene; 0 where it meets one (the hit is closed in), 1 where it does not.
float CarMatSkyOpen(float3 hit, float3 normal, float3 up)
{
    RayDesc ray;
    ray.Origin = hit + normal * (0.010 + 0.003 * hit.z);
    ray.Direction = CarMatSkyDirection(hit, normal, up);
    ray.TMin = 0.0;
    ray.TMax = kOpenReach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, CAR_RT_MASK_ALL, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// The colour of a hit (the history does not show it), in the history's units. `normal` is the unit normal facing the ray, `direction` the ray's unit direction, `hit` the point
// in view space, `footprint` the cone's width in metres at the hit, `tanHalf` the cone's half angle's tangent, `units` 2^exp_bias, `height` the hit's height in metres over the ground of its car
// (-1: not known).
bool CarHitShade(uint instance, uint primitive, float2 barycentrics, float3 normal, float3 direction, float3 hit, float footprint, float tanHalf, float units, float height,
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
    const float nv = saturate(-dot(normal, direction));
    const float3 albedo = CarMatAlbedo(m, instance, primitive, barycentrics, footprint, nv);

    // the light: the ambient with a hemisphere term, the sun by N.L and its visibility
    const float3 up = f.axisY;
    const float hemisphere = RtHitHemisphere(f, normal);
    const bool sunKnown = (f.flags & RT_HIT_FRAME_SUN) != 0u;
    const float3 toSun = sunKnown ? f.toSun : up;
    // the hour from the sun's height (the host's row, NightFromSunHeight) where the frame knows the sun: the sun's light ends at night, the sky's fill and the lamps' images take over
    const float night = sunKnown ? f.night : 0.0;
    const float daySun = 1.0 - night;
    const float ndl = saturate(dot(normal, toSun));
    // The sun's visibility: the game's cascades at the hit's world point (the buildings', the trees', the bridges' shadows: the world the reflected car stands in), with the
    // collector's own fade, and one ray towards the sun against the cars (a car's shadow on itself and on its neighbour, which the cascades do not hold). The darker of the two
    // (car_rt_material_rules.h SunVisibility).
    float visible = 1.0;
    [branch]
    if (ndl > 0.0 && daySun > 0.0)
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
    // RT-HIT-SHADING-5: what the hit keeps of its ambient and its reflection (the class, the sky's openness, the contact with the ground), each scaled by the frame's detail
    const float detail = max(f.detail, 0.0);
    const float strength = saturate(detail);
    float openness = 1.0;
    [branch]
    if (detail > 0.0)
    {
        openness = CarMatSkyOpen(hit, normal, up);
    }
    const float contact = height >= 0.0 ? kContactMin + (1.0 - kContactMin) * smoothstep(0.0, kContactHeight, height) : 1.0;
    const float ground = lerp(1.0, kOpenMin + (1.0 - kOpenMin) * openness, strength) * lerp(1.0, contact, strength);
    const float classAmbient = lerp(1.0, kClassAmbient[m.cls], strength);
    const float3 fill = max(f.ambient, f.sun * lerp(kRtHitFillShare, kNightFillShare, night));
    // RT-HIT-SHADING-6: the lamp over the street lights what faces up (a roof, a bonnet, a shoulder), at night, by the sky's openness: the flank under the neighbour's roof line stays in its shade
    const float lampLight = kLampDiffuse * night * saturate(dot(normal, up)) * openness * strength;
    const float3 light = fill * (f.ambientGain * hemisphere * classAmbient * ground) + f.sun * (ndl * visible * daySun + lampLight);
    gCarHitClass = m.cls;
    gCarHitNdl = ndl * daySun;  // at night the sun does not reach anything: "facing away" in the census
    gCarHitVisible = visible;
    const float glowNight = CarMatGlowScale(m, night);
    gCarHitGlow = (m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u ? m.emissive * glowNight : 0.0;

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
    const float fresnel = CarMatFresnel(m, nv);
    const float3 tint = m.cls == CAR_MAT_CLASS_CHROME ? albedo : float3(1.0, 1.0, 1.0);

    float3 result;
    [branch]
    if ((f.flags & RT_HIT_FRAME_PBR) != 0u)
    {
        // CHECK: Compare PBR on/off in car reflections and inspect the lower body panels for a colour cast.
        // PBR 1B: the hit through the shared model. The surface: the albedo by the body's share, the class's roughness, chrome a metal, the draw's fresnel minimum as the dielectric F0.
        const PbrSurface surface = PbrMakeSurface(normal, albedo * kClassBody[m.cls], kClassRoughness[m.cls], m.cls == CAR_MAT_CLASS_CHROME ? 1.0 : 0.0, clamp(m.fresnelMin, kF0Min, kF0Max));
        const float3 toViewer = -direction;
        const float3 ambientLight = fill * (f.ambientGain * hemisphere * classAmbient * ground);
        const bool direct = detail > 0.0;   // the frame's detail 0 is the shading as it was: the sun and the lamp light the surface diffusely only
        result = PbrRtDiffuse(surface, nv, direct ? ambientLight : ambientLight + f.sun * (ndl * visible * daySun + lampLight));
        [branch]
        if (direct && ndl > 0.0 && daySun > 0.0)
        {
            result += PbrRtDirect(surface, toViewer, toSun, f.sun * (visible * daySun));
        }
        [branch]
        if (direct && lampLight > 0.0)
        {
            PbrSurface lampSurface = surface;
            lampSurface.roughness = max(surface.roughness, kLampRoughness);   // a lamp over the street is a wider light than the sun's disc
            result += PbrRtDirectIrradiance(lampSurface, toViewer, up, f.sun * lampLight);
        }
        result += PbrRtReflection(surface, nv, env) * (kClassSpecGain[m.cls] * f.specGain * ground);
    }
    else
    {
        result = albedo * light * kClassDiffuseGain[m.cls] + env * (fresnel * kClassSpecGain[m.cls] * f.specGain) * tint * ground;
        // the clear coat's image of the sun (by day) and of a lamp high over the street (at night): a lobe where the reflected ray points at the light, by the coat's fresnel at the ray's own view
        [branch]
        if (detail > 0.0 && kClassGlint[m.cls] > 0.0)
        {
            const float coat = m.cls == CAR_MAT_CLASS_CHROME ? fresnel : kCoatF0 + (1.0 - kCoatF0) * pow(saturate(1.0 - nv), kCoatPower);
            const float sunLobe = ndl > 0.0 ? pow(saturate(dot(reflected, toSun)), kSunGlintPower) : 0.0;
            const float lampLobe = night > 0.0 ? pow(saturate(dot(reflected, up)), kLampGlintPower) : 0.0;
            result += f.sun * (kSunRadiance * sunLobe * visible * daySun + kLampRadiance * lampLobe * night * openness) * (coat * kClassGlint[m.cls] * detail) * tint;
        }
    }
    if ((m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u)
    {
        result += albedo * (m.emissive * kRtHitEmissiveGain * glowNight);
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
