#pragma once
#include <array>
#include <cstdint>
namespace larecomp::gpu {
struct LocalShadowView { float world_to_shadow[16]; float atlas_rect[4]; float depth_bias; float texel_size; uint32_t light_index; uint32_t valid; };
struct LocalShadowFrame { std::array<LocalShadowView, 4> views{}; uint32_t count = 0; };
}
