#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class FunSettings final : public rex::SettingOwner {
 public:
  FunSettings() : SettingOwner("larecomp") {}

  REX_SETTING(f64, squash_amplitude, 0.25, "Squash depth, 0..0.9. 0.25 = the body loses a quarter of its height at the "
    "bottom of the cycle and gains half of that in width.", rex::kDebugOnly);

  REX_SETTING(f64, squash_hz, 1.5, "Oscillations per second.", rex::kDebugOnly);

  REX_SETTING(f64, squash_pivot, 0.0, "Pivot height in metres along the up axis, in car space. 0 = scale about the "
    "car origin (the body sinks and rises). Negative values push the pivot down "
    "toward the tyre contact patch so the car keeps its ground contact.", rex::kDebugOnly);

  REX_SETTING(bool, squash_stretch, false, "Squash & stretch the player's car like the \"Why's This Dealer?\" meme. "
    "Visual only — physics and collision are untouched.", rex::kDebugOnly);

  REX_SETTING(bool, squash_traffic, false, "Apply the squash to every car in the world instead of just the player's.", rex::kDebugOnly);

  REX_SETTING(i32, squash_up_axis, 2, "Which car-space axis is up: 0 = X, 1 = Y, 2 = Z (RAGE default). The other "
    "two axes take the anti-phase widening.", rex::kDebugOnly);

};

inline FunSettings g_fun;

inline auto& squash_amplitude = g_fun.squash_amplitude;
inline auto& squash_hz = g_fun.squash_hz;
inline auto& squash_pivot = g_fun.squash_pivot;
inline auto& squash_stretch = g_fun.squash_stretch;
inline auto& squash_traffic = g_fun.squash_traffic;
inline auto& squash_up_axis = g_fun.squash_up_axis;

}  // namespace larecomp::settings
