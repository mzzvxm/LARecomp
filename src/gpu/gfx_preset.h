#pragma once
// ===========================================================================
// MCLA GPU renderer -- the graphics presets: the table, the names and the rules (GFX-PRESETS)
// ===========================================================================
// Six presets (the owner's names, 2026-11-22/23): VANILLA, ENHANCED, ENHANCED ULTRA, ENHANCED RT, ENHANCED RT ULTRA and CUSTOM. A preset is ONE fixed value for each row of the four
// effect pages (IMAGE QUALITY, LIGHTING AND SHADOWS, REFLECTIONS, WORLD DETAIL), the same on every graphics card. kRows below is that table. It is the only place those numbers are
// written down: gpu_class.h's `auto` cells (kEffects, kScales) and rt_tier.h's (kRtEffects) are READ from it, the pause menu applies it, the settings-ui.json export prints it and
// the launcher draws it.
//
//   VANILLA            the console game's look at the player's resolution and frame rate: the fixes and the speed-ups stay, every look-changing row is at its original value
//                      (perf\LEGACY-GFX-AUDIT.md; the owner's calls on the grey cases: the game's own anisotropy, 2x MSAA, the stock LOD, draw distance and street density,
//                      the game's own shadow atlas). It is what a fresh install and a migrated fork start on.
//   ENHANCED           every row at the `auto` of a mid-range card (the Mid class), the ray traced rows off.
//   ENHANCED ULTRA     the high-end card's `auto` (the High class), the ray traced rows off.
//   ENHANCED RT        ENHANCED with the ray traced rows at what `auto` gives a card that traces rays at the weak tier (the mid-range RT cards).
//   ENHANCED RT ULTRA  ENHANCED ULTRA with the ray traced rows at what `auto` gives a strong card.
//   CUSTOM             no values of its own: the player's own rows (and the preset they started from, mcla_gpu_preset_from).
// The two RT presets are offered only where the card traces rays (gpu.rt_tier is not none).
//
// WHAT THE NUMBERS ARE TODAY. The mechanism lands before the final optimisation pass (perf\BACKLOG-opt.md GFX-OPT-PASS), so every cell of ENHANCED, ENHANCED ULTRA, ENHANCED RT and
// ENHANCED RT ULTRA is what `auto` gave that class before this table existed (gpu_class.h kEffects / kScales, rt_tier.h kRtEffects, the defaults of the rows that have no `auto`);
// gfx_preset_test.cpp pins every one of them against the old literals. Two things follow, and the final pass decides both:
//   * ENHANCED ULTRA is not literally "every row at its highest level": where the High class's `auto` is below the row's top level, the cell holds that (kBelowMax lists the rows).
//     Raising them would raise the `auto` of every High card, which the owner's "nothing changes for Mz or the testers" does not allow before the cost pass has run.
//     REFLECTIONS ON SURFACES is off in EVERY preset, the two ULTRA ones included, and its `auto` is off on every class: the owner saw the wet-road reflections flicker and
//     look unstable in motion (2026-11-24, run integ8-ssr-show-rain; perf/scripts/chain-ssr-motion.ps1 measures it). It is on the below-max list until that is fixed; gpu_class.h
//     keeps its class cell as a literal.
//   * A value a PRESET row holds is always a word or number the row accepts and never the `auto` of a quality (the one `auto` in the table is the ray traced half of CAR SELF-REFLECTIONS,
//     whose setting has no `on`): a preset on a card that cannot do the effect is still clamped by the effect's own capability
//     rules (no ray tracing hardware, the memory budgets, D3D12-only features). Those limits are outside the presets.
//
// HOW A PRESET REACHES THE ROWS (gfx_preset_apply.h): the config keeps the preset's NAME. While a named preset is in force its values are held over the rows (the same hold the trailer
// scenes use, so they are never written to larecomp.toml); the player changing any covered row in the pause menu turns the preset into CUSTOM (the rows keep the values they had, and
// mcla_gpu_preset_from remembers the preset they came from, so rows added later take that preset's value); picking CUSTOM again from a preset the player was only trying
// brings back what they had. Command line and environment settings sit on top of a preset, are never saved and never turn it into CUSTOM.
//
// FIRST START (DecideInitial): no larecomp.toml, or one with no covered row in it: VANILLA. A larecomp.toml that has covered rows and no mcla_gpu_preset line (every file written
// before presets): CUSTOM, so nothing changes for the owner, Mz or the testers.
//
// This header includes nothing of Plume or ReXGlue: gfx_preset_test.cpp checks the table, tools\check_gfx_presets.py checks it against the menu's pages.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcla::gpu::gfx_preset {

// The order is the order of the pause row, the launcher's picker and presets.order in settings-ui.json.
enum class Preset : uint8_t { kVanilla, kEnhanced, kEnhancedUltra, kEnhancedRt, kEnhancedRtUltra, kCustom };
inline constexpr int kPresetCount = 6;
inline constexpr int kNamedCount = 5;  // the presets that hold values: every one but CUSTOM

constexpr bool IsNamed(Preset p) { return p != Preset::kCustom; }
constexpr bool NeedsRt(Preset p) { return p == Preset::kEnhancedRt || p == Preset::kEnhancedRtUltra; }

// The stored word (mcla_gpu_preset in larecomp.toml, the command line and settings-ui.json).
constexpr std::string_view Id(Preset p) {
  switch (p) {
    case Preset::kVanilla: return "vanilla";
    case Preset::kEnhanced: return "enhanced";
    case Preset::kEnhancedUltra: return "enhanced_ultra";
    case Preset::kEnhancedRt: return "enhanced_rt";
    case Preset::kEnhancedRtUltra: return "enhanced_rt_ultra";
    default: return "custom";
  }
}

// The pause menu's words (the launcher shows them in Title Case).
constexpr std::string_view Label(Preset p) {
  switch (p) {
    case Preset::kVanilla: return "VANILLA";
    case Preset::kEnhanced: return "ENHANCED";
    case Preset::kEnhancedUltra: return "ENHANCED ULTRA";
    case Preset::kEnhancedRt: return "ENHANCED RT";
    case Preset::kEnhancedRtUltra: return "ENHANCED RT ULTRA";
    default: return "CUSTOM";
  }
}

// One plain sentence for the picker and the launcher (the player's words, no setting names).
constexpr std::string_view Help(Preset p) {
  switch (p) {
    case Preset::kVanilla:
      return "The game as it shipped on the console: none of the new effects, the original's look at your resolution and frame rate.";
    case Preset::kEnhanced:
      return "The new effects at the settings that suit a mid-range graphics card. No ray tracing.";
    case Preset::kEnhancedUltra:
      return "The new effects at the settings that suit a high-end graphics card. No ray tracing.";
    case Preset::kEnhancedRt:
      return "Enhanced, with ray traced effects at the settings that suit a mid-range ray tracing card.";
    case Preset::kEnhancedRtUltra:
      return "Enhanced Ultra, with ray traced effects at the settings that suit a high-end ray tracing card.";
    default:
      return "Your own choices on the effect pages. Changing any single effect switches the preset to Custom.";
  }
}

// What a preset needs, as a settings-ui expression (null where it always works) and what the launcher says where it does not.
constexpr std::string_view Needs(Preset p) { return NeedsRt(p) ? std::string_view("gpu.rt_tier != 'none'") : std::string_view(); }
inline constexpr std::string_view kNeedsRtReason = "Needs a graphics card that can ray trace.";

inline bool Parse(std::string_view text, Preset* out) {
  for (int i = 0; i < kPresetCount; ++i) {
    if (text == Id(static_cast<Preset>(i))) {
      *out = static_cast<Preset>(i);
      return true;
    }
  }
  return false;
}

// The pause row's own words, in the order of Preset (the row cycles them; settings-ui.json lists the same ids).
inline constexpr const char* kIds[kPresetCount] = {"vanilla", "enhanced", "enhanced_ultra", "enhanced_rt", "enhanced_rt_ultra", "custom"};
inline constexpr const char* kLabels[kPresetCount] = {"VANILLA", "ENHANCED", "ENHANCED ULTRA", "ENHANCED RT", "ENHANCED RT ULTRA", "CUSTOM"};

// The two settings. mcla_gpu_preset is the player's; mcla_gpu_preset_from is where a CUSTOM started (a named preset, or `none`: CUSTOM from a file written before presets, whose rows
// that the file does not name stay at their own defaults, which is what they were).
inline constexpr std::string_view kPresetSetting = "mcla_gpu_preset";
inline constexpr std::string_view kFromSetting = "mcla_gpu_preset_from";
inline constexpr std::string_view kFromNone = "none";
inline constexpr Preset kFirstStart = Preset::kVanilla;

// --- the table ---------------------------------------------------------------------------------------------------------------------

// Where a row is. kSupport rows are not on a page: they hold a cell that an `auto` table reads (the diagnostic cvar mcla_gpu_ao_rt), no preset applies them and changing them
// does not turn a preset into CUSTOM.
enum class Page : uint8_t { kImageQuality, kLighting, kReflections, kWorldDetail, kSupport };

struct Row {
  std::string_view cvar;
  Page page;
  // VANILLA, ENHANCED, ENHANCED ULTRA, ENHANCED RT, ENHANCED RT ULTRA: the text the setting is written with.
  std::string_view cell[kNamedCount];
};

constexpr int Col(Preset p) { return static_cast<int>(p); }

// The rows, page by page in the order of the pause menu. A cell is a word or a number the row's own list holds.
// Rows with an `auto` (the ones that have a per-class cell today) read their cells for ENHANCED, ENHANCED ULTRA and VANILLA as the Mid, High and Low class's `auto`; the rest are the
// row's default (ENHANCED, the RT presets) and the original's (VANILLA).
inline constexpr Row kRows[] = {
    // --- IMAGE QUALITY ---
    // MSAA: the original's 2 samples; ULTRA takes 4 (8 samples cost the most memory and time for the least gain, and every pass with a per-sample image grows with it).
    {"mcla_gpu_msaa", Page::kImageQuality, {"2", "2", "4", "2", "4"}},
    // POST AA: off in every preset (FXAA and SMAA turn the HDR highlights off, audit B5; they come on top of the MSAA).
    {"mcla_gpu_aa", Page::kImageQuality, {"off", "off", "off", "off", "off"}},
    // ANISOTROPIC: the game's own fetch constants in VANILLA (owner), ENHANCED keeps today's default (`game`), ULTRA 16x (no cost on the RTX 5080, at most 1.5% on a 2-CU iGPU: the fork's measurement).
    {"mcla_gpu_anisotropy", Page::kImageQuality, {"game", "game", "16", "game", "16"}},
    // DEPTH OF FIELD (disable_dof): the original has it (false = on); the default today is off (Mz's and BadassBaboon's taste, saves no GPU time) and stays that in the ENHANCED presets.
    {"disable_dof", Page::kImageQuality, {"false", "true", "true", "true", "true"}},
    // MOTION BLUR (disable_motion_blur): on, as the game has it, in every preset.
    {"disable_motion_blur", Page::kImageQuality, {"false", "false", "false", "false", "false"}},

    // --- LIGHTING AND SHADOWS ---
    // SHADOWS (perf_no_shadows, inverted): on in every preset.
    {"perf_no_shadows", Page::kLighting, {"false", "false", "false", "false", "false"}},
    // FOLIAGE SHADOWS (disable_imposter_shadows, inverted): the original has the far trees' shadows (and the fork ships them); the default today is off (a 2026-08 performance default,
    // audit B2) and stays that in the ENHANCED presets until the audit's A1 to A4 runs decide it.
    {"disable_imposter_shadows", Page::kLighting, {"false", "true", "true", "true", "true"}},
    {"mcla_gpu_shadow_resolution", Page::kLighting, {"game", "high", "high", "high", "high"}},
    {"mcla_gpu_shadow_filter", Page::kLighting, {"game", "pcss", "pcss", "pcss", "pcss"}},
    {"mcla_gpu_contact_shadows", Page::kLighting, {"off", "high", "ultra", "high", "ultra"}},
    // RT SHADOWS: the ray tracing rung's cells (none off, weak high, strong high).
    {"mcla_gpu_rt_shadows", Page::kLighting, {"off", "off", "off", "high", "high"}},
    {"mcla_gpu_traffic_shadows", Page::kLighting, {"off", "near", "near", "near", "near"}},
    // CAR SHADOWS: `auto` is the 2048 map on a discrete card (car_shadow_map_rules.h AutoLevel, which still reads the class: its Low cell is the 1024 map, the one `auto` that is not VANILLA's).
    {"mcla_gpu_car_shadow_map", Page::kLighting, {"off", "ultra", "ultra", "ultra", "ultra"}},
    // AMBIENT OCCLUSION: ray traced (`rt`) in the one preset that is everything at the top of a strong card.
    {"mcla_gpu_ao", Page::kLighting, {"off", "high", "ultra", "high", "rt"}},
    {"mcla_gpu_ao_intensity", Page::kLighting, {"default", "default", "default", "default", "default"}},
    {"mcla_gpu_ssgi", Page::kLighting, {"off", "off", "high", "off", "high"}},
    // RT BOUNCE LIGHT: the rung's cells (off, off, high); a weak card keeps the screen-space bounce light.
    {"mcla_gpu_rt_gi", Page::kLighting, {"off", "off", "off", "off", "high"}},
    {"mcla_gpu_ssgi_intensity", Page::kLighting, {"default", "default", "default", "default", "default"}},
    // SUN SHAFTS: a strength, not a quality: DEFAULT wherever the GPU is not integrated.
    {"mcla_gpu_sun_shafts", Page::kLighting, {"off", "default", "default", "default", "default"}},
    {"mcla_gpu_headlights", Page::kLighting, {"off", "on", "on", "on", "on"}},
    {"mcla_gpu_light_shadows", Page::kLighting, {"off", "medium", "ultra", "medium", "ultra"}},
    {"mcla_gpu_neon_light", Page::kLighting, {"off", "on", "on", "on", "on"}},
    {"mcla_gpu_volumetrics", Page::kLighting, {"off", "medium", "high", "medium", "high"}},

    // --- REFLECTIONS ---
    {"mcla_gpu_reflection_resolution", Page::kReflections, {"game", "high", "high", "high", "high"}},
    {"mcla_gpu_env_resolution", Page::kReflections, {"game", "game", "high", "game", "high"}},
    {"mcla_gpu_car_selfrefl", Page::kReflections, {"off", "on", "on", "on", "on"}},
    // The ray traced half of CAR SELF-REFLECTIONS (the row writes both settings: off, on, ray traced). The setting takes auto or off only: the RT presets write `auto`, which the ray tracing
    // rung's table turns into `on` where the card traces (Resolved below).
    {"mcla_gpu_car_selfrefl_rt", Page::kReflections, {"off", "off", "off", "auto", "auto"}},
    {"mcla_gpu_rt_reflections", Page::kReflections, {"off", "off", "off", "off", "high"}},
    {"mcla_gpu_side_mirrors", Page::kReflections, {"off", "on", "on", "on", "on"}},
    // REFLECTIONS ON SURFACES: `auto` is off on every class (gpu_class.h kEffects: the owner held it, 2026-11-24, the reflections flicker in motion), so VANILLA, ENHANCED and ENHANCED RT are
    // off; the two ULTRA presets are the explicit maximum presets and hold the top level.
    {"mcla_gpu_ssr", Page::kReflections, {"off", "off", "off", "off", "off"}},

    // --- WORLD DETAIL --- (VANILLA is the stock game: 400 m, one times the pedestrians and the parked cars, the original LOD distances and prop draw distance.)
    {"traffic_unspawn_dist", Page::kWorldDetail, {"400", "300", "400", "300", "400"}},
    {"ped_density_scale", Page::kWorldDetail, {"1", "0.75", "1", "0.75", "1"}},
    {"parked_car_scale", Page::kWorldDetail, {"1", "0.75", "1", "0.75", "1"}},
    {"global_max_draw_distance", Page::kWorldDetail, {"0", "0", "0", "0", "0"}},
    {"lod_traffic_scale", Page::kWorldDetail, {"1", "2", "3", "2", "3"}},
    {"lod_city_scale", Page::kWorldDetail, {"1", "2", "2", "2", "2"}},

    // --- not on a page: a cell an `auto` table reads ---
    // The car's ray traced occlusion and sun ray (rt_tier.h kRtEffects): none off, weak the sun ray alone, strong both. A diagnostic cvar: the presets do not apply it.
    {"mcla_gpu_ao_rt", Page::kSupport, {"off", "off", "off", "contact", "both"}},
};
inline constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// A cvar and a few words about it.
struct Note {
  std::string_view cvar;
  std::string_view text;
};

// A row that is on one of the four pages and that no preset covers, with the reason (the lint and the test make every row of the pages either covered or listed here).
inline constexpr Note kExempt[] = {
    {"mcla_ui_sharp", "the HUD and menu art's resolution is not an effect: it stays whatever the preset"},
    {"mcla_ui_minimap_scale", "the HUD's small views' resolution is not an effect: it stays whatever the preset"},
    {"anisotropic_override", "the Xenia renderer's own row: presets are the native renderer's"},
};
inline constexpr int kExemptCount = static_cast<int>(sizeof(kExempt) / sizeof(kExempt[0]));

// The rows whose ENHANCED ULTRA cell is below the row's top level because that is what the High class's `auto` was (see the header), with the top level: the final pass raises them or
// says why not. (MSAA is below its top, 8x, on purpose. LOD TRAFFIC 3X is the ceiling that matters: gpu_class.h kScales.)
inline constexpr Note kBelowMax[] = {
    {"mcla_gpu_shadow_resolution", "ultra"}, {"mcla_gpu_env_resolution", "ultra"}, {"mcla_gpu_reflection_resolution", "ultra"},
    {"mcla_gpu_ssgi", "ultra"}, {"mcla_gpu_volumetrics", "ultra"}, {"mcla_gpu_traffic_shadows", "all"},
    {"mcla_gpu_msaa", "8"}, {"mcla_gpu_ssr", "ultra"},
};
inline constexpr int kBelowMaxCount = static_cast<int>(sizeof(kBelowMax) / sizeof(kBelowMax[0]));

// Settings that belong to DISPLAY or UPSCALING: a preset never names one (the owner's scope rule: no change in the graphics section may affect the display section).
inline constexpr std::string_view kNeverCovered[] = {
    "fullscreen", "fullscreen_exclusive", "resolution", "fullscreen_resolution", "aspect_ratio", "display_gamma", "mcla_gpu_vsync", "fps_limit",
    "mcla_gpu_low_latency", "mcla_gpu_hdr", "mcla_gpu_hdr_paper_white_nits", "mcla_gpu_hdr_peak_nits", "mcla_gpu_hdr_highlights",
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

// What a cell stands for in an `auto` table (rt_tier.h kRtEffects): the text the preset writes, or, for the one row whose setting takes `auto` where the effect is on, what that `auto` comes to.
constexpr std::string_view Resolved(std::string_view cvar, std::string_view cell) {
  return (cvar == "mcla_gpu_car_selfrefl_rt" && cell == "auto") ? std::string_view("on") : cell;
}

// A row a preset applies and a change of which turns a named preset into CUSTOM: on one of the four pages.
constexpr bool IsCovered(std::string_view cvar) {
  const int row = FindRow(cvar);
  return row >= 0 && kRows[row].page != Page::kSupport;
}
// Any row of the table, including inputs to the Auto rules.
constexpr bool IsKnownRow(std::string_view cvar) { return FindRow(cvar) >= 0; }

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

// --- the class -> preset map (the "Recommended for this GPU" tag) -----------------------------------------------------------------------
// One table maps the class and the ray tracing rung to a preset (rt_tier.h RecommendedPreset is its code; kClassToPreset is the same rule as the settings-ui expressions the launcher
// reads, in order, the first true one wins, the last is always true):
//   a strong card of the High class      ENHANCED RT ULTRA
//   a card at the weak rung              ENHANCED RT     (the mid-range RT cards: a Mid class card that traces, an RTX 3060-class High card)
//   the High class                       ENHANCED ULTRA
//   the Mid class                        ENHANCED
//   everything else (Low: shared memory, software, the Steam Deck)  VANILLA
// The first start stays VANILLA whatever this says: the tag is a recommendation.
struct ClassRule {
  std::string_view when;
  Preset preset;
};
inline constexpr ClassRule kClassToPreset[] = {
    {"gpu.rt_rung == 'strong'", Preset::kEnhancedRtUltra},
    {"gpu.rt_rung == 'weak'", Preset::kEnhancedRt},
    {"gpu.class == 'high'", Preset::kEnhancedUltra},
    {"gpu.class == 'mid'", Preset::kEnhanced},
    {"true", Preset::kVanilla},
};
inline constexpr int kClassRuleCount = static_cast<int>(sizeof(kClassToPreset) / sizeof(kClassToPreset[0]));

// --- first start and the older files ---------------------------------------------------------------------------------------------------

// What a larecomp.toml says about presets, from the keys it has.
struct FileFacts {
  bool has_preset_key = false;  // a mcla_gpu_preset line
  bool has_covered_key = false;  // a line for a setting a preset applies

  constexpr void NoteKey(std::string_view name) {
    if (name == kPresetSetting) has_preset_key = true;
    if (IsCovered(name)) has_covered_key = true;
  }
};
// Start on VANILLA unless a file without a preset line already holds a covered value. Then start on CUSTOM with no base preset.
constexpr Preset DecideInitial(const FileFacts& file) {
  return (!file.has_preset_key && file.has_covered_key) ? Preset::kCustom : kFirstStart;
}

// --- what the pause menu does with a change --------------------------------------------------------------------------------------------

// A covered row changed by the player while `current` is in force: the preset it turns into. A named preset becomes CUSTOM; CUSTOM stays; the change of the preset itself, of the
// `from` setting, of a support row or of a row outside the table is not a change of a covered row.
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
