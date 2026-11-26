// MCLA GPU renderer -- the masks the temporal upscalers take as inputs (upscaler_masks.h).
//
// The Scene draws some of the game's draws a second time into an RGBA8 image (a "coverage" image), with
// the same depth and stencil test the game's own draw has: the cars' shadow decals, the particles and the
// light volumes into one, the car glass into another. What the second draw leaves there is, where the
// game's draw changes the scene colour:
//   alpha   the draw's opacity: the union of the layers, a + (1 - a) * what was there. Written for a
//           draw whose blend is an alpha blend (the source alpha says how much of the pixel it is).
//   rgb     the draw's colour, added: written for a draw that adds light whose amount only its colour
//           says (an additive blend with no alpha in it, a premultiplied one).
// This pass reads the image and writes the R8 mask: the larger of the alpha and the brightest colour
// channel, times a gain, so 1 is a pixel the upscaler should not trust its history on (the reactive mask
// of FSR, the bias-current-colour and reactive hints of DLSS) or whose pixel lock it should give up
// (FSR's transparency and composition mask, DLSS's hint of the same name).
//
// MASKS_MS (upscaler_masks_ms_ps): the coverage image has the samples of the game's depth (the host's
// MSAA level stands under the upscaler, mcla_gpu_msaa_with_upscaler: the second draws test the
// multisampled depth, so they draw into a multisampled image). The mask is made for each sample and the
// samples are averaged, which is what the game's own resolve does to the colour: the mask is the share
// of the pixel's samples the layer covers (an edge is fractional, as the colour's is), exact for the
// samples drawn. The mask of the mean would not be: the gain saturates (a glass pixel of alpha 0.3
// covering 2 of 4 samples is 0.5 here, 0.6 from the mean alpha).
//
// A zero sample count is the diagnostic (mcla_gpu_upscaler_mask_test): every pixel 1, nothing read, to see whether
// an upscaler takes the mask at all (the picture loses its accumulation).

#include "host_pass_common.hlsli"

#ifndef MASKS_MS
#define MASKS_MS 0
#endif

struct MaskInput {
  uint coverage;      // slot: the coverage image, RGBA8
  float gain;         // the mask's value for an opaque layer, and the share a fainter one keeps
  float color_gain;   // the same for the added colour
  uint samples;       // 0 all ones, 1 single sample, 2/4/8 multisampled
};

struct MaskPush {
  MaskInput first;
#if MASKS_PAIR
  MaskInput second;
#endif
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MaskPush> g_push;
#else
ConstantBuffer<MaskPush> g_push : register(b0, space4);
#endif

float MaskOf(float4 layers, MaskInput input) {
  const float added = max(layers.r, max(layers.g, layers.b));
  return saturate(max(layers.a * input.gain, added * input.color_gain));
}

float ResolveMask(int2 at, MaskInput input) {
  if (input.samples == 0u) {
    return 1.0f;
  }
  const uint coverage = input.coverage & 0x7FFFFFFFu;
#if MASKS_MS || MASKS_PAIR
  if (input.samples > 1u) {
    float sum = 0.0f;
    for (uint i = 0u; i < input.samples; ++i) {
      sum += MaskOf(g_textures_ms[coverage].Load(at, i), input);
    }
    return sum / float(input.samples);
  }
#endif
  return MaskOf(g_textures[coverage].Load(int3(at, 0)), input);
}

#if MASKS_PAIR
struct MaskOutputs {
  float reactive : SV_Target0;
  float transparency : SV_Target1;
};

MaskOutputs main(float4 position : SV_Position) {
  const int2 at = int2(position.xy);
  MaskOutputs out_px;
  out_px.reactive = ResolveMask(at, g_push.first);
  out_px.transparency = ResolveMask(at, g_push.second);
  return out_px;
}
#else
float main(float4 position : SV_Position) : SV_Target {
  return ResolveMask(int2(position.xy), g_push.first);
}
#endif
