#pragma once
// ===========================================================================
// MCLA GPU renderer -- what the cars' lights say, from the game's own light points (perf/LIGHT-UNIFY.md package LU-7 = step U7): the producer of CarLights
// ===========================================================================
// light_points.h is the table of every light point the game submits (head, hi-beam, fog, brake, tail, turn, reverse, police ...) as the hook at 0x8237AFB8 saw them. This turns them,
// once a frame on the render thread, into per CAR records:
//
//   Car        one car's lamps (up to kMaxLamps): each with its kind and family, side (left / right of the car's centre line), its place in the WORLD and in the CAR's space, the way it
//              shines, the intensity the guest read, its level 0..1 and whether it is on; the car's frame (where car space is from); and `lights`, the LightFrame's CarLights (the bits of
//              what is on and the four levels): what ray traced hit shading's lens emissives and the G-buffer's emissives read INSTEAD OF the night factor's guess.
//   Lights     MakePointLights: the omni Light candidates the lamps make for the picker (police first; brake, tail, turn and reverse are a policy away): the LightSource kPolice and
//              kOther, ids in light_id.h's point domain, no shadow views.
//
// WHICH CAR. A point's light object is one per car (light_points.h): the group key. A racer's or the player's has an owner and so a vehicle address (the join to the renderer's own cars:
// car_rt_memory VehicleRead, the player's world matrix); a TRAFFIC car has none (CarLights::vehicle 0): it is joined to a renderer car by PLACE (`FrameIn` within kFrameMatchMetres of the
// lamps' centre), each frame of the renderer to at most one car, nearest first. A car the renderer has no frame for still gets one, DERIVED from its own lamps: the horizontal
// direction of its headlights' stored axis (the car's back, which the probe verified for an owner's points), or, without one, from the headlight pair's midpoint to the rear lamps'.
//
// WHAT THE LEVELS MEAN. A lamp is ON above light_points.h kOffIntensity; its level is its peak intensity over the family's full one. The brake family (kinds 0 and 2) carries BOTH the tail
// glow and the pedal: the player's brake lamps read 0.075 and a racer's 0.5 with nothing pressed, a traffic car's 0.9 (one scalar of the car times the kind's constant), so `brake` here is
// "the brake lamps glow" at that level, not "the pedal is down" (CHECK in a run: the census line below, a player who brakes). The levels of the four slots (light_frame.h CarLights::level):
// head = max of the head, hi-beam and fog lamps, brake = the brake family, tail/turn = the taillights and the turn signals, police = the three police kinds; reverse has its bit only.
//
// WHAT THE CONSUMERS TAKE
//   RT hit shading (feat/rt-hit-shading's emissive scale, LU-8, DONE: car_rt_lights_rules.h): per LENS instance of another car, the lamps of `Car::lamps` (their kind, family, level, on, and place in the
//     WORLD) that stand in the lens mesh's box give its glow (the lamp's world place taken into the lens mesh's own space by the inverse of the draw's matrix): no car join is needed, the lamps of
//     another car are metres from the lens, so a racer (owned light object), a traffic car (none) and a car placed from memory or model data all join the same way. The scale replaces the night
//     factor's guess for that lens; a lens no lamp stands in keeps the guess. The ray traced scene has a CarLightsSource of its own (consumer kRt), so its switching does not touch the manager's.
//   per-headlight views (hl-per-lamp's two views per car): Car::lamps with kind 3 (index 0 the left lamp, 1 the right: `side` and `local[0]` say it), their `world` places and `emit` axes; the rig
//     keeps its own tracking for now (it matches lens lamps to headlight_points.h's table) and takes the lamp pair from here when the manager owns the cars (LU-3).
//   the picker (LU-3) and the deferred pass (GB-H3): MakePointLights.
//
// Pure: no guest, no renderer, no SDK (it includes light_frame.h, light_id.h, light_laws.h, light_convert.h and light_points.h only); car_lights_test.cpp holds it to the probe's recorded points.
// ===========================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"
#include "light_points.h"

namespace larecomp::gpu::car_lights {

namespace lp = light_points;
using lp::Family;

enum class Side : uint8_t { kCentre = 0, kLeft = 1, kRight = 2 };
enum class FrameSource : uint8_t { kNone = 0, kDerived = 1, kExternal = 2 };

inline constexpr uint32_t kMaxLamps = 32;          // a car's lamps kept (a car has about 25 points: 19 kinds of one to four)
inline constexpr float kSideBand = 0.05f;          // m: a lamp nearer than this to the car's centre line is on neither side
inline constexpr double kFrameMatchMetres = 3.5;   // a renderer frame names a car with no owner when its root is this near the car's lamps (a car is about 5 m long)
inline constexpr std::chrono::milliseconds kMaxAge{100};  // the points the table gives: seen within this (the light submit runs every frame; 3 frames at 30 FPS)

// A car's frame the renderer knows (car_rt_memory_rules.h VehicleRead, the player's body matrix): the game's rows right, up, back and position, as row vectors.
struct FrameIn {
  uint32_t vehicle = 0;       // the vehicle's guest address; 0: matched by place only
  double rows[4][3] = {};
};

// The axes car space is measured on (right x up = back, as the rig's Pose): local x is to the car's right, y up, z BACK (a lamp at the front has a negative z).
struct Axes {
  FrameSource source = FrameSource::kNone;
  double origin[3] = {};
  double right[3] = {1, 0, 0};
  double up[3] = {0, 1, 0};
  double back[3] = {0, 0, 1};
};

struct Lamp {
  uint32_t kind = 0;
  Family family = Family::kOther;
  Side side = Side::kCentre;
  uint32_t index = 0;
  float world[3] = {};
  float local[3] = {};     // car space (Axes): x right, y up, z back; the offset from the lamps' centre in world axes when there is no frame
  float emit[3] = {};      // the unit vector the lamp shines along (minus its stored direction; without one: away from the car's middle along its axis); zero without a frame
  float intensity = 0.0f;  // the guest's reading (the peak of the last few ms)
  float level = 0.0f;      // 0..1 (light_points.h Level)
  bool on = false;
};

struct Car {
  CarLights lights;        // what the LightFrame carries (car_key: PointCarKey; vehicle: 0 for traffic)
  uint32_t object = 0;     // the light object: the car's key in the table
  uint32_t vehicle = 0;
  bool player = false;
  bool owned = false;
  Axes axes;
  double centre[3] = {};   // the mean of the lamps, world
  float distance = 0.0f;   // to the reference point of the Build (0 when there was none)
  float dir_vs_layout = 2.0f;  // the cosine between the headlights' stored direction and the layout's back axis (rear lamps minus headlights, horizontal); 2: not both known. A car with no
                               // owner has a stored direction only by inference (light_points.h ReadPoint): this is the number a run reads to decide kTrustOwnerlessDirection
  std::array<float, lp::kFamilyCount> family_level{};  // the largest level of each family (light_points.h Family): reverse and the families CarLights has no slot for
  std::array<Lamp, kMaxLamps> lamps{};
  uint32_t lamp_count = 0;
};

struct CarLightsFrame {
  std::array<Car, kMaxCars> cars{};
  uint32_t count = 0;
  uint32_t points_in = 0;     // the points given
  uint32_t cars_seen = 0;     // the cars they made, before the cap
  uint32_t cars_dropped = 0;  // the ones past kMaxCars (farthest, never the player's)
  uint32_t frames_matched = 0;  // cars joined to a renderer frame
};

// A car's key in LightFrame::cars when the points are all that is known: the top bit set (the rig's track keys are small counters), never 0. The manager replaces it with the rig's key for a
// car the rig tracks.
inline uint32_t PointCarKey(uint32_t id) { return 0x80000000u | (uint32_t(light_id::Mix64(id)) & 0x7FFFFFFFu) | 1u; }

// ---------------------------------------------------------------------------
// vector helpers (double, the renderer's own frames are)
// ---------------------------------------------------------------------------

namespace detail {

inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double Len(const double a[3]) { return std::sqrt(Dot(a, a)); }
inline void Cross(const double a[3], const double b[3], double out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool Normalize(double v[3]) {
  const double l = Len(v);
  if (!(l > 1e-6)) return false;
  for (int i = 0; i < 3; ++i) v[i] /= l;
  return true;
}
inline double Distance(const double a[3], const double b[3]) {
  const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return Len(d);
}

// A frame from the horizontal part of a back axis: up is the world's, right is up x back.
inline bool AxesFromBack(const double back_in[3], const double origin[3], Axes* out) {
  double back[3] = {back_in[0], 0.0, back_in[2]};
  if (!Normalize(back)) return false;
  const double up[3] = {0, 1, 0};
  double right[3];
  Cross(up, back, right);
  out->source = FrameSource::kDerived;
  for (int i = 0; i < 3; ++i) {
    out->origin[i] = origin[i];
    out->right[i] = right[i];
    out->up[i] = up[i];
    out->back[i] = back[i];
  }
  return true;
}

inline bool AxesFromRows(const FrameIn& in, Axes* out) {
  double right[3], up[3], back[3];
  for (int i = 0; i < 3; ++i) {
    right[i] = in.rows[0][i];
    up[i] = in.rows[1][i];
    back[i] = in.rows[2][i];
  }
  if (!Normalize(right) || !Normalize(up) || !Normalize(back)) return false;
  out->source = FrameSource::kExternal;
  for (int i = 0; i < 3; ++i) {
    out->origin[i] = in.rows[3][i];
    out->right[i] = right[i];
    out->up[i] = up[i];
    out->back[i] = back[i];
  }
  return true;
}

// The stored direction of a point is used for the car's frame and for the way a lamp shines only where the probe verified it: an OWNED car's (a racer's, the player's). A car with no owner has one
// only by inference (light_points.h ReadPoint reads the row below the position sub_8237A698 made): CHECK in a run (the census prints both), then flip this.
inline constexpr bool kTrustOwnerlessDirection = false;
inline bool TrustedDirection(const lp::Point& p) { return p.has_dir && (p.owned || kTrustOwnerlessDirection); }

struct Group {
  uint32_t key = 0;  // the light object
  uint32_t vehicle = 0;
  bool owned = false;
  bool player = false;
  std::vector<const lp::Point*> points;
  double centre[3] = {};
};

}  // namespace detail

// ---------------------------------------------------------------------------
// the lamps' state
// ---------------------------------------------------------------------------

// One lamp into its car: its family's level (family_level, on or off), and when it is on its bit and level slot in CarLights (light_frame.h: kCarLight*, kCarLevel*).
inline void NoteLamp(Car* car, const Lamp& lamp) {
  float& fam = car->family_level[uint32_t(lamp.family)];
  fam = std::max(fam, lamp.level);
  if (!lamp.on) return;
  CarLights& c = car->lights;
  uint32_t level_slot = 0xFFFFFFFFu;
  switch (lamp.family) {
    case Family::kHead: c.bits |= kCarLightHead; level_slot = kCarLevelHead; break;
    case Family::kHiBeam: c.bits |= kCarLightHiBeam; level_slot = kCarLevelHead; break;
    case Family::kFog: c.bits |= kCarLightFog; level_slot = kCarLevelHead; break;
    case Family::kBrake: c.bits |= kCarLightBrake; level_slot = kCarLevelBrake; break;
    case Family::kTail: c.bits |= kCarLightTail; level_slot = kCarLevelTailTurn; break;
    case Family::kTurn:
      // A lamp on neither side (the player's two sit at the rear centre) could be either: both bits.
      if (lamp.side != Side::kRight) c.bits |= kCarLightTurnL;
      if (lamp.side != Side::kLeft) c.bits |= kCarLightTurnR;
      level_slot = kCarLevelTailTurn;
      break;
    case Family::kReverse: c.bits |= kCarLightReverse; break;
    case Family::kPolice:
      c.bits |= lamp.kind == lp::kKindPolice0 ? kCarLightPoliceA : (lamp.kind == lp::kKindPolice1 ? kCarLightPoliceB : kCarLightPoliceC);
      level_slot = kCarLevelPolice;
      break;
    case Family::kOther: break;
  }
  if (level_slot < 4) c.level[level_slot] = std::max(c.level[level_slot], lamp.level);
}

// ---------------------------------------------------------------------------
// the producer
// ---------------------------------------------------------------------------

// The cars the points make. `frames`: the renderer's own cars (may be empty). `reference`: the world point cars are ranked by (the camera; null: by key), the player's car always first.
// Cars past kMaxCars are dropped, the farthest first.
inline void Build(const std::vector<lp::Point>& points, const std::vector<FrameIn>& frames, const double* reference, CarLightsFrame* out) {
  *out = CarLightsFrame{};
  out->points_in = uint32_t(points.size());
  if (points.empty()) return;

  // 1. the cars: points grouped by their light object (sorted, so that the order does not depend on the table's)
  std::vector<const lp::Point*> sorted;
  sorted.reserve(points.size());
  for (const lp::Point& p : points) sorted.push_back(&p);
  std::sort(sorted.begin(), sorted.end(), [](const lp::Point* a, const lp::Point* b) {
    if (a->object != b->object) return a->object < b->object;
    if (a->kind != b->kind) return a->kind < b->kind;
    return a->index < b->index;
  });
  std::vector<detail::Group> groups;
  for (const lp::Point* p : sorted) {
    if (groups.empty() || groups.back().key != p->object) {
      groups.emplace_back();
      groups.back().key = p->object;
    }
    detail::Group& g = groups.back();
    g.points.push_back(p);
    if (p->vehicle != 0) g.vehicle = p->vehicle;
    g.owned = g.owned || p->owned;
    g.player = g.player || p->player;
  }
  for (detail::Group& g : groups) {
    for (int i = 0; i < 3; ++i) {
      double sum = 0.0;
      for (const lp::Point* p : g.points) sum += double(p->pos[i]);
      g.centre[i] = sum / double(g.points.size());
    }
  }

  // 2. the renderer's frames to the cars: a frame with the car's own vehicle address first, then by place, nearest first, each frame and each car once
  struct Pair {
    size_t group, frame;
    double distance;
  };
  std::vector<Pair> pairs;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    for (size_t fi = 0; fi < frames.size(); ++fi) {
      if (groups[gi].vehicle != 0 && frames[fi].vehicle == groups[gi].vehicle) {
        pairs.push_back({gi, fi, -1.0});  // by name: before any by place
      } else if (groups[gi].vehicle == 0 || frames[fi].vehicle == 0) {
        const double d = detail::Distance(groups[gi].centre, frames[fi].rows[3]);
        if (d <= kFrameMatchMetres) pairs.push_back({gi, fi, d});
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
  std::vector<int> frame_of(groups.size(), -1);
  std::vector<bool> frame_taken(frames.size(), false);
  for (const Pair& p : pairs) {
    if (frame_of[p.group] >= 0 || frame_taken[p.frame]) continue;
    frame_of[p.group] = int(p.frame);
    frame_taken[p.frame] = true;
  }

  // 3. each car
  std::vector<Car> cars;
  cars.reserve(groups.size());
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    const detail::Group& g = groups[gi];
    cars.emplace_back();
    Car& car = cars.back();
    car.object = g.key;
    car.vehicle = g.vehicle;
    car.player = g.player;
    car.owned = g.owned;
    for (int i = 0; i < 3; ++i) car.centre[i] = g.centre[i];
    car.lights.vehicle = g.vehicle;
    car.lights.car_key = PointCarKey(g.vehicle != 0 ? g.vehicle : g.key);

    // the frame: the renderer's, else the lamps' own
    if (frame_of[gi] >= 0 && detail::AxesFromRows(frames[size_t(frame_of[gi])], &car.axes)) {
      ++out->frames_matched;
    } else {
      double head[3] = {}, rear[3] = {}, head_dir[3] = {}, stored_dir[3] = {};
      uint32_t heads = 0, rears = 0, head_dirs = 0, stored_dirs = 0;
      for (const lp::Point* p : g.points) {
        if (p->kind == lp::kKindHeadlight) {
          ++heads;
          for (int i = 0; i < 3; ++i) head[i] += double(p->pos[i]);
          if (p->has_dir) {
            ++stored_dirs;
            for (int i = 0; i < 3; ++i) stored_dir[i] += double(p->dir[i]);
          }
          if (detail::TrustedDirection(*p)) {
            ++head_dirs;
            for (int i = 0; i < 3; ++i) head_dir[i] += double(p->dir[i]);
          }
        } else if (p->kind == lp::kKindBrake || p->kind == lp::kKindTail || p->kind == lp::kKindThirdBrake) {
          ++rears;
          for (int i = 0; i < 3; ++i) rear[i] += double(p->pos[i]);
        }
      }
      double origin[3] = {car.centre[0], car.centre[1], car.centre[2]};
      double back[3] = {};
      bool have_back = false;
      if (heads > 0) {
        for (int i = 0; i < 3; ++i) head[i] /= double(heads);
      }
      if (rears > 0) {
        for (int i = 0; i < 3; ++i) rear[i] /= double(rears);
      }
      if (heads > 0 && rears > 0) {
        for (int i = 0; i < 3; ++i) origin[i] = 0.5 * (head[i] + rear[i]);
        double layout[3] = {rear[0] - head[0], 0.0, rear[2] - head[2]};
        double stored[3] = {stored_dir[0], 0.0, stored_dir[2]};
        if (stored_dirs > 0 && detail::Normalize(layout) && detail::Normalize(stored)) car.dir_vs_layout = float(detail::Dot(layout, stored));
      }
      if (head_dirs > 0) {
        for (int i = 0; i < 3; ++i) back[i] = head_dir[i];
        have_back = true;  // the headlights' stored direction is the car's back axis (the probe)
      } else if (heads > 0 && rears > 0) {
        for (int i = 0; i < 3; ++i) back[i] = rear[i] - head[i];
        have_back = true;
      }
      if (!(have_back && detail::AxesFromBack(back, origin, &car.axes))) {
        car.axes = Axes{};
        for (int i = 0; i < 3; ++i) car.axes.origin[i] = car.centre[i];
      }
    }

    // the lamps
    for (const lp::Point* p : g.points) {
      if (car.lamp_count >= kMaxLamps) break;
      Lamp& lamp = car.lamps[car.lamp_count++];
      lamp.kind = p->kind;
      lamp.family = lp::FamilyOf(p->kind);
      lamp.index = p->index;
      lamp.intensity = std::max(p->peak, p->intensity);
      lamp.level = lp::Level(lamp.family, lamp.intensity);
      lamp.on = lamp.intensity > lp::kOffIntensity;
      const double world[3] = {double(p->pos[0]), double(p->pos[1]), double(p->pos[2])};
      const double d[3] = {world[0] - car.axes.origin[0], world[1] - car.axes.origin[1], world[2] - car.axes.origin[2]};
      double local[3] = {d[0], d[1], d[2]};
      const bool framed = car.axes.source != FrameSource::kNone;
      if (framed) {
        local[0] = detail::Dot(d, car.axes.right);
        local[1] = detail::Dot(d, car.axes.up);
        local[2] = detail::Dot(d, car.axes.back);
      }
      for (int i = 0; i < 3; ++i) {
        lamp.world[i] = p->pos[i];
        lamp.local[i] = float(local[i]);
      }
      // the side: named for the kinds that say it, else by which side of the car's centre line the lamp is
      if (p->kind == lp::kKindTurnFL || p->kind == lp::kKindTurnBL) {
        lamp.side = Side::kLeft;
      } else if (p->kind == lp::kKindTurnFR || p->kind == lp::kKindTurnBR) {
        lamp.side = Side::kRight;
      } else if (framed) {
        lamp.side = local[0] > double(kSideBand) ? Side::kRight : (local[0] < -double(kSideBand) ? Side::kLeft : Side::kCentre);
      }
      // the way it shines
      if (detail::TrustedDirection(*p)) {
        double e[3] = {-double(p->dir[0]), -double(p->dir[1]), -double(p->dir[2])};
        if (detail::Normalize(e)) {
          for (int i = 0; i < 3; ++i) lamp.emit[i] = float(e[i]);
        }
      } else if (framed) {
        const double s = local[2] < 0.0 ? -1.0 : 1.0;  // a lamp at the front shines forward, one at the rear back
        for (int i = 0; i < 3; ++i) lamp.emit[i] = float(s * car.axes.back[i]);
      }
      NoteLamp(&car, lamp);
    }
  }

  // 4. at most kMaxCars: the player's first, then the nearest to the reference
  if (reference != nullptr) {
    for (Car& car : cars) car.distance = float(detail::Distance(car.centre, reference));
  }
  std::sort(cars.begin(), cars.end(), [](const Car& a, const Car& b) {
    if (a.player != b.player) return a.player;
    if (a.distance != b.distance) return a.distance < b.distance;
    return a.object < b.object;
  });
  out->cars_seen = uint32_t(cars.size());
  out->count = uint32_t(std::min<size_t>(cars.size(), kMaxCars));
  out->cars_dropped = out->cars_seen - out->count;
  for (uint32_t i = 0; i < out->count; ++i) out->cars[i] = cars[i];
}

// The LightFrame's record of the cars (LightFrame::cars, car_count).
inline void FillLightFrame(LightFrame* frame, const CarLightsFrame& cars) {
  frame->cars = {};
  frame->car_count = cars.count;
  for (uint32_t i = 0; i < cars.count && i < kMaxCars; ++i) frame->cars[i] = cars.cars[i].lights;
}

// The car of a vehicle address in the lights (the join of a racer or the player to the renderer's own car); null when there is none. For a car with no owner: FindByPlace.
inline const Car* FindByVehicle(const CarLightsFrame& frame, uint32_t vehicle) {
  if (vehicle == 0) return nullptr;
  for (uint32_t i = 0; i < frame.count; ++i) {
    if (frame.cars[i].vehicle == vehicle) return &frame.cars[i];
  }
  return nullptr;
}
// The nearest car with no owner to a world place within `max_metres` (a traffic car the renderer knows by its matrix): the join by place.
inline const Car* FindByPlace(const CarLightsFrame& frame, const double place[3], double max_metres = kFrameMatchMetres) {
  const Car* best = nullptr;
  double best_d = max_metres;
  for (uint32_t i = 0; i < frame.count; ++i) {
    if (frame.cars[i].vehicle != 0) continue;
    const double d = detail::Distance(frame.cars[i].centre, place);
    if (d <= best_d) {
      best_d = d;
      best = &frame.cars[i];
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// the lights the lamps make (candidates for the picker)
// ---------------------------------------------------------------------------

// Which families make Light candidates. Police only to begin with (LIGHT-UNIFY U7: "police and mod lights in the LightFrame"); the others are a switch of the manager's policy, for the deferred
// pass (GB-H3) once it exists.
inline constexpr uint32_t FamilyBit(Family f) { return 1u << uint32_t(f); }
inline constexpr uint32_t kFamiliesPolice = FamilyBit(Family::kPolice);
inline constexpr uint32_t kFamiliesRear = FamilyBit(Family::kBrake) | FamilyBit(Family::kTail) | FamilyBit(Family::kTurn) | FamilyBit(Family::kReverse);

// How a lamp looks as a light: its colour (rgb 0..1) and its intensity in the game's lamp units (a street lamp is 55 to 70; the light's range is the game's reach sqrt(100 I) kept in 8..120 m,
// its plateau I). [I]: LOOK values, to be set by eye with the deferred pass; the colours of the police kinds are a guess (CHECK: the probe saw only Police1, in no colour).
struct Look {
  float rgb[3];
  float intensity;
};
inline Look LookOf(uint32_t kind) {
  switch (kind) {
    case lp::kKindBrake: case lp::kKindThirdBrake: return {{1.0f, 0.04f, 0.02f}, 0.8f};
    case lp::kKindTail: return {{1.0f, 0.03f, 0.02f}, 0.4f};
    case lp::kKindReverse: return {{1.0f, 0.95f, 0.85f}, 1.5f};
    case lp::kKindTurn: case lp::kKindTurnFL: case lp::kKindTurnFR: case lp::kKindTurnBL: case lp::kKindTurnBR: return {{1.0f, 0.42f, 0.04f}, 0.8f};
    case lp::kKindPolice0: return {{1.0f, 0.03f, 0.02f}, 6.0f};
    case lp::kKindPolice1: return {{0.04f, 0.12f, 1.0f}, 6.0f};
    case lp::kKindPolice2: return {{1.0f, 0.9f, 0.8f}, 6.0f};
    default: return {{1.0f, 1.0f, 1.0f}, 0.0f};
  }
}

// The omni Light of each lamp of `car` that is on and whose family is in `families`: colour x level, the game's distance law with the look's intensity as its plateau, source kPolice for the
// police kinds and kOther for the rest, the id in the points' domain (car, kind, index), the car's key and player flag, no shadow views (the lamps are small: roles surface, volume and RT).
inline void MakePointLights(const Car& car, uint32_t families, std::vector<Light>* out) {
  const uint32_t car_id = car.vehicle != 0 ? car.vehicle : car.object;
  for (uint32_t i = 0; i < car.lamp_count; ++i) {
    const Lamp& lamp = car.lamps[i];
    if (!lamp.on || (families & FamilyBit(lamp.family)) == 0) continue;
    const Look look = LookOf(lamp.kind);
    if (look.intensity <= 0.0f) continue;
    Light l = MakeOmniLight(lamp.family == Family::kPolice ? LightSource::kPolice : LightSource::kOther, lamp.world, look.rgb, lamp.level * look.intensity, look.intensity,
                            light_laws::LampRange(look.intensity), light_id::PointId(car_id, (lamp.kind << 8) | lamp.index));
    l.roles = uint8_t(kRoleMaskSurface | kRoleMaskVolume | kRoleMaskRt);
    l.car_key = car.lights.car_key;
    l.flags = car.player ? kLightFlagPlayer : 0u;
    out->push_back(l);
  }
}

// ---------------------------------------------------------------------------
// the census line (the probe the CHECKs of LIGHT-UNIFY C3 and C4 are read from)
// ---------------------------------------------------------------------------

// One line a car: its vehicle, who it is, where its frame came from, the bits and the four levels, then per kind the points seen, how many are on and the largest intensity.
inline std::string FormatCensus(const Car& car) {
  char head[256];
  std::snprintf(head, sizeof(head), "[car-lights] %s 0x%x (object 0x%x) frame %s bits 0x%x level head %.2f brake %.2f tail/turn %.2f police %.2f |", car.player ? "player" : (car.owned ? "car" : "traffic"),
                car.vehicle, car.object, car.axes.source == FrameSource::kExternal ? "renderer" : (car.axes.source == FrameSource::kDerived ? "derived" : "none"), car.lights.bits,
                double(car.lights.level[0]), double(car.lights.level[1]), double(car.lights.level[2]), double(car.lights.level[3]));
  std::string line = head;
  if (car.dir_vs_layout <= 1.0f) {
    char dir[64];
    std::snprintf(dir, sizeof(dir), " dir/layout %.3f |", double(car.dir_vs_layout));
    line += dir;
  }
  for (uint32_t kind = 0; kind < lp::kKindCount; ++kind) {
    uint32_t n = 0, on = 0;
    float peak = 0.0f;
    for (uint32_t i = 0; i < car.lamp_count; ++i) {
      if (car.lamps[i].kind != kind) continue;
      ++n;
      on += car.lamps[i].on ? 1u : 0u;
      peak = std::max(peak, car.lamps[i].intensity);
    }
    if (n == 0) continue;
    char part[96];
    std::snprintf(part, sizeof(part), " k%u %s x%u on %u max %.3f;", kind, lp::KindName(kind), n, on, double(peak));
    line += part;
  }
  return line;
}

// ---------------------------------------------------------------------------
// the source (render thread)
// ---------------------------------------------------------------------------

class CarLightsSource {
 public:
  // `consumer` names who asks the hook for the kinds (light_points.h Want: one mask a consumer, the hook notes the union): the manager's source is kManager, the ray traced car
  // scene's (car_rt_lights_rules.h, LU-8) kRt, so that one switching off does not take the kinds the other still reads.
  explicit CarLightsSource(lp::Consumer consumer = lp::Consumer::kManager) : consumer_(consumer) {}

  // The owner's switch: while it is on the hook notes every kind. Off: the hook costs one relaxed load.
  void SetEnabled(bool enabled) {
    enabled_ = enabled;
    lp::Want(consumer_, enabled ? lp::kAllKinds : 0u);
    if (!enabled) frame_ = CarLightsFrame{};
  }
  bool enabled() const { return enabled_; }

  // Once a frame (present): the table's recent points to cars. `frames` the renderer's cars, `reference` the camera (or null).
  const CarLightsFrame& Poll(const std::vector<FrameIn>& frames, const double* reference, lp::Table::Clock::time_point now = lp::Table::Clock::now(),
                             lp::Table& table = lp::Global()) {
    if (!enabled_) return frame_;
    table.Recent(&points_, kMaxAge, now);
    Build(points_, frames, reference, &frame_);
    return frame_;
  }
  const CarLightsFrame& Frame() const { return frame_; }

 private:
  lp::Consumer consumer_;
  bool enabled_ = false;
  std::vector<lp::Point> points_;
  CarLightsFrame frame_;
};

}  // namespace larecomp::gpu::car_lights
