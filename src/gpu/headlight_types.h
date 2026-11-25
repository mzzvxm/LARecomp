#pragma once
// ===========================================================================
// The headlight rig's frozen interface: the lights it hands over (headlight_rig.h), with no renderer in it
// ===========================================================================
// The headlight shadow maps (local_shadow.h) read the set; the standalone checks of their rules (local_shadow_rules_test.cpp) need the types without the
// rig's renderer headers, so they live here and headlight_rig.h includes this. The other half of the interface is local_shadow_types.h.
// ===========================================================================

#include <array>
#include <cstdint>

namespace mcla::gpu {

// One headlight as the rig hands it over. Positions and directions are world space (the game's: y up). `dir` is a unit vector (the beam's axis, dipped a
// little below the car's forward), `cos_outer` / `cos_inner` the cosines of the cone's outer and inner half angles, `range` the reach in metres, `color` the
// light's colour already times the strength, the fade and the night factor (so a light that fades is a dimmer one), `car_key` a number that stays with a car from
// frame to frame, `is_player` the player's car's.
// THE LIGHT IS A CAR'S, not a lamp's: `pos` is the middle of the car's two headlamps and one term lights what the pair lights. `lamp_offset` is the vector from `pos`
// to the car's right lamp (the left one is the opposite), in metres: the shadow maps are made from each lamp (local_shadow.h), so that a pole in front of the car casts
// two shadows and a car's shadow has the half-lit band of one lamp. Zero: the rig does not know where the lamps are, and the light has one view (from `pos`).
struct Headlight {
  float pos[3];
  float dir[3];
  float cos_outer;
  float cos_inner;
  float range;
  float color[3];
  uint32_t car_key;
  bool is_player;
  float lamp_offset[3];
};

struct HeadlightSet {
  std::array<Headlight, 4> lights;
  uint32_t count = 0;
};

// The set of the frame being built (empty: no light, and the game's shaders draw).
const HeadlightSet& CurrentHeadlights();

}  // namespace mcla::gpu

// The same names where the shared interface's other half (local_shadow_types.h) lives.
namespace larecomp::gpu {
using mcla::gpu::CurrentHeadlights;
using mcla::gpu::Headlight;
using mcla::gpu::HeadlightSet;
}  // namespace larecomp::gpu

