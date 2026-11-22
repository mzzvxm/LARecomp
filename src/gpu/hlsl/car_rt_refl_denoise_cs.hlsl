// MCLA GPU renderer -- the player's car reflecting itself, ray traced: the denoise (car_rt_refl.h, car_rt_refl_rules.h "The denoise").
//
// Two passes of this shader (the push's pass 0 and 1: an a-trous filter with its taps one, then two texels apart) take the noise out of the
// image the trace made (car_rt_refl_cs.hlsl) before the paint reads it: the first reads the trace's image and writes a scratch image, the
// second reads the scratch and writes the trace's image again. One thread is one texel of the image (the history's size).
//
//   input    the image to filter (bindless slot): rgb the colour of the hits, alpha the code (0 no data, 1 a miss, 1 + w a hit of weight w, -(1 + w)
//            a hit on a part the camera does not see)
//   history  the history image's alpha: the view distance of the vehicle pixel (0 elsewhere)
//   aux      the trace's second output: x the hit distance, y the instance, z N.V, w 1 (down to 0.51: 1 - 0.49 the share of the texel's weight that is the clear coat's mirror ray,
//            car_rt_hit_rules.h EncodeAuxSharp; car_rt_refl_cs.hlsl)
//   output   the image's codes again; a texel with no data is copied as it is (alpha 0: the paint's march answers there)
//
// A 3x3 kernel of 1 2 1 / 2 4 2 / 1 2 1 over the taps that (1) hold data (a miss counts, as a weight of 0), (2) are the same instance, (3) are the
// same surface (the history's distance within Tolerance of the texel's, the slope's slack from the texel's N.V and the taps' distance apart).
// The sums are premultiplied by each tap's own weight (the paint's own sum: a miss's black does not darken a hit), and the result is mixed with
// the texel's own value by a strength that follows the cone: the width in texels of the cone at the hit (2 distance tan(half angle) / the texel's
// metres), the largest among the taps that count. Under a texel's half the texel is left exactly as it was (the penumbra is narrower than a texel:
// a binary edge, no noise to take out); the first pass is in full from 2.5 texels, the second (the wider kernel) from 10, which keeps the glossy
// edge: a ramp grows by at most a fifth. The strength is multiplied by what the texel's clear-coat share leaves (car_rt_hit_rules.h DenoiseScale): the cone's grain is taken out,
// the mirror ray's sharp reflection is not blurred. Everything here is car_rt_refl_rules.h's DenoiseTexel in HLSL (and car_rt_hit_rules.h for that scale); change one, change the other.
//
// THE THIRD PASS (the push's pass 2; NOISE-RESOLVE, noise_resolve.h, mcla_gpu_noise_resolve_no_upscaler). With no temporal upscaler the cone's turn changes every frame (the pattern is the blue tile stepped by the
// frame) and nothing averaged it but the two passes above, whose residual is under 1% of the weight. The third pass averages the finished image F (the passes' result, input slot y) with the last
// frame's result S (the bindless slot in image.w, written by this same pass a frame ago, in the same texel: no reprojection) and writes the next S, which the paint reads. It is texel for texel, so it
// holds only where the car's picture stands where it stood; the rest is the clamp's: the history's weight, hidden share and premultiplied colour are each clamped to the range of F's 3 x 3 neighbourhood
// (the texels that hold data) plus a margin, so a reflection that moved (the cone's penumbra moves with the view: car_rt_refl_rules.h "NOT TEMPORAL" weighed 0.5 degrees a frame against a 2.5 degree cone)
// leaves a ghost only inside the neighbourhood's own range: about a texel of the ramp, a fifth of the cone's width. A texel with no data in F or in S, or a texel whose clear coat's sharp share (the
// aux image's w) is the whole of it, is F's own. The weight is image.z (0: the pass copies F: the first frame, a gap, a new size). Nothing is read from the history image or written to a second image.
//
// Shader Model 6.0. The layout (car_rt_refl.cpp): set 0 the bindless Texture2D<float4> set (space 0), set 1 the output (u0, space 1), the push
// block (b0, space 2).

#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the images are R16G16B16A16_FLOAT.
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);

struct CarRtDenoisePush
{
    // x the history image's bindless slot, y the image to filter's, z the aux image's, w the pass (0: the taps one texel apart, 1: two)
    uint4 slots;
    // xy the image's size in texels; pass 2: z the weight the history keeps, w the last result's bindless slot
    float4 image;
    // x tan of half the vertical field of view of the history's lens, y the reflection ray's reach (m), z the tangent of the cone's half angle
    float4 lens;
    // x, y the first pass's footprint (texels) where it starts and where it is in full, z, w the second pass's
    float4 tune;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtDenoisePush> g_push;
#else
ConstantBuffer<CarRtDenoisePush> g_push : register(b0, space2);
#endif

static const float kMinCode = 0.5;
static const float kAuxSharpScale = 0.49;
static const float kMinWeight = 1.0 / 512.0;
// NOISE-RESOLVE, pass 2: the margin the history may lie outside the neighbourhood's range, in weight and, of the premultiplied colour, as a share of the range's top (noise_resolve.h kConeClampMargin)
static const float kTemporalMargin = 0.05;

// One texel's decoded value: the weight, the part of it that is on another car's parts, and the weight-premultiplied colour (a miss is weight 0).
struct Decoded
{
    float weight;
    float hidden;
    float3 colour;
};

Decoded Decode(float4 texel)
{
    Decoded d;
    d.weight = saturate(abs(texel.a) - 1.0);
    d.hidden = texel.a < 0.0 ? d.weight : 0.0;
    d.colour = d.weight * texel.rgb;
    return d;
}

// The pass's third mode: the finished image averaged with the last frame's result, texel for texel.
void Temporal(int2 centre, int2 size)
{
    Texture2D<float4> input = g_textures[g_push.slots.y];
    const float4 own = input.Load(int3(centre, 0));
    const float strength = g_push.image.z;
    if (!(abs(own.a) >= kMinCode) || !(strength > 0.0))
    {
        g_output[centre] = own;
        return;
    }
    const float4 last = g_textures[uint(g_push.image.w + 0.5)].Load(int3(centre, 0));
    if (!(abs(last.a) >= kMinCode))
    {
        g_output[centre] = own;  // the history holds no car here
        return;
    }
    // the clear coat's mirror share (the aux image's w, EncodeAuxSharp) is not noise: left as it is
    const float4 ownAux = g_textures[g_push.slots.z].Load(int3(centre, 0));
    const float sharp = ownAux.w > 0.5 ? saturate((1.0 - ownAux.w) / kAuxSharpScale) : 0.0;
    const float keep = strength * (1.0 - sharp);
    // the range of the neighbourhood's decoded values, the texels that hold data
    Decoded now = Decode(own);
    Decoded lo = now;
    Decoded hi = now;
    [unroll]
    for (int k = 0; k < 9; ++k)
    {
        const int2 at = centre + int2(k % 3 - 1, k / 3 - 1);
        if (any(at < int2(0, 0)) || any(at >= size))
        {
            continue;
        }
        const float4 tap = input.Load(int3(at, 0));
        if (!(abs(tap.a) >= kMinCode))
        {
            continue;
        }
        const Decoded d = Decode(tap);
        lo.weight = min(lo.weight, d.weight);
        hi.weight = max(hi.weight, d.weight);
        lo.hidden = min(lo.hidden, d.hidden);
        hi.hidden = max(hi.hidden, d.hidden);
        lo.colour = min(lo.colour, d.colour);
        hi.colour = max(hi.colour, d.colour);
    }
    Decoded before = Decode(last);
    before.weight = clamp(before.weight, lo.weight - kTemporalMargin, hi.weight + kTemporalMargin);
    before.hidden = clamp(before.hidden, lo.hidden - kTemporalMargin, hi.hidden + kTemporalMargin);
    const float3 colourMargin = kTemporalMargin * (hi.colour + 0.01);
    before.colour = clamp(before.colour, lo.colour - colourMargin, hi.colour + colourMargin);
    const float mixedWeight = lerp(now.weight, before.weight, keep);
    const float mixedHidden = lerp(now.hidden, before.hidden, keep);
    const float3 mixedColour = lerp(now.colour, before.colour, keep);
    float4 result = float4(0.0, 0.0, 0.0, 1.0);  // a miss
    if (mixedWeight >= kMinWeight)
    {
        result = float4(mixedColour / mixedWeight, (mixedHidden > 0.5 * mixedWeight ? -1.0 : 1.0) * (1.0 + min(mixedWeight, 1.0)));
    }
    g_output[centre] = result;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 size = int2(g_push.image.xy);
    const int2 centre = int2(id.xy);
    if (any(centre >= size))
    {
        return;
    }
    if (g_push.slots.w == 2u)
    {
        Temporal(centre, size);
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

    // the part of the texel's weight that is the clear coat's mirror (the aux's w, EncodeAuxSharp) is left as it is
    const float sharp = saturate((1.0 - ownAux.w) / kAuxSharpScale);
    const float strength = (pass == 0u ? smoothstep(g_push.tune.x, g_push.tune.y, footprint) : smoothstep(g_push.tune.z, g_push.tune.w, footprint)) * (1.0 - sharp);
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
