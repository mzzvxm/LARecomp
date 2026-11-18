#pragma once
//
// LARecomp settings that were not REXCVAR_DEFINE lines of LARecomp: they lived in the fork's SDK or are new with the
// move to rexglue 0.11. Same scope and naming rule as the generated owners (settings/larecomp_settings.h):
// the member is the old flat name, the full name is `larecomp.<name>`.
//
#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class AppSettings final : public rex::SettingOwner {
 public:
  AppSettings() : SettingOwner("larecomp") {}

  // The fork SDK defined this (src/ui/rex_app.cpp). In 0.11 the renderer CHOICE is the SDK's `gpu.driver`:
  // "larecomp" (the default: LARecomp's no-command-processor driver, nocp_driver.h, which carries the Plume
  // renderer) or "any" / "d3d12" / "vulkan" (the SDK's Xenia GPU emulation, the old XENIA renderer). This setting
  // stays the graphics API of the Plume renderer.
  REX_SETTING(std::string, graphics_backend, "auto",
              "Graphics API: auto, d3d12, vulkan. Read at start-up by the Plume renderer (and, for the XENIA renderer, "
              "it picks the SDK's d3d12 or vulkan driver)",
              rex::Allowed("auto", "d3d12", "vulkan"), rex::kRestart);
};

inline AppSettings g_app;
inline auto& graphics_backend = g_app.graphics_backend;

}  // namespace larecomp::settings
