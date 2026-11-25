// MCLA GPU renderer -- the ray traced visibility of the top K lights: the shared maths of the trace (rt_lights.h, rt_lights_rules.h; perf/RT-DIRECT-SURVEY.md section 4 O1, RT-LIGHTS-D1).
//
// What is here has no resource of its own: it takes plain numbers and gives plain numbers back, so that src/gpu/rt_lights_test.cpp compiles THIS FILE as C++ (hlsl_cpp_shim.h) and checks every function
// against the twin in rt_lights_rules.h and against brute force. hlsl/rt_lights_trace_cs.hlsl is the pass that reads the buffers, traces and writes. Keep to the subset both languages read: scalars and
// float2 / float3, structs returned by value (no out parameters), the intrinsics abs sqrt min max clamp saturate dot normalize cross length frac cos sin.
//
// THE RAY. From a pixel's surface point P (view space of the top level: x right, y up, z forward; N the unit normal facing the camera) to a point of the light's DISC (radius R, facing the pixel), so
// that the visibility of the light is the share of the disc the pixel sees: a soft edge for free (survey O4). The ray starts lifted off the surface by RtvLift (the pixel's footprint), is kept
// above the surface's tangent plane (RtvMinAbove) and ends `stopShort` before the light (its own head and arm are in the structures: CHECK C10). A pair of lamps (a car's two headlamps) sends the
// ray to one of the two, picked by the noise: the average over the pixel's neighbours and frames is the average of the two views, which is what the atlas's views are combined by.
//
// THE NOISE (AGENTS.md AGENT-RULE-NOISE): every number a ray takes comes from the shared per-frame blue noise (blue_noise_frame.hlsli, the pass includes it) at its own SAMPLE INDEX
// RtvSampleIndex(frame, light, slot): the sample moves along the R2 sequence every frame, the same step for every pixel, so nothing is a fixed lattice and nothing slides; no two rays of a pixel's
// frame share a sample. The filter (RtDenoise: temporal + a-trous) resolves it.

#ifndef MCLA_RT_LIGHTS_COMMON_HLSLI
#define MCLA_RT_LIGHTS_COMMON_HLSLI

// ---- the numbers (rt_lights_rules.h) ----------------------------------------------------------------------------------------------------------

static const float kRtvMinNdotL = 0.02;
static const float kRtvMinAbove = 0.08;
static const float kRtvLiftFloor = 0.006;
static const float kRtvLiftPixels = 3.0;
static const float kRtvLiftCap = 0.4;
static const float kRtvTwoPi = 6.283185307179586;
static const float kRtvGolden = 0.6180339887;
static const uint kRtvMaxLights = 9u;
static const uint kRtvMaxRays = 4u;
static const uint kRtvSlots = 8u;          // sample indices a light has in a frame: the rays 0..3, 4 the pair's pick, 5 the coin of a non opaque triangle
static const uint kRtvSlotPair = 4u;
static const uint kRtvSlotCoin = 5u;
static const uint kRtvPhase = 2654435761u; // 0x9E3779B1: the golden ratio's 32 bit fraction
static const uint kRtvFlagSpot = 1u;       // rt_lights_rules.h kGpuFlagSpot
static const uint kRtvFlagPair = 2u;

// ---- the top level's space ---------------------------------------------------------------------------------------------------------------------

// A world point in the top level's view space: the world frame's axes are the WORLD's axes in view space (rt_world_frame.hlsli RtWorld::axisX..Z, `camera` the camera's place in the world), so
// a world vector (x, y, z) is axisX x + axisY y + axisZ z in view space.
float3 RtvWorldToView(float3 world, float3 camera, float3 axisX, float3 axisY, float3 axisZ)
{
    const float3 rel = world - camera;
    return axisX * rel.x + axisY * rel.y + axisZ * rel.z;
}

float3 RtvDirToView(float3 dir, float3 axisX, float3 axisY, float3 axisZ)
{
    return axisX * dir.x + axisY * dir.y + axisZ * dir.z;
}

// ---- which lamp, which pixels ---------------------------------------------------------------------------------------------------------------

// The lamp a ray goes to: the light's place, or, for a pair, one of pos + offset and pos - offset by the noise number `pick` in [0, 1).
float3 RtvLampPoint(float3 pos, float3 offset, bool pair, float pick)
{
    if (!pair)
    {
        return pos;
    }
    return pick < 0.5 ? pos + offset : pos - offset;
}

struct RtvReach
{
    bool covered;      // the light can light this surface point: inside its range and its cone, and facing it
    float distance;    // metres, pixel to light
    float3 toLight;    // unit, from the pixel
};

// Whether the light reaches the point P of the surface with the unit normal N (all view space): the same tests the light's term makes (light_laws.h LightScale: the range, the cone's outer
// cosine, N.L), because a light that adds nothing at a pixel has no use for a visibility there. A pixel for which this is false takes no ray and its visibility is 1.
RtvReach RtvReaches(float3 P, float3 N, float3 lampPos, float3 lampDir, float cosOuter, float range, bool spot)
{
    RtvReach r;
    r.covered = false;
    r.distance = 0.0;
    r.toLight = float3(0.0, 0.0, 0.0);
    const float3 to = lampPos - P;
    const float d = length(to);
    if (!(d > 0.001) || !(d < range))
    {
        return r;
    }
    const float3 u = to * (1.0 / d);
    if (!(dot(N, u) > kRtvMinNdotL))
    {
        return r;
    }
    if (spot && !(-dot(u, lampDir) > cosOuter))
    {
        return r;
    }
    r.covered = true;
    r.distance = d;
    r.toLight = u;
    return r;
}

// ---- the ray ---------------------------------------------------------------------------------------------------------------------------------

// An orthonormal frame about the unit vector n (Duff et al. 2017).
struct RtvBasis
{
    float3 t;
    float3 b;
};

RtvBasis RtvFrame(float3 n)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    RtvBasis r;
    r.t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    r.b = float3(c, s + n.y * n.y * a, -n.y);
    return r;
}

// The k-th of `rays` points of a unit disc: the disc's area stratified over the rays, turned by the two noise numbers.
float2 RtvDiscSample(uint k, uint rays, float noise1, float noise2)
{
    const float u = (float(k) + min(noise1, 0.9999)) / float(rays);
    const float r = sqrt(u);
    const float angle = kRtvTwoPi * frac(noise2 + kRtvGolden * float(k));
    return float2(r * cos(angle), r * sin(angle));
}

// How far the ray's origin is lifted off the surface along its normal: a floor and three pixels' footprint at the pixel's distance t, at most kRtvLiftCap. `footprintUnit` = 2 tan(half fov y) / height.
float RtvLift(float footprintUnit, float t)
{
    return min(kRtvLiftFloor + kRtvLiftPixels * footprintUnit * t, kRtvLiftCap);
}

struct RtvRay
{
    float3 origin;
    float3 direction;   // unit
    float tMax;         // metres along it: the end, `stopShort` before the disc's point
};

// The ray from the surface point `position` (lifted along `normal` by `lift`) to the point `disc` (a point of the unit disc) of the lamp's disc of radius `radius` at `lampPoint` (the disc faces
// the ray), ending `stopShort` short of it, the direction kept above the surface's tangent plane.
RtvRay RtvMakeRay(float3 position, float3 normal, float lift, float3 lampPoint, float radius, float2 disc, float stopShort)
{
    RtvRay r;
    r.origin = position + normal * lift;
    const float3 toCentre = lampPoint - r.origin;
    const float3 axis = normalize(toCentre);
    const RtvBasis basis = RtvFrame(axis);
    const float3 target = lampPoint + (basis.t * disc.x + basis.b * disc.y) * radius;
    const float3 toTarget = target - r.origin;
    const float distance = length(toTarget);
    float3 direction = toTarget * (1.0 / max(distance, 1e-6));
    const float above = dot(direction, normal);
    if (above < kRtvMinAbove)
    {
        // bent up to exactly kRtvMinAbove above the tangent plane, keeping its direction along the plane (a unit vector: the cosine with the normal is kRtvMinAbove, the rest along the tangent)
        const float3 tangent = direction - normal * above;
        const float tangentLength = length(tangent);
        direction = tangentLength > 1e-6 ? normal * kRtvMinAbove + tangent * (sqrt(1.0 - kRtvMinAbove * kRtvMinAbove) / tangentLength) : normal;
    }
    r.direction = direction;
    r.tMax = max(distance - stopShort, 0.0);
    return r;
}

// ---- the noise's sample index -----------------------------------------------------------------------------------------------------------------

// The sample of the blue noise a (frame, light, slot) takes: the frame's own number plus a constant phase for each (light, slot) (a multiple of the golden ratio's 32 bit fraction: odd, so no two
// (light, slot) of a frame share a sample, and the values they give differ by a fixed turn of the circle). The frame's step along the R2 sequence is the helper's own (the same for every pixel and
// every light: the frames' numbers keep the low discrepancy the helper proves, nothing slides). `slot` < kRtvSlots (a ray's number, kRtvSlotPair, kRtvSlotCoin).
uint RtvSampleIndex(uint frame, uint light, uint slot)
{
    return frame + (light * kRtvSlots + slot) * kRtvPhase;
}

// The coin of a non opaque candidate (a fence, a grate): the noise number plus the primitive's golden-ratio offset, in [0, 1): it blocks the ray when under one half. Changes every frame with the noise.
float RtvCoin(float noise, uint primitive)
{
    return frac(noise + float(primitive & 1023u) * kRtvGolden);
}

#endif  // MCLA_RT_LIGHTS_COMMON_HLSLI
