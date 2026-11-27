// Check of the G-buffer's material channel (gbuffer_material.h, and part 3 of gbuffer_rules.h): the class table (every group of the game has a class), the gloss table,
// the specular exponent's mapping to a smoothness, the material word's packing, the table file's layout, and the draw's word. A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_material_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gbuffer_material_test [<the folder shaderpack\baseline: hlsl-rpf and hlsl-xex with their shader_report.tsv>]
//
// With the folder it also checks the tables against the translated HLSL of the retail game: every group of the shader reports has a row (and no row names a group the game
// does not have), every gloss row's register is the one the shader's text declares, and every shader whose text declares a SpecularExponent has a gloss row.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_material.h"

namespace gb = mcla::gpu::gbuffer;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// A constant bank as the guest holds it: big-endian words, 4096 bytes.
struct Bank {
  std::array<uint8_t, 4096> bytes{};
  void SetX(uint32_t reg, float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    uint8_t* p = bytes.data() + size_t(reg) * 16;
    p[0] = uint8_t(bits >> 24);
    p[1] = uint8_t(bits >> 16);
    p[2] = uint8_t(bits >> 8);
    p[3] = uint8_t(bits);
  }
};

// ---------------------------------------------------------------------------
// The class table
// ---------------------------------------------------------------------------
constexpr size_t kRetailGroups = 192;  // the pixel and vertex shader groups of the retail game (shaderpack\baseline: city, cars, characters, effects, ui, the executable's)

void TableShapeChecks() {
  Check(gb::kGroupRowCount == kRetailGroups, "the class table has a row for each of the 192 groups of the game (" + std::to_string(gb::kGroupRowCount) + ")");
  std::set<std::string_view> seen;
  bool unique = true;
  bool valid = true;
  bool none_plain = true;
  for (const gb::GroupRow& row : gb::kGroupRows) {
    unique = unique && seen.insert(row.group).second && !row.group.empty();
    valid = valid && uint32_t(row.cls) < uint32_t(gb::SurfaceClass::kCount) && (row.flags & ~(gb::kMaterialFlagSsr | gb::kMaterialFlagVehicle)) == 0;
    // the alpha covered flag is per draw; a class 0 row carries no flags
    if (row.cls == gb::SurfaceClass::kNone && row.flags != 0) none_plain = false;
    // every group name is the translator's: x<Name>, no double underscore (the split relies on it)
    valid = valid && row.group.find("__") == std::string_view::npos && row.group.front() == 'x';
  }
  Check(unique, "every group is in the class table once");
  Check(valid, "every row has a class that exists, only the table's flags, and a group name the split can take");
  Check(none_plain, "a class 0 row has no flags");
  size_t per_class[size_t(gb::SurfaceClass::kCount)] = {};
  for (const gb::GroupRow& row : gb::kGroupRows) ++per_class[uint32_t(row.cls)];
  // every class the writers can put in a pixel but the wetness (kWetRoad: a state, not a shader) has a group
  for (uint32_t c = 0; c < uint32_t(gb::SurfaceClass::kCount); ++c) {
    if (c == uint32_t(gb::SurfaceClass::kWetRoad)) {
      Check(per_class[c] == 0, "no group is a wet road by name: wetness is the weather's, the road writer promotes kRoad");
    } else {
      Check(per_class[c] > 0, "the class " + std::to_string(c) + " has at least one group");
    }
  }
}

struct Expect {
  const char* group;
  gb::SurfaceClass cls;
  uint32_t flags;
};
void ClassSpotChecks() {
  using gb::SurfaceClass;
  const uint32_t S = gb::kMaterialFlagSsr;
  const uint32_t V = gb::kMaterialFlagVehicle;
  const Expect expect[] = {
      {"xCityDefault", SurfaceClass::kWorld, 0},
      {"xCityBumpSpec", SurfaceClass::kWorld, 0},
      {"xPropDefault", SurfaceClass::kWorld, 0},
      {"xCityRoad", SurfaceClass::kRoad, S},
      {"xCityTunnelRoad", SurfaceClass::kRoad, S},
      {"xCitySidewalk", SurfaceClass::kSidewalk, S},
      {"xCityTerrain", SurfaceClass::kTerrain, 0},
      {"xCityLOD", SurfaceClass::kLod, 0},
      {"xCityGroundLOD", SurfaceClass::kLod, 0},
      {"xPropFoliageImpostor", SurfaceClass::kFoliage, 0},
      {"xCityGrass", SurfaceClass::kFoliage, 0},
      {"xCityNeon", SurfaceClass::kEmissive, 0},
      {"xPropNeonTint", SurfaceClass::kEmissive, 0},
      {"xCityStoreSign", SurfaceClass::kEmissive, 0},
      {"xPropTrafficSignal", SurfaceClass::kEmissive, 0},
      {"xCityLampGlassTint", SurfaceClass::kEmissive, 0},
      {"xPropLampGlassTint", SurfaceClass::kEmissive, 0},
      {"xInteriorTexturedEmissive", SurfaceClass::kEmissive, 0},
      {"xCityStoreInterior", SurfaceClass::kInterior, 0},
      {"xCityWindowLower", SurfaceClass::kGlass, S},
      {"xPropGlass", SurfaceClass::kGlass, S},
      {"xCityOceanWater", SurfaceClass::kWater, S},
      {"xPropMetal", SurfaceClass::kMetal, S},
      {"xCarPaint", SurfaceClass::kPaint, V},
      {"xAmbientCarPaint", SurfaceClass::kPaint, V},
      {"xAmbientBumpSpec", SurfaceClass::kPaint, V},  // the traffic cars' body
      {"xChrome", SurfaceClass::kChrome, V},
      {"xRimMain", SurfaceClass::kChrome, V},
      {"xBrushedMetal", SurfaceClass::kMetal, V},
      {"xCarGlass", SurfaceClass::kGlass, V},
      {"xBlackOpaqueGlass", SurfaceClass::kGlass, V},
      {"xCarLight", SurfaceClass::kEmissive, V},
      {"xAmbientCarLight", SurfaceClass::kEmissive, V},
      {"xPed", SurfaceClass::kSkin, 0},
      {"xCharacter_skin", SurfaceClass::kSkin, 0},
      {"xSkyhat", SurfaceClass::kNone, 0},
      {"xGroundHat", SurfaceClass::kNone, 0},
      {"xrage_postfx", SurfaceClass::kNone, 0},
      {"xrage_im", SurfaceClass::kNone, 0},
      {"xrmptfx_default", SurfaceClass::kNone, 0},
      {"xShadows", SurfaceClass::kNone, 0},
  };
  for (const Expect& e : expect) {
    const gb::GroupRow* row = gb::FindGroupRow(e.group);
    Check(row != nullptr, std::string("a row for ") + e.group);
    if (row == nullptr) continue;
    Check(row->cls == e.cls && row->flags == e.flags, std::string("the class of ") + e.group + ": " + std::to_string(int(row->cls)) + " flags " + std::to_string(row->flags));
  }
  // The survey's gating: paint and chrome keep the game's environment reflections (no SSR), roads, sidewalks, glass and water may take them.
  for (const gb::GroupRow& row : gb::kGroupRows) {
    if (row.cls == SurfaceClass::kPaint || row.cls == SurfaceClass::kChrome) {
      Check((row.flags & S) == 0, std::string("paint and chrome are not SSR eligible: ") + std::string(row.group));
    }
    if (row.cls == SurfaceClass::kEmissive || row.cls == SurfaceClass::kNone || row.cls == SurfaceClass::kLod || row.cls == SurfaceClass::kFoliage) {
      Check((row.flags & S) == 0, std::string("emissive, none, lod and foliage are not SSR eligible: ") + std::string(row.group));
    }
  }
  // The vehicle flag marks the car groups and only them: the cars' classes and the traffic.
  for (const gb::GroupRow& row : gb::kGroupRows) {
    const bool car_group = row.group.starts_with("xAmbient") || row.group.starts_with("xCar") || row.group.starts_with("xRim") || row.group.starts_with("xrage_") ||
                           row.group.starts_with("xBumpSpec") || row.group.starts_with("xBumped") || row.group.starts_with("xGrill") || row.group == "xChrome" ||
                           row.group == "xBlackOpaqueGlass" || row.group == "xBrushedMetal" || row.group == "xAnisotropicBilletMetal" || row.group == "xWheelRubber" ||
                           row.group == "xWheelBrakeRotor" || row.group == "xbrakecaliper" || row.group == "xbumpedglass" || row.group == "xblackmatte" ||
                           row.group == "xBumperSubframe" || row.group == "xEngineBay" || row.group == "xUnderbody" || row.group == "xTexturedPlastic" ||
                           row.group == "xLicensePlate" || row.group == "xCharacterSkin" || row.group == "xInteriorTrim" || row.group == "xInteriorTrimAlpha" ||
                           row.group == "xInteriorTexturedEmissive" || row.group == "xInteriorTexturedEmissiveAlpha";
    if ((row.flags & V) != 0) {
      Check(car_group, std::string("only a car group is flagged a vehicle: ") + std::string(row.group));
    }
  }
}

void NameChecks() {
  using gb::SurfaceClass;
  const auto split = gb::SplitShaderName("xCharacter_skin_arm__PS_HighQualitySkin");
  Check(split.group == "xCharacter_skin_arm" && split.entry == "PS_HighQualitySkin", "a name splits at the first double underscore, the groups' single underscores stay");
  const auto bare = gb::SplitShaderName("unnamed_0123456789abcdef");
  Check(bare.group == "unnamed_0123456789abcdef" && bare.entry.empty(), "a name without a double underscore is all group");
  const auto entry_underscore = gb::SplitShaderName("xBumpSpec__PS_BumpSpec_true_true");
  Check(entry_underscore.group == "xBumpSpec" && entry_underscore.entry == "PS_BumpSpec_true_true", "an entry with underscores keeps them");

  const auto floor = gb::MaterialOfShaderName("xCityGarageFloor__PSMultiLight");
  Check(floor.cls == gb::SurfaceClass::kGarageFloor && floor.smoothness > 0.6f &&
        (floor.flags & gb::kMaterialFlagSsr) != 0, "the garage floor has its own reflective material");
  const auto bump = gb::MaterialOfShaderName("xCityBumpSpec__PSMultiLight");
  Check(bump.known && bump.cls == SurfaceClass::kWorld && bump.lit == 1.0f && Near(bump.smoothness, gb::DefaultSmoothness(SurfaceClass::kWorld), 0.0f), "a night shader of a world group");
  const auto interior = gb::MaterialOfShaderName("xCityBumpSpec__PSMultiLightInterior");
  Check(interior.known && interior.cls == SurfaceClass::kInterior, "the interior light model's entry makes a world surface an interior one");
  const auto road_interior = gb::MaterialOfShaderName("xCityRoad__PSMultiLightInterior");
  Check(road_interior.cls == SurfaceClass::kRoad && (road_interior.flags & gb::kMaterialFlagSsr) != 0, "the interior entry of a road is still a road");
  const auto glass_interior = gb::MaterialOfShaderName("xCityWindowLower__PSMultiLightInterior");
  Check(glass_interior.cls == SurfaceClass::kGlass, "and of a window still glass");
  const auto neon = gb::MaterialOfShaderName("xCityNeon__PSMultiLight");
  Check(neon.cls == SurfaceClass::kEmissive && neon.lit == 0.0f, "neon: emissive, lit weight 0");
  const auto paint = gb::MaterialOfShaderName("xCarPaint__PS_CarPaint");
  Check(paint.cls == SurfaceClass::kPaint && (paint.flags & gb::kMaterialFlagVehicle) != 0 && Near(paint.smoothness, 0.85f, 0.0f), "paint: a vehicle's clear coat");
  for (const char* aux : {"xCityDefault__PS_ShadowBlend", "xCityDefault__PS_ShadowDepthAlpha", "xCityDefault__PS_zPrePassAlpha", "xCityDefault__PSAmbientOcclusion",
                          "xCarPaint__PS_AO", "xCarPaint__PS_drawAo", "xCarPaint__PS_Layout", "xCarPaint__PS_ShadowDepth", "xCarPaint__PS_zPrePass", "xPed__PS_ShadowNight",
                          "xPropFoliage__PSGenerateImposterDay", "xCityRoad__PSMultiLightDebug", "xCityLOD__PSCityLODDebug", "xCityLOD__PS_ShadowFastBlend",
                          "xCityLOD__PS_Blur", "xCityLOD__PS_Copy", "xAmbientOcclusionShadows__PS_CalcShadowsCone", "xShadowImposterCollector__PS_ImposterBlend"}) {
    const auto m = gb::MaterialOfShaderName(aux);
    Check(m.known && m.cls == SurfaceClass::kNone && m.flags == 0, std::string("an auxiliary pass is class 0: ") + aux);
  }
  for (const char* lit : {"xCityDefault__PSCityDefault", "xCityDefault__PSMultiLight", "xCityRoad__PSCityRoad", "xCityRoad__PSMultiLightEnv", "xCarPaint__PS_CarPaint",
                          "xCarPaintCustomizable__PS_HoodCarbonFiber", "xPed__PS", "xPed__PSMultiLight", "xAmbientVehicle__PSMultiLight", "xPropFoliage__PSPropFoliage"}) {
    const auto m = gb::MaterialOfShaderName(lit);
    Check(m.known && m.cls != SurfaceClass::kNone, std::string("a lit shader keeps its group's class: ") + lit);
  }
  const auto unknown = gb::MaterialOfShaderName("xSomeNewGroup__PSMultiLight");
  Check(!unknown.known && unknown.cls == SurfaceClass::kNone, "a group the table does not have is unknown");
  Check(!gb::MaterialOfShaderName("unnamed_0123").known, "a container with no name is unknown");
  Check(!gb::MaterialOfShaderName("").known, "an empty name is unknown");
}

void DefaultChecks() {
  using gb::SurfaceClass;
  for (uint32_t c = 0; c < uint32_t(SurfaceClass::kCount); ++c) {
    const float s = gb::DefaultSmoothness(SurfaceClass(c));
    Check(s >= 0.0f && s <= 1.0f, "the default smoothness of class " + std::to_string(c) + " is in 0..1");
    Check(gb::DefaultLit(SurfaceClass(c)) == (c == uint32_t(SurfaceClass::kEmissive) ? 0.0f : 1.0f), "the lit weight is 0 for emissive and 1 for the rest: class " + std::to_string(c));
  }
  Check(gb::DefaultSmoothness(SurfaceClass::kChrome) > gb::DefaultSmoothness(SurfaceClass::kPaint) && gb::DefaultSmoothness(SurfaceClass::kPaint) > gb::DefaultSmoothness(SurfaceClass::kMetal) &&
            gb::DefaultSmoothness(SurfaceClass::kMetal) > gb::DefaultSmoothness(SurfaceClass::kRoad) && gb::DefaultSmoothness(SurfaceClass::kRoad) > gb::DefaultSmoothness(SurfaceClass::kWorld) &&
            gb::DefaultSmoothness(SurfaceClass::kWorld) > gb::DefaultSmoothness(SurfaceClass::kTerrain),
        "the defaults are ordered chrome > paint > metal > road > world > terrain");
  Check(gb::DefaultSmoothness(SurfaceClass::kWetRoad) > gb::DefaultSmoothness(SurfaceClass::kRoad), "a wet road is smoother than a dry one");
}

// ---------------------------------------------------------------------------
// The gloss mapping and the gloss table
// ---------------------------------------------------------------------------
void GlossMappingChecks() {
  // The formulas: alpha = sqrt(2 / (n + 2)), smoothness = 1 - sqrt(alpha).
  Check(Near(gb::GgxAlphaOfPower(0.0f), 1.0f, 1e-6f), "power 0 is alpha 1");
  Check(Near(gb::GgxAlphaOfPower(2.0f), std::sqrt(0.5f), 1e-6f), "power 2 is alpha sqrt(1/2)");
  Check(Near(gb::GgxAlphaOfPower(48.0f), 0.2f, 1e-6f), "power 48 is alpha 0.2");
  Check(Near(gb::GgxAlphaOfPower(-5.0f), 1.0f, 1e-6f), "a negative power is power 0");
  Check(std::isnan(gb::GgxAlphaOfPower(std::nanf(""))), "a NaN power stays NaN (the caller tests)");
  Check(Near(gb::SmoothnessOfPower(0.0f), 0.0f, 1e-6f), "power 0: smoothness 0");
  Check(Near(gb::SmoothnessOfPower(1.0f), 0.0964f, 1e-3f), "power 1: smoothness 0.10");
  Check(Near(gb::SmoothnessOfPower(10.0f), 0.361f, 1e-3f), "power 10: smoothness 0.36");
  Check(Near(gb::SmoothnessOfPower(100.0f), 0.626f, 1e-3f), "power 100: smoothness 0.63");
  Check(Near(gb::SmoothnessOfPower(1000.0f), 0.789f, 1e-3f), "power 1000: smoothness 0.79");
  Check(gb::SmoothnessOfPower(1e30f) > 0.999f && gb::SmoothnessOfPower(1e30f) <= 1.0f, "a huge power is a mirror, not above 1");
  // Monotonic: a higher power is never rougher, and the bytes follow.
  float last = -1.0f;
  uint32_t last_byte = 0;
  bool monotonic = true;
  bool bytes_monotonic = true;
  for (float n = 0.0f; n < 5000.0f; n += 0.5f) {
    const float s = gb::SmoothnessOfPower(n);
    monotonic = monotonic && s >= last;
    const uint32_t byte = uint32_t(s * 255.0f + 0.5f);
    bytes_monotonic = bytes_monotonic && byte >= last_byte && byte <= 255;
    last = s;
    last_byte = byte;
  }
  Check(monotonic, "smoothness never falls as the power rises");
  Check(bytes_monotonic, "nor do the stored bytes");
  // The perceptual encoding: alpha = (1 - s)^2, a round trip.
  bool round_trip = true;
  for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.01f) {
    round_trip = round_trip && Near(gb::AlphaOfSmoothness(gb::SmoothnessOfAlpha(alpha)), alpha, 1e-5f);
  }
  Check(round_trip, "alpha -> smoothness -> alpha is the identity");
  Check(Near(gb::AlphaOfSmoothness(1.0f), 0.0f, 0.0f) && Near(gb::AlphaOfSmoothness(0.0f), 1.0f, 0.0f), "a mirror is alpha 0, a rough surface alpha 1");
  Check(Near(gb::AlphaOfByte(255), 0.0f, 0.0f) && Near(gb::AlphaOfByte(0), 1.0f, 0.0f) && Near(gb::SmoothnessOfByte(51), 0.2f, 1e-6f), "a stored byte reads back");
  Check(Near(gb::SmoothnessOfAlpha(2.0f), 0.0f, 0.0f) && Near(gb::SmoothnessOfAlpha(-1.0f), 1.0f, 0.0f), "alpha is clamped to 0..1");
  // The stages: the pixel shaders' exponent is half the power, the vertex shaders' is the power.
  Check(gb::GlossPowerScale(gb::GlossBank::kPixel) == 2.0f && gb::GlossPowerScale(gb::GlossBank::kVertex) == 1.0f, "the pixel exponent counts twice");
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kPixel, 20.0f, 0.5f), gb::GlossSmoothness(gb::GlossBank::kVertex, 40.0f, 0.5f), 1e-6f), "pixel exponent 20 = vertex exponent 40");
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kVertex, 50.0f, 0.5f), gb::SmoothnessOfPower(50.0f), 1e-6f), "vertex exponent 50 is power 50");
  // Garbage in the bank: the class default.
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kPixel, std::nanf(""), 0.3f), 0.3f, 0.0f), "a NaN exponent: the fallback");
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kPixel, -1.0f, 0.3f), 0.3f, 0.0f), "a negative exponent: the fallback");
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kPixel, std::numeric_limits<float>::infinity(), 0.3f), 0.3f, 0.0f), "an infinite exponent: the fallback");
  Check(Near(gb::GlossSmoothness(gb::GlossBank::kPixel, 0.0f, 0.3f), 0.0f, 1e-6f), "exponent 0 is a real (rough) material, not the fallback");
}

void GlossTableChecks() {
  Check(gb::kGlossRowCount == 85, "the gloss table has the 85 shaders of the retail game that read SpecularExponent (" + std::to_string(gb::kGlossRowCount) + ")");
  bool sorted = true;
  bool valid = true;
  bool stage_ok = true;
  bool group_known = true;
  size_t vertex_rows = 0;
  for (size_t i = 0; i < gb::kGlossRowCount; ++i) {
    const gb::GlossRow& row = gb::kGlossRows[i];
    if (i > 0) sorted = sorted && gb::kGlossRows[i - 1].name < row.name;
    valid = valid && row.bank != gb::GlossBank::kNone && row.reg < gb::kMaterialBankRegisters;
    const gb::ShaderName split = gb::SplitShaderName(row.name);
    const bool vertex_name = split.entry.starts_with("VS");
    stage_ok = stage_ok && (vertex_name == (row.bank == gb::GlossBank::kVertex));
    vertex_rows += row.bank == gb::GlossBank::kVertex ? 1 : 0;
    group_known = group_known && gb::FindGroupRow(split.group) != nullptr;
  }
  Check(sorted, "the gloss rows are in strictly increasing name order");
  Check(valid, "every gloss row has a bank and a register inside the bank");
  Check(stage_ok, "a row of a VS... entry is the vertex bank's, any other the pixel bank's");
  Check(group_known, "every gloss row's group has a class row");
  Check(vertex_rows == 12, "twelve vertex shaders keep an exponent (the day city shaders are lit per vertex): " + std::to_string(vertex_rows));
  const gb::GlossRow* day = gb::FindGlossRow("xCityDefault__VSCityDefaultDay");
  Check(day != nullptr && day->bank == gb::GlossBank::kVertex && day->reg == 73, "VSCityDefaultDay: vertex bank c73");
  const gb::GlossRow* night = gb::FindGlossRow("xCityBumpSpec__PSMultiLight");
  Check(night != nullptr && night->bank == gb::GlossBank::kPixel && night->reg == 101, "xCityBumpSpec PSMultiLight: pixel bank c101");
  Check(gb::FindGlossRow("xCityDefault__PSCityDefault") == nullptr, "a day shader whose gloss is in its vertex shader has no row of its own");
  Check(gb::FindGlossRow("") == nullptr && gb::FindGlossRow("zzz") == nullptr && gb::FindGlossRow("xCityBumpSpec") == nullptr, "a miss is null");
  Check(gb::FindGlossRow("xAmbientTire__PSAmbientTire") == nullptr, "the tire's low and high pair is not a gloss row");
}

// ---------------------------------------------------------------------------
// The word
// ---------------------------------------------------------------------------
void WordChecks() {
  using gb::SurfaceClass;
  // every field in its place, round trip
  for (uint32_t c = 0; c < uint32_t(SurfaceClass::kCount); ++c) {
    for (uint32_t flags = 0; flags < 8; ++flags) {
      for (const float s : {0.0f, 0.2f, 0.5f, 0.9f, 1.0f}) {
        for (const float lit : {0.0f, 1.0f}) {
          const uint32_t word = gb::MaterialWord(s, c, flags, lit);
          const bool ok = gb::MaterialRt3Smoothness(word) == uint32_t(s * 255.0f + 0.5f) && uint32_t(gb::MaterialClassOf(word)) == c && gb::MaterialFlagsOf(word) == flags &&
                          gb::MaterialLitByte(word) == (lit > 0.5f ? 255u : 0u) && (word >> 24) == 0;
          if (!ok) Check(false, "the word's fields round trip: class " + std::to_string(c) + " flags " + std::to_string(flags));
        }
      }
    }
  }
  Check(true, "the word's fields round trip (every class, flag set, smoothness, lit)");
  const uint32_t word = gb::MaterialWord(0.5f, SurfaceClass::kRoad, gb::kMaterialFlagSsr, 1.0f);
  Check(gb::MaterialWordWithFlags(word, gb::kMaterialFlagAlphaCovered) == gb::MaterialWord(0.5f, SurfaceClass::kRoad, gb::kMaterialFlagSsr | gb::kMaterialFlagAlphaCovered, 1.0f),
        "the alpha covered flag is added to the word");
  Check(gb::MaterialWordWithFlags(word, 0) == word, "no flag, the same word");
  Check(gb::MaterialWordWithFlags(word, 0xFF) >> 24 == 0, "flags beyond the three bits do not leak into the reserved byte");
  Check(gb::MaterialRt3ClassAndFlags(gb::MaterialWordWithFlags(word, gb::kMaterialFlagAlphaCovered)) == (uint32_t(SurfaceClass::kRoad) | ((gb::kMaterialFlagSsr | gb::kMaterialFlagAlphaCovered) << 5)),
        "the class and the flags are in RT3's one byte: class in the low 5 bits, flags above");
  Check(gb::DefaultMaterialWord() == gb::MaterialWord(0.0f, SurfaceClass::kWorld, 0u, 1.0f), "the default word is unchanged");
  static_assert(sizeof(gb::GBufferConstants) == 48, "no field was added to the interface");
}

// ---------------------------------------------------------------------------
// The table file
// ---------------------------------------------------------------------------
std::vector<gb::NamedShader> SomeShaders() {
  return {
      {0x40, true, "xCityBumpSpec__PSMultiLight"},        // known, with a pixel gloss row
      {0x10, true, "xCityDefault__PSCityDefault"},        // known, no gloss
      {0x30, false, "xCityDefault__VSCityDefaultDay"},    // a vertex shader with a gloss row
      {0x20, false, "xCityDefault__VSCityDefaultNight"},  // a vertex shader with none
      {0x50, true, "xSomeNewGroup__PSMultiLight"},        // unknown group
      {0x60, true, "unnamed_60"},                         // unknown
      {0x70, true, "xCityDefault__PS_ShadowBlend"},       // known, class 0
      {0x80, true, "xCityDefault__VSCityDefaultDay"},     // a PIXEL record of a vertex name: the gloss row names the vertex bank, so no gloss; the group is known
      {0x40, true, "xCityNeon__PSMultiLight"},             // a second shader with identity 0x40: the first wins
  };
}

uint8_t FindRow(std::string_view name) { return gb::FindDiffuseSlot(name); }

void DiffuseTableChecks() {
  Check(gb::kDiffuseRowCount == std::size(gb::kDiffuseRows) && gb::kDiffuseRowCount > 50, "the diffuse table has rows (" + std::to_string(gb::kDiffuseRowCount) + ")");
  bool sorted = true, valid = true, known = true;
  for (size_t i = 0; i < gb::kDiffuseRowCount; ++i) {
    const gb::DiffuseRow& row = gb::kDiffuseRows[i];
    if (i > 0) sorted = sorted && gb::kDiffuseRows[i - 1].name < row.name;
    valid = valid && row.slot != 0 && (row.slot < 32 || row.slot == gb::kNoDiffuseSlot) && row.name.find("__") != std::string_view::npos;
    const gb::ShaderMaterial m = gb::MaterialOfShaderName(row.name);
    known = known && m.known && m.cls != gb::SurfaceClass::kNone;
  }
  Check(sorted, "the diffuse rows are sorted by name, strictly");
  Check(valid, "no row says slot 0 (that is the default), every slot is 0..31 or none, every name is group__entry");
  Check(known, "every row is a pixel shader of a classed group (the others never carry the G-buffer's word)");
  Check(gb::FindDiffuseSlot("xCityDefault__PSMultiLight") == 0 && gb::FindDiffuseSlot("nonsense") == 0 && gb::FindDiffuseSlot("") == 0, "no row: slot 0");
  Check(gb::FindDiffuseSlot("xCityWindowLower__PSMultiLight") == 1 && gb::FindDiffuseSlot("xPropGlass__PSPropGlass") == gb::kNoDiffuseSlot, "the window shader's slot 1, the prop glass's none");
}

void BuilderChecks() {
  const std::vector<gb::NamedShader> shaders = SomeShaders();
  const std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(shaders);
  std::map<uint64_t, gb::MaterialRecord> by_key;
  bool sorted = true;
  for (size_t i = 0; i < records.size(); ++i) {
    if (i > 0) sorted = sorted && records[i - 1].key < records[i].key;
    by_key[records[i].key] = records[i];
  }
  Check(sorted, "the records are sorted by identity, strictly");
  Check(records.size() == 5, "five records: " + std::to_string(records.size()));
  Check(by_key.count(0x10) == 1 && by_key[0x10].pixel && by_key[0x10].cls == gb::SurfaceClass::kWorld && by_key[0x10].gloss_bank == gb::GlossBank::kNone, "a pixel shader with no gloss row");
  Check(by_key.count(0x40) == 1 && by_key[0x40].gloss_bank == gb::GlossBank::kPixel && by_key[0x40].gloss_reg == 101 && by_key[0x40].cls == gb::SurfaceClass::kWorld,
        "a pixel shader with a gloss row: bank and register (and the first of two with one identity)");
  Check(by_key.count(0x30) == 1 && !by_key[0x30].pixel && by_key[0x30].gloss_bank == gb::GlossBank::kVertex && by_key[0x30].gloss_reg == 73 && by_key[0x30].cls == gb::SurfaceClass::kNone,
        "a vertex shader: bank and register, class 0");
  Check(by_key.count(0x20) == 0, "a vertex shader without a gloss row has no record");
  Check(by_key.count(0x50) == 0 && by_key.count(0x60) == 0, "unknown groups have no record");
  Check(by_key.count(0x70) == 1 && by_key[0x70].cls == gb::SurfaceClass::kNone, "a known auxiliary shader is a record of class 0 (told from an unknown one)");
  Check(by_key.count(0x80) == 1 && by_key[0x80].gloss_bank == gb::GlossBank::kNone, "a gloss row of the other stage is not taken");
  Check(by_key[0x10].smoothness == gb::ToByte(gb::DefaultSmoothness(gb::SurfaceClass::kWorld)) && by_key[0x10].lit == 255, "the class defaults are in the record");
  Check(gb::BuildMaterialRecords({}).empty(), "no shaders, no records");
  Check(by_key[0x10].diffuse_slot == 0 && by_key[0x30].diffuse_slot == gb::kNoDiffuseSlot, "a pixel shader's diffuse slot is 0 by default, a vertex shader has none");
  {
    const std::vector<gb::NamedShader> reflective = {{0x1, true, "xCityWindowLower__PSMultiLight"}, {0x2, true, "xPropGlass__PSPropGlass"}, {0x3, true, "xCityDefault__PSMultiLight"},
                                                     {0x4, true, "xAmbientVehicle__PS_AmbientVehicle"}};
    const std::vector<gb::MaterialRecord> r = gb::BuildMaterialRecords(reflective);
    Check(r.size() == 4 && r[0].diffuse_slot == 1 && r[1].diffuse_slot == gb::kNoDiffuseSlot && r[2].diffuse_slot == 0 && r[3].diffuse_slot == FindRow("xAmbientVehicle__PS_AmbientVehicle"),
          "the window shader reads its diffuse at slot 1 (slot 0 is its reflection map), the prop glass has none, an ordinary shader slot 0");
  }
  // The order of the input does not matter.
  std::vector<gb::NamedShader> reversed(shaders.rbegin(), shaders.rend());
  const std::vector<gb::MaterialRecord> from_reversed = gb::BuildMaterialRecords(reversed);
  bool same = from_reversed.size() == records.size();
  for (size_t i = 0; same && i < records.size(); ++i) {
    same = records[i].key == from_reversed[i].key && (records[i].key == 0x40 || records[i].cls == from_reversed[i].cls);
  }
  Check(same, "the same records in any input order (but for which of two same-identity shaders comes first)");
}

void FileChecks() {
  const std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(SomeShaders());
  std::vector<uint8_t> bytes;
  std::string error;
  Check(gb::LayOutMaterialTable(records, &bytes, &error), "the file lays out: " + error);
  Check(bytes.size() == gb::kMaterialTableHeaderBytes + records.size() * gb::kMaterialTableEntryBytes, "the file's size is the header and 24 bytes a record");
  Check(std::memcmp(bytes.data(), "MSGM", 4) == 0, "the magic");
  std::vector<gb::MaterialRecord> parsed;
  std::string why;
  Check(gb::ParseMaterialTable(bytes, &parsed, &why), "the file parses: " + why);
  bool equal = parsed.size() == records.size();
  for (size_t i = 0; equal && i < records.size(); ++i) {
    const gb::MaterialRecord& a = records[i];
    const gb::MaterialRecord& b = parsed[i];
    equal = a.key == b.key && a.pixel == b.pixel && a.cls == b.cls && a.flags == b.flags && a.smoothness == b.smoothness && a.lit == b.lit && a.gloss_bank == b.gloss_bank &&
            a.gloss_reg == b.gloss_reg && a.diffuse_slot == b.diffuse_slot && a.metal == b.metal;
  }
  Check(equal, "every record survives the file");
  // an empty table is a file too
  std::vector<uint8_t> empty;
  Check(gb::LayOutMaterialTable({}, &empty, &error) && empty.size() == gb::kMaterialTableHeaderBytes && gb::ParseMaterialTable(empty, &parsed, &why) && parsed.empty(), "an empty table round trips");
  // deterministic
  std::vector<uint8_t> again;
  gb::LayOutMaterialTable(records, &again, &error);
  Check(again == bytes, "the same records, the same bytes");

  // the reader refuses what it should
  const auto refuses = [&](std::vector<uint8_t> broken, const std::string& what) {
    std::vector<gb::MaterialRecord> out;
    std::string reason;
    const bool ok = gb::ParseMaterialTable(broken, &out, &reason);
    Check(!ok && out.empty() && !reason.empty(), "the reader refuses " + what + ": " + reason);
  };
  std::vector<uint8_t> broken = bytes;
  broken[0] = 'X';
  refuses(broken, "a wrong magic");
  broken = bytes;
  broken[4] = uint8_t(gb::kMaterialTableVersion + 1);
  refuses(broken, "another layout version");
  broken = bytes;
  broken[4] = 1;
  refuses(broken, "the first layout version (a pack built before the diffuse slot)");
  broken = bytes;
  broken[4] = 2;
  refuses(broken, "the second layout version (16 byte records, a pack built before the metalness)");
  broken = bytes;
  broken[12] = uint8_t(gb::kMaterialRulesVersion + 1);
  refuses(broken, "another rules version");
  broken = bytes;
  broken.pop_back();
  refuses(broken, "a truncated file");
  broken = bytes;
  broken.push_back(0);
  refuses(broken, "a file with trailing bytes");
  refuses({}, "an empty file");
  refuses(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20), "a header cut short");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 8] = 'q';
  refuses(broken, "an unknown stage");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 9] = 32;
  refuses(broken, "a class above 31");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 10] = 8;
  refuses(broken, "flags above three bits");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 13] = 3;
  refuses(broken, "an unknown bank");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 15] = 32;
  refuses(broken, "a diffuse slot above 31 that is not 'none'");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 17] = 1;
  refuses(broken, "a reserved byte that is set");
  broken = bytes;
  broken[gb::kMaterialTableHeaderBytes + 23] = 0x80;
  refuses(broken, "the last reserved byte set");
  broken = bytes;
  // swap the first two records' keys: out of order
  for (uint32_t i = 0; i < 8; ++i) std::swap(broken[gb::kMaterialTableHeaderBytes + i], broken[gb::kMaterialTableHeaderBytes + gb::kMaterialTableEntryBytes + i]);
  refuses(broken, "records out of order");

  // and the writer refuses what it should
  std::vector<gb::MaterialRecord> bad = records;
  std::swap(bad[0], bad[1]);
  Check(!gb::LayOutMaterialTable(bad, &bytes, &error) && !error.empty(), "the writer refuses unsorted records");
  bad = records;
  bad[0].cls = gb::SurfaceClass(40);
  Check(!gb::LayOutMaterialTable(bad, &bytes, &error), "the writer refuses a class above 31");
  bad = records;
  bad[0].gloss_bank = gb::GlossBank::kNone;
  bad[0].gloss_reg = 5;
  Check(!gb::LayOutMaterialTable(bad, &bytes, &error), "the writer refuses a register with no bank");
  bad = {records[0], records[0]};
  Check(!gb::LayOutMaterialTable(bad, &bytes, &error), "the writer refuses a repeated identity");
  {
    std::vector<gb::MaterialRecord> with_metal = records;
    size_t vertex_at = with_metal.size();
    for (size_t i = 0; i < with_metal.size(); ++i) {
      if (!with_metal[i].pixel) vertex_at = i;
    }
    if (vertex_at < with_metal.size()) {
      with_metal[vertex_at].metal = 7;
      Check(!gb::LayOutMaterialTable(with_metal, &bytes, &error), "the writer refuses a metalness on a vertex shader's record");
      std::vector<uint8_t> good;
      gb::LayOutMaterialTable(records, &good, &error);
      good[gb::kMaterialTableHeaderBytes + vertex_at * gb::kMaterialTableEntryBytes + 16] = 7;
      refuses(good, "a metalness on a vertex shader's record");
    }
  }
}

// ---------------------------------------------------------------------------
// A draw's word
// ---------------------------------------------------------------------------
gb::MaterialTable TableOf(std::vector<gb::NamedShader> shaders) {
  const std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(shaders);
  std::vector<uint8_t> bytes;
  std::string error;
  gb::LayOutMaterialTable(records, &bytes, &error);
  gb::MaterialTable table;
  std::string why;
  Check(table.Load(bytes, &why), "the table loads from its file's bytes: " + why);
  return table;
}

// ---------------------------------------------------------------------------
// PBR-2: the metalness, the family table, the car families' own defaults
// ---------------------------------------------------------------------------
void MetalDefaultChecks() {
  using gb::SurfaceClass;
  int metals = 0;
  for (uint32_t c = 0; c < uint32_t(SurfaceClass::kCount); ++c) {
    const float m = gb::DefaultMetal(SurfaceClass(c));
    Check(m >= 0.0f && m <= 1.0f, "the default metalness of class " + std::to_string(c) + " is in 0..1");
    const bool metal_class = c == uint32_t(SurfaceClass::kChrome) || c == uint32_t(SurfaceClass::kMetal);
    Check((m > 0.0f) == metal_class, "only chrome and metal are metals: class " + std::to_string(c));
    metals += m > 0.0f ? 1 : 0;
  }
  Check(metals == 2, "two metal classes");
  Check(gb::DefaultMetal(SurfaceClass::kChrome) == 1.0f && gb::DefaultMetal(SurfaceClass::kMetal) == 1.0f, "chrome and metal are metal 1");
  Check(gb::DefaultMetal(SurfaceClass::kPaint) == 0.0f && gb::DefaultMetal(SurfaceClass::kGlass) == 0.0f && gb::DefaultMetal(SurfaceClass::kRoad) == 0.0f &&
            gb::DefaultMetal(SurfaceClass::kWater) == 0.0f && gb::DefaultMetal(SurfaceClass::kEmissive) == 0.0f && gb::DefaultMetal(SurfaceClass::kWorld) == 0.0f,
        "paint, glass, asphalt, water, emissive and the world are dielectrics");
}

void FamilyTableChecks() {
  Check(gb::FamilyRowsSorted(), "the family table is sorted by group, strictly");
  std::set<std::string_view> seen;
  bool valid = true;
  bool refines = true;
  bool known = true;
  bool written = true;
  for (const gb::FamilyRow& row : gb::kFamilyRows) {
    valid = valid && seen.insert(row.group).second && (row.smoothness == gb::kClassDefault || (row.smoothness >= 0.0f && row.smoothness <= 1.0f)) &&
            (row.metal == gb::kClassDefault || (row.metal >= 0.0f && row.metal <= 1.0f));
    refines = refines && (row.smoothness != gb::kClassDefault || row.metal != gb::kClassDefault);  // a row that changes nothing is a mistake
    const gb::GroupRow* group = gb::FindGroupRow(row.group);
    known = known && group != nullptr;
    written = written && group != nullptr && group->cls != gb::SurfaceClass::kNone;  // a class 0 group has no material to refine
  }
  Check(valid, "every family row is once in the table, its numbers 0..1 or the class's");
  Check(refines, "every family row changes the smoothness or the metalness");
  Check(known, "every family row names a group of the class table");
  Check(written, "no family row is for a group the G-buffer never writes");
  Check(gb::FindFamilyRow("xWheelRubber") != nullptr && gb::FindFamilyRow("xCityRoad") == nullptr && gb::FindFamilyRow("") == nullptr && gb::FindFamilyRow("xZzz") == nullptr,
        "FindFamilyRow: a hit, a group without a row, an empty name, a name past the end");
  for (const gb::FamilyRow& row : gb::kFamilyRows) {
    Check(gb::FindFamilyRow(row.group) == &row, "FindFamilyRow finds " + std::string(row.group));
  }
}

// What the tables say of a shader by its name, as numbers a test reads.
struct Said {
  gb::SurfaceClass cls;
  float smoothness;
  float metal;
};
Said SaidOf(const char* name) {
  const gb::ShaderMaterial m = gb::MaterialOfShaderName(name);
  Check(m.known, std::string("the shader ") + name + " is known");
  return {m.cls, m.smoothness, m.metal};
}

void FamilyDefaultChecks() {
  using gb::SurfaceClass;
  // the cars, from the family alone (before any census): chrome is a mirror metal, paint a clear coat over a dielectric, glass a dielectric, a tyre rough rubber, lamps emissive
  Said s = SaidOf("xChrome__PS_Chrome");
  Check(s.cls == SurfaceClass::kChrome && s.metal == 1.0f && Near(s.smoothness, 0.95f, 1e-6f), "car chrome: class chrome, metal 1, near a mirror");
  s = SaidOf("xBumpedAlphaChrome__PS_Chrome");
  Check(s.cls == SurfaceClass::kChrome && s.metal == 1.0f, "bumped chrome: metal 1");
  s = SaidOf("xCarPaintCustomizable__PS_CarPaint");
  Check(s.cls == SurfaceClass::kPaint && s.metal == 0.0f && Near(s.smoothness, 0.85f, 1e-6f), "customisable paint: clear coat, a dielectric");
  s = SaidOf("xCarPaint__PS_CarPaint");
  Check(s.cls == SurfaceClass::kPaint && s.metal == 0.0f, "plain paint: a dielectric");
  s = SaidOf("xAmbientCarPaint__PSAmbientCarPaint");
  Check(s.cls == SurfaceClass::kPaint && s.metal == 0.0f, "a traffic car's paint: a dielectric");
  s = SaidOf("xCarGlass__PS_GlassFront");
  Check(s.cls == SurfaceClass::kGlass && s.metal == 0.0f && s.smoothness >= 0.9f, "car glass: a smooth dielectric");
  s = SaidOf("xBlackOpaqueGlass__PS_Glass");
  Check(s.cls == SurfaceClass::kGlass && s.metal == 0.0f && s.smoothness >= 0.85f && s.smoothness < gb::DefaultSmoothness(SurfaceClass::kGlass),
        "the dark glass: a dielectric, a little rougher than a window");
  s = SaidOf("xWheelRubber__PS_ParallaxRubber2_true");
  Check(s.cls == SurfaceClass::kWorld && s.metal == 0.0f && s.smoothness <= 0.12f, "a tyre: a dielectric with a roughness of 0.88 or more");
  s = SaidOf("xAmbientTire__PSAmbientTire");
  Check(s.cls == SurfaceClass::kWorld && s.metal == 0.0f && s.smoothness <= 0.12f, "a traffic car's tyre: rubber too");
  s = SaidOf("xRimMain__PS_Multi");
  Check(s.cls == SurfaceClass::kChrome && s.metal == 1.0f && Near(s.smoothness, 0.85f, 1e-6f), "a rim: polished alloy");
  s = SaidOf("xWheelBrakeRotor__PS_BrakeRotor");
  Check(s.cls == SurfaceClass::kMetal && s.metal == 1.0f && s.smoothness < 0.6f, "a brake rotor: metal, rough");
  s = SaidOf("xbrakecaliper__PS_BrakeCaliper");
  Check(s.cls == SurfaceClass::kPaint && s.metal == 0.0f && Near(s.smoothness, 0.60f, 1e-6f), "a brake caliper is painted: the paint class, a dielectric, a little rougher than a clear coat");
  s = SaidOf("xBrushedMetal__PS_BrushedMetal");
  Check(s.cls == SurfaceClass::kMetal && s.metal == 1.0f && s.smoothness < gb::DefaultSmoothness(SurfaceClass::kMetal), "brushed metal: metal, rougher than the class's default");
  s = SaidOf("xAnisotropicBilletMetal__PS_Billet");
  Check(s.cls == SurfaceClass::kMetal && s.metal == 1.0f, "billet: metal");
  s = SaidOf("xCarbonFiber__PS_CarbonFiber");
  Check(s.cls == SurfaceClass::kPaint && s.metal == 0.0f, "carbon fibre: the paint class, a dielectric");
  s = SaidOf("xCarLight__PS_Light");
  Check(s.cls == SurfaceClass::kEmissive && s.metal == 0.0f, "a lamp lens: emissive, a dielectric");
  // the city
  s = SaidOf("xCityGrimeMetal__PSMultiLight");
  Check(s.cls == SurfaceClass::kMetal && s.metal > 0.5f && s.metal < 1.0f, "grimy metal: metal under dirt");
  s = SaidOf("xPropMetal__PSPropMetal");
  Check(s.cls == SurfaceClass::kMetal && Near(s.metal, 0.9f, 1e-6f) && Near(s.smoothness, gb::DefaultSmoothness(SurfaceClass::kMetal), 1e-6f),
        "a metal prop: the metal class's smoothness, a little rust in the metalness");
  s = SaidOf("xCityRoad__PSMultiLight");
  Check(s.cls == SurfaceClass::kRoad && s.metal == 0.0f && Near(s.smoothness, gb::DefaultSmoothness(SurfaceClass::kRoad), 1e-6f), "asphalt: a rough dielectric, the class's numbers");
  s = SaidOf("xCityDefault__PSMultiLight");
  Check(s.cls == SurfaceClass::kWorld && s.metal == 0.0f, "a plain wall: a dielectric");
  s = SaidOf("xCityWindowLower__PSMultiLight");
  Check(s.cls == SurfaceClass::kGlass && s.metal == 0.0f && s.smoothness >= 0.9f, "a window: a smooth dielectric");
  s = SaidOf("xCityOceanWater__PSCityOceanWater");
  Check(s.cls == SurfaceClass::kWater && s.metal == 0.0f, "water: a dielectric");
  s = SaidOf("xCityNeon__PSMultiLight");
  Check(s.cls == SurfaceClass::kEmissive && s.metal == 0.0f, "neon: emissive");
  // an auxiliary entry of a group with a family row is class 0 and carries nothing of the row
  const gb::ShaderMaterial shadow = gb::MaterialOfShaderName("xWheelRubber__PS_ShadowDepth");
  Check(shadow.known && shadow.cls == SurfaceClass::kNone && shadow.smoothness == 0.0f && shadow.metal == 0.0f, "a shadow pass of the tyre group: no data, nothing of the family");
  // the interior entry turns a wall into an interior surface and keeps the group's own numbers
  const gb::ShaderMaterial interior = gb::MaterialOfShaderName("xCityTunnelTile__PSMultiLightInterior");
  Check(interior.cls == SurfaceClass::kInterior && Near(interior.smoothness, 0.55f, 1e-6f), "the tile group keeps its family smoothness on its interior entry (the class turns interior)");
  // a group the tables do not have: nothing is known
  const gb::ShaderMaterial unknown = gb::MaterialOfShaderName("xNotAGroup__PS");
  Check(!unknown.known && unknown.metal == 0.0f, "an unknown group has no material");

  // the invariants over every group, so that a new row cannot break the families' sense: every group of a metal class has a metalness (the screen consumers read the class and the metal flag, so a
  // dielectric is never filed under a metal class), no group of another class is a metal, and every tyre and rubber group is a rough dielectric
  bool chrome_ok = true;
  bool dielectric_ok = true;
  bool rubber_ok = true;
  for (const gb::GroupRow& row : gb::kGroupRows) {
    if (row.cls == SurfaceClass::kNone) continue;
    const std::string name = std::string(row.group) + "__PSMultiLight";
    const gb::ShaderMaterial m = gb::MaterialOfShaderName(name);
    const bool is_metal_class = row.cls == SurfaceClass::kChrome || row.cls == SurfaceClass::kMetal;
    if (is_metal_class && m.metal == 0.0f) chrome_ok = false;
    if (!is_metal_class && m.metal != 0.0f) dielectric_ok = false;
    if ((row.group.find("Tire") != std::string_view::npos || row.group.find("Rubber") != std::string_view::npos) && (m.metal != 0.0f || m.smoothness > 0.15f)) rubber_ok = false;
  }
  Check(chrome_ok, "every chrome or metal group has a metalness");
  Check(dielectric_ok, "no group of another class is a metal");
  Check(rubber_ok, "every tyre and rubber group is a rough dielectric");
}

void MetalWordChecks() {
  using gb::SurfaceClass;
  const uint64_t kChromePs = 0x1, kRimPs = 0x2, kPaintPs = 0x3, kWallPs = 0x4, kCaliperPs = 0x5, kUnknownPs = 0x6, kRoadPs = 0x7;
  gb::MaterialTable table = TableOf({
      {kChromePs, true, "xChrome__PS_Chrome"},
      {kRimPs, true, "xRimMain__PS_Multi"},
      {kPaintPs, true, "xCarPaint__PS_CarPaint"},
      {kWallPs, true, "xCityDefault__PSMultiLight"},
      {kCaliperPs, true, "xbrakecaliper__PS_BrakeCaliper"},
      {kRoadPs, true, "xCityRoad__PSMultiLight"},
  });
  Bank ps_bank;
  Bank vs_bank;
  const std::span<const uint8_t> ps = ps_bank.bytes;
  const std::span<const uint8_t> vs = vs_bank.bytes;
  Check(gb::MaterialMetalByte(table.WordFor(kChromePs, 0, ps, vs, false)) == 255, "a chrome draw's word has metalness 1");
  Check(gb::MaterialMetalByte(table.WordFor(kRimPs, 0, ps, vs, false)) == 255 && gb::MaterialRt3Smoothness(table.WordFor(kRimPs, 0, ps, vs, false)) == gb::ToByte(0.85f),
        "a rim: metal 1 and the rim's smoothness");
  Check(gb::MaterialMetalByte(table.WordFor(kPaintPs, 0, ps, vs, false)) == 0, "a paint draw's word has metalness 0");
  Check(gb::MaterialMetalByte(table.WordFor(kWallPs, 0, ps, vs, false)) == 0 && gb::MaterialMetalByte(table.WordFor(kRoadPs, 0, ps, vs, false)) == 0, "a wall's and a road's words have metalness 0");
  const uint32_t caliper = table.WordFor(kCaliperPs, 0, ps, vs, false);
  Check(gb::MaterialClassOf(caliper) == SurfaceClass::kPaint, "a painted caliper writes the paint class to RT3");
  Check(gb::MaterialMetalByte(caliper) == 0, "a painted caliper writes a zero metalness byte");
  Check(gb::MaterialRt3Smoothness(caliper) == 153, "a painted caliper writes perceptual roughness 0.40");
  Check(gb::MaterialFlagsOf(caliper) == gb::kMaterialFlagVehicle, "a painted caliper writes the vehicle flag");
  Check(gb::MaterialMetalByte(table.WordFor(kUnknownPs, 0, ps, vs, false)) == 0 && gb::MaterialMetalByte(table.WordFor(0, 0, ps, vs, false)) == 0, "an unknown shader's word has metalness 0");
  // the metal byte does not disturb the other fields, with or without the alpha twin
  const uint32_t plain = table.WordFor(kChromePs, 0, ps, vs, false);
  const uint32_t twin = table.WordFor(kChromePs, 0, ps, vs, true);
  Check((plain & 0x00FFFFFFu) == gb::MaterialWord(gb::DefaultSmoothness(SurfaceClass::kChrome), SurfaceClass::kChrome, gb::kMaterialFlagVehicle, 1.0f) && (twin & 0xFF000000u) == 0xFF000000u &&
            gb::MaterialFlagsOf(twin) == (gb::kMaterialFlagVehicle | gb::kMaterialFlagAlphaCovered),
        "chrome's word: smoothness, class, flags and lit as before, metal in the top byte, the twin's flag on top");
  // the draw's own exponent still beats the family's smoothness but not the family's metalness
  gb::MaterialTable gloss = TableOf({{0x20, true, "xCityGrimeMetal__PSMultiLight"}});
  ps_bank.SetX(100, 30.0f);
  const uint32_t grimy = gloss.WordFor(0x20, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(grimy) == uint32_t(gb::SmoothnessOfPower(60.0f) * 255.0f + 0.5f) && gb::MaterialMetalByte(grimy) == gb::ToByte(0.75f),
        "grimy metal: the smoothness from the exponent, the metalness from the family");
  // the helper
  Check(gb::MaterialWordWithMetalByte(0x00112233u, 0xAB) == 0xAB112233u && gb::MaterialWordWithMetalByte(0xFF112233u, 0) == 0x00112233u, "MaterialWordWithMetalByte sets the top byte only");
  // the pack's records carry the family's metalness
  const std::vector<gb::MaterialRecord> records =
      gb::BuildMaterialRecords(std::vector<gb::NamedShader>{{0x1, true, "xChrome__PS_Chrome"}, {0x2, true, "xCarPaint__PS_CarPaint"}, {0x3, false, "xCityDefault__VSCityDefaultDay"}});
  Check(records.size() == 3 && records[0].metal == 255 && records[1].metal == 0 && !records[2].pixel && records[2].metal == 0, "the records have the metalness: chrome 255, paint 0, a vertex shader 0");
}

void WordForChecks() {
  using gb::SurfaceClass;
  // the shaders of three draws: a night one (the pixel shader holds the exponent), a day one (the vertex shader does), a road at night (no exponent anywhere)
  const uint64_t kNightPs = 0x1001, kDayPs = 0x1002, kDayVs = 0x2001, kRoadPs = 0x1003, kNeonPs = 0x1004, kUnknownPs = 0x1005, kPaintPs = 0x1006, kPlainVs = 0x2002;
  gb::MaterialTable table = TableOf({
      {kNightPs, true, "xCityBumpSpec__PSMultiLight"},
      {kDayPs, true, "xCityDefault__PSCityDefault"},
      {kDayVs, false, "xCityDefault__VSCityDefaultDay"},
      {kRoadPs, true, "xCityRoad__PSMultiLight"},
      {kNeonPs, true, "xCityNeon__PSMultiLight"},
      {kPaintPs, true, "xCarPaint__PS_CarPaint"},
      {kPlainVs, false, "xCityRoad__VS_zPrePass"},
  });
  Check(table.size() == 6, "six records in the table (the plain vertex shader has none): " + std::to_string(table.size()));
  Check(table.Find(0) == nullptr && table.Find(0x9999) == nullptr && table.Find(kNightPs) != nullptr, "Find: a hit, a miss, key 0");

  Bank ps_bank;
  Bank vs_bank;
  const std::span<const uint8_t> ps = ps_bank.bytes;
  const std::span<const uint8_t> vs = vs_bank.bytes;

  // night: the pixel shader's exponent, register 101 of the PIXEL bank
  ps_bank.SetX(101, 20.0f);
  vs_bank.SetX(101, 999.0f);  // the vertex bank's c101 must not be read
  uint32_t word = table.WordFor(kNightPs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == uint32_t(gb::SmoothnessOfPower(40.0f) * 255.0f + 0.5f), "a night draw's smoothness is the pixel exponent's (power 2 x 20)");
  Check(gb::MaterialClassOf(word) == SurfaceClass::kWorld && gb::MaterialFlagsOf(word) == 0 && gb::MaterialLitByte(word) == 255, "and its class, flags and lit weight the group's");
  // alpha test twin
  word = table.WordFor(kNightPs, 0, ps, vs, true);
  Check(gb::MaterialFlagsOf(word) == gb::kMaterialFlagAlphaCovered, "the alpha test twin adds the alpha covered flag");

  // day: the vertex shader's exponent, register 73 of the VERTEX bank; the pixel bank's c73 must not be read
  vs_bank.SetX(73, 50.0f);
  ps_bank.SetX(73, 5.0f);
  word = table.WordFor(kDayPs, kDayVs, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == uint32_t(gb::SmoothnessOfPower(50.0f) * 255.0f + 0.5f), "a day draw's smoothness is the vertex exponent's (power 50)");
  // a day draw with a vertex shader that holds no exponent, or none: the class default
  word = table.WordFor(kDayPs, kPlainVs, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "a vertex shader with no exponent: the class default");
  word = table.WordFor(kDayPs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "no vertex shader: the class default");
  word = table.WordFor(kDayPs, 0x7777, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "a vertex shader the table does not know: the class default");
  // the pixel shader's own exponent wins over a vertex shader's
  word = table.WordFor(kNightPs, kDayVs, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == uint32_t(gb::SmoothnessOfPower(40.0f) * 255.0f + 0.5f), "the pixel shader's exponent wins over the vertex shader's");

  // a road at night: no exponent, the road's class default and its SSR flag
  word = table.WordFor(kRoadPs, 0, ps, vs, false);
  Check(gb::MaterialClassOf(word) == SurfaceClass::kRoad && (gb::MaterialFlagsOf(word) & gb::kMaterialFlagSsr) != 0 &&
            gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kRoad)),
        "a road: its class, its SSR flag, its default smoothness");
  constexpr uint64_t kGaragePs = 0x1007;
  auto garage_table = TableOf({{kGaragePs, true, "xCityGarageFloor__PSMultiLight"}});
  ps_bank.SetX(102, 1.0f);
  word = garage_table.WordFor(kGaragePs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kGarageFloor)),
        "low guest gloss does not remove the indoor floor's dry sheen");
  ps_bank.SetX(102, 1000.0f);
  word = garage_table.WordFor(kGaragePs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) > gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kGarageFloor)),
        "a high guest gloss can sharpen the garage floor's reflection");
  // neon: lit weight 0
  word = table.WordFor(kNeonPs, 0, ps, vs, false);
  Check(gb::MaterialClassOf(word) == SurfaceClass::kEmissive && gb::MaterialLitByte(word) == 0, "neon: emissive, lit weight 0");
  // paint: a vehicle's flag, no SSR
  word = table.WordFor(kPaintPs, 0, ps, vs, false);
  Check(gb::MaterialClassOf(word) == SurfaceClass::kPaint && gb::MaterialFlagsOf(word) == gb::kMaterialFlagVehicle, "paint: the vehicle flag, no SSR");
  // an unknown pixel shader: the default word (with the alpha flag when it is the twin)
  word = table.WordFor(kUnknownPs, kDayVs, ps, vs, false);
  Check(word == gb::DefaultMaterialWord(), "an unknown pixel shader gets the default word");
  word = table.WordFor(kUnknownPs, 0, ps, vs, true);
  Check(word == gb::MaterialWordWithFlags(gb::DefaultMaterialWord(), gb::kMaterialFlagAlphaCovered), "... with the alpha flag for the twin");
  word = table.WordFor(0, 0, ps, vs, false);
  Check(word == gb::DefaultMaterialWord(), "no pixel shader (identity 0): the default word");
  word = table.WordFor(kDayVs, 0, ps, vs, false);
  Check(word == gb::DefaultMaterialWord(), "a vertex shader's identity given as the pixel shader: the default word");

  // garbage in the bank: the class default
  ps_bank.SetX(101, std::nanf(""));
  word = table.WordFor(kNightPs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "a NaN in the bank: the class default");
  ps_bank.SetX(101, -3.0f);
  word = table.WordFor(kNightPs, 0, ps, vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "a negative number in the bank: the class default");
  // a bank too short for the register
  word = table.WordFor(kNightPs, 0, ps.subspan(0, 100), vs, false);
  Check(gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(SurfaceClass::kWorld)), "a bank too short for the register: the class default");
  word = table.WordFor(kNightPs, 0, {}, {}, false);
  Check(gb::MaterialClassOf(word) == SurfaceClass::kWorld, "no banks at all: still the class");

  // the counters
  const gb::MaterialTable::Stats& stats = table.stats();
  Check(stats.words == 18 && stats.unknown == 4 && stats.gloss_pixel == 4 && stats.gloss_vertex == 1,
        "the counters: " + std::to_string(stats.words) + " words, " + std::to_string(stats.unknown) + " unknown, " + std::to_string(stats.gloss_pixel) + " pixel gloss, " +
            std::to_string(stats.gloss_vertex) + " vertex gloss");
  Check(stats.words == stats.unknown + stats.gloss_pixel + stats.gloss_vertex + stats.class_default, "every word is counted once");
  // an empty table: every draw takes the default word
  gb::MaterialTable empty;
  Check(empty.size() == 0 && empty.WordFor(kNightPs, kDayVs, ps, vs, false) == gb::DefaultMaterialWord(), "an empty table: the default word");
  std::string why;
  Check(!empty.Load(std::vector<uint8_t>{1, 2, 3}, &why) && empty.size() == 0, "a file that is not a table leaves the table empty");
}

// GB-H2-FOLLOW: what the deferred light pass lights (MaterialTable::PassLights): the headlight route `pass` keeps the forward headlight on the draws it does not.
void PassLightsChecks() {
  // the eight emissive shaders the night stage holds out of the pass (xCityNeon, xCityStoreSign, the tints and the lamp glass, their interior twins among them), a lit road, a lit prop, a sky shader
  // (class none), a vertex shader that shares a group with a lit one, a shader the table does not have
  const uint64_t kNeon = 0x2101, kNeonTint = 0x2102, kStoreSign = 0x2103, kLampGlass = 0x2104, kLampGlassInterior = 0x2105, kPropLampGlass = 0x2106, kPropNeon = 0x2107, kRoad = 0x2108, kBump = 0x2109,
                 kSky = 0x210A, kRoadVs = 0x210B;
  gb::MaterialTable table = TableOf({
      {kNeon, true, "xCityNeon__PSMultiLight"},
      {kNeonTint, true, "xCityNeonTint__PSMultiLight"},
      {kStoreSign, true, "xCityStoreSign__PSMultiLight"},
      {kLampGlass, true, "xCityLampGlassTint__PSMultiLight"},
      {kLampGlassInterior, true, "xCityLampGlassTint__PSMultiLightInterior"},
      {kPropLampGlass, true, "xPropLampGlassTint__PSMultiLight"},
      {kPropNeon, true, "xPropNeonTint__PSMultiLight"},
      {kRoad, true, "xCityRoad__PSMultiLight"},
      {kBump, true, "xCityBumpSpec__PSMultiLight"},
      {kSky, true, "xSkyhat__PSSkyhat"},
      {kRoadVs, false, "xCityDefault__VSCityDefaultDay"},
  });
  for (const uint64_t emissive : {kNeon, kNeonTint, kStoreSign, kLampGlass, kLampGlassInterior, kPropLampGlass, kPropNeon}) {
    const gb::MaterialRecord* record = table.Find(emissive);
    Check(record != nullptr && record->cls == gb::SurfaceClass::kEmissive && record->lit == 0 && !table.PassLights(emissive), "an emissive shader is one the pass does not light (class emissive, lit weight 0)");
  }
  Check(table.PassLights(kRoad) && table.PassLights(kBump), "a lit shader is lit by the pass");
  Check(table.Find(kSky) != nullptr && table.Find(kSky)->cls == gb::SurfaceClass::kNone && !table.PassLights(kSky), "a shader of the class none is not lit by the pass");
  Check(table.PassLights(0x9999) && table.PassLights(0), "a shader the table does not have gets the default word, which the pass lights (so the planes and the forward stage agree)");
  Check(table.Find(kRoadVs) != nullptr && !table.Find(kRoadVs)->pixel && table.PassLights(kRoadVs), "a vertex shader's record says nothing about a pixel shader's pixels");
  gb::MaterialTable empty;
  Check(empty.PassLights(kNeon), "with no table every draw has the default word, and the pass lights it");
  // the rule follows the weight alone too: a record of a lit class with a lit weight of 0 is not lit
  const std::vector<gb::NamedShader> one = {{0x3001, true, "xCityRoad__PSMultiLight"}};
  std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(one);
  Check(records.size() == 1 && records[0].lit != 0, "the road's record has a lit weight");
  records[0].lit = 0;
  std::vector<uint8_t> bytes;
  std::string error, why;
  gb::MaterialTable zero;
  Check(gb::LayOutMaterialTable(records, &bytes, &error) && zero.Load(bytes, &why) && !zero.PassLights(0x3001), "a record with a lit weight of 0 is not lit by the pass, whatever its class");
}

// ---------------------------------------------------------------------------
// The tables against the game's translated HLSL (given the baseline folder)
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t from = 0;
  while (true) {
    const size_t tab = line.find('\t', from);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(from));
      break;
    }
    fields.push_back(line.substr(from, tab - from));
    from = tab + 1;
  }
  return fields;
}

// The fetch slot of a translated pixel shader's primary texture, from its text (the rule of gbuffer_material.h's diffuse table): the samplers' `X_Texture2DDescriptorIndex` defines give
// each one's slot (the byte offset into the shared constants over 4; the 3D and cube defines are 128 and 256 bytes further), the uses outside the defines tell which are read; the
// DiffuseSampler's slot where it is read, else the lowest used sampler that is not a reflection map, none (255) when only reflection maps are read. -1: no sampler is read at all.
int PrimaryTextureSlot(const std::string& text) {
  static const char* const kinds[] = {"_Texture2DDescriptorIndex", "_Texture3DDescriptorIndex", "_TextureCubeDescriptorIndex"};
  std::map<std::string, uint32_t> slots;
  std::set<std::pair<uint32_t, std::string>> used;
  std::vector<std::string> body;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("#define ", 0) == 0) {
      for (const char* kind : kinds) {
        const size_t at = line.find(kind);
        const size_t offset_at = line.find("SharedConstants + ");
        if (at == std::string::npos || offset_at == std::string::npos) continue;
        const std::string name = line.substr(8, at - 8);
        if (name.empty() || name.find(' ') != std::string::npos) continue;
        slots.emplace(name, uint32_t(std::strtoul(line.c_str() + offset_at + std::strlen("SharedConstants + "), nullptr, 10) / 4 % 32));
      }
    } else {
      body.push_back(line);
    }
  }
  for (const std::string& text_line : body) {
    for (const char* kind : kinds) {
      for (size_t at = text_line.find(kind); at != std::string::npos; at = text_line.find(kind, at + 1)) {
        size_t begin = at;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(text_line[begin - 1])) || text_line[begin - 1] == '_')) --begin;
        const std::string name = text_line.substr(begin, at - begin);
        const auto it = slots.find(name);
        if (it != slots.end()) used.insert({it->second, name});
      }
    }
  }
  if (used.empty()) return -1;
  for (const auto& [slot, name] : used) {
    if (name == "DiffuseSampler") return int(slot);
  }
  for (const auto& [slot, name] : used) {
    if (name.find("Reflection") == std::string::npos) return int(slot);
  }
  return int(gb::kNoDiffuseSlot);
}

void BaselineChecks(const std::string& baseline) {
  std::set<std::string> groups;
  std::map<std::string, std::pair<gb::GlossBank, uint32_t>> declared;  // name -> bank, register, from the HLSL
  std::map<std::string, int> primary;  // pixel shader name -> PrimaryTextureSlot, from the HLSL
  std::vector<gb::NamedShader> named;
  std::vector<std::string> names;
  size_t shaders = 0;
  size_t unreadable = 0;
  for (const char* sub : {"hlsl-rpf", "hlsl-xex"}) {
    const std::string folder = baseline + "/" + sub + "/";
    std::ifstream report(folder + "shader_report.tsv");
    Check(bool(report), std::string("the shader report of ") + sub + " is readable");
    std::string header;
    if (!std::getline(report, header)) continue;
    const std::vector<std::string> columns = SplitTabs(header);
    const auto column = [&](const char* name) {
      for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == name) return int(i);
      }
      return -1;
    };
    const int c_group = column("group"), c_entry = column("entry_point"), c_stage = column("stage"), c_hlsl = column("hlsl");
    Check(c_group >= 0 && c_entry >= 0 && c_stage >= 0 && c_hlsl >= 0, "the report has the columns group, entry_point, stage, hlsl");
    if (c_group < 0 || c_entry < 0 || c_stage < 0 || c_hlsl < 0) continue;
    std::string line;
    while (std::getline(report, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::vector<std::string> f = SplitTabs(line);
      if (f.size() <= size_t(std::max({c_group, c_entry, c_stage, c_hlsl}))) continue;
      ++shaders;
      groups.insert(f[c_group]);
      const std::string name = f[c_group] + "__" + f[c_entry];
      const std::string text = ReadFile(folder + f[c_hlsl]);
      if (text.empty()) {
        ++unreadable;
        continue;
      }
      const size_t at = text.find("float4 SpecularExponent : packoffset(c");
      if (at != std::string::npos) {
        const uint32_t reg = uint32_t(std::strtoul(text.c_str() + at + std::strlen("float4 SpecularExponent : packoffset(c"), nullptr, 10));
        declared[name] = {f[c_stage] == "vs" ? gb::GlossBank::kVertex : gb::GlossBank::kPixel, reg};
      }
      if (f[c_stage] != "vs") {
        const int slot = PrimaryTextureSlot(text);
        const auto [it, fresh] = primary.emplace(name, slot);
        if (!fresh && it->second != slot) Check(false, "two translated shaders of one name read their primary texture from different slots: " + name);
      }
      names.push_back(name);
      named.push_back({0, f[c_stage] != "vs", {}});
    }
  }
  std::printf("baseline: %zu shaders, %zu groups, %zu declare SpecularExponent, %zu unreadable\n", shaders, groups.size(), declared.size(), unreadable);
  Check(groups.size() == kRetailGroups, "the reports hold 192 groups: " + std::to_string(groups.size()));
  Check(unreadable == 0, "every translated shader's text is readable");
  for (const std::string& group : groups) {
    Check(gb::FindGroupRow(group) != nullptr, "the group " + group + " of the game has a class row");
  }
  for (const gb::GroupRow& row : gb::kGroupRows) {
    Check(groups.count(std::string(row.group)) == 1, "the row " + std::string(row.group) + " is a group of the game");
  }
  for (const auto& [name, where] : declared) {
    const gb::GlossRow* row = gb::FindGlossRow(name);
    Check(row != nullptr && row->bank == where.first && row->reg == where.second, "the gloss row of " + name + " matches its text: c" + std::to_string(where.second));
  }
  for (const gb::GlossRow& row : gb::kGlossRows) {
    const auto it = declared.find(std::string(row.name));
    Check(it != declared.end(), "the gloss row " + std::string(row.name) + " is a shader that declares SpecularExponent");
  }
  // the diffuse table: every classed pixel shader has the slot its text says (no row: slot 0, also for a shader that reads no sampler at all), and every row is such a shader
  size_t classed = 0, off_zero = 0;
  for (const auto& [name, slot] : primary) {
    const gb::ShaderMaterial m = gb::MaterialOfShaderName(name);
    if (!m.known || m.cls == gb::SurfaceClass::kNone) continue;
    ++classed;
    const int want = slot < 0 ? 0 : slot;
    off_zero += want != 0 ? 1 : 0;
    Check(int(gb::FindDiffuseSlot(name)) == want, "the diffuse slot of " + name + " is the text's: " + std::to_string(want));
  }
  for (const gb::DiffuseRow& row : gb::kDiffuseRows) {
    const auto it = primary.find(std::string(row.name));
    Check(it != primary.end() && it->second == int(row.slot), "the diffuse row " + std::string(row.name) + " is a pixel shader whose text reads its primary texture at slot " + std::to_string(int(row.slot)));
  }
  std::printf("baseline: %zu classed pixel shaders, %zu with a primary texture other than slot 0 (the table has %zu rows)%c", classed, off_zero, gb::kDiffuseRowCount, 10);
  Check(off_zero == gb::kDiffuseRowCount, "the table has exactly the classed pixel shaders whose primary texture is not at slot 0");
  // every lit shader of a known group gets a record through the builder's own path: build the records the pack build would write for this game
  std::vector<gb::NamedShader> all;
  uint64_t key = 1;
  for (size_t i = 0; i < names.size(); ++i) {
    all.push_back({key++, named[i].pixel, names[i]});
  }
  const std::vector<gb::MaterialRecord> records = gb::BuildMaterialRecords(all);
  size_t pixel_records = 0, pixel_shaders = 0, vertex_records = 0, nones = 0, with_gloss = 0;
  for (const gb::NamedShader& s : all) pixel_shaders += s.pixel ? 1 : 0;
  for (const gb::MaterialRecord& r : records) {
    pixel_records += r.pixel ? 1 : 0;
    vertex_records += r.pixel ? 0 : 1;
    nones += r.pixel && r.cls == gb::SurfaceClass::kNone ? 1 : 0;
    with_gloss += r.gloss_bank != gb::GlossBank::kNone ? 1 : 0;
  }
  std::printf("baseline: records %zu = %zu pixel (of %zu pixel shaders; %zu class 0) + %zu vertex; %zu with a gloss register\n", records.size(), pixel_records, pixel_shaders, nones,
              vertex_records, with_gloss);
  size_t by_class[16] = {};
  for (const gb::MaterialRecord& r : records) {
    if (r.pixel) ++by_class[uint32_t(r.cls) & 15u];
  }
  std::printf("baseline: pixel shaders by class:");
  for (uint32_t c = 0; c < 16; ++c) std::printf(" %u=%zu", c, by_class[c]);
  std::printf("\n");
  Check(pixel_records == pixel_shaders, "every pixel shader of the game is in the table (its group is classed)");
  Check(vertex_records == 12 && with_gloss == declared.size(), "the vertex shaders with an exponent and every gloss row are in the table");
}

}  // namespace

int main(int argc, char** argv) {
  TableShapeChecks();
  DiffuseTableChecks();
  ClassSpotChecks();
  NameChecks();
  DefaultChecks();
  GlossMappingChecks();
  GlossTableChecks();
  WordChecks();
  BuilderChecks();
  FileChecks();
  MetalDefaultChecks();
  FamilyTableChecks();
  FamilyDefaultChecks();
  MetalWordChecks();
  WordForChecks();
  PassLightsChecks();
  if (argc > 1) BaselineChecks(argv[1]);
  std::printf("gbuffer_material_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
