// MCLA GPU renderer -- the player's side mirrors show the real view behind, ray traced (car_mirror_rt.h; src/gpu/car_mirror_rt_rules.h has the design, the numbers and the tests).
//
// Run at the car ray tracing's trigger (the game's mid-scene depth copy), right after this frame's top level was built (car_rt_scene.h: the player's car and the cars beside it, in the frame's VIEW
// space, x right, y up, z forward: a ray from the origin through a pixel with the direction (ndc.x tanX, ndc.y tanY, 1) hits at RayT = the view z). One thread is one pixel of a mirror's
// rectangle (the push's rect: first pixel and size, in render pixels). It writes the image (u0, the push's image.xy is the rectangle's first texel; R16G16B16A16_FLOAT): rgb the colour of what
// the mirror shows in the scene colour's units, alpha the share of the game's glass it replaces (the coverage of the glass in the pixel times the strength; 0 off the glass).
//
//   1. THE GLASS: four primary rays (a rotated grid inside the pixel) against the player's parts (the push's image.z). Each goes on through the panes of see-through glass (the car's windows:
//      CAR_RT_FLAG_SEE_THROUGH in the instance's row), so a cockpit camera sees the mirror through the side window as it does in the game; the nearest other part decides. The share of the rays whose
//      nearest part is a mirror glass (CAR_RT_FLAG_MIRROR) is the pixel's coverage: the glass's edge is as soft as the pixel and a pillar in front of it covers it exactly. The first such ray gives the
//      glass's point and its smooth mesh normal (the glass is slightly convex: the normals vary over it, as a real mirror's view widens).
//   2. THE MIRROR RAY: one ray from the glass along reflect(view, normal), the start lifted off it along the normal, against the player's parts and the cars beside (the push's image.w; the glass's own
//      panes of see-through glass are looked through as for the primary ray). A hit on a car is shaded from its material (car_rt_hit_shade.hlsli CarHitShade: the paint's colour, the diffuse texture, the
//      sun and its visibility, the sky's openness, the environment by the fresnel, the lamps at night), and a hit on ANOTHER car fades into the world's colour over the last quarter of the cars' reach
//      (the scene holds cars only within it: a car is not cut off at the edge of the scene). What the ray misses is MirrorWorld().
//   3. THE WORLD: MirrorWorld() is the game's environment map at the mirror direction (a dual paraboloid made around the camera: the sky and the LOD city, read as the game's own glass reads it,
//      times the colour target's units). It is what the ray leaves behind: the sky, and the street beyond the city's reach (or the whole street where the city is not traced, see 4).
//   4. THE CITY (MIRROR-CITY, src/gpu/car_mirror_city_rules.h has the design and the numbers): where the push's city flags say on (the setting mcla_gpu_car_mirror_rt_world, the city's data standing), the
//      mirror ray is cast over the longer of the cars' and the city's reach against the cars' parts AND the city's triangles in the one top level (CityHitTrace: the closest hit, the cut-outs' exact
//      alpha), still going on through the player's own see-through panes. A hit on the city is shaded by the city hit library (hlsl/city_hit_shade.hlsli, ResolveHit without the picture: the point is behind
//      the camera) with the world frame's light and lamps, as the car paint's reflections do (hlsl/city_refl.hlsli: the same params, its own code here because that file declares the reflections' set 3),
//      and lies over the environment map by the weight of its distance. A hit with no data in the pool, or past the city's reach, is a MISS (the environment stays: never black); a car's part that weighs
//      nothing does not hide the city behind it; a car in its fade band lets the city show through. With the city off (the flags 0) every line below is the mirror as it was before this package.
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_mirror_rt.cpp): set 0 the bindless Texture2D<float4> set (space 0: the environment map and the diffuse textures of the
// materials and of the city, read with Load), set 1 the output (u0, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2: the top level t0, the hit data t6..t9, the world rows t10 and the
// lamps t11), the push block (b0, space 3).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the image is R16G16B16A16_FLOAT (car_mirror_rt.cpp EnsureImage).
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);

struct CarMirrorPush
{
    // x, y the first pixel of the rectangle in the picture, z, w its size in pixels
    uint4 rect;
    // x, y the rectangle's first texel in the image, z the primary ray's parts (CAR_RT_MASK_*), w the mirror ray's parts
    uint4 image;
    // x, y tan of half the horizontal and the vertical field of view, z, w the picture's size in pixels
    float4 lens;
    // x the strength (the share of the game's glass the picture replaces), y the cars' reach in metres (the mirror ray's length), z the share of it up to which a hit on another car weighs 1,
    // w the units: the colour target's over the shaders' output, 2^exp_bias (16 at the game's bias of 4): a colour made from the materials' constants is multiplied by it
    float4 tune;
    // MIRROR-CITY (car_mirror_city_rules.h): x the flags (1 the city is traced, 2 the lamp term, 4 the world frame's light), y the city's reach in metres and z the share of it up to which a city hit weighs 1
    // (float bits), w reserved. Flags 0: the city is not traced and the rest is not read.
    uint4 city;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarMirrorPush> g_push;
#else
ConstantBuffer<CarMirrorPush> g_push : register(b0, space3);
#endif

#include "car_rt_hit_shade.hlsli"

// MIRROR-CITY: the city hit library on the car scene's set, as the car paint's reflections bind it (hlsl/city_refl.hlsli, which this file cannot include: it declares the reflections' own set 3, and the
// mirror needs none): the world frame's rows (t10: the world's light, the lamps' schedule, the class gains) and lamps (t11) are in the car scene's set, the hit data's pool and tables (t6..t9) too. The hooks:
// the class gains are the world frame's (the calibration the other consumers close; the mirror has no picture to measure against), the lamp light is the world frame's lamp grid, what glows glows by the world's
// schedule. No picture (the mirror's points are behind the camera), no haze, no probe field, no counters (the library drops what it is not given).
#define RT_WORLD_SPACE CAR_RT_SPACE
#include "rt_world_frame.hlsli"

static bool gMirrorGlowWorld = false;
static float gMirrorGlowOld = 0.0;

// What a glowing material glows by: the world rows' schedule (gMirrorGlowWorld), or the sun's elevation's (gMirrorGlowOld); a traffic signal always (city_refl.hlsli CityReflGlowFactor).
float MirrorGlowFactor(uint kind)
{
    if (kind == RT_WORLD_GLOW_ALWAYS)
    {
        return 1.0;
    }
    return gMirrorGlowWorld ? RtWorldGlowFactor(kind) : gMirrorGlowOld;
}

#define CITY_HIT_SPACE CAR_RT_SPACE
#define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)
#define CITY_HIT_LAMP_GRID
#define CITY_HIT_GLOW_FACTOR(kind) MirrorGlowFactor(kind)
#include "city_hit_shade.hlsli"

// car_mirror_rt_rules.h: the same numbers
static const float kMirrorEnvLod = 0.5;          // the environment map's level for a mirror
static const float kMirrorLift = 0.003;          // m: the mirror ray starts this far off the glass along the normal ...
static const float kMirrorLiftPerMetre = 0.001;  // ... plus this a metre of the glass's view distance
static const float kMirrorMinNv = 0.03;          // a glass seen edge on is not traced
static const float kMirrorPrimaryReach = 40.0;   // m
static const uint kMirrorSeeThroughSteps = 3;    // panes of see-through glass a ray goes on through
static const uint kMirrorTaps = 4;               // the coverage's rays a pixel
static const float kMirrorPaneSkip = 0.002;      // m: the ray goes on this far past a pane it looked through
// car_mirror_city_rules.h: the city's numbers
static const uint kMirrorCityOn = 1u;            // the push's city.x flags
static const uint kMirrorCityLamps = 2u;
static const uint kMirrorCityWorldLight = 4u;
static const uint kMirrorCityMask = 0x80u;       // the city's instance bit (city_refl_rules.h kPartCity), added to the mirror ray's parts
static const float kMirrorMinFade = 1.0 / 512.0; // a city hit that weighs less is a miss (city_refl.hlsli kCityMinFade)
static const float kMirrorSunTan = 0.0;          // the hit's sun ray is sharp: this image has no history to average a jitter, so a jitter would be grain (and there is no noise to vary)
static const float kMirrorAoMix = 0.5;           // the share of the baked vertex occlusion in a hit's albedo (city_refl_rules.h kAoMix)
static const float kMirrorNightFull = 3.0;       // the lamp schedule's fallback where the world rows are not valid (city_refl_rules.h LampSchedule)
static const float kMirrorNightNone = 12.0;
static const float kMirrorRadToDeg = 57.29577951308232;
// the four taps: a rotated grid inside the pixel (offsets from the pixel's centre, in pixels)
static const float2 kMirrorTap[4] = {float2(-0.125, -0.375), float2(0.375, -0.125), float2(0.125, 0.375), float2(-0.375, 0.125)};

// The nearest part of the scene along a ray whose parts meet `mask`, going on through the panes of see-through glass (the car's windows), at most kMirrorSeeThroughSteps of them. `t` of the result is
// the distance from the ray's ORIGIN in the direction's own units (the sum over the steps). `flags` is the hit instance's flags word.
bool MirrorTrace(float3 origin, float3 direction, float reach, uint mask, out CarRtHit hit, out uint flags)
{
    hit = (CarRtHit)0;
    flags = 0u;
    float travelled = 0.0;
    float3 from = origin;
    [loop]
    for (uint step = 0u; step <= kMirrorSeeThroughSteps; ++step)
    {
        const CarRtHit h = CarRtTrace(from, direction, 0.0, max(reach - travelled, 0.0), mask);
        if (!h.hit)
        {
            return false;
        }
        const uint f = gCarRtInstances[h.instance_index].z;
        if ((f & CAR_RT_FLAG_SEE_THROUGH) != 0u && step < kMirrorSeeThroughSteps)
        {
            const float skip = h.t + kMirrorPaneSkip;
            from = from + direction * skip;
            travelled += skip;
            continue;
        }
        hit = h;
        hit.t = travelled + h.t;
        flags = f;
        return true;
    }
    return false;
}

// The closest hit of the mirror ray over the cars' parts and the city (`mask` has the city's bit where the city is traced), going on through the player's own see-through panes as MirrorTrace does, at
// most kMirrorSeeThroughSteps of them. `t` of the result is the distance from the ray's ORIGIN (the sum over the steps). `footprint0` is the ray cone's width where it started (the pixel's width at the glass),
// `tanHalf` the tangent of its half angle (half the pixel's angle): the cut-outs' alpha is read at the cone's level (CityHitTrace). A pane skipped moves the start on and the cone's width with it.
CityHitTraceResult MirrorTraceCity(float3 origin, float3 direction, float reach, uint mask, float footprint0, float tanHalf)
{
    CityHitTraceResult none = CityHitTraceNone();
    float travelled = 0.0;
    float3 from = origin;
    [loop]
    for (uint step = 0u; step <= kMirrorSeeThroughSteps; ++step)
    {
        const float width = footprint0 + 2.0 * tanHalf * travelled;
        CityHitTraceResult h = CityHitTrace(from, direction, 0.0, max(reach - travelled, 0.0), mask, width, tanHalf, 0.5);
        if (h.hit == 0u)
        {
            return none;
        }
        if (!CityHitIsCity(h.inst.instanceId) && (gCarRtInstances[h.instanceIndex].z & CAR_RT_FLAG_SEE_THROUGH) != 0u && step < kMirrorSeeThroughSteps)
        {
            const float skip = h.t + kMirrorPaneSkip;
            from = from + direction * skip;
            travelled += skip;
            continue;
        }
        h.t = travelled + h.t;
        return h;
    }
    return none;
}

// The weight of a city hit by its distance along the mirror ray: 1 up to the cell's fade start, a smoothstep to 0 at the city's reach (city_refl_rules.h CityFade, city_refl.hlsli CityReflFade).
float MirrorCityFade(float distance)
{
    const float reach = asfloat(g_push.city.y);
    const float lo = asfloat(g_push.city.z) * reach;
    const float t = saturate((distance - lo) / max(reach - lo, 1e-6));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

// 1 at night, 0 by day, from the sun's elevation (city_refl_rules.h LampSchedule: the headlight rig's night curve); 0 where the frame has no sun direction. The glow of a hit where the world rows are not valid.
float MirrorLampSchedule(RtHitFrame f)
{
    if ((f.flags & RT_HIT_FRAME_SUN) == 0u)
    {
        return 0.0;
    }
    const float degrees = asin(clamp(dot(f.toSun, f.axisY), -1.0, 1.0)) * kMirrorRadToDeg;
    const float t = saturate((degrees - kMirrorNightFull) / (kMirrorNightNone - kMirrorNightFull));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

// What a ray whose closest hit `h` is a CITY triangle shows: the library's ResolveHit without the picture (the model's colour times its class's gain). `origin` and `direction` (unit) are the ray's, `h.t` the
// distance from the origin, `footprint` the pixel's width at the hit. False: nothing to show (the hit's data is not in the pool, the frame has no light, or the hit is past the city's reach): the caller treats
// the ray as a MISS and the environment stays (CITY-RT-REVIEW M5: never a black hit). True: `colour` in the colour target's units and `fade` the weight of the distance. The light is the WORLD frame's where
// the host's switch says so and the world rows hold a sun and a shading frame (city_refl_rules.h WorldLightFor), else the cars' draws' (city_refl.hlsli CityReflHit: the same two choices).
bool MirrorCityHit(float3 origin, float3 direction, CityHitTraceResult h, float footprint, out float3 colour, out float fade)
{
    colour = float3(0.0, 0.0, 0.0);
    fade = MirrorCityFade(h.t);
    if (fade < kMirrorMinFade)
    {
        return false;  // beyond the reach
    }
    RtHitFrame f = RtHitLoadFrame();
    const uint4 status = gRtWorld[RT_WORLD_ROW_STATUS];
    const bool worldValid = (status.y & RT_WORLD_VALID) != 0u;
    if ((g_push.city.x & kMirrorCityWorldLight) != 0u && worldValid)
    {
        const RtHitFrame w = RtWorldHitFrame();
        if ((w.flags & (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)) == (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN))
        {
            f = w;
        }
    }
    CityHitParams p;
    p.mode = CITY_HIT_MODE_REFLECT;
    p.flags = CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE;
    p.units = g_push.tune.w;
    p.sunReach = 0.0;
    p.sunTan = kMirrorSunTan;
    // the lamp term (city_refl_rules.h LampTermFor): the world frame's lamp grid and glow schedule where the host's switch says so and the rows are valid, else the sun's elevation's schedule and no lamp light
    const bool lampsWorld = (g_push.city.x & kMirrorCityLamps) != 0u && worldValid;
    float lampFactor = lampsWorld ? asfloat(status.w) : MirrorLampSchedule(f);
    if (lampsWorld)
    {
        p.flags = p.flags | CITY_HIT_P_LAMPS;
    }
    gMirrorGlowWorld = lampsWorld;
    gMirrorGlowOld = lampFactor;
    p.lampFactor = lampFactor;
    p.wet = worldValid ? saturate(asfloat(gRtWorld[RT_WORLD_ROW_LIGHT].y)) : 0.0;  // the world's wetness where the rows are valid (car_mirror_city_rules.h WetFor), else dry
    p.aoMix = kMirrorAoMix;
    p.lodBias = 0.0;
    p.bounce = 0.0;
    p.lampFirst = 0u;
    p.lampCount = 0u;
    p.noise = float3(0.5, 0.5, 0.5);  // unused: the sun's ray is sharp and the lamp sum has no shadow ray
    CityHitScreen s;
    s.lens = float4(g_push.lens.x, g_push.lens.y, 1.0, 1.0);
    s.tolerance = float4(0.0, 0.0, 0.0, 0.0);
    s.cap = 0.0;
    CityHitRay ray;
    ray.origin = origin;
    ray.direction = direction;
    ray.t = h.t;
    ray.footprint = footprint;
    const CityHitResolved r = CityHitResolve(p, f, s, ray, h.inst);
    if (r.source == CITY_HIT_SOURCE_NONE)
    {
        return false;  // no data for this hit: a miss, the environment stays
    }
    colour = r.colour;
    return true;
}

// The colour of the world at a direction (view space): the environment, what the mirror ray leaves behind (see the header). The game's environment map at the direction by the world's axes, read as the paint's sharp read
// is (rt_hit_sample.hlsli RtHitEnvAt: the map's own paraboloid) and in the colour target's units; without the map (a frame whose draws read none) the car's ambient and a sky gradient.
// `known` is false when the frame holds neither the map nor the shading's rows (the ambient is then zero, which would paint the glass black): the caller leaves the game's glass alone.
float3 MirrorWorld(float3 direction, out bool known)
{
    const RtHitFrame f = RtHitLoadFrame();
    known = (f.flags & (RT_HIT_FRAME_ENV | RT_HIT_FRAME_SHADE)) != 0u;
    if ((f.flags & RT_HIT_FRAME_ENV) != 0u)
    {
        return RtHitEnvAt(f, direction, kMirrorEnvLod) * g_push.tune.w;
    }
    return f.ambient * (0.6 + 0.4 * dot(direction, f.axisY)) * (f.gain * g_push.tune.w);
}

// A hit on a car's part (`t` along the mirror ray from `origin`), shaded from its material. False: the part has no usable normal or no material (the caller keeps what it had). `other`: another car's part
// (it fades into the world over the cars' reach), not the player's own. The footprint of the pixel at the hit: the pixel's angle times the distance travelled (the camera to the glass, then the ray).
bool MirrorCarHit(uint instanceIndex, uint primitiveIndex, float2 barycentrics, float3x4 objectToWorld, float3 origin, float3 mirror, float t, float glassT, out float3 shaded, out bool other)
{
    shaded = float3(0.0, 0.0, 0.0);
    other = false;
    const float3 hitNormal = CarRtHitNormal(instanceIndex, primitiveIndex, barycentrics, objectToWorld, mirror);
    if (!(dot(hitNormal, hitNormal) > 0.25))
    {
        return false;
    }
    other = (CarRtInstanceMask(CarRtLoadInstance(instanceIndex)) & CAR_RT_MASK_OTHERS) != 0u;
    const float3 hitPoint = origin + mirror * t;
    const float pixelAngle = 2.0 * g_push.lens.y / max(g_push.lens.w, 1.0);
    const float footprint = max(pixelAngle * (glassT + t), 1e-3);
    return CarHitShade(instanceIndex, primitiveIndex, barycentrics, hitNormal, mirror, hitPoint, footprint, 0.0, g_push.tune.w, -1.0, shaded);
}

// The mirror ray with the city (the push's city flags say on): the closest hit of the cars' parts and the city's triangles over the longer of the two reaches; the pixel is the environment `world`, the city
// over it by the city's weight, a car's part over that by its own (car_mirror_city_rules.h Compose, the twin). `colour` and `shown` come in as the environment's and go out as the pixel's.
//   - a city hit that shows (data in the pool, inside the reach) is the ray's answer; one that does not is a MISS: the environment stays
//   - a car's part past its reach (it weighs nothing) must not hide the city behind it: the ray is cast again for the city alone
//   - a car in its fade band lets what is behind it show: the city beyond, cast for alone, else the environment
//   - the car's own fade into the background is only where there is a background to fade into (the environment, or a city hit of full weight): else it weighs 1, as before (`(other && known)`)
void MirrorCityRay(float3 origin, float3 mirror, float glassT, float reach, float3 world, bool worldKnown, inout float3 colour, inout bool shown)
{
    const float pixelAngle = 2.0 * g_push.lens.y / max(g_push.lens.w, 1.0);
    const float footprint0 = pixelAngle * glassT;  // the cone's width at the glass: the pixel's, one pixel's angle at the glass's distance
    const float tanHalf = 0.5 * pixelAngle;
    const float cast = max(reach, asfloat(g_push.city.y));
    CityHitTraceResult h = MirrorTraceCity(origin, mirror, cast, g_push.image.w | kMirrorCityMask, footprint0, tanHalf);
    if (h.hit != 0u && !CityHitIsCity(h.inst.instanceId))
    {
        const bool farCar = (CarRtInstanceMask(CarRtLoadInstance(h.instanceIndex)) & CAR_RT_MASK_OTHERS) != 0u && 1.0 - smoothstep(g_push.tune.z * reach, reach, h.t) < kMirrorMinFade;
        if (farCar)
        {
            h = MirrorTraceCity(origin, mirror, cast, kMirrorCityMask, footprint0, tanHalf);
        }
    }
    if (h.hit == 0u)
    {
        return;  // nothing of the cars or the city in reach: the environment stays
    }
    float3 cityColour = float3(0.0, 0.0, 0.0);
    float cityFade = 0.0;
    bool cityShows = false;
    float3 shaded = float3(0.0, 0.0, 0.0);
    bool other = false;
    bool carShows = false;
    float carFade = 1.0;
    if (CityHitIsCity(h.inst.instanceId))
    {
        cityShows = MirrorCityHit(origin, mirror, h, max(pixelAngle * (glassT + h.t), 1e-3), cityColour, cityFade);
    }
    else
    {
        const float3x4 objectToWorld = float3x4(float4(h.inst.row0, 0.0), float4(h.inst.row1, 0.0), float4(h.inst.row2, 0.0));
        carShows = MirrorCarHit(h.instanceIndex, h.inst.primitiveIndex, h.inst.barycentrics, objectToWorld, origin, mirror, h.t, glassT, shaded, other);
        carFade = (carShows && other) ? 1.0 - smoothstep(g_push.tune.z * reach, reach, h.t) : 1.0;
        // what is behind a car that fades, or that has nothing to show: the city beyond it (the cars' structures end at their reach, the city's does not)
        if (!carShows || carFade < 1.0)
        {
            const CityHitTraceResult behind = MirrorTraceCity(origin, mirror, cast, kMirrorCityMask, footprint0, tanHalf);
            if (behind.hit != 0u)
            {
                cityShows = MirrorCityHit(origin, mirror, behind, max(pixelAngle * (glassT + behind.t), 1e-3), cityColour, cityFade);
            }
        }
    }
    // the background: the city over the environment by the city's weight; a car's part over that by its own weight
    float3 background = world;
    bool backgroundShown = worldKnown;
    if (cityShows)
    {
        background = lerp(world, cityColour, cityFade);
        backgroundShown = worldKnown || cityFade >= 1.0;
    }
    colour = background;
    shown = backgroundShown;
    if (carShows)
    {
        const float weight = (other && backgroundShown) ? carFade : 1.0;
        colour = lerp(background, shaded, weight);
        shown = backgroundShown || weight >= 1.0;
    }
}

// The pixel: rgb what the mirror shows, alpha the share of the game's glass it replaces (0 where the pixel is not mirror glass).
float4 MirrorPixel(uint2 pixel)
{
    const float2 render = g_push.lens.zw;
    uint hits = 0u;
    bool have = false;
    CarRtHit glass = (CarRtHit)0;
    float3 glassDirection = float3(0.0, 0.0, 1.0);
    [unroll]
    for (uint k = 0u; k < kMirrorTaps; ++k)
    {
        const float2 uv = (float2(pixel) + 0.5 + kMirrorTap[k]) / render;
        const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
        CarRtHit h;
        uint flags;
        if (MirrorTrace(float3(0.0, 0.0, 0.0), direction, kMirrorPrimaryReach, g_push.image.z, h, flags) && (flags & CAR_RT_FLAG_MIRROR) != 0u)
        {
            hits += 1u;
            if (!have)
            {
                have = true;
                glass = h;
                glassDirection = direction;
            }
        }
    }
    if (!have)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float coverage = float(hits) / float(kMirrorTaps);
    const float3 viewDirection = normalize(glassDirection);
    const float3 normal = CarRtHitNormal(glass.instance_index, glass.primitive_index, glass.barycentrics, glass.object_to_world, viewDirection);
    if (!(dot(normal, normal) > 0.25) || abs(dot(normal, viewDirection)) < kMirrorMinNv)
    {
        return float4(0.0, 0.0, 0.0, 0.0);  // no usable normal, or the glass seen edge on: the game's own picture stays
    }

    // the mirror ray
    const float glassT = glass.t;  // along the (non unit) direction, z = 1: the view z of the point
    const float3 glassPoint = glassDirection * glassT;
    const float3 mirror = reflect(viewDirection, normal);
    const float3 origin = glassPoint + normal * (kMirrorLift + kMirrorLiftPerMetre * glassT);
    const float reach = g_push.tune.y;
    bool worldKnown;
    const float3 world = MirrorWorld(mirror, worldKnown);
    float3 colour = world;
    bool shown = worldKnown;  // something real to show: the world, or a part of the player's own car
    [branch]
    if ((g_push.city.x & kMirrorCityOn) != 0u)
    {
        MirrorCityRay(origin, mirror, glassT, reach, world, worldKnown, colour, shown);
    }
    else
    {
        CarRtHit r;
        uint rflags;
        [branch]
        if (MirrorTrace(origin, mirror, reach, g_push.image.w, r, rflags))
        {
            float3 shaded;
            bool other;
            if (MirrorCarHit(r.instance_index, r.primitive_index, r.barycentrics, r.object_to_world, origin, mirror, r.t, glassT, shaded, other))
            {
                const float fade = (other && worldKnown) ? 1.0 - smoothstep(g_push.tune.z * reach, reach, r.t) : 1.0;
                colour = lerp(world, shaded, fade);
                shown = shown || fade >= 1.0;
            }
        }
    }
    if (!shown || any(isnan(colour)) || any(isinf(colour)))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return float4(min(max(colour, 0.0), float3(60000.0, 60000.0, 60000.0)), coverage * saturate(g_push.tune.x));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_push.rect.z || id.y >= g_push.rect.w)
    {
        return;
    }
    g_output[id.xy + g_push.image.xy] = MirrorPixel(id.xy + g_push.rect.xy);
}
