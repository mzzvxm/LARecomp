// Check of the city in the car paint's reflections (city_refl_rules.h, CITY-RT M2 R1 and R2): the mask, the level a setting comes to on every class and tier (against rt_tier.h's own table), the cells by level and
// budget class, the opponents' budget, the weight of a hit by its distance (against car_rt_hit_rules.h's HitFade), the footprint, the lamp schedule (against the headlight rig's night curve), the constants
// rows the shader reads, the counters and the census lines (against the report's own parser, perf/scripts/city_rt_m2_report.py); and, given the src folder, that hlsl/city_refl.hlsli, hlsl/car_rt_refl_cs.hlsl, the
// opponents' variant, the host and the settings files hold the header's numbers.
// REFL-LAMPS: the lamp flag in the rows and the library's flags, the lamp term's decision and glow factor (LampTermFor, LampFlags, GlowFactorFor), the lamp census words and line, and the shader, host and setting text.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_city_refl_rules_test, then run src/gpu/larecomp_city_refl_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_hit_rules.h"
#include "city_refl_rules.h"
#include "gbuffer_types.h"
#include "headlight_rig_rules.h"
#include "screen_frame.h"

namespace cr = mcla::gpu::city_refl;
namespace gc = mcla::gpu::gpu_class;
namespace cm = mcla::gpu::city_rt;
using mcla::gpu::TextureScope;
using mcla::gpu::gpu_class::GpuClass;
using mcla::gpu::gpu_class::RtTier;

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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t CountOf(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
  return n;
}

// The number a `static const <type> <name> = <value>;` line of an HLSL text holds (u or f suffix and a trailing .0 allowed); NaN when the line is not there.
double HlslNumber(const std::string& text, const std::string& name) {
  const std::regex re("(?:uint|float|int)\\s+" + name + "\\s*=\\s*(-?[0-9.]+)[uf]?\\s*;");
  std::smatch m;
  if (!std::regex_search(text, m, re)) {
    return std::nan("");
  }
  return std::stod(m[1].str());
}

const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
const RtTier kTiers[] = {RtTier::kNone, RtTier::kWeak, RtTier::kStrong};
const char* const kTexts[] = {"auto", "off", "high", "ultra", "", "garbage", "HIGH"};

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : "";
  if (src.empty()) {
    src = __FILE__;
    const size_t cut = src.find_last_of("/\\");
    src = cut == std::string::npos ? "." : src.substr(0, cut);
    src += "/..";
  }

  // ---- the mask ---------------------------------------------------------------------------------------------------------------------------------
  Check(cr::kPartCity == 0x80 && cr::kPartCity == cm::kMaskCity, "the city's mask bit is the structures' own, 0x80");
  Check((cr::kPartCity & 0x7F) == 0 && (mcla::gpu::car_rt_refl::kPartAll & cr::kPartCity) == 0, "the city's bit is above every car part (0x7F: body, glass, tyres, driver, others, chrome, paint)");
  Check((mcla::gpu::car_rt_refl::kPrimaryTraceParts & cr::kPartCity) == 0, "the primary ray never takes the city's bit");
  {
    const uint32_t parts = mcla::gpu::car_rt_refl::kDefaultReflectionParts;
    Check(cr::ReflectionParts(parts, cm::Level::kHigh) == (parts | 0x80) && cr::ReflectionParts(parts, cm::Level::kUltra) == (parts | 0x80), "a level adds the city's bit to the reflection ray's parts");
    Check(cr::ReflectionParts(parts, cm::Level::kOff) == parts && cr::ReflectionParts(parts | 0x80, cm::Level::kOff) == parts, "off leaves the parts as they were and takes a stray city bit away");
    Check(cr::CastLength(parts, 18.0f, 150.0f) == 18.0f && cr::CastLength(parts | 0x80, 18.0f, 150.0f) == 150.0f && cr::CastLength(parts | 0x80, 18.0f, 5.0f) == 18.0f,
          "the ray is cast as far as the longest reach of what it may hit");
  }
  Check(cr::ClassesFor(cm::Level::kOff) == 0 && cr::ClassesFor(cm::Level::kHigh) == cr::kServedClasses && cr::ClassesFor(cm::Level::kUltra) == cr::kServedClasses,
        "the paint of the player's car and of other cars, the chrome and the glass are served at high and ultra, nothing at off (R1, R2, R3)");
  Check(cr::kServedClasses == (cr::kClassPaintPlayer | cr::kClassPaintOthers | cr::kClassChrome | cr::kClassGlass) && cr::Serves(cm::Level::kHigh, cr::kClassChrome) &&
            cr::Serves(cm::Level::kUltra, cr::kClassGlass) && !cr::Serves(cm::Level::kOff, cr::kClassChrome) && !cr::Serves(cm::Level::kOff, cr::kClassGlass),
        "chrome and glass are served (R3), by the level only: Serves says no at off");

  // ---- the level of a setting on every class and tier ----------------------------------------------------------------------------------------------
  int combos = 0;
  for (GpuClass cls : kClasses) {
    for (RtTier tier : kTiers) {
      for (const char* text : kTexts) {
        ++combos;
        const cm::Level level = cr::LevelFor(cls, tier, text);
        const RtTier rung = gc::RtRung(cls, tier);
        const std::string name = std::string("class ") + std::to_string(int(cls)) + ", tier " + std::to_string(int(tier)) + ", '" + text + "'";
        if (rung == RtTier::kNone) {
          Check(level == cm::Level::kOff, name + ": no ray tracing rung, off whatever the text says");
          continue;
        }
        const std::string t = text;
        if (t == "off") {
          Check(level == cm::Level::kOff, name + ": off is off");
        } else if (t == "high") {
          Check(level == cm::Level::kHigh, name + ": high by hand runs on any card that traces");
        } else if (t == "ultra") {
          Check(level == cm::Level::kUltra, name + ": ultra by hand runs on any card that traces");
        } else {
          // auto, and what the list does not know (HIGH in capitals included: the setting list is lower case): rt_tier.h's own cell
          const std::string cell = gc::ResolveRtFor(cls, tier, gc::RtEffect::kRtReflections, "auto");
          Check(level == cm::LevelOfCell(cell), name + ": auto is rt_tier.h's cell of kRtReflections '" + cell + "'");
        }
      }
    }
  }
  Check(combos == 3 * 3 * 7, "every class, tier and text was looked at");
  // the owner's goal is a strong card's look: strong -> high, weak and none -> off (the brief of R1)
  Check(cr::LevelFor(GpuClass::kHigh, RtTier::kStrong, "auto") == cm::Level::kOff, "legacy Auto is off on a strong High card");
  Check(cr::LevelFor(GpuClass::kHigh, RtTier::kWeak, "auto") == cm::Level::kOff && cr::LevelFor(GpuClass::kHigh, RtTier::kNone, "auto") == cm::Level::kOff, "auto on a weak card or none is off");
  Check(cr::LevelFor(GpuClass::kMid, RtTier::kStrong, "auto") == cm::Level::kOff, "auto on a Mid class is off (its rung is at most weak)");
  Check(cr::LevelFor(GpuClass::kLow, RtTier::kStrong, "ultra") == cm::Level::kOff, "the Low class never runs it");
  Check(std::string(gc::RtCell(gc::RtEffect::kRtReflections, RtTier::kNone)) == "off" && std::string(gc::RtCell(gc::RtEffect::kRtReflections, RtTier::kWeak)) == "off" &&
            std::string(gc::RtCell(gc::RtEffect::kRtReflections, RtTier::kStrong)) == "off",
        "rt_tier.h's cells of mcla_gpu_rt_reflections: off on all rungs");

  // ---- the cells ----------------------------------------------------------------------------------------------------------------------------------
  {
    const cr::Cell off = cr::CellFor(cm::Level::kOff, GpuClass::kHigh);
    Check(off.level == cm::Level::kOff && off.reach == 0.0f && off.opp_texels == 0 && off.textures == TextureScope::kNone && !off.coat_layer, "off is an empty cell");
    Check(cr::CellFor(cm::Level::kHigh, GpuClass::kLow) == cr::Cell{} && cr::CellFor(cm::Level::kUltra, GpuClass::kLow) == cr::Cell{}, "the Low class has no cell");
    const cr::Cell hh = cr::CellFor(cm::Level::kHigh, GpuClass::kHigh);
    const cr::Cell uh = cr::CellFor(cm::Level::kUltra, GpuClass::kHigh);
    const cr::Cell hm = cr::CellFor(cm::Level::kHigh, GpuClass::kMid);
    const cr::Cell um = cr::CellFor(cm::Level::kUltra, GpuClass::kMid);
    Check(hh.reach == 150.0f && hh.fade_from == 0.6f && hh.cone_rays == 4 && hh.opp_texels == 250000 && hh.opp_rays == 2, "HIGH on the High class: 150 m, the cone's four rays, the opponents' 250 k texels at 2 rays");
    Check(uh.reach == 1000.0f && uh.fade_from == 0.3f && uh.cone_rays == 4 && uh.opp_texels == 500000 && uh.opp_rays == 4, "ULTRA on the High class: 1,000 m with the weight full to 300 m, 500 k texels at 4 rays");
    Check(hm.reach == 60.0f && hm.cone_rays == 2 && um.reach == 150.0f && um.cone_rays == 2, "a Mid class by hand: 60 m at high, 150 m at ultra, the cone's outer pair");
    Check(uh.reach > hh.reach && hh.reach > hm.reach && um.reach > hm.reach && uh.opp_texels > hh.opp_texels && hh.opp_texels > hm.opp_texels, "reach and budget rise with the level and the class");
    for (const cr::Cell& c : {hh, uh, hm, um}) {
      Check(c.textures == TextureScope::kAll && c.coat_layer && c.level != cm::Level::kOff && c.fade_from > 0.0f && c.fade_from < 1.0f && c.opp_rays >= 1 && c.opp_rays <= 4 && c.cone_rays >= 1 && c.cone_rays <= 4,
            "an on cell reads every texture, takes the coat layer and has sane numbers");
    }
    // the structures' ring covers the reach (city_rt_rules.h BudgetFor: the hi ring), the far level of detail the long reach
    const cm::Budget bh = cm::BudgetFor(GpuClass::kHigh, cm::Level::kHigh);
    const cm::Budget bu = cm::BudgetFor(GpuClass::kHigh, cm::Level::kUltra);
    Check(bh.hi_radius >= hh.reach && bu.hi_radius >= 0.5f * uh.reach && bu.far_radius >= uh.reach, "the structures' rings (300 m / 600 m, far 2,000 / 3,000 m) cover each level's reach");
    const cm::Budget bm = cm::BudgetFor(GpuClass::kMid, cm::Level::kHigh);
    Check(bm.hi_radius >= hm.reach, "a Mid card's ring (150 m) covers its 60 m reach");
    // the debug override
    Check(cr::WithReach(hh, 0.0f) == hh && cr::WithReach(hh, 40.0f).reach == 40.0f && cr::WithReach(hh, 1.0f).reach == 5.0f && cr::WithReach(hh, 1e6f).reach == 3000.0f && cr::WithReach(off, 40.0f) == off,
          "the reach override: 0 keeps the level's, else 5 to 3000 m, never on an off cell");
    // what the scene service is asked
    const cr::Wants w = cr::WantsFor(uh);
    Check(w.cars && w.city == cm::Level::kUltra && w.hit == cm::Level::kUltra && w.textures == TextureScope::kAll, "an on cell asks for the cars, the structures and the hit data at its level and every texture");
    const cr::Wants none = cr::WantsFor(off);
    Check(!none.cars && none.city == cm::Level::kOff && none.hit == cm::Level::kOff && none.textures == TextureScope::kNone, "an off cell asks for nothing");
    // the opponents' budget
    Check(cr::OppBudget(500000, hh) == 250000 && cr::OppBudget(200000, hh) == 200000 && cr::OppBudget(500000, uh) == 500000 && cr::OppBudget(500000, off) == 500000 && cr::OppBudget(0, uh) == 0,
          "the opponents' texel budget is the smaller of the class's and the cell's, the class's with the city off");
    Check(cr::OppRays(4, hh) == 2 && cr::OppRays(1, hh) == 1 && cr::OppRays(2, uh) == 2 && cr::OppRays(4, uh) == 4 && cr::OppRays(4, off) == 4 && cr::OppRays(4, hm) == 1, "the opponents' rays a texel are capped by the cell");
  }

  // ---- the fade, the footprint ----------------------------------------------------------------------------------------------------------------------
  {
    bool same = true;
    for (float reach : {60.0f, 150.0f, 1000.0f}) {
      for (float from : {0.3f, 0.6f, 0.75f}) {
        for (int i = 0; i <= 200; ++i) {
          const float d = reach * 1.2f * float(i) / 200.0f;
          same = same && std::fabs(cr::CityFade(d, reach, from) - mcla::gpu::car_rt_hit::HitFade(d, reach, from)) < 1e-6f;
        }
      }
    }
    Check(same, "the city's fade is the car hits' HitFade");
    Check(cr::CityFade(0.0f, 150.0f, 0.6f) == 1.0f && cr::CityFade(90.0f, 150.0f, 0.6f) == 1.0f && cr::CityFade(150.0f, 150.0f, 0.6f) == 0.0f && cr::CityFade(200.0f, 150.0f, 0.6f) == 0.0f &&
              std::fabs(cr::CityFade(120.0f, 150.0f, 0.6f) - 0.5f) < 1e-6f && cr::CityFade(10.0f, 0.0f, 0.6f) == 0.0f,
          "the fade: 1 up to 60% of the reach, half at its middle, 0 at the reach");
    float last = 2.0f;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
      const float f = cr::CityFade(1000.0f * float(i) / 100.0f, 1000.0f, 0.3f);
      monotone = monotone && f <= last + 1e-6f;
      last = f;
    }
    Check(monotone, "the fade never rises with the distance");
    const float texel = cr::TexelTan(0.414f, 1080.0f);
    Check(std::fabs(texel - 2.0f * 0.414f / 1080.0f) < 1e-9f, "a history texel's angular size is 2 tan over the image's height");
    Check(cr::HitFootprint(100.0f, 0.0f, texel) == 100.0f * texel && std::fabs(cr::HitFootprint(100.0f, 0.0437f, texel) - (8.74f + 100.0f * texel)) < 1e-3f, "the footprint: a mirror is a texel's spread, a cone adds 2 t tan");
    Check(cr::HitFootprint(100.0f, 0.0f, texel) < 0.1f, "a mirror hit at 100 m is under 10 cm wide at 1080 lines (sharp lamp heads)");
  }

  // ---- what a city hit is to the texel's mix (the pass sets visible 1 and `car` by the coat flag: the rules are car_rt_hit_rules.h's) ------------------------------------------------------------
  {
    namespace ch = mcla::gpu::car_rt_hit;
    // with the coat flag a city hit is a mirror's: the mirror's share is at least 0.9 where the mirror hit the city, a cone ray on the city where the mirror missed is dropped
    Check(ch::MirrorShare(0.45f, true) == ch::kOthersSharpShare && ch::MirrorShare(0.95f, true) == 0.95f && ch::MirrorShare(0.45f, false) == 0.45f, "a mirror hit on the city (coat flag) is at least 0.9 of the texel");
    Check(!ch::ConeHitCounts(true, true, false) && ch::ConeHitCounts(true, true, true) && ch::ConeHitCounts(false, true, false), "a cone hit on the city is dropped where the mirror missed it (coat flag), kept where it hit, kept without the flag");
    Check(ch::ConeHitCounts(true, false, false), "a level with no mirror ray (the Mid class's cell) keeps the cone's city hits");
    // a texel whose rays all hit the city at the fade f weighs f and shows the mean of the hits' colours (visible 1: no stand-in): the lobes add by the share
    ch::Lobe cone, coat, mixed;
    const float f = cr::CityFade(100.0f, 150.0f, 0.6f);
    cone.weight = f;
    cone.colour[0] = 0.8f; cone.colour[1] = 0.6f; cone.colour[2] = 0.4f;
    cone.car_weight = f;  // the coat flag: counts for the code's sign and the paint's coat layer
    coat = cone;
    coat.colour[0] = 0.2f;
    float sharp = 0.0f;
    mixed = ch::MixCoat(cone, coat, 0.9f, &sharp);
    Check(std::fabs(mixed.weight - f) < 1e-6f && std::fabs(mixed.car_weight - f) < 1e-6f && std::fabs(mixed.colour[0] - (0.1f * 0.8f + 0.9f * 0.2f)) < 1e-5f && std::fabs(sharp - 0.9f) < 1e-5f,
          "two lobes on the city at the same fade: weight f, the colours by the share, the mirror's part of the weight 0.9");
    Check(f > 0.0f && f < 1.0f, "a hit at 100 m of a 150 m reach weighs between 0 and 1 (the environment map takes the rest)");
  }

  // ---- the lamp schedule ------------------------------------------------------------------------------------------------------------------------------
  {
    bool same = true;
    for (int i = -100; i <= 100; ++i) {
      const double s = double(i) / 100.0;
      same = same && std::fabs(double(cr::LampSchedule(float(s), true)) - mcla::gpu::headlight_rules::NightFactor(s)) < 2e-4;
    }
    Check(same, "the lamp schedule is the headlight rig's night curve of the sun's sine");
    Check(cr::LampSchedule(1.0f, true) == 0.0f && cr::LampSchedule(-0.5f, true) == 1.0f && cr::LampSchedule(std::sin(3.0f * 3.14159265f / 180.0f), true) > 0.999f &&
              cr::LampSchedule(std::sin(12.0f * 3.14159265f / 180.0f), true) < 1e-3f && cr::LampSchedule(-0.5f, false) == 0.0f,
          "the lamps are on from 3 degrees down, off from 12 up, and off where the frame has no sun direction");
    Check(cr::kNightFullDegrees == float(mcla::gpu::headlight_rules::kNightFull) && cr::kNightNoneDegrees == float(mcla::gpu::headlight_rules::kNightNone), "the curve's two ends are the rig's");
  }

  // ---- the picture's mips ---------------------------------------------------------------------------------------------------------------------------
  {
    bool same = true;
    for (uint32_t size : {1u, 2u, 3u, 540u, 1080u, 1081u, 1920u}) {
      for (uint32_t level = 0; level < 8; ++level) {
        same = same && cr::LevelSize(size, level) == mcla::gpu::screen::HizLevelSize(size, level);
      }
    }
    Check(same, "a colour_post level's size is the screen owner's (halved, the odd edge up)");
    Check(cr::ClampMip(-1.0f, 5) == 0.0f && cr::ClampMip(2.5f, 5) == 2.5f && cr::ClampMip(9.0f, 5) == 4.0f && cr::ClampMip(3.0f, 0) == 0.0f, "the mip is clamped to the chain");
    Check(cr::kMaxColourLevels == mcla::gpu::screen::kColourPostMaxLevels, "the rows hold as many levels as the screen owner can make");
  }

  // ---- the constants rows --------------------------------------------------------------------------------------------------------------------------
  {
    Check(cr::kRows * cr::kRowBytes <= cr::kBufferBytes && cr::kRowBytes == 16 && cr::kRowGainsLast == cr::kRowGains + 3 && cr::kRowMore == cr::kRowGainsLast + 1 && cr::kRows == cr::kRowMore + 1,
          "the rows are 13 of 16 bytes in a buffer of 256");
    uint32_t zero[cr::kRows][cr::kRowWords];
    cr::RowsIn off;
    cr::MakeRows(off, zero);
    bool all_zero = true;
    for (auto& row : zero) {
      for (uint32_t w : row) {
        all_zero = all_zero && w == 0;
      }
    }
    Check(all_zero, "an off frame's rows are all zero: the shader sees flags 0");
    cr::RowsIn in;
    in.level = cm::Level::kUltra;
    in.cell = cr::CellFor(cm::Level::kUltra, GpuClass::kHigh);
    in.stats = true;
    in.serial = 77;
    in.screen = true;
    in.depth_slot = 1234;
    in.depth_width = 1920;
    in.depth_height = 1080;
    in.colour_levels = 5;
    in.colour_width = 1920;
    in.colour_height = 1080;
    for (uint32_t i = 0; i < 5; ++i) {
      in.colour_slots[i] = 2000 + i;
    }
    in.wet = 0.25f;
    in.view = 2;
    for (uint32_t c = 0; c < 16; ++c) {
      in.gains[c] = 0.25f + float(c) * 0.25f;  // 0.25 .. 4: clamped to 0.5 .. 3
    }
    uint32_t rows[cr::kRows][cr::kRowWords];
    cr::MakeRows(in, rows);
    Check(rows[cr::kRowControl][0] == (cr::kFlagOn | cr::kFlagScreen | cr::kFlagStats | cr::kFlagCoat) && rows[cr::kRowControl][1] == 2 && rows[cr::kRowControl][2] == 77 &&
              rows[cr::kRowControl][3] == (cr::kPSunRay | cr::kPEmissive | cr::kPScreen | cr::kPCalibrate),
          "control: the flags, the level, the serial and the library's flags (sun ray, glow, the picture, the calibration)");
    Check(rows[cr::kRowDepth][0] == 1234 && rows[cr::kRowDepth][1] == 1920 && rows[cr::kRowDepth][2] == 1080 && rows[cr::kRowDepth][3] == 5 && rows[cr::kRowColour][0] == 1920 && rows[cr::kRowColour][1] == 1080,
          "the depth's slot and size, the colour's size and levels");
    Check(rows[cr::kRowSlotsA][0] == 2000 && rows[cr::kRowSlotsA][3] == 2003 && rows[cr::kRowSlotsB][0] == 2004 && rows[cr::kRowSlotsB][1] == 0xFFFFFFFFu && rows[cr::kRowSlotsB][3] == 0xFFFFFFFFu,
          "the colour's slots, a slot past the chain is none");
    Check(cr::FloatOf(rows[cr::kRowReach][0]) == 1000.0f && cr::FloatOf(rows[cr::kRowReach][1]) == 0.3f && cr::FloatOf(rows[cr::kRowReach][2]) == cr::kSunTan, "the reach, where its fade starts, the sun's disc");
    Check(cr::FloatOf(rows[cr::kRowScreen][0]) == cr::kScreenToleranceM && cr::FloatOf(rows[cr::kRowScreen][1]) == cr::kScreenToleranceRel && cr::FloatOf(rows[cr::kRowScreen][2]) == cr::kScreenEdgeBand &&
              cr::FloatOf(rows[cr::kRowScreen][3]) == cr::kScreenMinFacing,
          "the picture's tolerances");
    Check(cr::FloatOf(rows[cr::kRowAlbedo][0]) == cr::kAoMix && cr::FloatOf(rows[cr::kRowAlbedo][2]) == 0.25f && rows[cr::kRowAlbedo][3] == 2, "the occlusion's share, the wetness, the view");
    bool gains_ok = true;
    for (uint32_t c = 0; c < 16; ++c) {
      const float g = cr::FloatOf(rows[cr::kRowGains + c / 4][c % 4]);
      gains_ok = gains_ok && g == (c == cr::kClassEmissive ? std::clamp(in.gains[c], cr::kEmissiveMinGain, cr::kEmissiveMaxGain) : std::clamp(in.gains[c], cr::kMinGain, cr::kMaxGain));
    }
    Check(gains_ok, "the 16 class gains in rows 8 to 11, clamped to 0.5 .. 3 (the glowing class: 0.25 .. 64)");
    // CITY-M2-OPEN: the rows' range of each class is the calibration's own (so a gain the calibration moved is not cut by the rows)
    {
      namespace cal = mcla::gpu::rt_hit_cal;
      const auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); };
      Check(near(cr::kMinGain, std::exp2(cal::kCityParams.min_stops)) && near(cr::kMaxGain, std::exp2(cal::kCityParams.max_stops)) && near(cr::kEmissiveMinGain, std::exp2(cal::kEmissiveParams.min_stops)) &&
                near(cr::kEmissiveMaxGain, std::exp2(cal::kEmissiveParams.max_stops)) && cr::kClassEmissive == cal::kEmissiveClass,
            "the rows' gain ranges are the calibration's: x0.5 to x3, the glowing class x0.25 to x64");
      cal::ClassGains g;
      for (uint32_t c = 0; c < cal::kClasses; ++c) {
        g.stops[c] = 99.0f;  // far past every range
      }
      bool ranges_ok = true;
      for (uint32_t c = 0; c < cal::kClasses; ++c) {
        ranges_ok = ranges_ok && near(cr::RowGain(c, g.Gain(c)), g.Gain(c));  // the calibration's own value is never cut by the row
      }
      for (uint32_t c = 0; c < cal::kClasses; ++c) {
        g.stops[c] = -99.0f;
        ranges_ok = ranges_ok && near(cr::RowGain(c, g.Gain(c)), g.Gain(c));
      }
      Check(ranges_ok && cr::RowGain(cr::kClassEmissive, 20.0f) == 20.0f && cr::RowGain(1, 20.0f) == 3.0f && cr::RowGain(cr::kClassEmissive, 0.1f) == 0.25f && cr::RowGain(1, 0.1f) == 0.5f && cr::RowGain(7, 0.0f) == 1.0f &&
                cr::RowGain(cr::kClassEmissive, 0.0f) == 1.0f,
            "RowGain: a class keeps what the calibration gives inside its own range, an unset gain is 1");
    }
    // no picture: the screen flag and the picture's rows stay empty; a gain of 0 (unset) is 1
    in.screen = false;
    in.gains[5] = 0.0f;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & cr::kFlagScreen) == 0 && (rows[cr::kRowControl][3] & cr::kPScreen) == 0 && rows[cr::kRowDepth][0] == 0 && rows[cr::kRowSlotsA][0] == 0 &&
              cr::FloatOf(rows[cr::kRowGains + 1][1]) == 1.0f,
          "without the picture the rows have none of it, an unset gain is 1");
    // a picture that is half there (no colour levels, a zero size) is none
    in.screen = true;
    in.colour_levels = 0;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & cr::kFlagScreen) == 0, "a picture with no colour levels is none");
    in.colour_levels = 3;
    in.depth_slot = 0xFFFFFFFFu;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & cr::kFlagScreen) == 0, "a picture with no depth slot is none");
    in.depth_slot = 9;
    in.stats = false;
    in.cell.coat_layer = false;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & (cr::kFlagStats | cr::kFlagCoat)) == 0 && (rows[cr::kRowControl][3] & cr::kPCalibrate) == 0, "no stats and no coat flag when the frame has none");
    Check(cr::ParamFlags(false, false) == (cr::kPSunRay | cr::kPEmissive) && cr::kPCalibrate == 32 && cr::kPScreen == 64 && cr::kPSunRay == 1 && cr::kPEmissive == 16, "the library's flags (hlsl CITY_HIT_P_*)");
    // REFL-LAMPS: the lamp flag: the host's switch in the control flags and the library's lamp flag in the params, only together, never in an off frame; the flags of old are unchanged without it
    Check(cr::kFlagLamps == 16 && cr::kFlagLamps != cr::kFlagOn && cr::kFlagLamps != cr::kFlagScreen && cr::kFlagLamps != cr::kFlagStats && cr::kFlagLamps != cr::kFlagCoat, "the lamp flag is a control flag of its own (16)");
    Check(cr::ParamFlags(false, false, true) == (cr::kPSunRay | cr::kPEmissive | cr::kPLamps) && cr::kPLamps == 4 && cr::ParamFlags(true, true, true) == (cr::kPSunRay | cr::kPEmissive | cr::kPLamps | cr::kPScreen | cr::kPCalibrate) &&
              cr::ParamFlags(true, false) == (cr::kPSunRay | cr::kPEmissive | cr::kPScreen) && cr::ParamFlags(false, true) == (cr::kPSunRay | cr::kPEmissive | cr::kPCalibrate) && (cr::ParamFlags(true, true, true) & cr::kPLampShadow) == 0,
          "the params' flags with the lamp term: the lamp flag only on request, no lamp shadow ray (the unshadowed sum)");
    in.lamps = true;
    in.stats = true;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & cr::kFlagLamps) != 0 && (rows[cr::kRowControl][3] & cr::kPLamps) != 0 && (rows[cr::kRowControl][3] & cr::kPLampShadow) == 0 &&
              rows[cr::kRowControl][3] == cr::ParamFlags((rows[cr::kRowControl][0] & cr::kFlagScreen) != 0, true, true),
          "the rows carry the lamp flag and the library's lamp flag together");
    in.lamps = false;
    cr::MakeRows(in, rows);
    Check((rows[cr::kRowControl][0] & cr::kFlagLamps) == 0 && (rows[cr::kRowControl][3] & cr::kPLamps) == 0, "no lamp flag in either word when the setting is off (the rows of old)");
    // CITY-M2-OPEN: the world light's flag is a control flag of its own (32), set only on request, never in an off frame, and touches no other word
    {
      in.lamps = false;
      in.world_light = false;
      cr::MakeRows(in, rows);
      uint32_t without[cr::kRows][cr::kRowWords];
      std::memcpy(without, rows, sizeof(without));
      in.world_light = true;
      cr::MakeRows(in, rows);
      bool only_the_flag = (rows[cr::kRowControl][0] & cr::kFlagWorldLight) != 0 && (without[cr::kRowControl][0] & cr::kFlagWorldLight) == 0 && (rows[cr::kRowControl][0] & ~cr::kFlagWorldLight) == without[cr::kRowControl][0];
      for (uint32_t r = 0; r < cr::kRows; ++r) {
        for (uint32_t w = 0; w < cr::kRowWords; ++w) {
          only_the_flag = only_the_flag && (r == cr::kRowControl && w == 0 ? true : rows[r][w] == without[r][w]);
        }
      }
      Check(cr::kFlagWorldLight == 64 && cr::kFlagWorldLight != cr::kFlagBlueNoise && cr::kFlagWorldLight != cr::kFlagOn && cr::kFlagWorldLight != cr::kFlagScreen && cr::kFlagWorldLight != cr::kFlagStats && cr::kFlagWorldLight != cr::kFlagCoat && cr::kFlagWorldLight != cr::kFlagLamps &&
                only_the_flag,
            "the world light flag is a control flag of its own (64, not the blue noise flag's 32) and changes nothing else in the rows");
      const cr::Level kept_level = in.level;
      in.level = cr::Level::kOff;
      cr::MakeRows(in, rows);
      bool off_zero = true;
      for (uint32_t r = 0; r < cr::kRows; ++r) {
        for (uint32_t w = 0; w < cr::kRowWords; ++w) {
          off_zero = off_zero && rows[r][w] == 0;
        }
      }
      Check(off_zero, "an off frame stays all zero with the world light asked for");
      in.level = kept_level;
      in.world_light = false;
      in.lamps = true;
    }
    // CITY-M2-OPEN: whose light a city hit is shaded with (WorldLightFor, the twin of the shader's frame choice): the world's only with the switch, valid world rows and a light in the world's frame
    {
      const uint32_t valid = cr::kWorldValidFlag, shade = cr::kFrameShadeFlag | cr::kFrameSunFlag;
      Check(cr::WorldLightFor(true, valid, shade) && cr::WorldLightFor(true, valid | 4u | 8u, shade | 2u | 4u | 16u), "the world's light is taken with the switch on, valid world rows and a light and a sun direction in the world's frame");
      Check(!cr::WorldLightFor(false, valid, shade) && !cr::WorldLightFor(true, 0u, shade) && !cr::WorldLightFor(true, valid, 0u) && !cr::WorldLightFor(true, 4u, shade) && !cr::WorldLightFor(true, valid, 2u | 4u) &&
                !cr::WorldLightFor(true, valid, cr::kFrameShadeFlag) && !cr::WorldLightFor(true, valid, cr::kFrameSunFlag),
            "the cars' light is kept (the pass as it was) with the switch off, world rows not valid, no light or no sun direction in the world's frame");
      Check(cr::kFrameShadeFlag == 1 && cr::kFrameSunFlag == 8 && cr::kWorldValidFlag == 1, "the flags are the world frame's and the hit frame's (RT_WORLD_VALID, RT_HIT_FRAME_SHADE, RT_HIT_FRAME_SUN)");
    }
    in.lamps = true;
    in.level = cr::Level::kOff;
    cr::MakeRows(in, rows);
    bool lamps_all_zero = true;
    for (uint32_t r = 0; r < cr::kRows; ++r) {
      for (uint32_t w = 0; w < cr::kRowWords; ++w) {
        lamps_all_zero = lamps_all_zero && rows[r][w] == 0;
      }
    }
    Check(lamps_all_zero, "an off frame is all zero even with the lamp switch on");
  }

  // ---- the lamp term's decision and glow (REFL-LAMPS) -------------------------------------------------------------------------------------------------
  {
    const float kSun = 0.37f, kWorld = 0.82f;
    // the switch on and the rows valid: the world's lamp factor, the lamp flag kept, the glow by the world's schedule
    const cr::LampTerm on = cr::LampTermFor(true, cr::kWorldValidFlag | 2u | 32u, kWorld, kSun);
    Check(on.world && on.lamp_factor == kWorld && cr::LampFlags(cr::ParamFlags(false, false, true), on) == cr::ParamFlags(false, false, true), "switch on and rows valid: the world's lamp factor and the lamp flag stay");
    // the switch off, or the rows not valid: the sun's schedule, the lamp flag dropped
    const cr::LampTerm off = cr::LampTermFor(false, cr::kWorldValidFlag, kWorld, kSun);
    const cr::LampTerm blind = cr::LampTermFor(true, 0u, kWorld, kSun);
    const cr::LampTerm both = cr::LampTermFor(false, 0u, kWorld, kSun);
    Check(!off.world && off.lamp_factor == kSun && !blind.world && blind.lamp_factor == kSun && !both.world && both.lamp_factor == kSun, "switch off or rows not valid: the sun's schedule");
    for (const cr::LampTerm& t : {off, blind, both}) {
      Check(cr::LampFlags(cr::ParamFlags(true, true, true), t) == cr::ParamFlags(true, true, false), "no lamp flag where the world is not used");
    }
    // the world frame's flag is the valid bit only: the other flags of the status word decide nothing
    Check(!cr::LampTermFor(true, 2u | 4u | 8u | 16u | 32u | 64u | 128u | 256u, kWorld, kSun).world && cr::LampTermFor(true, 1u, kWorld, kSun).world && cr::kWorldValidFlag == 1,
          "the status row's valid bit alone says the rows are this frame's");
    // the glow: a signal always, a lens by the street lamps', everything else by the fill lights'; without the world the sun's schedule, a signal always
    for (const float street : {0.0f, 0.4f, 1.0f}) {
      for (const float fill : {0.0f, 0.7f, 1.0f}) {
        Check(cr::GlowFactorFor(on, cr::kGlowAlways, street, fill) == 1.0f && cr::GlowFactorFor(on, cr::kGlowLens, street, fill) == street && cr::GlowFactorFor(on, cr::kGlowNight, street, fill) == fill &&
                  cr::GlowFactorFor(on, 3, street, fill) == fill,
              "with the world frame: a signal always, a lens the street lamps' schedule, the rest the fill lights'");
        Check(cr::GlowFactorFor(off, cr::kGlowAlways, street, fill) == 1.0f && cr::GlowFactorFor(off, cr::kGlowLens, street, fill) == kSun && cr::GlowFactorFor(off, cr::kGlowNight, street, fill) == kSun &&
                  cr::GlowFactorFor(blind, 3, street, fill) == kSun,
              "without it: a signal always, everything else the sun's schedule (the library's own default)");
      }
    }
    Check(cr::GlowFactorFor(cr::LampTermFor(false, 0u, 1.7f, 1.7f), cr::kGlowLens, 0.0f, 0.0f) == 1.0f && cr::GlowFactorFor(cr::LampTermFor(true, 1u, 0.5f, 0.0f), cr::kGlowLens, 1.7f, 0.0f) == 1.0f &&
              cr::GlowFactorFor(cr::LampTermFor(true, 1u, 0.5f, 0.0f), cr::kGlowNight, 0.0f, -0.5f) == 0.0f,
          "the glow factor is held in 0..1");
    // by day (both schedules 0) the lamp term does nothing and nothing glows but a signal, with or without the world
    const cr::LampTerm day = cr::LampTermFor(true, 1u, 0.0f, 0.0f);
    Check(day.lamp_factor == 0.0f && cr::GlowFactorFor(day, cr::kGlowLens, 0.0f, 0.0f) == 0.0f && cr::GlowFactorFor(day, cr::kGlowAlways, 0.0f, 0.0f) == 1.0f, "by day nothing glows but a signal");
    // the schedule of old is the headlight rig's night curve (kept as the fallback)
    Check(cr::LampSchedule(-0.3f, true) == 1.0f && cr::LampSchedule(0.9f, true) == 0.0f && cr::LampSchedule(0.9f, false) == 0.0f, "the sun's schedule: full at night, none by day, none without a sun direction");
  }

  // ---- the counters and the census lines --------------------------------------------------------------------------------------------------------------
  {
    Check(cr::kCalWords == 48 && cr::kStatTexels == 48 && cr::kStatNoData == 56 && cr::kStatsBytes == cr::kStatsWords * 4 && cr::kStatsWords >= cr::kStatNoData + 1, "48 calibration words, then the census's");
    Check(cr::kClassEmissive == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kEmissive), "the emissive class number is the G-buffer's");
    Check(cr::kGainClassSolid == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kWorld) && cr::kGainClassGround == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kRoad) &&
              cr::kGainClassGlass == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kGlass) && cr::kGainClassEmissive == uint32_t(mcla::gpu::gbuffer::SurfaceClass::kEmissive),
          "the gain line's classes are the G-buffer's world, road, glass and emissive");
    uint32_t words[cr::kStatsWords] = {};
    words[cr::kStatTexels] = 1000;
    words[cr::kStatRays] = 4000;
    words[cr::kStatCity] = 2500;
    words[cr::kStatCar] = 300;
    words[cr::kStatMiss] = 1200;
    words[cr::kStatScreen] = 1500;
    words[cr::kStatShaded] = 900;
    words[cr::kStatLamps] = 40;
    words[mcla::gpu::rt_hit_cal::StatsWord(1, mcla::gpu::rt_hit_cal::kCountWord)] = 120;
    words[mcla::gpu::rt_hit_cal::StatsWord(2, mcla::gpu::rt_hit_cal::kCountWord)] = 30;
    cr::Totals t;
    t.Add(words);
    t.Add(words);
    Check(t.frames == 2 && t.texels == 2000 && t.rays == 8000 && t.city == 5000 && t.car == 600 && t.miss == 2400 && t.screen == 3000 && t.shaded == 1800 && t.lamps == 80 && t.samples == 300, "the window sums the stats frames");
    Check(t.city + t.car + t.miss == t.rays, "city + car + miss = rays (the README's identity)");
    // REFL-LAMPS: the lamp term's eight counters follow the census, inside the buffer, and sum over a window
    Check(cr::kStatLampCensus == 64 && cr::kStatLampCensus == cr::kStatFlat + 1 && cr::kLampCensusWords == 8 && cr::kStatsWords == cr::kStatLampCensus + cr::kLampCensusWords && cr::kStatsWords == 72 &&
              cr::kStatsBytes == 288 && cr::kStatsBytes % 16 == 0,
          "the lamp term's counters are the eight words after the census and the light's counters: 64 to 71, the buffer 288 bytes (a multiple of 16)");
    {
      uint32_t lamp_words[cr::kStatsWords] = {};
      const uint32_t per_frame[cr::kLampCensusWords] = {100, 200, 300, 4, 5, 600, 700, 800};  // lookups, examined, lit, count cap, light cap, glow night, lens, always
      for (uint32_t i = 0; i < cr::kLampCensusWords; ++i) {
        lamp_words[cr::kStatLampCensus + i] = per_frame[i];
      }
      cr::Totals lt;
      lt.Add(lamp_words);
      lt.Add(lamp_words);
      bool sums = lt.frames == 2;
      for (uint32_t i = 0; i < cr::kLampCensusWords; ++i) {
        sums = sums && lt.lamp_words[i] == 2u * per_frame[i];
      }
      Check(sums && lt.texels == 0 && lt.samples == 0, "the window sums the lamp term's words and nothing else");
      // the line: its own prefix (the report's census and gain patterns do not take it), the per-lookup means, the caps in percent, the glow's hits by kind a stats frame
      const std::string line = cr::LampCensusText(1200, lt);
      const std::regex census_re("\\[city refl\\] frame (\\d+): (.*)$"), gain_re("\\[city refl\\] gain frame (\\d+): (.*)$");
      std::smatch m;
      Check(Has(line, "[city refl] lamp term frame 1200: lookups 100, lamps examined 2.0, lit 3.0 a lookup, stopped at the lamp cap 4.0%, over the light cap 5.0%, glow: night 600, lens 700, always 800") &&
                !std::regex_search(line, m, census_re) && !std::regex_search(line, m, gain_re),
            "the lamp term's line: " + line);
      cr::Totals empty;
      Check(Has(cr::LampCensusText(0, empty), "lookups 0, lamps examined 0.0, lit 0.0 a lookup"), "an empty window prints zeros (no division by zero)");
    }
    // CITY-M2-OPEN: the shaded hits' light: five counters after the census, summed over a window, one line of their own (a prefix the report's census and gain patterns do not take)
    {
      uint32_t light_words[cr::kStatsWords] = {};
      light_words[cr::kStatSunLit] = 600;
      light_words[cr::kStatSunBlocked] = 300;
      light_words[cr::kStatSunAway] = 100;
      light_words[cr::kStatBehind] = 250;
      light_words[cr::kStatWorldFrame] = 1000;
      light_words[cr::kStatLodHit] = 400;
      light_words[cr::kStatFlat] = 500;
      light_words[cr::kStatScreen] = 200;
      light_words[cr::kStatShaded] = 1000;
      cr::Totals lt;
      lt.Add(light_words);
      lt.Add(light_words);
      Check(lt.sun_lit == 1200 && lt.sun_blocked == 600 && lt.sun_away == 200 && lt.behind == 500 && lt.world_frame == 2000 && lt.lod_hit == 800 && lt.flat == 1000 && lt.texels == 0 && lt.samples == 0,
            "the window sums the light's words and the detail's");
      const std::string line = cr::LightCensusText(1200, lt);
      const std::regex census_re("\\[city refl\\] frame (\\d+): (.*)$"), gain_re("\\[city refl\\] gain frame (\\d+): (.*)$");
      std::smatch m;
      Check(Has(line, "[city refl] light frame 1200: shaded hits 1000, sun lit 60.0%, sun blocked 30.0%, facing away 10.0%, behind the camera 25.0%, world light 100.0%, far LOD 33.3% of the hits, flat colour 50.0%") && !std::regex_search(line, m, census_re) &&
                !std::regex_search(line, m, gain_re),
            "the light line: " + line);
      cr::Totals empty;
      Check(Has(cr::LightCensusText(0, empty), "shaded hits 0, sun lit 0.0%, sun blocked 0.0%, facing away 0.0%, behind the camera 0.0%, world light 0.0%, far LOD 0.0% of the hits, flat colour 0.0%"), "an empty window prints zeros (no division by zero)");
      Check(cr::kStatSunLit == cr::kCalWords + 9 && cr::kStatWorldFrame == cr::kCalWords + 13 && cr::kStatFlat == cr::kCalWords + 15 && cr::kStatSunLit > cr::kStatNoData && cr::kStatWorldFrame < cr::kStatLampCensus, "the light's counters sit between the census and the lamp term's");
    }
    // the lines, through the report's own regular expressions (perf/scripts/city_rt_m2_report.py CENSUS_RE, GAIN_RE, PAIR_RE)
    const std::string census = cr::CensusText(1200, cm::Level::kHigh, 150.0f, t);
    const std::regex census_re("\\[city refl\\] frame (\\d+): (.*)$");
    const std::regex pair_re("^([a-z][a-z ]*?) ([-+]?\\d+(?:\\.\\d+)?)(?: ?(?:m|%|MiB|px))?$");
    std::smatch m;
    Check(std::regex_search(census, m, census_re) && m[1].str() == "1200", "the census line starts as the report's regex wants: " + census);
    std::set<std::string> names;
    std::string level;
    double rays = 0, city = 0, car = 0, miss = 0;
    if (std::regex_search(census, m, census_re)) {
      std::stringstream body(m[2].str());
      std::string part;
      while (std::getline(body, part, ',')) {
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        std::smatch lm;
        if (std::regex_match(part, lm, std::regex("^level (\\w+)$"))) {
          level = lm[1].str();
          continue;
        }
        std::smatch pm;
        if (std::regex_match(part, pm, pair_re)) {
          names.insert(pm[1].str());
          const double v = std::stod(pm[2].str());
          if (pm[1].str() == "rays") rays = v;
          if (pm[1].str() == "city") city = v;
          if (pm[1].str() == "car") car = v;
          if (pm[1].str() == "miss") miss = v;
        } else {
          Check(false, "a part of the census line the report's pair regex does not parse: '" + part + "'");
        }
      }
    }
    const std::set<std::string> want = {"reach", "texels", "rays", "city", "car", "miss", "screen", "shaded", "lamps"};
    Check(level == "high" && names == want, "the census line has level and the nine names of the README (reach texels rays city car miss screen shaded lamps)");
    Check(rays == 4000 && city == 2500 && car == 300 && miss == 1200, "the numbers are the mean of a stats frame of the window");
    const std::string census_ultra = cr::CensusText(3, cm::Level::kUltra, 1000.0f, t);
    Check(Has(census_ultra, "level ultra, reach 1000 m, texels 1000,"), "an ultra line: " + census_ultra);
    mcla::gpu::rt_hit_cal::ClassGains g;
    g.stops[1] = 0.263034f;   // 1.2
    g.stops[2] = -0.07f;
    const std::string gain = cr::GainText(1200, g, 5120);
    const std::regex gain_re("\\[city refl\\] gain frame (\\d+): (.*)$");
    Check(std::regex_search(gain, m, gain_re) && m[1].str() == "1200", "the gain line starts as the report's regex wants: " + gain);
    int pairs = 0;
    if (std::regex_search(gain, m, gain_re)) {
      std::stringstream body(m[2].str());
      std::string part;
      while (std::getline(body, part, ',')) {
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        std::smatch pm;
        Check(std::regex_match(part, pm, pair_re), "a part of the gain line the report's pair regex parses: '" + part + "'");
        ++pairs;
      }
    }
    Check(pairs == 5 && Has(gain, "solid 1.20,") && Has(gain, "ground 0.95,") && Has(gain, "glass 1.00,") && Has(gain, "emissive 1.00,") && Has(gain, "samples 5120"), "the gain line: solid, ground, glass, emissive, samples");
    Check(cr::kLogEveryFrames == 120, "the census line is every 120 frames (the README)");
  }

  // ---- the files hold the header's numbers -----------------------------------------------------------------------------------------------------------
  {
    const std::string hlsli = ReadFile(src + "/gpu/hlsl/city_refl.hlsli");
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string opp = ReadFile(src + "/gpu/hlsl/car_rt_refl_opp.hlsli");
    const std::string host = ReadFile(src + "/gpu/city_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/city_refl.h");
    const std::string player = ReadFile(src + "/gpu/car_rt_refl.cpp");
    const std::string player_h = ReadFile(src + "/gpu/car_rt_refl.h");
    const std::string opp_host = ReadFile(src + "/gpu/car_rt_opp_refl.cpp");
    const std::string opp_host_h = ReadFile(src + "/gpu/car_rt_opp_refl.h");
    const std::string selfrefl = ReadFile(src + "/gpu/car_selfrefl.cpp");
    const std::string renderer = ReadFile(src + "/gpu/renderer.cpp");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    const std::string screen_rules = ReadFile(src + "/gpu/screen_rules.h");
    const std::string tier = ReadFile(src + "/gpu/rt_tier.h");
    if (hlsli.empty() || refl.empty() || host.empty() || player.empty()) {
      std::printf("(no source folder given or found: the file checks are skipped; pass the src folder)\n");
    } else {
      // the rows and flags
      const std::pair<const char*, uint32_t> rows[] = {{"kCityRowControl", cr::kRowControl}, {"kCityRowDepth", cr::kRowDepth},   {"kCityRowColour", cr::kRowColour}, {"kCityRowSlotsA", cr::kRowSlotsA},
                                                        {"kCityRowSlotsB", cr::kRowSlotsB},   {"kCityRowReach", cr::kRowReach}, {"kCityRowScreen", cr::kRowScreen}, {"kCityRowAlbedo", cr::kRowAlbedo},
                                                        {"kCityRowGains", cr::kRowGains},     {"kCityFlagOn", cr::kFlagOn},     {"kCityFlagScreen", cr::kFlagScreen}, {"kCityFlagStats", cr::kFlagStats},
                                                        {"kCityFlagCoat", cr::kFlagCoat},     {"kCityStatTexels", cr::kStatTexels}, {"kCityStatRays", cr::kStatRays}, {"kCityStatCity", cr::kStatCity},
                                                        {"kCityStatCar", cr::kStatCar},       {"kCityStatMiss", cr::kStatMiss}, {"kCityStatScreen", cr::kStatScreen}, {"kCityStatShaded", cr::kStatShaded},
                                                        {"kCityStatLamps", cr::kStatLamps},   {"kCityStatNoData", cr::kStatNoData}, {"kCityClassEmissive", cr::kClassEmissive},
                                                        {"kCityFlagLamps", cr::kFlagLamps},   {"kCityStatLampCensus", cr::kStatLampCensus},
                                                        {"kCityFlagWorldLight", cr::kFlagWorldLight}, {"kCityStatSunLit", cr::kStatSunLit}, {"kCityStatSunBlocked", cr::kStatSunBlocked}, {"kCityStatSunAway", cr::kStatSunAway},
                                                        {"kCityStatBehind", cr::kStatBehind}, {"kCityStatWorldFrame", cr::kStatWorldFrame}, {"kCityStatLodHit", cr::kStatLodHit}, {"kCityStatFlat", cr::kStatFlat}};
      for (const auto& r : rows) {
        Check(HlslNumber(hlsli, r.first) == double(r.second), std::string("hlsl/city_refl.hlsli: ") + r.first + " is " + std::to_string(r.second));
      }
      Check(HlslNumber(hlsli, "kCityNightFull") == double(cr::kNightFullDegrees) && HlslNumber(hlsli, "kCityNightNone") == double(cr::kNightNoneDegrees), "hlsl/city_refl.hlsli: the lamp schedule's two ends");
      Check(Has(hlsli, "#define CAR_RT_MASK_CITY 0x80"), "hlsl/city_refl.hlsli: the city's mask bit");
      Check(Has(hlsli, "ByteAddressBuffer g_city : register(t0, space3);") && Has(hlsli, "RWByteAddressBuffer g_cityStats : register(u1, space3);"), "hlsl/city_refl.hlsli: the set 3 registers t0 and u1");
      Check(Has(hlsli, "#define CITY_HIT_SPACE CAR_RT_SPACE") && Has(hlsli, "#include \"city_hit_shade.hlsli\""), "hlsl/city_refl.hlsli: the library is included with the car set's space");
      Check(Has(hlsli, "min(cls, 15u) * 4u") && Has(hlsli, "(kCityRowGains * 16u)"), "hlsl/city_refl.hlsli: the gains are read from rows 8 to 11");
      Check(Has(hlsli, "max(c.reach - lo, 1e-6)") && Has(hlsli, "return 1.0 - t * t * (3.0 - 2.0 * t);"), "hlsl/city_refl.hlsli: the fade is the smoothstep of the rules");
      Check(Has(hlsli, "2.0 * h.t * coneTan + PixelMeters(h.t, g_push.lens.y, g_push.image.y)"), "hlsl/city_refl.hlsli: the hit's footprint is HitFootprint");
      Check(Has(hlsli, "(size + (1u << level) - 1u) >> level"), "hlsl/city_refl.hlsli: a mip's size is LevelSize");
      Check(Has(hlsli, "r.screen >= 0.5 ? kCityStatScreen : kCityStatShaded"), "hlsl/city_refl.hlsli: a hit counts as the picture's at a weight of a half");
      Check(Has(hlsli, "r.cls == kCityClassEmissive"), "hlsl/city_refl.hlsli: lamps are the emissive class");
      // REFL-LAMPS: the world frame's lamp term in CityReflHit, and what leaves it out
      {
        const size_t world_at = hlsli.find("#define RT_WORLD_SPACE CAR_RT_SPACE");
        const size_t include_at = hlsli.find("#include \"rt_world_frame.hlsli\"");
        const size_t lib_at = hlsli.find("#include \"city_hit_shade.hlsli\"");
        const size_t grid_at = hlsli.find("#define CITY_HIT_LAMP_GRID");
        Check(world_at != std::string::npos && include_at != std::string::npos && grid_at != std::string::npos && lib_at != std::string::npos && world_at < include_at && include_at < lib_at && grid_at < lib_at,
              "hlsl/city_refl.hlsli: the world frame (in the car set's space) is included before the library, and the lamp grid is the library's lamp loop");
        Check(Has(hlsli, "#define CITY_HIT_GLOW_FACTOR(kind) CityReflGlowFactor(kind)") && Has(hlsli, "#define CITY_HIT_CENSUS_ADD(word, value) CityReflLampCensus(word, value)") &&
                  Has(hlsli, "CityStat(kCityStatLampCensus + word, value);"),
              "hlsl/city_refl.hlsli: the glow factor and the lamp term's counters are the library's hooks");
        Check(Has(hlsli, "if (kind == RT_WORLD_GLOW_ALWAYS)") && Has(hlsli, "return gCityGlowWorld ? RtWorldGlowFactor(kind) : gCityGlowOld;"), "hlsl/city_refl.hlsli: a signal always, else the world rows' glow or the sun's schedule (GlowFactorFor)");
        Check(Has(hlsli, "if ((c.flags & kCityFlagLamps) != 0u)") && Has(hlsli, "const uint4 status = gRtWorld[RT_WORLD_ROW_STATUS];") && Has(hlsli, "lampsWorld = (status.y & RT_WORLD_VALID) != 0u;") &&
                  Has(hlsli, "lampFactor = asfloat(status.w);") && Has(hlsli, "lampFactor = CityReflLampSchedule(f);") && Has(hlsli, "p.flags = p.flags & ~CITY_HIT_P_LAMPS;") && Has(hlsli, "p.lampFactor = lampFactor;") &&
                  Has(hlsli, "gCityGlowWorld = lampsWorld;"),
              "hlsl/city_refl.hlsli: the world's lamp factor and grid where the switch is on and the rows valid (one load of the status row), else the sun's schedule and no lamp flag (LampTermFor, LampFlags)");
        // CITY-M2-OPEN: the frame choice (WorldLightFor) and the light's counters; the pass's own frame rows (the cars') are the fallback
        Check(Has(hlsli, "RtHitFrame f = RtHitLoadFrame();") && Has(hlsli, "(c.flags & kCityFlagWorldLight) != 0u && (gRtWorld[RT_WORLD_ROW_STATUS].y & RT_WORLD_VALID) != 0u") && Has(hlsli, "const RtHitFrame w = RtWorldHitFrame();") &&
                  Has(hlsli, "if ((w.flags & (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)) == (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN))") && Has(hlsli, "f = w;") && Has(hlsli, "worldFrame = true;"),
              "hlsl/city_refl.hlsli: the world's hit frame where the switch is on, the world rows are valid and the frame holds a light and a sun direction (one load of the status row), else the cars' frame");
        Check(Has(hlsli, "CityStat(r.ndl > 0.0 ? (r.visible > 0.0 ? kCityStatSunLit : kCityStatSunBlocked) : kCityStatSunAway, 1u);") && Has(hlsli, "CityStat(kCityStatBehind, (origin.z + direction.z * h.t) < 0.0 ? 1u : 0u);") &&
                  Has(hlsli, "CityStat(kCityStatWorldFrame, worldFrame ? 1u : 0u);") && Has(hlsli, "if (r.source != CITY_HIT_SOURCE_SCREEN)"),
              "hlsl/city_refl.hlsli: a shaded hit counts in exactly one of lit, blocked, away, and in behind and world frame by its own test");
        Check(Has(hlsli, "CityStat(kCityStatLodHit, lodHit ? 1u : 0u);") && Has(hlsli, "const bool lodHit = (h.inst.instanceId & 1u) != 0u;") && Has(hlsli, "CityStat(kCityStatFlat, textured ? 0u : 1u);") &&
                  Has(hlsli, "hitTexture.slot != CITY_HIT_NO_SLOT && hitTexture.mipCount != 0u && (decoded.runFlags & CITY_HIT_RUN_HAS_UV) != 0u"),
              "hlsl/city_refl.hlsli: the far LOD's hits (the instance's detail bit) and the flat albedos (CityHitAlbedo's own test of a texture) are counted");
        const std::string world_hlsli = ReadFile(src + "/gpu/hlsl/rt_world_frame.hlsli");
        Check(Has(world_hlsli, "#define RT_WORLD_ROW_STATUS 32u") && Has(world_hlsli, "#define RT_WORLD_VALID 1u") && cr::kWorldValidFlag == 1 && Has(world_hlsli, "#define RT_WORLD_GLOW_ALWAYS 2u") &&
                  Has(world_hlsli, "#define RT_WORLD_GLOW_LENS 1u") && Has(world_hlsli, "#define RT_WORLD_GLOW_NIGHT 0u") && cr::kGlowNight == 0 && cr::kGlowLens == 1 && cr::kGlowAlways == 2 &&
                  Has(world_hlsli, "StructuredBuffer<uint4> gRtWorld : register(t10, RT_WORLD_SPACE_NAME);") && Has(world_hlsli, "StructuredBuffer<uint4> gRtLamps : register(t11, RT_WORLD_SPACE_NAME);"),
              "hlsl/rt_world_frame.hlsli: the status row, the valid bit, the glow kinds and t10 and t11 are the numbers of the header");
        // the no-lamps compile is the code of before: the whole lamp part is inside the guards, nothing outside them reads the world frame
        Check(Has(hlsli, "#ifdef CITY_REFL_NO_LAMPS\n    p.lampFactor = CityReflLampSchedule(f);\n#else") && CountOf(hlsli, "gRtWorld[") == 2 && CountOf(hlsli, "RtWorldGlowFactor(") == 1 && CountOf(hlsli, "#ifndef CITY_REFL_NO_LAMPS") == 3 && CountOf(hlsli, "#ifdef CITY_REFL_NO_LAMPS") == 2,
              "hlsl/city_refl.hlsli: with CITY_REFL_NO_LAMPS the file is the one of before (the lamp part is inside the guards)");
        // the paint, the opponents' variant and the glass pass share the shader file and the car set (space CAR_RT_SPACE 2, where t10 and t11 are): nothing to add to their layouts
        Check(Has(refl, "#define CAR_RT_SPACE 2") && Has(refl, "#include \"city_refl.hlsli\"") && Has(refl, "#include \"car_rt_refl_glass.hlsli\"") && Has(refl, "#include \"car_rt_refl_opp.hlsli\""),
              "hlsl/car_rt_refl_cs.hlsl: the player's pass, the opponents' variant and the glass variant are one file with the car set in space 2");
      }
      // the trace
      Check(Has(refl, "register(b0, space4)") && !Has(refl, "register(b0, space3)"), "hlsl/car_rt_refl_cs.hlsl: the push block is in space 4");
      Check(Has(refl, "#include \"city_refl.hlsli\"") && Has(refl, "uint traceMask = g_push.slots.y;") && Has(refl, "CityHitTrace(origin, direction, 0.0, reach, traceMask, footprint0, coneTan, 0.5)") &&
                Has(refl, "CityReflHit(origin, direction, h, coneTan, cityColour, cityFade)"),
            "hlsl/car_rt_refl_cs.hlsl: a reflection ray with the city's bit is traced through the library and a city hit is resolved");
      Check(Has(refl, "traceMask = CAR_RT_MASK_CITY;") && Has(refl, "if (1.0 - smoothstep(carFrom * carReach, carReach, h.t) < 1.0 / 512.0)"),
            "hlsl/car_rt_refl_cs.hlsl: a car's part past its own reach does not hide the city behind it (the ray is traced again for the city alone)");
      Check(Has(refl, "visible = 1.0;") && Has(refl, "car = (CityReflFrameNow().flags & kCityFlagCoat) != 0u;"), "hlsl/car_rt_refl_cs.hlsl: a city hit is seen in full and takes the mirror's rule by the host's flag");
      Check(Has(refl, "CityReflOn(g_push.slots.y)") && Has(refl, "gCityTexel = texel;"), "hlsl/car_rt_refl_cs.hlsl: the gate is the push's parts and the rows' flag, the noise's texel is set");
      Check(Has(opp, "gCityTexel = texel;") && Has(opp, "CityStat(kCityStatTexels, 1u);"), "hlsl/car_rt_refl_opp.hlsli: the opponents' texels are counted and carry their noise's texel");
      // the host
      Check(Has(player, "builder.addDescriptorSet(city_.Builder())") && Has(player, "city_set != kCitySet") && Has(player, "list->setComputeDescriptorSet(city.set, kCitySet);"), "car_rt_refl.cpp: set 3 is the city's");
      Check(Has(player_h, "kCitySet = CityRefl::kSetIndex") && Has(player_h, "kPushSpace = CityRefl::kPushSpace") && Has(host_h, "static constexpr uint32_t kPushSpace = 4;") && Has(host_h, "static constexpr uint32_t kSetIndex = 3;"), "car_rt_refl.h: the push block's space is the city's 4, its set 3");
      Check(Has(player, "city_refl::ReflectionParts(parts,") && Has(player, "city_refl::CastLength(reflection_parts, car_length, city.cell.reach)") && Has(player, "std::min(level.cone_rays, city.cell.cone_rays)"),
            "car_rt_refl.cpp: the parts, the cast length and the cone's rays follow the cell");
      Check(Has(player, "push.slots[1] = reflection_parts;") && Has(player, "city_.Prepare(frames_->slot(), info.screen, info.frame, stats_frame)"), "car_rt_refl.cpp: the push's parts and the rows' frame slot");
      Check(Has(player, "city_.BeginFrame(true, one_ray_)") && Has(player, "city_.BeginFrame(false)") && Has(player, "city_.StatsBegin(list)") && Has(player, "city_.StatsEnd(list, info.frame)"), "car_rt_refl.cpp: the city's frame and its counters' frames");
      Check(Has(opp_host, "builder.addDescriptorSet(city_builder)") && Has(opp_host, "list->setComputeDescriptorSet(f.city->set, kCitySet);") && Has(opp_host, "city_refl::OppBudget(") && Has(opp_host, "city_refl::OppRays(plan.rays,") &&
                Has(opp_host_h, "kPushSpace = CityRefl::kPushSpace"),
            "car_rt_opp_refl: the city's set, the cell's budget and rays, the push space");
      Check(Has(host, "set_builder_.addByteAddressBuffer(0);") && Has(host, "set_builder_.addReadWriteByteAddressBuffer(1);") && Has(host, "CreateDescriptorSet(set_builder_, 2, 0, \"city refl set\")"), "city_refl.cpp: the set's two views, t0 and u1");
      // REFL-LAMPS: the host's switch and the census line; no set grew (the counters are words of u1's buffer, the world frame is in the car scene's set)
      Check(Has(host, "in.lamps = on && larecomp::settings::mcla_gpu_city_refl_lamps.Get();") && Has(host, "cr::LampCensusText(frame, window_)") && Has(host, "window_.lamp_words[0] != 0") &&
                Has(host, "DefaultBuffer(cr::kStatsBytes,") && Has(host, "s.set->setBuffer(1, stats_.get(), cr::kStatsBytes);"),
            "city_refl.cpp: the lamp switch is the setting's, the lamp term's line is logged, the counters stay in u1's buffer (no view added)");
      Check(Has(host_h, "mcla_gpu_city_refl_lamps") && Has(host_h, "t10 and t11 are in the car scene's 12 view set"), "city_refl.h names the setting and says where t10 and t11 are");
      // CITY-M2-OPEN: the world light's switch and the light line
      Check(Has(host, "in.world_light = on && larecomp::settings::mcla_gpu_city_refl_world_light.Get();") && Has(host, "cr::LightCensusText(frame, window_)"), "city_refl.cpp: the world light's switch is the setting's and the light line is logged");
      Check(Has(host_h, "mcla_gpu_city_refl_world_light"), "city_refl.h names the world light's setting");
      Check(Has(host, "rt_scene::Consumer::kCityReflections") && Has(host, "cr::WantsFor(cell)") && Has(host, "screen::ProductBit(screen::kColourPost) | screen::ProductBit(screen::kLinDepth)"), "city_refl.cpp: the wants");
      Check(Has(host, "gpu_class::CurrentRtClass()") && Has(host, "gpu_class::CurrentRtTier()") && Has(host, "LiveText text(\"mcla_gpu_rt_reflections\")"), "city_refl.cpp: the setting is read live and the cell is the RT class's");
      Check(Has(host, "constexpr uint32_t kCloseEveryStats = 4;") && Has(host, "cr::kLogEveryFrames"), "city_refl.cpp: the calibration's window and the census's period");
      Check(Has(selfrefl, "screen_->Want(screen::Consumer::kCarRtRefl, rt_->ScreenProducts());") && Has(selfrefl, "FillCityScreen(&info.screen);"), "car_selfrefl.cpp: the screen wants and the picture's products");
      Check(Has(renderer, "car_rt_refl_.SetSceneService(&rt_scene_);"), "renderer.cpp: the pass has the scene service");
      Check(Has(cmake, "city_refl.cpp") && Has(cmake, "larecomp_city_refl_rules_test"), "CMakeLists.txt: the source and the test");
      Check(Has(screen_rules, "wants.Of(Consumer::kCarRtRefl) & (1u << kColourPost)"), "screen_rules.h: the car ray traced reflections' want makes colour_post's mips");
      Check(Has(tier, "{\"mcla_gpu_rt_reflections\", \"off\", \"off\", \"off\"},") && Has(ReadFile(src + "/gpu/gfx_preset.h"), "{\"mcla_gpu_rt_reflections\", Page::kReflections, {\"off\", \"off\", \"off\", \"off\", \"high\"}},"),
            "rt_tier.h: legacy Auto stays off and the RT Ultra preset selects High");
      // the settings: in the header, the manifest and the meta, with the aliases (the city's own switch is not debug-only since R8, which gave it the row RT CITY REFLECTIONS: city_refl_setting_test.cpp)
      const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
      const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
      const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
      for (const char* name : {"mcla_gpu_rt_reflections", "mcla_gpu_city_refl_screen", "mcla_gpu_city_refl_coat", "mcla_gpu_city_refl_reach", "mcla_gpu_city_refl_view", "mcla_gpu_city_refl_lamps", "mcla_gpu_city_refl_world_light"}) {
        const std::string n = name;
        Check(Has(settings, "REX_SETTING(") && Has(settings, ", " + n + ", ") && Has(settings, "inline auto& " + n + " = g_gpu." + n + ";"), "settings_gpu.h defines and aliases " + n);
        Check(Has(manifest, "\"" + n + "\": {") && Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},"), "the manifest and the meta list " + n);
      }
      {
        // REFL-LAMPS: the lamp term's switch is a debug-only bool, true by default (no player-visible off switch: the owner's rule)
        const size_t lamps_at = settings.find("bool, mcla_gpu_city_refl_lamps, true, ");
        const size_t lamps_next = lamps_at == std::string::npos ? std::string::npos : settings.find("\n  REX_SET" "TING(", lamps_at);
        const std::string lamps_def = lamps_at == std::string::npos ? "" : settings.substr(lamps_at, lamps_next == std::string::npos ? std::string::npos : lamps_next - lamps_at);
        Check(!lamps_def.empty() && lamps_def.find("rex::kDebugOnly") != std::string::npos, "mcla_gpu_city_refl_lamps is a debug-only bool, true by default");
        const size_t key = manifest.find("\"mcla_gpu_city_refl_lamps\": {");
        const std::string entry = key == std::string::npos ? "" : manifest.substr(key, manifest.find("\n },", key) - key);
        Check(!entry.empty() && Has(entry, "\"type\": \"bool\"") && Has(entry, "\"default\": \"true\"") && Has(entry, "rex::kDebugOnly"), "the manifest has mcla_gpu_city_refl_lamps as a debug-only bool, true");
      }
      {
        // CITY-M2-OPEN: the world light's switch is a debug-only bool, true by default (the fix is on; an A/B switch on the hidden debug menu only)
        const size_t world_at = settings.find("bool, mcla_gpu_city_refl_world_light, true, ");
        const size_t world_next = world_at == std::string::npos ? std::string::npos : settings.find("\n  REX_SET" "TING(", world_at);
        const std::string world_def = world_at == std::string::npos ? "" : settings.substr(world_at, world_next == std::string::npos ? std::string::npos : world_next - world_at);
        Check(!world_def.empty() && world_def.find("rex::kDebugOnly") != std::string::npos, "mcla_gpu_city_refl_world_light is a debug-only bool, true by default");
        const size_t world_key = manifest.find("\"mcla_gpu_city_refl_world_light\": {");
        const std::string world_entry = world_key == std::string::npos ? "" : manifest.substr(world_key, manifest.find("\n },", world_key) - world_key);
        Check(!world_entry.empty() && Has(world_entry, "\"type\": \"bool\"") && Has(world_entry, "\"default\": \"true\"") && Has(world_entry, "rex::kDebugOnly"), "the manifest has mcla_gpu_city_refl_world_light as a debug-only bool, true");
      }
      const size_t at = settings.find("std::string, mcla_gpu_rt_reflections, \"off\", ");
      const size_t next = at == std::string::npos ? std::string::npos : settings.find("\n  REX_SET" "TING(", at);
      const std::string def = at == std::string::npos ? "" : settings.substr(at, next == std::string::npos ? std::string::npos : next - at);
      Check(!def.empty() && def.find("rex::Allowed(\"auto\", \"off\", \"on\", \"high\", \"ultra\")") != std::string::npos && def.find("kDebugOnly") == std::string::npos,
            "mcla_gpu_rt_reflections is On plus compatible legacy words, and not debug-only (R8 gave it its row)");
    }
  }

  {
    const auto cell = cr::PaintCell(cm::Level::kHigh, 150.0f);
    Check(cell == cr::PaintCell(cm::Level::kUltra, 150.0f) && cell.level == cm::Level::kHigh &&
          cell.reach == 150.0f && cell.cone_rays == 1u && cell.opp_rays == 1u,
          "EV-9 maps legacy city levels to the same 150 m one-ray signal");
    Check(cr::PaintCell(cm::Level::kOff, 150.0f).level == cm::Level::kOff, "EV-9 city Off requests no work");
  }

  std::printf("city_refl_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
