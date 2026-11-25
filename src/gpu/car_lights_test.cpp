// Check of the game's vehicle light points as the light manager's sources read them (light_points.h, car_lights.h, mod_light_source.h; perf/LIGHT-UNIFY.md package LU-7 = step U7): the kinds and
// their families, the hook's decoder on a made-up guest object, the table (keyed by the light object, the peak of a few milliseconds), the headlight rig's own table contract, THE NIGHT
// PROBE'S RECORDED POINTS (the player, two racers and three traffic cars; ports/run/nprobe1-night, perf/out/NIGHT-PROBE.txt C12 and C13) made into cars (the frame, the sides, the
// emitted direction, the bits and levels, the ranking and cap), the lights the lamps make, the census line, and a mod's lamps as candidates. A program of its own: no renderer, no
// ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_lights_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_lights_test

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "car_lights.h"
#include "headlight_points.h"
#include "mod_light_source.h"

namespace {

using namespace larecomp::gpu;
namespace lp = larecomp::gpu::light_points;
namespace cl = larecomp::gpu::car_lights;
namespace mls = larecomp::gpu::mod_light_source;
namespace hp = mcla::gpu::headlight_points;
using Clock = lp::Table::Clock;
using ms = std::chrono::milliseconds;

int g_checks = 0, g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}
void CheckNear(double a, double b, double tol, const std::string& what) {
  ++g_checks;
  if (!(std::fabs(a - b) <= tol)) {
    ++g_failed;
    std::printf("FAIL: %s: %.6f vs %.6f (tolerance %g)\n", what.c_str(), a, b, tol);
  }
}

// ---------------------------------------------------------------------------
// the probe's recorded points: `who vehicle kind index intensity x y z [dx dy dz | -]`, the lines of the night probe's `light point:` records in the order the guest submitted them
// (one light object after another, its sets by kind). Frame 3603: a racer whose lights are off and the player; frame 13623: a racer with its lights on; frame 122: three traffic cars (no
// owner, no direction).
// ---------------------------------------------------------------------------

const char* const kRecorded_frame_3603 = R"(
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

const char* const kRecorded_frame_13623 = R"(
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

const char* const kRecorded_frame_122 = R"(
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

// The lines as Points: a new light object starts where the kind goes back (the guest submits an object's sets in kind order). Objects get addresses from `object_base`; a car with a
// vehicle address is owned, one with 0 is not; a `-` direction means none was recorded.
std::vector<lp::Point> Parse(const char* text, uint32_t object_base) {
  std::vector<lp::Point> out;
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
    lp::Point p;
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

std::vector<lp::Point> Concat(std::initializer_list<std::vector<lp::Point>> parts) {
  std::vector<lp::Point> all;
  for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end());
  return all;
}

const cl::Car* CarOfObject(const cl::CarLightsFrame& f, uint32_t object) {
  for (uint32_t i = 0; i < f.count; ++i) {
    if (f.cars[i].object == object) return &f.cars[i];
  }
  return nullptr;
}
const cl::Lamp* LampOf(const cl::Car& car, uint32_t kind, uint32_t index) {
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    if (car.lamps[i].kind == kind && car.lamps[i].index == index) return &car.lamps[i];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// the kinds, the thresholds
// ---------------------------------------------------------------------------

void KindChecks() {
  Check(lp::kKindCount == 19 && lp::kAllKinds == 0x7FFFFu, "sub_8237AC90's object has 19 sets");
  Check(std::string(lp::KindName(3)) == "Headlight" && std::string(lp::KindName(0)) == "Brakelight" && std::string(lp::KindName(12)) == "None" &&
            std::string(lp::KindName(18)) == "Kind18" && std::string(lp::KindName(19)) == "unnamed",
        "the kind names (the probe's string table order)");
  using F = lp::Family;
  const F expect[lp::kKindCount] = {F::kBrake, F::kTail,  F::kBrake, F::kHead,   F::kHiBeam, F::kFog,   F::kTurn,   F::kPolice, F::kPolice, F::kPolice,
                                    F::kOther, F::kReverse, F::kOther, F::kTurn, F::kTurn,   F::kTurn,  F::kTurn,   F::kOther,  F::kOther};
  bool all = true;
  for (uint32_t k = 0; k < lp::kKindCount; ++k) all = all && lp::FamilyOf(k) == expect[k];
  Check(all, "every kind's family: both brake kinds, the five turn kinds, the three police kinds");
  Check(lp::FamilyOf(40) == F::kOther, "a kind past the table is no family");
  Check(std::string(lp::FamilyName(F::kPolice)) == "police" && std::string(lp::FamilyName(F::kOther)) == "other", "family names");

  const uint32_t object = 0xBD000000u;
  uint32_t kind = 99;
  Check(lp::KindOfSlot(object, object + 16 + 4 * 3, &kind) && kind == 3, "the set pointer of kind 3 is object + 28");
  Check(lp::KindOfSlot(object, object + 16, &kind) && kind == 0 && lp::KindOfSlot(object, object + 16 + 4 * 18, &kind) && kind == 18, "kinds 0 and 18 are the object's first and last");
  Check(!lp::KindOfSlot(object, object + 16 + 4 * 19, &kind), "the 20th set is not a kind");
  Check(!lp::KindOfSlot(object, object + 18, &kind) && !lp::KindOfSlot(object, object + 12, &kind) && !lp::KindOfSlot(object, object - 4, &kind), "a misaligned slot, one below the sets, one before the object");

  Check(lp::FullIntensity(F::kHead) == 2.0f && lp::FullIntensity(F::kHiBeam) == 2.0f && lp::FullIntensity(F::kBrake) == 1.0f && lp::FullIntensity(F::kPolice) == 1.0f,
        "the headlights read 2.0 when on, every other family about 1");
  Check(lp::Level(F::kHead, 2.0f) == 1.0f && lp::Level(F::kHead, 3.4f) == 1.0f && lp::Level(F::kBrake, 0.5f) == 0.5f && lp::Level(F::kBrake, -1.0f) == 0.0f,
        "a level is the intensity over the family's full one, capped (a traffic car's 3.4 is full)");
  Check(lp::kOffIntensity == hp::kOffIntensity && lp::kKindHeadlight == hp::kHeadlightKind && lp::kPlayerVehiclePtr == 0x8288DCF8u, "the numbers are headlight_points.h's");
}

// ---------------------------------------------------------------------------
// who wants what
// ---------------------------------------------------------------------------

void WantedChecks() {
  for (const auto who : {lp::Consumer::kManager, lp::Consumer::kRt, lp::Consumer::kDebug}) lp::Want(who, 0);
  Check(!lp::AnyWanted() && !lp::Wanted(3), "nobody asks: the hook is one load");
  lp::Want(lp::Consumer::kManager, 1u << 3);
  Check(lp::AnyWanted() && lp::Wanted(3) && !lp::Wanted(0) && !lp::Wanted(19), "one consumer, one kind");
  lp::Want(lp::Consumer::kRt, 1u << 0);
  Check(lp::Wanted(3) && lp::Wanted(0) && !lp::Wanted(1), "two consumers: the union");
  lp::Want(lp::Consumer::kManager, 0);
  Check(!lp::Wanted(3) && lp::Wanted(0), "one stops asking and does not switch the other off");
  lp::Want(lp::Consumer::kDebug, 0xFFFFFFFFu);
  Check(lp::Wanted(0) && lp::Wanted(18) && !lp::Wanted(19) && !lp::Wanted(31), "all bits asked: the 19 kinds, never a 20th");
  for (const auto who : {lp::Consumer::kManager, lp::Consumer::kRt, lp::Consumer::kDebug}) lp::Want(who, 0);
  Check(!lp::AnyWanted(), "all off again");
}

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

lp::Point PointOf(uint32_t object, uint32_t kind, uint32_t index, float intensity, float x = 0.0f) {
  lp::Point p;
  p.object = object;
  p.kind = kind;
  p.index = index;
  p.intensity = p.peak = intensity;
  p.pos[0] = x;
  return p;
}

void TableChecks() {
  lp::Table table;
  const auto t0 = Clock::now();
  std::vector<lp::Point> out;
  table.Note(PointOf(0x1000, 3, 0, 2.0f), t0);
  table.Note(PointOf(0x1000, 3, 1, 2.0f), t0);
  table.Note(PointOf(0x1000, 4, 0, 0.0f), t0);
  table.Note(PointOf(0x2000, 3, 0, 1.0f), t0);
  table.Recent(&out, ms(100), t0 + ms(10));
  Check(out.size() == 4, "a point is its object, kind and index: same index of another kind or another car is another point");
  Check(std::all_of(out.begin(), out.end(), [](const lp::Point& p) { return std::fabs(p.age_ms - 10.0f) < 0.5f; }), "Recent says how old each is");

  // a moving car with no owner: the same lamp every frame, 20 places. headlight_points.h keys such a point by place and makes a new key per quarter metre.
  table.Clear();
  for (int i = 0; i < 20; ++i) table.Note(PointOf(0x3000, 0, 0, 0.9f, float(i) * 0.4f), t0 + ms(i));
  table.Recent(&out, ms(1000), t0 + ms(30));
  Check(out.size() == 1 && std::fabs(out[0].pos[0] - 19 * 0.4f) < 1e-4f, "a moving traffic car's lamp is one entry, the latest place");
  hp::Table old_table;
  for (int i = 0; i < 20; ++i) {
    hp::Point q;
    q.pos[0] = float(i) * 0.4f;
    q.intensity = 0.9f;
    old_table.Note(q, t0 + ms(i));
  }
  std::vector<hp::Point> old_out;
  old_table.Recent(&old_out, ms(1000), t0 + ms(30));
  Check(old_out.size() > 1, "(the rig's table makes a new entry per quarter metre of a car with no owner: why this one is keyed by the object)");

  // the peak of a few milliseconds
  table.Clear();
  table.Note(PointOf(0x4000, 3, 0, 3.1f), t0);
  table.Note(PointOf(0x4000, 3, 0, 0.23f), t0 + ms(16));
  table.Recent(&out, ms(100), t0 + ms(20));
  Check(out.size() == 1 && out[0].intensity == 0.23f && out[0].peak == 3.1f, "two submissions in a frame: the latest and the peak are both kept");
  table.Note(PointOf(0x4000, 3, 0, 0.5f), t0 + ms(80));
  table.Recent(&out, ms(100), t0 + ms(81));
  Check(out.size() == 1 && out[0].peak == 0.5f, "the peak window is 50 ms: an old peak does not stay");
  table.Note(PointOf(0x4000, 3, 0, 0.0f), t0 + ms(90));
  table.Recent(&out, ms(100), t0 + ms(91));
  Check(out[0].peak == 0.5f && out[0].intensity == 0.0f, "a lamp that goes out still reads its peak for the window (about three frames)");

  // ageing and a full table
  table.Recent(&out, ms(100), t0 + ms(400));
  Check(out.empty(), "a point not seen for the age limit is gone");
  table.Clear();
  for (uint32_t i = 0; i < lp::Table::kCapacity * 3 / 2; ++i) table.Note(PointOf(0x10000 + i * 0x400, 3, 0, 1.0f), t0 + ms(i % 1000));
  table.Recent(&out, ms(100000), t0 + ms(2000));
  Check(out.size() <= lp::Table::kCapacity && out.size() > lp::Table::kCapacity * 3 / 4, "a table filled one and a half times over stays at its capacity, nearly full");
  table.Note(PointOf(0xABCDE000, 0, 0, 1.0f), t0 + ms(1500));
  table.Recent(&out, ms(100000), t0 + ms(2000));
  Check(std::any_of(out.begin(), out.end(), [](const lp::Point& p) { return p.object == 0xABCDE000u; }), "the newest point is always in");
  Check(table.notes() == lp::Table::kCapacity * 3 / 2 + 1, "the notes are counted");
}

// ---------------------------------------------------------------------------
// the hook's decoder on a made-up guest object
// ---------------------------------------------------------------------------

struct FakeMem {
  std::map<uint32_t, uint32_t> words;
  std::map<uint32_t, float> floats;
  uint32_t U32(uint32_t a) const {
    const auto it = words.find(a);
    return it == words.end() ? 0u : it->second;
  }
  float F32(uint32_t a) const {
    const auto it = floats.find(a);
    return it == floats.end() ? 0.0f : it->second;
  }
  void Vec(uint32_t a, float x, float y, float z) {
    floats[a] = x;
    floats[a + 4] = y;
    floats[a + 8] = z;
  }
};

constexpr uint32_t kObject = 0xBD039000u, kOwner = 0xBD100000u, kVehicle = 0xBF4E7380u, kSet = 0xBD200000u, kArray = 0xBD210000u, kPoint0 = 0xBD220000u;
constexpr uint32_t kTableFloat = 0x827E7688u, kValueFloat = 0xBD039400u;

// An owned object with kind 3's set holding two points as the probe read the player's pair.
FakeMem MakeOwnedObject() {
  FakeMem m;
  m.words[kObject] = kOwner;
  m.words[kOwner + 12] = kVehicle + 16;
  m.words[kObject + 16 + 4 * 3] = kSet;
  m.words[kSet + 20] = kArray;
  m.words[kArray + 0] = kPoint0;
  m.words[kArray + 4] = kPoint0 + 0x100;
  m.words[0x8288DCF8u] = kVehicle;
  const float pos[2][3] = {{819.23f, 18.09f, -806.40f}, {818.19f, 18.14f, -806.96f}};
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t p = kPoint0 + i * 0x100;
    m.Vec(p + 0, -0.88f, 0.0f, -0.47f);  // right
    m.Vec(p + 16, 0.0f, 1.0f, 0.0f);     // up
    m.Vec(p + 32, 0.47f, 0.04f, -0.88f);  // direction
    m.Vec(p + 48, pos[i][0], pos[i][1], pos[i][2]);
  }
  m.floats[kTableFloat] = 1.0f;
  m.floats[kValueFloat] = 2.0f;
  return m;
}

lp::Regs RegsOf(uint32_t kind, uint32_t index) {
  lp::Regs r;
  r.object = kObject;
  r.slot = kObject + 16 + 4 * kind;
  r.index_x4 = 4 * index;
  r.pos_ptr = 0;
  r.table_ptr = kTableFloat;
  r.value_ptr = kValueFloat;
  return r;
}

void ReadPointChecks() {
  FakeMem owned = MakeOwnedObject();
  lp::Point p;
  Check(lp::ReadPoint(owned, RegsOf(3, 1), kVehicle, &p), "an owned point decodes");
  Check(p.kind == 3 && p.index == 1 && p.object == kObject && p.vehicle == kVehicle && p.owned && p.player && p.has_dir, "its kind, index, object, vehicle, owner and the player's flag");
  CheckNear(p.pos[0], 818.19, 1e-3, "the position (+48), x");
  CheckNear(p.pos[2], -806.96, 1e-3, "... z");
  CheckNear(p.dir[0], 0.47, 1e-6, "the direction (+32), x");
  CheckNear(p.dir[2], -0.88, 1e-6, "... z");
  CheckNear(p.right[0], -0.88, 1e-6, "the right row (+0)");
  CheckNear(p.up[1], 1.0, 1e-6, "the up row (+16)");
  CheckNear(p.intensity, 2.0, 1e-6, "the intensity is the two floats' product");
  Check(p.peak == p.intensity, "the first reading is its own peak");
  lp::Point q;
  Check(lp::ReadPoint(owned, RegsOf(3, 0), 0xBF000000u, &q) && !q.player, "another vehicle is the player's: no");
  FakeMem no_link = owned;
  no_link.words[kOwner + 12] = 0;
  Check(lp::ReadPoint(no_link, RegsOf(3, 0), kVehicle, &q) && q.vehicle == 0 && q.owned && !q.player, "an owner with no vehicle link is owned, vehicle 0");

  // a car with no owner: the matrix sub_8237A698 made, +48 is r30
  FakeMem traffic;
  constexpr uint32_t kTrafficObject = 0xBD500000u, kBuffer = 0xBC001000u;
  traffic.words[kTrafficObject] = 0;
  traffic.Vec(kBuffer + 0, 1.0f, 0.0f, 0.0f);
  traffic.Vec(kBuffer + 16, 0.0f, 1.0f, 0.0f);
  traffic.Vec(kBuffer + 32, 0.0f, 0.0f, -1.0f);
  traffic.Vec(kBuffer + 48, -2970.5f, -23.96f, 433.7f);
  traffic.floats[kTableFloat] = 1.0f;
  traffic.floats[kValueFloat] = 0.84f;
  lp::Regs r;
  r.object = kTrafficObject;
  r.slot = kTrafficObject + 16;
  r.index_x4 = 0;
  r.pos_ptr = kBuffer + 48;
  r.table_ptr = kTableFloat;
  r.value_ptr = kValueFloat;
  Check(lp::ReadPoint(traffic, r, kVehicle, &q), "a point with no owner decodes");
  Check(q.kind == 0 && q.vehicle == 0 && !q.owned && !q.player && q.object == kTrafficObject, "kind 0 of a car with no owner");
  CheckNear(q.pos[0], -2970.5, 1e-3, "the position is at r30");
  CheckNear(q.pos[2], 433.7, 1e-3, "... z");
  CheckNear(q.intensity, 0.84, 1e-6, "its intensity");
  Check(q.has_dir && std::fabs(q.dir[2] + 1.0f) < 1e-6f && std::fabs(q.up[1] - 1.0f) < 1e-6f && std::fabs(q.right[0] - 1.0f) < 1e-6f,
        "the row below the position is read as the direction [I], the two before it as up and right");

  // nothing implausible is read
  FakeMem broken = owned;
  broken.words[kSet + 20] = 0;
  Check(!lp::ReadPoint(broken, RegsOf(3, 0), kVehicle, &q), "a set with no array of points is no point");
  broken = owned;
  broken.words[kArray] = 0x100;
  Check(!lp::ReadPoint(broken, RegsOf(3, 0), kVehicle, &q), "a point pointer below 0x10000 is not read");
  broken = owned;
  broken.words[kObject + 16 + 4 * 3] = 0;
  Check(!lp::ReadPoint(broken, RegsOf(3, 0), kVehicle, &q), "an empty set slot is not read");
  lp::Regs wrong = RegsOf(3, 0);
  wrong.slot = kObject + 16 + 4 * 20;
  Check(!lp::ReadPoint(owned, wrong, kVehicle, &q), "a slot that is not one of the 19 is not read");
  wrong = RegsOf(3, 0);
  wrong.table_ptr = 0;
  Check(!lp::ReadPoint(owned, wrong, kVehicle, &q), "no intensity pointer, no point");
  wrong = RegsOf(3, 0);
  wrong.object = 0;
  Check(!lp::ReadPoint(owned, wrong, kVehicle, &q), "no object, no point");
}

// The rig's own table must get exactly what the old hook gave it: the Headlight points, an owned point with its direction, one with no owner with its place only.
void RigContractChecks() {
  FakeMem owned = MakeOwnedObject();
  lp::Point p;
  Check(lp::ReadPoint(owned, RegsOf(3, 0), kVehicle, &p), "decode for the rig");
  const hp::Point h = lp::ToHeadlightPoint<hp::Point>(p);
  Check(h.has_dir && h.player && h.vehicle == kVehicle && h.index == 0 && h.intensity == p.intensity, "an owned point: direction, player, vehicle, index and intensity as the old hook made them");
  CheckNear(h.pos[0], 819.23, 1e-3, "... the place");
  CheckNear(h.dir[2], -0.88, 1e-6, "... and the back axis");
  lp::Point traffic;
  traffic.pos[0] = 10.0f;
  traffic.dir[2] = -1.0f;  // read by the inference: not the rig's to see
  traffic.has_dir = true;
  traffic.owned = false;
  traffic.intensity = 3.4f;
  const hp::Point t = lp::ToHeadlightPoint<hp::Point>(traffic);
  Check(!t.has_dir && t.dir[0] == 0.0f && t.dir[1] == 0.0f && t.dir[2] == 0.0f && !t.player && t.vehicle == 0 && t.pos[0] == 10.0f && t.intensity == 3.4f,
        "a point with no owner: its place and state only, no direction, as the rig has always had it");
}

// ---------------------------------------------------------------------------
// the recorded points as cars
// ---------------------------------------------------------------------------

void RecordedPlayerAndRacers() {
  const auto racer_off_and_player = Parse(kRecorded_frame_3603, 0xBF100000u);
  const auto racer_lit = Parse(kRecorded_frame_13623, 0xBF200000u);
  Check(racer_off_and_player.size() == 22 && racer_lit.size() == 11, "the recorded lines parse (22 and 11 points)");
  const uint32_t racer_off_object = 0xBF100000u, player_object = 0xBF100400u, racer_lit_object = 0xBF200000u;
  Check(racer_off_and_player[0].object == racer_off_object && racer_off_and_player[11].object == player_object && racer_off_and_player[11].player, "the objects split where the kinds restart");

  const double camera[3] = {820.0, 18.0, -808.0};
  cl::CarLightsFrame f;
  cl::Build(Concat({racer_off_and_player, racer_lit}), {}, camera, &f);
  Check(f.count == 3 && f.points_in == 33 && f.cars_seen == 3 && f.cars_dropped == 0 && f.frames_matched == 0, "three cars from 33 points, no renderer frames");
  Check(f.cars[0].player && f.cars[0].object == player_object, "the player's car is first");

  const cl::Car& player = f.cars[0];
  Check(player.owned && player.vehicle == kPlayerVehicle && player.lamp_count == 11 && player.lights.vehicle == kPlayerVehicle, "the player's: owned, its vehicle address, 11 points");
  Check((player.lights.car_key & 0x80000000u) != 0 && player.lights.car_key == cl::PointCarKey(kPlayerVehicle), "its key: made from the vehicle, the top bit set (the rig's are small counters)");
  Check(player.axes.source == cl::FrameSource::kDerived, "no renderer frame: derived from the headlights' stored direction");
  const double back_len = std::sqrt(0.47 * 0.47 + 0.88 * 0.88);
  CheckNear(player.axes.back[0], 0.47 / back_len, 1e-6, "the derived back axis is the recorded one, horizontal (x)");
  CheckNear(player.axes.back[1], 0.0, 1e-12, "... (y)");
  CheckNear(player.axes.back[2], -0.88 / back_len, 1e-6, "... (z)");
  CheckNear(player.axes.right[0], -0.88 / back_len, 1e-6, "right is up x back (x)");
  CheckNear(player.axes.right[2], -0.47 / back_len, 1e-6, "... (z)");

  const cl::Lamp* head0 = LampOf(player, 3, 0);
  const cl::Lamp* head1 = LampOf(player, 3, 1);
  Check(head0 != nullptr && head1 != nullptr, "both headlights are there");
  Check(head0->side == cl::Side::kLeft && head1->side == cl::Side::kRight, "headlight 0 is the left one, 1 the right one (by the car's right axis)");
  CheckNear(head1->local[0] - head0->local[0], 1.18, 0.03, "the player's headlights are 1.2 m apart across the car (the probe's note)");
  Check(std::fabs(head0->local[2] - head1->local[2]) < 0.7f, "the pair is about level along the car (the player's is 0.56 m out of step: the points are not exactly across)");
  CheckNear(head0->local[2], -2.0, 0.15, "the headlights are 2 m ahead of the car's middle (z is back: negative at the front)");
  const cl::Lamp* brake0 = LampOf(player, 0, 0);
  CheckNear(brake0->local[2], 2.0, 0.15, "the brake lamps are 2 m behind it");
  Check(brake0->local[2] > 0.0f && head0->local[2] < 0.0f, "front and rear by the sign of z");
  CheckNear(head0->emit[0], -0.47 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-3, "a headlight shines along minus its stored direction: forward (x)");
  CheckNear(head0->emit[2], 0.88 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-3, "... (z)");
  CheckNear(brake0->emit[0], 0.47 / std::sqrt(0.47 * 0.47 + 0.04 * 0.04 + 0.88 * 0.88), 1e-3, "a brake lamp's stored direction is forward, so it shines back (x)");
  Check(brake0->emit[0] * head0->emit[0] < 0.0f, "head and brake lamps shine opposite ways");

  Check(player.lights.bits == (kCarLightHead | kCarLightBrake | kCarLightPoliceB), "the player's bits: head, the brake lamps' faint glow, Police1");
  CheckNear(player.lights.level[kCarLevelHead], 1.0, 1e-6, "head level 1 (2.0 over 2.0)");
  CheckNear(player.lights.level[kCarLevelBrake], 0.075, 1e-6, "brake level 0.075 (the idle reading of the player's brake lamps)");
  CheckNear(player.lights.level[kCarLevelTailTurn], 0.0, 1e-6, "no tail, no turn");
  CheckNear(player.lights.level[kCarLevelPolice], 1.0, 1e-6, "police level 1 (the player's Police1 reads 1.00)");
  Check(!LampOf(player, 4, 0)->on && !LampOf(player, 6, 0)->on && LampOf(player, 8, 1)->on, "hi-beam and turn are off, Police1 is on");
  Check(player.dir_vs_layout > 0.999f && player.dir_vs_layout <= 1.0f, "the stored direction agrees with the layout's back axis (rear lamps minus headlights): cosine 0.9998");
  CheckNear(player.family_level[uint32_t(lp::Family::kHead)], 1.0, 1e-6, "the family levels are kept too");

  // the racer whose lights are off: its lamps are all there and none is on
  const cl::Car* racer_off = CarOfObject(f, racer_off_object);
  Check(racer_off != nullptr && racer_off->lamp_count == 11 && racer_off->lights.bits == 0 && racer_off->lights.level[0] == 0.0f && !racer_off->player,
        "a racer with its lights off: 11 lamps, no bit, no level");
  CheckNear(racer_off->axes.back[2], -0.99 / std::sqrt(0.11 * 0.11 + 0.99 * 0.99), 1e-3, "... and a frame from its headlights' direction (this car heads +z: its back is -z)");

  // the racer with its lights on, the third brake lamp in the brake family
  const cl::Car* racer = CarOfObject(f, racer_lit_object);
  Check(racer != nullptr && racer->lights.bits == (kCarLightHead | kCarLightBrake), "the lit racer: head and brake");
  CheckNear(racer->lights.level[kCarLevelBrake], 1.0, 1e-6, "brake level is the largest of the family: the third lamp reads 1.002, capped at 1");
  CheckNear(racer->family_level[uint32_t(lp::Family::kBrake)], 1.0, 1e-6, "(the family level agrees)");
  CheckNear(LampOf(*racer, 0, 0)->level, 0.5, 1e-6, "the brake lamps themselves read 0.5");
  Check(LampOf(*racer, 3, 0)->side == cl::Side::kLeft && LampOf(*racer, 3, 1)->side == cl::Side::kRight, "the racer's headlights by the car's right axis, as the player's");

  // the order of the points does not change the answer
  std::vector<lp::Point> shuffled = Concat({racer_off_and_player, racer_lit});
  std::reverse(shuffled.begin(), shuffled.end());
  cl::CarLightsFrame g;
  cl::Build(shuffled, {}, camera, &g);
  bool same = g.count == f.count;
  for (uint32_t i = 0; same && i < f.count; ++i) {
    same = g.cars[i].object == f.cars[i].object && g.cars[i].lights.bits == f.cars[i].lights.bits && g.cars[i].lamp_count == f.cars[i].lamp_count;
    for (uint32_t k = 0; same && k < f.cars[i].lamp_count; ++k) same = g.cars[i].lamps[k].kind == f.cars[i].lamps[k].kind && g.cars[i].lamps[k].index == f.cars[i].lamps[k].index;
  }
  Check(same, "the same cars, in the same order with the same lamps, whatever the order the table gave the points in");
}

// A layout-derived frame (no stored direction) must agree with the direction-derived one: this is what the traffic cars have to do without one.
void LayoutFrameChecks() {
  auto player_points = Parse(kRecorded_frame_3603, 0xBF100000u);
  player_points.erase(player_points.begin(), player_points.begin() + 11);  // the player's 11
  cl::CarLightsFrame with_dir, without;
  cl::Build(player_points, {}, nullptr, &with_dir);
  for (lp::Point& p : player_points) p.has_dir = false;
  cl::Build(player_points, {}, nullptr, &without);
  Check(with_dir.count == 1 && without.count == 1 && without.cars[0].axes.source == cl::FrameSource::kDerived, "the player's lamps without their directions still make a frame, from the layout");
  const double dot = with_dir.cars[0].axes.back[0] * without.cars[0].axes.back[0] + with_dir.cars[0].axes.back[2] * without.cars[0].axes.back[2];
  Check(dot > std::cos(1.5 * 3.14159265358979 / 180.0), "the layout's back axis is within 1.5 degrees of the recorded one");
  const cl::Lamp* a = LampOf(without.cars[0], 3, 0);
  const cl::Lamp* b = LampOf(without.cars[0], 3, 1);
  Check(a->side == cl::Side::kLeft && b->side == cl::Side::kRight, "and the sides agree");
  Check(a->emit[2] > 0.0f && LampOf(without.cars[0], 0, 0)->emit[2] < 0.0f, "no stored direction: a lamp at the front shines forward, one at the rear back");
  // a car with no owner whose points DO carry a direction (the inference of light_points.h): not trusted for the frame, but compared with the layout and said
  {
    const auto traffic = Parse(kRecorded_frame_122, 0xB0000000u);
    std::vector<lp::Point> car2(traffic.begin() + 13, traffic.begin() + 26);
    cl::CarLightsFrame plain;
    cl::Build(car2, {}, nullptr, &plain);
    Check(plain.count == 1 && plain.cars[0].dir_vs_layout == 2.0f, "no stored direction: nothing to compare (2)");
    for (lp::Point& p : car2) {
      p.has_dir = true;
      p.dir[0] = float(plain.cars[0].axes.back[0]);
      p.dir[2] = float(plain.cars[0].axes.back[2]);
    }
    cl::CarLightsFrame inferred;
    cl::Build(car2, {}, nullptr, &inferred);
    Check(inferred.count == 1 && inferred.cars[0].dir_vs_layout > 0.999f, "an inferred direction that is the layout's back axis reads cosine 1");
    CheckNear(inferred.cars[0].axes.back[2], plain.cars[0].axes.back[2], 1e-12, "and the frame is still the layout's (an inferred direction is not trusted: kTrustOwnerlessDirection)");
    for (lp::Point& p : car2) {
      p.dir[0] = -p.dir[0];
      p.dir[2] = -p.dir[2];
    }
    cl::CarLightsFrame flipped;
    cl::Build(car2, {}, nullptr, &flipped);
    Check(flipped.cars[0].dir_vs_layout < -0.999f, "a direction the other way round reads -1: what a run would show if the inference were wrong");
    CheckNear(LampOf(flipped.cars[0], 3, 0)->emit[2], -plain.cars[0].axes.back[2], 1e-3, "(and a lamp then still shines by the layout, forward at the front)");
  }
  // a car with neither: the lamps are only offsets from their centre
  std::vector<lp::Point> lonely;
  lonely.push_back(PointOf(0x7000, 6, 0, 1.0f, 5.0f));
  cl::CarLightsFrame h;
  cl::Build(lonely, {}, nullptr, &h);
  Check(h.count == 1 && h.cars[0].axes.source == cl::FrameSource::kNone && h.cars[0].lamp_count == 1 && h.cars[0].lamps[0].emit[0] == 0.0f && h.cars[0].lamps[0].side == cl::Side::kCentre,
        "one lamp and nothing else: no frame, no side, no direction");
  Check(h.cars[0].lights.bits == (kCarLightTurnL | kCarLightTurnR), "a lit turn lamp whose side is not known sets both sides");
}

void RecordedTraffic() {
  const auto traffic = Parse(kRecorded_frame_122, 0xB0000000u);
  Check(traffic.size() == 41, "the recorded traffic lines parse (41 points)");
  const double camera[3] = {-2970.0, -24.0, 425.0};
  cl::CarLightsFrame f;
  cl::Build(traffic, {}, camera, &f);
  Check(f.count == 3, "three traffic cars from the three objects");
  Check(std::all_of(f.cars.begin(), f.cars.begin() + 3, [](const cl::Car& c) { return !c.owned && !c.player && c.vehicle == 0 && c.lights.vehicle == 0 && c.axes.source == cl::FrameSource::kDerived; }),
        "no owner, no vehicle address, a frame derived from the layout");
  // the nearest to the camera first: car 2 (z about 422) then car 3 (428) then car 1 (436)
  Check(f.cars[0].object == 0xB0000400u && f.cars[1].object == 0xB0000800u && f.cars[2].object == 0xB0000000u, "ranked by the distance to the camera");
  Check(f.cars[0].distance < f.cars[1].distance && f.cars[1].distance < f.cars[2].distance, "... the distances say so");

  const cl::Car* car1 = CarOfObject(f, 0xB0000000u);
  const cl::Car* car2 = CarOfObject(f, 0xB0000400u);
  const cl::Car* car3 = CarOfObject(f, 0xB0000800u);
  Check(car1->lamp_count == 13 && car2->lamp_count == 13 && car3->lamp_count == 15, "13, 13 and 15 lamps");
  // the cars head +z: the headlights have the larger z and are at the front (negative local z)
  for (const cl::Car* car : {car1, car2, car3}) {
    CheckNear(car->axes.back[2], -1.0, 0.02, "the car heads +z, so its back is -z");
    Check(LampOf(*car, 3, 0)->local[2] < -1.0f && LampOf(*car, 0, 0)->local[2] > 1.0f, "headlights ahead of the middle, brake lamps behind it");
    Check(LampOf(*car, 3, 0)->side != LampOf(*car, 3, 1)->side && LampOf(*car, 3, 0)->side != cl::Side::kCentre, "a headlight pair: one on each side");
    Check(LampOf(*car, 3, 1)->local[0] > LampOf(*car, 3, 0)->local[0], "index 1 is the right headlight, 0 the left (the span across the car is positive)");
  }
  // the turn signals named left and right (kinds 15 and 16) lie where the car's frame says: this holds the names, the layout and the frame together
  for (const cl::Car* car : {car1, car3}) {
    const cl::Lamp* bl = LampOf(*car, 15, 0);
    const cl::Lamp* br = LampOf(*car, 16, 0);
    Check(bl != nullptr && br != nullptr, "a car with the two front turn signals");
    Check(bl->side == cl::Side::kLeft && br->side == cl::Side::kRight, "named left and right");
    Check(bl->local[0] < -0.2f && br->local[0] > 0.2f, "and the car's frame puts them there: kind 15 on the left, 16 on the right, 0.6 m or more off the centre line");
    Check(bl->local[2] < -1.0f && br->local[2] < -1.0f, "both at the FRONT (the names say B, the probe saw them beside the headlights)");
  }
  // the turn signals of kind 6 sit at the rear corners of a traffic car, on opposite sides
  for (const cl::Car* car : {car1, car2, car3}) {
    const cl::Lamp* t0 = LampOf(*car, 6, 0);
    const cl::Lamp* t1 = LampOf(*car, 6, 1);
    Check(t0->local[2] > 1.0f && t0->side != t1->side && t0->side != cl::Side::kCentre && t1->side != cl::Side::kCentre, "kind 6: the rear corners of the car, on opposite sides");
  }
  // state
  Check(car1->lights.bits == (kCarLightHead | kCarLightBrake), "car 1: head and brake (0.84)");
  CheckNear(car1->lights.level[kCarLevelBrake], 0.84, 1e-5, "its brake level 0.84");
  CheckNear(car1->lights.level[kCarLevelHead], 1.0, 1e-6, "its headlights read 3.36: full");
  Check(car2->lights.bits == (kCarLightHead | kCarLightBrake | kCarLightPoliceB), "car 2: head, brake and Police1 (1.825)");
  CheckNear(car2->lights.level[kCarLevelPolice], 1.0, 1e-6, "a traffic car's Police1 reads 1.8: full");
  Check(car3->lights.bits == (kCarLightHead | kCarLightBrake | kCarLightTail | kCarLightPoliceB), "car 3: and a Taillight set");
  CheckNear(car3->lights.level[kCarLevelTailTurn], 0.902, 1e-5, "the tail/turn level is the taillight's");
  Check(!LampOf(*car1, 17, 0)->on && LampOf(*car1, 17, 0)->family == lp::Family::kOther, "kinds 17 and 18 are 'other' and read 0");
  Check(f.cars[0].lights.car_key != f.cars[1].lights.car_key && f.cars[1].lights.car_key != f.cars[2].lights.car_key, "three keys");
  Check(cl::FindByVehicle(f, 0) == nullptr, "a vehicle address of 0 is no car's");
  const double near_car2[3] = {car2->centre[0] + 0.5, car2->centre[1], car2->centre[2]};
  Check(cl::FindByPlace(f, near_car2) == car2, "a traffic car is found by place");
  const double far_away[3] = {0.0, 0.0, 0.0};
  Check(cl::FindByPlace(f, far_away) == nullptr, "... and not far from it");
}

// The renderer's frames: a racer by its vehicle address, a traffic car by place, each frame once.
void FrameJoinChecks() {
  const auto player_and_racers = Parse(kRecorded_frame_3603, 0xBF100000u);
  const auto traffic = Parse(kRecorded_frame_122, 0xB0000000u);
  cl::FrameIn player_frame;
  player_frame.vehicle = kPlayerVehicle;
  // the player's body matrix: rows right, up, back, position: the recorded axes, the root at the middle of the car's lamps (an invented ground point under it)
  const double bl = std::sqrt(0.47 * 0.47 + 0.88 * 0.88);
  const double rows[4][3] = {{-0.88 / bl, 0, -0.47 / bl}, {0, 1, 0}, {0.47 / bl, 0, -0.88 / bl}, {819.69, 17.0, -808.5}};
  std::memcpy(player_frame.rows, rows, sizeof(rows));
  // two traffic frames close to car 2 (the nearer one wins), and one far from every car
  cl::FrameIn near_a, near_b, far_c;
  const std::vector<lp::Point> car2_points(traffic.begin() + 13, traffic.begin() + 26);
  cl::CarLightsFrame derived;
  cl::Build(car2_points, {}, nullptr, &derived);
  const double* c2 = derived.cars[0].centre;
  const double ident[4][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 0}};
  std::memcpy(near_a.rows, ident, sizeof(ident));
  std::memcpy(near_b.rows, ident, sizeof(ident));
  std::memcpy(far_c.rows, ident, sizeof(ident));
  near_a.vehicle = 0xBF900000u;
  near_b.vehicle = 0xBF900100u;
  far_c.vehicle = 0xBF900200u;
  for (int i = 0; i < 3; ++i) {
    near_a.rows[3][i] = c2[i] + (i == 0 ? 1.0 : 0.0);
    near_b.rows[3][i] = c2[i] + (i == 0 ? 2.5 : 0.0);
    far_c.rows[3][i] = c2[i] + (i == 0 ? 40.0 : 0.0);
  }
  cl::CarLightsFrame f;
  cl::Build(Concat({player_and_racers, car2_points}), {player_frame, near_b, near_a, far_c}, nullptr, &f);
  Check(f.count == 3 && f.frames_matched == 2, "the player (by its vehicle address) and the traffic car (by place) are joined: two frames used");
  const cl::Car& player = f.cars[0];
  Check(player.player && player.axes.source == cl::FrameSource::kExternal, "the player's frame is the renderer's");
  CheckNear(player.axes.origin[0], 819.69, 1e-9, "its origin is the matrix's position");
  const cl::Lamp* head0 = LampOf(player, 3, 0);
  Check(head0->side == cl::Side::kLeft && head0->local[2] < -1.0f, "left and ahead in the renderer's own frame");
  const cl::Car* traffic_car = CarOfObject(f, 0xB0000400u);
  Check(traffic_car != nullptr, "the traffic car is there");
  if (traffic_car == nullptr) return;
  Check(traffic_car->axes.source == cl::FrameSource::kExternal, "the traffic car's frame is a renderer one");
  CheckNear(traffic_car->axes.origin[0], near_a.rows[3][0], 1e-9, "the nearer of the two frames within 3.5 m took it");
  const cl::Car* racer = CarOfObject(f, 0xBF100000u);
  Check(racer != nullptr && racer->axes.source == cl::FrameSource::kDerived, "a racer with no frame of its own stays derived (a frame with another vehicle's address does not take it)");

  // one frame, two cars: the nearer takes it, the other is derived
  cl::FrameIn only;
  std::memcpy(only.rows, ident, sizeof(ident));
  only.vehicle = 0;
  for (int i = 0; i < 3; ++i) only.rows[3][i] = c2[i];
  std::vector<lp::Point> two = car2_points;
  for (lp::Point p : car2_points) {
    p.object += 0x10000u;
    p.pos[0] += 2.0f;  // a second car 2 m aside
    two.push_back(p);
  }
  cl::Build(two, {only}, nullptr, &f);
  Check(f.count == 2 && f.frames_matched == 1, "two cars, one frame: it is used once");
  const int external = (f.cars[0].axes.source == cl::FrameSource::kExternal) + (f.cars[1].axes.source == cl::FrameSource::kExternal);
  Check(external == 1, "... by one of them only");
}

void CapChecks() {
  std::vector<lp::Point> all;
  for (uint32_t c = 0; c < 12; ++c) {
    // twelve cars in a row, 10 m apart on x, each with a lit headlight pair; the player's is the farthest
    for (uint32_t i = 0; i < 2; ++i) {
      lp::Point p = PointOf(0x9000u + c * 0x400u, 3, i, 2.0f, float(c) * 10.0f + float(i));
      p.vehicle = 0xBF000000u + c;
      p.owned = true;
      p.player = c == 11;
      all.push_back(p);
    }
  }
  const double camera[3] = {0, 0, 0};
  cl::CarLightsFrame f;
  cl::Build(all, {}, camera, &f);
  Check(f.count == kMaxCars && f.cars_seen == 12 && f.cars_dropped == 4, "twelve cars: the cap is kMaxCars (8), four are dropped");
  Check(f.cars[0].player && f.cars[0].object == 0x9000u + 11u * 0x400u, "the player's car is kept first even where it is the farthest");
  bool nearest = true;
  for (uint32_t i = 1; i < f.count; ++i) nearest = nearest && f.cars[i].object == 0x9000u + (i - 1u) * 0x400u;
  Check(nearest, "the rest are the nearest seven, in order");

  LightFrame lf;
  lf.car_count = 99;
  cl::FillLightFrame(&lf, f);
  Check(lf.car_count == kMaxCars && lf.cars[0].vehicle == 0xBF000000u + 11u && (lf.cars[0].bits & kCarLightHead) != 0 && lf.cars[0].level[kCarLevelHead] == 1.0f,
        "FillLightFrame puts the cars' CarLights into the frame");
  cl::CarLightsFrame empty;
  cl::FillLightFrame(&lf, empty);
  Check(lf.car_count == 0 && lf.cars[0].bits == 0, "an empty frame empties them");
}

// ---------------------------------------------------------------------------
// the lights the lamps make
// ---------------------------------------------------------------------------

void LightsChecks() {
  const auto traffic = Parse(kRecorded_frame_122, 0xB0000000u);
  cl::CarLightsFrame f;
  cl::Build(traffic, {}, nullptr, &f);
  const cl::Car* car1 = CarOfObject(f, 0xB0000000u);
  const cl::Car* car2 = CarOfObject(f, 0xB0000400u);
  const cl::Car* car3 = CarOfObject(f, 0xB0000800u);

  std::vector<Light> police;
  cl::MakePointLights(*car2, cl::kFamiliesPolice, &police);
  Check(police.size() == 2, "car 2's two Police1 lamps are two lights");
  Check(police[0].source == LightSource::kPolice && police[0].kind == LightKind::kPoint && police[0].law == LightLaw::kOmni && police[0].cos_outer == -1.0f, "omni lights of the police source");
  Check(police[0].color[2] > police[0].color[0] * 4.0f, "Police1 is blue (a guess: CHECK)");
  CheckNear(police[0].color[2], 6.0, 1e-4, "colour x level x the look's intensity (level 1, intensity 6)");
  CheckNear(police[0].range, light_laws::LampRange(6.0f), 1e-4, "the range is the game's reach for that intensity");
  CheckNear(police[0].law_a, 6.0, 1e-6, "the plateau is the intensity");
  Check((police[0].roles & kRoleMaskShadow) == 0 && (police[0].roles & kRoleMaskSurface) != 0 && (police[0].roles & kRoleMaskVolume) != 0 && (police[0].roles & kRoleMaskRt) != 0,
        "surface, volume and RT; no shadow views for a beacon");
  Check(police[0].car_key == car2->lights.car_key && police[0].flags == 0u, "the car's key, not the player's");
  Check(police[0].id != kNoLightId && police[0].id != police[1].id && police[0].id == light_id::PointId(car2->object, (8u << 8) | 0u), "ids in the points' domain: car, kind and index");
  std::vector<Light> again;
  cl::MakePointLights(*car2, cl::kFamiliesPolice, &again);
  Check(again.size() == 2 && again[0].id == police[0].id && again[1].id == police[1].id, "the same ids a frame later");

  std::vector<Light> none;
  cl::MakePointLights(*car1, cl::kFamiliesPolice, &none);
  Check(none.empty(), "car 1 has no police lamp on");
  std::vector<Light> rear;
  cl::MakePointLights(*car3, cl::kFamiliesRear, &rear);
  Check(rear.size() == 4 && std::all_of(rear.begin(), rear.end(), [](const Light& l) { return l.source == LightSource::kOther && l.color[0] > l.color[2]; }),
        "car 3's two brake and two tail lamps: four red lights (the turn signals are off)");
  CheckNear(rear[0].color[0], 0.902 * 0.8, 1e-3, "a brake lamp's colour is the level (0.902) times the look's intensity");
  std::vector<Light> nothing;
  cl::MakePointLights(*car3, 0u, &nothing);
  Check(nothing.empty(), "no family asked, no light");

  // the player's lamps carry the player's flag
  const auto racer_off_and_player = Parse(kRecorded_frame_3603, 0xBF100000u);
  cl::CarLightsFrame g;
  cl::Build(racer_off_and_player, {}, nullptr, &g);
  std::vector<Light> mine;
  cl::MakePointLights(g.cars[0], cl::kFamiliesPolice | cl::kFamiliesRear, &mine);
  Check(g.cars[0].player && mine.size() == 4 && std::all_of(mine.begin(), mine.end(), [](const Light& l) { return (l.flags & kLightFlagPlayer) != 0; }),
        "the player's police (2) and faint brake lamps (2) carry the player's flag");

  // the census line: what the probe run reads the CHECKs from
  const std::string line = cl::FormatCensus(g.cars[0]);
  Check(line.find("[car-lights] player 0xbf4e7380") == 0 && line.find("frame derived") != std::string::npos && line.find("dir/layout 1.000 |") != std::string::npos && line.find("k3 Headlight x2 on 2 max 2.000;") != std::string::npos &&
            line.find("k0 Brakelight x2 on 2 max 0.075;") != std::string::npos && line.find("k8 Police1 x2 on 2 max 1.000;") != std::string::npos && line.find("k12 None x1 on 0") != std::string::npos,
        "the census line names the car, its frame and each kind: " + line);
  const std::string tline = cl::FormatCensus(*car2);
  Check(tline.find("[car-lights] traffic 0x0") == 0 && tline.find("k17 Kind17 x1 on 0 max 0.000;") != std::string::npos, "and a traffic car's");
}

// ---------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------

void SourceChecks() {
  for (const auto who : {lp::Consumer::kManager, lp::Consumer::kRt, lp::Consumer::kDebug}) lp::Want(who, 0);
  cl::CarLightsSource source;
  lp::Table table;
  const auto t0 = Clock::now();
  for (lp::Point p : Parse(kRecorded_frame_3603, 0xBF100000u)) table.Note(p, t0);
  const double camera[3] = {820.0, 18.0, -808.0};
  Check(source.Poll({}, camera, t0 + ms(5), table).count == 0 && !source.enabled(), "disabled: nothing is read");
  source.SetEnabled(true);
  Check(source.enabled() && lp::Wanted(0) && lp::Wanted(18) && !lp::Wanted(19), "enabled: the hook is asked for every kind");
  const cl::CarLightsFrame& f = source.Poll({}, camera, t0 + ms(5), table);
  Check(f.count == 2 && f.cars[0].player && &f == &source.Frame(), "two cars, the player's first");
  Check(source.Poll({}, camera, t0 + ms(500), table).count == 0, "points not seen for 100 ms are gone: no cars");
  source.SetEnabled(false);
  Check(!lp::AnyWanted() && source.Frame().count == 0, "disabled again: the hook is off and the frame is empty");
}

// ---------------------------------------------------------------------------
// a mod's lamps
// ---------------------------------------------------------------------------

void ModChecks() {
  mls::Set set;
  set.mod = "test-track";
  set.mod_key = light_id::ModKey(set.mod);
  for (uint32_t i = 0; i < 100; ++i) {
    mls::Record r;
    r.pos[0] = float(i) * 15.0f;
    r.pos[1] = 6.0f;
    r.colour[0] = 1.0f;
    r.colour[1] = 0.8f;
    r.colour[2] = 0.5f;
    r.intensity = 40.0f;
    r.cone_deg = 125.0f;
    r.spread_deg = -50.5f;
    r.type = 1;
    r.id = 1000u + i;
    r.on = i % 10 == 3 ? 0 : 1;
    set.lamps.push_back(r);
  }
  set.lamps[7].type = 0;
  set.lamps[7].cone_deg = 360.0f;
  const float camera[3] = {150.0f, 1.8f, 2.0f};  // by lamp 10
  std::vector<Light> out;
  mls::Candidates(set, camera, 8, &out);
  Check(out.size() == 8, "at most max_out lamps");
  Check(std::all_of(out.begin(), out.end(), [&](const Light& l) { return l.source == LightSource::kMod && l.roles == kRoleMaskGameLit && (l.flags & kLightFlagGameLit) != 0; }),
        "source kMod, the roles and flag of a game-lit lamp (the mod's grid already lights the surface)");
  Check(out[0].id == light_id::ModLampId(set.mod_key, 1010u), "the nearest lamp (record 10, 0.0 m along) is first");
  bool off_in = false, ids_unique = true;
  std::set<uint64_t> seen;
  for (const Light& l : out) {
    for (uint32_t i = 0; i < 100; ++i) {
      if (set.lamps[i].on == 0 && l.id == light_id::ModLampId(set.mod_key, set.lamps[i].id)) off_in = true;
    }
    ids_unique = ids_unique && seen.insert(l.id).second;
  }
  Check(!off_in && ids_unique, "lamps that are off are not candidates; the ids are the mod's");
  // 8 nearest on-lamps to x = 150: records 10 (150), 9 and 11 (135, 165), 8 and 12, 7 and 13, then 6 or 14; 13 is off (i % 10 == 3)
  bool in_order = true;
  for (size_t i = 1; i < out.size(); ++i) {
    const float di = std::fabs(out[i].pos[0] - camera[0]), dp = std::fabs(out[i - 1].pos[0] - camera[0]);
    in_order = in_order && di >= dp - 1e-3f;
  }
  Check(in_order, "best first: the nearer a lamp, the better its score");
  const Light* omni = nullptr;
  for (const Light& l : out) {
    if (l.id == light_id::ModLampId(set.mod_key, 1007u)) omni = &l;
  }
  Check(omni != nullptr && omni->kind == LightKind::kPoint && omni->law == LightLaw::kOmni && omni->cos_outer == -1.0f, "a lamp of type 0 is an omni light");
  CheckNear(out[0].range, light_laws::LampRange(40.0f), 1e-4, "a spot lamp: the game's reach sqrt(100 x 40) = 63 m");
  Check(out[0].kind == LightKind::kSpot && out[0].law == LightLaw::kGameLamp && out[0].cos_outer < 0.5f, "its cone is the game's");

  // far lamps are not candidates, and a tie is told by the record's id
  std::vector<Light> few;
  const float far_camera[3] = {5000.0f, 1.8f, 2.0f};
  mls::Candidates(set, far_camera, 64, &few);
  Check(few.empty(), "a camera far from every lamp's reach has no candidates");
  mls::Set tie;
  tie.mod_key = 77;
  for (uint32_t i = 0; i < 4; ++i) {
    mls::Record r;
    r.pos[0] = 10.0f;
    r.intensity = 40.0f;
    r.id = 50u - i;
    tie.lamps.push_back(r);
  }
  std::vector<Light> tied;
  const float origin[3] = {0, 0, 0};
  mls::Candidates(tie, origin, 2, &tied);
  Check(tied.size() == 2 && tied[0].id == light_id::ModLampId(77, 47u) && tied[1].id == light_id::ModLampId(77, 48u), "equal scores: the lower record id first, not the vector's order");
  std::vector<Light> none;
  mls::Set empty;
  mls::Candidates(empty, origin, 8, &none);
  Check(none.empty(), "an empty set makes no light");

  // the feed
  mls::Feed feed;
  uint64_t gen = 99;
  Check(feed.Current(&gen) == nullptr && gen == 0, "nothing published: no set");
  auto shared = std::make_shared<const mls::Set>(set);
  feed.Publish(shared);
  uint64_t gen2 = 0;
  Check(feed.Current(&gen2) == shared && gen2 == 1, "a published set comes back, the generation says so");
  feed.Publish(nullptr);
  Check(feed.Current(&gen) == nullptr && gen == 2, "the camera left the mod's rectangle: null, a new generation");
  Check(shared->lamps.size() == 100, "the holder of an old set still has it");
}

}  // namespace

int main() {
  KindChecks();
  WantedChecks();
  TableChecks();
  ReadPointChecks();
  RigContractChecks();
  RecordedPlayerAndRacers();
  LayoutFrameChecks();
  RecordedTraffic();
  FrameJoinChecks();
  CapChecks();
  LightsChecks();
  SourceChecks();
  ModChecks();
  std::printf("car_lights_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
