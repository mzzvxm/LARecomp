// MCLA GPU renderer -- the exact alpha of a cut-out triangle of the city (CITY-RT M1c; the C++ twin is src/gpu/city_hit_alpha_rules.h).
//
// The ray traced sun shadows meet the city's cut-out triangles (fences, grates, neon cut-outs: the pack's second geometry of a sector, not opaque) as CANDIDATES of a ray query. Until now a
// candidate was a blocker by a rule that knew nothing of the texture (never, always, or by a hash: city_rt_shadow_rules.h AlphaMode). With the hit data's corner streams (H1) and the
// texture set (H2) the candidate's own texture coordinates are known and so is its picture: this file decodes the candidate (city_hit_common.hlsli CityHitDecode takes the Candidate* twins of
// the committed values), finds its material's diffuse texture, and reads the texture's ALPHA at the candidate's UV. The ray is a blocker where the alpha reaches the reference (the game's
// alpha test passes a texel whose alpha is above its reference: RB_ALPHA_REF, mostly 0.5 for the cut-outs the pack keeps; the host pushes it), and goes through the holes.
//
// THE LEVEL. A shadow ray's footprint at the hit is the width of the pixel it started from plus the sun disc's spread over the distance it travelled (the includer says the width, `cone`,
// in metres); in texels that is cone x the triangle's texture repeats a metre (CityHitUvDensity) x the texture's size. The level is log2 of it, rounded, never COARSER than the lowest level
// that keeps the shape of a cut-out: a level of fewer than 8 texels on its short side (CITY_HIT_ALPHA_SHAPE_MIN_LOG2) has averaged a fence's wires and a grate's bars to a grey that the
// reference cuts all or nothing. The texture holds the levels from the class's cap (firstMip) on: a level above the first resident one reads the first, a level beyond the last stored reads
// the last.
//
// WHEN IT IS NOT KNOWN. The slot is not resident (the texture is still being read, or the set is off), the material has no texture, the sector has no corners or the run no uv, the pool does
// not hold the block: CityHitAlphaOf answers known = 0 and the caller does what it did before (the stochastic rule). Nothing waits for a texture and nothing is wrong while it loads.
//
// The includer defines CITY_HIT_ALPHA_TEXEL(slot, texel, level) before including: the alpha (0..1) of the texel `texel` (an int2, inside the level) of level `level` (0 = the first
// resident level) of the bindless texture `slot` (bit 31 of a slot is the gamma bit, which the alpha does not care for; the macro masks it). The shadow pass's is
//   g_textures[NonUniformResourceIndex((slot) & 0x7FFFFFFFu)].Load(int3(texel, int(level))).w
// and the test's reads a fake texture set. src/gpu/city_hit_alpha_rules_test.cpp compiles THIS FILE as C++ (a shim of the HLSL types, as city_hit_format_test.cpp does for the decode) and
// checks it against the twin; that is why the text keeps to scalars and vectors, .x .y .z .w, no out parameters and only floor, log2, min, max, abs, sqrt (and the common file's).

#ifndef MCLA_CITY_HIT_ALPHA_HLSLI
#define MCLA_CITY_HIT_ALPHA_HLSLI

#include "city_hit_common.hlsli"

#ifndef CITY_HIT_ALPHA_TEXEL
#error "define CITY_HIT_ALPHA_TEXEL(slot, texel, level) before including city_hit_alpha.hlsli"
#endif

// A level is not read below this many texels (log2) on the texture's short side.
#define CITY_HIT_ALPHA_SHAPE_MIN_LOG2 3
// The sector flags a hit must have for its UV to be there: the runs (the material and the uv transform) and the corners.
#define CITY_HIT_ALPHA_SECTOR_NEEDS (CITY_HIT_SECTOR_RUNS | CITY_HIT_SECTOR_CORNERS)

struct CityHitAlpha
{
    uint known;    // 1: the alpha was read from the texture; 0: it could not be (the caller falls back)
    float alpha;   // 0..1 (1 where not known)
    uint level;    // the level read, counted from the first resident one
};

// The texel footprint of a ray of width `cone` metres on this hit's texture: texture repeats a metre x the texture's size (the geometric mean of its sides).
float CityHitAlphaTexels(CityHit h, CityHitTexture t, float cone)
{
    return cone * CityHitUvDensity(h) * sqrt(float(t.width) * float(t.height));
}

// The coarsest level (counted from the texture's full size) that keeps the shape: the last with at least 2^CITY_HIT_ALPHA_SHAPE_MIN_LOG2 texels on its short side. 0 for a small texture.
int CityHitAlphaShapeLimit(CityHitTexture t)
{
    const int shortSide = int(min(t.width, t.height));
    const int log2Short = int(floor(log2(float(max(shortSide, 1)))));
    return max(log2Short - CITY_HIT_ALPHA_SHAPE_MIN_LOG2, 0);
}

// The level to read, counted from the first resident one: the footprint's level (rounded), not coarser than the shape limit, shifted by what the class's cap dropped, inside what is stored.
uint CityHitAlphaLevel(CityHitTexture t, float texels)
{
    const int wanted = min(int(floor(log2(max(texels, 1.0f)) + 0.5f)), CityHitAlphaShapeLimit(t));
    const int resident = max(int(t.mipCount), 1);
    return uint(clamp(wanted - int(t.firstMip), 0, resident - 1));
}

// One texel of the level, the coordinates wrapped (the cut-outs tile) inside the level's size.
float CityHitAlphaTexel(uint slot, int x, int y, int width, int height, uint level)
{
    const int wx = ((x % width) + width) % width;
    const int wy = ((y % height) + height) % height;
    return CITY_HIT_ALPHA_TEXEL(slot, int2(wx, wy), level);
}

// The alpha of the level at `uv` (texture repeats), bilinear as the game's own sampler reads it.
float CityHitAlphaBilinear(uint slot, float2 uv, uint level, CityHitTexture t)
{
    const int at = int(t.firstMip) + int(level);
    const int width = max(int(t.width) >> at, 1);
    const int height = max(int(t.height) >> at, 1);
    const float px = uv.x * float(width) - 0.5f;
    const float py = uv.y * float(height) - 0.5f;
    const float fx = floor(px);
    const float fy = floor(py);
    const float tx = px - fx;
    const float ty = py - fy;
    const int x0 = int(fx);
    const int y0 = int(fy);
    const float a = CityHitAlphaTexel(slot, x0, y0, width, height, level);
    const float b = CityHitAlphaTexel(slot, x0 + 1, y0, width, height, level);
    const float c = CityHitAlphaTexel(slot, x0, y0 + 1, width, height, level);
    const float d = CityHitAlphaTexel(slot, x0 + 1, y0 + 1, width, height, level);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

CityHitAlpha CityHitAlphaNone()
{
    CityHitAlpha r;
    r.known = 0u;
    r.alpha = 1.0f;
    r.level = 0u;
    return r;
}

// The alpha of a decoded hit for a ray of footprint `cone` metres.
CityHitAlpha CityHitAlphaOf(CityHit h, float cone)
{
    CityHitAlpha r = CityHitAlphaNone();
    if (h.valid == 0u || (h.sectorFlags & CITY_HIT_ALPHA_SECTOR_NEEDS) != CITY_HIT_ALPHA_SECTOR_NEEDS || (h.runFlags & CITY_HIT_RUN_HAS_UV) == 0u)
    {
        return r;
    }
    const CityHitMaterial m = CityHitLoadMaterial(h.material);
    if (m.textureIndex == CITY_HIT_NO_TEXTURE)
    {
        return r;
    }
    const CityHitTexture t = CityHitLoadTexture(m.textureIndex);
    if (t.slot == CITY_HIT_NO_SLOT || t.mipCount == 0u || t.width == 0u || t.height == 0u)
    {
        return r;
    }
    r.level = CityHitAlphaLevel(t, CityHitAlphaTexels(h, t, cone));
    r.alpha = CityHitAlphaBilinear(t.slot, h.uv, r.level, t);
    r.known = 1u;
    return r;
}

// A ray query's candidate triangle of the city's cut-out geometry (the Candidate* values of the query).
CityHitAlpha CityHitAlphaOfCandidate(uint instanceId, uint geometryIndex, uint primitiveIndex, float2 barycentrics, float cone)
{
    return CityHitAlphaOf(CityHitDecode(instanceId, geometryIndex, primitiveIndex, barycentrics), cone);
}

// The ray is a blocker at this alpha when it reaches the reference (the game's alpha test keeps a texel whose alpha is at least the reference).
bool CityHitAlphaBlocks(float alpha, float reference)
{
    return alpha >= reference;
}

#endif  // MCLA_CITY_HIT_ALPHA_HLSLI
