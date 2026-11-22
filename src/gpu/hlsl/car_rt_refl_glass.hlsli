// MCLA GPU renderer -- the player's car windows reflecting the city, ray traced: the entry point of the glass variant of car_rt_refl_cs.hlsl (car_rt_glass_refl.h, car_glass_rules.h has the design, the
// numbers and the tests). Included at the END of car_rt_refl_cs.hlsl when CAR_RT_REFL_GLASS is defined (the variant car_rt_refl_glass_cs of src/gpu/CMakeLists.txt); the player's `main` is renamed
// CarRtReflPlayerMain there and not emitted. Everything above it in that file is used as it is: the push block (with the one extra member `glass`, below), the bindings, TraceRay (the ray, the
// self-hit rule, the cars' parts, the city: ResolveHit through CityReflHit), VisibleAt.
//
// WHAT IT DOES. The car's windows are drawn after the world and write no depth, so the picture holds the interior under them and only a ray knows where the pane is. One thread is one texel of the
// history grid (the history's size, the divisor of the push's lens.z: the same texel->pixel mapping as the paint's pass), inside the rectangle of the car's box (the push's glass.xy is its first texel):
//   1. a primary ray from the camera through the texel's pixel against the player's GLASS instances alone (the push's slots.z, CAR_RT_MASK_GLASS), cast as far as the picture's own depth there (lin_depth, the
//      screen shortcut's image: the city's rows say whether the frame has it) plus a slack. A hit nearer than the picture's depth is a window pixel with nothing in front of it: the pane's exact point and
//      its mesh normal. No hit (the car is not there, the wall of a building is nearer): the texel is not glass (alpha 0).
//   2. ONE mirror ray along reflect(view, normal), off the pane along the normal, through TraceRay (coneTan 0: the clear-coat ray's arithmetic with no cone): the cars' parts (the player's body and
//      tyres, the cars beside) and the city, its hit made a colour by the city hit library (the picture's own colour where the hit is on the screen, shaded from its material where not), by the fade of
//      its distance over the reach (the push's lens.w).
//   3. the Fresnel of the glass (Schlick, F0 0.04, power 5, at most 0.6: car_glass_rules.h Fresnel) times the hit's weight times the strength (the push's glass.z): the colour the pass ADDS.
// The texel is written (u0) as rgb the colour to add (in the history's units: the main colour target's own, so the apply pass adds it as it is), alpha 1 for a glass texel (0 for none: the apply
// pass normalises by it at the rectangle's edge and at the half size image's block edges). A ray that finds nothing (the sky: a miss; the game's own reflection of the environment map is already in
// the colour) writes a black glass texel. The aux image (u1) and the counters (u2) are the layout's and are not written.
//
// HIT KINDS (TraceRay's answers): a city hit has visible 1 and its colour; a part of the player's own car has the history's colour of it where the camera sees it (visible) and no weight where it does
// not (the pane has no "own surface in shade" to stand in); another car has its colour or its stand-in as the paint's pass takes it.
//
// A change to TraceRay's signature breaks this file's compile, which is the point.

#ifndef CAR_RT_REFL_GLASS_HLSLI
#define CAR_RT_REFL_GLASS_HLSLI

// car_glass_rules.h: the same numbers
static const float kGlassF0 = 0.04;            // the reflectance of glass facing the camera
static const float kGlassPower = 5.0;          // Schlick's exponent
static const float kGlassMaxFresnel = 0.6;     // the most of the reflection a pane takes (a grazing view)
static const float kGlassDepthSlack = 0.02;    // m: the pane may be this far behind the picture's depth ...
static const float kGlassDepthSlackRel = 0.01; // ... plus this share of it (the depth is stored in a float and read at a texel)
static const float kGlassFarDepth = 10000.0;   // m: the primary ray never looks further (the sky's depth is far)
static const float kGlassCapUnits = 64.0;      // the paint's cap on a reflected colour, in the paint's units (times the push's up.w for the history's)

float GlassFresnel(float nv)
{
    return min(kGlassF0 + (1.0 - kGlassF0) * pow(saturate(1.0 - nv), kGlassPower), kGlassMaxFresnel);
}

// The texel: rgb the colour to add, alpha 1 for a texel of glass (0: not glass, or the frame has no picture depth to test against).
float4 TraceGlassTexel(Texture2D<float4> history, uint2 texel)
{
    gCityTexel = texel;  // the noise of the city hits' sun rays (city_refl.hlsli)
    gCityHits = 0u;
    const CityReflFrame c = CityReflFrameNow();
    if ((c.flags & (kCityFlagOn | kCityFlagScreen)) != (kCityFlagOn | kCityFlagScreen))
    {
        return float4(0.0, 0.0, 0.0, 0.0);  // no city in the reflections this frame, or no lin_depth to tell glass from what is in front of it
    }
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    const float2 pixel = min(floor((float2(texel) + 0.5) * divisor), render - 1.0);
    const float2 uv = (pixel + 0.5) / render;
    const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);

    // 1. the pane at this pixel, in front of what the picture shows there
    const int2 depthTexel = clamp(int2(uv * float2(c.depthWidth, c.depthHeight)), int2(0, 0), int2(int(c.depthWidth) - 1, int(c.depthHeight) - 1));
    const float depth = CityReflDepth(depthTexel);
    RayDesc primary;
    primary.Origin = float3(0.0, 0.0, 0.0);
    primary.Direction = direction;
    primary.TMin = 0.001;
    primary.TMax = min(depth * (1.0 + kGlassDepthSlackRel) + kGlassDepthSlack, kGlassFarDepth);
    RayQuery<RAY_FLAG_FORCE_OPAQUE> first;
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary);
    while (first.Proceed())
    {
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float hitT = first.CommittedRayT();
    const uint primaryInstance = first.CommittedInstanceIndex();
    const uint primaryPrimitive = first.CommittedPrimitiveIndex();
    const float3 viewDirection = normalize(direction);
    const float3 normal = CarRtHitNormal(primaryInstance, primaryPrimitive, first.CommittedTriangleBarycentrics(), first.CommittedObjectToWorld3x4(), viewDirection);
    if (!(dot(normal, normal) > 0.25))
    {
        return float4(0.0, 0.0, 0.0, 0.0);  // no usable normal
    }
    const float nv = abs(dot(normal, viewDirection));
    if (nv < kMinNv)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // grazing: a glass texel with nothing to add (as the paint's pass does not trace it)
    }
    if (CityReflStatsOn())
    {
        CityStat(kCityStatTexels, 1u);
    }

    // 2. the mirror ray
    const float3 mirror = reflect(viewDirection, normal);
    const float3 origin = direction * hitT + normal * (0.004 + 0.002 * hitT);
    float fade;
    float visible;
    float3 seen;
    float hitDistance;
    bool other;
    float3 hidden;
    bool car;
    if (!TraceRay(history, origin, mirror, g_push.lens.w, 0.0, primaryInstance, primaryPrimitive, normal, fade, visible, seen, hitDistance, other, hidden, car))
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // nothing of the cars or the city in the way: the game's own reflection stays
    }

    // 3. what the hit shows, by the glass's Fresnel. Another car: its colour where the history shows it, its stand-in where not (the paint's blend); the rest: the history's colour where the camera sees it.
    float3 colour = seen;
    float weight = fade * visible;
    if (other)
    {
        colour = hidden + (seen - hidden) * visible;
        weight = fade * (g_push.tune.y + (1.0 - g_push.tune.y) * visible);
    }
    const float strength = asfloat(g_push.glass.z);
    const float cap = kGlassCapUnits * g_push.up.w;
    const float3 add = min(NgHeld3(colour, kMaxColour) * (GlassFresnel(nv) * weight * strength), float3(cap, cap, cap));   // (a NaN colour is black, decided by the bits: nan_guard.hlsli)
    return float4(add, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 texel = id.xy + g_push.glass.xy;
    const uint2 size = uint2(g_push.image.xy);
    if (any(texel >= size))
    {
        return;
    }
    Texture2D<float4> history = g_textures[g_push.slots.x];
    g_output[texel] = TraceGlassTexel(history, texel);
}

#endif  // CAR_RT_REFL_GLASS_HLSLI
