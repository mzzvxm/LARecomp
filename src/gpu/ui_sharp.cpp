// MCLA GPU renderer -- sharper UI art where the game's bitmaps are magnified: the cvars. See ui_sharp.h.

#include "ui_sharp.h"

#include "settings/larecomp_settings.h"

namespace mcla::gpu::ui_sharp {

double Strength() {
  if (!larecomp::settings::mcla_ui_sharp.Get()) {
    return 0.0;
  }
  return ClampStrength(larecomp::settings::mcla_ui_sharp_strength.Get());
}

bool CensusOn() {
  return larecomp::settings::mcla_ui_sharp_census.Get();
}

}  // namespace mcla::gpu::ui_sharp
