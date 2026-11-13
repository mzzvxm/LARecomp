// MCLA GPU renderer -- the sun's shadow cascades as a side view class. See side_views.h.
//
// EMPTY ON PURPOSE: the class exists so that the mechanism (EdramTargets, ResolveCache, TextureCache)
// is complete and package G1a (cascade resolution, GFX-UPGRADES-DESIGN.md 3.2 1a) fills in this one
// file and nothing else: the identities, the setting and the samples.
//
// What G1a needs from the census (GFX-UPGRADES-DESIGN.md 1.1, 6A.2 item 2): the four cascades draw into
// depth kD24S8 at EDRAM tile 0, pitch 640, 4 samples, declared 640x640, no colour target; the four
// resolves into the 1280x1280 k_24_8 atlas (base 06E65000) take one sample each (sample bits 1).
//   - matches: that depth key (and nothing else: the main view's and the reflection's depth keys have
//     other pitches).
//   - host_samples: 1 at every scale (one guest sample is ever read; at the guest's size the sample
//     position changes by up to 0.4 texel against guest sample 0 of 4: CHECK the bias in a picture).
//   - setting: mcla_gpu_shadow_resolution (auto | game | high | ultra; autos {game, high, high}), live,
//     defined here beside the rule; and the pause row SHADOW DETAIL.
//   - nothing else in the mechanism: the atlas product is read at its own scale (Product::own_scale),
//     the resolve's destination offsets (multiples of 32) are scaled by the source target's scale.

#include <cstdint>
#include <string>

#include "edram.h"
#include "side_views.h"

namespace mcla::gpu {

namespace {

bool Matches(const EdramKey&, uint32_t) {
  return false;  // G1a
}

std::string Setting() {
  return "game";  // G1a: mcla_gpu_shadow_resolution
}

uint32_t HostSamples(Scale, uint32_t guest_samples) {
  return guest_samples;  // G1a: 1
}

}  // namespace

const SideViewRule kShadowRule = {
    .view = SideView::kShadow,
    .name = "sun shadows",
    .matches = Matches,
    .setting = Setting,
    .host_samples = HostSamples,
};

}  // namespace mcla::gpu
