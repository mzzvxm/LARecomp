#pragma once
// ===========================================================================
// MCLA GPU renderer -- ambient occlusion on a car's paint (pure maths, no GPU)
// ===========================================================================
// hlsl/ao_apply_ps.hlsl multiplies the scene colour by the occlusion, ao.cpp passes the setting. The rule that decides how much of
// the occlusion a vehicle's pixel takes lives HERE, with a program of its own that checks it (ao_paint_test.cpp: it needs no
// renderer, no ReXGlue and no GPU); the shader repeats the rule and its constants (marked "ao_paint.h" in its comments: change them
// here first) and the check compares them.
//
// What the owner saw (2026-11-17, 3840x2160, the sun low at 17.7 h, a black Nissan Skyline R34 with a rear wing on two uprights):
// with every effect on, the wing's uprights and the boot lid under the wing went almost black; with the occlusion off they showed the
// body colour again, and with everything off they were clearly tan.
//
// WHAT THE APPLY MULTIPLIES. The game draws a car's paint as ONE colour, albedo * (ambient + sun) + the reflection of the environment
// (its env map), and the apply has no G-buffer to take the parts apart: it multiplies the whole colour by
//   lerp(1, ao, strength * share * fade)
// where `share` is the ambient light's share of the light on the pixel from the depth's normal (La / (La + Ls N.L), moved towards 1
// by the look's direct number), as it is on a wall, a road or a kerb (the game's sun shadow mask is not part of it: only the contact
// shadows and the bounce light read it). On paint that share measures the wrong thing: nearly all the colour of dark glossy paint IS
// the reflection, which the ambient/sun split says nothing about, and the multiply took the occlusion off the reflection like the
// ambient part of a matte surface. How hard that bites: the stored visibility is the raw one (the open share of the hemisphere, cosine
// weighted) raised to the look's power (3.6 for the default, 3.2 to 4.5 over the four looks) and never below 0.1, so a surface that
// sees 80% of its sky is stored as 0.45 and one that sees 70% as 0.28, and a panel whose whole colour is reflection keeps, at a share
// of 0.8 (the default look):
//   the open share of the sky     0.9    0.8    0.7    0.6    0.5 and under
//   the colour the panel keeps    0.75   0.56   0.42   0.33   0.28      (the test's --print has the table)
// A part that sees only 60 to 70% of its sky (between a wing and a lid) loses 58 to 67% of its colour.
//
// WHY THAT IS NOT RIGHT FOR A REFLECTION. The occlusion's number answers "how much of the sky can this point see", a question about
// the whole hemisphere. A glossy surface sends the environment's light to the eye only from the direction of the mirror ray, and
// only a blocker on that ray (or in its narrow lobe) takes any of it away. The common specular occlusion (Lagarde and de Rousiers,
// 2014: saturate(pow(N.V + ao, exp2(-16 roughness - 1)) - 1 + ao), ao the raw visibility) comes out within 0.1 of ao itself for a clear
// coat (roughness 0.1 to 0.3) at every viewing angle, and above it at the angles a car is mostly seen at (0.74 for an ao of 0.7 at N.V
// 0.6 and roughness 0.1): a reflection is occluded by about the visibility, not by the visibility raised to a power that makes the
// diffuse term's creases dark. Nothing here knows the mirror ray (it needs the normal and the view in the same place as the roughness,
// and the paint's roughness is in no buffer this pass can read), so the rule below is the visibility-level answer of the common one.
//
// WHAT IS DONE. On a vehicle's pixel (the depth pass's mark, the share of the pixel's neighbourhood that has it, the same one the
// bounce light's strength uses: ssgi_light.h StrengthFor) the stored visibility is raised to a power below 1 before it goes into the
// multiply: the paint power, kDefaultPower = 5/16. For the default look it takes out the look's own power again (3.6 * 0.3125 =
// 1.125: about the visibility itself, which is what the specular occlusion above comes to: the test holds it within 0.06 of the raw
// visibility from 60% open to 100%), and the four looks stay in order (3.2, 3.6, 4.0 and 4.5 give 1.0, 1.125, 1.25 and 1.41: light
// darkens the paint least, max most). The stored floor of 0.1 comes out as 0.49, so the deepest crease of a car keeps about half of its
// colour at a full share (the multiply's weight, the share, is under 1 wherever the sun lights the surface). The world (a pixel with
// no vehicle in its neighbourhood) is not touched by one bit: the factor of a wall, a kerb, the road under and beside a car, the
// car_reach halo work (ao_halo.h) and the dark contact line under a car (the road's pixels, and the contact shadows' factor, which
// this does not touch) are as before. The rule starts where over half of a pixel's neighbourhood is vehicle (kVehicleFrom), all of it
// at 1: at half size the share is a bilinear mix of four texels, and the road beside a tyre has a vehicle texel or two in it. A
// silhouette blends in between.
//
// THE BOOT LID UNDER THE WING. The occlusion there is right in kind: the wing covers part of the lid's sky (a stored visibility of
// 0.16 to 0.45 is a lid that sees 60 to 80% of it), and the part of the lid directly below the wing's underside does reflect that
// underside, black and close. What was wrong was the size of the dip: a part that sees 70% of its sky kept 0.42 of its colour (at a
// share of 0.8), and keeps 0.74 now, a dip of about a quarter in step with the share of the sky the wing covers. The lid's reflection
// OF the wing itself, where the wing is in the picture, is the self reflection's work (car_selfrefl), not the occlusion's.
//
// The code rides in the apply's flags, bits 17 to 20 (the place of the denoise's radius, which the apply does not read; ao.cpp clears
// them): code + 1 sixteenths, so code 4 is 0.3125 (the default), 15 is 1 (the paint is darkened like the world: off) and 0 is the
// smallest, 0.0625 (mcla_gpu_ao_paint).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::ao_paint {

// The place and the size of the paint power's code in the apply's flags (bits 17 to 20).
inline constexpr uint32_t kCodeShift = 17;
inline constexpr uint32_t kCodeBits = 4;
inline constexpr uint32_t kCodeMask = ((1u << kCodeBits) - 1u) << kCodeShift;
// The code's unit: the power is (code + 1) of them.
inline constexpr float kCodeUnit = 1.0f / 16.0f;
// The code whose power is 1: the paint is darkened like the world.
inline constexpr uint32_t kCodeOff = 15;
// The power that is the default: 5 / 16, code 4.
inline constexpr float kDefaultPower = 0.3125f;
inline constexpr uint32_t kDefaultCode = 4;

// How much of a pixel's neighbourhood must be vehicle for the paint's rule to start: over this share, to all of it at 1. At half size the
// share is a bilinear mix of four texels (the depth-aware weights of the apply's upsample), and the road beside a tyre has a vehicle texel
// or two in it: it keeps the world's visibility, so the contact line under and beside a car is not lightened.
inline constexpr float kVehicleFrom = 0.5f;

inline float Saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

// The power a code stands for (the shader's AoPaintPower).
inline float PowerOf(uint32_t code) { return float((code & 0xFu) + 1u) * kCodeUnit; }

// The code of the setting's power: the nearest sixteenth, from 1/16 to 1; anything that is not a number is the default.
inline uint32_t CodeFor(double power) {
  if (!std::isfinite(power)) {
    return kDefaultCode;
  }
  const double steps = std::clamp(std::round(power / double(kCodeUnit)), 1.0, 16.0);
  return uint32_t(steps) - 1u;
}

// Whether a code changes anything (the shader's AoPaintOn).
inline bool Relaxes(uint32_t code) { return (code & 0xFu) != kCodeOff; }

// The visibility a pixel takes into the multiply (the shader's PaintVisibility): `ao` the stored visibility, `vehicle` the share of
// the pixel's neighbourhood the depth pass marked as a vehicle (0 to 1). A pixel with at most kVehicleFrom of its neighbourhood vehicle
// keeps its value, bit for bit; the rule is all there at 1, and a silhouette blends in between.
inline float PaintVisibility(float ao, float vehicle, uint32_t code) {
  const float on_car = Saturate((vehicle - kVehicleFrom) * 2.0f);
  if (on_car <= 0.0f || !Relaxes(code)) {
    return ao;
  }
  return ao + (std::pow(ao, PowerOf(code)) - ao) * on_car;
}

// The ambient light's share of the light on a surface (the apply's ambient_share and share): La / (La + Ls N.L), moved towards 1 by
// the look's direct number.
inline float Share(float ambient, float sun, float n_dot_l, float direct) {
  const float s = sun * Saturate(n_dot_l);
  const float ambient_share = (ambient + s) > 1e-6f ? ambient / (ambient + s) : 1.0f;
  return ambient_share + (1.0f - ambient_share) * Saturate(direct);
}

// The occlusion's own part of the apply's factor (the shader's ao_factor): lerp(1, seen, strength * share * fade).
inline float OcclusionFactor(float seen, float strength, float share, float fade) {
  return 1.0f + (seen - 1.0f) * Saturate(strength * share * fade);
}

// The stored visibility of a raw one (hlsl/ao_main_ps.hlsl: pow, then the floor) and back: the raw visibility is the open share of
// the hemisphere, cosine weighted. kStoredFloor is ao.cpp's kMinVisibility.
inline constexpr float kStoredFloor = 0.1f;
inline float StoredVisibility(float raw, float look_power) { return std::max(std::pow(raw, look_power), kStoredFloor); }

// The share of its colour that a panel whose colour is all reflection (a black glossy panel) keeps, for a raw visibility, the look's
// power, the share and a paint code: the apply's multiply on a vehicle's pixel.
inline float PanelKept(float raw, float look_power, float share, uint32_t code) {
  const float stored = StoredVisibility(raw, look_power);
  return OcclusionFactor(PaintVisibility(stored, 1.0f, code), 1.0f, share, 1.0f);
}

}  // namespace mcla::gpu::ao_paint
