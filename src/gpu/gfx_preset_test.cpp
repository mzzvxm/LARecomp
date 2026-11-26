// Check of the graphics presets' table and rules (gfx_preset.h, and the `auto` tables that read it: gpu_class.h kEffects / kScales, rt_tier.h kRtEffects). A program of its own: it needs
// no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gfx_preset_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gfx_preset_test
//
// The expected values are written out here again on purpose, from the sources that decided them (the old `auto` tables, perf\LEGACY-GFX-AUDIT.md and the owner's calls on its grey cases):
// a cell changed in gfx_preset.h alone fails the check, and changing one on purpose means changing it here too. tools\check_gfx_presets.py is the other half: it reads the pause menu's
// pages and the settings' own lists.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <iterator>

#include "gfx_preset.h"
#include "gpu_class.h"
#include "rt_tier.h"
#include "city_rt_rules.h"
#include "gi_setting.h"
#include "rtao_setting.h"

namespace city_rt = mcla::gpu::city_rt;
namespace gi_setting = mcla::gpu::gi_setting;
namespace rtao_setting = mcla::gpu::rtao_setting;

using namespace mcla::gpu::gpu_class;
namespace gp = mcla::gpu::gfx_preset;
using gp::Preset;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckEq(std::string_view got, std::string_view want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%.*s', want '%.*s'\n", what.c_str(), static_cast<int>(got.size()), got.data(), static_cast<int>(want.size()), want.data());
  }
}

void CheckNum(double got, double want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g\n", what.c_str(), got, want);
  }
}

constexpr Preset kNamed[gp::kNamedCount] = {Preset::kVanilla, Preset::kEnhanced, Preset::kEnhancedRt};
constexpr GpuClass kClasses[3] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};

std::string Name(std::string_view cvar, Preset p) { return std::string(cvar) + " in " + std::string(gp::Id(p)); }

// --- the values each row can take (the settings' own lists and the pause menu's rows) -------------------------------------------------------------------------
struct Allowed {
  const char* cvar;
  const char* words;  // '|' separated
};
const Allowed kAllowed[] = {
    {"mcla_gpu_anisotropy", "game|off|2|4|8|16"},
    {"disable_dof", "true|false"},
    {"disable_motion_blur", "true|false"},
    {"perf_no_shadows", "true|false"},
    {"disable_imposter_shadows", "true|false"},
    {"mcla_gpu_shadow_resolution", "game|high|ultra"},
    {"mcla_gpu_shadow_filter", "game|soft|pcss"},
    {"mcla_gpu_contact_shadows", "off|high|ultra"},
    {"mcla_gpu_rt_shadows", "off|high|ultra"},
    {"mcla_gpu_traffic_shadows", "off|near|all"},
    {"mcla_gpu_car_shadow_map", "off|high|ultra"},
    {"mcla_gpu_ao", "off|on|rt"},
    {"mcla_gpu_ao_intensity", "light|default|strong|max"},
    {"mcla_gpu_ssgi", "off|high|ultra"},
    {"mcla_gpu_rt_gi", "off|high|ultra"},
    {"mcla_gpu_ssgi_intensity", "light|default|strong|stronger|strongest"},
    {"mcla_gpu_sun_shafts", "off|light|default|strong|stronger|strongest"},
    {"mcla_gpu_headlights", "off|on"},
    {"mcla_gpu_light_shadows", "off|medium|high|ultra"},
    {"mcla_gpu_neon_light", "off|on"},
    {"mcla_gpu_light_bounce", "off|on"},
    {"mcla_gpu_volumetrics", "off|medium|high|ultra"},
    {"mcla_gpu_reflection_resolution", "game|high|ultra"},
    {"mcla_gpu_env_resolution", "off|on"},
    {"mcla_gpu_car_selfrefl", "off|on"},
    {"mcla_gpu_car_selfrefl_rt", "off|on"},
    {"mcla_gpu_rt_reflections", "off|on"},
    {"mcla_gpu_side_mirrors", "off|on"},
    {"mcla_gpu_ssr", "off|high|ultra"},
    {"mcla_gpu_ssr_rt", "off|high|ultra"},
    {"mcla_gpu_rt_lights", "off|on"},
    {"traffic_unspawn_dist", "100|150|200|250|300|400|500|600"},
    {"ped_density_scale", "0|0.25|0.5|0.75|1|1.5|2"},
    {"parked_car_scale", "0|0.25|0.5|0.75|1|1.5|2"},
    {"global_max_draw_distance", "0|250|500|1000|1500"},
    {"lod_traffic_scale", "0.5|1|1.5|2|3|5"},
    {"lod_city_scale", "0.5|1|1.5|2"},
    {"mcla_gpu_ao_rt", "off|ao|contact|both"},
};

bool InList(std::string_view words, std::string_view cell) {
  std::string_view rest(words);
  while (true) {
    const std::size_t bar = rest.find('|');
    if (rest.substr(0, bar) == cell) return true;
    if (bar == std::string_view::npos) return false;
    rest.remove_prefix(bar + 1);
  }
}

// --- the table -------------------------------------------------------------------------------------------------------------------------------------------------
void CheckTable() {
  Check(gp::kRowCount == static_cast<int>(sizeof(kAllowed) / sizeof(kAllowed[0])), "every row has a list of the words its setting takes in this test");
  Check(gp::kPresetCount == 4 && gp::kNamedCount == 3, "four presets, three of them named");

  for (int i = 0; i < gp::kRowCount; ++i) {
    const gp::Row& row = gp::kRows[i];
    const std::string cvar(row.cvar);
    Check(!cvar.empty(), "row " + std::to_string(i) + " has a cvar");
    for (int k = i + 1; k < gp::kRowCount; ++k) {
      Check(gp::kRows[k].cvar != row.cvar, cvar + " is in the table once");
    }
    std::string_view allowed;
    for (const auto& spec : kAllowed) {
      if (row.cvar == spec.cvar) allowed = spec.words;
    }
    Check(!allowed.empty(), cvar + ": has a list of allowed values");
    for (const Preset p : kNamed) {
      const std::string_view cell = gp::CellOf(row.cvar, p);
      Check(!cell.empty(), Name(row.cvar, p) + ": has a cell (every covered row has a value in every preset)");
      Check(cell != "auto", Name(row.cvar, p) + ": a preset must use a concrete value");
      Check(InList(allowed, cell), Name(row.cvar, p) + ": '" + std::string(cell) + "' is a value the row takes");
      Check(cell.find("low") == std::string_view::npos, Name(row.cvar, p) + ": no setting has a level called low");
    }
    CheckEq(gp::CellOf(row.cvar, Preset::kCustom), "", cvar + ": CUSTOM has no cell of its own");
    Check(gp::FindRow(row.cvar) == i, cvar + ": FindRow");
    Check(gp::IsKnownRow(row.cvar), cvar + ": IsKnownRow");
    Check(gp::IsCovered(row.cvar), cvar + ": covered");
    Check(!gp::IsNeverCovered(row.cvar), cvar + ": is not a DISPLAY or UPSCALING setting (a preset never touches those)");
    Check(!gp::IsExempt(row.cvar), cvar + ": is covered, so it is not on the exempt list");
  }
  Check(gp::FindRow("fps_limit") == -1 && gp::FindRow("mcla_gpu_upscaler") == -1 && gp::FindRow("") == -1, "no DISPLAY or UPSCALING setting is a row");
  Check(!gp::IsCovered("mcla_gpu_preset") && !gp::IsCovered("mcla_gpu_preset_from"), "the preset's own settings are not covered rows");
  for (const std::string_view name : gp::kNeverCovered) {
    Check(gp::FindRow(name) < 0, std::string(name) + ": a DISPLAY or UPSCALING setting has no row");
  }

  Check(gp::kRowCount == 38, "all 38 native effects have preset cells");
  Check(gp::kExemptCount == 1, "the Xenia anisotropy row is outside native preset coverage");
  for (const gp::Note& e : gp::kExempt) {
    Check(gp::FindRow(e.cvar) < 0, std::string(e.cvar) + ": an exempt row is not in the table");
    Check(!e.text.empty(), std::string(e.cvar) + ": an exempt row says why");
  }
}

// --- VANILLA: the audit's values --------------------------------------------------------------------------------------------------------------------------------
struct Want {
  const char* cvar;
  const char* vanilla;
};
// perf\LEGACY-GFX-AUDIT.md, with the owner's calls on its grey cases (anisotropy: the game's own; anti-aliasing: the original's 2x MSAA; LOD, draw distance and street density: stock; the
// shadow detail: the game's own atlas); every new effect off.
const Want kVanilla[] = {
    {"mcla_gpu_anisotropy", "game"},
    {"disable_dof", "false"},                   // the game has depth of field
    {"disable_motion_blur", "false"},           // and motion blur
    {"perf_no_shadows", "false"},               // shadows on
    {"disable_imposter_shadows", "false"},      // the far trees' shadows are the game's own (and the fork's)
    {"mcla_gpu_shadow_resolution", "game"},
    {"mcla_gpu_shadow_filter", "game"},
    {"mcla_gpu_contact_shadows", "off"},
    {"mcla_gpu_rt_shadows", "off"},
    {"mcla_gpu_traffic_shadows", "off"},
    {"mcla_gpu_car_shadow_map", "off"},
    {"mcla_gpu_ao", "off"},
    {"mcla_gpu_ao_intensity", "default"},
    {"mcla_gpu_ssgi", "off"},
    {"mcla_gpu_rt_gi", "off"},
    {"mcla_gpu_ssgi_intensity", "default"},
    {"mcla_gpu_sun_shafts", "off"},
    {"mcla_gpu_headlights", "off"},
    {"mcla_gpu_light_shadows", "off"},
    {"mcla_gpu_neon_light", "off"},
    {"mcla_gpu_light_bounce", "off"},
    {"mcla_gpu_volumetrics", "off"},
    {"mcla_gpu_reflection_resolution", "game"},
    {"mcla_gpu_env_resolution", "off"},
    {"mcla_gpu_car_selfrefl", "off"},
    {"mcla_gpu_car_selfrefl_rt", "off"},
    {"mcla_gpu_rt_reflections", "off"},
    {"mcla_gpu_side_mirrors", "off"},
    {"mcla_gpu_ssr", "off"},
    {"mcla_gpu_ssr_rt", "off"},
    {"mcla_gpu_rt_lights", "off"},
    {"traffic_unspawn_dist", "400"},            // the game's own range
    {"ped_density_scale", "1"},                 // the game's own density
    {"parked_car_scale", "1"},
    {"global_max_draw_distance", "0"},          // STOCK
    {"lod_traffic_scale", "1"},                 // the game's own distances
    {"lod_city_scale", "1"},
    {"mcla_gpu_ao_rt", "off"},
};

void CheckVanilla() {
  Check(sizeof(kVanilla) / sizeof(kVanilla[0]) == static_cast<std::size_t>(gp::kRowCount), "VANILLA is checked for every row");
  for (const Want& w : kVanilla) {
    CheckEq(gp::CellOf(w.cvar, Preset::kVanilla), w.vanilla, Name(w.cvar, Preset::kVanilla));
  }
  // The new effects off, the look-changing rows at the original's value: nothing in VANILLA is above the original.
  for (const gp::Row& row : gp::kRows) {
    const std::string_view cell = row.cell[gp::Col(Preset::kVanilla)];
    if (row.page == gp::Page::kLighting || row.page == gp::Page::kReflections) {
      const bool off_or_game = cell == "off" || cell == "game" || cell == "default" || cell == "false";
      Check(off_or_game, Name(row.cvar, Preset::kVanilla) + ": a lighting or reflection row is off or the game's own");
    }
  }
}

// Pin both enhanced columns to the approved values, independently of runtime resolution.
struct EnhancedValues {
  const char* cvar;
  const char* enhanced;
  const char* enhanced_rt;
};
constexpr EnhancedValues kEnhanced[] = {
    {"mcla_gpu_anisotropy", "game", "16"},
    {"disable_dof", "true", "true"},
    {"disable_motion_blur", "false", "false"},
    {"perf_no_shadows", "false", "false"},
    {"mcla_gpu_shadow_resolution", "high", "high"},
    {"mcla_gpu_shadow_filter", "pcss", "pcss"},
    {"disable_imposter_shadows", "true", "true"},
    {"mcla_gpu_traffic_shadows", "near", "near"},
    {"mcla_gpu_car_shadow_map", "ultra", "ultra"},
    {"mcla_gpu_contact_shadows", "high", "ultra"},
    {"mcla_gpu_rt_shadows", "off", "high"},
    {"mcla_gpu_ao", "on", "rt"},
    {"mcla_gpu_ao_rt", "off", "both"},
    {"mcla_gpu_ao_intensity", "default", "default"},
    {"mcla_gpu_ssgi", "off", "high"},
    {"mcla_gpu_rt_gi", "off", "high"},
    {"mcla_gpu_ssgi_intensity", "default", "default"},
    {"mcla_gpu_headlights", "on", "on"},
    {"mcla_gpu_light_shadows", "medium", "ultra"},
    {"mcla_gpu_rt_lights", "off", "on"},
    {"mcla_gpu_neon_light", "on", "on"},
    {"mcla_gpu_light_bounce", "on", "on"},
    {"mcla_gpu_sun_shafts", "default", "default"},
    {"mcla_gpu_volumetrics", "medium", "high"},
    {"mcla_gpu_reflection_resolution", "high", "high"},
    {"mcla_gpu_ssr", "high", "ultra"},
    {"mcla_gpu_ssr_rt", "off", "high"},
    {"mcla_gpu_env_resolution", "on", "on"},
    {"mcla_gpu_car_selfrefl", "on", "on"},
    {"mcla_gpu_car_selfrefl_rt", "off", "on"},
    {"mcla_gpu_rt_reflections", "off", "on"},
    {"mcla_gpu_side_mirrors", "on", "on"},
    {"traffic_unspawn_dist", "300", "400"},
    {"ped_density_scale", "0.75", "1"},
    {"parked_car_scale", "0.75", "1"},
    {"global_max_draw_distance", "0", "0"},
    {"lod_traffic_scale", "2", "3"},
    {"lod_city_scale", "2", "2"},
};

void CheckCarAoAuto() {
  for (const bool world_rt : {false, true}) {
    CheckEq(gp::CarAoAuto(Preset::kVanilla, world_rt), "off", "Vanilla car AO Auto selects no traced terms");
    CheckEq(gp::CarAoAuto(Preset::kEnhanced, world_rt), "off", "Enhanced car AO Auto selects no traced terms");
    CheckEq(gp::CarAoAuto(Preset::kEnhancedRt, world_rt), "both", "Enhanced RT car AO Auto selects both traced terms");
  }
  CheckEq(gp::CarAoAuto(Preset::kCustom, false), "off", "Custom raster AO selects no traced car terms");
  CheckEq(gp::CarAoAuto(Preset::kCustom, true), "both", "Custom world RT AO selects both traced car terms");
}

void CheckEnhanced() {
  Check(std::size(kEnhanced) == gp::kRowCount, "both enhanced columns are covered");
  for (const auto& row : kEnhanced) {
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhanced), row.enhanced, Name(row.cvar, Preset::kEnhanced));
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhancedRt), row.enhanced_rt, Name(row.cvar, Preset::kEnhancedRt));
  }
}

void CheckNoClassDependence() {
  for (const auto cls : kClasses) {
    for (int i = 0; i < kEffectCount; ++i) {
      const auto effect = static_cast<Effect>(i);
      CheckEq(ResolveFor(cls, false, effect, "auto"), Cell(effect, GpuClass::kHigh), std::string(CvarName(effect)));
      CheckEq(ResolveFor(cls, true, effect, "auto"), Cell(effect, GpuClass::kHigh), std::string(CvarName(effect)) + " on a handheld");
    }
    for (int i = 0; i < kScaleCount; ++i) {
      const auto scale = static_cast<Scale>(i);
      const auto want = ScaleAutoFor(scale, true, GpuClass::kHigh);
      CheckNum(ScaleAutoFor(scale, true, cls), want, std::string(ScaleCvar(scale)));
      CheckNum(ScaleAutoFor(scale, false, cls), want, std::string(ScaleCvar(scale)) + " before the probe");
    }
    for (const auto tier : {RtTier::kWeak, RtTier::kStrong}) {
      for (const auto word : {"off", "high", "ultra"}) {
        const auto shadow = city_rt::ParseChoice(word);
        const auto level = city_rt::LevelForRung(cls, tier, shadow);
        const auto baseline = city_rt::LevelForRung(GpuClass::kHigh, RtTier::kStrong, shadow);
        Check(level == baseline, "city shadow level");
        const auto got = city_rt::BudgetFor(cls, level);
        const auto want = city_rt::BudgetFor(GpuClass::kHigh, baseline);
        Check(got.hi_radius == want.hi_radius && got.far_radius == want.far_radius && got.hysteresis == want.hysteresis &&
              got.memory_cap == want.memory_cap && got.build_triangles == want.build_triangles &&
              got.loads_in_flight == want.loads_in_flight && got.upload_bytes == want.upload_bytes, "city shadow budget");
      }
      const auto gi = gi_setting::Decide(cls, tier, "ultra", true);
      Check(gi.layout == mcla::gpu::gi::Layout::kExtended && mcla::gpu::gi::PixelPassAllowed(gi.rung, gi.layout), "GI Ultra");
      const auto ao = rtao_setting::Decide(cls, true, tier, rtao_setting::Choice::kRt, true);
      Check(ao.rt, "RT AO capability");
    }
  }
}

void CheckRetiredIds() {
  CheckEq(gp::MigrateId("enhanced_ultra"), "enhanced", "retired Enhanced Ultra ID");
  CheckEq(gp::MigrateId("enhanced_rt_ultra"), "enhanced_rt", "retired Enhanced RT Ultra ID");
  for (const auto id : gp::kIds) CheckEq(gp::MigrateId(id), id, "current ID stays canonical");
  CheckEq(gp::MigrateId("none"), "none", "custom without a source preset");
  CheckEq(gp::MigrateId("other"), "other", "unknown IDs stay available for validation");
  Preset parsed;
  Check(!gp::Parse("enhanced_ultra", &parsed) && !gp::Parse("enhanced_rt_ultra", &parsed), "retired IDs are absent from the picker");
}

// --- the numbers ---------------------------------------------------------------------------------------------------------------------------------------------------
void CheckNumber() {
  CheckNum(gp::Number("0"), 0.0, "0");
  CheckNum(gp::Number("1"), 1.0, "1");
  CheckNum(gp::Number("400"), 400.0, "400");
  CheckNum(gp::Number("0.75"), 0.75, "0.75");
  CheckNum(gp::Number("0.5"), 0.5, "0.5");
  CheckNum(gp::Number("1.5"), 1.5, "1.5");
  CheckNum(gp::Number("0.25"), 0.25, "0.25");
  CheckNum(gp::Number("2"), 2.0, "2");
  CheckNum(gp::Number("0.1"), 0.1, "0.1 is the nearest double to the text, as the literal is");
}

// --- the names ---------------------------------------------------------------------------------------------------------------------------------------------------------
void CheckNames() {
  const char* const ids[] = {"vanilla", "enhanced", "enhanced_rt", "custom"};
  const char* const labels[] = {"VANILLA", "ENHANCED", "ENHANCED RT", "CUSTOM"};
  for (int i = 0; i < gp::kPresetCount; ++i) {
    const Preset p = static_cast<Preset>(i);
    CheckEq(gp::Id(p), ids[i], "the id of preset " + std::to_string(i));
    CheckEq(gp::Label(p), labels[i], "the label of preset " + std::to_string(i));
    CheckEq(gp::kIds[i], ids[i], "kIds " + std::to_string(i));
    CheckEq(gp::kLabels[i], labels[i], "kLabels " + std::to_string(i));
    Preset back = Preset::kCustom;
    Check(gp::Parse(gp::Id(p), &back) && back == p, "Parse(Id) round trips for " + std::string(ids[i]));
    Check(!gp::Help(p).empty(), "preset " + std::string(ids[i]) + " has a help line");
    Check(gp::Label(p).size() <= 17, "the label of " + std::string(ids[i]) + " fits the pause row's value width (17)");
    Check(gp::Help(p).find("low") == std::string_view::npos && gp::Label(p).find("LOW") == std::string_view::npos, "no preset text says low");
  }
  Preset out = Preset::kVanilla;
  Check(!gp::Parse("", &out) && !gp::Parse("auto", &out) && !gp::Parse("Vanilla", &out) && !gp::Parse("enhanced ultra", &out) && !gp::Parse("none", &out), "Parse takes the stored words only");
  CheckEq(gp::kPresetSetting, "mcla_gpu_preset", "the preset setting's name");
  CheckEq(gp::kFromSetting, "mcla_gpu_preset_from", "the from setting's name");
  Check(gp::IsNamed(Preset::kVanilla) && gp::IsNamed(Preset::kEnhancedRt) && !gp::IsNamed(Preset::kCustom), "every preset but CUSTOM is named");
}

// --- RT presets need an RT tier ----------------------------------------------------------------------------------------------------------------------------------------
void CheckRtNeeds() {
  for (int i = 0; i < gp::kPresetCount; ++i) {
    const Preset p = static_cast<Preset>(i);
    const bool rt = p == Preset::kEnhancedRt;
    Check(gp::NeedsRt(p) == rt, "only Enhanced RT need ray tracing: " + std::string(gp::Id(p)));
    CheckEq(gp::Needs(p), rt ? "gpu.rt_tier != 'none'" : "", "the expression that offers " + std::string(gp::Id(p)));
    Check(gp::Offered(p, true), std::string(gp::Id(p)) + " is offered where the card traces rays");
    Check(gp::Offered(p, false) == !rt, std::string(gp::Id(p)) + " is offered without ray tracing only if it is not an RT preset");
    // The row keeps listing the one in force, so a larecomp.toml carried to a PC without ray tracing does not show another preset's name.
    for (int k = 0; k < gp::kPresetCount; ++k) {
      const Preset current = static_cast<Preset>(k);
      Check(gp::Listed(p, false, current) == (!rt || p == current), std::string(gp::Id(p)) + " listed without ray tracing while " + std::string(gp::Id(current)) + " is in force");
    }
  }
  Check(!gp::kNeedsRtReason.empty(), "the RT presets say why they are not offered");
}

// --- the class -> preset map ------------------------------------------------------------------------------------------------------------------------------------------------
void CheckRecommendation() {
  Check(gp::kClassRuleCount == 3, "there are three recommendation rules");
  CheckEq(gp::kClassToPreset[0].when, "gpu.class == 'high' && gpu.rt_tier == 'strong'", "the exported RT recommendation");
  Check(gp::kClassToPreset[0].preset == Preset::kEnhancedRt, "the exported RT preset");
  CheckEq(gp::kClassToPreset[1].when, "gpu.class == 'low'", "the exported Low recommendation");
  Check(gp::kClassToPreset[1].preset == Preset::kVanilla, "the exported Low preset");
  CheckEq(gp::kClassToPreset[2].when, "true", "the exported fallback is unconditional");
  Check(gp::kFallbackPreset == Preset::kEnhanced, "the exported fallback preset");

  constexpr std::optional<GpuClass> classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh, std::nullopt};
  constexpr std::optional<RtTier> tiers[] = {RtTier::kNone, RtTier::kWeak, RtTier::kStrong, std::nullopt};
  constexpr Preset expected[][4] = {
      {Preset::kVanilla, Preset::kVanilla, Preset::kVanilla, Preset::kVanilla},
      {Preset::kEnhanced, Preset::kEnhanced, Preset::kEnhanced, Preset::kEnhanced},
      {Preset::kEnhanced, Preset::kEnhanced, Preset::kEnhancedRt, Preset::kEnhanced},
      {Preset::kEnhanced, Preset::kEnhanced, Preset::kEnhanced, Preset::kEnhanced},
  };
  for (size_t i = 0; i < std::size(classes); ++i) {
    for (size_t j = 0; j < std::size(tiers); ++j) {
      const auto cls = classes[i];
      const auto tier = tiers[j];
      const std::string label = std::string(cls ? ClassName(*cls) : "unknown") + " / " +
                                std::string(tier ? RtTierName(*tier) : "unknown");
      const Preset recommended = RecommendedPreset(cls, tier);
      Check(recommended == expected[i][j], "recommendation: " + label);
      Check(gp::DecideInitial({}, recommended) == expected[i][j], "first start: " + label);
    }
  }
}

// --- first start, an older file, the CUSTOM flip, picking a preset ------------------------------------------------------------------------------------------------------------
void CheckFirstStart() {
  constexpr Preset recommended = Preset::kEnhancedRt;
  gp::FileFacts older;
  older.NoteKey("mcla_gpu_ao");
  older.NoteKey("mcla_gpu_ao_rt");
  Check(gp::DecideInitial(older, recommended) == Preset::kCustom, "a covered setting followed by a support setting still starts on CUSTOM");
  older.NoteKey("mcla_gpu_preset");
  Check(!gp::DecideInitial(older, recommended), "an explicit preset suppresses the CUSTOM migration");
  gp::FileFacts only_preset;
  only_preset.NoteKey("mcla_gpu_preset");
  Check(!gp::DecideInitial(only_preset, recommended), "a file with only the preset line: the line decides");
  only_preset.NoteKey("mcla_gpu_ao");
  Check(!gp::DecideInitial(only_preset, recommended), "an explicit preset suppresses migration even before a covered key");
  for (const gp::Row& row : gp::kRows) {
    gp::FileFacts facts;
    facts.NoteKey(row.cvar);
    Check(gp::DecideInitial(facts, recommended) == Preset::kCustom, std::string(row.cvar) + ": covered key triggers legacy CUSTOM");
  }
  for (const auto key : {"mcla_gpu_msaa", "mcla_gpu_aa", "larecomp.mcla_gpu_aa",
                         "mcla_gpu_car_mirror_rt", "larecomp.mcla_gpu_car_mirror_rt"}) {
    gp::FileFacts facts;
    facts.NoteKey(key);
    Check(gp::DecideInitial(facts, recommended) == Preset::kCustom, "a formerly covered key preserves an old config");
    facts.NoteKey("larecomp.mcla_gpu_preset");
    Check(!gp::DecideInitial(facts, recommended), "a scoped preset overrides legacy config inference");
  }
  for (const auto key : {"fullscreen", "mcla_gpu_upscaler", "language"}) {
    gp::FileFacts facts;
    facts.NoteKey(key);
    Check(gp::DecideInitial(facts, recommended) == recommended, std::string(key) + ": outside preset coverage");
  }
}

void CheckFlip() {
  for (int i = 0; i < gp::kRowCount; ++i) {
    const gp::Row& row = gp::kRows[i];
    for (int k = 0; k < gp::kPresetCount; ++k) {
      const Preset current = static_cast<Preset>(k);
      const Preset after = gp::AfterRowChange(current, row.cvar);
      if (current == Preset::kCustom) {
        Check(after == current, std::string(row.cvar) + " changed on " + std::string(gp::Id(current)) + ": nothing to flip");
      } else {
        Check(after == Preset::kCustom, std::string(row.cvar) + " changed on " + std::string(gp::Id(current)) + ": the preset becomes CUSTOM");
      }
    }
  }
  for (const std::string_view name : gp::kNeverCovered) {
    for (int k = 0; k < gp::kPresetCount; ++k) {
      const Preset current = static_cast<Preset>(k);
      Check(gp::AfterRowChange(current, name) == current, std::string(name) + " (DISPLAY or UPSCALING) changed on " + std::string(gp::Id(current)) + ": the preset stays");
    }
  }
  for (const gp::Note& e : gp::kExempt) {
    Check(gp::AfterRowChange(Preset::kEnhancedRt, e.cvar) == Preset::kEnhancedRt, std::string(e.cvar) + " changed: an exempt row does not flip the preset");
  }
  Check(gp::AfterRowChange(Preset::kEnhanced, "mcla_gpu_preset") == Preset::kEnhanced && gp::AfterRowChange(Preset::kEnhanced, "mcla_gpu_preset_from") == Preset::kEnhanced,
        "the preset's own settings do not flip it");
  Check(gp::AfterRowChange(Preset::kEnhanced, "language") == Preset::kEnhanced && gp::AfterRowChange(Preset::kVanilla, "") == Preset::kVanilla, "a setting outside the table does not flip it");
}

void CheckPicked() {
  for (int a = 0; a < gp::kPresetCount; ++a) {
    for (int b = 0; b < gp::kPresetCount; ++b) {
      const Preset in_force = static_cast<Preset>(a);
      const Preset picked = static_cast<Preset>(b);
      for (const bool own : {false, true}) {
        const gp::Action got = gp::OnPicked(in_force, picked, own);
        const std::string at = std::string(gp::Id(in_force)) + " -> " + std::string(gp::Id(picked)) + (own ? " (own values)" : " (no own values)");
        gp::Action want = gp::Action::kNone;
        if (in_force != picked) {
          if (picked != Preset::kCustom) {
            want = gp::Action::kOverlay;
          } else {
            want = own ? gp::Action::kRestore : gp::Action::kFreeze;
          }
        }
        Check(got == want, at);
      }
    }
  }
  // Trying a preset and coming back to CUSTOM gives the player's own values back; CUSTOM after a first-start preset keeps what that preset gave.
  Check(gp::OnPicked(Preset::kCustom, Preset::kEnhancedRt, true) == gp::Action::kOverlay, "CUSTOM -> a preset holds its values over the rows");
  Check(gp::OnPicked(Preset::kEnhancedRt, Preset::kCustom, true) == gp::Action::kRestore, "a preset -> CUSTOM, with own values: they come back");
  Check(gp::OnPicked(Preset::kVanilla, Preset::kCustom, false) == gp::Action::kFreeze, "VANILLA -> CUSTOM on a fresh install: the rows keep VANILLA's values as the player's own");
}

// --- DISPLAY and UPSCALING are never touched ---------------------------------------------------------------------------------------------------------------------------------
void CheckScope() {
  for (const std::string_view name : gp::kNeverCovered) {
    for (const Preset p : kNamed) {
      CheckEq(gp::CellOf(name, p), "", std::string(name) + ": no preset has a value for it");
    }
    Check(!gp::IsCovered(name) && !gp::IsKnownRow(name), std::string(name) + ": not covered, not known");
  }
  int per_page[4] = {};
  for (const gp::Row& row : gp::kRows) ++per_page[static_cast<int>(row.page)];
  Check(per_page[0] == 3, "Image Quality has three preset rows");
  Check(per_page[1] == 21, "Lighting and Shadows has twenty-one preset rows");
  Check(per_page[2] == 8, "Reflections has eight preset rows");
  Check(per_page[3] == 6, "World Detail has six preset rows");
}

}  // namespace

int main() {
  CheckTable();
  CheckVanilla();
  CheckEnhanced();
  CheckCarAoAuto();
  CheckNoClassDependence();
  CheckRetiredIds();
  CheckNumber();
  CheckNames();
  CheckRtNeeds();
  CheckRecommendation();
  CheckFirstStart();
  CheckFlip();
  CheckPicked();
  CheckScope();
  if (g_failed != 0) {
    std::printf("gfx_preset_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("gfx_preset_test: %d checks passed\n", g_checks);
  return 0;
}
