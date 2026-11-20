#pragma once
#include <array>
#include <cstdint>
namespace larecomp::gpu {
struct Headlight { float pos[3]; float dir[3]; float cos_outer; float cos_inner; float range; float color[3]; uint32_t car_key; bool is_player; };
struct HeadlightSet { std::array<Headlight, 4> lights; uint32_t count = 0; };
const HeadlightSet& CurrentHeadlights();
}
