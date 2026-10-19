// MCLA GPU renderer -- FSR1 EASU, one upscaling pass of the present (present.h).
//
// AMD FidelityFX Super Resolution 1.0, Edge Adaptive Spatial Upsampling, in
// FP32 (thirdparty/ffx-fsr1, MIT, Copyright (c) 2021 Advanced Micro Devices).
// The integration follows Xenia's guest_output_ffx_fsr_easu.ps.xesl
// (Copyright 2022 Ben Vanik, BSD-3-Clause): the four constants FsrEasuCon
// would write are built here from the input/output size ratio and the input's
// inverse size (the input image fills its texture), so the push block stays
// small; the taps are Gather R/G/B through a linear clamp sampler.

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

struct EasuPush {
  uint source;            // the input image, a bindless 2D slot
  uint reserved;
  float2 ratio;           // input size / output size
  float2 input_size_inv;  // 1 / input size
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<EasuPush> g_push;
#else
ConstantBuffer<EasuPush> g_push : register(b0, space4);
#endif

#define A_GPU 1
#define A_HLSL 1
#include "ffx-fsr1/ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) {
  return g_textures[g_push.source].GatherRed(g_samplers[kSamplerLinearClamp], p);
}
AF4 FsrEasuGF(AF2 p) {
  return g_textures[g_push.source].GatherGreen(g_samplers[kSamplerLinearClamp], p);
}
AF4 FsrEasuBF(AF2 p) {
  return g_textures[g_push.source].GatherBlue(g_samplers[kSamplerLinearClamp], p);
}
#include "ffx-fsr1/ffx_fsr1.h"

float4 main(float4 position : SV_Position) : SV_Target {
  const uint4 con0 = asuint(float4(g_push.ratio, 0.5f * g_push.ratio - 0.5f));
  const uint4 con1 = asuint(float4(1.0f, 1.0f, 1.0f, -1.0f) * g_push.input_size_inv.xyxy);
  const uint4 con2 = asuint(float4(-1.0f, 2.0f, 1.0f, 2.0f) * g_push.input_size_inv.xyxy);
  const uint4 con3 = asuint(float4(0.0f, 4.0f * g_push.input_size_inv.y, 0.0f, 0.0f));
  AF3 color;
  // The pass covers its whole target from the origin: the pixel is the
  // integer part of SV_Position.
  FsrEasuF(color, AU2(position.xy), con0, con1, con2, con3);
  return float4(color, 1.0f);
}
