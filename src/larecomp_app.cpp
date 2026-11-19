// LarecompApp on rexglue 0.11. See larecomp_app.h for the hook-by-hook map from the 0.10 ReXApp.
// Not compiled yet: each `CHECK:` is a question only the build or a run can answer.

#include "larecomp_app.h"

// The generated image of the game (generated/larecomp_init.cpp). Declared here as that file does instead of including the
// whole generated surface (larecomp_init.h pulls in every recompiled function's declaration).
#include <rex/ppc/image.h>
extern const rex::ppc::PPCImage kPPCImageConfig;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include <rex/app/app.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_exit.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/path_settings.h>
#include <rex/platform/file.h>
#include <rex/platform/process.h>
#include <rex/ui/dialog.h>
#include <rex/xam/achievement_manager.h>

#include "settings/cli_rules.h"
#include "settings/larecomp_settings.h"

#include "achievement_metadata.h"
#include "bootprogress/larecomp_boot_progress.h"
#include "bootprogress/larecomp_pipeline_build.h"
#include "bootprogress/larecomp_shader_build.h"
#include "cpu_level.h"
#include "crash_handler.h"
#include "discord_rpc/discord_rpc.h"
#include "game_marker.h"
#include "isoinstaller/larecomp_iso_installer.h"
#include "larecomp_log.h"
#include "mc_engine/cinematic/capture_audio.h"
#include "mc_engine/core/logging.h"
#include "mc_engine/input/adaptive_triggers.h"
#include "mc_engine/music/audio_output.h"
#include "process_exit.h"
#include "mc_engine/core/config_save.h"
#include "mc_engine/menus/debug_menu.h"
#include "mc_engine/input/device_watch.h"
#include "mc_engine/core/guest_profiler.h"
#include "mc_engine/display/display_mode.h"
#include "mc_engine/hooks/hooks.h"
#include "mc_engine/input/impulse_triggers.h"
#include "mc_engine/input/map_mouse.h"
#include "mc_engine/gameplay/map_warp.h"
#include "mc_engine/modloader/modsbrowser/mod_browser_ui.h"
#include "mc_engine/sdk_access.h"
#include "mc_engine/ui/overlay_support.h"
#include "mc_engine/ui_events.h"
#include "mc_engine/modloader/modloader.h"
#include "mc_engine/online/identity/identity_store.h"
#include "mc_engine/online/identity/identity_ui.h"
#include "mc_engine/online/online_toast.h"
#include "mc_engine/online/server_browser/browser_ui.h"
#include "mc_engine/menus/pause_menu.h"
#include "mc_engine/core/restart.h"
#include "mc_engine/core/save_protect.h"
#include "mc_engine/menus/string_table.h"
#include "mc_engine/core/threading.h"
#include "mc_engine/ui/achievements.h"
#include "mc_engine/ui/controller_toast.h"
#include "mc_engine/ui/debug_overlay.h"
#include "mc_engine/ui/imgui_theme.h"
#include "mc_engine/ui/settings.h"
#include "mc_engine/display/ultrawide.h"
#include "mc_engine/wheel/wheel_input.h"
#include "native_gfx/nocp/nocp_app.h"
#include "native_gfx/nocp/nocp_driver.h"
#include "os/thread_priority.h"
#include "profile/larecomp_profile_picker.h"
#include "saveporter/larecomp_save_porter.h"
#include "spdlog_console.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#include "gpu/native_window.h"
#endif
#include "mc_engine/display/effect_compare.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>  // DedicatedVideoMemory, to size the texture cache against the real adapter
#endif

extern uint8_t* g_guest_mem;

namespace ls = larecomp::settings;

namespace {

constexpr rex::CLIAction kActions[] = {
    REX_CLI_ACTION(build_shader_packs, "Builds the shader packs from the game files and exits (the launcher's setup)."),
    REX_CLI_ACTION(precache_pipelines, "Fills the graphics driver's pipeline cache from the packs and exits."),
};

// Dedicated VRAM of the adapter the game will run on, in MiB, or 0. Used to size the texture cache (the limits are what
// the cache may hold before it evicts: above the card's VRAM it never evicts and WDDM demotes to system memory).
uint32_t DetectVideoMemoryMB() {
#if defined(_WIN32)
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return 0;
  }
  uint64_t best = 0;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      best = std::max<uint64_t>(best, desc.DedicatedVideoMemory);
    }
    adapter->Release();
    adapter = nullptr;
  }
  factory->Release();
  return static_cast<uint32_t>(best / (1024ull * 1024ull));
#else
  return 0;
#endif
}

// Whether a file can be created in `dir` (made if missing). Controlled Folder Access lets unknown programs read
// Documents but not write there; only a real write shows it.
bool CanWriteTo(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) return false;
  const std::filesystem::path probe = dir / ".larecomp_write_test";
  {
    std::ofstream f(probe, std::ios::binary | std::ios::trunc);
    if (!f || !(f << 'x') || !f.flush()) return false;
  }
  std::filesystem::remove(probe, ec);
  return true;
}

// %LOCALAPPDATA%\larecomp: where the identity key already lives, and a place that protection leaves writable.
std::filesystem::path LocalUserFolder() {
#if defined(_WIN32)
  if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local) {
    return std::filesystem::path(local) / L"larecomp";
  }
#endif
  return {};
}

// Linux: where the graphics driver keeps the pipelines it compiled (see the 0.10 comment: Steam gives each program a
// cache folder of its own, so the setup run and the game must be pointed at the same one).
void PinDriverShaderCache(const std::filesystem::path& cache_root) {
#if !defined(_WIN32) && !defined(__APPLE__)
  const auto set = [](const char* name, const char* value) {
    const char* now = std::getenv(name);
    const bool by_steam = std::getenv("SteamGameId") || std::getenv("SteamAppId");
    if (now && *now && !by_steam) return;  // the player's own choice
    setenv(name, value, 1);
  };
  std::error_code ec;
  const std::filesystem::path folder = cache_root / "driver";
  std::filesystem::create_directories(folder, ec);
  if (ec) return;
  const std::string text = folder.string();
  set("MESA_SHADER_CACHE_DIR", text.c_str());
  set("MESA_GLSL_CACHE_DIR", text.c_str());
  set("__GL_SHADER_DISK_CACHE_PATH", text.c_str());
  set("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1");
#else
  (void)cache_root;
#endif
}

// A valid game root has default.xex plus one of the archives: the ISO installer writes the xex first and the archives
// after, so a half-finished install must not be accepted and then fail at boot.
bool IsValidGameRoot(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::exists(p / "default.xex", ec)) return false;
  return std::filesystem::exists(p / "xarchive_cache.rpf", ec) || std::filesystem::exists(p / "xarchive_audio.rpf", ec) ||
         std::filesystem::exists(p / "xarchive_audlo.rpf", ec);
}

// The game files: MCLA_GAME_DATA, the folder the installer remembered, then a walk up from the exe (MCLA_Game_Files/,
// game/, assets/, the folder itself). Empty when nothing is found (the installer wizard then runs).
std::filesystem::path FindGameRoot(const std::filesystem::path& user_root, std::vector<std::string>& notes) {
  if (const char* env = std::getenv("MCLA_GAME_DATA"); env && *env) {
    std::filesystem::path p(env);
    if (IsValidGameRoot(p)) return p;
    notes.push_back(std::string("MCLA_GAME_DATA is set to '") + env +
                    "' but that folder has no default.xex plus xarchive_*.rpf; ignoring it");
  }
  if (const auto remembered = larecomp::RememberedGameInstall(user_root); !remembered.empty() && IsValidGameRoot(remembered)) {
    return remembered;
  }
  const auto exe = rex::platform::GetExecutableFolder();
  for (std::filesystem::path dir = exe; !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
    for (const char* sub : {"MCLA_Game_Files", "game", "assets"}) {
      if (IsValidGameRoot(dir / sub)) return dir / sub;
    }
    if (IsValidGameRoot(dir)) return dir;
  }
  notes.push_back("no game data found from the executable folder (" + exe.string() + ")");
  return {};
}

}  // namespace

// -----------------------------------------------------------------------------------------------------------------
LarecompApp::LarecompApp() : TitleApp("larecomp", kPPCImageConfig) {
  // First of all: the named object that says a game is running (game_marker.h). The launcher checks it instead of
  // listing processes. Before the restart wait on purpose: a restarted game holds it beside the old one, so the
  // launcher never sees a gap.
  larecomp::marker::Create();
  // Started by RESTART GAME: wait for the old process to be gone before this one opens its log, reads the settings
  // or touches the saves (restart.h). CHECK: rex::app::RequestRestart() can replace mcla::RelaunchGame (the SDK relaunches
  // with the same arguments); does it wait for the old process before the new one reads the config? If so this goes.
  mcla::WaitForRestartedGame();
  // CHECK (L3): LARecomp's own spdlog console/file sink (spdlog_console.cpp) is the SDK's rex::Logger now
  // (ILogger::AddSink, log.* settings); InitLarecompLogging has to become an AddSink in OnInit.
  InitLarecompLogging();
  LARECOMP_APP_INFO("LA Recompiled - @by mzzvxm");
  LARECOMP_APP_INFO("[cpu] built for {}; CPU: {}", larecomp::cpu::BuiltLevel(), larecomp::cpu::DescribeFeatures());
  LARECOMP_APP_INFO("[marker] {}", larecomp::marker::Describe());
  InstallCrashLogger();
}

LarecompApp::~LarecompApp() = default;

// The systems the program needs: TitleApp's (Xenos graphics family, audio, input, UI); none for the batch actions,
// which run before any window, renderer or runtime exists (the 0.10 SetupEnvironment did the same by exiting early).
rex::AppSysRequirements LarecompApp::GetRequirements() const {
  if (batch_ != BatchMode::kNone) {
    return {};
  }
  return TitleApp::GetRequirements();
}

std::span<const rex::CLIAction> LarecompApp::GetActions() const { return kActions; }

rex::Result<void> LarecompApp::OnAction(const rex::CLIActionHit& hit) {
  // main scanned the arguments already (SetBatchMode) so GetRequirements() was right; nothing else to do here.
  (void)hit;
  return rex::Ok();
}

// The command line the SDK gets: its actions, its path flags and the positionals. Every other option is a SETTING option and
// stays here for OnConfigure, so that one the SDK would stop the start for (see settings/cli_rules.h) costs a log line instead.
rex::CLIArgList LarecompApp::PrepareCommandLine(const rex::CLIArgList& args) {
  std::vector<larecomp::cli::Option> options;
  options.reserve(args.GetOptions().size());
  for (const rex::CLIOption& option : args.GetOptions()) {
    options.push_back({option.name, option.value});
  }
  std::vector<std::string_view> actions;
  for (const rex::CLIAction& action : kActions) {
    actions.push_back(action.name);
  }
  larecomp::cli::Split split = larecomp::cli::SplitCommandLine(options, args.GetPositionals(), actions);
  cli_options_ = std::move(split.settings);
  cli_notes_ = std::move(split.notes);
  return rex::CLIArgList::Parse(std::span<const std::string>(split.sdk));
}

void LarecompApp::OnModules(std::vector<std::string>& modules) {
  TitleApp::OnModules(modules);
  // apu.driver is "larecomp" (below): nothing would ask for the SDL3 audio module, which the audio driver of
  // mc_engine/audio_output.h wraps. The batch actions load no audio at all.
  if (batch_ == BatchMode::kNone) {
    modules.push_back(mcla::audio::kInnerModuleName);
  }
}

void LarecompApp::OnRegisterModules(std::vector<rex::IModule*>& modules) {
  // The no-command-processor driver, selected by gpu.driver = larecomp (nocp_driver.h).
  modules.push_back(&mcla::native_gfx::nocp::Module());
  // The audio driver that never fails to open, selected by apu.driver = larecomp (audio_output.h).
  modules.push_back(&mcla::audio::Module());
}

// -----------------------------------------------------------------------------------------------------------------
// OnConfigure runs after the modules connect and BEFORE the config file, the environment and the command line apply.
// So everything set here is the TITLE layer: LARecomp's own default, which a value from any of those layers overrides.
// That replaces the 0.10 FlagWasConfigured/NoteStartupCvar bookkeeping.
rex::Result<void> LarecompApp::OnConfigure(rex::SettingsRegistry& settings) {
  if (auto* logger = rex::AppInterfaces::Get<rex::ILogger>()) {
    static constexpr std::array<const rex::log::LogCategory*, 3> kCategories = {&larecomp::log::App, &larecomp::log::Crash,
                                                                                &mc::log::mc};
    (void)logger->AddCategories(kCategories);
  }
  REX_TRY_VOID(ls::RegisterAll(settings));  // the owners and the aliases of the old flat names
  ls::ApplyLegacyEnvironment();             // REX_DEBUG_MENU, REX_MCLA_GPU_AO ... as 0.10 spelled them

  // Defaults first (title layer), then the player's file: the file's migrated values (renderer, resolution_scale,
  // input_backend) are written at the same layer, and the last write wins.
  ApplyStartupDefaults(settings);
  ChooseUserFolder();

  // The player's larecomp.toml, key by key (a refused value costs only its own line: see larecomp_settings.h) and with the
  // values whose meaning changed converted. The command line is not applied yet; main hands over a --config_file it saw.
  const std::filesystem::path config =
      config_file_hint_.empty() ? rex::GetPathSettings().config_file.Get() : config_file_hint_;
  ls::ApplyPlayerConfig(settings, config);

  // The setting options of the command line, at the layer the SDK would have used (it parses the rest, which has none of them).
  ApplyCommandLineSettings(settings);
  return rex::Ok();
}

// What the SDK does with a command line, minus its habit of stopping the start: each setting option goes to the registry at the
// command line layer (the same call, the same alias handling, the same precedence: the command line beats the file and the
// environment whatever the order), except the ones settings/cli_rules.h says cannot apply, which become one note each:
//   * a setting of a driver this run does not load (--d3d12_adapter=0 with gpu.driver = larecomp): judged by the four selector
//     settings as the SDK will see them (command line, then environment, then what the defaults and larecomp.toml gave);
//   * a name that is not a setting (no dot, not an alias); a --no- of a setting that is not a bool; a changed-meaning flag with no
//     usable value;
//   * a value the registry refuses (the setting keeps what it had).
// CHECK: a dotted name that no loaded module registers by the end of OnInit (a typo under a driver that is loaded) is still
// "unknown setting ... from CommandLine" in the SDK's App::ReportUnknownSettings and stops the start; only an SDK change closes that.
void LarecompApp::ApplyCommandLineSettings(rex::SettingsRegistry& settings) {
  if (cli_options_.empty()) return;
  namespace cli = larecomp::cli;

  const auto selector = [&](const char* name, const char* variable) {
    std::string current;
    if (auto found = settings.Find(name)) current = (*found)->GetText();
    return cli::EffectiveValue(cli::LastValueOf(cli_options_, name), rex::platform::GetEnvironmentValue(variable), current);
  };
  cli::Selection selection;
  selection.gpu = selector("gpu.driver", "REX_GPU_DRIVER");
  selection.apu = selector("apu.driver", "REX_APU_DRIVER");
  selection.hid = selector("hid.drivers", "REX_HID_DRIVERS");
  selection.ui = selector("ui.driver", "REX_UI_DRIVER");
#if defined(_WIN32)
  selection.windows = true;
#else
  selection.windows = false;
#endif

  struct RegistryLookup {
    rex::SettingsRegistry& registry;
    bool is_alias(std::string_view name) const { return registry.IsAlias(name); }
    std::string full_name(std::string_view name) const { return ls::ResolveName(name); }
    std::optional<bool> registered_is_bool(std::string_view full) const {
      auto found = registry.Find(full);
      if (!found) return std::nullopt;
      return (*found)->GetType() == rex::SettingType::kBool;
    }
  };
  const RegistryLookup lookup{settings};

  std::vector<std::string> candidates;  // for "did you mean": built on the first name nothing knows
  for (const cli::Option& option : cli_options_) {
    const cli::Decision decision = cli::Decide(option, selection, lookup);
    if (decision.verdict != cli::Verdict::kApply) {
      std::string note = decision.note;
      if (decision.verdict == cli::Verdict::kUnknown) {
        std::string typed = option.name;
        if (typed.starts_with("no-")) typed.erase(0, 3);
        if (candidates.empty()) {
          for (rex::SettingBase* setting : ls::AllSettings()) candidates.push_back(ls::FlatName(*setting));
        }
        if (const std::string closest = cli::ClosestName(typed, candidates); !closest.empty()) {
          note += " (did you mean " + closest + "?)";
        }
      }
      cli_notes_.push_back(std::move(note));
      continue;
    }
    for (const cli::Assignment& assignment : decision.set) {
      if (auto set = settings.SetText(rex::SettingLayer::kCommandLine, assignment.name, assignment.value); !set) {
        cli_notes_.push_back(cli::Spell(option) + " ignored: " + set.error().GetDescription());
      }
    }
  }
}

void LarecompApp::ApplyStartupDefaults(rex::SettingsRegistry&) {
  const auto def = [](const char* name, const std::string& value) {
    if (!ls::SetDefault(name, value)) {
      LARECOMP_APP_WARN("[settings] default {} = {} was not accepted (name not known yet or value refused)", name, value);
    }
  };

  // --build_shader_packs / --precache_pipelines: no window, no sound, no pad, no graphics driver (GetRequirements() asks
  // for nothing, but the driver settings choose what rex::Init loads: "none" for each). Read from the SDK: ui.driver, apu.driver
  // ("none for silence in an app that does not require audio"), hid.drivers (a comma list, "none" skipped) and gpu.driver
  // ("none for no driver in an app that does not require one") all take "none" exactly when GetRequirements() does not ask for the
  // system (AudioModule::Init fails with "no audio driver is available" only when it does); DetectHeadless() reads ui.driver.
  if (batch_ != BatchMode::kNone) {
    def("ui.driver", "none");
    def("apu.driver", "none");
    def("hid.drivers", "none");
    def("gpu.driver", "none");
    return;
  }

  // The audio output: the SDK's apu.driver. "larecomp" wraps the SDL3 driver and turns a device that does not open (or an SDL audio
  // system that does not start) into the SDK's own silent mix (audio_output.h); mcla_audio_output = null forces the silence.
  def("apu.driver", std::string(mcla::audio::kDriverName));

  // The renderer: the SDK's gpu.driver. Plume with no emulated GPU is the default; a player (or the pause menu's
  // RENDERER row, or MigrateLegacyConfig) picks the SDK's Xenia GPU with any / d3d12 / vulkan.
  def("gpu.driver", std::string(mcla::native_gfx::nocp::kDriverName));

  // vsync false by default for maximum throughput (about 30% higher frame rate, no 15.625 ms quantization grid);
  // MCLA_VSYNC / MCLA_REFRESH_RATE still override as before.
  const char* vs = std::getenv("MCLA_VSYNC");
  def("gpu.vsync", (vs && *vs) ? vs : "false");
  const char* rr = std::getenv("MCLA_REFRESH_RATE");
  def("ui.video_mode_refresh_rate", (rr && *rr) ? rr : "60");

  // Texture cache limits from the adapter's VRAM (a flat 2560/4096 is the whole of a 4 GB card): half the VRAM for the
  // hard ceiling, three quarters of that for the soft one, floored at the SDK stock and capped at 2560/4096.
  const uint32_t vram_mb = DetectVideoMemoryMB();
  const uint32_t hard_mb = vram_mb ? std::clamp<uint32_t>(vram_mb / 2, 768, 4096) : 2048;
  const uint32_t soft_mb = std::clamp<uint32_t>(hard_mb * 3 / 4, 384, 2560);
  MC_INFO("[gpu] adapter VRAM {} MB -> texture cache soft {} MB / hard {} MB", vram_mb, soft_mb, hard_mb);
  const auto env_or = [](const char* var, uint32_t fallback) {
    const char* v = std::getenv(var);
    return (v && *v) ? std::string(v) : std::to_string(fallback);
  };
  def("gpu.xenos.texture_cache_memory_limit_soft", env_or("MCLA_TEX_SOFT", soft_mb));
  def("gpu.xenos.texture_cache_memory_limit_hard", env_or("MCLA_TEX_HARD", hard_mb));
  def("gpu.xenos.texture_cache_memory_limit_render_to_texture", env_or("MCLA_TEX_RTT", 64));
  def("gpu.anisotropic_override", "5");  // 16x unless larecomp.toml says otherwise

  // The Xenia GPU's own settings (the XENIA renderer only; a name whose owner is not loaded waits and is dropped).
  def("gpu.xenos.async_shader_compilation", "true");
  def("gpu.d3d12.bindless", "true");
  def("gpu.d3d12.readback_resolve", "false");
  def("gpu.vulkan.readback_resolve", "false");  // opened only around vinyls and photos (hooks/readback_window.cpp)
  def("gpu.xenos.readback_memexport_fast", "true");
  def("gpu.d3d12.allow_tearing", "true");
  const char* fetch = std::getenv("MCLA_ALLOW_INVALID_FETCH");
  def("gpu.xenos.allow_invalid_fetch_constants", (fetch && *fetch) ? fetch : "true");
  // The old submit_on_primary_buffer_end / vsync_fast_poll copies: gpu_vsync_fast_poll was never defined anywhere.
  def("gpu.d3d12.submit_on_primary_buffer_end", ls::submit_on_primary_buffer_end.Get() ? "true" : "false");
  SetXeniaHostDefaults();

  // The SDK's keyboard-and-mouse emulation is retired as a player path: the native keyboard (mc_engine/keyboard_native.cpp)
  // is always on, and with both on a key would be a pad button AND a native key. CHECK: the layer a title default has
  // against --hid.mnk_mode=true on the command line (the command line wins now; the old code forced it off).
  def("hid.mnk_mode", "false");
  def("hid.mnk_mouse", "false");
  // hid.drivers stays at its default "sdl3" on every platform: the 0.10 default input_backend was "sdl" everywhere too
  // (names read from the SDK: src/hid/sdl3 registers "sdl3", src/hid/xinput registers "xinput"; ApplyPlayerConfig maps a
  // player's old input_backend line). A 'controller_toast_enable' default is gone: it was never defined (a silent FAIL in
  // 0.10), and the SDK's controller toast is not part of the title overlay set LARecomp adds.
  // The language choice is applied in OnInit (InitLanguageSetting): it needs the player's `language` line, which is not
  // read yet here.
}

// The emulated GPU's memory and CPU settings for this game (the XENIA renderer), unless the player set them.
void LarecompApp::SetXeniaHostDefaults() {
  static const char* const kDefaults[][2] = {
      {"gpu.xenos.render_target_max_height", "1024"},
      {"gpu.xenos.render_target_trim_seconds", "60"},
      {"gpu.xenos.memory_pressure_percent", "90"},
      {"gpu.xenos.shared_memory_hot_page_faults", "4"},
      {"gpu.xenos.shared_memory_hot_page_ms", "10000"},
      {"gpu.d3d12.hoist_shared_memory_uploads", "true"},
      // the resolve rules: custom vinyls baked into a 512x512 8_8_8_8 target, and the rain's four 128x128 32_32_FLOAT targets above 1x
      {"gpu.xenos.readback_resolve_rules", "len=1048576,pitch=512,height=512,fmt=6;addr=0x051A0000-0x051F7FFF"},
      {"gpu.xenos.resolve_unscaled_copy_rules", "len=131072,pitch=128,height=128,fmt=37"},
  };
  for (const auto& d : kDefaults) {
    (void)ls::SetDefault(d[0], d[1]);
  }
#if defined(LARECOMP_HAS_GPU)
  // Which GPU the emulated GPU runs on: the number of the GPU the Plume renderer would use (gpu.h XeniaAdapterIndex),
  // for both APIs, since the API is only known after the layers apply.
  const std::string adapter_name = ls::mcla_gpu_adapter.Get();
  if (const int d3d12 = mcla::gpu::XeniaAdapterIndex(false, adapter_name); d3d12 >= 0) {
    (void)ls::SetDefault("gpu.d3d12.adapter", std::to_string(d3d12));
  }
  if (const int vulkan = mcla::gpu::XeniaAdapterIndex(true, adapter_name); vulkan >= 0) {
    (void)ls::SetDefault("gpu.vulkan.device", std::to_string(vulkan));
  }
#endif
}

// The default of app.user_root, as in 0.10: <Documents>\larecomp (rex::platform::GetUserFolder() is Documents on Windows and the XDG
// data home elsewhere, the folder the old SDK's GetUserFolder() gave), the cache under it. The SDK's own default is
// <app root>\user, which would leave every existing player's saves, identity key, remembered game folder and shader cache behind:
// this is the default of the TITLE layer, so a user_data_root / cache_root the player wrote (file, environment, command line)
// still wins. A cache_root default that names the user folder as {app.user_root} follows a player's own user_data_root.
// When Documents cannot be written (Controlled Folder Access lets unknown programs read Documents but not write there, and the
// SDK's shader storage then throws while creating cache\) the folder is %LOCALAPPDATA%\larecomp instead; a marker keeps that
// choice, so lifting the block later does not split the saves. The copy of what is already in Documents is MoveUserDataIfNeeded.
void LarecompApp::ChooseUserFolder() {
  rex::PathSettings& paths = rex::GetPathSettings();
  const std::filesystem::path user_folder = rex::platform::GetUserFolder();
  const std::filesystem::path documents = user_folder.empty() ? std::filesystem::path() : user_folder / "larecomp";
  const std::filesystem::path fallback = LocalUserFolder();
  std::filesystem::path chosen = documents;
  if (!fallback.empty()) {
    std::error_code ec;
    const bool moved_before = std::filesystem::exists(fallback / ".moved_from_documents", ec);
    if (documents.empty() || moved_before || !CanWriteTo(documents)) {
      if (CanWriteTo(fallback)) {
        chosen = fallback;
      } else {
        path_notes_.push_back("cannot write to '" + documents.string() + "' nor to '" + fallback.string() +
                              "'; saves and the shader cache will fail");
      }
    }
  }
  if (chosen.empty()) {
    return;  // the SDK's default (<app root>\user) stays: nothing better is known on this platform
  }
  (void)paths.user_root.Set(chosen);
  (void)paths.cache_root.SetText(rex::SettingLayer::kTitle, "{app.user_root}/cache");
}

// -----------------------------------------------------------------------------------------------------------------
rex::Result<void> LarecompApp::OnInit() {
  // The window is open, the systems run, app.* is frozen. The order below is the order the 0.10 start-up had.
  settings_config_path_ = rex::GetPathSettings().config_file.Get();
  InstallCrashLoggerAfterRuntime();  // CHECK: the SDK installed its crash reporter in Init; ours goes behind it
  for (const auto& note : path_notes_) LARECOMP_APP_WARN("Game data: {}", note);
  path_notes_.clear();
  // The command line options that could not be used (one line each; the start goes on): ApplyCommandLineSettings, PrepareCommandLine.
  for (const auto& note : cli_notes_) LARECOMP_APP_WARN("[cli] {}", note);
  cli_notes_.clear();

  if (batch_ != BatchMode::kNone) {
    RunBatchMode();  // asks for a shutdown with the exit code; nothing else of the start-up happens
    return rex::Ok();
  }

  MoveUserDataIfNeeded();
  larecomp::shaderbuild::ConfigurePackFolder(rex::GetPathSettings().cache_root.Get());
  PinDriverShaderCache(rex::GetPathSettings().cache_root.Get());

  // log flush: a warning flushed the file at once, so a repeating warning cost a disk write each time (0.10 comment).
  // CHECK: ILogger has no flush_on level: log.flush_interval (hot) covers it.
  larecomp::InitDebugMenu();  // the command line, the environment and larecomp.toml are all read by now
  // The game's language: LARecomp's `language` choice becomes the console's user language before the title launches
  // (kernel.user_language is kUntilLaunch: writable until then). Needs the player's `language` line, so not before OnInit.
  InitLanguageSetting();

  // The game files: when nothing valid is set, look around as the 0.10 start-up did; the wizard handles the rest.
  rex::kernel::TitlePaths& tp = GetTitleRuntime().GetTitlePaths();
  if (!IsValidGameRoot(tp.game_root.Get())) {
    std::vector<std::string> notes;
    if (auto found = FindGameRoot(rex::GetPathSettings().user_root.Get(), notes); !found.empty()) {
      (void)tp.game_root.Set(found);
    } else if (tp.game_root.Get().empty()) {
      (void)tp.game_root.Set(rex::platform::GetExecutableFolder() / "assets");  // the historical default: IsGameInstalled fails there
    }
    for (const auto& n : notes) LARECOMP_APP_WARN("Game data: {}", n);
  }

  larecomp::PrepareProfileChoice(rex::GetPathSettings().user_root.Get());
  larecomp::identity::SetUserDataRoot(rex::GetPathSettings().user_root.Get());
  larecomp::impulse::EarlyInit();  // before anything starts this executable's SDK joystick layer
  larecomp::wheel::EarlyInit();    // before the window starts SDL's joystick layer: keeps wheels out of the gamepad layer

  mc::EnableHighResTimer();
  larecomp::os::SetLogCallback([](const std::string& line) { LARECOMP_APP_INFO("{}", line); });
  MCLA_StartInputProbe();
  larecomp::devwatch::Start();  // which controllers are plugged in (the OS tells us; the first list on a thread of its own)

  StartOverlays();

  ui_listener_ = GetUI().AddEventListener(*this);  // resize, focus, close
  mc::ultrawide::SetWindowSize(GetUI().GetVideoMode().width, GetUI().GetVideoMode().height);  // CHECK: window pixel size, not the video mode

#if defined(LARECOMP_HAS_GPU)
  // The Plume renderer starts BEFORE LaunchTitle: LaunchTitle runs the first-launch profile prompt and the DLC folder
  // install (WP-C3), and both must draw. It takes the window from IUISystem::GetNativeWindow and draws the overlays
  // with IUISystem::DrawOverlays(its IOverlayRenderer) on its own render thread.
  if (mcla::native_gfx::nocp::WantNoCommandProcessor() && mcla::gpu::Enabled()) {
    mcla::compare::Start(GetUI());  // the compare key (F1)
    larecomp::shaderbuild::InstallStartupGate(tp.game_root.Get());
    auto window = GetUI().GetNativeWindow();
    if (!window) {
      return rex::Err(rex::ErrorCategory::kRuntime, "the Plume renderer needs the window and it is not open");
    }
    // CHECK (L5/L6): mcla::gpu::Startup(IUISystem&, const NativeWindow&) is the new signature (was window, app_context, drawer).
    if (!mcla::gpu::Startup(GetUI(), *window)) {
      return rex::Err(rex::ErrorCategory::kRuntime, "the Plume renderer could not start");
    }
  }
#endif

  exit_listener_ = {};  // set in OnTitleLaunch
  BeginSetupStep();
  return rex::Ok();
}

// The window's overlays and hotkeys on rexglue 0.11 (mc_engine/ui/overlay_support.h says how they are written). UI thread, the
// window open, before the renderer starts.
//   * the theme (fonts, style) goes in first: InstallMclaTheme adds the dialog that keeps it (imgui_theme.h);
//   * the SDK's own overlays that sit on the keys of LARecomp's (debug F3, settings F4, achievements F7) are unbound: their
//     bind is the setting ui.bind.<name> and an empty text is "no key". A player's own value for them (the file, the command
//     line) is a higher layer and still wins;
//   * the SDK's console (the grave accent key, ui.bind.console) is a developer tool: only a build with LARECOMP_DEV_TOOLS
//     (the CMake option, off by default) keeps its key. Any other build locks the bind to "no key" at the lock layer, which
//     no file, environment or command-line value can replace, so a player has no console. The overlay itself is still made
//     by the SDK (it registers its standard overlays itself), but nothing can open it;
//   * LARecomp's overlays and hotkeys are OverlayConfig entries: the name makes the setting ui.bind.<name>, which is the old
//     bind_<name> cvar (sdk_names.inc maps the three overlays; the rows for the others are listed in the report of WP-L2B).
void LarecompApp::StartOverlays() {
  rex::ui::IUISystem& ui = GetUI();
  mcla::ui::InstallMclaTheme(ui);

  rex::SettingsRegistry& settings = REX_SETTINGS();
  for (const char* name : {"ui.bind.debug", "ui.bind.settings", "ui.bind.achievements"}) {
    if (auto set = settings.SetText(rex::SettingLayer::kTitle, name, ""); !set) {
      LARECOMP_APP_WARN("[ui] the SDK's key {} could not be freed for LARecomp's overlay: {}", name, set.error().GetDescription());
    }
  }
#if !defined(LARECOMP_DEV_TOOLS)
  if (auto locked = settings.SetText(rex::SettingLayer::kLock, "ui.bind.console", ""); !locked) {
    LARECOMP_APP_WARN("[ui] the SDK's console key could not be removed: {}", locked.error().GetDescription());
  }
#endif

  mcla::ui::SetSettingsConfigPath(settings_config_path_);
  using Hotkey = std::unique_ptr<rex::ui::IDialog> (*)(rex::ui::IUISystem&);
  std::vector<rex::ui::OverlayConfig> overlays;
  overlays.push_back(mcla::ui::DebugOverlayConfig());                                                  // F3: telemetry card
  overlays.push_back(mcla::ui::SettingsOverlayConfig());                                               // F4: settings
  overlays.push_back(mcla::ui::AchievementsOverlayConfig());                                           // F7: achievement list
  overlays.push_back({"mod_browser", {rex::VirtualKey::kF5}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&larecomp::ModBrowserKeyPressed>)});
  overlays.push_back({"server_browser", {rex::VirtualKey::kF6}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&larecomp::ServerBrowserKeyPressed>)});
  overlays.push_back({"identity", {rex::VirtualKey::kF8}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&larecomp::IdentityKeyPressed>)});
  overlays.push_back({"map_warp", {rex::VirtualKey::kF9}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&MapWarpKeyPressed>)});
#if defined(LARECOMP_HAS_GPU)
  // The keys of the Plume renderer (they do nothing without it): F2 texture dump, F10 raw frame trace, F11 RenderDoc capture.
  if (mcla::native_gfx::nocp::WantNoCommandProcessor() && mcla::gpu::Enabled()) {
    overlays.push_back({"gpu_texture_dump", {rex::VirtualKey::kF2}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&mcla::gpu::RequestTextureDump>)});
    overlays.push_back({"gpu_trace_next", {rex::VirtualKey::kF10}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&mcla::gpu::RequestFrameTrace>)});
    overlays.push_back({"gpu_renderdoc_capture", {rex::VirtualKey::kF11}, static_cast<Hotkey>(&mcla::ui::MakeHotkey<&mcla::gpu::RequestRenderDocCapture>)});
    overlays.push_back(mcla::compare::KeyOverlayConfig());                                             // F1: the compare key
  }
#endif
  if (auto added = ui.AddOverlays(overlays); !added) {
    LARECOMP_APP_ERROR("[ui] the overlays were not added, so no F-key menu works: {}", added.error().GetDescription());
  }
  mcla::ui::StartControllerToasts(ui);  // the connect / disconnect panels (always on the screen's list, drawing nothing between pads)
  mcla::display::Initialize(ui);        // the DISPLAY MODE rows: follows the fullscreen / resolution settings (0.10 did this in SetupPresentation)
}

// The window's client size for the ultrawide maths: physical pixels, from the native window (the video mode is the title's, not the window's).
static void PublishWindowSize(rex::ui::IUISystem& ui) {
#if defined(LARECOMP_HAS_GPU)
  if (auto window = ui.GetNativeWindow()) {
    uint32_t width = 0, height = 0;
    mcla::gpu::GetWindowPixelSize(*window, width, height);
    if (width && height) {
      mc::ultrawide::SetWindowSize(width, height);
      return;
    }
  }
#endif
  if (const auto mode = ui.GetVideoMode(); mode.width) mc::ultrawide::SetWindowSize(mode.width, mode.height);
}

// The user folder moved by ChooseUserFolder: bring what is already in Documents over (reading it is allowed), without
// the cache, which is rebuilt. A marker keeps the choice, so lifting the block later does not split the saves.
void LarecompApp::MoveUserDataIfNeeded() {
  const std::filesystem::path fallback = LocalUserFolder();
  const std::filesystem::path now = rex::GetPathSettings().user_root.Get();
  if (fallback.empty() || now != fallback) return;
  const std::filesystem::path marker = fallback / ".moved_from_documents";
  std::error_code ec;
  if (std::filesystem::exists(marker, ec)) {
    LARECOMP_APP_WARN("Game data: user data in '{}' (moved there from Documents on an earlier run)", fallback.string());
    return;
  }
  const std::filesystem::path documents = rex::platform::GetUserFolder() / "larecomp";  // where ChooseUserFolder looked first
  size_t copied = 0;
  if (std::filesystem::is_directory(documents, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(documents, ec)) {
      if (entry.path().filename() == "cache") continue;
      std::filesystem::copy(entry.path(), fallback / entry.path().filename(),
                            std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, ec);
      if (!ec) ++copied;
    }
  }
  { std::ofstream(marker) << documents.string() << '\n'; }
  LARECOMP_APP_WARN("Game data: '{}' cannot be written; user data moved to '{}', {} item(s) copied", documents.string(),
                    fallback.string(), copied);
  GetUI().ShowMessage(rex::MessageType::kWarning, "LARecomp",
                      "LARecomp cannot save to\n" + documents.string() + "\n\nYour saves are kept in\n" + fallback.string() +
                          "\ninstead, and the ones already there were copied over. Nothing else to do.");
}

larecomp::Paths LarecompApp::CurrentPaths() {
  larecomp::Paths p;
  rex::PathSettings& ps = rex::GetPathSettings();
  p.user_root = ps.user_root.Get();
  p.cache_root = ps.cache_root.Get();
  p.config_file = ps.config_file.Get();
  rex::kernel::TitlePaths& tp = GetTitleRuntime().GetTitlePaths();
  p.game_root = tp.game_root.Get();
  p.update_root = tp.update_root.Get();
  p.metadata_root = tp.metadata_root.Get();
  return p;
}

// -----------------------------------------------------------------------------------------------------------------
// The first-launch wizards, between Init and LaunchTitle. Each is asynchronous (an IDialog the UI system draws); the
// main loop calls RunSetupFrame until SetupPending() is false. Order as in 0.10: the save first (the save check and the
// content manager both key on the profile name it settles), then the game files, then the save import offer.
bool LarecompApp::SetupPending() const { return step_ != SetupStep::kDone; }

void LarecompApp::BeginSetupStep() {
  step_started_ = false;
  step_done_ = false;
}

bool LarecompApp::RunSetupFrame() {
  if (!RunFrame()) return false;
  if (step_ == SetupStep::kDone) return true;
  rex::ui::IUISystem& ui = GetUI();
  const auto advance = [this](SetupStep next) {
    step_ = next;
    BeginSetupStep();
  };
  // "done" callbacks come from a dialog's own draw: they only set a flag, the step is left here, outside the draw.
  switch (step_) {
    case SetupStep::kProfile:
      if (!step_started_) {
        step_started_ = true;
        // Seeds kernel.user_name (so the SDK's own prompt, which would save larecomp.toml in its own format, stays shut)
        // and writes the choice as flat lines of larecomp.toml. A first run with no save and no name asks here too.
        if (!larecomp::ShowProfilePicker(ui, CurrentPaths(), [this] { step_done_ = true; })) step_done_ = true;
      }
      if (step_done_) advance(SetupStep::kInstall);
      break;
    case SetupStep::kInstall:
      if (!step_started_) {
        step_started_ = true;
        larecomp::Paths paths = CurrentPaths();
        if (larecomp::IsGameInstalled(paths.game_root)) {
          step_done_ = true;
        } else {
          bool attempted = false;
          if (larecomp::TryAutomatedIsoInstall(paths, attempted)) {
            step_done_ = true;  // installed unattended
          } else if (attempted) {
            rex::app::RequestShutdown(1);
            return false;
          } else {
            // The folder the player installed to becomes the game root (title.game_root is kUntilLaunch: writable until
            // LaunchTitle); the next step and the SDK's LaunchTitle read it from there.
            larecomp::ShowRexglueIsoInstallWizard(ui, paths, [this](larecomp::Paths installed) {
              if (auto set = GetTitleRuntime().GetTitlePaths().game_root.Set(installed.game_root); !set) {
                LARECOMP_APP_WARN("Game install: title.game_root could not be set to '{}': {}", installed.game_root.string(),
                                  set.error().GetDescription());
              }
              step_done_ = true;
            });
          }
        }
      }
      if (step_done_) advance(SetupStep::kSaveOffer);
      break;
    case SetupStep::kSaveOffer:
      if (!step_started_) {
        step_started_ = true;
        if (larecomp::SaveAlreadyPresent(CurrentPaths().user_root) ||
            !larecomp::ShowSaveImportWizard(ui, CurrentPaths(), [this] { step_done_ = true; })) {
          step_done_ = true;  // skipping is fine: the game creates a new save on its own
        }
      }
      if (step_done_) advance(SetupStep::kDone);
      break;
    case SetupStep::kDone:
      break;
  }
  return true;
}

// -----------------------------------------------------------------------------------------------------------------
// --build_shader_packs / --precache_pipelines (the launcher's setup): no window, no renderer, no runtime. The paths and
// the config are final and logging is up, so the folders are the ones the game itself will use.
// CHECK: the exe is a GUI program (WIN32): stdout reaches a parent only through a pipe the parent made (the launcher does).
void LarecompApp::RunBatchMode() {
  const std::filesystem::path game_root = GetTitleRuntime().GetTitlePaths().game_root.Get();
  int code = 0;
  if (batch_ == BatchMode::kBuildShaderPacks) {
    code = larecomp::shaderbuild::RunCommandLine(game_root);
  } else if (batch_ == BatchMode::kPrecachePipelines) {
    code = larecomp::pipelinebuild::RunCommandLine(game_root);
  }
  // 0.10 also ran the pipeline pre-cache after the packs when both were given: main passes both as one mode in that
  // case (kBuildShaderPacks then kPrecachePipelines), see main.cpp.
  rex::app::RequestShutdown(code);
}

// -----------------------------------------------------------------------------------------------------------------
// Between TitleRuntime::Load and the start of the guest. What 0.10 did in OnPostLoadXexImage, OnPostSetup and
// LaunchModule: the achievement labels, the hooks' environment, then the shader packs and the mod/music build behind
// the progress popup, then the hooks.
rex::Result<void> LarecompApp::OnTitleLaunch(rex::kernel::TitleRuntime& title) {
  title_ = &title;
  mcla::ui::SetAchievementManager(&title.GetAchievementManager());  // the F7 list
  g_guest_mem = title.GetMembase().GetBase();
  LARECOMP_APP_INFO("by @mzzvxm. base memory: 0x{:016X}", reinterpret_cast<std::uintptr_t>(g_guest_mem));

  // The achievement labels from the resource embedded in the exe, layered over the XDBF ones by id.
  // (LoadAchievementMetadata works on rex::xam::IAchievementManager: it lists the catalog the XDBF filled and registers
  // the layered entries back.)
#ifdef _WIN32
  if (HMODULE mod = GetModuleHandleW(nullptr)) {
    if (HRSRC res = FindResourceA(mod, "ACHIEVEMENTS_TOML", RT_RCDATA)) {
      HGLOBAL handle = LoadResource(mod, res);
      const DWORD size = SizeofResource(mod, res);
      const char* bytes = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
      if (bytes && size) {
        larecomp::LoadAchievementMetadata(title.GetAchievementManager(), std::string_view(bytes, size),
                                          "<embedded achievements.toml>");
      }
    }
  }
#else
  {
    std::ifstream in(rex::platform::GetExecutableFolder() / "assets" / "achievements.toml", std::ios::binary);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      larecomp::LoadAchievementMetadata(title.GetAchievementManager(), text, "assets/achievements.toml");
    }
  }
#endif

  // The identity's XUID has to reach the profile before the guest first asks for it: ApplyProfileIdentity calls
  // UserProfile::set_xuid on TitleRuntime::GetUserProfile() (REX_RUNTIME() is set from the end of Load, so it is valid here).
  larecomp::identity::ApplyProfileIdentity();

  // The stub safety net of 0.10 (about 1.7 million unmapped addresses registered as logging no-ops, so an unmapped indirect
  // call is a logged no-op rather than a crash) needs FunctionDispatcher::SetFunction, which a TitleRuntime does not give
  // out. It was a debug net (stubs.txt stays empty in practice); the one functional entry, the disc error handler, is a
  // manifest hook now (MCLA_DiscErrorBypass). On 0.11 an unmapped indirect call is NOT a logged no-op: ResolveFunction hands
  // back InvalidFunctionTrap, which logs "Call to invalid or unregistered function at guest address 0x..." as a fatal
  // (REXCPU_FATAL: flush and abort). Accepted: the net never caught anything, and the log names the address.

  // The F3 overlay's "Guest: FPS": nothing in the SDK counts guest frames without a command processor, so the swap hook
  // supplies them (nocp_app.h GuestFrameStats; IGraphicsSystem::GetFrameStats counts the same swaps through WaitForFrame).

  // OnTitleExit: kRelaunch (XamLoaderLaunchTitle) and the player's choice are handled by the base class.
  exit_listener_ = title.OnTitleExit([this](rex::kernel::TitleExitReason reason) { (void)reason; });

  if (!RunBootBuild(title)) {
    // RunBootBuild ends this way only when the player closed the window or pressed QUIT on the dialog that said why the build
    // failed (ShowFailure): the start has to stop, with nothing more to tell. The SDK reports an Err from here in a modal box, which a
    // player had to click away after choosing QUIT (and which held the process open until the close watchdog ended it).
    boot_build_ended_by_player_.store(true, std::memory_order_release);
    return rex::Err(rex::ErrorCategory::kRuntime, "the boot build was cancelled or failed");
  }
  return rex::Ok();
}

// The SDK shows an error of a lifecycle step in a modal box (TitleApp::ReportError). After the player ended the boot build themselves
// (OnTitleLaunch above) the log line is all there is: the dialog they chose QUIT on already said what went wrong.
void LarecompApp::ReportError(std::string_view message) {
  if (boot_build_ended_by_player_.load(std::memory_order_acquire)) {
    rex::app::App::ReportError(message);
    return;
  }
  TitleApp::ReportError(message);
}

// The boot work behind the progress popup: first the shader packs, when the renderer has none (LARecomp builds them from
// the player's own game files, both formats), then the mods and the music. It runs off the UI thread with the popup
// drawing: this loop pumps RunFrame (OnTitleLaunch is on the UI thread) until it is done. A shader build that fails or is
// cancelled ends in a dialog and the guest does not start; Retry runs it again.
// CHECK: RunFrame from inside OnTitleLaunch (nested pump) is allowed: the phase is kTitleLaunch, the UI thread is this one.
bool LarecompApp::RunBootBuild(rex::kernel::TitleRuntime& title) {
  bool force = false;
  for (;;) {
    auto shaders = std::make_shared<larecomp::shaderbuild::Phase>(GetTitleRuntime().GetTitlePaths().game_root.Get(),
                                                                   title.GetMembase().GetBase(), force);
    std::atomic<bool> finished{false};
    larecomp::StartBootBuildWithOverlay(
        GetUI(),  // CHECK (L5): the popup is an IDialog added to IUISystem
        [shaders]() {
          shaders->Run();
          if (shaders->Ok()) mc::modloader::Init();
        },
        [&finished]() { finished = true; });
    while (!finished) {
      if (!RunFrame()) {
        larecomp::RequestBootBuildCancel();
        larecomp::WaitForBootBuild();
        return false;
      }
    }
    if (shaders->Finish()) break;
    bool retry = false, decided = false;
    shaders->ShowFailure(GetUI(), [&](bool f) { force = f; retry = true; decided = true; }, [&] { decided = true; });
    while (!decided) {
      if (!RunFrame()) return false;
    }
    if (!retry) return false;
  }

  LARECOMP_Discord_Init();
  InitPauseMenuHooks();
  larecomp::StartOnlineToast(GetUI());
  larecomp::StartServerBrowser(GetUI(), &GetInput());
  larecomp::StartIdentity(GetUI());
  InitMapMouse();
  InitStringTableTools();
  InitHooks();  // its own modloader::Init call is a no-op after the build
  return true;
}

// -----------------------------------------------------------------------------------------------------------------
// The title ended by itself. The base class asks the player to restart or exit (or shuts down when headless) and handles
// kRelaunch (XamLoaderLaunchTitle's sidecar): LARecomp has nothing to add yet.
void LarecompApp::OnTitleExit(rex::kernel::TitleExitReason reason) { TitleApp::OnTitleExit(reason); }

// A normal window close: SDL's close event reaches the listeners first (here), then the SDK's close handler asks for a
// shutdown (RequestShutdown(0)), the main loop ends and App::Shutdown stops the title (TitleApp::OnStop) and calls OnShutdown.
// The save wait must come BEFORE the title is terminated, so it runs from the event, on the UI thread, before the
// shutdown request. Read from sdl3_ui_driver.cpp (2026-11-18): kClose (SDL_EVENT_WINDOW_CLOSE_REQUESTED and SDL_EVENT_QUIT) is
// delivered by State::Process from WaitEvents, i.e. from the frame loop (RunFrame) on the UI thread. The event WATCH (WatchEvent)
// delivers only kResize and kExposed, and only inside the Windows modal move/size loop, so the 0.10 deadlock (the close ran
// inside a watcher with the mouse-capture mutex held) cannot happen on kClose.
void LarecompApp::OnUIEvent(const rex::ui::UIEvent& event) {
  using Type = rex::ui::UIEvent::Type;
  larecomp::uievents::DispatchWindow(event);  // the handlers of the input state and the keyboard
  switch (event.GetType()) {
    case Type::kClose:
      FinishWindowCloseWork();
      break;
    case Type::kResize:
      PublishWindowSize(GetUI());
#if defined(LARECOMP_HAS_GPU)
      mcla::gpu::RequestResize();
#endif
      break;
    case Type::kFocus:
      // The event carries no direction and IsFocused() still has the old value while the listeners run (UISystem::State::OnEvent
      // tells them first, then updates its flag), so the answer is read a moment later, on the same thread.
      GetUI().PostTask([this] {
        const bool focused = GetUI().IsFocused();
        mcla::display::OnFocusChanged(focused);  // FULLSCREEN (exclusive) gives the display its mode back
        larecomp::uievents::DispatchFocus(focused);
      });
      break;
    default:
      break;
  }
}

// The UI system hands every key, mouse and wheel event that no overlay holds to its listeners, in order, on the UI thread, before it
// runs its own binds (so a handler skips a key a bind owns: ui/overlay_support.h IsBoundKey).
void LarecompApp::OnInputEvent(rex::InputEvent& event) { larecomp::uievents::DispatchInput(event); }

// What the close needs before the title ends: shader build told to stop, saves finished, devices released. Latched: the event can come
// twice, and StopBeforeTitleEnds does it again for a shutdown that did not come from the window. The renderer is NOT stopped here (0.10 did): this
// runs inside the delivery of the close event, on the UI thread, and the renderer stops with the UI loop still served (StopBeforeTitleEnds).
void LarecompApp::FinishWindowCloseWork() {
  if (close_work_started_.exchange(true, std::memory_order_acq_rel)) return;
  larecomp::RequestBootBuildCancel();
  const auto started = std::chrono::steady_clock::now();
  MclaWaitForSaves();  // a career save in flight finishes (10 s limit inside)
  LogAudioDropoutSummary();
  mc::cinematic::capture_audio::Abort();  // a take's sound holds the audio worker's callback at a gate: let it go before the SDK stops the worker
  larecomp::triggers::Shutdown();  // the DualSense's triggers back to slack
  larecomp::devwatch::Stop();
  larecomp::impulse::Shutdown();   // the Xbox triggers back to still
  mcla::display::Shutdown();       // the display's own mode back after FULLSCREEN
  LARECOMP_APP_INFO("[quit] LARecomp's shutdown took {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
  // A bound for whatever the SDK's close still does: armed after the save wait and the renderer stop, which have their own
  // limits. CHECK: App::Shutdown has its own terminate timeout (runtime.terminate_timeout_ms) and ends the process
  // normally; this watchdog only covers a hang.
  std::thread([]() {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    LARECOMP_APP_ERROR("[quit] the process was still running 5 s after the window was asked to close; exiting");
    if (auto* logger = rex::AppInterfaces::Get<rex::ILogger>()) logger->Flush();
    larecomp::EndProcessNow(0);  // not std::_Exit: process_exit.h says why (the 2026-11-18 Vulkan crash at exit)
  }).detach();
}

// What LARecomp must do before the guest ends: the close work (for a shutdown that did not come from the window: the pause menu's QUIT,
// a restart, a setup failure), the release of the achievement manager, then the renderer. The render thread never waits for the UI
// thread (overlay.h: it posts a pass and takes the last frame), so joining it here cannot deadlock against a pass that never runs. It
// stops BEFORE the title unloads: it reads the guest's memory and the GPU state the title owns. Latched (OnRun's end and OnShutdown
// both call it).
void LarecompApp::StopBeforeTitleEnds() {
  if (before_title_end_done_.exchange(true, std::memory_order_acq_rel)) return;
  FinishWindowCloseWork();
  mcla::ui::SetAchievementManager(nullptr);  // the manager goes with the title
#if defined(LARECOMP_HAS_GPU)
  mcla::gpu::Shutdown();  // the render thread stops, the GPU drains, the swap chain is given back
#endif
}

// rexglue 0.11 made TitleApp::OnStop final (0.10's ReXApp could override it), and App::Shutdown calls it right after IUISystem
// BeginStop, so the pre-stop work moves to the end of the run: Run() returns on every path that ends the title (the window's close, the
// pause menu's QUIT, a restart, the guest's own exit) and main calls Shutdown at once. A path that never reaches Run (Init or LaunchTitle
// failed, a batch action) takes the latch in OnShutdown, after the title stopped.
// CHECK: DrawOverlays is still served here (BeginStop comes later in Shutdown): the renderer's last overlay passes may post to a UI
// thread that no longer runs frames; the 0.10 order stopped the renderer after BeginStop. Run a quit from the pause menu and from the
// window's close and look for a hang at exit (the 5 s watchdog in FinishWindowCloseWork would end the process with a log line).
void LarecompApp::OnRun() {
  TitleApp::OnRun();
  StopBeforeTitleEnds();
}

// After the title stopped, before the drivers shut down (logging and settings still work).
void LarecompApp::OnShutdown() {
  // The UI system's event guard and the cached interface pointers go before rex::Stop takes the systems away.
  ui_listener_ = {};
  larecomp::sdk::Reset();
  StopBeforeTitleEnds();  // the latched work of OnRun's end, for a path that never ran (restart, setup failure, batch actions)
  larecomp::RequestBootBuildCancel();
  larecomp::WaitForBootBuild();
  LARECOMP_Discord_Shutdown();
  mc::profiler::Shutdown();
  mc::DisableHighResTimer();
  ls::UnregisterAll();
  ShutdownLarecompLogging();
}

rex::platform::CrashReporterOptions LarecompApp::GetCrashOptions() {
  // CHECK (L3/L4): LARecomp's own crash handler (crash_handler.cpp: minidump, log of the last hook, Linux signal handlers)
  // and the SDK's reporter both want the process-wide filter; the SDK's is installed first, then InstallCrashLoggerAfterRuntime.
  return TitleApp::GetCrashOptions();
}
