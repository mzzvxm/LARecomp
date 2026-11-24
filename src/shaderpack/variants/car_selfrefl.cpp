// LARecomp shader pack builder -- variant "car_selfrefl": the player's car paint reflects its own car (see car_selfrefl.h).

#include "shaderpack/variants/car_selfrefl.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "shaderpack/pack_format.h"
#include "shaderpack/variants/stage_constants.h"
#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

using namespace textedit;

namespace {

// The two environment reads of the paint and the last write of its colour, as the translator writes them.
constexpr std::string_view kSharpRead =
    "r10.xyz = tfetch2DGrad(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r1.yw, "
    "float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyz;";
constexpr std::string_view kBlurredRead =
    "r13.xyz = tfetch2D(dpSpecFrontSampler_Texture2DDescriptorIndex, dpSpecFrontSampler_SamplerDescriptorIndex, r0.xy, "
    "float2(0, 0)).xyz;";
constexpr std::string_view kFinalWrite = "oC0.xyz = r3.www * -r0.xzy + r0.xzy;";

// What follows each of them. The trace runs once, after the sharp read: the world position (iTexCoord1), the normal
// (iTexCoord2) and the view vector (iTexCoord6, with N.V in .w) are the vertex shader's outputs in the order the pixel
// shader receives them (the VS writes oTexCoord1.xyz = (x, y, z) of the world position, oTexCoord2 the world normal of
// the car's matrices, oTexCoord6 = (V, N.V) with V towards the camera: xCarPaintCustomizable__VS_CarPaint.hlsl).
constexpr std::string_view kAfterSharpRead =
    "\n\tcarRefl = CarReflTrace(iTexCoord1.xyz, iTexCoord2.xyz, iTexCoord6.xyz);"
    "\n\tr10.xyz = CarReflApply(r10.xyz, carRefl, 1.0);";
constexpr std::string_view kAfterBlurredRead = "\n\tr13.xyz = CarReflApply(r13.xyz, carRefl, 0.5);";
// Version 8: the clear coat's layer over the paint's finished colour (CarReflCoat: a reflection of another car at the coat's own fresnel), then the debug view, which replaces the colour;
// iTexCoord4.w is the baked vertex AO (r4.w of the game's lines).
constexpr std::string_view kAfterFinalWrite =
    "\n\toC0.xyz = CarReflCoat(oC0.xyz, carRefl);"
    "\n\t[branch] if (g_CarReflB.y > 0.5) { oC0.xyz = CarReflDebug(carRefl, iTexCoord4.w); }";

// The helper and its types, before main. The constants are declared by the two edits below it. (Three raw strings that join: the
// compiler's limit for one string literal is 16 KB.)
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant car_selfrefl (src/shaderpack/variants/car_selfrefl.cpp): the paint's two environment reads occluded by,
// or replaced with, the car's own colour of the previous frame. The history image (g_CarReflC.x, the picture's size or half of
// it): a the view distance of the pixels of a vehicle (0 for everything else), rgb the frame's scene colour of those pixels
// (0 elsewhere: the host pass reads the colour of a vehicle's pixels only, hlsl/car_history_ps.hlsl), both read as points
// (no filtering: a filtered read at a vehicle's edge would mix its colour and distance with the zeros of the world's).
// g_CarReflM0..M3 take a world position of this frame
// into the history frame's clip space (the vehicle's own motion is in them). g_CarReflA: x mode (0 off, 1 darken, 2 replace),
// y ray length (m), z thickness (m), w strength. g_CarReflB: x steps, y debug view, z colour gain, w the share of the ray's points
// that, all inside a part, take the whole hit.
// The numbers below are also written as plain functions in src/gpu/car_selfrefl_rules.h (RayDistance, SurfaceSlack, MakeWindowShape,
// WindowWeight, WindowIntegral, SegmentCover, Approach, InWindow, HasOutline, MayCover, OutlineShare, SegmentOf, RayFade, HitSum), which
// src/gpu/car_selfrefl_rules_test.cpp checks: keep the two in step.
static const float kCarReflStartMin = 0.04;   // m: no sample is nearer the shaded point than this
static const float kCarReflDark = 0.85;       // the most of the reflection a hit takes away (mode 1)
static const float kCarReflBackFloor = 0.5;   // the weight of a hit the ray reached from behind the surface, against 1 from the front
static const float kCarReflShade = 0.4;       // the paint's own colour in shade, for the face the camera does not see (mode 2)
static const float kCarReflFadeFrom = 0.3;    // the share of the ray's length from which what it finds weighs less...
static const float kCarReflFadeTo = 0.9;      // ...to nothing at this one
static const float kCarReflApproachSpan = 0.15;  // m: the length of ray a rise of a tenth of the thickness is measured over
static const int kCarReflOutlineSteps = 3;    // the halvings that find a part's outline in a segment: 1/8 of it

struct CarReflResult
{
    float3 colour;  // what the ray found, in the paint's own units
    float3 own;     // the shaded point's own colour of the previous frame, in the same units
    float hit;      // 0..1: the share of the reflection the hit takes over
    float face;     // 0..1: 1 the ray met the surface the camera sees from its front, 0 it reached it from behind (a hidden face)
    float dist;     // the hit's distance as a share of the ray length
    float state;    // debug: 0 off, 1 the shaded point is not in the history, 2 the ray left the picture, 3 no hit, 4 hit
    float rt;       // debug, version 5: 0 the ray traced image was not read, 1 it has no data here (the march ran), 2 it says no hit, 3 a hit on the player's own part (or nothing of another car), 4 on another car (version 8: the sign of the image's code)
    float coat;     // version 8: the weight of the hit that is on ANOTHER CAR (hit x the share of the image's weight with the sign): what the clear coat's layer takes by the fresnel
    float nv;       // version 8: N.V of the shaded point (the layer's fresnel)
};

// What the helpers need of the ray: the matrix to the history frame's clip space, the ray, the image's size, the slack of a slanted
// surface and the thickness.
struct CarReflRay
{
    float4 m0;
    float4 m1;
    float4 m2;
    float4 m3;
    float3 world;
    float3 direction;
    float2 size;
    float slack;
    float thickness;
    float reach;
    float3 shape;    // the ray's window (CarReflWindowShape: x bias, y ramp, z where the fall starts), made with the shaded point's distance
};

// A point of the ray as the history gives it.
struct CarReflPoint
{
    float t;         // distance along the ray
    float on;        // 1 where the history holds a vehicle pixel
    float diff;      // the ray point's view distance minus that pixel's: positive is behind it
    float stored;    // that pixel's view distance (0 where there is none)
    float2 uv;       // where it is in the picture
    float3 colour;   // that pixel's colour (0 off the vehicles)
};

struct CarReflSegment
{
    float cover;     // 0..1: the share of the segment inside the window
    float face;      // 1: the ray goes in at the visible face; 0: it comes back out of the hidden one
    float share;     // the share of the segment's colour that is the second end's
};

// What the segments of the ray add up to.
struct CarReflSum
{
    float covers;    // the sum of cover x fade x edge
    float seen;      // the same times the face
    float middle;    // the same times the middle's distance
    float3 colour;   // the same times the colour
};

float4 CarReflProject(float4 m0, float4 m1, float4 m2, float4 m3, float3 p)
{
    return p.x * m0 + p.y * m1 + p.z * m2 + m3;
}

float2 CarReflUv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float4 CarReflLoad(Texture2D<float4> history, float2 size, float2 uv)
{
    return history.Load(int3(int2(clamp(uv * size, float2(0.0, 0.0), size - 1.0)), 0));
}

// The distance of the ray's sample `index` (0 to steps - 1): the samples are closer together near the shaded point, where
// the parts that matter are (a wing over a trunk lid, a mirror over a door), and the same for every pixel and every frame.
float CarReflDistance(int index, int steps, float rayLength)
{
    const float u = float(index + 1) / float(steps);
    return max(rayLength * u * sqrt(u), kCarReflStartMin);
}

// How far off the stored distance of a slanted surface can be: the history is read as points, so it holds the surface's
// distance at a texel's centre, up to a texel from where the ray point projects, and a surface seen at a slant changes its
// distance by `slope` metres per metre across the picture. `focal` is the projection's scale (the matrix's y row).
float CarReflSlack(float depth, float historyHeight, float focal, float nv)
{
    const float texel = depth * 2.0 / (historyHeight * max(focal, 0.001));
    const float slope = min(sqrt(max(1.0 - nv * nv, 0.0)) / max(nv, 0.1), 8.0);
    return min(1.5 * texel * slope, 0.05);
}

)HLSL" R"HLSL(// The window of a ray (made with the shaded point's view distance `depth`): how much a ray point is "just behind" a vehicle pixel, as a
// function of diff (how far behind). It rises from 0 at the bias (the game's depth noise, and the slack of a slanted surface) over a short ramp to 1, stays there,
// and falls to 0 between half the thickness and the thickness (a ray that is far behind a thin shell has passed it). The two ramps
// never overlap (the bias is held under half the thickness less the ramp), so the window is the difference of two smoothsteps and has
// an exact integral.
float3 CarReflWindowShape(float depth, float thickness, float slack)
{
    const float ramp = min(0.02 + 0.004 * depth, 0.25 * thickness);
    const float bias = min(0.01 + 0.004 * depth + slack, 0.5 * thickness - ramp);
    return float3(bias, ramp, 0.5 * thickness);
}

float CarReflWindow(float diff, float3 shape, float thickness)
{
    return smoothstep(shape.x, shape.x + shape.y, diff) - smoothstep(shape.z, thickness, diff);
}

// The integral of smoothstep(edge0, edge1, y) for y up to x.
float CarReflRampIntegral(float edge0, float edge1, float x)
{
    const float width = edge1 - edge0;
    const float u = saturate((x - edge0) / width);
    return width * (u * u * u - 0.5 * u * u * u * u) + max(x - edge1, 0.0);
}

float CarReflWindowIntegral(float diff, float3 shape, float thickness)
{
    return CarReflRampIntegral(shape.x, shape.x + shape.y, diff) - CarReflRampIntegral(shape.z, thickness, diff);
}

// 0..1: how much a ray point has come nearer to the vehicle pixel than the previous point was, when that one was behind it
// (previousDiff > 0), from nothing to a tenth of the thickness per kCarReflApproachSpan of the ray's length (dt is the distance
// between the two points, so a segment cut in two by the search for an outline has the same face as the whole one). A ray that is
// coming out of the back of a surface towards its front reaches the surface from behind it, at the face the picture does not hold;
// a ray that goes deeper behind the surface went in at the front (0), and so did one that was in front of it at the previous point.
float CarReflApproach(float previousDiff, float diff, float dt, float thickness)
{
    return (previousDiff > 0.0) ? smoothstep(0.0, 0.1 * thickness, (previousDiff - diff) * (kCarReflApproachSpan / max(dt, 1e-3))) : 0.0;
}

float CarReflFade(float share)
{
    return 1.0 - smoothstep(kCarReflFadeFrom, kCarReflFadeTo, share);
}

// The point of the ray at distance t, read from the history. False where it is behind the camera or outside the picture.
bool CarReflSample(Texture2D<float4> history, CarReflRay ray, float t, out CarReflPoint p)
{
    p = (CarReflPoint)0;
    const float4 c = CarReflProject(ray.m0, ray.m1, ray.m2, ray.m3, ray.world + ray.direction * t);
    const float2 uv = CarReflUv(c);
    if (c.w < 0.1 || any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
    {
        return false;
    }
    const float4 texel = CarReflLoad(history, ray.size, uv);
    p.t = t;
    p.on = (texel.a > 0.0) ? 1.0f : 0.0f;
    p.diff = c.w - texel.a;
    p.stored = texel.a;
    p.uv = uv;
    p.colour = texel.rgb;  // a point read: the image's rgb is a vehicle's pixel or 0
    return true;
}

// 0..1: the fade towards the picture's edge.
float CarReflEdge(float2 uv)
{
    return saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 12.5);
}

// The point is in the window of a vehicle pixel (the window is open: diff is past the bias and short of the thickness): the side of
// an outline the ray's hit is on.
bool CarReflInWindow(CarReflPoint p, CarReflRay ray)
{
    return p.on > 0.5 && p.diff > ray.shape.x && p.diff < ray.thickness;
}

// Version 5: what the host's ray traced image (src/gpu/car_rt_refl.h, car_rt_refl_rules.h ReadForPaint is the same arithmetic) says at a
// place in the history. The image is the history's size: alpha 0 no data, 1 the player's car and none of the cone's rays hits it, 1 + w a hit
// on parts the camera sees (w the weight: the share of the cone the hits cover), -(1 + w) a hit on parts it does not (rgb the stand-in colour).
// Version 6: the place is read with a quadratic B-spline of 3x3 texels (the nearest texel and its eight neighbours, the weights of
// car_rt_refl_rules.h BSplineWeights) in place of the 2x2 bilinear read, whose weights have a corner at every texel and showed the
// half size image's blocks at a hit's edge; only the texels the history shows at the paint's own distance (the same surface) count, a
// texel with no data is left out, and the image answers where the texels with data are at least half of those that are that surface
// (else the march). The weight is the share of the footprint that hits, the colour the mean of the hits' (a miss's black does not darken a hit).
// Version 8: the code's sign is "mostly on ANOTHER CAR" (it was "mostly on parts the camera does not see"): `car` is the share of the weight with that sign over the footprint.
struct CarReflRtRead
{
    float use;       // 1: the image answers
    float weight;    // 0..1
    float3 colour;   // in the history's units
    float car;       // 0..1: the share of the weight that is on another car
    float state;     // 1 no data, 2 no hit, 3 a hit on the player's own part, 4 on another car
};

CarReflRtRead CarReflRtLookup(Texture2D<float4> history, float2 size, float2 uv, float depth, float tolerance, uint slot)
{
    CarReflRtRead r = (CarReflRtRead)0;
    r.state = 1.0;
    Texture2D<float4> image = g_Texture2DDescriptorHeap[TFETCH_INDEX(slot)];
    const float2 f = uv * size - 0.5;
    const float2 nearest = floor(f + 0.5);
    const float2 d = f - nearest;
    const float3 wx = float3(0.5 * (0.5 - d.x) * (0.5 - d.x), 0.75 - d.x * d.x, 0.5 * (0.5 + d.x) * (0.5 + d.x));
    const float3 wy = float3(0.5 * (0.5 - d.y) * (0.5 - d.y), 0.75 - d.y * d.y, 0.5 * (0.5 + d.y) * (0.5 + d.y));
    const int2 last = int2(size) - 1;
    float geometry = 0.0;
    float known = 0.0;
    float weight = 0.0;
    float car = 0.0;
    float3 colour = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 offset = int2(k % 3, k / 3) - 1;
        const int2 texel = clamp(int2(nearest) + offset, int2(0, 0), last);
        const float wb = wx[k % 3] * wy[k / 3];
        const float4 h = history.Load(int3(texel, 0));
        const float agree = (h.a > 0.0) ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(h.a - depth)) : 0.0;
        const float wg = wb * agree;
        geometry += wg;
        const float4 t = image.Load(int3(texel, 0));
        const float code = abs(t.a);
        if (code >= 0.5)
        {
            const float wt = saturate(code - 1.0);
            known += wg;
            weight += wg * wt;
            car += (t.a < 0.0) ? wg * wt : 0.0;
            colour += wg * wt * t.rgb;
        }
    }
    if (geometry > 1e-4 && known >= 0.5 * geometry)
    {
        r.use = 1.0;
        r.weight = weight / known;
        r.colour = (weight > 1e-5) ? colour / weight : float3(0.0, 0.0, 0.0);
        r.car = (weight > 1e-5) ? saturate(car / weight) : 0.0;
        // The f suffixes are needed: a ternary of bare literals is double precision in HLSL, and here dxc kept it as `select double` in the DXIL, which a GPU without
        // double precision (the Intel iGPUs) refuses to create (D3D12: "Shader uses double precision float ops"). Version 9.
        r.state = (r.weight <= 0.02) ? 2.0f : ((car > 0.5 * weight) ? 4.0f : 3.0f);
    }
    return r;
}

)HLSL" R"HLSL(// The ends disagree about the surface in a way a line in diff does not say: one is in a vehicle's window and the other not, and either
// the other is on no vehicle or the vehicle's distance steps by more than half the thickness between them (a part in front of a door).
// A segment like that has an outline in it.
bool CarReflHasOutline(CarReflPoint a, CarReflPoint b, CarReflRay ray)
{
    return CarReflInWindow(a, ray) != CarReflInWindow(b, ray) &&
           (!(a.on > 0.5 && b.on > 0.5) || abs(a.stored - b.stored) > 0.5 * ray.thickness);
}

// Can a segment between two points on a vehicle take anything: does the line in diff between them pass through the open window? (A
// shortcut: where it cannot, the cover is 0 and the integral need not be made.)
bool CarReflMayCover(CarReflPoint a, CarReflPoint b, CarReflRay ray)
{
    return a.on > 0.5 && b.on > 0.5 && max(a.diff, b.diff) > ray.shape.x && min(a.diff, b.diff) < ray.thickness;
}

// The segment from a to b (a nearer the shaded point) when both ends are on a vehicle: the mean of the window along the line in diff (the
// integral's difference over the diff's difference; one window for the whole ray). An end on no vehicle takes nothing: the outline of a
// part is looked for by the caller.
CarReflSegment CarReflSpan(CarReflPoint a, CarReflPoint b, CarReflRay ray)
{
    CarReflSegment s;
    s.cover = 0.0;
    s.face = 1.0;
    s.share = 1.0;
    if (a.on > 0.5 && b.on > 0.5)
    {
        const float de = b.diff - a.diff;
        const float safe = (abs(de) < 1e-4) ? 1.0 : de;
        const float along = (CarReflWindowIntegral(b.diff, ray.shape, ray.thickness) - CarReflWindowIntegral(a.diff, ray.shape, ray.thickness)) / safe;
        const float level = CarReflWindow(0.5 * (a.diff + b.diff), ray.shape, ray.thickness);
        s.cover = saturate((abs(de) < 1e-4) ? level : along);
        s.face = 1.0 - CarReflApproach(a.diff, b.diff, b.t - a.t, ray.thickness);
        const float wa = CarReflWindow(a.diff, ray.shape, ray.thickness);
        const float wb = CarReflWindow(b.diff, ray.shape, ray.thickness);
        s.share = (wa + wb > 1e-4) ? wb / (wa + wb) : 0.5;
    }
    return s;
}

void CarReflAdd(inout CarReflSum sum, CarReflSegment s, float aT, float bT, float edge, float3 aColour, float3 bColour, float rayLength)
{
    const float mid = 0.5 * (aT + bT);
    const float c = saturate(s.cover) * CarReflFade(mid / rayLength) * edge;
    sum.covers += c;
    sum.seen += c * s.face;
    sum.middle += c * mid;
    sum.colour += c * lerp(aColour, bColour, s.share);
}

// What the segment from a to b takes of the ray. A segment with an outline in it is halved kCarReflOutlineSteps times to find where
// (three more reads of the history, only for such segments), and takes the share of its length on the window's side of it: so a
// part's outline that moves across the segment moves the weight continuously, and a part narrower than the segment takes the share
// it is wide.
void CarReflSegmentOf(Texture2D<float4> history, CarReflRay ray, inout CarReflSum sum, CarReflPoint a, CarReflPoint b)
{
    if (CarReflHasOutline(a, b, ray))
    {
        CarReflPoint inEnd = a;
        CarReflPoint otherEnd = b;
        if (!CarReflInWindow(a, ray))
        {
            inEnd = b;
            otherEnd = a;
        }
        CarReflPoint inner = inEnd;
        CarReflPoint outer = otherEnd;
        [unroll]
        for (int k = 0; k < kCarReflOutlineSteps; ++k)
        {
            CarReflPoint middle;
            if (!CarReflSample(history, ray, 0.5 * (inner.t + outer.t), middle))
            {
                middle = outer;  // not reachable between two points inside the picture; no change if it were
            }
            if (CarReflInWindow(middle, ray))
            {
                inner = middle;
            }
            else
            {
                outer = middle;
            }
        }
        const float outline = 0.5 * (inner.t + outer.t);
        const float whole = abs(otherEnd.t - inEnd.t);
        const float share = (whole > 0.0) ? saturate(abs(outline - inEnd.t) / whole) : 0.0;
        CarReflPoint first = inner;
        CarReflPoint second = inEnd;
        if (inEnd.t < inner.t)
        {
            first = inEnd;
            second = inner;
        }
        CarReflSegment s = CarReflSpan(first, second, ray);
        s.cover *= share;
        CarReflAdd(sum, s, min(inEnd.t, outline), max(inEnd.t, outline), min(CarReflEdge(inEnd.uv), CarReflEdge(inner.uv)), first.colour, second.colour,
                   ray.reach);
    }
    else if (CarReflMayCover(a, b, ray))
    {
        CarReflAdd(sum, CarReflSpan(a, b, ray), a.t, b.t, min(CarReflEdge(a.uv), CarReflEdge(b.uv)), a.colour, b.colour, ray.reach);
    }
}

CarReflResult CarReflTrace(float3 world, float3 normal, float3 viewVector)
{
    CarReflResult result = (CarReflResult)0;
    result.face = 1.0;
    const float4 settings = g_CarReflA;
    if (settings.x < 0.5)
    {
        return result;
    }
    const float4 more = g_CarReflB;
    const uint4 slots = g_CarReflC;
    const float normalSquare = dot(normal, normal);
    const float viewSquare = dot(viewVector, viewVector);
    if (normalSquare < 1e-8 || viewSquare < 1e-8)
    {
        return result;
    }
    const float3 n = normal * rsqrt(normalSquare);
    const float3 v = viewVector * rsqrt(viewSquare);
    const float nv = dot(n, v);
    if (nv < 0.03)
    {
        return result;
    }
    result.nv = nv;

    CarReflRay ray;
    ray.m0 = g_CarReflM0;
    ray.m1 = g_CarReflM1;
    ray.m2 = g_CarReflM2;
    ray.m3 = g_CarReflM3;
    ray.world = world;
    ray.direction = 2.0 * nv * n - v;  // the view ray reflected: where the reflection looks
    Texture2D<float4> history = g_Texture2DDescriptorHeap[TFETCH_INDEX(slots.x)];
    ray.size = float2(getTexture2DDimensions(history));
    ray.thickness = settings.z;
    ray.reach = max(settings.y, 0.05);

    // 1. Is the shaded point in the history, at the distance it has now? A disocclusion, a wrong guess of the car's
    // motion or a cut puts it elsewhere: the whole effect fades with the disagreement. Its colour there is the paint's own.
    const float4 start = CarReflProject(ray.m0, ray.m1, ray.m2, ray.m3, world);
    result.state = 1.0;
    if (start.w < 0.1)
    {
        return result;
    }
    const float2 startUv = CarReflUv(start);
    if (any(startUv < float2(0.0, 0.0)) || any(startUv > float2(1.0, 1.0)))
    {
        return result;
    }
    const float4 own = CarReflLoad(history, ray.size, startUv);
    ray.slack = CarReflSlack(start.w, ray.size.y, length(float3(ray.m0.y, ray.m1.y, ray.m2.y)), nv);
    ray.shape = CarReflWindowShape(start.w, ray.thickness, ray.slack);
    const float tolerance = 0.06 + 0.02 * start.w + ray.slack;
    const float agreement = 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(own.a - start.w));
    if (!(own.a > 0.0) || agreement <= 0.0)
    {
        return result;
    }
    result.own = min(max(own.rgb, 0.0) * more.z, 64.0);

    // 1b. Versions 5 and 6: where the host ray traced this car's reflection of itself for the frame the history is of (g_CarReflC.w bit 0: the
    // image at g_CarReflC.z, the history's size), its answer at the shaded point's place in the history IS the hit, with the weight the
    // trace says (version 6: of a cone of rays, read with a 3x3 B-spline), and the march below is not done. Where the image has no data there (a pixel that is not the player's car by its triangles,
    // another vehicle) the march runs as before; with the bit clear this block does nothing and the output is version 4's.
    if ((slots.w & 1u) != 0u)
    {
        const CarReflRtRead rt = CarReflRtLookup(history, ray.size, startUv, start.w, 2.0 * (0.01 + 0.004 * start.w + ray.slack), slots.z);
        result.rt = rt.state;
        if (rt.use > 0.5)
        {
            result.colour = min(max(rt.colour, 0.0) * more.z, 64.0);
            result.face = 1.0;
            result.hit = rt.weight * agreement;
            result.coat = result.hit * rt.car;
            result.state = (result.hit > 0.02) ? 4.0f : 3.0f;
            return result;
        }
    }

    // 2. The march: `steps` points along the ray, the same for every pixel (a noise per pixel would make neighbours disagree
    // about a part narrower than the steps, a dotted patch; without it a pixel's answer follows the picture). The ray is a chain
    // of segments from point to point, the first from the shaded point (on the vehicle, diff 0, in no window). Each takes the
    // share of its length that is inside a part (CarReflSegmentOf); the hit is the sum, whole at `more.w` of the points.
    const int steps = clamp(int(more.x + 0.5), 4, 24);
    const float full = max(max(more.w, 0.05) * float(steps), 1.0);
    CarReflSum sum = (CarReflSum)0;
    CarReflPoint previous = (CarReflPoint)0;
    previous.on = 1.0;
    previous.stored = start.w;
    previous.uv = startUv;
    previous.colour = own.rgb;
    result.state = 3.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        CarReflPoint current;
        if (!CarReflSample(history, ray, CarReflDistance(i, steps, ray.reach), current))
        {
            result.state = 2.0;
            break;
        }
        CarReflSegmentOf(history, ray, sum, previous, current);
        previous = current;
        if (sum.covers * kCarReflBackFloor + sum.seen * (1.0 - kCarReflBackFloor) >= full)
        {
            break;
        }
    }

    // 3. The hit: its colour, face and distance are the covers' means, its weight their sum with the self-test.
    if (sum.covers > 0.0)
    {
        const float occupied = sum.covers * kCarReflBackFloor + sum.seen * (1.0 - kCarReflBackFloor);
        result.colour = min(max(sum.colour / sum.covers, 0.0) * more.z, 64.0);
        result.dist = sum.middle / (sum.covers * ray.reach);
        result.face = sum.seen / sum.covers;
        result.hit = saturate(occupied / full) * agreement;
        if (result.hit > 0.02)
        {
            result.state = 4.0;
        }
    }
    return result;
}

// What the ray sees: the colour of the vehicle pixel it met where that is the face the camera sees, the paint's own colour in
// shade where the ray reached the surface from behind (the hidden face of a body part is usually the same paint, unlit).
float3 CarReflSeen(CarReflResult hit)
{
    return lerp(hit.own * kCarReflShade, hit.colour, hit.face);
}

// The reflection `env` (the sharp read, share 1; the blurred one, share 0.5) with the hit applied. The weight already holds how
// sure the hit is, so a thin or hidden-face hit takes less of the reflection, whatever the mode.
float3 CarReflApply(float3 env, CarReflResult hit, float share)
{
    if (hit.hit <= 0.0)
    {
        return env;
    }
    const float4 settings = g_CarReflA;
    const float weight = saturate(hit.hit * settings.w * share);
    if (settings.x > 1.5)
    {
        return lerp(env, CarReflSeen(hit), weight);
    }
    return env * (1.0 - kCarReflDark * weight);
}

// Version 8 (RT-HIT-SHADING-4; car_rt_hit_rules.h CoatLayerFresnel, CoatLayerAmount, CoatLayerMix are the same arithmetic): the clear coat's LAYER over the paint's finished colour. The paint's own
// reflection of the environment is a few percent of what it reflects at a grazing view (a door seen along its plane); a real clear coat, a dielectric over the paint, reflects a car beside it
// at its own fresnel (Schlick, F0 0.04, power 5: 0.07 at N.V 0.5, 0.20 at 0.3, 0.36 at 0.2). For the reflection of ANOTHER CAR only (the ray traced image's sign) the layer takes
// that fresnel of the paint's colour by the hit's weight: lerp(paint, the car's colour, amount). Mode 2 only, and only where the host asks (g_CarReflC.w bit 1: mcla_gpu_car_rt_coat_layer).
static const float kCarReflCoatF0 = 0.04;      // the clear coat's reflectance facing the camera
static const float kCarReflCoatPower = 5.0;    // Schlick's exponent
static const float kCarReflCoatMax = 0.6;      // the most of the paint the layer takes

float CarReflCoatAmount(CarReflResult hit)
{
    const float fresnel = min(kCarReflCoatF0 + (1.0 - kCarReflCoatF0) * pow(saturate(1.0 - hit.nv), kCarReflCoatPower), kCarReflCoatMax);
    return saturate(hit.coat) * fresnel;
}

float3 CarReflCoat(float3 paint, CarReflResult hit)
{
    if ((g_CarReflC.w & 2u) == 0u || g_CarReflA.x < 1.5 || hit.coat <= 0.0)
    {
        return paint;
    }
    return lerp(paint, hit.colour, CarReflCoatAmount(hit));
}

// The debug views of g_CarReflB.y: 1 the hit weight, 2 the colour the ray sees (the ray traced reflection alone, at full intensity: no weight, no fresnel; black where it has no hit), 3 the paint's baked
// vertex AO, 4 the state of the trace (red: the point is not in the history, blue: the ray left the picture, green: no hit, yellow: a hit, orange where the march's hit is the hidden face; brighter the heavier),
// 5 the ray traced image's state at the pixel, 6 (version 8) the clear coat's layer's amount (the fresnel times the share of the hit on another car; white is 1), 7 (version 8) what the layer adds: the
// car's colour times that amount. The write of the colour is scaled by the exponent bias of the target, so they are in its units.
static const float kCarReflAmountViewGain = 64.0;  // debug view 6: the amount, times the target's unit, times this

float3 CarReflDebug(CarReflResult hit, float bakedAo)
{
    const uint view = uint(g_CarReflB.y + 0.5);
    const float unit = gInvColorExpBias.x;
    if (view == 1u)
    {
        return float3(hit.hit, hit.hit, hit.hit) * unit;
    }
    if (view == 2u)
    {
        return CarReflSeen(hit);
    }
    if (view == 3u)
    {
        return float3(bakedAo, bakedAo, bakedAo) * unit;
    }
    if (view == 6u)
    {
        // RT-HIT-SHADING-6: a script reads the edge's ramp off this picture, and 0.04 to 0.3 at the unit were 8-bit codes 2 to 4 (rthit4)
        const float amount = CarReflCoatAmount(hit);
        return float3(amount, amount, amount) * (unit * kCarReflAmountViewGain);
    }
    if (view == 7u)
    {
        return hit.colour * CarReflCoatAmount(hit);
    }
    if (view == 5u)
    {
        // The ray traced image at the paint's pixel: red the shaded point is not in the history, purple no image this frame, grey the image was
        // not read (a grazing view), cyan it has no data here (the march ran), green no hit, yellow a hit on the player's own part, orange on another car
        // (version 8; it was "on a part the camera does not see"); brighter the heavier.
        float3 tint = float3(0.0, 0.0, 0.0);
        if (hit.state > 0.5 && hit.state < 1.5)
        {
            tint = float3(1.0, 0.0, 0.0);
        }
        else if ((g_CarReflC.w & 1u) == 0u)
        {
            tint = float3(0.35, 0.0, 0.35);
        }
        else if (hit.rt < 0.5)
        {
            tint = float3(0.15, 0.15, 0.15);
        }
        else if (hit.rt < 1.5)
        {
            tint = float3(0.0, 0.5, 0.5);
        }
        else if (hit.rt < 2.5)
        {
            tint = float3(0.0, 0.5, 0.0);
        }
        else
        {
            tint = ((hit.rt > 3.5) ? float3(1.0, 0.3, 0.0) : float3(1.0, 1.0, 0.0)) * (0.25 + 0.75 * saturate(hit.hit));
        }
        return tint * unit;
    }
    float3 colour = float3(0.0, 0.0, 0.0);
    if (hit.state > 3.5)
    {
        colour = float3(1.0, 0.3 + 0.7 * hit.face, 0.0) * (0.25 + 0.75 * saturate(hit.hit));
    }
    else if (hit.state > 2.5)
    {
        colour = float3(0.0, 0.5, 0.0);
    }
    else if (hit.state > 1.5)
    {
        colour = float3(0.0, 0.0, 1.0);
    }
    else if (hit.state > 0.5)
    {
        colour = float3(1.0, 0.0, 0.0);
    }
    return colour * unit;
}
)HLSL";

}  // namespace

VariantTransformResult TransformCarSelfReflHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  VariantTransformResult result;
  if (shader.name != kCarPaint.name) {
    result.error = std::format("{} is not the car paint this variant was written for", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string error;

  // 1. The three lines the edits follow: each exactly once.
  if (!ReplaceAllExactly(text, kSharpRead, std::string(kSharpRead) + std::string(kAfterSharpRead), 1,
                         "the sharp environment read", &error) ||
      !ReplaceAllExactly(text, kBlurredRead, std::string(kBlurredRead) + std::string(kAfterBlurredRead), 1,
                         "the blurred environment read", &error) ||
      !ReplaceAllExactly(text, kFinalWrite, std::string(kFinalWrite) + std::string(kAfterFinalWrite), 1,
                         "the last write of the colour", &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // 2. The anchored edits: the local result, the constants of both backends and the helper.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tCarReflResult carRefl = (CarReflResult)0;";
                   },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  static constexpr ConstantMember kMembers[] = {{"float4", "g_CarReflM0"}, {"float4", "g_CarReflM1"}, {"float4", "g_CarReflM2"},
                                                {"float4", "g_CarReflM3"}, {"float4", "g_CarReflA"},  {"float4", "g_CarReflB"},
                                                {"uint4", "g_CarReflC"}};
  if (!DeclareConstants(text, ctx, "Car self-reflection variant (src/gpu/constants.h CarSelfReflConstants): 112 bytes after the 608.", kMembers, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // What the text must hold now: the helper's definitions and their calls, one declaration of each constant in
  // each backend's declaration, and the game's three lines nowhere without their edit. The history is read as points
  // (never a filtered sample), with one read of the image per ray point, three for a segment with an outline in it (in the one
  // function that samples a point) and one for the shaded point.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"CarReflResult CarReflTrace(", 1}, {"= CarReflTrace(", 1},
      {"float3 CarReflApply(", 1},        {"CarReflApply(r10.xyz, carRefl, 1.0)", 1},
      {"CarReflApply(r13.xyz, carRefl, 0.5)", 1},
      {"float3 CarReflDebug(", 1},        {"CarReflDebug(carRefl, iTexCoord4.w)", 1},
      {"CarReflResult carRefl = (CarReflResult)0;", 1},
      {"float3 CarReflCoat(", 1},         {"oC0.xyz = CarReflCoat(oC0.xyz, carRefl);", 1},
      {"float CarReflCoatAmount(", 1},
      {"history.SampleLevel(", 0},        {"history.Sample(", 0},
      {"CarReflLoad(history, ray.size, uv)", 1}, {"CarReflLoad(history, ray.size, startUv)", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      result.error = std::format("{}: the transformed HLSL has '{}' {} times (need {})", shader.name, check.needle,
                                 count, check.want);
      return result;
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeCarSelfReflVariant() {
  VariantDef def;
  def.feature = kFeatureCarSelfRefl;
  def.name = "car_selfrefl";
  def.version = kCarSelfReflVariantVersion;
  def.needs_rpf = true;  // xCarPaintCustomizable lives in xarchive_cache.rpf (cars)
  def.pixel_shaders = {kCarPaint};
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformCarSelfReflHlsl(shader, hlsl, ctx);
  };
  return def;
}

}  // namespace larecomp::shaderpack
