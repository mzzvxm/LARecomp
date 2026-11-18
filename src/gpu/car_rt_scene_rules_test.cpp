// Check of the car ray tracing scene's rules (car_rt_scene_rules.h): the instance transform and the camera's z direction, the masks and the
// table's rows, the clean-up of a mesh read from guest memory, the range allocator, the eviction policy and the build budget, and the check
// pass's statistics. Given the src/gpu folder it also checks that hlsl/car_rt_check_cs.hlsl counts with the header's numbers. A program of its
// own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_scene_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_scene_rules_test [<src/gpu dir>]

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
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
  Check(rr::kMaskBody == 1 && rr::kMaskGlass == 2 && rr::kMaskTyres == 4 && rr::kMaskDriver == 8, "the masks are the contract's");
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
  words[rr::kStatCovered] = 5000;  // more than the vehicle pixels: clamped, never a negative miss
  s = rr::DecodeCheckStats(words);
  Check(s.covered == 1000 && s.missed == 0, "a covered count past the vehicle pixels is clamped");
}

// The shader counts into the header's words with the header's edges and unit.
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
  std::ifstream common(folder + "/hlsl/car_rt_common.hlsli", std::ios::binary);
  const std::string ctext((std::istreambuf_iterator<char>(common)), std::istreambuf_iterator<char>());
  char mask[64];
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_BODY 0x%02X", rr::kMaskBody);
  Check(ctext.find(mask) != std::string::npos, "the include's body mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_GLASS 0x%02X", rr::kMaskGlass);
  Check(ctext.find(mask) != std::string::npos, "the include's glass mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_MASK_TYRES 0x%02X", rr::kMaskTyres);
  Check(ctext.find(mask) != std::string::npos, "the include's tyre mask is the header's");
  std::snprintf(mask, sizeof(mask), "CAR_RT_FLAG_AFTER_WORLD 0x%03X", rr::kFlagAfterWorld);
  Check(ctext.find(mask) != std::string::npos, "the include's after-world flag is the header's");
}

}  // namespace

int main(int argc, char** argv) {
  TestTransform();
  TestViewSign();
  TestMasks();
  TestMesh();
  TestAllocator();
  TestEviction();
  TestStats();
  if (argc > 1) {
    TestShaderParity(argv[1]);
  }
  std::printf("car_rt_scene_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
