// Pasted into the translated road pixel shaders by road_mirror_blur.cpp.
struct RoadMirrorFilter {
    float radius;
    float keep;
};

RoadMirrorFilter RoadMirrorParams(float reciprocalDepth, float wetness)
{
    RoadMirrorFilter filter;
    filter.radius = 0.0f;
    filter.keep = 1.0f;
    // PS position W is reciprocal clip W, which is reciprocal view depth on these road draws.
    if (!(reciprocalDepth > 0.0f) || !(wetness > 0.18f))
        return filter;

    float depth = 1.0f / reciprocalDepth;
    float wet = smoothstep(0.18f, 0.8f, wetness);
    // Wetness supplies the roughness proxy. The guest's puddle mask still controls the local blend.
    float roughness = 1.0f - saturate(wetness);
    float spread = lerp(0.65f, 2.5f, smoothstep(4.0f, 32.0f, depth));
    filter.radius = wet * spread * (1.0f + 0.5f * roughness) / 256.0f;
    filter.keep = 1.0f - wet * smoothstep(10.0f, 48.0f, depth);
    return filter;
}

float RoadMirrorTapWeight(int offset)
{
    return offset == 0 ? 0.5f : 0.25f;
}

#ifndef __cplusplus
float4 RoadMirrorSample(uint image, uint samplerIndex, float2 uv, RoadMirrorFilter filter)
{
    if (filter.keep == 0.0f)
        return 0.0f;
    if (filter.radius == 0.0f)
        return tfetch2D(image, samplerIndex, uv, float2(0, 0));

    // Separable 3x3 Gaussian evaluated in place on the one-level resolve. Radius is in UV,
    // so the same screen footprint applies at 256, 512 and 768. Clamp taps to texel centres
    // to avoid pulling in the opposite edge at the borders, regardless of sampler address mode.
    float2 radius = filter.radius * float2(0.625f, 1.0f);
    float2 edge = 0.5f / max(float2(getTexture2DDimensions(g_Texture2DDescriptorHeap[TFETCH_INDEX(image)])), 1.0f);
    float4 value = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 tap = clamp(uv + float2(x, y) * radius, edge, 1.0f - edge);
            value += tfetch2DLod(image, samplerIndex, tap, float2(0, 0), 0.0f)
                     * (RoadMirrorTapWeight(x) * RoadMirrorTapWeight(y));
        }
    }
    return value;
}
#endif
