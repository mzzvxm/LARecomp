#pragma once
// ===========================================================================
// MCLA GPU renderer -- the graphics presets: the table, the names and the rules (GFX-PRESETS)
// ===========================================================================
// Named presets apply concrete values across Image Quality, Lighting and Shadows,
// Reflections and World Detail. Display (including AA), upscaling and debug controls
// are independent. GPU class and RT tier select the recommendation and the first-start preset.
//
// HOW A PRESET REACHES THE ROWS (gfx_preset_apply.h): the config keeps the preset's NAME. While a named preset is in force its values are held over the rows (the same hold the trailer
// scenes use, so they are never written to larecomp.toml); the player changing any covered row in the pause menu turns the preset into CUSTOM (the rows keep the values they had, and
// mcla_gpu_preset_from remembers the preset they came from, so rows added later take that preset's value); picking CUSTOM again from a preset the player was only trying
// brings back what they had. Command line and environment settings sit on top of a preset, are never saved and never turn it into CUSTOM.
//
// FIRST START (DecideInitial): without a saved preset or covered rows, use the recommendation. A config with covered rows and no preset starts on CUSTOM.
//
// This header includes nothing of Plume or ReXGlue: gfx_preset_test.cpp checks the table, tools\check_gfx_presets.py checks it against the menu's pages.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcla::gpu::gfx_preset {

// The order is the order of the pause row, the launcher's picker and presets.order in settings-ui.json.
enum class Preset : uint8_t { kVanilla, kEnhanced, kEnhancedRt, kCustom };
inline constexpr int kPresetCount = 4;
inline constexpr int kNamedCount = 3;  // the presets that hold values: every one but CUSTOM

constexpr bool IsNamed(Preset p) { return p != Preset::kCustom; }
constexpr bool NeedsRt(Preset p) { return p == Preset::kEnhancedRt; }

// The stored word (mcla_gpu_preset in larecomp.toml, the command line and settings-ui.json).
constexpr std::string_view Id(Preset p) {
  switch (p) {
    case Preset::kVanilla: return "vanilla";
    case Preset::kEnhanced: return "enhanced";
    case Preset::kEnhancedRt: return "enhanced_rt";
    default: return "custom";
  }
}

// The pause menu's words (the launcher shows them in Title Case).
constexpr std::string_view Label(Preset p) {
  switch (p) {
    case Preset::kVanilla: return "VANILLA";
    case Preset::kEnhanced: return "ENHANCED";
    case Preset::kEnhancedRt: return "ENHANCED RT";
    default: return "CUSTOM";
  }
}

// One plain sentence for the picker and the launcher (the player's words, no setting names).
constexpr std::string_view Help(Preset p) {
  switch (p) {
    case Preset::kVanilla:
      return "The game as it shipped on the console: none of the new effects, the original's look at your resolution and frame rate.";
    case Preset::kEnhanced:
      return "Enhanced graphics with screen-space effects.";
    case Preset::kEnhancedRt:
      return "Enhanced graphics with ray traced lighting and reflections.";
    default:
      return "Your own choices on the effect pages. Changing any single effect switches the preset to Custom.";
  }
}

// What a preset needs, as a settings-ui expression (null where it always works) and what the launcher says where it does not.
constexpr std::string_view Needs(Preset p) { return NeedsRt(p) ? std::string_view("gpu.rt_tier != 'none'") : std::string_view(); }
inline constexpr std::string_view kNeedsRtReason = "Needs a graphics card that can ray trace.";

inline bool Parse(std::string_view text, Preset* out) {
  if (!out) return false;
  for (int i = 0; i < kPresetCount; ++i) {
    if (text == Id(static_cast<Preset>(i))) {
      *out = static_cast<Preset>(i);
      return true;
    }
  }
  return false;
}

// Old saves and launch arguments may contain either of these retired preset IDs.
constexpr std::string_view MigrateId(std::string_view id) {
  if (id == "enhanced_ultra") return "enhanced";
  if (id == "enhanced_rt_ultra") return "enhanced_rt";
  return id;
}

// The pause row's own words, in the order of Preset (the row cycles them; settings-ui.json lists the same ids).
inline constexpr const char* kIds[kPresetCount] = {"vanilla", "enhanced", "enhanced_rt", "custom"};
inline constexpr const char* kLabels[kPresetCount] = {"VANILLA", "ENHANCED", "ENHANCED RT", "CUSTOM"};

// The two settings. mcla_gpu_preset is the player's; mcla_gpu_preset_from is where a CUSTOM started (a named preset, or `none`: CUSTOM from a file written before presets, whose rows
// that the file does not name stay at their own defaults, which is what they were).
inline constexpr std::string_view kPresetSetting = "mcla_gpu_preset";
inline constexpr std::string_view kFromSetting = "mcla_gpu_preset_from";
inline constexpr std::string_view kFromNone = "none";

// --- the table ---------------------------------------------------------------------------------------------------------------------

enum class Page : uint8_t { kImageQuality, kLighting, kReflections, kWorldDetail };

struct Row {
  std::string_view cvar;
  Page page;
  // VANILLA, ENHANCED, ENHANCED RT: the text the setting is written with.
  std::string_view cell[kNamedCount];
};

constexpr int Col(Preset p) { return static_cast<int>(p); }

// Concrete settings in menu order.
inline constexpr Row kRows[] = {
    {"mcla_gpu_anisotropy", Page::kImageQuality, {"game", "16", "16"}},  // owner 11-27: 16x on both ENHANCED presets (integrated GPUs start on VANILLA)
    {"disable_dof", Page::kImageQuality, {"false", "true", "true"}},
    {"disable_motion_blur", Page::kImageQuality, {"false", "false", "false"}},

    {"perf_no_shadows", Page::kLighting, {"false", "false", "false"}},
    {"mcla_gpu_shadow_resolution", Page::kLighting, {"game", "high", "high"}},
    {"mcla_gpu_shadow_filter", Page::kLighting, {"game", "pcss", "pcss"}},
    {"disable_imposter_shadows", Page::kLighting, {"false", "true", "true"}},
    {"mcla_gpu_traffic_shadows", Page::kLighting, {"off", "near", "near"}},
    {"mcla_gpu_car_shadow_map", Page::kLighting, {"off", "ultra", "ultra"}},
    {"mcla_gpu_contact_shadows", Page::kLighting, {"off", "high", "ultra"}},
    {"mcla_gpu_rt_shadows", Page::kLighting, {"off", "off", "high"}},
    {"mcla_gpu_ao", Page::kLighting, {"off", "on", "rt"}},
    {"mcla_gpu_ao_rt", Page::kLighting, {"off", "off", "both"}},
    {"mcla_gpu_ao_intensity", Page::kLighting, {"default", "default", "default"}},
    {"mcla_gpu_ssgi", Page::kLighting, {"off", "off", "high"}},
    {"mcla_gpu_rt_gi", Page::kLighting, {"off", "off", "high"}},
    {"mcla_gpu_ssgi_intensity", Page::kLighting, {"default", "default", "default"}},
    {"mcla_gpu_headlights", Page::kLighting, {"off", "on", "on"}},
    {"mcla_gpu_light_shadows", Page::kLighting, {"off", "medium", "ultra"}},
    {"mcla_gpu_rt_lights", Page::kLighting, {"off", "off", "on"}},
    {"mcla_gpu_neon_light", Page::kLighting, {"off", "on", "on"}},
    {"mcla_gpu_light_bounce", Page::kLighting, {"off", "on", "on"}},
    {"mcla_gpu_sun_shafts", Page::kLighting, {"off", "default", "default"}},
    {"mcla_gpu_volumetrics", Page::kLighting, {"off", "medium", "high"}},

    {"mcla_gpu_reflection_resolution", Page::kReflections, {"game", "high", "high"}},
    // CHECK: compare wet-road motion and pinned highlights with these preset levels.
    {"mcla_gpu_ssr", Page::kReflections, {"off", "high", "ultra"}},
    {"mcla_gpu_ssr_rt", Page::kReflections, {"off", "off", "high"}},
    {"mcla_gpu_env_resolution", Page::kReflections, {"off", "on", "on"}},
    {"mcla_gpu_car_selfrefl", Page::kReflections, {"off", "on", "on"}},
    {"mcla_gpu_car_selfrefl_rt", Page::kReflections, {"off", "off", "on"}},
    {"mcla_gpu_rt_reflections", Page::kReflections, {"off", "off", "on"}},
    {"mcla_gpu_side_mirrors", Page::kReflections, {"off", "on", "on"}},

    {"traffic_unspawn_dist", Page::kWorldDetail, {"400", "300", "400"}},
    {"ped_density_scale", Page::kWorldDetail, {"1", "0.75", "1"}},
    {"parked_car_scale", Page::kWorldDetail, {"1", "0.75", "1"}},
    {"global_max_draw_distance", Page::kWorldDetail, {"0", "0", "0"}},
    {"lod_traffic_scale", Page::kWorldDetail, {"1", "2", "3"}},
    {"lod_city_scale", Page::kWorldDetail, {"1", "2", "2"}},
};
inline constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// A cvar and a few words about it.
struct Note {
  std::string_view cvar;
  std::string_view text;
};

// A row that is on one of the four pages and that no preset covers, with the reason (the lint and the test make every row of the pages either covered or listed here).
inline constexpr Note kExempt[] = {
    {"anisotropic_override", "the Xenia renderer's own row: presets are the native renderer's"},
};
inline constexpr int kExemptCount = static_cast<int>(sizeof(kExempt) / sizeof(kExempt[0]));

// ENHANCED RT uses these levels below the row maximum. Traffic detail distance 3X is its ceiling.
inline constexpr Note kBelowMax[] = {
    {"mcla_gpu_shadow_resolution", "ultra"}, {"mcla_gpu_reflection_resolution", "ultra"},
    {"mcla_gpu_ssgi", "ultra"}, {"mcla_gpu_volumetrics", "ultra"}, {"mcla_gpu_traffic_shadows", "all"},
};
inline constexpr int kBelowMaxCount = static_cast<int>(sizeof(kBelowMax) / sizeof(kBelowMax[0]));

// Settings that belong to DISPLAY or UPSCALING: a preset never names one (the owner's scope rule: no change in the graphics section may affect the display section).
inline constexpr std::string_view kNeverCovered[] = {
    "fullscreen", "fullscreen_exclusive", "resolution", "fullscreen_resolution", "aspect_ratio", "display_gamma", "mcla_gpu_vsync", "fps_limit",
    "mcla_gpu_msaa", "mcla_gpu_aa", "disable_msaa", "mcla_gpu_low_latency", "mcla_gpu_hdr", "mcla_gpu_hdr_paper_white_nits", "mcla_gpu_hdr_peak_nits", "mcla_gpu_hdr_highlights",
    "mcla_gpu_upscaler", "mcla_gpu_fsr", "mcla_gpu_fsr_preset", "mcla_gpu_fsr31_mode", "mcla_gpu_fsr4_mode", "mcla_gpu_resolution_scale",
    "mcla_gpu_upscaler_sharpness", "mcla_gpu_fsr_sharpness", "mcla_gpu_frame_generation", "mcla_gpu_frame_generation_multiplier",
    "mcla_gpu_frame_generation_fps_limit", "mcla_gpu_vendor_denoiser",
};

// --- lookups -----------------------------------------------------------------------------------------------------------------------

// The row of a cvar, or -1.
constexpr int FindRow(std::string_view cvar) {
  for (int i = 0; i < kRowCount; ++i) {
    if (kRows[i].cvar == cvar) return i;
  }
  return -1;
}

// The row of a cvar the code names. In a constant expression an unknown name stops the build (the throw is not constant); at run time FindRow says -1.
constexpr int MustFindRow(std::string_view cvar) {
  const int row = FindRow(cvar);
  if (row < 0) throw "gfx_preset: the table has no row for this cvar";
  return row;
}

// The text of a preset's cell for a cvar; empty for CUSTOM and for a cvar that has no row.
constexpr std::string_view CellOf(std::string_view cvar, Preset p) {
  const int row = FindRow(cvar);
  if (row < 0 || !IsNamed(p)) return {};
  return kRows[row].cell[Col(p)];
}
constexpr std::string_view CellAt(int row, Preset p) { return kRows[row].cell[Col(p)]; }

// A legacy Auto car AO value follows the active preset or explicit Custom world RT.
constexpr std::string_view CarAoAuto(Preset preset, bool custom_world_rt) {
  return IsNamed(preset) ? CellOf("mcla_gpu_ao_rt", preset) : (custom_world_rt ? "both" : "off");
}

constexpr bool IsCovered(std::string_view cvar) { return FindRow(cvar) >= 0; }
constexpr bool IsKnownRow(std::string_view cvar) { return IsCovered(cvar); }

constexpr bool IsExempt(std::string_view cvar) {
  for (const Note& e : kExempt) {
    if (e.cvar == cvar) return true;
  }
  return false;
}
constexpr bool IsNeverCovered(std::string_view cvar) {
  for (const std::string_view name : kNeverCovered) {
    if (name == cvar) return true;
  }
  return false;
}

// The number a cell holds ("0.75", "400", "1"): digits and one point only (the table has no other number). The digits are one whole number and a power of ten, divided once, so the
// result is the nearest double to the text, exactly what the same literal in the source would be.
constexpr double Number(std::string_view text) {
  double digits = 0.0;
  double divisor = 1.0;
  bool after_point = false;
  for (const char ch : text) {
    if (ch == '.') {
      after_point = true;
    } else if (ch >= '0' && ch <= '9') {
      digits = digits * 10.0 + (ch - '0');
      if (after_point) divisor *= 10.0;
    } else {
      throw "gfx_preset: a cell that is not a plain number";
    }
  }
  return digits / divisor;
}

// --- the class -> preset map (first start and the "Recommended for this GPU" tag) -------------------------------------------------------
struct ClassRule {
  std::string_view when;
  Preset preset;
};
inline constexpr ClassRule kClassToPreset[] = {
    {"gpu.class == 'high' && gpu.rt_tier == 'strong'", Preset::kEnhancedRt},
    {"gpu.class == 'low'", Preset::kVanilla},
    {"true", Preset::kEnhanced},
};
inline constexpr int kClassRuleCount = static_cast<int>(sizeof(kClassToPreset) / sizeof(kClassToPreset[0]));
inline constexpr Preset kFallbackPreset = kClassToPreset[kClassRuleCount - 1].preset;

// --- first start and the older files ---------------------------------------------------------------------------------------------------

// Formerly covered AA and RT mirror keys can still signal an existing custom setup.
inline constexpr std::string_view kLegacyCoveredKeys[] = {"mcla_gpu_msaa", "mcla_gpu_aa", "mcla_gpu_car_mirror_rt"};

// What a larecomp.toml says about presets, from the keys it has.
struct FileFacts {
  bool has_preset_key = false;  // a mcla_gpu_preset line
  bool has_covered_key = false;  // a line for a setting a preset applies

  constexpr void NoteKey(std::string_view name) {
    if (name.starts_with("larecomp.")) name.remove_prefix(sizeof("larecomp.") - 1);
    if (name == kPresetSetting) has_preset_key = true;
    if (IsCovered(name)) has_covered_key = true;
    for (const auto key : kLegacyCoveredKeys) {
      if (name == key) has_covered_key = true;
    }
  }
};
// A preset key leaves the file in charge. Existing effect choices imply CUSTOM; an empty effect config uses the recommendation.
constexpr std::optional<Preset> DecideInitial(const FileFacts& file, Preset recommended) {
  if (file.has_preset_key) return std::nullopt;
  return file.has_covered_key ? Preset::kCustom : recommended;
}

// --- what the pause menu does with a change --------------------------------------------------------------------------------------------

// A covered row changed by the player while `current` is in force: the preset it turns into. A named preset becomes CUSTOM; CUSTOM stays; the change of the preset itself, of the
// `from` setting or of a row outside the table is not a change of a covered row.
constexpr Preset AfterRowChange(Preset current, std::string_view cvar) {
  if (!IsNamed(current) || !IsCovered(cvar)) return current;
  return Preset::kCustom;
}


// What picking a preset does to the values held over the rows (gfx_preset_apply.cpp does it; the decision is plain). `in_force` is the named preset whose values are held now, CUSTOM when
// none are; `had_own_values` is whether the rows had values of the player's own (from the file or set this session) when the first preset was put over them.
//   kOverlay   hold the named preset's values over the rows (the player's own stay underneath, unsaved changes included)
//   kRestore   CUSTOM picked again: the player's own values come back
//   kFreeze    CUSTOM picked from a preset when there were no values of their own: the rows keep what the preset gave them, as their own (mcla_gpu_preset_from remembers the preset)
enum class Action : uint8_t { kNone, kOverlay, kRestore, kFreeze };
constexpr Action OnPicked(Preset in_force, Preset picked, bool had_own_values) {
  if (picked == in_force) return Action::kNone;
  if (IsNamed(picked)) return Action::kOverlay;
  return had_own_values ? Action::kRestore : Action::kFreeze;
}

// The RT presets are offered where the card traces rays (gpu.rt_tier is not none); everywhere else they are not. The pause row also keeps listing the one that is in force (a larecomp.toml
// carried to a PC without ray tracing), so the row can say so ("(NO RT GPU)") instead of showing another preset's name.
constexpr bool Offered(Preset p, bool rt_available) { return !NeedsRt(p) || rt_available; }
constexpr bool Listed(Preset p, bool rt_available, Preset current) { return Offered(p, rt_available) || p == current; }

}  // namespace mcla::gpu::gfx_preset
