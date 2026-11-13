// MCLA GPU renderer -- the environment map as a side view class. See side_views.h.
//
// EMPTY ON PURPOSE: the class exists so that the mechanism (EdramTargets, ResolveCache, TextureCache)
// is complete and package G2a (environment map resolution, GFX-UPGRADES-DESIGN.md 4.2 2a and 4.3) fills
// in this one file and nothing else: the identities, the setting and the samples.
//
// What G2a needs from the census (GFX-UPGRADES-DESIGN.md 1.1, 4.3, 6A.2 item 6):
//   - the view: colour k_2_10_10_10_FLOAT at EDRAM tile 416, pitch 520, 4 samples, and depth kD24FS8 at
//     tile 0, pitch 520, 4 samples, both declared 512x256; resolved into the 512x256
//     k_16_16_16_16_EXPAND product (base 032E2000, mips at 033E2000, levels 0 to 3);
//   - AND the three small guest-size passes that make mip levels 1 to 3 (resolves with flags 0x100 into
//     levels 1, 2, 3, rects 256x128, 128x64, 64x32, caller 82184898): their identities must be members
//     too (package G0b finds them: the draws between the level 0 and the level 3 resolve), or the
//     product's levels would come from sources at two scales and be made again at every resolve
//     (ResolveCache::Activate), or a 2x map would be downsampled into guest-size levels. Every member
//     has the class's scale, so the product's four levels are one scale;
//   - host_samples: 2 at the scales above 1x (or 1), the guest's at 1x; the colour resolve averages
//     the samples;
//   - setting: mcla_gpu_env_resolution (auto | game | high | ultra; autos {game, game, high}), live,
//     defined here beside the rule; and the pause row CAR REFLECTIONS.
//   - nothing else in the mechanism: the product with its mips is read at its own scale
//     (Product::own_scale: no reduced copy, no null texture for more than one level).

#include <cstdint>
#include <string>

#include "edram.h"
#include "side_views.h"

namespace mcla::gpu {

namespace {

bool Matches(const EdramKey&, uint32_t) {
  return false;  // G2a
}

std::string Setting() {
  return "game";  // G2a: mcla_gpu_env_resolution
}

uint32_t HostSamples(Scale, uint32_t guest_samples) {
  return guest_samples;  // G2a
}

}  // namespace

const SideViewRule kEnvRule = {
    .view = SideView::kEnv,
    .name = "car reflections",
    .matches = Matches,
    .setting = Setting,
    .effect = gpu_class::Effect::kEnvResolution,
    .host_samples = HostSamples,
};

}  // namespace mcla::gpu
