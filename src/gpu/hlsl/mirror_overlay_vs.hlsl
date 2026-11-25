// MCLA GPU renderer -- the side mirrors' glass overlay, the vertex stage (mirror_overlay.h, perf/MIRROR-VIEW.md package MV4).
//
// One draw of the glass patch's own triangles, from the host's byte-swapped copy of the vertices the game's draw read (the car shadow map's way: POSITION as the one input, three floats of a float3
// or float4, or the first three of four halves, the pipeline's element says which). The arithmetic is mirror_overlay.hlsli's; the 128 byte push block is shared with the pixel stage. On Vulkan dxc
// flips the output y (-fvk-invert-y), the same pixels on both backends.

#include "mirror_overlay.hlsli"

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MirrorOverlayPush> g_push;
#else
ConstantBuffer<MirrorOverlayPush> g_push : register(b0, space4);
#endif

struct VsInput {
  [[vk::location(0)]] float3 position : POSITION;
};

struct VsOutput {
  float4 position : SV_Position;
  float3 view_pos : TEXCOORD0;  // the point in the main camera's view space, eye-relative: what the pixel stage reflects
};

VsOutput main(VsInput input) {
  const MirrorOverlayPush p = g_push;
  VsOutput o;
  const float3 pv = MirrorOverlayViewPos(p, input.position);
  o.position = MirrorOverlayClip(p, pv);
  o.view_pos = pv;
  return o;
}
