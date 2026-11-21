// MCLA GPU renderer -- the opponents' cars reflecting, ray traced: the entry point of the opponents' variant of car_rt_refl_cs.hlsl (car_rt_opp_refl.h,
// car_rt_opp_refl_rules.h has the design). Included at the END of car_rt_refl_cs.hlsl when CAR_RT_REFL_OPPONENTS is defined (the variant car_rt_refl_opp_cs of
// src/gpu/CMakeLists.txt); the player's `main` is renamed CarRtReflPlayerMain there and not emitted. Everything above it in that file is used as it is: the push
// block (with the one extra member opp, below), the bindings, the cone (ConeDirection, ConeRotation), TraceRay (the ray, the self-hit rule, the history's colour of the hit,
// the colour of a hit on another car), VisibleAt, SurfaceMean, OtherCarColour.
//
// WHAT IT DOES. It is the player's TraceTexel for a texel of ANOTHER car's paint: the primary ray's mask is the push's z (CAR_RT_MASK_OTHER_PAINT: only the instances of
// another car's draws of the paint's shader), so the hit is the opponent's own panel at the pixel, with its mesh normal; the cone of rays goes from it against the push's parts
// (the player's body, glass and tyres and every other car, the opponent's own other instances among them), and the texel is written in the player's codes (alpha 0 no data,
// 1 + w a hit on parts the camera sees, -(1 + w) on parts it does not, rgb the colour; the aux image: hit distance, an instance code of its own, N.V, 1). What differs from
// the player's texel:
//   - the colour of a hit on the PLAYER's car the history does not show. The player's pass takes the pixel's own surface in shade for it (the pixel IS the player's car); here
//     the pixel is the opponent's, so the player's car has a record in the instances' table (List::player_row, the row in the push's opp.z) and OtherCarColour reads it as it
//     reads another car's: the colour the camera sees of the car round its centre, or its paint. See ResolvePlayerHit in car_rt_opp_refl_rules.h (the same decision).
//   - the instance code (EncodeOppInstance): an opponent's instance index is past 255 with a second car, and 255 is the chrome texel's.
//   - the rays: the push's opp.w (4 the full cone, 2 the outer pair, 1 the mirror: car_rt_opp_refl_rules.h PlanDispatches says by the area it spends), at most the level's (slots.w).
//   - RT-HIT-SHADING part two, as the player's texel: the others' reach, the clear-coat mirror ray (only at the full cone: it is the budget's top cell), the cascades' sun shadow at a
//     shaded hit. The clear coat's lobe and the cone's are mixed by CarCoatMix as TraceTexel does.
//   - a texel with no data is NOT written: the dispatch runs after the player's over the same images, and a texel that is not an opponent's paint (the player's own, another car's
//     chrome, a vehicle with no triangles in the scene) is the first dispatch's.
// The dispatch covers one rectangle of the image: its first texel is the push's opp.xy (the group grid starts at it); a thread past the image does nothing.
//
// A change to TraceTexel's combining of the rays (the weights, the colours, the codes) goes here too: this is that arithmetic for the opponents' texel. A change to TraceRay's
// signature or to OtherCarColour breaks this file's compile, which is the point.

#ifndef CAR_RT_REFL_OPP_HLSLI
#define CAR_RT_REFL_OPP_HLSLI

static const uint kOppCodeBase = 200u;  // the aux image's instance codes of the opponents' texels: 200 .. 254 (car_rt_opp_refl_rules.h kCodeBase, kCodes)
static const uint kOppCodes = 55u;

float EncodeOppInstance(uint instance)
{
    return float(kOppCodeBase + instance % kOppCodes) / 255.0;
}

// The share of the player's car's colour that is known (OtherCarColour: the history round the car's centre, or 1 for the paint), and the colour (in the history's units; not shaded).
float PlayerCarColour(Texture2D<float4> history, out float3 colour)
{
    colour = float3(0.0, 0.0, 0.0);
    const uint row = g_push.opp.z;
    if (row == 0u)
    {
        return 0.0;  // the frame gave no record of the player's car
    }
    CarRtInstance named;  // an instance's `draw` names the row of a car's record
    named.first_index = 0u;
    named.first_vertex = 0u;
    named.flags = 0u;
    named.draw = row;
    return OtherCarColour(history, CarRtLoadCar(named), colour);
}

float4 TraceOpponentTexel(Texture2D<float4> history, uint2 texel, float4 own, out float4 aux)
{
    aux = float4(0.0, 0.0, 0.0, 0.0);
    const float t = own.a;
    const float divisor = g_push.lens.z;
    const float2 render = g_push.image.zw;
    const float2 pixel = min(floor((float2(texel) + 0.5) * divisor), render - 1.0);
    const float2 uv = (pixel + 0.5) / render;
    const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
    const float pixelMeters = PixelMeters(t, g_push.lens.y, render.y);

    // 1. the primary ray: another car's paint at this pixel, in the window around the history's distance
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
        return float4(0.0, 0.0, 0.0, kNoData);
    }
    const float nv = abs(dot(normal, viewDirection));
    if (abs(hitT - t) > Tolerance(t, pixelMeters, 0.75, nv))
    {
        return float4(0.0, 0.0, 0.0, kNoData);  // a surface of that car, but not the one the picture shows here
    }
    aux = float4(0.0, EncodeOppInstance(primaryInstance), saturate(nv), 1.0);
    if (nv < kMinNv)
    {
        return float4(0.0, 0.0, 0.0, 1.0);  // grazing: the paint does not trace it either (a miss: nothing changes)
    }

    // 2. the clear coat's ray and the cone of reflection rays, off the surface along the normal: as the player's, with the push's number of rays (the level's at most)
    const uint maxRays = min(g_push.opp.w, LevelRays(g_push.slots.w));
    const float3 mirror = reflect(viewDirection, normal);
    const float reach = g_push.lens.w;
    const float tanHalf = maxRays >= 2u ? g_push.tune.w : 0.0;
    const float rays = tanHalf > 0.0 ? float(min(maxRays, 4u)) : 1.0;
    const float3 origin = direction * hitT + normal * (0.004 + 0.002 * hitT);
    const float turns = ConeRotation(texel, g_push.slots.w & 7u);
    float coatShare = 0.0;
    float coatFade = 0.0;
    float coatVisible = 0.0;
    float3 coatSeen = float3(0.0, 0.0, 0.0);
    float coatDistance = 0.0;
    bool coatOther = false;
    float3 coatHidden = float3(0.0, 0.0, 0.0);
    bool coatHit = false;
    [branch]
    if (g_push.coat.w > 0.0 && tanHalf > 0.0 && maxRays >= 4u)
    {
        coatShare = CarCoatShare(nv, CarMatFresnelPower(primaryInstance), g_push.coat.w);
        coatHit = TraceRay(history, origin, ConeDirection(mirror, normal, 0.0, 0, 0.0), reach, 0.0, primaryInstance, primaryPrimitive, normal, coatFade, coatVisible, coatSeen,
                           coatDistance, coatOther, coatHidden);
    }
    float fade[4] = {0.0, 0.0, 0.0, 0.0};
    float visible[4] = {0.0, 0.0, 0.0, 0.0};
    float3 seen[4] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    float distances[4] = {0.0, 0.0, 0.0, 0.0};
    float3 hidden[4] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    bool other[4] = {false, false, false, false};  // a hit on another car (the opponent's own other parts and the other opponents): `hidden` is its colour
    bool found = coatHit;  // the mirror ray found a part: the cone's inner pair is cast too
    [unroll]
    for (int slot = 0; slot < 4; ++slot)
    {
        if (slot >= 1 && !(tanHalf > 0.0))
        {
            break;  // a mirror: one ray
        }
        if (slot >= int(maxRays))
        {
            break;  // the budget's rays
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
        if (TraceRay(history, origin, direction2, reach, tanHalf, primaryInstance, primaryPrimitive, normal, f, v, c, d, o, hc))
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
        return float4(0.0, 0.0, 0.0, 1.0);  // nothing of any car in the reflection: the environment stays
    }

    // 3. what the hits show. A hit on another car: TraceRay made the colour of its hidden part. A hit on the PLAYER's car (not `other`): the history's colour where the camera
    // sees it, the player's record where it does not (ResolvePlayerHit), a miss where there is neither.
    bool needPlayer = coatHit && coatFade > 0.0 && coatVisible < 1.0 && !coatOther;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        needPlayer = needPlayer || (fade[k] > 0.0 && visible[k] < 1.0 && !other[k]);
    }
    float3 playerColour = float3(0.0, 0.0, 0.0);
    float playerKnown = 0.0;
    [branch]
    if (needPlayer)
    {
        playerKnown = PlayerCarColour(history, playerColour);
        playerColour *= g_push.tune.x;
    }
    float weightSum = 0.0;
    float visibleSum = 0.0;
    float3 colourSum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        float3 standIn = hidden[j];
        if (!other[j] && fade[j] > 0.0 && visible[j] < 1.0)
        {
            if (playerKnown >= kOtherCarMinShare)
            {
                standIn = playerColour;
            }
            else if (visible[j] > 0.0)
            {
                standIn = seen[j] * g_push.tune.x;
            }
            else
            {
                fade[j] = 0.0;  // hidden, and the player's colour is not known: nothing (the environment stays)
            }
        }
        const float w = fade[j] * (g_push.tune.y + (1.0 - g_push.tune.y) * visible[j]);
        weightSum += w;
        visibleSum += w * visible[j];
        colourSum += w * (standIn + (seen[j] - standIn) * visible[j]);
    }
    // the coat's lobe (a hit on the player's car by the same rule as the cone's rays above), then the two lobes added by the share
    float3 coatStandIn = coatHidden;
    if (!coatOther && coatFade > 0.0 && coatVisible < 1.0)
    {
        if (playerKnown >= kOtherCarMinShare)
        {
            coatStandIn = playerColour;
        }
        else if (coatVisible > 0.0)
        {
            coatStandIn = coatSeen * g_push.tune.x;
        }
        else
        {
            coatFade = 0.0;  // hidden, and the player's colour is not known: nothing (the environment stays)
        }
    }
    const float coatWeight = coatHit ? coatFade * (g_push.tune.y + (1.0 - g_push.tune.y) * coatVisible) : 0.0;
    const float3 coatColour = coatStandIn + (coatSeen - coatStandIn) * coatVisible;
    float3 mixed;
    float mixedVisible;
    float sharp;
    const float weight = CarCoatMix(weightSum, colourSum, visibleSum, rays, coatShare, coatWeight, coatColour, coatWeight * coatVisible, mixed, mixedVisible, sharp);
    if (weight < 1.0 / 512.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float3 colour = min(max(mixed, 0.0), kMaxColour);
    float distanceSum = 0.0;
    float hitCount = 0.0;
    [unroll]
    for (int m = 0; m < 4; ++m)
    {
        distanceSum += fade[m] > 0.0 ? distances[m] : 0.0;
        hitCount += fade[m] > 0.0 ? 1.0 : 0.0;
    }
    if (hitCount == 0.0 && coatWeight > 0.0)
    {
        distanceSum = coatDistance;
        hitCount = 1.0;
    }
    aux.x = EncodeHitDistance(distanceSum / max(hitCount, 1.0), reach);
    aux.w = 1.0 - kAuxSharpScale * sharp;  // the denoise leaves the mirror's part of the texel alone
    return float4(colour, mixedVisible >= 0.5 * weight ? 1.0 + min(weight, 1.0) : -(1.0 + min(weight, 1.0)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(g_push.image.xy);
    const uint2 texel = id.xy + g_push.opp.xy;
    if (any(texel >= size))
    {
        return;
    }
    Texture2D<float4> history = g_textures[g_push.slots.x];
    const float4 own = history.Load(int3(texel, 0));
    if (!(own.a > 0.0 && own.a < 60000.0))
    {
        return;  // no vehicle here: the first dispatch's texel stands
    }
    float4 aux = float4(0.0, 0.0, 0.0, 0.0);
    const float4 result = TraceOpponentTexel(history, texel, own, aux);
    if (result.a == kNoData)
    {
        return;  // not another car's paint (or no surface of it at this distance): the first dispatch's texel stands
    }
    g_output[texel] = result;
    g_aux[texel] = aux;
}

#endif  // CAR_RT_REFL_OPP_HLSLI
