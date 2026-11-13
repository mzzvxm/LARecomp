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
// mode 1 is the diagnostic (mcla_gpu_upscaler_mask_test): every pixel 1, nothing read, to see whether
// an upscaler takes the mask at all (the picture loses its accumulation).

#include "host_pass_common.hlsli"

struct MaskPush {
  uint coverage;      // slot: the coverage image, RGBA8 (not read in mode 1)
  float gain;         // the mask's value for an opaque layer, and the share a fainter one keeps
  float color_gain;   // the same for the added colour
  uint mode;          // 0 the coverage image, 1 every pixel 1
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MaskPush> g_push;
#else
ConstantBuffer<MaskPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  if (g_push.mode == 1u) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const int3 at = int3(int2(position.xy), 0);
  const float4 layers = g_textures[g_push.coverage & 0x7FFFFFFFu].Load(at);
  const float added = max(layers.r, max(layers.g, layers.b));
  const float coverage = max(layers.a * g_push.gain, added * g_push.color_gain);
  return float4(saturate(coverage), 0.0f, 0.0f, 0.0f);
}
