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

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/graphics/flags.h>
#include <rex/ui/flags.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

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

  static void SetFlag(const char* name, const char* value) {
    rex::cvar::SetFlagByName(name, value);
  }

  void DumpEffectiveConfig() {
    static const char* kWatched[] = {
        "video_mode_refresh_rate", "video_mode_width", "video_mode_height",
        "vsync", "fullscreen", "window_width", "window_height",
        "resolution_scale", "async_shader_compilation", "clear_memory_page_state",
        "d3d12_bindless", "d3d12_readback_resolve", "readback_resolve",
        "readback_memexport_fast", "d3d12_pipeline_creation_threads",
        "d3d12_allow_variable_refresh_rate_and_tearing", "d3d12_tiled_shared_memory",
        "render_target_path_d3d12", "texture_cache_memory_limit_soft",
        "texture_cache_memory_limit_hard",
        "texture_cache_memory_limit_render_to_texture",
        "anisotropic_override", "gpu_allow_invalid_fetch_constants", "log_level",
        "audio_maxqframes", "audio_mute",
    };
    std::filesystem::create_directories("logs");
    if (FILE* f = fopen("logs/effective_config.txt", "w")) {
      fprintf(f, "=== effective cvar values (sampled in OnPostSetup, after all flags applied) ===\n");
      for (const char* name : kWatched) {
        std::string v = rex::cvar::GetFlagByName(name);
        fprintf(f, "%-46s = %s\n", name, v.empty() ? "<empty/unset>" : v.c_str());
      }
      fprintf(f, "\n=== env overrides ===\n");
      for (const char* e : {"MCLA_REFRESH_RATE", "MCLA_MAX_FRAME_MS",
                            "MCLA_TIMING_LOG", "MCLA_NO_TIMER_RES", "MCLA_VSYNC", "MCLA_PRESENT_INTERVAL", "MCLA_FPS_CAP",
                            "MCLA_ALLOW_INVALID_FETCH", "MCLA_SUBSTEPS",
                            "MCLA_RT_PATH", "MCLA_RESOLUTION_SCALE",
                            "MCLA_TEX_SOFT", "MCLA_TEX_HARD", "MCLA_TEX_RTT", "MCLA_TILED_SHARED",
                            "MCLA_SKIP_INTRO", "MCLA_LOD_CITY_SCALE",
                            "MCLA_CAMERA_SMOOTH_SCALE",
                            "MCLA_TRAFFIC_DENSITY_SCALE", "MCLA_PED_DENSITY_SCALE",
                            "MCLA_PARKED_CAR_SCALE", "MCLA_TRAFFIC_UNSPAWN_MAX",
                            "MCLA_DISABLE_DOF", "MCLA_DISABLE_MSAA",
                            "MCLA_DISABLE_MOTION_BLUR", "MCLA_DISABLE_IMPOSTER_SHADOWS",
                            "MCLA_RESOLVE_SYMBOLS", "MCLA_GAME_DATA",
                            "MCLA_STRINGS_FILE", "MCLA_NO_STUB_SWEEP", "MCLA_CACHE_FENCE",
                            "MCLA_AUDIO_QFRAMES",
                            "REX_LOG_LEVEL", "LARECOMP_LOG_FILE"}) {
        const char* v = getenv(e);
        fprintf(f, "%-46s = %s\n", e, v ? v : "<not set>");
      }
      fclose(f);
    }
  }

  void ApplyGpuFlags() {
    // BadassBaboon's Recomp Adjustments: Increased texture cache limits (1536MB soft / 2048MB hard / 64MB RTT)
    // to prevent premature eviction of CTX1 normal maps and sector texture dictionaries during high-speed driving.
    const char* tex_soft = getenv("MCLA_TEX_SOFT");
    SetFlag("texture_cache_memory_limit_soft", (tex_soft && *tex_soft) ? tex_soft : "1536");

    const char* tex_hard = getenv("MCLA_TEX_HARD");
    SetFlag("texture_cache_memory_limit_hard", (tex_hard && *tex_hard) ? tex_hard : "2048");

    const char* tex_rtt = getenv("MCLA_TEX_RTT");
    SetFlag("texture_cache_memory_limit_render_to_texture", (tex_rtt && *tex_rtt) ? tex_rtt : "64");

    // anisotropic_override: 5 = 16x
    SetFlag("anisotropic_override", "5");

    SetFlag("async_shader_compilation", "true");
    SetFlag("d3d12_bindless", "true");
    SetFlag("d3d12_readback_resolve", "false");
    SetFlag("readback_memexport_fast", "true");

    const char* fetch = getenv("MCLA_ALLOW_INVALID_FETCH");
    SetFlag("gpu_allow_invalid_fetch_constants", (fetch && *fetch) ? fetch : "true");

    // BadassBaboon's Recomp Adjustments: vsync is false by default for maximum throughput (~30% higher framerate, eliminating 15.625ms quantization grid).
    const char* vs = getenv("MCLA_VSYNC");
    SetFlag("vsync", (vs && *vs) ? vs : "false");

    const char* rr = getenv("MCLA_REFRESH_RATE");
    SetFlag("video_mode_refresh_rate", (rr && *rr) ? rr : "60");
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The Xenos GPU emulation is a runtime-loaded plugin as of SDK 0.9.0 and
    // the gpu_plugin cvar defaults to empty (= no GPU at all). Name it here so
    // the game works without anything in larecomp.toml; an explicit cvar still
    // wins because this only fills in the blank.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }

    SetFlag("d3d12_allow_variable_refresh_rate_and_tearing", "true");
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
    // BadassBaboon's Recomp Adjustments: Enable 1ms timer resolution and apply optimal GPU/Texture limits
    mc::EnableHighResTimer();
    ApplyGpuFlags();
    DumpEffectiveConfig();

    // Register t: drive - game uses it for city/art/collision data (.loc files etc.)
    if (auto* rt = rex::Runtime::instance()) {
      if (auto* fs = rt->file_system()) {
        fs->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
      }
    }

    auto* fd = rex::Runtime::instance()->function_dispatcher();
    uint8_t* base = rex::Runtime::instance()->virtual_membase();

    if (fd && base) {
      // Deduplicating stub safety net logger
      struct StubEntry {
        uint32_t r3, r4, r5, r6;
        std::atomic<uint32_t> count{0};
      };
      static FILE* stub_log = fopen("stubs.txt", "w");
      static std::mutex stub_mutex;
      static std::unordered_map<uint64_t, StubEntry> stub_map;

      static PPCFunc* stub = [](PPCContext& ctx, uint8_t*) noexcept {
        uint32_t addr = ctx.ctr.u32;
        uint32_t lr   = ctx.lr;
        uint64_t key  = (uint64_t(addr) << 32) | lr;

        std::lock_guard<std::mutex> lock(stub_mutex);
        auto [it, inserted] = stub_map.emplace(
            std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());

        if (inserted) {
          it->second.r3 = ctx.r3.u32;
          it->second.r4 = ctx.r4.u32;
          it->second.r5 = ctx.r5.u32;
          it->second.r6 = ctx.r6.u32;
          it->second.count.store(1);
          if (stub_log) {
            fprintf(stub_log,
                    "[stub] addr=0x%08X LR=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                    addr, lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
            fflush(stub_log);
          }
        } else {
          uint32_t n = it->second.count.fetch_add(1) + 1;
          if ((n & (n - 1)) == 0 && stub_log) {
            fprintf(stub_log,
                    "[stub] addr=0x%08X LR=0x%08X  (x%u)\n", addr, lr, n);
            fflush(stub_log);
          }
        }
      };

      // Pass 1: scan static initializer tables (base = 0x82770000).
      static constexpr uint32_t kTables[][2] = {
          {0x82770010, 0x827713E0},
          {0x827713E4, 0x827713F0},
      };
      static constexpr uint32_t kXexBase = 0x82000000;
      static constexpr uint32_t kXexEnd  = 0x829E0000;

      for (auto& [start, end] : kTables) {
        for (uint32_t addr = start; addr < end; addr += 4) {
          uint32_t fn = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr));
          if (fn < kXexBase || fn >= kXexEnd) continue;
          if (!fd->GetFunction(fn)) {
            fd->SetFunction(fn, stub);
          }
        }
      }

      // Bypass disc error handler: sub_82130678
      static PPCFunc* disc_error_bypass = [](PPCContext&, uint8_t*) noexcept {};
      fd->SetFunction(0x82130678, disc_error_bypass);

      // Pass 2: walk the full XEX code region and stub unmapped targets
      if (const char* e = getenv("MCLA_NO_STUB_SWEEP"); !(e && *e == '1')) {
        static constexpr uint32_t kCodeBase = 0x82130000;
        static constexpr uint32_t kCodeEnd  = 0x827CD054;

        auto t0 = std::chrono::steady_clock::now();
        uint32_t stubbed = 0;
        for (uint32_t addr = kCodeBase; addr < kCodeEnd; addr += 4) {
          if (!fd->GetFunction(addr)) {
            fd->SetFunction(addr, stub);
            ++stubbed;
          }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();

        if (FILE* f = fopen("logs/effective_config.txt", "a")) {
          fprintf(f, "\n=== stub sweep ===\n");
          fprintf(f, "scanned %u addresses, stubbed %u, took %lld ms\n",
                  (kCodeEnd - kCodeBase) / 4, stubbed, (long long)ms);
          fclose(f);
        }
      }
    }

    LARECOMP_APP_INFO("by @mzzvxm. base memory: 0x{:016X}",
                      reinterpret_cast<std::uintptr_t>(g_guest_mem));

    window()->SetTitle("LARecomp (60 FPS Enhanced)");

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
