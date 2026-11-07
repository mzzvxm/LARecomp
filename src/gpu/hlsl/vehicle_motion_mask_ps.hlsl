// MCLA GPU renderer -- the shadow decals' mask for the upscaler (vehicle_motion_mask.h).
//
// The game draws a blended dark patch on the road under each car (and four small ones under the player's
// wheels): it moves with the car while the road under it moves with the camera, so a pixel of it has two
// motions and no vector is right (feature-parity/GHOSTING-DESIGN.md 9.4). The scene draws those draws a
// second time into an RGBA8 image whose alpha is the patch's opacity, without the depth and stencil test
// the game's own draw has; this pass keeps that alpha where the game's draw would have drawn, which is
// where the stencil is 0 (the game's draw tests stencil EQUAL 0 with write mask 0: never on a vehicle, and
// not on the world pixels whose stencil is 1), and writes it as the R8 mask the upscaler takes as its
// reactive input (FSR) or its bias-current-colour hint (DLSS): 1 rejects the history of the pixel.

#include "temporal_common.hlsli"

struct MaskPush {
  uint coverage;  // slot: the second draws' image, RGBA8, alpha = the patch's opacity
  uint stencil;   // stencil reference (temporal_common.hlsli)
  float gain;     // the mask's value for an opaque patch
  uint reserved;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MaskPush> g_push;
#else
ConstantBuffer<MaskPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 at = int3(int2(position.xy), 0);
  const float opacity = g_textures[g_push.coverage & 0x7FFFFFFFu].Load(at).a;
  const float keep = StencilByte(g_push.stencil, at) == 0u ? 1.0f : 0.0f;
  return float4(saturate(opacity * g_push.gain) * keep, 0.0f, 0.0f, 0.0f);
}
