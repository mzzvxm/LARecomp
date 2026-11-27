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
//                  sun's share of the light, so they cannot darken what the game already shadows. The share is
//                  taken PER COLOUR CHANNEL (sun_share_c = S_c * N.L * vis / (A_c + S_c * N.L * vis), the two lights'
//                  colours in shape.w: contact_trace.h "the contact factor per colour channel"): a shadow takes the
//                  sun's colour away and leaves the sky's, as the game's own shadow does; a grey light gives the luma's
//                  share below, bit for bit. (The luma's share darkened the right amount in the wrong colour: the car's
//                  shadow at noon, which the car's map owns, was warmer than the game's own shadows beside it.)
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
//                    by 2), 1 where it has none (6 h, 18 h, rain, night); on a vehicle's pixel not the
//                    mask (it is made before the cars are drawn, so at a car's pixel it is the world
//                    behind the car's, and a car's own shader does not use it: contact_trace.h
//                    "the game's sun shadow mask is not a car's"; the R34's trunk lid under the wing
//                    had the iron fence's bars behind it as a comb) but what the car's own shader
//                    uses, the game's car sun image at the pixel's place on the ground (see
//                    CarAwareSunVisibility; a car in a building's shade has vis 0 and no contact
//                    shadow of the sun's), 1 where the frame has no such image;
//     strength       extra.w: the setting's, times the sun's elevation gate and the wetness gate
//                    (ao.cpp), so 0 never reaches this shader;
//     fade           1 up to lens.z, to 0 at lens.w (smoothstep): a ray of a few tenths of a metre
//                    is under a pixel far away.
//
// Where the factor is near 1 (the sky, flat surfaces, past the fades) the pixel is not written.
//
// shape.w of this pass is the lights' colours (four 7 bit values, contact_trace.h kChromaScale: the ambient's and the sun's red and blue over their luma in 1/32;
// kNeutralChroma is grey), no longer the distance fade's start: that is half of extra.y, as ao.cpp FillPush has always made it.
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
// PBR LEVEL 1C (perf\PBR.md; mcla_gpu_pbr): the bounce is multiplied by the DIFFUSE SHARE of the pixel in the shared model (pbr_screen_common.hlsli PbrScreenDiffuseShare: 0 for a metal, 1 - the
// reflection's weight for a dielectric: what the specular layer lets through), read from RT3 where the frame has the plane at the picture's size: slots.y or the car-sun lookup holds
// its bindless slot + 1 (0: none, the share is 1 as it was). The estimate of the albedo (colour / T) is unchanged: the share is the part of it that takes incoming diffuse light.
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
// visibility (the game's mask as it is, or 1: a vehicle's pixel shows the world behind it there), 11 the sun's share of the light, 12 the pixels the depth pass took for
// vehicles (white), 13 the sun's visibility the share took (the mask on the world, the game's car sun image on a vehicle, 1 on a vehicle where
// the frame has no such image); written as a grey of 0.3 of the value so
// that 1.0 reads below the tone map's white.

#include "ao_common.hlsli"
#include "ao_rate_rules.hlsli"
#include "ssgi_quarter.hlsli"
#include "cabin_shadow_rules.hlsli"
#include "host_pass_samplers.hlsli"
#include "nan_guard.hlsli"
#if AO_GI
#include "ssgi_common.hlsli"
#include "pbr_screen_common.hlsli"
#endif

static const float kViewScale = 0.3f;
static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
// The two lights' colours (contact_trace.h ChromaRgb, SunShareRgb: keep in step): shape.w holds, 7 bits each, the chroma = channel / luma in 1/32 of the
// ambient's red and blue and of the sun's red and blue (bits 0-27, the top bits are a marker); the green follows from the luma weights.
static const float kChromaScale = 32.0f;
float3 ChromaRgb(uint red_byte, uint blue_byte) {
  const float r = float(red_byte) * (1.0f / kChromaScale);
  const float b = float(blue_byte) * (1.0f / kChromaScale);
  return float3(r, max((1.0f - 0.2126f * r - 0.0722f * b) / 0.7152f, 0.0f), b);
}
// The sun's share of the light in each channel: S_c * lit / (A_c + S_c * lit), lit = N.L times the sun's visibility; the lumas are light.w and extra.x.
float3 SunShareRgb(float ambient_luma, float sun_luma, float lit) {
  const uint code = asuint(g_push.shape.w);
  const float3 amb = ambient_luma * ChromaRgb(code & 0x7Fu, (code >> 7) & 0x7Fu);
  const float3 sunlit = (sun_luma * lit) * ChromaRgb((code >> 14) & 0x7Fu, (code >> 21) & 0x7Fu);
  return sunlit / max(amb + sunlit, 1e-6f);
}
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
// The depth image's slot (the occlusion's linear depth, level 0) is the low 16 bits of slots.w; the high 16 are the normal image's slot, at the picture's size (0: none: the pass takes
// the normal from the depth, below). ao.cpp RecordApply, ao_gbuffer.h: the apply reads the normal image only where one has the picture's size.
uint ApplyDepthSlot() { return g_push.slots.w & 0xFFFFu; }
uint ApplyNormalSlot() { return g_push.slots.w >> 16; }
uint ContactDepthSlot() { return AoContactFull() ? (g_push.slots2.y >> 16) : ApplyDepthSlot(); }
int2 ContactImageSize() { return AoContactFull() ? int2(g_push.sizes.xy) : int2(g_push.sizes.zw); }
uint ColourSlot() { return g_push.slots2.y & 0xFFFFu; }
uint AoVisibilitySlot() { return g_push.slots.z & 0xFFFFu; }
uint ContactSlot() { return g_push.slots2.x & 0xFFFFu; }
bool GiQuarter() { return (g_push.slots.y & kSsgiQuarterApply) != 0u; }
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
// slots.y bit 15 (ao_gbuffer::kApplyMaterialFlag) admits RT3. With car sun on,
// the same lookup already resident for that frame carries the RT3 slot + 1.
uint GiMaterialCode() {
  if ((g_push.slots.y & (1u << 15)) == 0u) return 0u;
  const uint slot = g_push.slots.y >> 16u;
  return (g_push.slots.y & 0x100u) != 0u ? uint(g_textures[slot].Load(int3(2, 0, 0)).x) : slot;
}
// RT3 at the pixel (the guest's sample 0 where the planes are multisampled: the host sample the depth is read at).
float3 GiLoadMaterial(int2 p, uint slot) {
#if AO_DEPTH_MS
  return g_textures_ms[slot].Load(p, g_push.slots.y & 0xFFu).xyz;
#else
  return g_textures[slot].Load(int3(p, 0)).xyz;
#endif
}
float GiCap() { return float((g_push.slots2.z >> 26) & 0x3Fu) * 0.25f; }
#endif

// The apply's slots.x and slots.y carry two more things beside the depth plane's slot and the host sample (contact_trace.h "a car's own sun
// visibility"): the low 16 bits of slots.x are the depth plane's bindless slot and the high 16 the car sun image's; the low 8 bits of slots.y are
// the host sample, bit 8 says that the frame has a car sun image, and the high 16 bits are the slot of the 4x1 image that holds its lookup.
// (ao_common.hlsli's AoLoadStored reads the whole words; the apply reads its own.)
float ApplyLoadStored(int2 p) {
#if AO_DEPTH_MS
  return g_textures_ms[g_push.slots.x & 0xFFFFu].Load(p, g_push.slots.y & 0xFFu).r;
#else
  return g_textures[g_push.slots.x & 0xFFFFu].Load(int3(p, 0)).r;
#endif
}

// The stored depth at `p` clamped into the target, as a linear distance.
float LinearAt(int2 p, int2 last) {
  return AoLinearDepth(ApplyLoadStored(clamp(p, int2(0, 0), last)));
}

// A visibility image (the AO's or the contact shadows') at the pixel of the picture: as it is, or,
// from the half resolution one, a bilinear upsample whose weights fall with the difference of linear
// depth. `vehicle` is the share of the same neighbourhood that the depth pass marked as a vehicle (the
// depth image's lowest bit, 0 to 1; the weights are the visibility's): the depth is read for the weights
// anyway, so it costs next to nothing at half size and one load of the pixel's own depth at full size.
float VisibilityAt(uint slot, int2 pixel, float tc, out float vehicle, bool thin = false, bool rotated = false) {
  if (!AoLow()) {
    vehicle = AoIsVehicle(AoLoadLinear(ApplyDepthSlot(), pixel)) ? 1.0f : 0.0f;
    return g_textures[slot].Load(int3(pixel, 0)).x;
  }
  const int2 asize = int2(g_push.sizes.zw);
  const float2 q = (float2(pixel) + 0.5f) * 0.5f - 0.5f -
                   (rotated ? AoGridOffset(AoGridPhase()) : float2(0.0f, 0.0f));
  const int2 q0 = int2(floor(q));
  const float2 f = q - float2(q0);
  float4 taps = 0.0f;
  if (thin) {
    taps = g_textures[slot].GatherRed(g_samplers[kSamplerLinearClamp], (float2(q0) + 1.0f) / float2(asize));
    // A positive bilateral average, then paint and strength, cannot darken an open footprint.
    if (all(taps >= 0.999f)) {
      vehicle = 0.0f;
      return 1.0f;
    }
  }
  float sum = 0.0f;
  float vehicle_sum = 0.0f;
  float weights = 0.0f;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int2 c = clamp(q0 + int2(i, j), int2(0, 0), asize - 1);
      const float bilinear = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
      const float td = AoLoadLinear(ApplyDepthSlot(), c);
      const float dz = abs(td - tc) / max(0.02f * tc, 0.05f);
      const float w = bilinear * max(exp(-dz * dz), 1e-4f);
      const float v = thin ? taps[AoGatherIndex(i, j)] : g_textures[slot].Load(int3(c, 0)).x;
      sum += v * w;
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
  return AoIsVehicle(AoLoadLinear(ApplyDepthSlot(), clamp(AoLow() ? (pixel >> 1) : pixel, int2(0, 0), asize - 1)));
}

// The game's car sun image is in this frame's push (slots.y bit 8; ao.cpp FindCarSunSlot, RecordCarSunLookup).
bool CarSunOn() { return (g_push.slots.y & 0x100u) != 0u; }

// The sun's visibility a car's shader takes at the view-space point `pc`, from the game's car sun image (contact_trace.h "a car's own sun
// visibility", the twin CarSunUv and CarSunFromImage): a 256 m wide top-down image around the camera, one texel a metre, read at the point's world x
// and z relative to the truncated camera, saturate(s - 0.25) * 4/3 of its stored value, with the guest's bilinear sampler. The 4x1 lookup image holds
// the 8 numbers that take a view-space point to the world's x and z (texel 0: ax, ox; texel 1: az, oz).
float CarSunVisibility(float3 pc) {
  const uint lookup = g_push.slots.y >> 16;
  const float4 a = g_textures[lookup].Load(int3(0, 0, 0));
  const float4 b = g_textures[lookup].Load(int3(1, 0, 0));
  const float2 uv = float2(dot(a.xyz, pc) + a.w, dot(b.xyz, pc) + b.w) * (1.0f / 256.0f) + 0.5f;
  const float s = g_textures[g_push.slots.x >> 16].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f).x;
  return min(saturate(s - 0.25f) * 1.3333325f, 1.0f);
}

// The sun's visibility the share of the light is worked out with (contact_trace.h CarAwareSunVisibility): the game's sun shadow mask's value `mask_vis`
// at the pixel on the world's pixels; on a vehicle's pixel (`vehicle`) not that: the game collects the mask before the cars are drawn, so at a car's pixel
// it is the world behind the car's (the fence's bars on the R34's trunk lid: the comb under the wing), and a car's own shader does not use it. It uses the
// car sun image (CarSunVisibility), so a vehicle's pixel takes that, and 1 where the frame has none.
float CarAwareSunVisibility(float mask_vis, bool vehicle, float3 pc) {
  if (!vehicle) {
    return mask_vis;
  }
  return CarSunOn() ? CarSunVisibility(pc) : 1.0f;
}

#if AO_GI
uint QuarterReceiverVehicle(int2 pixel) {
  const uint stencil_slot = g_push.slots2.w >> 16;
  if (stencil_slot == 0xFFFFu) {
    return 0u;
  }
#if AO_DEPTH_MS
  const uint stencil = StencilOf(g_utextures_ms[stencil_slot].Load(pixel, g_push.slots.y & 0xFFu));
#else
  const uint stencil = StencilOf(g_utextures[stencil_slot].Load(int3(pixel, 0)));
#endif
  const uint id = stencil >> 2;
  return id <= 23u ? id : 0u;
}

void QuarterGiAt(int2 pixel, float tc, float3 pc, float3 normal, uint vehicle,
                 out float3 bounce, out float4 colour) {
  const int2 size = (int2(g_push.sizes.xy) + 3) / 4;
  const float2 q = (float2(pixel) + 0.5f) * 0.25f - 0.5f;
  const int2 q0 = int2(floor(q));
  const float2 f = q - float2(q0);
  const uint depth_slot = g_push.slots2.x >> 16;
  const uint gi_slot = g_push.slots.z >> 16;
  const uint colour_slot = ColourSlot();
  float weights = 0.0f;
  float3 sum = 0.0f;
  float4 colour_sum = 0.0f;
  [unroll] for (int y = 0; y < 2; ++y) {
    [unroll] for (int x = 0; x < 2; ++x) {
      const int2 tap = clamp(q0 + int2(x, y), int2(0, 0), size - 1);
      const float td = AoLoadLinear(depth_slot, tap);
      if (td >= kAoSky * 0.5f || SsgiQuarterVehicle(td) != vehicle) {
        continue;
      }
      const float3 p = AoViewPos(float2(tap) + 0.5f, float2(size), td);
      const float bilinear = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
      const float w = bilinear * SsgiQuarterWeight(td, tc, dot(normal, p - pc));
      if (w <= 0.0f) {
        continue;
      }
      sum += g_textures[gi_slot].Load(int3(tap, 0)).yzw * w;
      colour_sum += g_textures[colour_slot].Load(int3(tap, 0)) * w;
      weights += w;
    }
  }
  // Fade out when the trace has little or no coverage of this surface.
  bounce = sum / max(weights, 0.05f);
  colour = colour_sum / max(weights, 1e-5f);
}

// The image of the visibility and the bounce (`gi_slot`) and the colour chain's level 0 (`colour_slot`) at the
// pixel of the picture: as they are, or, from the half resolution ones, one bilinear upsample whose weights fall
// with the difference of linear depth (VisibilityAt's), the two images by the same weights. `vehicle` is the share
// of the same neighbourhood that the depth pass marked as a vehicle (the depth image's lowest bit), 0 to 1.
void GiAt(uint gi_word, uint colour_slot, int2 pixel, float tc, out float4 gi, out float4 colour,
          out float vehicle) {
  const uint gi_slot = gi_word;
  if (!AoLow()) {
    gi = g_textures[gi_slot].Load(int3(pixel, 0));
    colour = g_textures[colour_slot].Load(int3(pixel, 0));
    vehicle = AoIsVehicle(AoLoadLinear(ApplyDepthSlot(), pixel)) ? 1.0f : 0.0f;
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
      const float td = AoLoadLinear(ApplyDepthSlot(), c);
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
  const float reach = max((ao_on || GiQuarter()) ? fade_end : 0.0f, contact_on ? g_push.lens.w : 0.0f);
  if (view == 0u && tc >= reach) {
    discard;
  }

  // The occlusion's visibility.
#if AO_GI
  // From the image of the bounce light too (the pass is only used with the occlusion on).
  const bool quarter = GiQuarter();
  float4 gi_image = 0.0f;
  float4 gi_colour = 0.0f;
  float gi_vehicle = 0.0f;
  float ao_vehicle = 0.0f;
  float ao = 1.0f;
  if (quarter) {
    if (ao_on) {
      ao = VisibilityAt(AoVisibilitySlot(), pixel, tc, ao_vehicle);
    }
  } else {
    GiAt(g_push.slots.z, ColourSlot(), pixel, tc, gi_image, gi_colour, gi_vehicle);
    ao = gi_image.x;
    ao_vehicle = gi_vehicle;
  }
#else
  float ao_vehicle = 0.0f;
  const bool optimized = (g_push.slots2.z & (1u << 31)) != 0u;
  const bool thin = optimized && view == 0u;
  const float ao = ao_on ? VisibilityAt(g_push.slots.z, pixel, tc, ao_vehicle, thin, optimized && AoRotatingGrid()) : 1.0f;
  if (thin && !contact_on && ao >= 0.999f) {
    discard;
  }
#endif

  // The normal: the normal image's where the frame has one at this size (GB-E; ao_normal_ps.hlsl: the G-buffer's where a pixel has it, else the depth's), else from the full
  // resolution depth, as the main pass has it. The four neighbours' depths below are the folded contact filter's too (they are read for it where the normal comes from the image).
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const uint normal_slot = ApplyNormalSlot();
  float3 normal;
  float tl = 0.0f;
  float tr = 0.0f;
  float tt = 0.0f;
  float tb = 0.0f;
  if (normal_slot != 0u) {
    normal = GbOctDecode(g_textures[normal_slot].Load(int3(pixel, 0)).xy);
  } else {
    tl = LinearAt(pixel + int2(-1, 0), last);
    tr = LinearAt(pixel + int2(1, 0), last);
    tt = LinearAt(pixel + int2(0, -1), last);
    tb = LinearAt(pixel + int2(0, 1), last);
    const float3 h = abs(tl - tc) < abs(tr - tc)
                         ? pc - AoViewPos(float2(pixel + int2(-1, 0)) + 0.5f, size, tl)
                         : AoViewPos(float2(pixel + int2(1, 0)) + 0.5f, size, tr) - pc;
    const float3 v = abs(tt - tc) < abs(tb - tc)
                         ? AoViewPos(float2(pixel + int2(0, -1)) + 0.5f, size, tt) - pc
                         : pc - AoViewPos(float2(pixel + int2(0, 1)) + 0.5f, size, tb);
    normal = cross(v, h);
    const float normal_len = length(normal);
    normal = normal_len > 1e-20f ? normal / normal_len : float3(0.0f, 0.0f, -1.0f);
  }

  // The share of the light that is ambient.
  const float ambient = g_push.light.w;
  const float n_dot_l = saturate(dot(normal, g_push.light.xyz));
  const float sun = g_push.extra.x * n_dot_l;
  const float ambient_share = (ambient + sun) > 1e-6f ? ambient / (ambient + sun) : 1.0f;
  const float share = lerp(ambient_share, 1.0f, saturate(g_push.extra.z));
  const float fade = 1.0f - smoothstep(0.5f * fade_end, fade_end, tc);
  // The visibility the factor takes: ao itself on the world, the paint's on a vehicle (PaintVisibility).
  const float ao_seen = ao_on ? PaintVisibility(ao, ao_vehicle) : 1.0f;
  const float ao_factor = ao_on ? lerp(1.0f, ao_seen, saturate(g_push.shape.z * share * fade)) : 1.0f;

  // The contact shadows: on the sun's share of the light only.
  float cs = 1.0f;
  float sun_vis = 1.0f;      // the game's mask as it is (the debug view 10)
  float sun_vis_used = 1.0f; // what the share takes: a vehicle's pixel has the car sun image's value, or 1 (CarAwareSunVisibility; the debug view 13)
  float sun_share = 0.0f;
  float contact_factor = 1.0f;                       // the luma's (the debug view 8)
  float3 contact_factor3 = float3(1.0f, 1.0f, 1.0f); // the one the picture takes: the sun's share of each channel
  if (contact_on) {
    if (MaskOn()) {
      const float2 uv = (float2(pixel) + 0.5f) / size;
      sun_vis = saturate(g_push.shape.x *
                         g_textures[g_push.slots2.w & 0xFFFFu].SampleLevel(g_samplers[kSamplerLinearClamp],
                                                                 uv, 0.0f).x);
      sun_vis_used = sun_vis;
    }
    if (MaskOn() || CarSunOn()) {
      sun_vis_used = CarAwareSunVisibility(sun_vis_used, PixelIsVehicle(pixel), pc);
    }
    const float sun_lit = sun * sun_vis_used;
    sun_share = (ambient + sun_lit) > 1e-6f ? sun_lit / (ambient + sun_lit) : 0.0f;
    const float contact_fade = 1.0f - smoothstep(g_push.lens.z, g_push.lens.w, tc);
    const float contact_weight = g_push.extra.w * sun_share * contact_fade;
    if (!AoContactFilter()) {
      cs = AoContactFull() ? g_textures[ContactSlot()].Load(int3(pixel, 0)).x : VisibilityAt(ContactSlot(), pixel, tc);
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
      if (normal_slot != 0u) {
        tl = LinearAt(pixel + int2(-1, 0), last);
        tr = LinearAt(pixel + int2(1, 0), last);
        tt = LinearAt(pixel + int2(0, -1), last);
        tb = LinearAt(pixel + int2(0, 1), last);
      }
      cs = AoBilateralEdgeFolded(ContactSlot(), ContactDepthSlot(), pixel, ContactImageSize(), tc, pc, normal,
                                 float4(tl, tr, tt, tb));
    }
    // Give back a share of the sun removed by the combined mask, after filtering and history.
    const uint player_id = CabinApplyId(g_push.slots.y);
    if (view == 0u && player_id != 0u) {
      const uint stencil_slot = g_push.slots2.w >> 16;
#if AO_DEPTH_MS
      const uint stencil = StencilOf(g_utextures_ms[stencil_slot].Load(pixel, g_push.slots.y & 0xFFu));
#else
      const uint stencil = StencilOf(g_utextures[stencil_slot].Load(int3(pixel, 0)));
#endif
      // CHECK: Tune the alternate fill on gauges and skin by day, in shade and at night.
      cs = CabinFillVisibility(cs, player_id, stencil >> 2);
    }
    contact_factor = 1.0f - saturate(contact_weight * (1.0f - cs));
    const float3 share_rgb = SunShareRgb(ambient, g_push.extra.x, n_dot_l * sun_vis_used);
    contact_factor3 = 1.0f - saturate((g_push.extra.w * contact_fade * (1.0f - cs)) * share_rgb);
  }
  const float3 factor3 = NgNumberOr3(ao_factor * contact_factor3, 1.0f);   // the picture is multiplied by this: a NaN or an infinity is no change (decided by the bits, nan_guard.hlsli)
  const float factor = dot(factor3, kLuma);  // its luma: the ceiling's room and the debug views

#if AO_GI
  // The bounce light. T is the light the game's constants say fell on the pixel: the ambient and the sun's
  // term, which the game's sun shadow mask takes away where the pixel is in a shadow (1 where the frame has
  // no mask). The pixel is brighter than T when something the constants do not count lights it (a street
  // lamp, headlights) or is one (neon, a glint): then the albedo is taken to be 1 (T is the chain's luma,
  // so the pixel gets B at most), and a pixel the chain says is a source of light (its alpha, ramped from where
  // a source starts) is not a surface and gets less, none when it is entirely one.
  const uint receiver_vehicle = quarter ? QuarterReceiverVehicle(pixel) : 0u;
  if (quarter && tc < fade_end) {
    // CHECK: Verify the four-tap resolve on kerbs, thin props and vehicle silhouettes.
    QuarterGiAt(pixel, tc, pc, normal, receiver_vehicle, gi_image.yzw, gi_colour);
    gi_vehicle = receiver_vehicle != 0u ? 1.0f : 0.0f;
  }
  float gi_sun_vis = 1.0f;
  if (MaskOn()) {
    const float2 gi_uv = (float2(pixel) + 0.5f) / size;
    gi_sun_vis = saturate(g_push.shape.x *
                          g_textures[g_push.slots2.w & 0xFFFFu].SampleLevel(g_samplers[kSamplerLinearClamp],
                                                                  gi_uv, 0.0f).x);
  }
  if (MaskOn() || CarSunOn()) {
    gi_sun_vis = CarAwareSunVisibility(gi_sun_vis, quarter ? receiver_vehicle != 0u : PixelIsVehicle(pixel), pc);
  }
  const float light_t = max(ambient + sun * gi_sun_vis, 1e-3f);
  const float luma_c = dot(gi_colour.rgb, kLuma);
  const float surface = 1.0f - saturate(gi_colour.a);
  const float gi_strength = SsgiStrengthFor(g_push.shape.y, gi_vehicle);
  float diffuse_share = 1.0f;   // PBR level 1C: the share of the pixel's colour that takes incoming diffuse light (a metal none, a dielectric 1 minus the reflection's weight)
  const uint material_code = GiMaterialCode();
  if (material_code != 0u) {
    const float3 material = GiLoadMaterial(pixel, material_code - 1u);
    const uint cls = GbRt3Class(material.xy);
    if (cls != GB_CLASS_NONE) {
      diffuse_share = PbrScreenDiffuseShare(cls, GbRt3MetalFlag(material), GbRt3Smoothness(material.xy),
                                           saturate(dot(normal, pc * (-1.0f / max(length(pc), 1.0e-6f)))));
    }
  }
  float3 bounce = (gi_strength * fade * surface * diffuse_share / max(light_t, luma_c)) * gi_image.yzw;
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
  const float3 gi_factor = NgNumberOr3(factor3 + bounce, 1.0f);   // (nan_guard.hlsli: a NaN or an infinity is no change)
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
      const float v = AoIsVehicle(AoLoadLinear(ApplyDepthSlot(), q)) ? 1.0f : 0.0f;
      grey = float3(v, v, v);
    } else if (view == 8u) {
      grey = float3(contact_factor, contact_factor, contact_factor);
    } else if (view == 9u) {
      grey = float3(cs, cs, cs);
    } else if (view == 10u) {
      grey = float3(sun_vis, sun_vis, sun_vis);
    } else if (view == 11u) {
      grey = float3(sun_share, sun_share, sun_share);
    } else if (view == 13u) {
      grey = float3(sun_vis_used, sun_vis_used, sun_vis_used);  // what the share took: the mask on the world, the car sun image on a vehicle
    }
    return float4(grey * kViewScale, 1.0f);
  }
#if AO_GI
  if (all(abs(gi_factor - 1.0f) < 0.001f)) {
    discard;
  }
  return float4(gi_factor, 1.0f);
#else
  if (all(factor3 >= 0.999f)) {
    discard;
  }
  return float4(factor3, 1.0f);
#endif
}
