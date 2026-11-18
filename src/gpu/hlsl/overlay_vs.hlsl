// MCLA GPU renderer -- overlay vertex shader.
//
// Input is mcla::gpu::OverlayVertex = ImGui's ImDrawVert (x, y, u, v, RGBA8 color) in the
// frame's coordinate space (ImDrawData::DisplaySize): y grows down,
// so scale.y is negative. On Vulkan dxc flips the output y (-fvk-invert-y),
// which keeps one set of push constants for both backends.

#include "overlay_common.hlsli"

struct OverlayVertexInput {
  [[vk::location(0)]] float2 position : POSITION;
  [[vk::location(1)]] float2 texcoord : TEXCOORD;
  [[vk::location(2)]] float4 color : COLOR;
};

OverlayVaryings main(OverlayVertexInput input) {
  OverlayVaryings output;
  output.position = float4(input.position * g_push.scale + g_push.translate, 0.0f, 1.0f);
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}
