// MCLA GPU renderer -- FXAA on the composed scene, before the UI (post.h).
//
// NVIDIA FXAA 3.11 (thirdparty/fxaa, BSD-3-Clause, Copyright (c) 2014-2015
// NVIDIA CORPORATION), PC quality path, HLSL 5 texture functions. The
// parameters are Xenia's "fxaa" (src/xenia/gpu/shaders/fxaa.cs.hlsl and
// fxaa.hlsli, Copyright Ben Vanik, BSD-3-Clause): preset 12, subpixel 0.75,
// edge thresholds 0.166 and 0.0833, the luma read from alpha (post_luma_ps).
// A pixel shader here, writing the scene's own target through an RGB write
// mask (its alpha stays the guest's).

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

struct FxaaPush {
  uint source;  // post_luma_ps's image: rgb and luma
  uint reserved;
  float2 rcp_frame;  // 1 / the image's size
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<FxaaPush> g_push;
#else
ConstantBuffer<FxaaPush> g_push : register(b0, space4);
#endif

#define FXAA_PC 1
#define FXAA_HLSL_5 1
#define FXAA_QUALITY__PRESET 12
#include "fxaa/FXAA3_11.h"

float4 main(float4 position : SV_Position) : SV_Target {
  FxaaTex image;
  image.smpl = g_samplers[kSamplerLinearClamp];
  image.tex = g_textures[g_push.source];
  const float2 rcp = g_push.rcp_frame;
  // SV_Position is the pixel's centre: its texture coordinate as is. The
  // console arguments are unused on the PC path; given as Xenia gives them.
  const float2 pixel = floor(position.xy);
  return float4(FxaaPixelShader(position.xy * rcp,
                                (pixel.xyxy + float2(0.0f, 1.0f).xxyy) * rcp.xyxy,
                                image, image, image, rcp,
                                float2(-0.5f, 0.5f).xxyy * rcp.xyxy,
                                float2(-2.0f, 2.0f).xxyy * rcp.xyxy,
                                float2(8.0f, -4.0f).xxyy * rcp.xyxy,
                                0.75f, 0.166f, 0.0833f, 8.0f, 0.125f, 0.05f,
                                float4(1.0f, -1.0f, 0.25f, -0.25f))
                    .rgb,
                1.0f);
}
