// MCLA GPU renderer -- the frame's rows and what a hit's light is made of (CITY-RT M2 package H4; the C++ twin is src/gpu/rt_hit_frame.h). First half of rt_hit_common.hlsli.
//
// The frame's rows (the sun's direction and colour, the ambient, the environment map's slots, the night factor, the world's axes, the game's sun cascades' places), RtHitFrame and its loader,
// the unpacking of a packed colour, the hemisphere term and the ambient light of a hit. The reading of textures, the environment map, the cascades and the sun's ray are the second half,
// rt_hit_sample.hlsli; most includers take both through rt_hit_common.hlsli. The car's hit shading includes this half first and the other after its own records, which keeps the
// order of the functions in the compiled shader as it was before the move (tools/check_rt_hit_identity.py compares the two compiles).
//
// THE INCLUDER defines, before including, RT_HIT_FRAME_ROW(i): the frame's row i as a uint4 (the car passes' is `gCarRtMaterials[i]`: the frame's rows come first in binding 5; a pass without
// the car's instances has them in a buffer of its own: the SSR fallback, the GI passes).

#ifndef MCLA_RT_HIT_FRAME_HLSLI
#define MCLA_RT_HIT_FRAME_HLSLI

#ifndef RT_HIT_FRAME_ROW
#error "define RT_HIT_FRAME_ROW(i) (the frame's row i, a uint4) before including rt_hit_frame.hlsli"
#endif

#define RT_HIT_FRAME_SHADE 1
#define RT_HIT_FRAME_SHADOW 2
#define RT_HIT_FRAME_ENV 4
#define RT_HIT_FRAME_SUN 8
#define RT_HIT_FRAME_CASCADE 16

static const uint kRtHitFrameRows = 32;
static const uint kRtHitCascadeHeaderRow = 8;
static const uint kRtHitCameraRow = 9;
static const uint kRtHitCascadeRightRow = 10;
static const uint kRtHitCascadeTopRow = 11;
static const uint kRtHitCascadeFirstRow = 12;
static const uint kRtHitCascadeRowsEach = 5;
static const uint kRtHitMaxCascades = 4;
static const float kRtHitCascadeNormalOffset = 0.03;
static const uint kRtHitNoSlot = 0xFFFFFFFFu;
static const float kRtHitTintMax = 2.0;
static const float kRtHitConeLodScale = 20.0;
static const float kRtHitEmissiveGain = 3.0;
static const float kRtHitFillShare = 0.2;  // the ambient is at least this share of the sun's colour (rt_hit_frame.h kFillShare)

struct RtHitFrame
{
    float3 toSun;      // unit, view space
    uint flags;
    float3 sun;        // the sun's colour, the paint's units
    float shadowReach; // metres
    float3 ambient;
    float ambientGain;
    uint envSharp;     // bindless slot, bit 31 the gamma bit
    uint envBlur;
    float night;       // 0 day, 1 night
    float specGain;
    float3 sky;
    float gain;
    float3 axisX;      // the world's axes in view space
    float3 axisY;
    float3 axisZ;
};

RtHitFrame RtHitLoadFrame()
{
    const uint4 r0 = RT_HIT_FRAME_ROW(0);
    const uint4 r1 = RT_HIT_FRAME_ROW(1);
    const uint4 r2 = RT_HIT_FRAME_ROW(2);
    const uint4 r3 = RT_HIT_FRAME_ROW(3);
    const uint4 r4 = RT_HIT_FRAME_ROW(4);
    RtHitFrame f;
    f.toSun = asfloat(r0.xyz);
    f.flags = r0.w;
    f.sun = asfloat(r1.xyz);
    f.shadowReach = asfloat(r1.w);
    f.ambient = asfloat(r2.xyz);
    f.ambientGain = asfloat(r2.w);
    f.envSharp = r3.x;
    f.envBlur = r3.y;
    f.night = asfloat(r3.z);
    f.specGain = asfloat(r3.w);
    f.sky = asfloat(r4.xyz);
    f.gain = asfloat(r4.w);
    f.axisX = asfloat(RT_HIT_FRAME_ROW(5).xyz);
    f.axisY = asfloat(RT_HIT_FRAME_ROW(6).xyz);
    f.axisZ = asfloat(RT_HIT_FRAME_ROW(7).xyz);
    return f;
}

float3 RtHitRgb10(uint v)
{
    return float3(float(v & 1023u), float((v >> 10) & 1023u), float((v >> 20) & 1023u)) * (kRtHitTintMax / 1023.0);
}

// The hemisphere term of a hit with the unit `normal`: what faces down is darker (0.2 at the floor of it), what faces up is lit in full.
float RtHitHemisphere(RtHitFrame f, float3 normal)
{
    return clamp(0.6 + 0.4 * dot(normal, f.axisY), 0.2, 1.0);
}

// The ambient light of a hit given its hemisphere term: at least kRtHitFillShare of the sun's colour (in the sun's hue), by the frame's gain and the hemisphere.
float3 RtHitAmbientLight(RtHitFrame f, float hemisphere)
{
    const float3 fill = max(f.ambient, f.sun * kRtHitFillShare);
    return fill * (f.ambientGain * hemisphere);
}

#endif  // MCLA_RT_HIT_FRAME_HLSLI
