#include "side_view_foliage_impostor.h"

#include <rex/xenos/format.h>

#include "edram.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu {
namespace {

static_assert(foliage_impostor_view::kColorFormat == uint8_t(rex::xenos::ColorRenderTargetFormat::k_8_8_8_8));
static_assert(foliage_impostor_view::kDepthFormat == uint8_t(rex::xenos::DepthRenderTargetFormat::kD24FS8));

bool Enabled() {
  return foliage_impostor_view::FollowsOutput(larecomp::settings::mcla_gpu_impostor_native_size.Get(),
                                             larecomp::settings::mcla_ui_minimap_scale.Get());
}

}  // namespace

// The guest's foliage loop (sub_8230EE20) selects recently drawn entries.
// sub_8230DDD0 staggers the bakes by render-frame bits and checks the day/night
// mode. Its view setup reads the game clock. Keep that scheduling and all four
// samples, but bake the cards at their original size.
// CHECK: compare moving foliage at noon, dusk and night on D3D12 and Vulkan.
// Confirm the three pitch-160 attachments move together on a live toggle and
// their resolves remain readable while the guest refreshes cards in turn.
const SideViewRule kFoliageImpostorRule = {
    .view = SideView::kFoliageImpostor,
    .name = "foliage impostors",
    .matches = foliage_impostor_view::Matches<EdramKey>,
    .follows_output = true,
    .enabled = Enabled,
};

}  // namespace mcla::gpu
