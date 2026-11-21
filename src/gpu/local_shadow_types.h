#pragma once
// The shadow half of the headlights' interface (the lights are in headlight_types.h): what the producer of the maps (local_shadow.h) hands the pass that
// lights the world with the headlights (headlight_rig.h SetLocalShadows).
#include <array>
#include <cstdint>
namespace larecomp::gpu {
// How many views a frame holds: a car's two headlamps have a view each (a pole that both lamps light casts two shadows), and the rig lights four cars at most, but the
// levels shadow three at the most (local_shadow_rules.h), so six are made; eight is the room the lit shaders' block and the volume's constants keep.
inline constexpr uint32_t kMaxShadowViews = 8;
// One map: the world point -> atlas matrix (column vectors, row-major: out = M * (p, 1), then / w gives the atlas uv in xy and the depth in z), the
// tile in the atlas as u0 v0 u1 v1 (a point outside it is lit), the constant depth bias the lookup subtracts from the receiver's depth (z/w units),
// the size of one texel of the tile as a share of the tile's own width and height (so one texel is texel_size * (u1 - u0) in u and
// texel_size * (v1 - v0) in v), the place in the rig's set (headlight_types.h) of the light the map is of (the rig matches it to a light by the
// car, headlight_rig.h SetLocalShadows), and whether the view counts. A light (a car's) has one view, or two: one for each of its headlamps; the light's share
// that reaches a point is then the mean of its two views' (each lamp lights the point or does not).
struct LocalShadowView { float world_to_shadow[16]; float atlas_rect[4]; float depth_bias; float texel_size; uint32_t light_index; uint32_t valid; };
// The maps of a frame: one for each lamp of a car that has its lamps apart, one for a car whose lamps share a view.
struct LocalShadowFrame { std::array<LocalShadowView, kMaxShadowViews> views{}; uint32_t count = 0; };
}
