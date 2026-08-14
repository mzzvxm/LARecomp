#pragma once

#include <rex/rex_app.h>
#include <rex/system/flags.h>
#include <rex/system/achievement_manager.h>
#include "discord_rpc/discord_rpc.h"
#include "mc_engine/threading.h"
#include "mc_engine/logging.h"
#include "mc_engine/graphics_button.h"
#include "spdlog_console.h"
#include "larecomp_log.h"
#include "crash_handler.h"
#include "mc_engine/hooks.h"
#include "mc_engine/pause_menu.h"
#include "mc_engine/string_table.h"
#include "isoinstaller/larecomp_iso_installer.h"
#include "saveporter/larecomp_save_porter.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern uint8_t* g_guest_mem;

class LarecompApp : public rex::ReXApp {
 public:
  LarecompApp(rex::ui::WindowedAppContext& ctx,
              std::string_view name,
              rex::PPCImageInfo ppc_info)
      : rex::ReXApp(ctx, name, ppc_info) {
    InitLarecompLogging();
    LARECOMP_APP_INFO("LA Recompiled - @by mzzvxm");
    InstallCrashLogger();
    if (auto* w = window()) {
      w->SetTitle("Midnight Club Los Angeles - by @mzzvxm");
    }
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LarecompApp>(
        new LarecompApp(ctx, "larecomp", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The Xenos GPU emulation is a runtime-loaded plugin as of SDK 0.9.0 and
    // the gpu_plugin cvar defaults to empty (= no GPU at all). Name it here so
    // the game works without anything in larecomp.toml; an explicit cvar still
    // wins because this only fills in the blank.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  void OnShutdown() override {
    LARECOMP_Discord_Shutdown();
    mc::DisableHighResTimer();
    ShutdownLarecompLogging();
    std::_Exit(0);
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto root = ExeDir();

    // A bare default.xex is not enough to call a directory the game root: the
    // ISO installer writes the xex first and the archives after, so a half-
    // finished install would otherwise be accepted and then fail at boot.
    // Requiring one of the archives next to it makes the probe honest.
    auto is_valid_game_root = [](const std::filesystem::path& p) {
      std::error_code ec;
      if (!std::filesystem::exists(p / "default.xex", ec)) return false;
      return std::filesystem::exists(p / "xarchive_cache.rpf", ec) ||
             std::filesystem::exists(p / "xarchive_audio.rpf", ec) ||
             std::filesystem::exists(p / "xarchive_audlo.rpf", ec);
    };

    // If the user has manually passed --game_data_root and it is valid,
    // keeps the manual path.
    if (!paths.game_data_root.empty() && is_valid_game_root(paths.game_data_root)) {
      return;
    }

    // BadassBaboon's Recomp Adjustments: walk up from the exe looking for the
    // game data, so a portable layout works without --game_data_root. Checked
    // at every level, nearest first: MCLA_Game_Files/, game/, assets/, then the
    // directory itself (exe dropped straight into the game folder).
    for (std::filesystem::path dir = root; !dir.empty() && dir != dir.parent_path();
         dir = dir.parent_path()) {
      for (const char* sub : {"MCLA_Game_Files", "game", "assets"}) {
        if (is_valid_game_root(dir / sub)) {
          paths.game_data_root = dir / sub;
          return;
        }
      }
      if (is_valid_game_root(dir)) {
        paths.game_data_root = dir;
        return;
      }
    }

    // Nothing found: keep the historical default so IsGameInstalled fails there
    // and OnFinalizePaths runs the ISO install wizard into it.
    paths.game_data_root = root / "assets";
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig paths = defaults;

    if (!larecomp::IsGameInstalled(paths.game_data_root)) {
      rex::PathConfig installed_paths;
      if (!larecomp::RunRexglueIsoInstallWizardBlocking(app_context(), window(), imgui_drawer(), paths, installed_paths)) {
        std::_Exit(1);
      }
      paths = installed_paths;
    }

    // First launch with no save: offer to import one from Xenia / RPCS3.
    // Skipping is fine -- the game creates a new save on its own.
    if (!larecomp::SaveAlreadyPresent(paths.user_data_root)) {
      larecomp::RunSaveImportWizardBlocking(app_context(), window(), imgui_drawer(), paths);
    }

    return paths;
  }

  void OnPostLoadXexImage() override {
    // Load achievement metadata from the resource embedded in the exe instead of
    // a loose assets/achievements.toml. Runs after the SDK has loaded the base
    // achievements from the title's XDBF, so this layers labels/descriptions on
    // top by matching achievement id.
#ifdef _WIN32
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceA(mod, "ACHIEVEMENTS_TOML", RT_RCDATA);
    if (res) {
      HGLOBAL handle = LoadResource(mod, res);
      DWORD size = SizeofResource(mod, res);
      const char* bytes = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
      if (bytes && size) {
        achievements().LoadMetadataString(std::string_view(bytes, size),
                                          "<embedded achievements.toml>");
      } else {
        LARECOMP_APP_ERROR("Embedded achievements.toml resource is empty");
      }
    } else {
      LARECOMP_APP_ERROR("Embedded achievements.toml resource not found");
    }
#endif
  }

  void OnPostSetup() override {
    LARECOMP_APP_INFO("by @mzzvxm. base memory: 0x{:016X}",
                      reinterpret_cast<std::uintptr_t>(g_guest_mem));

  window()->SetTitle("LARecomp");

    std::filesystem::path src_dir = std::filesystem::path(__FILE__).parent_path();
    std::string icon_path = (src_dir / "assets" / "mcla.ico").string();
    
    void* native_hwnd = window()->GetNativeWindowHandle();
    if (native_hwnd) {
#ifdef _WIN32
      HICON hIcon = (HICON)LoadImageA(NULL, icon_path.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
      if (hIcon) {
        SendMessageA((HWND)native_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageA((HWND)native_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
      } else {
        LARECOMP_APP_ERROR("Failed to load icon file with LoadImageA!");
      }
#endif
    } else {
      LARECOMP_APP_ERROR("Failed to get native window handle!");
    }

    LARECOMP_Discord_Init();
    mc::ui::InitGraphicsButtonPatch();
    InitPauseMenuHooks();
    InitStringTableTools();
    InitHooks();
  }
};
