// MCLA GPU renderer -- frame generation: a finished present image into the swap chain's back buffer
// (frame_generation.h).
//
// The pacer thread's whole drawing: texel for texel, from the bindless slot into a target of the same
// size and format (the swap chain's, SDR 8-bit or HDR10), so nothing is converted: what the present
// pass wrote is what the display gets. Drawn rather than copied so that on Vulkan the back buffer's
// first use is a colour attachment write, which is what the acquire semaphore's wait guards.

#include "host_pass_common.hlsli"

struct CopyPush {
  uint source;  // the present image's bindless slot
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CopyPush> g_push;
#else
ConstantBuffer<CopyPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  return g_textures[g_push.source].Load(int3(int2(position.xy), 0));
}
