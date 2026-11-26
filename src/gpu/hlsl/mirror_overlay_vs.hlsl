// MCLA GPU renderer -- the side mirrors' glass overlay, the vertex stage (mirror_overlay.h, perf/MIRROR-VIEW.md package MV4).
//
// One draw of the glass patch's own triangles, from the host's byte-swapped copy of the vertices the game's draw read (the car shadow map's way: POSITION as the one input, three floats of a float3
// or float4, or the first three of four halves, the pipeline's element says which). The arithmetic is mirror_overlay.hlsli's; the 128 byte push block is shared with the pixel stage. On Vulkan dxc
// flips the output y (-fvk-invert-y), the same pixels on both backends.

#include "mirror_overlay.hlsli"

#ifdef __spirv__
struct DrawAddress { uint64_t address; };
[[vk::push_constant]] ConstantBuffer<DrawAddress> g_address;
#define g_draw vk::RawBufferLoad<MirrorOverlayDraw>(g_address.address, 16)
#else
ConstantBuffer<MirrorOverlayDraw> g_draw : register(b0, space4);
#endif

struct VsInput {
  [[vk::location(0)]] float3 position : POSITION;
};

MirrorOverlayVaryings main(VsInput input) {
  const MirrorOverlayDraw draw = g_draw;
  const MirrorOverlayPush p = draw.mirror;
  MirrorOverlayVaryings o;
  const float4 model = float4(input.position, 1.0);
  o.mask_uv = float2(dot(model, draw.mask_u), dot(model, draw.mask_v));
  const float3 pv = MirrorOverlayViewPos(p, input.position);
  o.position = MirrorOverlayClip(p, pv);
  o.view_pos = pv;
  return o;
}
