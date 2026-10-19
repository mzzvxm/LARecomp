// MCLA GPU renderer -- SMAA 1x on the composed scene, before the UI (post.h).
//
// SMAA (thirdparty/smaa, MIT, Copyright (C) 2013 Jorge Jimenez, Jose I.
// Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez), preset HIGH,
// luma edge detection, its three passes chosen by SMAA_PASS:
//   1 edges   the scene's copy -> edges (R8G8), discarding pixels with none
//             (the image is cleared first)
//   2 weights edges + AreaTex + SearchTex -> blending weights (R8G8B8A8)
//   3 blend   the scene's copy + weights -> the scene's target (RGB written)
// The porting macros are the HLSL 4.1 ones of SMAA.hlsl over the renderer's
// bindless textures and samplers (SMAA_CUSTOM_SL); SMAA_RT_METRICS comes from
// the push block. The vertex-shader halves (offsets) run here, per pixel.

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

struct SmaaPush {
  uint source;    // 1: the copy; 2: the edges; 3: the copy
  uint second;    // 3: the weights
  uint area;      // 2: AreaTex
  uint search;    // 2: SearchTex
  float4 metrics;  // (1 / width, 1 / height, width, height)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<SmaaPush> g_push;
#else
ConstantBuffer<SmaaPush> g_push : register(b0, space4);
#endif

#define SMAA_RT_METRICS g_push.metrics
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(g_samplers[kSamplerLinearClamp], coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) \
  tex.SampleLevel(g_samplers[kSamplerPointClamp], coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
  tex.SampleLevel(g_samplers[kSamplerLinearClamp], coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(g_samplers[kSamplerLinearClamp], coord)
#define SMAASamplePoint(tex, coord) tex.Sample(g_samplers[kSamplerPointClamp], coord)
#define SMAASampleOffset(tex, coord, offset) \
  tex.Sample(g_samplers[kSamplerLinearClamp], coord, offset)
#define SMAAGather(tex, coord) tex.Gather(g_samplers[kSamplerLinearClamp], coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#include "smaa/SMAA.hlsl"

float4 main(float4 position : SV_Position) : SV_Target {
  // The pass covers its image from the origin: the pixel's centre is its
  // texture coordinate times the size.
  const float2 texcoord = position.xy * SMAA_RT_METRICS.xy;
#if SMAA_PASS == 1
  float4 offset[3];
  SMAAEdgeDetectionVS(texcoord, offset);
  return float4(SMAALumaEdgeDetectionPS(texcoord, offset, g_textures[g_push.source]), 0.0f, 0.0f);
#elif SMAA_PASS == 2
  float2 pixcoord;
  float4 offset[3];
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
  return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, g_textures[g_push.source],
                                         g_textures[g_push.area], g_textures[g_push.search],
                                         float4(0.0f, 0.0f, 0.0f, 0.0f));
#else
  float4 offset;
  SMAANeighborhoodBlendingVS(texcoord, offset);
  return SMAANeighborhoodBlendingPS(texcoord, offset, g_textures[g_push.source],
                                    g_textures[g_push.second]);
#endif
}
