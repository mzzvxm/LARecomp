// LARecomp shader pack builder -- variant "car_chrome": the car's chrome under the car's own body (see car_chrome.h).

#include "shaderpack/variants/car_chrome.h"

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

// The environment read, the ambient and the last write of the colour of xChrome::PS_Chrome, as the translator writes them.
constexpr std::string_view kEnvRead =
    "r1.xyz = tfetch2DGrad(dpFrontSampler_Texture2DDescriptorIndex, dpFrontSampler_SamplerDescriptorIndex, r1.zw, "
    "float2(0, 0), tfetchGradientH.xy, tfetchGradientV.xy).xyz;";
constexpr std::string_view kAmbientLine = "r0.xyz = -r1.xyz + ambYPos.xyz;";
constexpr std::string_view kFinalWrite = "oC0.xyz = r0.www * r0.xyz;";

// What follows the environment read: the setup (the world position iTexCoord0.xyz, the world normal iTexCoord1.xyz and the vector to the
// camera, which is what the shader itself makes of them: r3.xyz = -r0.xyz + gViewInverse(3).xyz with r0 = iTexCoord0) and `env` through
// the occlusion. The ambient line takes the blend the game computes two lines later (r1.w = saturate(r2.y + r2.z)); r2.y and r2.z are set
// on the line before it.
constexpr std::string_view kAfterEnvRead =
    "\n\tcarChrome = CarChrSetup(iTexCoord0.xyz, iTexCoord1.xyz, gViewInverse(3).xyz - iTexCoord0.xyz);"
    "\n\tr1.xyz = CarChromeEnv(r1.xyz, carChrome);";
constexpr std::string_view kNewAmbientLine =
    "r0.xyz = -r1.xyz + CarChromeAmbient(ambYPos.xyz, carChrome, saturate(r2.y + r2.z));";
// The debug view: replaces the colour; the baked vertex AO is the vertex colour's alpha (iColor0.w; r2.w of the game's lines).
constexpr std::string_view kAfterFinalWrite =
    "\n\t[branch] if (g_CarReflB.y > 0.5) { oC0.xyz = CarChromeDebug(carChrome, iColor0.w); }";

// The helper and its types, before main. The constants are declared by the edits below it. (Three raw strings that join: the compiler's
// limit for one string literal is 16 KB.)
constexpr std::string_view kHelper = R"HLSL(// LARecomp variant car_chrome (src/shaderpack/variants/car_chrome.cpp): the chrome's environment reflection and its sky ambient occluded
// by the car's own parts, found with two short rays through the history image of the previous frame. The helper is the car paint
// variant's (car_selfrefl.cpp version 4: the window, the segments, the outline found by halving) with the numbers of parts that are
// centimetres apart and without the hit's colour: it makes two weights. The history (g_CarReflC.x): a the view distance of the pixels of
// a vehicle (0 elsewhere), rgb the scene colour, read as points. g_CarReflM0..M3 take a world position of this frame into the history
// frame's clip space; g_CarReflA.x is the mode (0 off), .w the strength; g_CarReflB.y the debug view; g_CarReflC.zw the ray traced image.
// The numbers are also written as plain functions in src/gpu/car_chrome_rules.h (RayDistance, Thickness, SurfaceSlack, MakeWindowShape,
// Agreement, SkyDirection, Occlude), checked by src/gpu/car_chrome_rules_test.cpp: keep the two in step.
static const float kCarChrReflectLength = 0.25;   // m: the reflection probe's reach
static const float kCarChrSkyLength = 0.2;        // m: the sky probe's reach
static const int kCarChrReflectSteps = 12;        // points on the reflection probe
static const int kCarChrSkySteps = 8;             // points on the sky probe
static const float kCarChrStartMin = 0.008;       // m: no point is nearer the shaded point than this
static const float kCarChrDark = 0.9;             // the most of `env` or of the ambient a hit takes away
static const float kCarChrFull = 0.25;            // the share of the points that, all inside a part, take the whole hit
static const float kCarChrBackFloor = 0.5;        // the weight of a hit the ray reached from behind the surface, against 1 from the front
static const float kCarChrFadeFrom = 0.3;         // the share of the ray's length from which what it finds weighs less...
static const float kCarChrFadeTo = 0.9;           // ...to nothing at this one
static const float kCarChrApproachSpan = 0.04;    // m: the length of ray a rise of a tenth of the thickness is measured over
static const float kCarChrNormalMin = 0.03;       // N.V under which the reflection probe is not made
static const int kCarChrOutlineSteps = 3;         // the halvings that find a part's outline in a segment: 1/8 of it
static const int kCarChrSilhouetteTaps = 12;      // the texels round the shaded point's own that the self-test also tries (a part's rim)
static const int2 kCarChrSilhouetteOffsets[12] = {int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1), int2(-1, -1), int2(1, -1),
                                                  int2(-1, 1), int2(1, 1), int2(-2, 0), int2(0, -2), int2(2, 0), int2(0, 2)};

// What the probes need of the shaded point: the matrix to the history frame's clip space, where it is, the image's size, its place in the
// history, the slack of a slanted surface, the thickness of "just behind a vehicle pixel" and the window made with them.
struct CarChrRay
{
    float4 m0;
    float4 m1;
    float4 m2;
    float4 m3;
    float3 world;
    float2 size;
    float2 startUv;
    float depth;
    float slack;
    float thickness;
    float3 shape;    // the window (CarChrWindowShape: x bias, y ramp, z where the fall starts)
};

// A point of a probe as the history gives it.
struct CarChrPoint
{
    float t;         // distance along the ray
    float on;        // 1 where the history holds a vehicle pixel
    float diff;      // the ray point's view distance minus that pixel's: positive is behind it
    float stored;    // that pixel's view distance (0 where there is none)
    float2 uv;       // where it is in the picture
};

struct CarChrSegment
{
    float cover;     // 0..1: the share of the segment inside the window
    float face;      // 1: the ray goes in at the visible face; 0: it comes back out of the hidden one
};

struct CarChrSum
{
    float covers;    // the sum of cover x fade x edge
    float seen;      // the same times the face
};

// What the setup learned of the pixel and what the probes found.
struct CarChrState
{
    float valid;       // 1: the shaded point is in the history at its own distance: the probes can run
    float agreement;   // 0..1: how well it finds itself there
    float3 n;          // the unit normal (0: none)
    float nv;          // N.V
    CarChrRay ray;
    float reflectHit;  // 0..1: the reflection probe's weight
    float skyHit;      // 0..1: the sky probe's weight
    float skyKnown;    // 1: skyHit came from the ray traced image (another car's chrome): the sky probe is not marched
    float state;       // debug: 0 off, 1 not in the history, 3 marched and no hit, 4 marched and hit, 5 the ray traced image answered (the player's car),
                       // 6 it answered both probes (another car's chrome)
};

float4 CarChrProject(float4 m0, float4 m1, float4 m2, float4 m3, float3 p)
{
    return p.x * m0 + p.y * m1 + p.z * m2 + m3;
}

float2 CarChrUv(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

float4 CarChrLoad(Texture2D<float4> history, float2 size, float2 uv)
{
    return history.Load(int3(int2(clamp(uv * size, float2(0.0, 0.0), size - 1.0)), 0));
}

// The distance of the probe's point `index` (0 to steps - 1): closer together near the shaded point, the same for every pixel and frame.
float CarChrDistance(int index, int steps, float rayLength)
{
    const float u = float(index + 1) / float(steps);
    return max(rayLength * u * sqrt(u), kCarChrStartMin);
}

// How far off the stored distance of a slanted surface can be: the history is read as points, so it holds the surface's distance at a
// texel's centre, up to a texel from where a ray point projects, and a surface seen at a slant changes its distance by `slope` metres per
// metre across the picture. `focal` is the projection's scale (the matrix's y row).
float CarChrSlack(float depth, float historyHeight, float focal, float nv)
{
    const float texel = depth * 2.0 / (historyHeight * max(focal, 0.001));
    const float slope = min(sqrt(max(1.0 - nv * nv, 0.0)) / max(nv, 0.1), 8.0);
    return min(1.5 * texel * slope, 0.03);
}

// How thick "just behind a vehicle pixel" is at a view distance: the parts to find are 1 to 10 cm apart.
float CarChrThickness(float depth)
{
    return 0.08 + 0.006 * depth;
}

// The window of a probe (made with the shaded point's view distance `depth`): how much a ray point is "just behind" a vehicle pixel, as a
// function of diff (how far behind). It rises from 0 at the bias (the history's noise, and the slack of a slanted surface) over a short
// ramp to 1, stays there, and falls to 0 between half the thickness and the thickness. The two ramps never overlap (the bias is held under
// half the thickness less the ramp), so the window is the difference of two smoothsteps and has an exact integral.
float3 CarChrWindowShape(float depth, float thickness, float slack)
{
    const float ramp = min(0.006 + 0.0015 * depth, 0.25 * thickness);
    const float bias = min(0.004 + 0.0015 * depth + slack, 0.5 * thickness - ramp);
    return float3(bias, ramp, 0.5 * thickness);
}

float CarChrWindow(float diff, float3 shape, float thickness)
{
    return smoothstep(shape.x, shape.x + shape.y, diff) - smoothstep(shape.z, thickness, diff);
}

// The integral of smoothstep(edge0, edge1, y) for y up to x.
float CarChrRampIntegral(float edge0, float edge1, float x)
{
    const float width = edge1 - edge0;
    const float u = saturate((x - edge0) / width);
    return width * (u * u * u - 0.5 * u * u * u * u) + max(x - edge1, 0.0);
}

float CarChrWindowIntegral(float diff, float3 shape, float thickness)
{
    return CarChrRampIntegral(shape.x, shape.x + shape.y, diff) - CarChrRampIntegral(shape.z, thickness, diff);
}

// 0..1: how much a ray point has come nearer to the vehicle pixel than the previous point was, when that one was behind it
// (previousDiff > 0), from nothing to a tenth of the thickness per kCarChrApproachSpan of the ray's length (dt is the distance between the
// two points, so a segment cut in two by the search for an outline has the same face as the whole one). A ray that is coming out of the
// back of a surface towards its front reaches the surface from behind it, at the face the picture does not hold; a ray that goes deeper
// behind the surface went in at the front (0), and so did one that was in front of it at the previous point.
float CarChrApproach(float previousDiff, float diff, float dt, float thickness)
{
    return (previousDiff > 0.0) ? smoothstep(0.0, 0.1 * thickness, (previousDiff - diff) * (kCarChrApproachSpan / max(dt, 1e-3))) : 0.0;
}

float CarChrFade(float share)
{
    return 1.0 - smoothstep(kCarChrFadeFrom, kCarChrFadeTo, share);
}

// The point of the probe at distance t, read from the history. False where it is behind the camera or outside the picture.
bool CarChrSample(Texture2D<float4> history, CarChrRay ray, float3 direction, float t, out CarChrPoint p)
{
    p = (CarChrPoint)0;
    const float4 c = CarChrProject(ray.m0, ray.m1, ray.m2, ray.m3, ray.world + direction * t);
    const float2 uv = CarChrUv(c);
    if (c.w < 0.1 || any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
    {
        return false;
    }
    const float4 texel = CarChrLoad(history, ray.size, uv);
    p.t = t;
    p.on = (texel.a > 0.0) ? 1.0 : 0.0;
    p.diff = c.w - texel.a;
    p.stored = texel.a;
    p.uv = uv;
    return true;
}

// 0..1: the fade towards the picture's edge.
float CarChrEdge(float2 uv)
{
    return saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 12.5);
}

// The point is in the window of a vehicle pixel (the window is open: diff is past the bias and short of the thickness): the side of an
// outline the probe's hit is on.
bool CarChrInWindow(CarChrPoint p, CarChrRay ray)
{
    return p.on > 0.5 && p.diff > ray.shape.x && p.diff < ray.thickness;
}

)HLSL" R"HLSL(// The ends disagree about the surface in a way a line in diff does not say: one is in a vehicle's window and the other not, and either
// the other is on no vehicle or the vehicle's distance steps by more than half the thickness between them (a part in front of another).
// A segment like that has an outline in it.
bool CarChrHasOutline(CarChrPoint a, CarChrPoint b, CarChrRay ray)
{
    return CarChrInWindow(a, ray) != CarChrInWindow(b, ray) &&
           (!(a.on > 0.5 && b.on > 0.5) || abs(a.stored - b.stored) > 0.5 * ray.thickness);
}

// Can a segment between two points on a vehicle take anything: does the line in diff between them pass through the open window? (A
// shortcut: where it cannot, the cover is 0 and the integral need not be made.)
bool CarChrMayCover(CarChrPoint a, CarChrPoint b, CarChrRay ray)
{
    return a.on > 0.5 && b.on > 0.5 && max(a.diff, b.diff) > ray.shape.x && min(a.diff, b.diff) < ray.thickness;
}

// The segment from a to b (a nearer the shaded point) when both ends are on a vehicle: the mean of the window along the line in diff (the
// integral's difference over the diff's difference; one window for the whole probe). An end on no vehicle takes nothing: the outline of a
// part is looked for by the caller.
CarChrSegment CarChrSpan(CarChrPoint a, CarChrPoint b, CarChrRay ray)
{
    CarChrSegment s;
    s.cover = 0.0;
    s.face = 1.0;
    if (a.on > 0.5 && b.on > 0.5)
    {
        const float de = b.diff - a.diff;
        const float safe = (abs(de) < 1e-4) ? 1.0 : de;
        const float along = (CarChrWindowIntegral(b.diff, ray.shape, ray.thickness) - CarChrWindowIntegral(a.diff, ray.shape, ray.thickness)) / safe;
        const float level = CarChrWindow(0.5 * (a.diff + b.diff), ray.shape, ray.thickness);
        s.cover = saturate((abs(de) < 1e-4) ? level : along);
        s.face = 1.0 - CarChrApproach(a.diff, b.diff, b.t - a.t, ray.thickness);
    }
    return s;
}

void CarChrAdd(inout CarChrSum sum, CarChrSegment s, float aT, float bT, float edge, float reach)
{
    const float mid = 0.5 * (aT + bT);
    const float c = saturate(s.cover) * CarChrFade(mid / reach) * edge;
    sum.covers += c;
    sum.seen += c * s.face;
}

// What the segment from a to b takes of the probe. A segment with an outline in it is halved kCarChrOutlineSteps times to find where (three
// more reads of the history, only for such segments), and takes the share of its length on the window's side of it: so a part's outline that
// moves across the segment moves the weight continuously, and a part narrower than the segment takes the share it is wide.
void CarChrSegmentOf(Texture2D<float4> history, CarChrRay ray, float3 direction, float reach, inout CarChrSum sum, CarChrPoint a, CarChrPoint b)
{
    if (CarChrHasOutline(a, b, ray))
    {
        CarChrPoint inEnd = a;
        CarChrPoint otherEnd = b;
        if (!CarChrInWindow(a, ray))
        {
            inEnd = b;
            otherEnd = a;
        }
        CarChrPoint inner = inEnd;
        CarChrPoint outer = otherEnd;
        [unroll]
        for (int k = 0; k < kCarChrOutlineSteps; ++k)
        {
            CarChrPoint middle;
            if (!CarChrSample(history, ray, direction, 0.5 * (inner.t + outer.t), middle))
            {
                middle = outer;  // not reachable between two points inside the picture; no change if it were
            }
            if (CarChrInWindow(middle, ray))
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
        CarChrPoint first = inner;
        CarChrPoint second = inEnd;
        if (inEnd.t < inner.t)
        {
            first = inEnd;
            second = inner;
        }
        CarChrSegment s = CarChrSpan(first, second, ray);
        s.cover *= share;
        CarChrAdd(sum, s, min(inEnd.t, outline), max(inEnd.t, outline), min(CarChrEdge(inEnd.uv), CarChrEdge(inner.uv)), reach);
    }
    else if (CarChrMayCover(a, b, ray))
    {
        CarChrAdd(sum, CarChrSpan(a, b, ray), a.t, b.t, min(CarChrEdge(a.uv), CarChrEdge(b.uv)), reach);
    }
}

// The weight of a probe from the shaded point along `direction`: `steps` points up to `reach` metres, the same for every pixel (a noise per
// pixel would make neighbours disagree about a part narrower than the steps), a chain of segments from the shaded point (on the vehicle, in
// no window). The hit is the sum of the segments' covers, whole at kCarChrFull of the points, times how well the shaded point is in the
// history.
float CarChrProbe(Texture2D<float4> history, CarChrState s, float3 direction, int steps, float reach)
{
    const float full = max(kCarChrFull * float(steps), 1.0);
    CarChrSum sum = (CarChrSum)0;
    CarChrPoint previous = (CarChrPoint)0;
    previous.on = 1.0;
    previous.stored = s.ray.depth;
    previous.uv = s.ray.startUv;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        CarChrPoint current;
        if (!CarChrSample(history, s.ray, direction, CarChrDistance(i, steps, reach), current))
        {
            break;
        }
        CarChrSegmentOf(history, s.ray, direction, reach, sum, previous, current);
        previous = current;
        if (sum.covers * kCarChrBackFloor + sum.seen * (1.0 - kCarChrBackFloor) >= full)
        {
            break;
        }
    }
    const float occupied = sum.covers * kCarChrBackFloor + sum.seen * (1.0 - kCarChrBackFloor);
    return saturate(occupied / full) * s.agreement;
}

// The ray traced image (src/gpu/car_rt_refl.h, variant 6 of the paint: g_CarReflC.z its slot, the history's size):
// alpha 0 no data, 1 the car and no ray of the cone hits it, 1 + w a hit of weight w (the share of the cone), -(1 + w) a hit on a part the
// camera does not see. Read at the shaded point's place in the history with a 3x3 quadratic B-spline; only the texels the history shows at
// the shaded point's own distance (the same surface) count; the image answers where the texels with data are at least half of those.
// TWO KINDS OF TEXEL (src/gpu/car_chrome_rt_rules.h): the player's (the colour's red is not negative: the alpha above) and another car's chrome (the colour's
// red is -(1 + the sky probe's weight), the alpha 1 + the reflection probe's weight: the host traced both probes for it). The image answers with the first
// kind where its texels are at least half of the matching ones, else with the second where they are (the first kind's arithmetic is exactly the older one's).
// x = 1: it answers; y = the weight of a hit (the share of the cone, a miss's zero included, or the reflection probe's); z = 1: another car's chrome, and
// w = the sky probe's weight (else 0).
float4 CarChrRtRead(Texture2D<float4> history, float2 size, float2 uv, float depth, float tolerance, uint slot)
{
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
    float knownOther = 0.0;
    float weightOther = 0.0;
    float skyOther = 0.0;
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
            if (t.r < -0.5)
            {
                knownOther += wg;
                weightOther += wg * saturate(code - 1.0);
                skyOther += wg * saturate(-t.r - 1.0);
            }
            else
            {
                known += wg;
                weight += wg * saturate(code - 1.0);
            }
        }
    }
    if (geometry > 1e-4 && known >= 0.5 * geometry)
    {
        return float4(1.0, weight / known, 0.0, 0.0);
    }
    if (geometry > 1e-4 && knownOther >= 0.5 * geometry)
    {
        return float4(1.0, weightOther / knownOther, 1.0, skyOther / knownOther);
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}

)HLSL" R"HLSL(// The self-test's agreement of one texel's stored distance with the shaded point's: 1 within half the tolerance, 0 beyond it, 0 where the texel holds no vehicle.
float CarChrTexelAgreement(float stored, float depth, float tolerance)
{
    return (stored > 0.0) ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(stored - depth)) : 0.0;
}

// The rim of a part: the history holds one sample of a pixel (the guest's sample 0; at 2560 pixels across and more one texel stands for a 2x2 block), the
// shaded pixel is made at its centre, so at the silhouette of a tip the texel under the pixel can be what is behind the tip (the bumper's cavity, 3 to 7 cm
// further; or no vehicle) and the self-test would fail on the pixel's whole 1 pixel rim: the game's chrome with its bright sky ambient, a thin bright line.
// The texels round it (one or two steps; the offsets are car_chrome_rules.h's kSilhouetteOffsets) are tried as well: the best agreement is the pixel's. Only a
// pixel whose own texel does not agree fully makes the 12 reads (a few percent of the chrome's pixels), and one whose neighbours do not agree either stays at
// the 0 it had.
float CarChrSilhouetteAgreement(Texture2D<float4> history, float2 size, float2 uv, float depth, float tolerance, float own)
{
    float best = own;
    [branch]
    if (own < 1.0)
    {
        const int2 centre = int2(clamp(uv * size, float2(0.0, 0.0), size - 1.0));
        const int2 last = int2(size) - 1;
        [unroll]
        for (int k = 0; k < kCarChrSilhouetteTaps; ++k)
        {
            const float stored = history.Load(int3(clamp(centre + kCarChrSilhouetteOffsets[k], int2(0, 0), last), 0)).a;
            best = max(best, CarChrTexelAgreement(stored, depth, tolerance));
        }
    }
    return best;
}

// The setup, at the environment read: is the shaded point in the history at its own view distance, and the reflection probe (the view ray
// reflected, 2 (N.V) N - V: where the chrome looks), made here because `env` is occluded at once. A point the history does not hold (the
// first frames, a cut, a pause view the host sends mode 0 for, a disocclusion, a part that is not a vehicle pixel there) is left to the game.
CarChrState CarChrSetup(float3 world, float3 normal, float3 viewVector)
{
    CarChrState s = (CarChrState)0;
    const float4 settings = g_CarReflA;
    if (settings.x < 0.5)
    {
        return s;
    }
    const uint4 slots = g_CarReflC;
    const float normalSquare = dot(normal, normal);
    const float viewSquare = dot(viewVector, viewVector);
    if (normalSquare < 1e-8 || viewSquare < 1e-8)
    {
        return s;
    }
    s.n = normal * rsqrt(normalSquare);
    const float3 v = viewVector * rsqrt(viewSquare);
    s.nv = dot(s.n, v);
    s.ray.m0 = g_CarReflM0;
    s.ray.m1 = g_CarReflM1;
    s.ray.m2 = g_CarReflM2;
    s.ray.m3 = g_CarReflM3;
    s.ray.world = world;
    Texture2D<float4> history = g_Texture2DDescriptorHeap[TFETCH_INDEX(slots.x)];
    s.ray.size = float2(getTexture2DDimensions(history));

    // 1. Is the shaded point in the history, at the distance it has now? A disocclusion, a wrong guess of the car's motion or a cut puts it
    // elsewhere: the whole effect fades with the disagreement (the tolerance is a few centimetres: the lip beside a tube is not the tube). The texels
    // round the own one count too (CarChrSilhouetteAgreement): the rim of a tip, whose own texel is what is behind it.
    const float4 start = CarChrProject(s.ray.m0, s.ray.m1, s.ray.m2, s.ray.m3, world);
    s.state = 1.0;
    if (start.w < 0.1)
    {
        return s;
    }
    s.ray.startUv = CarChrUv(start);
    if (any(s.ray.startUv < float2(0.0, 0.0)) || any(s.ray.startUv > float2(1.0, 1.0)))
    {
        return s;
    }
    const float4 own = CarChrLoad(history, s.ray.size, s.ray.startUv);
    s.ray.depth = start.w;
    s.ray.slack = CarChrSlack(start.w, s.ray.size.y, length(float3(s.ray.m0.y, s.ray.m1.y, s.ray.m2.y)), s.nv);
    s.ray.thickness = CarChrThickness(start.w);
    s.ray.shape = CarChrWindowShape(start.w, s.ray.thickness, s.ray.slack);
    const float tolerance = 0.015 + 0.005 * start.w + s.ray.slack;
    s.agreement = CarChrSilhouetteAgreement(history, s.ray.size, s.ray.startUv, start.w, tolerance, CarChrTexelAgreement(own.a, start.w, tolerance));
    if (s.agreement <= 0.0)
    {
        return s;
    }
    s.valid = 1.0;
    s.state = 3.0;

    // 2. The ray traced image, where the host made one for this frame's history (g_CarReflC.w bit 0: the image at g_CarReflC.z).
    float4 rt = float4(0.0, 0.0, 0.0, 0.0);
    if ((slots.w & 1u) != 0u)
    {
        rt = CarChrRtRead(history, s.ray.size, s.ray.startUv, start.w, 2.0 * (0.004 + 0.0015 * start.w + s.ray.slack), slots.z);
    }
    // Another car's chrome: the image has both probes' weights at the shaded point's place, and nothing is marched (the reflection probe is not made at a grazing
    // view, as the march's is not).
    if (rt.z > 0.5)
    {
        s.reflectHit = (s.nv >= kCarChrNormalMin) ? rt.y * s.agreement : 0.0;
        s.skyHit = rt.w * s.agreement;
        s.skyKnown = 1.0;
        s.state = 6.0;
        return s;
    }

    // 3. The reflection probe. Where the image has the player's texels there, its answer IS the weight and the march is not made (the sky probe is
    // marched); where it has no data there, or no image was made, the march runs.
    if (s.nv >= kCarChrNormalMin)
    {
        if (rt.x > 0.5)
        {
            s.reflectHit = rt.y * s.agreement;
            s.state = 5.0;
            return s;
        }
        s.reflectHit = CarChrProbe(history, s, 2.0 * s.nv * s.n - v, kCarChrReflectSteps, kCarChrReflectLength);
        s.state = (s.reflectHit > 0.02) ? 4.0 : 3.0;
    }
    return s;
}

// `env` (the sharp environment read) with the reflection probe's occlusion: scaled by the strength, never under 1 - kCarChrDark of itself.
float3 CarChromeEnv(float3 env, CarChrState s)
{
    if (s.reflectHit <= 0.0)
    {
        return env;
    }
    return env * (1.0 - kCarChrDark * saturate(s.reflectHit * g_CarReflA.w));
}

// The sky's ambient (ambYPos) with the sky probe's occlusion. The probe goes along the normal bent towards the sky, normalize(n + up) (a
// normal that points straight down has no sky to look at and takes up), and is made where the game's blend takes the ambient (`blend`
// above 0.02: a thin tube far away is all ambient) and for the debug view that shows it. This is where a ray traced ambient occlusion
// replaces the march when the host has one.
float3 CarChromeAmbient(float3 ambient, inout CarChrState s, float blend)
{
    if (s.valid < 0.5 || (blend <= 0.02 && uint(g_CarReflB.y + 0.5) != 2u))
    {
        return ambient;
    }
    if (s.skyKnown > 0.5)
    {
        // another car's chrome: the ray traced image has the sky probe's weight (CarChrRtRead), nothing to march
        return ambient * (1.0 - kCarChrDark * saturate(s.skyHit * g_CarReflA.w));
    }
    const uint4 slots = g_CarReflC;
    Texture2D<float4> history = g_Texture2DDescriptorHeap[TFETCH_INDEX(slots.x)];
    float3 sky = s.n + float3(0.0, 1.0, 0.0);
    const float skyLength = length(sky);
    sky = (skyLength < 0.2) ? float3(0.0, 1.0, 0.0) : sky / skyLength;
    s.skyHit = CarChrProbe(history, s, sky, kCarChrSkySteps, kCarChrSkyLength);
    return ambient * (1.0 - kCarChrDark * saturate(s.skyHit * g_CarReflA.w));
}

// The debug views of g_CarReflB.y: 1 the reflection probe's weight, 2 the sky probe's, 3 the baked vertex AO, 4 the state (red: the point is
// not in the history, green: marched and no hit, yellow: a hit, blue: the ray traced image answered, cyan: it answered for another car's chrome (both probes);
// brighter the heavier). The write of
// the colour is scaled by the exponent bias of the target, so they are in its units.
float3 CarChromeDebug(CarChrState s, float bakedAo)
{
    const uint view = uint(g_CarReflB.y + 0.5);
    const float unit = gInvColorExpBias.x;
    if (view == 1u)
    {
        return float3(s.reflectHit, s.reflectHit, s.reflectHit) * unit;
    }
    if (view == 2u)
    {
        return float3(s.skyHit, s.skyHit, s.skyHit) * unit;
    }
    if (view == 3u)
    {
        return float3(bakedAo, bakedAo, bakedAo) * unit;
    }
    float3 colour = float3(0.0, 0.0, 0.0);
    if (s.state > 5.5)
    {
        colour = float3(0.0, 0.9, 0.9) * (0.25 + 0.75 * saturate(max(s.reflectHit, s.skyHit)));
    }
    else if (s.state > 4.5)
    {
        colour = float3(0.0, 0.3, 1.0) * (0.25 + 0.75 * saturate(s.reflectHit));
    }
    else if (s.state > 3.5)
    {
        colour = float3(1.0, 0.8, 0.0) * (0.25 + 0.75 * saturate(s.reflectHit));
    }
    else if (s.state > 2.5)
    {
        colour = float3(0.0, 0.5, 0.0);
    }
    else if (s.state > 0.5)
    {
        colour = float3(1.0, 0.0, 0.0);
    }
    return colour * unit;
}
)HLSL";

}  // namespace

VariantTransformResult TransformCarChromeHlsl(const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
  VariantTransformResult result;
  if (shader.name != kChrome.name) {
    result.error = std::format("{} is not the chrome this variant was written for", shader.name);
    return result;
  }
  std::string text = NormalizeNewlines(hlsl);
  std::string error;

  // 1. The three lines the edits follow or replace: each exactly once.
  if (!ReplaceAllExactly(text, kEnvRead, std::string(kEnvRead) + std::string(kAfterEnvRead), 1, "the environment read", &error) ||
      !ReplaceAllExactly(text, kAmbientLine, std::string(kNewAmbientLine), 1, "the ambient line", &error) ||
      !ReplaceAllExactly(text, kFinalWrite, std::string(kFinalWrite) + std::string(kAfterFinalWrite), 1,
                         "the last write of the colour", &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // 2. The anchored edits: the local state, the constants of both backends and the helper. The constants are the car paint variant's
  // (src/gpu/constants.h CarSelfReflConstants): 112 bytes after the 608, the same declarations.
  static constexpr Piece kCubeMap[] = {Lit("CubeMapData cubeMapData = (CubeMapData)0;")};
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  if (!ReplaceOnce(text, kCubeMap, 0, 0, "the start of main's registers",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tCarChrState carChrome = (CarChrState)0;";
                   },
                   &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  static constexpr ConstantMember kMembers[] = {{"float4", "g_CarReflM0"}, {"float4", "g_CarReflM1"}, {"float4", "g_CarReflM2"},
                                                {"float4", "g_CarReflM3"}, {"float4", "g_CarReflA"},  {"float4", "g_CarReflB"},
                                                {"uint4", "g_CarReflC"}};
  if (!DeclareConstants(text, ctx, "Car self-reflection variants (src/gpu/constants.h CarSelfReflConstants): 112 bytes after the 608.", kMembers, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [](std::string_view group) { return std::string(kHelper) + "\n" + std::string(group); }, &error)) {
    result.error = std::string(shader.name) + ": " + error;
    return result;
  }

  // What the text must hold now: the helper's definitions and their calls, one declaration of each constant in each backend's declaration,
  // and the game's lines nowhere without their edit. The history is read as points (never a filtered sample): one function reads it for a
  // point of a probe (the loop's and the halvings'), the setup reads the shaded point's own, and the ray traced image's read takes the
  // 3x3 texels of the history and of the image.
  const struct {
    std::string_view needle;
    size_t want;
  } kChecks[] = {
      {"CarChrState CarChrSetup(", 1},       {"= CarChrSetup(", 1},
      {"float3 CarChromeEnv(", 1},           {"CarChromeEnv(r1.xyz, carChrome)", 1},
      {"float3 CarChromeAmbient(", 1},       {"CarChromeAmbient(ambYPos.xyz, carChrome, saturate(r2.y + r2.z))", 1},
      {"float3 CarChromeDebug(", 1},         {"CarChromeDebug(carChrome, iColor0.w)", 1},
      {"CarChrState carChrome = (CarChrState)0;", 1},
      {"history.SampleLevel(", 0},           {"history.Sample(", 0},
      {"CarChrLoad(history, ray.size, uv)", 1}, {"CarChrLoad(history, s.ray.size, s.ray.startUv)", 1},
      {"-r1.xyz + ambYPos.xyz", 0},
      {"float4 CarChrRtRead(", 1},           {"= CarChrRtRead(", 1},
      {"if (t.r < -0.5)", 1},                {"if (s.skyKnown > 0.5)", 1},
  };
  for (const auto& check : kChecks) {
    const size_t count = CountOccurrences(text, check.needle);
    if (count != check.want) {
      result.error = std::format("{}: the transformed HLSL has '{}' {} times (need {})", shader.name, check.needle, count,
                                 check.want);
      return result;
    }
  }
  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

VariantDef MakeCarChromeVariant() {
  VariantDef def;
  def.feature = kFeatureCarChrome;
  def.name = "car_chrome";
  def.version = kCarChromeVariantVersion;
  def.needs_rpf = true;  // xChrome lives in xarchive_cache.rpf (cars), as xCarPaintCustomizable does
  def.pixel_shaders = {kChrome};
  def.transform = [](const VariantShader& shader, std::string_view hlsl, const StageContext& ctx) {
    return TransformCarChromeHlsl(shader, hlsl, ctx);
  };
  return def;
}

}  // namespace larecomp::shaderpack
