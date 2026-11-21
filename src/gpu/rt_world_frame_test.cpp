// Check of the ray tracing world frame (CITY-RT package FRAME): rt_world_frame_rules.h and its shader side hlsl/rt_world_frame.hlsli. A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -ffp-contract=off -I src -I src/gpu src/gpu/rt_world_frame_test.cpp src/citypack/lamp_merge.cpp src/citypack/lamp_table.cpp src/citypack/city_hit_format.cpp \
//           src/citypack/pack_format.cpp -o rt_world_frame_test
//   rt_world_frame_test <src dir>
//
// It checks the layout and the packing of the two buffers (the world rows over random inputs, the hit rows inside them unchanged), the lamp schedule (the on hours and the night curve:
// the street lamps' fraction over the prop pass's jitter, the fill lights' ramp, the midnight wrap, the hint for an unknown hour), the merge of the static table with the game's live list (the
// street lamps the fresh list covers are not taken twice, the list's order does not matter), the activity folded into the record's colour, the budget (the nearest are kept), the window and its
// cell table (a partition of the lamps, every lamp in its cell), the lookup against a brute force over the window (complete within the radius, in the order the shader walks, no
// duplicate), the lamp light through the grid against the library's twin over every lamp (exact for a radius of 100 m, a bound below), and the census. Then it compiles the SHADER FILE ITSELF as
// C++ (hlsl_cpp_shim.h): RtWorldLoad, RtWorldHitFrame, RtWorldClassGain, RtWorldPoint, RtLampRow and the cursor, on the same buffers, against the twin, and every #define against the
// header's numbers. With the src folder it reads the text of the shaders, the car scene's descriptor set (the counts of D3D12), the wiring in the scene, the renderer and the time of day hook.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "hlsl_cpp_shim.h"
#include "rt_world_frame_rules.h"
#include "world_clock.h"

namespace rw = mcla::gpu::rt_world;
namespace rh = mcla::gpu::rt_hit;
namespace hc = mcla::gpu::rt_hit_cal;
namespace cs = mcla::gpu::city_hit_shade;
namespace ch = larecomp::citypack;

// ---------------------------------------------------------------------------
// the shader file as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
StructuredBuffer<uint4> gRtWorld;
StructuredBuffer<uint4> gRtLamps;
}  // namespace hlsl_shim

#define RT_WORLD_NO_RESOURCES
#define RT_WORLD_LOOP
#define RT_HIT_FRAME_ROW(i) hlsl_shim::gRtWorld[i]

namespace hlsl_shim {
#include "hlsl/rt_world_frame.hlsli"
}  // namespace hlsl_shim

namespace {

int g_checks = 0;
int g_failed = 0;
void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    if (g_failed <= 60) std::printf("FAILED: %s\n", what.c_str());
  }
}
#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond)
bool Near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }

struct Lcg {
  uint32_t state = 20261121u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float Next() { return float(U32() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : (U32() >> 8) % n; }
};

float Comp(const cs::V3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------
// a synthetic city's lamps
// ---------------------------------------------------------------------------

// `n` fill lights (omni, intensity 10 to 30) and `m` street lamps (spots, 40 to 100, cone 125, pointing down) over a box of `side` metres around the origin.
std::vector<ch::Lamp> MakeLamps(Lcg& rng, uint32_t n, uint32_t m, float side, bool street_in_table) {
  std::vector<ch::Lamp> lamps;
  for (uint32_t i = 0; i < n; ++i) {
    ch::Lamp l;
    l.pos[0] = rng.Range(-side, side);
    l.pos[1] = rng.Range(0.0f, 12.0f);
    l.pos[2] = rng.Range(-side, side);
    l.colour[0] = rng.Range(0.5f, 1.0f);
    l.colour[1] = rng.Range(0.5f, 1.0f);
    l.colour[2] = rng.Range(0.3f, 1.0f);
    l.intensity = rng.Range(10.0f, 30.0f);
    l.spot = 0;
    l.cone_deg = 360.0f;
    l.source = ch::LampSource::kFile;
    l.file_index = i;
    lamps.push_back(l);
  }
  if (street_in_table) {
    for (uint32_t i = 0; i < m; ++i) {
      ch::Lamp l;
      l.pos[0] = rng.Range(-side, side);
      l.pos[1] = rng.Range(6.0f, 9.0f);
      l.pos[2] = rng.Range(-side, side);
      l.colour[0] = 1.0f;
      l.colour[1] = 0.8f;
      l.colour[2] = 0.5f;
      l.intensity = rng.Range(40.0f, 100.0f);
      l.spot = 1;
      l.cone_deg = 125.0f;
      l.spread_deg = -50.5f;
      l.dir[0] = 0.0f;
      l.dir[1] = -1.0f;
      l.dir[2] = 0.0f;
      l.source = ch::LampSource::kStreetLamp;
      lamps.push_back(l);
    }
  }
  return lamps;
}

rw::LiveLamp MakeLive(Lcg& rng, float cx, float cz, float radius, float scale) {
  rw::LiveLamp g;
  const float angle = rng.Range(0.0f, 6.2831853f);
  const float d = radius * std::sqrt(rng.Next());
  g.lamp.pos[0] = cx + d * std::cos(angle);
  g.lamp.pos[1] = rng.Range(6.0f, 9.0f);
  g.lamp.pos[2] = cz + d * std::sin(angle);
  g.lamp.colour[0] = 1.0f;
  g.lamp.colour[1] = 0.8f;
  g.lamp.colour[2] = 0.5f;
  g.lamp.intensity = rng.Range(40.0f, 100.0f);
  g.lamp.spot = 1;
  g.lamp.cone_deg = 125.0f;
  g.lamp.spread_deg = -50.5f;
  g.lamp.source = ch::LampSource::kStreetLamp;
  g.scale = scale;
  return g;
}

rw::LampInput NightInput(const ch::LampTable* table, const std::vector<rw::LiveLamp>* live, bool fresh, double cx, double cz, float hour, rw::Tier tier) {
  rw::LampInput in;
  in.table = table;
  in.live = live;
  in.live_fresh = fresh;
  in.camera[0] = cx;
  in.camera[1] = 3.0;
  in.camera[2] = cz;
  in.hour = hour;
  in.schedule = rw::ScheduleAt(table != nullptr ? table->hours() : ch::LampHours{}, hour, -1.0f);
  in.budget = rw::BudgetFor(tier);
  return in;
}

// the lamp of record `i` of a frame as the library's twin reads it
cs::Lamp RecordLamp(const rw::LampFrame& f, uint32_t i) {
  uint32_t rows[3][4];
  rw::LampRows(f.words.data(), i, rows);
  return cs::UnpackGpuLamp(rows);
}

// ---------------------------------------------------------------------------
// the numbers: the header's and the shader's
// ---------------------------------------------------------------------------

void TestNumbers(const std::string& hlsl) {
  std::map<std::string, std::string> defines;
  std::istringstream in(hlsl);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#define ", 0) != 0) continue;
    std::istringstream ls(line.substr(8));
    std::string name, value;
    ls >> name >> value;
    if (name.find('(') == std::string::npos) defines[name] = value;
  }
  const auto number = [&](const char* name) -> double {
    const auto it = defines.find(name);
    if (it == defines.end()) {
      Check(false, std::string("the shader has no #define ") + name);
      return -1.0;
    }
    return std::strtod(it->second.c_str(), nullptr);
  };
  CHECK(number("RT_WORLD_ROW_STATUS") == rw::kRowStatus);
  CHECK(number("RT_WORLD_ROW_LIGHT") == rw::kRowLight);
  CHECK(number("RT_WORLD_ROW_CAMERA") == rw::kRowCamera);
  CHECK(number("RT_WORLD_ROW_SUN_WORLD") == rw::kRowSunWorld);
  CHECK(number("RT_WORLD_ROW_GRID") == rw::kRowGrid);
  CHECK(number("RT_WORLD_ROW_WINDOW") == rw::kRowWindow);
  CHECK(number("RT_WORLD_ROW_COUNTS") == rw::kRowCounts);
  CHECK(number("RT_WORLD_ROW_CALIBRATION") == rw::kRowCalibration);
  CHECK(number("RT_WORLD_ROW_GAINS") == rw::kRowGains);
  CHECK(number("RT_WORLD_ROW_SCHEDULE") == rw::kRowSchedule && rw::kRowSchedule == 47 && rw::kRowSchedule < rw::kTotalRows);
  CHECK(number("RT_WORLD_EVIDENCE_ON") == rw::kEvidenceOn && number("RT_WORLD_EVIDENCE_OFF") == rw::kEvidenceOff);
  CHECK(number("RT_WORLD_GLOW_NIGHT") == ch::kGlowNight && number("RT_WORLD_GLOW_LENS") == ch::kGlowLens && number("RT_WORLD_GLOW_ALWAYS") == ch::kGlowAlways);
  CHECK(number("RT_WORLD_TOTAL_ROWS") == rw::kTotalRows);
  CHECK(number("RT_WORLD_CLASSES") == rw::kClasses);
  CHECK(number("RT_WORLD_VALID") == rw::kWorldValid);
  CHECK(number("RT_WORLD_HOUR") == rw::kWorldHour);
  CHECK(number("RT_WORLD_CAMERA_KNOWN") == rw::kWorldCameraKnown);
  CHECK(number("RT_WORLD_SUN_WORLD") == rw::kWorldSunWorld);
  CHECK(number("RT_WORLD_LIVE_FRESH") == rw::kWorldLiveFresh);
  CHECK(number("RT_WORLD_LAMPS") == rw::kWorldLamps);
  CHECK(number("RT_WORLD_STATS_FRAME") == rw::kWorldStatsFrame);
  CHECK(number("RT_WORLD_CAR") == rw::kWorldCar);
  CHECK(number("RT_WORLD_LIGHT") == rw::kWorldLight);
  CHECK(number("RT_WORLD_LOOKUP_AXIS") == rw::kMaxLookupAxis);
  CHECK(number("RT_WORLD_LOOKUP_CELLS") == rw::kMaxLookupAxis * rw::kMaxLookupAxis);
  CHECK(number("RT_WORLD_LOOKUP_MAX_CELLS") == rw::kMaxLookupCells);
  CHECK(number("RT_WORLD_LAMP_ROWS") == rw::kLampRowsEach);
  CHECK(defines["RT_WORLD_UNKNOWN_WEATHER"] == "0xFFFFFFFFu");
  CHECK(rw::kUnknownWeather == 0xFFFFFFFFu);
  // the flags are single distinct bits
  uint32_t seen = 0;
  for (const uint32_t f : {rw::kWorldValid, rw::kWorldHour, rw::kWorldCameraKnown, rw::kWorldSunWorld, rw::kWorldLiveFresh, rw::kWorldLamps, rw::kWorldStatsFrame, rw::kWorldCar, rw::kWorldLight}) {
    CHECK((f & (f - 1)) == 0 && f != 0 && (seen & f) == 0);
    seen |= f;
  }
  // the layout is what the header's comment says
  CHECK(rw::kHitRows == 32 && rh::kFrameRows == 32);
  CHECK(rw::kRowGains + rw::kClasses / 4 <= rw::kTotalRows);
  CHECK(rw::kWorldBytes == 768);
  CHECK(rw::kWorldRegister == 10 && rw::kLampsRegister == 11 && rw::kBindingCount == 2);
  CHECK(rw::kMaxWindowCells == 256 && rw::kCellRowsMax == 128);
  CHECK(rw::kLampBufferBytes == (1 + 128 + 3 * 2048) * 16);
  // the registers in the shader
  CHECK(Has(hlsl, "gRtWorld : register(t10, RT_WORLD_SPACE_NAME)"));
  CHECK(Has(hlsl, "gRtLamps : register(t11, RT_WORLD_SPACE_NAME)"));
  CHECK(Has(hlsl, "#error \"define RT_WORLD_SPACE"));
  CHECK(Has(hlsl, "#ifndef RT_HIT_FRAME_ROW\n#define RT_HIT_FRAME_ROW(i) gRtWorld[i]\n#endif"));
}

// ---------------------------------------------------------------------------
// the schedule
// ---------------------------------------------------------------------------

void TestSchedule() {
  const ch::LampHours hours;  // 18, 6, 0.1
  // by day nothing shines, at night everything does, midnight is in the night
  for (const float h : {7.0f, 9.0f, 12.0f, 15.5f, 17.0f}) {
    const rw::Schedule s = rw::ScheduleAt(hours, h, -1.0f);
    CHECK(s.from_hour && s.street == 0.0f && s.fill == 0.0f && s.factor == 0.0f);
  }
  for (const float h : {19.0f, 20.5f, 23.0f, 23.99f, 0.0f, 1.0f, 3.0f, 5.0f}) {
    const rw::Schedule s = rw::ScheduleAt(hours, h, -1.0f);
    CHECK(s.street == 1.0f && s.fill == 1.0f && s.factor == 1.0f);
  }
  // the street lamps come on one by one over the jitter's 0.25 h: a staircase from 0 to 1 in sixteenths, never down, and every step is a fraction of sixteen
  float previous = 0.0f;
  int steps = 0;
  for (float h = 17.4f; h < 18.6f; h += 0.001f) {
    const float s = rw::StreetFraction(hours, h);
    CHECK(s >= previous);
    CHECK(std::fabs(s * 16.0f - std::round(s * 16.0f)) < 1e-6f);
    if (s != previous) ++steps;
    previous = s;
  }
  CHECK(previous == 1.0f && steps >= 8 && steps <= 16);
  // the morning goes the other way
  previous = 1.0f;
  for (float h = 5.4f; h < 6.8f; h += 0.001f) {
    const float s = rw::StreetFraction(hours, h);
    CHECK(s <= previous);
    previous = s;
  }
  CHECK(previous == 0.0f);
  // the first lamp comes on at 18 - 0.1 - 0.25, the last at 18 - 0.1
  CHECK(rw::StreetFraction(hours, 17.64f) == 0.0f);
  CHECK(rw::StreetFraction(hours, 17.66f) > 0.0f);
  CHECK(rw::StreetFraction(hours, 17.89f) < 1.0f);
  CHECK(rw::StreetFraction(hours, 17.91f) == 1.0f);
  // the fill lights follow EstimateNightFactor (a 0.5 h ramp), the schedule is the larger
  for (float h = 17.0f; h < 18.2f; h += 0.05f) {
    const rw::Schedule s = rw::ScheduleAt(hours, h, -1.0f);
    CHECK(s.fill == ch::EstimateNightFactor(hours, h));
    CHECK(s.factor == std::max(s.street, s.fill));
    CHECK(s.factor >= 0.0f && s.factor <= 1.0f);
  }
  // no hour: the world sun's night factor stands in for both; none of that either: day
  const rw::Schedule hinted = rw::ScheduleAt(hours, -1.0f, 0.7f);
  CHECK(!hinted.from_hour && hinted.street == 0.7f && hinted.fill == 0.7f && hinted.factor == 0.7f);
  const rw::Schedule none = rw::ScheduleAt(hours, -1.0f, -1.0f);
  CHECK(!none.from_hour && none.factor == 0.0f);
  CHECK(rw::ScheduleAt(hours, 24.5f, 0.3f).from_hour == false);
  CHECK(rw::ScheduleAt(hours, std::nanf(""), 0.3f).factor == 0.3f);
  CHECK(rw::ScheduleAt(hours, 12.0f, 1.5f).from_hour);  // a good hour outranks the hint
  // other lamp hours (the lighting manager's own floats): 20 and 5
  ch::LampHours late;
  late.on_hour = 20.0f;
  late.off_hour = 5.0f;
  CHECK(rw::StreetFraction(late, 19.0f) == 0.0f && rw::StreetFraction(late, 21.0f) == 1.0f && rw::StreetFraction(late, 4.0f) == 1.0f && rw::StreetFraction(late, 6.0f) == 0.0f);

  // a lamp's activity: a street lamp by its own jitter, a fill light by the night factor, an off lamp never
  ch::Lamp street;
  street.source = ch::LampSource::kStreetLamp;
  street.pos[0] = 12.5f;
  ch::Lamp fill;
  const rw::Schedule dusk = rw::ScheduleAt(hours, 17.8f, -1.0f);
  CHECK(rw::ActivityOf(fill, dusk, hours, 17.8f) == dusk.fill);
  const float a = rw::ActivityOf(street, dusk, hours, 17.8f);
  CHECK(a == 0.0f || a == 1.0f);
  CHECK(a == (ch::StreetLampOn(hours, 17.8f, ch::LampJitterHours(ch::LampKeyOf(street))) ? 1.0f : 0.0f));
  ch::Lamp dead = fill;
  dead.on = 0;
  CHECK(rw::ActivityOf(dead, dusk, hours, 17.8f) == 0.0f);
  const rw::Schedule unknown = rw::ScheduleAt(hours, -1.0f, 0.4f);
  CHECK(rw::ActivityOf(street, unknown, hours, -1.0f) == 0.4f && rw::ActivityOf(fill, unknown, hours, -1.0f) == 0.4f);
  // the units and the wetness
  CHECK(rw::UnitsOf(16.0f) == 16.0f && rw::UnitsOf(1.0f) == rw::kDefaultUnits && rw::UnitsOf(0.0f) == rw::kDefaultUnits && rw::UnitsOf(std::nanf("")) == rw::kDefaultUnits && rw::UnitsOf(2.0f) == 2.0f);
  CHECK(rw::kDefaultUnits == std::ldexp(1.0f, rw::kMainTargetExpBias));
  CHECK(rw::WetOf(0.095f) == 0.0f && rw::WetOf(1.17f) == 1.0f && rw::WetOf(3.0f) == 1.0f && rw::WetOf(0.0f) == 0.0f && Near(rw::WetOf(0.6325f), 0.5, 1e-4) && rw::WetOf(std::nanf("")) == 0.0f);
  // the stats frames come one in kStatsPeriod
  uint32_t n = 0;
  for (uint64_t f = 0; f < 2000; ++f) n += rw::StatsFrameDue(f) ? 1u : 0u;
  CHECK(n == 2000 / rw::kStatsPeriod && rw::StatsFrameDue(0) && !rw::StatsFrameDue(1) && rw::StatsFrameDue(rw::kStatsPeriod));
}

// ---------------------------------------------------------------------------
// a lamp's record
// ---------------------------------------------------------------------------

void TestRecords() {
  Lcg rng;
  for (int round = 0; round < 400; ++round) {
    ch::Lamp lamp;
    for (int c = 0; c < 3; ++c) {
      lamp.pos[c] = rng.Range(-500.0f, 500.0f);
      lamp.colour[c] = rng.Range(0.0f, 1.0f);
    }
    lamp.intensity = rng.Range(5.0f, 120.0f);
    lamp.spot = rng.Below(2) ? 1 : 0;
    lamp.cone_deg = rng.Below(5) == 0 ? 360.0f : rng.Range(40.0f, 145.0f);
    lamp.spread_deg = rng.Range(-60.0f, 60.0f);
    // a rotated frame: a random rotation about y and a random camera
    const float yaw = rng.Range(0.0f, 6.2831853f);
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float axes[3][3] = {{c, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {s, 0.0f, c}};  // the world's axes in view space
    const double camera[3] = {rng.Range(-300.0f, 300.0f), rng.Range(0.0f, 30.0f), rng.Range(-300.0f, 300.0f)};
    const float weight = rng.Range(0.0f, 3.0f);
    lamp.dir[0] = rng.Range(-0.2f, 0.2f);
    lamp.dir[1] = -1.0f;
    lamp.dir[2] = rng.Range(-0.2f, 0.2f);
    uint32_t rows[3][4];
    rw::MakeLampRecord(lamp, weight, camera, axes, rows);
    const cs::Lamp back = cs::UnpackGpuLamp(rows);
    for (int k = 0; k < 3; ++k) {
      float expected = 0.0f;
      for (int j = 0; j < 3; ++j) expected += (lamp.pos[j] - float(camera[j])) * axes[j][k];
      CHECK(Near(Comp(back.position, k), expected, 1e-4));
      CHECK(Near(Comp(back.colour, k), lamp.colour[k] * weight, 1e-6));
    }
    CHECK(back.intensity == lamp.intensity);
    CHECK(Near(cs::Length(back.axis), 1.0, 1e-5));
    if (!lamp.is_spot() || lamp.cone_deg >= 359.0f) {
      CHECK(back.cos_outer == -2.0f && back.cos_inner == -2.0f);  // no cone: the library's rule is "outer below -1.5"
    } else {
      float outer = 0, inner = 0;
      ch::LampConeCosines(lamp.cone_deg, lamp.spread_deg, &outer, &inner);
      CHECK(back.cos_outer == outer && back.cos_inner == inner && back.cos_outer > -1.5f && back.cos_outer != back.cos_inner);
    }
  }
  // a spot with no axis has no cone (the game's rule), an omni light of 360 degrees likewise
  ch::Lamp spot;
  spot.spot = 1;
  spot.cone_deg = 100.0f;
  spot.dir[0] = spot.dir[1] = spot.dir[2] = 0.0f;
  const float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const double origin[3] = {0, 0, 0};
  uint32_t rows[3][4];
  rw::MakeLampRecord(spot, 1.0f, origin, id, rows);
  CHECK(cs::UnpackGpuLamp(rows).cos_outer == -2.0f);
}

// ---------------------------------------------------------------------------
// the frame's lamps
// ---------------------------------------------------------------------------

// every invariant of a built frame, against its input
void CheckFrame(const rw::LampFrame& f, const rw::LampInput& in, const char* what) {
  const std::string w = what;
  Check(f.cell_m == rw::kCellMetres, w + ": the cell");
  Check(f.nx >= 1 && f.nx <= rw::kMaxWindowAxis && f.nz >= 1 && f.nz <= rw::kMaxWindowAxis, w + ": the window's size");
  Check(f.lamp_count == f.stats.kept && f.lamp_count <= rw::kMaxLampsHard, w + ": the count");
  Check(f.lamp_row_base == 1 + rw::CellRowsFor(f.cells()), w + ": the lamps' first row");
  Check(f.rows() == f.lamp_row_base + 3 * f.lamp_count, w + ": the rows");
  Check(f.rows() <= rw::MaxLampRows(rw::kMaxLampsHard), w + ": the buffer's room");
  Check(f.words[0] == f.lamp_count && f.words[1] == f.cells() && f.words[2] == f.lamp_row_base, w + ": the header");
  // the cells partition the lamps in order
  uint32_t next = 0;
  bool partition = true, in_cell = true;
  for (uint32_t cz = 0; cz < f.nz; ++cz) {
    for (uint32_t cx = 0; cx < f.nx; ++cx) {
      uint32_t first = 0, count = 0;
      rw::CellRange(f.words.data(), f.nx, int32_t(cx), int32_t(cz), &first, &count);
      if (count == 0) continue;
      partition = partition && first == next;
      next = first + count;
      for (uint32_t i = first; i < first + count && i < f.lamp_count; ++i) {
        uint32_t rows[3][4];
        rw::LampRows(f.words.data(), i, rows);
        const cs::Lamp l = cs::UnpackGpuLamp(rows);
        // identity axes in these inputs: world = view + camera
        const double wx = double(l.position.x) + in.camera[0], wz = double(l.position.z) + in.camera[2];
        in_cell = in_cell && int32_t(std::floor(wx / 64.0)) - f.cx0 == int32_t(cx) && int32_t(std::floor(wz / 64.0)) - f.cz0 == int32_t(cz);
      }
    }
  }
  Check(partition && next == f.lamp_count, w + ": the cells partition the lamps");
  Check(in_cell, w + ": every lamp is in its cell");
}

void TestLampFrame() {
  Lcg rng;
  const float side = 1500.0f;
  std::vector<ch::Lamp> all = MakeLamps(rng, 2600, 900, side, /*street_in_table=*/true);
  const ch::LampTable table = ch::LampTable::Build(all);
  const ch::LampTable fill_only = ch::LampTable::Build(MakeLamps(rng, 2600, 0, side, false));
  CHECK(table.size() == 3500);
  const double cx = 200.0, cz = -150.0;

  // day: the window, no lamp
  {
    rw::LampFrame f;
    rw::BuildLampFrame(NightInput(&table, nullptr, false, cx, cz, 12.0f, rw::Tier::kHigh), &f);
    CheckFrame(f, NightInput(&table, nullptr, false, cx, cz, 12.0f, rw::Tier::kHigh), "day");
    CHECK(f.lamp_count == 0 && f.stats.kept == 0 && f.words.size() == size_t(f.lamp_row_base) * 4);
  }

  // night: the lamps within the radius that shine, brute force
  const auto brute = [&](const ch::LampTable& t, const std::vector<rw::LiveLamp>* live, bool fresh, float hour, float radius, uint32_t* from_table) {
    std::set<std::array<float, 3>> positions;
    const ch::LampHours hours = t.hours();
    const rw::Schedule schedule = rw::ScheduleAt(hours, hour, -1.0f);
    uint32_t count = 0;
    for (const ch::Lamp& l : t.lamps()) {
      const double dx = double(l.pos[0]) - cx, dy = double(l.pos[1]) - 3.0, dz = double(l.pos[2]) - cz;
      if (std::sqrt(dx * dx + dy * dy + dz * dz) > radius) continue;
      if (fresh && l.source == ch::LampSource::kStreetLamp && std::sqrt(dx * dx + dy * dy + dz * dz) <= 300.0) continue;
      if (!(rw::ActivityOf(l, schedule, hours, hour) > 0.0f)) continue;
      positions.insert({l.pos[0], l.pos[1], l.pos[2]});
      ++count;
    }
    *from_table = count;
    return positions;
  };
  for (const float hour : {23.0f, 2.0f, 17.8f, 18.1f, 5.9f, 6.2f}) {
    rw::LampFrame f;
    const rw::LampInput in = NightInput(&table, nullptr, false, cx, cz, hour, rw::Tier::kHigh);
    rw::BuildLampFrame(in, &f);
    CheckFrame(f, in, "night");
    uint32_t expected = 0;
    const auto positions = brute(table, nullptr, false, hour, in.budget.radius_m, &expected);
    // the window may be smaller than the radius' circle (square cells): a lamp inside the radius is in the window when the window covers [cam - R, cam + R]
    CHECK(f.stats.from_table == expected);
    CHECK(f.stats.kept + f.stats.over_budget + f.stats.outside_window == expected);
    CHECK(f.stats.kept <= in.budget.max_lamps);
    CHECK(f.stats.outside_window == 0);  // High: 320 m = 11 cells at most
    std::set<std::array<float, 3>> got;
    for (uint32_t i = 0; i < f.lamp_count; ++i) {
      const cs::Lamp l = RecordLamp(f, i);
      got.insert({float(l.position.x + float(in.camera[0])), 0.0f, float(l.position.z + float(in.camera[2]))});
    }
    CHECK(got.size() == f.lamp_count || f.lamp_count - got.size() <= 2);  // distinct places (two lamps on one spot are one)
    CHECK(positions.size() == expected || positions.size() + 2 >= expected);
  }

  // the colour holds the activity over the schedule: a fill light at dusk, a street lamp on
  {
    const float hour = 17.7f;
    rw::LampFrame f;
    const rw::LampInput in = NightInput(&table, nullptr, false, cx, cz, hour, rw::Tier::kHigh);
    rw::BuildLampFrame(in, &f);
    CHECK(in.schedule.factor > 0.0f && in.schedule.factor < 1.0f);
    bool fill_seen = false, street_seen = false;
    for (uint32_t i = 0; i < f.lamp_count; ++i) {
      const cs::Lamp l = RecordLamp(f, i);
      const bool spot = l.cos_outer > -1.5f;
      // find the table lamp by position (view + camera)
      const float wx = l.position.x + float(in.camera[0]), wy = l.position.y + float(in.camera[1]), wz = l.position.z + float(in.camera[2]);
      const ch::Lamp* match = nullptr;
      for (const ch::Lamp& t : table.lamps()) {
        if (std::fabs(t.pos[0] - wx) < 1e-2f && std::fabs(t.pos[1] - wy) < 1e-2f && std::fabs(t.pos[2] - wz) < 1e-2f) {
          match = &t;
          break;
        }
      }
      CHECK(match != nullptr);
      if (match == nullptr) continue;
      const float activity = rw::ActivityOf(*match, in.schedule, table.hours(), hour);
      CHECK(activity > 0.0f);
      for (int c = 0; c < 3; ++c) CHECK(Near(Comp(l.colour, c), match->colour[c] * activity / in.schedule.factor, 1e-5));
      // x the schedule gives the activity back, as the library multiplies
      for (int c = 0; c < 3; ++c) CHECK(Near(Comp(l.colour, c) * in.schedule.factor, match->colour[c] * activity, 1e-5));
      CHECK(spot == (match->source == ch::LampSource::kStreetLamp));
      fill_seen = fill_seen || !spot;
      street_seen = street_seen || spot;
    }
    CHECK(fill_seen && street_seen);
  }

  // the live list: fresh, it replaces the table's street lamps in its reach; the lamps beyond its reach stay
  {
    const float hour = 23.0f;
    std::vector<rw::LiveLamp> live;
    for (int i = 0; i < 70; ++i) live.push_back(MakeLive(rng, float(cx), float(cz), 280.0f, rng.Range(0.3f, 1.0f)));
    for (const bool fresh : {true, false}) {
      rw::LampFrame f;
      const rw::LampInput in = NightInput(&table, &live, fresh, cx, cz, hour, rw::Tier::kUltra);
      rw::BuildLampFrame(in, &f);
      CheckFrame(f, in, fresh ? "live fresh" : "live stale");
      CHECK(f.stats.from_live == 70 && f.stats.live_given == 70);
      uint32_t table_street_near = 0, table_street_far = 0;
      for (const ch::Lamp& l : table.lamps()) {
        if (l.source != ch::LampSource::kStreetLamp) continue;
        const double dx = double(l.pos[0]) - cx, dy = double(l.pos[1]) - 3.0, dz = double(l.pos[2]) - cz;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= in.budget.radius_m) (d <= 300.0 ? table_street_near : table_street_far) += 1;
      }
      // from_table counts the fill lights, the street lamps beyond 300 m, and (stale) those within it
      uint32_t table_fill = 0;
      for (const ch::Lamp& l : table.lamps()) {
        if (l.source == ch::LampSource::kStreetLamp) continue;
        const double dx = double(l.pos[0]) - cx, dy = double(l.pos[1]) - 3.0, dz = double(l.pos[2]) - cz;
        table_fill += std::sqrt(dx * dx + dy * dy + dz * dz) <= in.budget.radius_m ? 1u : 0u;
      }
      CHECK(f.stats.from_table == table_fill + table_street_far + (fresh ? 0u : table_street_near));
      // the live lamps carry their fade as their activity: the colour's share of the schedule (1 at night)
      CHECK(in.schedule.factor == 1.0f);
    }
    // the order of the game's list does not change a byte
    rw::LampFrame a, b;
    const rw::LampInput in = NightInput(&table, &live, true, cx, cz, hour, rw::Tier::kHigh);
    rw::BuildLampFrame(in, &a);
    std::vector<rw::LiveLamp> shuffled = live;
    for (size_t i = shuffled.size(); i > 1; --i) std::swap(shuffled[i - 1], shuffled[rng.Below(uint32_t(i))]);
    rw::LampInput in2 = in;
    in2.live = &shuffled;
    rw::BuildLampFrame(in2, &b);
    CHECK(a.words == b.words);
    rw::LampFrame c;
    rw::BuildLampFrame(in, &c);
    CHECK(a.words == c.words);  // the same input, the same words
    // a live lamp that is off, has no fade, or no intensity is not uploaded
    std::vector<rw::LiveLamp> dead = live;
    dead[0].lamp.on = 0;
    dead[1].scale = 0.0f;
    dead[2].lamp.intensity = 0.0f;
    rw::LampInput in3 = in;
    in3.live = &dead;
    rw::LampFrame d;
    rw::BuildLampFrame(in3, &d);
    CHECK(d.stats.from_live == 67 && d.stats.inactive >= 3);
    // a live lamp's colour is colour x its fade (at a schedule of 1)
    rw::LampFrame e;
    std::vector<rw::LiveLamp> one(1, MakeLive(rng, float(cx), float(cz), 50.0f, 0.5f));
    rw::LampInput in4 = NightInput(nullptr, &one, true, cx, cz, hour, rw::Tier::kHigh);
    rw::BuildLampFrame(in4, &e);
    CHECK(e.lamp_count == 1);
    if (e.lamp_count == 1) {
      const cs::Lamp l = RecordLamp(e, 0);
      CHECK(Near(l.colour.x, one[0].lamp.colour[0] * 0.5f, 1e-6) && Near(l.colour.y, one[0].lamp.colour[1] * 0.5f, 1e-6));
    }
  }

  // no table at all (the pack has no lamps): the live list alone; nothing at all: empty
  {
    std::vector<rw::LiveLamp> live;
    for (int i = 0; i < 20; ++i) live.push_back(MakeLive(rng, 0.0f, 0.0f, 100.0f, 1.0f));
    rw::LampFrame f;
    rw::BuildLampFrame(NightInput(nullptr, &live, true, 0.0, 0.0, 22.0f, rw::Tier::kMid), &f);
    CHECK(f.lamp_count == 20);
    rw::LampFrame g;
    rw::BuildLampFrame(NightInput(nullptr, nullptr, false, 0.0, 0.0, 22.0f, rw::Tier::kMid), &g);
    CHECK(g.lamp_count == 0 && g.rows() == g.lamp_row_base);
  }

  // the budget keeps the NEAREST lamps
  {
    rw::LampInput in = NightInput(&table, nullptr, false, cx, cz, 23.0f, rw::Tier::kUltra);
    rw::LampFrame full;
    rw::BuildLampFrame(in, &full);
    CHECK(full.stats.kept > 60 && full.stats.over_budget == 0);
    in.budget.max_lamps = 40;
    rw::LampFrame cut;
    rw::BuildLampFrame(in, &cut);
    CheckFrame(cut, in, "budget");
    CHECK(cut.lamp_count == 40 && cut.stats.over_budget == full.stats.kept - 40);
    // distances of the kept 40 against the full set's 40 nearest
    std::vector<float> all_d, kept_d;
    for (uint32_t i = 0; i < full.lamp_count; ++i) {
      const cs::Lamp l = RecordLamp(full, i);
      all_d.push_back(l.position.x * l.position.x + (l.position.y - 0.0f) * (l.position.y - 0.0f) + l.position.z * l.position.z);
    }
    for (uint32_t i = 0; i < cut.lamp_count; ++i) {
      const cs::Lamp l = RecordLamp(cut, i);
      kept_d.push_back(l.position.x * l.position.x + l.position.y * l.position.y + l.position.z * l.position.z);
    }
    std::sort(all_d.begin(), all_d.end());
    std::sort(kept_d.begin(), kept_d.end());
    bool nearest = true;
    for (size_t i = 0; i < 40; ++i) nearest = nearest && std::fabs(all_d[i] - kept_d[i]) < 1e-2f * std::max(1.0f, all_d[i]);
    CHECK(nearest);
    // the tiers
    CHECK(rw::BudgetFor(rw::Tier::kMid).max_lamps < rw::BudgetFor(rw::Tier::kHigh).max_lamps && rw::BudgetFor(rw::Tier::kHigh).max_lamps < rw::BudgetFor(rw::Tier::kUltra).max_lamps);
    CHECK(rw::BudgetFor(rw::Tier::kMid).radius_m < rw::BudgetFor(rw::Tier::kHigh).radius_m && rw::BudgetFor(rw::Tier::kHigh).radius_m < rw::BudgetFor(rw::Tier::kUltra).radius_m);
    CHECK(rw::BudgetFor(rw::Tier::kUltra).radius_m * 2.0f / rw::kCellMetres + 2.0f <= float(rw::kMaxWindowAxis));  // Ultra's circle fits the window
    CHECK(rw::BudgetFor(rw::Tier::kUltra).max_lamps <= rw::kMaxLampsHard);
  }

  // the window is capped at 16 cells an axis, centred on the camera's cell; lamps outside are counted
  {
    rw::LampInput in = NightInput(&table, nullptr, false, cx, cz, 23.0f, rw::Tier::kUltra);
    in.budget.radius_m = 5000.0f;
    in.budget.max_lamps = 2048;
    rw::LampFrame f;
    rw::BuildLampFrame(in, &f);
    CheckFrame(f, in, "wide");
    CHECK(f.nx == 16 && f.nz == 16);
    const int32_t camera_cx = int32_t(std::floor(cx / 64.0)), camera_cz = int32_t(std::floor(cz / 64.0));
    CHECK(f.cx0 <= camera_cx && camera_cx < f.cx0 + 16 && f.cz0 <= camera_cz && camera_cz < f.cz0 + 16);
    CHECK(f.stats.outside_window > 0);
  }

  // a camera far from every lamp: an empty window
  {
    rw::LampFrame f;
    rw::LampInput in = NightInput(&table, nullptr, false, 50000.0, 50000.0, 23.0f, rw::Tier::kHigh);
    rw::BuildLampFrame(in, &f);
    CHECK(f.lamp_count == 0);
    CheckFrame(f, in, "far");
  }
  // the fill-only table (no street lamps in it): the live list is what the street has
  {
    std::vector<rw::LiveLamp> live;
    for (int i = 0; i < 64; ++i) live.push_back(MakeLive(rng, float(cx), float(cz), 280.0f, 1.0f));
    rw::LampFrame f;
    rw::BuildLampFrame(NightInput(&fill_only, &live, true, cx, cz, 23.0f, rw::Tier::kHigh), &f);
    CHECK(f.stats.from_live == 64);
    uint32_t spots = 0;
    for (uint32_t i = 0; i < f.lamp_count; ++i) spots += RecordLamp(f, i).cos_outer > -1.5f ? 1u : 0u;
    CHECK(spots == 64);
  }
  // an unknown hour: the hint gates (a hint of 0 makes the lamps vanish)
  {
    rw::LampInput in = NightInput(&table, nullptr, false, cx, cz, -1.0f, rw::Tier::kHigh);
    in.schedule = rw::ScheduleAt(table.hours(), -1.0f, 0.0f);
    rw::LampFrame f;
    rw::BuildLampFrame(in, &f);
    CHECK(f.lamp_count == 0);
    in.schedule = rw::ScheduleAt(table.hours(), -1.0f, 0.8f);
    rw::BuildLampFrame(in, &f);
    CHECK(f.lamp_count > 0);
    CheckFrame(f, in, "hint");
  }
}

// ---------------------------------------------------------------------------
// the world rows
// ---------------------------------------------------------------------------

rw::WorldInput RandomWorld(Lcg& rng, const rw::LampFrame* lamps) {
  rw::WorldInput in;
  in.frame = (uint64_t(rng.U32()) << 20) ^ rng.U32();
  in.hit.tuning.enabled = rng.Below(8) != 0;
  in.hit.tuning.shadow = rng.Below(2);
  in.hit.tuning.cascade = rng.Below(2);
  in.hit.tuning.ambient = rng.Range(0.5f, 1.5f);
  in.hit.tuning.spec = rng.Range(0.5f, 1.5f);
  in.hit.tuning.gain = rng.Range(0.5f, 1.5f);
  in.hit.light.valid = rng.Below(8) != 0;
  for (int c = 0; c < 3; ++c) {
    in.hit.light.sun[c] = rng.Range(0.0f, 6.0f);
    in.hit.light.ambient[c] = rng.Range(0.0f, 0.5f);
    in.hit.light.sky[c] = rng.Range(0.0f, 1.0f);
  }
  in.hit.light.env_sharp = rng.Below(3) ? 0x80000000u | rng.Below(4000) : 0u;
  in.hit.light.env_blur = rng.Below(4000);
  in.hit.camera_known = true;
  in.hit.sun_known = rng.Below(4) != 0;
  for (int c = 0; c < 3; ++c) {
    in.hit.camera_world[c] = rng.Range(-3000.0f, 3000.0f);
    in.camera[c] = in.hit.camera_world[c];
    in.hit.to_sun_view[c] = rng.Range(-1.0f, 1.0f);
    in.to_sun_world[c] = rng.Range(-1.0f, 1.0f);
    for (int k = 0; k < 3; ++k) in.hit.world_axes_view[k][c] = rng.Range(-1.0f, 1.0f);
  }
  in.camera_known = rng.Below(8) != 0;
  in.sun_world_known = rng.Below(4) != 0;
  in.hit.cascades.valid = rng.Below(2);
  in.hit.cascades.atlas_slot = rng.Below(1000);
  in.hit.cascades.count = 1 + rng.Below(4);
  in.hit.cascades.fade = rng.Range(0.0f, 1.0f);
  in.hit.cascades.depth_bias = rng.Range(0.0f, 0.01f);
  for (int c = 0; c < 3; ++c) {
    in.hit.cascades.right[c] = rng.Range(-1.0f, 1.0f);
    in.hit.cascades.top[c] = rng.Range(-1.0f, 1.0f);
  }
  for (uint32_t i = 0; i < rh::kMaxCascades; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int c = 0; c < 4; ++c) in.hit.cascades.rows[i][j][c] = rng.Range(-2.0f, 2.0f);
      in.hit.cascades.box[i][j] = rng.Range(1.0f, 100.0f);
    }
  }
  in.hour = rng.Range(0.0f, 24.0f);
  in.weather = rng.Below(5) == 0 ? rw::kUnknownWeather : rng.Below(4);
  in.units = rng.Below(2) ? 16.0f : rng.Range(1.0f, 20.0f);
  in.wet = rng.Range(0.0f, 1.0f);
  in.time_s = rng.Range(0.0f, 4096.0f);
  in.world_light = rng.Below(2);
  in.car = rng.Below(2);
  in.live_fresh = rng.Below(2);
  in.stats_frame = rng.Below(2);
  in.calibration_windows = rng.Below(1000);
  in.world_kind = rng.Below(5);
  in.schedule = rw::ScheduleAt(ch::LampHours{}, rng.Below(6) == 0 ? -1.0f : in.hour, rng.Range(0.0f, 1.0f));
  in.schedule.evidence = rng.Below(4) == 0 ? rng.Below(3) : 0u;
  in.live_shining = rng.Below(3) == 0 ? rng.Below(90) : 0u;
  in.lamps = lamps;
  for (uint32_t c = 0; c < rw::kClasses; ++c) in.gains[c] = rng.Range(0.5f, 3.0f);
  return in;
}

void TestWorldRows() {
  Lcg rng;
  const std::vector<ch::Lamp> all = MakeLamps(rng, 600, 200, 400.0f, true);
  const ch::LampTable table = ch::LampTable::Build(all);
  rw::LampFrame lamps;
  rw::BuildLampFrame(NightInput(&table, nullptr, false, 0.0, 0.0, 22.0f, rw::Tier::kHigh), &lamps);
  for (int round = 0; round < 600; ++round) {
    const bool with_lamps = round % 5 != 0;
    const rw::WorldInput in = RandomWorld(rng, with_lamps ? &lamps : nullptr);
    uint32_t rows[rw::kTotalRows][4];
    rw::MakeWorldRows(in, rows);
    // rows 0..31 are the hit frame exactly
    uint32_t hit_rows[rh::kFrameRows][4];
    rh::MakeFrameRows(in.hit, hit_rows);
    CHECK(std::memcmp(rows, hit_rows, sizeof(hit_rows)) == 0);
    // the reserved rows are zero
    bool zero = true;
    for (uint32_t r = 44; r < rw::kRowSchedule; ++r) for (int c = 0; c < 4; ++c) zero = zero && rows[r][c] == 0;
    CHECK(zero);
    rw::WorldDecoded d = rw::DecodeWorldRows(rows);
    CHECK(d.Valid());
    CHECK(d.serial == uint32_t(in.frame & 0xFFFFFFFFu));
    CHECK(((d.flags & rw::kWorldHour) != 0) == in.schedule.from_hour);
    CHECK(((d.flags & rw::kWorldCameraKnown) != 0) == in.camera_known);
    CHECK(((d.flags & rw::kWorldSunWorld) != 0) == in.sun_world_known);
    CHECK(((d.flags & rw::kWorldLiveFresh) != 0) == in.live_fresh);
    CHECK(((d.flags & rw::kWorldLamps) != 0) == with_lamps);
    CHECK(((d.flags & rw::kWorldStatsFrame) != 0) == in.stats_frame);
    CHECK(((d.flags & rw::kWorldCar) != 0) == in.car);
    CHECK(((d.flags & rw::kWorldLight) != 0) == in.world_light);
    CHECK(in.schedule.from_hour ? d.hour == in.hour : d.hour == -1.0f);
    CHECK(d.lamp_factor == in.schedule.factor);
    CHECK(d.street == std::clamp(in.schedule.street, 0.0f, 1.0f) && d.fill == std::clamp(in.schedule.fill, 0.0f, 1.0f) && d.live_shining == in.live_shining && d.evidence == in.schedule.evidence);
    CHECK(d.GlowFactor(ch::kGlowAlways) == 1.0f && d.GlowFactor(ch::kGlowLens) == d.street && d.GlowFactor(ch::kGlowNight) == d.fill && d.GlowFactor(3) == d.fill);
    CHECK(d.units == in.units && d.wet == in.wet && d.weather == in.weather && d.time_s == in.time_s);
    for (int c = 0; c < 3; ++c) {
      CHECK(d.camera[c] == (in.camera_known ? in.camera[c] : 0.0f));
      CHECK(d.to_sun_world[c] == (in.sun_world_known ? in.to_sun_world[c] : 0.0f));
    }
    CHECK(d.sun_night == rh::NightFactor(in.hit.light.sun));
    CHECK(d.cell_m == rw::kCellMetres && d.lookup_m == rw::kDefaultLookupMetres && d.origin_x == 0.0f && d.origin_z == 0.0f);
    if (with_lamps) {
      CHECK(d.cx0 == lamps.cx0 && d.cz0 == lamps.cz0 && d.nx == lamps.nx && d.nz == lamps.nz);
      CHECK(d.lamp_count == lamps.lamp_count && d.cells == lamps.cells() && d.live_count == lamps.stats.live_given);
      CHECK(d.lamps_in_window == lamps.stats.kept + lamps.stats.over_budget);
    } else {
      CHECK(d.nx == 0 && d.nz == 0 && d.lamp_count == 0 && d.cells == 0);
    }
    CHECK(d.stats_frame == (in.stats_frame ? 1u : 0u) && d.windows == in.calibration_windows && d.world_kind == in.world_kind);
    for (uint32_t c = 0; c < rw::kClasses; ++c) CHECK(d.gains[c] == in.gains[c]);

    // the shader's loader over the same rows
    hlsl_shim::StructuredBuffer<hlsl_shim::uint4> buffer;
    std::vector<hlsl_shim::uint4> storage(rw::kTotalRows);
    for (uint32_t r = 0; r < rw::kTotalRows; ++r) storage[r] = hlsl_shim::uint4(rows[r][0], rows[r][1], rows[r][2], rows[r][3]);
    buffer.data = storage.data();
    buffer.count = storage.size();
    hlsl_shim::gRtWorld = buffer;
    const hlsl_shim::RtWorld w = hlsl_shim::RtWorldLoad();
    CHECK(w.serial == d.serial && w.flags == d.flags && w.hour == d.hour && w.lampFactor == d.lamp_factor);
    CHECK(w.units == d.units && w.wet == d.wet && w.weather == d.weather && w.time == d.time_s);
    CHECK(w.camera.x == d.camera[0] && w.camera.y == d.camera[1] && w.camera.z == d.camera[2] && w.sunNight == d.sun_night);
    CHECK(w.toSunWorld.x == d.to_sun_world[0] && w.toSunWorld.y == d.to_sun_world[1] && w.toSunWorld.z == d.to_sun_world[2]);
    CHECK(w.cellM == d.cell_m && w.lookupM == d.lookup_m && w.originX == d.origin_x && w.originZ == d.origin_z);
    CHECK(w.cx0 == d.cx0 && w.cz0 == d.cz0 && w.nx == d.nx && w.nz == d.nz);
    CHECK(w.lampCount == d.lamp_count && w.lampsInWindow == d.lamps_in_window && w.cells == d.cells && w.liveCount == d.live_count);
    CHECK(w.statsFrame == d.stats_frame && w.windows == d.windows && w.worldKind == d.world_kind);
    CHECK(w.street == d.street && w.fill == d.fill && w.liveShining == d.live_shining && w.evidence == d.evidence);
    for (uint32_t kind = 0; kind < 4; ++kind) CHECK(hlsl_shim::RtWorldGlowFactor(kind) == d.GlowFactor(kind));
    CHECK(w.axisX.x == in.hit.world_axes_view[0][0] && w.axisX.y == in.hit.world_axes_view[0][1] && w.axisX.z == in.hit.world_axes_view[0][2]);
    CHECK(w.axisY.x == in.hit.world_axes_view[1][0] && w.axisZ.z == in.hit.world_axes_view[2][2]);
    for (uint32_t cls = 0; cls < rw::kClasses; ++cls) CHECK(hlsl_shim::RtWorldClassGain(cls) == in.gains[cls]);
    CHECK(hlsl_shim::RtWorldClassGain(99) == in.gains[rw::kClasses - 1]);  // out of range: the last class
    // the hit frame: RtHitLoadFrame (RT_HIT_FRAME_ROW is gRtWorld here) and RtWorldHitFrame say the same, and what the rows hold
    const hlsl_shim::RtHitFrame f = hlsl_shim::RtWorldHitFrame();
    const hlsl_shim::RtHitFrame g = hlsl_shim::RtHitLoadFrame();
    CHECK(f.flags == g.flags && f.flags == hit_rows[0][3]);
    CHECK(f.toSun.x == g.toSun.x && f.sun.y == g.sun.y && f.ambient.z == g.ambient.z && f.envSharp == g.envSharp && f.envBlur == g.envBlur && f.night == g.night && f.sky.x == g.sky.x &&
          f.axisY.y == g.axisY.y && f.gain == g.gain && f.specGain == g.specGain && f.shadowReach == g.shadowReach && f.ambientGain == g.ambientGain);
    CHECK(f.sun.x == in.hit.light.sun[0] && f.envSharp == in.hit.light.env_sharp && f.ambientGain == in.hit.tuning.ambient && f.specGain == in.hit.tuning.spec && f.gain == in.hit.tuning.gain);
    CHECK(f.sky.y == in.hit.light.sky[1] && f.axisZ.x == in.hit.world_axes_view[2][0] && f.night == rh::NightFactor(in.hit.light.sun));
    // the point in the world: the camera plus the point along the world's axes (the twin's ViewToWorld and the header's WorldPoint)
    const float view[3] = {rng.Range(-300.0f, 300.0f), rng.Range(-30.0f, 30.0f), rng.Range(0.0f, 500.0f)};
    const hlsl_shim::float3 p = hlsl_shim::RtWorldPoint(w, hlsl_shim::float3(view[0], view[1], view[2]));
    float twin[3], header[3];
    rw::ViewToWorld(d, in.hit.world_axes_view, view, twin);
    rh::WorldPoint(d.camera, in.hit.world_axes_view, view, header);
    CHECK(p.x == twin[0] && p.y == twin[1] && p.z == twin[2]);
    CHECK(Near(twin[0], header[0], 1e-6) && Near(twin[1], header[1], 1e-6) && Near(twin[2], header[2], 1e-6));
  }
  // a buffer that was not filled reads as not valid
  {
    std::vector<hlsl_shim::uint4> zero(rw::kTotalRows);
    hlsl_shim::StructuredBuffer<hlsl_shim::uint4> buffer;
    buffer.data = zero.data();
    buffer.count = zero.size();
    hlsl_shim::gRtWorld = buffer;
    const hlsl_shim::RtWorld w = hlsl_shim::RtWorldLoad();
    CHECK((w.flags & RT_WORLD_VALID) == 0u);
    CHECK(w.lampCount == 0u && w.nx == 0u);
    std::vector<hlsl_shim::uint4> no_lamps(4);
    hlsl_shim::StructuredBuffer<hlsl_shim::uint4> lb;
    lb.data = no_lamps.data();
    lb.count = no_lamps.size();
    hlsl_shim::gRtLamps = lb;
    CHECK(hlsl_shim::RtLampBegin(w, hlsl_shim::float3(1.0f, 2.0f, 3.0f), 64.0f).done == 1u);
  }
}

// ---------------------------------------------------------------------------
// the lookup
// ---------------------------------------------------------------------------

// the brute force: the window's cells that overlap the square [p - r, p + r], clamped to kMaxLookupAxis an axis, z outer, x inner, a cell's lamps in order
std::vector<uint32_t> BruteCells(const rw::LampFrame& f, const rw::WorldDecoded& d, const float p[3], float radius) {
  std::vector<uint32_t> out;
  const float r = std::clamp(radius, 0.0f, rw::kMaxLookupCells * d.cell_m);
  int32_t x0 = int32_t(std::floor((p[0] - r) / d.cell_m)) - f.cx0, x1 = int32_t(std::floor((p[0] + r) / d.cell_m)) - f.cx0;
  int32_t z0 = int32_t(std::floor((p[2] - r) / d.cell_m)) - f.cz0, z1 = int32_t(std::floor((p[2] + r) / d.cell_m)) - f.cz0;
  x0 = std::max(x0, 0);
  z0 = std::max(z0, 0);
  x1 = std::min(x1, int32_t(f.nx) - 1);
  z1 = std::min(z1, int32_t(f.nz) - 1);
  x1 = std::min(x1, x0 + int32_t(rw::kMaxLookupAxis) - 1);
  z1 = std::min(z1, z0 + int32_t(rw::kMaxLookupAxis) - 1);
  for (int32_t cz = z0; cz <= z1; ++cz) {
    for (int32_t cx = x0; cx <= x1; ++cx) {
      uint32_t first = 0, count = 0;
      rw::CellRange(f.words.data(), f.nx, cx, cz, &first, &count);
      for (uint32_t i = 0; i < count; ++i) out.push_back(first + i);
    }
  }
  return out;
}

void TestLookup() {
  Lcg rng;
  const std::vector<ch::Lamp> all = MakeLamps(rng, 4500, 1500, 1200.0f, true);
  const ch::LampTable table = ch::LampTable::Build(all);
  int nonempty = 0, total_found = 0;
  for (int scene = 0; scene < 6; ++scene) {
    // a camera, a yaw, and the world's axes in that view
    const double camera[3] = {double(rng.Range(-500.0f, 500.0f)), double(rng.Range(0.0f, 40.0f)), double(rng.Range(-500.0f, 500.0f))};
    const float yaw = rng.Range(0.0f, 6.2831853f), pitch = rng.Range(-0.4f, 0.4f);
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    // R = Rx(pitch) * Ry(yaw): world axes in view space (rows are the axes)
    const float axes[3][3] = {{cy, 0.0f, -sy}, {sy * sp, cp, cy * sp}, {sy * cp, -sp, cy * cp}};
    rw::LampInput in = NightInput(&table, nullptr, false, camera[0], camera[2], 23.0f, scene % 2 ? rw::Tier::kUltra : rw::Tier::kHigh);
    in.camera[1] = camera[1];
    for (int k = 0; k < 3; ++k) for (int c = 0; c < 3; ++c) in.axes[k][c] = axes[k][c];
    rw::LampFrame f;
    rw::BuildLampFrame(in, &f);
    CHECK(f.lamp_count > 20);
    rw::WorldInput wi;
    wi.camera_known = true;
    for (int c = 0; c < 3; ++c) wi.camera[c] = float(camera[c]);
    wi.schedule = in.schedule;
    wi.lamps = &f;
    for (int k = 0; k < 3; ++k) for (int c = 0; c < 3; ++c) wi.hit.world_axes_view[k][c] = axes[k][c];
    uint32_t rows[rw::kTotalRows][4];
    rw::MakeWorldRows(wi, rows);
    const rw::WorldDecoded d = rw::DecodeWorldRows(rows);
    // the shader's buffers
    std::vector<hlsl_shim::uint4> world_storage(rw::kTotalRows), lamp_storage(f.words.size() / 4);
    for (uint32_t r = 0; r < rw::kTotalRows; ++r) world_storage[r] = hlsl_shim::uint4(rows[r][0], rows[r][1], rows[r][2], rows[r][3]);
    for (size_t r = 0; r < lamp_storage.size(); ++r) lamp_storage[r] = hlsl_shim::uint4(f.words[4 * r], f.words[4 * r + 1], f.words[4 * r + 2], f.words[4 * r + 3]);
    hlsl_shim::StructuredBuffer<hlsl_shim::uint4> wb, lb;
    wb.data = world_storage.data();
    wb.count = world_storage.size();
    lb.data = lamp_storage.data();
    lb.count = lamp_storage.size();
    hlsl_shim::gRtWorld = wb;
    hlsl_shim::gRtLamps = lb;
    const hlsl_shim::RtWorld w = hlsl_shim::RtWorldLoad();

    for (int probe = 0; probe < 400; ++probe) {
      // a point in the view space: somewhere in front of and around the camera, as a hit is
      const float view[3] = {rng.Range(-350.0f, 350.0f), rng.Range(-40.0f, 40.0f), rng.Range(-100.0f, 450.0f)};
      const float radius = probe % 7 == 0 ? 0.0f : (probe % 7 == 1 ? 10.0f : (probe % 7 == 2 ? 100.0f : (probe % 7 == 3 ? 500.0f : (probe % 7 == 4 ? d.lookup_m : rng.Range(1.0f, 130.0f)))));
      std::vector<uint32_t> found;
      rw::LampsNear(d, f.words.data(), axes, view, radius, &found);
      float p[3];
      rw::ViewToWorld(d, axes, view, p);
      const std::vector<uint32_t> expected = BruteCells(f, d, p, radius);
      CHECK(found == expected);
      total_found += int(found.size());
      nonempty += found.empty() ? 0 : 1;
      // no duplicate, every index in range
      std::set<uint32_t> unique(found.begin(), found.end());
      CHECK(unique.size() == found.size());
      bool in_range = true;
      for (const uint32_t i : found) in_range = in_range && i < f.lamp_count;
      CHECK(in_range);
      // complete within the radius (the radius clamped to two cells): every lamp of the window whose world place is inside the circle is found, unless the walk was cut at five cells
      const float rr = std::clamp(radius, 0.0f, rw::kMaxLookupCells * d.cell_m);
      uint32_t missed = 0;
      for (uint32_t i = 0; i < f.lamp_count; ++i) {
        const cs::Lamp l = RecordLamp(f, i);
        // the lamp's world place: the view position turned back (R^T): world = camera + sum_k view_k ... the records are in view space: v = sum_j (pos_j - cam_j) axes[j], so pos_j = cam_j + dot(v, axes[j])
        float lp[3];
        const float lv[3] = {l.position.x, l.position.y, l.position.z};
        rw::ViewToWorld(d, axes, lv, lp);
        const float dx = lp[0] - p[0], dz = lp[2] - p[2];
        if (std::fabs(dx) <= rr * 0.999f && std::fabs(dz) <= rr * 0.999f && unique.count(i) == 0) ++missed;
      }
      CHECK(missed == 0);
      // the shader's cursor walks the same lamps in the same order
      std::vector<uint32_t> walked;
      for (hlsl_shim::RtLampCursor c = hlsl_shim::RtLampBegin(w, hlsl_shim::float3(view[0], view[1], view[2]), radius); c.done == 0u; c = hlsl_shim::RtLampStep(w, c)) {
        walked.push_back(c.index);
        if (walked.size() > 5000) break;
      }
      CHECK(walked == found);
      // a lamp's rows through the shader's loader are the host's
      if (!found.empty()) {
        const uint32_t i = found[rng.Below(uint32_t(found.size()))];
        uint32_t host[3][4];
        rw::LampRows(f.words.data(), i, host);
        bool same = true;
        for (uint32_t k = 0; k < 3; ++k) {
          const hlsl_shim::uint4 row = hlsl_shim::RtLampRow(w, i, k);
          same = same && row.x == host[k][0] && row.y == host[k][1] && row.z == host[k][2] && row.w == host[k][3];
        }
        CHECK(same);
      }
    }
    CHECK(hlsl_shim::gRtLamps.errors == 0 && hlsl_shim::gRtWorld.errors == 0);
  }
  CHECK(nonempty > 400 && total_found > 4000);
}

// ---------------------------------------------------------------------------
// the lamp light through the grid
// ---------------------------------------------------------------------------

void TestLampLight() {
  Lcg rng;
  const std::vector<ch::Lamp> all = MakeLamps(rng, 3000, 1000, 800.0f, true);
  const ch::LampTable table = ch::LampTable::Build(all);
  const double camera[3] = {100.0, 5.0, -80.0};
  rw::LampInput in = NightInput(&table, nullptr, false, camera[0], camera[2], 23.0f, rw::Tier::kUltra);
  in.camera[1] = camera[1];
  rw::LampFrame f;
  rw::BuildLampFrame(in, &f);
  CheckFrame(f, in, "light");
  CHECK(f.lamp_count > 100 && f.stats.over_budget == 0);
  rw::WorldInput wi;
  wi.camera_known = true;
  for (int c = 0; c < 3; ++c) wi.camera[c] = float(camera[c]);
  wi.schedule = in.schedule;
  wi.lamps = &f;
  uint32_t rows[rw::kTotalRows][4];
  rw::MakeWorldRows(wi, rows);
  const rw::WorldDecoded d = rw::DecodeWorldRows(rows);
  const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::vector<cs::Lamp> every;
  for (uint32_t i = 0; i < f.lamp_count; ++i) every.push_back(RecordLamp(f, i));
  double worst_missing = 0.0;
  int equal_at_100 = 0, strictly_less_at_64 = 0, points = 0;
  for (int probe = 0; probe < 1500; ++probe) {
    // a hit inside the window: a point on a wall or the road within the 320 m the lamps were taken in
    const float view[3] = {rng.Range(-200.0f, 200.0f), rng.Range(-5.0f, 15.0f), rng.Range(-200.0f, 200.0f)};
    cs::V3 normal{rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f)};
    normal = cs::Normalize(normal);
    const cs::V3 position{view[0], view[1], view[2]};
    const float factor = in.schedule.factor;
    const cs::V3 full = cs::LampsSum(factor, every.data(), uint32_t(every.size()), position, normal);
    for (const float radius : {64.0f, 100.0f}) {
      std::vector<uint32_t> found;
      rw::LampsNear(d, f.words.data(), axes, view, radius, &found);
      std::vector<cs::Lamp> through;
      for (const uint32_t i : found) through.push_back(every[i]);
      const cs::V3 grid = cs::LampsSum(factor, through.data(), uint32_t(through.size()), position, normal);
      // the grid takes a subset of the lamps, every term is non-negative: never more than everything
      CHECK(grid.x <= full.x * (1.0f + 1e-5f) + 1e-6f && grid.y <= full.y * (1.0f + 1e-5f) + 1e-6f && grid.z <= full.z * (1.0f + 1e-5f) + 1e-6f);
      if (radius == 100.0f) {
        // every lamp of the game reaches 100 m at most (sqrt(100 I) of I <= 100): the lookup of 100 m is exact
        CHECK(Near(grid.x, full.x, 1e-5) && Near(grid.y, full.y, 1e-5) && Near(grid.z, full.z, 1e-5));
        equal_at_100 += 1;
      } else {
        const double lost = double(cs::Luma(full)) - double(cs::Luma(grid));
        if (cs::Luma(full) > 1e-4f) worst_missing = std::max(worst_missing, lost / double(cs::Luma(full)));
        strictly_less_at_64 += lost > 1e-6 ? 1 : 0;
      }
    }
    ++points;
  }
  CHECK(equal_at_100 == points);
  // a lookup of 64 m (one cell) loses, at most, the lamps between 64 and their reach: a few per cent of the light on this layout
  CHECK(worst_missing < 0.2);
  std::printf("lamp light: 64 m lookup loses at most %.1f%% of a point's light (%d of %d points lose any), 100 m exact\n", 100.0 * worst_missing, strictly_less_at_64, points);

  // The limit: the file's brightest lights (up to 470) reach sqrt(100 I) = 216 m, farther than a lookup's two cells (128 m): they are cut there, whatever radius is asked. The grid is still never more
  // than everything, and the strong light is lost only beyond the clamp.
  {
    std::vector<ch::Lamp> strong;
    for (int i = 0; i < 60; ++i) {
      ch::Lamp l;
      l.pos[0] = float(camera[0]) + rng.Range(-300.0f, 300.0f);
      l.pos[1] = 8.0f;
      l.pos[2] = float(camera[2]) + rng.Range(-300.0f, 300.0f);
      l.intensity = 450.0f;
      l.source = ch::LampSource::kFile;
      l.file_index = uint32_t(i);
      strong.push_back(l);
    }
    const ch::LampTable strong_table = ch::LampTable::Build(strong);
    rw::LampInput si = NightInput(&strong_table, nullptr, false, camera[0], camera[2], 23.0f, rw::Tier::kUltra);
    si.camera[1] = camera[1];
    rw::LampFrame sf;
    rw::BuildLampFrame(si, &sf);
    CHECK(sf.lamp_count == 60);
    rw::WorldInput sw;
    sw.camera_known = true;
    for (int c = 0; c < 3; ++c) sw.camera[c] = float(camera[c]);
    sw.schedule = si.schedule;
    sw.lamps = &sf;
    uint32_t srows[rw::kTotalRows][4];
    rw::MakeWorldRows(sw, srows);
    const rw::WorldDecoded sd = rw::DecodeWorldRows(srows);
    std::vector<cs::Lamp> strong_every;
    for (uint32_t i = 0; i < sf.lamp_count; ++i) strong_every.push_back(RecordLamp(sf, i));
    int loses = 0;
    for (int probe = 0; probe < 400; ++probe) {
      const float view[3] = {rng.Range(-150.0f, 150.0f), 0.0f, rng.Range(-150.0f, 150.0f)};
      const cs::V3 position{view[0], view[1], view[2]};
      const cs::V3 up{0.0f, 1.0f, 0.0f};
      const cs::V3 full = cs::LampsSum(si.schedule.factor, strong_every.data(), uint32_t(strong_every.size()), position, up);
      std::vector<uint32_t> found;
      rw::LampsNear(sd, sf.words.data(), axes, view, 500.0f, &found);
      std::vector<cs::Lamp> through;
      for (const uint32_t i : found) through.push_back(strong_every[i]);
      const cs::V3 grid = cs::LampsSum(si.schedule.factor, through.data(), uint32_t(through.size()), position, up);
      CHECK(grid.x <= full.x * (1.0f + 1e-5f) + 1e-6f);
      loses += cs::Luma(full) - cs::Luma(grid) > 1e-6f ? 1 : 0;
    }
    CHECK(loses > 0);  // a limit to know about, not a bug: R7 may raise rt_world::kMaxLookupCells (and the walk's five cells) if the first run says the strong lights matter
  }
}

// ---------------------------------------------------------------------------
// the census, the clock, the live list's mask, the calibration
// ---------------------------------------------------------------------------

void TestMisc() {
  rw::Census c;
  CHECK(rw::CensusText(c, -1.0f, 0.0f, 0, false).empty());
  c.frames = 600;
  c.frames_no_view = 3;
  c.frames_hour = 600;
  c.lamps_sum = 600 * 150;
  c.lamps_max = 212;
  c.bytes_sum = uint64_t(600) * 30000;
  const std::string text = rw::CensusText(c, 22.5f, 1.0f, 150, true);
  CHECK(text.rfind("; ray tracing world frame: 600 frames filled (3 without a view)", 0) == 0);
  CHECK(Has(text, "now 22.50") && Has(text, "212 most") && Has(text, "150 now") && Has(text, "the static table loaded") && Has(text, "29.3 KiB written a frame"));
  CHECK(Has(rw::CensusText(c, 22.5f, 1.0f, 150, false), "not loaded"));

  // the clock the time of day hook publishes
  namespace wc = mcla::gpu::world_clock;
  CHECK(wc::Hour() == -1.0f && wc::Weather() == -1);
  wc::Publish(17.25f, 2);
  CHECK(wc::Hour() == 17.25f && wc::Weather() == 2);
  wc::Publish(24.0f, 4);
  CHECK(wc::Hour() == -1.0f && wc::Weather() == -1);
  wc::Publish(-1.0f, -1);
  CHECK(wc::Hour() == -1.0f && wc::Weather() == -1);
  wc::Publish(0.0f, 0);
  CHECK(wc::Hour() == 0.0f && wc::Weather() == 0);

  // the live list's hook is asked by two readers, neither switches the other off
  namespace vl = mcla::gpu::vol_lamps;
  CHECK(!vl::Wanted());
  vl::SetWanted(true);
  CHECK(vl::Wanted());
  vl::SetWantedBy(vl::kWantedByWorldFrame, true);
  vl::SetWanted(false);
  CHECK(vl::Wanted());  // the world frame still wants it
  vl::SetWantedBy(vl::kWantedByWorldFrame, false);
  CHECK(!vl::Wanted());
  vl::SetWantedBy(vl::kWantedByWorldFrame, true);
  vl::SetWanted(true);
  vl::SetWantedBy(vl::kWantedByWorldFrame, false);
  CHECK(vl::Wanted());
  vl::SetWanted(false);
  CHECK(!vl::Wanted());
  // the game's record as a lamp
  vl::GameLamp g;
  g.pos[0] = 1.0f;
  g.pos[1] = 7.0f;
  g.pos[2] = -3.0f;
  g.colour[0] = 0.9f;
  g.intensity = 70.0f;
  g.cone_deg = 125.0f;
  g.spread_deg = -50.5f;
  g.scale = 0.75f;
  g.type = 1;
  g.on = 1;
  const rw::LiveLamp l = rw::LiveLampOf(g);
  CHECK(l.lamp.source == ch::LampSource::kStreetLamp && l.lamp.spot == 1 && l.lamp.on == 1 && l.lamp.intensity == 70.0f && l.scale == 0.75f && l.lamp.pos[1] == 7.0f && l.lamp.cone_deg == 125.0f);
  g.type = 0;
  g.on = 0;
  CHECK(rw::LiveLampOf(g).lamp.spot == 0 && rw::LiveLampOf(g).lamp.on == 0);

  // the calibration: the cadence's gains follow a window (a model that is a stop too dark moves the gain up half way, 0.5 rate)
  hc::ClassGains gains;
  uint32_t words[hc::kStatsWords] = {};
  const uint32_t cls = 3;
  words[hc::StatsWord(cls, hc::kCountWord)] = 200;
  words[hc::StatsWord(cls, hc::kSeenWord)] = 200 * uint32_t(hc::kLogScale * 5.0f);
  words[hc::StatsWord(cls, hc::kModelWord)] = 200 * uint32_t(hc::kLogScale * 4.0f);
  gains.Add(words);
  CHECK(gains.Close() == 1);
  float table[rw::kClasses];
  gains.Table(table);
  CHECK(Near(table[cls], std::exp2(0.5f), 1e-4) && table[0] == 1.0f && gains.windows == 1);
  rw::WorldInput in;
  std::memcpy(in.gains, table, sizeof(table));
  uint32_t rows[rw::kTotalRows][4];
  rw::MakeWorldRows(in, rows);
  CHECK(rw::DecodeWorldRows(rows).gains[cls] == table[cls]);
}

// ---------------------------------------------------------------------------
// the sources: text checks of the shaders, the descriptor set, the wiring
// ---------------------------------------------------------------------------

size_t CountOf(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
  return n;
}

void TestSources(const std::string& src) {
  const std::string hlsl_dir = src + "/gpu/hlsl/";
  const std::string probe = ReadFile(hlsl_dir + "rt_world_probe_cs.hlsl");
  const std::string hlsli = ReadFile(hlsl_dir + "rt_world_frame.hlsli");
  CHECK(!probe.empty() && !hlsli.empty());
  CHECK(Has(probe, "#define RT_WORLD_SPACE 2\n#include \"rt_world_frame.hlsli\""));
  CHECK(Has(probe, "[numthreads(64, 1, 1)]") && Has(probe, "RtLampBegin(w, probe.xyz, radius)") && Has(probe, "RtLampStep(w, c)"));
  const std::string lamp_light = ReadFile(hlsl_dir + "rt_world_lamp_light_cs.hlsl");
  CHECK(Has(lamp_light, "#include \"rt_world_frame.hlsli\"") && Has(lamp_light, "#include \"city_hit_shade.hlsli\"") && Has(lamp_light, "RtLampBegin(w, position, radius)") && Has(lamp_light, "RtLampStep(w, c)") &&
        Has(lamp_light, "CityHitLampAt(p, lamp, position, normal).radiance") && Has(lamp_light, "p.lampFactor = w.lampFactor;"));
  // the identity tool compiles both reference consumers of the world frame (DXIL, SPIR-V, spirv-val)
  const std::string tool = ReadFile(src + "/../tools/check_rt_hit_identity.py");
  CHECK(Has(tool, "('rt_world_probe_cs', 'rt_world_probe_cs.hlsl', [])") && Has(tool, "('rt_world_lamp_light_cs', 'rt_world_lamp_light_cs.hlsl', [])"));
  CHECK(!Has(hlsli, "RayQuery") && !Has(hlsli, "RaytracingAccelerationStructure"));  // the world frame needs no ray query: a consumer without a top level can read it

  const std::string header = ReadFile(src + "/gpu/rt_world_frame.h");
  const std::string cpp = ReadFile(src + "/gpu/rt_world_frame.cpp");
  CHECK(!header.empty() && !cpp.empty());
  // AddBindings adds exactly kBindingCount ranges, both StructuredBuffers
  {
    const size_t at = cpp.find("RtWorldFrame::Bindings RtWorldFrame::AddBindings");
    const size_t end = cpp.find("\n}\n", at);
    const std::string body = cpp.substr(at, end - at);
    CHECK(CountOf(body, "builder.add") == rw::kBindingCount);
    CHECK(Has(body, "builder.addStructuredBuffer(world_register)") && Has(body, "builder.addStructuredBuffer(lamps_register)"));
  }
  CHECK(Has(header, "static constexpr uint32_t kWorldRegister = rt_world::kWorldRegister;") && Has(header, "static constexpr uint32_t kBindingCount = rt_world::kBindingCount;"));

  // the car scene's set: the descriptor count names every module that adds bindings, and the ranges add up
  const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
  const std::string hit_h = ReadFile(src + "/gpu/city_hit_scene.h");
  CHECK(!scene.empty() && !hit_h.empty());
  {
    const size_t begin = scene.find("set_builder_.begin();");
    const size_t end = scene.find("set_builder_.end();", begin);
    CHECK(begin != std::string::npos && end != std::string::npos);
    const std::string ranges = scene.substr(begin, end - begin);
    uint32_t direct = 0;
    for (size_t at = ranges.find("set_builder_.add"); at != std::string::npos; at = ranges.find("set_builder_.add", at + 1)) ++direct;
    uint32_t hit_bindings = 0;
    {
      const size_t k = hit_h.find("static constexpr uint32_t kBindingCount = ");
      CHECK(k != std::string::npos);
      hit_bindings = uint32_t(std::strtoul(hit_h.c_str() + k + std::strlen("static constexpr uint32_t kBindingCount = "), nullptr, 10));
    }
    CHECK(Has(ranges, "CityHitScene::AddBindings(set_builder_)") && Has(ranges, "RtWorldFrame::AddBindings(set_builder_)"));
    CHECK(direct == 6 && hit_bindings == 4);
    const size_t call = scene.find("device.CreateDescriptorSet(set_builder_, ", end);
    CHECK(call != std::string::npos);
    const std::string line = scene.substr(call, scene.find('\n', call) - call);
    CHECK(Has(line, "6 + CityHitScene::kBindingCount + RtWorldFrame::kBindingCount, 0, \"car rt set\""));
    CHECK(direct + hit_bindings + rw::kBindingCount == 12);
  }
  // the two top level paths tell the world frame the view and bind its buffers, and the order is: the hit data, then the world's
  {
    CHECK(CountOf(scene, "world_->NoteView(") == 2 && CountOf(scene, "world_->Bind(res.set.get(), world_bindings_, slot);") == 2);
    CHECK(Has(scene, "world_->NoteView(frame.view, flip_z, /*car=*/true);") && Has(scene, "world_->NoteView(view, flip_z, /*car=*/false);"));
    const size_t first = scene.find("world_->NoteView(frame.view");
    CHECK(scene.rfind("hit_->Bind(res.set.get(), hit_bindings_, slot);", first) != std::string::npos);
  }
  // the scene: the frame's start, the cascades, the fill right after the top level, before the probe field
  {
    const std::string sc = ReadFile(src + "/gpu/scene.cpp");
    const size_t begin = sc.find("rt_world_->BeginFrame(frame_serial_);");
    const size_t cascades = sc.find("rt_world_->SetSunCascades(cascades);");
    const size_t build = sc.find("car_rt_->OnResolve(record, summary, list, slot_);");
    const size_t fill = sc.find("rt_world_->OnTrigger(slot_);");
    const size_t probe_field = sc.find("gi_integration_->OnResolve(summary, list, slot_);");
    CHECK(begin != std::string::npos && cascades != std::string::npos && build != std::string::npos && fill != std::string::npos && probe_field != std::string::npos);
    CHECK(cascades < build && build < fill && fill < probe_field);
    CHECK(CountOf(sc, "rt_world_->OnTrigger(") == 1 && CountOf(sc, "rt_world_->BeginFrame(") == 1);
    const std::string sh = ReadFile(src + "/gpu/scene.h");
    CHECK(Has(sh, "void SetRtWorldFrame(RtWorldFrame* rt_world)") && Has(sh, "RtWorldFrame* rt_world_ = nullptr;"));
  }
  // the renderer: made, wired, stopped before the owners it points to, in the census line
  {
    const std::string rc = ReadFile(src + "/gpu/renderer.cpp");
    const std::string rh_ = ReadFile(src + "/gpu/renderer.h");
    CHECK(Has(rh_, "RtWorldFrame rt_world_;"));
    CHECK(Has(rc, "rt_world_.Initialize(device_, frames_, world_sources);") && Has(rc, "car_rt_.SetWorldFrame(&rt_world_);") && Has(rc, "scene_.SetRtWorldFrame(&rt_world_);"));
    CHECK(Has(rc, "rt_world_.CensusText()"));
    CHECK(CountOf(rc, "rt_world_.Shutdown();") == 2);
    size_t at = 0;
    for (int i = 0; i < 2; ++i) {
      const size_t down = rc.find("rt_world_.Shutdown();", at);
      CHECK(down != std::string::npos);
      CHECK(down < rc.find("ao_.Shutdown();", down) && down < rc.find("screen_.Shutdown();", down) && down < rc.find("city_rt_.Shutdown();", down));
      at = down + 1;
    }
    const size_t init = rc.find("rt_world_.Initialize(");
    CHECK(rc.rfind("rt_scene_.Initialize(&car_rt_, &city_rt_, &city_hit_);", init) != std::string::npos);
  }
  // the hooks: the time of day hook publishes, the AO collects while asked, the lamp list's mask, the pack accessor
  {
    const std::string tod = ReadFile(src + "/mc_engine/gameplay/time_of_day.cpp");
    CHECK(Has(tod, "#include \"gpu/world_clock.h\"") && Has(tod, "mcla::gpu::world_clock::Publish(ReadF32(base, mgr + kTodCurrent), static_cast<int32_t>(ReadU32(base, kWeatherIndex)));"));
    const size_t mgr = tod.find("g_mgr.store(mgr, std::memory_order_relaxed);");
    const size_t publish = tod.find("world_clock::Publish(");
    const size_t weather = tod.find("// ── Weather ──", publish);
    CHECK(mgr < publish && publish < weather);
    const std::string ao_h = ReadFile(src + "/gpu/ao.h");
    CHECK(Has(ao_h, "|| census_every_ != 0 || screen_wants_ || world_light_wanted_;") && Has(ao_h, "void SetWorldLightWanted(bool wanted) { world_light_wanted_ = wanted; }") &&
          Has(ao_h, "bool FrameWetness(float* value) const { return PickWet(value); }"));
    const std::string ao_c = ReadFile(src + "/gpu/ao.cpp");
    CHECK(Has(ao_c, "if (contact_mode_ != Mode::kOff || world_light_wanted_) {"));
    CHECK(Has(ReadFile(src + "/gpu/city_rt_scene.h"), "const citypack::PackReader* pack_if_open() const { return pack_.is_open() ? &pack_ : nullptr; }"));
    CHECK(Has(cpp, "src_.ao->SetWorldLightWanted(on);") && Has(cpp, "vol_lamps::SetWantedBy(vol_lamps::kWantedByWorldFrame, on);"));
    CHECK(Has(cpp, "pack->ReadLamps(&table, &error)") && Has(cpp, "src_.city->pack_if_open()"));
  }
  // the hit library keeps the flat range for a consumer that defines CITY_HIT_LAMP_LOAD and takes the world frame's grid for one that defines CITY_HIT_LAMP_GRID (R7)
  {
    const std::string lib = ReadFile(hlsl_dir + "city_hit_shade.hlsli");
    CHECK(Has(lib, "for (uint i = 0u; i < p.lampCount; ++i)") && Has(lib, "#if defined(CITY_HIT_LAMP_GRID)") && Has(lib, "RtLampBegin(w, position, radius)") && Has(lib, "RtLampStep(w, c)") &&
          Has(lib, "CityHitLampAtIndex(p, w, c.index, position, normal)"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "";
  const std::string hlsl = src.empty() ? std::string() : ReadFile(src + "/gpu/hlsl/rt_world_frame.hlsli");
  if (!src.empty()) TestNumbers(hlsl);
  TestSchedule();
  TestRecords();
  TestLampFrame();
  TestWorldRows();
  TestLookup();
  TestLampLight();
  TestMisc();
  if (!src.empty()) TestSources(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
