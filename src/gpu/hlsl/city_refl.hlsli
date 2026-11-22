// MCLA GPU renderer -- the city in the car paint's reflections: the shader side (CITY-RT M2 packages R1 and R2; src/gpu/city_refl_rules.h has the design, the numbers and the tests, src/gpu/city_refl.h
// the host).
//
// Included by hlsl/car_rt_refl_cs.hlsl (the player's pass and, through the same file, the opponents' variant) after car_rt_hit_shade.hlsli. A reflection ray whose mask has CAR_RT_MASK_CITY is traced against
// the cars and the city (CityHitTrace: the closest hit, the cut-outs' exact alpha in the candidate loop); where its closest hit is a city triangle, CityReflHit makes the colour with the city hit library
// (hlsl/city_hit_shade.hlsli: ResolveHit, "the picture where the hit is seen, shaded from its material where it is not") and the fade of the distance over the city's reach; the caller takes it as a
// hit whose colour is known in full (visible 1, seen the colour).
//
// SET 3 (the host's constants, city_refl.cpp): t0 the rows (city_refl_rules.h kRows, the host writes them every frame), u1 the counters (the calibration's three words a class, then the census:
// kCityStat*). The push block moved to space 4 with this set.
//
// THE PICTURE. lin_depth level 0 (a bindless slot: the vehicle's mark in the lowest bit of a float), colour_post's mip chain (one slot a level): a hit whose point is in the picture takes the picture's
// colour at the mip of its footprint, trilinear by hand (the heap's textures are read with Load: there is no sampler in the layout).
//
// THE LAMPS (CITY-RT R7's lamp term and glow factor, REFL-LAMPS): a shaded city hit is lit by the world frame's lamps and what glows glows by the world's schedule, unless the host's row says no (kCityFlagLamps,
// mcla_gpu_city_refl_lamps) or the frame's world rows are not valid (then the hit is lit as it was: the sun's elevation's schedule, no lamp light). The world frame is t10 (the rows) and t11 (the lamps) of the car
// scene's set, which this pass's layout already carries (space CAR_RT_SPACE: car_rt_scene.cpp's set has 12 views), so this file binds nothing new. The library's lamp loop is CITY_HIT_LAMP_GRID: the lamps of the 64 m
// cells around the hit in view space (the top level's), 96 at most, their light's luminance capped at 6, no shadow ray (CITY_HIT_P_LAMP_SHADOW is not set: the mirror's ray is not denoised); the glow of a lamp lens
// follows the street lamps' schedule, neon, signs and lit windows the fill lights', a traffic signal is always on (CITY_HIT_GLOW_FACTOR = CityReflGlowFactor). The lamp term's counters (the library's census) are
// words kCityStatLampCensus.. of the city's counter buffer. `#define CITY_REFL_NO_LAMPS` before including leaves all of this out: the file is then what it was before REFL-LAMPS, line for line (the identity
// check, tools/check_rt_hit_identity.py, compiles it that way against the parent's).
//
// CITY-M2-OPEN: THE LIGHT. A wall the paint reflects is lit by the WORLD frame's light (t10's rows 0 to 31: the sun and its direction, the ambient, the sky and the environment map of the main view's draws), when
// the host's row says so (kCityFlagWorldLight, mcla_gpu_city_refl_world_light) and the world rows are valid and hold a light; else by the constants of the cars' draws, as before (the pass's own frame rows, whose sun
// direction is a rig of the cars' own: car_shadow_map.h). The census line `[city refl] light` says how the shaded hits fared (sun reached, blocked, facing away, behind the camera, world light taken).
//
// CHECK (nothing was run): the units (the frame's colours are the paint's; p.units 2^exp_bias makes the shaded colour the picture's: the same factor as the history's), the world rows' lamp factor in
// this pass (rt_world_frame.h: the lamps are in the VIEW space of the top level, which is the space the hits are in), no haze (CITY_HIT_HAZE is not wired), the wetness 0, the registers of set 3 on both APIs
// (t0 and u1: distinct numbers in one set, as gi_field's), t10 and t11 in space 2 on both APIs.

#ifndef MCLA_CITY_REFL_HLSLI
#define MCLA_CITY_REFL_HLSLI

#define CAR_RT_MASK_CITY 0x80

ByteAddressBuffer g_city : register(t0, space3);
RWByteAddressBuffer g_cityStats : register(u1, space3);

// the rows (city_refl_rules.h)
static const uint kCityRowControl = 0u;
static const uint kCityRowDepth = 1u;
static const uint kCityRowColour = 2u;
static const uint kCityRowSlotsA = 3u;
static const uint kCityRowSlotsB = 4u;
static const uint kCityRowReach = 5u;
static const uint kCityRowScreen = 6u;
static const uint kCityRowAlbedo = 7u;
static const uint kCityRowGains = 8u;
static const uint kCityFlagOn = 1u;
static const uint kCityFlagScreen = 2u;
static const uint kCityFlagStats = 4u;
static const uint kCityFlagCoat = 8u;
static const uint kCityFlagLamps = 16u;
static const uint kCityFlagBlueNoise = 32u;   // the hits' sun and lamp numbers are the blue noise tile's (city_refl_noise.hlsli; city_refl_rules.h kFlagBlueNoise)
static const uint kCityFlagWorldLight = 64u;   // (bit 5, 32, is kCityFlagBlueNoise's)
// the counters (city_refl_rules.h kStat*: the calibration's 48 words come first)
static const uint kCityStatTexels = 48u;
static const uint kCityStatRays = 49u;
static const uint kCityStatCity = 50u;
static const uint kCityStatCar = 51u;
static const uint kCityStatMiss = 52u;
static const uint kCityStatScreen = 53u;
static const uint kCityStatShaded = 54u;
static const uint kCityStatLamps = 55u;
static const uint kCityStatNoData = 56u;
static const uint kCityStatSunLit = 57u;       // CITY-M2-OPEN: the shaded hits' light (city_refl_rules.h kStatSunLit ...)
static const uint kCityStatSunBlocked = 58u;
static const uint kCityStatSunAway = 59u;
static const uint kCityStatBehind = 60u;
static const uint kCityStatWorldFrame = 61u;
static const uint kCityStatLodHit = 62u;
static const uint kCityStatFlat = 63u;
static const uint kCityStatLampCensus = 64u;   // eight words: the library's CITY_HIT_CENSUS_* (the lamp term's and the glow's counters)
static const uint kCityClassEmissive = 13u;
static const float kCityNightFull = 3.0;
static const float kCityNightNone = 12.0;
static const float kCityRadToDeg = 57.29577951308232;
static const float kCityMinFade = 1.0 / 512.0;

struct CityReflFrame
{
    uint flags;
    uint level;
    uint serial;
    uint params;
    uint depthSlot;
    uint depthWidth;
    uint depthHeight;
    uint colourLevels;
    uint colourWidth;
    uint colourHeight;
    uint4 slotsA;
    uint4 slotsB;
    float reach;
    float fadeFrom;
    float sunTan;
    float sunReach;
    float4 screen;   // tolerance in metres, its share of the depth, the edge band, the least facing
    float aoMix;
    float lodBias;
    float wet;
    int view;
};

static CityReflFrame gCity;
static bool gCityLoaded = false;
static uint2 gCityTexel = uint2(0u, 0u);   // the texel being traced: the noise of the hit's sun ray and lamp choice
static uint gCityHits = 0u;               // the city hits shaded in the texel so far: the hit's number in the cone (city_refl_noise.hlsli)

CityReflFrame CityReflLoad()
{
    CityReflFrame c;
    const uint4 r0 = g_city.Load4(kCityRowControl * 16u);
    const uint4 r1 = g_city.Load4(kCityRowDepth * 16u);
    const uint4 r2 = g_city.Load4(kCityRowColour * 16u);
    const uint4 r5 = g_city.Load4(kCityRowReach * 16u);
    const uint4 r6 = g_city.Load4(kCityRowScreen * 16u);
    const uint4 r7 = g_city.Load4(kCityRowAlbedo * 16u);
    c.flags = r0.x;
    c.level = r0.y;
    c.serial = r0.z;
    c.params = r0.w;
    c.depthSlot = r1.x;
    c.depthWidth = r1.y;
    c.depthHeight = r1.z;
    c.colourLevels = r1.w;
    c.colourWidth = r2.x;
    c.colourHeight = r2.y;
    c.slotsA = g_city.Load4(kCityRowSlotsA * 16u);
    c.slotsB = g_city.Load4(kCityRowSlotsB * 16u);
    c.reach = asfloat(r5.x);
    c.fadeFrom = asfloat(r5.y);
    c.sunTan = asfloat(r5.z);
    c.sunReach = asfloat(r5.w);
    c.screen = asfloat(r6);
    c.aoMix = asfloat(r7.x);
    c.lodBias = asfloat(r7.y);
    c.wet = asfloat(r7.z);
    c.view = int(r7.w);
    return c;
}

// The frame's constants, read once for the thread.
CityReflFrame CityReflFrameNow()
{
    if (!gCityLoaded)
    {
        gCity = CityReflLoad();
        gCityLoaded = true;
    }
    return gCity;
}

// The pass traces the city this frame (the push's parts have the bit and the host's rows say on).
bool CityReflOn(uint parts)
{
    return (parts & CAR_RT_MASK_CITY) != 0u && (CityReflFrameNow().flags & kCityFlagOn) != 0u;
}

bool CityReflStatsOn()
{
    return (CityReflFrameNow().flags & kCityFlagStats) != 0u;
}

void CityStat(uint word, uint value)
{
    if (value != 0u)
    {
        uint original;
        g_cityStats.InterlockedAdd(word * 4u, value, original);
    }
}

// ---- the picture (the screen shortcut of ResolveHit) --------------------------------------------------------------------------------------------

float CityReflDepth(int2 texel)
{
    const CityReflFrame c = CityReflFrameNow();
    return g_textures[NonUniformResourceIndex(c.depthSlot)].Load(int3(texel, 0)).x;
}

// A vehicle's pixel (the mark in the depth's lowest bit) is not the answer for a ray from a vehicle's paint.
uint CityReflReject(int2 texel)
{
    return (asuint(CityReflDepth(texel)) & 1u) != 0u ? 1u : 0u;
}

uint CityReflColourSlot(CityReflFrame c, uint level)
{
    return level < 4u ? c.slotsA[level] : c.slotsB[level - 4u];
}

// The size of mip `level` for a level 0 of `size`: halved with the odd edge rounded up (city_refl_rules.h LevelSize).
uint CityReflLevelSize(uint size, uint level)
{
    return max(1u, (size + (1u << level) - 1u) >> level);
}

// One level of colour_post, bilinear by hand (texel centres at +0.5), clamped to the edge.
float3 CityReflColourLevel(CityReflFrame c, uint level, float2 uv)
{
    const int2 size = int2(CityReflLevelSize(c.colourWidth, level), CityReflLevelSize(c.colourHeight, level));
    const float2 f = uv * float2(size) - 0.5;
    const int2 base = int2(floor(f));
    const float2 fr = f - float2(base);
    const int2 last = size - 1;
    const uint slot = CityReflColourSlot(c, level);
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const int2 offset = int2(k & 1, k >> 1);
        const int2 texel = clamp(base + offset, int2(0, 0), last);
        const float w = (offset.x != 0 ? fr.x : 1.0 - fr.x) * (offset.y != 0 ? fr.y : 1.0 - fr.y);
        sum += w * g_textures[NonUniformResourceIndex(slot)].Load(int3(texel, 0)).rgb;
    }
    return sum;
}

// The picture's colour at uv (0..1, y down) at mip level `mip` (the hit's footprint in pixels, log2), trilinear between two levels of the chain.
float3 CityReflColour(float2 uv, float mip)
{
    const CityReflFrame c = CityReflFrameNow();
    const float m = clamp(mip, 0.0, float(max(c.colourLevels, 1u) - 1u));
    const uint lo = uint(floor(m));
    const uint hi = min(lo + 1u, max(c.colourLevels, 1u) - 1u);
    float3 colour = CityReflColourLevel(c, lo, uv);
    if (hi != lo)
    {
        colour = lerp(colour, CityReflColourLevel(c, hi, uv), frac(m));
    }
    return max(colour, 0.0);
}

// The class's gain (the calibration's: city_refl.cpp keeps rt_hit_calibration.h ClassGains and writes the table into rows 8 to 11).
float CityReflGain(uint cls)
{
    return asfloat(g_city.Load((kCityRowGains * 16u) + min(cls, 15u) * 4u));
}

void CityReflCalAdd(uint word, uint value)
{
    CityStat(word, value);
}

#ifndef CITY_REFL_NO_LAMPS
// ---- the world frame's lamps and glow (CITY-RT R7, REFL-LAMPS) ----------------------------------------------------------------------------------

// t10 the world rows and t11 the lamps of the car scene's set (rt_world_frame.hlsli declares them in space RT_WORLD_SPACE); the hit frame stays the car's own (RT_HIT_FRAME_ROW is gCarRtMaterials, set by
// car_rt_hit_shade.hlsli before this file), the world frame's rows are read for the lamps, the schedule and the glow only.
#define RT_WORLD_SPACE CAR_RT_SPACE
#include "rt_world_frame.hlsli"
#include "city_refl_noise.hlsli"

// What a glowing material glows by while this hit is shaded: the world rows' schedule (gCityGlowWorld), or the sun's elevation's (gCityGlowOld, the lamp factor of old).
static bool gCityGlowWorld = false;
static float gCityGlowOld = 0.0;

float CityReflGlowFactor(uint kind)
{
    if (kind == RT_WORLD_GLOW_ALWAYS)
    {
        return 1.0;
    }
    return gCityGlowWorld ? RtWorldGlowFactor(kind) : gCityGlowOld;
}

void CityReflLampCensus(uint word, uint value)
{
    CityStat(kCityStatLampCensus + word, value);
}
#endif

// The library's hooks (hlsl/city_hit_shade.hlsli lists them): the picture, the vehicle rule, the class gains, the calibration's counters, and (REFL-LAMPS) the world frame's lamp grid, glow schedule and the lamp
// term's counters. No haze, no probe field.
#define CITY_HIT_SPACE CAR_RT_SPACE
#define CITY_HIT_SCREEN_DEPTH(texel) CityReflDepth(texel)
#define CITY_HIT_SCREEN_COLOUR(uv, mip) CityReflColour(uv, mip)
#define CITY_HIT_SCREEN_REJECT(texel) CityReflReject(texel)
#define CITY_HIT_GAIN(cls) CityReflGain(cls)
#define CITY_HIT_STAT_ADD(word, value) CityReflCalAdd(word, value)
#ifndef CITY_REFL_NO_LAMPS
#define CITY_HIT_LAMP_GRID
#define CITY_HIT_GLOW_FACTOR(kind) CityReflGlowFactor(kind)
#define CITY_HIT_CENSUS_ADD(word, value) CityReflLampCensus(word, value)
#endif
#include "city_hit_shade.hlsli"

// ---- the lamp schedule ----------------------------------------------------------------------------------------------------------------------

// 1 at night, 0 by day, from the sun's elevation (city_refl_rules.h LampSchedule: the headlight rig's night curve); 0 where the frame has no sun direction.
float CityReflLampSchedule(RtHitFrame f)
{
    if ((f.flags & RT_HIT_FRAME_SUN) == 0u)
    {
        return 0.0;
    }
    const float degrees = asin(clamp(dot(f.toSun, f.axisY), -1.0, 1.0)) * kCityRadToDeg;
    const float t = saturate((degrees - kCityNightFull) / (kCityNightNone - kCityNightFull));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

// The weight of a city hit by its distance: 1 up to the fade's start, a smoothstep to 0 at the reach (city_refl_rules.h CityFade).
float CityReflFade(CityReflFrame c, float distance)
{
    const float lo = c.fadeFrom * c.reach;
    const float t = saturate((distance - lo) / max(c.reach - lo, 1e-6));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

// What a ray whose closest hit is a CITY triangle shows (the library's ResolveHit). `origin`, `direction` (unit) and the hit `h` as CityHitTrace gave them; `coneTan` the tangent of the ray's cone (0: a
// mirror). False: nothing to show (the hit's data is not in the pool, or the hit is past the reach): the caller treats the ray as a miss and the paint keeps the environment map. True: `colour`
// in the history's units and `fade` the weight of the distance. The census (a stats frame only) counts what became of it.
bool CityReflHit(float3 origin, float3 direction, CityHitTraceResult h, float coneTan, out float3 colour, out float fade)
{
    colour = float3(0.0, 0.0, 0.0);
    const CityReflFrame c = CityReflFrameNow();
    fade = CityReflFade(c, h.t);
    if (fade < kCityMinFade)
    {
        return false;  // beyond the reach
    }
    const bool stats = (c.flags & kCityFlagStats) != 0u;
#ifdef CITY_REFL_NO_LAMPS
    const RtHitFrame f = RtHitLoadFrame();
#else
    RtHitFrame f = RtHitLoadFrame();
    bool worldFrame = false;
    // CITY-M2-OPEN (city_refl_rules.h WorldLightFor): a wall of the city is lit by the WORLD's light, not by the constants of the cars' draws (the pass's own frame rows). The world frame's rows 0 to 31 are the
    // hit frame of the main view's draws (the sun and its direction, the ambient, the sky, the environment map), filled in every frame with a top level; used when the host's switch says so, the world rows are
    // valid and the world's frame holds a light and its sun's direction. Else the cars' frame, as before.
    if ((c.flags & kCityFlagWorldLight) != 0u && (gRtWorld[RT_WORLD_ROW_STATUS].y & RT_WORLD_VALID) != 0u)
    {
        const RtHitFrame w = RtWorldHitFrame();
        if ((w.flags & (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)) == (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN))
        {
            f = w;
            worldFrame = true;
        }
    }
#endif
    CityHitParams p;
    p.mode = CITY_HIT_MODE_REFLECT;
    p.flags = c.params;
    p.units = g_push.up.w;
    p.sunReach = c.sunReach;
    p.sunTan = c.sunTan;
#ifdef CITY_REFL_NO_LAMPS
    p.lampFactor = CityReflLampSchedule(f);
#else
    // REFL-LAMPS (city_refl_rules.h LampTermFor, LampFlags, GlowFactorFor): the world frame's lamp factor, lamp grid and glow schedule when the host's row says so and the frame's world rows are valid (the status row's
    // flags and factor: one load); else the sun's elevation's schedule as of old, no lamp light.
    float lampFactor = 0.0;
    bool lampsWorld = false;
    if ((c.flags & kCityFlagLamps) != 0u)
    {
        const uint4 status = gRtWorld[RT_WORLD_ROW_STATUS];
        lampsWorld = (status.y & RT_WORLD_VALID) != 0u;
        lampFactor = asfloat(status.w);
    }
    if (!lampsWorld)
    {
        lampFactor = CityReflLampSchedule(f);
        p.flags = p.flags & ~CITY_HIT_P_LAMPS;
    }
    gCityGlowWorld = lampsWorld;
    gCityGlowOld = lampFactor;
    p.lampFactor = lampFactor;
#endif
    p.wet = c.wet;
    p.aoMix = c.aoMix;
    p.lodBias = c.lodBias;
    p.bounce = 0.0;
    p.lampFirst = 0u;
    p.lampCount = 0u;
    const float2 noise = CityReflNoise(int(gCityTexel.x), int(gCityTexel.y), c.serial, min(gCityHits++, kCityReflRaysPerFrame - 1u), direction, (c.flags & kCityFlagBlueNoise) != 0u);
    p.noise = float3(noise.x, noise.y, frac(noise.x + noise.y * 1.618));
    CityHitScreen s;
    s.lens = float4(g_push.lens.x, g_push.lens.y, float(c.depthWidth), float(c.depthHeight));
    s.tolerance = c.screen;
    s.cap = 0.0;
    if ((c.flags & kCityFlagScreen) == 0u)
    {
        p.flags = p.flags & ~CITY_HIT_P_SCREEN;
    }
    CityHitRay ray;
    ray.origin = origin;
    ray.direction = direction;
    ray.t = h.t;
    // the width of the ray where it hits: the cone's and the history texel's angular size along the path (city_refl_rules.h HitFootprint)
    ray.footprint = 2.0 * h.t * coneTan + PixelMeters(h.t, g_push.lens.y, g_push.image.y);
    const CityHitResolved r = CityHitResolve(p, f, s, ray, h.inst);
    if (r.source == CITY_HIT_SOURCE_NONE)
    {
        if (stats)
        {
            CityStat(kCityStatNoData, 1u);
        }
        return false;  // no data for this hit: a miss, the environment stays
    }
    if (stats)
    {
        CityStat(r.screen >= 0.5 ? kCityStatScreen : kCityStatShaded, 1u);
        CityStat(kCityStatLamps, r.cls == kCityClassEmissive ? 1u : 0u);
#ifndef CITY_REFL_NO_LAMPS
        // CITY-M2-OPEN: the hit's detail (the far LOD's geometry has no texture coordinates: its albedo is the material's average colour) ...
        const bool lodHit = (h.inst.instanceId & 1u) != 0u;
        CityStat(kCityStatLodHit, lodHit ? 1u : 0u);
        if (r.source != CITY_HIT_SOURCE_SCREEN)
        {
            // ... the model shaded this hit (alone or in a blend): was its sun there? facing the sun and reached, facing the sun and blocked, or facing away; and the frame it was lit by; and was its albedo a
            // texture's or a flat average (the test of CityHitAlbedo: a resident texture, texture coordinates and corners in the sector)
            CityStat(r.ndl > 0.0 ? (r.visible > 0.0 ? kCityStatSunLit : kCityStatSunBlocked) : kCityStatSunAway, 1u);
            CityStat(kCityStatBehind, (origin.z + direction.z * h.t) < 0.0 ? 1u : 0u);
            CityStat(kCityStatWorldFrame, worldFrame ? 1u : 0u);
            const CityHit decoded = CityHitDecode(h.inst.instanceId, h.inst.geometryIndex, h.inst.primitiveIndex, h.inst.barycentrics);
            const CityHitTexture hitTexture = CityHitLoadTexture(CityHitLoadMaterial(decoded.material).textureIndex);
            const bool textured = hitTexture.slot != CITY_HIT_NO_SLOT && hitTexture.mipCount != 0u && (decoded.runFlags & CITY_HIT_RUN_HAS_UV) != 0u &&
                                  (decoded.sectorFlags & (CITY_HIT_SECTOR_RUNS | CITY_HIT_SECTOR_CORNERS)) == (CITY_HIT_SECTOR_RUNS | CITY_HIT_SECTOR_CORNERS);
            CityStat(kCityStatFlat, textured ? 0u : 1u);
        }
#endif
    }
    colour = r.colour;
    if (c.view == 1)
    {
        // the source: screen green, shaded red, both yellow (brightness: in the picture's units)
        colour = (r.source == CITY_HIT_SOURCE_SCREEN ? float3(0.0, 1.0, 0.0) : (r.source == CITY_HIT_SOURCE_SHADED ? float3(1.0, 0.0, 0.0) : float3(1.0, 1.0, 0.0))) * g_push.up.w * 0.25;
    }
    else if (c.view == 2)
    {
        // the class by a ramp
        const float v = float(r.cls) / 15.0;
        colour = float3(saturate(1.5 - abs(v * 4.0 - 3.0)), saturate(1.5 - abs(v * 4.0 - 2.0)), saturate(1.5 - abs(v * 4.0 - 1.0))) * g_push.up.w * 0.25;
    }
    return true;
}

#endif  // MCLA_CITY_REFL_HLSLI
