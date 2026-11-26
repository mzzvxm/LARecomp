// MCLA GPU renderer -- sun shafts, pass 3: the light added to the scene (shafts.h).
//
// Drawn into the main view's colour target at the render size, in a pipeline whose blend adds
// (ONE, ONE: every sample of a multisampled target gets the same light, the alpha is not written).
// The pixel takes the blurred image bilinearly (it is half the picture's size or less, and smooth) and gives
// it the light's colour, which the host made of the sun's colour, the intensity and the fades.
//
// Debug views (the push's view): 1 the sky image and 2 the blurred image as a grey (0.3 of the value,
// so 1.0 reads below the tone map's white), 3 the light alone; the pipeline then replaces the colour
// instead of adding.

#include "shafts_common.hlsli"
#include "host_pass_samplers.hlsli"
#include "shafts_apply.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 uv = position.xy / g_push.sizes.xy;
  const uint view = g_push.slots.w;
  if (view == 0u) {
    const float3 light = SampleShaftLight(uv, g_push.slots.y, g_push.tint.xyz);
    if (!any(light > 0.0f)) {
      discard;
    }
    return float4(light, 0.0f);
  }
  if (view == 3u) {
    return float4(g_push.tint.xyz * SampleShaftValue(uv, g_push.slots.y), 1.0f);
  }
  const float grey = 0.3f * SampleShaftValue(uv, g_push.slots.y);
  return float4(grey, grey, grey, 1.0f);
}
