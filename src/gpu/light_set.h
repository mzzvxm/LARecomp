#pragma once
// ===========================================================================
// MCLA GPU renderer -- the light set the deferred light pass takes: THE FROZEN INTERFACE (package IF of perf/GBUFFER-SURVEY.md 7.2)
// ===========================================================================
// A superset of the headlight rig's HeadlightSet (headlight_rig.h: up to four spot lights, `Headlight` = position, direction, the two cone
// cosines, range, colour, car key, is_player): what the deferred light pass (GB-H) draws, whoever made the lights. The producers are the headlight
// rig (spot lights on cars, next frame), the city's own lamps and neon where the game draws them as nothing (a second grid for modded ground:
// mod_lights.cpp), the police lights, and a mod's lights.bin. The pass adds each light's term into the main view's colour at the mid-scene depth copy,
// scene referred (the units of the game's own lamps: "20 is a LA street lamp, reaches sqrt(100 x 20) = 44.7 m"), and, when a light has a shadow view
// (larecomp::gpu::LocalShadowFrame of the headlight shadow replay's local_shadow_types.h, which is not in this tree yet: `shadow_view` indexes its
// `views`), takes its visibility from there.
//
// A producer fills a LightSet for the NEXT frame at the frame's end (the rig's rule: OnPresent) and publishes it (the call that publishes is GB-H's
// to write); the pass reads the one published for its frame. A light the producer wants gone fades out through its colour,
// it is not removed in the middle of a frame.
//
// Units and conventions: positions and directions are WORLD space (the game's: y up), distances in the game's metres. `color` is the colour times the
// intensity, already times the strength, the fade and the night factor: a light that fades is a dimmer one. `dir` is a unit vector. The cone: `cos_outer`
// is the cosine of the outer half angle (the light is 0 outside it), `cos_inner` of the inner one (full inside it), smooth between; a point light has
// kind kPoint, dir zero and both cosines -1.
//
// Threading: plain data; a producer and the pass are both on the render thread.
// ===========================================================================

#include <array>
#include <cstdint>

namespace larecomp::gpu {

enum class LightKind : uint32_t { kSpot = 0, kPoint = 1 };

// Who made the light, for the pass's per-source settings and its debug lines.
enum class LightSource : uint32_t {
  kHeadlight = 0,  // a car's headlight (headlight_rig.h)
  kLamp = 1,       // a street lamp or a window the game does not draw as a light volume
  kNeon = 2,
  kPolice = 3,
  kMod = 4,        // lights.bin of a mod (mod_lights.cpp)
  kOther = 5,
};

inline constexpr uint32_t kLightFlagPlayer = 1u << 0;     // the player's car's lamp (the rig's is_player)
inline constexpr uint32_t kLightFlagLightsGeometry = 1u << 1;  // the light also lights the surface that carries it (a lamp's own housing: usually off)

inline constexpr int32_t kNoShadowView = -1;
inline constexpr uint32_t kMaxLights = 32;  // a handful to 32 a frame (the survey's budget)

struct Light {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float range = 0.0f;                      // metres: the light is 0 beyond
  float dir[3] = {0.0f, 0.0f, 0.0f};       // the axis (a spot); zero for a point light
  float cos_outer = -1.0f;
  float color[3] = {0.0f, 0.0f, 0.0f};     // colour x intensity, scene units
  float cos_inner = -1.0f;
  LightKind kind = LightKind::kSpot;
  LightSource source = LightSource::kOther;
  uint32_t car_key = 0;                    // a number that stays with a car from frame to frame (0: not a car's)
  int32_t shadow_view = kNoShadowView;     // index into LocalShadowFrame::views of the frame's shadow atlas, kNoShadowView: unshadowed
  uint32_t flags = 0;                      // kLightFlag*
};

struct LightSet {
  std::array<Light, kMaxLights> lights{};
  uint32_t count = 0;
  uint32_t frame = 0;  // the frame the set is for (low 32 bits of the Scene's frame serial)
};

// The headlight rig's `Headlight` (headlight_rig.h) is a Light of kind kSpot, source kHeadlight: the same fields. A producer that holds a
// HeadlightSet converts it with this (a template so that this header needs no rig include; any type with those members converts).
template <class HeadlightT>
inline Light LightFromHeadlight(const HeadlightT& h) {
  Light l;
  for (int k = 0; k < 3; ++k) {
    l.pos[k] = h.pos[k];
    l.dir[k] = h.dir[k];
    l.color[k] = h.color[k];
  }
  l.range = h.range;
  l.cos_outer = h.cos_outer;
  l.cos_inner = h.cos_inner;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  l.car_key = h.car_key;
  l.flags = h.is_player ? kLightFlagPlayer : 0u;
  return l;
}

// A set from a HeadlightSet-shaped list (`lights` an indexable array, `count` how many are in use).
template <class HeadlightSetT>
inline LightSet LightSetFromHeadlights(const HeadlightSetT& set, uint32_t frame) {
  LightSet out;
  out.frame = frame;
  for (uint32_t i = 0; i < set.count && i < kMaxLights; ++i) {
    out.lights[i] = LightFromHeadlight(set.lights[i]);
    ++out.count;
  }
  return out;
}

}  // namespace larecomp::gpu
