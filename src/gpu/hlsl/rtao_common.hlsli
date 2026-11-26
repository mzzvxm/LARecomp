#ifndef MCLA_GPU_RTAO_COMMON_HLSLI
#define MCLA_GPU_RTAO_COMMON_HLSLI

static const uint kFlagRaysShift = 0;
static const uint kFlagBoost = 1u << 3;
static const uint kFlagAlphaShift = 4;
static const uint kFlagAlphaRefShift = 12;
static const uint kFlagMs = 1u << 6;
static const uint kFlagRaw = 1u << 7;
static const uint kFlagCheckOpen = 1u << 8;
static const uint kFlagCheckClosed = 1u << 9;
static const uint kFlagInterior = 1u << 10;
static const uint kFlagBlue = 1u << 11;
static const uint kFlagProbe = 1u << 20;
static const uint kFlagDepthHalf = 1u << 21;
static const uint kFlagRate = 1u << 23;
static const uint kFlagStats = 1u << 28;

static const uint kStatRays = 0u, kStatHits = 1u, kStatCandidates = 2u, kStatAlpha = 3u;
static const uint kStatCabinRays = 4u, kStatCabinHits = 5u, kStatUncovered = 6u;
static const float kMinAbove = 0.08;
static const float kTwoPi = 6.283185307179586;

struct RtAoPush {
  uint4 slots;
  uint4 sizes;
  uint4 trace;
  float4 lens;
  float4 look;
  float4 box_min;
  float4 box_max;
  uint4 rate;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtAoPush> g_push;
#else
ConstantBuffer<RtAoPush> g_push : register(b0, space6);
#endif

#endif
