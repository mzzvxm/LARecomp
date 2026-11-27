// Run with no arguments. Each save and reload uses a fresh process so it sees
// the same setting layers as a real game restart.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <toml++/toml.hpp>

#include "gfx_preset_apply.h"
#include "gpu_class.h"
#include "mc_engine/core/config_save.h"
#include "rt_tier.h"
#include "settings/larecomp_settings.h"

// The preset runtime normally gets its startup recommendation from the device.
namespace mcla::gpu::gpu_class {
bool Known() { return false; }
GpuClass Current() { return GpuClass::kHigh; }
RtTier CurrentRtTier() { return RtTier::kNone; }
}  // namespace mcla::gpu::gpu_class

namespace {

namespace gp = mcla::gpu::gfx_preset;
namespace cv = larecomp::cvar;
namespace ls = larecomp::settings;

constexpr std::string_view kFiltering = "mcla_gpu_anisotropy";
constexpr std::string_view kDebugSetting = "mcla_gpu_ui_sharp_background";
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, std::string_view label) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::fprintf(stderr, "FAILED: %.*s\n", static_cast<int>(label.size()), label.data());
  }
}

void Expect(std::string_view name, std::string_view expected) {
  const auto actual = cv::GetFlagByName(name);
  if (actual != expected) {
    std::fprintf(stderr, "  got '%s', expected '%.*s'\n", actual.c_str(), static_cast<int>(expected.size()), expected.data());
  }
  Check(actual == expected, name);
}

void Save(const std::filesystem::path& path, gp::Preset origin, std::string_view edit, bool hold_overrides) {
  Check(gp::Current() == origin, "startup selects the saved named preset");
  for (const auto& row : gp::kRows) {
    Expect(row.cvar, gp::CellOf(row.cvar, origin));
  }
  Check(gp::NoteRowWrite(kFiltering), "editing a covered row selects CUSTOM");
  Check(cv::SetFlagByName(kFiltering, edit), "apply the filtering edit");
  Check(cv::SetFlagByName(kDebugSetting, "false"), "change an unrelated debug setting");
  if (hold_overrides) {
    Check(gp::SetSessionPreset(origin), "temporarily apply the Director preset");
    Expect(kFiltering, gp::CellOf(kFiltering, origin));
    Check(larecomp::SetCvarForScene(gp::kFromSetting, gp::kFromNone), "temporarily hold the origin");
  }
  Check(larecomp::SaveCvarsToConfig(path), "save to TOML");
  if (hold_overrides) {
    Check(larecomp::RestoreCvarForScene(gp::kFromSetting), "restore the origin");
    Check(gp::SetSessionPreset(std::nullopt), "release the Director preset");
    Expect(kFiltering, edit);
  }

  const auto saved = toml::parse_file(path.string());
  Check(saved[gp::kPresetSetting].value_or("") == gp::Id(gp::Preset::kCustom), "CUSTOM was saved");
  Check(saved[gp::kFromSetting].value_or("") == gp::Id(origin), "preset origin was saved");
  Check(saved[kFiltering].value_or("") == edit, "explicit filtering override was saved");
  Check(!saved.contains("disable_motion_blur"), "an untouched row at its inherited and registered default stays implicit");
  Check(!saved.contains(kDebugSetting), "the unrelated debug setting stays session-only");
}

void Read(gp::Preset origin, std::string_view edit) {
  Check(gp::Current() == gp::Preset::kCustom, "startup selects CUSTOM");
  Expect(gp::kFromSetting, gp::IsNamed(origin) ? gp::Id(origin) : gp::kFromNone);
  Expect(kFiltering, edit);
  Check(cv::GetFlagSource(kFiltering) == cv::Source::kConfig, "explicit filtering overrides the base at the file layer");
  for (const auto& row : gp::kRows) {
    if (row.cvar == kFiltering) continue;
    const auto* info = cv::GetFlagInfo(row.cvar);
    Check(info != nullptr, row.cvar);
    if (info) Expect(row.cvar, gp::IsNamed(origin) ? gp::CellOf(row.cvar, origin) : std::string_view(info->default_value));
  }
}

std::string_view FilteringEdit(gp::Preset origin) {
  return origin == gp::Preset::kEnhanced || origin == gp::Preset::kEnhancedRt ? "game" : "4";
}

int RunPhase(const char* self, const char* phase, const char* origin, const std::filesystem::path& path) {
  const std::string file = path.string();
#ifdef _WIN32
  const std::string quoted = '"' + file + '"';
  return static_cast<int>(_spawnlp(_P_WAIT, self, self, phase, origin, quoted.c_str(), nullptr));
#else
  const pid_t pid = fork();
  if (pid == 0) {
    execl(self, self, phase, origin, file.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool WriteFixture(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  Check(static_cast<bool>(file), "write a disposable config fixture");
  return static_cast<bool>(file);
}

int RunFixtures(const char* self) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() / ("larecomp_preset_roundtrip_" + std::to_string(stamp));
  if (!std::filesystem::create_directory(dir)) return 2;
  const auto path = dir / "larecomp.toml";
  for (int i = 0; i < gp::kNamedCount; ++i) {
    const char* origin = gp::kIds[i];
    const std::string saved_origin = std::string(" = \"") + origin + "\"\n";
    if (WriteFixture(path, "mcla_gpu_preset" + saved_origin)) {
      Check(RunPhase(self, "save", origin, path) == 0, "save a named preset as CUSTOM");
      Check(RunPhase(self, "read", origin, path) == 0, "reload the game-written CUSTOM");
    }
    const std::string edit(FilteringEdit(static_cast<gp::Preset>(i)));
    if (WriteFixture(path, "mcla_gpu_preset = \"custom\"\nmcla_gpu_preset_from" + saved_origin +
                               "mcla_gpu_anisotropy = \"" + edit + "\"\n")) {
      Check(RunPhase(self, "read", origin, path) == 0, "load a launcher-written CUSTOM");
    }
  }
  if (WriteFixture(path, "mcla_gpu_preset = \"enhanced\"\n")) {
    Check(RunPhase(self, "save-held", "enhanced", path) == 0, "save while the Director holds overrides");
    Check(RunPhase(self, "read", "enhanced", path) == 0, "reload the player values from before the overrides");
  }
  for (const char* prefix : {"", "mcla_gpu_preset = \"custom\"\n"}) {
    if (WriteFixture(path, std::string(prefix) + "mcla_gpu_anisotropy = \"4\"\n")) {
      Check(RunPhase(self, "read", "none", path) == 0, "load a config without the origin key");
    }
  }
  std::filesystem::remove(path);
  std::filesystem::remove(dir);
  std::printf("preset round trip fixtures: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 1) return RunFixtures(argv[0]);
  const std::string_view phase = argc == 4 ? argv[1] : "";
  const std::string_view from = argc == 4 ? argv[2] : "";
  gp::Preset origin = gp::Preset::kCustom;
  const bool named = gp::Parse(from, &origin) && gp::IsNamed(origin);
  const bool save = phase == "save" || phase == "save-held";
  if ((!save && phase != "read") || (!named && (save || from != gp::kFromNone))) {
    std::fprintf(stderr, "usage: preset_roundtrip_test [<save|save-held|read> <preset|none> <config>]\n");
    return 2;
  }

  auto& registry = rex::GetSettingsRegistry();
  if (!ls::RegisterAll(registry)) return 2;
  const auto* origin_info = cv::GetFlagInfo(gp::kFromSetting);
  Check(origin_info && origin_info->is_debug_only, "the origin stays hidden in settings metadata");
  const std::filesystem::path path = argv[3];
  const auto facts = ls::ApplyPlayerConfig(registry, path);
  Check(static_cast<bool>(registry.Load(path, rex::SettingLayer::kFile)), "SDK file load accepts the config");
  ls::MigrateGraphicsPresetIds();
  gp::Start(facts);
  const std::string_view edit = FilteringEdit(origin);
  if (save) Save(path, origin, edit, phase == "save-held");
  else Read(origin, edit);
  ls::UnregisterAll();

  std::printf("preset round trip %s %s: %d checks, %d failed\n", argv[1], argv[2], g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
