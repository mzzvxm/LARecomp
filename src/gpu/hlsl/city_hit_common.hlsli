// MCLA GPU renderer -- the city's hit data, the shader side of src/citypack/city_hit_format.h (CITY-RT M2 package H0: THE FROZEN INTERFACE of H1 to H4 and G0).
//
// What a ray's hit on the city looks up: the sector table (which pool block a structure's triangles are in), the pool (indices, positions, run ids, the run table, the
// corner attributes), the global material table and the texture table. The layouts, the reasons and the file side are city_hit_format.h's comment; this file is the DECODE only:
// the shading (ResolveHit, CityHitShade) is hlsl/city_hit_shade.hlsli (H4), which includes this.
//
// The includer defines CITY_HIT_SPACE (the register space of the ray tracing descriptor set: the car scene's CAR_RT_SPACE) before including, and binds, after the car scene's
// t0..t3 and rt-hit-shading's t4 and t5:
//   t6  gCityPool       ByteAddressBuffer          every sector detail's per-triangle data, 32 bit byte offsets (city_hit_format.h: THE GPU LAYOUT)
//   t7  gCitySectors    StructuredBuffer<uint4>    two rows a slot, slot = InstanceID & CITY_HIT_INSTANCE_ID_MASK = (sector << 1) | detail (GpuSectorRow)
//   t8  gCityMaterials  StructuredBuffer<uint4>    two rows a material (HitMaterial::Pack), indexed by a run's material id; material 0 is the default
//   t9  gCityTextures   StructuredBuffer<uint4>    one row a texture (GpuTextureRow), indexed by a material's texture index
// The decode of a ray query's committed triangle hit: CityHitDecode(InstanceID, CommittedGeometryIndex, CommittedPrimitiveIndex, CommittedTriangleBarycentrics); inside the candidate
// loop of an alpha tested geometry the same call takes the Candidate* twins of the four values (the exact alpha of M1c reads the texture's alpha at the decoded uv). The positions it
// returns are in the structure's own space (the world: the pack's positions are world positions); the instance's transform (CommittedObjectToWorld3x4) takes them to the frame's
// view space, the geometric normal likewise (it is the normal of an unscaled rigid transform: rotate it).
//
// src/citypack/city_hit_format_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL types and the two buffers, with CITY_HIT_NO_RESOURCES) and checks it against the C++
// twin (DecodeHit) and against the data that was packed, and compares every #define below with the C++ values, so the two cannot drift. That is why the text keeps to a subset both
// languages read: scalar and vector arithmetic, .x .y .z .w, no swizzles of more than one component, no static const arrays, no out parameters, only abs, sqrt, dot, cross,
// min, max, asfloat, f16tof32.
//
// The names: every one starts with CityHit or CITY_HIT_.

#ifndef MCLA_CITY_HIT_COMMON_HLSLI
#define MCLA_CITY_HIT_COMMON_HLSLI

// ---- the numbers (src/citypack/city_hit_format.h) -------------------------------------------------------------------------------------------

#define CITY_HIT_FORMAT_VERSION 1

#define CITY_HIT_INSTANCE_ID_MASK 0x7FFFFFu
#define CITY_HIT_SECTOR_ROWS 2u
#define CITY_HIT_MATERIAL_ROWS 2u
#define CITY_HIT_RUN_BYTES 32u
#define CITY_HIT_TRIANGLE_BYTES 12u
#define CITY_HIT_POSITION_BYTES 12u
#define CITY_HIT_CORNER_BYTES 8u
#define CITY_HIT_CORNERS_BYTES 24u

#define CITY_HIT_SECTOR_VALID 1u
#define CITY_HIT_SECTOR_RUNS 2u
#define CITY_HIT_SECTOR_CORNERS 4u
#define CITY_HIT_SECTOR_FOLIAGE 8u

#define CITY_HIT_RUN_ALPHA_TESTED 1u
#define CITY_HIT_RUN_HAS_UV 2u
#define CITY_HIT_RUN_HAS_COLOUR 4u
#define CITY_HIT_RUN_TWO_SIDED 8u

#define CITY_HIT_NO_TEXTURE 0xFFFFu
#define CITY_HIT_NO_SLOT 0xFFFFFFFFu
#define CITY_HIT_GAMMA_BIT 0x80000000u
#define CITY_HIT_RGB10_MAX 2.0f

#define CITY_HIT_WORD_SMOOTHNESS_SHIFT 0u
#define CITY_HIT_WORD_CLASS_SHIFT 8u
#define CITY_HIT_WORD_FLAGS_SHIFT 13u
#define CITY_HIT_WORD_LIT_SHIFT 16u
#define CITY_HIT_WORD_METAL_SHIFT 24u
#define CITY_HIT_WORD_CLASS_MASK 31u
#define CITY_HIT_WORD_FLAG_SSR 1u
#define CITY_HIT_WORD_FLAG_ALPHA_COVERED 2u
#define CITY_HIT_WORD_FLAG_VEHICLE 4u

#define CITY_HIT_GLOW_NIGHT 0u    // what a glowing material's glow follows (the tint word's bits 30..31: city_hit_format.h kGlow*): the fill lights' night curve
#define CITY_HIT_GLOW_LENS 1u     // a street lamp's lens: the street lamps' schedule
#define CITY_HIT_GLOW_ALWAYS 2u   // a traffic signal: always
#define CITY_HIT_GLOW_SHIFT 30u
#define CITY_HIT_GREY_IN_RED_SHIFT 30u  // the average word's bit 30 (city_hit_format.h kMaterialGreyInRed): the diffuse is packed (grey, specular mask), the albedo is its red channel as a grey

// ---- the resources (an includer that has its own names defines CITY_HIT_NO_RESOURCES and declares gCityPool, gCitySectors, gCityMaterials, gCityTextures itself) ----------------

#ifndef CITY_HIT_NO_RESOURCES
#ifndef CITY_HIT_SPACE
#error "define CITY_HIT_SPACE (the register space of the ray tracing descriptor set) before including city_hit_common.hlsli"
#endif
#define CITY_HIT_JOIN_(a, b) a##b
#define CITY_HIT_JOIN(a, b) CITY_HIT_JOIN_(a, b)
#define CITY_HIT_SPACE_NAME CITY_HIT_JOIN(space, CITY_HIT_SPACE)

ByteAddressBuffer gCityPool : register(t6, CITY_HIT_SPACE_NAME);
StructuredBuffer<uint4> gCitySectors : register(t7, CITY_HIT_SPACE_NAME);
StructuredBuffer<uint4> gCityMaterials : register(t8, CITY_HIT_SPACE_NAME);
StructuredBuffer<uint4> gCityTextures : register(t9, CITY_HIT_SPACE_NAME);
#endif

// ---- the sector table ------------------------------------------------------------------------------------------------------------------------

struct CityHitSector
{
    uint indexOffset;
    uint positionOffset;
    uint attributeOffset;
    uint runIdOffset;
    uint runTableOffset;
    uint triangleCount;
    uint opaqueTriangles;
    uint flags;
};

// The slot of a structure's instance: InstanceID is city_rt_rules.h InstanceId(sector, detail), the slot is its low 23 bits.
CityHitSector CityHitLoadSector(uint instanceId)
{
    const uint row = (instanceId & CITY_HIT_INSTANCE_ID_MASK) * CITY_HIT_SECTOR_ROWS;
    const uint4 a = gCitySectors[row];
    const uint4 b = gCitySectors[row + 1u];
    CityHitSector s;
    s.indexOffset = a.x;
    s.positionOffset = a.y;
    s.attributeOffset = a.z;
    s.runIdOffset = a.w;
    s.runTableOffset = b.x;
    s.triangleCount = b.y;
    s.opaqueTriangles = b.z;
    s.flags = b.w;
    return s;
}

// The triangle's number in the sector detail's order (opaque first, then alpha tested) from a committed hit: a structure has one geometry for the opaque triangles and, when
// there are alpha tested ones, a second; with no opaque triangles the alpha tested geometry is geometry 0 and the offset is 0 anyway.
uint CityHitTriangle(CityHitSector s, uint geometryIndex, uint primitiveIndex)
{
    return primitiveIndex + (geometryIndex == 1u ? s.opaqueTriangles : 0u);
}

// ---- the pool ---------------------------------------------------------------------------------------------------------------------------------

uint CityHitRunId(CityHitSector s, uint tri)
{
    const uint word = gCityPool.Load(s.runIdOffset + (tri >> 1u) * 4u);
    return (word >> ((tri & 1u) * 16u)) & 0xFFFFu;
}

struct CityHitRun
{
    uint material;
    uint flags;
    float2 uvBias;
    float2 uvScale;
};

CityHitRun CityHitLoadRun(CityHitSector s, uint runId)
{
    const uint4 r = gCityPool.Load4(s.runTableOffset + runId * CITY_HIT_RUN_BYTES);
    CityHitRun run;
    run.material = r.x;
    run.flags = r.y;
    run.uvBias = float2(f16tof32(r.z & 0xFFFFu), f16tof32(r.z >> 16u));
    run.uvScale = float2(f16tof32(r.w & 0xFFFFu), f16tof32(r.w >> 16u));
    return run;
}

// A corner's texture coordinates from its first word (unorm16 over the run's range) and its vertex colour from its second (RGBA8, the alpha is the baked occlusion).
float2 CityHitUv(CityHitRun run, uint word)
{
    const float qu = float(word & 0xFFFFu) * (1.0f / 65535.0f);
    const float qv = float(word >> 16u) * (1.0f / 65535.0f);
    return float2(run.uvBias.x + run.uvScale.x * qu, run.uvBias.y + run.uvScale.y * qv);
}

float4 CityHitColour(uint word)
{
    return float4(float(word & 255u), float((word >> 8u) & 255u), float((word >> 16u) & 255u), float(word >> 24u)) * (1.0f / 255.0f);
}

// ---- the whole decode of a committed hit ----------------------------------------------------------------------------------------------------

struct CityHit
{
    uint valid;            // 1: the slot is built and the triangle is in it
    uint triangleIndex;
    uint sectorFlags;
    uint material;         // the global material id (0, the default, where the slot has no runs)
    uint runFlags;
    float3 p0;             // the triangle's corners in the structure's space (the world)
    float3 p1;
    float3 p2;
    float3 normal;         // unit, (p1 - p0) x (p2 - p0), NOT turned toward the ray
    float worldArea;
    float2 uv;             // interpolated texture coordinates (0 without corners)
    float uvArea;          // the triangle's area in texture coordinates
    float4 colour;         // interpolated vertex colour, 0..1 (white without corners)
};

CityHit CityHitEmpty()
{
    CityHit h;
    h.valid = 0u;
    h.triangleIndex = 0u;
    h.sectorFlags = 0u;
    h.material = 0u;
    h.runFlags = 0u;
    h.p0 = float3(0.0f, 0.0f, 0.0f);
    h.p1 = float3(0.0f, 0.0f, 0.0f);
    h.p2 = float3(0.0f, 0.0f, 0.0f);
    h.normal = float3(0.0f, 1.0f, 0.0f);
    h.worldArea = 0.0f;
    h.uv = float2(0.0f, 0.0f);
    h.uvArea = 0.0f;
    h.colour = float4(1.0f, 1.0f, 1.0f, 1.0f);
    return h;
}

// `barycentrics` are the committed hit's (b1, b2): the point is (1 - b1 - b2) p0 + b1 p1 + b2 p2 (CommittedTriangleBarycentrics).
CityHit CityHitDecode(uint instanceId, uint geometryIndex, uint primitiveIndex, float2 barycentrics)
{
    CityHit h = CityHitEmpty();
    const CityHitSector s = CityHitLoadSector(instanceId);
    h.sectorFlags = s.flags;
    if ((s.flags & CITY_HIT_SECTOR_VALID) == 0u)
    {
        return h;
    }
    const uint tri = CityHitTriangle(s, geometryIndex, primitiveIndex);
    h.triangleIndex = tri;
    if (tri >= s.triangleCount)
    {
        return h;
    }
    h.valid = 1u;
    const uint3 index = gCityPool.Load3(s.indexOffset + tri * CITY_HIT_TRIANGLE_BYTES);
    h.p0 = asfloat(gCityPool.Load3(s.positionOffset + index.x * CITY_HIT_POSITION_BYTES));
    h.p1 = asfloat(gCityPool.Load3(s.positionOffset + index.y * CITY_HIT_POSITION_BYTES));
    h.p2 = asfloat(gCityPool.Load3(s.positionOffset + index.z * CITY_HIT_POSITION_BYTES));
    const float3 c = cross(h.p1 - h.p0, h.p2 - h.p0);
    const float l2 = dot(c, c);
    h.worldArea = 0.5f * sqrt(l2);
    if (l2 > 1e-24f)
    {
        h.normal = c * (1.0f / sqrt(l2));
    }
    if ((s.flags & CITY_HIT_SECTOR_RUNS) != 0u)
    {
        const CityHitRun run = CityHitLoadRun(s, CityHitRunId(s, tri));
        h.material = run.material;
        h.runFlags = run.flags;
        if ((s.flags & CITY_HIT_SECTOR_CORNERS) != 0u)
        {
            const uint base = s.attributeOffset + tri * CITY_HIT_CORNERS_BYTES;
            const uint2 k0 = gCityPool.Load2(base);
            const uint2 k1 = gCityPool.Load2(base + CITY_HIT_CORNER_BYTES);
            const uint2 k2 = gCityPool.Load2(base + 2u * CITY_HIT_CORNER_BYTES);
            const float2 uv0 = CityHitUv(run, k0.x);
            const float2 uv1 = CityHitUv(run, k1.x);
            const float2 uv2 = CityHitUv(run, k2.x);
            const float w0 = 1.0f - barycentrics.x - barycentrics.y;
            h.uv = uv0 * w0 + uv1 * barycentrics.x + uv2 * barycentrics.y;
            h.colour = CityHitColour(k0.y) * w0 + CityHitColour(k1.y) * barycentrics.x + CityHitColour(k2.y) * barycentrics.y;
            h.uvArea = 0.5f * abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
        }
    }
    return h;
}

// Texture repeats a metre of the triangle has (sqrt of the area ratio): the texel footprint of a cone of width w metres is w x this x the texture's size, in texels.
float CityHitUvDensity(CityHit h)
{
    return sqrt(h.uvArea / max(h.worldArea, 1e-12f));
}

// ---- the material and the texture ----------------------------------------------------------------------------------------------------------

float3 CityHitRgb10(uint v)
{
    return float3(float(v & 1023u), float((v >> 10u) & 1023u), float((v >> 20u) & 1023u)) * (CITY_HIT_RGB10_MAX / 1023.0f);
}

struct CityHitMaterial
{
    uint word;          // the G-buffer's material word + metal
    uint cls;           // SurfaceClass (gbuffer_types.h; hlsl/gbuffer_common.hlsli GB_CLASS_*)
    uint flags;         // GB_FLAG_*: CITY_HIT_WORD_FLAG_*
    float smoothness;
    float lit;
    float metal;
    uint textureIndex;  // the diffuse's index in the texture table (CITY_HIT_NO_TEXTURE: none)
    uint layerIndex;    // the second layer's (version 1: always none)
    float3 tint;
    float3 average;     // tint x the diffuse's mean: what a hit shows where the texture is not uploaded (a grey from the red mean where greyInRed)
    bool greyInRed;     // the diffuse is the road's packing (grey, specular mask): the albedo is its red channel as a grey (RT-GREEN-BAND; the average word's bit 30)
    float emissive;     // scale of the glow (0: none)
    uint glowKind;      // CITY_HIT_GLOW_*: what the glow follows (word 2's bits 30..31; 3 reads as the night curve)
};

CityHitMaterial CityHitLoadMaterial(uint material)
{
    const uint row = material * CITY_HIT_MATERIAL_ROWS;
    const uint4 a = gCityMaterials[row];
    const uint4 b = gCityMaterials[row + 1u];
    CityHitMaterial m;
    m.word = a.x;
    m.cls = (a.x >> CITY_HIT_WORD_CLASS_SHIFT) & CITY_HIT_WORD_CLASS_MASK;
    m.flags = (a.x >> CITY_HIT_WORD_FLAGS_SHIFT) & 7u;
    m.smoothness = float((a.x >> CITY_HIT_WORD_SMOOTHNESS_SHIFT) & 255u) * (1.0f / 255.0f);
    m.lit = float((a.x >> CITY_HIT_WORD_LIT_SHIFT) & 255u) * (1.0f / 255.0f);
    m.metal = float(a.x >> CITY_HIT_WORD_METAL_SHIFT) * (1.0f / 255.0f);
    m.textureIndex = a.y & 0xFFFFu;
    m.layerIndex = a.y >> 16u;
    m.tint = CityHitRgb10(a.z);
    m.average = CityHitRgb10(a.w);
    m.greyInRed = ((a.w >> CITY_HIT_GREY_IN_RED_SHIFT) & 1u) != 0u;
    m.emissive = asfloat(b.x);
    m.glowKind = a.z >> CITY_HIT_GLOW_SHIFT;
    return m;
}

struct CityHitTexture
{
    uint slot;       // the bindless slot, bit 31 the gamma bit (CITY_HIT_NO_SLOT: none or not uploaded)
    uint width;      // the file's full size
    uint height;
    uint mipCount;   // resident levels
    uint firstMip;   // the first resident level (0: the full size is there): a sample's level is the wanted level minus this, at least 0
    uint flags;      // HitTexture::flags
};

CityHitTexture CityHitLoadTexture(uint textureIndex)
{
    CityHitTexture t;
    t.slot = CITY_HIT_NO_SLOT;
    t.width = 0u;
    t.height = 0u;
    t.mipCount = 0u;
    t.firstMip = 0u;
    t.flags = 0u;
    if (textureIndex != CITY_HIT_NO_TEXTURE)
    {
        const uint4 r = gCityTextures[textureIndex];
        t.slot = r.x;
        t.width = r.y & 0xFFFFu;
        t.height = r.y >> 16u;
        t.mipCount = r.z & 255u;
        t.firstMip = (r.z >> 8u) & 255u;
        t.flags = (r.z >> 16u) & 255u;
    }
    return t;
}

#endif  // MCLA_CITY_HIT_COMMON_HLSLI
