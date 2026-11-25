// MCLA GPU renderer -- the city hit library: what a ray that hit the city SHOWS (CITY-RT M2 package H4; the C++ twin is src/gpu/city_hit_shade_rules.h).
//
// THE GOAL (the owner's, 2026-11-20): in a reflection you can clearly identify what you see. A hit on the city is a wall, a road, a lamp head, a window, a sign: this file turns the hit data
// (hlsl/city_hit_common.hlsli: the decode of the pool, the material, the texture) and the frame's light (hlsl/rt_hit_frame.hlsli: the sun, the ambient, the environment map, the cascades;
// the car hit shading's own frame, so a reflected wall and a reflected car sit in the same light) into a colour. One library for every consumer: the player's paint and the other cars'
// (R1, R2: REFLECT mode, a mirror ray's hit), the SSR fallback and the glass (R3, R4), and the GI's probes and per-pixel rays (G2, G5: GI mode).
//
//   CityHitTrace      a closest hit of a ray in the top level, the cut-outs' exact alpha in the candidate loop (hlsl/city_hit_alpha.hlsli), the city's hit or a car's (cars are for their own shading)
//   CityHitShade      the colour of a decoded hit: the albedo (REFLECT: the texture at the level of the ray cone's footprint, tinted, the baked occlusion in it; GI: the average colour), the light
//                     (the frame's ambient, the sun by N.L and its visibility, one lamp of the nearest by importance, the GI field's feedback), the class's own way (a wet road darker, glass
//                     and metal and water take the sky by Schlick, a lamp lens or a neon sign glows by the lamp schedule), in the picture's units
//   CityHitResolve    ResolveHit of the plan (CITY-RT-M2 section 4): "the screen where it is seen, shaded where it is not": a hit whose point is in the picture takes that pixel's lit colour (the
//                     game's own picture: the lights, the foliage, the particles, the exposure), a hit that is off screen or hidden is SHADED; the two blend where the picture's claim fades
//                     (the edge, the depth that does not agree, a surface that faces away). The shaded colour is multiplied by its class's GAIN, measured against the picture where both exist (the
//                     CALIBRATION HOOK: rt_hit_calibration.h, one gain a class), so the model's exposure, sun and ambient follow the game's
//
// THE INCLUDER defines, before including this file (every name that starts with CITY_HIT_ is the library's; the includer's own are listed):
//   CAR_RT_SPACE / CITY_HIT_SPACE   the register space of the ray tracing descriptor set (city_hit_common.hlsli: t6..t9 are the pool and the tables)
//   RT_HIT_FRAME_ROW(i), RT_HIT_TLAS, `Texture2D<float4> g_textures[]`   as rt_hit_common.hlsli says
//   CITY_HIT_ALPHA_TEXEL(slot, texel, level)    optional: defaults to a Load on g_textures (city_hit_alpha.hlsli)
//   CITY_HIT_SCREEN_DEPTH(texel)                the picture's linear view depth at an int2 pixel (the lin_depth image: R32_FLOAT, 60000 for the sky); with CITY_HIT_SCREEN_COLOUR(uv, mip)
//                                               the picture's lit colour at a float2 uv in 0..1 (y down) and a mip level (colour_post's chain); both or neither (without them ResolveHit
//                                               shades everything)
//   CITY_HIT_SCREEN_REJECT(texel)               optional: nonzero for a pixel that must not be the answer (a vehicle's pixel for a ray from that vehicle's paint: the self-hit rule)
//   CITY_HIT_GAIN(cls)                          optional: the class's gain (a float; 1 without)
//   CITY_HIT_STAT_ADD(word, value)              optional: the calibration's counters (a uint word index and a uint to add: rt_hit_calibration.h StatsWord); counted only in a stats frame
//   CITY_HIT_LAMP_LOAD(i)                       optional: lamp i of the frame's list as a CityHitLamp (view space); with it the lamp light of a hit is made, over the range the params give
//   CITY_HIT_LAMP_GRID                          optional (CITY-RT R7): the lamp light of a hit is made from the WORLD FRAME's lamps (hlsl/rt_world_frame.hlsli, included BEFORE this file: its RtWorld, RtWorldLoad,
//                                               RtLampBegin, RtLampStep, RtLampRow): the lamps of the 64 m cells around the hit, at most kCityHitLampMax of those that reach it, the sum capped at kCityHitLampSumCap;
//                                               the params' lampFirst and lampCount are not used (the records hold each lamp's activity over p.lampFactor = the world rows' schedule). Wins over CITY_HIT_LAMP_LOAD.
//   CITY_HIT_GLOW_FACTOR(kind)                  optional (R7): what a glowing material of this kind (CITY_HIT_GLOW_*) glows by, 0..1: the world frame's is RtWorldGlowFactor (a lamp lens by the street lamps'
//                                               schedule, neon, signs and lit windows by the fill lights', a traffic signal always); without it kind ALWAYS is 1 and everything else saturate(p.lampFactor)
//   CITY_HIT_CENSUS_ADD(word, value)            optional (R7): the lamp term's and the glow's counters (CITY_HIT_CENSUS_*: words 0..7 of a counter buffer of the includer's choosing: place them beside the calibration's
//                                               or in a buffer of their own), counted only in a stats frame (CITY_HIT_P_CALIBRATE); src/gpu/rt_world_frame_rules.h LampCensus reads them
//   CITY_HIT_EMITTER_COUNT, CITY_HIT_EMITTER_RADIANCE(i, P, N), CITY_HIT_EMITTER_TO(i, P)
//                                               optional (GI-BOUNCE-RT, all three or none; hlsl/rt_emitters.hlsli makes them over the world frame's emitter rows): the lights that are not the world's own and light a hit
//                                               (a car's neon, headlights, tail lamps, police bars, a mod's lamps), by the same GpuLight laws as the light pass. COUNT is the number of emitters the includer reads (a uint
//                                               expression), RADIANCE(i, P, N) the unshadowed light emitter i sends to the view space point P (N the unit normal facing the viewer): a float3 in the paint's units, the law,
//                                               N.L and the colour in it, 0 where it does not reach; TO(i, P) the vector from P to emitter i in view space (its length is the distance), asked for the one emitter a
//                                               shadow ray goes to. With CITY_HIT_P_EMITTERS the sum of the emitters is added to the hit's light (CityHitEmitters)
//   CITY_HIT_AMBIENT(P, N, ambient)             optional: replaces the frame's hemisphere ambient at a point with something better (the GI's probe field), given the default
//   CITY_HIT_FEEDBACK(P, N)                     optional: the GI field's irradiance at a point, a float3, for the hit's bounce (the second bounce of a probe ray)
//   CITY_HIT_HAZE(colour, distance)             optional: the air between the camera and the hit (the game's distance fog) on a shaded colour, given the colour (picture's units) and the distance in
//                                               metres; the picture's own colour has it already
//   CITY_HIT_SHADE_NO_RAYQUERY                  no ray queries (the test compiles this file as C++): the includer then declares CityHitSunRay, CityHitLampRay and CityHitEmitterRay itself
// src/gpu/city_hit_shade_rules_test.cpp compiles THIS FILE as C++ (through a shim of the HLSL types and the buffers, with CITY_HIT_SHADE_NO_RAYQUERY) and checks it against city_hit_shade_rules.h
// on thousands of random hits, and compares every number below with the header's; that is why the text keeps to a subset both languages read: scalar and vector arithmetic, no out
// parameters (a function returns a struct), the intrinsics abs sqrt saturate clamp min max lerp pow log2 exp2 floor frac dot cross normalize smoothstep, CITY_HIT_BRANCH for [branch].
//
// UNITS. The frame's colours (the sun, the ambient) are in the paint's units (a shader's output); the picture is in those times 2^exp_bias (CityHitParams::units, as the car pass's g_push.up.w).
// The shaded colour is in the picture's units. The frame's own gain row (the cars' calibration) is NOT applied: a hit on the city has the class gains.
//
// WHAT THE CONSUMERS STILL NEED (found while writing the library; none of it is in this package):
//   R1, R2   the car pass's history image holds only vehicle pixels, so the picture for ResolveHit is ScreenShared's colour_post and lin_depth (SS-1), read at the trigger; the player's pixels are
//            CITY_HIT_SCREEN_REJECT. CityHitTrace's mask is the city's bit with the cars' (CAR_RT_MASK_ALL | 0x80); a city hit is told from a car's by CityHitIsCity(InstanceID).
//   R4, G2   the frame's rows exist today only in a frame that has a car (car_rt_scene.cpp BuildMaterialRows: the light is read from the cars' own draws); the top level without a car
//            (OnResolveCityOnly) writes one empty row, so CityHitShade answers "no data" there. A pass of the world needs a frame of its own: the sun, the ambient, the environment map, the
//            cascades from the volumetric light or the light frame, written through rt_hit_frame.h MakeFrameRows. The lamp list (CITY_HIT_LAMP_LOAD) is G2b's table (src/citypack/lamp_table.h,
//            feat/city-rt-g2b) as city_hit_shade_rules.h WorldLamp / MakeGpuLamp, in view space each frame; its schedule is CityHitParams::lampFactor.
//   G2       GI mode with CITY_HIT_AMBIENT and CITY_HIT_FEEDBACK wired to the probe field, CITY_HIT_P_SUN_RAY | LAMPS | EMISSIVE, no screen.
//
// CHECK (nothing here was run against the game): the ambient's hemisphere term for a wall (the cars' 0.6 + 0.4 N.up; M2's 0.5 + 0.5), the vertex colour's rgb (small, ignored), the level's grazing
// factor, the sun's disc, the wet and glass models, the lamp laws' units against the picture; each has a number below, and the first census says which way to move it.

#ifndef MCLA_CITY_HIT_SHADE_HLSLI
#define MCLA_CITY_HIT_SHADE_HLSLI

#include "city_hit_common.hlsli"
#include "rt_hit_common.hlsli"
#include "nan_guard.hlsli"

#ifndef CITY_HIT_BRANCH
#define CITY_HIT_BRANCH [branch]
#endif
#ifndef CITY_HIT_LOOP
#define CITY_HIT_LOOP [loop]
#endif

// ---- the numbers (src/gpu/city_hit_shade_rules.h) ---------------------------------------------------------------------------------------------

#define CITY_HIT_MODE_REFLECT 0u
#define CITY_HIT_MODE_GI 1u

#define CITY_HIT_P_SUN_RAY 1u       // the hit's sun visibility by a ray toward the sun against the whole top level (the city has real shadows)
#define CITY_HIT_P_SUN_CASCADE 2u   // ... and/or by the game's cascades (where the frame has them): the darker of the two
#define CITY_HIT_P_LAMPS 4u         // the lamps' light at the hit (needs CITY_HIT_LAMP_LOAD and the params' range)
#define CITY_HIT_P_LAMP_SHADOW 8u   // the lamp's light is by one stochastic lamp with a shadow ray (else the sum of the lamps, no rays)
#define CITY_HIT_P_EMISSIVE 16u     // lamp lenses, neon and lit windows glow by the lamp schedule
#define CITY_HIT_P_CALIBRATE 32u    // this is a stats frame: the resolve adds its samples to the calibration's counters
#define CITY_HIT_P_SCREEN 64u       // the resolve may take the picture's colour
#define CITY_HIT_P_EMITTERS 128u    // GI-BOUNCE-RT: the emitters' light at the hit (needs CITY_HIT_EMITTER_RADIANCE): a car's neon, headlights, tail lamps ...
#define CITY_HIT_P_EMITTER_SHADOW 256u  // ... with ONE shadow ray against the city toward one emitter chosen by its brightness (else the sum, no ray)

#define CITY_HIT_SOURCE_NONE 0u     // no data: the caller keeps what it had (the environment map)
#define CITY_HIT_SOURCE_SCREEN 1u   // the picture's colour
#define CITY_HIT_SOURCE_SHADED 2u   // the model's
#define CITY_HIT_SOURCE_BLEND 3u    // both, by the picture's weight

// The G-buffer's classes (hlsl/gbuffer_common.hlsli GB_CLASS_*, gbuffer_types.h): the library reads the material word's class bits and needs these
#define CITY_HIT_CLASS_NONE 0u
#define CITY_HIT_CLASS_WORLD 1u
#define CITY_HIT_CLASS_ROAD 2u
#define CITY_HIT_CLASS_WET_ROAD 3u
#define CITY_HIT_CLASS_SIDEWALK 4u
#define CITY_HIT_CLASS_TERRAIN 5u
#define CITY_HIT_CLASS_LOD 6u
#define CITY_HIT_CLASS_FOLIAGE 7u
#define CITY_HIT_CLASS_PAINT 8u
#define CITY_HIT_CLASS_CHROME 9u
#define CITY_HIT_CLASS_METAL 10u
#define CITY_HIT_CLASS_GLASS 11u
#define CITY_HIT_CLASS_SKIN 12u
#define CITY_HIT_CLASS_EMISSIVE 13u
#define CITY_HIT_CLASS_INTERIOR 14u
#define CITY_HIT_CLASS_WATER 15u
#define CITY_HIT_CLASS_COUNT 16u

static const float kCityHitGrazingMin = 0.35;     // N.V below this reads the texture at the level of this (a ray that grazes a wall covers more of it than its width says)
static const float kCityHitSpecFrom = 0.3;        // a surface smoother than this reflects the sky: weight ((smoothness - from) / (1 - from))^2
static const float kCityHitEnvLodSmooth = 3.0;    // the environment map's level at smoothness 0 (the map has four levels, 0 to 3)
static const float kCityHitWetDarken = 0.4;       // a wet road's albedo is times 1 - this x the wetness
static const float kCityHitDielectricF0 = 0.04;
static const float kCityHitLiftFloor = 0.006;     // the sun ray starts this far off the surface along its normal (m) ...
static const float kCityHitLiftPerMetre = 0.003;  // ... plus this a metre of the hit's view distance ...
static const float kCityHitLiftCap = 0.4;         // ... at most this
static const float kCityHitSunReach = 2000.0;     // the sun ray's length where the params say 0 (m)
static const float kCityHitMinAbove = 0.08;       // a jittered sun ray is kept at least this far (as a cosine) above the surface's tangent plane
static const float kCityHitTwoPi = 6.283185307179586;
static const float kCityHitLampReach = 100.0;     // a lamp lights out to sqrt(this x its intensity): where its term is 1 percent (the game's laws, the mod loader's reach)
static const float kCityHitLampEnd = 0.3;         // the lamp's shadow ray stops this short of the lamp (m): its head is in the geometry
static const float kCityHitMinDepth = 0.05;       // a hit nearer than this (view z) is not in the picture
static const float kCityHitScreenTrust = 0.98;    // a hit with at least this weight of the picture is a calibration sample
static const float kCityHitCalFloor = 0.02;       // both luminances over this (rt_hit_calibration.h kCarParams.floor)
static const float kCityHitLogFloor = 0.0001;     // rt_hit_calibration.h: the fixed point log of a luminance
static const float kCityHitLogBias = 16.0;
static const float kCityHitLogScale = 64.0;
static const float kCityHitLogMax = 2047.0;
static const float kCityHitMaxColour = 60000.0;     // a hit's colour in the picture's units: what a half float keeps with room (the car reflections' kMaxColour)
static const uint kCityHitLampMax = 96u;           // R7: the lamps of the world frame's lookup that reach one hit and are examined at most (a 9 cell lookup holds 60 at night on average in the budget's densest place; the loop is bounded)
static const uint kCityHitLampWalkMax = 288u;      // R7: the lamps the walk visits at most, whether they reach the hit or not (3 x kCityHitLampMax: only the lamps that reach count against kCityHitLampMax, this bounds the time)
static const float kCityHitLampSumCap = 6.0;       // R7: the lamp light's luminance is capped at this (a lamp's term is at most 1: six lamps at full strength; a cluster of omni lights is not a sun)
static const float kCityHitLampLookup = 64.0;      // R7: the lookup's radius where the world rows give none (m)
static const uint kCityHitEmitterMax = 32u;        // GI-BOUNCE-RT: the emitters examined at most (the list's own limit, rt_emitters_rules.h kMaxEmitters)
static const float kCityHitEmitterSumCap = 8.0;    // ... the emitters' light's luminance is capped at this (a lamp is at most 2 at a metre: four cars' lamps, no more)
static const float kCityHitEmitterEnd = 0.3;       // ... the shadow ray stops this short of the emitter (m): a neon tube sits under its car, a lens in its housing
static const uint kCityHitCityMask = 0x80u;        // ... the ray sees the CITY only: the car that carries a light must not shadow it (its tube is inside the car's bounds), and cars are no occluders of the field's rays either

// R7: the lamp term's and the glow's counters (CITY_HIT_CENSUS_ADD word numbers)
#define CITY_HIT_CENSUS_LOOKUPS 0u      // hits whose lamp light was looked up
#define CITY_HIT_CENSUS_EXAMINED 1u     // lamps examined (the ones that reach the hit, the ones the cap counts), summed
#define CITY_HIT_CENSUS_LIT 2u          // ... of which the lamp lit the hit
#define CITY_HIT_CENSUS_CAP_COUNT 3u    // lookups that stopped at kCityHitLampMax reaching lamps or at kCityHitLampWalkMax lamps walked, with lamps left
#define CITY_HIT_CENSUS_CAP_SUM 4u      // lookups whose sum was over kCityHitLampSumCap
#define CITY_HIT_CENSUS_GLOW_NIGHT 5u   // hits on a glowing material, by what its glow follows (this word + the kind: 5 night, 6 lens, 7 always)
#define CITY_HIT_CENSUS_WORDS 8u

// Per class: the diffuse's share (glass and chrome let most of the light go through or off), the sky reflection's share. Indexed by the G-buffer's class (CITY_HIT_CLASS_*).
static const float kCityHitDiffuseGain[16] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.1, 0.5, 0.25, 1.0, 1.0, 1.0, 0.15};
static const float kCityHitSpecGain[16] = {0.0, 0.5, 1.0, 1.0, 1.0, 0.5, 0.5, 0.3, 1.0, 1.0, 1.0, 1.0, 0.5, 0.0, 0.5, 1.0};

// ---- the inputs and results -------------------------------------------------------------------------------------------------------------------

struct CityHitParams
{
    uint mode;        // CITY_HIT_MODE_*
    uint flags;       // CITY_HIT_P_*
    float units;      // 2^exp_bias: the frame's colours are in the paint's units, the picture's are these times that
    float sunReach;   // the sun ray's length in metres (0: kCityHitSunReach)
    float sunTan;     // tan of the sun disc's angular radius for the ray's jitter (0: a sharp ray)
    float lampFactor; // 0..1: the lamps', neon's and lit windows' schedule (0 by day, 1 at night); NOT the cars' night factor (it read 0.97 at 14 h and 23 h alike: CITY-RT-M2 R7)
    float wet;        // 0 dry .. 1 rain: a wet road is darker
    float aoMix;      // the share of the baked vertex occlusion in the albedo (0.5)
    float lodBias;    // levels added to the texture level
    float bounce;     // GI mode: the share of CITY_HIT_FEEDBACK added (0: none, 0.5: about two bounces)
    uint lampFirst;   // the range of the frame's lamp list the hit's lamps are taken from (CITY_HIT_LAMP_LOAD)
    uint lampCount;
    float3 noise;     // three numbers in 0..1: the sun disc's place (x, y) and the lamp's choice (z); a pass gives every ray its own
};

// The ray: view space, as the top level is.
struct CityHitRay
{
    float3 origin;
    float3 direction;   // unit
    float t;            // the distance to the hit along `direction`
    float footprint;    // the ray cone's width at the hit, metres (the pixel's width where the ray started + 2 tan(half angle) t)
};

// What the query committed: the instance id and the triangle, and the instance's 3x3 (the rows of CommittedObjectToWorld3x4: the pool's positions are the structure's own, the world's).
struct CityHitInstance
{
    uint instanceId;
    uint geometryIndex;
    uint primitiveIndex;
    float2 barycentrics;
    float3 row0;
    float3 row1;
    float3 row2;
};

struct CityHitShaded
{
    uint valid;       // 0: nothing was shaded (the hit's data is not in the pool, or the frame has no light): the caller keeps what it had
    uint cls;         // the material's G-buffer class
    float3 colour;    // in the picture's units, the class's gain NOT applied
    float3 albedo;    // the surface's colour (REFLECT: the texture's at the level read; GI: the average), with the occlusion and the wetness
    float3 normal;    // view space, unit, facing the ray
    float ndl;        // N.L of the sun
    float visible;    // the sun's visibility
    float glow;       // the emissive part's weight (0 where the surface does not glow)
};

struct CityHitScreen
{
    float4 lens;       // x, y: tan of half the horizontal and the vertical field of view; z, w: the depth image's width and height in pixels
    float4 tolerance;  // x: the depth tolerance in metres, y: and its share of the view depth, z: the edge band in ndc units, w: the facing cosine under which the pixel is not trusted
    float cap;         // GI: the picture's colour is capped at this luminance (0: not)
};

struct CityHitResolved
{
    uint source;       // CITY_HIT_SOURCE_*
    uint cls;
    float3 colour;     // in the picture's units
    float screen;      // the picture's weight, 0..1
    float3 normal;
    float ndl;
    float visible;
    float glow;
};

// ---- the ray's hit ----------------------------------------------------------------------------------------------------------------------------

bool CityHitIsCity(uint instanceId)
{
    return (instanceId & 0x800000u) != 0u;
}

float CityHitLuma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// The hit's unit normal in view space, turned to face the ray: the triangle's geometric normal in the structure's space through the instance's 3x3 (a rotation, possibly mirrored: a normal
// is carried by it as a vector up to the sign, and the sign is taken from the ray).
float3 CityHitViewNormal(CityHit h, CityHitInstance i, float3 direction)
{
    float3 n = float3(dot(i.row0, h.normal), dot(i.row1, h.normal), dot(i.row2, h.normal));
    const float l2 = dot(n, n);
    n = l2 > 1e-30 ? n * (1.0 / sqrt(l2)) : float3(0.0, 1.0, 0.0);
    return dot(n, direction) > 0.0 ? -n : n;
}

#ifndef CITY_HIT_SHADE_NO_RAYQUERY
#ifndef CITY_HIT_ALPHA_TEXEL
#define CITY_HIT_ALPHA_TEXEL(slot, texel, level) g_textures[NonUniformResourceIndex((slot) & 0x7FFFFFFFu)].Load(int3(texel, int(level))).w
#endif
#include "city_hit_alpha.hlsli"

struct CityHitTraceResult
{
    uint hit;
    float t;
    uint instanceIndex;   // InstanceIndex(): a car's instance row (the car scene's table)
    CityHitInstance inst;
};

CityHitTraceResult CityHitTraceNone()
{
    CityHitTraceResult r;
    r.hit = 0u;
    r.t = 0.0;
    r.instanceIndex = 0u;
    r.inst.instanceId = 0u;
    r.inst.geometryIndex = 0u;
    r.inst.primitiveIndex = 0u;
    r.inst.barycentrics = float2(0.0, 0.0);
    r.inst.row0 = float3(1.0, 0.0, 0.0);
    r.inst.row1 = float3(0.0, 1.0, 0.0);
    r.inst.row2 = float3(0.0, 0.0, 1.0);
    return r;
}

// The closest hit of the ray `origin` + t `direction` (t from tMin to tMax) against the instances whose mask meets `mask` (the city's bit and the cars' parts it should see). A cut-out of the city
// (the structure's second geometry: fences, grates, neon cut-outs) is a candidate: its texture's alpha at the candidate's own uv decides (a blocker at or above `reference`: the game's alpha
// test), read at the level of the ray cone there (`footprint0` the cone's width where the ray started, `tanHalf` the tangent of its half angle); where the alpha cannot be read (the texture is
// not resident yet, no corners) the candidate is opaque. Cars are built opaque.
CityHitTraceResult CityHitTrace(float3 origin, float3 direction, float tMin, float tMax, uint mask, float footprint0, float tanHalf, float reference)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(RT_HIT_TLAS, RAY_FLAG_NONE, mask, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            bool accept = true;
            const uint instance = query.CandidateInstanceID();
            if (CityHitIsCity(instance))
            {
                const float width = footprint0 + 2.0 * tanHalf * query.CandidateTriangleRayT();
                const CityHitAlpha a = CityHitAlphaOfCandidate(instance, query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), width);
                accept = a.known == 0u || CityHitAlphaBlocks(a.alpha, reference);
            }
            if (accept)
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }
    CityHitTraceResult r = CityHitTraceNone();
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        r.hit = 1u;
        r.t = query.CommittedRayT();
        r.instanceIndex = query.CommittedInstanceIndex();
        r.inst.instanceId = query.CommittedInstanceID();
        r.inst.geometryIndex = query.CommittedGeometryIndex();
        r.inst.primitiveIndex = query.CommittedPrimitiveIndex();
        r.inst.barycentrics = query.CommittedTriangleBarycentrics();
        const float3x4 m = query.CommittedObjectToWorld3x4();
        r.inst.row0 = m[0].xyz;
        r.inst.row1 = m[1].xyz;
        r.inst.row2 = m[2].xyz;
    }
    return r;
}

// One yes or no ray for the hit's light: does anything of the instances whose mask meets `mask` lie within tMax of origin along direction? The cut-outs' candidates are decided as CityHitTrace does.
bool CityHitBlockedBy(float3 origin, float3 direction, float tMax, float footprint0, float tanHalf, uint mask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(RT_HIT_TLAS, RAY_FLAG_NONE, mask, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            bool accept = true;
            const uint instance = query.CandidateInstanceID();
            if (CityHitIsCity(instance))
            {
                const float width = footprint0 + 2.0 * tanHalf * query.CandidateTriangleRayT();
                const CityHitAlpha a = CityHitAlphaOfCandidate(instance, query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), width);
                accept = a.known == 0u || CityHitAlphaBlocks(a.alpha, 0.5);
            }
            if (accept)
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The city and the cars (the sun's and the lamps' rays).
bool CityHitBlocked(float3 origin, float3 direction, float tMax, float footprint0, float tanHalf)
{
    return CityHitBlockedBy(origin, direction, tMax, footprint0, tanHalf, 0xFFu);
}

bool CityHitSunRay(float3 origin, float3 direction, float reach, float footprint, float sunTan)
{
    return CityHitBlocked(origin, direction, reach, footprint, sunTan);
}

bool CityHitLampRay(float3 origin, float3 direction, float reach, float footprint)
{
    return CityHitBlocked(origin, direction, reach, footprint, 0.0);
}

// The emitters' ray: the city alone (kCityHitCityMask).
bool CityHitEmitterRay(float3 origin, float3 direction, float reach, float footprint)
{
    return CityHitBlockedBy(origin, direction, reach, footprint, 0.0, kCityHitCityMask);
}
#endif  // CITY_HIT_SHADE_NO_RAYQUERY

// ---- the albedo ------------------------------------------------------------------------------------------------------------------------------

// The texture's level (counted from its full size) a ray cone of `footprint` metres reads at a hit whose triangle repeats the texture `density` times a metre (CityHitUvDensity), seen at N.V `nv`:
// log2 of the texels the footprint covers (the geometric mean of the sides), a grazing view covering more (nv is not taken below kCityHitGrazingMin), by the bias.
float CityHitTextureLevel(float footprint, float density, uint width, uint height, float nv, float bias)
{
    const float texels = footprint * density * sqrt(float(width) * float(height)) / max(nv, kCityHitGrazingMin);
    return log2(max(texels, 1e-4)) + bias;
}

// A texel as the material's albedo reads it: the colour, or for a road's packed diffuse (greyInRed: (grey, specular mask, nothing)) its red channel as a grey. Read as a colour that texture is green
// or orange (RT-GREEN-BAND). city_hit_shade_rules.h TexelAlbedo is the twin.
float3 CityHitTexelAlbedo(CityHitMaterial m, float3 texel)
{
    return m.greyInRed ? float3(texel.x, texel.x, texel.x) : texel;
}

// The surface's albedo. REFLECT with a resident texture: the texture at the level of the footprint, bilinear in two levels, wrap, the game's gamma where the slot says, times the tint; beyond the
// last stored level it fades to the average colour (a cone wider than the whole texture sees the mean). GI mode, no texture or not resident yet: the average colour. The baked vertex occlusion (the
// corner colour's alpha) is mixed in by p.aoMix.
float3 CityHitAlbedo(CityHitParams p, CityHit h, CityHitMaterial m, CityHitTexture t, float footprint, float nv)
{
    float3 albedo = m.average;
    CITY_HIT_BRANCH
    if (p.mode == CITY_HIT_MODE_REFLECT && t.slot != CITY_HIT_NO_SLOT && t.mipCount != 0u && (h.runFlags & CITY_HIT_RUN_HAS_UV) != 0u &&
        (h.sectorFlags & (CITY_HIT_SECTOR_RUNS | CITY_HIT_SECTOR_CORNERS)) == (CITY_HIT_SECTOR_RUNS | CITY_HIT_SECTOR_CORNERS))
    {
        const float level = CityHitTextureLevel(footprint, CityHitUvDensity(h), t.width, t.height, nv, p.lodBias);
        const float last = float(t.firstMip + t.mipCount - 1u);
        const float3 texel = CityHitTexelAlbedo(m, RtHitSample(t.slot, h.uv, level - float(t.firstMip), true).rgb);
        albedo = lerp(texel * m.tint, m.average, saturate(level - last));
    }
    if ((h.runFlags & CITY_HIT_RUN_HAS_COLOUR) != 0u)
    {
        albedo = albedo * lerp(1.0, h.colour.w, p.aoMix);
    }
    return albedo;
}

// ---- the light --------------------------------------------------------------------------------------------------------------------------------

// The orthonormal frame about a unit vector (Duff et al. 2017), as city_rt_shadow_cs.hlsl's Frame, as two vectors.
struct CityHitBasis
{
    float3 t;
    float3 b;
};

CityHitBasis CityHitMakeBasis(float3 n)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    CityHitBasis r;
    r.t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    r.b = float3(c, s + n.y * n.y * a, -n.y);
    return r;
}

// The place on the sun's disc for one ray (city_rt_shadow_rules.h DiscSample with one ray): the disc's area, turned by the noise.
float2 CityHitDiscSample(float noise1, float noise2)
{
    const float r = sqrt(min(noise1, 0.9999));
    const float angle = kCityHitTwoPi * frac(noise2);
    return float2(r * cos(angle), r * sin(angle));
}

// The direction of the sun ray: toward the sun, to a place on its disc (tan of its radius `sunTan`), kept above the surface's tangent plane.
float3 CityHitSunDirection(float3 toSun, float3 normal, float sunTan, float2 noise)
{
    float3 direction = toSun;
    CITY_HIT_BRANCH
    if (sunTan > 0.0)
    {
        const CityHitBasis basis = CityHitMakeBasis(toSun);
        const float2 d = CityHitDiscSample(noise.x, noise.y);
        direction = normalize(toSun + basis.t * (d.x * sunTan) + basis.b * (d.y * sunTan));
        const float above = dot(direction, normal);
        if (above < kCityHitMinAbove)
        {
            direction = normalize(direction + (kCityHitMinAbove - above) * normal);
        }
    }
    return direction;
}

// How far off the surface along its normal a ray for the hit's light starts. The distance is the hit's view z, which is NEGATIVE for a hit behind the camera (the reflection of the street behind a
// chase camera): its magnitude counts, else the lift goes below zero past 2 m and the origin sits under the surface, so the sun ray meets the surface it left and the hit is in shadow.
float CityHitLift(float viewDistance)
{
    return min(kCityHitLiftFloor + kCityHitLiftPerMetre * abs(viewDistance), kCityHitLiftCap);
}

// The sun's visibility at a hit (1 lit, 0 shadow): the game's cascades (where the params and the frame have them) and/or one ray toward the sun against the whole top level (the buildings' and the
// cars' shadows on the street: the city has real shadows). The darker of the two.
float CityHitSunVisible(CityHitParams p, RtHitFrame f, float3 hit, float3 normal, float3 toSun, float footprint)
{
    float visible = 1.0;
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_SUN_CASCADE) != 0u && (f.flags & RT_HIT_FRAME_CASCADE) != 0u)
    {
        visible = RtHitCascadeVisible(f, hit, normal);
    }
    CITY_HIT_BRANCH
    if (visible > 0.0 && (p.flags & CITY_HIT_P_SUN_RAY) != 0u)
    {
        const float3 origin = hit + normal * CityHitLift(hit.z);
        const float3 direction = CityHitSunDirection(toSun, normal, p.sunTan, float2(p.noise.x, p.noise.y));
        const float reach = p.sunReach > 0.0 ? p.sunReach : kCityHitSunReach;
        visible = min(visible, CityHitSunRay(origin, direction, reach, footprint, p.sunTan) ? 0.0 : 1.0);
    }
    return visible;
}

// A lamp of the frame's list, in view space, in the game's units (src/citypack/lamp_table.h: the intensity 20 a fill light and 55 to 70 a street lamp; the cone as the cosines of its two
// edges, LampConeCosines; cosOuter below -1.5 for a light with no cone). The loader (CITY_HIT_LAMP_LOAD) converts the world's lamp into the frame's view space.
struct CityHitLamp
{
    float3 position;
    float intensity;
    float3 axis;       // unit: the direction the lamp points along
    float cosOuter;    // the cone's factor is 0 at this cosine ...
    float3 colour;     // 0..1
    float cosInner;    // ... and 1 at this one
};

// A lamp from its three rows of the GPU list (src/gpu/city_hit_shade_rules.h MakeGpuLamp writes them): row 0 xyz the position in view space (float bits), w the intensity; row 1 xyz the unit axis,
// w the outer cosine; row 2 xyz the colour, w the inner cosine. The includer's CITY_HIT_LAMP_LOAD(i) is `CityHitLampFromRows(rows[3 i], rows[3 i + 1], rows[3 i + 2])` over its own buffer.
CityHitLamp CityHitLampFromRows(uint4 a, uint4 b, uint4 c)
{
    CityHitLamp l;
    l.position = float3(asfloat(a.x), asfloat(a.y), asfloat(a.z));
    l.intensity = asfloat(a.w);
    l.axis = float3(asfloat(b.x), asfloat(b.y), asfloat(b.z));
    l.cosOuter = asfloat(b.w);
    l.colour = float3(asfloat(c.x), asfloat(c.y), asfloat(c.z));
    l.cosInner = asfloat(c.w);
    return l;
}

// The game's laws (lamp_table.h LampTerm, LampConeFactor; the lit shaders'): colour x saturate(I / d^2) x cone x N.L, scaled by the lamp schedule. `toLamp` is the unit vector from the point to the lamp.
float CityHitLampCone(CityHitLamp l, float3 toLamp)
{
    float cone = 1.0;
    if (l.cosOuter > -1.5)
    {
        cone = saturate((dot(l.axis, -toLamp) - l.cosOuter) / (l.cosInner - l.cosOuter));
    }
    return cone;
}

float CityHitLampTerm(float intensity, float distance2)
{
    return distance2 <= intensity ? 1.0 : intensity / distance2;
}

struct CityHitLampEval
{
    float3 toLamp;     // unit, from the point to the lamp
    float distance;    // metres
    float3 radiance;   // the unshadowed light the lamp sends to the point, N.L in it, the schedule in it
    uint reaches;      // 1 where the lamp is in the game's reach of the point (d^2 < kCityHitLampReach x intensity): the lamps the lookup's cap counts
};

CityHitLampEval CityHitLampAt(CityHitParams p, CityHitLamp l, float3 position, float3 normal)
{
    CityHitLampEval e;
    const float3 d = l.position - position;
    const float d2 = max(dot(d, d), 1e-6);
    e.distance = sqrt(d2);
    e.toLamp = d * (1.0 / e.distance);
    e.radiance = float3(0.0, 0.0, 0.0);
    e.reaches = 0u;
    if (d2 < kCityHitLampReach * l.intensity)
    {
        e.reaches = 1u;
        e.radiance = l.colour * (CityHitLampTerm(l.intensity, d2) * CityHitLampCone(l, e.toLamp) * saturate(dot(normal, e.toLamp)) * p.lampFactor);
    }
    return e;
}

// The census (a stats frame only).
void CityHitCensus(CityHitParams p, uint word, uint value)
{
#ifdef CITY_HIT_CENSUS_ADD
    if ((p.flags & CITY_HIT_P_CALIBRATE) != 0u)
    {
        CITY_HIT_CENSUS_ADD(word, value);
    }
#endif
}

#if defined(CITY_HIT_LAMP_GRID)
// Lamp `index` of the world frame's list at a point: row 0 first (the position and the intensity), rows 1 and 2 (the axis, the cone, the colour) only where the lamp reaches the point (the game's reach, where its
// term is 1 per cent): the same arithmetic as CityHitLampAt over CityHitLampFromRows, a third of the loads for a lamp that does not reach.
CityHitLampEval CityHitLampAtIndex(CityHitParams p, RtWorld w, uint index, float3 position, float3 normal)
{
    const uint4 a = RtLampRow(w, index, 0u);
    const float3 lampPosition = float3(asfloat(a.x), asfloat(a.y), asfloat(a.z));
    const float intensity = asfloat(a.w);
    const float3 d = lampPosition - position;
    const float d2 = max(dot(d, d), 1e-6);
    CityHitLampEval e;
    e.distance = sqrt(d2);
    e.toLamp = d * (1.0 / e.distance);
    e.radiance = float3(0.0, 0.0, 0.0);
    e.reaches = 0u;
    if (d2 < kCityHitLampReach * intensity)
    {
        e.reaches = 1u;
        const CityHitLamp l = CityHitLampFromRows(a, RtLampRow(w, index, 1u), RtLampRow(w, index, 2u));
        e.radiance = l.colour * (CityHitLampTerm(l.intensity, d2) * CityHitLampCone(l, e.toLamp) * saturate(dot(normal, e.toLamp)) * p.lampFactor);
    }
    return e;
}

// The light of the world frame's lamps at a hit (CITY-RT R7). The lamps are the 64 m cells' around the hit (RtLampBegin: the world rows' lookup radius) in the cursor's order; the walk takes the first
// kCityHitLampMax of them THAT REACH the hit (a lamp out of the game's reach adds nothing and takes no slot: in a dense street the near lamps are not dropped for the far ones the walk met first) and stops
// after kCityHitLampWalkMax lamps walked whatever they were; the sum of their light is capped in luminance at kCityHitLampSumCap. Without the lamp shadow flag that is the light. With it ONE lamp is chosen by its unshadowed brightness (p.noise.z over the sum), a shadow
// ray goes toward it, and its light times sum / its own is the estimate of the lit lamps' sum (as the flat range's), times the cap's factor.
float3 CityHitLamps(CityHitParams p, float3 position, float3 normal, float footprint)
{
    const RtWorld w = RtWorldLoad();
    const float radius = w.lookupM > 0.0 ? w.lookupM : kCityHitLampLookup;
    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    uint examined = 0u;   // the lamps that reach the hit (the cap's count)
    uint walked = 0u;     // the lamps the cursor passed, those that reach and those that do not
    uint lit = 0u;
    RtLampCursor c = RtLampBegin(w, position, radius);
    CITY_HIT_LOOP
    while (c.done == 0u && examined < kCityHitLampMax && walked < kCityHitLampWalkMax)
    {
        const CityHitLampEval e = CityHitLampAtIndex(p, w, c.index, position, normal);
        const float luma = CityHitLuma(e.radiance);
        sum += e.radiance;
        weightSum += luma;
        lit += luma > 0.0 ? 1u : 0u;
        examined += e.reaches;
        walked += 1u;
        c = RtLampStep(w, c);
    }
    const bool capSum = weightSum > kCityHitLampSumCap;
    CityHitCensus(p, CITY_HIT_CENSUS_LOOKUPS, 1u);
    CityHitCensus(p, CITY_HIT_CENSUS_EXAMINED, examined);
    CityHitCensus(p, CITY_HIT_CENSUS_LIT, lit);
    CityHitCensus(p, CITY_HIT_CENSUS_CAP_COUNT, c.done == 0u ? 1u : 0u);
    CityHitCensus(p, CITY_HIT_CENSUS_CAP_SUM, capSum ? 1u : 0u);
    const float keep = capSum ? kCityHitLampSumCap / weightSum : 1.0;
    if ((p.flags & CITY_HIT_P_LAMP_SHADOW) == 0u || !(weightSum > 0.0))
    {
        return sum * keep;
    }
    const float pick = p.noise.z * weightSum;
    float running = 0.0;
    float3 chosen = float3(0.0, 0.0, 0.0);
    float3 toChosen = float3(0.0, 1.0, 0.0);
    float distanceChosen = 0.0;
    float weightChosen = 0.0;
    uint seen = 0u;       // the lamps that reach, replayed
    uint replayed = 0u;   // the lamps passed: the same walk as the first loop's
    RtLampCursor d = RtLampBegin(w, position, radius);
    CITY_HIT_LOOP
    while (d.done == 0u && replayed < walked && weightChosen <= 0.0)
    {
        const CityHitLampEval e = CityHitLampAtIndex(p, w, d.index, position, normal);
        const float lampWeight = CityHitLuma(e.radiance);
        running += lampWeight;
        seen += e.reaches;
        replayed += 1u;
        if (lampWeight > 0.0 && (running >= pick || seen == examined))
        {
            chosen = e.radiance;
            toChosen = e.toLamp;
            distanceChosen = e.distance;
            weightChosen = lampWeight;
        }
        d = RtLampStep(w, d);
    }
    if (!(weightChosen > 0.0))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 origin = position + normal * CityHitLift(position.z);
    const bool blocked = CityHitLampRay(origin, toChosen, max(distanceChosen - kCityHitLampEnd, 0.0), footprint);
    return blocked ? float3(0.0, 0.0, 0.0) : chosen * (keep * weightSum / weightChosen);
}
#elif defined(CITY_HIT_LAMP_LOAD)
// The light of the frame's lamps at a hit. Without the lamp shadow flag: the sum of every lamp of the params' range (a few dozen at night, none by day; no rays). With it: ONE lamp, chosen by its
// unshadowed brightness (p.noise.z over the sum of them), with a shadow ray toward it, its light times sum / its own (the estimate's expectation is the sum of the lit lamps').
float3 CityHitLamps(CityHitParams p, float3 position, float3 normal, float footprint)
{
    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    CITY_HIT_LOOP
    for (uint i = 0u; i < p.lampCount; ++i)
    {
        const CityHitLampEval e = CityHitLampAt(p, CITY_HIT_LAMP_LOAD(p.lampFirst + i), position, normal);
        sum += e.radiance;
        weightSum += CityHitLuma(e.radiance);
    }
    if ((p.flags & CITY_HIT_P_LAMP_SHADOW) == 0u || !(weightSum > 0.0))
    {
        return sum;
    }
    const float pick = p.noise.z * weightSum;
    float running = 0.0;
    float3 chosen = float3(0.0, 0.0, 0.0);
    float3 toChosen = float3(0.0, 1.0, 0.0);
    float distanceChosen = 0.0;
    float weightChosen = 0.0;
    CITY_HIT_LOOP
    for (uint j = 0u; j < p.lampCount && weightChosen <= 0.0; ++j)
    {
        const CityHitLampEval e = CityHitLampAt(p, CITY_HIT_LAMP_LOAD(p.lampFirst + j), position, normal);
        const float w = CityHitLuma(e.radiance);
        running += w;
        if (w > 0.0 && (running >= pick || j + 1u == p.lampCount))
        {
            chosen = e.radiance;
            toChosen = e.toLamp;
            distanceChosen = e.distance;
            weightChosen = w;
        }
    }
    if (!(weightChosen > 0.0))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 origin = position + normal * CityHitLift(position.z);
    const bool blocked = CityHitLampRay(origin, toChosen, max(distanceChosen - kCityHitLampEnd, 0.0), footprint);
    return blocked ? float3(0.0, 0.0, 0.0) : chosen * (weightSum / weightChosen);
}
#endif

// ---- the emitters -----------------------------------------------------------------------------------------------------------------------------

#if defined(CITY_HIT_EMITTER_RADIANCE)
// The number the choice of the one emitter is made with: from the params' noise, but not the lamps' own (z) nor the sun's disc (x, y) alone: frac(3 z + 7 y). With whole number factors it is exactly uniform given z
// and given y (the other one spans whole periods), so it is as uniform as either and independent of each; a factor that is not a whole number would not be (frac(2.7 z) over-weights the low values).
float CityHitEmitterNoise(CityHitParams p)
{
    return frac(p.noise.z * 3.0 + p.noise.y * 7.0);
}

// The light of the includer's emitters at a hit (GI-BOUNCE-RT): the sum of the ones that reach it, its luminance capped at kCityHitEmitterSumCap with the colours' ratios kept; with CITY_HIT_P_EMITTER_SHADOW
// ONE of them is chosen by its unshadowed brightness (the noise over the sum), a ray against the city goes toward it, and its light times sum / its own is the estimate of the lit emitters' sum (the expectation is
// the sum of the ones that are not blocked: the lamps' estimator, CityHitLamps). THE BUDGET IS COUNTED AFTER THE REACH: a hit no emitter reaches (nearly every hit away from a car) casts nothing, and the
// loops are over at most kCityHitEmitterMax records whose first load rejects the ones out of range.
float3 CityHitEmitters(CityHitParams p, float3 position, float3 normal, float footprint)
{
    const uint count = min(uint(CITY_HIT_EMITTER_COUNT), kCityHitEmitterMax);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    uint reaching = 0u;
    CITY_HIT_LOOP
    for (uint i = 0u; i < count; ++i)
    {
        const float3 radiance = CITY_HIT_EMITTER_RADIANCE(i, position, normal);
        const float luma = CityHitLuma(radiance);
        sum += radiance;
        weightSum += luma;
        reaching += luma > 0.0 ? 1u : 0u;
    }
    if (!(weightSum > 0.0))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float keep = weightSum > kCityHitEmitterSumCap ? kCityHitEmitterSumCap / weightSum : 1.0;
    if ((p.flags & CITY_HIT_P_EMITTER_SHADOW) == 0u)
    {
        return sum * keep;
    }
    const float pick = CityHitEmitterNoise(p) * weightSum;
    float running = 0.0;
    float3 chosen = float3(0.0, 0.0, 0.0);
    uint chosenAt = 0u;
    float weightChosen = 0.0;
    uint seen = 0u;
    CITY_HIT_LOOP
    for (uint j = 0u; j < count && weightChosen <= 0.0; ++j)
    {
        const float3 radiance = CITY_HIT_EMITTER_RADIANCE(j, position, normal);
        const float luma = CityHitLuma(radiance);
        running += luma;
        seen += luma > 0.0 ? 1u : 0u;
        if (luma > 0.0 && (running >= pick || seen == reaching))
        {
            chosen = radiance;
            chosenAt = j;
            weightChosen = luma;
        }
    }
    if (!(weightChosen > 0.0))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 to = CITY_HIT_EMITTER_TO(chosenAt, position);
    const float distance = sqrt(max(dot(to, to), 1e-12));
    const float3 origin = position + normal * CityHitLift(position.z);
    const bool blocked = CityHitEmitterRay(origin, to * (1.0 / distance), max(distance - kCityHitEmitterEnd, 0.0), footprint);
    return blocked ? float3(0.0, 0.0, 0.0) : chosen * (keep * weightSum / weightChosen);
}
#endif

// ---- the shading ------------------------------------------------------------------------------------------------------------------------------

float CityHitSchlick(float f0, float nv)
{
    return f0 + (1.0 - f0) * pow(max(1.0 - nv, 1e-4), 5.0);
}

// The share of the sky's reflection a surface of this smoothness has: nothing for a rough wall, all for glass and a wet road.
float CityHitSpecWeight(float smoothness)
{
    const float s = saturate((smoothness - kCityHitSpecFrom) / (1.0 - kCityHitSpecFrom));
    return s * s;
}

// What a glowing material's glow follows (the tint word's bits 30..31): 3 is not a kind and reads as the night curve, a pack written before the kinds has 0 there.
uint CityHitGlowKindOf(uint kind)
{
    return kind == CITY_HIT_GLOW_LENS ? CITY_HIT_GLOW_LENS : (kind == CITY_HIT_GLOW_ALWAYS ? CITY_HIT_GLOW_ALWAYS : CITY_HIT_GLOW_NIGHT);
}

// How much of its glow a material of this kind shows (CITY-RT R7), 0..1: a traffic signal always; the rest by the includer's schedule (the world frame's: a lens by the street lamps', neon, signs and lit windows by
// the fill lights'), else by p.lampFactor. The glow itself is the albedo x the material's scale x kRtHitEmissiveGain x this.
float CityHitGlowFactor(CityHitParams p, uint kind)
{
#ifdef CITY_HIT_GLOW_FACTOR
    return saturate(CITY_HIT_GLOW_FACTOR(kind));
#else
    return kind == CITY_HIT_GLOW_ALWAYS ? 1.0 : saturate(p.lampFactor);
#endif
}

CityHitShaded CityHitShadedNone()
{
    CityHitShaded r;
    r.valid = 0u;
    r.cls = 0u;
    r.colour = float3(0.0, 0.0, 0.0);
    r.albedo = float3(0.0, 0.0, 0.0);
    r.normal = float3(0.0, 1.0, 0.0);
    r.ndl = 0.0;
    r.visible = 1.0;
    r.glow = 0.0;
    return r;
}

// The colour of a decoded hit on the city, in the picture's units: the albedo by the light (the frame's ambient, the sun, the lamps, the GI field's feedback), the class's way (the wet road, the sky on
// glass and metal and water), the glow of what glows. REFLECT mode is for a mirror ray's hit (the texture, the sky's reflection); GI mode for a bounce (the average colour, no reflection).
CityHitShaded CityHitShadeHit(CityHitParams p, RtHitFrame f, CityHitRay ray, CityHitInstance inst, CityHit h)
{
    CityHitShaded r = CityHitShadedNone();
    if ((f.flags & RT_HIT_FRAME_SHADE) == 0u || h.valid == 0u)
    {
        return r;
    }
    const CityHitMaterial m = CityHitLoadMaterial(h.material);
    const CityHitTexture t = CityHitLoadTexture(m.textureIndex);
    const float3 hit = ray.origin + ray.direction * ray.t;
    const float3 normal = CityHitViewNormal(h, inst, ray.direction);
    const float nv = saturate(-dot(normal, ray.direction));
    float3 albedo = CityHitAlbedo(p, h, m, t, ray.footprint, nv);
    if (m.cls == CITY_HIT_CLASS_ROAD || m.cls == CITY_HIT_CLASS_WET_ROAD || m.cls == CITY_HIT_CLASS_SIDEWALK)
    {
        albedo = albedo * (1.0 - kCityHitWetDarken * saturate(p.wet));
    }

    // the light: the ambient with its hemisphere term, the sun by N.L and its visibility, the lamps, the field's feedback
    const float hemisphere = RtHitHemisphere(f, normal);
    float3 light = RtHitAmbientLight(f, hemisphere);
#ifdef CITY_HIT_AMBIENT
    light = CITY_HIT_AMBIENT(hit, normal, light);
#endif
    const bool sunKnown = (f.flags & RT_HIT_FRAME_SUN) != 0u;
    const float3 toSun = sunKnown ? f.toSun : f.axisY;
    const float ndl = saturate(dot(normal, toSun));
    float visible = 1.0;
    CITY_HIT_BRANCH
    if (ndl > 0.0)
    {
        visible = CityHitSunVisible(p, f, hit, normal, toSun, ray.footprint);
    }
    light += f.sun * (ndl * visible);
#if defined(CITY_HIT_LAMP_GRID)
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_LAMPS) != 0u && p.lampFactor > 0.0)
    {
        light += CityHitLamps(p, hit, normal, ray.footprint);
    }
#elif defined(CITY_HIT_LAMP_LOAD)
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_LAMPS) != 0u && p.lampCount != 0u && p.lampFactor > 0.0)
    {
        light += CityHitLamps(p, hit, normal, ray.footprint);
    }
#endif
#if defined(CITY_HIT_EMITTER_RADIANCE)
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_EMITTERS) != 0u)
    {
        light += CityHitEmitters(p, hit, normal, ray.footprint);   // the cars' lamps (GI-BOUNCE-RT): their own luminance, not the field's
    }
#endif
#ifdef CITY_HIT_FEEDBACK
    CITY_HIT_BRANCH
    if (p.mode == CITY_HIT_MODE_GI && p.bounce > 0.0)
    {
        light += CITY_HIT_FEEDBACK(hit, normal) * p.bounce;
    }
#endif

    // the surface: diffuse by the class's share (and what a metal does not take), the sky by Schlick for the smooth, the glow
    const float metal = m.metal;
    const uint cls = min(m.cls, CITY_HIT_CLASS_COUNT - 1u);
    float3 result = albedo * light * (kCityHitDiffuseGain[cls] * (1.0 - metal));
    CITY_HIT_BRANCH
    if (p.mode == CITY_HIT_MODE_REFLECT)
    {
        const float weight = CityHitSpecWeight(m.smoothness) * kCityHitSpecGain[cls];
        CITY_HIT_BRANCH
        if (weight > 0.0)
        {
            const float3 reflected = reflect(ray.direction, normal);
            const float lod = (1.0 - m.smoothness) * kCityHitEnvLodSmooth + log2(1.0 + kRtHitConeLodScale * ray.footprint / max(2.0 * ray.t, 1e-3));
            float3 env = f.ambient * (0.6 + 0.4 * dot(reflected, f.axisY));
            if ((f.flags & RT_HIT_FRAME_ENV) != 0u)
            {
                env = RtHitEnvAt(f, reflected, lod);
            }
            const float3 f0 = lerp(float3(kCityHitDielectricF0, kCityHitDielectricF0, kCityHitDielectricF0), albedo, metal);
            const float3 fresnel = float3(CityHitSchlick(f0.x, nv), CityHitSchlick(f0.y, nv), CityHitSchlick(f0.z, nv));
            result = result * (1.0 - weight * fresnel) + env * fresnel * (weight * f.specGain);
        }
    }
    float glow = 0.0;
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_EMISSIVE) != 0u && m.emissive > 0.0)
    {
        const uint kind = CityHitGlowKindOf(m.glowKind);
        glow = CityHitGlowFactor(p, kind);
        result += albedo * (m.emissive * kRtHitEmissiveGain * glow);
        CityHitCensus(p, CITY_HIT_CENSUS_GLOW_NIGHT + kind, 1u);
    }

    // in the picture's units: not negative, at most what a half float keeps, a NaN black (decided by the bits, nan_guard.hlsli: max(NaN, 0) is 0 on D3D12 only, and an infinity would make a NaN of the blend
    // with the picture's colour); every consumer of a hit's colour (the reflections, the bounce light, the mirror) takes it from here
    float3 shaded = NgHeld3(result * p.units, kCityHitMaxColour);
#ifdef CITY_HIT_HAZE
    shaded = CITY_HIT_HAZE(shaded, length(hit));
#endif
    r.valid = 1u;
    r.cls = cls;
    r.colour = shaded;
    r.albedo = albedo;
    r.normal = normal;
    r.ndl = ndl;
    r.visible = visible;
    r.glow = glow;
    return r;
}

// The same for a hit the caller has not decoded: the decode of the committed triangle (city_hit_common.hlsli CityHitDecode), then the shading.
CityHitShaded CityHitShade(CityHitParams p, RtHitFrame f, CityHitRay ray, CityHitInstance inst)
{
    return CityHitShadeHit(p, f, ray, inst, CityHitDecode(inst.instanceId, inst.geometryIndex, inst.primitiveIndex, inst.barycentrics));
}

// ---- ResolveHit ------------------------------------------------------------------------------------------------------------------------------

float CityHitGainOf(uint cls)
{
#ifdef CITY_HIT_GAIN
    return CITY_HIT_GAIN(cls);
#else
    return 1.0;
#endif
}

// The luminance as the fixed point log the calibration's counters sum (rt_hit_calibration.h FixedLog2).
uint CityHitFixedLog(float luminance)
{
    return uint(clamp((log2(max(luminance, kCityHitLogFloor)) + kCityHitLogBias) * kCityHitLogScale, 0.0, kCityHitLogMax));
}

// How much of the hit the picture can speak for, 0..1, and where (the pixel of the hit's view-space point). The point must be in front of the camera and inside the picture, away from its edge by
// the band; the depth the picture holds there must agree with the point's (a tolerance of the screen's, plus what a slanted surface changes across a pixel); and the hit's surface must face the
// camera (else the picture shows its back or something in front of it). Each fades smoothly: the weight is the product.
struct CityHitScreenAt
{
    float weight;
    float2 uv;         // 0..1, y down
    float pixel;       // the width of a pixel in metres at the point
};

CityHitScreenAt CityHitScreenWeight(CityHitScreen s, float3 hit, float3 normal)
{
    CityHitScreenAt a;
    a.weight = 0.0;
    a.uv = float2(0.0, 0.0);
    a.pixel = 1.0;
#if defined(CITY_HIT_SCREEN_DEPTH) && defined(CITY_HIT_SCREEN_COLOUR)
    if (!(hit.z > kCityHitMinDepth))
    {
        return a;
    }
    const float2 ndc = float2(hit.x / (hit.z * s.lens.x), hit.y / (hit.z * s.lens.y));
    const float edge = max(max(abs(ndc.x), abs(ndc.y)), 0.0);
    if (!(edge < 1.0))
    {
        return a;
    }
    a.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    a.pixel = 2.0 * s.lens.y * hit.z / max(s.lens.w, 1.0);
    const int2 texel = int2(min(int(a.uv.x * s.lens.z), int(s.lens.z) - 1), min(int(a.uv.y * s.lens.w), int(s.lens.w) - 1));
#ifdef CITY_HIT_SCREEN_REJECT
    if (CITY_HIT_SCREEN_REJECT(texel) != 0)
    {
        return a;
    }
#endif
    const float depth = CITY_HIT_SCREEN_DEPTH(texel);
    const float facing = dot(normal, -normalize(hit));
    const float slope = sqrt(max(1.0 - facing * facing, 0.0)) / max(facing, 0.2);
    const float tolerance = s.tolerance.x + s.tolerance.y * hit.z + a.pixel * slope;
    const float edgeWeight = saturate((1.0 - edge) / max(s.tolerance.z, 1e-4));
    const float depthWeight = 1.0 - smoothstep(tolerance, 2.0 * tolerance, abs(depth - hit.z));
    const float faceWeight = saturate((facing - s.tolerance.w) / 0.15);
    a.weight = edgeWeight * depthWeight * faceWeight;
#endif
    return a;
}

CityHitResolved CityHitResolvedNone()
{
    CityHitResolved r;
    r.source = CITY_HIT_SOURCE_NONE;
    r.cls = 0u;
    r.colour = float3(0.0, 0.0, 0.0);
    r.screen = 0.0;
    r.normal = float3(0.0, 1.0, 0.0);
    r.ndl = 0.0;
    r.visible = 1.0;
    r.glow = 0.0;
    return r;
}

// ResolveHit: what a ray's hit on the city shows, for every consumer. The picture's lit colour where the hit's point is in the picture (its mip level the cone's footprint in pixels), the model's
// (CityHitShade times its class's gain) where it is not, both by the picture's weight where it fades. In GI mode the picture's colour is capped (CityHitScreen::cap: a lamp or a neon sign is a
// bounded source). A stats frame (CITY_HIT_P_CALIBRATE) adds, for a hit the picture speaks for in full, the log of its luminance and of the UNGAINED model's to the class's counters. `source` NONE
// says there was nothing to resolve (no frame light, no data in the pool): the caller keeps its colour.
CityHitResolved CityHitResolve(CityHitParams p, RtHitFrame f, CityHitScreen s, CityHitRay ray, CityHitInstance inst)
{
    CityHitResolved result = CityHitResolvedNone();
    const float3 hit = ray.origin + ray.direction * ray.t;
    const CityHit decoded = CityHitDecode(inst.instanceId, inst.geometryIndex, inst.primitiveIndex, inst.barycentrics);
    if (decoded.valid == 0u)
    {
        return result;  // the hit's data is not in the pool: nothing to resolve, the caller keeps its colour
    }
    float weight = 0.0;
    float3 seen = float3(0.0, 0.0, 0.0);
    float3 normalHint = CityHitViewNormal(decoded, inst, ray.direction);
    CITY_HIT_BRANCH
    if ((p.flags & CITY_HIT_P_SCREEN) != 0u)
    {
        const CityHitScreenAt at = CityHitScreenWeight(s, hit, normalHint);
        weight = at.weight;
#if defined(CITY_HIT_SCREEN_DEPTH) && defined(CITY_HIT_SCREEN_COLOUR)
        CITY_HIT_BRANCH
        if (weight > 0.0)
        {
            const float mip = log2(max(ray.footprint / max(at.pixel, 1e-6), 1.0));
            seen = CITY_HIT_SCREEN_COLOUR(at.uv, mip);
            if (s.cap > 0.0)
            {
                const float luma = CityHitLuma(seen);
                seen = luma > s.cap ? seen * (s.cap / luma) : seen;
            }
        }
#endif
    }
    CityHitShaded shaded = CityHitShadedNone();
    CITY_HIT_BRANCH
    if (weight < 1.0 || (p.flags & CITY_HIT_P_CALIBRATE) != 0u)
    {
        shaded = CityHitShadeHit(p, f, ray, inst, decoded);
    }
    result.normal = shaded.valid != 0u ? shaded.normal : normalHint;
    result.cls = shaded.cls;
    result.ndl = shaded.ndl;
    result.visible = shaded.visible;
    result.glow = shaded.glow;
    result.screen = weight;
    if (shaded.valid == 0u && weight <= 0.0)
    {
        return result;
    }
    // the calibration's sample: the picture speaks for the hit in full and both colours are over the floor
    CITY_HIT_BRANCH
    if (shaded.valid != 0u && weight >= kCityHitScreenTrust && (p.flags & CITY_HIT_P_CALIBRATE) != 0u)
    {
        const float seenLuma = CityHitLuma(seen);
        const float modelLuma = CityHitLuma(shaded.colour);
        if (seenLuma > kCityHitCalFloor && modelLuma > kCityHitCalFloor)
        {
#ifdef CITY_HIT_STAT_ADD
            const uint cls = min(shaded.cls, CITY_HIT_CLASS_COUNT - 1u);
            CITY_HIT_STAT_ADD(cls * 3u, 1u);
            CITY_HIT_STAT_ADD(cls * 3u + 1u, CityHitFixedLog(seenLuma));
            CITY_HIT_STAT_ADD(cls * 3u + 2u, CityHitFixedLog(modelLuma));
#endif
        }
    }
    const float3 model = shaded.colour * CityHitGainOf(shaded.cls);
    if (shaded.valid == 0u)
    {
        result.colour = seen;
        result.source = CITY_HIT_SOURCE_SCREEN;
    }
    else if (weight >= 1.0)
    {
        result.colour = seen;
        result.source = CITY_HIT_SOURCE_SCREEN;
    }
    else if (weight <= 0.0)
    {
        result.colour = model;
        result.source = CITY_HIT_SOURCE_SHADED;
    }
    else
    {
        result.colour = lerp(model, seen, weight);
        result.source = CITY_HIT_SOURCE_BLEND;
    }
    return result;
}

#endif  // MCLA_CITY_HIT_SHADE_HLSLI
