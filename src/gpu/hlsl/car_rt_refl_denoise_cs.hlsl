// MCLA GPU renderer -- the player's car reflecting itself, ray traced: the denoise (car_rt_refl.h, car_rt_refl_rules.h "The denoise").
//
// Two passes of this shader (the push's pass 0 and 1: an a-trous filter with its taps one, then two texels apart) take the noise out of the
// image the trace made (car_rt_refl_cs.hlsl) before the paint reads it: the first reads the trace's image and writes a scratch image, the
// second reads the scratch and writes the trace's image again. One thread is one texel of the image (the history's size).
//
//   input    the image to filter (bindless slot): rgb the colour of the hits, alpha the code (0 no data, 1 a miss, 1 + w a hit of weight w, -(1 + w)
//            a hit on a part the camera does not see)
//   history  the history image's alpha: the view distance of the vehicle pixel (0 elsewhere)
//   aux      the trace's second output: x the hit distance, y the instance, z N.V, w 1 (car_rt_refl_cs.hlsl)
//   output   the image's codes again; a texel with no data is copied as it is (alpha 0: the paint's march answers there)
//
// A 3x3 kernel of 1 2 1 / 2 4 2 / 1 2 1 over the taps that (1) hold data (a miss counts, as a weight of 0), (2) are the same instance, (3) are the
// same surface (the history's distance within Tolerance of the texel's, the slope's slack from the texel's N.V and the taps' distance apart).
// The sums are premultiplied by each tap's own weight (the paint's own sum: a miss's black does not darken a hit), and the result is mixed with
// the texel's own value by a strength that follows the cone: the width in texels of the cone at the hit (2 distance tan(half angle) / the texel's
// metres), the largest among the taps that count. Under a texel's half the texel is left exactly as it was (the penumbra is narrower than a texel:
// a binary edge, no noise to take out); the first pass is in full from 2.5 texels, the second (the wider kernel) from 10, which keeps the glossy
// edge: a ramp grows by at most a fifth. Everything here is car_rt_refl_rules.h's DenoiseTexel in HLSL; change one, change the other.
//
// Shader Model 6.0. The layout (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0), set 1 the output (u0, space 1), the push
// block (b0, space 2).

#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
RWTexture2D<float4> g_output : register(u0, space1);

struct CarRtDenoisePush
{
    // x the history image's bindless slot, y the image to filter's, z the aux image's, w the pass (0: the taps one texel apart, 1: two)
    uint4 slots;
    // xy the image's size in texels
    float4 image;
    // x tan of half the vertical field of view of the history's lens, y the reflection ray's reach (m), z the tangent of the cone's half angle
    float4 lens;
    // x, y the first pass's footprint (texels) where it starts and where it is in full, z, w the second pass's
    float4 tune;
};

ConstantBuffer<CarRtDenoisePush> g_push : register(b0, space2);

static const float kMinCode = 0.5;
static const float kMinWeight = 1.0 / 512.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 size = int2(g_push.image.xy);
    const int2 centre = int2(id.xy);
    if (any(centre >= size))
    {
        return;
    }
    Texture2D<float4> input = g_textures[g_push.slots.y];
    const float4 own = input.Load(int3(centre, 0));
    // Most of the image is no vehicle (no data): one read and one write.
    if (!(abs(own.a) >= kMinCode))
    {
        g_output[centre] = own;
        return;
    }
    Texture2D<float4> history = g_textures[g_push.slots.x];
    Texture2D<float4> aux = g_textures[g_push.slots.z];
    const float t = history.Load(int3(centre, 0)).a;
    const float4 ownAux = aux.Load(int3(centre, 0));
    if (!(t > 0.0) || !(ownAux.w > 0.5))
    {
        g_output[centre] = own;  // no distance or no aux for this texel: nothing to judge the taps by
        return;
    }
    const int instance = int(ownAux.y * 255.0 + 0.5);
    const float nv = ownAux.z;
    const uint pass = g_push.slots.w;
    const int step = int(1u << min(pass, 1u));
    const float texelMeters = PixelMeters(t, g_push.lens.x, g_push.image.y);
    const float reach = g_push.lens.y;
    const float tanHalf = g_push.lens.z;

    float n = 0.0;
    float weight = 0.0;
    float hidden = 0.0;
    float3 colour = float3(0.0, 0.0, 0.0);
    float footprint = 0.0;
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 offset = int2(k % 3 - 1, k / 3 - 1);
        const int2 at = centre + offset * step;
        if (any(at < int2(0, 0)) || any(at >= size))
        {
            continue;
        }
        const float4 tap = input.Load(int3(at, 0));
        const float code = abs(tap.a);
        if (!(code >= kMinCode))
        {
            continue;
        }
        const float4 tapAux = aux.Load(int3(at, 0));
        if (int(tapAux.y * 255.0 + 0.5) != instance)
        {
            continue;
        }
        const float tapT = history.Load(int3(at, 0)).a;
        const float apart = float(step) * sqrt(float(offset.x * offset.x + offset.y * offset.y));
        const float kernel = (offset.x == 0 ? 2.0 : 1.0) * (offset.y == 0 ? 2.0 : 1.0) * MatchWeight(tapT, t, Tolerance(t, texelMeters, apart, nv));
        if (!(kernel > 0.0))
        {
            continue;
        }
        const float w = saturate(code - 1.0);
        n += kernel;
        weight += kernel * w;
        hidden += tap.a < 0.0 ? kernel * w : 0.0;
        colour += kernel * w * tap.rgb;
        if (tapAux.x > 0.0)
        {
            // the cone's width at the hit, in texels at the tap's own distance (ConeFootprint)
            const float hitDistance = tapAux.x * tapAux.x * reach;
            footprint = max(footprint, 2.0 * hitDistance * tanHalf / PixelMeters(tapT, g_push.lens.x, g_push.image.y));
        }
    }

    const float strength = pass == 0u ? smoothstep(g_push.tune.x, g_push.tune.y, footprint) : smoothstep(g_push.tune.z, g_push.tune.w, footprint);
    if (!(strength > 0.0))
    {
        g_output[centre] = own;  // no cone wide enough around to have noise: as the trace made it
        return;
    }
    const float ownWeight = saturate(abs(own.a) - 1.0);
    const float ownHidden = own.a < 0.0 ? ownWeight : 0.0;
    const float inverse = 1.0 / max(n, 1e-6);
    const float mixedWeight = ownWeight + (weight * inverse - ownWeight) * strength;
    const float mixedHidden = ownHidden + (hidden * inverse - ownHidden) * strength;
    const float3 ownColour = ownWeight * own.rgb;
    const float3 mixedColour = ownColour + (colour * inverse - ownColour) * strength;
    float4 result = float4(0.0, 0.0, 0.0, 1.0);  // a miss
    if (mixedWeight >= kMinWeight)
    {
        result = float4(mixedColour / mixedWeight, (mixedHidden > 0.5 * mixedWeight ? -1.0 : 1.0) * (1.0 + min(mixedWeight, 1.0)));
    }
    g_output[centre] = result;
}
