// ===========================================================================
// MCLA Native Graphics Runtime — FXAA (NVIDIA, Timothy Lottes; 3.11 quality)
// ===========================================================================
// Compute, not a raster pass, because that is the shape the present path here
// already has: the display buffer this writes is never bound as an RTV (that
// was the continuous-mode device removal), and the tonemap and gamma-ramp
// passes beside it are compute for the same reason.
//
// Runs at present, on the LDR composite. FXAA is a luma-edge filter and wants
// tonemapped, roughly perceptual values; run earlier on the HDR scene target it
// would chase edges that tonemapping is about to move. It goes BEFORE the
// display gamma ramp, which is where the console applies its own ramp -- the
// DC_LUT is scanout, after everything the GPU drew.
//
// Regenerate the embedded blob:
//   dxc -T cs_6_0 -E CSMain -O3 -Fo fxaa_cs.dxil fxaa.hlsl
//   python tools/embed_dxil.py src/native_gfx/d3d12/fxaa_shaders.h \
//       mcla::native_gfx FxaaCs=fxaa_cs.dxil
// ===========================================================================

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gDest : register(u0);
SamplerState gSampler : register(s0);

cbuffer FxaaParams : register(b0)
{
    uint gWidth;
    uint gHeight;
    // Minimum local contrast, as a fraction of the brighter luma, before any
    // filtering happens at all. Below it the pixel is passed through untouched.
    float gEdgeThreshold;
    // How much of the sub-pixel (thin feature) term is allowed through. 0
    // disables it and keeps the image sharpest; 1 is the softest.
    float gSubpixel;
};

float FxaaLuma(float3 rgb)
{
    // Rec.601 luma. FXAA is defined against a perceptual luma; the cheaper
    // green-channel-only variant visibly misses coloured edges.
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

// Edge-search step lengths. The longer steps at the far end let a shallow edge
// be followed far enough to matter without paying for every texel along it.
static const float kStep[12] = {
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0
};

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gWidth || tid.y >= gHeight)
    {
        return;
    }
    const float2 rcp = float2(1.0 / float(gWidth), 1.0 / float(gHeight));
    const float2 uv = (float2(tid.xy) + 0.5) * rcp;

    const float4 texelM = gSource.SampleLevel(gSampler, uv, 0);
    const float3 rgbM = texelM.rgb;

    const float lumaM = FxaaLuma(rgbM);
    const float lumaN = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(0, -1)).rgb);
    const float lumaS = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(0, 1)).rgb);
    const float lumaW = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(-1, 0)).rgb);
    const float lumaE = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(1, 0)).rgb);

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = lumaMax - lumaMin;

    // Flat enough: write the pixel through untouched. This early-out is what
    // keeps FXAA from softening the whole frame -- most pixels take it.
    if (range < max(0.0312, lumaMax * gEdgeThreshold))
    {
        gDest[tid.xy] = texelM;
        return;
    }

    // The four diagonals, needed both to pick the edge direction and for the
    // sub-pixel term at the end.
    const float lumaNW = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(-1, -1)).rgb);
    const float lumaNE = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(1, -1)).rgb);
    const float lumaSW = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(-1, 1)).rgb);
    const float lumaSE = FxaaLuma(gSource.SampleLevel(gSampler, uv, 0, int2(1, 1)).rgb);

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float lumaSWSE = lumaSW + lumaSE;
    const float lumaNWSW = lumaNW + lumaSW;
    const float lumaNESE = lumaNE + lumaSE;

    // Second derivative along each axis; the larger one is across the edge.
    const float edgeH = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 +
                        abs(-2.0 * lumaE + lumaNESE);
    const float edgeV = abs(-2.0 * lumaN + lumaNWNE) + abs(-2.0 * lumaM + lumaWE) * 2.0 +
                        abs(-2.0 * lumaS + lumaSWSE);
    const bool horizontal = edgeH >= edgeV;

    // The neighbour on the steeper side decides which way to step off the edge.
    // luma1 has to be the neighbour that -stepLength points at and luma2 the one
    // +stepLength points at, because the branch below negates stepLength exactly
    // when luma1 wins and then averages the centre with luma1. D3D texture v
    // grows DOWNWARD, so +rcp.y is south and -rcp.y is north: luma1 is N, not S.
    // Getting this backwards averages against the wrong side, which poisons
    // lumaLocalAvg, makes correctVariation false almost everywhere, and collapses
    // the whole filter to its sub-pixel term (measured: max delta 4/255 on a
    // hard silhouette edge that should blend about half way).
    const float luma1 = horizontal ? lumaN : lumaW;
    const float luma2 = horizontal ? lumaS : lumaE;
    const float grad1 = luma1 - lumaM;
    const float grad2 = luma2 - lumaM;
    const bool is1Steepest = abs(grad1) >= abs(grad2);
    const float gradScaled = 0.25 * max(abs(grad1), abs(grad2));

    float stepLength = horizontal ? rcp.y : rcp.x;
    float lumaLocalAvg = 0.0;
    if (is1Steepest)
    {
        stepLength = -stepLength;
        lumaLocalAvg = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocalAvg = 0.5 * (luma2 + lumaM);
    }

    // Move half a texel onto the edge itself, then walk both ways along it.
    float2 currentUv = uv;
    if (horizontal) { currentUv.y += stepLength * 0.5; }
    else            { currentUv.x += stepLength * 0.5; }

    const float2 offset = horizontal ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    float2 uv1 = currentUv - offset;
    float2 uv2 = currentUv + offset;

    float lumaEnd1 = FxaaLuma(gSource.SampleLevel(gSampler, uv1, 0).rgb) - lumaLocalAvg;
    float lumaEnd2 = FxaaLuma(gSource.SampleLevel(gSampler, uv2, 0).rgb) - lumaLocalAvg;
    bool reached1 = abs(lumaEnd1) >= gradScaled;
    bool reached2 = abs(lumaEnd2) >= gradScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1) { uv1 -= offset; }
    if (!reached2) { uv2 += offset; }

    [loop]
    for (int it = 0; it < 12 && !reachedBoth; ++it)
    {
        if (!reached1)
        {
            lumaEnd1 = FxaaLuma(gSource.SampleLevel(gSampler, uv1, 0).rgb) - lumaLocalAvg;
            reached1 = abs(lumaEnd1) >= gradScaled;
        }
        if (!reached2)
        {
            lumaEnd2 = FxaaLuma(gSource.SampleLevel(gSampler, uv2, 0).rgb) - lumaLocalAvg;
            reached2 = abs(lumaEnd2) >= gradScaled;
        }
        if (!reached1) { uv1 -= offset * kStep[it]; }
        if (!reached2) { uv2 += offset * kStep[it]; }
        reachedBoth = reached1 && reached2;
    }

    const float dist1 = horizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    const float dist2 = horizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    const bool isDir1 = dist1 < dist2;
    const float distFinal = min(dist1, dist2);
    const float edgeLength = dist1 + dist2;
    const float pixelOffset = -distFinal / max(edgeLength, 1e-6) + 0.5;

    // If the end we are closest to has its luma on the same side of the local
    // average as the centre, this pixel is not on the side of the edge that
    // needs moving and the offset would push it the wrong way.
    const bool isLumaCenterSmaller = lumaM < lumaLocalAvg;
    const bool correctVariation = ((isDir1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel term: catches thin features and lone pixels the edge walk above
    // cannot resolve. Taken as a max so it never fights the edge offset.
    const float lumaAvg = (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaNWSW + lumaNESE);
    const float subDelta = saturate(abs(lumaAvg - lumaM) / max(range, 1e-6));
    const float subWeight = (-2.0 * subDelta + 3.0) * subDelta * subDelta;
    const float subOffset = subWeight * subWeight * gSubpixel;
    finalOffset = max(finalOffset, subOffset);

    float2 finalUv = uv;
    if (horizontal) { finalUv.y += finalOffset * stepLength; }
    else            { finalUv.x += finalOffset * stepLength; }

    // Alpha carried through from the centre: the present blit downstream reads
    // this buffer as-is, and the plain copy this replaces preserved it.
    gDest[tid.xy] = float4(gSource.SampleLevel(gSampler, finalUv, 0).rgb, texelM.a);
}
