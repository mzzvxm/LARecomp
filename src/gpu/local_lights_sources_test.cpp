// Check of what feeds the deferred local light pass besides the frame's own lights (local_lights_sources.h; perf/LIGHT-UNIFY.md 11.2, package GB-H3): the tier and the budget table, the police
// light bars (the game's own flash, the colours, the kind 8 that is not a siren), the cars' rear lamps (brake and tail as one light, each turn side, reverse), the head lamps of the cars the
// forward stage does not light (the rig's beam, smaller; covered by a frame light or not), the mods' lamps the game's grid is not lighting (the three lamp A/B modes), the selection (the classes'
// caps, the priority, the pass's 32), the picker's score, and the pixel term of such a light (held to the one laws). THE NIGHT PROBE'S RECORDED POINTS (the player, a racer with its lights on and
// off, three traffic cars: the same lines car_lights_test.cpp holds) are the cars. With the `src` folder it also reads the sources: the settings, the manager's cars, the mod loader's publish
// and the pass's wiring. A program of its own: no renderer, no ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_local_lights_sources_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_local_lights_sources_test [<the src folder>]     (found from this file's place without an argument)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "local_lights_sources.h"

namespace {

using namespace larecomp::gpu;
namespace lp = larecomp::gpu::local_lights;
namespace pts = larecomp::gpu::light_points;
namespace cl = larecomp::gpu::car_lights;
namespace mls = larecomp::gpu::mod_light_source;

int g_checks = 0, g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
void CheckNear(double got, double want, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}

// ---------------------------------------------------------------------------
// the probe's recorded points (car_lights_test.cpp's): `who vehicle kind index intensity x y z [dx dy dz | -]`
// ---------------------------------------------------------------------------

const char* const kFrame3603 = R"(
other 0xbf5bf600 0 0 0.000 834.89 18.87 -845.58 0.11 -0.02 0.99
other 0xbf5bf600 0 1 0.000 833.62 18.91 -845.43 0.11 -0.02 0.99
other 0xbf5bf600 3 0 0.000 835.32 18.60 -841.47 -0.11 0.02 -0.99
other 0xbf5bf600 3 1 0.000 834.10 18.64 -841.33 -0.11 0.02 -0.99
other 0xbf5bf600 4 0 0.000 835.17 18.59 -841.36 -0.11 0.02 -0.99
other 0xbf5bf600 4 1 0.000 834.27 18.62 -841.26 -0.11 0.02 -0.99
other 0xbf5bf600 6 0 0.000 834.41 18.56 -845.64 0.11 -0.02 0.99
other 0xbf5bf600 6 1 0.000 834.05 18.57 -845.61 0.11 -0.02 0.99
other 0xbf5bf600 8 0 0.000 835.42 18.61 -841.52 -0.11 0.02 -0.99
other 0xbf5bf600 8 1 0.000 833.99 18.66 -841.36 -0.11 0.02 -0.99
other 0xbf5bf600 12 0 0.000 834.25 19.01 -845.55 0.11 -0.02 0.99
player 0xbf4e7380 0 0 0.075 821.23 18.44 -809.99 -0.47 -0.04 0.88
player 0xbf4e7380 0 1 0.075 820.11 18.50 -810.59 -0.47 -0.04 0.88
player 0xbf4e7380 3 0 2.000 819.23 18.09 -806.40 0.47 0.04 -0.88
player 0xbf4e7380 3 1 2.000 818.19 18.14 -806.96 0.47 0.04 -0.88
player 0xbf4e7380 4 0 0.000 819.08 18.09 -806.40 0.47 0.04 -0.88
player 0xbf4e7380 4 1 0.000 818.27 18.13 -806.82 0.47 0.04 -0.88
player 0xbf4e7380 6 0 0.000 820.88 18.14 -810.34 -0.47 -0.04 0.88
player 0xbf4e7380 6 1 0.000 820.57 18.16 -810.52 -0.47 -0.04 0.88
player 0xbf4e7380 8 0 1.000 819.33 18.09 -806.37 0.47 0.04 -0.88
player 0xbf4e7380 8 1 1.000 818.11 18.15 -807.02 0.47 0.04 -0.88
player 0xbf4e7380 12 0 0.000 820.71 18.59 -810.35 -0.47 -0.04 0.88
)";

const char* const kFrame13623 = R"(
other 0xbf5873b0 0 0 0.500 835.87 17.01 -783.09 -0.88 0.00 -0.47
other 0xbf5873b0 0 1 0.500 836.55 17.01 -784.39 -0.88 0.00 -0.47
other 0xbf5873b0 2 0 1.002 832.55 16.86 -785.03 0.88 -0.00 0.47
other 0xbf5873b0 2 1 1.002 833.09 16.86 -786.05 0.88 -0.00 0.47
other 0xbf5873b0 3 0 2.000 832.46 16.80 -784.94 0.88 -0.00 0.47
other 0xbf5873b0 3 1 2.000 833.11 16.80 -786.17 0.88 -0.00 0.47
other 0xbf5873b0 4 0 0.000 832.42 16.78 -785.08 0.88 -0.00 0.47
other 0xbf5873b0 4 1 0.000 832.98 16.78 -786.13 0.88 -0.00 0.47
other 0xbf5873b0 6 0 0.000 836.00 17.01 -783.23 -0.88 0.00 -0.47
other 0xbf5873b0 6 1 0.000 836.51 17.01 -784.21 -0.88 0.00 -0.47
other 0xbf5873b0 12 0 0.000 835.60 17.19 -784.07 -0.88 0.00 -0.47
)";

const char* const kFrame122 = R"(
none 0 0 0 0.840 -2970.50 -23.96 433.70 -
none 0 0 1 0.840 -2971.49 -23.97 433.81 -
none 0 3 0 3.360 -2969.82 -23.81 438.78 -
none 0 3 1 3.360 -2971.02 -23.80 438.92 -
none 0 4 0 0.000 -2969.93 -23.81 438.81 -
none 0 4 1 0.000 -2970.91 -23.80 438.92 -
none 0 6 0 0.000 -2970.38 -24.02 433.72 -
none 0 6 1 0.000 -2971.60 -24.01 433.86 -
none 0 12 0 0.000 -2970.92 -23.62 434.47 -
none 0 15 0 0.000 -2969.66 -23.83 438.66 -
none 0 16 0 0.000 -2971.21 -23.82 438.83 -
none 0 17 0 0.000 -2970.24 -24.00 433.78 -
none 0 18 0 0.000 -2971.73 -24.00 433.95 -
none 0 0 0 0.911 -2971.93 -24.40 419.99 -
none 0 0 1 0.911 -2973.64 -24.40 420.23 -
none 0 3 0 3.643 -2971.33 -24.28 425.16 -
none 0 3 1 3.643 -2972.79 -24.27 425.37 -
none 0 4 0 0.000 -2971.34 -24.41 425.20 -
none 0 4 1 0.000 -2972.77 -24.40 425.40 -
none 0 6 0 0.000 -2973.67 -24.61 420.24 -
none 0 6 1 0.000 -2971.90 -24.61 420.00 -
none 0 8 0 1.825 -2971.38 -24.77 425.39 -
none 0 8 1 1.825 -2972.69 -24.76 425.57 -
none 0 12 0 0.000 -2972.57 -23.67 421.62 -
none 0 17 0 0.000 -2971.93 -24.51 419.99 -
none 0 18 0 0.000 -2973.64 -24.51 420.23 -
none 0 0 0 0.902 -2968.01 -24.17 426.63 -
none 0 0 1 0.902 -2966.59 -24.18 426.45 -
none 0 1 0 0.902 -2968.02 -24.32 426.60 -
none 0 1 1 0.902 -2966.59 -24.32 426.42 -
none 0 3 0 3.607 -2966.07 -24.15 430.77 -
none 0 3 1 3.607 -2967.44 -24.15 430.95 -
none 0 4 0 0.000 -2966.07 -24.24 430.81 -
none 0 4 1 0.000 -2967.43 -24.24 430.98 -
none 0 6 0 0.000 -2966.95 -24.25 426.42 -
none 0 6 1 0.000 -2967.67 -24.25 426.51 -
none 0 8 0 1.807 -2966.07 -24.50 430.99 -
none 0 8 1 1.807 -2967.39 -24.50 431.15 -
none 0 12 0 0.000 -2967.31 -24.11 426.44 -
none 0 15 0 0.000 -2966.07 -24.42 431.00 -
none 0 16 0 0.000 -2967.39 -24.42 431.17 -
)";

constexpr uint32_t kPlayerVehicle = 0xBF4E7380u;

std::vector<pts::Point> Parse(const char* text, uint32_t object_base) {
  std::vector<pts::Point> out;
  std::istringstream in(text);
  std::string line;
  uint32_t object = object_base - 0x400u, last_kind = 99, last_index = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream t(line);
    std::string who, veh, dx, dy, dz;
    uint32_t kind = 0, index = 0;
    double intensity = 0, x = 0, y = 0, z = 0;
    t >> who >> veh >> kind >> index >> intensity >> x >> y >> z >> dx;
    if (kind < last_kind || (kind == last_kind && index <= last_index)) object += 0x400u;
    last_kind = kind;
    last_index = index;
    pts::Point p;
    p.kind = kind;
    p.index = index;
    p.object = object;
    p.vehicle = uint32_t(std::stoul(veh, nullptr, 16));
    p.owned = p.vehicle != 0;
    p.player = p.vehicle == kPlayerVehicle;
    p.pos[0] = float(x);
    p.pos[1] = float(y);
    p.pos[2] = float(z);
    if (dx != "-") {
      t >> dy >> dz;
      p.dir[0] = std::stof(dx);
      p.dir[1] = std::stof(dy);
      p.dir[2] = std::stof(dz);
      p.has_dir = true;
    }
    p.intensity = p.peak = float(intensity);
    out.push_back(p);
  }
  return out;
}

std::vector<pts::Point> Concat(std::initializer_list<std::vector<pts::Point>> parts) {
  std::vector<pts::Point> all;
  for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end());
  return all;
}

// The six cars of the three frames: the racer with its lights off (0xB0000000...), the player (0xB0000400), the racer with its lights on (0xB1000000), three traffic cars (0xB2000000...).
struct Probe {
  cl::CarLightsFrame cars;
  std::vector<pts::Point> points;
  double camera[3] = {820.0, 19.0, -800.0};
  const cl::Car* player = nullptr;
  const cl::Car* racer_on = nullptr;
  const cl::Car* racer_off = nullptr;
  std::array<const cl::Car*, 3> traffic{};
};

const cl::Car* CarOfObject(const cl::CarLightsFrame& f, uint32_t object) {
  for (uint32_t i = 0; i < f.count; ++i) {
    if (f.cars[i].object == object) return &f.cars[i];
  }
  return nullptr;
}

void FillProbe(Probe* pp) {
  Probe& p = *pp;
  p.points = Concat({Parse(kFrame3603, 0xB0000000u), Parse(kFrame13623, 0xB1000000u), Parse(kFrame122, 0xB2000000u)});
  cl::Build(p.points, {}, p.camera, &p.cars);
  for (uint32_t i = 0; i < p.cars.count; ++i) {
    if (p.cars.cars[i].player) p.player = &p.cars.cars[i];
  }
  p.racer_off = CarOfObject(p.cars, 0xB0000000u);
  p.racer_on = CarOfObject(p.cars, 0xB1000000u);
  p.traffic = {CarOfObject(p.cars, 0xB2000000u), CarOfObject(p.cars, 0xB2000400u), CarOfObject(p.cars, 0xB2000800u)};
}

// ---------------------------------------------------------------------------
// a made-up police car: an owned car heading -z at the origin; the head lamps at the front, the brake lamps at the back, the civilians' kind 8 pair beside the headlights, a light bar on the roof
// (kinds 7 and 9 each a pair, kind 8 one lamp in the middle), every intensity a parameter.
// ---------------------------------------------------------------------------

struct CopState {
  float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f;  // the kinds 7, 8 (the roof's one lamp) and 9
  float head = 2.0f;
  float brake = 0.5f;
  float turn_l = 0.0f, turn_r = 0.0f, reverse = 0.0f;
  float civilian_police1 = 1.0f;  // the kind 8 pair beside the headlights
};

pts::Point Pt(uint32_t object, uint32_t kind, uint32_t index, float intensity, float x, float y, float z, const float dir[3] = nullptr, uint32_t vehicle = 0x1000u, bool owned = true) {
  pts::Point p;
  p.kind = kind;
  p.index = index;
  p.object = object;
  p.vehicle = vehicle;
  p.owned = owned;
  p.pos[0] = x;
  p.pos[1] = y;
  p.pos[2] = z;
  if (dir != nullptr) {
    p.dir[0] = dir[0];
    p.dir[1] = dir[1];
    p.dir[2] = dir[2];
    p.has_dir = true;
  }
  p.intensity = p.peak = intensity;
  return p;
}

std::vector<pts::Point> CopPoints(uint32_t object, float ox, float oz, const CopState& s) {
  static const float back[3] = {0.0f, 0.0f, 1.0f};      // the headlights' stored direction: the car's back axis
  static const float forward[3] = {0.0f, 0.0f, -1.0f};  // the rear lamps': the car's forward
  std::vector<pts::Point> v;
  v.push_back(Pt(object, pts::kKindBrake, 0, s.brake, ox - 0.7f, 0.8f, oz + 2.0f, forward));
  v.push_back(Pt(object, pts::kKindBrake, 1, s.brake, ox + 0.7f, 0.8f, oz + 2.0f, forward));
  v.push_back(Pt(object, pts::kKindHeadlight, 0, s.head, ox - 0.7f, 0.7f, oz - 2.0f, back));
  v.push_back(Pt(object, pts::kKindHeadlight, 1, s.head, ox + 0.7f, 0.7f, oz - 2.0f, back));
  v.push_back(Pt(object, pts::kKindTurn, 0, s.turn_l, ox - 0.8f, 0.8f, oz + 2.0f, forward));
  v.push_back(Pt(object, pts::kKindTurn, 1, s.turn_r, ox + 0.8f, 0.8f, oz + 2.0f, forward));
  v.push_back(Pt(object, pts::kKindPolice0, 0, s.p0, ox - 0.5f, 1.5f, oz + 0.0f, forward));
  v.push_back(Pt(object, pts::kKindPolice0, 1, s.p0, ox - 0.2f, 1.5f, oz + 0.0f, forward));
  v.push_back(Pt(object, pts::kKindPolice1, 0, s.civilian_police1, ox - 0.6f, 0.7f, oz - 2.0f, back));  // beside the left headlight
  v.push_back(Pt(object, pts::kKindPolice1, 1, s.civilian_police1, ox + 0.8f, 0.7f, oz - 2.0f, back));  // and the right one
  v.push_back(Pt(object, pts::kKindPolice1, 2, s.p1, ox + 0.0f, 1.55f, oz + 0.0f, forward));            // the roof's
  v.push_back(Pt(object, pts::kKindPolice2, 0, s.p2, ox + 0.2f, 1.5f, oz + 0.0f, forward));
  v.push_back(Pt(object, pts::kKindPolice2, 1, s.p2, ox + 0.5f, 1.5f, oz + 0.0f, forward));
  v.push_back(Pt(object, pts::kKindReverse, 0, s.reverse, ox + 0.3f, 0.9f, oz + 2.05f, forward));
  return v;
}

cl::CarLightsFrame BuildCars(const std::vector<pts::Point>& points, const double camera[3]) {
  cl::CarLightsFrame f;
  cl::Build(points, {}, camera, &f);
  return f;
}

const double kCamera[3] = {0.0, 1.0, 10.0};

// ---------------------------------------------------------------------------
// the tiers and the budgets
// ---------------------------------------------------------------------------

void TierChecks() {
  Check(lp::TierOfWord("ultra") == lp::Tier::kUltra && lp::TierOfWord("high") == lp::Tier::kHigh && lp::TierOfWord("medium") == lp::Tier::kMedium && lp::TierOfWord("off") == lp::Tier::kMedium &&
            lp::TierOfWord("") == lp::Tier::kMedium && lp::TierOfWord("auto") == lp::Tier::kMedium,
        "the light shadows' words are the tiers: ultra, high, and everything else (off, medium) the smallest");
  Check(std::string(lp::TierName(lp::Tier::kUltra)) == "ultra" && std::string(lp::TierName(lp::Tier::kHigh)) == "high" && std::string(lp::TierName(lp::Tier::kMedium)) == "medium", "the tier names");
  const lp::ClassBudget m = lp::BudgetOf(lp::Tier::kMedium), h = lp::BudgetOf(lp::Tier::kHigh), u = lp::BudgetOf(lp::Tier::kUltra);
  Check(m.police <= h.police && h.police <= u.police && m.head <= h.head && h.head <= u.head && m.rear <= h.rear && h.rear <= u.rear && m.mod <= h.mod && h.mod <= u.mod, "a higher tier never has less of any class");
  Check(m.police < u.police && m.head < u.head && m.rear < u.rear && m.mod < u.mod, "and the ends differ in every class");
  Check(m.police >= 1 && m.head >= 1 && m.rear >= 1 && m.mod >= 1, "every class has a light at the smallest tier");
  Check(m.police + m.head + m.rear + m.mod <= lp::kMaxPassLights && h.police + h.head + h.rear + h.mod <= lp::kMaxPassLights, "medium and high fit the pass's 32 whole");
  Check(u.police + u.head + u.rear + u.mod > lp::kMaxPassLights, "ultra's classes together are over the pass's 32: the priority decides (a documented over-ask)");
  Check(lp::CapOf(u, lp::SourceClass::kPolice) == u.police && lp::CapOf(u, lp::SourceClass::kHeadLamps) == u.head && lp::CapOf(u, lp::SourceClass::kRearLamps) == u.rear &&
            lp::CapOf(u, lp::SourceClass::kMod) == u.mod && lp::CapOf(u, lp::SourceClass::kFrame) == u.frame && u.frame == 4 && lp::CapOf(u, lp::SourceClass::kHandedOver) == lp::kMaxPassLights,
        "CapOf names each class's number (GB-H2: the frame's is the rig's four), the handed over ones have the pass's");
  Check(lp::PoliceReach(lp::Tier::kMedium) < lp::PoliceReach(lp::Tier::kHigh) && lp::PoliceReach(lp::Tier::kHigh) < lp::PoliceReach(lp::Tier::kUltra), "the police reach grows with the tier");
  Check(lp::HeadReach(lp::Tier::kMedium) < lp::HeadReach(lp::Tier::kHigh) && lp::HeadReach(lp::Tier::kHigh) < lp::HeadReach(lp::Tier::kUltra) && lp::HeadReach(lp::Tier::kUltra) < 70.0f,
        "the head lamps' reach grows with the tier and stays under the rig's 70 m: small local lights");
  Check(lp::RearReach(lp::Tier::kMedium) < lp::RearReach(lp::Tier::kHigh) && lp::RearReach(lp::Tier::kHigh) < lp::RearReach(lp::Tier::kUltra), "the rear reach grows with the tier");
  // the source mask: six bits, cars are three of them
  Check(lp::kSourceMask == 63 && lp::kSourceCars == (4 | 8 | 16) && (lp::kSourceMask & lp::kSourceMod) != 0 && lp::kSourceFrameAndTest == 3, "the sources mask is six bits, 4 + 8 + 16 are the cars'");
  Check(lp::kSourceClassCount == 7 && std::string(lp::SourceClassName(lp::SourceClass::kPolice)) == "police" && std::string(lp::SourceClassName(lp::SourceClass::kMod)) == "mod lamps", "the class names");
}

// ---------------------------------------------------------------------------
// the picker's score
// ---------------------------------------------------------------------------

void ScoreChecks() {
  const float cam[3] = {0.0f, 0.0f, 0.0f};
  const float p1[3] = {3.0f, 4.0f, 0.0f};  // 5 m
  const float red[3] = {1.0f, 0.0f, 0.0f};
  Light a = MakeOmniLight(LightSource::kPolice, p1, red, 12.0f, 12.0f, 14.0f, 77);
  CheckNear(lp::Score(a, cam), 12.0 / (25.0 + 12.0), 1.0e-6, "the score is the lamp picker's: brightness over distance squared plus the plateau");
  Light b = a;
  b.pos[0] = 6.0f;
  b.pos[1] = 8.0f;  // 10 m
  Check(lp::Score(b, cam) < lp::Score(a, cam), "a nearer light scores more");
  Light dim = a;
  dim.color[0] = 3.0f;
  Check(lp::Score(dim, cam) < lp::Score(a, cam), "a dimmer light scores less");
  Light off = a;
  off.color[0] = off.color[1] = off.color[2] = 0.0f;
  CheckNear(lp::Score(off, cam), 0.0, 1.0e-9, "no colour, no score");
  Light head = a;
  head.law_a = 0.0f;
  CheckNear(lp::Score(head, cam), 12.0 / (25.0 + 1.0), 1.0e-6, "a plateau under 1 counts as 1");

  // ranking: best first, ties by id, a cap
  std::vector<Light> list;
  for (uint32_t i = 0; i < 6; ++i) {
    Light l = a;
    l.id = 100 + (5 - i);  // ids descending in the list
    l.pos[0] = 3.0f;
    l.pos[1] = 4.0f;
    list.push_back(l);
  }
  Light far = a;
  far.id = 1;
  far.pos[0] = 30.0f;
  list.push_back(far);
  Light near = a;
  near.id = 999;
  near.pos[0] = 0.0f;
  near.pos[1] = 2.0f;
  list.push_back(near);
  lp::RankByScore(&list, cam);
  Check(list.size() == 8 && list.front().id == 999 && list.back().id == 1, "the nearest first, the farthest last");
  bool ties_by_id = true;
  for (size_t i = 1; i + 1 < list.size() - 1; ++i) {
    ties_by_id = ties_by_id && list[i].id < list[i + 1].id;
  }
  Check(ties_by_id, "equal scores go by id, never by the order the table gave them");
  Check(list[0].importance > list[1].importance && list[1].importance == list[2].importance, "the score is kept as the light's importance");
  lp::RankByScore(&list, cam, 3);
  Check(list.size() == 3 && list[0].id == 999, "a cap keeps the best");
}

// ---------------------------------------------------------------------------
// the police light bars
// ---------------------------------------------------------------------------

void PoliceChecks(const Probe& probe) {
  // THE PROBE'S CIVILIANS: Police1 on at the headlights (1.0 for the player, 1.8 on traffic) is not a siren
  std::vector<Light> out;
  for (const cl::Car* car : {probe.player, probe.racer_on, probe.racer_off, probe.traffic[0], probe.traffic[1], probe.traffic[2]}) {
    Check(car != nullptr, "the probe's car is there");
    lp::MakePoliceLights(*car, lp::Tier::kUltra, 1.0f, &out);
  }
  Check(out.empty(), "the recorded player, racers and traffic make NO police light: their kind 8 lamps sit beside the headlights (0.1 to 0.55 m)");
  uint32_t kind8_seen = 0, kind8_not_sirens = 0;
  for (const cl::Car* car : {probe.player, probe.traffic[1], probe.traffic[2]}) {
    for (uint32_t i = 0; i < car->lamp_count; ++i) {
      if (car->lamps[i].kind == pts::kKindPolice1) {
        ++kind8_seen;
        kind8_not_sirens += lp::IsSirenLamp(*car, car->lamps[i]) ? 0u : 1u;
      }
    }
  }
  Check(kind8_seen == 6 && kind8_not_sirens == 6, "all six recorded kind 8 lamps are the headlights' neighbours, not sirens");
  Check(lp::kPoliceNearHeadlamp > 0.55f && lp::kPoliceNearHeadlamp < 1.2f, "the neighbour distance holds the farthest recorded (0.55 m) and is under a roof's distance from the headlights");

  // a made-up police car, flashing: the red side, then the blue side
  const lp::Tier tier = lp::Tier::kHigh;
  CopState red_side;
  red_side.p0 = 1.0f;
  red_side.p2 = 0.0f;
  cl::CarLightsFrame f1 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, red_side), kCamera);
  Check(f1.count == 1, "one car");
  out.clear();
  lp::MakePoliceLights(f1.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1, "kind 7 lit, the rest off: one police light");
  const Light red = out[0];
  Check(red.source == LightSource::kPolice && red.law == LightLaw::kOmni && red.kind == LightKind::kPoint && red.roles == kRoleMaskSurface && (red.flags & kLightFlagGameLit) == 0,
        "an omni light of the police source, surface light only, not game lit");
  CheckNear(red.pos[0], -0.35, 1.0e-5, "at the middle of the kind's two lit lamps: x");
  CheckNear(red.pos[1], 1.5, 1.0e-5, "... y");
  CheckNear(red.pos[2], 0.0, 1.0e-5, "... z");
  const lp::Look look7 = lp::PoliceLook(pts::kKindPolice0);
  CheckNear(red.color[0], look7.rgb[0] * look7.intensity, 1.0e-5, "red: colour x level 1 x I");
  Check(red.color[0] > 10.0f * red.color[1] && red.color[0] > 10.0f * red.color[2], "Police0 is red");
  CheckNear(red.law_a, look7.intensity, 1.0e-6, "the plateau is the look's intensity");
  CheckNear(red.range, lp::PoliceReach(tier), 1.0e-6, "the reach is the tier's");
  Check(red.id != kNoLightId && red.car_key == f1.cars[0].lights.car_key && red.car_key != 0, "an id and the car's key");

  CopState blue_side;
  blue_side.p0 = 0.0f;
  blue_side.p2 = 1.0f;
  cl::CarLightsFrame f2 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, blue_side), kCamera);
  out.clear();
  lp::MakePoliceLights(f2.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1 && out[0].color[2] > 5.0f * out[0].color[0] && out[0].color[2] > out[0].color[1], "the other half of the flash: one blue light (kind 9)");
  Check(out[0].id != red.id, "a kind's light has its own id");
  CheckNear(out[0].pos[0], 0.35, 1.0e-5, "at the middle of its two lamps");

  CopState both;
  both.p0 = 1.0f;
  both.p2 = 1.0f;
  both.p1 = 0.0f;
  cl::CarLightsFrame f3 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, both), kCamera);
  out.clear();
  lp::MakePoliceLights(f3.cars[0], tier, 1.0f, &out);
  Check(out.size() == 2, "both halves lit: two lights");
  // the same lamp in the next frame keeps its id; the flash changes only what is lit
  Check((out[0].id == red.id || out[1].id == red.id), "the red light keeps its id from a frame to the next");

  // the roof's kind 8 lamp is a siren (it is not at the headlights), the headlight pair's neighbours are not
  CopState white;
  white.p1 = 1.0f;
  cl::CarLightsFrame f4 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, white), kCamera);
  out.clear();
  lp::MakePoliceLights(f4.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1, "the roof's kind 8 lamp lit (the pair at the headlights too, as every civilian's): one light, the roof's");
  Check(out.size() == 1 && out[0].pos[1] > 1.4f && std::fabs(out[0].pos[2]) < 0.1f, "... at the roof, not at the headlights");
  Check(out.size() == 1 && out[0].color[0] > 0.9f * out[0].color[1] && out[0].color[1] > 0.8f * out[0].color[0] && out[0].color[2] > 0.6f * out[0].color[0], "... and white");

  // the level: a lamp at 0.4 is a dimmer light, one at 0.05 or below is off, one over 1 saturates
  CopState dimmed;
  dimmed.p0 = 0.4f;
  cl::CarLightsFrame f5 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, dimmed), kCamera);
  out.clear();
  lp::MakePoliceLights(f5.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1, "a lamp at 0.4 is lit");
  CheckNear(out[0].color[0], 0.4 * look7.rgb[0] * look7.intensity, 1.0e-4, "its light is the level's share (the flash fades as the game's value does)");
  CopState fading;
  fading.p0 = 0.05f;
  cl::CarLightsFrame f6 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, fading), kCamera);
  out.clear();
  lp::MakePoliceLights(f6.cars[0], tier, 1.0f, &out);
  Check(out.empty(), "a lamp at the off threshold (0.05) adds no light");
  CopState hot;
  hot.p0 = 1.9f;
  cl::CarLightsFrame f7 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, hot), kCamera);
  out.clear();
  lp::MakePoliceLights(f7.cars[0], tier, 1.0f, &out);
  CheckNear(out[0].color[0], look7.rgb[0] * look7.intensity, 1.0e-4, "a reading over 1 (a traffic car's 1.8) saturates at 1");

  // the gain and the tier
  out.clear();
  lp::MakePoliceLights(f1.cars[0], tier, 2.5f, &out);
  CheckNear(out[0].color[0], 2.5 * red.color[0], 1.0e-4, "the cars' gain multiplies the colour");
  std::vector<Light> tiers;
  lp::MakePoliceLights(f1.cars[0], lp::Tier::kMedium, 1.0f, &tiers);
  lp::MakePoliceLights(f1.cars[0], lp::Tier::kUltra, 1.0f, &tiers);
  Check(tiers.size() == 2 && tiers[0].range < tiers[1].range && tiers[0].color[0] == tiers[1].color[0], "the tier changes the reach, not the colour");

  // two cars: two ids
  std::vector<pts::Point> two = CopPoints(0xC0000000u, 0.0f, 0.0f, red_side);
  const std::vector<pts::Point> other = [&] {
    std::vector<pts::Point> v = CopPoints(0xC0000400u, 30.0f, 0.0f, red_side);
    for (pts::Point& p : v) p.vehicle = 0x2000u;
    return v;
  }();
  two.insert(two.end(), other.begin(), other.end());
  const cl::CarLightsFrame f8 = BuildCars(two, kCamera);
  out.clear();
  for (uint32_t i = 0; i < f8.count; ++i) lp::MakePoliceLights(f8.cars[i], tier, 1.0f, &out);
  Check(f8.count == 2 && out.size() == 2 && out[0].id != out[1].id, "two police cars make two lights with their own ids");
  // the same car in another frame (other intensities) has the same ids
  cl::CarLightsFrame f9 = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.5f, CopState{}), kCamera);  // moved, nothing lit
  Check(f9.count == 1 && f9.cars[0].lights.car_key == f1.cars[0].lights.car_key, "the car's key is the vehicle's: the same in the next frame, moved");

  // the pixel it makes: a wall 4 m from the red lamp, facing it
  const float wall[3] = {red.pos[0], red.pos[1], red.pos[2] - 4.0f};
  const float facing[3] = {0.0f, 0.0f, 1.0f};
  const float one[3] = {1.0f, 1.0f, 1.0f};
  lp::Units units;
  float px[3];
  lp::PixelLight(red, units, wall, facing, one, 1.0f, px);
  CheckNear(px[0], look7.rgb[0] * look7.intensity / 16.0, 1.0e-4, "a wall 4 m from the red lamp: colour x I over d^2 (4 m is past the plateau of a lamp of I 12)");
  CheckNear(px[1], look7.rgb[1] * look7.intensity / 16.0, 1.0e-5, "... in every channel");
  const float inside[3] = {red.pos[0], red.pos[1], red.pos[2] - 1.5f};
  lp::PixelLight(red, units, inside, facing, one, 1.0f, px);
  CheckNear(px[0], look7.rgb[0], 1.0e-4, "inside the plateau the term is the colour's own (x level)");
  const float edge[3] = {red.pos[0], red.pos[1], red.pos[2] - (lp::PoliceReach(tier) + 0.5f)};
  lp::PixelLight(red, units, edge, facing, one, 1.0f, px);
  Check(px[0] == 0.0f && px[1] == 0.0f && px[2] == 0.0f, "past the reach nothing");
  const float away[3] = {0.0f, 0.0f, -1.0f};
  lp::PixelLight(red, units, wall, away, one, 1.0f, px);
  Check(px[0] == 0.0f, "a surface turned away from the lamp takes nothing");
  // by day: the night light intensity is 0
  lp::Units day;
  day.night = 0.0f;
  lp::PixelLight(red, day, wall, facing, one, 1.0f, px);
  Check(px[0] == 0.0f, "by day (the night light intensity 0) a police light adds nothing");
}

// ---------------------------------------------------------------------------
// the rear lamps
// ---------------------------------------------------------------------------

void RearChecks(const Probe& probe) {
  const lp::Tier tier = lp::Tier::kHigh;
  // the recorded traffic: car 1 brake 0.84 (two lamps), car 2 0.911, car 3 brake 0.902 and tail 0.902: one light each, at the middle of the lit lamps
  std::vector<Light> out;
  lp::MakeRearLights(*probe.traffic[0], tier, 1.0f, &out);
  Check(out.size() == 1, "traffic car 1: its two brake lamps make one light (the turn signals read 0)");
  const lp::Look bt = lp::kBrakeTailLook;
  CheckNear(out[0].color[0], bt.rgb[0] * 0.84 * bt.intensity, 1.0e-4, "its colour is the brake look x the lamps' 0.84 level");
  Check(out[0].color[0] > 10.0f * out[0].color[1], "red");
  CheckNear(out[0].pos[0], 0.5 * (-2970.50 + -2971.49), 1.0e-3, "at the middle of the two lamps: x");
  CheckNear(out[0].pos[2], 0.5 * (433.70 + 433.81), 1.0e-3, "... z");
  CheckNear(out[0].range, lp::RearReach(tier), 1.0e-6, "the tier's reach");
  CheckNear(out[0].law_a, bt.intensity, 1.0e-6, "the plateau is the look's");
  Check(out[0].source == LightSource::kOther && out[0].law == LightLaw::kOmni && out[0].roles == kRoleMaskSurface, "an omni surface light");
  out.clear();
  lp::MakeRearLights(*probe.traffic[2], tier, 1.0f, &out);
  Check(out.size() == 1, "traffic car 3: brake and taillight together are one light");
  CheckNear(out[0].pos[0], 0.25 * (-2968.01 - 2966.59 - 2968.02 - 2966.59), 2.0e-3, "at the middle of its four lit lamps");
  out.clear();
  lp::MakeRearLights(*probe.player, tier, 1.0f, &out);
  Check(out.size() == 1, "the player's brake lamps read 0.075 (the glow): lit, one light");
  CheckNear(out[0].color[0], bt.rgb[0] * 0.075 * bt.intensity, 1.0e-4, "at the glow's level: dim");
  out.clear();
  lp::MakeRearLights(*probe.racer_on, tier, 1.0f, &out);
  Check(out.size() == 1, "the racer's brake (0.5) and third brake (1.0) lamps are one light");
  CheckNear(out[0].color[0], bt.rgb[0] * 1.0 * bt.intensity, 1.0e-4, "at the strongest lamp's level");
  out.clear();
  lp::MakeRearLights(*probe.racer_off, tier, 1.0f, &out);
  Check(out.empty(), "a racer whose lights are off makes none");

  // turn signals, per side; reverse
  CopState s;
  s.brake = 0.0f;
  s.turn_l = 1.0f;
  cl::CarLightsFrame f = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, s), kCamera);
  out.clear();
  lp::MakeRearLights(f.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1, "the left turn signal alone: one light");
  CheckNear(out[0].color[0], lp::kTurnLook.rgb[0] * lp::kTurnLook.intensity, 1.0e-4, "orange: the turn look");
  Check(out[0].color[0] > out[0].color[1] && out[0].color[1] > 5.0f * out[0].color[2], "orange, not red");
  Check(out[0].pos[0] < 0.0f, "on the car's left (negative x here: the car heads -z)");
  s.turn_r = 1.0f;
  f = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, s), kCamera);
  out.clear();
  lp::MakeRearLights(f.cars[0], tier, 1.0f, &out);
  Check(out.size() == 2 && out[0].id != out[1].id && out[0].pos[0] * out[1].pos[0] < 0.0f, "the hazards: both sides, two lights on opposite sides with their own ids");
  s.turn_l = s.turn_r = 0.0f;
  s.reverse = 1.0f;
  f = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, s), kCamera);
  out.clear();
  lp::MakeRearLights(f.cars[0], tier, 1.0f, &out);
  Check(out.size() == 1 && out[0].color[0] > 0.9f * out[0].color[2] && out[0].color[2] > 0.7f * out[0].color[0], "the reverse lamp: one white light");
  s.brake = 0.9f;
  f = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, s), kCamera);
  out.clear();
  lp::MakeRearLights(f.cars[0], tier, 1.0f, &out);
  Check(out.size() == 2, "brake and reverse: two lights");
  std::set<uint64_t> ids;
  for (const Light& l : out) ids.insert(l.id);
  Check(ids.size() == 2, "with different ids");
  // the police and head lamps are not rear lamps
  CopState cop;
  cop.p0 = 1.0f;
  cop.p2 = 1.0f;
  cop.brake = 0.0f;
  f = BuildCars(CopPoints(0xC0000000u, 0.0f, 0.0f, cop), kCamera);
  out.clear();
  lp::MakeRearLights(f.cars[0], tier, 1.0f, &out);
  Check(out.empty(), "the light bar and the headlights are not rear lamps");

  // a rear light on the road behind the car: small, red, short: the pixel
  Light brake = [&] {
    std::vector<Light> v;
    lp::MakeRearLights(*probe.traffic[0], tier, 1.0f, &v);
    return v[0];
  }();
  const float behind[3] = {brake.pos[0], brake.pos[1], brake.pos[2] + 2.0f};
  const float facing[3] = {0.0f, 0.0f, -1.0f};
  const float one[3] = {1.0f, 1.0f, 1.0f};
  float px[3];
  lp::PixelLight(brake, lp::Units{}, behind, facing, one, 1.0f, px);
  CheckNear(px[0], brake.color[0] / 4.0, 1.0e-3, "a bumper 2 m behind a braking car takes colour over d^2 (past the plateau's radius, 1.6 m)");
  Check(px[0] > 5.0f * px[1], "a red wash");
  const float far_behind[3] = {brake.pos[0], brake.pos[1], brake.pos[2] + lp::RearReach(tier) + 1.0f};
  lp::PixelLight(brake, lp::Units{}, far_behind, facing, one, 1.0f, px);
  Check(px[0] == 0.0f, "no light past the reach");
}

// ---------------------------------------------------------------------------
// the head lamps the forward stage does not light
// ---------------------------------------------------------------------------

Light RigLightAt(const float pos[3], uint64_t id) {
  Light l;
  for (int i = 0; i < 3; ++i) l.pos[i] = pos[i];
  l.dir[2] = 1.0f;
  l.dir[1] = 0.0f;
  l.cos_outer = 0.788f;
  l.cos_inner = 0.978f;
  l.range = 70.0f;
  l.color[0] = 1.0f;
  l.color[1] = 0.93f;
  l.color[2] = 0.78f;
  l.law_a = 26.0f;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  l.law = LightLaw::kHeadlight;
  l.roles = kRoleMaskAll;
  l.id = id;
  return l;
}

void HeadChecks(const Probe& probe) {
  const lp::Tier tier = lp::Tier::kHigh;
  LightFrame frame;  // no rig light
  std::vector<Light> out;
  const cl::Car& car2 = *probe.traffic[1];
  lp::MakeHeadLight(car2, frame, tier, 1.0f, &out);
  Check(out.size() == 1, "a traffic car whose headlights are on and not in the frame gets a head lamp light");
  const Light& l = out[0];
  Check(l.kind == LightKind::kSpot && l.law == LightLaw::kHeadlight && l.source == LightSource::kHeadlight && l.roles == kRoleMaskSurface && (l.flags & kLightFlagGameLit) == 0,
        "a spot of the headlight law, surface light only");
  CheckNear(l.law_a, light_laws::kHeadlightFalloffDistance, 1.0e-6, "the rig's falloff distance");
  CheckNear(l.cos_outer, std::cos(38.0 * 3.14159265358979323846 / 180.0), 1.0e-5, "the rig's outer cone, 38 degrees");
  CheckNear(l.cos_inner, std::cos(12.0 * 3.14159265358979323846 / 180.0), 1.0e-5, "and the inner, 12");
  CheckNear(l.range, lp::HeadReach(tier), 1.0e-6, "the tier's reach");
  CheckNear(std::sqrt(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]), 1.0, 1.0e-5, "a unit axis");
  Check(l.dir[2] > 0.97f && std::fabs(l.dir[0]) < 0.2f, "the car heads +z in the recording (a little to +x): the beam goes along it");
  CheckNear(l.dir[1], -std::tan(2.0 * 3.14159265358979323846 / 180.0) / std::sqrt(1.0 + std::pow(std::tan(2.0 * 3.14159265358979323846 / 180.0), 2.0)), 3.0e-3, "dipped 2 degrees");
  // at the middle of the two lit lamps, the nudge in front
  const double mid_x = 0.5 * (-2971.33 + -2972.79), mid_y = 0.5 * (-24.28 + -24.27), mid_z = 0.5 * (425.16 + 425.37);
  CheckNear(l.pos[0], mid_x + 0.12 * l.dir[0], 2.0e-3, "at the middle of the lamps and 0.12 m in front of them: x");
  CheckNear(l.pos[1], mid_y + 0.12 * l.dir[1], 2.0e-3, "... y");
  CheckNear(l.pos[2], mid_z + 0.12 * l.dir[2], 2.0e-3, "... z");
  CheckNear(l.color[0], 1.0 * 0.5, 1.0e-5, "the colour: the rig's warm white x the level (3.6 reads full) x half");
  Check(l.color[0] > l.color[1] && l.color[1] > l.color[2], "warm");
  Check(l.id != kNoLightId && l.car_key == car2.lights.car_key, "an id and the car's key");
  // the gain
  std::vector<Light> g;
  lp::MakeHeadLight(car2, frame, tier, 2.0f, &g);
  CheckNear(g[0].color[0], 2.0 * l.color[0], 1.0e-5, "the cars' gain multiplies it");

  // covered: a frame light where the rig puts one (the middle of the pair, the nudge in front)
  LightFrame covered;
  covered.lights[0] = RigLightAt(l.pos, 4242);
  covered.count = 1;
  out.clear();
  lp::MakeHeadLight(car2, covered, tier, 1.0f, &out);
  Check(out.empty(), "a car with the rig's headlight at its lamps is covered: no second light");
  float far_pos[3] = {l.pos[0] + 6.0f, l.pos[1], l.pos[2]};
  LightFrame elsewhere;
  elsewhere.lights[0] = RigLightAt(far_pos, 4243);
  elsewhere.count = 1;
  lp::MakeHeadLight(car2, elsewhere, tier, 1.0f, &out);
  Check(out.size() == 1, "a rig light 6 m away is another car's: this one is not covered");
  float near_pos[3] = {l.pos[0] + 2.5f, l.pos[1], l.pos[2]};
  LightFrame close;
  close.lights[0] = RigLightAt(near_pos, 4244);
  close.count = 1;
  out.clear();
  lp::MakeHeadLight(car2, close, tier, 1.0f, &out);
  Check(out.empty(), "within 3 m it is covered");
  // only a headlight counts as cover: a street lamp, an off light, an unused slot
  LightFrame notcover;
  Light lamp = RigLightAt(l.pos, 4245);
  lamp.source = LightSource::kLamp;
  lamp.law = LightLaw::kGameLamp;
  notcover.lights[0] = lamp;
  Light off = RigLightAt(l.pos, 4246);
  off.range = 0.0f;
  notcover.lights[1] = off;
  Light hole = RigLightAt(l.pos, kNoLightId);
  notcover.lights[2] = hole;
  notcover.count = 3;
  out.clear();
  lp::MakeHeadLight(car2, notcover, tier, 1.0f, &out);
  Check(out.size() == 1, "a street lamp, a light of no range and an unused slot are not a car's headlight");
  const float mid[3] = {float(mid_x), float(mid_y), float(mid_z)};
  Check(!lp::HeadlightCovered(notcover, mid) && lp::HeadlightCovered(covered, mid) && !lp::HeadlightCovered(LightFrame{}, mid), "HeadlightCovered");

  // the player (an owned car: its own stored direction): the beam goes along minus the stored direction
  out.clear();
  lp::MakeHeadLight(*probe.player, frame, tier, 1.0f, &out);
  Check(out.size() == 1, "the player's headlights, when the rig is not lighting them (HEADLIGHTS off), make a light");
  CheckNear(out[0].dir[0], -0.47 / 1.0, 0.03, "the beam goes along minus the stored direction: x");
  Check(out[0].dir[2] > 0.85f, "... z");
  // a car whose headlights are off, and a racer with them on
  out.clear();
  lp::MakeHeadLight(*probe.racer_off, frame, tier, 1.0f, &out);
  Check(out.empty(), "headlights at 0 (by day, or off): no light");
  lp::MakeHeadLight(*probe.racer_on, frame, tier, 1.0f, &out);
  Check(out.size() == 1 && out[0].dir[0] < -0.8f && out[0].dir[2] < -0.4f, "a racer with its headlights on: the beam goes along minus the stored direction, (-0.88, 0, -0.47) here");
  // a car whose frame is not known: no direction, no light
  std::vector<pts::Point> lone;
  lone.push_back(Pt(0xD0000000u, pts::kKindHeadlight, 0, 2.0f, 5.0f, 1.0f, 5.0f, nullptr, 0, false));
  lone.push_back(Pt(0xD0000000u, pts::kKindHeadlight, 1, 2.0f, 6.0f, 1.0f, 5.0f, nullptr, 0, false));
  const cl::CarLightsFrame fl = BuildCars(lone, kCamera);
  out.clear();
  lp::MakeHeadLight(fl.cars[0], frame, tier, 1.0f, &out);
  Check(fl.count == 1 && fl.cars[0].axes.source == cl::FrameSource::kNone && out.empty(), "an ownerless car with only headlights has no frame: no way to aim, no light");

  // the pixel: the road 15 m ahead of the lamp on the axis, a wall 20 m ahead: the rig's law at the tier's reach, half the strength
  const float road[3] = {l.pos[0] + l.dir[0] * 15.0f, l.pos[1] - 0.6f, l.pos[2] + l.dir[2] * 15.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const float one[3] = {1.0f, 1.0f, 1.0f};
  float px[3];
  lp::Units u;
  u.night = 0.3f;  // the headlight's law does not take the night light intensity
  lp::PixelLight(l, u, road, up, one, 1.0f, px);
  const float scale = light_laws::HeadlightScale(l, [&] {
    light_laws::LightSurface s;
    for (int i = 0; i < 3; ++i) {
      s.position[i] = road[i];
      s.normal[i] = up[i];
    }
    return s;
  }());
  CheckNear(px[0], l.color[0] * scale, 1.0e-6, "the road ahead: the headlight law times the half strength colour, the night light intensity does not touch it");
  Check(px[0] > 0.02f && px[0] < 1.0f, "a plausible light on the road 15 m ahead");
  const float beyond[3] = {l.pos[0] + l.dir[0] * (lp::HeadReach(tier) + 2.0f), l.pos[1], l.pos[2] + l.dir[2] * (lp::HeadReach(tier) + 2.0f)};
  lp::PixelLight(l, u, beyond, up, one, 1.0f, px);
  Check(px[0] == 0.0f, "past the reach nothing");
}

// ---------------------------------------------------------------------------
// every car at once
// ---------------------------------------------------------------------------

void GatherCarChecks(const Probe& probe) {
  lp::CarSourceInputs in;
  in.cars = &probe.cars;
  in.sources = lp::kSourceCars;
  in.tier = lp::Tier::kUltra;
  for (int i = 0; i < 3; ++i) in.camera[i] = float(probe.camera[i]);
  lp::Candidates cand;
  lp::GatherCarLights(in, &cand);
  Check(cand.police.empty(), "the recorded cars: no police light (the civilians' kind 8)");
  Check(cand.head.size() == 5, "the recorded cars: five head lamp lights (the player, the lit racer and the three traffic cars; the dark racer none)");
  Check(cand.rear.size() == 5, "... and five rear lights (the player's glow, the racer, the three traffic cars)");
  // best first: the player is the nearest to the camera
  bool sorted = true;
  for (size_t i = 1; i < cand.head.size(); ++i) sorted = sorted && cand.head[i - 1].importance >= cand.head[i].importance;
  for (size_t i = 1; i < cand.rear.size(); ++i) sorted = sorted && cand.rear[i - 1].importance >= cand.rear[i].importance;
  Check(sorted, "each class is ranked best first");
  Check(cand.head[0].car_key == probe.player->lights.car_key && (cand.head[0].flags & kLightFlagPlayer) != 0, "the nearest head lamp light is the player's, flagged");
  bool player_rear = false;
  for (const Light& l : cand.rear) player_rear = player_rear || l.car_key == probe.player->lights.car_key;
  Check(player_rear, "the player's rear light (its brake glow) is among the rear lights");

  // the frame's rig light at the player's lamps covers the player's car only
  std::vector<Light> one;
  lp::MakeHeadLight(*probe.player, LightFrame{}, lp::Tier::kUltra, 1.0f, &one);
  LightFrame frame;
  frame.lights[0] = RigLightAt(one[0].pos, 9001);
  frame.count = 1;
  in.frame = &frame;
  lp::Candidates cand2;
  lp::GatherCarLights(in, &cand2);
  Check(cand2.head.size() == 4 && cand2.rear.size() == 5, "with the rig lighting the player's car its head lamp light goes and the rear ones stay");
  for (const Light& l : cand2.head) Check(l.car_key != probe.player->lights.car_key, "no head lamp light of the player's car is left");

  // the sources mask
  lp::Candidates only_police, only_head, only_rear, none;
  in.frame = nullptr;
  in.sources = lp::kSourceHeadLamps;
  lp::GatherCarLights(in, &only_head);
  in.sources = lp::kSourceRearLamps;
  lp::GatherCarLights(in, &only_rear);
  in.sources = lp::kSourcePolice;
  lp::GatherCarLights(in, &only_police);
  in.sources = lp::kSourceFrame | lp::kSourceTest | lp::kSourceMod;
  lp::GatherCarLights(in, &none);
  Check(only_head.head.size() == 5 && only_head.rear.empty() && only_head.police.empty(), "the head lamps bit makes head lamp lights only");
  Check(only_rear.rear.size() == 5 && only_rear.head.empty() && only_rear.police.empty(), "the rear lamps bit makes rear lights only");
  Check(only_police.police.empty() && only_police.head.empty() && only_police.rear.empty(), "the police bit alone: the recorded cars have none");
  Check(none.head.empty() && none.rear.empty() && none.police.empty(), "without a car bit nothing is made");
  in.cars = nullptr;
  in.sources = lp::kSourceCars;
  lp::Candidates nocars;
  lp::GatherCarLights(in, &nocars);
  Check(nocars.head.empty() && nocars.rear.empty() && nocars.police.empty(), "no cars, no lights");

  // a police chase: the recorded cars and a police car among them
  std::vector<pts::Point> chase = probe.points;
  CopState lit;
  lit.p0 = 1.0f;
  lit.p2 = 1.0f;
  const std::vector<pts::Point> cop = CopPoints(0xC0000800u, 820.0f, -790.0f, lit);
  chase.insert(chase.end(), cop.begin(), cop.end());
  cl::CarLightsFrame f;
  cl::Build(chase, {}, probe.camera, &f);
  Check(f.count == 7, "seven cars with the police car");
  lp::CarSourceInputs ci;
  ci.cars = &f;
  ci.sources = lp::kSourceCars;
  ci.tier = lp::Tier::kHigh;
  for (int i = 0; i < 3; ++i) ci.camera[i] = float(probe.camera[i]);
  lp::Candidates cc;
  lp::GatherCarLights(ci, &cc);
  Check(cc.police.size() == 2 && cc.head.size() == 6 && cc.rear.size() == 6, "the chase: two police lights, six head lamp lights, six rear lights");
  Check(cc.police[0].car_key == cc.police[1].car_key && cc.police[0].car_key != 0 && cc.police[0].car_key != probe.player->lights.car_key, "both police lights are the police car's");
}

// ---------------------------------------------------------------------------
// the mods' lamps
// ---------------------------------------------------------------------------

mls::Record Lamp(float x, float y, float z, float intensity, uint32_t id, bool on = true) {
  mls::Record r;
  r.pos[0] = x;
  r.pos[1] = y;
  r.pos[2] = z;
  r.colour[0] = 1.0f;
  r.colour[1] = 0.8f;
  r.colour[2] = 0.6f;
  r.dir[1] = -1.0f;
  r.intensity = intensity;
  r.cone_deg = 125.0f;
  r.spread_deg = -50.5f;
  r.id = id;
  r.type = 1;
  r.on = on ? 1 : 0;
  return r;
}

mls::SetPtr MakeSet(const std::string& name, std::vector<mls::Record> lamps) {
  auto set = std::make_shared<mls::Set>();
  set->mod = name;
  set->mod_key = light_id::ModKey(name);
  set->lamps = std::move(lamps);
  return set;
}

void ModChecks() {
  // two mods: A (the camera is over it) and B (it is not)
  const mls::SetPtr a = MakeSet("mod_a", {Lamp(10.0f, 6.0f, 0.0f, 55.0f, 1), Lamp(30.0f, 6.0f, 0.0f, 55.0f, 2)});
  const mls::SetPtr b = MakeSet("mod_b", {Lamp(5000.0f, 6.0f, 0.0f, 55.0f, 1), Lamp(5000.0f, 6.0f, 20.0f, 55.0f, 2), Lamp(5000.0f, 6.0f, 40.0f, 55.0f, 3), Lamp(5000.0f, 6.0f, 60.0f, 20.0f, 4, false),
                                         Lamp(5000.0f, 6.0f, 80.0f, 0.0f, 5), Lamp(5000.0f, 6.0f, 900.0f, 55.0f, 6)});
  const mls::BuiltSets both = std::make_shared<const std::vector<mls::SetPtr>>(std::vector<mls::SetPtr>{a, b});
  const float over_a[3] = {0.0f, 1.0f, 0.0f};
  const float over_b[3] = {5010.0f, 1.0f, 30.0f};
  const float nowhere[3] = {-50000.0f, 1.0f, 0.0f};

  std::vector<Light> out;
  lp::GatherModLamps(both, b.get(), lp::Ab::kGrid, over_b, &out);
  Check(out.empty(), "the camera is over B (its grid lights its lamps): the pass adds none of B's, and A is far");
  out.clear();
  lp::GatherModLamps(both, a.get(), lp::Ab::kGrid, over_a, &out);
  Check(out.empty(), "the camera over A: A's lamps are the grid's, B's are 5000 m away: nothing");
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kGrid, over_b, &out);
  Check(out.size() == 3, "over the city (no mod grid): B's three lit lamps in reach of the camera are the pass's (the lamp that is off, the one of intensity 0 and the one 900 m away are not)");
  for (const Light& l : out) {
    Check((l.flags & kLightFlagGameLit) == 0 && (l.roles & kRoleMaskSurface) != 0 && l.source == LightSource::kMod && lp::TakesFrameLight(l), "each is a mod's lamp allowed to add surface light");
    Check(l.law == LightLaw::kGameLamp && l.law_a == 55.0f, "in the game's law with the plateau I");
  }
  // ranked by score: the camera at z 30: the lamps at z 20 and 40 are 10 m away, z 0 is 30 m
  bool ranked = true;
  for (size_t i = 1; i < out.size(); ++i) ranked = ranked && out[i - 1].importance >= out[i].importance;
  Check(ranked && std::fabs(out[2].pos[2] - 0.0f) < 1.0e-3f, "best first: the lamp 30 m away is last");
  std::set<uint64_t> ids;
  for (const Light& l : out) ids.insert(l.id);
  Check(ids.size() == 3 && ids.count(light_id::ModLampId(b->mod_key, 1)) == 1, "their ids are the mod's key and the record's own id: the same as the manager's");
  // C14: the camera outside B's rectangle (a mod's rectangle is its lamps' extent plus 100 m): a lamp 150 m away is past its own reach (74 m) and still the pass's, 300 m away it is not
  const float outside_b[3] = {5000.0f + 150.0f, 1.0f, 20.0f};
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kGrid, outside_b, &out);
  Check(out.size() == 3, "C14: a camera 150 m outside the mod's lamps (past their reach) still gets them: the pass lights without a grid");
  const float far_b[3] = {5000.0f + 300.0f, 1.0f, 20.0f};
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kGrid, far_b, &out);
  Check(out.empty(), "300 m away (reach 74 m + 200 m): none");
  Check(lp::kModMargin > 100.0f, "the margin is past a mod's rectangle's own 100 m");
  // both sets from the city: A's and B's lamps near the camera
  out.clear();
  const float between[3] = {20.0f, 1.0f, 0.0f};
  lp::GatherModLamps(both, nullptr, lp::Ab::kGrid, between, &out);
  Check(out.size() == 2, "from the middle of A, over the city: A's two lamps (B is far)");
  // the control: nothing
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kNowhere, over_b, &out);
  Check(out.empty(), "the A/B's control mode: no lamp is added by anything");
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kPass, over_b, &out);
  Check(out.size() == 3, "the A/B's pass mode: the inactive sets' lamps are still taken here (the active one is the A/B's own path)");
  out.clear();
  lp::GatherModLamps(nullptr, nullptr, lp::Ab::kGrid, over_b, &out);
  Check(out.empty(), "no built sets: nothing");
  out.clear();
  lp::GatherModLamps(both, nullptr, lp::Ab::kGrid, nowhere, &out);
  Check(out.empty(), "far from every lamp: nothing");
  // the cap: a set of 100 lamps hands over at most the pass's 32
  std::vector<mls::Record> many;
  for (uint32_t i = 0; i < 100; ++i) many.push_back(Lamp(7000.0f + float(i), 6.0f, 0.0f, 55.0f, i + 1));
  const mls::SetPtr c = MakeSet("mod_c", many);
  const mls::BuiltSets with_c = std::make_shared<const std::vector<mls::SetPtr>>(std::vector<mls::SetPtr>{c});
  const float over_c[3] = {7050.0f, 1.0f, 0.0f};
  out.clear();
  lp::GatherModLamps(with_c, nullptr, lp::Ab::kGrid, over_c, &out);
  Check(out.size() == 32, "a hundred lamps in reach: the nearest 32 (a ranked, capped list)");
  Check(std::fabs(out[0].pos[0] - 7050.0f) <= 1.0f, "the first is the nearest");
  // determinism: the same call twice, the same order
  std::vector<Light> again;
  lp::GatherModLamps(with_c, nullptr, lp::Ab::kGrid, over_c, &again);
  bool same = again.size() == out.size();
  for (size_t i = 0; same && i < out.size(); ++i) same = again[i].id == out[i].id;
  Check(same, "the same lamps in the same order every time");

  // the A/B's own path
  out.clear();
  lp::HandedOverLamps(lp::Ab::kPass, a.get(), over_a, &out);
  Check(out.size() == 2 && (out[0].flags & kLightFlagGameLit) == 0 && (out[0].roles & kRoleMaskSurface) != 0, "the A/B's pass mode hands over the active set's lamps, allowed to add surface light");
  out.clear();
  lp::HandedOverLamps(lp::Ab::kGrid, a.get(), over_a, &out);
  lp::HandedOverLamps(lp::Ab::kNowhere, a.get(), over_a, &out);
  lp::HandedOverLamps(lp::Ab::kPass, nullptr, over_a, &out);
  Check(out.empty(), "the grid and control modes hand over nothing, nor does a camera that is not over a mod");
  // the same lamp is the one the manager's source makes
  std::vector<Light> manager;
  mls::Candidates(*a, over_a, 8, &manager);
  std::vector<Light> handed;
  lp::HandedOverLamps(lp::Ab::kPass, a.get(), over_a, &handed);
  Check(manager.size() == handed.size() && handed.size() == 2 && handed[0].id == manager[0].id && handed[0].range == manager[0].range && handed[0].color[0] == manager[0].color[0],
        "the A/B's lamp is the manager's candidate, nothing but the permission changed");

  // the feed
  mls::Feed feed;
  Check(feed.Built() == nullptr, "a new feed has no built list");
  feed.PublishBuilt({a, b});
  const mls::BuiltSets built = feed.Built();
  Check(built && built->size() == 2 && (*built)[0] == a && (*built)[1] == b, "the built sets come back as the objects that were published");
  Check(feed.Current() == nullptr, "the set the camera is over is separate");
  feed.Publish(b);
  Check(feed.Current() == b && feed.Built() == built, "publishing the active set leaves the built list alone, and the active set is one of the list's objects");
  Check(feed.Current().get() == (*feed.Built())[1].get(), "a reader tells the camera's set by pointer");
  feed.PublishBuilt({});
  Check(feed.Built() && feed.Built()->empty() && built->size() == 2, "an empty list replaces it; a reader that holds the old one keeps it");
  feed.Publish(nullptr);
  Check(feed.Current() == nullptr, "back over the city");
}

// ---------------------------------------------------------------------------
// the selection
// ---------------------------------------------------------------------------

Light Dummy(LightSource source, uint64_t id, float x) {
  const float pos[3] = {x, 1.0f, 0.0f};
  const float white[3] = {1.0f, 1.0f, 1.0f};
  Light l = MakeOmniLight(source, pos, white, 3.0f, 3.0f, 10.0f, id);
  l.roles = kRoleMaskSurface;
  return l;
}

Light FrameHeadlight(uint64_t id) {
  const float pos[3] = {0.0f, 1.0f, 0.0f};
  Light l = RigLightAt(pos, id);
  return l;
}

lp::Candidates ManyCandidates(uint32_t n) {
  lp::Candidates c;
  for (uint32_t i = 0; i < n; ++i) {
    c.police.push_back(Dummy(LightSource::kPolice, 1000 + i, float(i)));
    c.head.push_back(Dummy(LightSource::kHeadlight, 2000 + i, float(i)));
    c.rear.push_back(Dummy(LightSource::kOther, 3000 + i, float(i)));
    c.mod.push_back(Dummy(LightSource::kMod, 4000 + i, float(i)));
  }
  return c;
}

uint32_t CountIds(const lp::Selection& s, uint64_t lo, uint64_t hi) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < s.count; ++i) n += (s.lights[i].id >= lo && s.lights[i].id < hi) ? 1u : 0u;
  return n;
}

void SelectionChecks() {
  LightFrame frame;
  frame.lights[0] = FrameHeadlight(101);
  frame.lights[1] = FrameHeadlight(102);
  frame.count = 2;
  const std::vector<Light> extras = {Dummy(LightSource::kMod, 7001, 0.0f), Dummy(LightSource::kOther, 7002, 1.0f), Dummy(LightSource::kOther, 7003, 2.0f)};
  const lp::Candidates none;

  // no candidates: SelectLights, for every mask
  bool identical = true;
  for (uint32_t mask = 0; mask <= lp::kSourceMask; ++mask) {
    const lp::Selection base = lp::SelectLights(frame, mask, extras.data(), uint32_t(extras.size()));
    const lp::Selection s = lp::SelectPassLights(frame, mask, extras.data(), uint32_t(extras.size()), none, lp::Tier::kUltra);
    bool same = s.count == base.count && s.from_frame == base.from_frame && s.from_extra == base.from_extra && s.dropped == base.dropped;
    for (uint32_t i = 0; same && i < s.count; ++i) same = s.lights[i].id == base.lights[i].id;
    identical = identical && same && s.cut_class[2] == 0 && s.from_class[2] == 0;
  }
  Check(identical, "with no candidates the selection is GB-H1's SelectLights, for all 64 masks: the lamp A/B and the test lamp are unchanged");

  // the budgets per tier, with all four classes asked for and twenty lights in each
  const lp::Candidates big = ManyCandidates(20);
  const uint32_t all = lp::kSourcePolice | lp::kSourceHeadLamps | lp::kSourceRearLamps | lp::kSourceMod;
  for (lp::Tier tier : {lp::Tier::kMedium, lp::Tier::kHigh}) {
    const lp::ClassBudget b = lp::BudgetOf(tier);
    const lp::Selection s = lp::SelectPassLights(LightFrame{}, all, nullptr, 0, big, tier);
    Check(CountIds(s, 1000, 2000) == b.police && CountIds(s, 2000, 3000) == b.head && CountIds(s, 3000, 4000) == b.rear && CountIds(s, 4000, 5000) == b.mod,
          std::string("the ") + lp::TierName(tier) + " tier takes each class up to its budget");
    Check(s.count == b.police + b.head + b.rear + b.mod && s.dropped == 0, "... and they fit the 32");
    Check(s.from_class[size_t(lp::SourceClass::kPolice)] == b.police && s.cut_class[size_t(lp::SourceClass::kPolice)] == 20 - b.police && s.cut_class[size_t(lp::SourceClass::kRearLamps)] == 20 - b.rear,
          "the lights over a class's budget are counted as cut");
  }
  {
    // ultra asks for 48: the priority police, mods, head lamps, rear lamps fills the 32
    const lp::ClassBudget b = lp::BudgetOf(lp::Tier::kUltra);
    const lp::Selection s = lp::SelectPassLights(LightFrame{}, all, nullptr, 0, big, lp::Tier::kUltra);
    Check(s.count == lp::kMaxPassLights, "ultra's budgets add to more than 32: the pass is full");
    Check(CountIds(s, 1000, 2000) == b.police && CountIds(s, 4000, 5000) == b.mod, "police and mods come first and get all of theirs");
    Check(CountIds(s, 2000, 3000) == lp::kMaxPassLights - b.police - b.mod && CountIds(s, 3000, 4000) == 0, "the head lamps take what is left and the rear lamps none");
    Check(s.dropped == (20 - (lp::kMaxPassLights - b.police - b.mod)) + 20, "the head lamps the 32 cut and all the rear lamps are counted in `dropped` (the lights over a class's own budget only in cut_class)");
    // the order in the list: police, mods, head lamps
    uint32_t last_class = 0;
    bool ordered = true;
    for (uint32_t i = 0; i < s.count; ++i) {
      const uint64_t id = s.lights[i].id;
      const uint32_t cls = id < 2000 ? 0u : (id < 3000 ? 2u : (id < 4000 ? 3u : 1u));  // police 0, mods 1, head 2, rear 3
      ordered = ordered && cls >= last_class;
      last_class = cls;
    }
    Check(ordered, "the lights are in the priority's order: police, mods, head lamps, rear lamps");
    // within a class the list's own order (best first) is kept
    bool within = true;
    for (uint32_t i = 1; i < s.count; ++i) {
      if (s.lights[i].id / 1000 == s.lights[i - 1].id / 1000) within = within && s.lights[i].id == s.lights[i - 1].id + 1;
    }
    Check(within, "within a class the best come first: the ranking is kept");
  }
  // the frame's and the handed over lights first, and they take room from the classes
  {
    const lp::Selection s = lp::SelectPassLights(frame, all | lp::kSourceFrame, extras.data(), uint32_t(extras.size()), big, lp::Tier::kUltra);
    Check(s.count == lp::kMaxPassLights && s.from_frame == 2 && s.from_extra == 3, "the frame's two and the three handed over lights are in");
    Check(s.lights[0].id == 101 && s.lights[2].id == 7001 && s.lights[5].id == 1000, "in the order: the frame's, the handed over, then the classes");
    Check(s.from_class[size_t(lp::SourceClass::kFrame)] == 2 && s.from_class[size_t(lp::SourceClass::kHandedOver)] == 3, "the class counts say so");
    const lp::ClassBudget b = lp::BudgetOf(lp::Tier::kUltra);
    Check(CountIds(s, 1000, 2000) == b.police && CountIds(s, 4000, 5000) == lp::kMaxPassLights - 5 - b.police && CountIds(s, 2000, 3000) == 0 && CountIds(s, 3000, 4000) == 0,
          "the classes get what the 32 leave: police all, mods the 11 that are left, head and rear lamps none");
  }
  // the bits gate the classes
  {
    const lp::Selection s = lp::SelectPassLights(LightFrame{}, lp::kSourcePolice, nullptr, 0, big, lp::Tier::kHigh);
    Check(s.count == lp::BudgetOf(lp::Tier::kHigh).police && CountIds(s, 1000, 2000) == s.count, "only the police bit: only police lights");
    Check(s.cut_class[size_t(lp::SourceClass::kMod)] == 0 && s.cut_class[size_t(lp::SourceClass::kHeadLamps)] == 0, "a class not asked for is not even counted as cut");
    const lp::Selection t = lp::SelectPassLights(LightFrame{}, lp::kSourceFrame | lp::kSourceTest, nullptr, 0, big, lp::Tier::kHigh);
    Check(t.count == 0, "the frame and test bits take none of the classes' lights");
    const lp::Selection m = lp::SelectPassLights(LightFrame{}, lp::kSourceMod | lp::kSourceRearLamps, nullptr, 0, big, lp::Tier::kMedium);
    Check(m.count == lp::BudgetOf(lp::Tier::kMedium).mod + lp::BudgetOf(lp::Tier::kMedium).rear && CountIds(m, 4000, 5000) == lp::BudgetOf(lp::Tier::kMedium).mod, "mods and rear lamps: their budgets");
  }
  // few candidates: all taken
  {
    const lp::Candidates few = ManyCandidates(2);
    const lp::Selection s = lp::SelectPassLights(LightFrame{}, all, nullptr, 0, few, lp::Tier::kUltra);
    Check(s.count == 8 && s.dropped == 0 && s.cut_class[2] == 0, "fewer lights than the budget: all of them");
  }
  // an over-full frame of handed over lights leaves no room: the classes are dropped, counted
  {
    std::vector<Light> thirty_two;
    for (uint32_t i = 0; i < 32; ++i) thirty_two.push_back(Dummy(LightSource::kOther, 8000 + i, float(i)));
    const lp::Selection s = lp::SelectPassLights(LightFrame{}, lp::kSourcePolice, thirty_two.data(), 32, big, lp::Tier::kHigh);
    Check(s.count == 32 && CountIds(s, 1000, 2000) == 0 && s.dropped == 20 && s.cut_class[size_t(lp::SourceClass::kPolice)] == 20,
          "32 handed over lights fill the pass: the classes get nothing and the cut is counted");
  }
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what) { return text.find(what); }

void SourceChecks(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string cpp = ReadFile(gpu + "local_lights.cpp");
  const std::string header = ReadFile(gpu + "local_lights.h");
  const std::string manager_h = ReadFile(gpu + "light_manager.h");
  const std::string manager_cpp = ReadFile(gpu + "light_manager.cpp");
  const std::string cmake = ReadFile(gpu + "CMakeLists.txt");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string mod = ReadFile(src + "/mc_engine/modloader/features/mod_lights.cpp");
  Check(!cpp.empty() && !manager_h.empty() && !settings.empty() && !mod.empty() && !cmake.empty(), "the sources are readable: " + src);

  // the settings: the sources' range widened to the six bits, the cars' gain defined in all three files, debug only
  Check(Has(settings, "rex::kDebugOnly, rex::Range{0, 63});") && !Has(settings, "mcla_gpu_local_lights_sources, 0, \"Diagnostic (the deferred local light pass, GB-H1): which"), "the sources' range is 0 to 63");
  Check(Has(manifest, "\"rex::Range{0, 63}\""), "manifest.json has the range");
  Check(Has(settings, "f64, mcla_gpu_local_lights_car_gain, 1.0,") && Has(settings, "inline auto& mcla_gpu_local_lights_car_gain = g_gpu.mcla_gpu_local_lights_car_gain;") &&
            Has(manifest, "\"mcla_gpu_local_lights_car_gain\": {") && Has(meta, "{\"mcla_gpu_local_lights_car_gain\", \"MCLA/GPU\"},"),
        "the cars' gain is defined in the three generated files, default 1");
  {
    const size_t at = settings.find(", mcla_gpu_local_lights_car_gain, ");
    const size_t end = settings.find(");", at);
    Check(at != std::string::npos && settings.substr(at, end - at).find("rex::kDebugOnly") != std::string::npos, "the cars' gain is debug only");
  }
  Check(Has(settings, "4 the police light bars") && Has(settings, "8 the headlights of the cars the forward headlight stage does not light") && Has(settings, "32 the lamps of a mod's lights.bin"),
        "the sources' help names the new bits");

  // the manager: the cars and the reference
  Check(Has(manager_h, "const larecomp::gpu::car_lights::CarLightsFrame& Cars() const { return cars_.Frame(); }") && Has(manager_h, "void SetCarReference(const double xyz[3])"), "the manager hands out its cars and takes a reference");
  Check(Has(manager_cpp, "cars_.Poll(car_frames_, have_car_reference_ ? car_reference_ : nullptr)") && Has(manager_cpp, "have_car_reference_ = false;"), "the poll ranks the cars by the reference, which the manager forgets with the rest");

  // the mod loader: the built sets are published, the active one is among them
  Check(Has(mod, "PublishBuilt({})") && Has(mod, "PublishBuilt(std::move(built_sets))") && Has(mod, "built_sets.push_back(grid.published)"), "mod_lights.cpp publishes every set it built, and none before");
  Check(At(mod, "PublishBuilt({})") < At(mod, "if (g_grids.empty() || !larecomp::settings::mod_lights.Get()) return;"), "... the empty list before the early return, so a setting that is off leaves none");

  // the pass's wiring
  Check(Has(cpp, "manager_->Want(planes_wanted_ && (sources_ & (ll::kSourceFrame | ll::kSourceCars)) != 0);"), "the manager runs for the cars' sources too");
  Check(Has(cpp, "sources_ = hp::SourcesFor(route, uint32_t(std::clamp<int>(int(st::mcla_gpu_local_lights_sources.Get()), 0, int(ll::kSourceMask))) & ll::kSourceMask);"), "the setting is read as the six bits (the clamp is not GB-H1's 0..3), and the headlight route adds the frame's source");
  Check(Has(cpp, "manager_->SetCarReference(reference);") && Has(cpp, "(sources_ & ll::kSourceCars) != 0"), "the pass gives the manager the camera for the cars' sources");
  Check(Has(cpp, "ll::GatherCarLights(cars_in, &candidates_);") && Has(cpp, "ll::GatherModLamps(mls::Global().Built(), active_mod.get(), ab_, rows.position, &candidates_.mod);") &&
            Has(cpp, "ll::HandedOverLamps(ab_, active_mod.get(), rows.position, &extras_);") && Has(cpp, "ll::SelectPassLights(frame, sources_, extras_.data(), uint32_t(extras_.size()), candidates_, tier_);"),
        "the pass gathers the cars' lights, the mods' lamps and the A/B's, and selects with the budgets");
  Check(Has(cpp, "larecomp::settings::mod_lights.Get()") && Has(cpp, "mcla_gpu_local_lights_car_gain.Get()"), "the mod lamps respect the mod lights setting, the cars' look its gain");
  Check(Has(cpp, "std::erase_if(candidates_.police, unseen);") && Has(cpp, "std::erase_if(candidates_.rear, unseen);") && Has(cpp, "std::erase_if(candidates_.head, unseen);") && Has(cpp, "std::erase_if(candidates_.mod, unseen);"),
        "the lights that cannot be seen are left out of each class before the budgets");
  {
    const size_t gather = At(cpp, "ll::GatherCarLights(cars_in, &candidates_);");
    const size_t unseen = At(cpp, "std::erase_if(candidates_.police, unseen);");
    const size_t select = At(cpp, "ll::SelectPassLights(frame,");
    Check(gather != std::string::npos && gather < unseen && unseen < select, "gather, then drop the unseen, then select");
  }
  Check(Has(cpp, "static LiveText text(\"mcla_gpu_light_shadows\");") && Has(cpp, "ll::TierOfWord(local_shadow::LevelName(choice.level))"), "the tier is the light shadows' level word");
  Check(Has(header, "#include \"local_lights_sources.h\"") && Has(header, "Candidates candidates_;"), "the class holds the candidates");
  Check(Has(cmake, "add_executable(larecomp_local_lights_sources_test EXCLUDE_FROM_ALL local_lights_sources_test.cpp)"), "the CMake has this test");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    std::string here = __FILE__;
    for (char& c : here) {
      if (c == '\\') c = '/';
    }
    const size_t slash = here.rfind('/');
    src = slash == std::string::npos ? std::string(".") : here.substr(0, slash) + "/..";
  }
  static Probe probe;
  FillProbe(&probe);
  Check(probe.cars.count == 6 && probe.player != nullptr && probe.racer_on != nullptr && probe.racer_off != nullptr && probe.traffic[0] != nullptr && probe.traffic[1] != nullptr && probe.traffic[2] != nullptr,
        "the probe's six cars: the player, two racers, three traffic cars");
  TierChecks();
  ScoreChecks();
  PoliceChecks(probe);
  RearChecks(probe);
  HeadChecks(probe);
  GatherCarChecks(probe);
  ModChecks();
  SelectionChecks();
  SourceChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
