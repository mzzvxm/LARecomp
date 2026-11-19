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
//   ao_factor      lerp(1, ao_seen, strength * ambient_share * distance_fade)
//     ao             the denoised visibility at the pixel; from the half resolution image (AoLow) by a
//                    bilinear upsample whose weights fall with the difference of linear depth, so
//                    it does not leak over an edge;
//     ao_seen        ao on the world; on a vehicle's pixel, ao raised to the paint power (below), by the share of the
//                    pixel's neighbourhood that the depth pass marked as a vehicle;
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
//     cs             the contact visibility (ao_contact_ps and its one edge-keeping filter pass), upsampled as ao is; with
//                    mcla_gpu_contact_shadows_fast (flag bit 25, AoContactFilter, only at the picture's size) the image
//                    is the march's own and this pass does the filter (AoBilateralEdgeFolded: the same 3x3 weights),
//                    where the contact shadows can change the factor at all, so the filter pass and its target are gone;
//                    above 1080 lines the occlusion's image is half size (ao_size.h) while the contact shadows' stays at the
//                    picture's: flag bit 21 (AoContactFull), the image is read as it is and its filter's depth is the full
//                    size one (the high 16 bits of slots2.y);
//     sun_share      Ls * N.L * vis / (La + Ls * N.L * vis), vis the game's sun shadow mask where
//                    the frame has one (flag bit 24: slots2.w its bindless slot, shape.x the factor
//                    that turns its stored value into a visibility, 2: the lit shaders multiply it
//                    by 2), 1 where it has none (6 h, 18 h, rain, night) and 1 on a vehicle's pixel
//                    (the mask is made before the cars are drawn, so at a car's pixel it is the world
//                    behind the car's, and a car's own shader does not use it: contact_trace.h
//                    "the game's sun shadow mask is not a car's"; the R34's trunk lid under the wing
//                    had the iron fence's bars behind it as a comb);
//     strength       extra.w: the setting's, times the sun's elevation gate and the wetness gate
//                    (ao.cpp), so 0 never reaches this shader;
//     fade           1 up to lens.z, to 0 at lens.w (smoothstep): a ray of a few tenths of a metre
//                    is under a pixel far away.
//
// Where the factor is near 1 (the sky, flat surfaces, past the fades) the pixel is not written.
//
// THE PAINT (ao_paint.h has the story, the numbers and the test). The game draws a car's paint as one colour, and the
// factor multiplies all of it: on dark glossy paint nearly all of that colour is the reflection of the environment, which
// only a blocker on the mirror ray takes away, not the share of the hemisphere that a part cannot see (the visibility is
// stored raised to the look's power, 3.6 by default, which makes a part that sees 70% of its sky 0.28: with the
// reflection multiplied like a diffuse term the uprights of a wing and the lid under it lost over half of their colour,
// owner, 2026-11-17). A vehicle's pixel takes ao raised to the paint power (flags bits 17-20, 5/16 by default: the look's
// own power taken out again, 3.6 * 5/16 = 1.125, the visibility itself, a little more, which is what the specular occlusion
// of a clear coat comes to; mcla_gpu_ao_paint) in place of ao. Nothing else of the factor changes: the share, the fade, the
// contact factor and the bounce light, and a pixel with at most half of its neighbourhood vehicle (the world, the road under
// and beside a car: the contact line that grounds it) takes ao as it is, to the bit.
//
// BOUNCE LIGHT (compiled with AO_GI=1 as ao_apply_gi_ps / ao_apply_gi_ms_ps, the pass the bounce light's setting
// uses; the plain compilation above is the source without it, to the byte): the occlusion's visibility and the
// bounce light come from one image (x the visibility, yzw the bounce: the cosine weighted mean radiance of what
// blocks the pixel's hemisphere, in the scene colour's own units, ao_main_ps.hlsl) and the factor becomes one per
// colour channel,
//
//   factor.rgb = ao_factor * contact_factor + bounce
//   bounce     = strength * B.rgb / max(T, luma(colour)) * (1 - source) * fade, then kept by two soft clips (ssgi_common.hlsli
//                SsgiSoftClip, the hue stays): its largest channel to `limit` times the pixel's own colour (a large number,
//                2 to 8: a guard against amplifying the noise of a nearly black pixel, not the look), and the light it adds
//                to the room under the ceiling, `cap` times the frame's light (ambient + sun, never under 0.5): the bounce
//                may fill a pixel up to what a white surface in full sun would be, and not past it, so a shadow gets lighter
//                and nothing gets whiter than the sun-lit white of the frame. A pixel at or over the ceiling gets nothing.
//                (The bounce was kept to half of the pixel's own colour before: a shaded pixel, where the bounce matters
//                most, gained nothing. ssgi_light.h has the whole story and the same maths in C++.)
//                A VEHICLE's pixel (the depth pass's mark, the share of the pixel's neighbourhood that has it) takes
//                a smaller strength (ssgi_common.hlsli SsgiStrengthFor): its colour is mostly the paint's gloss, the
//                reflection of the sky and the floor, not a diffuse albedo, so colour / T over-reads its albedo, and the
//                low parts of a car (skirts, lower doors, tyres, the underside) are what the bright floor lights most:
//                at the full strength a black car glowed (owner, the garage, 2026-11-13).
//
// The scene colour is albedo * (T + point lights) + specular and the game's ambient stands for "the average
// surroundings": the multiply by lerp(1, V, share) took the blocked part of the ambient away, the bounce gives
// back what the blockers really send, albedo * B. The albedo is not known (no G-buffer), but the pixel's own
// colour is there as the multiply's destination, so albedo * B = colour * B / T with T the light the constants
// and the mask say fell on it (La + Ls N.L, the sun's term times the game's sun shadow mask where the frame has
// one: a pixel in a shadow is lit by La alone): the same multiply carries it, no second pass and no read of the
// colour target. Where the pixel is brighter than T says (a street lamp's pool, headlights, a specular glint,
// neon: light the constants do not count) the estimate is held to an albedo of 1, T is the colour chain's luma
// instead (albedo 1: the pixel receives B at most, never more), and where the chain says the colour was capped
// (alpha: the share of its samples above the cap of ao_color_ps: a source of light, not a surface) the pixel
// receives nothing. Replace, not add: a flat surface has B near 0 and its factor stays the occlusion's, a crease
// beside a lit wall gets the wall's colour back where the occlusion took the ambient away.
// Push fields of this variant (the apply's others as above): slots.z the image of the visibility and the bounce
// (RGBA16F, at the passes' size: the a-trous filter's output), slots2.y the colour chain's level 0 (RGBA16F: rgb
// the cleaned colour, a the share of capped samples), shape.y the strength, flags bits 8-15 the limit in units
// of 0.04, bits 26-31 the cap in units of 0.25 (the ceiling and, in the colour chain, the radiance cap); bit 25 is
// the contact filter flag (above: the apply does the contact filter itself), the same in both compilations.
// The sun shadow mask (flag bit 24, slots2.w, shape.x) is read for T whether or not the contact shadows are on; a vehicle's pixel takes 1 for it
// there too (CarAwareSunVisibility).
// Debug views 16-21 (flags bits 0-7): 16 the factor in colour, 17 the bounce
// the pixel gets (four times as bright as the others), 18 the albedo estimate (colour / T, 0 to 1), 19 T, 20 the
// share of the colour that is a surface (white; black: a source), 21 the picture's change against the game's
// own (grey 0.5: none; unscaled).
//
// Debug views (flags bits 0-7): 1 the factor, 2 the visibility, 3 the ambient share, 4 the
// view-space normal, 5 the linear depth (log), 6 the temporal pass's mask (the image it reads then
// holds the share of the history each pixel kept instead of the visibility: white a full history,
// black none, grey part of it), 7 the occlusion's own factor (the visibility with the ambient share, the
// strength and the distance fade, without the contact shadows; the picture's factor when they are off),
// 8 the contact factor, 9 the contact visibility, 10 the sun's
// visibility (the game's mask as it is, or 1: a vehicle's pixel shows the world behind it there and takes 1 in the share), 11 the sun's share of the light, 12 the pixels the depth pass took for
// vehicles (white); written as a grey of 0.3 of the value so
// that 1.0 reads below the tone map's white.

#include "ao_common.hlsli"
#include "host_pass_samplers.hlsli"
#if AO_GI
#include "ssgi_common.hlsli"
#endif

static const float kViewScale = 0.3f;
// The folded contact filter (AoContactFilter in this pass) is not run where the contact shadows change the factor
// by less than this (the filter keeps cs between 0 and 1, so the factor moves by at most the weight).
static const float kFoldedFilterSkip = 0.002f;

bool AoTermOn() { return (g_push.slots2.z & (1u << 22)) != 0u; }
bool ContactOn() { return (g_push.slots2.z & (1u << 23)) != 0u; }
bool MaskOn() { return (g_push.slots2.z & (1u << 24)) != 0u; }
// The occlusion's (and the bounce light's) images are half size (AoLow) while the contact shadows' image is at the picture's size (flags
// bit 21, ao.cpp kApplyContactFullFlag; Plan::SplitContact): it is read as it is, no upsample, and its filter's depth is the full size
// linear depth whose slot is in the high 16 bits of slots2.y (the low 16 are the bounce light's colour chain slot). Without the flag the
// contact image is at the passes' size and its depth is slots.w, as before.
bool AoContactFull() { return (g_push.slots2.z & (1u << 21)) != 0u; }
uint ContactDepthSlot() { return AoContactFull() ? (g_push.slots2.y >> 16) : g_push.slots.w; }
int2 ContactImageSize() { return AoContactFull() ? int2(g_push.sizes.xy) : int2(g_push.sizes.zw); }
uint ColourSlot() { return g_push.slots2.y & 0xFFFFu; }
// The paint power's code (ao_paint.h): flags bits 17-20, the place of the denoise's radius, which this pass does not read (ao.cpp
// clears them): the power is (code + 1) sixteenths, and code 15, the power 1, is off (the paint is darkened like the world).
uint AoPaintCode() { return (g_push.slots2.z >> 17) & 0xFu; }
float AoPaintPower() { return float(AoPaintCode() + 1u) * (1.0f / 16.0f); }
bool AoPaintOn() { return AoPaintCode() != 15u; }
#if AO_GI
// The most the bounce factor may be, in multiples of the pixel's own colour: flags bits 8-15 in units of 0.04 (the
// apply has no use for the slices and steps the main pass keeps there). The cap multiple of the ceiling: bits 26-31 in
// units of 0.25.
float GiLimit() { return float((g_push.slots2.z >> 8) & 0xFFu) * 0.04f; }
float GiCap() { return float((g_push.slots2.z >> 26) & 0x3Fu) * 0.25f; }
static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
#endif

// The stored depth at `p` clamped into the target, as a linear distance.
float LinearAt(int2 p, int2 last) {
  return AoLinearDepth(AoLoadStored(clamp(p, int2(0, 0), last)));
}

// A visibility image (the AO's or the contact shadows') at the pixel of the picture: as it is, or,
// from the half resolution one, a bilinear upsample whose weights fall with the difference of linear
// depth. `vehicle` is the share of the same neighbourhood that the depth pass marked as a vehicle (the
// depth image's lowest bit, 0 to 1; the weights are the visibility's): the depth is read for the weights
// anyway, so it costs next to nothing at half size and one load of the pixel's own depth at full size.
float VisibilityAt(uint slot, int2 pixel, float tc, out float vehicle) {
  if (!AoLow()) {
    vehicle = AoIsVehicle(AoLoadLinear(g_push.slots.w, pixel)) ? 1.0f : 0.0f;
    return g_textures[slot].Load(int3(pixel, 0)).x;
  }
  const int2 asize = int2(g_push.sizes.zw);
  const float2 q = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
  const int2 q0 = int2(floor(q));
  const float2 f = q - float2(q0);
  float sum = 0.0f;
  float vehicle_sum = 0.0f;
  float weights = 0.0f;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int2 c = clamp(q0 + int2(i, j), int2(0, 0), asize - 1);
      const float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
      const float td = AoLoadLinear(g_push.slots.w, c);
      const float dz = abs(td - tc) / max(0.02f * tc, 0.05f);
      const float w = bilinear * max(exp(-dz * dz), 1e-4f);
      sum += g_textures[slot].Load(int3(c, 0)).x * w;
      vehicle_sum += (AoIsVehicle(td) ? 1.0f : 0.0f) * w;
      weights += w;
    }
  }
  vehicle = vehicle_sum / max(weights, 1e-8f);
  return sum / max(weights, 1e-8f);
}

// The same without the vehicle share (the contact shadows' image).
float VisibilityAt(uint slot, int2 pixel, float tc) {
  float unused_vehicle;
  return VisibilityAt(slot, pixel, tc, unused_vehicle);
}

// The visibility a pixel takes into the factor (ao_paint.h PaintVisibility): `ao` on the world, and on a vehicle's pixel ao raised
// to the paint power. `vehicle` is the share of the pixel's neighbourhood that is a vehicle; the paint's rule starts where over half
// of it is (ao_paint.h kVehicleFrom: at half size the share is a bilinear mix of four texels, and the road beside a tyre has a
// vehicle texel or two in it, which must not lighten the contact line) and is all there at 1.
float PaintVisibility(float ao, float vehicle) {
  const float on_car = saturate((vehicle - 0.5f) * 2.0f);
  if (on_car <= 0.0f || !AoPaintOn()) {
    return ao;
  }
  return lerp(ao, pow(ao, AoPaintPower()), on_car);
}

// Whether the depth pass marked the pixel as a vehicle's (the lowest bit of the level-0 depth): in the contact shadows' own full size depth where they
// have one (flag bit 21), else in the occlusion's level 0 (half size where AoLow, as the debug view 12 reads it).
bool PixelIsVehicle(int2 pixel) {
  if (AoContactFull()) {
    return AoIsVehicle(AoLoadLinear(ContactDepthSlot(), pixel));
  }
  const int2 asize = int2(g_push.sizes.zw);
  return AoIsVehicle(AoLoadLinear(g_push.slots.w, clamp(AoLow() ? (pixel >> 1) : pixel, int2(0, 0), asize - 1)));
}

// The sun's visibility the share of the light is worked out with, from the game's sun shadow mask's value `mask_vis` at the pixel (contact_trace.h
// CarSunVisibility, "the game's sun shadow mask is not a car's"): the game collects the mask before the cars are drawn, so at a car's pixel it is the
// world behind the car's (the fence's bars on the R34's trunk lid: the comb under the wing), and a car's own shader does not use it. A vehicle's
// pixel takes 1.
float CarAwareSunVisibility(float mask_vis, int2 pixel) {
  return PixelIsVehicle(pixel) ? 1.0f : mask_vis;
}

#if AO_GI
// The image of the visibility and the bounce (`gi_slot`) and the colour chain's level 0 (`colour_slot`) at the
// pixel of the picture: as they are, or, from the half resolution ones, one bilinear upsample whose weights fall
// with the difference of linear depth (VisibilityAt's), the two images by the same weights. `vehicle` is the share
// of the same neighbourhood that the depth pass marked as a vehicle (the depth image's lowest bit), 0 to 1.
void GiAt(uint gi_slot, uint colour_slot, int2 pixel, float tc, out float4 gi, out float4 colour,
          out float vehicle) {
  if (!AoLow()) {
    gi = g_textures[gi_slot].Load(int3(pixel, 0));
    colour = g_textures[colour_slot].Load(int3(pixel, 0));
    vehicle = AoIsVehicle(AoLoadLinear(g_push.slots.w, pixel)) ? 1.0f : 0.0f;
    return;
  }
  const int2 asize = int2(g_push.sizes.zw);
  const float2 q = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
  const int2 q0 = int2(floor(q));
  const float2 f = q - float2(q0);
  float4 gi_sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  float4 colour_sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  float vehicle_sum = 0.0f;
  float weights = 0.0f;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int2 c = clamp(q0 + int2(i, j), int2(0, 0), asize - 1);
      const float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
      const float td = AoLoadLinear(g_push.slots.w, c);
      const float dz = abs(td - tc) / max(0.02f * tc, 0.05f);
      const float w = bilinear * max(exp(-dz * dz), 1e-4f);
      gi_sum += g_textures[gi_slot].Load(int3(c, 0)) * w;
      colour_sum += g_textures[colour_slot].Load(int3(c, 0)) * w;
      vehicle_sum += (AoIsVehicle(td) ? 1.0f : 0.0f) * w;
      weights += w;
    }
  }
  gi = gi_sum / max(weights, 1e-8f);
  colour = colour_sum / max(weights, 1e-8f);
  vehicle = vehicle_sum / max(weights, 1e-8f);
}
#endif

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
#if AO_GI
  // From the image of the bounce light too (the pass is only used with the occlusion on).
  float4 gi_image;
  float4 gi_colour;
  float gi_vehicle;
  GiAt(g_push.slots.z, ColourSlot(), pixel, tc, gi_image, gi_colour, gi_vehicle);
  const float ao = gi_image.x;
  const float ao_vehicle = gi_vehicle;
#else
  float ao_vehicle = 0.0f;
  const float ao = ao_on ? VisibilityAt(g_push.slots.z, pixel, tc, ao_vehicle) : 1.0f;
#endif

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
  // The visibility the factor takes: ao itself on the world, the paint's on a vehicle (PaintVisibility).
  const float ao_seen = ao_on ? PaintVisibility(ao, ao_vehicle) : 1.0f;
  const float ao_factor = ao_on ? lerp(1.0f, ao_seen, saturate(g_push.shape.z * share * fade)) : 1.0f;

  // The contact shadows: on the sun's share of the light only.
  float cs = 1.0f;
  float sun_vis = 1.0f;      // the game's mask as it is (the debug view 10)
  float sun_vis_used = 1.0f; // what the share takes: a vehicle's pixel has 1 (CarAwareSunVisibility)
  float sun_share = 0.0f;
  float contact_factor = 1.0f;
  if (contact_on) {
    if (MaskOn()) {
      const float2 uv = (float2(pixel) + 0.5f) / size;
      sun_vis = saturate(g_push.shape.x *
                         g_textures[g_push.slots2.w].SampleLevel(g_samplers[kSamplerLinearClamp],
                                                                 uv, 0.0f).x);
      sun_vis_used = CarAwareSunVisibility(sun_vis, pixel);
    }
    const float sun_lit = sun * sun_vis_used;
    sun_share = (ambient + sun_lit) > 1e-6f ? sun_lit / (ambient + sun_lit) : 0.0f;
    const float contact_fade = 1.0f - smoothstep(g_push.lens.z, g_push.lens.w, tc);
    const float contact_weight = g_push.extra.w * sun_share * contact_fade;
    if (!AoContactFilter()) {
      cs = AoContactFull() ? g_textures[g_push.slots2.x].Load(int3(pixel, 0)).x : VisibilityAt(g_push.slots2.x, pixel, tc);
    } else if (view != 0u || contact_weight > kFoldedFilterSkip) {
      // mcla_gpu_contact_shadows_fast: slots2.x is the march's own image, and the filter the denoise pass would
      // have made of it is done here, with the depths read above for the normal (the passes' size is the
      // picture's: the flag is only set then). A pixel the contact shadows cannot darken (the game's own
      // shadow there, the surface turned from the sun, past the fade) skips it: its factor is 1 - weight *
      // (1 - cs), under kFoldedFilterSkip of a change.
      // KEEP IN STEP with ao_contact_ps.hlsl (THE SKIP UNDER THE GAME'S OWN SHADOW): the march returns 1 for a pixel
      // whose 3x3 neighbourhood has every mask texel at 0 (the weight bound strength * Ls * vis / (La + Ls * vis) under
      // half of kFoldedFilterSkip), which is exact only because a pixel with weight <= kFoldedFilterSkip never reads the
      // image. Change the weight (sun_share, the fade, the mask's sample position or gain) or this test and that bound
      // changes with it (contact_trace.h MaskCannotLight).
      cs = AoBilateralEdgeFolded(g_push.slots2.x, ContactDepthSlot(), pixel, ContactImageSize(), tc, pc, normal,
                                 float4(tl, tr, tt, tb));
    }
    contact_factor = 1.0f - saturate(contact_weight * (1.0f - cs));
  }
  const float factor = ao_factor * contact_factor;

#if AO_GI
  // The bounce light. T is the light the game's constants say fell on the pixel: the ambient and the sun's
  // term, which the game's sun shadow mask takes away where the pixel is in a shadow (1 where the frame has
  // no mask). The pixel is brighter than T when something the constants do not count lights it (a street
  // lamp, headlights) or is one (neon, a glint): then the albedo is taken to be 1 (T is the chain's luma,
  // so the pixel gets B at most), and a pixel the chain says is a source of light (its alpha, ramped from where
  // a source starts) is not a surface and gets less, none when it is entirely one.
  float gi_sun_vis = 1.0f;
  if (MaskOn()) {
    const float2 gi_uv = (float2(pixel) + 0.5f) / size;
    gi_sun_vis = CarAwareSunVisibility(saturate(g_push.shape.x *
                                                g_textures[g_push.slots2.w].SampleLevel(g_samplers[kSamplerLinearClamp],
                                                                                        gi_uv, 0.0f).x),
                                       pixel);
  }
  const float light_t = max(ambient + sun * gi_sun_vis, 1e-3f);
  const float luma_c = dot(gi_colour.rgb, kLuma);
  const float surface = 1.0f - saturate(gi_colour.a);
  const float gi_strength = SsgiStrengthFor(g_push.shape.y, gi_vehicle);
  float3 bounce = (gi_strength * fade * surface / max(light_t, luma_c)) * gi_image.yzw;
  // The gain limit: the largest channel through the soft clip, the hue kept.
  const float bounce_peak = max(max(bounce.x, bounce.y), bounce.z);
  if (bounce_peak > 0.0f) {
    bounce *= SsgiSoftClip(bounce_peak, GiLimit()) / bounce_peak;
  }
  // The ceiling: what the bounce adds (the pixel's colour times it) may fill the pixel up to `cap` times the frame's
  // light (never under the floor) and no more; a pixel already there gets nothing.
  const float ceiling = max(GiCap() * (ambient + g_push.extra.x), kSsgiCapFloor);
  const float room = max(ceiling - luma_c * factor, 0.0f);
  const float added = dot(gi_colour.rgb * bounce, kLuma);
  if (added > 1e-6f) {
    bounce *= SsgiSoftClip(added, room) / added;
  }
  const float3 gi_factor = factor + bounce;
  if (view >= 16u) {
    float3 shown = gi_factor * kViewScale;
    if (view == 17u) {
      shown = bounce * (4.0f * kViewScale);
    } else if (view == 18u) {
      shown = saturate(gi_colour.rgb / light_t) * kViewScale;
    } else if (view == 19u) {
      shown = float3(light_t, light_t, light_t) * kViewScale;
    } else if (view == 20u) {
      shown = float3(surface, surface, surface) * kViewScale;
    } else if (view == 21u) {
      const float change = saturate(0.5f + 2.0f * (dot(gi_factor, kLuma) - 1.0f));
      shown = float3(change, change, change);
    }
    return float4(shown, 1.0f);
  }
#endif

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
    } else if (view == 7u) {
      grey = float3(ao_factor, ao_factor, ao_factor);  // the occlusion's own part of the factor
    } else if (view == 12u) {
      // The pixels the depth pass marked as vehicles (white).
      const int2 asize = int2(g_push.sizes.zw);
      const int2 q = clamp(AoLow() ? (pixel >> 1) : pixel, int2(0, 0), asize - 1);
      const float v = AoIsVehicle(AoLoadLinear(g_push.slots.w, q)) ? 1.0f : 0.0f;
      grey = float3(v, v, v);
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
#if AO_GI
  if (all(abs(gi_factor - 1.0f) < 0.001f)) {
    discard;
  }
  return float4(gi_factor, 1.0f);
#else
  if (factor >= 0.999f) {
    discard;
  }
  return float4(factor, factor, factor, 1.0f);
#endif
}
