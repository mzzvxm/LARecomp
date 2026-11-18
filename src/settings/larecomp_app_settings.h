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

  // The three below were REXCVAR_DEFINE lines that cvar_convert.py cannot move (a validator, a default that is a
  // constant of another file); the script removes the old definitions (HAND_DEFINED) and rewrites their uses.
  REX_SETTING(bool, debug_menu, false,
              "Unlock the hidden DEBUG menu and the developer settings, for testing and for bug reports. "
              "Turn it on with --debug_menu on the command line, REX_LARECOMP_DEBUG_MENU=1 in the environment, or "
              "debug_menu = true in larecomp.toml. It cannot be changed while the game runs, and the game "
              "never writes it to the file. Settings changed in the DEBUG menu are not saved either.",
              rex::kStartupOnly, rex::kDebugOnly);

  // The old validator (adaptive, or a number of milliseconds 16 to 1000) is not available: the reader
  // (hooks/frame_timing.cpp ParseStepCapMs / IsAdaptiveStepCap) falls back to adaptive for any other text.
  REX_SETTING(std::string, frame_step_cap, "adaptive",
              "How far one frame may advance the game's clock after a stall (a texture-streaming hitch, a "
              "long load): the step is cut to this and the game plays that instant in slow motion, as the "
              "console did, instead of jumping ahead. adaptive (the default) is 1.5 times "
              "the recent median frame time, never below 34 ms or above 125 ms: about 34 ms at 60 FPS, 50 ms "
              "at a steady 30 FPS (so a slow PC never runs in slow motion). Or a number of milliseconds for a "
              "fixed cap, 16 to 1000 (125 was the old fixed cap). The cap cannot be turned off, and the "
              "MCLA_MAX_FRAME_MS environment variable, when set, wins (as a fixed number).",
              rex::kDebugOnly);

  // rmr::kDefaultPort (mc_engine/online/rate_my_ride/rmr_protocol.h); rmr_client.cpp static_asserts the two agree.
  REX_SETTING(rex::i32, rmr_server_port, 31002,
              "TCP port of the Rate My Ride server when it runs on the relay host "
              "(default 31002). A port typed into rmr_server overrides it.");
};

inline AppSettings g_app;
inline auto& graphics_backend = g_app.graphics_backend;
inline auto& debug_menu = g_app.debug_menu;
inline auto& frame_step_cap = g_app.frame_step_cap;
inline auto& rmr_server_port = g_app.rmr_server_port;

}  // namespace larecomp::settings
