#pragma once
//
// LarecompApp on rexglue 0.11: a rex::app::TitleApp (UPSTREAM-0.11-PLAN.md 8.2, the app-model spike).
//
// 0.10 ReXApp hook                                    -> 0.11
//   OnPreSetup(RuntimeConfig&) (flags, gpu_plugin,     -> OnConfigure(SettingsRegistry&): SetDefault() calls (title layer: a
//     audio/input factories)                              player's value always wins) and OnRegisterModules (the no-CP driver)
//   OnConfigurePaths / OnFinalizePaths (paths,         -> app.user_root decided in OnConfigure, the side effects in OnInit;
//     wizards)                                            wizards between Init() and LaunchTitle() (RunSetupFrame)
//   SetupPresentation / OnCreateImmediateDrawer /      -> OnInit: the Plume renderer starts before LaunchTitle
//     fonts / style / hotkeys                             (IUISystem fonts, style, OverlayConfig + ui.bind.<name>)
//   SetupEnvironment's --build_shader_packs exit       -> GetActions/OnAction, run in OnInit with no drivers loaded
//   OnPostSetup / OnPostLoadXexImage / LaunchModule    -> OnTitleLaunch(TitleRuntime&): after Load, before the guest starts
//   OnWindowCloseRequested (veto + FinishWindowClose)  -> IUIEventListener kClose (saves, devices: the UI thread, before RequestShutdown),
//                                                         the end of OnRun (the same work again if it did not run, then the renderer stops), then the SDK
//                                                         stops the title (TitleApp::OnStop is final in 0.11: nothing of ours can run in it)
//   OnShutdown                                         -> OnShutdown (the SDK ends the process through App::Shutdown)
//   OnWindowPixelSizeChanged / OnWindowFocusChanged    -> IUIEventListener kResize / kFocus
//
// Nothing here has been compiled: every `CHECK:` in larecomp_app.cpp is a point only a build can settle.
//
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/app/title_app.h>
#include <rex/cli.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "app_paths.h"

class LarecompApp final : public rex::app::TitleApp, private rex::ui::IUIEventListener {
 public:
  // What a command line action asks for instead of a game: the launcher's setup runs both.
  enum class BatchMode : uint8_t { kNone, kBuildShaderPacks, kPrecachePipelines };

  LarecompApp();
  ~LarecompApp() override;

  // Called by main before Init (the arguments are scanned for the actions there, because the system requirements
  // depend on them and GetRequirements() runs after the layers are applied).
  void SetBatchMode(BatchMode mode) { batch_ = mode; }
  BatchMode GetBatchMode() const { return batch_; }

  // A --config_file= the command line carries: OnConfigure reads larecomp.toml before the SDK applies the command line, so it
  // cannot see the option itself (main scans for it, as it does for the actions).
  void SetConfigFileHint(std::filesystem::path file) { config_file_hint_ = std::move(file); }

  // First-launch wizards between Init() and LaunchTitle(): the save picker, the ISO installer, the save importer.
  // RunSetupFrame pumps one UI frame and advances the state machine; false when a shutdown was requested.
  bool SetupPending() const;
  bool RunSetupFrame();

  // The six folders, from the settings (valid from OnInit on).
  larecomp::Paths CurrentPaths();  // not const: GetTitleRuntime() is not

 protected:
  rex::AppSysRequirements GetRequirements() const override;
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override;
  // A player whose larecomp.toml cannot be read still gets into the game (defaults, the file kept as .unreadable.bak), and a refused value
  // costs only its own key (rex::app::App::ToleratesBrokenConfig).
  bool ToleratesBrokenConfig() const override { return true; }
  std::span<const rex::CLIAction> GetActions() const override;
  rex::Result<void> OnAction(const rex::CLIActionHit& hit) override;
  void OnModules(std::vector<std::string>& modules) override;
  void OnRegisterModules(std::vector<rex::IModule*>& modules) override;
  rex::Result<void> OnInit() override;
  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override;
  void OnTitleExit(rex::kernel::TitleExitReason reason) override;
  void OnRun() override;   // TitleApp::OnRun, then StopBeforeTitleEnds (TitleApp::OnStop is final in 0.11)
  void OnShutdown() override;
  rex::platform::CrashReporterOptions GetCrashOptions() override;

 private:
  // IUIEventListener
  void OnUIEvent(const rex::ui::UIEvent& event) override;
  void OnInputEvent(rex::InputEvent& event) override;

  enum class SetupStep : uint8_t { kProfile, kInstall, kSaveOffer, kDone };

  void ApplyStartupDefaults(rex::SettingsRegistry& settings);
  void ChooseUserFolder();           // OnConfigure: the default of app.user_root
  void MoveUserDataIfNeeded();       // OnInit: the side effects of that choice
  void StartOverlays();              // OnInit: fonts, style, hotkeys, overlays
  void BeginSetupStep();
  void FinishWindowCloseWork();      // the pre-close work (saves, devices) that must precede the title's end
  void StopBeforeTitleEnds();        // that work, the achievement manager's release and the renderer's stop: once, before the SDK unloads the title
  bool RunBootBuild(rex::kernel::TitleRuntime& title);
  void RunBatchMode();
  void SetXeniaHostDefaults();

  BatchMode batch_ = BatchMode::kNone;
  std::vector<std::string> path_notes_;
  std::filesystem::path config_file_hint_;
  std::filesystem::path settings_config_path_;
  SetupStep step_ = SetupStep::kProfile;
  bool step_started_ = false;
  std::atomic<bool> step_done_{false};
  std::atomic<bool> close_work_started_{false};
  std::atomic<bool> before_title_end_done_{false};
  rex::ScopeGuard ui_listener_;
  rex::ScopeGuard exit_listener_;
  rex::kernel::TitleRuntime* title_ = nullptr;
};
