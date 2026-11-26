// Check of the side mirrors' glass finder for cards without ray tracing (mirror_glass_rules.h, mirror_glass_need.h; perf/MIRROR-VIEW.md MV3): the gate of the draws worth reading, the measure of a mesh
// (the same numbers the ray traced finder gets), the cache by mesh_key (one read a mesh, the retry of an unreadable one, the sweep, the cap, the budget of a frame's reads), the pair chosen among
// synthetic cars (a car with mirrors, with none, with one, with its two glasses made from one mesh and a mirrored matrix, drawn three times as the pause view does, in any pose of the car and in
// either z convention), the cameras that hide the mirrors (ahead of them, away from them, too far to matter), the need rule and its hold, the published flag, and that the setting, the listener and
// the build files hold what the header says. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_mirror_glass_rules_test, then run src/gpu/larecomp_mirror_glass_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)
// The compile by hand: clang++ -std=c++23 -I <src> -I <src>/gpu mirror_glass_rules_test.cpp -o mirror_glass_rules_test.exe

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mirror_glass_rules.h"

namespace mg = mcla::gpu::mirror_glass;
namespace cm = mcla::gpu::car_mirror;
namespace cmat = mcla::gpu::car_rt_material;
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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// vectors, matrices, meshes
// ---------------------------------------------------------------------------

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Unit(V3 a) {
  const double l = std::sqrt(Dot(a, a));
  return l > 0 ? a * (1.0 / l) : a;
}

struct Mesh {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  std::vector<float> normals;
};

// A patch in the plane spanned by u and v (unit, at right angles), centred on the origin: `w` along u, `h` along v, nu x nv quads of two triangles. The middle is pushed along the normal u x v by
// `bulge` (a slightly convex glass). The winding makes the face normal u x v; vertex normals are the smooth ones of the winding (the mesh's own, as the car's declaration has them).
Mesh MakePatch(V3 u, V3 v, double w, double h, int nu, int nv, double bulge) {
  Mesh m;
  const V3 n = Unit(Cross(u, v));
  for (int j = 0; j <= nv; ++j) {
    for (int i = 0; i <= nu; ++i) {
      const double s = double(i) / nu * 2.0 - 1.0;
      const double t = double(j) / nv * 2.0 - 1.0;
      const V3 p = u * (s * w * 0.5) + v * (t * h * 0.5) + n * (bulge * (1.0 - s * s) * (1.0 - t * t));
      m.positions.push_back(float(p.x));
      m.positions.push_back(float(p.y));
      m.positions.push_back(float(p.z));
    }
  }
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      const uint32_t a = uint32_t(j * (nu + 1) + i), b = a + 1, c = a + uint32_t(nu + 1), d = c + 1;
      for (const uint32_t k : {a, b, d, a, d, c}) {
        m.indices.push_back(k);
      }
    }
  }
  rr::SmoothNormals(m.positions, m.indices, &m.normals);
  return m;
}

// A mirror glass facing `normal` (a world's horizontal direction mostly): the R34's 12 cm by 16.5 cm, 18 triangles, slightly convex.
Mesh MakeGlass(V3 normal) {
  const V3 up{0, 1, 0};
  const V3 u = Unit(Cross(up, normal));  // lateral-ish
  const V3 v = Cross(normal, u);         // up-ish
  return MakePatch(u, v, 0.165, 0.12, 3, 3, 0.003);
}

// A flat plate of `w` by `h` facing `normal`, nu x nv quads, no bulge (a handle, a badge, a lamp lens, a window).
Mesh MakePlate(V3 normal, double w, double h, int nu, int nv) {
  const V3 up{0, 1, 0};
  const V3 u = Unit(Cross(up, normal));
  const V3 v = Cross(normal, u);
  return MakePatch(u, v, w, h, nu, nv, 0.0);
}

// Row vector 4x4 `world = (p, 1) M` that turns the car's model space by `heading` about the world's up and puts the car's origin at `car`, after moving a part by `offset` in the car's frame.
tp::Mat4 Placement(double heading, V3 offset, V3 car, bool mirror_z = false) {
  tp::Mat4 m = tp::Identity();
  const double c = std::cos(heading), s = std::sin(heading);
  // column-vector rotation about y: x' = c x + s z, z' = -s x + c z  ->  row-vector matrix R[row][col] = Rcol[col][row]
  const double rcol[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      m.m[r][k] = rcol[k][r];
    }
  }
  if (mirror_z) {
    for (int k = 0; k < 3; ++k) {
      m.m[2][k] = -m.m[2][k];  // the part's z is negated first (the right mirror made from the left one's mesh), then turned
    }
  }
  const double off[3] = {offset.x, offset.y, offset.z};
  const double pos[3] = {car.x, car.y, car.z};
  for (int k = 0; k < 3; ++k) {
    double t = pos[k];
    for (int r = 0; r < 3; ++r) {
      t += rcol[k][r] * off[r];
    }
    m.m[3][k] = t;
  }
  return m;
}

V3 Apply(const tp::Mat4& m, V3 p) {
  return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
          p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
V3 ApplyDir(const tp::Mat4& m, V3 d) {
  return {d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0], d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1], d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]};
}

struct Cam {
  tp::Mat4 view;
  tp::Mat4 view_projection;
  V3 eye;
};

// A camera at `eye` looking along `forward` with the world's up; the game's own matrix looks down -z (z backward), the occlusion's has z forward. `tan_x`, `tan_y`: the half fields.
Cam MakeCam(V3 eye, V3 forward, double tan_x, double tan_y, bool z_forward = true) {
  const V3 f = Unit(forward);
  const V3 r = Unit(Cross(f, V3{0, 1, 0}));
  const V3 u = Cross(r, f);
  const V3 basis[3] = {r, u, z_forward ? f : f * -1.0};
  Cam cam;
  cam.eye = eye;
  cam.view = tp::Identity();
  for (int c = 0; c < 3; ++c) {
    cam.view.m[0][c] = basis[c].x;
    cam.view.m[1][c] = basis[c].y;
    cam.view.m[2][c] = basis[c].z;
    cam.view.m[3][c] = -Dot(eye, basis[c]);
  }
  tp::Mat4 proj;
  proj.m[0][0] = 1.0 / tan_x;
  proj.m[1][1] = 1.0 / tan_y;
  proj.m[2][2] = 1.0;
  proj.m[2][3] = 1.0;
  cam.view_projection = tp::Mul(cam.view, proj);
  return cam;
}

mg::Camera AsCamera(const Cam& cam, bool flip_z = false) {
  mg::Camera c;
  c.view = cam.view;
  c.view_projection = cam.view_projection;
  c.flip_z = flip_z;
  return c;
}

// ---------------------------------------------------------------------------
// synthetic cars: the shaders by their runtime identities (car_rt_material_rules.h), the geometry in the R34 dump's frame (the car faces -x, up is +y, z is across)
// ---------------------------------------------------------------------------

constexpr uint64_t kPaintPs = 0x439B193A3F675533ull;   // xCarPaintCustomizable: paint
constexpr uint64_t kGlassPs = 0x8F31933E41A20C72ull;   // xBumpSpecAlpha: the mirror glass's own, class other
constexpr uint64_t kLampPs = 0xCC710F7164C151DEull;    // xCarLight::PS_Light: a lamp lens
constexpr uint64_t kWindowPs = 0x4594B239C3B63D42ull;  // xCarGlass::PS_GlassFront
constexpr uint64_t kTyrePs = 0xA02CCC061B63E702ull;    // xWheelRubber
constexpr uint64_t kUnknownPs = 0x1234567812345678ull; // a shader the table does not know

struct Draw {
  bool rigid = true;
  uint64_t key = 0;
  uint32_t indices = 0;
  uint64_t ps = 0;
  tp::Mat4 model_to_world;
  const Mesh* mesh = nullptr;  // what the guest's memory holds for the key
};

struct Car {
  std::deque<Mesh> meshes;  // owned here: Draw::mesh points into it (a deque never moves what it holds)
  std::vector<Draw> draws;
  double heading = 0.0;
  V3 position;

  Car(double heading_in, V3 position_in) : heading(heading_in), position(position_in) {}

  const Mesh* Own(Mesh m) {
    meshes.push_back(std::move(m));
    return &meshes.back();
  }
  Draw& Add(uint64_t key, const Mesh* mesh, uint64_t ps, V3 offset, bool mirror_z = false) {
    Draw d;
    d.key = key;
    d.mesh = mesh;
    d.indices = uint32_t(mesh->indices.size());
    d.ps = ps;
    d.model_to_world = Placement(heading, offset, position, mirror_z);
    draws.push_back(d);
    return draws.back();
  }
};

// The R34-like car: a big body panel (paint), the two mirror glasses (xBumpSpecAlpha) at +-0.9 m across, 0.87 m up, 0.35 m ahead of the middle, facing back and in (the dump's normals), the two door
// handles (flat, small, facing out), a badge, the two tail lamp lenses (facing back), a window and a tyre.
struct R34Keys {
  uint64_t left_glass = 0x1001, right_glass = 0x1002;
};
void AddBody(Car* car) {
  car->Add(0x2001, car->Own(MakePatch({0, 0, 1}, {0, 1, 0}, 4.2, 1.2, 12, 12, 0.01)), kPaintPs, {0, 0.7, 0});  // 288 triangles
  car->Add(0x2002, car->Own(MakePlate({0, 0, 1}, 1.0, 0.45, 1, 1)), kWindowPs, {-0.1, 1.0, 0.8});            // a window: two triangles, too big a plate
  car->Add(0x2003, car->Own(MakePlate({0, 0, 1}, 0.3, 0.3, 8, 8)), kTyrePs, {-1.3, 0.3, 0.8});               // a tyre-class draw with many small triangles
}
void AddHandles(Car* car) {
  const Mesh* handle_l = car->Own(MakePlate({0, 0, 1}, 0.12, 0.05, 1, 1));   // faces out, +z
  const Mesh* handle_r = car->Own(MakePlate({0, 0, -1}, 0.12, 0.05, 1, 1));  // faces out, -z
  car->Add(0x3001, handle_l, kUnknownPs, {0.2, 0.60, 0.93});
  car->Add(0x3002, handle_r, kUnknownPs, {0.2, 0.60, -0.93});
  car->Add(0x3003, car->Own(MakePlate({-1, 0, 0}, 0.03, 0.03, 1, 1)), kUnknownPs, {-2.0, 0.6, 0.0});  // a badge: under the area's floor
}
void AddLamps(Car* car, uint64_t ps) {
  const Mesh* lamp = car->Own(MakePlate({1, 0, 0}, 0.12, 0.10, 2, 2));  // facing back (+x)
  car->Add(0x4001, lamp, ps, {2.0, 0.75, 0.65});
  car->Add(0x4002, lamp, ps, {2.0, 0.75, -0.65});
}
void AddGlasses(Car* car, const R34Keys& keys = R34Keys{}) {
  car->Add(keys.left_glass, car->Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  car->Add(keys.right_glass, car->Own(MakeGlass(Unit({0.93, 0.0, 0.37}))), kGlassPs, {-0.35, 0.87, -0.9});
}

Car MakeR34(double heading = 0.0, V3 position = {0, 0, 0}) {
  Car car(heading, position);
  AddBody(&car);
  AddHandles(&car);
  AddLamps(&car, kLampPs);
  AddGlasses(&car);
  return car;
}

// A camera fixed to the car (it turns and moves with it): `offset` in the car's frame, looking along `look` in the car's frame.
Cam CarCamera(const Car& car, V3 offset, V3 look, double tan_x, double tan_y, bool z_forward = true) {
  const tp::Mat4 m = Placement(car.heading, {0, 0, 0}, car.position);
  return MakeCam(Apply(m, offset), ApplyDir(m, look), tan_x, tan_y, z_forward);
}

// A cockpit camera: the driver's head, 0.35 m left of the middle, a little behind the glasses, looking forward; a wide field (112 degrees) as a cockpit camera has.
Cam Cockpit(const Car& car, bool z_forward = true) { return CarCamera(car, {0.2, 1.05, 0.35}, {-1, 0, 0}, 1.6, 0.9, z_forward); }

// ---------------------------------------------------------------------------
// the harness: frames of draws through the finder, as the listener and the scene do
// ---------------------------------------------------------------------------

struct Sim {
  mg::Finder finder;
  uint64_t frame = 0;
  uint32_t reads = 0;  // calls of the guest read
  std::map<uint64_t, uint32_t> reads_by_key;

  // One frame: the listener's calls for each draw, then the evaluation. `outcomes` gets the listener's answers.
  mg::Result Run(const Car& car, const mg::Camera& camera, std::vector<mg::Finder::Reply>* outcomes = nullptr, uint32_t* candidates = nullptr) {
    ++frame;
    finder.BeginFrame(frame);
    for (const Draw& d : car.draws) {
      const mg::Finder::Reply reply = finder.OnDraw(d.rigid, d.key, d.indices, d.ps, [&](std::vector<float>* p, std::vector<uint32_t>* i, std::vector<float>* n) {
        ++reads;
        ++reads_by_key[d.key];
        *p = d.mesh->positions;
        *i = d.mesh->indices;
        *n = d.mesh->normals;
        return true;
      });
      if (outcomes) {
        outcomes->push_back(reply);
      }
    }
    return Evaluate(car, camera, candidates);
  }

  mg::Result Evaluate(const Car& car, const mg::Camera& camera, uint32_t* candidates = nullptr) {
    finder.BeginEvaluation();
    uint32_t considered = 0;
    for (uint32_t i = 0; i < car.draws.size(); ++i) {
      const Draw& d = car.draws[i];
      considered += finder.Consider(i, d.rigid, d.key, d.indices, d.ps, d.model_to_world) ? 1u : 0u;
    }
    if (candidates) {
      *candidates = considered;
    }
    return finder.Choose(frame, camera);
  }
};

// Runs a car until every mesh is read (the first frames of a new car), then once more: the steady frame's answer.
mg::Result Settle(Sim* sim, const Car& car, const mg::Camera& camera) {
  mg::Result r;
  for (int i = 0; i < 6; ++i) {
    r = sim->Run(car, camera);
  }
  return r;
}

uint32_t IndexOfKey(const Car& car, uint64_t key) {
  for (uint32_t i = 0; i < car.draws.size(); ++i) {
    if (car.draws[i].key == key) {
      return i;
    }
  }
  return 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

void TestGate() {
  using S = mg::Skip;
  Check(mg::kMaxIndices == 480 && mg::kMaxIndices == 3 * cm::kMaxPatchTriangles, "the gate's index limit is three a triangle of the patch rule's limit (480)");
  Check(mg::kMinIndices == 6, "two triangles at least");
  Check(mg::WhySkipped(true, 7, 54, cm::kClassOther) == S::kNone, "a small rigid draw of a glass class is looked at");
  Check(mg::WhySkipped(false, 7, 54, cm::kClassOther) == S::kNotBody, "the driver (skinned) is never looked at");
  Check(mg::WhySkipped(true, 0, 54, cm::kClassOther) == S::kNoKey, "no mesh key: left out");
  Check(mg::WhySkipped(true, 7, 5, cm::kClassOther) == S::kFewIndices, "under two triangles");
  Check(mg::WhySkipped(true, 7, 6, cm::kClassOther) == S::kNone, "two triangles are enough");
  Check(mg::WhySkipped(true, 7, 480, cm::kClassOther) == S::kNone, "480 indices (160 triangles) are the limit and pass");
  Check(mg::WhySkipped(true, 7, 481, cm::kClassOther) == S::kManyIndices, "481 indices are over");
  Check(mg::WhySkipped(true, 7, 90000, cm::kClassOther) == S::kManyIndices, "a body panel is over");
  for (const uint8_t cls : {cm::kClassPaint, cm::kClassTyre, cm::kClassLight, cm::kClassInterior}) {
    Check(mg::WhySkipped(true, 7, 54, cls) == S::kClass, "class " + std::to_string(cls) + " is never a glass");
  }
  for (const uint8_t cls : {cm::kClassNone, cm::kClassGlass, cm::kClassChrome, cm::kClassOther}) {
    Check(mg::WhySkipped(true, 7, 54, cls) == S::kNone, "class " + std::to_string(cls) + " may be a glass");
  }
  // one rule: the class gate is the patch rule's own, for every value a byte can have
  bool same = true;
  for (uint32_t cls = 0; cls < 256; ++cls) {
    cm::PatchStats good;
    good.valid = true;
    good.triangles = 18;
    good.area = 0.02f;
    good.extent = 0.2f;
    good.flat = 0.99f;
    same = same && (mg::ClassMayBeGlass(uint8_t(cls)) == cm::IsPatchCandidate(good, uint8_t(cls)));
  }
  Check(same, "ClassMayBeGlass agrees with car_mirror::IsPatchCandidate for every class value");
  // the class of a shader is the ray traced scene's table, the same enumerators as car_mirror's
  Check(uint8_t(cmat::Class::kNone) == cm::kClassNone && uint8_t(cmat::Class::kPaint) == cm::kClassPaint && uint8_t(cmat::Class::kGlass) == cm::kClassGlass &&
            uint8_t(cmat::Class::kTyre) == cm::kClassTyre && uint8_t(cmat::Class::kChrome) == cm::kClassChrome && uint8_t(cmat::Class::kLight) == cm::kClassLight &&
            uint8_t(cmat::Class::kInterior) == cm::kClassInterior && uint8_t(cmat::Class::kOther) == cm::kClassOther,
        "car_mirror's class numbers are the material table's");
  Check(mg::ClassOfShader(kPaintPs) == cm::kClassPaint, "xCarPaintCustomizable is paint");
  Check(mg::ClassOfShader(kGlassPs) == cm::kClassOther, "xBumpSpecAlpha (the mirror glass's shader) is class other");
  Check(mg::ClassOfShader(kLampPs) == cm::kClassLight, "the lamp lens is a light");
  Check(mg::ClassOfShader(kWindowPs) == cm::kClassGlass, "the window is glass");
  Check(mg::ClassOfShader(kTyrePs) == cm::kClassTyre, "the tyre is a tyre");
  Check(mg::ClassOfShader(kUnknownPs) == cm::kClassNone && mg::ClassOfShader(0) == cm::kClassNone, "a shader the table does not know, and the identity 0, are class none");
  bool all_same = true;
  for (const cmat::ShaderRule& rule : cmat::kShaders) {
    const cmat::Material material = cmat::Capture(rule.ps, nullptr, [](uint32_t) { return cmat::FetchInfo{}; });
    all_same = all_same && uint8_t(material.cls) == mg::ClassOfShader(rule.ps);
  }
  Check(all_same, "the class of every shader of the table is what the draw's Material would carry (so both finders take the same draws)");
  Check(mg::SkipName(S::kClass) != nullptr && std::string(mg::SkipName(S::kCount)) == "?", "skip names");
}

// ---------------------------------------------------------------------------
// the measure
// ---------------------------------------------------------------------------

void TestMeasure() {
  Mesh glass = MakeGlass(Unit({0.91, 0.0, -0.41}));
  std::vector<float> p = glass.positions, n = glass.normals;
  std::vector<uint32_t> i = glass.indices;
  const mg::Measured m = mg::MeasureMesh(&p, &i, &n);
  Check(m.ok && m.stats.valid && m.triangles == 18, "a glass patch is measured: 18 triangles");
  Check(m.stats.area > 0.018f && m.stats.area < 0.022f, "its area is about 12 by 16.5 cm");
  Check(m.stats.flat >= cm::kMinFlat, "a slightly convex glass is flat enough");
  const V3 normal = Unit({0.91, 0.0, -0.41});
  Check(Near(m.stats.normal[0], normal.x, 0.02) && Near(m.stats.normal[1], normal.y, 0.02) && Near(m.stats.normal[2], normal.z, 0.02), "its mean normal is the glass's face");
  Check(m.has_box && m.box_max[0] - m.box_min[0] > 0.01f, "its box is made");
  Check(cm::IsPatchCandidate(m.stats, cm::kClassOther), "it is a candidate of the glass's class");

  // the numbers are the ray traced finder's: the same clean-up and the same MeasurePatch
  std::vector<float> p2 = glass.positions, n2 = glass.normals;
  std::vector<uint32_t> i2 = glass.indices;
  const cm::PatchStats direct = cm::MeasurePatch(p2, i2, n2);
  Check(direct.valid && Near(direct.area, m.stats.area) && Near(direct.flat, m.stats.flat) && Near(direct.normal[0], m.stats.normal[0]) && Near(direct.centroid[1], m.stats.centroid[1]),
        "MeasureMesh gives MeasurePatch's numbers for a clean mesh");

  // the ray traced scene's clean-up keeps every vertex, so a vertex that is not a number still makes the patch invalid (as it does there): the mesh is read, and found no glass
  std::vector<float> pn = glass.positions, nn = glass.normals;
  std::vector<uint32_t> in = glass.indices;
  pn.insert(pn.end(), {std::nanf(""), 0.0f, 0.0f});
  nn.insert(nn.end(), {0.0f, 0.0f, 1.0f});
  const mg::Measured nan_vertex = mg::MeasureMesh(&pn, &in, &nn);
  Check(nan_vertex.ok && !nan_vertex.stats.valid, "a mesh with a vertex that is not a number is read and is no patch (as in the ray traced finder)");

  // a triangle that names a vertex that is not there is dropped, not fatal
  std::vector<float> pb = glass.positions, nb = glass.normals;
  std::vector<uint32_t> ib = glass.indices;
  ib.insert(ib.end(), {0, 1, 9999});
  const mg::Measured bad_tri = mg::MeasureMesh(&pb, &ib, &nb);
  Check(bad_tri.ok && bad_tri.triangles == 18 && bad_tri.stats.valid, "a triangle that names no vertex is dropped");

  // no normals in the declaration: smooth ones from the winding, the same patch
  std::vector<float> pw = glass.positions, nw;
  std::vector<uint32_t> iw = glass.indices;
  const mg::Measured no_normals = mg::MeasureMesh(&pw, &iw, &nw);
  Check(no_normals.ok && no_normals.stats.valid && Near(no_normals.stats.normal[0], m.stats.normal[0], 1e-3), "a mesh with no normals is measured by its winding");

  // a big mesh is measured (read once) and found no patch; an empty one is not ok
  Mesh panel = MakePatch({0, 0, 1}, {0, 1, 0}, 4.2, 1.2, 12, 12, 0.01);
  const mg::Measured big = mg::MeasureMesh(&panel.positions, &panel.indices, &panel.normals);
  Check(big.ok && !big.stats.valid && big.triangles == 288, "a 288 triangle panel is no patch");
  std::vector<float> pe;
  std::vector<uint32_t> ie;
  std::vector<float> ne;
  Check(!mg::MeasureMesh(&pe, &ie, &ne).ok, "an empty mesh is not ok");
  Mesh window = MakePlate({0, 0, 1}, 0.5, 0.3, 1, 1);
  const mg::Measured win = mg::MeasureMesh(&window.positions, &window.indices, &window.normals);
  Check(win.ok && !win.stats.valid, "a window is too wide to be a patch");
  Mesh badge = MakePlate({0, 0, 1}, 0.03, 0.03, 1, 1);
  const mg::Measured bd = mg::MeasureMesh(&badge.positions, &badge.indices, &badge.normals);
  Check(bd.ok && bd.stats.valid && !cm::IsPatchCandidate(bd.stats, cm::kClassOther), "a badge is a patch with too little area: no candidate");
  Mesh handle = MakePlate({0, 0, 1}, 0.12, 0.05, 1, 1);
  const mg::Measured hd = mg::MeasureMesh(&handle.positions, &handle.indices, &handle.normals);
  Check(hd.ok && cm::IsPatchCandidate(hd.stats, cm::kClassOther) && !cm::IsPatchCandidate(hd.stats, cm::kClassPaint), "a door handle is a candidate unless it is paint");
}

// ---------------------------------------------------------------------------
// the cache
// ---------------------------------------------------------------------------

mg::Measured MeasuredPatch() {
  Mesh glass = MakeGlass(Unit({0.91, 0.0, -0.41}));
  return mg::MeasureMesh(&glass.positions, &glass.indices, &glass.normals);
}

void TestCache() {
  using Need = mg::GlassCache::Need;
  mg::GlassCache cache;
  Check(cache.Probe(11, 100) == Need::kRead && cache.size() == 0, "an unknown key is read");
  cache.NoteMeasured(11, MeasuredPatch(), 100);
  Check(cache.size() == 1 && cache.Probe(11, 101) == Need::kHave, "a measured key is known");
  const mg::GlassCache::Entry* e = cache.Find(11);
  Check(e && e->state == mg::GlassCache::State::kMeasured && e->stats.valid && e->has_box && e->seen == 101 && e->made == 100, "the entry holds the stats and box, and the frame it was last named");
  Check(cache.Find(12) == nullptr, "another key is not there");
  // a mesh that is no patch is remembered all the same
  mg::Measured nopatch;
  nopatch.ok = true;
  cache.NoteMeasured(13, nopatch, 100);
  Check(cache.Probe(13, 102) == Need::kHave && !cache.Find(13)->stats.valid, "a mesh that is no patch is not read again");
  // an unreadable mesh waits, then is tried again
  cache.NoteFailed(14, 200);
  Check(cache.Probe(14, 201) == Need::kWait, "an unreadable mesh waits");
  Check(cache.Probe(14, 200 + mg::kRetryFailedFrames - 1) == Need::kWait, "... until the retry frame");
  Check(cache.Probe(14, 200 + mg::kRetryFailedFrames) == Need::kRead, "... then it is read again");
  cache.NoteMeasured(14, MeasuredPatch(), 200 + mg::kRetryFailedFrames);
  Check(cache.Probe(14, 200 + mg::kRetryFailedFrames + 1) == Need::kHave && cache.Find(14)->stats.valid, "a retry that works replaces the failure");
  // the sweep: a key no draw names for kKeepFrames goes, one that is named stays
  mg::GlassCache sweep;
  sweep.NoteMeasured(1, MeasuredPatch(), 0);
  sweep.NoteMeasured(2, MeasuredPatch(), 0);
  sweep.Probe(2, mg::kKeepFrames);  // named at frame 1800
  Check(sweep.Sweep(mg::kKeepFrames + 1) == 1 && sweep.Find(1) == nullptr && sweep.Find(2) != nullptr, "the sweep drops the key nobody named and keeps the one a draw named");
  Check(sweep.Sweep(mg::kKeepFrames) == 0, "a sweep with nothing old drops nothing");
  sweep.Probe(2, 2 * mg::kKeepFrames);
  Check(sweep.Sweep(2 * mg::kKeepFrames + mg::kKeepFrames) == 0 && sweep.size() == 1, "a key that is named again lives on");
  // the cap
  mg::GlassCache full;
  for (uint64_t k = 1; k <= mg::kMaxEntries + 10; ++k) {
    full.NoteMeasured(k, MeasuredPatch(), k);
  }
  Check(full.size() == mg::kMaxEntries, "the cache holds at most kMaxEntries");
  Check(full.Find(mg::kMaxEntries + 10) != nullptr && full.Find(1) == nullptr, "a full cache gives up the stalest key first");
  full.Clear();
  Check(full.size() == 0, "Clear");
}

void TestFinderReads() {
  // one read a mesh key, however many draws and frames name it
  Car car = MakeR34();
  Sim sim;
  const Cam cockpit = Cockpit(car);
  std::vector<mg::Finder::Reply> outcomes;
  sim.Run(car, AsCamera(cockpit), &outcomes);
  // the small candidates of the R34-like car: 2 glasses, 2 handles, a badge, 2 lamps (a lamp is class light: skipped before it is read; the one lamp mesh is drawn twice), the window (2 triangles)
  uint32_t measured = 0, skipped_class = 0, skipped_many = 0;
  for (const mg::Finder::Reply& r : outcomes) {
    measured += r.outcome == mg::Finder::Outcome::kMeasured;
    skipped_class += r.skip == mg::Skip::kClass;
    skipped_many += r.skip == mg::Skip::kManyIndices;
  }
  Check(skipped_many == 1, "the 288 triangle body panel is over the index limit and never read");
  Check(skipped_class == 3, "the tyre-class draw and the two lamp draws (class light) are never read");
  Check(measured == sim.reads && measured == 2 + 2 + 1 + 1, "the first frame reads the two glasses, the two handles, the badge and the window once each");
  Check(sim.reads_by_key[0x2001] == 0 && sim.reads_by_key[0x4001] == 0 && sim.reads_by_key[0x2003] == 0, "the panel, the lamp and the tyre meshes were never read");
  const uint32_t reads_first = sim.reads;
  for (int f = 0; f < 10; ++f) {
    sim.Run(car, AsCamera(cockpit));
  }
  Check(sim.reads == reads_first, "ten more frames of the same car read nothing: the cache answers by mesh key");
  // the same mesh under another key is another read; the same key under another matrix is none
  Car two(0.0, {0, 0, 0});
  const Mesh* shared = two.Own(MakeGlass(Unit({0.91, 0.0, -0.41})));
  two.Add(0x5001, shared, kGlassPs, {-0.35, 0.87, 0.9});
  two.Add(0x5001, shared, kGlassPs, {-0.35, 0.87, -0.9}, true);
  Sim s2;
  s2.Run(two, AsCamera(Cockpit(two)));
  Check(s2.reads == 1 && s2.reads_by_key[0x5001] == 1, "two draws of one key (the right glass made from the left one's mesh) read the mesh once");
  Car moved = two;
  moved.draws[0].key = 0x5002;  // streamed in again over the same address with other bytes: another key
  s2.Run(moved, AsCamera(Cockpit(moved)));
  Check(s2.reads == 2 && s2.reads_by_key[0x5002] == 1 && s2.reads_by_key[0x5001] == 1, "a new key is read once (and the old key's other draw is still known)");
  // an unreadable mesh: tried once, then it waits
  Sim bad;
  Car one(0.0, {0, 0, 0});
  one.Add(0x6001, one.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {0, 0.87, 0.9});
  uint32_t calls = 0;
  bad.finder.BeginFrame(1);
  const auto fail = [&](std::vector<float>*, std::vector<uint32_t>*, std::vector<float>*) {
    ++calls;
    return false;
  };
  const auto r1 = bad.finder.OnDraw(true, 0x6001, 54, kGlassPs, fail);
  bad.finder.BeginFrame(2);
  const auto r2 = bad.finder.OnDraw(true, 0x6001, 54, kGlassPs, fail);
  Check(r1.outcome == mg::Finder::Outcome::kUnreadable && r2.outcome == mg::Finder::Outcome::kWaiting && calls == 1, "an unreadable mesh is read once and then waits");
  bad.finder.BeginFrame(2 + mg::kRetryFailedFrames);
  const auto r3 = bad.finder.OnDraw(true, 0x6001, 54, kGlassPs, fail);
  Check(r3.outcome == mg::Finder::Outcome::kUnreadable && calls == 2, "... and is tried again after the retry frames");
  // a read that gives an empty mesh is a failure too
  Sim empty;
  empty.finder.BeginFrame(1);
  const auto r4 = empty.finder.OnDraw(true, 0x6002, 54, kGlassPs, [](std::vector<float>*, std::vector<uint32_t>*, std::vector<float>*) { return true; });
  Check(r4.outcome == mg::Finder::Outcome::kUnreadable, "a read that returns no triangles is unreadable");
}

void TestBudget() {
  // a new car: a hundred small meshes the first frame; the frame's reads are limited and the rest come in the next frames
  Car car(0.0, {0, 0, 0});
  for (uint64_t k = 0; k < 100; ++k) {
    car.Add(0x7000 + k, car.Own(MakePlate({0, 0, 1}, 0.12, 0.05, 1, 1)), kUnknownPs, {double(k) * 0.01, 0.6, 0.9});
  }
  Sim sim;
  const Cam cam = Cockpit(car);
  std::vector<uint32_t> per_frame;
  std::vector<mg::Finder::Reply> outcomes;
  uint32_t deferred_first = 0;
  for (int f = 0; f < 5; ++f) {
    outcomes.clear();
    const uint32_t before = sim.reads;
    sim.Run(car, AsCamera(cam), &outcomes);
    per_frame.push_back(sim.reads - before);
    if (f == 0) {
      for (const auto& r : outcomes) {
        deferred_first += r.outcome == mg::Finder::Outcome::kDeferred;
      }
    }
  }
  Check(per_frame[0] == mg::kReadsPerFrame && per_frame[1] == mg::kReadsPerFrame && per_frame[2] == mg::kReadsPerFrame, "a frame reads kReadsPerFrame meshes and no more");
  Check(per_frame[3] == 100 - 3 * mg::kReadsPerFrame && per_frame[4] == 0, "the rest is read in the next frame and then nothing is left");
  Check(deferred_first == 100 - mg::kReadsPerFrame, "the draws past the budget are told 'deferred'");
  Check(sim.finder.cache().size() == 100, "every mesh is in the cache after the fourth frame");
}

// ---------------------------------------------------------------------------
// the pair: car shapes
// ---------------------------------------------------------------------------

void TestR34Cockpit() {
  Car car = MakeR34();
  Sim sim;
  const Cam cockpit = Cockpit(car);
  uint32_t candidates = 0;
  const mg::Result r = Settle(&sim, car, AsCamera(cockpit));
  sim.Run(car, AsCamera(cockpit), nullptr, &candidates);
  Check(candidates == 4, "the candidates of the R34-like car: the two glasses and the two door handles (the badge's area is too small, the window too wide, the lamps and the tyre are the wrong class)");
  Check(r.found && r.glass[0].mesh_key != r.glass[1].mesh_key, "a pair is found: two draws of two meshes");
  const uint32_t left = IndexOfKey(car, 0x1001), right = IndexOfKey(car, 0x1002);
  Check(r.glass[0].draw == left && r.glass[1].draw == right, "the glass on the left of the picture (the car's +z side: the camera looks along -x, its right is -z) is first");
  Check(r.glass[0].mesh_key == 0x1001 && r.glass[1].mesh_key == 0x1002, "the keys follow");
  Check(r.glass[0].centre_view[0] < r.glass[1].centre_view[0], "ordered by the view's x");
  Check(r.why == mg::Why::kNeeded && r.need.found && r.need.need(), "the cockpit camera needs the view");
  Check(r.glass[0].needed && !r.glass[1].needed && r.need.mask == 1, "the left glass is on the picture and wide; the right one is beyond the picture's edge (112 degrees across)");
  Check(r.glass[0].px_wide >= 100 && r.glass[0].px_wide <= 250, "the left glass is about 160 px wide at 1080 lines (" + std::to_string(r.glass[0].px_wide) + ")");
  Check(r.glass[1].rect.Empty() && r.glass[1].px_wide == 0, "the right glass has no rectangle");
  Check(r.need.widest == r.glass[0].px_wide, "the widest needed glass is the published width");
  Check(r.lens_known && Near(r.tan_x, 1.6, 1e-3) && Near(r.tan_y, 0.9, 1e-3), "the lens is read back from the two matrices");
  Check(r.eye_known && Near(r.eye_world[0], cockpit.eye.x, 1e-6) && Near(r.eye_world[1], cockpit.eye.y, 1e-6) && Near(r.eye_world[2], cockpit.eye.z, 1e-6), "the eye is the camera's place in the world");
  Check(Near(r.up_view[1], 1.0, 1e-4) && std::fabs(r.up_view[0]) < 1e-4, "the world's up in the view of a level camera is up");
  Check(r.candidates == 4 && r.score > 1.0f, "four candidates, and the pair's score is the area ratio and the likeness of the faces");
  // the glass's pose in the view: the left glass is 0.55 m ahead of the eye and 0.55 m to its left, 0.18 m below it
  Check(Near(r.glass[0].centre_view[0], -0.55, 0.03) && Near(r.glass[0].centre_view[1], -0.18, 0.03) && Near(r.glass[0].centre_view[2], 0.55, 0.03), "the left glass's centre in the camera's view space");
  // the pose in the world: the glass's centre at the car's origin plus the offset, the normal as the patch faces
  const V3 centre_world = Apply(car.draws[left].model_to_world, {0, 0, 0});
  Check(Near(r.glass[0].centre_world[0], centre_world.x, 2e-3) && Near(r.glass[0].centre_world[1], centre_world.y, 2e-3) && Near(r.glass[0].centre_world[2], centre_world.z, 2e-3), "the world centre");
  Check(Near(r.glass[0].normal_world[0], 0.91 / std::sqrt(0.91 * 0.91 + 0.41 * 0.41), 0.02) && Near(r.glass[0].normal_world[2], -0.41 / std::sqrt(0.91 * 0.91 + 0.41 * 0.41), 0.02),
        "the world normal faces back and inward");
  Check(r.glass[0].box.valid && r.glass[0].box.max[2] > r.glass[0].box.min[2] && r.glass[0].box.min[2] > 0.3f, "the glass's box is in the view space, ahead of the eye");
  Check(r.glass[0].area > 0.018f && r.glass[0].area < 0.022f, "the area is the patch's");
}

void TestHiddenByCamera() {
  Car car = MakeR34();
  Sim sim;
  // a chase camera 8 m behind and 1.5 m up: both glasses on the picture, a few dozen pixels wide at most
  const Cam chase = CarCamera(car, {8, 1.5, 0}, {-1, -0.1, 0}, 0.889, 0.5);
  mg::Result r = Settle(&sim, car, AsCamera(chase));
  Check(r.found && r.why == mg::Why::kTooSmall && !r.need.need() && r.need.found, "a chase camera: the pair is found, both glasses are too small for a view");
  Check(!r.glass[0].rect.Empty() && !r.glass[1].rect.Empty() && r.glass[0].px_wide > 0 && r.glass[0].px_wide < mg::kMinNeedWidth && r.glass[1].px_wide < mg::kMinNeedWidth,
        "... they are on the picture and narrower than " + std::to_string(mg::kMinNeedWidth) + " px (" + std::to_string(r.glass[0].px_wide) + ", " + std::to_string(r.glass[1].px_wide) + ")");
  Check(r.glass[0].mesh_key == 0x1001 && r.glass[0].centre_view[0] < r.glass[1].centre_view[0], "the camera behind the car also looks along -x: the car's +z glass is on the left of the picture");
  // a closer camera: 2 m behind, the glasses are wide enough
  const Cam near_chase = CarCamera(car, {2.2, 1.2, 0}, {-1, -0.05, 0}, 0.889, 0.5);
  r = Settle(&sim, car, AsCamera(near_chase));
  Check(r.found && r.why == mg::Why::kNeeded && r.need.mask == 3, "a camera 2.5 m from the glasses needs both");
  // a camera ahead of the mirrors looking forward (a bumper or hood camera): both boxes are behind the camera's plane
  const Cam bumper = CarCamera(car, {-2.5, 0.8, 0}, {-1, 0, 0}, 1.2, 0.675);
  r = Settle(&sim, car, AsCamera(bumper));
  Check(r.found && r.why == mg::Why::kOffPicture && !r.need.need() && r.need.found, "a camera ahead of the mirrors: found, off the picture, no need");
  Check(r.glass[0].rect.Empty() && r.glass[1].rect.Empty() && !r.glass[0].needed && !r.glass[1].needed, "... no rectangle for either");
  // a camera looking away to the side: the glasses are off the picture's edge
  const Cam side = CarCamera(car, {2.2, 1.2, 0}, {0, 0, 1}, 0.889, 0.5);
  r = Settle(&sim, car, AsCamera(side));
  Check(r.found && !r.need.need() && (r.why == mg::Why::kOffPicture || r.why == mg::Why::kTooSmall), "a camera turned away: no need");
  // a camera that does not draw the car at all: the frame has no draws
  Car none(0.0, {0, 0, 0});
  r = sim.Run(none, AsCamera(bumper));
  Check(!r.found && r.why == mg::Why::kFewCandidates && !r.need.found && r.candidates == 0, "a frame with no draws of the car: no candidates");
  // the mirror draws missing from the frame (the car drawn, the glass culled)
  Car culled = MakeR34();
  culled.draws.erase(culled.draws.begin() + IndexOfKey(culled, 0x1001));
  culled.draws.erase(culled.draws.begin() + IndexOfKey(culled, 0x1002));
  r = Settle(&sim, culled, AsCamera(Cockpit(culled)));
  Check(!r.found && r.why == mg::Why::kNoPair && r.candidates == 2, "the glass draws missing: only the handles are left, no pair");
  // the glass back: the pair is found at once (the cache still has the keys)
  const uint32_t reads_before = sim.reads;
  r = sim.Run(car, AsCamera(Cockpit(car)));
  Check(r.found && r.why == mg::Why::kNeeded && sim.reads == reads_before, "the glass draws back: found again in the same frame, with no read");
}

void TestNoMirrors() {
  // no small flat patch at all: the body, the tyre and the window
  Car bare(0.0, {0, 0, 0});
  AddBody(&bare);
  Sim sim;
  mg::Result r = Settle(&sim, bare, AsCamera(Cockpit(bare)));
  Check(!r.found && r.why == mg::Why::kFewCandidates && r.candidates == 0 && !r.need.found && !r.need.need(), "a car with no small flat patch: no candidates");
  // handles, a badge and lamp lenses of class light: two candidates (the handles), and no pair: they face out along the line from one to the other
  Car no_mirrors(0.0, {0, 0, 0});
  AddBody(&no_mirrors);
  AddHandles(&no_mirrors);
  AddLamps(&no_mirrors, kLampPs);
  Sim s2;
  r = Settle(&s2, no_mirrors, AsCamera(Cockpit(no_mirrors)));
  Check(!r.found && r.why == mg::Why::kNoPair && r.candidates == 2, "handles and lamps: the handles are candidates, the lamps (class light) are not, and no two are a pair");
  // the lamp lenses WITHOUT a class (a shader the table does not know): a pair across the car at one height facing back: taken, as MIRROR-RT's finder takes it (its own CHECK)
  Car unknown_lamps(0.0, {0, 0, 0});
  AddBody(&unknown_lamps);
  AddLamps(&unknown_lamps, kUnknownPs);
  Sim s3;
  r = Settle(&s3, unknown_lamps, AsCamera(Cockpit(unknown_lamps)));
  Check(r.found && r.candidates == 2, "lamp lenses of an unknown shader look like a pair of mirrors: the class table is what keeps them out");
  Car classed(0.0, {0, 0, 0});
  AddBody(&classed);
  AddLamps(&classed, kLampPs);
  Sim s4;
  r = Settle(&s4, classed, AsCamera(Cockpit(classed)));
  Check(!r.found && r.candidates == 0, "the same lenses of the lamp shader are not candidates at all");
  // one mirror: the other glass is missing
  Car one(0.0, {0, 0, 0});
  AddBody(&one);
  AddHandles(&one);
  one.Add(0x1001, one.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  Sim s5;
  r = Settle(&s5, one, AsCamera(Cockpit(one)));
  Check(!r.found && r.why == mg::Why::kNoPair && r.candidates == 3, "one mirror and two handles: three candidates, no pair (a glass and a handle are no pair)");
  Car lone(0.0, {0, 0, 0});
  AddBody(&lone);
  lone.Add(0x1001, lone.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  Sim s6;
  r = Settle(&s6, lone, AsCamera(Cockpit(lone)));
  Check(!r.found && r.why == mg::Why::kFewCandidates && r.candidates == 1, "one lone patch is never taken");
  // two glasses of different sizes: not a pair
  Car sizes(0.0, {0, 0, 0});
  sizes.Add(0x1001, sizes.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  sizes.Add(0x1002, sizes.Own(MakePatch({0, 1, 0}, Unit({-0.37, 0, 0.93}), 0.06, 0.06, 2, 2, 0.002)), kGlassPs, {-0.35, 0.87, -0.9});
  Sim s7;
  r = Settle(&s7, sizes, AsCamera(Cockpit(sizes)));
  Check(!r.found, "two patches of unlike areas are no pair");
  // two glasses at unlike heights: not a pair
  Car heights(0.0, {0, 0, 0});
  heights.Add(0x1001, heights.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  heights.Add(0x1002, heights.Own(MakeGlass(Unit({0.93, 0.0, 0.37}))), kGlassPs, {-0.35, 1.20, -0.9});
  Sim s8;
  r = Settle(&s8, heights, AsCamera(Cockpit(heights)));
  Check(!r.found, "two patches at unlike heights are no pair");
}

void TestDrawOrder() {
  // the glass on the left of the picture is first whatever order the draws came in: the right glass listed first, and a camera that looks the other way round
  Car car(0.0, {0, 0, 0});
  AddBody(&car);
  car.Add(0x1002, car.Own(MakeGlass(Unit({0.93, 0.0, 0.37}))), kGlassPs, {-0.35, 0.87, -0.9});  // the car's -z glass first
  car.Add(0x1001, car.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  Sim sim;
  const Cam cam = CarCamera(car, {2.2, 1.2, 0}, {-1, -0.05, 0}, 0.889, 0.5);
  mg::Result r = Settle(&sim, car, AsCamera(cam));
  Check(r.found && r.glass[0].mesh_key == 0x1001 && r.glass[1].mesh_key == 0x1002 && r.glass[0].centre_view[0] < r.glass[1].centre_view[0], "the right glass listed first: the left of the picture is still glass[0]");
  Check(r.glass[0].draw == IndexOfKey(car, 0x1001) && r.glass[1].draw == IndexOfKey(car, 0x1002), "... and each glass keeps its own draw");
  // the same car seen from the front, looking back at it: left and right swap on the picture, and so does the order
  const Cam front = CarCamera(car, {-6, 1.2, 0}, {1, -0.05, 0}, 0.889, 0.5);
  Sim s2;
  r = Settle(&s2, car, AsCamera(front));
  Check(r.found && r.glass[0].mesh_key == 0x1002 && r.glass[1].mesh_key == 0x1001, "seen from the front the car's -z glass is on the left of the picture");
  // one lone patch and a camera that is no camera: the cheap answer (under two candidates) comes first
  Car lone(0.0, {0, 0, 0});
  lone.Add(0x1001, lone.Own(MakeGlass(Unit({0.91, 0.0, -0.41}))), kGlassPs, {-0.35, 0.87, 0.9});
  Sim s3;
  Settle(&s3, lone, AsCamera(cam));
  mg::Camera broken = AsCamera(cam);
  broken.view = tp::Mat4{};
  r = s3.Evaluate(lone, broken);
  Check(!r.found && r.why == mg::Why::kFewCandidates && r.candidates == 1, "one candidate: 'few candidates', before the camera is looked at");
}

void TestMirroredMesh() {
  // the right mirror made from the left one's mesh and a mirrored matrix: one key, two draws, two places
  Car car(0.0, {0, 0, 0});
  AddBody(&car);
  const Mesh* glass = car.Own(MakeGlass(Unit({0.91, 0.0, -0.41})));
  car.Add(0x5001, glass, kGlassPs, {-0.35, 0.87, 0.9});
  car.Add(0x5001, glass, kGlassPs, {-0.35, 0.87, -0.9}, true);
  Sim sim;
  const Cam cam = CarCamera(car, {2.2, 1.2, 0}, {-1, -0.05, 0}, 0.889, 0.5);
  const mg::Result r = Settle(&sim, car, AsCamera(cam));
  Check(r.found && r.glass[0].mesh_key == 0x5001 && r.glass[1].mesh_key == 0x5001 && r.glass[0].draw != r.glass[1].draw, "two draws of one mesh key are a pair (two different places)");
  Check(sim.reads_by_key[0x5001] == 1, "the mesh was read once");
  // the mirrored matrix reflects the normal as a vector: the right glass faces back and to the +z side... the other way across
  const mg::Glass& a = r.glass[0];
  const mg::Glass& b = r.glass[1];
  Check(Near(a.normal_world[0], b.normal_world[0], 1e-3) && Near(a.normal_world[2], -b.normal_world[2], 1e-3) && std::fabs(a.normal_world[2]) > 0.3, "the two glasses' normals are mirror images across the car");
  Check(a.normal_world[2] * (a.centre_world[2]) < 0 && b.normal_world[2] * b.centre_world[2] < 0, "each glass faces inward, towards the middle of the car");
}

void TestPauseTiles() {
  // the pause view and the garage draw the car once per tile: the same draws again with the same matrices
  Car car = MakeR34();
  const size_t n = car.draws.size();
  for (int tile = 0; tile < 2; ++tile) {
    for (size_t i = 0; i < n; ++i) {
      car.draws.push_back(car.draws[i]);
    }
  }
  Sim sim;
  const Cam cam = CarCamera(car, {2.2, 1.2, 0}, {-1, -0.05, 0}, 0.889, 0.5);
  uint32_t considered = 0;
  const mg::Result r = Settle(&sim, car, AsCamera(cam));
  sim.Run(car, AsCamera(cam), nullptr, &considered);
  Check(considered == 12, "three listings of the car give twelve candidates before the duplicates are dropped");
  Check(r.found && r.candidates == 4, "the duplicates are dropped: four candidates, as for one listing");
  Check(r.glass[0].draw < n && r.glass[1].draw < n, "the glass draws are the first listing's");
  Check(r.need.mask == 3, "the need is the same");
  // two candidates of one mesh this near each other are one; further apart they are two places
  mg::Finder f;
  f.BeginFrame(1);
  Mesh patch = MakeGlass(Unit({0.91, 0.0, -0.41}));
  const auto read = [&](std::vector<float>* p, std::vector<uint32_t>* i, std::vector<float>* nrm) {
    *p = patch.positions;
    *i = patch.indices;
    *nrm = patch.normals;
    return true;
  };
  f.OnDraw(true, 9, 54, kGlassPs, read);
  f.BeginEvaluation();
  const tp::Mat4 m0 = Placement(0.0, {0, 0, 0}, {0, 0, 0});
  const tp::Mat4 m1 = Placement(0.0, {0.0004, 0, 0}, {0, 0, 0});
  const tp::Mat4 m2 = Placement(0.0, {0.05, 0, 0}, {0, 0, 0});
  f.Consider(0, true, 9, 54, kGlassPs, m0);
  f.Consider(1, true, 9, 54, kGlassPs, m1);
  f.Consider(2, true, 9, 54, kGlassPs, m2);
  const Cam cam2 = MakeCam({0, 0, 3}, {0, 0, -1}, 1.0, 0.6);
  const mg::Result rr2 = f.Choose(1, AsCamera(cam2));
  Check(rr2.candidates == 2 && rr2.why == mg::Why::kNoPair, "a copy 0.4 mm away is the same draw; one 5 cm away is another candidate (and two patches 5 cm apart are no pair)");
}

void TestPoses() {
  // the same car and cockpit camera, moved and turned anywhere in the world: the same answer in the view, and the world numbers follow
  Car home = MakeR34();
  Sim home_sim;
  const mg::Result a = Settle(&home_sim, home, AsCamera(Cockpit(home)));
  Check(a.found && a.need.mask == 1, "the reference pose");
  for (const double heading : {0.7, -2.1, 3.0}) {
    const V3 where{1500.0 * std::cos(heading), 3.0, -2200.0 * std::sin(heading)};
    Car car = MakeR34(heading, where);
    Sim sim;
    const Cam cam = Cockpit(car);
    const mg::Result b = Settle(&sim, car, AsCamera(cam));
    Check(b.found && b.need.mask == a.need.mask && b.why == a.why, "heading " + std::to_string(heading) + ": the same pair and the same need");
    bool same_view = true;
    for (int k = 0; k < 2; ++k) {
      for (int c = 0; c < 3; ++c) {
        same_view = same_view && std::fabs(b.glass[k].centre_view[c] - a.glass[k].centre_view[c]) < 2e-3 && std::fabs(b.glass[k].normal_view[c] - a.glass[k].normal_view[c]) < 2e-3;
      }
    }
    Check(same_view, "heading " + std::to_string(heading) + ": the glasses' centres and normals in the view space do not change with the pose");
    Check(std::abs(b.glass[0].px_wide - a.glass[0].px_wide) <= 1, "heading " + std::to_string(heading) + ": the width on the picture is the same");
    const tp::Mat4 turn = Placement(heading, {0, 0, 0}, {0, 0, 0});
    const V3 expected = ApplyDir(turn, {a.glass[0].normal_world[0], a.glass[0].normal_world[1], a.glass[0].normal_world[2]});
    Check(Near(b.glass[0].normal_world[0], expected.x, 2e-3) && Near(b.glass[0].normal_world[1], expected.y, 2e-3) && Near(b.glass[0].normal_world[2], expected.z, 2e-3),
          "heading " + std::to_string(heading) + ": the world normal turns with the car");
    const V3 expected_centre = Apply(car.draws[IndexOfKey(car, 0x1001)].model_to_world, {0, 0, 0});
    Check(std::fabs(b.glass[0].centre_world[0] - expected_centre.x) < 3e-3 && std::fabs(b.glass[0].centre_world[2] - expected_centre.z) < 3e-3, "heading " + std::to_string(heading) + ": the world centre follows");
    Check(std::fabs(b.eye_world[0] - cam.eye.x) < 1e-2 && std::fabs(b.eye_world[2] - cam.eye.z) < 1e-2, "heading " + std::to_string(heading) + ": the eye follows");
  }
}

void TestZConvention() {
  // the frame's view has z forward by construction; the game's own matrix (z backward) gives the same pair with the flag set, and a wrong flag puts the car behind the camera
  Car car = MakeR34();
  const Cam forward = Cockpit(car, true);
  const Cam backward = Cockpit(car, false);
  Sim s1, s2, s3;
  const mg::Result a = Settle(&s1, car, AsCamera(forward, false));
  const mg::Result b = Settle(&s2, car, AsCamera(backward, true));
  Check(a.found && b.found && a.need.mask == b.need.mask && a.flip_z == false && b.flip_z == true, "z backward with the flip gives the same pair and the same need");
  bool same = true;
  for (int k = 0; k < 2; ++k) {
    for (int c = 0; c < 3; ++c) {
      same = same && std::fabs(a.glass[k].centre_view[c] - b.glass[k].centre_view[c]) < 1e-3;
    }
  }
  Check(same && a.glass[0].px_wide == b.glass[0].px_wide, "... and the same view-space numbers");
  const mg::Result wrong = Settle(&s3, car, AsCamera(backward, false));
  Check(!wrong.need.need(), "a z backward matrix taken as z forward has the glasses behind the camera: no need (the flag matters)");
}

void TestUnusableCamera() {
  Car car = MakeR34();
  Sim sim;
  Cam cam = Cockpit(car);
  Settle(&sim, car, AsCamera(cam));
  // no lens: a view projection that is not a perspective's
  mg::Camera no_lens = AsCamera(cam);
  no_lens.view_projection = tp::Mat4{};
  mg::Result r = sim.Evaluate(car, no_lens);
  Check(r.found && !r.lens_known && r.why == mg::Why::kNoLens && r.need.found && !r.need.need(), "no lens: the pair is found, nothing can be said of the picture, no need");
  // a view that is not a number
  mg::Camera nan_view = AsCamera(cam);
  nan_view.view.m[0][0] = std::nan("");
  r = sim.Evaluate(car, nan_view);
  Check(!r.found && !r.need.need(), "a view that is not a number: no pair, no need");
  // a degenerate view (a zero matrix): no up
  mg::Camera zero = AsCamera(cam);
  zero.view = tp::Mat4{};
  r = sim.Evaluate(car, zero);
  Check(!r.found && !r.need.need(), "a zero view: no pair");
  // a model matrix that is not affine or not finite leaves its draw out
  Car broken = MakeR34();
  broken.draws[IndexOfKey(broken, 0x1002)].model_to_world.m[3][0] = std::nan("");
  r = sim.Evaluate(broken, AsCamera(cam));
  Check(!r.found && r.candidates == 3, "a glass draw whose matrix is not a number is left out: its partner has no pair");
}

void TestCandidatesAreFromDraws() {
  // only a rigid draw whose mesh the cache knows is a candidate: before the first read nothing is
  Car car = MakeR34();
  Sim sim;
  const Cam cam = Cockpit(car);
  sim.finder.BeginFrame(1);
  mg::Result r = sim.Evaluate(car, AsCamera(cam));
  Check(!r.found && r.why == mg::Why::kFewCandidates, "before any read there is nothing to choose from");
  r = sim.Run(car, AsCamera(cam));
  Check(r.found, "after the first frame's reads the pair is found in that frame");
  // a skinned draw is never a candidate even with a key
  Car skinned = MakeR34();
  for (Draw& d : skinned.draws) {
    if (d.key == 0x1001 || d.key == 0x1002) {
      d.rigid = false;
    }
  }
  Sim s2;
  r = Settle(&s2, skinned, AsCamera(cam));
  Check(!r.found, "the driver's draws (skinned) are never glass");
  // a glass of a paint shader is no candidate (a paint panel can be a small flat patch too)
  Car painted = MakeR34();
  for (Draw& d : painted.draws) {
    if (d.key == 0x1001 || d.key == 0x1002) {
      d.ps = kPaintPs;
    }
  }
  Sim s3;
  r = Settle(&s3, painted, AsCamera(cam));
  Check(!r.found, "two small flat patches of paint are no mirrors");
}

// ---------------------------------------------------------------------------
// the reference picture
// ---------------------------------------------------------------------------

void TestReferencePicture() {
  Check(mg::kReferenceHeight == 1080.0 && mg::kMinNeedWidth == 40, "the need is judged on a picture 1080 lines high, 40 px across");
  // a box 2 m across, 10 m ahead, with the lens of a 16:9 picture (tan_x 1, tan_y 9/16): the picture is 1920 x 1080, the box spans x = -0.1 .. 0.1 of the half width
  mcla::gpu::car_rt_instances::ViewBox box;
  const float lo[3] = {-1.0f, -0.5f, 10.0f};
  const float hi[3] = {1.0f, 0.5f, 10.0f};
  box.Add(lo);
  box.Add(hi);
  const auto r = mg::ReferenceRect(box, 1.0, 0.5625);
  Check(r.x0 == 863 && r.x1 == 1056 && r.Width() == 193, "a 2 m box at 10 m with a 90 degree field covers 193 px of 1920 (the rectangle has no margin)");
  Check(r.Height() >= 95 && r.Height() <= 99 && r.y0 >= 489 && r.y0 <= 493, "... and about 97 lines, centred on the middle of the 1080");
  // a lens twice as wide makes the picture twice as wide at the same height: the box has the same pixels across, as tall
  const auto wide = mg::ReferenceRect(box, 2.0, 0.5625);
  Check(wide.Height() == r.Height() && wide.Width() >= 190 && wide.Width() <= 196, "a lens twice as wide at the same height: the same box has the same pixels across");
  // half the distance: twice the width
  const float near_lo[3] = {-1.0f, -0.5f, 5.0f};
  const float near_hi[3] = {1.0f, 0.5f, 5.0f};
  mcla::gpu::car_rt_instances::ViewBox near_box;
  near_box.Add(near_lo);
  near_box.Add(near_hi);
  const auto n = mg::ReferenceRect(near_box, 1.0, 0.5625);
  Check(n.Width() >= 382 && n.Width() <= 388, "half the distance: twice the width");
  // a box behind the camera has no rectangle
  mcla::gpu::car_rt_instances::ViewBox behind;
  const float b0[3] = {-1.0f, -0.5f, -10.0f};
  const float b1[3] = {1.0f, 0.5f, -9.0f};
  behind.Add(b0);
  behind.Add(b1);
  Check(mg::ReferenceRect(behind, 1.0, 0.5625).Empty(), "a box behind the camera has no rectangle");
  // a box off to the side has none either
  mcla::gpu::car_rt_instances::ViewBox side;
  const float s0[3] = {30.0f, -0.5f, 10.0f};
  const float s1[3] = {31.0f, 0.5f, 10.0f};
  side.Add(s0);
  side.Add(s1);
  Check(mg::ReferenceRect(side, 1.0, 0.5625).Empty(), "a box beyond the picture's edge has no rectangle");
}

// ---------------------------------------------------------------------------
// the need and its publication
// ---------------------------------------------------------------------------

void TestNeed() {
  using mg::NeedState;
  Check(mg::kMinNeedWidth == 40 && mg::kNeedHoldFrames == 20, "the numbers: 40 px, a hold of 20 frames");
  NeedState s;
  s.found = true;
  s.mask = 2;
  s.widest = 190;
  Check(mg::Unpack(mg::Pack(s)) == s, "pack and unpack round trip");
  NeedState big;
  big.found = true;
  big.mask = 3;
  big.widest = 65535;
  Check(mg::Unpack(mg::Pack(big)) == big, "... also the largest width");
  Check(mg::Pack(NeedState{}) == 0 && !mg::Unpack(0).need() && !mg::Unpack(0).found, "zero is 'nothing found'");
  Check(mg::Pack(NeedState{true, 0, 0}) != 0 && !mg::Unpack(mg::Pack(NeedState{true, 0, 0})).need(), "found with no need is its own value");
  mg::WithdrawNeed();
  Check(!mg::SideNeed(), "withdrawn: no need");
  mg::PublishNeed(s);
  Check(mg::SideNeed() && mg::CurrentNeed() == s, "published: the guest side reads it");
  mg::WithdrawNeed();
  Check(!mg::SideNeed() && !mg::CurrentNeed().found, "withdrawn again");

  // the hold
  mg::NeedHold hold;
  NeedState need;
  need.found = true;
  need.mask = 1;
  need.widest = 120;
  NeedState quiet;
  quiet.found = true;
  Check(hold.Update(quiet) == quiet && !hold.holding(), "no need yet: nothing is held");
  Check(hold.Update(need) == need && hold.holding(), "a need is published at once");
  bool held = true;
  for (uint32_t i = 1; i <= mg::kNeedHoldFrames; ++i) {
    const NeedState out = hold.Update(NeedState{});
    held = held && out.need() && out.mask == 1 && out.widest == 120 && out.found;
  }
  Check(held, "for kNeedHoldFrames frames without a need the last need stays up");
  Check(!hold.Update(NeedState{}).need() && !hold.holding(), "then it falls");
  // a need that comes back in time restarts the hold
  mg::NeedHold again;
  again.Update(need);
  for (uint32_t i = 0; i < mg::kNeedHoldFrames - 1; ++i) {
    again.Update(NeedState{});
  }
  again.Update(need);
  bool held2 = true;
  for (uint32_t i = 1; i <= mg::kNeedHoldFrames; ++i) {
    held2 = held2 && again.Update(NeedState{}).need();
  }
  Check(held2 && !again.Update(NeedState{}).need(), "a need that comes back restarts the hold");
  // a held frame says 'found' if the car's pair is found even with no need; the new need replaces the old mask
  mg::NeedHold mask;
  mask.Update(need);
  NeedState other = need;
  other.mask = 2;
  other.widest = 60;
  Check(mask.Update(other) == other, "a new need replaces the held one");
  hold.Reset();
  Check(!hold.holding() && !hold.Update(NeedState{}).need(), "Reset forgets the hold");
}

// ---------------------------------------------------------------------------
// the files
// ---------------------------------------------------------------------------

void TestFiles(const std::string& src) {
  const std::string rules = ReadFile(src + "/gpu/mirror_glass_rules.h");
  const std::string need = ReadFile(src + "/gpu/mirror_glass_need.h");
  const std::string host = ReadFile(src + "/gpu/mirror_glass.cpp");
  const std::string host_h = ReadFile(src + "/gpu/mirror_glass.h");
  const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
  const std::string gpu_settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string scene = ReadFile(src + "/gpu/scene.cpp");
  const std::string renderer = ReadFile(src + "/gpu/renderer.cpp");
  if (rules.empty() || host.empty() || cmake.empty()) {
    std::printf("NOTE: the source files were not found under %s: the file checks are left out\n", src.c_str());
    return;
  }
  // the rules header reuses MIRROR-RT's pure header and does not copy its numbers
  Check(Has(rules, "#include \"car_mirror_patch_rules.h\"") && Has(rules, "cm::ChoosePair") && Has(rules, "cm::Place") && Has(rules, "cm::MeasurePatch") && Has(rules, "cm::IsPatchCandidate"),
        "the rules header calls MIRROR-RT's MeasurePatch, IsPatchCandidate, Place and ChoosePair");
  Check(!Has(rules, "kMinFlat =") && !Has(rules, "kMaxSeparation =") && !Has(rules, "kMinPatchArea ="), "... and does not redefine their numbers");
  // the guest side includes only the small header
  Check(Has(need, "#include <atomic>") && !Has(need, "#include \"car_") && !Has(need, "plume"), "mirror_glass_need.h holds nothing of the renderer (the guest hooks include it)");
  // the setting: debug only, off
  const std::string define = std::string("REX_") + "SETTING(bool, mcla_gpu_mirror_glass, false,";  // (spelled in two pieces: tools/check_settings_exposure.py reads every source for definitions)
  Check(Has(gpu_settings, define) && Has(manifest, "\"mcla_gpu_mirror_glass\"") && Has(meta, "{\"mcla_gpu_mirror_glass\", \"MCLA/GPU\"}"),
        "mcla_gpu_mirror_glass is defined, default false, in the three generated files");
  const size_t at = gpu_settings.find(define.substr(0, define.size() - std::string(" false,").size() + 1));
  const size_t end = at == std::string::npos ? at : gpu_settings.find(");", at);
  Check(at != std::string::npos && end != std::string::npos && Has(gpu_settings.substr(at, end - at), "rex::kDebugOnly") && !Has(gpu_settings.substr(at, end - at), "kRequiresRestart"),
        "... debug only and live");
  Check(Has(host, "mcla_gpu_mirror_glass") && Has(host, "mg::Finder") && Has(host_h, "car_frame::Listener") && Has(host, "mg::PublishNeed(") && Has(host, "mg::WithdrawNeed("),
        "the listener reads the setting, owns the finder and publishes (and withdraws) the need");
  // the build: the source and the test
  Check(Has(cmake, "mirror_glass.cpp") && Has(cmake, "add_executable(larecomp_mirror_glass_rules_test EXCLUDE_FROM_ALL mirror_glass_rules_test.cpp)"), "CMakeLists.txt builds the listener and the test");
  // the wiring: the listener is asked in the frame's start, and evaluates before CarFrame's present clears the frame
  const size_t begin_glass = scene.find("mirror_glass_->BeginFrame()");
  const size_t begin_frame = scene.find("car_frame_->BeginFrame()");
  Check(begin_glass != std::string::npos && begin_frame != std::string::npos && begin_glass < begin_frame, "Scene::BeginFrame asks the finder before CarFrame::BeginFrame collects (the request is for the next frame)");
  const size_t present_glass = scene.find("mirror_glass_->OnPresent()");
  const size_t present_frame = scene.find("car_frame_->OnPresent()");
  Check(present_glass != std::string::npos && present_frame != std::string::npos && present_glass < present_frame, "Scene::OnPresent runs the finder before CarFrame::OnPresent clears the frame");
  Check(Has(renderer, "mirror_glass_.Initialize(car_frame_)") && Has(renderer, "scene_.SetMirrorGlass(&mirror_glass_)"), "the renderer makes the listener and gives it to the scene");
}

// `--table`: what the finder says of the R34-like car for the cameras of the tests (perf/MIRROR-VIEW.md quotes it).
void PrintTable() {
  Car car = MakeR34();
  struct Row {
    const char* name;
    Cam cam;
  };
  const Row rows[] = {
      {"cockpit, 112 degrees across", Cockpit(car)},
      {"chase 2.5 m from the glasses", CarCamera(car, {2.2, 1.2, 0}, {-1, -0.05, 0}, 0.889, 0.5)},
      {"chase 8 m behind", CarCamera(car, {8, 1.5, 0}, {-1, -0.1, 0}, 0.889, 0.5)},
      {"bumper, ahead of the glasses", CarCamera(car, {-2.5, 0.8, 0}, {-1, 0, 0}, 1.2, 0.675)},
      {"turned away to the side", CarCamera(car, {2.2, 1.2, 0}, {0, 0, 1}, 0.889, 0.5)},
  };
  std::printf("%-32s %-28s %-6s %10s %10s\n", "camera (R34-like car)", "verdict", "mask", "left px", "right px");
  for (const Row& row : rows) {
    Sim sim;
    const mg::Result r = Settle(&sim, car, AsCamera(row.cam));
    std::printf("%-32s %-28s %-6u %10d %10d\n", row.name, mg::WhyName(r.why), unsigned(r.need.mask), r.glass[0].px_wide, r.glass[1].px_wide);
  }
}

}  // namespace

void TestReaderExtent() {
  namespace mg = mcla::gpu::mirror_glass;
  mg::Result glass;
  glass.need.mask = 1;
  glass.glass[0].box.valid = true;
  glass.glass[0].box.min[0] = -0.1f;
  glass.glass[0].box.max[0] = 0.1f;
  glass.glass[0].box.min[1] = -0.1f;
  glass.glass[0].box.max[1] = 0.1f;
  glass.glass[0].box.min[2] = glass.glass[0].box.max[2] = 1.0f;
  glass.glass[0].normal_view[2] = 1.0f;
  glass.glass[1] = glass.glass[0];
  glass.glass[1].normal_view[0] = 0.235f;
  glass.glass[1].normal_view[2] = 0.972f;

  mg::FitRearField(glass);
  Check(glass.need.hfov_deg == 13 && glass.need.vfov_deg == 13, "the centred flat glass fits its reflected corners");
  const auto one = glass.need;
  glass.need.mask = 3;
  mg::FitRearField(glass);
  Check(glass.need.hfov_deg > one.hfov_deg, "the more oblique glass widens the union only while needed");
  Check(mg::Unpack(mg::Pack(glass.need)) == glass.need, "both fields travel with the need mask");
}

int main(int argc, char** argv) {
  TestReaderExtent();
  if (argc > 1 && std::string(argv[1]) == "--table") {
    PrintTable();
    return 0;
  }
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    const std::string self = __FILE__;
    const size_t slash = self.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : self.substr(0, slash);  // .../src/gpu
    src = dir + "/..";
  }
  TestGate();
  TestMeasure();
  TestCache();
  TestFinderReads();
  TestBudget();
  TestR34Cockpit();
  TestHiddenByCamera();
  TestNoMirrors();
  TestDrawOrder();
  TestMirroredMesh();
  TestPauseTiles();
  TestPoses();
  TestZConvention();
  TestUnusableCamera();
  TestCandidatesAreFromDraws();
  TestReferencePicture();
  TestNeed();
  TestFiles(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
