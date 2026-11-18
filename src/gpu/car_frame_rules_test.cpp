// Check of the car frame's rules (car_frame_rules.h): the mesh key, which draw is the player's (the identity tracker), the matrix of a draw
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
  p.index_stamp = 555u;
  p.reset = 0;
  p.reset_index = 0;
  return p;
}

void TestMeshKey() {
  const fr::MeshKeyParts base = BaseParts();
  const uint64_t key = fr::MeshKeyOf(base);
  Check(key != 0, "a mesh key is never 0");
  Check(fr::MeshKeyOf(BaseParts()) == key, "the same parts make the same key");

  // Every part of a mesh's identity changes the key (a mesh streamed in again at the same address changes the stamps).
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
  add([](fr::MeshKeyParts& p) { p.position.stamp += 1; });  // the vertex bytes were written again
  add([](fr::MeshKeyParts& p) { p.normal.address = 0x03000000u; });
  add([](fr::MeshKeyParts& p) { p.normal.size = 4096; });
  add([](fr::MeshKeyParts& p) { p.normal.stride = 12; });
  add([](fr::MeshKeyParts& p) { p.normal.offset += 4; });
  add([](fr::MeshKeyParts& p) { p.normal.format += 1; });
  add([](fr::MeshKeyParts& p) { p.normal.stamp = 9; });
  add([](fr::MeshKeyParts& p) { p.indexed = 0; });
  add([](fr::MeshKeyParts& p) { p.primitive = 6; });
  add([](fr::MeshKeyParts& p) { p.count += 3; });
  add([](fr::MeshKeyParts& p) { p.start += 3; });
  add([](fr::MeshKeyParts& p) { p.base_vertex = -5; });
  add([](fr::MeshKeyParts& p) { p.index_address += 2; });
  add([](fr::MeshKeyParts& p) { p.index_bytes += 2; });
  add([](fr::MeshKeyParts& p) { p.index32 = 1; });
  add([](fr::MeshKeyParts& p) { p.endian = 1; });
  add([](fr::MeshKeyParts& p) { p.index_stamp += 1; });  // the index bytes were written again
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

}  // namespace

int main() {
  TestMeshKey();
  TestIdentity();
  TestDecodeModel();
  TestCamera();
  TestNormals();
  TestBoxes();
  std::printf("car_frame_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
