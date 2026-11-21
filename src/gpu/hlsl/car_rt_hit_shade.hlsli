// MCLA GPU renderer -- a hit on a car, shaded from the car's own material (RT-HIT-SHADING; src/gpu/car_rt_material_rules.h has the design, the record's layout, the arithmetic's
// reference and the tests, car_rt_scene.cpp writes the rows).
//
// Included by hlsl/car_rt_refl_cs.hlsl after car_rt_common.hlsli (the top level, the instances' rows, the indices, CarRtHitNormal) and after the includer's own
// `Texture2D<float4> g_textures[]` (the bindless textures, space 0): the textures are read with Load, so no sampler set is needed in the pass's layout. The includer defines
// CAR_RT_SPACE (car_rt_common.hlsli says so).
//
// binding 4  gCarRtUvs        every mesh's texture coordinates: one uint a vertex, a half2 (u in the low half), the ranges the normals have
// binding 5  gCarRtMaterials  the frame's rows (kMatFrameRows), then kMatRows rows an instance in the instances' order
//
// RT-HIT-SHADING part two (car_rt_hit_rules.h has the design): the sun's visibility at a hit also reads the game's own cascades (CarMatCascadeLit, frame rows 8 to 31: the
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
// The numbers of the classes, the flags and the rows are car_rt_material_rules.h's (car_rt_material_rules_test.cpp checks that this file holds them).
//
// CHECK (nothing here was run): the units of the environment map against the paint's (it is read unscaled and the result times 2^exp_bias); the paraboloid's sign and axis (decoded
// from xChrome::PS_Chrome); the sun's colour at night; the gains. mcla_gpu_car_rt_hit_gain, _ambient and _spec move them live; the log's first lines say what the draws held.

#ifndef CAR_RT_HIT_SHADE_HLSLI
#define CAR_RT_HIT_SHADE_HLSLI

ByteAddressBuffer gCarRtUvs : register(t4, CAR_RT_SPACE_NAME);
StructuredBuffer<uint4> gCarRtMaterials : register(t5, CAR_RT_SPACE_NAME);

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

#define CAR_MAT_FRAME_SHADE 1
#define CAR_MAT_FRAME_SHADOW 2
#define CAR_MAT_FRAME_ENV 4
#define CAR_MAT_FRAME_SUN 8
#define CAR_MAT_FRAME_CASCADE 16

static const uint kMatFrameRows = 32;
static const uint kMatCascadeHeaderRow = 8;
static const uint kMatCameraRow = 9;
static const uint kMatCascadeRightRow = 10;
static const uint kMatCascadeTopRow = 11;
static const uint kMatCascadeFirstRow = 12;
static const uint kMatCascadeRowsEach = 5;
static const uint kMatMaxCascades = 4;
static const float kCascadeNormalOffset = 0.03;
static const uint kMatRows = 2;
static const uint kMatNoSlot = 0xFFFFFFFFu;
static const float kMatTintMax = 2.0;
static const float kConeLodScale = 20.0;
static const float kEmissiveGain = 3.0;
static const float kFillShare = 0.2;  // the ambient is at least this share of the sun's colour (car_rt_material_rules.h kFillShare)
// RT-HIT-SHADING-5 (car_rt_material_rules.h has each number and says what it is)
static const float kNightFillShare = 0.4;
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

// The class's own numbers where the draw's constants say nothing (car_rt_material_rules.h kDefaults: none, paint, glass, tyre, chrome, light, interior, other).
static const float kClassFresnelMin[8] = {0.04, 0.05, 0.04, 0.02, 0.70, 0.04, 0.02, 0.04};
static const float kClassFresnelMax[8] = {0.30, 0.60, 1.00, 0.10, 1.00, 0.90, 0.15, 0.30};
static const float kClassEnvLod[8] = {1.5, 0.5, 0.5, 3.0, 0.7, 0.5, 2.5, 1.5};
static const float kClassSpecGain[8] = {0.5, 1.0, 1.0, 0.3, 1.0, 1.0, 0.3, 0.5};
static const float kClassDiffuseGain[8] = {1.0, 1.0, 1.0, 1.0, 0.1, 0.3, 1.0, 1.0};
// RT-HIT-SHADING-5 (car_rt_material_rules.h kClassAmbient, kClassGlint)
static const float kClassAmbient[8] = {1.0, 1.0, 1.0, 0.8, 1.0, 1.0, 0.45, 0.85};
static const float kClassGlint[8] = {0.5, 1.0, 1.0, 0.0, 1.0, 0.8, 0.0, 0.3};

struct CarMatFrame
{
    float3 toSun;      // unit, view space
    uint flags;
    float3 sun;        // the sun's colour, the paint's units
    float shadowReach; // metres
    float3 ambient;
    float ambientGain;
    uint envSharp;     // bindless slot, bit 31 the gamma bit
    uint envBlur;
    float night;       // 0 day, 1 night
    float specGain;
    float3 sky;
    float gain;
    float3 axisX;      // the world's axes in view space
    float3 axisY;
    float3 axisZ;
    float detail;      // RT-HIT-SHADING-5: the hit's own detail (0: the shading as it was)
};

CarMatFrame CarMatLoadFrame()
{
    const uint4 r0 = gCarRtMaterials[0];
    const uint4 r1 = gCarRtMaterials[1];
    const uint4 r2 = gCarRtMaterials[2];
    const uint4 r3 = gCarRtMaterials[3];
    const uint4 r4 = gCarRtMaterials[4];
    CarMatFrame f;
    f.toSun = asfloat(r0.xyz);
    f.flags = r0.w;
    f.sun = asfloat(r1.xyz);
    f.shadowReach = asfloat(r1.w);
    f.ambient = asfloat(r2.xyz);
    f.ambientGain = asfloat(r2.w);
    f.envSharp = r3.x;
    f.envBlur = r3.y;
    f.night = asfloat(r3.z);
    f.specGain = asfloat(r3.w);
    f.sky = asfloat(r4.xyz);
    f.gain = asfloat(r4.w);
    f.axisX = asfloat(gCarRtMaterials[5].xyz);
    f.axisY = asfloat(gCarRtMaterials[6].xyz);
    f.axisZ = asfloat(gCarRtMaterials[7].xyz);
    f.detail = asfloat(gCarRtMaterials[5].w);
    return f;
}

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

float3 CarMatRgb10(uint v)
{
    return float3(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u)) * (kMatTintMax / 1023.0);
}

CarMat CarMatLoad(uint instance)
{
    const uint base = kMatFrameRows + instance * kMatRows;
    const uint4 r0 = gCarRtMaterials[base];
    const uint4 r1 = gCarRtMaterials[base + 1u];
    CarMat m;
    m.cls = r0.x & 0xFu;
    m.flags = (r0.x >> 4) & 0xFu;
    m.fresnelMin = float((r0.x >> 8) & 0xFFu) / 255.0;
    m.fresnelMax = float((r0.x >> 16) & 0xFFu) / 255.0;
    m.fresnelPower = float((r0.x >> 24) & 0xFFu) / 16.0;
    m.diffuse = r0.y;
    m.tintA = CarMatRgb10(r0.z);
    m.tintB = CarMatRgb10(r0.w);
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

// The game's piecewise-linear gamma to linear (the translated shaders' pwlGammaToLinearScalar; the fetch's bit 31 asks for it).
float CarMatPwl(float gamma)
{
    gamma = saturate(gamma);
    float scale;
    float offset;
    if (gamma >= 96.0 / 255.0)
    {
        if (gamma >= 192.0 / 255.0)
        {
            scale = 8.0 / 1024.0;
            offset = -1024.0;
        }
        else
        {
            scale = 4.0 / 1024.0;
            offset = -256.0;
        }
    }
    else
    {
        if (gamma >= 64.0 / 255.0)
        {
            scale = 2.0 / 1024.0;
            offset = -64.0;
        }
        else
        {
            scale = 1.0 / 1024.0;
            offset = 0.0;
        }
    }
    float lin = gamma * ((255.0 * 1024.0) * scale) + offset;
    lin += trunc(lin * scale);
    return lin * (1.0 / 1023.0);
}

// One texel of level `mip`, wrapped (the diffuse) or clamped (the environment), the gamma applied where the fetch asks.
float4 CarMatTexel(Texture2D<float4> tex, int2 p, uint mip, int2 size, bool wrap, bool gamma)
{
    int2 q;
    if (wrap)
    {
        q = ((p % size) + size) % size;
    }
    else
    {
        q = clamp(p, int2(0, 0), size - 1);
    }
    float4 t = tex.Load(int3(q, int(mip)));
    if (gamma)
    {
        t.rgb = float3(CarMatPwl(t.r), CarMatPwl(t.g), CarMatPwl(t.b));
    }
    return t;
}

float4 CarMatBilinear(Texture2D<float4> tex, float2 uv, uint mip, int2 size0, bool wrap, bool gamma)
{
    const int2 size = max(size0 >> int(mip), int2(1, 1));
    const float2 p = uv * float2(size) - 0.5;
    const int2 i0 = int2(floor(p));
    const float2 f = p - float2(i0);
    const float4 a = CarMatTexel(tex, i0, mip, size, wrap, gamma);
    const float4 b = CarMatTexel(tex, i0 + int2(1, 0), mip, size, wrap, gamma);
    const float4 c = CarMatTexel(tex, i0 + int2(0, 1), mip, size, wrap, gamma);
    const float4 d = CarMatTexel(tex, i0 + int2(1, 1), mip, size, wrap, gamma);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// A texture read at mip level `lod` (a float: two levels blended), by the bindless slot (bit 31 the gamma bit).
float4 CarMatSample(uint slot, float2 uv, float lod, bool wrap)
{
    Texture2D<float4> tex = g_textures[NonUniformResourceIndex(slot & 0x7FFFFFFFu)];
    uint w;
    uint h;
    uint levels;
    tex.GetDimensions(0, w, h, levels);
    const bool gamma = (slot & 0x80000000u) != 0u;
    const float l = clamp(lod, 0.0, float(max(levels, 1u) - 1u));
    const uint l0 = uint(floor(l));
    const uint l1 = min(l0 + 1u, max(levels, 1u) - 1u);
    const float f = l - float(l0);
    const int2 size0 = int2(int(w), int(h));
    float4 value = CarMatBilinear(tex, uv, l0, size0, wrap, gamma);
    if (l1 != l0 && f > 0.001)
    {
        value = lerp(value, CarMatBilinear(tex, uv, l1, size0, wrap, gamma), f);
    }
    return value;
}

// The game's environment map (a 512x256 dual paraboloid, four levels) at a unit WORLD direction (car_rt_material_rules.h EnvUv: decoded from xChrome::PS_Chrome).
float2 CarMatEnvUv(float3 r)
{
    const float a = -r.y;
    const float b = -r.z;
    const float c = -r.x;
    float2 uv;
    if (b > 0.0)
    {
        uv = float2(0.25 - c / (4.0 + 4.0 * b), 0.5 + a / (2.0 + 2.0 * b));
    }
    else
    {
        uv = float2(0.75 + c / (4.0 - 4.0 * b), 0.5 + a / (2.0 - 2.0 * b));
    }
    return uv;
}

// A depth texel of the cascade atlas, clamped to its size.
float CarMatLoadDepth(Texture2D<float4> atlas, int2 texel, int2 size)
{
    return atlas.Load(int3(clamp(texel, int2(0, 0), size - 1), 0)).x;
}

// The fraction of the sun's light that reaches the WORLD point `p` through the game's cascades: the collector's own lookup, as volumetrics_inject_cs.hlsl CascadeLit has it and
// car_rt_material_rules.h CascadeLit does (the finest box that holds the point, the atlas uv and depth from that cascade's matrix, a bilinear weighted compare: lit where the stored
// depth is at least the receiver's). 1 where the point is in none of the boxes.
float CarMatCascadeLit(float3 p)
{
    const uint4 header = gCarRtMaterials[kMatCascadeHeaderRow];
    const uint cascades = min(header.y, kMatMaxCascades);
    if (cascades == 0u || header.x == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    const float3 right = asfloat(gCarRtMaterials[kMatCascadeRightRow].xyz);
    const float3 top = asfloat(gCarRtMaterials[kMatCascadeTopRow].xyz);
    const float r = dot(p, right);
    const float t = dot(p, top);
    uint inside = 0u;
    [unroll]
    for (uint i = 0u; i < kMatMaxCascades; ++i)
    {
        if (i < cascades)
        {
            const float4 box = asfloat(gCarRtMaterials[kMatCascadeFirstRow + i * kMatCascadeRowsEach + 4u]);
            inside += (abs(r + box.x) < box.y && abs(t + box.z) < box.w) ? 1u : 0u;
        }
    }
    if (inside == 0u)
    {
        return 1.0;  // outside every cascade: no shadow data
    }
    const uint base = kMatCascadeFirstRow + (cascades - inside) * kMatCascadeRowsEach;
    const float4 r0 = asfloat(gCarRtMaterials[base]);
    const float4 r1 = asfloat(gCarRtMaterials[base + 1u]);
    const float4 r2 = asfloat(gCarRtMaterials[base + 2u]);
    const float4 r3 = asfloat(gCarRtMaterials[base + 3u]);
    const float u = dot(p, float3(r0.x, r1.x, r2.x)) + r3.x;
    const float v = dot(p, float3(r0.y, r1.y, r2.y)) + r3.y;
    const float depth = (dot(p, float3(r0.z, r1.z, r2.z)) + r3.z) * (dot(p, float3(r0.w, r1.w, r2.w)) + r3.w) + asfloat(header.z);
    Texture2D<float4> atlas = g_textures[header.x];
    uint w;
    uint h;
    uint levels;
    atlas.GetDimensions(0, w, h, levels);
    const int2 size = int2(int(w), int(h));
    const float2 tc = float2(u, v) * float2(size) - 0.5;
    const int2 i0 = int2(floor(tc));
    const float2 fr = tc - float2(i0);
    const float s00 = CarMatLoadDepth(atlas, i0, size) >= depth ? 1.0 : 0.0;
    const float s10 = CarMatLoadDepth(atlas, i0 + int2(1, 0), size) >= depth ? 1.0 : 0.0;
    const float s01 = CarMatLoadDepth(atlas, i0 + int2(0, 1), size) >= depth ? 1.0 : 0.0;
    const float s11 = CarMatLoadDepth(atlas, i0 + int2(1, 1), size) >= depth ? 1.0 : 0.0;
    return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);
}

// Is the point lit by the sun: one ray towards it against the whole scene, a few metres (a car's own shadow on itself and on its neighbour).
float CarMatSunVisible(float3 origin, float3 toSun, float reach)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = toSun;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, CAR_RT_MASK_ALL, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

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
    const CarMatFrame f = CarMatLoadFrame();
    if ((f.flags & CAR_MAT_FRAME_SHADE) == 0u)
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
    if (m.diffuse != kMatNoSlot && (m.flags & CAR_MAT_FLAG_UV) != 0u)
    {
        const float2 uv = CarRtHitUv(instance, primitive, barycentrics);
        const float lod = log2(max(footprint, 1e-4)) + m.lodBase;
        const float4 texel = CarMatSample(m.diffuse, uv, lod, true);
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
    const float hemisphere = clamp(0.6 + 0.4 * dot(normal, up), 0.2, 1.0);
    const bool sunKnown = (f.flags & CAR_MAT_FRAME_SUN) != 0u;
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
        if ((f.flags & CAR_MAT_FRAME_CASCADE) != 0u)
        {
            const float3 camera = asfloat(gCarRtMaterials[kMatCameraRow].xyz);
            const float3 lifted = hit + normal * kCascadeNormalOffset;
            const float3 world = camera + float3(dot(lifted, f.axisX), dot(lifted, f.axisY), dot(lifted, f.axisZ));
            const float lit = CarMatCascadeLit(world);
            visible = lit + (1.0 - lit) * saturate(asfloat(gCarRtMaterials[kMatCascadeHeaderRow].w));
        }
        if (visible > 0.0 && (f.flags & CAR_MAT_FRAME_SHADOW) != 0u)
        {
            visible = min(visible, CarMatSunVisible(hit + normal * (0.004 + 0.002 * hit.z), toSun, f.shadowReach));
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
    const float3 fill = max(f.ambient, f.sun * lerp(kFillShare, kNightFillShare, night));
    const float3 light = fill * (f.ambientGain * hemisphere * classAmbient * ground) + f.sun * (ndl * visible * daySun);
    gCarHitClass = m.cls;
    gCarHitNdl = ndl * daySun;  // at night the sun does not reach anything: "facing away" in the census
    gCarHitVisible = visible;
    gCarHitGlow = (m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u ? f.night : 0.0;

    // the reflection: the environment map at the reflected direction, by the fresnel
    const float3 reflected = reflect(direction, normal);
    float3 env;
    [branch]
    if ((f.flags & CAR_MAT_FRAME_ENV) != 0u)
    {
        const float3 world = float3(dot(f.axisX, reflected), dot(f.axisY, reflected), dot(f.axisZ, reflected));
        const float lod = kClassEnvLod[m.cls] + log2(1.0 + kConeLodScale * tanHalf);
        env = CarMatSample(f.envSharp, CarMatEnvUv(world), lod, false).rgb;
    }
    else
    {
        env = f.ambient * (0.6 + 0.4 * dot(reflected, up));
    }
    const float fresnel = m.fresnelMin + (m.fresnelMax - m.fresnelMin) * pow(max(1.0 - nv, 1e-4), max(m.fresnelPower, 0.01));
    const float3 tint = m.cls == CAR_MAT_CLASS_CHROME ? albedo : float3(1.0, 1.0, 1.0);

    float3 result = albedo * light * kClassDiffuseGain[m.cls] + env * (fresnel * kClassSpecGain[m.cls] * f.specGain) * tint * ground;
    // the clear coat's image of the sun (by day) and of a lamp high over the street (at night): a lobe where the reflected ray points at the light, by the coat's fresnel at the ray's own view
    [branch]
    if (detail > 0.0 && kClassGlint[m.cls] > 0.0)
    {
        const float coat = m.cls == CAR_MAT_CLASS_CHROME ? fresnel : kCoatF0 + (1.0 - kCoatF0) * pow(saturate(1.0 - nv), kCoatPower);
        const float sunLobe = ndl > 0.0 ? pow(saturate(dot(reflected, toSun)), kSunGlintPower) : 0.0;
        const float lampLobe = night > 0.0 ? pow(saturate(dot(reflected, up)), kLampGlintPower) : 0.0;
        result += f.sun * (kSunRadiance * sunLobe * visible * daySun + kLampRadiance * lampLobe * night * openness) * (coat * kClassGlint[m.cls] * detail) * tint;
    }
    if ((m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u)
    {
        result += albedo * (m.emissive * kEmissiveGain * f.night);
    }
    colour = max(result, 0.0) * (f.gain * units);
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
    return float((gCarRtMaterials[kMatFrameRows + instance * kMatRows].x >> 24) & 0xFFu) / 16.0;
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
