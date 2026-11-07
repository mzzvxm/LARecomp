// MCLA GPU renderer -- ambient occlusion, pass 4: the apply (ao.h).
//
// Drawn into the main view's colour target at the render size, in a pipeline whose blend is
// dst.rgb *= src.rgb (every sample of a multisampled target gets the same factor), or, for a debug
// view, a plain write of a grey. One pixel computes the factor
//
//   factor = lerp(1, ao, strength * ambient_share * distance_fade)
//
//   ao             the denoised visibility at the pixel; from the half resolution image (AoLow) by a
//                  bilinear upsample whose weights fall with the difference of linear depth, so
//                  it does not leak over an edge;
//   ambient_share  how much of the light on this surface is ambient, from the game's own constants:
//                  La / (La + Ls * N.L), La the ambient light's luma (PS c26), Ls the sun's (PS
//                  c39), L the direction to the sun (minus VS c16, which holds the way the light
//                  travels, through the camera), N the normal from the full resolution depth. The
//                  scene colour is albedo * (ambient + sun) + specular, so a multiply by
//                  lerp(1, ao, share) is the occlusion of the ambient part alone, whatever the
//                  albedo. The look's direct number (extra.z) moves the share towards 1:
//                  share' = lerp(share, 1, direct), so the occlusion darkens the sun-lit part too
//                  (direct 1: all of it);
//   distance_fade  1 up to shape.w, to 0 at extra.y (smoothstep).
//
// Where the factor is near 1 (the sky, flat surfaces, past the fade) the pixel is not written.
//
// Debug views (flags bits 0-7): 1 the factor, 2 the visibility, 3 the ambient share, 4 the
// view-space normal, 5 the linear depth (log), 6 the temporal pass's mask (the image it reads then
// holds the share of the history each pixel kept instead of the visibility: white a full history,
// black none, grey part of it); written as a grey of 0.3 of the value so that 1.0 reads below the
// tone map's white.

#include "ao_common.hlsli"

static const float kViewScale = 0.3f;

// The stored depth at `p` clamped into the target, as a linear distance.
float LinearAt(int2 p, int2 last) {
  return AoLinearDepth(AoLoadStored(clamp(p, int2(0, 0), last)));
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.xy;
  const int2 last = int2(size) - 1;
  const uint view = AoView();
  const float tc = LinearAt(pixel, last);
  const float fade_end = g_push.extra.y;
  if (view == 0u && tc >= fade_end) {
    discard;
  }

  // The visibility.
  float ao;
  if (AoLow()) {
    const int2 asize = int2(g_push.sizes.zw);
    const float2 q = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
    const int2 q0 = int2(floor(q));
    const float2 f = q - float2(q0);
    float sum = 0.0f;
    float weights = 0.0f;
    for (int j = 0; j < 2; ++j) {
      for (int i = 0; i < 2; ++i) {
        const int2 c = clamp(q0 + int2(i, j), int2(0, 0), asize - 1);
        const float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
        const float td = AoLoadLinear(g_push.slots.w, c);
        const float dz = abs(td - tc) / max(0.02f * tc, 0.05f);
        const float w = bilinear * max(exp(-dz * dz), 1e-4f);
        sum += g_textures[g_push.slots.z].Load(int3(c, 0)).x * w;
        weights += w;
      }
    }
    ao = sum / max(weights, 1e-8f);
  } else {
    ao = g_textures[g_push.slots.z].Load(int3(pixel, 0)).x;
  }

  // The normal from the full resolution depth, as the main pass has it.
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float tl = LinearAt(pixel + int2(-1, 0), last);
  const float tr = LinearAt(pixel + int2(1, 0), last);
  const float tt = LinearAt(pixel + int2(0, -1), last);
  const float tb = LinearAt(pixel + int2(0, 1), last);
  const float3 h = abs(tl - tc) < abs(tr - tc)
                       ? pc - AoViewPos(float2(pixel + int2(-1, 0)) + 0.5f, size, tl)
                       : AoViewPos(float2(pixel + int2(1, 0)) + 0.5f, size, tr) - pc;
  const float3 v = abs(tt - tc) < abs(tb - tc)
                       ? AoViewPos(float2(pixel + int2(0, -1)) + 0.5f, size, tt) - pc
                       : pc - AoViewPos(float2(pixel + int2(0, 1)) + 0.5f, size, tb);
  float3 normal = cross(v, h);
  const float normal_len = length(normal);
  normal = normal_len > 1e-20f ? normal / normal_len : float3(0.0f, 0.0f, -1.0f);

  // The share of the light that is ambient.
  const float ambient = g_push.light.w;
  const float sun = g_push.extra.x * saturate(dot(normal, g_push.light.xyz));
  const float ambient_share = (ambient + sun) > 1e-6f ? ambient / (ambient + sun) : 1.0f;
  const float share = lerp(ambient_share, 1.0f, saturate(g_push.extra.z));
  const float fade = 1.0f - smoothstep(g_push.shape.w, fade_end, tc);
  const float factor = lerp(1.0f, ao, saturate(g_push.shape.z * share * fade));

  if (view != 0u) {
    float3 grey = float3(factor, factor, factor);
    if (view == 2u || view == 6u) {
      grey = float3(ao, ao, ao);
    } else if (view == 3u) {
      grey = float3(share, share, share);
    } else if (view == 4u) {
      grey = normal * 0.5f + 0.5f;
    } else if (view == 5u) {
      const float d = saturate(log2(1.0f + min(tc, kAoSky)) / log2(1.0f + 2.0f * fade_end));
      grey = float3(d, d, d);
    }
    return float4(grey * kViewScale, 1.0f);
  }
  if (factor >= 0.999f) {
    discard;
  }
  return float4(factor, factor, factor, 1.0f);
}
