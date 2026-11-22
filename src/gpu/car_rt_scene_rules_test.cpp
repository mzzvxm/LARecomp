// Check of the car ray tracing scene's rules (car_rt_scene_rules.h): the instance transform and the camera's z direction, the masks and the
// table's rows, the clean-up of a mesh read from guest memory, the range allocator (and when a copy into a grown buffer needs a barrier), the
// eviction policy and the build budget, and the check
// pass's statistics and scope (which vehicle pixels the scene is meant to hold). Given the src/gpu folder it also checks that
// hlsl/car_rt_check_cs.hlsl counts with the header's numbers. A program of its
// own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_scene_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_scene_rules_test [<src/gpu dir>]

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "car_rt_scene_rules.h"

namespace rr = mcla::gpu::car_rt_rules;
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

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 m = tp::Identity();
  m.m[3][0] = x;
  m.m[3][1] = y;
  m.m[3][2] = z;
  return m;
}

// A rotation about the vertical axis (row vectors: v' = v R).
tp::Mat4 RotationY(double angle) {
  tp::Mat4 m = tp::Identity();
  m.m[0][0] = std::cos(angle);
  m.m[0][2] = -std::sin(angle);
  m.m[2][0] = std::sin(angle);
  m.m[2][2] = std::cos(angle);
  return m;
}

// (p, 1) * m.
void Apply(const tp::Mat4& m, const double p[3], double out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
  }
}

// out = the 3x4 (column vectors) applied to (p, 1).
void Apply34(const float m[3][4], const double p[3], double out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = double(m[i][0]) * p[0] + double(m[i][1]) * p[1] + double(m[i][2]) * p[2] + double(m[i][3]);
  }
}

// ---------------------------------------------------------------------------

void TestTransform() {
  // A game camera: at (10, 2, 5) looking down -z of its own view space (z backward), with a yaw of 0.3 rad.
  const tp::Mat4 camera_to_world = tp::Mul(RotationY(0.3), Translation(10, 2, 5));
  tp::Mat4 view;
  Check(tp::Inverse(camera_to_world, &view), "the camera's matrix has an inverse");
  const tp::Mat4 model = tp::Mul(RotationY(-0.7), Translation(12, 2.5, 3));  // a part placed near the camera
  const double p[3] = {0.4, -0.2, 1.3};

  float t[3][4];
  Check(rr::PackInstanceTransform(model, view, false, t), "a rigid placement packs");
  double world[3], expected[3], got[3];
  Apply(model, p, world);
  Apply(view, world, expected);
  Apply34(t, p, got);
  Check(Near(got[0], expected[0]) && Near(got[1], expected[1]) && Near(got[2], expected[2]), "the 3x4 does what (p 1) * model * view does");

  float flipped[3][4];
  Check(rr::PackInstanceTransform(model, view, true, flipped), "a flipped placement packs");
  Apply34(flipped, p, got);
  Check(Near(got[0], expected[0]) && Near(got[1], expected[1]) && Near(got[2], -expected[2]), "flipping negates the view's z only");

  // The row-to-column transposition: a pure translation lands in the last column.
  float tr[3][4];
  Check(rr::PackInstanceTransform(Translation(1, 2, 3), tp::Identity(), false, tr), "a translation packs");
  Check(tr[0][3] == 1.0f && tr[1][3] == 2.0f && tr[2][3] == 3.0f && tr[0][0] == 1.0f && tr[1][1] == 1.0f && tr[2][2] == 1.0f &&
            tr[0][1] == 0.0f && tr[1][0] == 0.0f,
        "the translation is the 3x4's last column, the rotation the identity");

  // A rotation's rows and columns: v' = v R, the 3x4 is R transposed.
  float rot[3][4];
  Check(rr::PackInstanceTransform(RotationY(0.5), tp::Identity(), false, rot), "a rotation packs");
  Check(Near(rot[0][2], std::sin(0.5)) && Near(rot[2][0], -std::sin(0.5)), "the rotation is transposed into the column form");

  // A scale (the squash and stretch of a part) packs; no volume and perspective do not.
  tp::Mat4 scale = tp::Identity();
  scale.m[1][1] = 0.9;
  float sc[3][4];
  Check(rr::PackInstanceTransform(scale, tp::Identity(), false, sc), "a scale packs");
  tp::Mat4 flat = tp::Identity();
  flat.m[1][1] = 0.0;
  Check(!rr::PackInstanceTransform(flat, tp::Identity(), false, sc), "a matrix with no volume is refused");
  tp::Mat4 perspective = tp::Identity();
  perspective.m[2][3] = 0.01;
  Check(!rr::PackInstanceTransform(perspective, tp::Identity(), false, sc), "a perspective column is refused");
  tp::Mat4 nan = tp::Identity();
  nan.m[3][0] = std::nan("");
  Check(!rr::PackInstanceTransform(nan, tp::Identity(), false, sc), "a matrix that is not a number is refused");
}

void TestViewSign() {
  // The game's camera at the origin looking down -z: the car at (0, 0, -5) is in front (z = -5 in the game's view space).
  const double car_game[3] = {0, 0, -5};
  Check(rr::ViewZSign(tp::Identity(), car_game) == -1, "a car along -z is on the game's side");
  const double car_ao[3] = {0, 0, 5};
  Check(rr::ViewZSign(tp::Identity(), car_ao) == 1, "a car along +z is on the AO's side");
  const double at_camera[3] = {0, 0.2, 0.1};
  Check(rr::ViewZSign(tp::Identity(), at_camera) == 0, "a car at the camera's plane cannot be told");
  // With the camera moved the sign is that of the view-space z.
  const tp::Mat4 view = Translation(0, 0, 20);
  const double world[3] = {0, 0, -30};
  Check(rr::ViewZSign(view, world) == -1, "the sign follows the view matrix, not the world's z");
  // The flipped matrix of the game's view puts the car at +z.
  float t[3][4];
  Check(rr::PackInstanceTransform(Translation(0, 0, -5), tp::Identity(), true, t) && t[2][3] == 5.0f, "flipping a car 5 m in front gives z = +5");
}

void TestMasks() {
  Check(rr::InstanceMask(true, true, true) == rr::kMaskTyres, "a tyre is a tyre, drawn after the world or not");
  Check(rr::InstanceMask(false, true, true) == rr::kMaskGlass, "a part after the world, with an informative rule, is glass");
  Check(rr::InstanceMask(false, true, false) == rr::kMaskBody, "with an uninformative rule everything is body");
  Check(rr::InstanceMask(false, false, true) == rr::kMaskBody, "a part before the world is a body");
  Check(!rr::GlassRuleInformative(0, 200), "no draw after the world: nothing to tell");
  Check(!rr::GlassRuleInformative(200, 200), "every draw after the world (a view where the car came after it): nothing to tell");
  Check(rr::GlassRuleInformative(18, 210), "18 of 210 after the world: the rule tells glass");
  const rr::InstanceRow row = rr::MakeRow(300, 120, rr::kMaskGlass, true, 7);
  Check(row.first_index == 300 && row.first_vertex == 120 && row.draw == 7 && (row.flags & 0xFFu) == rr::kMaskGlass &&
            (row.flags & rr::kFlagAfterWorld) != 0,
        "a row holds the mesh's place, the mask, the flag and the draw");
  Check(rr::kMaskBody == 1 && rr::kMaskGlass == 2 && rr::kMaskTyres == 4 && rr::kMaskDriver == 8 && rr::kMaskOthers == 0x10, "the masks are the contract's");
  Check((rr::kMaskOthers & (rr::kMaskBody | rr::kMaskGlass | rr::kMaskTyres | rr::kMaskDriver)) == 0, "the others' mask is a bit of its own");
  const rr::InstanceRow other = rr::MakeRow(10, 20, rr::kMaskOthers, true, 33);
  Check((other.flags & 0xFFu) == rr::kMaskOthers && (other.flags & rr::kFlagAfterWorld) != 0 && other.draw == 33, "a row of another car's instance holds the others' mask and its car's row in draw");
}

void TestMesh() {
  // A unit quad in the xy plane, two triangles, no normals.
  std::vector<float> positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
  std::vector<float> normals;
  rr::MeshClean report = rr::CleanMesh(&positions, &indices, &normals);
  Check(report.normals_made && report.triangles_dropped == 0 && normals.size() == 12, "a mesh with no normals gets smooth ones");
  bool all_z = true;
  for (size_t v = 0; v < 4; ++v) {
    all_z = all_z && Near(normals[v * 3], 0.0) && Near(normals[v * 3 + 1], 0.0) && Near(std::fabs(normals[v * 3 + 2]), 1.0);
  }
  Check(all_z, "the smooth normals of a flat quad are its normal");

  // A corner of a cube: the three faces' normals meet in the diagonal.
  std::vector<float> cp = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::vector<uint32_t> ci = {0, 2, 1, 0, 1, 3, 0, 3, 2};
  std::vector<float> cn;
  rr::CleanMesh(&cp, &ci, &cn);
  const double length = std::sqrt(double(cn[0]) * cn[0] + double(cn[1]) * cn[1] + double(cn[2]) * cn[2]);
  Check(Near(length, 1.0), "smooth normals are unit length");

  // Bad triangles: out of range, a repeated vertex, a position that is not a number.
  positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, std::nanf("")};
  indices = {0, 1, 2, 0, 1, 9, 2, 2, 1, 0, 2, 3};
  normals.clear();
  report = rr::CleanMesh(&positions, &indices, &normals);
  Check(report.triangles_in == 4 && report.triangles_dropped == 3 && indices.size() == 3, "out of range, repeated and NaN triangles are dropped");
  Check(indices[0] == 0 && indices[1] == 1 && indices[2] == 2, "the good triangle is kept in place");

  // Given normals are made unit; a zero one is replaced by the smooth normal.
  positions = {0, 0, 0, 1, 0, 0, 1, 1, 0};
  indices = {0, 1, 2};
  normals = {0, 0, 5, 0, 0, 0, 0, 0, -2};
  report = rr::CleanMesh(&positions, &indices, &normals);
  Check(!report.normals_made && report.normals_replaced == 1, "a zero normal is replaced, the others kept");
  Check(Near(normals[2], 1.0) && Near(std::fabs(normals[5]), 1.0) && Near(normals[8], -1.0), "given normals are normalised");
  // The wrong number of normals: made anew.
  normals = {0, 0, 1};
  report = rr::CleanMesh(&positions, &indices, &normals);
  Check(report.normals_made && normals.size() == 9, "a normal array of the wrong length is made anew");
}

void TestAllocator() {
  rr::RangeAllocator a;
  a.Reset(1000);
  uint64_t o0 = 0, o1 = 0, o2 = 0, o3 = 0;
  Check(a.Alloc(100, 1, &o0) && o0 == 0, "the first allocation starts at 0");
  Check(a.Alloc(200, 1, &o1) && o1 == 100, "the second follows");
  Check(a.Alloc(50, 64, &o2) && o2 == 320, "alignment moves the start up");
  Check(a.used() == 350, "used adds up");
  a.Free(o1, 200);
  Check(a.used() == 150, "a free gives its bytes back");
  Check(a.Alloc(150, 1, &o3) && o3 == 100, "first fit takes the freed range");
  a.Free(o0, 100);
  a.Free(o3, 150);
  Check(a.free_ranges() == 2 && a.used() == 50, "free ranges beside each other join (0-320 is one, the aligned block at 320 holds the rest apart)");
  a.Free(o2, 50);
  Check(a.free_ranges() == 1 && a.largest_free() == 1000 && a.used() == 0, "everything free is one range again");
  Check(!a.Alloc(1001, 1, &o0), "more than the capacity does not fit");
  Check(!a.Alloc(0, 1, &o0), "nothing is not an allocation");

  // High water and growth.
  a.Reset(100);
  a.Alloc(30, 1, &o0);
  a.Alloc(30, 1, &o1);
  Check(a.high_water() == 60, "the high water is the end of the last range in use");
  a.Free(o1, 30);
  Check(a.high_water() == 30, "freeing the last range lowers it");
  a.Alloc(60, 1, &o2);
  Check(!a.Alloc(20, 1, &o3), "full");
  a.Grow(300);
  Check(a.capacity() == 300 && a.Alloc(150, 1, &o3) && o3 == 90, "growing adds free space at the end");
  a.Grow(200);
  Check(a.capacity() == 300, "growing never shrinks");

  Check(rr::GrownCapacity(4, 5, 1, 100) == 8, "a growth doubles");
  Check(rr::GrownCapacity(4, 30, 8, 100) == 32, "a growth takes what is needed, rounded to the step");
  Check(rr::GrownCapacity(60, 70, 8, 100) == 100, "a growth stops at the limit");
  Check(rr::GrownCapacity(60, 130, 8, 100) == 130, "but never below what is needed");
}

// The 0.11 line's validation error (SYNC-HAZARD-WRITE-AFTER-WRITE, copies of 2544 and 216 bytes side by side): a growth's copy writes everything
// below the high water, the hole a given-back mesh left included, and first fit puts the next meshes into that hole.
void TestGrowthCopyOrder() {
  rr::RangeAllocator a;
  a.Reset(1000);
  uint64_t first = 0, given_back = 0, last = 0, m1 = 0, m2 = 0, m3 = 0;
  Check(a.Alloc(300, 1, &first) && a.Alloc(250, 1, &given_back) && a.Alloc(100, 1, &last) && given_back == 300 && last == 550, "three meshes");
  a.Free(given_back, 250);  // one is given back: a hole of 250 below the last
  const uint64_t copied = a.high_water();
  Check(copied == 650, "the growth's copy covers the hole and both meshes around it");
  a.Grow(2000);
  Check(a.high_water() == copied, "growing adds free space past the high water only");

  Check(a.Alloc(212, 1, &m1) && m1 == 300, "first fit gives the hole to the next mesh");
  Check(rr::CopyNeedsBarrier(copied, m1, 212), "its copy writes bytes the growth's copy wrote: it needs a barrier");
  Check(a.Alloc(18, 1, &m2) && m2 == 512 && rr::CopyNeedsBarrier(copied, m2, 18), "the mesh beside it in the hole as well");
  Check(a.Alloc(100, 1, &m3) && m3 == copied && !rr::CopyNeedsBarrier(copied, m3, 100),
        "a mesh past the high water was never written in the list: no barrier");
  Check(!rr::CopyNeedsBarrier(copied, copied, 10), "a range that starts where the growth's copy ended is apart from it");
  Check(rr::CopyNeedsBarrier(copied, copied - 1, 10), "one byte inside is not");
  Check(!rr::CopyNeedsBarrier(0, 0, 10), "no growth copy in the list: no barrier");
  Check(!rr::CopyNeedsBarrier(copied, 100, 0), "a copy of no bytes needs none");
}

void TestEviction() {
  std::vector<rr::MeshUse> meshes = {{1, 10, 100}, {2, 10, 5000}, {3, 10, 4990}, {4, 10, 4999}};
  rr::EvictionRequest request;
  request.frame = 5000;
  request.cap_bytes = 1000;
  rr::EvictionPlan plan = rr::PlanEviction(meshes, request);
  Check(plan.keys.size() == 1 && plan.keys[0] == 1 && plan.fits, "a mesh not seen for over 300 frames goes");

  request.cap_bytes = 35;  // 40 resident after the old one goes... 30 resident, 35 cap with 10 needed -> must free
  request.need_bytes = 10;
  plan = rr::PlanEviction(meshes, request);
  // resident after the old one: 30; 30 + 10 > 35: evict the least recently seen unprotected (3 at 4990), then 3 is enough (20 + 10 <= 35).
  Check(plan.keys.size() == 2 && plan.keys[0] == 1 && plan.keys[1] == 3 && plan.fits, "over the cap the least recently seen goes next");

  request.cap_bytes = 15;
  plan = rr::PlanEviction(meshes, request);
  // 4 (4999) is within 2 frames? age 1 <= protect 2: protected; 2 (5000) protected. After 1 and 3: 20 + 10 > 15: nothing else may go.
  Check(plan.keys.size() == 2 && !plan.fits, "a mesh seen in the last frames is never taken for the cap: no fit is said");

  // A tie goes to the larger.
  std::vector<rr::MeshUse> tie = {{7, 5, 100}, {8, 50, 100}};
  rr::EvictionRequest t;
  t.frame = 200;
  t.cap_bytes = 60;
  t.need_bytes = 10;
  plan = rr::PlanEviction(tie, t);
  Check(plan.keys.size() == 1 && plan.keys[0] == 8 && plan.fits, "at the same age the larger mesh goes first");

  Check(rr::TakeWithinBudget({100, 100, 100}, 250) == 2, "builds within the budget");
  Check(rr::TakeWithinBudget({1000, 100}, 250) == 1, "one mesh is always taken");
  Check(rr::TakeWithinBudget({}, 250) == 0, "none pending, none taken");
  Check(rr::TotalsChangedMuch(~uint64_t(0), 5), "the first totals are logged");
  Check(!rr::TotalsChangedMuch(10ull << 20, 11ull << 20), "a small change is not");
  Check(rr::TotalsChangedMuch(10ull << 20, 14ull << 20), "a big one is");
  Check(rr::TotalsChangedMuch(1ull << 20, 3ull << 20), "growth from a small total counts by the mebibyte");
}

void TestStats() {
  uint32_t words[rr::kStatsWords] = {};
  rr::CheckSummary s = rr::DecodeCheckStats(words);
  Check(s.vehicle == 0 && s.coverage == 0.0 && s.mean_error == 0.0, "no pixels, no numbers");
  words[rr::kStatVehicle] = 1000;
  words[rr::kStatCovered] = 900;
  words[rr::kStatErrorSum] = 900;  // 900 / 1000 / 900 = 0.001
  words[rr::kStatHistogram + 0] = 600;
  words[rr::kStatHistogram + 1] = 200;
  words[rr::kStatHistogram + 3] = 90;
  words[rr::kStatHistogram + 6] = 10;
  words[rr::kStatNearer] = 9;
  words[rr::kStatFarther] = 18;
  words[rr::kStatGlass] = 100;
  words[rr::kStatBody] = 800;
  s = rr::DecodeCheckStats(words);
  Check(s.vehicle == 1000 && s.covered == 900 && s.missed == 100 && Near(s.coverage, 0.9), "the coverage is the covered over the vehicle pixels");
  Check(Near(s.mean_error, 0.001), "the mean error is the sum in its unit over the covered");
  Check(Near(s.p50, 0.0025) && Near(s.p95, 0.02), "the percentiles are bucket edges");
  Check(Near(s.within_1pct, 800.0 / 900.0), "within 1% is the first three buckets");
  Check(Near(s.nearer, 0.01) && Near(s.farther, 0.02), "nearer and farther are shares of the covered");
  Check(s.glass == 100 && s.body == 800 && s.tyres == 0, "the hits by class");
  Check(s.other == 0, "the covered pixels the three classes count leave no other");
  words[rr::kStatBody] = 600;
  s = rr::DecodeCheckStats(words);
  Check(s.other == 200, "covered pixels no class counts are the other cars' (and the driver's)");
  words[rr::kStatBody] = 800;
  words[rr::kStatCovered] = 5000;  // more than the vehicle pixels: clamped, never a negative miss
  s = rr::DecodeCheckStats(words);
  Check(s.covered == 1000 && s.missed == 0, "a covered count past the vehicle pixels is clamped");

  // The scope: the pixels of cars outside the selection are counted apart; they are in neither the vehicle pixels nor the misses nor the coverage.
  uint32_t scoped[rr::kStatsWords] = {};
  scoped[rr::kStatVehicle] = 200000;
  scoped[rr::kStatCovered] = 199000;
  scoped[rr::kStatOutside] = 48000;
  scoped[rr::kStatOthers] = 30000;
  scoped[rr::kStatErrorSum] = 1000;
  scoped[rr::kStatHistogram] = 199000;
  s = rr::DecodeCheckStats(scoped);
  Check(s.vehicle == 200000 && s.outside == 48000 && s.others == 30000, "the vehicle pixels, the ones outside the scene by design and the other cars' share");
  Check(s.missed == 1000 && Near(s.coverage, 0.995), "the coverage is over the pixels the scene is meant to hold only");
  scoped[rr::kStatOthers] = 900000;
  s = rr::DecodeCheckStats(scoped);
  Check(s.others == 200000, "the other cars' share is never more than the vehicle pixels");
  uint32_t only_outside[rr::kStatsWords] = {};
  only_outside[rr::kStatOutside] = 1234;
  s = rr::DecodeCheckStats(only_outside);
  Check(s.vehicle == 0 && s.outside == 1234 && s.missed == 0 && s.coverage == 0.0, "pixels outside the scene alone leave no coverage to judge");
  Check(rr::kStatsWords % 4 == 0 && rr::kStatOutside < rr::kStatsWords && rr::kStatOthers < rr::kStatsWords, "the counter words fit the buffer, a multiple of 16 bytes");
}

// The scope: which vehicle pixels the scene is meant to hold. A view-space point of a pixel goes in the player's box, in a kept other car's box
// (grown by the margin) or in none (a car outside the selection).
void TestScope() {
  rr::ScopeBox player;
  player.lo[0] = -1.0f; player.lo[1] = -0.2f; player.lo[2] = 5.0f;
  player.hi[0] = 1.0f;  player.hi[1] = 1.4f;  player.hi[2] = 9.5f;
  rr::ScopeBox others[2];
  others[0].lo[0] = 2.0f; others[0].lo[1] = -0.2f; others[0].lo[2] = 8.0f;
  others[0].hi[0] = 4.0f; others[0].hi[1] = 1.4f;  others[0].hi[2] = 12.5f;
  others[1].lo[0] = -5.0f; others[1].lo[1] = -0.2f; others[1].lo[2] = 14.0f;
  others[1].hi[0] = -3.0f; others[1].hi[1] = 1.4f;  others[1].hi[2] = 18.5f;

  const float in_player[3] = {0.0f, 0.5f, 7.0f};
  const float in_first[3] = {3.0f, 0.5f, 10.0f};
  const float in_second[3] = {-4.0f, 0.5f, 16.0f};
  const float nowhere[3] = {10.0f, 0.5f, 30.0f};
  Check(rr::ScopeOf(in_player, &player, others, 2) == rr::kScopePlayer, "a pixel in the player's box is the player's");
  Check(rr::ScopeOf(in_first, &player, others, 2) == rr::kScopeOther, "a pixel in a kept car's box is that car's");
  Check(rr::ScopeOf(in_second, &player, others, 2) == rr::kScopeOther, "a pixel in the second kept car's box too");
  Check(rr::ScopeOf(nowhere, &player, others, 2) == rr::kScopeOutside, "a pixel in no box is of a car outside the selection: not in the scene by design");
  Check(rr::ScopeOf(in_first, &player, others, 0) == rr::kScopeOutside, "with no car kept (the cars 0 switch) every other car's pixel is outside");
  Check(rr::ScopeOf(in_player, &player, nullptr, 0) == rr::kScopePlayer, "the player's car is in whatever the other cars are");

  // A pixel just past the other car's edge is in by the margin and out beyond it; the player's box is as the host made it (no margin here).
  const float edge_in[3] = {4.0f + rr::kScopeBoxMargin * 0.9f, 0.5f, 10.0f};
  const float edge_out[3] = {4.0f + rr::kScopeBoxMargin * 1.1f, 0.5f, 10.0f};
  Check(rr::ScopeOf(edge_in, &player, others, 2) == rr::kScopeOther, "the other car's box is grown by the margin");
  Check(rr::ScopeOf(edge_out, &player, others, 2) == rr::kScopeOutside, "and not by more");
  const float player_edge[3] = {1.0f + rr::kScopeBoxMargin * 0.5f, 0.5f, 7.0f};
  Check(rr::ScopeOf(player_edge, &player, others, 2) == rr::kScopeOutside, "the player's box is taken as given (the host grew it)");

  // A neighbour's pixel standing in front of the player's car (a car between the camera and the car, not kept) is outside, not a miss.
  const float in_front[3] = {0.2f, 0.5f, 3.0f};
  Check(rr::ScopeOf(in_front, &player, others, 2) == rr::kScopeOutside, "an unkept car in front of the player's is outside the scene by design");

  // Boxes past the table's count are not read.
  Check(rr::ScopeOf(in_second, &player, others, 1) == rr::kScopeOutside, "only the boxes counted are read");

  // Without the player's box nothing can be told from its pixels: a pixel in no other car's box is counted as the player's.
  Check(rr::ScopeOf(nowhere, nullptr, others, 2) == rr::kScopePlayer, "no player's box: a pixel in no kept car's box counts");
  Check(rr::ScopeOf(in_first, nullptr, others, 2) == rr::kScopeOther, "no player's box: a kept car's pixel is still that car's");
  Check(rr::kScopeFlagPlayerBox == 1u, "the player's box flag is bit 0");
}


// The probe (RT-MODEL-CARS-4): the first cars' two rows as the shader read them, against what the host wrote.
void TestProbe() {
  Check(rr::kStatProbe == 20 && rr::kProbeCars == 4 && rr::kProbeWordsPerCar == 8 && rr::kStatsWords == rr::kStatProbe + rr::kProbeCars * rr::kProbeWordsPerCar,
        "the probe's words follow the counters'");
  Check(rr::kStatsWords % 4 == 0 && rr::kStatOthers < rr::kStatProbe, "the buffer is a multiple of 16 bytes and the probe overlaps no counter");
  uint32_t words[rr::kStatsWords] = {};
  // car 0: centre (1, 2, 3), radius -1.6 (not drawn), paint (0.25, 0.125, 0.0625) known; car 1: a car with no paint
  const float rows[2][8] = {{1.0f, 2.0f, 3.0f, -1.6f, 0.25f, 0.125f, 0.0625f, 0.0f}, {4.0f, 5.0f, 6.0f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f}};
  std::memcpy(words + rr::kStatProbe, rows[0], sizeof(rows[0]));
  std::memcpy(words + rr::kStatProbe + rr::kProbeWordsPerCar, rows[1], sizeof(rows[1]));
  words[rr::kStatProbe + 7] = 1u;  // flags: the paint is known
  const rr::ProbeCar a = rr::DecodeProbe(words, 0, 2);
  const rr::ProbeCar b = rr::DecodeProbe(words, 1, 2);
  Check(a.read && Near(a.centre[0], 1.0) && Near(a.centre[2], 3.0) && Near(a.radius, -1.6) && Near(a.paint[0], 0.25) && Near(a.paint[2], 0.0625) && a.flags == 1u,
        "a car's two rows are decoded: centre, radius, paint, flags");
  Check(b.read && Near(b.radius, 1.6) && b.flags == 0u && b.paint[0] == 0.0f, "a car with no paint: zeros and no flag");
  Check(!rr::DecodeProbe(words, 2, 2).read && !rr::DecodeProbe(words, 4, 9).read, "a slot past the cars' count or past the probe's cars is not read");
  const float centre0[3] = {1.0f, 2.0f, 3.0f};
  const float paint0[3] = {0.25f, 0.125f, 0.0625f};
  Check(rr::ProbeMatches(a, centre0, -1.6f, true, paint0), "the shader read exactly the bits the host wrote");
  const float paint_off[3] = {0.25f, 0.125f, 0.0626f};
  Check(!rr::ProbeMatches(a, centre0, -1.6f, true, paint_off), "one bit of a colour different: not the same");
  Check(!rr::ProbeMatches(a, centre0, -1.6f, false, paint0), "the host wrote no paint but the shader read one: not the same");
  const float centre1[3] = {4.0f, 5.0f, 6.0f};
  const float none[3] = {0.0f, 0.0f, 0.0f};
  Check(rr::ProbeMatches(b, centre1, 1.6f, false, none), "a car with no paint: zeros and a clear flag are the same");
  Check(!rr::ProbeMatches(rr::ProbeCar{}, centre1, 1.6f, false, none), "a slot that was not read is never the same");
}

// Whether a device can trace the car (UnsupportedReason): the decision, with a stand-in for plume::RenderDeviceCapabilities that has the members it reads.
struct FakeCaps {
  bool raytracing = false;
  bool raytracingStateUpdate = false;
  bool rayQuery = false;
  bool bufferDeviceAddress = false;
  bool descriptorIndexing = false;
  uint32_t shaderModel = 0;
};

// The wave operations the decision asks the device for (backend::WaveOps in the renderer): the ray tracing shaders' basic, ballot and arithmetic, in compute.
struct FakeWaves {
  bool known = true;
  bool basic = true;
  bool ballot = true;
  bool arithmetic = true;
  bool in_compute = true;
};

void TestSupportDecision() {
  const FakeWaves all_waves;
  // Direct3D 12: DXR 1.1 and shader model 6.5.
  FakeCaps d3d;
  d3d.raytracing = true;
  d3d.raytracingStateUpdate = true;
  d3d.shaderModel = 0x65;
  Check(rr::UnsupportedReason(d3d, false, all_waves).empty(), "D3D12 with DXR 1.1 and shader model 6.5 can trace");
  d3d.shaderModel = 0x66;
  Check(rr::UnsupportedReason(d3d, false, all_waves).empty(), "D3D12 with a newer shader model can trace");
  d3d.shaderModel = 0x64;
  const std::string old_model = rr::UnsupportedReason(d3d, false, all_waves);
  Check(old_model.find("shader model 6.4") != std::string::npos && old_model.find("6.5") != std::string::npos, "D3D12 with shader model 6.4 says so: " + old_model);
  d3d.shaderModel = 0x65;
  d3d.raytracingStateUpdate = false;
  Check(rr::UnsupportedReason(d3d, false, all_waves).find("tier 1.1") != std::string::npos, "D3D12 with DXR 1.0 only cannot trace inline");
  d3d.raytracingStateUpdate = true;
  d3d.raytracing = false;
  Check(!rr::UnsupportedReason(d3d, false, all_waves).empty(), "D3D12 with no ray tracing cannot trace");
  // D3D12 does not read the Vulkan members: a Plume that fills rayQuery in (it does since 502978a9: DXR 1.1 and shader model 6.5) changes nothing here.
  FakeCaps d3d_with_member;
  d3d_with_member.raytracing = true;
  d3d_with_member.raytracingStateUpdate = true;
  d3d_with_member.shaderModel = 0x65;
  d3d_with_member.rayQuery = true;
  Check(rr::UnsupportedReason(d3d_with_member, false, all_waves).empty(), "D3D12 answers the same with the rayQuery member set");
  d3d_with_member.rayQuery = false;
  Check(rr::UnsupportedReason(d3d_with_member, false, all_waves).empty(), "D3D12 answers the same with the rayQuery member clear");

  // Vulkan: Plume's rayQuery (the ray tracing pipelines are not needed), the buffer addresses and the descriptor indexing.
  FakeCaps vk;
  vk.bufferDeviceAddress = true;
  vk.descriptorIndexing = true;
  vk.rayQuery = true;
  Check(rr::UnsupportedReason(vk, true, all_waves).empty(), "Vulkan with ray queries (no ray tracing pipelines) can trace");
  vk.raytracing = true;
  Check(rr::UnsupportedReason(vk, true, all_waves).empty(), "Vulkan with ray queries and the pipelines can trace");
  vk.rayQuery = false;
  const std::string no_query = rr::UnsupportedReason(vk, true, all_waves);
  Check(no_query.find("VK_KHR_ray_query") != std::string::npos, "Vulkan without ray queries says which extension it lacks: " + no_query);
  Check(no_query.find("Plume") == std::string::npos, "the Vulkan answer no longer says it waits for a Plume");
  vk.rayQuery = true;
  vk.bufferDeviceAddress = false;
  Check(!rr::UnsupportedReason(vk, true, all_waves).empty(), "Vulkan with ray queries but no buffer device addresses cannot trace");
  vk.bufferDeviceAddress = true;
  vk.descriptorIndexing = false;
  Check(!rr::UnsupportedReason(vk, true, all_waves).empty(), "Vulkan with ray queries but no descriptor indexing cannot trace");
  // Vulkan does not read the D3D12 members.
  FakeCaps vk_only;
  vk_only.rayQuery = true;
  vk_only.bufferDeviceAddress = true;
  vk_only.descriptorIndexing = true;
  vk_only.shaderModel = 0;
  Check(rr::UnsupportedReason(vk_only, true, all_waves).empty(), "Vulkan does not look at the shader model or the DXR tier");
  // The wave operations the shaders use (WaveProblem): every one is needed on both APIs, in compute shaders, and the answer names what is missing.
  for (const bool vulkan : {false, true}) {
    const FakeCaps caps = vulkan ? vk_only : d3d_with_member;  // both are clean: every other need is met
    const std::string api = vulkan ? "Vulkan" : "D3D12";
    FakeWaves no_ballot;
    no_ballot.ballot = false;
    const std::string ballot_text = rr::UnsupportedReason(caps, vulkan, no_ballot);
    Check(ballot_text.find("ballot") != std::string::npos && ballot_text.find("WaveActiveCountBits") != std::string::npos, api + " without ballot says so: " + ballot_text);
    FakeWaves no_arithmetic;
    no_arithmetic.arithmetic = false;
    const std::string arithmetic_text = rr::UnsupportedReason(caps, vulkan, no_arithmetic);
    Check(arithmetic_text.find("arithmetic") != std::string::npos && arithmetic_text.find("WaveActiveSum") != std::string::npos,
          api + " without arithmetic says so: " + arithmetic_text);
    FakeWaves no_basic;
    no_basic.basic = false;
    Check(rr::UnsupportedReason(caps, vulkan, no_basic).find("WaveIsFirstLane") != std::string::npos, api + " without basic wave operations cannot trace");
    FakeWaves not_compute;
    not_compute.in_compute = false;
    Check(rr::UnsupportedReason(caps, vulkan, not_compute).find("compute") != std::string::npos, api + " with wave operations in no compute stage cannot trace");
    FakeWaves neither;
    neither.ballot = false;
    neither.arithmetic = false;
    const std::string both_text = rr::UnsupportedReason(caps, vulkan, neither);
    Check(both_text.find("ballot") != std::string::npos && both_text.find("arithmetic") != std::string::npos, api + " lists everything missing: " + both_text);
    // A device the backend could not ask is not turned away, whatever the flags say.
    FakeWaves unknown;
    unknown.known = false;
    unknown.basic = unknown.ballot = unknown.arithmetic = unknown.in_compute = false;
    Check(rr::UnsupportedReason(caps, vulkan, unknown).empty(), api + " with an unknown wave answer can trace (a failed query never turns a device away)");
    Check(rr::UnsupportedReason(caps, vulkan, all_waves).empty(), api + " with every wave operation can trace");
  }
  // The API's own reasons come first: a device with neither ray queries nor waves says the ray queries.
  FakeCaps vk_none;
  vk_none.bufferDeviceAddress = true;
  vk_none.descriptorIndexing = true;
  FakeWaves no_waves;
  no_waves.basic = no_waves.ballot = no_waves.arithmetic = no_waves.in_compute = false;
  Check(rr::UnsupportedReason(vk_none, true, no_waves).find("ray queries") != std::string::npos, "Vulkan without ray queries and waves names the ray queries first");
  FakeCaps d3d_none;
  d3d_none.shaderModel = 0x65;
  Check(rr::UnsupportedReason(d3d_none, false, no_waves).find("tier 1.1") != std::string::npos, "D3D12 without DXR and waves names DXR first");
}

// The shader counts into the header's words with the header's edges and unit.
// The mesh a bottom level build is recorded for (BuildableMesh): found again by its key, so a mesh given back after it was placed gets no build. The scene's own
// sequence (car_rt_scene.cpp OnResolve) on a stand-in map: place a mesh, give it back (Evict: the structure retired, the entry erased), record the builds.
struct FakeMesh {
  std::unique_ptr<int> structure;
  uint32_t index_count = 0;
};

void TestBuildableMesh() {
  std::unordered_map<uint64_t, FakeMesh> meshes;
  struct Placed {
    uint64_t key = 0;
    FakeMesh* node = nullptr;  // what the scene kept from placing (the pointer the bug read)
  };
  std::vector<Placed> placed;
  for (uint64_t key = 100; key < 140; ++key) {   // a car's worth of parts placed in one frame
    FakeMesh m;
    m.structure = std::make_unique<int>(int(key));
    m.index_count = uint32_t(key) * 3;
    placed.push_back(Placed{key, &meshes.emplace(key, std::move(m)).first->second});
  }
  Check(placed.size() == 40 && meshes.size() == 40, "40 meshes placed");
  // every placed mesh is found, with its own structure
  size_t found = 0;
  for (const Placed& p : placed) {
    FakeMesh* m = rr::BuildableMesh(meshes, p.key);
    found += (m != nullptr && m->structure && *m->structure == int(p.key)) ? 1 : 0;
  }
  Check(found == 40, "BuildableMesh finds every placed mesh by its key");
  // the walk's set drops three of them in the same frame (GiveBack -> Evict: the structure goes to the ring, the entry is erased)
  std::vector<std::unique_ptr<int>> ring;   // the frame ring's retired objects
  const uint64_t given_back[] = {105, 117, 139};
  for (uint64_t key : given_back) {
    FakeMesh& m = meshes.at(key);
    if (m.structure) {
      ring.push_back(std::move(m.structure));
    }
    meshes.erase(key);
  }
  // what the builds are recorded for: never a given-back mesh
  std::vector<uint64_t> built;
  size_t null_destinations = 0;
  for (const Placed& p : placed) {
    FakeMesh* m = rr::BuildableMesh(meshes, p.key);
    if (m == nullptr) {
      continue;
    }
    if (!m->structure) {
      ++null_destinations;
    }
    built.push_back(p.key);
  }
  Check(built.size() == 37 && null_destinations == 0, "a mesh given back after it was placed gets no build (37 of 40 are built, none with a null structure)");
  bool none_given_back = true;
  for (uint64_t key : built) {
    none_given_back = none_given_back && key != 105 && key != 117 && key != 139;
  }
  Check(none_given_back, "and the built ones are the others");
  Check(ring.size() == 3, "the three structures were retired, not lost");
  // a mesh with no structure (retired, entry not erased) gets no build either
  meshes.at(100).structure.reset();
  Check(rr::BuildableMesh(meshes, 100) == nullptr, "an entry with no structure gets no build");
  // a key that was never there
  Check(rr::BuildableMesh(meshes, 7) == nullptr, "an unknown key gets no build");
  // given back and placed again later: the key finds the NEW entry, not the old node
  FakeMesh again;
  again.structure = std::make_unique<int>(4242);
  FakeMesh* fresh = &meshes.emplace(uint64_t(105), std::move(again)).first->second;
  FakeMesh* looked = rr::BuildableMesh(meshes, 105);
  Check(looked == fresh && looked->structure && *looked->structure == 4242, "a key placed again finds its new entry");
  // a const map works too
  const auto& view = meshes;
  const FakeMesh* c = rr::BuildableMesh(view, 105);
  Check(c == fresh, "a const map is looked up the same way");
}

void TestShaderParity(const std::string& folder) {
  std::ifstream in(folder + "/hlsl/car_rt_check_cs.hlsl", std::ios::binary);
  if (!in) {
    std::printf("note: %s/hlsl/car_rt_check_cs.hlsl not found; the shader parity check is skipped\n", folder.c_str());
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto has = [&](const std::string& s) { return text.find(s) != std::string::npos; };
  Check(has("kStatsWords = " + std::to_string(rr::kStatsWords)), "the shader's word count is the header's");
  Check(has("kStatVehicle = " + std::to_string(rr::kStatVehicle)) && has("kStatCovered = " + std::to_string(rr::kStatCovered)) &&
            has("kStatErrorSum = " + std::to_string(rr::kStatErrorSum)) && has("kStatNearer = " + std::to_string(rr::kStatNearer)) &&
            has("kStatFarther = " + std::to_string(rr::kStatFarther)) && has("kStatHistogram = " + std::to_string(rr::kStatHistogram)) &&
            has("kStatGlass = " + std::to_string(rr::kStatGlass)) && has("kStatTyres = " + std::to_string(rr::kStatTyres)) &&
            has("kStatBody = " + std::to_string(rr::kStatBody)),
        "the shader's word numbers are the header's");
  for (uint32_t b = 0; b + 1 < rr::kBuckets; ++b) {
    char edge[32];
    std::snprintf(edge, sizeof(edge), "%g", rr::kBucketEdges[b]);
    Check(has(edge), std::string("the shader has the bucket edge ") + edge);
  }
  char unit[32];
  std::snprintf(unit, sizeof(unit), "%g", rr::kErrorUnit);
  Check(has(std::string("kErrorUnit = ") + unit), "the shader's error unit is the header's");
  // The scope: the words, the margin, the box count, the flag and the result numbers are the header's, and the shader reads the other cars' boxes
  // from the table as the ray traced occlusion does (two rows a box, grown by the margin).
  Check(has("kStatOutside = " + std::to_string(rr::kStatOutside)) && has("kStatOthers = " + std::to_string(rr::kStatOthers)),
        "the shader's scope words are the header's");
  char margin[32];
  std::snprintf(margin, sizeof(margin), "%g", double(rr::kScopeBoxMargin));
  Check(has(std::string("kScopeBoxMargin = ") + margin), "the shader's scope margin is the header's");
  Check(has("kScopeMaxOtherBoxes = " + std::to_string(rr::kScopeMaxOtherBoxes)) && has("kScopeFlagPlayerBox = " + std::to_string(rr::kScopeFlagPlayerBox) + "u"),
        "the shader's box count and flag are the header's");
  Check(has("kScopeOutside = " + std::to_string(rr::kScopeOutside) + "u") && has("kScopePlayer = " + std::to_string(rr::kScopePlayer) + "u") &&
            has("kScopeOther = " + std::to_string(rr::kScopeOther) + "u"),
        "the shader's scope results are the header's");
  Check(has("asfloat(gCarRtInstances[row].xyz) - kScopeBoxMargin") && has("asfloat(gCarRtInstances[row + 1u].xyz) + kScopeBoxMargin") &&
            has("row = g_push.scope.x + b * 2u"),
        "the shader reads the other cars' boxes as the table has them");
  Check(has("uint4 scope;") && has("float4 box_min;") && has("float4 box_max;"), "the shader's push block has the scope");
  // the probe
  Check(has("kStatProbe = " + std::to_string(rr::kStatProbe)) && has("kProbeCars = " + std::to_string(rr::kProbeCars)) && has("kProbeWordsPerCar = " + std::to_string(rr::kProbeWordsPerCar)),
        "the shader's probe words are the header's");
  Check(has("(g_push.slots.w >> 16) & 0xFFu") && has("const uint row = g_push.scope.w + k * 2u;") && has("gCarRtInstances[row + 1u]") && has("gStats.Store4((kStatProbe + k * kProbeWordsPerCar) * 4u, centre)") &&
            has("gStats.Store4((kStatProbe + k * kProbeWordsPerCar + 4u) * 4u, paint)") && has("if (id.x == 0u && id.y == 0u)"),
        "the shader's probe reads the cars' records from scope.w, as many as bits 16 to 23 of slots.w say, and stores them raw from word kStatProbe");
  Check(has("mode == 3u") && has("CarRtLoadCar(hit_instance)") && has("CAR_RT_MASK_OTHERS"), "view 3 shows another car's paint row as the shader's own load reads it");
  Check(has("kScopeOutside : kScopePlayer"), "without the player's box the shader counts the pixel as the player's");
  Check(has("!outside") && has("Count(kStatOutside, WaveActiveCountBits(outside))"), "the pixels outside are counted apart and not traced");
  std::ifstream common(folder + "/hlsl/car_rt_common.hlsli", std::ios::binary);
  const std::string ctext((std::istreambuf_iterator<char>(common)), std::istreambuf_iterator<char>());
  char mask[64];
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_BODY 0x%02X", rr::kMaskBody);
  Check(ctext.find(mask) != std::string::npos, "the include's body mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_GLASS 0x%02X", rr::kMaskGlass);
  Check(ctext.find(mask) != std::string::npos, "the include's glass mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_TYRES 0x%02X", rr::kMaskTyres);
  Check(ctext.find(mask) != std::string::npos, "the include's tyre mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_OTHERS 0x%02X", rr::kMaskOthers);
  Check(ctext.find(mask) != std::string::npos, "the include's others mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_FLAG_AFTER_WORLD 0x%03X", rr::kFlagAfterWorld);
  Check(ctext.find(mask) != std::string::npos, "the include's after-world flag is the header's");
}

// What the build says about the ray query shaders (src/gpu/CMakeLists.txt): every HLSL source that traces with RayQuery is built through
// larecomp_gpu_host_shader_rt (its SPIR-V for Vulkan 1.2, SPIR-V 1.5: the ray query extension needs it) and no other function, the function passes
// -fspv-target-env=vulkan1.2 to the SPIR-V compile only, and the guards that held Vulkan back are gone from the sources.
std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The stem (first argument) and source (SOURCE argument, else the stem) of every `function(` call in the CMake text: one entry per call.
std::vector<std::pair<std::string, std::string>> HostShaderCalls(const std::string& cmake, const std::string& function) {
  std::vector<std::pair<std::string, std::string>> calls;
  const std::string head = "\n" + function + "(";
  size_t at = cmake.find(head);
  while (at != std::string::npos) {
    const size_t open = at + head.size();
    const size_t close = cmake.find(')', open);
    if (close == std::string::npos) {
      break;
    }
    std::string args = cmake.substr(open, close - open);
    for (char& c : args) {
      if (c == '\n' || c == '\r' || c == '\t') {
        c = ' ';
      }
    }
    std::vector<std::string> words;
    size_t from = 0;
    while (from < args.size()) {
      const size_t sp = args.find(' ', from);
      const size_t end = sp == std::string::npos ? args.size() : sp;
      if (end > from) {
        words.push_back(args.substr(from, end - from));
      }
      from = end + 1;
    }
    if (!words.empty()) {
      std::string source = words[0];
      for (size_t i = 0; i + 1 < words.size(); ++i) {
        if (words[i] == "SOURCE") {
          source = words[i + 1];
        }
      }
      calls.emplace_back(words[0], source);
    }
    at = cmake.find(head, close);
  }
  return calls;
}

void TestSpirvBuild(const std::string& folder) {
  const std::string cmake = ReadAll(folder + "/CMakeLists.txt");
  if (cmake.empty()) {
    std::printf("note: %s/CMakeLists.txt not found; the SPIR-V build check is skipped\n", folder.c_str());
    return;
  }
  const auto has = [&](const std::string& text, const std::string& s) { return text.find(s) != std::string::npos; };
  const size_t rt_fn = cmake.find("function(larecomp_gpu_host_shader_rt STEM PROFILE)");
  Check(rt_fn != std::string::npos, "CMake has larecomp_gpu_host_shader_rt");
  if (rt_fn != std::string::npos) {
    const std::string body = cmake.substr(rt_fn, cmake.find("endfunction()", rt_fn) - rt_fn);
    Check(has(body, "SPIRV_ARGS -fspv-target-env=vulkan1.2"), "the ray query function compiles the SPIR-V for Vulkan 1.2 (SPIR-V 1.5)");
    Check(has(body, "larecomp_gpu_host_shader(${STEM} ${PROFILE}"), "the ray query function is the plain host shader with the extra SPIR-V argument");
  }
  const size_t plain_fn = cmake.find("function(larecomp_gpu_host_shader STEM PROFILE)");
  Check(plain_fn != std::string::npos, "CMake has larecomp_gpu_host_shader");
  if (plain_fn != std::string::npos) {
    const std::string body = cmake.substr(plain_fn, cmake.find("endfunction()", plain_fn) - plain_fn);
    const size_t spirv_branch = body.find("set(ext spirv)");
    Check(spirv_branch != std::string::npos && body.find("-spirv -fvk-use-dx-layout ${ARG_SPIRV_ARGS}") > spirv_branch,
          "the extra arguments are for the SPIR-V blob only, not the DXIL one");
    Check(has(body, "\"DEFINES;SPIRV_ARGS\""), "the host shader function takes SPIRV_ARGS");
  }
  const auto rt_calls = HostShaderCalls(cmake, "larecomp_gpu_host_shader_rt");
  const auto plain_calls = HostShaderCalls(cmake, "larecomp_gpu_host_shader");
  Check(rt_calls.size() >= 4, "the ray query shaders are built through the ray query function (the check, the reflection, its opponents' variant, the occlusion)");
  for (const auto& call : rt_calls) {
    Check(has(cmake, "larecomp_gpu_host_shader_rt(" + call.first + " cs_6_5"), "the ray query shader " + call.first + " is a Shader Model 6.5 compute shader");
  }
  // every .hlsl that traces is built only through the ray query function
  const char* const kRaySources[] = {"car_rt_check_cs", "car_rt_refl_cs", "car_rt_ao_cs"};
  for (const char* source : kRaySources) {
    const std::string text = ReadAll(folder + "/hlsl/" + source + ".hlsl");
    if (text.empty()) {
      std::printf("note: %s/hlsl/%s.hlsl not found\n", folder.c_str(), source);
      continue;
    }
    bool through_rt = false;
    for (const auto& call : rt_calls) {
      through_rt = through_rt || call.second == source;
    }
    bool through_plain = false;
    for (const auto& call : plain_calls) {
      through_plain = through_plain || call.second == source;
    }
    Check(through_rt && !through_plain, std::string("hlsl/") + source + ".hlsl is built for Vulkan 1.2 and for no other target");
  }
  // the source of every shader that has a RayQuery (the includes count for the files that include them) is one of the three
  for (const char* file : {"car_rt_common.hlsli", "car_rt_refl_opp.hlsli"}) {
    Check(has(ReadAll(folder + "/hlsl/" + file), "RayQuery<"), std::string("hlsl/") + file + " is one of the files that traces");
  }
  // the guards that held Vulkan back
  const std::string scene = ReadAll(folder + "/car_rt_scene.cpp") + ReadAll(folder + "/car_rt_scene_rules.h") + ReadAll(folder + "/car_rt_scene.h");
  Check(!scene.empty() && !has(scene, "waits for Plume") && !has(scene, "kCapsHaveRayQuery") && !has(scene, "CapsRayQuery"),
        "no guard of the car ray tracing waits for a Plume with ray queries any more");
  Check(has(ReadAll(folder + "/car_rt_scene.cpp"), "rr::UnsupportedReason(caps, vulkan, waves)") && has(ReadAll(folder + "/car_rt_scene_rules.h"), "!caps.rayQuery"),
        "the Vulkan decision reads Plume's rayQuery capability and the wave operations");
  // the effects, the renderer's start and the tier's probe decide with the device's own wave operations
  for (const char* file : {"car_rt_refl.cpp", "car_rt_ao.cpp"}) {
    Check(has(ReadAll(folder + "/" + file), "CarRtSupported(device.capabilities(), ActiveBackend() == Backend::kVulkan, device.wave_ops())"),
          std::string(file) + " decides with the device's wave operations");
  }
  Check(has(ReadAll(folder + "/renderer.cpp"), "CarRtUnsupportedReason(device_.capabilities(), vulkan, device_.wave_ops())") &&
            has(ReadAll(folder + "/device.cpp"), "CarRtUnsupportedReason(caps, probe.vulkan, wave_ops())"),
        "the renderer's start and the ray tracing tier's probe decide with the device's wave operations");
  const std::string effects = ReadAll(folder + "/car_rt_refl.h") + ReadAll(folder + "/car_rt_ao.h");
  Check(!effects.empty() && !has(effects, "Direct3D 12 only for now"), "the effects' headers do not say Direct3D 12 only");
  // and none of the rules' headers (the hit shading's plan and material rules were written while Vulkan waited) says Vulkan waits for a Plume
  std::string rules_text;
  for (const char* file : {"car_rt_hit_rules.h", "car_rt_material_rules.h", "car_rt_refl.cpp", "car_rt_ao.cpp"}) {
    rules_text += ReadAll(folder + "/" + file);
  }
  Check(!rules_text.empty() && !has(rules_text, "until Plume") && !has(rules_text, "waits for Plume") && !has(rules_text, "D3D12 only until"),
        "the hit shading's headers do not say Vulkan waits for a Plume");
}

// The wave operations the ray tracing shaders call (hlsl/car_rt_*.hlsl and .hlsli, hlsl/rt_*.hlsl): each one is in the list WaveProblem asks the device for (basic:
// WaveIsFirstLane, ballot: WaveActiveCountBits, arithmetic: WaveActiveSum), so a shader that starts to use another (a prefix sum, a vote, a quad operation) fails here
// until the decision asks for it.
void TestWaveOpsInShaders(const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::path(folder) / "hlsl";
  if (!fs::is_directory(dir, ec)) {
    std::printf("note: %s not found; the wave operation scan is skipped\n", dir.string().c_str());
    return;
  }
  const std::set<std::string> allowed = {"WaveIsFirstLane", "WaveActiveCountBits", "WaveActiveSum"};
  std::set<std::string> found;
  int files = 0;
  const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    const std::string extension = entry.path().extension().string();
    if (!(name.rfind("car_rt_", 0) == 0 || name.rfind("rt_", 0) == 0) || (extension != ".hlsl" && extension != ".hlsli")) {
      continue;
    }
    ++files;
    const std::string text = ReadAll(entry.path().string());
    for (size_t at = text.find("Wave"); at != std::string::npos; at = text.find("Wave", at + 4)) {
      size_t end = at;
      while (end < text.size() && is_word(text[end])) {
        ++end;
      }
      if ((at == 0 || !is_word(text[at - 1])) && end < text.size() && text[end] == '(') {
        found.insert(text.substr(at, end - at));
      }
    }
    for (const char* quad : {"QuadRead", "QuadAny", "QuadAll"}) {
      Check(text.find(quad) == std::string::npos, name + " uses no quad operation (the decision does not ask for them)");
    }
  }
  Check(files >= 5, "the scan found the ray tracing shaders (" + std::to_string(files) + " files)");
  for (const std::string& op : found) {
    Check(allowed.count(op) != 0, "the ray tracing shaders call " + op + ", which the wave operation decision does not ask the device for");
  }
  for (const std::string& op : allowed) {
    Check(found.count(op) != 0, "the ray tracing shaders still call " + op + " (the decision asks for it)");
  }
}

}  // namespace

int main(int argc, char** argv) {
  TestTransform();
  TestViewSign();
  TestMasks();
  TestMesh();
  TestAllocator();
  TestGrowthCopyOrder();
  TestEviction();
  TestStats();
  TestScope();
  TestProbe();
  TestSupportDecision();
  TestBuildableMesh();
  if (argc > 1) {
    TestShaderParity(argv[1]);
    TestSpirvBuild(argv[1]);
    TestWaveOpsInShaders(argv[1]);
  }
  std::printf("car_rt_scene_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
