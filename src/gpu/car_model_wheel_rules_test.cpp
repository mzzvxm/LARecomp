// Check of the wheels of a walked car (car_model_wheel_rules.h, car_model_parts_rt_rules.h; RT-MODEL-PARTS): the wheel manager's tyre and rim by the game's accessors (the id's range,
// the use count, the error word, the placed bits, the pack), the wheel draw's own flags (the tyre's, the rim's, the hidden object, the wheels the vehicle draws), the geometries the
// walk adds to a vehicle's walk (parts kTyrePart and kRimPart, in use by the flags, bones as the drawable says, counted in the tallies), the four hypotheses for where a wheel stands,
// the calibration that picks one against the draws of the cars the camera sees (per kind), one placement per wheel the vehicle draws, and the kinds of what a car brings. The memory is
// made up. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_model_wheel_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_model_wheel_rules_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "car_model_parts_rt_rules.h"
#include "car_model_parts_test_support.h"
#include "car_model_walk_rules.h"
#include "car_model_wheel_rules.h"

namespace cw = mcla::gpu::car_model_walk;
namespace crt = mcla::gpu::car_model_rt;
namespace cmp = mcla::gpu::car_model_parts;
namespace tp = mcla::gpu::temporal;
using namespace parts_test;

namespace {

// The made-up manager: 3 tyres, 5 rims. Tyre 2 and rim 4 are good (one geometry on one bone; two models on bones 0 and 1); the others are made bad one way each.
struct World {
  uint32_t vehicle = 0, entity = 0, info = 0, set = 0, manager = 0, hide = 0;
  uint32_t tyre_drawable = 0, rim_drawable = 0;
  uint32_t tyre_geometry = 0, rim_geometry_a = 0, rim_geometry_b = 0;
  uint32_t tyre_entries = 0, tyre_packs = 0, tyre_errors = 0, tyre_uses = 0;
  uint32_t rim_entries = 0, rim_packs = 0, rim_errors = 0, rim_uses = 0;
};

World MakeWorld(FakeMemory& m) {
  World w;
  w.vehicle = m.Alloc(7300);
  w.entity = m.Alloc(400);
  w.info = m.Alloc(64);
  w.set = m.Alloc(cw::kSetBytes + 16);
  m.P32(w.vehicle + 20, w.entity);
  m.P32(w.entity + 80, w.info);
  m.P32(w.info + 32, w.set);
  m.P8(w.info + 40, 1);
  m.P8(w.set + cw::kSetHasLod0Byte, 1);
  PutIdentity(m, w.entity + 144, 10.0, 2.0, -30.0);
  PutIdentity(m, w.entity + 208, 10.0, 2.0, -30.0);
  m.P32(w.vehicle + 5996, 0xFFFFFFFFu);

  // the manager
  w.manager = m.Alloc(128);
  m.P32(0x8288E010u, w.manager);
  m.P32(w.manager + 0, 3);
  m.P32(w.manager + 4, 5);
  w.tyre_entries = m.Alloc(16 * 3);
  w.tyre_packs = m.Alloc(16 * 3);
  w.tyre_errors = m.Alloc(4 * 3);
  w.tyre_uses = m.Alloc(4 * 3);
  w.rim_entries = m.Alloc(16 * 5);
  w.rim_packs = m.Alloc(16 * 5);
  w.rim_errors = m.Alloc(4 * 5);
  w.rim_uses = m.Alloc(4 * 5);
  m.P32(w.manager + 28, w.tyre_entries);
  m.P32(w.manager + 32, w.tyre_packs);
  m.P32(w.manager + 36, w.tyre_errors);
  m.P32(w.manager + 40, w.tyre_uses);
  m.P32(w.manager + 44, w.rim_entries);
  m.P32(w.manager + 48, w.rim_packs);
  m.P32(w.manager + 52, w.rim_uses);
  m.P32(w.manager + 56, w.rim_errors);

  GeometrySpec tyre_spec;
  tyre_spec.vertices = 6;
  tyre_spec.indices = {0, 1, 2, 3, 4, 5, 0, 2, 4};
  w.tyre_geometry = MakeGeometry(m, tyre_spec);
  w.tyre_drawable = MakeDrawable(m, {{w.tyre_geometry}}, {0});
  GeometrySpec rim_a;
  rim_a.vertices = 5;
  rim_a.x0 = 3.0f;
  rim_a.indices = {0, 1, 2, 2, 3, 4};
  GeometrySpec rim_b;
  rim_b.vertices = 4;
  rim_b.x0 = 7.0f;
  w.rim_geometry_a = MakeGeometry(m, rim_a);
  w.rim_geometry_b = MakeGeometry(m, rim_b);
  w.rim_drawable = MakeDrawable(m, {{w.rim_geometry_a}, {w.rim_geometry_b}}, {0, 1});

  // tyre 2 good; tyre 0 has an error word; tyre 1 is unplaced
  const auto put = [&](uint32_t entries, uint32_t packs, uint32_t uses, uint32_t errors, uint32_t id, uint32_t drawable, uint32_t flags, uint32_t pack_flags, int32_t use, uint32_t error) {
    m.P32(entries + 16 * id + 0, drawable);
    m.P32(entries + 16 * id + 8, flags);
    m.P32(packs + 16 * id + 0, drawable != 0 ? m.Alloc(32) : 0);
    m.P32(packs + 16 * id + 8, pack_flags);
    m.P32(uses + 4 * id, uint32_t(use));
    m.P32(errors + 4 * id, error);
  };
  put(w.tyre_entries, w.tyre_packs, w.tyre_uses, w.tyre_errors, 2, w.tyre_drawable, cw::kPlacedMask, cw::kPlacedMask, 1, 0);
  put(w.tyre_entries, w.tyre_packs, w.tyre_uses, w.tyre_errors, 0, w.tyre_drawable, cw::kPlacedMask, cw::kPlacedMask, 1, 7);   // an error word
  put(w.tyre_entries, w.tyre_packs, w.tyre_uses, w.tyre_errors, 1, w.tyre_drawable, 0x10000000u, cw::kPlacedMask, 1, 0);        // not placed
  put(w.rim_entries, w.rim_packs, w.rim_uses, w.rim_errors, 4, w.rim_drawable, cw::kPlacedMask, cw::kPlacedMask, 2, 0);
  put(w.rim_entries, w.rim_packs, w.rim_uses, w.rim_errors, 3, w.rim_drawable, cw::kPlacedMask, cw::kPlacedMask, 0, 0);          // no use count
  put(w.rim_entries, w.rim_packs, w.rim_uses, w.rim_errors, 2, w.rim_drawable, cw::kPlacedMask, 0, 1, 0);                       // the pack is not placed

  // the vehicle: ids, flags, the object that hides the wheels, the wheels' pointers, the matrices
  m.P32(w.vehicle + cw::kVehicleTyreId, 2);
  m.P32(w.vehicle + cw::kVehicleRimId, 4);
  m.P8(w.vehicle + cw::kVehicleTyreOn, 1);
  m.P8(w.vehicle + cw::kVehicleRimOn, 1);
  w.hide = m.Alloc(300);
  m.P32(w.vehicle + cw::kVehicleWheelHide, w.hide);
  for (uint32_t i = 0; i < 4; ++i) {
    m.P32(w.vehicle + cw::kVehicleWheelPointers + 4 * i, 0x1000 + i);
    PutIdentity(m, w.vehicle + cw::kVehicleTyreMatrices + 64 * i, 10.0 + i, 0.4, -30.0 + 0.1 * i);
    PutIdentity(m, w.vehicle + cw::kVehicleWheelMatrices + 64 * i, 20.0 + i, 0.5, -30.0);
    PutIdentity(m, w.vehicle + cw::kVehicleBrakeMatrices + 128 * i, 30.0 + i, 0.6, -30.0);
    PutIdentity(m, w.vehicle + cw::kVehicleBrakeMatrices + 128 * i + 64, 40.0 + i, 0.7, -30.0);
  }
  return w;
}

uint32_t CountPart(const cw::CarWalk& walk, uint32_t part, bool in_use_only = false) {
  uint32_t n = 0;
  for (const cw::Geometry& g : walk.geometries) {
    n += g.part == part && (!in_use_only || g.in_use);
  }
  return n;
}

void WalkTests() {
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    const cw::WheelWalk& ww = walk.wheels;
    Check(ww.read && ww.manager == w.manager, "the manager and the ids are read");
    Check(ww.tyre_id == 2 && ww.rim_id == 4, "the vehicle's tyre and rim ids");
    Check(ww.tyre_ok && ww.rim_ok, "both resources are handed out by the accessors");
    Check(ww.tyre_drawable == w.tyre_drawable && ww.rim_drawable == w.rim_drawable, "the drawables are the entries'");
    Check(ww.tyre_on && ww.rim_on && !ww.hidden, "the draw's flags: tyre on, rim on, not hidden");
    Check(ww.present[0] && ww.present[1] && ww.present[2] && ww.present[3], "the four wheels are drawn");
    Check(ww.tyre_geometries == 1 && ww.rim_geometries == 2, "one tyre geometry, two rim geometries");
    Check(CountPart(walk, cw::kTyrePart, true) == 1 && CountPart(walk, cw::kRimPart, true) == 2, "the wheel geometries are in use");
    Check(walk.parts.size() == 2 && walk.parts[0].part == cw::kTyrePart && walk.parts[1].part == cw::kRimPart, "two entries: the tyre, then the rim");
    Check(walk.parts[0].valid && walk.parts[0].in_use && walk.parts[1].in_use, "the entries are valid and in use");
    // the bones: the rim's models are on bones 0 and 1
    std::vector<uint32_t> rim_bones;
    for (const cw::Geometry& g : walk.geometries) {
      if (g.part == cw::kRimPart) {
        rim_bones.push_back(g.bone);
      }
    }
    Check(rim_bones.size() == 2 && rim_bones[0] == 0 && rim_bones[1] == 1, "the rim's two models are on bones 0 and 1");
    for (const cw::Geometry& g : walk.geometries) {
      Check(g.read && g.vb_ok && g.ib_ok && g.entry_index < walk.parts.size() && walk.parts[g.entry_index].part == g.part, "a wheel geometry knows its entry");
    }
    // the same two geometries as parts of the model set share buffers with nothing: their mesh keys are distinct from each other
    const crt::Part dummy;
    (void)dummy;
    Check(crt::GeometryKey(walk.geometries[0]) != crt::GeometryKey(walk.geometries[1]), "two wheel geometries have two mesh keys");
  }
  // a wheel drawable with no bone table: bone 0
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    uint32_t lod_object = 0;
    m.Word(w.tyre_drawable + cw::kDrawableLod, &lod_object);
    m.P32(lod_object + 8, 0);  // L+8: the bone array
    cw::CarWalk walk;
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(walk.geometries.size() == 3 && walk.geometries[0].part == cw::kTyrePart && walk.geometries[0].bone == 0, "no bone table: the tyre geometry is on bone 0");
    Check(crt::PickGeometries(walk, 0, 0).size() == 3, "and is picked");
  }
  // each flag
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    m.P8(w.vehicle + cw::kVehicleTyreOn, 0);
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(CountPart(walk, cw::kTyrePart, true) == 0 && CountPart(walk, cw::kTyrePart) == 1 && CountPart(walk, cw::kRimPart, true) == 2, "tyre byte 0: the tyre is walked, not in use; the rim is");
  }
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    m.P8(w.vehicle + cw::kVehicleLoop44A, 1);  // V+6477: the rim is not drawn
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(!walk.wheels.rim_on && walk.wheels.tyre_on && CountPart(walk, cw::kRimPart, true) == 0 && CountPart(walk, cw::kTyrePart, true) == 1, "V+6477 set: no rim, the tyre stays");
  }
  for (const uint32_t offset : {266u, 267u, 268u}) {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    m.P8(w.hide + offset, 1);
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(walk.wheels.hidden && CountPart(walk, cw::kTyrePart, true) == 0 && CountPart(walk, cw::kRimPart, true) == 0, "the wheels-hidden object's byte stops the wheels (" + std::to_string(offset) + ")");
  }
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    for (uint32_t i = 0; i < 4; ++i) {
      m.P32(w.vehicle + cw::kVehicleWheelPointers + 4 * i, 0);
    }
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(!walk.wheels.present[0] && !walk.wheels.present[3] && CountPart(walk, cw::kTyrePart, true) == 1, "no wheel pointer at all: the resources are still in use (the pointers' meaning is a CHECK)");
    // the render copy counts as well
    m.P32(w.vehicle + cw::kVehicleWheelPointers + 16 + 4 * 2, 0x2000);
    cw::CarWalk again;
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &again);
    Check(!again.wheels.present[0] && again.wheels.present[2] && CountPart(again, cw::kTyrePart, true) == 1, "a wheel pointer in the render copy only still counts for that wheel");
    Check(cmp::WheelPlacements(cw::ReadWheelMatrices(m, w.vehicle), again.wheels.present, false, 0, cw::WheelPose::kTyreMatrix).size() == 1, "and then only that wheel is placed");
  }
  // the accessors' refusals
  const auto refused = [&](uint32_t tyre_id, uint32_t rim_id, const char* what, bool tyre_ok, bool rim_ok) {
    FakeMemory m;
    const World w = MakeWorld(m);
    m.P32(w.vehicle + cw::kVehicleTyreId, tyre_id);
    m.P32(w.vehicle + cw::kVehicleRimId, rim_id);
    cw::CarWalk walk;
    cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
    Check(walk.wheels.read && walk.wheels.tyre_ok == tyre_ok && walk.wheels.rim_ok == rim_ok, std::string("accessors: ") + what);
    Check(CountPart(walk, cw::kTyrePart) == (tyre_ok ? 1u : 0u) && CountPart(walk, cw::kRimPart) == (rim_ok ? 2u : 0u), std::string("geometries follow the accessors: ") + what);
  };
  refused(3, 5, "ids at the counts are out of range", false, false);
  refused(0xFFFFFFFFu, 0xFFFFFFFEu, "negative ids", false, false);
  refused(0, 3, "an error word / no use count", false, false);
  refused(1, 2, "an unplaced entry / an unplaced pack", false, false);
  refused(2, 4, "the good pair", true, true);
  {
    FakeMemory m;
    cw::CarWalk walk;
    const uint32_t vehicle = m.Alloc(7300);
    cw::WalkWheels(m, vehicle, cw::Options{}, &walk);  // the manager's pointer is 0 in this world
    Check(!walk.wheels.read && walk.geometries.empty(), "no manager: nothing is walked and nothing is claimed");
  }
  // inside WalkVehicle: the tallies count the wheel geometries
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::Options options;
    options.validate = true;
    cw::CarWalk walk;
    cw::WalkVehicle(m, w.vehicle, options, &walk);
    Check(walk.ok, "WalkVehicle walks a vehicle with no parts");
    Check(walk.all.geometries == 3 && walk.in_use.geometries == 3, "the wheel geometries are in the walk's tallies (3 walked, 3 in use)");
    Check(walk.in_use.triangles == 3 + 2 + 2, "and their triangles");
    Check(walk.validated_in_use == 3 && walk.resident_in_use == 3, "and are validated like any in-use geometry (readable data, indices in range, finite positions)");
    options.wheels = false;
    cw::CarWalk without;
    cw::WalkVehicle(m, w.vehicle, options, &without);
    Check(without.ok && without.geometries.empty() && !without.wheels.read, "options.wheels = false: no wheel is walked");
  }
  // PickGeometries takes the wheel geometries (rigid, in use, a bone)
  {
    FakeMemory m;
    const World w = MakeWorld(m);
    cw::CarWalk walk;
    cw::WalkVehicle(m, w.vehicle, cw::Options{}, &walk);
    const std::vector<uint32_t> picks = crt::PickGeometries(walk, 18, 0);
    Check(picks.empty(), "the fixture's wheel meshes are under 18 indices: the class's smallest-part rule leaves them out");
    const std::vector<uint32_t> all = crt::PickGeometries(walk, 0, 0);
    Check(all.size() == 3, "PickGeometries takes the three wheel geometries");
  }
}

void PlacementTests() {
  FakeMemory m;
  const World w = MakeWorld(m);
  const cw::WheelMatrices wm = cw::ReadWheelMatrices(m, w.vehicle);
  Check(wm.any(), "the wheel matrices are read");
  for (uint32_t i = 0; i < 4; ++i) {
    Check(wm.tyre_ok[i] && wm.rim_ok[i] && wm.aux1_ok[i] && wm.aux2_ok[i], "all four matrices of wheel " + std::to_string(i));
    Check(Near(wm.tyre[i].m[3][0], 10.0 + i) && Near(wm.rim[i].m[3][0], 20.0 + i) && Near(wm.aux1[i].m[3][0], 30.0 + i) && Near(wm.aux2[i].m[3][0], 40.0 + i),
          "the matrices come from V+1344+64w, V+1600+64w, V+1856+128w and V+1920+128w");
  }
  tp::Mat4 out;
  Check(cw::PlaceWheel(wm, 1, cw::WheelPose::kTyreMatrix, 0, &out) && Near(out.m[3][0], 11.0), "tyre matrix, bone 0, wheel 1");
  Check(cw::PlaceWheel(wm, 1, cw::WheelPose::kTyreMatrix, 1, &out) && Near(out.m[3][0], 12.0), "tyre matrix, bone 1 is the next matrix in the array");
  Check(!cw::PlaceWheel(wm, 3, cw::WheelPose::kTyreMatrix, 1, &out), "tyre matrix: bone 1 of the last wheel runs off the array");
  Check(cw::PlaceWheel(wm, 2, cw::WheelPose::kRimMatrix, 5, &out) && Near(out.m[3][0], 22.0), "rim matrix: every bone");
  Check(cw::PlaceWheel(wm, 0, cw::WheelPose::kRimAux1, 0, &out) && Near(out.m[3][0], 20.0), "A1 bone 0 is the wheel");
  Check(cw::PlaceWheel(wm, 0, cw::WheelPose::kRimAux1, 1, &out) && Near(out.m[3][0], 30.0), "A1 bone 1 is the brake (V+1856+128w)");
  Check(cw::PlaceWheel(wm, 3, cw::WheelPose::kRimAux1, 1, &out) && Near(out.m[3][0], 33.0), "A1 bone 1 of wheel 3");
  Check(cw::PlaceWheel(wm, 0, cw::WheelPose::kRimAux1, 2, &out) && Near(out.m[3][0], 20.0), "A1 bone 2 is the stack's second copy of the wheel");
  Check(cw::PlaceWheel(wm, 0, cw::WheelPose::kRimAux1, 3, &out) && Near(out.m[3][0], 40.0), "A1 bone 3 runs on into the stack's A2 brake");
  Check(!cw::PlaceWheel(wm, 0, cw::WheelPose::kRimAux1, 4, &out), "A1 has no bone 4");
  Check(cw::PlaceWheel(wm, 1, cw::WheelPose::kRimAux2, 1, &out) && Near(out.m[3][0], 41.0), "A2 bone 1 is V+1920+128w");
  Check(!cw::PlaceWheel(wm, 1, cw::WheelPose::kRimAux2, 3, &out), "A2 has no bone 3");
  Check(!cw::PlaceWheel(wm, 4, cw::WheelPose::kRimMatrix, 0, &out), "no wheel 4");
  Check(cw::DefaultWheelPose(false) == cw::WheelPose::kTyreMatrix && cw::DefaultWheelPose(true) == cw::WheelPose::kRimAux1, "the asm's own pose is each kind's default");

  // a matrix that is not readable (NaN) makes that hypothesis unusable for that wheel only
  m.PF(w.vehicle + cw::kVehicleTyreMatrices + 64 * 1 + 0, std::nanf(""));
  const cw::WheelMatrices broken = cw::ReadWheelMatrices(m, w.vehicle);
  Check(!broken.tyre_ok[1] && broken.tyre_ok[0] && !cw::PlaceWheel(broken, 1, cw::WheelPose::kTyreMatrix, 0, &out) && cw::PlaceWheel(broken, 1, cw::WheelPose::kRimMatrix, 0, &out),
        "a NaN in wheel 1's tyre matrix: no tyre for that wheel, its rim is still placed");
  Check(!cw::PlaceWheel(broken, 0, cw::WheelPose::kTyreMatrix, 1, &out), "tyre bone 1 of wheel 0 reads wheel 1's matrix: unusable now");

  // WheelPlacements: one per wheel the vehicle draws
  const bool all[4] = {true, true, true, true};
  const bool three[4] = {true, false, true, true};
  const bool none[4] = {false, false, false, false};
  Check(cmp::WheelPlacements(wm, all, false, 0, cw::WheelPose::kTyreMatrix).size() == 4, "four wheels, four placements");
  const std::vector<tp::Mat4> three_placed = cmp::WheelPlacements(wm, three, true, 1, cw::WheelPose::kRimAux1);
  Check(three_placed.size() == 3 && Near(three_placed[0].m[3][0], 30.0) && Near(three_placed[1].m[3][0], 32.0) && Near(three_placed[2].m[3][0], 33.0), "wheel 1 not drawn: three placements, in wheel order");
  Check(cmp::WheelPlacements(wm, none, false, 0, cw::WheelPose::kRimMatrix).size() == 4, "no wheel pointer read at all: all four are placed (the pointers' meaning is a CHECK)");
  Check(cmp::WheelPlacements(broken, all, false, 0, cw::WheelPose::kTyreMatrix).size() == 3, "an unreadable matrix drops that wheel");
}

// One matched pair for the calibration: a geometry of a kind and bone, the draw's matrix being wheel `w` placed by pose `truth`.
void PairFor(cmp::WheelCalibration* cal, const cw::WheelMatrices& wm, const bool present[4], bool rim, uint32_t bone, cw::WheelPose truth, uint32_t w, double error = 0.0) {
  tp::Mat4 drawn;
  Check(cw::PlaceWheel(wm, w, truth, bone, &drawn), "the draw's matrix is made");
  drawn.m[3][1] += error;
  cal->Add(rim, wm, present, bone, drawn);
}

void CalibrationTests() {
  FakeMemory m;
  const World w = MakeWorld(m);
  const cw::WheelMatrices wm = cw::ReadWheelMatrices(m, w.vehicle);
  const bool present[4] = {true, true, true, true};

  {
    cmp::WheelCalibration cal;
    Check(!cal.decided(false) && !cal.decided(true) && !cal.fits(false), "nothing decided at the start");
    Check(cal.chosen(false) == cw::WheelPose::kTyreMatrix && cal.chosen(true) == cw::WheelPose::kRimAux1, "the defaults before a decision");
    // 8 rim geometries' draws: bone 0 (the wheel) of each of the four wheels twice, and bone 1 (the brake: V+1856), the draws are exactly A1's
    for (uint32_t rep = 0; rep < 2; ++rep) {
      for (uint32_t wheel = 0; wheel < 4; ++wheel) {
        PairFor(&cal, wm, present, true, 0, cw::WheelPose::kRimAux1, wheel);
        PairFor(&cal, wm, present, true, 1, cw::WheelPose::kRimAux1, wheel);
      }
    }
    Check(cal.samples(true) == 16, "sixteen rim pairs counted");
    Check(cal.Decide(true) && cal.decided(true) && cal.fits(true) && cal.chosen(true) == cw::WheelPose::kRimAux1, "the rims: A1 fits exactly and is chosen");
    Check(cal.tally(true, cw::WheelPose::kRimAux1).worst < 1e-6, "its worst error is nothing");
    Check(cal.tally(true, cw::WheelPose::kRimAux2).Mean() > 1.0, "A2 (bone 1 = V+1920) is a metre off");
    Check(!cal.decided(false), "the tyres are a kind of their own: still undecided");
  }
  {
    cmp::WheelCalibration cal;
    // the draws are A2's brake: bone 1 is the second hypothesis
    for (uint32_t rep = 0; rep < 2; ++rep) {
      for (uint32_t wheel = 0; wheel < 4; ++wheel) {
        PairFor(&cal, wm, present, true, 0, cw::WheelPose::kRimAux2, wheel);
        PairFor(&cal, wm, present, true, 1, cw::WheelPose::kRimAux2, wheel);
      }
    }
    Check(cal.Decide(true) && cal.chosen(true) == cw::WheelPose::kRimAux2 && cal.fits(true), "when the draws are A2's, A2 is chosen");
  }
  {
    cmp::WheelCalibration cal;
    for (uint32_t rep = 0; rep < 3; ++rep) {
      for (uint32_t wheel = 0; wheel < 4; ++wheel) {
        PairFor(&cal, wm, present, false, 0, cw::WheelPose::kTyreMatrix, wheel);
      }
    }
    Check(cal.Decide(false) && cal.chosen(false) == cw::WheelPose::kTyreMatrix && cal.fits(false), "the tyres: V+1344 matrix fits");
    Check(!cal.decided(true), "the rims are untouched");
  }
  {
    cmp::WheelCalibration cal;
    // a tyre drawn at the RIM's matrix (a mix-up the calibration must find): the draws are V+1600
    for (uint32_t rep = 0; rep < 3; ++rep) {
      for (uint32_t wheel = 0; wheel < 4; ++wheel) {
        PairFor(&cal, wm, present, false, 0, cw::WheelPose::kRimMatrix, wheel);
      }
    }
    Check(cal.Decide(false) && cal.chosen(false) == cw::WheelPose::kRimMatrix && cal.fits(false), "a tyre the draws put at V+1600 is placed there");
  }
  {
    cmp::WheelCalibration cal;
    // too few pairs: no decision
    for (uint32_t wheel = 0; wheel < 4; ++wheel) {
      PairFor(&cal, wm, present, false, 0, cw::WheelPose::kTyreMatrix, wheel);
    }
    Check(!cal.Decide(false) && !cal.decided(false), "four pairs are too few (the minimum is eight)");
  }
  {
    cmp::WheelCalibration cal;
    // every draw is a metre above every hypothesis: chosen (the least bad) but it does not fit
    for (uint32_t rep = 0; rep < 3; ++rep) {
      for (uint32_t wheel = 0; wheel < 4; ++wheel) {
        PairFor(&cal, wm, present, false, 0, cw::WheelPose::kTyreMatrix, wheel, 1.0);
      }
    }
    Check(cal.Decide(false) && cal.decided(false) && !cal.fits(false), "a metre off everywhere: decided, no fit: no wheel would be placed");
    cal.NewWindow();
    Check(cal.samples(false) == 0 && cal.decided(false), "a new window clears the tallies and keeps the choice");
  }
  {
    // which wheel a draw is of is not told: the best of the four counts; and only the wheels the vehicle draws
    cmp::WheelCalibration cal;
    const bool two[4] = {false, false, true, false};
    for (uint32_t rep = 0; rep < 10; ++rep) {
      PairFor(&cal, wm, two, false, 0, cw::WheelPose::kTyreMatrix, 2);
    }
    Check(cal.Decide(false) && cal.fits(false), "only wheel 2 drawn: ten pairs of wheel 2's draw fit");
    cmp::WheelCalibration other;
    for (uint32_t rep = 0; rep < 10; ++rep) {
      PairFor(&other, wm, two, false, 0, cw::WheelPose::kTyreMatrix, 0);  // a draw of wheel 0 when only wheel 2 is drawn
    }
    Check(other.Decide(false) && !other.fits(false), "a draw of an undrawn wheel does not fit the one that is drawn");
  }
  {
    // the proof's helper: the nearest wheel's placement
    tp::Mat4 drawn, best;
    cw::PlaceWheel(wm, 3, cw::WheelPose::kRimMatrix, 0, &drawn);
    drawn.m[3][2] += 0.01;
    Check(cmp::BestWheelPlacement(wm, present, 0, cw::WheelPose::kRimMatrix, drawn, &best) && Near(best.m[3][0], 23.0), "BestWheelPlacement finds wheel 3's placement for wheel 3's draw");
    const bool nobody[4] = {false, false, false, false};
    Check(cmp::BestWheelPlacement(wm, nobody, 0, cw::WheelPose::kRimMatrix, drawn, &best), "with no pointer read, all four are candidates");
    Check(!cmp::BestWheelPlacement(wm, present, 9, cw::WheelPose::kRimAux1, drawn, &best), "a bone the array has not: no placement");
  }
}

void KindTests() {
  Check(cmp::KindOf(0) == cmp::Kind::kBody && cmp::KindOf(54) == cmp::Kind::kBody && cmp::KindOf(cw::kBodyPart) == cmp::Kind::kBody, "the model set's parts and the body are the body kind");
  Check(cmp::KindOf(cw::kTyrePart) == cmp::Kind::kWheel && cmp::KindOf(cw::kRimPart) == cmp::Kind::kWheel, "tyre and rim are wheels");
  Check(cmp::KindOf(cw::kModelPart) == cmp::Kind::kModel, "a traffic car's drawable is the model kind");
  Check(cw::IsWheelPart(cw::kTyrePart) && cw::IsWheelPart(cw::kRimPart) && !cw::IsWheelPart(cw::kBodyPart) && cw::IsRimPart(cw::kRimPart) && !cw::IsRimPart(cw::kTyrePart),
        "the wheel part tests");
  Check(cw::kTyrePart != cw::kRimPart && cw::kModelPart != cw::kRimPart && cw::kBodyPart < cw::kTyrePart, "the pseudo parts are distinct and above the body's");
  cmp::Composition c;
  cmp::Count(&c, 2);
  cmp::Count(&c, cw::kBodyPart);
  cmp::Count(&c, cw::kTyrePart);
  cmp::Count(&c, cw::kTyrePart);
  cmp::Count(&c, cw::kRimPart);
  cmp::Count(&c, cw::kModelPart);
  Check(c.body == 2 && c.tyres == 2 && c.rims == 1 && c.model == 1 && c.driver == 0 && c.wheels() == 3 && c.total() == 6, "Composition counts instances by kind and never a driver");
  c.skinned_left_out = 4;
  const std::string racer = cmp::Describe(c, false);
  const std::string traffic = cmp::Describe(c, true);
  Check(racer.find("body 2") != std::string::npos && racer.find("wheels 3 (2 tyre + 1 rim") != std::string::npos && racer.find("driver 0") != std::string::npos && racer.find("4 skinned") != std::string::npos,
        "the racer's log words: body, wheels, driver, the skinned geometries left out");
  Check(traffic.find("model 1") != std::string::npos && traffic.find("driver 0") != std::string::npos, "the traffic car's log words: the model, driver 0");

  // SkinnedInUse
  FakeMemory m;
  const World w = MakeWorld(m);
  cw::CarWalk walk;
  cw::WalkWheels(m, w.vehicle, cw::Options{}, &walk);
  Check(cmp::SkinnedInUse(walk) == 0, "no wheel geometry is skinned");
  walk.geometries[0].palette_count = 4;
  Check(cmp::SkinnedInUse(walk) == 1, "an in-use skinned geometry is counted");
  walk.geometries[0].in_use = false;
  Check(cmp::SkinnedInUse(walk) == 0, "a skinned geometry that is not in use is not");
}

}  // namespace

int main() {
  WalkTests();
  PlacementTests();
  CalibrationTests();
  KindTests();
  std::printf("car_model_wheel_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
