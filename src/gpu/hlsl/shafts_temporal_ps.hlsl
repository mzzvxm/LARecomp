// MCLA GPU renderer -- sun shafts, pass 2b: the blurred image averaged over frames (shafts.h, noise_resolve.h).
//
// NOISE-RESOLVE (mcla_gpu_noise_resolve_no_upscaler). The radial blur starts every texel's ray a fraction of one step along its way, a blue noise tile stepped along the R2 sequence by the frame
// (shafts_noise.hlsli), so that the steps do not show as bands. With a temporal upscaler its history averages that noise; with none the noise used to stand still (the host sent frame 0) because
// nothing averaged a pattern that changed. This pass is the average: the frame's blurred image with the last frame's result, which the host keeps in an image of its own.
//
// The image is smooth (the sky's brightness blurred along the rays to the sun, half the picture's size or less) and everything in it is far, so a camera that turns moves it as the sun moves in the
// picture: the history is read at the texel's place shifted by the sun's own move (push tint.xy = the last frame's sun uv minus this frame's; the history texel of uv is at uv + shift), bilinearly,
// no matrices and no depth. It is clamped to the range of this frame's 3 x 3 neighbourhood plus a margin (sun.w), the clamp of a temporal anti-aliasing filter, so that a silhouette that moved by
// parallax (a tree beside the road) leaves a ghost only inside the margin; a texel the shift takes off the image has no history. The weight is sun.z (noise_resolve.h ShaftsHistoryWeight).
//
// Push (shafts_common.hlsli): slots.x the history image's slot, slots.y the blurred image's, slots.z bit 0 the history is on (else the pass copies the blurred image: the first frame, a cut, a
// frame whose sun moved too far); sizes.xy the target's size; sun.z the weight the history keeps; sun.w the clamp's margin; tint.xy the shift.
// Output R16_FLOAT: the next frame's history and what the add pass reads.

#include "shafts_common.hlsli"
#include "host_pass_samplers.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const float2 size = g_push.sizes.xy;
  const int2 pixel = int2(position.xy);
  const int2 isize = int2(size);
  const float current = g_textures[g_push.slots.y].Load(int3(pixel, 0)).x;
  if ((g_push.slots.z & 1u) == 0u || !(g_push.sun.z > 0.0f)) {
    return float4(current, 0.0f, 0.0f, 0.0f);
  }
  float lo = current;
  float hi = current;
  [unroll] for (int dy = -1; dy <= 1; ++dy) {
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
      const int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1);
      const float v = g_textures[g_push.slots.y].Load(int3(q, 0)).x;
      lo = min(lo, v);
      hi = max(hi, v);
    }
  }
  const float2 uv = (float2(pixel) + 0.5f) / size + g_push.tint.xy;
  if (any(uv < 0.0f) || any(uv > 1.0f)) {
    return float4(current, 0.0f, 0.0f, 0.0f);
  }
  const float history = g_textures[g_push.slots.x].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0).x;
  const float held = clamp(history, lo - g_push.sun.w, hi + g_push.sun.w);
  return float4(lerp(current, held, g_push.sun.z), 0.0f, 0.0f, 0.0f);
}
