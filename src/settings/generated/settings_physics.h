#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class PhysicsSettings final : public rex::SettingOwner {
 public:
  PhysicsSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, smooth_bike_gyro, true, "Fix: Step the motorcycle gyro's smoothing in continuous time, so bikes lean and settle "
    "the same at any frame rate.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Vehicle chassis suspension damping & ground depth continuous filter
  REX_SETTING(bool, smooth_chassis_depth, true, "Fix: Smooth vehicle chassis suspension and ground depth damping at 60 FPS.", rex::kDebugOnly);

  // BadassBaboon's Recomp Adjustments: Continuous-time vehicle physics filters at 60+ FPS
  REX_SETTING(bool, smooth_vehicle_physics, true, "Fix: Continuous-time tire slip filter at 60+ FPS.", rex::kDebugOnly);

};

inline PhysicsSettings g_physics;

inline auto& smooth_bike_gyro = g_physics.smooth_bike_gyro;
inline auto& smooth_chassis_depth = g_physics.smooth_chassis_depth;
inline auto& smooth_vehicle_physics = g_physics.smooth_vehicle_physics;

}  // namespace larecomp::settings
