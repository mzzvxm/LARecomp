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
#include <string>
#include <string_view>

#include "gfx_preset.h"
#include "gpu_class.h"
#include "rt_tier.h"

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

constexpr Preset kNamed[gp::kNamedCount] = {Preset::kVanilla, Preset::kEnhanced, Preset::kEnhancedUltra, Preset::kEnhancedRt, Preset::kEnhancedRtUltra};
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
    {"mcla_gpu_ao", "off|high|ultra|rt"},
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
  Check(gp::kPresetCount == 6 && gp::kNamedCount == 5, "six presets, five of them named");

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

// --- the old `auto` tables, pinned: ENHANCED is today's Mid, ENHANCED ULTRA today's High, the RT presets today's rungs -----------------------------------------------------------
struct OldEffect {
  Effect effect;
  const char* cvar;
  const char* low;
  const char* mid;
  const char* high;
};
// gpu_class.h kEffects as it was before the table (the cells are the old file's literals).
const OldEffect kOldEffects[] = {
    {Effect::kShadowFilter, "mcla_gpu_shadow_filter", "game", "pcss", "pcss"},
    {Effect::kShadowResolution, "mcla_gpu_shadow_resolution", "game", "high", "high"},
    {Effect::kReflectionResolution, "mcla_gpu_reflection_resolution", "game", "high", "high"},
    {Effect::kContactShadows, "mcla_gpu_contact_shadows", "off", "high", "ultra"},
    {Effect::kSunShafts, "mcla_gpu_sun_shafts", "off", "default", "default"},
    {Effect::kSsgi, "mcla_gpu_ssgi", "off", "off", "high"},
    {Effect::kTrafficShadows, "mcla_gpu_traffic_shadows", "off", "near", "near"},
    {Effect::kCarSelfRefl, "mcla_gpu_car_selfrefl", "off", "on", "on"},
    {Effect::kAmbientOcclusion, "mcla_gpu_ao", "off", "high", "ultra"},
    {Effect::kHeadlights, "mcla_gpu_headlights", "off", "on", "on"},
    {Effect::kLightShadows, "mcla_gpu_light_shadows", "off", "medium", "ultra"},
    {Effect::kVolumetrics, "mcla_gpu_volumetrics", "off", "medium", "high"},
    {Effect::kRtShadows, "mcla_gpu_rt_shadows", "off", "off", "off"},  // the one class cell that is not read from the table (gpu_class.h)
    {Effect::kSsr, "mcla_gpu_ssr", "off", "off", "off"},  // the legacy class table keeps SSR off
    {Effect::kNeonLight, "mcla_gpu_neon_light", "off", "on", "on"},
};

struct OldRtEffect {
  RtEffect effect;
  const char* cvar;
  const char* none;
  const char* weak;
  const char* strong;
};
// rt_tier.h kRtEffects as it was before the table.
const OldRtEffect kOldRt[] = {
    {RtEffect::kCarSelfReflRt, "mcla_gpu_car_selfrefl_rt", "off", "on", "on"},
    {RtEffect::kCarRtAo, "mcla_gpu_ao_rt", "off", "contact", "both"},
    {RtEffect::kRtShadows, "mcla_gpu_rt_shadows", "off", "high", "high"},
    {RtEffect::kRtReflections, "mcla_gpu_rt_reflections", "off", "on", "on"},
    {RtEffect::kRtGi, "mcla_gpu_rt_gi", "off", "off", "high"},
    {RtEffect::kRtAo, "mcla_gpu_ao", "", "", "rt"},
};

struct OldScaleCells {
  Scale scale;
  const char* cvar;
  double low, mid, high;
};
// gpu_class.h kScales as it was.
const OldScaleCells kOldScales[] = {
    {Scale::kCityLod, "lod_city_scale", 1.0, 2.0, 2.0},
    {Scale::kTrafficLod, "lod_traffic_scale", 1.5, 2.0, 3.0},
    {Scale::kTrafficRange, "traffic_unspawn_dist", 250.0, 300.0, 400.0},
    {Scale::kPedDensity, "ped_density_scale", 0.5, 0.75, 1.0},
    {Scale::kParkedDensity, "parked_car_scale", 0.5, 0.75, 1.0},
};

// The rows that have no `auto` today: their ENHANCED cell is the row's default, and the table's other cells are decisions of this item (the numbers below are pinned so that a change
// is deliberate). {cvar, ENHANCED = the default today, ENHANCED ULTRA}.
struct NoAuto {
  const char* cvar;
  const char* enhanced;
  const char* ultra;
};
const NoAuto kNoAuto[] = {
    {"mcla_gpu_anisotropy", "game", "16"},
    {"disable_dof", "true", "true"},
    {"disable_motion_blur", "false", "false"},
    {"perf_no_shadows", "false", "false"},
    {"disable_imposter_shadows", "true", "true"},
    {"mcla_gpu_car_shadow_map", "ultra", "ultra"},  // `auto` is car_shadow_map_rules.h AutoLevel: the 2048 map on a discrete card
    {"mcla_gpu_ao_intensity", "default", "default"},
    {"mcla_gpu_ssgi_intensity", "default", "default"},
    {"global_max_draw_distance", "0", "0"},
};

void CheckAutoUnchanged() {
  Check(sizeof(kOldEffects) / sizeof(kOldEffects[0]) == static_cast<std::size_t>(kEffectCount), "the old effects table is complete");
  for (int i = 0; i < kEffectCount; ++i) {
    const OldEffect& old = kOldEffects[i];
    const std::string cvar = old.cvar;
    Check(static_cast<int>(old.effect) == i, cvar + ": in the order of the enum");
    CheckEq(CvarName(old.effect), cvar, cvar + ": kEffects names it");
    CheckEq(Cell(old.effect, GpuClass::kLow), old.low, cvar + ": the Low cell is unchanged");
    CheckEq(Cell(old.effect, GpuClass::kMid), old.mid, cvar + ": the Mid cell is unchanged");
    CheckEq(Cell(old.effect, GpuClass::kHigh), old.high, cvar + ": the High cell is unchanged");
    if (old.effect == Effect::kRtShadows) continue;  // the ray tracing rung's cells decide (below); the class cell is the old literal
    if (old.effect == Effect::kSsr) {                // the other literal: `auto` is off on every class, the ULTRA presets are the explicit maximum
      CheckEq(gp::CellOf(old.cvar, Preset::kVanilla), "off", "SSR in VANILLA");
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhanced), "high", "SSR in Enhanced");
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedRt), "high", "SSR in EnhancedRt");
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedUltra), "ultra", "SSR in EnhancedUltra");
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedRtUltra), "ultra", "SSR in EnhancedRtUltra");
      continue;
    }
    // ENHANCED is the Mid class's `auto`, ENHANCED ULTRA the High class's, VANILLA the Low class's: the table and the `auto` cells are one thing.
    CheckEq(gp::CellOf(old.cvar, Preset::kVanilla), old.low, cvar + ": VANILLA is today's Low cell");
    CheckEq(gp::CellOf(old.cvar, Preset::kEnhanced), old.mid, cvar + ": ENHANCED is today's Mid cell");
    CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedUltra), old.high, cvar + ": ENHANCED ULTRA is today's High cell");
    // The RT presets' non-RT rows are their non-RT sibling's (the ray traced rows are the ones that differ).
    CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedRt), gp::CellOf(old.cvar, Preset::kEnhanced), cvar + ": ENHANCED RT is ENHANCED's cell where the row is not ray traced");
    if (old.effect != Effect::kAmbientOcclusion) {  // the one row that is both: ENHANCED RT ULTRA's occlusion is ray traced (`rt`)
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedRtUltra), gp::CellOf(old.cvar, Preset::kEnhancedUltra), cvar + ": ENHANCED RT ULTRA is ENHANCED ULTRA's cell where the row is not ray traced");
    }
    // And `auto` on a class resolves to the preset the class's family names.
    for (const GpuClass c : kClasses) {
      CheckEq(ResolveFor(c, old.effect, "auto"), std::string(Cell(old.effect, c)), cvar + ": Resolve(auto) is the cell");
    }
  }

  // The Steam Deck keeps its three handheld cells (they are not in the table: a residue, listed in the report).
  CheckEq(CellFor(Effect::kShadowFilter, GpuClass::kLow, true), "soft", "a handheld's shadow filter is unchanged");
  CheckEq(CellFor(Effect::kShadowResolution, GpuClass::kLow, true), "high", "a handheld's shadow detail is unchanged");
  CheckEq(CellFor(Effect::kReflectionResolution, GpuClass::kLow, true), "high", "a handheld's reflection detail is unchanged");

  // The scales: Mid and High read the table, Low is the old number (the city LOD's is VANILLA's too).
  for (const OldScaleCells& old : kOldScales) {
    const std::string cvar = old.cvar;
    CheckEq(ScaleCvar(old.scale), cvar, cvar + ": kScales names it");
    CheckNum(ScaleCell(old.scale, GpuClass::kLow), old.low, cvar + ": the Low cell is unchanged");
    CheckNum(ScaleCell(old.scale, GpuClass::kMid), old.mid, cvar + ": the Mid cell is unchanged");
    CheckNum(ScaleCell(old.scale, GpuClass::kHigh), old.high, cvar + ": the High cell is unchanged");
    CheckNum(gp::Number(gp::CellOf(old.cvar, Preset::kEnhanced)), old.mid, cvar + ": ENHANCED is today's Mid cell");
    CheckNum(gp::Number(gp::CellOf(old.cvar, Preset::kEnhancedUltra)), old.high, cvar + ": ENHANCED ULTRA is today's High cell");
  }
  // The residue: VANILLA is the stock game, and four of the Low class's cells differ from stock (the owner's 2026-11-20 LOD pass): the traffic range and the two densities are
  // below it (a weak PC's performance values), the traffic LOD multiple above it (1.5x the game's distances: a cheap detail gain kept for every class).
  CheckNum(ScaleCell(Scale::kCityLod, GpuClass::kLow), gp::Number(gp::CellOf("lod_city_scale", Preset::kVanilla)), "the city LOD's Low cell is VANILLA's (the game's own)");
  for (const Scale s : {Scale::kTrafficLod, Scale::kTrafficRange, Scale::kPedDensity, Scale::kParkedDensity}) {
    const double low = ScaleCell(s, GpuClass::kLow), stock = gp::Number(gp::CellOf(ScaleCvar(s), Preset::kVanilla));
    Check(s == Scale::kTrafficLod ? low > stock : low < stock,
          std::string(ScaleCvar(s)) + ": the Low class's `auto` keeps its own number against VANILLA's stock one (the one place the Low class is not VANILLA)");
  }
  // The ray tracing rungs: none is ENHANCED's cell, weak ENHANCED RT's, strong ENHANCED RT ULTRA's.
  Check(sizeof(kOldRt) / sizeof(kOldRt[0]) == static_cast<std::size_t>(kRtEffectCount), "the old ray traced table is complete");
  for (int i = 0; i < kRtEffectCount; ++i) {
    const OldRtEffect& old = kOldRt[i];
    const std::string cvar = old.cvar;
    Check(static_cast<int>(old.effect) == i, cvar + ": in the order of the RT enum");
    CheckEq(RtCvarName(old.effect), cvar, cvar + ": kRtEffects names it");
    const auto legacy = old.effect == RtEffect::kRtAo ? "" : "off";
    for (const auto tier : {RtTier::kNone, RtTier::kWeak, RtTier::kStrong}) {
      CheckEq(RtCell(old.effect, tier), legacy, cvar + ": legacy Auto never enables RT");
    }
    if (old.effect == RtEffect::kRtAo) {
      CheckEq(gp::CellOf(old.cvar, Preset::kEnhancedRtUltra), "rt", "the world's ambient occlusion is ray traced in ENHANCED RT ULTRA");
      continue;
    }
    const auto resolved = [&](Preset p) { return gp::CellOf(old.cvar, p); };
    CheckEq(resolved(Preset::kEnhanced), old.none, cvar + ": ENHANCED has the ray traced row off (the none rung)");
    CheckEq(resolved(Preset::kEnhancedRt), old.weak, cvar + ": ENHANCED RT is the weak rung's cell");
    CheckEq(resolved(Preset::kEnhancedRtUltra), old.strong, cvar + ": ENHANCED RT ULTRA is the strong rung's cell");
    CheckEq(resolved(Preset::kEnhancedUltra), old.none, cvar + ": ENHANCED ULTRA has the ray traced row off too");
    CheckEq(resolved(Preset::kVanilla), old.none, cvar + ": and VANILLA");
  }

  // The rows that had no `auto`.
  for (const NoAuto& row : kNoAuto) {
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhanced), row.enhanced, Name(row.cvar, Preset::kEnhanced));
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhancedUltra), row.ultra, Name(row.cvar, Preset::kEnhancedUltra));
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhancedRt), row.enhanced, Name(row.cvar, Preset::kEnhancedRt));
    CheckEq(gp::CellOf(row.cvar, Preset::kEnhancedRtUltra), row.ultra, Name(row.cvar, Preset::kEnhancedRtUltra));
  }
}

// ENHANCED ULTRA is the top level of a row where the row has levels and the High class's `auto` reached it; kBelowMax lists the rest.
struct Top {
  const char* cvar;
  const char* top;
};
const Top kTops[] = {
    {"mcla_gpu_anisotropy", "16"},
    {"mcla_gpu_shadow_resolution", "ultra"},
    {"mcla_gpu_shadow_filter", "pcss"},
    {"mcla_gpu_contact_shadows", "ultra"},
    {"mcla_gpu_traffic_shadows", "all"},
    {"mcla_gpu_car_shadow_map", "ultra"},
    {"mcla_gpu_ao", "ultra"},
    {"mcla_gpu_ssgi", "ultra"},
    {"mcla_gpu_headlights", "on"},
    {"mcla_gpu_light_shadows", "ultra"},
    {"mcla_gpu_neon_light", "on"},
    {"mcla_gpu_light_bounce", "on"},
    {"mcla_gpu_volumetrics", "ultra"},
    {"mcla_gpu_reflection_resolution", "ultra"},
    {"mcla_gpu_env_resolution", "on"},
    {"mcla_gpu_car_selfrefl", "on"},
    {"mcla_gpu_side_mirrors", "on"},
    {"mcla_gpu_ssr", "ultra"},
    {"lod_city_scale", "2"},
};

void CheckUltra() {
  int below = 0;
  for (const Top& t : kTops) {
    const std::string_view ultra = gp::CellOf(t.cvar, Preset::kEnhancedUltra);
    bool listed = false;
    for (const gp::Note& n : gp::kBelowMax) {
      if (n.cvar == t.cvar) {
        listed = true;
        CheckEq(n.text, t.top, std::string(t.cvar) + ": kBelowMax names the row's top level");
      }
    }
    if (ultra == t.top) {
      Check(!listed, std::string(t.cvar) + ": ENHANCED ULTRA is at the top, so the row is not on the below-max list");
    } else {
      Check(listed, std::string(t.cvar) + ": ENHANCED ULTRA is below the top ('" + std::string(ultra) + "'), so the row is on the below-max list");
      ++below;
    }
  }
  Check(below == gp::kBelowMaxCount, "the below-max list is exactly the rows that are below their top (" + std::to_string(below) + ")");
  // The ceilings of the LOD rows (gpu_class.h): the game's own range and densities, the city LOD that shows every loaded sector, the traffic LOD past which nothing is left to see.
  CheckNum(gp::Number(gp::CellOf("traffic_unspawn_dist", Preset::kEnhancedUltra)), 400.0, "ENHANCED ULTRA has the game's own traffic range");
  CheckNum(gp::Number(gp::CellOf("ped_density_scale", Preset::kEnhancedUltra)), 1.0, "ENHANCED ULTRA has the game's own pedestrians");
  CheckNum(gp::Number(gp::CellOf("parked_car_scale", Preset::kEnhancedUltra)), 1.0, "ENHANCED ULTRA has the game's own parked cars");
  CheckNum(gp::Number(gp::CellOf("lod_traffic_scale", Preset::kEnhancedUltra)), 3.0, "ENHANCED ULTRA has the traffic LOD ceiling");
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
  const char* const ids[] = {"vanilla", "enhanced", "enhanced_ultra", "enhanced_rt", "enhanced_rt_ultra", "custom"};
  const char* const labels[] = {"VANILLA", "ENHANCED", "ENHANCED ULTRA", "ENHANCED RT", "ENHANCED RT ULTRA", "CUSTOM"};
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
  Check(gp::kFirstStart == Preset::kVanilla, "the first start is VANILLA");
  CheckEq(gp::kPresetSetting, "mcla_gpu_preset", "the preset setting's name");
  CheckEq(gp::kFromSetting, "mcla_gpu_preset_from", "the from setting's name");
  Check(gp::IsNamed(Preset::kVanilla) && gp::IsNamed(Preset::kEnhancedRtUltra) && !gp::IsNamed(Preset::kCustom), "every preset but CUSTOM is named");
}

// --- RT presets need an RT tier ----------------------------------------------------------------------------------------------------------------------------------------
void CheckRtNeeds() {
  for (int i = 0; i < gp::kPresetCount; ++i) {
    const Preset p = static_cast<Preset>(i);
    const bool rt = p == Preset::kEnhancedRt || p == Preset::kEnhancedRtUltra;
    Check(gp::NeedsRt(p) == rt, "only the two RT presets need ray tracing: " + std::string(gp::Id(p)));
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
// The settings-ui expressions of kClassToPreset are plain: `true`, `gpu.class == 'x'` and `gpu.rt_rung == 'x'`; this reads them against a class and a tier, so the exported rule and
// rt_tier.h's RecommendedPreset cannot differ.
bool WhenHolds(std::string_view when, GpuClass c, RtTier t) {
  if (when == "true") return true;
  const std::string_view class_eq = "gpu.class == '";
  const std::string_view rung_eq = "gpu.rt_rung == '";
  if (when.substr(0, class_eq.size()) == class_eq) {
    const std::string_view word = when.substr(class_eq.size(), when.size() - class_eq.size() - 1);
    return word == ClassName(c);
  }
  if (when.substr(0, rung_eq.size()) == rung_eq) {
    const std::string_view word = when.substr(rung_eq.size(), when.size() - rung_eq.size() - 1);
    return word == RtTierName(RtRung(c, t));
  }
  std::printf("FAILED: an expression this test cannot read: %.*s\n", static_cast<int>(when.size()), when.data());
  ++g_failed;
  return false;
}

Preset ByExpressions(GpuClass c, RtTier t) {
  for (const gp::ClassRule& rule : gp::kClassToPreset) {
    if (WhenHolds(rule.when, c, t)) return rule.preset;
  }
  return Preset::kCustom;
}

void CheckRecommendation() {
  Check(gp::kClassToPreset[gp::kClassRuleCount - 1].when == "true", "the last rule is always true: every PC has a recommendation");
  struct Case {
    GpuClass c;
    RtTier t;
    Preset want;
  };
  const Case cases[] = {
      {GpuClass::kLow, RtTier::kNone, Preset::kVanilla},
      {GpuClass::kLow, RtTier::kWeak, Preset::kVanilla},  // an iGPU or a Deck that has ray tracing units: nothing heavy runs on shared memory
      {GpuClass::kLow, RtTier::kStrong, Preset::kVanilla},
      {GpuClass::kMid, RtTier::kNone, Preset::kEnhanced},
      {GpuClass::kMid, RtTier::kWeak, Preset::kEnhancedRt},
      {GpuClass::kMid, RtTier::kStrong, Preset::kEnhancedRt},  // a Mid class card is at most weak
      {GpuClass::kHigh, RtTier::kNone, Preset::kEnhancedUltra},
      {GpuClass::kHigh, RtTier::kWeak, Preset::kEnhancedRt},
      {GpuClass::kHigh, RtTier::kStrong, Preset::kEnhancedRtUltra},
  };
  for (const Case& k : cases) {
    const std::string at = std::string(ClassName(k.c)) + " + " + std::string(RtTierName(k.t));
    Check(RecommendedPreset(k.c, k.t) == k.want, at + ": recommended " + std::string(gp::Id(k.want)));
    Check(ByExpressions(k.c, k.t) == k.want, at + ": the exported expressions say the same");
    // A recommended preset is one the card is offered.
    Check(gp::Offered(RecommendedPreset(k.c, k.t), RtRung(k.c, k.t) != RtTier::kNone), at + ": the recommendation is offered on that card");
  }
  // The recommendation and `auto` agree on every row they both hold, wherever one preset holds the whole of `auto`: Low (VANILLA), Mid + none (ENHANCED), Mid + weak (ENHANCED RT),
  // High + none (ENHANCED ULTRA) and High + strong (ENHANCED RT ULTRA). High + weak is the one mixed case (the High class's cells with the weak rung's ray traced rows).
  for (const Case& k : cases) {
    const bool mixed = k.c == GpuClass::kHigh && k.t == RtTier::kWeak;
    const Preset p = RecommendedPreset(k.c, k.t);
    for (int i = 0; i < kEffectCount; ++i) {
      const Effect effect = static_cast<Effect>(i);
      if (effect == Effect::kRtShadows || effect == Effect::kAmbientOcclusion || effect == Effect::kSsr) continue;  // the three whose class cell is not the preset's (gpu_class.h)
      if (mixed) continue;
      if (k.c == GpuClass::kLow && k.t != RtTier::kNone) continue;
      CheckEq(Cell(effect, k.c), gp::CellOf(CvarName(effect), p), std::string(CvarName(effect)) + " on " + std::string(ClassName(k.c)) + " + " + std::string(RtTierName(k.t)) + " is the recommended preset's cell");
    }

  }
}

// --- first start, an older file, the CUSTOM flip, picking a preset ------------------------------------------------------------------------------------------------------------
void CheckFirstStart() {
  gp::FileFacts none;
  Check(gp::DecideInitial(none) == Preset::kVanilla, "no saved keys: VANILLA");
  gp::FileFacts older;
  older.NoteKey("mcla_gpu_ao");
  older.NoteKey("mcla_gpu_ao_rt");
  Check(gp::DecideInitial(older) == Preset::kCustom, "a covered setting followed by a support setting still starts on CUSTOM");
  older.NoteKey("mcla_gpu_preset");
  Check(gp::DecideInitial(older) == Preset::kVanilla, "an explicit preset suppresses the CUSTOM migration");
  gp::FileFacts only_preset;
  only_preset.NoteKey("mcla_gpu_preset");
  Check(gp::DecideInitial(only_preset) == Preset::kVanilla, "a file with only the preset line: the line decides");
  only_preset.NoteKey("mcla_gpu_ao");
  Check(gp::DecideInitial(only_preset) == Preset::kVanilla, "an explicit preset suppresses migration even before a covered key");
  for (const gp::Row& row : gp::kRows) {
    gp::FileFacts facts;
    facts.NoteKey(row.cvar);
    const auto expected = Preset::kCustom;
    Check(gp::DecideInitial(facts) == expected, std::string(row.cvar) + ": only a covered key triggers legacy CUSTOM");
  }
  for (const auto key : {"mcla_gpu_msaa", "mcla_gpu_aa", "larecomp.mcla_gpu_aa"}) {
    gp::FileFacts facts;
    facts.NoteKey(key);
    Check(gp::DecideInitial(facts) == Preset::kCustom, "formerly covered AA preserves an old config");
    facts.NoteKey("larecomp.mcla_gpu_preset");
    Check(gp::DecideInitial(facts) == Preset::kVanilla, "a scoped preset overrides legacy config inference");
  }
  for (const auto key : {"fullscreen", "mcla_gpu_upscaler", "language"}) {
    gp::FileFacts facts;
    facts.NoteKey(key);
    Check(gp::DecideInitial(facts) == Preset::kVanilla, std::string(key) + ": outside preset coverage");
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
  for (const Preset preset : kNamed) {
    const bool rt = preset == Preset::kEnhancedRt || preset == Preset::kEnhancedRtUltra;
    CheckEq(gp::CellOf("mcla_gpu_ssr_rt", preset), rt ? "high" : "off", "surface RT follows the RT preset");
  }
  CheckAutoUnchanged();
  CheckUltra();
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
