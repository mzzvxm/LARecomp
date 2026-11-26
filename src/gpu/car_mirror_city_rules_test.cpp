// Check of the city in the player's side mirrors (car_mirror_city_rules.h, MIRROR-CITY): the world setting's words, what the mirror's world comes to by class, tier and the data's state (the city, or why the
// environment map), the wants the scene service gets, the ray's parts and length, the push flags, the weight of a city hit by its distance, the mirror's view as the camera reflected in the glass (the mirror
// matrix, the primary ray's direction, the lifted start, the line from the virtual camera through the glass point), the pixel's two layers over the environment (the city with no data is a miss, never black; a car
// that weighs nothing; a car in its fade band; a frame that knows no world), and that hlsl/car_mirror_rt_cs.hlsl, the host (car_mirror_rt.cpp / .h), the scene service's consumer, the settings, the renderer
// and the CMake file hold the header's numbers. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_mirror_city_rules_test, then run src/gpu/larecomp_car_mirror_city_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_mirror_city_rules.h"

namespace cmc = mcla::gpu::car_mirror_city;
namespace cm = mcla::gpu::car_mirror;
namespace cr = mcla::gpu::city_refl;
namespace crs = mcla::gpu::city_refl_setting;
namespace gc = mcla::gpu::gpu_class;
namespace rs = mcla::gpu::rt_scene;
namespace tp = mcla::gpu::temporal;
using Level = mcla::gpu::city_rt::Level;
using gc::GpuClass;
using gc::RtTier;

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

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
int Count(const std::string& text, const std::string& part) {
  int n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
    ++n;
  }
  return n;
}

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Length(const double a[3]) { return std::sqrt(Dot(a, a)); }
void Normalize(double v[3]) {
  const double l = Length(v);
  for (int c = 0; c < 3; ++c) {
    v[c] /= l;
  }
}

constexpr GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
constexpr RtTier kTiers[] = {RtTier::kNone, RtTier::kWeak, RtTier::kStrong};
constexpr crs::Data kDatas[] = {crs::Data::kUnknown, crs::Data::kReady, crs::Data::kBuilding, crs::Data::kUnavailable};

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    src = __FILE__;
    for (int up = 0; up < 2; ++up) {
      const size_t cut = src.find_last_of("/\\");
      if (cut != std::string::npos) {
        src = src.substr(0, cut);
      }
    }
  }
  const std::string gpu = src + "/gpu";

  // =========================================================================
  // the setting's words
  // =========================================================================
  {
    Check(cmc::WorldOf("auto") == cmc::World::kAuto && cmc::WorldOf("env") == cmc::World::kEnv && cmc::WorldOf("city") == cmc::World::kCity, "the three words");
    Check(cmc::WorldOf("") == cmc::World::kAuto && cmc::WorldOf("Env") == cmc::World::kAuto && cmc::WorldOf("on") == cmc::World::kAuto && cmc::WorldOf("off") == cmc::World::kAuto,
          "a word that is none of the three is auto (an older larecomp.toml)");
    Check(std::string(cmc::WorldName(cmc::World::kAuto)) == "auto" && std::string(cmc::WorldName(cmc::World::kEnv)) == "env" && std::string(cmc::WorldName(cmc::World::kCity)) == "city", "the names");
    // the words the reflections' level rule is given: env off, city high (never ultra), auto auto
    for (const char* paint : {"auto", "off", "high", "ultra", "", "nonsense"}) {
      Check(std::string(cmc::LevelWord(cmc::World::kEnv, paint)) == "off" && std::string(cmc::LevelWord(cmc::World::kCity, paint)) == "high", "env is off and city is high whatever the paint's row says");
    }
    Check(std::string(cmc::LevelWord(cmc::World::kAuto, "auto")) == "auto" && std::string(cmc::LevelWord(cmc::World::kAuto, "off")) == "off" && std::string(cmc::LevelWord(cmc::World::kAuto, "high")) == "high" &&
              std::string(cmc::LevelWord(cmc::World::kAuto, "ultra")) == "high" && std::string(cmc::LevelWord(cmc::World::kAuto, "")) == "auto" && std::string(cmc::LevelWord(cmc::World::kAuto, "nonsense")) == "auto",
          "auto follows the paint's row: off is off, high and ultra are high (the mirror's cell is never ultra's), anything else is the reflections' own auto");
    for (const GpuClass cls : kClasses) {
      for (const RtTier tier : kTiers) {
        Check(cr::LevelFor(cls, tier, cmc::LevelWord(cmc::World::kEnv, "auto")) == Level::kOff, "env is off on every card");
        const Level city = cr::LevelFor(cls, tier, cmc::LevelWord(cmc::World::kCity, "auto"));
        Check(city == Level::kOff || city == Level::kHigh, "city is high or off, never ultra");
        Check((city == Level::kOff) == (gc::RtRung(cls, tier) == RtTier::kNone), "city is on wherever the card traces (the rung is not none), off where it does not");
      }
    }
  }

  // =========================================================================
  // what the mirror's world comes to (the decision)
  // =========================================================================
  {
    // the card's facts are the process's: class and tier as gpu_class::Current(), CurrentRtTier(); the budget class is RtClass of the two
    for (const GpuClass cls : kClasses) {
      for (const RtTier tier : kTiers) {
        const GpuClass rt_class = gc::RtClass(cls, tier);
        const gc::RtTier rung = gc::RtRung(cls, tier);
        for (const crs::Data data : kDatas) {
          // env: never the city, whatever the card and the data
          const cmc::Decision env = cmc::Decide(cmc::World::kEnv, "auto", cls, tier, rt_class, data);
          Check(env.wanted == Level::kOff && env.level == Level::kOff && !env.city() && env.why == cmc::Why::kSetting, "env: the environment map on every card, for the setting");
          Check(!cmc::NeedsFor(env).any(), "env asks the scene service for nothing of the city");
          // city: wherever the card traces
          const cmc::Decision city = cmc::Decide(cmc::World::kCity, "off", cls, tier, rt_class, data);  // the paint's row off: the debug word still wins
          if (rung == RtTier::kNone || rt_class == GpuClass::kLow) {
            Check(city.wanted == Level::kOff && !city.city() && city.why == cmc::Why::kCard, "city on a card that does not trace (or the Low class): the environment, for the card");
            Check(!cmc::NeedsFor(city).any(), "... and no wants");
          } else {
            Check(city.wanted == Level::kHigh, "city on a card that traces: wanted at HIGH");
            Check(city.cell == cr::CellFor(Level::kHigh, rt_class), "... with the budget class's cell");
            Check(city.city() == (data == crs::Data::kReady), "... it runs once the data stands and not before");
            Check(city.why == (data == crs::Data::kReady ? cmc::Why::kCity : cmc::Why::kData), "... and says why not");
            const rs::Needs needs = cmc::NeedsFor(city);
            Check(needs.cars && needs.city == Level::kHigh && needs.hit == Level::kHigh && needs.textures == mcla::gpu::TextureScope::kAll,
                  "the wants go out whether or not the data stands (that is what makes the app build it): the cars, the structures and the hit data at HIGH, every texture");
          }
          // auto: the reflections' own Auto cell at the rung
          const cmc::Decision autod = cmc::Decide(cmc::World::kAuto, "auto", cls, tier, rt_class, data);
          const bool auto_on = rung == RtTier::kStrong;
          if (rt_class == GpuClass::kLow || rung == RtTier::kNone) {
            Check(autod.wanted == Level::kOff && autod.why == cmc::Why::kCard, "auto on a card that does not trace: the environment, for the card");
          } else if (!auto_on) {
            Check(autod.wanted == Level::kOff && autod.why == cmc::Why::kAutoOff && !autod.city(), "auto on a weak ray tracing card: the environment (the city is a strong card's look)");
            Check(!cmc::NeedsFor(autod).any(), "... and it asks for nothing: a weak card is not made to build the city by an effect that costs it");
          } else {
            Check(autod.wanted == Level::kHigh && autod.cell.reach > 0.0f, "auto on a strong card: the city at HIGH");
            Check(autod.city() == (data == crs::Data::kReady), "... once the data stands");
          }
          // the decision is the paint's own: the same level for the same word (the budget class Low is the one place the mirror says off for a card the paint's rule lets through)
          if (rt_class != GpuClass::kLow) {
            Check(autod.wanted == cr::LevelFor(cls, tier, "auto") && city.wanted == cr::LevelFor(cls, tier, "high"), "auto and city are the level the player's paint comes to for those words on this card");
          }
          // auto follows the paint's row: OFF is off, HIGH and ULTRA by hand are the mirror's HIGH cell on any card that traces (the Mid class's 60 m on a weak one), AUTO is the reflections' own auto
          for (const char* paint : {"off", "high", "ultra", "auto"}) {
            const cmc::Decision follows = cmc::Decide(cmc::World::kAuto, paint, cls, tier, rt_class, data);
            const std::string word = paint;
            if (rung == RtTier::kNone || rt_class == GpuClass::kLow) {
              Check(follows.wanted == Level::kOff && follows.why == cmc::Why::kCard, "auto on a card that does not trace: the environment, whatever the paint's row says");
            } else if (word == "off") {
              Check(follows.wanted == Level::kOff && follows.why == cmc::Why::kPaintOff && !follows.city() && !cmc::NeedsFor(follows).any(), "the player turned RT CITY REFLECTIONS off: the mirror follows, and asks for nothing");
            } else if (word == "auto") {
              Check(follows.wanted == autod.wanted && follows.cell == autod.cell && follows.why == autod.why, "the paint's auto is the mirror's auto");
            } else {
              Check(follows.wanted == Level::kHigh && follows.cell == cr::CellFor(Level::kHigh, rt_class), "HIGH or ULTRA by hand: the mirror's HIGH cell, never ultra's (a weak card: the Mid class's)");
              Check(follows.city() == (data == crs::Data::kReady), "... once the data stands");
            }
          }
        }
      }
    }
    // the cells the classes run: a strong High card 150 m with the weight full to 60 percent; a Mid class by hand 60 m
    const cmc::Decision high = cmc::Decide(cmc::World::kAuto, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kReady);
    Check(high.city() && Near(high.cell.reach, 150.0) && Near(high.cell.fade_from, 0.6, 1e-6), "a strong High card: 150 m, the weight full to 60 percent");
    const cmc::Decision weak_high = cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kWeak, gc::RtClass(GpuClass::kHigh, RtTier::kWeak), crs::Data::kReady);
    Check(weak_high.city() && Near(weak_high.cell.reach, 60.0), "a weak card of the High class by hand: the Mid class's 60 m");
    const cmc::Decision mid = cmc::Decide(cmc::World::kCity, "auto", GpuClass::kMid, RtTier::kStrong, GpuClass::kMid, crs::Data::kReady);
    Check(mid.city() && Near(mid.cell.reach, 60.0), "a Mid class card by hand: 60 m");
    // the paint's row ULTRA on a strong card: the mirror keeps HIGH's reach
    const cmc::Decision ultra_paint = cmc::Decide(cmc::World::kAuto, "ultra", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kReady);
    Check(ultra_paint.city() && Near(ultra_paint.cell.reach, 150.0), "the paint at ULTRA (1,000 m): the mirror stays at HIGH's 150 m");
    // a budget class of Low with a word that wants the city (forced by hand on a card that does not trace): nothing
    Check(!cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kLow, crs::Data::kReady).city(), "a Low budget class: nothing to trace the city with");
    Check(cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kLow, crs::Data::kReady).why == cmc::Why::kCard, "... and the reason is the card");
    // the data's states one at a time on a strong card
    for (const crs::Data data : kDatas) {
      const cmc::Decision d = cmc::Decide(cmc::World::kAuto, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, data);
      Check(d.wanted == Level::kHigh && d.data == data, "the data's state is carried");
      Check(d.city() == (data == crs::Data::kReady), "the city runs only with the data ready");
      Check(cmc::NeedsFor(d).city == Level::kHigh, "the wants are out in every state of the data");
    }
    // the merge with the reflections' wants: HIGH with ULTRA is ULTRA, both are one structure
    rs::Needs ultra;
    ultra.cars = true;
    ultra.city = Level::kUltra;
    ultra.hit = Level::kUltra;
    ultra.textures = mcla::gpu::TextureScope::kAll;
    const rs::Needs merged = rs::Merge(cmc::NeedsFor(high), ultra);
    Check(merged.city == Level::kUltra && merged.hit == Level::kUltra && merged.cars, "the service's merge of the mirror's HIGH with the paint's ULTRA is ULTRA (one structure, one pool)");
    const rs::Needs only = rs::Merge(cmc::NeedsFor(high), rs::Needs{});
    Check(only.city == Level::kHigh && only.hit == Level::kHigh, "the mirror alone brings the structures and the hit data at HIGH");
    // the mirror is a consumer of its own: its want does not replace the paint's (the board keeps one want a consumer)
    rs::WantBoard board;
    board.Want(rs::Consumer::kCityReflections, ultra);
    board.Want(rs::Consumer::kCityMirror, cmc::NeedsFor(high));
    Check(board.Now().city == Level::kUltra && board.Wants(rs::Consumer::kCityMirror) && board.Wants(rs::Consumer::kCityReflections), "a consumer slot of its own: both wants stand");
    Check(std::string(rs::ConsumerName(rs::Consumer::kCityMirror)) == "city mirror", "the consumer's name");
    Check(rs::kConsumerCount == 10 && rs::Consumer::kCityMirror == static_cast<rs::Consumer>(7) && rs::Consumer::kRtAo == static_cast<rs::Consumer>(8), "the side mirror and AO consumer indices are preserved");
  }

  // =========================================================================
  // the ray's parts, length and the push flags
  // =========================================================================
  {
    const cmc::Decision high = cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kReady);
    const cmc::Decision waiting = cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kBuilding);
    const cmc::Decision env = cmc::Decide(cmc::World::kEnv, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kReady);
    Check(cmc::Parts(high) == (cm::kReflectionMask | 0x80u) && cmc::Parts(high) == 0x9F, "the city's bit joins the cars' parts while the city runs");
    Check(cmc::Parts(waiting) == cm::kReflectionMask && cmc::Parts(env) == cm::kReflectionMask && cmc::Parts(env) == 0x1F, "... and is not there while the data is built or the world is the environment (the mirror ray is the old one)");
    Check((cm::kPrimaryMask & cmc::kCityMask) == 0 && (cm::kReflectionMask & cmc::kCityMask) == 0, "the primary ray never finds the city (it finds the glass), the cars' parts leave the bit free");
    const float cars = cm::LimitsFor(GpuClass::kHigh, 0).reach;
    Check(Near(cmc::CastLength(high, cars), std::max(cars, high.cell.reach)) && Near(cmc::CastLength(high, cars), 150.0), "the ray is cast over the longer of the cars' reach and the city's");
    Check(Near(cmc::CastLength(env, cars), cars) && Near(cmc::CastLength(waiting, cars), cars), "without the city it is the cars' reach, as before");
    Check(cmc::FlagsFor(env, true, true, true) == 0 && cmc::FlagsFor(waiting, true, true, true) == 0, "no flags without the city: the shader's city code is not reached");
    Check(cmc::FlagsFor(high, false, true, true) == 0, "no flags for a mirror too small to show the city: the environment, as before");
    Check(cmc::FlagsFor(high, true, true, true) == (cmc::kFlagOn | cmc::kFlagLamps | cmc::kFlagWorldLight), "all three with the reflections' two switches on");
    Check(cmc::FlagsFor(high, true, false, true) == (cmc::kFlagOn | cmc::kFlagWorldLight) && cmc::FlagsFor(high, true, true, false) == (cmc::kFlagOn | cmc::kFlagLamps) && cmc::FlagsFor(high, true, false, false) == cmc::kFlagOn,
          "each switch moves its own bit");
    Check(cmc::kFlagOn == 1 && cmc::kFlagLamps == 2 && cmc::kFlagWorldLight == 4, "the flag bits");
  }

  // =========================================================================
  // the visibility rule: no city for a mirror too small to show it
  // =========================================================================
  {
    Check(!cmc::BigEnough(0) && !cmc::BigEnough(cmc::kCityMinPixels - 1) && cmc::BigEnough(cmc::kCityMinPixels) && cmc::BigEnough(160000), "a rectangle of at least kCityMinPixels is big enough");
    // a chase camera's mirror at 1080p (about 10 x 14 pixels with the margin) is far under it; a cockpit mirror at 4K (8 to 25 k pixels) is far over it
    Check(!cmc::BigEnough(16 * 20) && cmc::BigEnough(8000), "a chase camera's mirror is too small, a cockpit's is not");
    Check(cmc::kCityMinPixels > cm::kMinPixels && cmc::kCityMinPixels < 8000, "the city's threshold lies above the pass's own minimum and below the smallest cockpit mirror");
    // the hold: nothing before the first big pass, kCityHoldFrames after it, nothing after
    Check(!cmc::CityHeld(0, false, 0) && !cmc::CityHeld(1000, false, 900), "no want before a big enough mirror was ever seen (a flag, not a frame number)");
    Check(cmc::CityHeld(100, true, 100) && cmc::CityHeld(100 + cmc::kCityHoldFrames, true, 100) && !cmc::CityHeld(100 + cmc::kCityHoldFrames + 1, true, 100), "kept for kCityHoldFrames frames after the last big pass, not one more");
    Check(!cmc::CityHeld(50, true, 100), "a frame number that went backwards (a new session) holds nothing");
    Check(cmc::kCityHoldFrames == rs::WantBoard::kIdleReleaseFrames, "the hold is the scene service's own idle release: a scene nobody asks for in that long is given back");
  }

  // =========================================================================
  // the weight of a city hit by its distance
  // =========================================================================
  {
    const cmc::Decision high = cmc::Decide(cmc::World::kCity, "auto", GpuClass::kHigh, RtTier::kStrong, GpuClass::kHigh, crs::Data::kReady);
    const float reach = high.cell.reach, from = high.cell.fade_from * reach;
    Check(cmc::CityWeight(high, 0.5f) == 1.0f && cmc::CityWeight(high, from) == 1.0f, "a city hit inside the full weight's reach weighs 1");
    Check(cmc::CityWeight(high, reach) == 0.0f && cmc::CityWeight(high, reach * 2.0f) == 0.0f, "at the reach and past it nothing");
    float last = 1.0f;
    bool monotone = true;
    for (float d = from; d <= reach; d += (reach - from) / 64.0f) {
      const float w = cmc::CityWeight(high, d);
      monotone = monotone && w <= last + 1e-6f;
      last = w;
    }
    Check(monotone, "the weight falls with the distance between the two");
    Check(Near(cmc::CityWeight(high, 0.5f * (from + reach)), 0.5, 1e-6), "the smoothstep's middle is a half");
    // a hit that weighs under 1/512 is none: the shader returns false for it (a miss: the environment stays)
    int under = 0, over = 0;
    bool floor_holds = true;
    for (float d = from; d <= reach; d += (reach - from) / 4000.0f) {
      const float raw = cr::CityFade(d, high.cell.reach, high.cell.fade_from);
      const float w = cmc::CityWeight(high, d);
      if (raw > 0.0f && raw < cmc::kMinFade) {
        ++under;
        floor_holds = floor_holds && w == 0.0f;  // a weight under the floor is none, not a faint city
      } else {
        ++over;
        floor_holds = floor_holds && w == raw;
      }
    }
    Check(under > 5 && over > 100 && floor_holds, "a weight under 1/512 is none, a weight over it is the smoothstep's");
    Check(Near(cmc::kMinFade, 1.0 / 512.0), "the floor is 1/512");
    // the cell with the reach 0 (not asked): no weight, no division by zero
    Check(cmc::CityWeight(cmc::Decision{}, 10.0f) == 0.0f, "an empty decision weighs nothing");
  }

  // =========================================================================
  // the mirror's view: the camera reflected in the glass
  // =========================================================================
  {
    std::mt19937 rng(20261122);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    // the primary ray's direction: view space x right, y up, z forward; the picture's pixel y goes down
    {
      double d[3];
      cmc::PrimaryDirection(959.5, 539.5, 0, 0, 1920, 1080, 0.8, 0.45, d);  // the middle of a 1920x1080 picture
      Check(Near(d[0], 0.0, 1e-9) && Near(d[1], 0.0, 1e-9) && d[2] == 1.0, "the picture's middle looks straight ahead");
      cmc::PrimaryDirection(1919, 539.5, 0, 0, 1920, 1080, 0.8, 0.45, d);
      Check(d[0] > 0.79 && d[0] < 0.8 && Near(d[1], 0.0, 1e-9), "the right edge is +x, nearly tan x");
      cmc::PrimaryDirection(959.5, 0, 0, 0, 1920, 1080, 0.8, 0.45, d);
      Check(d[1] > 0.44 && d[1] < 0.45 && Near(d[0], 0.0, 1e-9), "the top edge is +y (the picture's y goes down, the view's up), nearly tan y");
      cmc::PrimaryDirection(0, 1079, 0, 0, 1920, 1080, 0.8, 0.45, d);
      Check(d[0] < 0 && d[1] < 0, "the bottom left is -x and -y");
      double a[3], b[3];
      cmc::PrimaryDirection(100, 100, 0.375, -0.125, 1920, 1080, 0.8, 0.45, a);
      cmc::PrimaryDirection(100.375, 99.875, 0, 0, 1920, 1080, 0.8, 0.45, b);
      Check(Near(a[0], b[0]) && Near(a[1], b[1]), "a tap's offset is in pixels");
    }
    // the matrix of the glass plane
    for (int round = 0; round < 400; ++round) {
      double n[3] = {uniform(-1, 1), uniform(-1, 1), uniform(-1, 1)};
      Normalize(n);
      const double point[3] = {uniform(-1, 1), uniform(-1, 1), uniform(0.2, 3)};
      const tp::Mat4 m = cmc::MirrorMatrix(point, n);
      Check(Near(cmc::Determinant3(m), -1.0, 1e-9), "a mirror reverses handedness: the 3x3's determinant is -1");
      double on_plane[3];
      for (int c = 0; c < 3; ++c) on_plane[c] = point[c];
      double out[3];
      cmc::Reflected(m, on_plane, out);
      Check(Near(out[0], point[0], 1e-9) && Near(out[1], point[1], 1e-9) && Near(out[2], point[2], 1e-9), "a point on the plane stays");
      const double q[3] = {uniform(-4, 4), uniform(-4, 4), uniform(-8, 8)};
      double once[3], twice[3];
      cmc::Reflected(m, q, once);
      cmc::Reflected(m, once, twice);
      Check(Near(twice[0], q[0], 1e-9) && Near(twice[1], q[1], 1e-9) && Near(twice[2], q[2], 1e-9), "reflected twice is the point again");
      double to_plane[3] = {q[0] - point[0], q[1] - point[1], q[2] - point[2]};
      double to_image[3] = {once[0] - point[0], once[1] - point[1], once[2] - point[2]};
      Check(Near(Dot(to_plane, n), -Dot(to_image, n), 1e-9), "the image is as far behind the plane as the point is in front of it");
    }
    // the ray of a glass pixel is the line from the virtual camera through the glass point
    int checked = 0;
    for (int round = 0; round < 600; ++round) {
      // a glass 0.4 to 3 m from the camera, facing the viewer (the mirror of a cockpit camera: off to the side, tilted)
      double n[3] = {uniform(-0.6, 0.6), uniform(-0.6, 0.6), uniform(-1.0, -0.3)};  // faces back towards the camera (the viewer is on the -z side of a glass ahead of it)
      Normalize(n);
      double primary[3];
      cmc::PrimaryDirection(uniform(0, 1919), uniform(0, 1079), 0, 0, 1920, 1080, 0.8, 0.45, primary);
      double view[3] = {primary[0], primary[1], primary[2]};
      Normalize(view);
      if (Dot(view, n) > -0.1) {
        continue;  // the glass is seen edge on or from behind: not a mirror's case (the shader leaves it: kMirrorMinNv)
      }
      const double t = uniform(0.4, 3.0);
      const cmc::MirrorRay r = cmc::MirrorRayOf(primary, t, n);
      ++checked;
      // the glass point is the primary direction times the view z
      Check(Near(r.glass[0], primary[0] * t) && Near(r.glass[1], primary[1] * t) && Near(r.glass[2], t, 1e-12), "the glass point is the primary direction at RayT = the view z");
      // the direction is the reflection of the view: unit, mirrored about the normal, off the glass on the viewer's side
      Check(Near(Length(r.direction), 1.0, 1e-9), "the mirror ray's direction is unit");
      const double k = 2.0 * Dot(view, n);
      Check(Near(r.direction[0], view[0] - k * n[0], 1e-12) && Near(r.direction[1], view[1] - k * n[1], 1e-12) && Near(r.direction[2], view[2] - k * n[2], 1e-12), "reflect(view, normal)");
      Check(Dot(r.direction, n) > 0.0 && Near(Dot(r.direction, n), -Dot(view, n), 1e-9), "it leaves on the viewer's side at the same angle to the glass");
      // the start is off the glass along the normal by the lift
      double off[3] = {r.origin[0] - r.glass[0], r.origin[1] - r.glass[1], r.origin[2] - r.glass[2]};
      Check(Near(Dot(off, n), double(cm::kLift) + double(cm::kLiftPerMetre) * t, 1e-12) && Near(Length(off), Dot(off, n), 1e-9), "the start is lifted along the normal by kLift and kLiftPerMetre a metre of the view distance");
      // the camera reflected in the glass: from there, through the glass point, goes the same line
      const double zero[3] = {0, 0, 0};
      const tp::Mat4 m = cmc::MirrorMatrix(r.glass, n);
      double virtual_camera[3];
      cmc::Reflected(m, zero, virtual_camera);
      double along[3] = {r.glass[0] - virtual_camera[0], r.glass[1] - virtual_camera[1], r.glass[2] - virtual_camera[2]};
      Normalize(along);
      Check(Near(along[0], r.direction[0], 1e-9) && Near(along[1], r.direction[1], 1e-9) && Near(along[2], r.direction[2], 1e-9), "the ray is the line from the virtual camera through the glass point");
    }
    Check(checked > 300, "enough glass poses were tried");
    // the picture of the world through the glass: a point Q behind the camera appears at the pixel where the camera looks at its image Q' = M Q; the mirror ray of that pixel passes through Q
    int hits = 0;
    for (int round = 0; round < 600; ++round) {
      double n[3] = {uniform(-0.5, 0.5), uniform(-0.5, 0.5), uniform(-1.0, -0.4)};
      Normalize(n);
      const double centre[3] = {uniform(-0.5, 0.5), uniform(-0.3, 0.3), uniform(0.5, 1.5)};
      const tp::Mat4 m = cmc::MirrorMatrix(centre, n);
      const double q[3] = {uniform(-6, 6), uniform(-1, 3), uniform(-30, -3)};  // behind the camera
      double image[3];
      cmc::Reflected(m, q, image);
      if (!(image[2] > 0.05)) {
        continue;  // the image is behind the camera: no pixel sees it
      }
      // the camera's primary ray through the image: direction image / image.z (z = 1); where it meets the glass plane
      double primary[3] = {image[0] / image[2], image[1] / image[2], 1.0};
      const double denom = Dot(primary, n);
      if (std::fabs(denom) < 0.05) {
        continue;
      }
      const double t_glass = Dot(centre, n) / denom;  // the view z of the glass point along the z = 1 direction
      if (!(t_glass > 0.1)) {
        continue;
      }
      double view[3] = {primary[0], primary[1], primary[2]};
      Normalize(view);
      if (Dot(view, n) > -0.05) {
        continue;
      }
      const cmc::MirrorRay r = cmc::MirrorRayOf(primary, t_glass, n);
      // distance of Q from the ray's line
      double to_q[3] = {q[0] - r.origin[0], q[1] - r.origin[1], q[2] - r.origin[2]};
      const double along = Dot(to_q, r.direction);
      double perp[3] = {to_q[0] - along * r.direction[0], to_q[1] - along * r.direction[1], to_q[2] - along * r.direction[2]};
      ++hits;
      Check(along > 0.0 && Length(perp) < 1e-3 + 2.0 * (double(cm::kLift) + double(cm::kLiftPerMetre) * t_glass), "a point behind the camera is where the mirror ray from the pixel that shows it goes (to within the lift)");
    }
    Check(hits > 100, "enough points behind the camera were tried");
  }

  // =========================================================================
  // the pixel: the city, the cars and the environment
  // =========================================================================
  {
    const float env[3] = {0.2f, 0.3f, 0.5f};
    const float cityRgb[3] = {0.9f, 0.4f, 0.1f};
    const float carRgb[3] = {0.1f, 0.8f, 0.2f};
    cmc::CityLayer none_city;
    cmc::CarLayer none_car;
    auto city_layer = [&](float weight) {
      cmc::CityLayer l;
      l.valid = true;
      l.weight = weight;
      for (int c = 0; c < 3; ++c) l.rgb[c] = cityRgb[c];
      return l;
    };
    auto car_layer = [&](bool other, float fade) {
      cmc::CarLayer l;
      l.valid = true;
      l.other = other;
      l.fade = fade;
      for (int c = 0; c < 3; ++c) l.rgb[c] = carRgb[c];
      return l;
    };
    auto same = [](const float a[3], const float b[3]) { return Near(a[0], b[0], 1e-6) && Near(a[1], b[1], 1e-6) && Near(a[2], b[2], 1e-6); };
    auto lerp3 = [](const float a[3], const float b[3], float w, float out[3]) {
      for (int c = 0; c < 3; ++c) out[c] = a[c] + (b[c] - a[c]) * w;
    };
    // nothing found: the environment, where the frame knows one
    {
      const cmc::Pixel p = cmc::Compose(true, env, none_city, none_car);
      Check(p.shown && same(p.rgb, env), "nothing hit: the environment map");
      const cmc::Pixel q = cmc::Compose(false, env, none_city, none_car);
      Check(!q.shown, "nothing hit and no known world: the game's glass stays (not a black hole)");
    }
    // a city hit with data inside the full weight's reach
    {
      const cmc::Pixel p = cmc::Compose(true, env, city_layer(1.0f), none_car);
      Check(p.shown && same(p.rgb, cityRgb), "a city hit of weight 1 is the city's colour");
      const cmc::Pixel unknown = cmc::Compose(false, env, city_layer(1.0f), none_car);
      Check(unknown.shown && same(unknown.rgb, cityRgb), "... and shown even where the frame knows no environment (a shaded hit is real)");
      float want[3];
      lerp3(env, cityRgb, 0.5f, want);
      const cmc::Pixel half = cmc::Compose(true, env, city_layer(0.5f), none_car);
      Check(half.shown && Near(half.rgb[0], want[0], 1e-6) && Near(half.rgb[1], want[1], 1e-6) && Near(half.rgb[2], want[2], 1e-6), "a city hit in the fade band is over the environment by its weight");
      const cmc::Pixel half_unknown = cmc::Compose(false, env, city_layer(0.5f), none_car);
      Check(!half_unknown.shown, "a city hit of half weight over a world nobody knows is not shown: that would be a dark hole");
    }
    // a city hit with no data, or past the reach, is a miss: the environment, never black
    {
      cmc::CityLayer nodata;  // valid false, rgb zero: what MirrorCityHit returns false for
      const cmc::Pixel p = cmc::Compose(true, env, nodata, none_car);
      Check(p.shown && same(p.rgb, env), "a city hit with no data: a miss, the environment stays (not black)");
      const cmc::Pixel q = cmc::Compose(false, env, nodata, none_car);
      Check(!q.shown, "... and with no environment either the game's glass stays");
      cmc::CityLayer past = city_layer(0.0f);  // a layer whose weight is 0 is the environment too
      const cmc::Pixel r = cmc::Compose(true, env, past, none_car);
      Check(same(r.rgb, env), "a city hit of weight 0 is the environment");
    }
    // the player's own car weighs 1 wherever it is
    {
      const cmc::Pixel p = cmc::Compose(true, env, none_city, car_layer(false, 0.0f));
      Check(p.shown && same(p.rgb, carRgb), "a part of the player's own car: its colour, whatever the fade says");
      const cmc::Pixel q = cmc::Compose(false, env, none_city, car_layer(false, 0.0f));
      Check(q.shown && same(q.rgb, carRgb), "... even where the frame knows no world");
    }
    // another car fades into the background
    {
      float want[3];
      lerp3(env, carRgb, 0.4f, want);
      const cmc::Pixel p = cmc::Compose(true, env, none_city, car_layer(true, 0.4f));
      Check(p.shown && Near(p.rgb[0], want[0], 1e-6) && Near(p.rgb[2], want[2], 1e-6), "another car in its fade band is over the environment by its fade");
      const cmc::Pixel q = cmc::Compose(false, env, none_city, car_layer(true, 0.4f));
      Check(q.shown && same(q.rgb, carRgb), "... with no world to fade into it weighs 1 (the rule of old)");
      // the city shows through the fading car (the car's fade band is no hole and does not hide the street behind)
      float behind[3], over[3];
      lerp3(env, cityRgb, 1.0f, behind);
      lerp3(behind, carRgb, 0.4f, over);
      const cmc::Pixel r = cmc::Compose(true, env, city_layer(1.0f), car_layer(true, 0.4f));
      Check(r.shown && Near(r.rgb[0], over[0], 1e-6) && Near(r.rgb[1], over[1], 1e-6) && Near(r.rgb[2], over[2], 1e-6), "a fading car is over the city behind it, not over the environment");
      const cmc::Pixel s = cmc::Compose(false, env, city_layer(1.0f), car_layer(true, 0.4f));
      Check(s.shown && Near(s.rgb[0], over[0], 1e-6), "... a city hit of full weight is a background to fade into even where the environment is unknown");
      const cmc::Pixel t = cmc::Compose(false, env, city_layer(0.5f), car_layer(true, 0.4f));
      Check(t.shown && same(t.rgb, carRgb), "... a city hit of half weight over nothing is not: the car weighs 1");
    }
    // a car with nothing to show (no material) is transparent: the layer is not valid, the city behind it shows
    {
      const cmc::Pixel p = cmc::Compose(true, env, city_layer(1.0f), none_car);
      Check(same(p.rgb, cityRgb), "a car with no material shows the city behind it (the shader traces beyond it)");
    }
    // NaN and infinity are not colours
    {
      cmc::CityLayer bad = city_layer(1.0f);
      bad.rgb[1] = std::nanf("");
      Check(!cmc::Compose(true, env, bad, none_car).shown, "a NaN colour leaves the game's glass");
      cmc::CarLayer inf = car_layer(false, 1.0f);
      inf.rgb[0] = INFINITY;
      Check(!cmc::Compose(true, env, none_city, inf).shown, "an infinity too");
    }
    // the car's weight rule on its own
    Check(cmc::CarWeight(false, true, 0.2f) == 1.0f && cmc::CarWeight(false, false, 0.2f) == 1.0f, "the player's own parts always weigh 1");
    Check(cmc::CarWeight(true, true, 0.2f) == 0.2f && cmc::CarWeight(true, false, 0.2f) == 1.0f && cmc::CarWeight(true, true, 1.5f) == 1.0f && cmc::CarWeight(true, true, -0.5f) == 0.0f, "another car's weight is its fade where there is a background, else 1, and is clamped");
    // over many random layers the result is always a convex mix of the layers' colours
    std::mt19937 rng(7);
    auto u = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    bool convex = true;
    for (int i = 0; i < 2000; ++i) {
      const float e[3] = {u(0, 2), u(0, 2), u(0, 2)};
      cmc::CityLayer cl;
      cl.valid = rng() % 2 == 0;
      cl.weight = u(0, 1);
      cmc::CarLayer kl;
      kl.valid = rng() % 2 == 0;
      kl.other = rng() % 2 == 0;
      kl.fade = u(0, 1);
      for (int c = 0; c < 3; ++c) {
        cl.rgb[c] = u(0, 2);
        kl.rgb[c] = u(0, 2);
      }
      const cmc::Pixel p = cmc::Compose(rng() % 2 == 0, e, cl, kl);
      for (int c = 0; c < 3; ++c) {
        const float lo = std::min({e[c], cl.valid ? cl.rgb[c] : e[c], kl.valid ? kl.rgb[c] : e[c]});
        const float hi = std::max({e[c], cl.valid ? cl.rgb[c] : e[c], kl.valid ? kl.rgb[c] : e[c]});
        convex = convex && p.rgb[c] >= lo - 1e-5f && p.rgb[c] <= hi + 1e-5f;
      }
    }
    Check(convex, "the pixel is always a mix of the environment, the city's colour and the car's: never darker than all of them (no black hit)");
    // the wetness
    Check(cmc::WetFor(1, 0.6f) == 0.6f && cmc::WetFor(0, 0.6f) == 0.0f && cmc::WetFor(1, 4.0f) == 1.0f && cmc::WetFor(1, -1.0f) == 0.0f && cmc::WetFor(1, std::nanf("")) == 0.0f,
          "the wetness is the world rows' where they are valid, clamped, else dry");
  }

  // =========================================================================
  // the shader, the host, the settings and the files hold the header's numbers
  // =========================================================================
  {
    const std::string cs = ReadFile(gpu + "/hlsl/car_mirror_rt_cs.hlsl");
    const std::string host = ReadFile(gpu + "/car_mirror_rt.cpp");
    const std::string header = ReadFile(gpu + "/car_mirror_rt.h");
    const std::string scene_rules = ReadFile(gpu + "/rt_scene_rules.h");
    const std::string renderer = ReadFile(gpu + "/renderer.cpp");
    const std::string cmake = ReadFile(gpu + "/CMakeLists.txt");
    const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
    const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
    const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
    const std::string refl = ReadFile(gpu + "/hlsl/city_refl.hlsli");
    if (cs.empty() || host.empty() || header.empty() || scene_rules.empty() || manifest.empty()) {
      std::printf("note: the source files were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      // the shader's numbers
      Check(Has(cs, "static const uint kMirrorCityOn = 1u;") && Has(cs, "static const uint kMirrorCityLamps = 2u;") && Has(cs, "static const uint kMirrorCityWorldLight = 4u;"), "the shader's flag bits are the header's");
      Check(Has(cs, "static const uint kMirrorCityMask = 0x80u;") && cmc::kCityMask == 0x80u, "the shader's city bit is the header's");
      Check(Has(cs, "static const float kMirrorMinFade = 1.0 / 512.0;") && Near(cmc::kMinFade, 1.0 / 512.0), "the shader's fade floor is the header's");
      Check(Has(cs, "static const float kMirrorSunTan = 0.0;") && cmc::kSunTan == 0.0f, "the sun's ray is sharp in both: no noise anywhere");
      Check(Has(cs, "static const float kMirrorAoMix = 0.5;") && cmc::kAoMix == 0.5f, "the baked occlusion's share is the header's");
      Check(Has(cs, "static const float kMirrorNightFull = 3.0;") && Has(cs, "static const float kMirrorNightNone = 12.0;") && cr::kNightFullDegrees == 3.0f && cr::kNightNoneDegrees == 12.0f, "the fallback lamp schedule is the reflections'");
      // the push block: 80 bytes, the city's words last
      Check(Has(cs, "uint4 city;") && Has(header, "uint32_t city[4];") && Has(header, "static_assert(sizeof(Push) == car_mirror_city::kPushBytes);") && cmc::kPushBytes == 80,
            "the push block's city words, 80 bytes");
      Check(Has(cs, "asfloat(g_push.city.y)") && Has(cs, "asfloat(g_push.city.z)") && Has(host, "std::memcpy(&push.city[1], &world_.cell.reach, sizeof(float));") && Has(host, "std::memcpy(&push.city[2], &world_.cell.fade_from, sizeof(float));"),
            "the city's reach and fade start travel as float bits in y and z");
      Check(Has(host, "push.city[0] = cmc::FlagsFor(world_, big_enough, larecomp::settings::mcla_gpu_city_refl_lamps.Get(), larecomp::settings::mcla_gpu_city_refl_world_light.Get());"), "the host's flags are the header's, with the reflections' two switches");
      Check(Has(host, "const bool big_enough = cmc::BigEnough(largest);") && Has(host, "largest = std::max<uint64_t>(largest, rects[k].rect.Area());") && Has(host, "city_seen_ = true;\n    city_seen_frame_ = frame_;") &&
                Has(host, "cmc::CityHeld(frame_, city_seen_, city_seen_frame_)"),
            "the host asks for the city only while a mirror big enough to show it has been seen lately");
      // the library, on the car scene's set
      Check(Has(cs, "#define RT_WORLD_SPACE CAR_RT_SPACE") && Has(cs, "#include \"rt_world_frame.hlsli\"") && Has(cs, "#define CITY_HIT_SPACE CAR_RT_SPACE") && Has(cs, "#include \"city_hit_shade.hlsli\""),
            "the shader takes the world frame and the city hit library on the car scene's set (no new set)");
      Check(Has(cs, "#define CITY_HIT_LAMP_GRID") && Has(cs, "#define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)") && Has(cs, "#define CITY_HIT_GLOW_FACTOR(kind) MirrorGlowFactor(kind)"), "the library's hooks: the world's lamp grid, class gains and glow schedule");
      Check(!Has(cs, "CITY_HIT_SCREEN_DEPTH") && !Has(cs, "CITY_HIT_SCREEN_COLOUR") && !Has(cs, "CITY_HIT_P_SCREEN") && !Has(cs, "CITY_HIT_P_LAMP_SHADOW") && !Has(cs, "CITY_HIT_STAT_ADD") && !Has(cs, "CITY_HIT_CENSUS_ADD"),
            "no picture, no lamp ray and no counters: the mirror's points are behind the camera and the mirror image is not denoised");
      Check(Has(cs, "p.flags = CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE;") && Has(cs, "p.flags = p.flags | CITY_HIT_P_LAMPS;") && Has(cs, "p.sunTan = kMirrorSunTan;") && Has(cs, "p.units = g_push.tune.w;") &&
                Has(cs, "p.mode = CITY_HIT_MODE_REFLECT;"),
            "the hit's params: a reflection's, the sun's ray sharp, the lamp term only with the world rows");
      Check(Has(cs, "p.noise = float3(0.5, 0.5, 0.5);") && !Has(cs, "BlueNoise") && !Has(cs, "CityReflNoise") && !Has(cs, "InterleavedGradient"), "no noise is read in the mirror shader (nothing to vary, nothing to slide)");
      // the decisions the shader makes
      Check(Has(cs, "const bool lampsWorld = (g_push.city.x & kMirrorCityLamps) != 0u && worldValid;") && Has(cs, "float lampFactor = lampsWorld ? asfloat(status.w) : MirrorLampSchedule(f);") &&
                Has(cs, "gMirrorGlowWorld = lampsWorld;"),
            "the lamp term: the world's where the switch is on and the rows valid (city_refl_rules.h LampTermFor)");
      Check(Has(cs, "(w.flags & (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)) == (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)") && Has(cs, "(g_push.city.x & kMirrorCityWorldLight) != 0u && worldValid") &&
                Has(refl, "(w.flags & (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)) == (RT_HIT_FRAME_SHADE | RT_HIT_FRAME_SUN)"),
            "the light: the world frame's where the switch is on and it holds a shading frame and a sun (city_refl_rules.h WorldLightFor), as the reflections choose it");
      Check(Has(cs, "p.wet = worldValid ? saturate(asfloat(gRtWorld[RT_WORLD_ROW_LIGHT].y)) : 0.0;"), "the wetness is the world's where the rows are valid (WetFor)");
      Check(Has(cs, "if (r.source == CITY_HIT_SOURCE_NONE)") && Has(cs, "return false;  // no data for this hit: a miss, the environment stays"), "a hit with no data is a miss (CITY-RT-REVIEW M5)");
      Check(Has(cs, "if (fade < kMirrorMinFade)") && Has(cs, "const float lo = asfloat(g_push.city.z) * reach;") && Has(cs, "return 1.0 - t * t * (3.0 - 2.0 * t);"), "the city's weight is city_refl_rules.h CityFade, under the floor a miss");
      Check(Has(cs, "g_push.image.w | kMirrorCityMask") && Has(cs, "const float cast = max(reach, asfloat(g_push.city.y));"), "the ray's parts take the city's bit and its length the longer reach (city_refl_rules.h CastLength)");
      Check(Has(cs, "CityHitTrace(from, direction, 0.0, max(reach - travelled, 0.0), mask, width, tanHalf, 0.5)") &&
                Has(cs, "if (!CityHitIsCity(h.inst.instanceId) && (gCarRtInstances[h.instanceIndex].z & CAR_RT_FLAG_SEE_THROUGH) != 0u && step < kMirrorSeeThroughSteps)") &&
                Has(cs, "const float skip = h.t + kMirrorPaneSkip;") && Has(cs, "from = from + direction * skip;") && Has(cs, "travelled += skip;") && Has(cs, "h.t = travelled + h.t;"),
            "the closest hit of the cars and the city, the cut-outs' alpha, going on through the player's own see-through panes (a pane skipped moves the start on and the distance is the sum)");
      Check(Has(cs, "const bool farCar = (CarRtInstanceMask(CarRtLoadInstance(h.instanceIndex)) & CAR_RT_MASK_OTHERS) != 0u && 1.0 - smoothstep(g_push.tune.z * reach, reach, h.t) < kMirrorMinFade;") &&
                Has(cs, "h = MirrorTraceCity(origin, mirror, cast, kMirrorCityMask, footprint0, tanHalf);"),
            "a car's part that weighs nothing is traced past for the city alone (a far car must not hide the street)");
      Check(Has(cs, "if (!carShows || carFade < 1.0)") && Has(cs, "const CityHitTraceResult behind = MirrorTraceCity(origin, mirror, cast, kMirrorCityMask, footprint0, tanHalf);"), "behind a fading car, or a car with nothing to show, the city is traced for alone");
      Check(Has(cs, "background = lerp(world, cityColour, cityFade);") && Has(cs, "backgroundShown = worldKnown || cityFade >= 1.0;") && Has(cs, "const float weight = (other && backgroundShown) ? carFade : 1.0;") &&
                Has(cs, "colour = lerp(background, shaded, weight);") && Has(cs, "shown = backgroundShown || weight >= 1.0;"),
            "the pixel's layers are Compose's: the city over the environment by its weight, a car over that by CarWeight");
      Check(Has(cs, "const float footprint0 = pixelAngle * glassT;") && Has(cs, "const float tanHalf = 0.5 * pixelAngle;") && Has(cs, "max(pixelAngle * (glassT + h.t), 1e-3)"), "the cone's width: a pixel's angle at the glass and along the ray");
      // off equals the old mirror: the old block is the else branch, the city code is behind the flag
      Check(Has(cs, "if ((g_push.city.x & kMirrorCityOn) != 0u)") && Has(cs, "MirrorCityRay(origin, mirror, glassT, reach, world, worldKnown, colour, shown);") && Has(cs, "if (MirrorTrace(origin, mirror, reach, g_push.image.w, r, rflags))") &&
                Has(cs, "const float fade = (other && worldKnown) ? 1.0 - smoothstep(g_push.tune.z * reach, reach, r.t) : 1.0;"),
            "with the flags 0 the pixel is the old one: the same trace, the same fade, the environment as the world");
      Check(Has(cs, "if (!shown || any(isnan(colour)) || any(isinf(colour)))"), "a NaN or an infinity leaves the game's glass");
      Check(Count(cs, "CityHitTrace(") == 1, "one trace function of the city in the file (every cast goes through MirrorTraceCity)");
      Check(Count(cs, "MirrorCarHit(") == 3, "the car's hit is shaded in one function, called by the old branch and the city's two");
      // the primary ray and the glass are the old ones: the primary mask never has the city's bit
      Check(Has(cs, "MirrorTrace(float3(0.0, 0.0, 0.0), direction, kMirrorPrimaryReach, g_push.image.z, h, flags)") && Has(cs, "(1.0 - uv.y * 2.0) * g_push.lens.y"), "the primary ray's mask is the push's (no city) and the pixel's y is flipped to the view's up");
      // the host
      Check(Has(host, "static LiveText text(\"mcla_gpu_car_mirror_rt_world\");") && Has(host, "static LiveText text(\"mcla_gpu_rt_reflections\");") &&
                Has(host, "cmc::Decide(word, PaintWordSetting().Get(), gpu_class::Current(), gpu_class::CurrentRtTier(), gpu_class::CurrentRtClass(), city_refl_setting::DataNow())"),
            "the host decides the world with the reflections' own rule: the player's RT CITY REFLECTIONS word, the card's class and tier, the budget class, the data's state");
      Check(!Has(host, "mcla_gpu_rt_reflections.Get()") && !Has(host, "mcla_gpu_car_mirror_rt_world.Get()"), "the two string cvars are read through a LiveText on the render thread, never with the cvar's own Get() (a data race with the menu's write)");
      Check(Has(host, "world_ = cmc::Decision{};  // the environment map, until the setting and the card say otherwise") && Has(host, "UpdateWorld();"), "an off frame has no city (the decision is reset at the frame's start)");
      Check(Has(host, "service_->Want(rt_scene::Consumer::kCityMirror, cmc::NeedsFor(world_));") && Has(host, "if (world_.wanted != cmc::Level::kOff && service_ != nullptr && cmc::CityHeld(frame_, city_seen_, city_seen_frame_)) {"),
            "the wants go to the scene service from the mirror's own consumer slot, whether or not the data stands, while a big enough mirror was seen lately");
      Check(Has(host, "cm::LimitsFor(gpu_class::CurrentRtClass(), int(") && Has(host, "push.tune[1] = limits.reach;"), "the cars' reach and the pixel cap are the class's, as before");
      Check(Has(host, "window_.city_passes += push.city[0] != 0 ? 1u : 0u;") && Has(header, "uint32_t city_passes = 0;"), "the log counts the passes that traced the city");
      Check(Has(header, "void SetService(RtSceneService* service)") && Has(renderer, "car_mirror_.SetService(&rt_scene_);"), "the renderer hands the mirror the scene service");
      Check(Has(host, "service_ = nullptr;"), "the shutdown lets the service go");
      // the consumer
      Check(Has(scene_rules, "kCityMirror,") && Has(scene_rules, "case Consumer::kCityMirror: return \"city mirror\";"), "the scene service has the consumer");
      // the setting
      Check(Has(manifest, "\"mcla_gpu_car_mirror_rt_world\": {") && Has(settings, "REX_SETTING(std::string, " "mcla_gpu_car_mirror_rt_world, \"auto\",") && Has(settings, "rex::Allowed(\"auto\", \"env\", \"city\"), rex::kDebugOnly);") &&
                Has(settings, "inline auto& mcla_gpu_car_mirror_rt_world = g_gpu.mcla_gpu_car_mirror_rt_world;") && Has(meta, "{\"mcla_gpu_car_mirror_rt_world\", \"MCLA/GPU\"},"),
            "the setting is debug-only, auto env city, in the manifest, the settings header and the meta list");
      Check(Count(settings, "REX_SETTING(std::string, " "mcla_gpu_car_mirror_rt_world,") == 1 && Count(settings, "inline auto& mcla_gpu_car_mirror_rt_world =") == 1, "the setting is defined once and aliased once");
      // the test is built
      Check(Has(cmake, "add_executable(larecomp_car_mirror_city_rules_test EXCLUDE_FROM_ALL car_mirror_city_rules_test.cpp)"), "the CMake file builds this test");
    }
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
