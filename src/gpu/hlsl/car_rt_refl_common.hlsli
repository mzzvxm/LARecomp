// MCLA GPU renderer -- the player's car reflecting itself, ray traced: the numbers the trace (car_rt_refl_cs.hlsl) and the denoise
// (car_rt_refl_denoise_cs.hlsl) share. One source of them for the two shaders; car_rt_refl_rules.h has the same arithmetic in C++ (the
// unit test checks that one): change one, change the other.

#ifndef CAR_RT_REFL_COMMON_HLSLI
#define CAR_RT_REFL_COMMON_HLSLI

static const float kMaxSlack = 0.05;         // m: the most the slope of a surface adds to a distance tolerance

// The spacing of half floats at t.
float HalfStep(float t)
{
    return exp2(floor(log2(max(t, 6.2e-5))) - 10.0);
}

float SurfaceSlope(float nv)
{
    return min(sqrt(max(1.0 - nv * nv, 0.0)) / max(nv, 0.1), 8.0);
}

// How far the distance of the same surface may differ between two sources of it: a half float's step, the game's depth noise, and the
// slope of the surface across `footprint` pixels (or texels), at most kMaxSlack.
float Tolerance(float t, float pixelMeters, float footprint, float nv)
{
    const float slack = min(footprint * pixelMeters * SurfaceSlope(nv), kMaxSlack);
    return HalfStep(t) + 0.01 + 0.005 * t + slack;
}

// One pixel's (or one image texel's) side in metres at view distance t, for a picture `edge` pixels high.
float PixelMeters(float t, float tanY, float edge)
{
    return t * 2.0 * tanY / max(edge, 1.0);
}

// How much a history texel at view distance texelDistance is the surface a point at distance t is on: 0 where the texel holds no vehicle,
// 1 within half the tolerance, falling to 0 at the tolerance.
float MatchWeight(float texelDistance, float t, float tolerance)
{
    return texelDistance > 0.0 ? 1.0 - smoothstep(0.5 * tolerance, tolerance, abs(texelDistance - t)) : 0.0;
}

#endif  // CAR_RT_REFL_COMMON_HLSLI
