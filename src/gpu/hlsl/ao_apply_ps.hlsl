// MCLA GPU renderer -- ambient occlusion and contact shadows, the apply (ao.h).
//
// Drawn into the main view's colour target at the render size, in a pipeline whose blend is
// dst.rgb *= src.rgb (every sample of a multisampled target gets the same factor), or, for a debug
// view, a plain write of a grey. One pixel computes the factor
//
//   factor = ao_factor * contact_factor
//
// the occlusion's (when its term is on, flag bit 22) and the contact shadows' (flag bit 23):
//
//   ao_factor      lerp(1, ao, strength * ambient_share * distance_fade)
//     ao             the denoised visibility at the pixel; from the half resolution image (AoLow) by a
//                    bilinear upsample whose weights fall with the difference of linear depth, so
//                    it does not leak over an edge;
//     ambient_share  how much of the light on this surface is ambient, from the game's own constants:
//                    La / (La + Ls * N.L), La the ambient light's luma (PS c26), Ls the sun's (PS
//                    c39), L the direction to the sun (minus VS c16, which holds the way the light
//                    travels, through the camera), N the normal from the full resolution depth. The
//                    scene colour is albedo * (ambient + sun) + specular, so a multiply by
//                    lerp(1, ao, share) is the occlusion of the ambient part alone, whatever the
//                    albedo. The look's direct number (extra.z) moves the share towards 1:
//                    share' = lerp(share, 1, direct), so the occlusion darkens the sun-lit part too
//                    (direct 1: all of it);
//     distance_fade  1 up to shape.w, to 0 at extra.y (smoothstep).
//
//   contact_factor 1 - sun_share * strength * (1 - cs) * fade: the contact shadows darken only the
//                  sun's share of the light, so they cannot darken what the game already shadows
//     cs             the contact visibility (ao_contact_ps and its one edge-keeping filter pass), upsampled as ao is;
//     sun_share      Ls * N.L * vis / (La + Ls * N.L * vis), vis the game's sun shadow mask where
//                    the frame has one (flag bit 24: slots2.w its bindless slot, shape.x the factor
//                    that turns its stored value into a visibility, 2: the lit shaders multiply it
//                    by 2), 1 where it has none (6 h, 18 h, rain, night);
//     strength       extra.w: the setting's, times the sun's elevation gate and the wetness gate
//                    (ao.cpp), so 0 never reaches this shader;
//     fade           1 up to lens.z, to 0 at lens.w (smoothstep): a ray of a few tenths of a metre
//                    is under a pixel far away.
//
// Where the factor is near 1 (the sky, flat surfaces, past the fades) the pixel is not written.
//
// Debug views (flags bits 0-7): 1 the factor, 2 the visibility, 3 the ambient share, 4 the
// view-space normal, 5 the linear depth (log), 6 the temporal pass's mask (the image it reads then
// holds the share of the history each pixel kept instead of the visibility: white a full history,
// black none, grey part of it), 8 the contact factor, 9 the contact visibility, 10 the sun's
// visibility (the mask, or 1), 11 the sun's share of the light; written as a grey of 0.3 of the value so
// that 1.0 reads below the tone map's white.

#include "ao_common.hlsli"
#include "host_pass_samplers.hlsli"

static const float kViewScale = 0.3f;

bool AoTermOn() { return (g_push.slots2.z & (1u << 22)) != 0u; }
bool ContactOn() { return (g_push.slots2.z & (1u << 23)) != 0u; }
bool MaskOn() { return (g_push.slots2.z & (1u << 24)) != 0u; }

// The stored depth at `p` clamped into the target, as a linear distance.
float LinearAt(int2 p, int2 last) {
  return AoLinearDepth(AoLoadStored(clamp(p, int2(0, 0), last)));
}

// A visibility image (the AO's or the contact shadows') at the pixel of the picture: as it is, or,
// from the half resolution one, a bilinear upsample whose weights fall with the difference of linear
// depth.
float VisibilityAt(uint slot, int2 pixel, float tc) {
  if (!AoLow()) {
    return g_textures[slot].Load(int3(pixel, 0)).x;
  }
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
      sum += g_textures[slot].Load(int3(c, 0)).x * w;
      weights += w;
    }
  }
  return sum / max(weights, 1e-8f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.xy;
  const int2 last = int2(size) - 1;
  const uint view = AoView();
  const float tc = LinearAt(pixel, last);
  const bool ao_on = AoTermOn();
  const bool contact_on = ContactOn();
  const float fade_end = g_push.extra.y;
  const float reach = max(ao_on ? fade_end : 0.0f, contact_on ? g_push.lens.w : 0.0f);
  if (view == 0u && tc >= reach) {
    discard;
  }

  // The occlusion's visibility.
  const float ao = ao_on ? VisibilityAt(g_push.slots.z, pixel, tc) : 1.0f;

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
  const float n_dot_l = saturate(dot(normal, g_push.light.xyz));
  const float sun = g_push.extra.x * n_dot_l;
  const float ambient_share = (ambient + sun) > 1e-6f ? ambient / (ambient + sun) : 1.0f;
  const float share = lerp(ambient_share, 1.0f, saturate(g_push.extra.z));
  const float fade = 1.0f - smoothstep(g_push.shape.w, fade_end, tc);
  const float ao_factor = ao_on ? lerp(1.0f, ao, saturate(g_push.shape.z * share * fade)) : 1.0f;

  // The contact shadows: on the sun's share of the light only.
  float cs = 1.0f;
  float sun_vis = 1.0f;
  float sun_share = 0.0f;
  float contact_factor = 1.0f;
  if (contact_on) {
    cs = VisibilityAt(g_push.slots2.x, pixel, tc);
    if (MaskOn()) {
      const float2 uv = (float2(pixel) + 0.5f) / size;
      sun_vis = saturate(g_push.shape.x *
                         g_textures[g_push.slots2.w].SampleLevel(g_samplers[kSamplerLinearClamp],
                                                                 uv, 0.0f).x);
    }
    const float sun_lit = sun * sun_vis;
    sun_share = (ambient + sun_lit) > 1e-6f ? sun_lit / (ambient + sun_lit) : 0.0f;
    const float contact_fade = 1.0f - smoothstep(g_push.lens.z, g_push.lens.w, tc);
    contact_factor = 1.0f - saturate(g_push.extra.w * sun_share * (1.0f - cs) * contact_fade);
  }
  const float factor = ao_factor * contact_factor;

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
    } else if (view == 8u) {
      grey = float3(contact_factor, contact_factor, contact_factor);
    } else if (view == 9u) {
      grey = float3(cs, cs, cs);
    } else if (view == 10u) {
      grey = float3(sun_vis, sun_vis, sun_vis);
    } else if (view == 11u) {
      grey = float3(sun_share, sun_share, sun_share);
    }
    return float4(grey * kViewScale, 1.0f);
  }
  if (factor >= 0.999f) {
    discard;
  }
  return float4(factor, factor, factor, 1.0f);
}
