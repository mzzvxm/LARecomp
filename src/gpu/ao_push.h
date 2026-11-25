#pragma once
// ===========================================================================
// MCLA GPU renderer -- the push block of the ambient occlusion's passes (hlsl/ao_common.hlsli AoPush), 128 bytes
// ===========================================================================
// One block for every pass of the AO's family, each pass reading the fields it needs (the field list of each is in hlsl/ao_common.hlsli and in the shader's own header). It is a header of its own
// since SS-2 (perf/LIGHT-UNIFY.md section 5.5): the depth chain, the normal image and the colour chain are recorded by the shared screen products (screen_extract.h, screen_shared.h), which
// build the same block the AO does. Plain data: no Plume, so that the tests build alone.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu {

struct AoPush {
  uint32_t slots[4];
  uint32_t slots2[4];
  float sizes[4];
  float lens[4];
  float lin[4];
  float shape[4];
  float light[4];
  float extra[4];
};
static_assert(sizeof(AoPush) == 128);

}  // namespace mcla::gpu
