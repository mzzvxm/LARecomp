// MCLA GPU renderer -- sun shafts, pass 3: the light added to the scene (shafts.h).
//
// Drawn into the main view's colour target at the render size, in a pipeline whose blend adds
// (ONE, ONE: every sample of a multisampled target gets the same light, the alpha is not written).
// The pixel takes the blurred image bilinearly (it is half the picture's size and smooth) and gives
// it the light's colour, which the host made of the sun's colour, the intensity and the fades.
//
// Debug views (the push's view): 1 the sky image and 2 the blurred image as a grey (0.3 of the value,
// so 1.0 reads below the tone map's white), 3 the light alone; the pipeline then replaces the colour
// instead of adding.

#include "shafts_common.hlsli"
#include "host_pass_samplers.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 uv = position.xy / g_push.sizes.xy;
  const float value =
      g_textures[g_push.slots.y].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0).x;
  const uint view = g_push.slots.w;
  const float3 light = g_push.tint.xyz * value;
  if (view == 0u) {
    if (!(max(light.x, max(light.y, light.z)) > 1.0f / 4096.0f)) {
      discard;
    }
    return float4(light, 0.0f);
  }
  if (view == 3u) {
    return float4(light, 1.0f);
  }
  const float grey = 0.3f * value;
  return float4(grey, grey, grey, 1.0f);
}
