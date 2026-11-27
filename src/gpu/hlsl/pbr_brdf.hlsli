// MCLA GPU renderer -- the shared physically based light model (PBR level 1, owner-approved 2026-11-22; design perf\PBR.md section 1). THE TWIN of src/gpu/pbr_brdf_rules.h.
//
// THE CONTRACT: every pass that lights a surface with OUR light (the deferred local light pass, the RT hit shading of the city and the cars, the side mirrors,
// the screen-space reflections' weighting, the bounce light's apply, the Ray Reconstruction / Ray Regeneration guides) uses these functions and nothing of its own,
// so a lamp, a headlight, a neon tube and a reflection agree on the same surface. The signatures of the first version (feat/pbr-base) are fixed: the implementation may be refined (PBR-1 owns
// it and its C++ twin src/gpu/pbr_brdf_rules.h), a change of a signature or of a meaning is announced in perf\PBR.md first; functions are only ADDED below.
//
// CONVENTIONS: vectors are unit length, in any one space; n the surface normal, v towards the eye, l towards the light. Roughness is PERCEPTUAL roughness in [0, 1] (the G-buffer's
// 1 - smoothness: perf\PBR.md 1.2); alpha = roughness * roughness is what the GGX formulas take; it is clamped at kPbrMinRoughness so a mirror-smooth surface lit by a light does not make an
// infinite highlight. metalness in [0, 1]. f0_dielectric is the reflectance at normal incidence of the non-metal part (0.04 by default; water 0.02; PbrF0OfClass).
// RADIANCE is in the PHYSICAL convention: a Lambert surface of albedo a lit at normal incidence by `radiance` returns a * radiance / pi. A pass that holds the first model's
// convention (colour x scale: a Lambert surface returns albedo x colour x scale) passes radiance = kPbrPi * colour * scale.
//
// THE TEXT is the subset of HLSL that C++ reads too: src/gpu/pbr_brdf_test.cpp compiles THIS FILE as C++ (through src/gpu/hlsl_cpp_shim.h) and compares every function with the twin, and the
// local light pass's hlsl/local_light.hlsli includes it, so it keeps to scalars, float2 / float3, .x .y .z, abs sqrt saturate dot normalize reflect lerp clamp min max exp2, no `static const`,
// no swizzle of more than one component, no `.xxx` on a scalar, no `out` parameters.
#ifndef MCLA_PBR_BRDF_HLSLI
#define MCLA_PBR_BRDF_HLSLI

#define kPbrPi 3.14159265358979f
#define kPbrMinRoughness 0.045f
#define kPbrDefaultF0 0.04f

// The classes the defaults below depend on (gbuffer_common.hlsli GB_CLASS_*; src/gpu/pbr_brdf_test.cpp checks the numbers) and the metalness of the metal classes.
#define PBR_CLASS_CHROME 9
#define PBR_CLASS_METAL 10
#define PBR_CLASS_SKIN 12
#define PBR_CLASS_WATER 15
#define PBR_METAL_CHROME 1.0f
#define PBR_METAL_METAL 0.85f
#define PBR_F0_WATER 0.02f
#define PBR_F0_SKIN 0.028f

// The sources of a light (light_laws.hlsli LL_SOURCE_*) and the radius of the source each one stands for, in metres (perf\PBR.md 1.5).
#define PBR_SOURCE_HEADLIGHT 0
#define PBR_SOURCE_LAMP 1
#define PBR_SOURCE_NEON 2
#define PBR_SOURCE_POLICE 3
#define PBR_SOURCE_MOD 4
#define PBR_RADIUS_HEADLIGHT 0.08f
#define PBR_RADIUS_LAMP 0.20f
#define PBR_RADIUS_NEON 0.50f
#define PBR_RADIUS_OTHER 0.15f

// The multi-scatter compensation divides by the directional albedo of a white specular layer, E = A + B: kept off 0 where the analytic fit of A + B gets small.
#define PBR_MIN_E 0.2f
// The multi-scatter factor is applied to a surface whose metalness is above PBR_MS_METAL, scaled in over the first 1 / PBR_MS_RAMP of the metalness (a dielectric's factor is below 1.09 even at
// roughness 1 and 1.03 up to 0.7: not worth the polynomial; the ramp keeps a table's small metalness from jumping).
#define PBR_MS_METAL 0.02f
#define PBR_MS_RAMP 8.0f

struct PbrSurface
{
    float3 n;
    float3 albedo;         // the base colour (the G-buffer's RT2 albedo, linear)
    float roughness;       // perceptual roughness [0, 1]
    float metalness;       // [0, 1]
    float f0_dielectric;   // the non-metal reflectance at normal incidence
};

PbrSurface PbrMakeSurface(float3 n, float3 albedo, float roughness, float metalness, float f0_dielectric)
{
    PbrSurface s;
    s.n = n;
    s.albedo = albedo;
    s.roughness = clamp(roughness, kPbrMinRoughness, 1.0f);
    s.metalness = saturate(metalness);
    s.f0_dielectric = f0_dielectric;
    return s;
}

// ---- the material: roughness, metalness, F0 -------------------------------------------------------------------------------------------------------------------------------------

// The G-buffer's smoothness [0, 1] (RT3.r) to perceptual roughness. The byte is already s = 1 - sqrt(alpha) with alpha = sqrt(2 / (n + 2)) of the game's Blinn-Phong power n
// (gbuffer_rules.h part 3: the pixel shaders' exponent is counted twice, gbuffer_material.h GlossPowerScale), so the roughness is exactly its complement: alpha_ggx = (1 - s)^2.
float PbrRoughnessFromSmoothness(float smoothness)
{
    return clamp(1.0f - saturate(smoothness), kPbrMinRoughness, 1.0f);
}

// A Blinn-Phong specular power n to perceptual roughness: alpha = sqrt(2 / (n + 2)), roughness = sqrt(alpha). `n` is the POWER (a pixel shader's exponent register is half of it: x2 first).
float PbrRoughnessFromBlinnPower(float n)
{
    const float alpha = sqrt(2.0f / (max(n, 0.0f) + 2.0f));
    return clamp(sqrt(alpha), kPbrMinRoughness, 1.0f);
}

// The metalness a surface CLASS has by default (perf\PBR.md 1.4): chrome 1, the metal class 0.85 (grimy city metal, brushed metal, rotors), every other class 0.
float PbrMetalOfClass(uint cls)
{
    if (cls == PBR_CLASS_CHROME)
        return PBR_METAL_CHROME;
    if (cls == PBR_CLASS_METAL)
        return PBR_METAL_METAL;
    return 0.0f;
}

// The metalness the SCREEN G-buffer says: RT3's metal flag (gbuffer_common.hlsli GbRt3Metal: the class field's bit 4, set where the material table says metal) or the class's default.
float PbrMetalOfScreen(uint cls, bool metal_flag)
{
    return metal_flag ? 1.0f : PbrMetalOfClass(cls);
}

// The metalness an RT hit's material word says: its metalness byte (bits 24..31, as 0..1) or the class's default, the larger.
float PbrMetalOfWord(uint cls, float metal_byte01)
{
    return max(PbrMetalOfClass(cls), saturate(metal_byte01));
}

// The reflectance at normal incidence of the non-metal part of a class: water 0.02, skin 0.028, everything else 0.04.
float PbrF0OfClass(uint cls)
{
    if (cls == PBR_CLASS_WATER)
        return PBR_F0_WATER;
    if (cls == PBR_CLASS_SKIN)
        return PBR_F0_SKIN;
    return kPbrDefaultF0;
}

// The surface a G-buffer pixel describes: its normal, albedo, smoothness, class and metal flag.
PbrSurface PbrSurfaceOfGbuffer(float3 n, float3 albedo, float smoothness, uint cls, bool metal_flag)
{
    return PbrMakeSurface(n, albedo, PbrRoughnessFromSmoothness(smoothness), PbrMetalOfScreen(cls, metal_flag), PbrF0OfClass(cls));
}

float3 PbrF0(PbrSurface s)
{
    return lerp(float3(s.f0_dielectric, s.f0_dielectric, s.f0_dielectric), s.albedo, s.metalness);
}

// The colour the diffuse term uses: a metal has none.
float3 PbrDiffuseColour(PbrSurface s)
{
    return s.albedo * (1.0f - s.metalness);
}

// The radius of the source a light of this kind stands for (light_laws.hlsli LL_SOURCE_*), metres: the neon underglow's points are a metre apart, each stands for half a spacing.
float PbrLightRadius(uint source)
{
    if (source == PBR_SOURCE_HEADLIGHT)
        return PBR_RADIUS_HEADLIGHT;
    if (source == PBR_SOURCE_LAMP)
        return PBR_RADIUS_LAMP;
    if (source == PBR_SOURCE_NEON)
        return PBR_RADIUS_NEON;
    return PBR_RADIUS_OTHER;
}

// ---- the lobes ----------------------------------------------------------------------------------------------------------------------------------------------------------------

float3 PbrFresnelSchlick(float3 f0, float cos_theta)
{
    const float m = 1.0f - saturate(cos_theta);
    const float m2 = m * m;
    return f0 + (1.0f - f0) * (m2 * m2 * m);
}

// GGX / Trowbridge-Reitz normal distribution; alpha = roughness^2.
float PbrDGgx(float nh, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPbrPi * d * d, 1.0e-20f);
}

// Height-correlated Smith visibility V = G / (4 nl nv).
float PbrVSmithGgxCorrelated(float nv, float nl, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = nl * sqrt(nv * nv * (1.0f - a2) + a2);
    const float gl = nv * sqrt(nl * nl * (1.0f - a2) + a2);
    return 0.5f / max(gv + gl, 1.0e-7f);
}

// The directional albedo of a white specular layer of THIS lobe (D with the height-correlated V, F = 1): E(roughness, N.V) = the integral of D V N.L over the light's hemisphere, and B(roughness, N.V) =
// the part of it that Schlick's Fresnel puts at F0 = 0: the albedo of a surface of reflectance F0 is F0 * (E - B) + B. Both are least squares fits of our own lobe (src/gpu/pbr_brdf_test.cpp
// checks them against the exact integral: E to 0.034, B to 0.020 over roughness 0.045 .. 1 and N.V 0.04 .. 1; the fit's input N.V is clamped at 0.04). (The usual mobile fit of Karis 2014 is for another
// shadowing term and is up to 0.2 off for this one at high roughness.)
float PbrDfgE(float roughness, float nv)
{
    const float x = sqrt(roughness);
    const float y = sqrt(clamp(nv, 0.04f, 1.0f));
    const float p0 = (((2.107673f) * y + (-1.726959f)) * y + (0.385717f)) * y + (0.868638f);
    const float p1 = (((-8.172101f) * y + (-0.226110f)) * y + (1.588044f)) * y + (1.353653f);
    const float p2 = (((6.440065f) * y + (17.661646f)) * y + (-0.243378f)) * y + (-8.244065f);
    const float p3 = ((-16.142164f) * y + (-16.899258f)) * y + (15.628299f);
    const float p4 = (14.536796f) * y + (-8.625241f);
    const float g = (((p4 * x + p3) * x + p2) * x + p1) * x + p0;
    return clamp(g, 0.0f, 1.0f);
}

float PbrDfgB(float roughness, float nv)
{
    const float x = sqrt(roughness);
    const float y = clamp(nv, 0.04f, 1.0f);
    const float p0 = (((6.852671f) * y + (7.361595f)) * y + (-3.673047f)) * y + (0.256724f);
    const float p1 = (((-4.659930f) * y + (-83.194467f)) * y + (22.290481f)) * y + (8.188444f);
    const float p2 = (((-1.864670f) * y + (166.076176f)) * y + (-2.968450f)) * y + (-29.168003f);
    const float p3 = ((-91.156636f) * y + (-58.213473f)) * y + (34.029022f);
    const float p4 = (42.980395f) * y + (-13.292207f);
    const float g = (((p4 * x + p3) * x + p2) * x + p1) * x + p0;
    const float m = 1.0f - y;
    return clamp(max(g, 0.0f) * (m * m * m * m * m), 0.0f, 1.0f);
}

// The split sum's (A, B): the albedo of a surface of reflectance F0 is F0 * A + B; A + B = E.
float2 PbrDfgAnalytic(float roughness, float nv)
{
    const float e = PbrDfgE(roughness, nv);
    const float b = min(PbrDfgB(roughness, nv), e);
    return float2(e - b, b);
}

// The multi-scatter compensation of a single scattering GGX lobe (Kulla and Conty, with the E of a white layer): 1 + F0 (1 / E - 1) per channel. 1 at F0 = 0.
float3 PbrMultiScatter(float3 f0, float roughness, float nv)
{
    const float e = clamp(PbrDfgE(roughness, nv), PBR_MIN_E, 1.0f);
    return float3(1.0f, 1.0f, 1.0f) + f0 * (1.0f / e - 1.0f);
}

// The weight of a reflection (an environment map, a ray traced or screen-space reflection) seen from v: the split sum F0 * A + B, times the multi-scatter factor for a surface that is (partly) a metal
// (a dielectric's is below 1.06 even at roughness 1: left out). Multiply the reflected radiance by it.
float3 PbrEnvSpecularWeight(PbrSurface s, float nv)
{
    const float2 ab = PbrDfgAnalytic(s.roughness, nv);
    const float3 f0 = PbrF0(s);
    const float3 weight = f0 * ab.x + float3(ab.y, ab.y, ab.y);
    if (s.metalness > PBR_MS_METAL)
    {
        const float e = clamp(ab.x + ab.y, PBR_MIN_E, 1.0f);
        return weight * (float3(1.0f, 1.0f, 1.0f) + f0 * ((1.0f / e - 1.0f) * saturate(s.metalness * PBR_MS_RAMP)));
    }
    return weight;
}

// The weight of incoming diffuse light (the bounce light, an ambient term): the diffuse colour times what the specular layer lets through.
float3 PbrEnvDiffuseWeight(PbrSurface s, float nv)
{
    return PbrDiffuseColour(s) * (float3(1.0f, 1.0f, 1.0f) - PbrEnvSpecularWeight(s, nv));
}

// The diffuse lobe: Lambert with the energy the specular layer took out, (1 - F(v.h)) x the diffuse colour / pi. No N.L (the caller's: the game's laws wrap it for some lights).
float3 PbrDiffuseLobe(PbrSurface s, float3 v, float3 l)
{
    const float3 vl = v + l;
    if (!(dot(vl, vl) > 1.0e-12f))
        return float3(0.0f, 0.0f, 0.0f);
    const float3 h = normalize(vl);
    const float vh = saturate(dot(v, h));
    const float3 f = PbrFresnelSchlick(PbrF0(s), vh);
    return (float3(1.0f, 1.0f, 1.0f) - f) * PbrDiffuseColour(s) * (1.0f / kPbrPi);
}

// The specular lobe: F D V with the multi-scatter factor, per steradian per unit radiance, WITHOUT N.L; 0 for a light below the horizon.
float3 PbrSpecularLobe(PbrSurface s, float3 v, float3 l)
{
    const float nl = dot(s.n, l);
    if (!(nl > 0.0f))
        return float3(0.0f, 0.0f, 0.0f);
    const float3 vl = v + l;
    if (!(dot(vl, vl) > 1.0e-12f))
        return float3(0.0f, 0.0f, 0.0f);
    const float nv = max(dot(s.n, v), 1.0e-4f);
    const float3 h = normalize(vl);
    const float nh = saturate(dot(s.n, h));
    const float vh = saturate(dot(v, h));
    const float alpha = s.roughness * s.roughness;
    const float3 f0 = PbrF0(s);
    const float3 f = PbrFresnelSchlick(f0, vh);
    const float dv = PbrDGgx(nh, alpha) * PbrVSmithGgxCorrelated(nv, nl, alpha);
    float3 ms = float3(1.0f, 1.0f, 1.0f);
    if (s.metalness > PBR_MS_METAL)
        ms = float3(1.0f, 1.0f, 1.0f) + (PbrMultiScatter(f0, s.roughness, nv) - float3(1.0f, 1.0f, 1.0f)) * saturate(s.metalness * PBR_MS_RAMP);
    return f * ms * dv;
}

// The full response to a punctual light of `radiance` arriving from l: the diffuse and the specular lobes times N.L.
float3 PbrDirect(PbrSurface s, float3 v, float3 l, float3 radiance)
{
    const float nl = saturate(dot(s.n, l));
    if (nl <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    return (PbrDiffuseLobe(s, v, l) + PbrSpecularLobe(s, v, l)) * radiance * nl;
}

// ---- a light with a size ---------------------------------------------------------------------------------------------------------------------------------------------------------

// What a spherical light adds, per unit radiance (perf\PBR.md 1.5): `diffuse` the lobe at the centre's direction WITHOUT its N.L, `nl` that centre N.L, `specular` the specular lobe at
// the representative point times that point's N.L and the energy normalisation (alpha / alpha')^2 (Karis 2013), alpha' = saturate(alpha + radius / (2 distance)). The light's total is
// (diffuse * nl + specular) * radiance; a pass that wraps the diffuse term itself puts its own factor where nl is.
struct PbrSphereLobes
{
    float3 diffuse;
    float nl;
    float3 specular;
};

PbrSphereLobes PbrSphereLightLobes(PbrSurface s, float3 v, float3 to_light, float radius)
{
    PbrSphereLobes o;
    o.diffuse = float3(0.0f, 0.0f, 0.0f);
    o.nl = 0.0f;
    o.specular = float3(0.0f, 0.0f, 0.0f);
    const float d2 = dot(to_light, to_light);
    if (!(d2 > 1.0e-8f))
        return o;
    const float dist = sqrt(d2);
    const float3 lc = to_light * (1.0f / dist);
    o.nl = saturate(dot(s.n, lc));
    o.diffuse = PbrDiffuseLobe(s, v, lc);
    // the representative point: the point of the sphere nearest to the mirror ray
    const float3 r = reflect(-v, s.n);
    const float3 centre_to_ray = r * dot(to_light, r) - to_light;
    const float c2 = dot(centre_to_ray, centre_to_ray);
    const float3 closest = to_light + centre_to_ray * saturate(radius / sqrt(max(c2, 1.0e-12f)));
    const float3 lr = normalize(closest);
    const float nl_r = saturate(dot(s.n, lr));
    const float alpha = s.roughness * s.roughness;
    const float alpha_w = saturate(alpha + radius / (2.0f * dist));
    const float norm = (alpha / alpha_w) * (alpha / alpha_w);
    o.specular = PbrSpecularLobe(s, v, lr) * (nl_r * norm);
    return o;
}

// The full response to a spherical light of `radiance` at `to_light` (the vector from the surface to the light's centre, metres) and `radius`.
float3 PbrDirectSphere(PbrSurface s, float3 v, float3 to_light, float radius, float3 radiance)
{
    const PbrSphereLobes o = PbrSphereLightLobes(s, v, to_light, radius);
    return (o.diffuse * o.nl + o.specular) * radiance;
}

#endif  // MCLA_PBR_BRDF_HLSLI
