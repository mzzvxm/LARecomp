#pragma once
//
// LARecomp settings that were not REXCVAR_DEFINE lines of LARecomp: they lived in the fork's SDK or are new with the
// move to rexglue 0.11. Same scope and naming rule as the generated owners (settings/larecomp_settings.h):
// the member is the old flat name, the full name is `larecomp.<name>`.
//
#include <string>

#include <rex/setting.h>
#include <rex/types.h>

#include "settings/step_cap.h"
#include "settings/immortal.h"
#include "settings/validated_setting.h"

namespace larecomp::settings {

class AppSettings final : public rex::SettingOwner {
 public:
  AppSettings() : SettingOwner("larecomp") {}

  // The fork SDK defined this (src/ui/rex_app.cpp). In 0.11 the renderer CHOICE is the SDK's `gpu.driver`:
  // "larecomp" (the default: LARecomp's no-command-processor driver, nocp_driver.h, which carries the Plume
  // renderer) or "any" / "d3d12" / "vulkan" (the SDK's Xenia GPU emulation, the old XENIA renderer). This setting
  // stays the graphics API of the Plume renderer; the XENIA renderer's API is the gpu.driver value itself.
  REX_SETTING(std::string, graphics_backend, "auto",
              "Graphics API of the Plume renderer: auto, d3d12, vulkan. Read at start-up. The XENIA renderer's API is "
              "the SDK's gpu.driver (d3d12 or vulkan), which also picks the renderer (larecomp, or the SDK's Xenia GPU)",
              rex::Allowed("auto", "d3d12", "vulkan"), rex::kRequiresRestart);

  // The three below were REXCVAR_DEFINE lines that cvar_convert.py cannot move (a validator, a default that is a
  // constant of another file); the script removes the old definitions (HAND_DEFINED) and rewrites their uses.
  //
  // THE TWO VALIDATORS THE CONVERSION DROPPED (the only two `.validator(` in the tree before the conversion,
  // `git grep "validator(" 4538f255 -- src`; the script's HAND_DEFINED set keeps their definitions out of the generated
  // headers):
  //   debug_menu       (mc_engine/debug_menu.cpp)         refused a write once InitDebugMenu had run.
  //                    NOW NATIVE: kInitOnly. The SDK freezes it just before OnInit (App::Init,
  //                    ISettingsRegistryControl::Freeze(kInitOnly)); a write after that fails with "can no longer
  //                    change", for the console, the settings panel and code. The old comment said the SDK never
  //                    enforced kInitOnly (nothing called FinalizeInit): 0.11 does.
  //   frame_step_cap   (mc_engine/hooks/frame_timing.cpp) "adaptive" or a number of ms (positive, finite).
  //                    RE-ADDED as settings/validated_setting.h's ValidatedString (the model has no validator trait;
  //                    a SettingBase whose Commit tests the text), with the same functions the reader uses
  //                    (settings/step_cap.h).
  REX_SETTING(bool, debug_menu, false,
              "Unlock the hidden DEBUG menu and the developer settings, for testing and for bug reports. "
              "Turn it on with --debug_menu on the command line, REX_DEBUG_MENU=1 (or REX_LARECOMP_DEBUG_MENU=1) in the "
              "environment, or debug_menu = true in larecomp.toml. It cannot be changed while the game runs, and the "
              "game never writes it to the file. Settings changed in the DEBUG menu are not saved either.",
              rex::kInitOnly, rex::kDebugOnly);

  // A test of the compatibility renderer offer (gpu/renderer_fallback.h), for the main session and testers: no graphics card is needed that
  // the native renderer refuses. Read in mcla::gpu::Startup, which hands it to Device::Options, so the real failure path runs (Plume's device
  // is refused at Device::Open for each API, the box lists the two refusals, the button writes gpu.driver and restarts).
  REX_SETTING(bool, mcla_gpu_force_refusal, false,
              "A test of the box that offers the compatibility renderer: both graphics APIs are refused on purpose, as if the graphics card "
              "could not run the native renderer, so the game shows that box (with its Use the compatibility renderer button) and stops instead "
              "of starting. Read at start. Debug only: use it as a launch argument; it is never saved.",
              rex::kInitOnly, rex::kDebugOnly);

  REX_SETTING(std::string, mcla_gpu_crash_diag, "auto",
              "GPU crash diagnostics: auto enables detailed capture for the launch after a GPU device loss. "
              "On always captures and off keeps only the basic loss report. Microsoft measured a 2-5% cost "
              "for D3D12 auto-breadcrumbs. Restart with --mcla_gpu_crash_diag=on or =off to override auto.",
              rex::Allowed("auto", "on", "off"), rex::kRequiresRestart, rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_force_device_removal, false,
              "D3D12 test: remove the game's graphics device after a submitted frame, once from frame 120 onward. "
              "The picture stops and the game needs a restart. Live. Vulkan requires an external driver reset.",
              rex::kDebugOnly);

  REX_SETTING(std::string, render_wait_mode, "auto",
              "How the Plume render thread waits. auto (the default) and wait block until work or the present deadline. "
              "spin restores the previous polling behavior for A/B tests. Takes effect on the next wait.",
              rex::Allowed("auto", "wait", "spin"), rex::kDebugOnly);

  REX_SETTING(bool, mcla_gpu_vk_present_pacing, false,
              "On Windows, limit windowed and borderless NVIDIA Vulkan presents to the display refresh or 120 FPS, "
              "whichever is lower (60 FPS if refresh is unknown). Lower frame limits still apply. Live within one second. "
              "Turn off for A/B tests of the Windows compositor.", rex::kDebugOnly);

  REX_SETTING(rex::u32, cpu_limit_lp, 0,
              "At startup, restrict the game to the first N logical processors available in its affinity mask. "
              "0 leaves the affinity alone. For CPU scaling tests.",
              rex::kInitOnly, rex::kDebugOnly);

  REX_SETTING(bool, mcla_test_present_nowait, false,
              "Headless test only: drop D3D12 presents when DXGI would block. Requires debug_menu. "
              "Read at startup.",
              rex::kInitOnly, rex::kDebugOnly);

  REX_SETTING(bool, mcla_test_d3d12_latency_one, false,
              "Test DXGI maximum frame latency of one on the D3D12 swap chain. "
              "Requires debug_menu. Read at startup. Applies with frame generation off.",
              rex::kInitOnly, rex::kDebugOnly);

  ValidatedString frame_step_cap{
      this, "frame_step_cap", "adaptive",
      "How far one frame may advance the game's clock after a stall (a texture-streaming hitch, a "
      "long load): the step is cut to this and the game plays that instant in slow motion, as the "
      "console did, instead of jumping ahead. adaptive (the default) is 1.5 times "
      "the recent median frame time, never below 34 ms or above 125 ms: about 34 ms at 60 FPS, 50 ms "
      "at a steady 30 FPS (so a slow PC never runs in slow motion). Or a number of milliseconds for a "
      "fixed cap, 16 to 1000 (125 was the old fixed cap). The cap cannot be turned off, and the "
      "MCLA_MAX_FRAME_MS environment variable, when set, wins (as a fixed number).",
      &IsValidStepCap, "is neither adaptive nor a number of milliseconds", rex::kDebugOnly};

  // rmr::kDefaultPort (mc_engine/online/rate_my_ride/rmr_protocol.h); rmr_client.cpp static_asserts the two agree.
  REX_SETTING(rex::i32, rmr_server_port, 31002,
              "TCP port of the Rate My Ride server when it runs on the relay host "
              "(default 31002). A port typed into rmr_server overrides it.");
};

inline AppSettings& g_app = larecomp::Leak<AppSettings>();
inline auto& graphics_backend = g_app.graphics_backend;
inline auto& debug_menu = g_app.debug_menu;
inline auto& mcla_gpu_force_refusal = g_app.mcla_gpu_force_refusal;
inline auto& mcla_gpu_crash_diag = g_app.mcla_gpu_crash_diag;
inline auto& mcla_gpu_force_device_removal = g_app.mcla_gpu_force_device_removal;
inline auto& render_wait_mode = g_app.render_wait_mode;
inline auto& mcla_gpu_vk_present_pacing = g_app.mcla_gpu_vk_present_pacing;
inline auto& cpu_limit_lp = g_app.cpu_limit_lp;
inline auto& mcla_test_present_nowait = g_app.mcla_test_present_nowait;
inline auto& mcla_test_d3d12_latency_one = g_app.mcla_test_d3d12_latency_one;
inline auto& frame_step_cap = g_app.frame_step_cap;
inline auto& rmr_server_port = g_app.rmr_server_port;

}  // namespace larecomp::settings
