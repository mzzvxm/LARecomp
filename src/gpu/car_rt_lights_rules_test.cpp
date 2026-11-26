// Check of the other cars' lamps in a ray traced reflection (car_rt_lights_rules.h, LIGHT-UNIFY LU-8): the glow law of each lamp family, the flat lamp set made of the producer's cars,
// the lens's glow from the lamps that stand in its mesh's box (any placement of the car: a property test over rigid motions), the lamps a front or a rear lens ignores, the record the
// shader reads (the driven bit, the scale) and the reference shading's glow with it, the instance list's use of the lamps (AppendOthers: a lens of a walked car and of a drawn car, a
// car with no lamps), the light points' consumers (the ray traced scene's source does not switch the manager's off), and that hlsl/car_rt_hit_shade.hlsli, the host and the settings hold
// the header's numbers. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_lights_rules_test, then run src/gpu/larecomp_car_rt_lights_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default the one this file is in)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "car_rt_instances_rules.h"
#include "car_rt_lights_rules.h"
#include "car_rt_material_rules.h"
#include "setting_source.h"

namespace cl = larecomp::gpu::car_lights;
namespace lp = larecomp::gpu::light_points;
namespace cr = mcla::gpu::car_rt_lights;
namespace crm = mcla::gpu::car_rt_material;
namespace ci = mcla::gpu::car_rt_instances;
namespace tp = mcla::gpu::temporal;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A rigid motion as the game's row-vector matrix (v' = v M): a turn about the y axis, then a move.
tp::Mat4 Rigid(double yaw, double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  const double c = std::cos(yaw), s = std::sin(yaw);
  m.m[0][0] = c;
  m.m[0][2] = -s;
  m.m[2][0] = s;
  m.m[2][2] = c;
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

void World(const tp::Mat4& m, const float p[3], float out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = float(double(p[0]) * m.m[0][c] + double(p[1]) * m.m[1][c] + double(p[2]) * m.m[2][c] + m.m[3][c]);
  }
}

lp::Point Point(uint32_t object, uint32_t kind, uint32_t index, const float pos[3], float intensity, bool owned = true) {
  lp::Point p;
  p.kind = kind;
  p.index = index;
  p.object = object;
  p.vehicle = owned ? object + 0x100u : 0u;
  p.owned = owned;
  for (int c = 0; c < 3; ++c) p.pos[c] = pos[c];
  p.intensity = intensity;
  p.peak = intensity;
  return p;
}

cr::LampSet SetOf(const std::vector<lp::Point>& points) {
  cl::CarLightsFrame frame;
  cl::Build(points, {}, nullptr, &frame);
  cr::LampSet set;
  cr::MakeLampSet(frame, &set);
  return set;
}

cr::LampSet SetOfLamps(const std::vector<cr::LampPoint>& lamps) {
  cr::LampSet set;
  set.lamps = lamps;
  set.cars = 1;
  for (const cr::LampPoint& l : lamps) set.on += l.on ? 1u : 0u;
  return set;
}

cr::LampPoint Lamp(lp::Family family, uint32_t kind, const float world[3], float intensity) {
  cr::LampPoint l;
  for (int c = 0; c < 3; ++c) l.world[c] = world[c];
  l.family = family;
  l.kind = uint8_t(kind);
  l.on = intensity > lp::kOffIntensity;
  l.level = lp::Level(family, intensity);
  return l;
}

constexpr float kLo[3] = {-0.20f, -0.10f, -0.10f};
constexpr float kHi[3] = {0.20f, 0.10f, 0.10f};

}  // namespace

int main(int argc, char** argv) {
  std::string src;
  if (argc > 1) {
    src = argv[1];
  } else {
    std::string self = __FILE__;
    std::replace(self.begin(), self.end(), '\\', '/');
    const size_t slash = self.rfind('/');
    src = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/..";
  }
  std::replace(src.begin(), src.end(), '\\', '/');

  // ---- the law -----------------------------------------------------------------------------------------------------------
  {
    using F = lp::Family;
    Check(cr::GlowOf(F::kHead, 1.0f, false) == 0.0f && cr::GlowOf(F::kBrake, 0.9f, false) == 0.0f && cr::GlowOf(F::kTurn, 1.0f, false) == 0.0f, "a lamp that is off does not glow, whatever its level");
    Check(Near(cr::GlowOf(F::kHead, 1.0f, true), 1.0) && Near(cr::GlowOf(F::kHiBeam, 1.0f, true), 1.0) && Near(cr::GlowOf(F::kFog, 1.0f, true), 1.0), "a headlight, a hi-beam and a fog lamp at full level glow fully");
    Check(Near(cr::GlowOf(F::kTail, 1.0f, true), cr::kTailGlow), "a tail lamp at its full level glows at kTailGlow");
    const float rest = cr::GlowOf(F::kBrake, 0.075f, true);  // the player's reading with the pedal up
    Check(rest > cr::kBrakeRest && rest < 0.4f, "the player's brake lamps with the pedal up (0.075) glow a little over the brake family's rest");
    Check(cr::GlowOf(F::kBrake, 0.0f, true) == cr::kBrakeRest && Near(cr::GlowOf(F::kBrake, 1.0f, true), 1.0), "the brake family goes from its rest glow at level 0 to the full glow at level 1");
    Check(cr::GlowOf(F::kBrake, 1.0f, true) > 2.5f * rest, "the pedal shows: a braking lamp is at least two and a half times a resting one");
    Check(Near(cr::GlowOf(F::kTurn, 1.0f, true), 1.0) && Near(cr::GlowOf(F::kReverse, 1.0f, true), 1.0) && Near(cr::GlowOf(F::kPolice, 1.0f, true), 1.0), "turn, reverse and police lamps glow by their level");
    Check(cr::GlowOf(F::kOther, 1.0f, true) == 0.0f && !cr::LensFamily(F::kOther), "a lamp of no family a lens has is no glow");
    bool monotonic = true, bounded = true;
    for (uint32_t f = 0; f < lp::kFamilyCount; ++f) {
      float last = 0.0f;
      for (int i = 0; i <= 20; ++i) {
        const float g = cr::GlowOf(F(f), float(i) / 20.0f, true);
        monotonic = monotonic && g >= last - 1e-6f;
        bounded = bounded && g >= 0.0f && g <= 1.0f;
        last = g;
      }
    }
    Check(monotonic && bounded, "the glow never falls as the level rises, and stays between 0 and 1");
    Check(cr::GlowOf(F::kHead, 7.0f, true) == 1.0f && cr::GlowOf(F::kBrake, -3.0f, true) == cr::kBrakeRest, "a level outside 0..1 is clamped");
    Check(cr::kBrakeRest > cr::kTailGlow - 0.2f && cr::kTailGlow > 0.0f && cr::kTailGlow < 1.0f, "the tail glow is a lit lamp that is not a headlight's");
  }

  // ---- the lamp set ------------------------------------------------------------------------------------------------------
  {
    const float a[3] = {10.0f, 0.6f, 5.0f}, b[3] = {10.8f, 0.6f, 5.0f}, c[3] = {10.4f, 1.0f, 5.0f}, d[3] = {50.0f, 0.6f, 5.0f};
    std::vector<lp::Point> points = {
        Point(0x10, lp::kKindBrake, 0, a, 0.5f),      Point(0x10, lp::kKindBrake, 1, b, 0.5f),  Point(0x10, lp::kKindTail, 0, a, 1.0f),
        Point(0x10, lp::kKindInterior, 0, c, 1.0f),   Point(0x10, lp::kKindHeadlight, 0, a, 0.0f), Point(0x20, lp::kKindHeadlight, 0, d, 2.0f),
        Point(0x20, lp::kKindNone, 0, d, 1.0f),
    };
    const cr::LampSet set = SetOf(points);
    Check(set.cars == 2 && set.Valid(), "the cars the producer made: two");
    Check(set.lamps.size() == 5, "five lamps a lens can be lit by (the interior's and the unnamed kind's are left out)");
    Check(set.on == 4, "four of them are on (the off headlight stays in the set, off)");
    uint32_t off = 0, head_on = 0;
    for (const cr::LampPoint& l : set.lamps) {
      off += l.on ? 0u : 1u;
      head_on += (l.family == lp::Family::kHead && l.on) ? 1u : 0u;
    }
    Check(off == 1 && head_on == 1, "the off lamp is there, off; the far car's headlight is on");
    cr::LampSet cleared = set;
    cleared.Clear();
    Check(!cleared.Valid() && cleared.cars == 0 && cleared.on == 0, "Clear empties it");
    cl::CarLightsFrame none;
    cr::LampSet empty;
    cr::MakeLampSet(none, &empty);
    Check(!empty.Valid(), "no cars, no lamps");
  }

  // ---- the lens ----------------------------------------------------------------------------------------------------------
  {
    const tp::Mat4 m = Rigid(0.6, 30.0, 0.5, -12.0);
    const float inside_model[3] = {0.1f, 0.0f, 0.0f};
    float inside[3];
    World(m, inside_model, inside);
    const float near_model[3] = {0.30f, 0.0f, 0.0f};  // 0.10 m beyond the box's face: inside the margin
    float near_edge[3];
    World(m, near_model, near_edge);
    const float far_model[3] = {0.45f, 0.0f, 0.0f};   // 0.25 m beyond: outside
    float far_edge[3];
    World(m, far_model, far_edge);

    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, inside, 0.5f)});
      const cr::Glow g = cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set);
      Check(g.driven && g.matched == 1 && g.lit == 1 && Near(g.scale, cr::GlowOf(lp::Family::kBrake, 0.5f, true)) && Near(g.level, 0.5), "a brake lamp inside the lens's box drives it: its glow, its level");
    }
    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, near_edge, 0.5f)});
      Check(cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set).driven, "a lamp 0.10 m outside the box is in the margin");
    }
    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, far_edge, 0.5f)});
      const cr::Glow g = cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set);
      Check(!g.driven && g.matched == 0 && g.scale == 0.0f, "a lamp 0.25 m outside the box is not the lens's: not driven, the night factor's guess stays");
    }
    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, inside, 0.0f)});
      const cr::Glow g = cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set);
      Check(g.driven && g.scale == 0.0f && g.lit == 0 && g.matched == 1, "a lamp that is off in the lens makes it DARK (driven, scale 0), not unknown");
    }
    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kTurn, 6, inside, 0.0f), Lamp(lp::Family::kBrake, 0, inside, 0.5f), Lamp(lp::Family::kReverse, 11, inside, 0.0f)});
      const cr::Glow g = cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set);
      Check(g.driven && g.matched == 3 && g.lit == 1 && Near(g.scale, cr::GlowOf(lp::Family::kBrake, 0.5f, true)), "a cluster: the lens takes the largest of its lamps' glows, the off ones add nothing");
    }
    {
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kTail, 1, inside, 1.0f), Lamp(lp::Family::kBrake, 0, inside, 0.9f)});
      const cr::Glow g = cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set);
      Check(Near(g.scale, cr::GlowOf(lp::Family::kBrake, 0.9f, true)) && g.scale > cr::kTailGlow, "tail and brake lamps in one lens: the brake's higher glow");
    }
    {
      const cr::LampSet rear = SetOfLamps({Lamp(lp::Family::kBrake, 0, inside, 0.5f)});
      const cr::LampSet front = SetOfLamps({Lamp(lp::Family::kHead, 3, inside, 2.0f)});
      Check(!cr::LensGlow(m, kLo, kHi, true, 11, rear).driven && cr::LensGlow(m, kLo, kHi, true, 11, front).driven, "a walked headlight (part 11) is lit by front lamps, not by a brake lamp");
      Check(cr::LensGlow(m, kLo, kHi, true, 12, rear).driven && cr::LensGlow(m, kLo, kHi, true, 13, rear).driven && !cr::LensGlow(m, kLo, kHi, true, 12, front).driven,
            "a walked taillight (parts 12 and 13) is lit by rear lamps, not by a headlight");
      Check(cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, rear).driven && cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, front).driven, "a drawn lens (no part slot) takes any lamp family");
      Check(cr::LensGlow(m, kLo, kHi, true, 11, SetOfLamps({Lamp(lp::Family::kTurn, 13, inside, 1.0f)})).driven, "a front lens takes a turn signal (a front lens holds one)");
    }
    {
      const float other_car[3] = {inside[0] + 3.0f, inside[1], inside[2]};
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, other_car, 0.9f)});
      Check(!cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, set).driven, "the lamps of another car, three metres off, never drive this lens");
    }
    {
      // no box: the instance's origin and kOriginRadius
      const std::array<float, 3> near_origin = {float(m.m[3][0]) + 0.3f, float(m.m[3][1]), float(m.m[3][2])};
      const cr::LampSet at_origin = SetOfLamps({Lamp(lp::Family::kBrake, 0, near_origin.data(), 0.5f)});
      Check(cr::LensGlow(m, nullptr, nullptr, false, cr::kNoPart, at_origin).driven, "no box: a lamp within kOriginRadius of the instance's origin drives the lens");
      const std::array<float, 3> far_from_origin = {float(m.m[3][0]) + 0.9f, float(m.m[3][1]), float(m.m[3][2])};
      Check(!cr::LensGlow(m, nullptr, nullptr, false, cr::kNoPart, SetOfLamps({Lamp(lp::Family::kBrake, 0, far_from_origin.data(), 0.5f)})).driven, "no box: a lamp 0.9 m from the origin does not");
    }
    {
      tp::Mat4 singular;  // all zero: no inverse
      const cr::LampSet set = SetOfLamps({Lamp(lp::Family::kBrake, 0, inside, 0.5f)});
      Check(!cr::LensGlow(singular, kLo, kHi, true, cr::kNoPart, set).driven, "a matrix with no inverse drives nothing");
      Check(!cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, cr::LampSet{}).driven, "no lamps this frame drives nothing");
      const float nan_lamp[3] = {std::nanf(""), 0.0f, 0.0f};
      Check(!cr::LensGlow(m, kLo, kHi, true, cr::kNoPart, SetOfLamps({Lamp(lp::Family::kBrake, 0, nan_lamp, 0.5f)})).driven, "a lamp whose place is not a number drives nothing");
    }
    // the same lens and lamps under any rigid placement of the car: the verdict is the same
    {
      std::mt19937 rng(20261121u);
      std::uniform_real_distribution<double> unit(0.0, 1.0);
      bool same = true;
      for (int trial = 0; trial < 300; ++trial) {
        const tp::Mat4 placed = Rigid(unit(rng) * 6.283, (unit(rng) - 0.5) * 4000.0, unit(rng) * 3.0, (unit(rng) - 0.5) * 4000.0);
        const float lamp_model[3] = {float((unit(rng) - 0.5) * 1.2), float((unit(rng) - 0.5) * 0.6), float((unit(rng) - 0.5) * 0.6)};
        bool expect = true;
        for (int c = 0; c < 3; ++c) expect = expect && lamp_model[c] >= kLo[c] - cr::kLensMargin && lamp_model[c] <= kHi[c] + cr::kLensMargin;
        float lamp_world[3];
        World(placed, lamp_model, lamp_world);
        const bool got = cr::LensGlow(placed, kLo, kHi, true, cr::kNoPart, SetOfLamps({Lamp(lp::Family::kBrake, 0, lamp_world, 0.5f)})).driven;
        // a lamp within 3 mm of the margin's face may fall either way in single precision at 2 km from the origin: not counted
        bool near_face = false;
        for (int c = 0; c < 3; ++c) {
          near_face = near_face || std::fabs(lamp_model[c] - (kLo[c] - cr::kLensMargin)) < 0.004f || std::fabs(lamp_model[c] - (kHi[c] + cr::kLensMargin)) < 0.004f;
        }
        same = same && (near_face || got == expect);
      }
      Check(same, "300 random placements of the car (a turn, a move up to 2 km away): a lamp drives the lens exactly when it is in the box and its margin");
    }
  }

  // ---- the record the shader reads ----------------------------------------------------------------------------------------
  {
    Check(cr::kGlowDrivenBit == crm::kRecordGlowDriven && crm::kRecordGlowDriven == 1u, "the driven bit is the same number in the lamps' header and the record's");
    crm::InstanceMaterial lens;
    lens.material.cls = crm::Class::kLight;
    lens.material.flags = crm::kFlagEmissive;
    lens.material.emissive = 1.0f;
    uint32_t rows[crm::kMaterialRows][4];
    crm::MakeRows(lens, crm::kNoSlot, rows);
    crm::RowsDecoded d = crm::DecodeRows(rows);
    Check(!d.glow_driven && Near(d.emissive, 1.0) && rows[1][3] == 0u, "a lens with no lamps: the class's scale 1 and the bit clear (as before)");
    lens.glow_driven = true;
    lens.glow = 0.35f;
    crm::MakeRows(lens, crm::kNoSlot, rows);
    d = crm::DecodeRows(rows);
    Check(d.glow_driven && Near(d.emissive, 0.35) && rows[1][3] == crm::kRecordGlowDriven, "a driven lens: the lamps' scale in the record, the bit set");
    lens.glow = 0.0f;
    crm::MakeRows(lens, crm::kNoSlot, rows);
    d = crm::DecodeRows(rows);
    Check(d.glow_driven && d.emissive == 0.0f, "a dark lens is driven at scale 0 (not the class's 1)");
    lens.glow = 9.0f;
    crm::MakeRows(lens, crm::kNoSlot, rows);
    Check(Near(crm::DecodeRows(rows).emissive, 1.0), "a scale over 1 is held to 1");
    Check(cr::NightFor(true, 0.0f) == 1.0f && cr::NightFor(true, 1.0f) == 1.0f && cr::NightFor(false, 0.3f) == 0.3f, "a driven lens is not multiplied by the night factor; an undriven one is");

    // the reference shading: by day a driven lens with its lamps on still glows; an undriven lens does not
    crm::ShadeInput in;
    in.cls = crm::Class::kLight;
    in.tint_a[0] = in.tint_a[1] = in.tint_a[2] = 0.5f;
    in.tint_b[0] = in.tint_b[1] = in.tint_b[2] = 0.5f;
    in.light.valid = true;
    in.light.ambient[0] = in.light.ambient[1] = in.light.ambient[2] = 0.0f;
    in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 0.0f;
    in.env_known = true;
    in.sun_visible = 0.0f;
    in.emissive = 0.6f;
    in.night = 0.0f;
    in.glow_driven = true;
    float driven[3], undriven[3];
    crm::ShadeReference(in, driven);
    in.glow_driven = false;
    crm::ShadeReference(in, undriven);
    Check(Near(driven[0], 0.5 * 0.6 * crm::kEmissiveGain) && undriven[0] < 1e-6, "a driven lens glows by albedo x scale x the glow's gain at any hour; an undriven one by day not at all");
  }

  // ---- RT-NEXT-DAY: a lens no lamp drives glows by the hour, and the hour needs the sun ------------------------------------------
  {
    // the rule itself
    Check(crm::GlowNight(true, true, 0.0f) == 1.0f && crm::GlowNight(true, false, 0.97f) == 1.0f, "a driven lens is multiplied by 1, with a sun or without");
    Check(crm::GlowNight(false, true, 0.3f) == 0.3f && crm::GlowNight(false, true, 1.0f) == 1.0f && crm::GlowNight(false, true, 0.0f) == 0.0f, "an undriven lens with a sun glows by the frame's night factor");
    Check(crm::GlowNight(false, false, 0.97f) == 0.0f && crm::GlowNight(false, false, 1.0f) == 0.0f, "an undriven lens with NO sun does not glow: the colour's 0.97 (which reads the same at 14 h and 23 h) is no hour");
    // through the shading reference: a frame that has no sun direction carries the colour's guess in `night` (rt_hit_frame.h MakeFrameRows): 0.97 by day
    crm::ShadeInput in;
    in.cls = crm::Class::kLight;
    in.tint_a[0] = in.tint_a[1] = in.tint_a[2] = 0.5f;
    in.tint_b[0] = in.tint_b[1] = in.tint_b[2] = 0.5f;
    in.light.valid = true;
    in.light.ambient[0] = in.light.ambient[1] = in.light.ambient[2] = 0.0f;
    in.light.sun[0] = in.light.sun[1] = in.light.sun[2] = 0.0f;
    in.env_known = true;
    in.sun_visible = 0.0f;
    in.emissive = 0.6f;
    in.night = 0.97f;
    in.glow_driven = false;
    float no_sun[3], with_sun[3], driven[3];
    in.sun_known = false;
    crm::ShadeReference(in, no_sun);
    in.sun_known = true;
    in.night = 1.0f;
    crm::ShadeReference(in, with_sun);
    in.sun_known = false;
    in.night = 0.97f;
    in.glow_driven = true;
    crm::ShadeReference(in, driven);
    Check(no_sun[0] < 1e-6 && Near(with_sun[0], 0.5 * 0.6 * crm::kEmissiveGain) && Near(driven[0], 0.5 * 0.6 * crm::kEmissiveGain),
          "by day with no sun an undriven lens is dark (the colour's guess is not used); at night with a sun it glows; a driven lens glows in both");
  }

  // ---- the instance list ----------------------------------------------------------------------------------------------------
  {
    // Two cars' draws: a drawn car's lens (a draw of the lens shader: class light, emissive) and a walked car's taillight part (no shader); each with a mesh box of 0.4 x 0.2 x 0.2 m.
    const uint64_t kLensPs = 0xCC710F7164C151DEull;
    const tp::Mat4 world_a = Rigid(0.3, 20.0, 0.6, -4.0);
    const tp::Mat4 world_b = Rigid(-1.2, 24.0, 0.6, 3.0);
    const float lamp_a_model[3] = {0.05f, 0.0f, 0.0f}, lamp_b_model[3] = {-0.05f, 0.02f, 0.0f};
    float lamp_a[3], lamp_b[3];
    World(world_a, lamp_a_model, lamp_a);
    World(world_b, lamp_b_model, lamp_b);
    const tp::Mat4 view = tp::Identity();

    auto lens_draw = [&](uint32_t id, uint64_t key, const tp::Mat4& placed, uint64_t ps, uint16_t part) {
      ci::OtherDraw d;
      d.stencil_id = id;
      d.mesh_key = key;
      d.ps = ps;
      d.model_to_world = placed;
      d.indices = 300;
      d.part = part;
      if (part == ci::OtherDraw::kNoPart) {
        const crm::ShaderRule* rule = crm::FindShader(ps);
        d.material.cls = rule ? rule->cls : crm::Class::kNone;
        d.material.flags = (rule && rule->emissive) ? crm::kFlagEmissive : 0;
        d.material.emissive = 1.0f;
      }  // a walked part has no shader and no material: AppendOthers makes its usual one from the part slot
      return d;
    };
    std::vector<ci::OtherDraw> draws = {lens_draw(1, 0xA1, world_a, kLensPs, ci::OtherDraw::kNoPart), lens_draw(2, 0xB1, world_b, 0, 12)};
    std::vector<ci::OtherCar> cars(2);
    for (uint32_t i = 0; i < 2; ++i) {
      cars[i].stencil_id = i + 1;
      cars[i].distance = 5.0 + i;
      cars[i].Add(draws[i].model_to_world, 5.0 + i);
    }
    auto find = [&](uint64_t key, ci::MeshRef* ref) {
      (void)key;
      ref->first_index = 0;
      ref->first_vertex = 0;
      for (int c = 0; c < 3; ++c) {
        ref->bounds_min[c] = kLo[c];
        ref->bounds_max[c] = kHi[c];
      }
      ref->has_bounds = true;
      return true;
    };
    {
      ci::List list;
      const cr::LampSet lamps = SetOfLamps({Lamp(lp::Family::kBrake, 0, lamp_a, 0.5f), Lamp(lp::Family::kBrake, 1, lamp_b, 0.0f)});
      ci::AppendOthers(&list, draws, cars, view, false, 640, 640, find, 4, 2.0f, &lamps);
      Check(list.materials.size() == 2, "both lenses are listed");
      Check(list.materials[0].glow_driven && Near(list.materials[0].glow, cr::GlowOf(lp::Family::kBrake, 0.5f, true)), "the drawn car's lens (the lens shader's draw): driven by its brake lamp");
      Check(list.materials[1].glow_driven && list.materials[1].glow == 0.0f, "the walked car's taillight (part 12): driven, dark: its lamp is off");
      Check(list.stats.lens_glowing == 1 && list.stats.lens_dark == 1 && list.stats.lens_undriven == 0 && Near(list.stats.lens_level_max, 0.5), "the stats say one glowing, one dark");
    }
    {
      ci::List list;
      const cr::LampSet lamps = SetOfLamps({Lamp(lp::Family::kBrake, 0, lamp_a, 0.5f)});  // none at the second car
      ci::AppendOthers(&list, draws, cars, view, false, 640, 640, find, 4, 2.0f, &lamps);
      Check(list.materials[0].glow_driven && !list.materials[1].glow_driven && list.stats.lens_undriven == 1, "a lens with no lamp in it stays undriven");
    }
    {
      ci::List list;
      ci::AppendOthers(&list, draws, cars, view, false, 640, 640, find, 4, 2.0f, nullptr);
      Check(!list.materials[0].glow_driven && !list.materials[1].glow_driven && list.stats.lens_glowing + list.stats.lens_dark + list.stats.lens_undriven == 0, "no lamps given: nothing is driven or counted (today's rule)");
      ci::List empty_set;
      const cr::LampSet none;
      ci::AppendOthers(&empty_set, draws, cars, view, false, 640, 640, find, 4, 2.0f, &none);
      Check(!empty_set.materials[0].glow_driven && empty_set.stats.lens_undriven == 0, "an empty lamp set is no lamps");
    }
    {
      // a body panel is no lens even with a lamp inside its box
      ci::OtherDraw panel = lens_draw(1, 0xA2, world_a, 0x439B193A3F675533ull, ci::OtherDraw::kNoPart);
      std::vector<ci::OtherDraw> one = {panel};
      std::vector<ci::OtherCar> car = {cars[0]};
      ci::List list;
      const cr::LampSet lamps = SetOfLamps({Lamp(lp::Family::kBrake, 0, lamp_a, 0.9f)});
      ci::AppendOthers(&list, one, car, view, false, 640, 640, find, 4, 2.0f, &lamps);
      Check(!list.materials[0].glow_driven && list.stats.lens_glowing + list.stats.lens_dark + list.stats.lens_undriven == 0, "the paint is not a lens: a lamp inside its box drives nothing");
    }
  }

  // ---- the light points' consumers -----------------------------------------------------------------------------------------
  {
    lp::Want(lp::Consumer::kManager, 0u);
    lp::Want(lp::Consumer::kRt, 0u);
    Check(!lp::AnyWanted(), "no consumer: the hook notes nothing");
    cl::CarLightsSource manager;                        // the manager's (the default)
    cl::CarLightsSource rt(lp::Consumer::kRt);          // the ray traced scene's
    rt.SetEnabled(true);
    Check(lp::AnyWanted() && lp::Wanted(lp::kKindBrake) && lp::Wanted(lp::kKindPolice1), "the ray traced scene's source asks the hook for every kind");
    manager.SetEnabled(true);
    manager.SetEnabled(false);
    Check(lp::Wanted(lp::kKindBrake), "the manager's source switching off does not take the kinds the ray traced scene still reads");
    manager.SetEnabled(true);
    rt.SetEnabled(false);
    Check(lp::Wanted(lp::kKindBrake) && !rt.enabled() && manager.enabled(), "the ray traced scene's source switching off leaves the manager's");
    manager.SetEnabled(false);
    Check(!lp::AnyWanted(), "both off: the hook notes nothing again");
  }

  // ---- the shader, the host, the settings ------------------------------------------------------------------------------------
  {
    const std::string hlsli = ReadFile(src + "/gpu/hlsl/car_rt_hit_shade.hlsli");
    const std::string material = ReadFile(src + "/gpu/hlsl/car_rt_hit_material.hlsli");
    const std::string scene = ReadFile(src + "/gpu/car_rt_scene.cpp");
    const std::string scene_h = ReadFile(src + "/gpu/car_rt_scene.h");
    const std::string inst = ReadFile(src + "/gpu/car_rt_instances.cpp");
    const std::string inst_h = ReadFile(src + "/gpu/car_rt_instances.h");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    Check(!hlsli.empty() && !scene.empty() && !inst.empty() && !cmake.empty(), "the sources were read (give the src folder as the first argument)");
    Check(Has(material, "static const uint kMatGlowDriven = 1u;") && Has(material, "m.glowDriven = (r1.w & kMatGlowDriven) != 0u;") && Has(material, "return m.glowDriven ? 1.0 : night;") &&
              Has(hlsli, "const float glowNight = CarMatGlowScale(m, night);") &&
              Has(hlsli, "result += albedo * (m.emissive * kRtHitEmissiveGain * glowNight);") && Has(hlsli, "gCarHitGlow = (m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u ? m.emissive * glowNight : 0.0;"),
          "the shader reads the driven bit of the record's row 1 w and does not multiply a driven lens's scale by the night factor");
    Check(Has(scene, "void CarRtScene::UpdateLamps(") && Has(scene, "UpdateLamps(frame, build_limits.On());") && Has(scene, "lights_source_.Poll(kNoFrames,") && Has(scene, "car_rt_lights::MakeLampSet(lights, &lamp_set_);") &&
              Has(scene, "instances_.SetLamps(lamp_set_.Valid() ? &lamp_set_ : nullptr);") && Has(scene, "lights_source_.SetEnabled(false);") && Has(scene, "mcla_gpu_car_rt_lamps.Get()"),
          "the scene polls the lamps once a frame before the list is built, hands them over, and gives the hook back at shutdown");
    Check(Has(scene_h, "CarLightsSource lights_source_{larecomp::gpu::light_points::Consumer::kRt};") && Has(inst_h, "void SetLamps(const car_rt_lights::LampSet* lamps)") &&
              Has(inst, "find, limits.cars, paint_gamma, lamps_);"),
          "the scene's source is the ray traced scene's consumer; the list takes the lamps in both of its AppendOthers calls");
    Check(Has(scene, "car_rt_lights::Text(lens_stats_)") && Has(scene, "lens_stats_.glowing += list.stats.lens_glowing;"), "the census line says what the lenses were");
    Check(Has(cmake, "larecomp_car_rt_lights_rules_test") && Has(cmake, "car_rt_lights_rules_test.cpp"), "the build has the test");
    const setting_source::Info lamps_switch = setting_source::Find(src + "/gpu", scene, "mcla_gpu_car_rt_lamps");
    Check(lamps_switch.found && setting_source::DefaultIsTrue(lamps_switch) && lamps_switch.debug_only && lamps_switch.live, "the switch is a live, debug-only bool, on by default");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
