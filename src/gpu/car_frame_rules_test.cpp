// Check of the car frame's rules (car_frame_rules.h): the mesh key and the fingerprints of a mesh's own bytes it is made from, which draw is the player's (the identity tracker), the matrix of a draw
// out of the registers its recipe reads, the camera and the sun out of a car draw's vertex bank, the normals as the declarations give them,
// the blend of a skinned normal, and the boxes. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_car_frame_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_frame_rules_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "car_frame_rules.h"

namespace cd = mcla::gpu::car_dump;
namespace fr = mcla::gpu::car_frame_rules;
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

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

double car_dump_match_metres() { return cd::kPlayerMatchMetres; }

// ---------------------------------------------------------------------------
// a hand-made vertex shader bank
// ---------------------------------------------------------------------------

struct Bank {
  uint8_t bytes[cd::kRegisterCount * 16] = {};

  void Set(uint32_t reg, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    for (int c = 0; c < 4; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &v[c], 4);
      uint8_t* p = bytes + size_t(reg) * 16 + size_t(c) * 4;
      p[0] = uint8_t(bits >> 24);
      p[1] = uint8_t(bits >> 16);
      p[2] = uint8_t(bits >> 8);
      p[3] = uint8_t(bits);
    }
  }
  void SetMatrix(uint32_t first, const tp::Mat4& m) {
    for (uint32_t r = 0; r < 4; ++r) {
      Set(first + r, float(m.m[r][0]), float(m.m[r][1]), float(m.m[r][2]), float(m.m[r][3]));
    }
  }
};

tp::Mat4 Perspective(double fov_y, double aspect, double n, double f) {
  tp::Mat4 p;
  const double h = 1.0 / std::tan(fov_y * 0.5);
  p.m[0][0] = h / aspect;
  p.m[1][1] = h;
  p.m[2][2] = f / (n - f);
  p.m[2][3] = -1.0;
  p.m[3][2] = n * f / (n - f);
  return p;
}

// The rotation about y by `a` radians as rows (row vectors).
tp::Mat4 RotationY(double a) {
  tp::Mat4 r = tp::Identity();
  r.m[0][0] = std::cos(a);
  r.m[0][2] = -std::sin(a);
  r.m[2][0] = std::sin(a);
  r.m[2][2] = std::cos(a);
  return r;
}

void BigEndian(uint32_t v, uint8_t* out) {
  out[0] = uint8_t(v >> 24);
  out[1] = uint8_t(v >> 16);
  out[2] = uint8_t(v >> 8);
  out[3] = uint8_t(v);
}

void BigEndianFloat(float f, uint8_t* out) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  BigEndian(bits, out);
}

// ---------------------------------------------------------------------------
// the mesh key
// ---------------------------------------------------------------------------

fr::MeshKeyParts BaseParts() {
  fr::MeshKeyParts p;
  p.position = fr::StreamKey{0x01234560u, 0x00040000u, 48u, 0u, 5u, 777u};
  p.normal = fr::StreamKey{0u, 0u, 0u, 24u, 40u, 0u};
  p.indexed = 1;
  p.primitive = 4;  // triangle list
  p.count = 3000;
  p.start = 0;
  p.base_vertex = 0;
  p.index_address = 0x02000000u;
  p.index_bytes = 6000;
  p.index32 = 0;
  p.endian = 2;
  p.index_content = 555u;
  p.reset = 0;
  p.reset_index = 0;
  return p;
}

void TestMeshKey() {
  const fr::MeshKeyParts base = BaseParts();
  const uint64_t key = fr::MeshKeyOf(base);
  Check(key != 0, "a mesh key is never 0");
  Check(fr::MeshKeyOf(BaseParts()) == key, "the same parts make the same key");

  // Every part of a mesh's identity changes the key (a mesh streamed in again at the same address has other bytes: other fingerprints).
  std::vector<fr::MeshKeyParts> changed;
  const auto add = [&](auto&& edit) {
    fr::MeshKeyParts p = BaseParts();
    edit(p);
    changed.push_back(p);
  };
  add([](fr::MeshKeyParts& p) { p.position.address += 16; });
  add([](fr::MeshKeyParts& p) { p.position.size += 16; });
  add([](fr::MeshKeyParts& p) { p.position.stride += 4; });
  add([](fr::MeshKeyParts& p) { p.position.offset += 4; });
  add([](fr::MeshKeyParts& p) { p.position.format += 1; });
  add([](fr::MeshKeyParts& p) { p.position.content += 1; });  // the vertex bytes the draw uses are other bytes
  add([](fr::MeshKeyParts& p) { p.normal.address = 0x03000000u; });
  add([](fr::MeshKeyParts& p) { p.normal.size = 4096; });
  add([](fr::MeshKeyParts& p) { p.normal.stride = 12; });
  add([](fr::MeshKeyParts& p) { p.normal.offset += 4; });
  add([](fr::MeshKeyParts& p) { p.normal.format += 1; });
  add([](fr::MeshKeyParts& p) { p.normal.content = 9; });
  add([](fr::MeshKeyParts& p) { p.indexed = 0; });
  add([](fr::MeshKeyParts& p) { p.primitive = 6; });
  add([](fr::MeshKeyParts& p) { p.count += 3; });
  add([](fr::MeshKeyParts& p) { p.start += 3; });
  add([](fr::MeshKeyParts& p) { p.base_vertex = -5; });
  add([](fr::MeshKeyParts& p) { p.index_address += 2; });
  add([](fr::MeshKeyParts& p) { p.index_bytes += 2; });
  add([](fr::MeshKeyParts& p) { p.index32 = 1; });
  add([](fr::MeshKeyParts& p) { p.endian = 1; });
  add([](fr::MeshKeyParts& p) { p.index_content += 1; });  // the index bytes are other bytes
  add([](fr::MeshKeyParts& p) { p.reset = 1; });
  add([](fr::MeshKeyParts& p) { p.reset_index = 0xFFFF; });
  std::set<uint64_t> keys;
  keys.insert(key);
  for (size_t i = 0; i < changed.size(); ++i) {
    const uint64_t k = fr::MeshKeyOf(changed[i]);
    Check(k != 0 && k != key, "changing part " + std::to_string(i) + " of a mesh changes its key");
    keys.insert(k);
  }
  Check(keys.size() == changed.size() + 1, "the keys of the changed meshes are all different");

  // Swapping two numbers that sit side by side is another mesh (the fields are not just added).
  fr::MeshKeyParts a = BaseParts(), b = BaseParts();
  a.count = 100;
  a.start = 200;
  b.count = 200;
  b.start = 100;
  Check(fr::MeshKeyOf(a) != fr::MeshKeyOf(b), "count and start swapped make another key");
  fr::MeshKeyParts c = BaseParts(), d = BaseParts();
  c.position.stride = 12;
  c.position.offset = 48;
  d.position.stride = 48;
  d.position.offset = 12;
  Check(fr::MeshKeyOf(c) != fr::MeshKeyOf(d), "stride and offset swapped make another key");

  // The key says when a cache entry is old.
  Check(!fr::Stale(100, 105, 10) && fr::Stale(100, 111, 10) && !fr::Stale(100, 110, 10), "a key last seen more than `keep` frames ago is stale");
  Check(!fr::Stale(200, 100, 10) && !fr::Stale(100, 100, 0), "a key seen now, or in the future, is not stale");
}

// ---------------------------------------------------------------------------
// which draw is the player's
// ---------------------------------------------------------------------------

void TestIdentity() {
  using Standing = fr::IdentityTracker::Standing;
  const double player[3] = {100.0, 0.0, 0.0};
  const auto at = [](double x, double y, double z) { return std::vector<double>{x, y, z}; };

  // The player's car drawn first: its root draw (0.5 m from the vehicle) confirms the ID; a tyre whose gWorld is far is told by the ID.
  {
    fr::IdentityTracker t;
    t.BeginFrame(0);
    auto p = at(100.5, 0, 0);
    fr::IdentityTracker::Verdict v = t.OnDraw(1, p.data(), player);
    Check(v.standing == Standing::kConfirmed && v.confirmed_now && !v.switched && v.improved && Near(v.distance, 0.5), "a draw near the vehicle confirms its ID");
    Check(t.confirmed() == 1 && t.assumed_id() == 1, "the ID is confirmed");
    p = at(110, 0, 0);
    v = t.OnDraw(1, p.data(), player);
    Check(v.standing == Standing::kConfirmed && !v.confirmed_now && !v.improved, "a draw of the confirmed ID whose gWorld is far is the player's all the same");
    p = at(103, 0, 0);
    v = t.OnDraw(2, p.data(), player);
    Check(v.standing == Standing::kOther && !v.switched && v.improved && t.confirmed() == 1, "another ID within 5 m but not nearer does not take the match");
    p = at(300, 0, 0);
    v = t.OnDraw(3, p.data(), player);
    Check(v.standing == Standing::kOther && !v.improved, "a far car is another car");
    p = at(100.1, 0, 0);
    v = t.OnDraw(2, p.data(), player);
    Check(v.switched && !v.confirmed_now && v.standing == Standing::kConfirmed && t.confirmed() == 2, "an ID that comes nearer than the confirmed one takes it");
    p = at(100.2, 0, 0);
    v = t.OnDraw(1, p.data(), player);
    Check(v.standing == Standing::kOther, "and the old one is another car now");
    Check(Near(t.best(2), 0.1) && Near(t.best(1), 0.2) && std::isinf(t.best(7)), "the nearest distance of each ID is kept");
  }

  // The frame before's ID stands in until a draw is near the vehicle.
  {
    fr::IdentityTracker t;
    t.BeginFrame(3);
    Check(t.previous() == 3 && t.confirmed() == 0 && t.assumed_id() == 3, "the previous frame's ID is assumed at the start");
    auto p = at(500, 0, 0);
    fr::IdentityTracker::Verdict v = t.OnDraw(3, p.data(), player);
    Check(v.standing == Standing::kTentative && !v.confirmed_now, "the previous frame's ID is tentative while nothing is near");
    v = t.OnDraw(4, p.data(), player);
    Check(v.standing == Standing::kOther, "another ID is not the player's");
    p = at(101, 0, 0);
    v = t.OnDraw(4, p.data(), player);
    Check(v.confirmed_now && v.standing == Standing::kConfirmed && t.confirmed() == 4, "a draw near the vehicle confirms another ID than the previous one's");
    p = at(500, 0, 0);
    v = t.OnDraw(3, p.data(), player);
    Check(v.standing == Standing::kOther && t.assumed_id() == 4, "once confirmed the previous frame's ID is no longer assumed");
  }

  // A confirmed ID equal to the previous one stays tentative until a draw is near; then confirmed.
  {
    fr::IdentityTracker t;
    t.BeginFrame(1);
    auto p = at(100, 0, 0);
    const fr::IdentityTracker::Verdict v = t.OnDraw(1, p.data(), player);
    Check(v.confirmed_now && v.standing == Standing::kConfirmed, "the previous ID's own near draw confirms it");
  }

  // The 5 m rule.
  {
    fr::IdentityTracker t;
    t.BeginFrame(0);
    auto p = at(105.01, 0, 0);
    fr::IdentityTracker::Verdict v = t.OnDraw(1, p.data(), player);
    Check(!v.improved && t.confirmed() == 0 && v.standing == Standing::kOther, "a draw 5.01 m from the vehicle is not a match");
    p = at(105.0, 0, 0);
    v = t.OnDraw(1, p.data(), player);
    Check(v.confirmed_now && t.confirmed() == 1, "a draw 5 m from the vehicle is");
  }

  // A car alongside (another car's ID within 5 m of the vehicle, drawn before the player's): with a previous ID it takes no match beyond 1.5 m.
  {
    fr::IdentityTracker t;
    t.BeginFrame(2);
    auto p = at(103.5, 0, 0);
    fr::IdentityTracker::Verdict v = t.OnDraw(1, p.data(), player);
    Check(!v.improved && !v.confirmed_now && t.confirmed() == 0 && v.standing == Standing::kOther && Near(v.distance, 3.5),
          "an opponent 3.5 m beside the vehicle, drawn first, does not take the player's place when the frame before had an ID");
    p = at(100.2, 0, 0);
    v = t.OnDraw(2, p.data(), player);
    Check(v.confirmed_now && t.confirmed() == 2 && v.standing == Standing::kConfirmed, "the previous ID's own draw near the vehicle confirms it as ever");
    p = at(102.0, 0, 0);
    v = t.OnDraw(1, p.data(), player);
    Check(v.standing == Standing::kOther && !v.switched && t.confirmed() == 2, "and the opponent stays another car");
    Check(fr::IdentityTracker::kStrictMatchMetres < car_dump_match_metres() && fr::IdentityTracker::kStrictMatchMetres > 1.0,
          "the strict match is between a car's own wheels' offset and the 5 m rule");
  }
  {
    // The strict match still takes an ID that is that near (a car on the vehicle: the player's ID changed).
    fr::IdentityTracker t;
    t.BeginFrame(2);
    auto p = at(101.0, 0, 0);
    const fr::IdentityTracker::Verdict v = t.OnDraw(5, p.data(), player);
    Check(v.confirmed_now && t.confirmed() == 5, "another ID within 1.5 m of the vehicle is the player's car under a new ID");
    // Without a previous ID the 5 m rule stands (the first frame).
    fr::IdentityTracker f;
    f.BeginFrame(0);
    auto q = at(103.5, 0, 0);
    Check(f.OnDraw(1, q.data(), player).confirmed_now, "with no previous ID a draw 3.5 m from the vehicle is a match");
    // The ID of the previous frame is matched at 5 m even when another ID is nearer than 1.5 m is not required.
    fr::IdentityTracker g;
    g.BeginFrame(3);
    auto r = at(104.5, 0, 0);
    Check(g.OnDraw(3, r.data(), player).confirmed_now && g.confirmed() == 3, "the previous frame's ID is matched at the 5 m rule (the vehicle's state lags at speed)");
  }

  // No player's vehicle, no translation, no usable ID: nothing is confirmed and nothing crashes.
  {
    fr::IdentityTracker t;
    t.BeginFrame(2);
    auto p = at(100, 0, 0);
    fr::IdentityTracker::Verdict v = t.OnDraw(2, p.data(), nullptr);
    Check(v.standing == Standing::kTentative && t.confirmed() == 0, "no player's vehicle: only the previous ID is assumed");
    v = t.OnDraw(2, nullptr, player);
    Check(v.standing == Standing::kTentative && !v.improved && t.confirmed() == 0, "no translation: no match");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double bad[3] = {nan, 0, 0};
    v = t.OnDraw(5, bad, player);
    Check(!v.improved && v.standing == Standing::kOther && t.confirmed() == 0, "a translation that is not a number is no match");
    Check(t.OnDraw(0, p.data(), player).standing == Standing::kOther && t.OnDraw(24, p.data(), player).standing == Standing::kOther &&
              t.OnDraw(99, p.data(), player).standing == Standing::kOther,
          "ID 0 and IDs past the table are no vehicle");
  }

  // The next frame starts from what this one found.
  {
    fr::IdentityTracker t;
    t.BeginFrame(0);
    auto p = at(100, 0, 0);
    t.OnDraw(5, p.data(), player);
    const uint32_t found = t.confirmed();
    t.BeginFrame(found);
    Check(found == 5 && t.previous() == 5 && t.confirmed() == 0 && std::isinf(t.best(5)), "BeginFrame forgets the distances and keeps the ID as the previous one");
    t.BeginFrame(77);
    Check(t.previous() == 0, "an ID past the table is not kept as the previous one");
  }
}

// ---------------------------------------------------------------------------
// the matrix of a draw
// ---------------------------------------------------------------------------

Bank CarBank() {
  Bank bank;
  tp::Mat4 world = RotationY(0.5);
  world.m[3][0] = 10.0;
  world.m[3][1] = 2.0;
  world.m[3][2] = -5.0;
  bank.SetMatrix(cd::kWorldRegister, world);
  bank.Set(cd::kPaletteRegister + 6, 2, 0, 0, 0.5f);  // bone 2: a scale of 2 and an offset along x
  bank.Set(cd::kPaletteRegister + 7, 0, 2, 0, 0);
  bank.Set(cd::kPaletteRegister + 8, 0, 0, 2, 0);
  bank.Set(cd::kBoneIndexRegister, 2.75f, 0.0f, 0.0f, 0.0f);
  return bank;
}

void TestDecodeModel() {
  const Bank bank = CarBank();
  for (cd::Recipe recipe : {cd::Recipe::kPalette, cd::Recipe::kWorld, cd::Recipe::kSkinned}) {
    const cd::CarMatrices full = cd::DecodeCarMatrices(bank.bytes);
    const fr::ModelDecode lean = fr::DecodeModel(bank.bytes, recipe);
    Check(lean.finite, std::string("the model matrix of recipe '") + cd::RecipeName(recipe) + "' is finite");
    Check(tp::MaxAbsDiff(lean.model, cd::ModelMatrix(full, recipe)) < 1e-12, std::string("DecodeModel makes DecodeCarMatrices' model matrix for '") + cd::RecipeName(recipe) + "'");
    Check(tp::MaxAbsDiff(lean.world, full.world) == 0.0, "... and the same gWorld");
  }
  const fr::ModelDecode palette = fr::DecodeModel(bank.bytes, cd::Recipe::kPalette);
  Check(palette.bone_valid && palette.bone == 2, "the palette recipe takes bone trunc(c204.x)");
  const fr::ModelDecode world = fr::DecodeModel(bank.bytes, cd::Recipe::kWorld);
  Check(!world.bone_valid && tp::MaxAbsDiff(world.model, world.world) == 0.0, "the world-only recipe reads no bone and its model is gWorld");
  const fr::ModelDecode skinned = fr::DecodeModel(bank.bytes, cd::Recipe::kSkinned);
  Check(tp::MaxAbsDiff(skinned.model, skinned.world) == 0.0, "a skinned draw's matrix is gWorld alone");

  // A bone index outside the palette leaves gWorld alone; a NaN in gWorld is not finite; a NaN in the bone's rows is not.
  Bank bad = bank;
  bad.Set(cd::kBoneIndexRegister, 64.0f, 0, 0, 0);
  const fr::ModelDecode out_of_range = fr::DecodeModel(bad.bytes, cd::Recipe::kPalette);
  Check(!out_of_range.bone_valid && tp::MaxAbsDiff(out_of_range.model, out_of_range.world) == 0.0, "a bone outside the palette leaves gWorld alone");
  Bank nan_world = bank;
  nan_world.Set(cd::kWorldRegister + 1, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0);
  Check(!fr::DecodeModel(nan_world.bytes, cd::Recipe::kWorld).finite, "a gWorld with a NaN is not finite");
  Bank nan_bone = bank;
  nan_bone.Set(cd::kPaletteRegister + 7, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0);
  Check(!fr::DecodeModel(nan_bone.bytes, cd::Recipe::kPalette).finite, "a NaN in the bone's rows is not finite for the palette recipe");
  Check(fr::DecodeModel(nan_bone.bytes, cd::Recipe::kWorld).finite, "... and does not matter for a recipe that reads no palette");

  double t[3];
  Check(fr::WorldTranslation(bank.bytes, t) && Near(t[0], 10.0) && Near(t[1], 2.0) && Near(t[2], -5.0), "gWorld's translation is row 3 of c0-c3");
  Bank nan_t = bank;
  nan_t.Set(cd::kWorldRegister + 3, 1.0f, std::numeric_limits<float>::infinity(), 3.0f, 1.0f);
  Check(!fr::WorldTranslation(nan_t.bytes, t), "a translation with an infinity is not finite");
}

// ---------------------------------------------------------------------------
// the camera and the sun
// ---------------------------------------------------------------------------

void TestCamera() {
  // A camera at (10, 2, -5) looking along world +x: right = (0, 0, 1), up = (0, 1, 0), BACKWARD (c14) = (-1, 0, 0).
  tp::Mat4 view_inverse = tp::Identity();
  view_inverse.m[0][0] = 0.0;
  view_inverse.m[0][2] = 1.0;
  view_inverse.m[2][0] = -1.0;
  view_inverse.m[2][2] = 0.0;
  view_inverse.m[3][0] = 10.0;
  view_inverse.m[3][1] = 2.0;
  view_inverse.m[3][2] = -5.0;
  tp::Mat4 game_view;
  Check(tp::Inverse(view_inverse, &game_view), "the test camera's view inverse inverts");
  const tp::Mat4 vp = tp::Mul(game_view, Perspective(1.0471975511965976, 16.0 / 9.0, 0.2, 1000.0));

  // A car 30 m ahead, turned 30 degrees.
  tp::Mat4 world = RotationY(0.5236);
  world.m[3][0] = 40.0;
  world.m[3][1] = 1.0;
  world.m[3][2] = -3.0;
  const tp::Mat4 wvp = tp::Mul(world, vp);

  Bank bank;
  bank.SetMatrix(cd::kWorldRegister, world);
  bank.SetMatrix(cd::kWvpRegister, wvp);
  bank.SetMatrix(fr::kViewInverseRegister, view_inverse);
  bank.Set(fr::kSunRegister, 0.2f, -0.8f, 0.1f, 0.0f);  // the direction the light travels: down

  const fr::BankCamera cam = fr::CameraFromBank(bank.bytes, world, wvp);
  Check(cam.valid, "a car draw's bank with a view inverse gives a camera");
  // The occlusion's view space (ao_common.hlsli AoViewPos): x right, y up, z forward = the distance along the view axis. The point 5 m
  // ahead, 2 m to the right and 1 m up of the camera is (2, 1, 5) there.
  const double ahead[3] = {10.0 + 5.0, 2.0 + 1.0, -5.0 + 2.0};  // + 5 f + 1 up + 2 right, f = +x, right = +z
  double v[4];
  cd::TransformPoint(cam.view, ahead, v);
  Check(Near(v[0], 2.0, 1e-6) && Near(v[1], 1.0, 1e-6) && Near(v[2], 5.0, 1e-6) && Near(v[3], 1.0, 1e-9),
        "the view puts a point 5 m ahead, 2 m right and 1 m up at (2, 1, 5): z is forward, positive");
  const double behind[3] = {10.0 - 4.0, 2.0, -5.0};
  cd::TransformPoint(cam.view, behind, v);
  Check(v[2] < 0.0 && Near(v[2], -4.0, 1e-6), "a point behind the camera has a negative z");
  // The game's own view looks along -z: its z is the negative of this one's.
  const double game_ahead[3] = {15.0, 3.0, -3.0};
  double g[4];
  cd::TransformPoint(game_view, game_ahead, g);
  cd::TransformPoint(cam.view, game_ahead, v);
  Check(Near(g[0], v[0], 1e-9) && Near(g[1], v[1], 1e-9) && Near(g[2], -v[2], 1e-9), "the view is the game's with z negated (ao.cpp: view[2] = -view[2])");

  // gWorld^-1 * gWorldViewProj is the world's view-projection.
  Check(tp::MaxAbsDiff(cam.view_projection, vp) <= 1e-4 * tp::MaxAbs(vp), "gWorld^-1 * gWorldViewProj is the camera's view-projection");
  // The clip w of a world point is its distance along the view axis: the same as the occlusion's z.
  const cd::Vec4 clip = cd::Project(cam.view_projection, ahead);
  Check(Near(clip.w, 5.0, 1e-4), "the clip w through view_projection is the distance the view's z gives");

  // The sun: toward the sun is minus the travel direction, unit length, pointing up.
  Check(cam.sun_valid && cam.to_sun[1] > 0.9 * 0.97 && Near(cam.to_sun[0] * cam.to_sun[0] + cam.to_sun[1] * cam.to_sun[1] + cam.to_sun[2] * cam.to_sun[2], 1.0, 1e-9),
        "the direction to the sun is unit length and points up when the light travels down");
  Check(Near(cam.to_sun[0], -0.2 / std::sqrt(0.04 + 0.64 + 0.01), 1e-6) && Near(cam.to_sun[2], -0.1 / std::sqrt(0.04 + 0.64 + 0.01), 1e-6),
        "... and is minus c16 normalised");

  // What is not a camera.
  Bank no_view = bank;
  for (uint32_t r = 0; r < 4; ++r) {
    no_view.Set(fr::kViewInverseRegister + r, 0, 0, 0, 0);
  }
  Check(!fr::CameraFromBank(no_view.bytes, world, wvp).valid, "a bank with no view inverse (zero registers) gives no camera");
  Bank not_rigid = bank;
  not_rigid.Set(fr::kViewInverseRegister + 3, 10.0f, 2.0f, -5.0f, 0.5f);  // w of the position row is not 1
  Check(!fr::CameraFromBank(not_rigid.bytes, world, wvp).valid, "a view inverse whose position row has w != 1 is no camera");
  Check(!fr::CameraFromBank(bank.bytes, tp::Mat4{}, wvp).valid, "a gWorld that cannot be inverted gives no camera");
  tp::Mat4 nan_wvp = wvp;
  nan_wvp.m[1][1] = std::numeric_limits<double>::quiet_NaN();
  Check(!fr::CameraFromBank(bank.bytes, world, nan_wvp).valid, "a gWorldViewProj with a NaN gives no camera");
  Bank no_sun = bank;
  no_sun.Set(fr::kSunRegister, 0, 0, 0, 0);
  const fr::BankCamera dark = fr::CameraFromBank(no_sun.bytes, world, wvp);
  Check(dark.valid && !dark.sun_valid && dark.to_sun[0] == 0.0 && dark.to_sun[1] == 0.0 && dark.to_sun[2] == 0.0, "a zero light direction leaves the sun empty, the camera stays");
  Check(fr::LooksLikeViewInverse(view_inverse) && !fr::LooksLikeViewInverse(tp::Mat4{}), "LooksLikeViewInverse tells a camera from zeros");
}

// ---------------------------------------------------------------------------
// normals
// ---------------------------------------------------------------------------

void TestNormals() {
  Check(fr::NormalFormatOf(0x002A23B9) == fr::NormalFormat::kFloat3 && fr::NormalFormatOf(0x001A23A6) == fr::NormalFormat::kFloat4 &&
            fr::NormalFormatOf(0x001A2360) == fr::NormalFormat::kHalf4 && fr::NormalFormatOf(0x001A2187) == fr::NormalFormat::kPacked &&
            fr::NormalFormatOf(0x00182886) == fr::NormalFormat::kNone,
        "the NORMAL formats the declarations give");
  Check(fr::NormalBytes(fr::NormalFormat::kFloat3) == 12 && fr::NormalBytes(fr::NormalFormat::kFloat4) == 16 && fr::NormalBytes(fr::NormalFormat::kHalf4) == 8 &&
            fr::NormalBytes(fr::NormalFormat::kPacked) == 4 && fr::NormalBytes(fr::NormalFormat::kNone) == 0,
        "their sizes");

  float n[3];
  uint8_t f3[16] = {};
  BigEndianFloat(0.0f, f3);
  BigEndianFloat(1.0f, f3 + 4);
  BigEndianFloat(-0.5f, f3 + 8);
  BigEndianFloat(9.0f, f3 + 12);  // the w of a float4: ignored
  fr::DecodeNormal(f3, fr::NormalFormat::kFloat3, n);
  Check(n[0] == 0.0f && n[1] == 1.0f && n[2] == -0.5f, "three big-endian floats");
  fr::DecodeNormal(f3, fr::NormalFormat::kFloat4, n);
  Check(n[0] == 0.0f && n[1] == 1.0f && n[2] == -0.5f, "four floats: x y z, the w is left");
  const uint8_t halves[8] = {0x3C, 0x00, 0x00, 0x00, 0xBC, 0x00, 0x00, 0x00};  // 1, 0, -1, 0
  fr::DecodeNormal(halves, fr::NormalFormat::kHalf4, n);
  Check(n[0] == 1.0f && n[1] == 0.0f && n[2] == -1.0f, "four big-endian half floats");

  // The packed dword: three signed 10-bit fields at bits 0, 10 and 20 over 511.
  const auto packed = [&](int x, int y, int z, float out[3]) {
    const uint32_t v = (uint32_t(x) & 0x3FFu) | ((uint32_t(y) & 0x3FFu) << 10) | ((uint32_t(z) & 0x3FFu) << 20) | (1u << 30);
    uint8_t bytes[4];
    BigEndian(v, bytes);
    fr::DecodeNormal(bytes, fr::NormalFormat::kPacked, out);
  };
  packed(511, -511, 0, n);
  Check(Near(n[0], 1.0, 1e-6) && Near(n[1], -1.0, 1e-6) && n[2] == 0.0f, "packed: +511 is +1, -511 is -1, 0 is 0");
  packed(-1, 255, -256, n);
  Check(Near(n[0], -1.0 / 511.0, 1e-6) && Near(n[1], 255.0 / 511.0, 1e-6) && Near(n[2], -256.0 / 511.0, 1e-6), "packed: the sign comes from bit 9 of each field");
  packed(-512, 0, 1, n);
  Check(Near(n[0], -512.0 / 511.0, 1e-6) && n[1] == 0.0f && Near(n[2], 1.0 / 511.0, 1e-6), "packed: -512 is just below -1 (the unclamped two's complement)");
  fr::DecodeNormal(f3, fr::NormalFormat::kNone, n);
  Check(n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f, "no format: zeros");

  // The blend of a normal: the rotation of each bone, no translation. Bone 2: a quarter turn about z with an offset; bone 5: identity with an
  // offset.
  auto palette = std::make_unique<cd::SkinPalette>();
  std::memset(palette.get(), 0, sizeof(cd::SkinPalette));
  const float bone2[3][4] = {{0, -1, 0, 5}, {1, 0, 0, 7}, {0, 0, 1, 9}};
  const float bone5[3][4] = {{1, 0, 0, 100}, {0, 1, 0, 200}, {0, 0, 1, 300}};
  std::memcpy(palette->rows[2], bone2, sizeof(bone2));
  std::memcpy(palette->rows[5], bone5, sizeof(bone5));
  cd::SkinInfluence skin;
  skin.bone[0] = 2;
  skin.weight[0] = 0.25f;
  skin.bone[1] = 5;
  skin.weight[1] = 0.75f;
  const float in[3] = {1.0f, 0.0f, 0.0f};
  float out[3];
  fr::SkinBlendDirection(*palette, skin, in, out);
  // bone 2 turns (1,0,0) into (0,1,0); bone 5 leaves it: 0.25 (0,1,0) + 0.75 (1,0,0)
  Check(Near(out[0], 0.75, 1e-6) && Near(out[1], 0.25, 1e-6) && Near(out[2], 0.0, 1e-6), "a skinned direction is the weighted sum of the bones' rotations, without their translations");
  skin.bone[1] = cd::kNoBone;
  fr::SkinBlendDirection(*palette, skin, in, out);
  Check(Near(out[0], 0.0, 1e-6) && Near(out[1], 0.25, 1e-6), "an influence with no bone adds nothing");
}

// ---------------------------------------------------------------------------
// boxes
// ---------------------------------------------------------------------------

void TestBoxes() {
  // Four vertices; the triangle names the first three, the fourth (far away) belongs to another mesh's range of the same stream.
  const float positions[12] = {-1, 0, 2, 3, 1, 0, 0, 5, -2, 1000, 1000, 1000};
  const uint32_t tri[3] = {0, 1, 2};
  float lo[3], hi[3];
  Check(fr::BoxOfMesh(positions, 4, tri, 3, lo, hi), "a box of a triangle");
  Check(lo[0] == -1.0f && lo[1] == 0.0f && lo[2] == -2.0f && hi[0] == 3.0f && hi[1] == 5.0f && hi[2] == 2.0f, "the box holds the vertices the triangles name, not the others");
  const uint32_t bad[3] = {0, 1, 9};
  Check(fr::BoxOfMesh(positions, 4, bad, 3, lo, hi) && hi[0] == 3.0f, "a vertex number past the stream is left out");
  const uint32_t none[2] = {7, 8};
  Check(!fr::BoxOfMesh(positions, 4, none, 2, lo, hi), "no vertex named: no box");
  float nan_positions[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
  const uint32_t one[1] = {0};
  Check(!fr::BoxOfMesh(nan_positions, 1, one, 1, lo, hi), "a vertex that is not a number is no box");

  // Through a matrix: a translation, then a quarter turn about y.
  fr::Box3 box;
  Check(box.Empty(), "a new box is empty");
  const float mlo[3] = {0, 0, 0};
  const float mhi[3] = {1, 2, 3};
  tp::Mat4 move = tp::Identity();
  move.m[3][0] = 10;
  move.m[3][1] = 20;
  move.m[3][2] = 30;
  fr::AddTransformedBox(&box, move, mlo, mhi);
  Check(!box.Empty() && box.lo[0] == 10 && box.lo[1] == 20 && box.lo[2] == 30 && box.hi[0] == 11 && box.hi[1] == 22 && box.hi[2] == 33, "a translated box");
  fr::Box3 turned;
  tp::Mat4 turn = tp::Identity();  // x goes to -z, z goes to +x (row vectors)
  turn.m[0][0] = 0;
  turn.m[0][2] = -1;
  turn.m[2][0] = 1;
  turn.m[2][2] = 0;
  fr::AddTransformedBox(&turned, turn, mlo, mhi);
  Check(Near(turned.lo[0], 0) && Near(turned.hi[0], 3) && Near(turned.lo[1], 0) && Near(turned.hi[1], 2) && Near(turned.lo[2], -1) && Near(turned.hi[2], 0),
        "a turned box is the box of its eight corners");
  fr::AddTransformedBox(&turned, move, mlo, mhi);
  Check(turned.hi[0] == 11 && turned.hi[2] == 33 && turned.lo[0] == 0, "boxes add up");
  fr::Box3 holes;
  tp::Mat4 broken = tp::Identity();
  broken.m[3][1] = std::numeric_limits<double>::quiet_NaN();
  fr::AddTransformedBox(&holes, broken, mlo, mhi);
  Check(holes.Empty(), "corners that are not numbers add nothing");
}


// ---------------------------------------------------------------------------
// the mesh's own bytes: the fingerprints that replace the geometry cache's stamp in the key
// ---------------------------------------------------------------------------

// Deterministic bytes (an xorshift), so that a failure reproduces.
std::vector<uint8_t> RandomBytes(size_t n, uint64_t seed) {
  std::vector<uint8_t> out(n);
  uint64_t x = seed * 0x9E3779B97F4A7C15ull + 12345u;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    out[i] = uint8_t(x >> 24);
  }
  return out;
}

// Host index numbers as the little-endian bytes a draw with endian "none" holds (the swap is then a copy).
std::vector<uint8_t> LittleEndianBytes(const std::vector<uint32_t>& values, bool index32) {
  std::vector<uint8_t> out;
  for (uint32_t v : values) {
    for (uint32_t b = 0; b < (index32 ? 4u : 2u); ++b) {
      out.push_back(uint8_t(v >> (8 * b)));
    }
  }
  return out;
}

void TestIndexSummary() {
  // The summary reads indices as the renderer does (car_dump::DecodeIndices): for every endian, width and count (a block's edge, an odd
  // tail) the same indices in the little-endian layout give the same lo, hi and fingerprint, ordered or not.
  bool all_agree = true;
  for (uint32_t endian = 0; endian < 4; ++endian) {
    for (int wide = 0; wide < 2; ++wide) {
      for (uint32_t count : {0u, 1u, 2u, 3u, 5u, 95u, 96u, 97u, 191u, 192u, 193u, 300u, 301u}) {
        const uint32_t size = wide ? 4u : 2u;
        const std::vector<uint8_t> raw = RandomBytes(size_t(count) * size, endian * 1000 + wide * 100 + count);
        std::vector<uint32_t> decoded;
        cd::DecodeIndices(raw.data(), count, wide != 0, endian, &decoded);
        const std::vector<uint8_t> flat = LittleEndianBytes(decoded, wide != 0);
        for (int ordered = 0; ordered < 2; ++ordered) {
          const fr::IndexSummary a = fr::SummarizeIndices(raw.data(), count, wide != 0, endian, ordered == 0, false, 0);
          const fr::IndexSummary b = fr::SummarizeIndices(flat.data(), count, wide != 0, 0, ordered == 0, false, 0);
          bool ok = a.fingerprint == b.fingerprint && a.any == b.any && a.lo == b.lo && a.hi == b.hi && a.fingerprint != 0;
          if (count != 0) {
            ok = ok && a.any && a.lo == *std::min_element(decoded.begin(), decoded.end()) &&
                 a.hi == *std::max_element(decoded.begin(), decoded.end());
          } else {
            ok = ok && !a.any;
          }
          all_agree &= ok;
        }
      }
    }
  }
  Check(all_agree, "the index summary reads every endian and width as car_dump's DecodeIndices does");

  // A triangle list: the order of the triangles does not matter, anything else does.
  const std::vector<uint32_t> list = {0, 1, 2, 2, 1, 3, 3, 1, 4, 5, 6, 7, 7, 6, 8, 9, 10, 11};
  const auto summary = [&](const std::vector<uint32_t>& v, bool order_free) {
    const std::vector<uint8_t> bytes = LittleEndianBytes(v, false);
    return fr::SummarizeIndices(bytes.data(), uint32_t(v.size()), false, 0, order_free, false, 0);
  };
  const fr::IndexSummary base = summary(list, true);
  Check(base.any && base.lo == 0 && base.hi == 11, "the smallest and largest index of a list");
  std::vector<uint32_t> shuffled;  // the same six triangles, drawn in another order
  const int order[6] = {4, 0, 5, 2, 1, 3};
  for (int t : order) {
    shuffled.insert(shuffled.end(), list.begin() + t * 3, list.begin() + t * 3 + 3);
  }
  Check(summary(shuffled, true).fingerprint == base.fingerprint, "the same triangles in another order give the same list fingerprint");
  Check(summary(shuffled, false).fingerprint != summary(list, false).fingerprint, "an ordered (strip) fingerprint follows the order");
  std::vector<uint32_t> edited = list;
  edited[7] = 4;  // one corner of one triangle
  Check(summary(edited, true).fingerprint != base.fingerprint, "another corner of one triangle changes the list fingerprint");
  std::vector<uint32_t> wound = list;
  std::swap(wound[1], wound[2]);  // the first triangle's winding turned over
  Check(summary(wound, true).fingerprint != base.fingerprint, "a triangle's winding is part of the list fingerprint");
  std::vector<uint32_t> doubled = list;
  doubled.insert(doubled.end(), list.begin(), list.begin() + 3);
  Check(summary(doubled, true).fingerprint != base.fingerprint, "a triangle drawn twice is not the same mesh");
  std::vector<uint32_t> shorter(list.begin(), list.end() - 3);
  Check(summary(shorter, true).fingerprint != base.fingerprint, "a triangle fewer is not the same mesh");
  Check(summary(list, true).fingerprint != summary(list, false).fingerprint, "a list and a strip over the same indices are told apart");

  // A strip's primitive reset is a cut, not a vertex.
  const std::vector<uint32_t> strip = {3, 4, 5, 0xFFFF, 8, 9, 10};
  const std::vector<uint8_t> strip_bytes = LittleEndianBytes(strip, false);
  const fr::IndexSummary cut = fr::SummarizeIndices(strip_bytes.data(), 7, false, 0, false, true, 0xFFFF);
  Check(cut.any && cut.lo == 3 && cut.hi == 10, "a reset index is left out of the smallest and largest index");
  const fr::IndexSummary literal = fr::SummarizeIndices(strip_bytes.data(), 7, false, 0, false, false, 0xFFFF);
  Check(literal.hi == 0xFFFF && literal.fingerprint == cut.fingerprint, "with the reset off the same index is a vertex, and the fingerprint is of the same bytes");
  const std::vector<uint32_t> only_cuts = {0xFFFF, 0xFFFF, 0xFFFF};
  const std::vector<uint8_t> cut_bytes = LittleEndianBytes(only_cuts, false);
  Check(!fr::SummarizeIndices(cut_bytes.data(), 3, false, 0, false, true, 0xFFFF).any, "indices that are all resets name no vertex");
  Check(fr::SummarizeIndices(cut_bytes.data(), 3, false, 0, false, true, 0xFFFF).fingerprint != 0, "and still have a fingerprint");

  // A long list (several blocks): any one index changed is seen, wherever it is.
  std::vector<uint32_t> long_list;
  for (uint32_t t = 0; t < 700; ++t) {
    long_list.push_back(t);
    long_list.push_back(t + 1);
    long_list.push_back(t + 2);
  }
  const fr::IndexSummary long_base = summary(long_list, true);
  bool every_edit_seen = true;
  for (size_t at : {size_t(0), size_t(95), size_t(96), size_t(97), size_t(1500), size_t(2099)}) {
    std::vector<uint32_t> e = long_list;
    e[at] += 1000;
    every_edit_seen &= summary(e, true).fingerprint != long_base.fingerprint;
  }
  Check(every_edit_seen, "an index changed in any block of a long list changes the fingerprint");
}

void TestOwnVertices() {
  // An indexed draw: the vertices its indices name plus the base vertex.
  fr::VertexSpan s = fr::OwnVertices(true, 0, 3000, 100, true, 4, 250, 1000);
  Check(!s.whole && s.first == 104 && s.count == 247, "an indexed draw uses lo + base .. hi + base");
  s = fr::OwnVertices(true, 0, 3000, -4, true, 4, 250, 1000);
  Check(!s.whole && s.first == 0 && s.count == 247, "a negative base vertex ends at the window's first vertex");
  s = fr::OwnVertices(true, 0, 3000, 0, true, 10, 5000, 1000);
  Check(!s.whole && s.first == 10 && s.count == 990, "the span is clipped to the window");
  s = fr::OwnVertices(true, 0, 3000, 2000, true, 0, 10, 1000);
  Check(!s.whole && s.count == 0, "a span entirely past the window holds nothing");
  s = fr::OwnVertices(true, 0, 3000, 0, false, 0, 0, 1000);
  Check(s.whole, "an indexed draw whose indices are not known uses the whole window");
  // An auto draw: start .. start + count - 1.
  s = fr::OwnVertices(false, 40, 36, 0, false, 0, 0, 1000);
  Check(!s.whole && s.first == 40 && s.count == 36, "an auto draw uses its own run of vertices");
  s = fr::OwnVertices(false, 990, 36, 0, false, 0, 0, 1000);
  Check(!s.whole && s.first == 990 && s.count == 10, "an auto draw is clipped to the window");
  s = fr::OwnVertices(false, 0, 0, 0, false, 0, 0, 1000);
  Check(s.whole, "an auto draw with no vertices uses the whole window");

  // Other spans of the same bytes are other fingerprints and other slots.
  fr::VertexSpan a, b;
  a.whole = b.whole = false;
  a.first = 10;
  a.count = 20;
  b.first = 10;
  b.count = 21;
  Check(fr::VertexContent(77, a) != fr::VertexContent(77, b) && fr::VertexContent(77, a) != fr::VertexContent(78, a) &&
            fr::VertexContent(0, a) != 0,
        "the vertex fingerprint follows the bytes and the span, and is never 0");
  Check(fr::VertexSlot(0x100, 4096, 28, a) != fr::VertexSlot(0x100, 4096, 28, b) &&
            fr::VertexSlot(0x100, 4096, 28, a) != fr::VertexSlot(0x104, 4096, 28, a) &&
            fr::VertexSlot(0x100, 4096, 28, a) != fr::VertexSlot(0x100, 4096, 32, a),
        "the slot of a fingerprint follows the window, the stride and the span");
  Check(fr::IndexSlot(0x200, 600, false, 2, true, false, 0) != fr::IndexSlot(0x200, 600, false, 2, false, false, 0) &&
            fr::IndexSlot(0x200, 600, false, 2, true, false, 0) != fr::IndexSlot(0x200, 600, false, 1, true, false, 0) &&
            fr::IndexSlot(0x200, 600, false, 2, true, false, 0) != fr::IndexSlot(0x200, 600, true, 2, true, false, 0) &&
            fr::IndexSlot(0x200, 600, false, 2, false, true, 0xFFFF) != fr::IndexSlot(0x200, 600, false, 2, false, true, 0xFFFE),
        "the slot of an index fingerprint follows the range and how it is read");
}

void TestContentCache() {
  using Outcome = fr::ContentCache::Outcome;
  fr::ContentCache cache;
  int computed = 0;
  uint64_t bytes_say = 100;  // what the guest's bytes would fingerprint to
  const auto get = [&](uint64_t slot, uint64_t stamp, uint64_t frame, Outcome* outcome) {
    return cache.Get(slot, stamp, frame,
                     [&](fr::ContentEntry* e) {
                       ++computed;
                       e->content = bytes_say;
                       e->has_range = true;
                       e->lo = 3;
                       e->hi = 9;
                     },
                     outcome);
  };
  Outcome o = Outcome::kHit;
  fr::ContentEntry e = get(1, 50, 10, &o);
  Check(o == Outcome::kNew && computed == 1 && e.content == 100 && e.has_range && e.lo == 3 && e.hi == 9 && e.stamp == 50, "a new slot is made");
  e = get(1, 50, 11, &o);
  Check(o == Outcome::kHit && computed == 1 && e.content == 100 && e.seen == 11, "the same stamp reads nothing");
  for (uint64_t frame = 12; frame < 20; ++frame) {
    get(1, 50, frame, nullptr);
  }
  Check(computed == 1, "a parked mesh costs one lookup a draw and no reading");
  e = get(1, 51, 20, &o);
  Check(o == Outcome::kSame && computed == 2 && e.content == 100 && e.stamp == 51, "a stamp that moved over the same bytes gives the same fingerprint");
  e = get(1, 51, 21, &o);
  Check(o == Outcome::kHit && computed == 2, "and is then current again");
  bytes_say = 200;
  e = get(1, 52, 22, &o);
  Check(o == Outcome::kChanged && computed == 3 && e.content == 200, "a stamp that moved over other bytes gives another fingerprint");
  get(2, 52, 22, &o);
  Check(o == Outcome::kNew && computed == 4 && cache.size() == 2, "another slot is another entry");
  // Fresh says whether a Get would hit, without making anything.
  Check(cache.Fresh(1, 52) && cache.Fresh(2, 52) && !cache.Fresh(1, 53) && !cache.Fresh(3, 52) && computed == 4 && cache.size() == 2,
        "Fresh: a slot at its stamp is current, a slot at another stamp or a slot never made is not, and nothing is made by asking");
  get(1, 53, 23, &o);
  Check(cache.Fresh(1, 53) && !cache.Fresh(1, 52), "after a Get at a new stamp the new stamp is the fresh one");

  // Sweeping: an entry not asked for in `keep` frames goes.
  cache.Sweep(30, 100);
  Check(cache.size() == 2, "entries seen lately stay");
  cache.Sweep(1000, 100);
  Check(cache.size() == 0, "entries not seen for long are dropped");
  get(1, 52, 1001, &o);
  Check(o == Outcome::kNew, "a dropped slot is made again");
  cache.Clear();
  Check(cache.size() == 0, "Clear empties it");
}

// A mesh and the guest memory around it, to see which writes the key notices. The key is made as car_frame.cpp's ComputeMeshKey makes it
// (a window of vertices, the draw's own indices, the base vertex), with the geometry cache's stamp as numbers the test moves by hand.
struct FakeGuest {
  std::vector<uint8_t> vertices;  // the stream's whole fetch window
  std::vector<uint8_t> indices;   // the draw's own index bytes, little endian (endian none)
  uint32_t stride = 28;
  int32_t base_vertex = 100;      // the draw's vertices are numbered from here in the window
  uint32_t index_count = 0;
  uint64_t vertex_stamp = 1;
  uint64_t index_stamp = 1;
  fr::ContentCache cache;
  int reads = 0;                  // times guest bytes were read

  uint64_t Key(fr::ContentCache::Outcome* worst = nullptr) {
    using Outcome = fr::ContentCache::Outcome;
    fr::MeshKeyParts p;
    p.position = fr::StreamKey{0x01000000u, uint32_t(vertices.size()), stride, 0, 5, 0};
    p.indexed = 1;
    p.primitive = 4;
    p.count = index_count;
    p.base_vertex = base_vertex;
    p.index_address = 0x02000000u;
    p.index_bytes = index_count * 2;
    p.endian = 0;
    const uint64_t index_slot = fr::IndexSlot(p.index_address, p.index_bytes, false, 0, true, false, 0);
    Outcome io = Outcome::kHit, vo = Outcome::kHit;
    const fr::ContentEntry index = cache.Get(index_slot, index_stamp, 1,
                                             [&](fr::ContentEntry* e) {
                                               ++reads;
                                               const fr::IndexSummary s = fr::SummarizeIndices(indices.data(), index_count, false, 0, true, false, 0);
                                               e->content = s.fingerprint;
                                               e->has_range = s.any;
                                               e->lo = s.lo;
                                               e->hi = s.hi;
                                             },
                                             &io);
    p.index_content = index.content;
    const fr::VertexSpan span =
        fr::OwnVertices(true, 0, index_count, base_vertex, index.has_range, index.lo, index.hi, uint32_t(vertices.size()) / stride);
    const uint64_t slot = fr::VertexSlot(p.position.address, p.position.size, stride, span);
    const fr::ContentEntry vertex = cache.Get(slot, vertex_stamp, 1,
                                              [&](fr::ContentEntry* e) {
                                                ++reads;
                                                const uint32_t offset = span.whole ? 0 : span.first * stride;
                                                const uint32_t length = span.whole ? uint32_t(vertices.size()) : span.count * stride;
                                                uint64_t h = 1469598103934665603ull;  // any byte hash will do for the test
                                                for (uint32_t i = 0; i < length; ++i) {
                                                  h = (h ^ vertices[offset + i]) * 1099511628211ull;
                                                }
                                                e->content = fr::VertexContent(h, span);
                                              },
                                              &vo);
    p.position.content = vertex.content;
    if (worst) {
      *worst = (io == Outcome::kChanged || vo == Outcome::kChanged) ? Outcome::kChanged
               : (io == Outcome::kSame || vo == Outcome::kSame)     ? Outcome::kSame
               : (io == Outcome::kNew || vo == Outcome::kNew)       ? Outcome::kNew
                                                                    : Outcome::kHit;
    }
    return fr::MeshKeyOf(p);
  }
};

void TestKeyIsStableForUnchangedTriangles() {
  using Outcome = fr::ContentCache::Outcome;
  FakeGuest g;
  g.vertices = RandomBytes(28 * 2000, 5);  // a window of 2000 vertices; the mesh uses 100 + (10 .. 59)
  std::vector<uint32_t> list;
  for (uint32_t t = 0; t < 48; ++t) {
    list.push_back(10 + t);
    list.push_back(10 + t + 1);
    list.push_back(10 + t + 2);
  }
  g.indices = LittleEndianBytes(list, false);
  g.index_count = uint32_t(list.size());
  Outcome o = Outcome::kHit;
  const uint64_t key = g.Key(&o);
  Check(key != 0 && o == Outcome::kNew, "the first key makes the fingerprints");
  const int reads_after_first = g.reads;
  Check(reads_after_first == 2, "the first key reads the indices once and the vertices once");

  // The key of a parked mesh costs no reading.
  for (int i = 0; i < 10; ++i) {
    Check(g.Key(&o) == key && o == Outcome::kHit, "a parked mesh keeps its key");
  }
  Check(g.reads == reads_after_first, "and nothing is read for it");

  // The stamp moves with no change in the bytes (a neighbour in the page was written, a watched/hashed switch, a region made again).
  g.vertex_stamp += 7;
  g.index_stamp += 3;
  Check(g.Key(&o) == key && o == Outcome::kSame, "a stamp that moved over the same bytes keeps the key");
  Check(g.reads == reads_after_first + 2, "and the bytes were read once to tell");
  Check(g.Key(&o) == key && o == Outcome::kHit && g.reads == reads_after_first + 2, "after that nothing is read again");

  // Bytes of the window that are not the mesh's own change (another mesh's vertices in the same window): the key stays.
  g.vertices[28 * 5 + 3] ^= 0xFF;     // vertex 5: before the mesh's first vertex (110)
  g.vertices[28 * 1500 + 9] ^= 0x55;  // vertex 1500: after its last (159)
  g.vertex_stamp += 1;
  Check(g.Key(&o) == key && o == Outcome::kSame, "a write in the window outside the mesh's own vertices keeps the key");

  // A vertex of its own changes (a morph, a mesh streamed in again over the same address): the key moves.
  g.vertices[28 * 120 + 4] ^= 0x01;
  g.vertex_stamp += 1;
  const uint64_t moved = g.Key(&o);
  Check(moved != key && o == Outcome::kChanged, "a change in the mesh's own vertices is a new key");
  g.vertices[28 * 120 + 4] ^= 0x01;  // and back: the same bytes are the same key again
  g.vertex_stamp += 1;
  Check(g.Key(&o) == key, "the same bytes again are the old key (a mesh that changes back is not a new mesh)");

  // The triangles are sorted (a translucent part drawn back to front): the same triangles, the key stays.
  std::vector<uint32_t> sorted;
  for (int t = 47; t >= 0; --t) {
    sorted.insert(sorted.end(), list.begin() + t * 3, list.begin() + t * 3 + 3);
  }
  g.indices = LittleEndianBytes(sorted, false);
  g.index_stamp += 1;
  Check(g.Key(&o) == key && o == Outcome::kSame, "the same triangles in another order keep the key");

  // One triangle changes: the key moves.
  sorted[4] = 59;
  g.indices = LittleEndianBytes(sorted, false);
  g.index_stamp += 1;
  Check(g.Key(&o) != key && o == Outcome::kChanged, "another triangle is a new key");

  // The base vertex is the draw's, not the bytes': it is in the key's parts.
  FakeGuest h = g;
  h.indices = LittleEndianBytes(list, false);
  h.index_stamp += 1;
  h.vertex_stamp += 1;
  const uint64_t same_again = h.Key(&o);
  h.base_vertex = 101;
  Check(h.Key(&o) != same_again, "another base vertex is another mesh");
}

}  // namespace

int main() {
  TestMeshKey();
  TestIdentity();
  TestDecodeModel();
  TestCamera();
  TestNormals();
  TestBoxes();
  TestIndexSummary();
  TestOwnVertices();
  TestContentCache();
  TestKeyIsStableForUnchangedTriangles();
  std::printf("car_frame_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
