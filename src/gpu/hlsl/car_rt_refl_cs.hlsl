// MCLA GPU renderer -- the player's car reflecting itself, ray traced (car_rt_refl.h).
//
// Run at the history trigger (the game's mid-scene depth copy, car_selfrefl.h) right after the history image is made, when this
// frame's acceleration structures of the player's car exist (car_rt_scene.h: the top level is in VIEW space, x right, y up, z forward,
// the AO's: ao_common.hlsli AoViewPos). One thread is one texel of the history image (R16G16B16A16_FLOAT: rgb the scene colour of a
// vehicle's pixel, a its view distance, 0 for everything else; the picture's size over the divisor, 1 or 2) and writes the texel of
// the output image of the same size. What the output holds is in car_rt_refl_rules.h (one source of the numbers: this file keeps its
// own copy of them, change one, change the other):
//
//   alpha 0               no data (not the player's car, or no vehicle): the paint marches the history as before
//   alpha 1               the player's car, nothing of the car in the reflection's reach: the environment stays
//   alpha 1 + w           the reflection hits parts the camera sees, rgb their colour in the history; w the weight, the share of the cone's rays
//   alpha -(1 + w)        the reflection hits parts the camera does not see (or off the picture), rgb the surface's own colour in shade
//
// THE PRIMARY RAY goes from the camera through the texel's render pixel with the direction (ndc.x tanX, ndc.y tanY, 1), so a distance
// along it is the view distance t that the history holds. It looks only in a window around the history's distance (the tolerance of
// car_rt_refl_rules.h) at the opaque rigid parts: a hit there is the player's car at this pixel, and gives the point and the unit
// normal (CarRtHitNormal: the mesh's vertex normals, interpolated: the same normal the paint shades with). No hit, or a hit that is not
// at the history's distance once the slope is known: no data.
//
// THE CONE: from the point (put off the surface along the normal) kConeRays rays leave in directions spread over a cone of
// g_push.tune.w (the tangent of its half angle: the paint's gloss; 0 = a mirror and one ray) around reflect(view, normal), against
// body, glass and tyres (the push's parts). The pattern is two rings of equal area and four directions that add up to the mirror's, turned
// by an angle that is different at every texel (and, with a temporal upscaler running, at every frame): car_rt_refl_rules.h ConeDisc
// and ConeRotation. A ray that hits is taken into the SAME frame's history image: if the texels around its point show a surface at its
// distance it is a part the camera sees, and the hit's colour is theirs (an edge-aware bilinear mean: only the texels that are that surface
// count); if they do not (the point is on the back of a part, or hidden by another, or off the picture) the colour is the surface's own
// mean colour around the pixel in shade (kShade) and the weight is a little less (kHiddenWeight). The weight of a ray falls to nothing from
// 0.6 of its length to its end. A ray that hits the panel it left (the grazing self-hit: the ray runs under its own triangle's plane,
// which the mesh's smooth normal does not know) is a miss. The texel's weight is the sum of the rays' over the number of rays, its colour
// their weighted mean. After the first two rays (the outer pair) that found nothing the inner two are not cast.
//
// OTHER CARS (car_rt_instances_rules.h): the scene also holds the cars driving alongside (the instances' mask CAR_RT_MASK_OTHERS), which the reflection ray
// hits like any part (the push's parts) and the primary ray never does (the paint of the player's car is what it finds). A hit on one is taken into the
// history like any other (every vehicle's pixels are in it), so the part of another car the camera sees has its colour; a part it does not see has no
// colour of the pixel's own surface to stand in (that is the player's paint): it takes the colour the camera sees of THAT car, the mean of the history's
// vehicle texels around the car's body centre (OtherCarColour, the car's record in the instances' table), made dark by the same shade. Where the history
// shows nothing of the car (the game draws a car beside the player when its sphere reaches the view, though the camera sees none of it: its centre is behind the
// camera or off the picture), the car's PAINT stands in, from the game's customisation record (the car's second row, given by the model walk); a car with no
// paint colour makes a hidden hit a miss: the environment stays there. A car placed from memory or built from its own model data (the camera does not draw it:
// its record's radius is negative, car_rt_memory_rules.h) has no colour in the history at all and takes its paint, or a fixed grey, instead.
//
// ANOTHER CAR'S CHROME (car_chrome_rt_rules.h): the primary ray's mask (the push's z) also has CAR_RT_MASK_OTHER_CHROME, the bit of the instances of the other cars'
// draws of the chrome's shader (tailpipes, badges, trim). A texel whose primary hit is one of those is a pixel of that chrome: the pass traces the chrome variant's two
// probes for it as rays (ChromeTexel: the mirror ray 0.25 m, four sky rays 0.20 m, against every part of another car) and writes a texel of a kind of its own: alpha 1 + the
// reflection probe's weight, the colour's red -(1 + the sky probe's weight) (no colour is negative: that is how the chrome variant tells it from the player's texels), and
// an aux texel with no aux (so that the denoise copies it as it is). The paint of the other car (mask 0x10 alone) is not found by the primary ray: no data, its march.
//
// A SECOND OUTPUT, for the denoise that follows (car_rt_refl_denoise_cs.hlsl, car_rt_refl_rules.h "The denoise"): g_aux (RGBA8_UNORM, the same
// size) holds, for a texel with data, x the hit distance (sqrt of the mean over the rays that hit / the ray's length; 0 where none did), y the
// instance of the primary ray's hit / 255, z N.V of the surface, w 1; all 0 where the texel has no data.
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0:
// the history), set 1 the outputs (u0 the image, u1 the aux, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2), the push
// block (b0, space 3).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"
#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the image is R16G16B16A16_FLOAT, the aux
// image R8G8B8A8_UNORM (car_rt_refl.cpp EnsureOutput).
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);
#ifdef __spirv__
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> g_aux : register(u1, space1);

struct CarRtReflPush
{
    // x the history image's bindless slot, y the reflection ray's parts (CAR_RT_MASK_*), z the primary ray's parts (the player's body, and another car's chrome),
    // w the pattern's phase
    // (0, or 0 .. 7 by the frame where a temporal upscaler runs)
    uint4 slots;
    // xy the image's size in texels, zw the render size in pixels
    float4 image;
    // x, y tan of half the horizontal and the vertical field of view (the history's lens), z the image's divisor (1 or 2), w the
    // reflection ray's length in metres
    float4 lens;
    // x the shade (the surface's own colour in shade, 0.4), y the weight of a hit the history cannot show (0.85), z the share of the ray's
    // length where the fade starts (0.6), w the tangent of the cone's half angle (0: a mirror, one ray)
    float4 tune;
    // xyz the WORLD's up in view space (unit: the chrome's sky probe looks that way; the camera pitches), w the UNITS: the history's over the paint's, 2^exp_bias (16 at the
    // game's bias of 4; car_selfrefl_rules.h ExpBiasUnits). A colour this pass makes up from a paint constant (a car's paint, the grey of a car placed from memory) is in the
    // paint's units (a shader's output) and is multiplied by it to be in the history's, the units of the image the paint reads (it multiplies the image by 2^-bias).
    float4 up;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtReflPush> g_push;
#else
ConstantBuffer<CarRtReflPush> g_push : register(b0, space3);
#endif

static const float kMinNv = 0.03;
static const float kMinDepth = 0.05;
static const float kVisibleFullShare = 0.5;
static const float kMaxColour = 60000.0;
static const float kNoData = 0.0;
static const float kMinAbove = 0.03;         // a cone ray is kept at least this far (as a cosine) above the point's tangent plane
static const float kSelfCos = 0.85;          // a hit on the primary ray's instance that faces within 32 degrees of the point's normal is the panel itself
static const float kSurfaceSpacing = 2.0;    // texels between the taps of the surface's mean colour
static const float kOtherCarSpread = 0.5;    // radii of the other car between the taps of its colour (car_rt_refl_rules.h kOtherCarSpread)
static const float kOtherCarDepth = 1.2;     // radii: the depth window around its centre's distance
static const float kOtherCarMaxStep = 24.0;  // texels between those taps, at most
static const float kOtherCarEdge = 0.97;     // its centre is moved into this share of the picture
static const float kOtherCarMinShare = 0.15; // the share of the taps' weights under which the car has no colour
static const float kRememberedCarColour = 0.45; // the colour a car placed from memory shows when its paint is not known (car_rt_refl_rules.h kRememberedCarColour); the shade makes it darker still
static const float kTwoPi = 6.283185307179586;
// Another car's chrome (car_chrome_rt_rules.h: the same numbers; car_chrome_rules.h for the march's)
static const float kChromeReflectReach = 0.25;   // m: the mirror ray (the march's reflection probe)
static const float kChromeSkyReach = 0.2;        // m: the sky rays (the march's sky probe)
static const float kChromeFadeFrom = 0.3;        // the share of the reach up to which a hit weighs 1
static const float kChromeFadeTo = 0.9;          // ... and from which it weighs 0
static const int kChromeSkyRays = 4;             // the centre ray and three on a cone around it
static const float kChromeSkySpread = 0.6;       // tan of the half angle of that cone
static const float kChromeSkyUpMin = 0.2;        // |n + up| under this: the centre ray takes up
static const float kChromeLift = 0.002;          // m: the rays start this far off the surface ...
static const float kChromeLiftPerMetre = 0.0005; // ... plus this a metre of the point's view distance

// ---------------------------------------------------------------------------------------------------------------------
// the aux image (EncodeHitDistance, EncodeInstance of car_rt_refl_rules.h)
// ---------------------------------------------------------------------------------------------------------------------

// sqrt of the hit distance's share of the reach: more steps near the panel, where the cone is narrow; a hit never rounds to 0 (no hit).
float EncodeHitDistance(float distance, float reach)
{
    return distance > 0.0 ? max(sqrt(min(distance / max(reach, 1e-3), 1.0)), 1.0 / 255.0) : 0.0;
}

float EncodeInstance(uint instance)
{
    return float(min(instance, 255u)) / 255.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// the cone (ConeRadius, ConeOrder, ConeDisc, ConeRotation, ConeDirection of car_rt_refl_rules.h)
// ---------------------------------------------------------------------------------------------------------------------

static const float kConeRadius[4] = {0.5, 0.8660254037844386, 0.5, 0.8660254037844386};
static const int kConeOrder[4] = {1, 3, 0, 2};

// The angle the texel's pattern is turned by, in turns.
float ConeRotation(uint2 texel, uint phase)
{
    const float noise = frac(52.9829189 * frac(0.06711056 * float(texel.x) + 0.00583715 * float(texel.y)));
    return frac(noise + 0.6180339887 * float(phase));
}

// Ray `ray` of the cone around the mirror direction (both unit vectors in, a unit vector out), lifted to kMinAbove above the tangent plane.
float3 ConeDirection(float3 mirror, float3 normal, float tanHalf, int ray, float turns)
{
    const float3 up = abs(mirror.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(up, mirror));
    const float3 b = cross(mirror, t);
    const float angle = kTwoPi * (turns + 0.25 * float(ray));
    const float2 disc = kConeRadius[ray] * float2(cos(angle), sin(angle));
    float3 d = normalize(mirror + (t * disc.x + b * disc.y) * tanHalf);
    const float above = dot(d, normal);
    if (above < kMinAbove)
    {
        d = normalize(d + (kMinAbove - above) * normal);
    }
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// a ray's hit, taken into this frame's history
// ---------------------------------------------------------------------------------------------------------------------

// How much of the 2x2 footprint around the point q (view space) in the history image is the surface q is on, and the colour of the
// texels that are. `nv` is N.V of the surface at q seen from the camera.
float VisibleAt(Texture2D<float4> history, float3 q, float nv, out float3 colour)
{
    colour = float3(0.0, 0.0, 0.0);
    if (!(q.z > kMinDepth))
    {
        return 0.0;
    }
    const float2 ndc = float2(q.x / (q.z * g_push.lens.x), q.y / (q.z * g_push.lens.y));
    if (any(abs(ndc) > 1.0))
    {
        return 0.0;
    }
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    const float2 place = float2((ndc.x * 0.5 + 0.5) * render.x, (0.5 - ndc.y * 0.5) * render.y);
    // texel i is the render pixel floor((i + 0.5) divisor): centred at i * divisor + floor(divisor / 2) + 0.5
    const float2 f = (place - (floor(divisor * 0.5) + 0.5)) / divisor;
    const float2 base = floor(f);
    const float2 fr = f - base;
    const int2 last = int2(g_push.image.xy) - 1;
    const float texelMeters = PixelMeters(q.z, g_push.lens.y, g_push.image.y);
    const float tolerance = Tolerance(q.z, texelMeters, 1.0, nv);
    float coverage = 0.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const int2 offset = int2(k & 1, k >> 1);
        const int2 texel = clamp(int2(base) + offset, int2(0, 0), last);
        const float wb = (offset.x != 0 ? fr.x : 1.0 - fr.x) * (offset.y != 0 ? fr.y : 1.0 - fr.y);
        const float4 h = history.Load(int3(texel, 0));
        const float w = wb * MatchWeight(h.a, q.z, tolerance);
        coverage += w;
        sum += w * h.rgb;
    }
    if (coverage > 1e-6)
    {
        colour = sum / coverage;
    }
    return coverage;
}

// The mean colour of the pixel's own surface around it: a 3x3 of history texels kSurfaceSpacing apart, those within the distance tolerance,
// tent weighted (SurfaceMean). The fallback colour of a hit the history cannot show is this times the shade.
float3 SurfaceMean(Texture2D<float4> history, uint2 texel, float t, float nv)
{
    const float texelMeters = PixelMeters(t, g_push.lens.y, g_push.image.y);
    const float tolerance = Tolerance(t, texelMeters, kSurfaceSpacing, nv);
    const int2 last = int2(g_push.image.xy) - 1;
    float total = 0.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 tapOffset = int2(k % 3, k / 3) - 1;
        const int2 at = clamp(int2(texel) + tapOffset * int(kSurfaceSpacing), int2(0, 0), last);
        const float4 h = history.Load(int3(at, 0));
        const float tent = (tapOffset.x == 0 ? 1.0 : 0.5) * (tapOffset.y == 0 ? 1.0 : 0.5);
        const float w = tent * MatchWeight(h.a, t, tolerance);
        total += w;
        sum += w * h.rgb;
    }
    return total > 1e-6 ? sum / total : history.Load(int3(int2(texel), 0)).rgb;
}

// The colour of another car, for a hit on it that the history does not show (car_rt_refl_rules.h FindOtherCarColour has the same arithmetic and says why).
// Returns the share of the colour that is known (0 .. 1; under kOtherCarMinShare: no colour).
//   a car the camera does not draw (a negative radius: placed from memory, or built from its own model data): the history holds nothing of it; its colour is its PAINT
//   where the game's customisation record gave it (the car's second row: region 0's colour A), else a fixed mid grey; either is made darker by the shade like any hidden part.
//   a car the camera draws: the colour the camera sees of IT, nine history texels around the car's body centre, kOtherCarSpread radii apart, those that are a vehicle's
//   within kOtherCarDepth radii of the centre's distance, tent weighted. Where they hold too little (the car is beside or behind the camera: the game still draws a car
//   within a few metres of the player whose sphere reaches the view, and the camera sees none of it; or its centre is off the picture) the car's PAINT stands in,
//   where its record has one (the walk gave it): without a paint such a car's hidden hits are a miss.
float OtherCarSeenColour(Texture2D<float4> history, CarRtCar car, out float3 colour)
{
    colour = float3(0.0, 0.0, 0.0);
    if (!(car.radius > 0.0) || !(car.centre.z > kMinDepth))
    {
        return 0.0;  // no centre, or the centre is behind the camera: the history shows nothing round it
    }
    const float2 ndc = clamp(float2(car.centre.x / (car.centre.z * g_push.lens.x), car.centre.y / (car.centre.z * g_push.lens.y)),
                             float2(-kOtherCarEdge, -kOtherCarEdge), float2(kOtherCarEdge, kOtherCarEdge));
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    const float2 place = float2((ndc.x * 0.5 + 0.5) * render.x, (0.5 - ndc.y * 0.5) * render.y);
    // texel i is centred at i * divisor + floor(divisor / 2) + 0.5 (as VisibleAt)
    const int2 middle = int2(round((place - (floor(divisor * 0.5) + 0.5)) / divisor));
    const float texelMeters = PixelMeters(car.centre.z, g_push.lens.y, g_push.image.y);
    const int step = int(clamp(kOtherCarSpread * car.radius / max(texelMeters, 1e-6), 1.0, kOtherCarMaxStep));
    const float tolerance = kOtherCarDepth * car.radius;
    const int2 last = int2(g_push.image.xy) - 1;
    float total = 0.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 tapOffset = int2(k % 3, k / 3) - 1;
        const int2 at = clamp(middle + tapOffset * step, int2(0, 0), last);
        const float4 h = history.Load(int3(at, 0));
        const float tent = (tapOffset.x == 0 ? 1.0 : 0.5) * (tapOffset.y == 0 ? 1.0 : 0.5);
        const float w = tent * MatchWeight(h.a, car.centre.z, tolerance);
        total += w;
        sum += w * h.rgb;
    }
    if (total > 1e-6)
    {
        colour = sum / total;
    }
    return total * 0.25;
}

float OtherCarColour(Texture2D<float4> history, CarRtCar car, out float3 colour)
{
    // the paint and the grey are the paint's units, the history's (and so this image's) are 2^exp_bias times that (g_push.up.w; car_rt_refl_rules.h RememberedOtherCarColour)
    const float units = g_push.up.w;
    if (car.radius < 0.0)
    {
        colour = (car.has_paint ? car.paint : float3(kRememberedCarColour, kRememberedCarColour, kRememberedCarColour)) * units;
        return 1.0;
    }
    const float share = OtherCarSeenColour(history, car, colour);
    if (share < kOtherCarMinShare && car.has_paint)
    {
        colour = car.paint * units;
        return 1.0;
    }
    return share;
}

// One ray of the cone. False: a miss (nothing of the car in reach, a hit beyond the weight's fade, or the panel the ray left). True: `fade`
// the fade of its distance, `visible` how much of its hit the history shows (0..1), `seen` the colour the history shows there, `hitDistance`
// the distance along the ray (for the aux image). `other` is set for a hit on another car, and `hidden` is then the colour of the part the history
// does not show (the car's, darkened); for a hit on the player's own car the pass makes it from the pixel's own surface.
bool TraceRay(Texture2D<float4> history, float3 origin, float3 direction, float reach, uint originInstance, uint originPrimitive,
              float3 originNormal, out float fade, out float visible, out float3 seen, out float hitDistance, out bool other, out float3 hidden)
{
    fade = 0.0;
    visible = 0.0;
    seen = float3(0.0, 0.0, 0.0);
    hitDistance = 0.0;
    other = false;
    hidden = float3(0.0, 0.0, 0.0);
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.y, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;  // nothing of the car in the way: the environment stays
    }
    const float distance = query.CommittedRayT();
    const uint instance = query.CommittedInstanceIndex();
    const uint primitive = query.CommittedPrimitiveIndex();
    const float3 hitNormal = CarRtHitNormal(instance, primitive, query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), direction);
    const bool usable = dot(hitNormal, hitNormal) > 0.25;
    if (usable && instance == originInstance && (primitive == originPrimitive || dot(hitNormal, originNormal) > kSelfCos))
    {
        return false;  // the panel itself: the ray ran under its own surface (IsSelfHit)
    }
    const float faded = 1.0 - smoothstep(g_push.tune.z * reach, reach, distance);
    if (faded < 1.0 / 512.0)
    {
        return false;  // beyond the reach: a miss
    }
    const float3 hit = origin + direction * distance;
    const float hitNv = usable ? abs(dot(hitNormal, normalize(hit))) : 1.0;
    const float coverage = VisibleAt(history, hit, hitNv, seen);
    const float seenShare = saturate(coverage / kVisibleFullShare);
    const CarRtInstance hitInstance = CarRtLoadInstance(instance);
    if ((CarRtInstanceMask(hitInstance) & CAR_RT_MASK_OTHERS) != 0u)
    {
        // Another car: the part the history shows has its colour; the rest has the car's.
        other = true;
        if (seenShare < 1.0)
        {
            const float known = OtherCarColour(history, CarRtLoadCar(hitInstance), hidden);
            if (known < kOtherCarMinShare)
            {
                if (seenShare <= 0.0)
                {
                    return false;  // hidden, and the car's colour is not known: nothing (the environment stays)
                }
                hidden = seen * g_push.tune.x;  // partly shown: the shown colour stands in for the rest
            }
            else
            {
                hidden *= g_push.tune.x;
            }
        }
    }
    fade = faded;
    visible = seenShare;
    hitDistance = distance;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// another car's chrome (car_chrome_rt_rules.h)
// ---------------------------------------------------------------------------------------------------------------------

// One probe of the chrome's: the weight (0 a miss, up to 1) of the first hit within `reach` against every part of another car. A hit on the point's own triangle,
// or on its own instance on a surface that faces the way the point's normal does, is the panel itself (the ray ran under its own surface): a miss, as TraceRay.
float ChromeProbe(float3 origin, float3 direction, float reach, uint originInstance, uint originPrimitive, float3 originNormal)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, CAR_RT_MASK_OTHERS, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return 0.0;
    }
    const uint instance = query.CommittedInstanceIndex();
    const uint primitive = query.CommittedPrimitiveIndex();
    const float3 hitNormal = CarRtHitNormal(instance, primitive, query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), direction);
    if (dot(hitNormal, hitNormal) > 0.25 && instance == originInstance && (primitive == originPrimitive || dot(hitNormal, originNormal) > kSelfCos))
    {
        return 0.0;
    }
    return 1.0 - smoothstep(kChromeFadeFrom * reach, kChromeFadeTo * reach, query.CommittedRayT());
}

// The sky probe: the share of kChromeSkyRays rays that hit (each weighted by its distance): the centre ray along normalize(n + up) (up if the normal points straight
// down), and three on a cone of tan kChromeSkySpread around it 120 degrees apart, the cone's frame from the world's up (the x axis where the centre is within 25
// degrees of it), each kept at least kMinAbove above the surface's tangent plane (as ConeDirection).
float ChromeSky(float3 origin, float3 normal, float3 up, uint instance, uint primitive)
{
    float3 centre = normal + up;
    const float centreLength = length(centre);
    centre = centreLength < kChromeSkyUpMin ? up : centre / centreLength;
    const float3 helper = abs(dot(centre, up)) < 0.9 ? up : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(helper, centre));
    const float3 b = cross(centre, t);
    float sum = 0.0;
    [unroll]
    for (int k = 0; k < kChromeSkyRays; ++k)
    {
        float3 d = centre;
        if (k > 0)
        {
            const float angle = kTwoPi * float(k - 1) / 3.0;
            d = normalize(centre + (t * cos(angle) + b * sin(angle)) * kChromeSkySpread);
        }
        const float above = dot(d, normal);
        if (above < kMinAbove)
        {
            d = normalize(d + (kMinAbove - above) * normal);
        }
        sum += ChromeProbe(origin, d, kChromeSkyReach, instance, primitive, normal);
    }
    return sum / float(kChromeSkyRays);
}

// The texel of a pixel of another car's chrome, found by the primary ray at `hitPoint` (view space) with the mesh's unit normal facing the camera and N.V `nv`: alpha
// 1 + the mirror ray's weight (not cast at a grazing view: the paint's and the march's rule), the red -(1 + the sky probe's weight).
float4 ChromeTexel(float3 hitPoint, float3 viewDirection, float3 normal, float nv, uint instance, uint primitive)
{
    const float3 origin = hitPoint + normal * (kChromeLift + kChromeLiftPerMetre * hitPoint.z);
    float reflection = 0.0;
    if (nv >= kMinNv)
    {
        reflection = ChromeProbe(origin, reflect(viewDirection, normal), kChromeReflectReach, instance, primitive, normal);
    }
    const float sky = ChromeSky(origin, normal, g_push.up.xyz, instance, primitive);
    return float4(-(1.0 + sky), 0.0, 0.0, 1.0 + reflection);
}

// ---------------------------------------------------------------------------------------------------------------------
// one texel
// ---------------------------------------------------------------------------------------------------------------------

float4 TraceTexel(Texture2D<float4> history, uint2 texel, float4 own, out float4 aux)
{
    aux = float4(0.0, 0.0, 0.0, 0.0);
    const float t = own.a;
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    // the render pixel the history pass read for this texel (hlsl/car_history_ps.hlsl: int2(position.xy * divisor), position at the centre)
    const float2 pixel = min(floor((float2(texel) + 0.5) * divisor), render - 1.0);
    const float2 uv = (pixel + 0.5) / render;
    const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
    const float pixelMeters = PixelMeters(t, g_push.lens.y, render.y);

    // 1. the primary ray: the player's car at this pixel, within the window around the history's distance
    const float window = Tolerance(t, pixelMeters, 0.75, 0.0);
    RayDesc primary;
    primary.Origin = float3(0.0, 0.0, 0.0);
    primary.Direction = direction;
    primary.TMin = max(t - window, 0.001);
    primary.TMax = t + window;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> first;
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary);
    while (first.Proceed())
    {
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return float4(0.0, 0.0, 0.0, kNoData);
    }
    const float hitT = first.CommittedRayT();
    const uint primaryInstance = first.CommittedInstanceIndex();
    const uint primaryPrimitive = first.CommittedPrimitiveIndex();
    const float3 viewDirection = normalize(direction);
    const float3 normal = CarRtHitNormal(primaryInstance, primaryPrimitive, first.CommittedTriangleBarycentrics(),
                                         first.CommittedObjectToWorld3x4(), viewDirection);
    if (!(dot(normal, normal) > 0.25))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // no usable normal: the march
    }
    const float nv = abs(dot(normal, viewDirection));
    if (abs(hitT - t) > Tolerance(t, pixelMeters, 0.75, nv))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // a surface of the car, but not the one the picture shows here
    }
    // Another car's chrome (the primary ray's mask has the bit of its instances): the chrome's two probes, in a texel of a kind of its own and no aux for the denoise
    // (w 0: it copies the texel as it is; the instance 255 is no instance of the player's car, so no tap of the player's takes it for its own).
    if ((CarRtInstanceMask(CarRtLoadInstance(primaryInstance)) & CAR_RT_MASK_OTHER_CHROME) != 0u)
    {
        aux = float4(0.0, 1.0, 0.0, 0.0);
        return ChromeTexel(direction * hitT, viewDirection, normal, nv, primaryInstance, primaryPrimitive);
    }
    // the texel has data from here on: what the denoise needs to know about it (its hit distance below, where a ray hit)
    aux = float4(0.0, EncodeInstance(primaryInstance), saturate(nv), 1.0);
    if (nv < kMinNv)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // grazing: the paint does not trace it either (a miss: nothing changes)
    }

    // 2. the cone of reflection rays, off the surface along the normal
    const float3 mirror = reflect(viewDirection, normal);
    const float reach = g_push.lens.w;
    const float tanHalf = g_push.tune.w;
    const float rays = tanHalf > 0.0 ? 4.0 : 1.0;
    const float3 origin = direction * hitT + normal * (0.004 + 0.002 * hitT);
    const float turns = ConeRotation(texel, g_push.slots.w);
    float fade[4] = {0.0, 0.0, 0.0, 0.0};      // 0: the ray found nothing (or was not cast)
    float visible[4] = {0.0, 0.0, 0.0, 0.0};
    float3 seen[4] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    float distances[4] = {0.0, 0.0, 0.0, 0.0};
    float3 hidden[4] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    bool other[4] = {false, false, false, false};  // the ray hit another car: `hidden` is its colour for the part the history does not show
    bool found = false;
    [unroll]
    for (int slot = 0; slot < 4; ++slot)
    {
        if (slot >= 1 && !(tanHalf > 0.0))
        {
            break;  // a mirror: one ray
        }
        if (slot == 2 && !found)
        {
            break;  // the outer pair found nothing: the inner two are not cast
        }
        const int ray = kConeOrder[slot];
        const float3 direction2 = ConeDirection(mirror, normal, tanHalf, ray, turns);
        float f = 0.0;
        float v = 0.0;
        float3 c = float3(0.0, 0.0, 0.0);
        float d = 0.0;
        bool o = false;
        float3 hc = float3(0.0, 0.0, 0.0);
        if (TraceRay(history, origin, direction2, reach, primaryInstance, primaryPrimitive, normal, f, v, c, d, o, hc))
        {
            fade[ray] = f;
            visible[ray] = v;
            seen[ray] = c;
            distances[ray] = d;
            other[ray] = o;
            hidden[ray] = hc;
            found = true;
        }
    }
    if (!found)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // nothing of the car in the reflection: the environment stays
    }

    // 3. what the hits show: the history's colour where the camera sees them, the surface's own colour in shade where it does not
    bool needShade = false;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        needShade = needShade || (fade[k] > 0.0 && visible[k] < 1.0 && !other[k]);
    }
    float3 shade = float3(0.0, 0.0, 0.0);
    [branch]
    if (needShade)
    {
        shade = SurfaceMean(history, texel, t, nv) * g_push.tune.x;
    }
    float weightSum = 0.0;
    float visibleSum = 0.0;
    float3 colourSum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        const float w = fade[j] * (g_push.tune.y + (1.0 - g_push.tune.y) * visible[j]);
        const float3 standIn = other[j] ? hidden[j] : shade;  // what a part the history does not show looks like
        weightSum += w;
        visibleSum += w * visible[j];
        colourSum += w * (standIn + (seen[j] - standIn) * visible[j]);
    }
    const float weight = min(weightSum / rays, 1.0);
    if (weight < 1.0 / 512.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float3 colour = min(max(colourSum / weightSum, 0.0), kMaxColour);
    // the mean distance of the rays that hit: how wide the cone is where it meets the part (the denoise's strength)
    float distanceSum = 0.0;
    float hitCount = 0.0;
    [unroll]
    for (int m = 0; m < 4; ++m)
    {
        distanceSum += fade[m] > 0.0 ? distances[m] : 0.0;
        hitCount += fade[m] > 0.0 ? 1.0 : 0.0;
    }
    aux.x = EncodeHitDistance(distanceSum / max(hitCount, 1.0), reach);
    return float4(colour, visibleSum >= 0.5 * weightSum ? 1.0 + weight : -(1.0 + weight));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(g_push.image.xy);
    if (any(id.xy >= size))
    {
        return;
    }
    Texture2D<float4> history = g_textures[g_push.slots.x];
    const float4 own = history.Load(int3(id.xy, 0));
    float4 result = float4(0.0, 0.0, 0.0, kNoData);
    float4 aux = float4(0.0, 0.0, 0.0, 0.0);
    // Most of the image is no vehicle (alpha 0): one read and two writes. A wave with no vehicle texel in it never reaches the rays.
    if (own.a > 0.0 && own.a < 60000.0)
    {
        result = TraceTexel(history, id.xy, own, aux);
    }
    g_output[id.xy] = result;
    g_aux[id.xy] = aux;
}
