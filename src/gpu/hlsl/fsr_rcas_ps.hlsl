// MCLA GPU renderer -- FSR1 RCAS, the present's sharpening pass (present.h).
//
// AMD FidelityFX Super Resolution 1.0, Robust Contrast Adaptive Sharpening, in
// FP32 (thirdparty/ffx-fsr1, MIT, Copyright (c) 2021 Advanced Micro Devices).
// The integration follows Xenia's guest_output_ffx_fsr_rcas.xesli (Copyright
// 2022 Ben Vanik, BSD-3-Clause): 1:1, the output may sit at an offset in its
// target (the letterbox), the sharpness arrives already as FsrRcasCon makes it
// (2^-stops). Taps outside the input are clamped to its edge, so the border
// pixels sharpen against themselves rather than against zeros.

#include "host_pass_common.hlsli"

struct RcasPush {
  uint source;         // the input image, a bindless 2D slot, the output's size
  float sharpness;     // 2^-stops
  int2 output_offset;  // target pixel of the input's (0, 0)
  int2 input_max;      // input size - 1
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RcasPush> g_push;
#else
ConstantBuffer<RcasPush> g_push : register(b0, space4);
#endif

#define A_GPU 1
#define A_HLSL 1
#include "ffx-fsr1/ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
  return AF4(g_textures[g_push.source].Load(int3(clamp(int2(p), int2(0, 0), g_push.input_max),
                                                 0)).rgb,
             1.0f);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx-fsr1/ffx_fsr1.h"

float4 main(float4 position : SV_Position) : SV_Target {
  const uint2 pixel = uint2(int2(position.xy) - g_push.output_offset);
  // FsrRcasCon: con[0] the sharpness; con[1] (packed halves) is for the FP16
  // path only.
  const uint4 con = uint4(asuint(g_push.sharpness), 0u, 0u, 0u);
  AF3 color;
  FsrRcasF(color.r, color.g, color.b, pixel, con);
  return float4(color, 1.0f);
}
