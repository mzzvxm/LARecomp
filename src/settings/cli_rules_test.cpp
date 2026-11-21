// A check that needs no game: what LARecomp does with each option of the command line (settings/cli_rules.h). Build:
// cmake --build <build dir> --target larecomp_cli_rules_test, then run it; exit 0 = passed.
//
// The registry is faked here (three small tables), so the rules are tested with the names the launchers and the old shortcuts
// really pass: the case of 2026-11-18 (--d3d12_adapter=0 with LARecomp's own renderer) is the first one.

#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "settings/cli_rules.h"

using namespace larecomp::cli;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

// The registry as the app sees it in OnConfigure: the SDK's always-loaded modules and LARecomp's owners are registered, a
// driver's settings are not; the alias table holds LARecomp's flat names and the SDK's old ones.
struct FakeLookup {
  std::set<std::string> aliases = {"d3d12_adapter",  "vulkan_device",  "texture_cache_memory_limit_soft", "vsync",
                                   "fullscreen",     "log_file",       "mcla_gpu_ao",                     "debug_menu",
                                   "build_shader_packs_force", "build_city_pack_force", "keybind_a", "user_name", "mcla_gpu_vsync", "fps_limit",
                                   // the removed names (kRemovedNames in settings/larecomp_settings.cpp): an alias to "", no setting
                                   "mcla_gpu", "mcla_native_gfx_nocp"};
  std::map<std::string, std::string> full = {
      {"d3d12_adapter", "gpu.d3d12.adapter"},
      {"vulkan_device", "gpu.vulkan.device"},
      {"texture_cache_memory_limit_soft", "gpu.xenos.texture_cache_memory_limit_soft"},
      {"vsync", "gpu.vsync"},
      {"fullscreen", "ui.fullscreen"},
      {"log_file", "log.file_name"},
      {"mcla_gpu_ao", "larecomp.mcla_gpu_ao"},
      {"debug_menu", "larecomp.debug_menu"},
      {"build_shader_packs_force", "larecomp.build_shader_packs_force"},
      {"build_city_pack_force", "larecomp.build_city_pack_force"},
      {"mcla_gpu_vsync", "larecomp.mcla_gpu_vsync"},
      {"fps_limit", "larecomp.fps_limit"},
      // "keybind_a" and "user_name" are aliases only the SDK has
  };
  std::map<std::string, bool> registered_bool = {
      {"gpu.vsync", true},          {"ui.fullscreen", true},    {"larecomp.debug_menu", true},
      {"larecomp.mcla_gpu_vsync", true}, {"larecomp.build_shader_packs_force", true}, {"larecomp.build_city_pack_force", true},
      {"larecomp.mcla_gpu_ao", false}, {"larecomp.fps_limit", false}, {"log.file_name", false},
  };

  bool is_alias(std::string_view name) const { return aliases.count(std::string(name)) != 0; }
  std::string full_name(std::string_view name) const {
    const auto it = full.find(std::string(name));
    return it == full.end() ? std::string() : it->second;
  }
  std::optional<bool> registered_is_bool(std::string_view name) const {
    const auto it = registered_bool.find(std::string(name));
    if (it == registered_bool.end()) return std::nullopt;
    return it->second;
  }
};

const Selection kPlume{"larecomp", "larecomp", "sdl3", "sdl3", true};            // the default of v0.11
const Selection kXeniaD3d12{"d3d12", "larecomp", "sdl3,xinput", "sdl3", true};   // gpu.driver = d3d12
const Selection kBatch{"none", "none", "none", "none", true};                     // --build_shader_packs, --precache_pipelines

Decision Run(const char* name, std::optional<std::string> value, const Selection& selection) {
  return Decide(Option{name, std::move(value)}, selection, FakeLookup{});
}

// ---------------------------------------------------------------------------------------------------------------
void TestTheReportedCase() {
  // v0.11, first boot: the game started with --d3d12_adapter=0 and gpu.driver = larecomp stopped on "unknown setting
  // 'gpu.d3d12.adapter' from CommandLine". Now one line and the start goes on.
  const Decision d = Run("d3d12_adapter", "0", kPlume);
  CHECK(d.verdict == Verdict::kUnloaded);
  CHECK(d.set.empty());
  CHECK(d.note == "--d3d12_adapter=0 ignored: gpu.d3d12.adapter is a setting of the d3d12 graphics driver, which this run "
                  "does not load (gpu.driver = larecomp)");
  // The same option with the d3d12 driver is applied, by the name the player typed (the registry resolves the alias).
  const Decision applied = Run("d3d12_adapter", "0", kXeniaD3d12);
  CHECK(applied.verdict == Verdict::kApply);
  CHECK(applied.set.size() == 1 && applied.set[0].name == "d3d12_adapter" && applied.set[0].value == "0");
  // The full name and the other driver's setting.
  CHECK(Run("gpu.d3d12.adapter", "1", kPlume).verdict == Verdict::kUnloaded);
  CHECK(Run("vulkan_device", "1", kXeniaD3d12).verdict == Verdict::kUnloaded);
  CHECK(Run("gpu.vulkan.device", "1", kXeniaD3d12).verdict == Verdict::kUnloaded);
  CHECK(Run("gpu.vulkan.device", "1", kPlume).verdict == Verdict::kUnloaded);
  // The batch actions load no driver at all.
  const Decision batch = Run("d3d12_adapter", "0", kBatch);
  CHECK(batch.verdict == Verdict::kUnloaded && batch.note.find("(gpu.driver = none)") != std::string::npos);
}

void TestDriverScopes() {
  // gpu.xenos.* belongs to every Xenia driver, d3d12 and vulkan and null.
  CHECK(Run("texture_cache_memory_limit_soft", "512", kPlume).verdict == Verdict::kUnloaded);
  CHECK(Run("gpu.xenos.async_shader_compilation", "true", kXeniaD3d12).verdict == Verdict::kApply);
  const Selection vulkan{"vulkan", "larecomp", "sdl3", "sdl3", true};
  const Selection null_gpu{"null", "larecomp", "sdl3", "sdl3", true};
  CHECK(Run("gpu.xenos.async_shader_compilation", "true", vulkan).verdict == Verdict::kApply);
  CHECK(Run("gpu.xenos.async_shader_compilation", "true", null_gpu).verdict == Verdict::kApply);
  CHECK(Run("gpu.d3d12.adapter", "0", null_gpu).verdict == Verdict::kUnloaded);
  // "any" is d3d12 on Windows and vulkan elsewhere.
  const Selection any_windows{"any", "larecomp", "sdl3", "sdl3", true};
  Selection any_other = any_windows;
  any_other.windows = false;
  CHECK(!WhyUnloaded("gpu.d3d12.adapter", any_windows));
  CHECK(WhyUnloaded("gpu.vulkan.device", any_windows).has_value());
  CHECK(!WhyUnloaded("gpu.vulkan.device", any_other));
  CHECK(WhyUnloaded("gpu.d3d12.adapter", any_other).has_value());
  // The input and window drivers.
  const Selection xinput_only{"larecomp", "larecomp", "xinput", "sdl3", true};
  const Selection both{"larecomp", "larecomp", "sdl3, xinput", "sdl3", true};
  CHECK(Run("hid.sdl.mappings_file", "x", xinput_only).verdict == Verdict::kUnloaded);
  CHECK(WhyUnloaded("hid.sdl.mappings_file", xinput_only).value() ==
        "hid.sdl.mappings_file is a setting of the sdl3 input driver, which this run does not load (hid.drivers = xinput)");
  CHECK(Run("hid.sdl.mappings_file", "x", both).verdict == Verdict::kApply);
  CHECK(Run("ui.sdl.video_driver", "x", kPlume).verdict == Verdict::kApply);
  const Selection headless{"larecomp", "larecomp", "sdl3", "none", true};
  CHECK(Run("ui.sdl.video_driver", "x", headless).verdict == Verdict::kUnloaded);
  // Settings of modules that are always there are never judged by a driver.
  CHECK(!WhyUnloaded("gpu.vsync", kPlume));
  CHECK(!WhyUnloaded("gpu.driver", kPlume));
  CHECK(!WhyUnloaded("hid.drivers", kPlume));
  CHECK(!WhyUnloaded("larecomp.mcla_gpu_ao", kPlume));
  CHECK(!WhyUnloaded("ui.bind.mcla_settings", kPlume));
  // A look-alike prefix is not the scope.
  CHECK(!WhyUnloaded("gpu.d3d12_adapter", kPlume));
}

void TestPlainSettings() {
  const Decision off = Run("vsync", "false", kPlume);
  CHECK(off.verdict == Verdict::kApply && off.set.size() == 1 && off.set[0].name == "vsync" && off.set[0].value == "false");
  const Decision bare = Run("vsync", std::nullopt, kPlume);
  CHECK(bare.set[0].value == "true");
  const Decision negated = Run("no-vsync", std::nullopt, kPlume);
  CHECK(negated.verdict == Verdict::kApply && negated.set[0].name == "vsync" && negated.set[0].value == "false");
  // The SDK's rule: --no-name is false whatever follows.
  CHECK(Run("no-vsync", "true", kPlume).set[0].value == "false");
  // --no- on a setting that is not a bool.
  const Decision bad = Run("no-mcla_gpu_ao", std::nullopt, kPlume);
  CHECK(bad.verdict == Verdict::kBadUse);
  CHECK(bad.note == "--no-mcla_gpu_ao ignored: larecomp.mcla_gpu_ao is not a bool, so --no- cannot turn it off");
  // A string with a path in it is passed untouched.
  const Decision log = Run("log_file", "C:/Users/me/My Games/run.log", kPlume);
  CHECK(log.verdict == Verdict::kApply && log.set[0].name == "log_file" && log.set[0].value == "C:/Users/me/My Games/run.log");
  // Selectors are plain settings.
  CHECK(Run("gpu.driver", "d3d12", kPlume).verdict == Verdict::kApply);
  CHECK(Run("hid.drivers", "xinput", kPlume).verdict == Verdict::kApply);
  // An empty value is a value (the registry decides).
  CHECK(Run("log_file", "", kPlume).set[0].value.empty());
}

void TestNamesNothingKnows() {
  // Flat names that are not aliases: the fork launcher's, typos, a help request.
  const Decision fps = Run("mcla_fps", "60", kPlume);
  CHECK(fps.verdict == Verdict::kUnknown);
  CHECK(fps.note == "--mcla_fps=60 ignored: not a setting of this version");
  CHECK(Run("mcla_msaa", "true", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_gpu_backend", "vulkan", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_gpu_aoo", "high", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("help", std::nullopt, kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("no-mcla_fps", std::nullopt, kPlume).verdict == Verdict::kUnknown);
  // Bad grammar even with dots: the SDK refuses these names too.
  CHECK(Run("Gpu.driver", "x", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("gpu..driver", "x", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("gpu.", "x", kPlume).verdict == Verdict::kUnknown);
  // A dotted name nothing is known about, and an alias only the SDK knows, go to the SDK: an owner may register later
  // (overlay binds register after OnConfigure) and the SDK's own check decides after OnInit.
  CHECK(Run("ui.bind.mcla_settings", "F1", kPlume).verdict == Verdict::kApply);
  CHECK(Run("gpu.foo.bar", "1", kPlume).verdict == Verdict::kApply);
  CHECK(Run("keybind_a", "X", kPlume).verdict == Verdict::kApply);
  CHECK(Run("user_name", "me", kPlume).verdict == Verdict::kApply);
}

void TestChangedMeaningFlags() {
  const Decision scale = Run("resolution_scale", "2", kPlume);
  CHECK(scale.verdict == Verdict::kApply && scale.set.size() == 2);
  CHECK(scale.set[0].name == "gpu.resolution_scale_x" && scale.set[0].value == "2");
  CHECK(scale.set[1].name == "gpu.resolution_scale_y" && scale.set[1].value == "2");
  CHECK(Run("resolution_scale", std::nullopt, kPlume).verdict == Verdict::kBadUse);
  CHECK(Run("resolution_scale", "", kPlume).verdict == Verdict::kBadUse);
  CHECK(Run("no-resolution_scale", std::nullopt, kPlume).verdict == Verdict::kUnknown);
  const Decision sdl = Run("input_backend", "sdl", kPlume);
  CHECK(sdl.set.size() == 1 && sdl.set[0].name == "hid.drivers" && sdl.set[0].value == "sdl3");
  CHECK(Run("input_backend", "xinput", kPlume).set[0].value == "xinput");
  CHECK(Run("input_backend", "gamepad", kPlume).verdict == Verdict::kBadUse);
  CHECK(Run("input_backend", std::nullopt, kPlume).verdict == Verdict::kBadUse);
}

// What the two launchers pass: LARecomp's (Play: --game_data_root; setup: --build_shader_packs [--build_shader_packs_force]; the
// "More game options" box) and the fork's (the box, and before its settings moved into the game's own file: --fullscreen, --vsync,
// --resolution_scale, --mcla_* choices and four Xenia tuning flags).
void TestLauncherFlags() {
  // LARecomp's launcher: Play and the setup's shader step.
  const std::vector<std::string_view> no_actions;
  CHECK(RouteOf("game_data_root", no_actions) == Route::kSdk);
  CHECK(Run("build_shader_packs_force", std::nullopt, kBatch).verdict == Verdict::kApply);
  // ... and the city pack step (--build_city_pack, with its force switch): an action the SDK keeps, a setting for the force.
  const std::vector<std::string_view> setup_actions = {"build_shader_packs", "precache_pipelines", "build_city_pack"};
  CHECK(RouteOf("build_city_pack", setup_actions) == Route::kSdk && RouteOf("build_shader_packs", setup_actions) == Route::kSdk);
  CHECK(RouteOf("build_city_pack_force", setup_actions) == Route::kSetting);
  CHECK(Run("build_city_pack_force", std::nullopt, kBatch).verdict == Verdict::kApply);
  // Its "More options" box and the fork launcher's flags.
  CHECK(Run("log_file", "run.log", kPlume).verdict == Verdict::kApply);
  CHECK(Run("vsync", "false", kPlume).verdict == Verdict::kApply);
  CHECK(Run("fullscreen", "true", kPlume).verdict == Verdict::kApply);
  CHECK(Run("fullscreen", "false", kPlume).verdict == Verdict::kApply);
  CHECK(Run("resolution_scale", "3", kPlume).verdict == Verdict::kApply);
  CHECK(Run("mcla_fps", "30", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_intro", "skip", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_button_prompts", "xbox", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_ui_hud_rate", "60", kPlume).verdict == Verdict::kUnknown);
  CHECK(Run("mcla_prepare_shaders", "true", kPlume).verdict == Verdict::kUnknown);
}

// The two renderer switches of 0.10 are not settings any more (gpu.driver chooses the renderer) but stay names the registry ignores
// (an alias to ""): an old shortcut or launcher box with one of them must not stop the start, and must not be reported as a
// name nothing knows. The verdict is kApply: ApplyCommandLineSettings hands the name to the registry, which answers "<name> was
// removed and is ignored" and takes nothing.
void TestRemovedRendererSwitches() {
  for (const Selection* selection : {&kPlume, &kXeniaD3d12}) {
    const Decision off = Run("mcla_gpu", "false", *selection);
    CHECK(off.verdict == Verdict::kApply && off.set.size() == 1 && off.set[0].name == "mcla_gpu" && off.set[0].value == "false");
    CHECK(Run("mcla_gpu", std::nullopt, *selection).verdict == Verdict::kApply);
    CHECK(Run("no-mcla_gpu", std::nullopt, *selection).verdict == Verdict::kApply);
    CHECK(Run("mcla_native_gfx_nocp", "false", *selection).verdict == Verdict::kApply);
    CHECK(Run("no-mcla_native_gfx_nocp", std::nullopt, *selection).verdict == Verdict::kApply);
  }
  // Only the exact names: the settings that start the same way are still judged by their own rules.
  CHECK(Run("mcla_gpu_ao", "high", kPlume).verdict == Verdict::kApply);
  CHECK(Run("mcla_gpu_backend", "vulkan", kPlume).verdict == Verdict::kUnknown);
}

void TestSplit() {
  const std::vector<Option> options = {{"build_shader_packs", std::nullopt},
                                       {"game_data_root", "C:/Games/My Game"},
                                       {"vsync", "false"},
                                       {"larecomp_restart_wait_handle", "0x1F4"},
                                       {"config_file", std::nullopt},
                                       {"d3d12_adapter", "0"},
                                       {"larecomp_restart_wait_pid", "123"},
                                       {"precache_pipelines", "true"}};
  const std::vector<std::string> positionals = {"extra.xex"};
  const std::vector<std::string_view> actions = {"build_shader_packs", "precache_pipelines", "build_city_pack"};
  const Split split = SplitCommandLine(options, positionals, actions);
  // The SDK gets the actions, its path flags and the positionals, spelled as typed (a value with a blank stays one token).
  CHECK((split.sdk == std::vector<std::string>{"--build_shader_packs", "--game_data_root=C:/Games/My Game", "--precache_pipelines=true",
                                               "extra.xex"}));
  // LARecomp keeps the settings, in the order typed; its relaunch flags are dropped without a word.
  CHECK(split.settings.size() == 2 && split.settings[0].name == "vsync" && split.settings[1].name == "d3d12_adapter");
  // A path flag with no value would be "unknown option" in the SDK: dropped with one line.
  CHECK(split.notes.size() == 1);
  CHECK(split.notes[0] == "--config_file ignored: it needs a value (--config_file=<value>)");
  // The names of the SDK's own flags (app/command_line.cpp kFlags).
  for (const char* flag : {"modules", "game_data_root", "user_data_root", "update_data_root", "cache_root", "metadata_root", "config_file"}) {
    CHECK(RouteOf(flag, actions) == Route::kSdk);
  }
  CHECK(RouteOf("larecomp_restart_wait_handle", actions) == Route::kInternal);
  CHECK(RouteOf("vsync", actions) == Route::kSetting);
  CHECK(RouteOf("no-vsync", actions) == Route::kSetting);
}

void TestSelectors() {
  CHECK(IsSelectorSetting("gpu.driver") && IsSelectorSetting("apu.driver") && IsSelectorSetting("hid.drivers") &&
        IsSelectorSetting("ui.driver"));
  CHECK(!IsSelectorSetting("gpu.vsync") && !IsSelectorSetting("gpu.drivers"));
  // The command line wins, then the environment, then what the registry holds.
  CHECK(EffectiveValue("vulkan", "d3d12", "larecomp") == "vulkan");
  CHECK(EffectiveValue(std::nullopt, "d3d12", "larecomp") == "d3d12");
  CHECK(EffectiveValue(std::nullopt, std::nullopt, "larecomp") == "larecomp");
  const std::vector<Option> options = {{"gpu.driver", "a"}, {"vsync", "x"}, {"gpu.driver", "b"}, {"hid.drivers", std::nullopt}};
  CHECK(LastValueOf(options, "gpu.driver").value() == "b");
  CHECK(!LastValueOf(options, "hid.drivers").has_value());
  CHECK(!LastValueOf(options, "ui.driver").has_value());
}

void TestNames() {
  CHECK(IsSettingName("gpu.d3d12.adapter") && IsSettingName("ui.bind.mcla_settings") && IsSettingName("larecomp.mcla_gpu_ao"));
  CHECK(!IsSettingName("gpu") && !IsSettingName("gpu.") && !IsSettingName(".gpu") && !IsSettingName("gpu..x") &&
        !IsSettingName("Gpu.x") && !IsSettingName("1a.b") && !IsSettingName("gpu.x-y"));
  CHECK(IsNamePart("mcla_gpu_ao") && IsNamePart("a1") && !IsNamePart("") && !IsNamePart("mcla-gpu") && !IsNamePart("_x"));
  CHECK(Spell({"vsync", "false"}) == "--vsync=false" && Spell({"debug_menu", std::nullopt}) == "--debug_menu");
}

void TestSuggestions() {
  const std::vector<std::string> names = {"fps_limit", "mcla_gpu_ao", "vsync", "gpu.vsync", "skip_intro"};
  CHECK(ClosestName("mcla_gpu_aoo", names) == "mcla_gpu_ao");
  CHECK(ClosestName("vsynk", names) == "vsync");
  CHECK(ClosestName("skip_intr", names) == "skip_intro");
  CHECK(ClosestName("mcla_fps", names).empty());  // nothing near enough: no guess
  CHECK(ClosestName("x", names).empty());
  CHECK(EditDistance("kitten", "sitting") == 3 && EditDistance("", "abc") == 3 && EditDistance("same", "same") == 0);
}

}  // namespace

int main() {
  TestTheReportedCase();
  TestDriverScopes();
  TestPlainSettings();
  TestNamesNothingKnows();
  TestChangedMeaningFlags();
  TestLauncherFlags();
  TestRemovedRendererSwitches();
  TestSplit();
  TestSelectors();
  TestNames();
  TestSuggestions();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
