// Check of the ray traced visibility of the top K lights (RT-LIGHTS-D1 part 1): rt_lights_rules.h, hlsl/rt_lights_common.hlsli, hlsl/rt_mask_read.hlsli, the trace shader's text and, with the src
// folder, the host class, the settings and the wiring. A program of its own: no renderer, no game files, no GPU.
//
//   clang++ -std=c++23 -ffp-contract=off -I src -I src/gpu src/gpu/rt_lights_test.cpp -o rt_lights_test
//   rt_lights_test [<src dir>]        (exit 0 = passed)
//
// It compiles the SHADER FILES THEMSELVES as C++ (hlsl_cpp_shim.h) and checks them against the twins and against brute force:
//   - the channels: image x 3 + component, every channel of the nine names one component of one image, the light's flag and mask channel survive the GPU packing;
//   - the settings' words (auto / off / on, K, rays, view, check, radius) and the reasons the pass does not run, in the order a frame finds them;
//   - the top K: the class order (headlight, neon, lamp, other), the score within a class, the keep bonus, stable ties, eligibility, no duplicate, the cap, any input order gives the same pick;
//   - the channels' owners: a light keeps its channel while it is picked, the others take the lowest free one, K going down lets go, the groups whose owners changed are named;
//   - the rows (64 bytes a light, a pair, a cone, the radius by class) and the table the light pass reads;
//   - the COMPOSITION (the rule of the whole stage): a visibility is a number in [0, 1], the atlas and the ray multiply, a light the game lights is only ever TAKEN AWAY from (the share is in
//     [0, term] whatever the visibility: zero negative pixels, NaN and infinity lit), a light we own is multiplied, and with V = 1 (RT off, no channel, or every ray free) both are bit-identical to the
//     light without it; the pixel model never goes negative where the pass's term is the grid's;
//   - the ray's geometry (hlsl/rt_lights_common.hlsli): the world to view move, the reach tests against an independent formula, the ray's origin above the surface and its end short of the light,
//     the disc's points and strata, the pair's lamp;
//   - the NOISE (AGENT-RULE-NOISE): the numbers of a light's ray change every frame at every pixel, leave no gap, are uniform, take the same step at every pixel, and the error after an exponential
//     average has no lattice; no two (light, slot) of a frame share a sample;
//   - the mask read (hlsl/rt_mask_read.hlsli): the same size reads the texel, a smaller one a depth guided bilinear against a brute force, an edge in depth keeps the far side out, a bad texel is lit;
//     (VERIFY-RTL: taps of one value, an all lit neighbourhood above all, read back as exactly that value, and a mean never leaves the range of its taps);
//   - the shader's text (the numbers against the header's, no colour target, the shared noise) and, with the src folder, the host class, the settings and the wiring.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "hlsl_cpp_shim.h"
#include "noise_resolve_check.h"
#include "rt_denoise_rules.h"
#include "rt_lights_rules.h"
#include "light_convert.h"
#include "light_laws.h"
#include "local_lights_rules.h"

namespace rl = larecomp::gpu::rt_lights;
namespace lg = larecomp::gpu;
namespace nc = noise_check;

// ---------------------------------------------------------------------------
// the shader files as C++
// ---------------------------------------------------------------------------
namespace hlsl_shim {
inline uint asuint(float v) { return std::bit_cast<uint>(v); }
// the mask read's seams: fake images the test fills
struct FakeMask {
  int w = 0, h = 0;
  std::vector<float4> texel;
};
inline std::vector<FakeMask>& Masks() {
  static std::vector<FakeMask> m;
  return m;
}
inline std::vector<std::vector<float>>& Depths() {
  static std::vector<std::vector<float>> d;
  return d;
}
inline int& DepthWidth() {
  static int w = 0;
  return w;
}
#include "hlsl/blue_noise.hlsli"
#include "hlsl/blue_noise_frame.hlsli"
#include "hlsl/rt_lights_common.hlsli"
#include "hlsl/rt_mask_read.hlsli"
#include "hlsl/rt_denoise_unorm8.hlsli"
float4 RtmLoadMask(uint slot, int2 p) {
  const FakeMask& m = Masks()[slot];
  return m.texel[size_t(p.y) * size_t(m.w) + size_t(p.x)];
}
float RtmLoadDepth(uint slot, int2 p) { return Depths()[slot][size_t(p.y) * size_t(DepthWidth()) + size_t(p.x)]; }
}  // namespace hlsl_shim
namespace hs = hlsl_shim;

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
  uint32_t state = 20261122u;
  uint32_t U32() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float F() { return float(U32() >> 8) * (1.0f / 16777216.0f); }
  float Range(float lo, float hi) { return lo + (hi - lo) * F(); }
  uint32_t Below(uint32_t n) { return U32() % n; }
};

lg::Light Lamp(float x, float y, float z, uint64_t id, float intensity = 60.0f) {
  const float pos[3] = {x, y, z};
  const float colour[3] = {1.0f, 0.9f, 0.7f};
  const float dir[3] = {0.0f, -1.0f, 0.0f};
  return lg::MakeGameLampLight(pos, colour, dir, intensity, 125.0f, -50.5f, 1.0f, 1, lg::LightSource::kLamp, id);
}
lg::Light Headlight(float x, float y, float z, uint64_t id, float level = 1.0f) {
  lg::Light l;
  l.pos[0] = x;
  l.pos[1] = y;
  l.pos[2] = z;
  l.dir[0] = 0.0f;
  l.dir[1] = 0.0f;
  l.dir[2] = 1.0f;
  l.cos_outer = 0.8f;
  l.cos_inner = 0.95f;
  l.range = 70.0f;
  l.color[0] = l.color[1] = l.color[2] = 2.0f * level;
  l.law_a = 26.0f;
  l.kind = lg::LightKind::kSpot;
  l.source = lg::LightSource::kHeadlight;
  l.law = lg::LightLaw::kHeadlight;
  l.roles = lg::kRoleMaskAll;
  l.offset[0] = 0.6f;
  l.id = id;
  return l;
}

// ---------------------------------------------------------------------------
// the channels
// ---------------------------------------------------------------------------
void TestChannels() {
  CHECK(rl::kChannelsPerImage == 3 && rl::kMaxImages == 3 && rl::kMaxLights == 9);
  std::set<std::pair<uint32_t, uint32_t>> seen;
  for (uint32_t c = 0; c < rl::kMaxLights; ++c) {
    CHECK(rl::ValidChannel(c));
    CHECK(rl::ChannelOf(rl::ImageOf(c), rl::ComponentOf(c)) == c);
    CHECK(rl::ImageOf(c) < rl::kMaxImages && rl::ComponentOf(c) < 3);
    CHECK(seen.insert({rl::ImageOf(c), rl::ComponentOf(c)}).second);
    CHECK(rl::GroupBit(c) == (1u << rl::ImageOf(c)));
  }
  CHECK(!rl::ValidChannel(9) && !rl::ValidChannel(rl::kNoChannel));
  for (uint32_t k = 0; k <= 12; ++k) {
    const uint32_t images = rl::ImagesFor(k);
    CHECK(images * 3 >= std::min(k, 9u) && (images == 0 || (images - 1) * 3 < std::min(k, 9u)));
  }
  // the light's flag and mask channel survive the GPU packing, and the atlas's kind and views beside them are untouched
  for (uint32_t c = 0; c < rl::kMaxLights; ++c) {
    lg::Light l = Lamp(1, 2, 3, 77);
    l.shadow.kind = lg::ShadowKind::kAtlas;
    l.shadow.first_view = 5;
    l.shadow.view_count = 2;
    l.shadow.weight = 0.5f;
    l.shadow.mask_channel = uint8_t(c);
    l.flags |= lg::kLightFlagRtVisibility;
    const lg::GpuLight g = lg::PackGpuLight(l);
    const lg::Light u = lg::UnpackGpuLight(g);
    CHECK(u.shadow.mask_channel == c && (u.flags & lg::kLightFlagRtVisibility) != 0 && (u.flags & lg::kLightFlagGameLit) != 0);
    CHECK(u.shadow.kind == lg::ShadowKind::kAtlas && u.shadow.first_view == 5 && u.shadow.view_count == 2 && u.shadow.weight == 0.5f);
  }
  CHECK(lg::kLightFlagRtVisibility == 8u && (lg::kLightFlagRtVisibility & lg::kGpuFlagsMask) != 0);
  CHECK(lg::kLightFlagRtVisibility != lg::kLightFlagGameLit && lg::kLightFlagRtVisibility != lg::kLightFlagPlayer && lg::kLightFlagRtVisibility != lg::kLightFlagLightsGeometry);
}

// ---------------------------------------------------------------------------
// the settings' words and the reasons
// ---------------------------------------------------------------------------
void TestSettings() {
  CHECK(rl::ParseMode("off") == rl::Mode::kOff && rl::ParseMode("OFF") == rl::Mode::kOff && rl::ParseMode(" 0 ") == rl::Mode::kOff && rl::ParseMode("false") == rl::Mode::kOff);
  CHECK(rl::ParseMode("on") == rl::Mode::kOn && rl::ParseMode("On\r\n") == rl::Mode::kOn && rl::ParseMode("1") == rl::Mode::kOn && rl::ParseMode("true") == rl::Mode::kOn);
  CHECK(rl::ParseMode("auto") == rl::Mode::kAuto && rl::ParseMode("") == rl::Mode::kAuto && rl::ParseMode("-1") == rl::Mode::kAuto && rl::ParseMode("whatever") == rl::Mode::kAuto);
  CHECK(std::string(rl::ModeName(rl::Mode::kOff)) == "off" && std::string(rl::ModeName(rl::Mode::kAuto)) == "auto" && std::string(rl::ModeName(rl::Mode::kOn)) == "on");
  // the pass is wanted: on whatever the row says, auto where the row's level is on, never when off
  for (int row = 0; row < 2; ++row) {
    CHECK(!rl::Wanted(rl::Mode::kOff, row != 0));
    CHECK(rl::Wanted(rl::Mode::kOn, row != 0));
    CHECK(rl::Wanted(rl::Mode::kAuto, row != 0) == (row != 0));
  }
  CHECK(rl::AutoK(false) == 3 && rl::AutoK(true) == 6);
  CHECK(rl::ClampK(0, false) == 3 && rl::ClampK(-4, true) == 6 && rl::ClampK(1, false) == 1 && rl::ClampK(5, false) == 5 && rl::ClampK(9, true) == 9 && rl::ClampK(99, false) == 9);
  CHECK(rl::ClampRays(0) == 1 && rl::ClampRays(1) == 1 && rl::ClampRays(3) == 3 && rl::ClampRays(40) == 4);
  CHECK(rl::ClampView(-1) == 0 && rl::ClampView(1) == 1 && rl::ClampView(2) == 2 && rl::ClampView(7) == 2);
  CHECK(rl::ClampCheck(-1) == 0 && rl::ClampCheck(1) == 1 && rl::ClampCheck(2) == 2 && rl::ClampCheck(9) == 2);
  for (int c = 0; c < 4; ++c) {
    const rl::Class cls = rl::Class(c);
    CHECK(rl::RadiusOf(0.0, cls) == rl::DefaultRadius(cls) && rl::RadiusOf(-1.0, cls) == rl::DefaultRadius(cls));
    CHECK(rl::RadiusOf(0.3, cls) == 0.3f && rl::RadiusOf(50.0, cls) == rl::kMaxRadius);
    CHECK(rl::DefaultRadius(cls) > 0.0f && rl::StopShort(cls) > 0.0f);
  }
  CHECK(rl::DefaultRadius(rl::Class::kHeadlight) < rl::DefaultRadius(rl::Class::kLamp) && rl::DefaultRadius(rl::Class::kLamp) < rl::DefaultRadius(rl::Class::kNeon));
  // the reasons, in the order a frame finds them
  rl::Facts f;
  CHECK(rl::Decide(f) == rl::Why::kOff);
  f.wanted = true;
  CHECK(rl::Decide(f) == rl::Why::kNoDevice);
  f.device_ok = true;
  CHECK(rl::Decide(f) == rl::Why::kNoScene);
  f.scene_ok = true;
  CHECK(rl::Decide(f) == rl::Why::kNoWorld);
  f.world_ok = true;
  CHECK(rl::Decide(f) == rl::Why::kNoDepth);
  f.depth_ok = true;
  CHECK(rl::Decide(f) == rl::Why::kNoLights);
  f.lights = 2;
  CHECK(rl::Decide(f) == rl::Why::kPipelines);
  f.pipelines_ready = true;
  CHECK(rl::Decide(f) == rl::Why::kResources);
  f.resources_ok = true;
  CHECK(rl::Decide(f) == rl::Why::kRuns);
  // each earlier reason hides the later ones
  for (int off = 0; off < 8; ++off) {
    rl::Facts g;
    g.wanted = off != 0;
    g.device_ok = off != 1;
    g.scene_ok = off != 2;
    g.world_ok = off != 3;
    g.depth_ok = off != 4;
    g.lights = off != 5 ? 1 : 0;
    g.pipelines_ready = off != 6;
    g.resources_ok = off != 7;
    CHECK(rl::Decide(g) == rl::Why(off + 1));
  }
  for (uint32_t w = 0; w < uint32_t(rl::Why::kCount); ++w) {
    CHECK(std::string(rl::WhyName(rl::Why(w))).size() > 3);
  }
}

// ---------------------------------------------------------------------------
// the top K
// ---------------------------------------------------------------------------
void TestSelection() {
  const float eye[3] = {0.0f, 1.5f, 0.0f};
  // eligibility
  lg::Light base = Lamp(5, 6, 5, 11);
  CHECK(rl::Eligible(base));
  { lg::Light l = base; l.id = lg::kNoLightId; CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.roles = lg::kRoleMaskSurface; CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.roles = lg::kRoleMaskRt; CHECK(rl::Eligible(l)); }
  { lg::Light l = base; l.range = 0.0f; CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.color[0] = l.color[1] = l.color[2] = 0.0f; CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.pos[1] = std::nanf(""); CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.dir[0] = INFINITY; CHECK(!rl::Eligible(l)); }
  { lg::Light l = base; l.range = INFINITY; CHECK(!rl::Eligible(l)); }
  CHECK(rl::Score(base, eye) > 0.0f);
  { lg::Light l = base; l.id = 0; CHECK(rl::Score(l, eye) == 0.0f); }
  // score falls with distance and rises with the colour
  CHECK(rl::Score(Lamp(3, 6, 0, 1), eye) > rl::Score(Lamp(30, 6, 0, 2), eye));
  CHECK(rl::Score(Lamp(3, 6, 0, 1, 100.0f), eye) > rl::Score(Lamp(3, 6, 0, 2, 40.0f), eye));
  // the class order: a headlight, then neon, then a lamp, then anything else, whatever the scores
  {
    std::vector<lg::Light> v;
    v.push_back(Lamp(2, 6, 0, 100));                           // the best lamp, nearest
    lg::Light other = Lamp(1, 3, 0, 101);
    other.source = lg::LightSource::kPolice;
    other.flags = 0;
    v.push_back(other);
    lg::Light neon = Lamp(40, 1, 0, 102);
    neon.source = lg::LightSource::kNeon;
    v.push_back(neon);
    v.push_back(Headlight(60, 1, 0, 103, 0.2f));              // far and dim, and still first
    const rl::Pick p = rl::SelectTopK(v.data(), uint32_t(v.size()), eye, nullptr, 0, 9);
    CHECK(p.count == 4 && p.id[0] == 103 && p.id[1] == 102 && p.id[2] == 100 && p.id[3] == 101);
    CHECK(rl::ClassOf(v[0]) == rl::Class::kLamp && rl::ClassOf(v[1]) == rl::Class::kOther && rl::ClassOf(v[2]) == rl::Class::kNeon && rl::ClassOf(v[3]) == rl::Class::kHeadlight);
    const rl::Pick two = rl::SelectTopK(v.data(), uint32_t(v.size()), eye, nullptr, 0, 2);
    CHECK(two.count == 2 && two.id[0] == 103 && two.id[1] == 102 && two.index[0] == 3 && two.index[1] == 2);
    CHECK(rl::SelectTopK(v.data(), uint32_t(v.size()), eye, nullptr, 0, 0).count == 0);
  }
  // within a class the score decides, then the keep bonus, then the id
  {
    std::vector<lg::Light> v = {Lamp(10, 6, 0, 1), Lamp(6, 6, 0, 2), Lamp(14, 6, 0, 3)};
    rl::Pick p = rl::SelectTopK(v.data(), 3, eye, nullptr, 0, 3);
    CHECK(p.id[0] == 2 && p.id[1] == 1 && p.id[2] == 3);
    // a lamp that held a channel scores 1.25 more: 1 (10 m) beats 2 (6 m) when its score x 1.25 is over 2's; here it is not, so the order stays; then a lamp near equal flips
    const uint64_t held[1] = {3};
    p = rl::SelectTopK(v.data(), 3, eye, held, 1, 3);
    CHECK(p.id[0] == 2 && p.id[1] == 1 && p.id[2] == 3);
    v = {Lamp(8.0f, 6, 0, 10), Lamp(8.5f, 6, 0, 11)};   // nearly equal: the one that holds wins
    const uint64_t held11[1] = {11};
    const uint64_t held10[1] = {10};
    CHECK(rl::SelectTopK(v.data(), 2, eye, nullptr, 0, 1).id[0] == 10);
    CHECK(rl::SelectTopK(v.data(), 2, eye, held11, 1, 1).id[0] == 11);
    CHECK(rl::SelectTopK(v.data(), 2, eye, held10, 1, 1).id[0] == 10);
    // equal scores: the smaller id first, whichever comes first in the input
    v = {Lamp(8, 6, 0, 20), Lamp(8, 6, 0, 19)};
    CHECK(rl::SelectTopK(v.data(), 2, eye, nullptr, 0, 2).id[0] == 19);
    std::swap(v[0], v[1]);
    CHECK(rl::SelectTopK(v.data(), 2, eye, nullptr, 0, 2).id[0] == 19);
  }
  // not eligible lights and duplicates are left out; the cap is K and nine
  {
    std::vector<lg::Light> v;
    for (uint32_t i = 0; i < 20; ++i) {
      v.push_back(Lamp(float(i) + 3.0f, 6, 0, 1000 + i));
    }
    v.push_back(Lamp(3.0f, 6, 0, 1000));   // the same id twice
    lg::Light dead = Lamp(1, 6, 0, 5000);
    dead.range = 0.0f;
    v.push_back(dead);
    const rl::Pick p = rl::SelectTopK(v.data(), uint32_t(v.size()), eye, nullptr, 0, 99);
    CHECK(p.count == 9);
    std::set<uint64_t> ids(p.id.begin(), p.id.begin() + p.count);
    CHECK(ids.size() == 9 && ids.count(5000) == 0);
    CHECK(rl::SelectTopK(v.data(), uint32_t(v.size()), eye, nullptr, 0, 4).count == 4);
    CHECK(rl::SelectTopK(v.data(), 0, eye, nullptr, 0, 4).count == 0);
  }
  // REVIEW S2 (RTL-D1-REVIEW, mcla_gpu_rt_lights_lamp_slot): class first ranking leaves the street lamps with no ray when traffic fills K; the A/B arm reserves the last channel for the strongest lamp.
  // Default (the argument left out, or false) = today's pick.
  {
    std::vector<lg::Light> v;
    v.push_back(Lamp(30, 6, 0, 300));                          // 0: a lamp at 30 m
    v.push_back(Headlight(0, 1, 25, 202));                     // 1
    v.push_back(Lamp(8, 6, 0, 301));                           // 2: the strongest lamp (8 m)
    v.push_back(Headlight(0, 1, 10, 201));                     // 3: the nearest headlight pair
    v.push_back(Headlight(0, 1, 40, 203));                     // 4
    lg::Light neon = Lamp(5, 0.3f, 0, 204);
    neon.source = lg::LightSource::kNeon;
    v.push_back(neon);                                          // 5
    v.push_back(Lamp(50, 6, 0, 302));                          // 6
    const uint32_t n = uint32_t(v.size());
    // OFF: three headlight pairs take all of K 3, no lamp; the argument false is the same thing
    rl::Pick off = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 3);
    CHECK(off.count == 3 && off.id[0] == 201 && off.id[1] == 202 && off.id[2] == 203);
    rl::Pick off2 = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 3, false);
    CHECK(off2.count == 3 && std::equal(off.id.begin(), off.id.begin() + 3, off2.id.begin()) && std::equal(off.index.begin(), off.index.begin() + 3, off2.index.begin()));
    // ON, K 3: the last channel goes to the strongest lamp (301 at 8 m, not the first lamp of the input), the two best headlights keep theirs
    rl::Pick on = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 3, true);
    CHECK(on.count == 3 && on.id[0] == 201 && on.id[1] == 202 && on.id[2] == 301 && on.index[2] == 2 && v[on.index[2]].id == 301);
    CHECK(rl::ClassOf(v[on.index[0]]) == rl::Class::kHeadlight && rl::ClassOf(v[on.index[2]]) == rl::Class::kLamp);
    // K 4: the neon is the last pick of today's list, so the lamp takes ITS place; K 5 has the lamp already: nothing changes; K 1 keeps the headlights' only channel; K 2 gives it to the lamp
    rl::Pick k4 = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 4, true);
    CHECK(k4.count == 4 && k4.id[0] == 201 && k4.id[1] == 202 && k4.id[2] == 203 && k4.id[3] == 301);
    CHECK(rl::SelectTopK(v.data(), n, eye, nullptr, 0, 4).id[3] == 204);
    {
      const rl::Pick a = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 5), b = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 5, true);
      CHECK(a.count == 5 && b.count == 5 && a.id[4] == 301 && std::equal(a.id.begin(), a.id.begin() + 5, b.id.begin()) && std::equal(a.index.begin(), a.index.begin() + 5, b.index.begin()));
    }
    {
      const rl::Pick a = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 1), b = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 1, true);
      CHECK(a.count == 1 && b.count == 1 && a.id[0] == 201 && b.id[0] == 201);
      const rl::Pick c = rl::SelectTopK(v.data(), n, eye, nullptr, 0, 2, true);
      CHECK(c.count == 2 && c.id[0] == 201 && c.id[1] == 301);
    }
    // the keep bonus decides between two lamps that are nearly equal: the one that held a channel keeps it (no churn from the reservation)
    std::vector<lg::Light> w = {Headlight(0, 1, 10, 1), Headlight(0, 1, 20, 2), Headlight(0, 1, 30, 3), Lamp(8.0f, 6, 0, 10), Lamp(8.5f, 6, 0, 11)};
    const uint64_t held11[1] = {11};
    const uint64_t held10[1] = {10};
    CHECK(rl::SelectTopK(w.data(), 5, eye, nullptr, 0, 3, true).id[2] == 10);
    CHECK(rl::SelectTopK(w.data(), 5, eye, held11, 1, 3, true).id[2] == 11);
    CHECK(rl::SelectTopK(w.data(), 5, eye, held10, 1, 3, true).id[2] == 10);
    // a set with no lamp, or only lamps, or fewer lights than K: the same pick on or off
    for (int kind = 0; kind < 3; ++kind) {
      std::vector<lg::Light> u;
      for (uint32_t i = 0; i < 4; ++i) {
        u.push_back(kind == 1 ? Lamp(5.0f + float(i), 6, 0, 700 + i) : Headlight(0, 1, 10.0f + float(i), 700 + i));
      }
      const uint32_t m = kind == 2 ? 2u : 4u;
      const rl::Pick a = rl::SelectTopK(u.data(), m, eye, nullptr, 0, 3), b = rl::SelectTopK(u.data(), m, eye, nullptr, 0, 3, true);
      CHECK(a.count == b.count && std::equal(a.id.begin(), a.id.begin() + a.count, b.id.begin()));
    }
    // a lamp that is not eligible is not reserved for (the strongest eligible one is), and "other" lights never take the slot
    std::vector<lg::Light> x = {Headlight(0, 1, 10, 1), Headlight(0, 1, 20, 2), Headlight(0, 1, 30, 3), Lamp(5, 6, 0, 20), Lamp(9, 6, 0, 21)};
    x[3].range = 0.0f;
    CHECK(rl::SelectTopK(x.data(), 5, eye, nullptr, 0, 3, true).id[2] == 21);
    x[4].source = lg::LightSource::kPolice;
    x[4].flags = 0;
    CHECK(rl::SelectTopK(x.data(), 5, eye, nullptr, 0, 3, true).id[2] == 3);
  }
  // properties on random sets: sorted by class then score, a subset of the eligible, any input order gives the same pick
  Lcg rng;
  for (int round = 0; round < 300; ++round) {
    const uint32_t n = 1 + rng.Below(14);
    std::vector<lg::Light> v;
    for (uint32_t i = 0; i < n; ++i) {
      lg::Light l = rng.Below(3) == 0 ? Headlight(rng.Range(-30, 30), 1, rng.Range(-30, 30), 500 + i, rng.Range(0.1f, 1.0f)) : Lamp(rng.Range(-40, 40), rng.Range(4, 9), rng.Range(-40, 40), 500 + i, rng.Range(20, 100));
      if (rng.Below(8) == 0) {
        l.source = rng.Below(2) ? lg::LightSource::kNeon : lg::LightSource::kPolice;
      }
      if (rng.Below(10) == 0) {
        l.range = 0.0f;
      }
      v.push_back(l);
    }
    std::vector<uint64_t> held;
    for (uint32_t i = 0; i < n; ++i) {
      if (rng.Below(3) == 0) {
        held.push_back(500 + i);
      }
    }
    const uint32_t k = 1 + rng.Below(9);
    const rl::Pick a = rl::SelectTopK(v.data(), n, eye, held.data(), uint32_t(held.size()), k);
    uint32_t eligible = 0;
    for (const lg::Light& l : v) {
      eligible += rl::Eligible(l) ? 1u : 0u;
    }
    CHECK(a.count == std::min({k, eligible, 9u}));
    for (uint32_t i = 0; i < a.count; ++i) {
      CHECK(v[a.index[i]].id == a.id[i] && rl::Eligible(v[a.index[i]]));
      if (i > 0) {
        CHECK(uint32_t(rl::ClassOf(v[a.index[i - 1]])) <= uint32_t(rl::ClassOf(v[a.index[i]])));
      }
    }
    std::vector<lg::Light> shuffled = v;
    for (uint32_t i = n; i > 1; --i) {
      std::swap(shuffled[i - 1], shuffled[rng.Below(i)]);
    }
    const rl::Pick b = rl::SelectTopK(shuffled.data(), n, eye, held.data(), uint32_t(held.size()), k);
    CHECK(a.count == b.count && std::equal(a.id.begin(), a.id.begin() + a.count, b.id.begin()));
    // the lamp slot arm (S2): the same count and class order; any input order gives the same pick; all but the last pick are today's; with a lamp among the lights and K >= 2 the pick holds the
    // strongest one (score with its keep bonus, then id), and with no change to make it IS today's pick
    const rl::Pick ar = rl::SelectTopK(v.data(), n, eye, held.data(), uint32_t(held.size()), k, true);
    const rl::Pick br = rl::SelectTopK(shuffled.data(), n, eye, held.data(), uint32_t(held.size()), k, true);
    CHECK(ar.count == a.count && br.count == ar.count && std::equal(ar.id.begin(), ar.id.begin() + ar.count, br.id.begin()));
    bool a_has_lamp = false;
    for (uint32_t i = 0; i < a.count; ++i) {
      a_has_lamp = a_has_lamp || rl::ClassOf(v[a.index[i]]) == rl::Class::kLamp;
    }
    uint64_t best_lamp = 0;
    float best_score = -1.0f;
    for (const lg::Light& l : v) {
      if (!rl::Eligible(l) || rl::ClassOf(l) != rl::Class::kLamp) {
        continue;
      }
      const bool was_held = std::find(held.begin(), held.end(), l.id) != held.end();
      const float score = rl::Score(l, eye) * (was_held ? rl::kKeepBonus : 1.0f);
      if (score > best_score || (score == best_score && l.id < best_lamp)) {
        best_score = score;
        best_lamp = l.id;
      }
    }
    const bool changes = !a_has_lamp && best_lamp != 0 && k >= 2 && a.count >= 2;
    if (changes) {
      CHECK(ar.id[ar.count - 1] == best_lamp && std::equal(a.id.begin(), a.id.begin() + a.count - 1, ar.id.begin()));
    } else {
      CHECK(std::equal(a.id.begin(), a.id.begin() + a.count, ar.id.begin()) && std::equal(a.index.begin(), a.index.begin() + a.count, ar.index.begin()));
    }
    for (uint32_t i = 0; i < ar.count; ++i) {
      CHECK(v[ar.index[i]].id == ar.id[i] && rl::Eligible(v[ar.index[i]]) && (i == 0 || uint32_t(rl::ClassOf(v[ar.index[i - 1]])) <= uint32_t(rl::ClassOf(v[ar.index[i]]))));
      for (uint32_t j = 0; j < i; ++j) {
        CHECK(ar.id[j] != ar.id[i]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the channels' owners
// ---------------------------------------------------------------------------
void TestAssignment() {
  rl::Assignment none;
  // the first frame: the wanted take channels 0, 1, 2 in order
  const uint64_t first[3] = {10, 20, 30};
  rl::Assigned a = rl::AssignChannels(none, first, 3, 3);
  CHECK(a.channel_of_wanted[0] == 0 && a.channel_of_wanted[1] == 1 && a.channel_of_wanted[2] == 2);
  CHECK(a.now.id[0] == 10 && a.now.id[1] == 20 && a.now.id[2] == 30 && rl::Used(a.now) == 3);
  CHECK(a.changed_groups == 1u);   // group 0's owners changed (from nobody)
  // the same lights again: nothing changes, a different order keeps every channel
  rl::Assigned b = rl::AssignChannels(a.now, first, 3, 3);
  CHECK(b.now == a.now && b.changed_groups == 0u);
  const uint64_t reordered[3] = {30, 10, 20};
  b = rl::AssignChannels(a.now, reordered, 3, 3);
  CHECK(b.channel_of_wanted[0] == 2 && b.channel_of_wanted[1] == 0 && b.channel_of_wanted[2] == 1 && b.now == a.now && b.changed_groups == 0u);
  // one leaves, a newcomer takes its channel (the lowest free): that group starts again
  const uint64_t swapped[3] = {10, 40, 30};
  b = rl::AssignChannels(a.now, swapped, 3, 3);
  CHECK(b.channel_of_wanted[0] == 0 && b.channel_of_wanted[1] == 1 && b.channel_of_wanted[2] == 2 && b.now.id[1] == 40 && b.changed_groups == 1u);
  CHECK(rl::ChannelFor(b.now, 40) == 1 && rl::ChannelFor(b.now, 20) == rl::kNoChannel && rl::ChannelFor(b.now, 0) == rl::kNoChannel);
  // fewer lights than channels: the freed channel is empty and its group is named changed
  const uint64_t two[2] = {10, 30};
  b = rl::AssignChannels(a.now, two, 2, 3);
  CHECK(b.now.id[1] == 0 && rl::Used(b.now) == 2 && b.changed_groups == 1u);
  // a newcomer in the second image changes only that group
  rl::Assignment full;
  full.id = {1, 2, 3, 0, 0, 0, 0, 0, 0};
  const uint64_t four[4] = {1, 2, 3, 9};
  b = rl::AssignChannels(full, four, 4, 6);
  CHECK(b.channel_of_wanted[3] == 3 && b.changed_groups == 2u);
  // K goes down: a holder past the limit lets go and a wanted light that held it takes a channel below
  rl::Assignment high;
  high.id = {0, 0, 0, 0, 0, 0, 7, 0, 0};
  const uint64_t seven[1] = {7};
  b = rl::AssignChannels(high, seven, 1, 3);
  CHECK(b.channel_of_wanted[0] == 0 && b.now.id[6] == 0 && b.now.id[0] == 7 && (b.changed_groups & 1u) != 0 && (b.changed_groups & 4u) != 0);
  // a light that holds a channel above the old count keeps it while the limit allows
  b = rl::AssignChannels(high, seven, 1, 9);
  CHECK(b.channel_of_wanted[0] == 6 && b.changed_groups == 0u);
  // more wanted than the limit holds: the extra get no channel (the caller never asks: n <= limit)
  const uint64_t many[5] = {1, 2, 3, 4, 5};
  b = rl::AssignChannels(none, many, 5, 3);
  CHECK(b.channel_of_wanted[3] == rl::kNoChannel && b.channel_of_wanted[4] == rl::kNoChannel && rl::Used(b.now) == 3);
  // a zero id is never a holder
  const uint64_t zero[2] = {0, 5};
  b = rl::AssignChannels(none, zero, 2, 3);
  CHECK(b.channel_of_wanted[0] == rl::kNoChannel && b.channel_of_wanted[1] == 0);
  // random sequences: every wanted light that fits has a channel, ids are unique, a light kept in the pick keeps its channel, the changed groups are exactly the groups whose owners differ
  Lcg rng;
  rl::Assignment held;
  for (int step = 0; step < 2000; ++step) {
    const uint32_t k = 1 + rng.Below(9);
    const uint32_t limit = rl::ImagesFor(k) * 3;
    std::vector<uint64_t> wanted;
    const uint32_t count = rng.Below(k + 1);
    while (wanted.size() < count) {
      const uint64_t id = 1 + rng.Below(14);
      if (std::find(wanted.begin(), wanted.end(), id) == wanted.end()) {
        wanted.push_back(id);
      }
    }
    const rl::Assigned r = rl::AssignChannels(held, wanted.data(), uint32_t(wanted.size()), limit);
    std::set<uint64_t> owners;
    for (uint32_t c = 0; c < rl::kMaxLights; ++c) {
      if (r.now.id[c] != 0) {
        CHECK(owners.insert(r.now.id[c]).second && c < limit);
      }
    }
    uint32_t expect_changed = 0;
    for (uint32_t i = 0; i < wanted.size(); ++i) {
      CHECK(r.channel_of_wanted[i] < limit && r.now.id[r.channel_of_wanted[i]] == wanted[i]);
      const uint32_t before = rl::ChannelFor(held, wanted[i]);
      if (before < limit) {
        CHECK(r.channel_of_wanted[i] == before);   // kept
      }
    }
    CHECK(rl::Used(r.now) == wanted.size());
    for (uint32_t c = 0; c < rl::kMaxLights; ++c) {
      if (r.now.id[c] != held.id[c]) {
        expect_changed |= rl::GroupBit(c);
      }
    }
    CHECK(r.changed_groups == expect_changed);
    held = r.now;
  }
}

// ---------------------------------------------------------------------------
// the rows and the table
// ---------------------------------------------------------------------------
void TestRows() {
  CHECK(sizeof(rl::GpuRtLight) == 64 && sizeof(rl::GpuRtHeader) == 64 && sizeof(rl::GpuRtMaskTable) == 64);
  CHECK(rl::kRowsBytes == 640 && rl::kRowsAlloc == 1024 && rl::kRowsHeaderBytes == 64);
  // a street lamp: a cone, no pair, the lamp class's radius and stop short
  const lg::Light lamp = Lamp(1, 6, 2, 9);
  rl::GpuRtLight g = rl::MakeGpuRtLight(lamp, 4, 0.0);
  CHECK(g.row0[0] == 1 && g.row0[1] == 6 && g.row0[2] == 2 && g.row0[3] == lamp.range);
  CHECK(g.row1[0] == lamp.dir[0] && g.row1[1] == lamp.dir[1] && g.row1[2] == lamp.dir[2] && g.row1[3] == lamp.cos_outer);
  CHECK(g.row2[3] == rl::kRadiusLamp && g.stop_short == rl::kStopLamp && g.channel == 4 && g.flags == rl::kGpuFlagSpot);
  // a headlight pair: the cone and the pair, the lens' radius
  const lg::Light head = Headlight(0, 1, 0, 3);
  g = rl::MakeGpuRtLight(head, 0, 0.0);
  CHECK(g.flags == (rl::kGpuFlagSpot | rl::kGpuFlagPair) && g.row2[0] == 0.6f && g.row2[3] == rl::kRadiusHeadlight && g.stop_short == rl::kStopHeadlight);
  // a point light has no cone; a setting radius overrides the class
  lg::Light point = Lamp(0, 5, 0, 4);
  point.kind = lg::LightKind::kPoint;
  point.cos_outer = -1.0f;
  g = rl::MakeGpuRtLight(point, 2, 0.5);
  CHECK(g.flags == 0u && g.row2[3] == 0.5f && g.channel == 2);
  // the table the light pass reads
  rl::MaskFrame f;
  rl::GpuRtMaskTable t = rl::BuildMaskTable(f);
  CHECK(!rl::MaskTableValid(t) && t.slot[0] == lg::kNoLightSlot && t.lin_slot == lg::kNoLightSlot);
  f.valid = true;
  f.images = 2;
  f.width = 1920;
  f.height = 1080;
  f.slot = {11, 12, lg::kNoLightSlot};
  f.raw_slot = {21, 22, lg::kNoLightSlot};
  f.lin_slot = 30;
  t = rl::BuildMaskTable(f);
  CHECK(rl::MaskTableValid(t) && t.images == 2 && t.width == 1920 && t.height == 1080 && t.slot[0] == 11 && t.slot[1] == 12 && t.slot[2] == lg::kNoLightSlot && t.lin_slot == 30);
  CHECK(t.raw_slot[0] == 21 && t.raw_slot[1] == 22 && t.depth_sigma == rl::kDepthSigma && t.flags == rl::kMaskFlagValid);
  CHECK(t.step == 1 && t.traced_width == 1920 && t.traced_height == 1080);   // no step or traced size given: a whole mask
  f.view = rl::kViewFiltered;
  CHECK(rl::BuildMaskTable(f).flags == (rl::kMaskFlagValid | rl::kMaskFlagView));
  f.view = rl::kViewRaw;
  CHECK(rl::BuildMaskTable(f).flags == (rl::kMaskFlagValid | rl::kMaskFlagView | rl::kMaskFlagRawView));
  // an image a light can name that is not there, no size, no images, too many: no table
  rl::MaskFrame bad = f;
  bad.slot[1] = lg::kNoLightSlot;
  CHECK(!rl::MaskTableValid(rl::BuildMaskTable(bad)));
  bad = f;
  bad.width = 0;
  CHECK(!rl::MaskTableValid(rl::BuildMaskTable(bad)));
  bad = f;
  bad.images = 0;
  CHECK(!rl::MaskTableValid(rl::BuildMaskTable(bad)));
  bad = f;
  bad.images = 4;
  CHECK(!rl::MaskTableValid(rl::BuildMaskTable(bad)));
  bad = f;
  bad.valid = false;
  CHECK(!rl::MaskTableValid(rl::BuildMaskTable(bad)));
  // the memory: see TestMemory
  CHECK(rl::HeldBytes(1920, 1080, 1) == uint64_t(1920) * 1080 * 28 && rl::HeldBytes(1920, 1080, 3) == uint64_t(1920) * 1080 * 68);
}

// ---------------------------------------------------------------------------
// the composition
// ---------------------------------------------------------------------------
void TestComposition() {
  CHECK(rl::CleanVisibility(0.0f) == 0.0f && rl::CleanVisibility(1.0f) == 1.0f && rl::CleanVisibility(0.37f) == 0.37f);
  CHECK(rl::CleanVisibility(-0.2f) == 0.0f && rl::CleanVisibility(1.3f) == 1.0f);
  CHECK(rl::CleanVisibility(std::nanf("")) == 1.0f && rl::CleanVisibility(INFINITY) == 1.0f && rl::CleanVisibility(-INFINITY) == 1.0f);
  // the product: 1 is the identity, either side can darken
  CHECK(rl::Combine(1.0f, 1.0f) == 1.0f && rl::Combine(0.5f, 1.0f) == 0.5f && rl::Combine(1.0f, 0.25f) == 0.25f && rl::Combine(0.5f, 0.5f) == 0.25f && rl::Combine(0.0f, 1.0f) == 0.0f);
  CHECK(rl::Combine(std::nanf(""), 0.5f) == 0.5f && rl::Combine(0.5f, std::nanf("")) == 0.5f);
  // the share of a game's lamp taken away: in [0, term], whatever the visibility
  Lcg rng;
  const float weird[] = {0.0f, 1.0f, -1.0f, 2.0f, 0.5f, 1e-30f, std::nanf(""), INFINITY, -INFINITY, 1e30f, -0.0f};
  for (float v : weird) {
    for (float term : {0.0f, 1e-20f, 0.3f, 16.0f, 1e6f, -2.0f, std::nanf(""), INFINITY, -INFINITY}) {
      const float share = rl::SubtractShare(term, v);
      CHECK(share >= 0.0f && std::isfinite(share));
      CHECK(!(term > 0.0f && std::isfinite(term)) || share <= term);
    }
  }
  for (int i = 0; i < 20000; ++i) {
    const float term = rng.Range(0.0f, 50.0f);
    const float v = rng.Range(-0.5f, 1.5f);
    const float share = rl::SubtractShare(term, v);
    CHECK(share >= 0.0f && share <= term);
    CHECK(Near(share, term * (1.0f - rl::CleanVisibility(v)), 1e-6));
  }
  // THE IDENTITY: V = 1 takes nothing from a game's lamp (a zero) and leaves an owned lamp's term as it is, bit for bit
  for (int i = 0; i < 5000; ++i) {
    const float term[3] = {rng.Range(0.0f, 20.0f), rng.Range(0.0f, 20.0f), rng.Range(0.0f, 20.0f)};
    float out[3];
    rl::Apply(true, term, 1.0f, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);            // -0.0 == 0.0: the ADD blend leaves the colour as it was
    CHECK(!(out[0] > 0.0f) && !(out[1] > 0.0f) && !(out[2] > 0.0f));      // never positive: a game's lamp is not added
    rl::Apply(false, term, 1.0f, out);
    CHECK(std::memcmp(out, term, sizeof(out)) == 0);
    // V = 0: a game's lamp is taken away in full, an owned one adds nothing
    rl::Apply(true, term, 0.0f, out);
    CHECK(out[0] == -term[0] && out[1] == -term[1] && out[2] == -term[2]);
    rl::Apply(false, term, 0.0f, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
    // any V: a game's lamp never adds, an owned one never adds more than its term
    const float v = rng.Range(-1.0f, 2.0f);
    rl::Apply(true, term, v, out);
    CHECK(out[0] <= 0.0f && out[1] <= 0.0f && out[2] <= 0.0f && out[0] >= -term[0] && out[1] >= -term[1] && out[2] >= -term[2]);
    rl::Apply(false, term, v, out);
    CHECK(out[0] >= 0.0f && out[0] <= term[0] && out[1] >= 0.0f && out[1] <= term[1] && out[2] >= 0.0f && out[2] <= term[2]);
  }
  // THE PIXEL MODEL: the game's forward colour holds the ambient and every grid lamp's term; the pass takes away the shadowed share of the lamps it knows (the same terms as the grid's, as the
  // units' check holds them): no pixel goes negative whatever the visibilities. A lamp the grid does not hold (the "fifth lamp") is the one way to go negative: the model shows it and the
  // pass's rule for it (the term capped at what the pixel holds) is not part of this stage (CHECK C6).
  int negatives = 0;
  for (int i = 0; i < 20000; ++i) {
    const float ambient = rng.Range(0.0f, 0.5f);
    float grid[4];
    float v[4];
    float colour = ambient;
    for (int k = 0; k < 4; ++k) {
      grid[k] = rng.Range(0.0f, 3.0f);
      v[k] = rl::Combine(rng.Range(0.0f, 1.2f), rng.Range(0.0f, 1.2f));
      colour += grid[k];
    }
    for (int k = 0; k < 4; ++k) {
      const float term[3] = {grid[k], grid[k], grid[k]};
      float out[3];
      rl::Apply(true, term, v[k], out);
      colour += out[0];
    }
    negatives += colour < -1e-5f ? 1 : 0;
    // and the picture with everything visible is the game's
    float same = ambient;
    for (int k = 0; k < 4; ++k) {
      same += grid[k];
    }
    float sum = ambient;
    for (int k = 0; k < 4; ++k) {
      sum += grid[k];
    }
    for (int k = 0; k < 4; ++k) {
      const float term[3] = {grid[k], grid[k], grid[k]};
      float out[3];
      rl::Apply(true, term, 1.0f, out);
      sum += out[0];
    }
    CHECK(sum == same);
  }
  CHECK(negatives == 0);
}

// ---------------------------------------------------------------------------
// the ray's geometry (the shader's own file as C++)
// ---------------------------------------------------------------------------
struct Axes {
  hs::float3 x, y, z;
};
Axes RandomAxes(Lcg& rng) {
  // a rotation from three random angles: the rows are the world's axes in view space, orthonormal
  const float a = rng.Range(0.0f, 6.28f), b = rng.Range(-1.4f, 1.4f), c = rng.Range(0.0f, 6.28f);
  const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
  const float m[3][3] = {{ca * cb, ca * sb * sc - sa * cc, ca * sb * cc + sa * sc}, {sa * cb, sa * sb * sc + ca * cc, sa * sb * cc - ca * sc}, {-sb, cb * sc, cb * cc}};
  return {hs::float3(m[0][0], m[1][0], m[2][0]), hs::float3(m[0][1], m[1][1], m[2][1]), hs::float3(m[0][2], m[1][2], m[2][2])};
}
// rt_world_frame.hlsli RtWorldPoint: camera + (dot(view, axisX), dot(view, axisY), dot(view, axisZ))
hs::float3 WorldPoint(hs::float3 view, hs::float3 camera, const Axes& ax) {
  return camera + hs::float3(hs::dot(view, ax.x), hs::dot(view, ax.y), hs::dot(view, ax.z));
}

void TestGeometry() {
  Lcg rng;
  // the world to view move is the inverse of the world frame's view to world
  for (int i = 0; i < 2000; ++i) {
    const Axes ax = RandomAxes(rng);
    const hs::float3 camera(rng.Range(-500, 500), rng.Range(0, 50), rng.Range(-500, 500));
    const hs::float3 view(rng.Range(-60, 60), rng.Range(-20, 20), rng.Range(0.5f, 200));
    const hs::float3 world = WorldPoint(view, camera, ax);
    const hs::float3 back = hs::RtvWorldToView(world, camera, ax.x, ax.y, ax.z);
    CHECK(Near(back.x, view.x, 1e-3) && Near(back.y, view.y, 1e-3) && Near(back.z, view.z, 1e-3));
    const hs::float3 dir(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
    const hs::float3 dv = hs::RtvDirToView(dir, ax.x, ax.y, ax.z);
    CHECK(Near(hs::length(dv), hs::length(dir), 1e-4));   // a rotation: lengths are kept
    const hs::float3 w0 = WorldPoint(hs::float3(0, 0, 0), camera, ax);
    CHECK(Near(w0.x, camera.x) && Near(w0.y, camera.y) && Near(w0.z, camera.z));
  }
  // the pair's lamp
  const hs::float3 pos(1, 2, 3), off(0.5f, 0, 0);
  CHECK(hs::RtvLampPoint(pos, off, false, 0.2f).x == 1.0f && hs::RtvLampPoint(pos, off, false, 0.9f).x == 1.0f);
  CHECK(hs::RtvLampPoint(pos, off, true, 0.2f).x == 1.5f && hs::RtvLampPoint(pos, off, true, 0.7f).x == 0.5f);
  // the reach tests against an independent formula
  for (int i = 0; i < 20000; ++i) {
    const hs::float3 P(rng.Range(-30, 30), rng.Range(0, 3), rng.Range(-30, 30));
    hs::float3 N(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
    N = hs::normalize(N);
    const hs::float3 lamp(rng.Range(-30, 30), rng.Range(4, 10), rng.Range(-30, 30));
    hs::float3 dir(rng.Range(-1, 1), rng.Range(-1, 0), rng.Range(-1, 1));
    dir = hs::normalize(dir);
    const float cosOuter = rng.Range(-0.2f, 0.9f);
    const float range = rng.Range(5, 60);
    const bool spot = rng.Below(4) != 0;
    const hs::RtvReach r = hs::RtvReaches(P, N, lamp, dir, cosOuter, range, spot);
    const double dx = lamp.x - P.x, dy = lamp.y - P.y, dz = lamp.z - P.z;
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double ux = dx / d, uy = dy / d, uz = dz / d;
    const double ndl = N.x * ux + N.y * uy + N.z * uz;
    const double cosA = -(dir.x * ux + dir.y * uy + dir.z * uz);
    const bool expect = d < range && ndl > rl::kMinNdotL && (!spot || cosA > cosOuter);
    if (std::fabs(d - range) > 1e-3 && std::fabs(ndl - rl::kMinNdotL) > 1e-5 && std::fabs(cosA - cosOuter) > 1e-5) {
      CHECK(r.covered == expect);
    }
    if (r.covered) {
      CHECK(Near(r.distance, d, 1e-4) && Near(r.toLight.x, ux, 1e-4) && Near(r.toLight.y, uy, 1e-4) && Near(r.toLight.z, uz, 1e-4));
    }
  }
  // hand worked: a lamp straight above a floor point lights it; a point beyond the range, behind the surface, or outside the cone does not
  const hs::float3 floorN(0, 1, 0), down(0, -1, 0);
  CHECK(hs::RtvReaches(hs::float3(0, 0, 0), floorN, hs::float3(0, 6, 0), down, 0.5f, 20.0f, true).covered);
  CHECK(!hs::RtvReaches(hs::float3(0, 0, 0), floorN, hs::float3(0, 6, 0), down, 0.5f, 5.0f, true).covered);
  CHECK(!hs::RtvReaches(hs::float3(0, 0, 0), hs::float3(0, -1, 0), hs::float3(0, 6, 0), down, 0.5f, 20.0f, true).covered);
  CHECK(!hs::RtvReaches(hs::float3(30, 0, 0), floorN, hs::float3(0, 6, 0), down, 0.9f, 80.0f, true).covered);
  CHECK(hs::RtvReaches(hs::float3(30, 0, 0), floorN, hs::float3(0, 6, 0), down, 0.9f, 80.0f, false).covered);   // a point light has no cone
  CHECK(!hs::RtvReaches(hs::float3(0, 6, 0), floorN, hs::float3(0, 6, 0), down, 0.5f, 20.0f, true).covered);    // at the lamp itself: nothing
  // the ray: origin above the surface by the lift, the end short of the light, a unit direction kept above the tangent plane
  for (int i = 0; i < 20000; ++i) {
    const hs::float3 P(rng.Range(-30, 30), rng.Range(0, 3), rng.Range(-30, 30));
    const hs::float3 N = hs::normalize(hs::float3(rng.Range(-0.3f, 0.3f), 1.0f, rng.Range(-0.3f, 0.3f)));
    const hs::float3 lamp(rng.Range(-30, 30), rng.Range(4, 12), rng.Range(-30, 30));
    const float radius = rng.Range(0.0f, 1.0f), stop = rng.Range(0.0f, 0.6f);
    const float lift = rng.Range(0.0f, 0.4f);
    const hs::float2 disc(rng.Range(-0.7f, 0.7f), rng.Range(-0.7f, 0.7f));
    const hs::RtvRay ray = hs::RtvMakeRay(P, N, lift, lamp, radius, disc, stop);
    const hs::float3 o = ray.origin - P;
    CHECK(Near(o.x, N.x * lift, 1e-5) && Near(o.y, N.y * lift, 1e-5) && Near(o.z, N.z * lift, 1e-5));
    CHECK(Near(hs::length(ray.direction), 1.0, 1e-4));
    CHECK(hs::dot(ray.direction, N) >= rl::kMinAbove - 2e-4);   // never under the tangent plane's margin
    CHECK(ray.tMax >= 0.0f);
    // the uncorrected target of the disc, by the shader's own basis: where the ray was to go
    const hs::float3 axis = hs::normalize(lamp - ray.origin);
    const hs::RtvBasis basis = hs::RtvFrame(axis);
    const hs::float3 target = lamp + (basis.t * disc.x + basis.b * disc.y) * radius;
    const double dist = hs::length(target - ray.origin);
    const hs::float3 want = hs::normalize(target - ray.origin);
    CHECK(Near(ray.tMax, std::max(dist - stop, 0.0), 1e-4));
    if (hs::dot(want, N) >= rl::kMinAbove) {
      // not bent: the ray goes straight to the point of the disc and ends `stop` short of it
      CHECK(Near(ray.direction.x, want.x, 1e-4) && Near(ray.direction.y, want.y, 1e-4) && Near(ray.direction.z, want.z, 1e-4));
      const hs::float3 end = ray.origin + ray.direction * ray.tMax;
      CHECK(dist <= stop || Near(hs::length(target - end), stop, 1e-3));
      CHECK(hs::length(target - lamp) <= radius + 1e-3);
    } else {
      // bent: exactly the margin above the plane, the same way along it
      CHECK(Near(hs::dot(ray.direction, N), rl::kMinAbove, 2e-4));
      const hs::float3 t0 = want - N * hs::dot(want, N);
      const hs::float3 t1 = ray.direction - N * hs::dot(ray.direction, N);
      if (hs::length(t0) > 1e-3 && hs::length(t1) > 1e-6) {
        CHECK(hs::dot(hs::normalize(t0), hs::normalize(t1)) > 0.9999);
      }
    }
  }
  // with a flat approach (no tangent correction) the target is on the disc: perpendicular to the axis, within the radius
  {
    const hs::float3 P(0, 0, 0), N(0, 1, 0), lamp(3, 8, 4);
    for (int i = 0; i < 2000; ++i) {
      const float r = 0.3f;
      const hs::float2 disc(rng.Range(-0.7f, 0.7f), rng.Range(-0.7f, 0.7f));
      const hs::RtvRay ray = hs::RtvMakeRay(P, N, 0.0f, lamp, r, disc, 0.0f);
      const hs::float3 target = ray.origin + ray.direction * ray.tMax;
      const hs::float3 axis = hs::normalize(lamp - P);
      CHECK(Near(hs::dot(target - lamp, axis), 0.0, 1e-3));   // the disc faces the ray
      CHECK(hs::length(target - lamp) <= r * std::sqrt(disc.x * disc.x + disc.y * disc.y) + 1e-3);
    }
  }
  // the basis is orthonormal
  for (int i = 0; i < 2000; ++i) {
    const hs::float3 n = hs::normalize(hs::float3(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1)));
    const hs::RtvBasis b = hs::RtvFrame(n);
    CHECK(Near(hs::length(b.t), 1.0, 1e-4) && Near(hs::length(b.b), 1.0, 1e-4) && std::fabs(hs::dot(b.t, b.b)) < 1e-4 && std::fabs(hs::dot(b.t, n)) < 1e-4 && std::fabs(hs::dot(b.b, n)) < 1e-4);
  }
  // the disc's points: inside the unit disc, and the strata cover the disc's area evenly
  for (uint32_t rays = 1; rays <= 4; ++rays) {
    std::vector<int> bins(8, 0);
    int n = 0;
    for (int i = 0; i < 4000; ++i) {
      for (uint32_t k = 0; k < rays; ++k) {
        const hs::float2 d = hs::RtvDiscSample(k, rays, rng.F(), rng.F());
        CHECK(d.x * d.x + d.y * d.y <= 1.0001f);
        // equal-area rings: r^2 in eight bins
        const float r2 = d.x * d.x + d.y * d.y;
        bins[size_t(std::min(7, int(r2 * 8.0f)))]++;
        ++n;
      }
    }
    for (int b : bins) {
      CHECK(std::fabs(double(b) / n - 0.125) < 0.02);
    }
  }
  // the lift
  CHECK(Near(hs::RtvLift(0.0f, 10.0f), rl::kLiftFloor) && hs::RtvLift(0.001f, 1000.0f) == rl::kLiftCap && hs::RtvLift(0.001f, 20.0f) > hs::RtvLift(0.001f, 10.0f));
  // the coin: in [0, 1), changes with the primitive, mean one half
  {
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < 20000; ++i) {
      const float c = hs::RtvCoin(rng.F(), rng.U32());
      CHECK(c >= 0.0f && c < 1.0f);
      sum += c;
      ++n;
    }
    CHECK(std::fabs(sum / n - 0.5) < 0.01);
    CHECK(hs::RtvCoin(0.3f, 1) != hs::RtvCoin(0.3f, 2) && hs::RtvCoin(0.3f, 5) == hs::RtvCoin(0.3f, 5 + 1024));
  }
}

// ---------------------------------------------------------------------------
// the soft shadow of a disc light: the shader's own ray against an analytic occluder
// ---------------------------------------------------------------------------
// A half plane at height 3 m (everything with x < 0 is blocked), a disc light of radius R above the origin at 8 m, a ground point at (px, 0, 0). A ray from the point to a point q of the disc crosses the
// plane at x = px (1 - 3/8) + qx 3/8; it is blocked when that is under 0, i.e. qx < -5 px / 3. The share of a disc left of the line qx = t is (asin(u) + u sqrt(1 - u^2)) / pi + 1/2 with u = t / R
// (the circular segment), so the visibility is 1 minus it. The rays are the shader's (RtvMakeRay to a point of the disc, the disc facing the ray) with the disc points of RtvDiscSample, the noise
// numbers uniform here (the noise's own tests are above): this checks the geometry of the soft shadow, the orientation and the area of the disc and that a stop short does not change where it is.
double DiscShare(double u) {
  if (u <= -1.0) return 0.0;
  if (u >= 1.0) return 1.0;
  return (std::asin(u) + u * std::sqrt(1.0 - u * u)) / 3.14159265358979 + 0.5;
}

void TestSoftShadow() {
  Lcg rng;
  const float radius = 0.5f;
  const hs::float3 lamp(0.0f, 8.0f, 0.0f), normal(0.0f, 1.0f, 0.0f);
  for (uint32_t rays : {1u, 2u, 4u}) {
    double worst = 0.0;
    for (int step = 0; step <= 40; ++step) {
      const float px = -1.2f + 0.06f * float(step);   // the geometric edge is at px = 0 (the disc's centre is over the origin)
      const hs::float3 point(px, 0.0f, 0.0f);
      int blocked = 0, total = 0;
      for (int i = 0; i < 6000; ++i) {
        for (uint32_t k = 0; k < rays; ++k) {
          const hs::float2 disc = hs::RtvDiscSample(k, rays, rng.F(), rng.F());
          const hs::RtvRay ray = hs::RtvMakeRay(point, normal, 0.0f, lamp, radius, disc, 0.35f);
          if (ray.tMax <= 0.0f) {
            continue;
          }
          // the first crossing of the plane y = 3 within the ray's length: blocked where x < 0 there
          const float t_plane = (3.0f - ray.origin.y) / ray.direction.y;
          bool hit = false;
          if (ray.direction.y > 1e-4f && t_plane > 0.0f && t_plane < ray.tMax) {
            hit = ray.origin.x + ray.direction.x * t_plane < 0.0f;
          }
          blocked += hit ? 1 : 0;
          ++total;
        }
      }
      const double v = 1.0 - double(blocked) / double(total);
      const double expect = 1.0 - DiscShare(-5.0 * double(px) / 3.0 / double(radius));
      worst = std::max(worst, std::fabs(v - expect));
    }
    CHECK(worst < 0.04);   // Monte Carlo of 6000 samples and a disc a little off its axis for the farthest points
  }
  // the visibility rises from the blocked side to the lit side, 0 well inside the shadow, 1 well outside, one half at the geometric edge, and the penumbra is as wide as the disc says: the edge of the
  // shadow of a half plane 3 m above the ground under an 0.5 m disc 8 m up is 2 x R x 3 / 5 x ... : the visibility goes 0 to 1 over px in +-R x 3 / 5 = +-0.3 m
  {
    const auto visibility = [&](float px) {
      const hs::float3 point(px, 0.0f, 0.0f);
      int blocked = 0;
      const int n = 20000;
      for (int i = 0; i < n; ++i) {
        const hs::float2 disc = hs::RtvDiscSample(0, 1, rng.F(), rng.F());
        const hs::RtvRay ray = hs::RtvMakeRay(point, normal, 0.0f, lamp, radius, disc, 0.0f);
        const float t_plane = (3.0f - ray.origin.y) / ray.direction.y;
        blocked += (t_plane > 0.0f && t_plane < ray.tMax && ray.origin.x + ray.direction.x * t_plane < 0.0f) ? 1 : 0;
      }
      return 1.0 - double(blocked) / double(n);
    };
    CHECK(visibility(-0.5f) < 0.01 && visibility(0.5f) > 0.99 && std::fabs(visibility(0.0f) - 0.5) < 0.02);
    CHECK(visibility(-0.28f) < 0.1 && visibility(0.28f) > 0.9);
  }
  // a stop short that is shorter than the plane's distance from the lamp changes nothing of where the shadow is; one longer than the whole ray ends the ray at the surface (no ray, nothing blocks)
  {
    const hs::float3 point(-0.5f, 0.0f, 0.0f);
    const hs::RtvRay with = hs::RtvMakeRay(point, normal, 0.0f, lamp, radius, hs::float2(0.0f, 0.0f), 0.5f);
    const hs::RtvRay without = hs::RtvMakeRay(point, normal, 0.0f, lamp, radius, hs::float2(0.0f, 0.0f), 0.0f);
    CHECK(Near(with.direction.x, without.direction.x) && Near(with.direction.y, without.direction.y) && Near(with.tMax, without.tMax - 0.5, 1e-4));
    CHECK(hs::RtvMakeRay(point, normal, 0.0f, lamp, radius, hs::float2(0.0f, 0.0f), 50.0f).tMax == 0.0f);
  }
}

// ---------------------------------------------------------------------------
// the noise
// ---------------------------------------------------------------------------
void TestNoise() {
  // no two (light, slot) of a frame share a sample; no two frames of a (light, slot) either (within a long window)
  {
    std::set<uint32_t> seen;
    bool distinct = true;
    for (uint32_t light = 0; light < hs::kRtvMaxLights; ++light) {
      for (uint32_t slot = 0; slot < hs::kRtvSlots; ++slot) {
        distinct = distinct && seen.insert(hs::RtvSampleIndex(1234, light, slot)).second;
      }
    }
    CHECK(distinct && seen.size() == 72);
    std::set<uint32_t> over_frames;
    bool moving = true;
    for (uint32_t frame = 0; frame < 5000; ++frame) {
      moving = moving && over_frames.insert(hs::RtvSampleIndex(frame, 3, 1)).second;
    }
    CHECK(moving);
    CHECK(hs::RtvSampleIndex(10, 0, 0) == 10u && hs::RtvSampleIndex(11, 0, 0) == 11u);   // the frame's step along the sequence is the helper's own
    CHECK(hs::RtvSampleIndex(0, 0, hs::kRtvSlotPair) != hs::RtvSampleIndex(0, 0, hs::kRtvSlotCoin) && hs::kRtvSlotPair < hs::kRtvSlots && hs::kRtvSlotCoin < hs::kRtvSlots && hs::kRtvMaxRays <= hs::kRtvSlotPair);
  }
  // AGENT-RULE-NOISE for a light's rays: the numbers the first ray of light 0 takes (the disc), a pair's pick and a coin, as the shader asks for them
  const auto ray_noise = [](uint32_t light, uint32_t slot) {
    return [light, slot](int x, int y, uint32_t frame) {
      const hs::float2 u = hs::BlueNoiseFrame2(x, y, hs::RtvSampleIndex(frame, light, slot));
      return std::pair<float, float>(u.x, u.y);
    };
  };
  const std::pair<uint32_t, uint32_t> streams[] = {{0, 0}, {0, 3}, {2, 0}, {8, hs::kRtvSlotPair}, {5, hs::kRtvSlotCoin}};
  for (const auto& s : streams) {
    for (int channel = 0; channel < 2; ++channel) {
      const nc::PerFrame p = nc::PerFrameOf(ray_noise(s.first, s.second), channel);
      CHECK(p.changed > 0.99);                                  // changes at every pixel every frame
      CHECK(p.max_gap < 0.25);                                  // 16 frames leave no gap over a quarter of the circle
      CHECK(std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004);   // uniform
      CHECK(p.step_spread < 1e-4);                              // the same step at every pixel: nothing slides
    }
  }
  // the error after the exponential average of a smooth response has no lattice (the shared helper's test) for a light's disc numbers and for the pair's pick
  const auto response = [](float u1, float u2) { return std::cos(6.2831853 * double(u1) + 0.7) + 0.6 * std::cos(6.2831853 * double(u2)); };
  for (const auto& s : streams) {
    const nc::Resolved r = nc::Resolve(ray_noise(s.first, s.second), response);
    // The shared helper's own wedge ratio over the 24 constant phases a stream's sample index adds is 1.7 to 2.6 with this response (the response's phase against the noise's, not a structure: measured),
    // and its peak 0.4% to 1.1%; a lattice has 14% in one frequency. So: no frequency stands out (under 1.5%) and no direction is a line (under 2.8 times the mean).
    CHECK(r.shape.peak_share < 0.015 && r.shape.wedge_ratio < 2.8);
    CHECK(!nc::IsLattice(r) || r.shape.wedge_ratio < 2.8);
    CHECK(!nc::Crawls(r));
  }
  // THE 8 BIT AVERAGE'S RANDOM ROUNDING (part 2, hlsl/rt_denoise_plain_consumer.hlsli RtDnDither3): the shared blue noise at the light pass's own frame number, the three channels n.x, n.y and frac(n.x + 0.618):
  // the same checks as the rays' streams (AGENT-RULE-NOISE: changes every frame at every pixel, no gap, uniform, nothing slides, no lattice in what the average of the stored values leaves)
  const auto dither_xy = [](int x, int y, uint32_t frame) {
    const hs::float2 u = hs::BlueNoiseFrame2(x, y, frame);
    return std::pair<float, float>(u.x, u.y);
  };
  const auto dither_z = [](int x, int y, uint32_t frame) {
    const hs::float2 u = hs::BlueNoiseFrame2(x, y, frame);
    return std::pair<float, float>(hs::frac(u.x + 0.6180339887f), u.y);
  };
  for (const auto& stream : {std::function<std::pair<float, float>(int, int, uint32_t)>(dither_xy), std::function<std::pair<float, float>(int, int, uint32_t)>(dither_z)}) {
    for (int channel = 0; channel < 2; ++channel) {
      const nc::PerFrame p = nc::PerFrameOf(stream, channel);
      CHECK(p.changed > 0.99 && p.max_gap < 0.25 && std::fabs(p.mean - 0.5) < 0.01 && std::fabs(p.var - 1.0 / 12.0) < 0.004 && p.step_spread < 1e-4);
    }
    const nc::Resolved r = nc::Resolve(stream, response);
    CHECK(r.shape.peak_share < 0.015 && r.shape.wedge_ratio < 2.8);
    CHECK(!nc::Crawls(r));
  }
  // the first versions' gradient noise, as the comparison switch has it, IS a lattice (the check can tell)
  const auto legacy = [](int x, int y, uint32_t frame) {
    const float f = float(frame & 1023u);
    const double a = blue_noise_spectrum::Ign(double(x), double(y)), b = blue_noise_spectrum::Ign(double(y) + 31.0, double(x) + 11.0);
    return std::pair<float, float>(float(std::fmod(a + double(f) * 0.75487766624669, 1.0)), float(std::fmod(b + double(f) * 0.56984029099806, 1.0)));
  };
  CHECK(nc::IsLattice(nc::Resolve(legacy, response)));
}

// ---------------------------------------------------------------------------
// the mask read (the shader's own file as C++)
// ---------------------------------------------------------------------------
// A fake set of images for a mask table: nine channel images of `tw` x `th` (three RGBA images, slot k), the raw images (slots 3..5) a different pattern, and the depth image (slot 10).
struct MaskWorld {
  int tw = 0, th = 0;
  std::vector<float> value[9];   // filtered, by channel
  std::vector<float> raw[9];
  std::vector<float> depth;      // the traced size's linear depth (raw: the vehicle bit may be set)
  int dw = 0, dh = 0;            // the traced size (the depth image's); 0: the mask's (step 1)
};

void Install(const MaskWorld& w) {
  hs::Masks().assign(6, hs::FakeMask{});
  for (uint32_t g = 0; g < 3; ++g) {
    for (int pass = 0; pass < 2; ++pass) {
      hs::FakeMask& m = hs::Masks()[g + (pass == 0 ? 0u : 3u)];
      m.w = w.tw;
      m.h = w.th;
      m.texel.resize(size_t(w.tw) * w.th);
      const std::vector<float>* src = pass == 0 ? w.value : w.raw;
      for (int i = 0; i < w.tw * w.th; ++i) {
        m.texel[size_t(i)] = hs::float4(src[g * 3 + 0][size_t(i)], src[g * 3 + 1][size_t(i)], src[g * 3 + 2][size_t(i)], 1.0f);
      }
    }
  }
  hs::Depths().assign(11, {});
  hs::Depths()[10] = w.depth;
  hs::DepthWidth() = w.dw != 0 ? w.dw : w.tw;
}

hs::RtmTable MakeTable(int images, int tw, int th, bool guided, uint32_t flags = RTM_VALID, uint32_t step = 1, int traced_w = 0, int traced_h = 0) {
  hs::RtmTable t;
  t.step = step;
  t.traced_w = uint32_t(traced_w != 0 ? traced_w : tw * int(step));
  t.traced_h = uint32_t(traced_h != 0 ? traced_h : th * int(step));
  t.images = uint32_t(images);
  t.flags = flags;
  t.width = uint32_t(tw);
  t.height = uint32_t(th);
  t.slot0 = 0;
  t.slot1 = 1;
  t.slot2 = 2;
  t.lin_slot = guided ? 10u : RTM_NO_SLOT;
  t.raw0 = 3;
  t.raw1 = 4;
  t.raw2 = 5;
  t.depth_sigma = rl::kDepthSigma;
  return t;
}

float CleanRef(double v) {
  if (!(v > -1e30 && v < 1e30)) return 1.0f;
  return float(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}

// The brute force of the depth guided bilinear (the shader's own arithmetic in double).
double Reference(const MaskWorld& w, int channel, bool raw, bool guided, int px, int py, int rw, int rh, double t, int step = 1, int traced_w = 0, int traced_h = 0) {
  const std::vector<float>& img = raw ? w.raw[channel] : w.value[channel];
  const int tracedw = traced_w != 0 ? traced_w : w.tw * step, tracedh = traced_h != 0 ? traced_h : w.th * step;
  const int dwidth = w.dw != 0 ? w.dw : w.tw;
  const double ux = ((px + 0.5) * tracedw / rw - 0.5) / step, uy = ((py + 0.5) * tracedh / rh - 0.5) / step;
  const double fx0 = std::floor(ux), fy0 = std::floor(uy);
  const double fx = ux - fx0, fy = uy - fy0;
  double sum = 0.0, weights = 0.0, nearest = 1.0, nearest_w = -1.0;
  for (int k = 0; k < 4; ++k) {
    const int dx = k & 1, dy = k >> 1;
    const int qx = std::min(std::max(int(fx0) + dx, 0), w.tw - 1), qy = std::min(std::max(int(fy0) + dy, 0), w.th - 1);
    const double bil = (dx == 0 ? 1.0 - fx : fx) * (dy == 0 ? 1.0 - fy : fy);
    const double v = CleanRef(img[size_t(qy) * w.tw + qx]);
    double dw = 1.0;
    if (guided && t > 0.0) {
      uint32_t bits;
      const int tx = std::min(qx * step, tracedw - 1), ty = std::min(qy * step, tracedh - 1);
      const float raw_depth = w.depth[size_t(ty) * dwidth + tx];
      std::memcpy(&bits, &raw_depth, 4);
      bits &= ~1u;
      float z;
      std::memcpy(&z, &bits, 4);
      if (!(z > 0.0f)) {
        dw = 0.001;
      } else {
        const double d = (double(z) - t) / std::max(double(rl::kDepthSigma) * t, 1e-4);
        dw = 1.0 / (1.0 + d * d);
      }
    }
    const double wk = bil * dw;
    sum += v * wk;
    weights += wk;
    if (wk > nearest_w) {
      nearest_w = wk;
      nearest = v;
    }
  }
  return weights > 1e-4 ? sum / weights : nearest;
}

void TestMaskRead() {
  Lcg rng;

  // the table's layout against the header's struct
  hs::RtmTable probe = MakeTable(2, 8, 8, true);
  CHECK(RTM_NO_SLOT == lg::kNoLightSlot && RTM_VALID == rl::kMaskFlagValid && RTM_RAW_VIEW == rl::kMaskFlagRawView && RTM_VIEW == rl::kMaskFlagView);
  CHECK(hs::RtmValid(probe) && !hs::RtmValid(MakeTable(0, 8, 8, true)) && !hs::RtmValid(MakeTable(2, 0, 8, true)) && !hs::RtmValid(MakeTable(2, 8, 8, true, 0u)));
  CHECK(hs::RtmSlotOf(probe, 0, false) == 0 && hs::RtmSlotOf(probe, 2, false) == 0 && hs::RtmSlotOf(probe, 3, false) == 1 && hs::RtmSlotOf(probe, 5, false) == 1 && hs::RtmSlotOf(probe, 6, false) == RTM_NO_SLOT);
  CHECK(hs::RtmSlotOf(probe, 4, true) == 4 && hs::RtmSlotOf(probe, 1, true) == 3 && hs::RtmSlotOf(probe, 8, true) == RTM_NO_SLOT);
  CHECK(hs::RtmClean(0.5f) == 0.5f && hs::RtmClean(-1.0f) == 0.0f && hs::RtmClean(4.0f) == 1.0f && hs::RtmClean(std::nanf("")) == 1.0f && hs::RtmClean(INFINITY) == 1.0f && hs::RtmClean(-INFINITY) == 1.0f);
  CHECK(hs::RtmDepthWeight(10.0f, 10.0f, 0.05f) == 1.0f && hs::RtmDepthWeight(0.0f, 10.0f, 0.05f) == 0.001f && hs::RtmDepthWeight(10.5f, 10.0f, 0.05f) < 0.6f && hs::RtmDepthWeight(20.0f, 10.0f, 0.05f) < 0.01f);

  for (int round = 0; round < 60; ++round) {
    MaskWorld w;
    w.tw = 4 + int(rng.Below(20));
    w.th = 4 + int(rng.Below(14));
    for (int c = 0; c < 9; ++c) {
      w.value[c].resize(size_t(w.tw) * w.th);
      w.raw[c].resize(size_t(w.tw) * w.th);
      for (int i = 0; i < w.tw * w.th; ++i) {
        float v = rng.F();
        if (rng.Below(40) == 0) v = std::nanf("");
        if (rng.Below(60) == 0) v = INFINITY;
        if (rng.Below(60) == 0) v = rng.Range(-2.0f, 3.0f);
        w.value[c][size_t(i)] = v;
        w.raw[c][size_t(i)] = rng.F();
      }
    }
    w.depth.resize(size_t(w.tw) * w.th);
    for (float& d : w.depth) {
      d = rng.Range(2.0f, 80.0f);
      if (rng.Below(30) == 0) d = 60000.0f;   // the sky
      if (rng.Below(30) == 0) d = 0.0f;
      uint32_t bits;
      std::memcpy(&bits, &d, 4);
      bits = (bits & ~1u) | (rng.Below(5) == 0 ? 1u : 0u);   // the vehicle bit
      std::memcpy(&d, &bits, 4);
    }
    Install(w);
    const int images = 1 + int(rng.Below(3));
    // the same size: the texel itself (cleaned)
    {
      const hs::RtmTable tb = MakeTable(images, w.tw, w.th, true);
      for (int i = 0; i < 80; ++i) {
        const int px = int(rng.Below(uint32_t(w.tw))), py = int(rng.Below(uint32_t(w.th)));
        const int channel = int(rng.Below(uint32_t(images * 3)));
        const bool raw = rng.Below(3) == 0;
        const float v = hs::RtmVisibility(tb, uint32_t(channel), hs::int2(px, py), float(w.tw), float(w.th), 10.0f, raw);
        const float expect = CleanRef((raw ? w.raw[channel] : w.value[channel])[size_t(py) * w.tw + px]);
        CHECK(v == expect);
      }
    }
    // a larger picture: the depth guided bilinear against brute force; and unguided bilinear without the depth image
    const int rw = w.tw * (1 + int(rng.Below(3))) + int(rng.Below(3)), rh = w.th * (1 + int(rng.Below(3))) + int(rng.Below(3));
    for (int guided = 0; guided < 2; ++guided) {
      const hs::RtmTable tb = MakeTable(images, w.tw, w.th, guided != 0);
      for (int i = 0; i < 80; ++i) {
        const int px = int(rng.Below(uint32_t(rw))), py = int(rng.Below(uint32_t(rh)));
        const int channel = int(rng.Below(uint32_t(images * 3)));
        const bool raw = rng.Below(4) == 0;
        const double t = rng.Below(10) == 0 ? 0.0 : rng.Range(2.0f, 80.0f);
        const float v = hs::RtmVisibility(tb, uint32_t(channel), hs::int2(px, py), float(rw), float(rh), float(t), raw);
        const double expect = Reference(w, channel, raw, guided != 0, px, py, rw, rh, double(float(t)));
        CHECK(std::fabs(double(v) - expect) < 2e-5);
        CHECK(v >= 0.0f && v <= 1.0f);
      }
    }
    // a channel past the images, an invalid table, a missing image: lit
    const hs::RtmTable tb = MakeTable(images, w.tw, w.th, true);
    CHECK(hs::RtmVisibility(tb, uint32_t(images * 3), hs::int2(0, 0), float(w.tw), float(w.th), 10.0f, false) == 1.0f);
    CHECK(hs::RtmVisibility(MakeTable(images, w.tw, w.th, true, 0u), 0, hs::int2(0, 0), float(w.tw), float(w.th), 10.0f, false) == 1.0f);
    hs::RtmTable gone = tb;
    gone.slot0 = RTM_NO_SLOT;
    CHECK(hs::RtmVisibility(gone, 0, hs::int2(0, 0), float(w.tw), float(w.th), 10.0f, false) == 1.0f);
  }

  // a constant image is read back exactly at any size and place, guided or not
  {
    MaskWorld w;
    w.tw = 16;
    w.th = 9;
    for (int c = 0; c < 9; ++c) {
      w.value[c].assign(size_t(w.tw) * w.th, 0.25f + 0.05f * float(c));
      w.raw[c].assign(size_t(w.tw) * w.th, 0.9f);
    }
    w.depth.assign(size_t(w.tw) * w.th, 12.0f);
    Install(w);
    for (int guided = 0; guided < 2; ++guided) {
      const hs::RtmTable tb = MakeTable(3, w.tw, w.th, guided != 0);
      for (int py = 0; py < 18; ++py) {
        for (int px = 0; px < 32; ++px) {
          for (uint32_t c = 0; c < 9; c += 4) {
            CHECK(Near(hs::RtmVisibility(tb, c, hs::int2(px, py), 32.0f, 18.0f, 12.0f, false), 0.25f + 0.05f * float(c), 1e-5));
          }
        }
      }
    }
  }
  // THE HALF SIZE MASK (part 2): a mask of half the traced size, the texel i standing for the traced pixel 2 i. Against the brute force of the same arithmetic, at many pictures and traced sizes (odd ones too:
  // the last texel covers one pixel), and the same size path is taken only by a whole mask the size of the picture.
  for (int round = 0; round < 60; ++round) {
    MaskWorld w;
    w.dw = 8 + int(rng.Below(40));
    w.dh = 8 + int(rng.Below(24));
    w.tw = (w.dw + 1) / 2;
    w.th = (w.dh + 1) / 2;
    for (int c = 0; c < 9; ++c) {
      w.value[c].resize(size_t(w.tw) * w.th);
      w.raw[c].resize(size_t(w.tw) * w.th);
      for (int i = 0; i < w.tw * w.th; ++i) {
        float v = rng.F();
        if (rng.Below(50) == 0) v = std::nanf("");
        w.value[c][size_t(i)] = v;
        w.raw[c][size_t(i)] = rng.F();
      }
    }
    w.depth.resize(size_t(w.dw) * w.dh);
    for (float& d : w.depth) {
      d = rng.Range(2.0f, 80.0f);
      if (rng.Below(30) == 0) d = 60000.0f;
      if (rng.Below(30) == 0) d = 0.0f;
      uint32_t bits;
      std::memcpy(&bits, &d, 4);
      bits = (bits & ~1u) | (rng.Below(5) == 0 ? 1u : 0u);
      std::memcpy(&d, &bits, 4);
    }
    Install(w);
    const int images = 1 + int(rng.Below(3));
    const int rw = w.dw * (1 + int(rng.Below(3))) + int(rng.Below(3)), rh = w.dh * (1 + int(rng.Below(3))) + int(rng.Below(3));
    for (int guided = 0; guided < 2; ++guided) {
      const hs::RtmTable tb = MakeTable(images, w.tw, w.th, guided != 0, RTM_VALID, 2, w.dw, w.dh);
      for (int i = 0; i < 80; ++i) {
        const int px = int(rng.Below(uint32_t(rw))), py = int(rng.Below(uint32_t(rh)));
        const int channel = int(rng.Below(uint32_t(images * 3)));
        const bool raw = rng.Below(4) == 0;
        const double t = rng.Below(10) == 0 ? 0.0 : rng.Range(2.0f, 80.0f);
        const float v = hs::RtmVisibility(tb, uint32_t(channel), hs::int2(px, py), float(rw), float(rh), float(t), raw);
        const double expect = Reference(w, channel, raw, guided != 0, px, py, rw, rh, double(float(t)), 2, w.dw, w.dh);
        CHECK(std::fabs(double(v) - expect) < 2e-5);
        CHECK(v >= 0.0f && v <= 1.0f);
      }
    }
    // a half mask whose size is the picture's own (a picture HALF the traced size: it cannot happen, the traced size is the picture's or less, but the arithmetic must not take the whole path for it): the bilinear
    {
      const hs::RtmTable tb = MakeTable(images, w.tw, w.th, true, RTM_VALID, 2, w.dw, w.dh);
      for (int i = 0; i < 60; ++i) {
        const int px = int(rng.Below(uint32_t(w.tw))), py = int(rng.Below(uint32_t(w.th))), channel = int(rng.Below(uint32_t(images * 3)));
        const double t = rng.Range(2.0f, 80.0f);
        const float v = hs::RtmVisibility(tb, uint32_t(channel), hs::int2(px, py), float(w.tw), float(w.th), float(t), false);
        CHECK(std::fabs(double(v) - Reference(w, channel, false, true, px, py, w.tw, w.th, double(float(t)), 2, w.dw, w.dh)) < 2e-5);
      }
    }
    // a whole mask the size of the picture reads the texel; a half mask over a picture of the traced size never does
    const hs::RtmTable whole = MakeTable(images, w.tw, w.th, true, RTM_VALID, 1, w.tw, w.th);
    CHECK(hs::RtmVisibility(whole, 0, hs::int2(w.tw / 2, w.th / 2), float(w.tw), float(w.th), 10.0f, false) == CleanRef(w.value[0][size_t(w.th / 2) * w.tw + w.tw / 2]));
  }
  // a constant half size mask is read back exactly at any picture size, guided or not; texel i is the traced pixel 2 i: the pixel 2 i of a picture of the traced size reads texel i alone
  {
    MaskWorld w;
    w.dw = 32;
    w.dh = 18;
    w.tw = 16;
    w.th = 9;
    for (int c = 0; c < 9; ++c) {
      w.value[c].assign(size_t(w.tw) * w.th, 0.2f + 0.07f * float(c));
      w.raw[c].assign(size_t(w.tw) * w.th, 0.9f);
    }
    w.depth.assign(size_t(w.dw) * w.dh, 12.0f);
    Install(w);
    for (int guided = 0; guided < 2; ++guided) {
      const hs::RtmTable tb = MakeTable(3, w.tw, w.th, guided != 0, RTM_VALID, 2, w.dw, w.dh);
      for (int py = 0; py < 36; ++py)
        for (int px = 0; px < 64; ++px)
          for (uint32_t c = 0; c < 9; c += 4) CHECK(Near(hs::RtmVisibility(tb, c, hs::int2(px, py), 64.0f, 36.0f, 12.0f, false), 0.2f + 0.07f * float(c), 1e-5));
    }
    // a ramp across the texels: texel i holds i / 15; the pixel 2 i of a picture the size of the traced image is exactly texel i, the pixel 2 i + 1 the mean of texels i and i + 1
    for (int i = 0; i < w.tw * w.th; ++i) {
      for (int c = 0; c < 9; ++c) w.value[c][size_t(i)] = float(i % w.tw) / 15.0f;
    }
    Install(w);
    const hs::RtmTable tb = MakeTable(1, w.tw, w.th, false, RTM_VALID, 2, w.dw, w.dh);
    bool ramp = true;
    for (int i = 0; i < 15; ++i) {
      ramp = ramp && Near(hs::RtmVisibility(tb, 0, hs::int2(2 * i, 6), 32.0f, 18.0f, 10.0f, false), float(i) / 15.0f, 1e-5) &&
             Near(hs::RtmVisibility(tb, 0, hs::int2(2 * i + 1, 6), 32.0f, 18.0f, 10.0f, false), (float(i) + 0.5f) / 15.0f, 1e-5);
    }
    CHECK(ramp);
  }
  // an EDGE in depth: the left half of the traced image is near (z 5, visibility 0.1), the right half far (z 30, visibility 0.9). A near pixel next to the edge reads the near side only, a far one the far side.
  {
    MaskWorld w;
    w.tw = 16;
    w.th = 8;
    for (int c = 0; c < 9; ++c) {
      w.value[c].resize(size_t(w.tw) * w.th);
      w.raw[c].assign(size_t(w.tw) * w.th, 0.5f);
    }
    w.depth.resize(size_t(w.tw) * w.th);
    for (int y = 0; y < w.th; ++y) {
      for (int x = 0; x < w.tw; ++x) {
        for (int c = 0; c < 9; ++c) {
          w.value[c][size_t(y) * w.tw + x] = x < 8 ? 0.1f : 0.9f;
        }
        w.depth[size_t(y) * w.tw + x] = x < 8 ? 5.0f : 30.0f;
      }
    }
    Install(w);
    const hs::RtmTable guided = MakeTable(1, w.tw, w.th, true), plain = MakeTable(1, w.tw, w.th, false);
    // the picture is twice the size: pixels 15 and 16 sit on the two sides of the edge (traced texels 7 and 8)
    const float near_guided = hs::RtmVisibility(guided, 0, hs::int2(15, 4), 32.0f, 16.0f, 5.0f, false);
    const float far_guided = hs::RtmVisibility(guided, 0, hs::int2(16, 4), 32.0f, 16.0f, 30.0f, false);
    const float near_plain = hs::RtmVisibility(plain, 0, hs::int2(15, 4), 32.0f, 16.0f, 5.0f, false);
    CHECK(Near(near_guided, 0.1, 2e-2) && Near(far_guided, 0.9, 2e-2));   // the guide keeps the other side out
    CHECK(near_plain > 0.2f);                                              // without it the edge leaks
    // a pixel whose depth matches no texel at all (a surface in front of both): the nearest weight decides, no NaN, in range
    const float odd = hs::RtmVisibility(guided, 0, hs::int2(15, 4), 32.0f, 16.0f, 500.0f, false);
    CHECK(odd >= 0.1f - 1e-6f && odd <= 0.9f + 1e-6f);
  }

  // THE IDENTITY OF CHECK 1 (VERIFY-RTL, 2026-11-24): taps that all hold one value read back as EXACTLY that value, at any place, size, step and guide, and a weighted mean never leaves the range of the taps
  // that carry weight (so a visibility never reads above its brightest tap: the pass can only take light away, and check 1's 1s are 1). A weighted mean of equal taps is NOT exact in float by itself:
  // sum(v w) / sum(w) with unequal weights lands an ulp off (and a GPU's divide may be an ulp out on top), which the clamp to the taps' range removes.
  {
    int off_value = 0;
    int checked = 0;
    for (int round = 0; round < 80; ++round) {
      MaskWorld w;
      const bool half = (round & 1) != 0;
      if (half) {
        w.dw = 8 + int(rng.Below(40));
        w.dh = 8 + int(rng.Below(24));
        w.tw = (w.dw + 1) / 2;
        w.th = (w.dh + 1) / 2;
      } else {
        w.tw = 4 + int(rng.Below(24));
        w.th = 4 + int(rng.Below(14));
      }
      static const float kValues[] = {1.0f, 0.0f, 0.3f, 0.7f, 0.999f, 0.0039215686f, 0.123456f, 0.9f, 0.5f, 1.0f};
      const float value = round < 40 ? kValues[round % 10] : rng.F();
      for (int c = 0; c < 9; ++c) {
        w.value[c].assign(size_t(w.tw) * w.th, value);
        w.raw[c].assign(size_t(w.tw) * w.th, value);
      }
      const int dw = half ? w.dw : w.tw, dh = half ? w.dh : w.th;
      w.depth.resize(size_t(dw) * dh);
      for (float& d : w.depth) {   // the guides vary (unequal weights): near, far, the sky, none, the vehicle bit
        d = rng.Range(2.0f, 80.0f);
        if (rng.Below(30) == 0) d = 60000.0f;
        if (rng.Below(30) == 0) d = 0.0f;
        uint32_t bits;
        std::memcpy(&bits, &d, 4);
        bits = (bits & ~1u) | (rng.Below(5) == 0 ? 1u : 0u);
        std::memcpy(&d, &bits, 4);
      }
      Install(w);
      const int images = 1 + int(rng.Below(3));
      const int rw = dw * (1 + int(rng.Below(3))) + int(rng.Below(3)), rh = dh * (1 + int(rng.Below(3))) + int(rng.Below(3));
      for (int guided = 0; guided < 2; ++guided) {
        const hs::RtmTable tb = half ? MakeTable(images, w.tw, w.th, guided != 0, RTM_VALID, 2, w.dw, w.dh) : MakeTable(images, w.tw, w.th, guided != 0);
        for (int i = 0; i < 120; ++i) {
          const int px = int(rng.Below(uint32_t(rw))), py = int(rng.Below(uint32_t(rh)));
          const int channel = int(rng.Below(uint32_t(images * 3)));
          const double t = rng.Below(10) == 0 ? 0.0 : rng.Range(2.0f, 80.0f);
          const float v = hs::RtmVisibility(tb, uint32_t(channel), hs::int2(px, py), float(rw), float(rh), float(t), rng.Below(4) == 0);
          ++checked;
          off_value += v == value ? 0 : 1;
        }
      }
    }
    CHECK(checked > 10000);
    CHECK(off_value == 0);   // every read of an all equal neighbourhood is that value, exactly (an all lit one is exactly 1: check 1 is the identity)
  }
  // a mean never leaves its taps: random masks, the read is inside [the smallest tap, the largest tap] of the whole channel image, and 1 only where some tap is 1
  {
    for (int round = 0; round < 40; ++round) {
      MaskWorld w;
      w.tw = 4 + int(rng.Below(20));
      w.th = 4 + int(rng.Below(14));
      float lo = 1.0f, hi = 0.0f;
      for (int c = 0; c < 9; ++c) {
        w.value[c].resize(size_t(w.tw) * w.th);
        w.raw[c].assign(size_t(w.tw) * w.th, 0.5f);
        for (float& v : w.value[c]) {
          v = rng.Range(0.2f, 0.8f);
          if (c == 0) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
          }
        }
      }
      w.depth.resize(size_t(w.tw) * w.th);
      for (float& d : w.depth) d = rng.Range(2.0f, 80.0f);
      Install(w);
      const int rw = w.tw * (1 + int(rng.Below(3))) + int(rng.Below(3)), rh = w.th * (1 + int(rng.Below(3))) + int(rng.Below(3));
      const hs::RtmTable tb = MakeTable(1, w.tw, w.th, true);
      for (int i = 0; i < 200; ++i) {
        const int px = int(rng.Below(uint32_t(rw))), py = int(rng.Below(uint32_t(rh)));
        const float v = hs::RtmVisibility(tb, 0, hs::int2(px, py), float(rw), float(rh), rng.Range(2.0f, 80.0f), false);
        CHECK(v >= lo && v <= hi && v < 1.0f);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the pass's selection and buffer (local_lights_rules.h)
// ---------------------------------------------------------------------------
namespace ll = larecomp::gpu::local_lights;

lg::Light OwnedLamp(float x, uint64_t id) {
  lg::Light l = Lamp(x, 6, 0, id);
  l.flags &= ~lg::kLightFlagGameLit;   // the pass adds it
  l.roles = uint8_t(lg::kRoleMaskGameLit | lg::kRoleMaskSurface);
  return l;
}

void TestSelectionAndBuffer() {
  const float eye[3] = {0.0f, 1.5f, 0.0f};
  // ANNOTATE: a light whose id holds a channel gets the flag and the number; its atlas kind and views stay; one that holds none loses the flag
  {
    ll::Selection sel;
    sel.lights[0] = Headlight(0, 1, 5, 10);
    sel.lights[1] = Lamp(3, 6, 0, 11);
    sel.lights[1].shadow.kind = lg::ShadowKind::kAtlas;
    sel.lights[1].shadow.first_view = 9;
    sel.lights[1].shadow.view_count = 2;
    sel.lights[1].shadow.weight = 0.5f;
    sel.lights[2] = Lamp(6, 6, 0, 12);
    sel.lights[2].flags |= lg::kLightFlagRtVisibility;   // a stale flag from another frame
    sel.lights[2].shadow.mask_channel = 5;
    sel.count = 3;
    rl::Assignment a;
    a.id[0] = 11;
    a.id[4] = 10;
    CHECK(ll::AnnotateSelection(a, &sel) == 2);
    CHECK((sel.lights[0].flags & lg::kLightFlagRtVisibility) != 0 && sel.lights[0].shadow.mask_channel == 4);
    CHECK((sel.lights[1].flags & lg::kLightFlagRtVisibility) != 0 && sel.lights[1].shadow.mask_channel == 0);
    CHECK(sel.lights[1].shadow.kind == lg::ShadowKind::kAtlas && sel.lights[1].shadow.first_view == 9 && sel.lights[1].shadow.view_count == 2 && sel.lights[1].shadow.weight == 0.5f);
    CHECK((sel.lights[2].flags & lg::kLightFlagRtVisibility) == 0);
    CHECK((sel.lights[1].flags & lg::kLightFlagGameLit) != 0 && (sel.lights[0].flags & lg::kLightFlagGameLit) == 0);   // the game's flag is untouched
  }
  // TRIM: a game's light with no source of visibility goes; with views, or with a channel, it stays; a light we own stays; the order and the counters follow
  {
    ll::Selection sel;
    sel.lights[0] = Headlight(0, 1, 5, 1);                          // ours, no source: stays
    sel.lights[1] = Lamp(3, 6, 0, 2);                               // the game's, no source: goes
    sel.lights[2] = Lamp(6, 6, 0, 3);                               // the game's, atlas views: stays
    sel.lights[2].shadow.kind = lg::ShadowKind::kAtlas;
    sel.lights[2].shadow.view_count = 1;
    sel.lights[3] = Lamp(9, 6, 0, 4);                               // the game's, a channel: stays
    sel.lights[3].flags |= lg::kLightFlagRtVisibility;
    sel.lights[4] = Lamp(12, 6, 0, 5);                              // the game's, no source: goes
    sel.lights[5] = OwnedLamp(15, 6);                               // owned, no source: stays
    sel.count = 6;
    sel.from_frame = 1;
    sel.from_extra = 5;
    sel.from_class[size_t(ll::SourceClass::kFrame)] = 1;
    sel.from_class[size_t(ll::SourceClass::kHandedOver)] = 5;
    CHECK(ll::TrimSelection(&sel) == 2);
    CHECK(sel.count == 4 && sel.lights[0].id == 1 && sel.lights[1].id == 3 && sel.lights[2].id == 4 && sel.lights[3].id == 6);
    CHECK(sel.from_frame == 1 && sel.from_extra == 3 && sel.from_class[size_t(ll::SourceClass::kHandedOver)] == 3);
    CHECK(sel.lights[4].id == lg::kNoLightId && sel.lights[5].id == lg::kNoLightId);   // the freed rows are empty
    CHECK(ll::TrimSelection(&sel) == 0);   // nothing more to take out
  }
  // THE WHOLE PICK, as the pass runs it: the frame's headlight, a lamp the atlas picked (views), a lamp handed over for its ray, a lamp nobody picked. K = 2: the headlight and the nearest lamp get channels.
  {
    ll::Selection sel;
    sel.lights[0] = Headlight(0, 1, 8, 100);
    sel.lights[1] = Lamp(4, 6, 0, 201);                            // nearest
    sel.lights[1].shadow.kind = lg::ShadowKind::kAtlas;
    sel.lights[1].shadow.view_count = 2;
    sel.lights[2] = Lamp(12, 6, 0, 202);                           // handed over for its ray only
    sel.lights[3] = Lamp(25, 6, 0, 203);                           // handed over, farther
    sel.count = 4;
    const rl::Pick pick = rl::SelectTopK(sel.lights.data(), sel.count, eye, nullptr, 0, 2);
    CHECK(pick.count == 2 && pick.id[0] == 100 && pick.id[1] == 201);
    const rl::Assigned assigned = rl::AssignChannels(rl::Assignment{}, pick.id.data(), pick.count, rl::ImagesFor(2) * 3);
    CHECK(ll::AnnotateSelection(assigned.now, &sel) == 2);
    CHECK(ll::TrimSelection(&sel) == 2);   // the two lamps nobody gave a source
    CHECK(sel.count == 2 && sel.lights[0].id == 100 && sel.lights[1].id == 201);
    // every light of the pass that the game lights has a source of visibility: the pass never ADDS a game's lamp
    for (uint32_t i = 0; i < sel.count; ++i) {
      CHECK((sel.lights[i].flags & lg::kLightFlagGameLit) == 0 || ll::HasVisibilitySource(sel.lights[i]));
    }
  }
  // random selections: after annotate and trim, no game's light is without a source, the survivors are in their order, and a light that holds a channel is never removed
  Lcg rng;
  for (int round = 0; round < 500; ++round) {
    ll::Selection sel;
    const uint32_t n = 1 + rng.Below(20);
    std::vector<uint64_t> ids;
    for (uint32_t i = 0; i < n; ++i) {
      lg::Light l = rng.Below(3) == 0 ? Headlight(rng.Range(-20, 20), 1, rng.Range(2, 30), 1000 + i) : Lamp(rng.Range(-30, 30), 6, rng.Range(-30, 30), 1000 + i);
      if (rng.Below(4) == 0) {
        l.shadow.kind = lg::ShadowKind::kAtlas;
        l.shadow.view_count = uint8_t(1 + rng.Below(2));
      }
      sel.lights[i] = l;
      ids.push_back(l.id);
    }
    sel.count = n;
    sel.from_extra = n;
    sel.from_class[size_t(ll::SourceClass::kHandedOver)] = n;
    const uint32_t k = 1 + rng.Below(9);
    const rl::Pick pick = rl::SelectTopK(sel.lights.data(), n, eye, nullptr, 0, k);
    const rl::Assigned assigned = rl::AssignChannels(rl::Assignment{}, pick.id.data(), pick.count, rl::ImagesFor(k) * 3);
    ll::AnnotateSelection(assigned.now, &sel);
    ll::Selection before = sel;
    ll::TrimSelection(&sel);
    uint32_t j = 0;
    for (uint32_t i = 0; i < before.count; ++i) {
      const bool kept = !((before.lights[i].flags & lg::kLightFlagGameLit) != 0 && !ll::HasVisibilitySource(before.lights[i]));
      if (kept) {
        CHECK(j < sel.count && sel.lights[j].id == before.lights[i].id);
        ++j;
      } else {
        CHECK(rl::ChannelFor(assigned.now, before.lights[i].id) == rl::kNoChannel);   // never one that holds a channel
      }
    }
    CHECK(j == sel.count);
    for (uint32_t i = 0; i < sel.count; ++i) {
      CHECK((sel.lights[i].flags & lg::kLightFlagGameLit) == 0 || ll::HasVisibilitySource(sel.lights[i]));
    }
  }
  // THE BUFFER: the ray traced table is written only when a selected light reads a channel of valid images; the header's flag says so; the bytes are the table
  {
    ll::PassInputs in;
    in.tan_x = in.tan_y = 1.0f;
    in.width = 1920;
    in.height = 1080;
    rl::MaskFrame frame;
    frame.valid = true;
    frame.images = 2;
    frame.width = 1920;
    frame.height = 1080;
    frame.slot = {41, 42, lg::kNoLightSlot};
    frame.raw_slot = {51, 52, lg::kNoLightSlot};
    frame.lin_slot = 60;
    in.rt_mask = &frame;
    ll::Selection sel;
    sel.lights[0] = Headlight(0, 1, 8, 7);
    sel.lights[0].flags |= lg::kLightFlagRtVisibility;
    sel.lights[0].shadow.mask_channel = 4;   // image 1
    sel.count = 1;
    CHECK(ll::RtTableNeeded(sel, &frame));
    std::vector<uint8_t> bytes(ll::kBufferAlloc, 0xAB);
    ll::BuildBuffer(in, sel, bytes.data());
    ll::PassHeader h;
    std::memcpy(&h, bytes.data(), sizeof(h));
    CHECK((h.flags & ll::kFlagRtMask) != 0);
    rl::GpuRtMaskTable t;
    std::memcpy(&t, bytes.data() + ll::kRtOffset, sizeof(t));
    const rl::GpuRtMaskTable want = rl::BuildMaskTable(frame);
    CHECK(std::memcmp(&t, &want, sizeof(t)) == 0 && t.slot[1] == 42 && t.lin_slot == 60);
    // the light's row carries the flag and the channel
    lg::GpuLight row;
    std::memcpy(&row, bytes.data() + ll::kLightsOffset, sizeof(row));
    const lg::Light back = lg::UnpackGpuLight(row);
    CHECK((back.flags & lg::kLightFlagRtVisibility) != 0 && back.shadow.mask_channel == 4);
    // a channel past the images, no flag on any light, no mask frame, an invalid one: no table and the flag clear and the region zero
    ll::Selection past = sel;
    past.lights[0].shadow.mask_channel = 6;
    ll::Selection unflagged = sel;
    unflagged.lights[0].flags &= ~lg::kLightFlagRtVisibility;
    rl::MaskFrame invalid = frame;
    invalid.valid = false;
    const struct {
      const ll::Selection* s;
      const rl::MaskFrame* f;
      const char* what;
    } none[] = {{&past, &frame, "a channel past the images"}, {&unflagged, &frame, "no light reads a channel"}, {&sel, nullptr, "no mask frame"}, {&sel, &invalid, "an invalid mask frame"}};
    for (const auto& c : none) {
      ll::PassInputs in2 = in;
      in2.rt_mask = c.f;
      std::vector<uint8_t> b2(ll::kBufferAlloc, 0xCD);
      ll::BuildBuffer(in2, *c.s, b2.data());
      ll::PassHeader h2;
      std::memcpy(&h2, b2.data(), sizeof(h2));
      bool zero = true;
      for (uint32_t i = ll::kRtOffset; i < ll::kBufferAlloc; ++i) {
        zero = zero && b2[i] == 0;
      }
      Check((h2.flags & ll::kFlagRtMask) == 0 && zero, std::string("no ray traced table: ") + c.what);
    }
    // the other tables' regions are untouched by the new one
    ll::PassInputs in3 = in;
    in3.rt_mask = nullptr;
    std::vector<uint8_t> b3(ll::kBufferAlloc, 0);
    ll::BuildBuffer(in3, sel, b3.data());
    CHECK(std::memcmp(bytes.data() + ll::kShadowOffset, b3.data() + ll::kShadowOffset, ll::kRtOffset - ll::kShadowOffset) == 0);
  }
  // THE SECOND LINE: a lamp the game lights with no source of visibility at all (no views, no channel; or the flag set but the table is not in this frame's buffer: rt_ref false) is never drawn, whatever
  // its term: the grid added it, the pass never does. An owned lamp in the same state is added as it always was.
  {
    const float world[3] = {2.0f, 0.0f, 1.0f}, normal[3] = {0.0f, 1.0f, 0.0f}, albedo[3] = {0.5f, 0.4f, 0.3f};
    ll::Units units;
    units.exposure = 16.0f;
    const auto nodepth = [](int, int) { return 1.0f; };
    for (int flagged = 0; flagged < 2; ++flagged) {
      lg::Light game = Lamp(1, 6, 1, 8);
      lg::Light own = OwnedLamp(1, 9);
      if (flagged != 0) {
        game.flags |= lg::kLightFlagRtVisibility;
        own.flags |= lg::kLightFlagRtVisibility;
      }
      float out[3] = {-1, -1, -1};
      float term[3];
      ll::PixelLight(game, units, world, normal, albedo, 1.0f, term);
      CHECK(term[0] > 0.0f);   // a lamp that does reach the pixel
      CHECK(!ll::PixelLampShadowed(game, units, nullptr, false, 0, world, normal, albedo, 1.0f, nodepth, out, nullptr, false, 1.0f));
      CHECK(!ll::PixelLampShadowed(game, units, nullptr, false, 0, world, normal, albedo, 1.0f, nodepth, out, nullptr, false, 0.0f));
      CHECK(ll::PixelLampShadowed(own, units, nullptr, false, 0, world, normal, albedo, 1.0f, nodepth, out, nullptr, false, 0.0f) && out[0] > 0.0f);
    }
  }
  // THE TWINS with no atlas in the way (the full lamp and headlight twin checks with the atlas are in lamp_shadows_test.cpp): the composition of the pass is rt_lights::Apply
  {
    const float world[3] = {2.0f, 0.0f, 1.0f}, normal[3] = {0.0f, 1.0f, 0.0f}, albedo[3] = {0.5f, 0.4f, 0.3f};
    ll::Units units;
    units.exposure = 16.0f;
    const auto nodepth = [](int, int) { return 1.0f; };
    for (int round = 0; round < 400; ++round) {
      lg::Light l = rng.Below(2) ? Lamp(rng.Range(-4, 4), 6, rng.Range(-4, 4), 5) : OwnedLamp(rng.Range(-4, 4), 5);
      l.flags |= lg::kLightFlagRtVisibility;
      const float v = rng.Range(-0.2f, 1.2f);
      float term[3], out[3];
      ll::PixelLight(l, units, world, normal, albedo, 1.0f, term);
      if (!(term[0] > 0.0f)) {
        continue;
      }
      ll::PixelLampShadowed(l, units, nullptr, false, 0, world, normal, albedo, 1.0f, nodepth, out, nullptr, true, v);
      float expect[3];
      rl::Apply((l.flags & lg::kLightFlagGameLit) != 0, term, rl::Combine(1.0f, v), expect);
      CHECK(out[0] == expect[0] && out[1] == expect[1] && out[2] == expect[2]);
    }
  }
}

// ---------------------------------------------------------------------------
// the memory (RT-LIGHTS-D1 part 2, C15)
// ---------------------------------------------------------------------------
double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// The occlusion's traced sizes the picture sizes give (the AO's plan: ao_size.h; screen_rules.h: half above 1080 lines, or at the AO's lower quality at any height): the cases the memory is reported for.
struct MemCase {
  const char* name;
  uint32_t render_w, render_h;
  uint32_t traced_w, traced_h;
};
constexpr MemCase kMemCases[] = {
    {"1080p, AO ultra (traced 1920x1080)", 1920, 1080, 1920, 1080}, {"1080p, AO high (traced 960x540)", 1920, 1080, 960, 540}, {"1440p (traced 1280x720)", 2560, 1440, 1280, 720},
    {"4K (traced 1920x1080)", 3840, 2160, 1920, 1080},              {"720p (traced 640x360)", 1280, 720, 640, 360},
};

void TestMemory(bool print) {
  CHECK(rl::kGroupBytes == 20 && rl::kGuideBytes == 8 && rl::kSignalBytes == 4 && rl::kDepthGuideBytes == 2 && rl::kNormalGuideBytes == 4 && rl::kDenoiseImages == 4 && rl::kDenoiseImageBytes == 4);
  CHECK(rl::HeldBytes(1, 1, 1) == 28 && rl::HeldBytes(1, 1, 2) == 48 && rl::HeldBytes(1, 1, 3) == 68 && rl::HeldBytes(0, 100, 2) == 0);
  CHECK(rl::HeldBytesPart1(1, 1, 1) == 60 && rl::HeldBytesPart1(1, 1, 2) == 100 && rl::HeldBytesPart1(1, 1, 3) == 140);
  // the step: auto is half where the traced size is taller than 720 lines, 1 whole, 2 half where there is room, anything else is auto
  CHECK(rl::ClampScale(-3) == 0 && rl::ClampScale(0) == 0 && rl::ClampScale(1) == 1 && rl::ClampScale(2) == 2 && rl::ClampScale(9) == 2);
  CHECK(rl::StepFor(0, 1920, 1080) == 2 && rl::StepFor(0, 1280, 721) == 2 && rl::StepFor(0, 1280, 720) == 1 && rl::StepFor(0, 960, 540) == 1 && rl::StepFor(0, 640, 360) == 1);
  CHECK(rl::StepFor(1, 3840, 2160) == 1 && rl::StepFor(1, 1920, 1080) == 1 && rl::StepFor(2, 960, 540) == 2 && rl::StepFor(2, 640, 360) == 2 && rl::StepFor(2, 255, 1080) == 1 && rl::StepFor(2, 1080, 255) == 1);
  CHECK(rl::StepFor(0, 100, 900) == 1 && rl::StepFor(7, 1920, 1080) == 2 && rl::StepFor(-1, 1920, 1080) == 2);   // too narrow a mask is traced whole; a setting out of its range is auto or 2
  // the mask's extent and the traced pixel a texel samples: every texel's corner is on the traced image, the last texel may cover fewer pixels
  CHECK(rl::MaskExtent(1920, 1) == 1920 && rl::MaskExtent(1920, 2) == 960 && rl::MaskExtent(1921, 2) == 961 && rl::MaskExtent(1, 2) == 1 && rl::MaskExtent(0, 2) == 0 && rl::MaskExtent(7, 0) == 7);
  for (uint32_t traced : {1u, 2u, 3u, 539u, 540u, 541u, 1080u, 1081u}) {
    for (uint32_t step : {1u, 2u}) {
      const uint32_t n = rl::MaskExtent(traced, step);
      bool on_image = true, covers = true;
      for (uint32_t i = 0; i < n; ++i) on_image = on_image && rl::TracedOf(i, step, traced) < traced && rl::TracedOf(i, step, traced) == i * step;
      covers = (n - 1) * step < traced && n * step >= traced;   // the blocks tile the traced image
      CHECK(on_image && covers);
    }
  }
  CHECK(rl::TracedOf(5, 2, 7) == 6 && rl::TracedOf(100, 2, 7) == 6 && rl::TracedOf(3, 1, 10) == 3 && rl::TracedOf(0, 2, 0) == 0);
  CHECK(rl::DenoiseIterations(1) == 2 && rl::DenoiseIterations(2) == 1);
  // THE NUMBERS, K 3 (one group), K 6, K 9, against part 1's layout at the same traced size (the old doc said 116 MiB at a traced 1080 lines: its table counted the signal twice and part 1's guides 28 bytes; the
  // formats are what they are: 8 + 4 x 8 for the group, 8 + 8 + 4 for the guides = 60 bytes a traced pixel for K 3)
  if (print) std::printf("MEMORY (MiB, masks only: the rows and counters add 3 KiB):\n  %-38s %8s %8s | %8s %8s %8s %8s\n", "case", "mask", "step", "K3 now", "K6 now", "K9 now", "K3 part1");
  for (const MemCase& c : kMemCases) {
    const uint32_t step = rl::StepFor(0, c.traced_w, c.traced_h);
    const uint32_t mw = rl::MaskExtent(c.traced_w, step), mh = rl::MaskExtent(c.traced_h, step);
    const uint64_t k3 = rl::HeldBytes(mw, mh, 1), k6 = rl::HeldBytes(mw, mh, 2), k9 = rl::HeldBytes(mw, mh, 3);
    const uint64_t old3 = rl::HeldBytesPart1(c.traced_w, c.traced_h, 1), old6 = rl::HeldBytesPart1(c.traced_w, c.traced_h, 2);
    if (print) std::printf("  %-38s %4ux%-4u %8u | %8.1f %8.1f %8.1f %8.1f   (part 1: K6 %.1f)\n", c.name, mw, mh, step, Mib(k3), Mib(k6), Mib(k9), Mib(old3), Mib(old6));
    CHECK(Mib(k3) < 25.0 && Mib(k6) < 45.0 && Mib(k9) < 65.0);                      // under a quarter of the part 1's worst case in every picture size
    CHECK(k3 * 2 <= old3 && k6 * 2 <= old6);                                         // at least half of part 1's at the same traced size, in every case (the packing alone is 2.1 times)
    CHECK(mh <= 720 || step == 1u);                                                  // the mask is never taller than 720 lines unless it is the whole traced size of a short picture
  }
  {
    // the headline cases against the 8 GB card's budget: K 3 at 1080p (both occlusion plans), 1440p and 4K
    const uint64_t a = rl::HeldBytes(960, 540, 1), b = rl::HeldBytes(1280, 720, 1);
    CHECK(Near(Mib(a), 13.84, 2e-3) && Near(Mib(b), 24.61, 2e-3));
    CHECK(Near(Mib(rl::HeldBytesPart1(1920, 1080, 1)), 118.65, 2e-3) && Near(Mib(rl::HeldBytesPart1(1280, 720, 1)), 52.73, 2e-3));
  }
}

// ---------------------------------------------------------------------------
// the 8 bit history (hlsl/rt_denoise_unorm8.hlsli against rt_lights_rules.h Round8 / Store8)
// ---------------------------------------------------------------------------
// One frame of the temporal pass's average over an 8 bit image: the history as stored, the sample, the length n; `u` the pixel's number this frame (negative: the hardware's rounding to the nearest level).
float Average8(float history, float sample, float n, float u) {
  const float weight = n / (n + 1.0f);
  const float result = sample + (history - sample) * weight;
  return u < 0.0f ? rl::Round8(result) : rl::Store8(result, u);
}
// The same, the way the shader does it where it also adds one level toward the sample when the result rounds to the history (measured and dropped: the bias of a noisy signal).
float AverageNudged8(float history, float sample, float n) {
  const float weight = n / (n + 1.0f);
  const float result = sample + (history - sample) * weight;
  const float r = rl::Round8(result), h = rl::Round8(history), c = rl::Round8(sample);
  if (r == h && c != h) return c > h ? std::min(h + 1.0f / 255.0f, 1.0f) : std::max(h - 1.0f / 255.0f, 0.0f);
  return r;
}

void TestUnorm8(bool print) {
  Lcg rng;
  // the file's functions are the twins
  bool same = true;
  for (int i = 0; i < 100000; ++i) {
    const float v = rng.Range(-0.5f, 1.5f), u = rng.F();
    same = same && hs::RtDnRound8(v) == rl::Round8(v) && hs::RtDnStore8(v, u) == rl::Store8(v, u);
  }
  CHECK(same);
  CHECK(hs::RtDnRound8(0.0f) == 0.0f && hs::RtDnRound8(1.0f) == 1.0f && hs::RtDnRound8(-3.0f) == 0.0f && hs::RtDnRound8(9.0f) == 1.0f && Near(hs::RtDnRound8(0.5f), 128.0f / 255.0f, 1e-6));
  CHECK(Near(hs::kRtDn8LengthScale, rl::kLengthScale) && hs::kRtDn8Levels == 255.0f);
  // THE RANDOM ROUNDING: the stored value is one of the two levels round the value, and its expectation over u is the value (the mean of an even spread of u is exact to a
  // thousandth of a level); the range is kept
  {
    bool levels = true, in_range = true;
    double worst_mean = 0.0;
    for (int i = 0; i < 2000; ++i) {
      const float v = rng.Range(-0.3f, 1.3f);
      const double clamped = std::min(std::max(double(v), 0.0), 1.0);
      double sum = 0.0;
      const int n = 4096;
      for (int k = 0; k < n; ++k) {
        const float s = rl::Store8(v, (float(k) + 0.5f) / float(n));
        sum += s;
        in_range = in_range && s >= 0.0f && s <= 1.0f;
        levels = levels && std::fabs(double(s) - clamped) < 1.0 / 255.0 + 1e-6;
      }
      worst_mean = std::max(worst_mean, std::fabs(sum / n - clamped));
    }
    CHECK(levels && in_range && worst_mean < 2e-5);
    if (print) std::printf("8 BIT RANDOM ROUNDING: the mean of the stored value over an even spread of u is within %.2e of the value (one level = %.2e)\n", worst_mean, 1.0 / 255.0);
  }
  // the length in the alpha: exact to a twentieth of a frame after the 8 bit rounding, saturated at the cap, monotone
  {
    double worst = 0.0;
    bool monotone = true;
    float last = -1.0f;
    for (int n = 0; n <= 40; ++n) {
      const float stored = hs::RtDnRound8(hs::RtDnEncodeLength(float(n)));
      const float back = hs::RtDnDecodeLength(stored);
      if (n <= 32) worst = std::max(worst, std::fabs(double(back) - double(n)));
      monotone = monotone && back >= last;
      last = back;
    }
    CHECK(worst < 0.07 && monotone);
    CHECK(hs::RtDnEncodeLength(32.0f) == 1.0f && hs::RtDnEncodeLength(100.0f) == 1.0f && hs::RtDnEncodeLength(0.0f) == 0.0f && hs::RtDnDecodeLength(1.0f) == 32.0f && hs::RtDnDecodeLength(0.0f) == 0.0f);
    if (print) std::printf("8 BIT LENGTH: n / 32 in the alpha, the round trip is within %.3f frames for n 0..32\n", worst);
  }
  // THE STALL: with the nearest rounding the average stops short of its target by a few per cent for ever; with the random rounding every start reaches the target to within a level or two
  {
    double worst_plain = 0.0, worst_dither = 0.0, worst_tail = 0.0, bias_sum = 0.0;
    int stalled = 0, total = 0;
    for (int start = 0; start <= 255; ++start) {
      for (float target : {0.0f, 1.0f, 128.0f / 255.0f, 64.0f / 255.0f, 230.0f / 255.0f, 0.37f}) {
        float plain = float(start) / 255.0f, dithered = plain;
        double tail = 0.0;
        for (int frame = 0; frame < 300; ++frame) {
          const float n = float(std::min(frame + 1, 32));
          plain = Average8(plain, target, n, -1.0f);
          dithered = Average8(dithered, target, n, rng.F());
          if (frame >= 200) tail += dithered;
        }
        ++total;
        worst_plain = std::max(worst_plain, std::fabs(double(plain) - double(target)));
        worst_dither = std::max(worst_dither, std::fabs(double(dithered) - double(target)));
        worst_tail = std::max(worst_tail, std::fabs(tail / 100.0 - double(target)));
        bias_sum += tail / 100.0 - double(target);
        stalled += std::fabs(double(plain) - double(target)) > 0.02 ? 1 : 0;
      }
    }
    if (print) std::printf("8 BIT AVERAGE: constant sample, n up to 32, 300 frames from every start level: nearest rounding ends up to %.3f from the target (%d of %d runs more than 0.02 off); random rounding ends up to %.4f from it "
                           "(one level = %.4f), the mean of a run's last 100 frames up to %.4f from it (a run toward 0 or 1 may sit on the last level for tens of frames: the average of the runs is %.6f off)\n", worst_plain, stalled, total, worst_dither, 1.0 / 255.0, worst_tail, bias_sum / double(total));
    CHECK(worst_plain > 0.03 && stalled > total / 10);      // the stall is real: the rule is needed
    CHECK(worst_dither <= 2.0 / 255.0 + 1e-6);              // ... and the random rounding removes it: within two levels at any frame (the noise of the rounding is one level)
    CHECK(worst_tail <= 1.01 / 255.0 && std::fabs(bias_sum / double(total)) < 0.05 / 255.0);   // ... and it is unbiased: the runs' mean sits on the target (one run may be on the last level for a while)
  }
  // a noisy sample (a binary visibility of probability p): the long run mean of the stored value. Random rounding is unbiased; the nearest rounding and the one level nudge (measured, dropped) are not.
  {
    double worst_bias = 0.0, worst_plain_bias = 0.0, worst_nudge_bias = 0.0;
    for (double p : {0.02, 0.05, 0.25, 0.5, 0.8, 0.95, 0.98}) {
      float plain = 1.0f, dithered = 1.0f, nudged = 1.0f;
      double sum_plain = 0.0, sum_dither = 0.0, sum_nudge = 0.0;
      bool in_range = true;
      const int frames = 120000, burn = 4000;
      for (int frame = 0; frame < frames; ++frame) {
        const float n = float(std::min(frame + 1, 32));
        const float sample = double(rng.F()) < p ? 1.0f : 0.0f;
        plain = Average8(plain, sample, n, -1.0f);
        dithered = Average8(dithered, sample, n, rng.F());
        nudged = AverageNudged8(nudged, sample, n);
        in_range = in_range && dithered >= 0.0f && dithered <= 1.0f;
        if (frame >= burn) {
          sum_plain += plain;
          sum_dither += dithered;
          sum_nudge += nudged;
        }
      }
      const double mean_plain = sum_plain / double(frames - burn), mean_dither = sum_dither / double(frames - burn), mean_nudge = sum_nudge / double(frames - burn);
      if (print) std::printf("8 BIT AVERAGE: binary visibility p = %.2f: mean of the stored value %.4f with random rounding, %.4f with nearest rounding, %.4f with the one level nudge\n", p, mean_dither, mean_plain, mean_nudge);
      CHECK(in_range);
      worst_bias = std::max(worst_bias, std::fabs(mean_dither - p));
      worst_plain_bias = std::max(worst_plain_bias, std::fabs(mean_plain - p));
      worst_nudge_bias = std::max(worst_nudge_bias, std::fabs(mean_nudge - p));
    }
    if (print) std::printf("8 BIT AVERAGE: the worst bias over the p: random rounding %.4f, nearest rounding %.4f, one level nudge %.4f\n", worst_bias, worst_plain_bias, worst_nudge_bias);
    CHECK(worst_bias < 0.003);                                   // unbiased (the 120000 frames' own scatter)
    CHECK(worst_plain_bias > 0.015 && worst_nudge_bias > 0.03);   // what the alternatives do
  }
}

// ---------------------------------------------------------------------------
// the half size trace: the filter and the read-back against the whole size (a 1D model of a soft shadow edge along the pixel row)
// ---------------------------------------------------------------------------
// The visibility along a row across the penumbra of a disc light behind a straight edge: DiscShare (above) of the distance to the edge over the half width. x in pixels of the PICTURE.
double EdgeVisibility(double x, double edge, double half_width) { return DiscShare((x - edge) / half_width); }

struct Model1D {
  double max_vs_part1 = 0.0, mean_vs_part1 = 0.0, max_vs_truth = 0.0, max_part1_vs_truth = 0.0;
};
// A mask of `ratio` picture pixels a texel (the texel j stands for the picture pixel ratio x j, as the trace's corner of the block does), filtered by the B3 a-trous `iterations` times (steps 1, 2) and read back
// at every picture pixel by the shader's bilinear (the depth guide is all equal here: the weights are the bilinear's).
std::vector<double> RowAtRatio(double edge, double half_width, int pixels, int ratio, int iterations) {
  const int n = (pixels + ratio - 1) / ratio;
  std::vector<double> cur;
  cur.resize(size_t(n));
  for (int j = 0; j < n; ++j) cur[size_t(j)] = EdgeVisibility(double(j * ratio) + 0.5, edge, half_width);   // the signal's expected value (the temporal average of the noisy rays)
  for (int it = 0; it < iterations; ++it) {
    std::vector<double> next;
    next.resize(size_t(n));
    const int step = 1 << it;
    for (int j = 0; j < n; ++j) {
      double sum = 0.0, wsum = 0.0;
      for (int d = -2; d <= 2; ++d) {
        const int q = j + d * step;
        if (q < 0 || q >= n) continue;
        const double wk = mcla::gpu::rt_denoise::Kernel1(d);
        sum += wk * cur[size_t(q)];
        wsum += wk;
      }
      next[size_t(j)] = sum / wsum;
    }
    cur = next;
  }
  std::vector<double> out;
  out.resize(size_t(pixels));
  for (int i = 0; i < pixels; ++i) {
    // the pixel's place in texels: (i + 0.5 - 0.5) / ratio (the corner of the block is the texel's own pixel), clamped at the ends
    const double u = double(i) / double(ratio);
    const int j0 = std::min(int(std::floor(u)), n - 1), j1 = std::min(j0 + 1, n - 1);
    const double f = u - std::floor(u);
    out[size_t(i)] = cur[size_t(j0)] * (1.0 - f) + cur[size_t(j1)] * f;
  }
  return out;
}

Model1D CompareRows(double half_width, int ratio_now, int iterations_now, int ratio_old, int iterations_old) {
  Model1D m;
  const int pixels = 480;
  int count = 0;
  for (double edge = 200.0; edge < 204.0; edge += 0.25) {
    const std::vector<double> now = RowAtRatio(edge, half_width, pixels, ratio_now, iterations_now);
    const std::vector<double> old = RowAtRatio(edge, half_width, pixels, ratio_old, iterations_old);
    for (int i = 100; i < 380; ++i) {
      const double truth = EdgeVisibility(double(i) + 0.5, edge, half_width);
      m.max_vs_part1 = std::max(m.max_vs_part1, std::fabs(now[size_t(i)] - old[size_t(i)]));
      m.mean_vs_part1 += std::fabs(now[size_t(i)] - old[size_t(i)]);
      m.max_vs_truth = std::max(m.max_vs_truth, std::fabs(now[size_t(i)] - truth));
      m.max_part1_vs_truth = std::max(m.max_part1_vs_truth, std::fabs(old[size_t(i)] - truth));
      ++count;
    }
  }
  m.mean_vs_part1 /= double(count);
  return m;
}

// The 2D kernel's number of effective samples (1 / the sum of squared weights) of `iterations` a-trous passes: how many rays the filter's footprint averages.
double EffectiveSamples(int iterations) {
  std::vector<double> kernel{1.0};
  for (int it = 0; it < iterations; ++it) {
    const int step = 1 << it;
    std::vector<double> next(kernel.size() + size_t(4 * step), 0.0);
    for (size_t a = 0; a < kernel.size(); ++a)
      for (int d = -2; d <= 2; ++d) next[a + size_t(2 * step + d * step)] += kernel[a] * mcla::gpu::rt_denoise::Kernel1(d);
    kernel = next;
  }
  double sum2 = 0.0;
  for (double k : kernel) sum2 += k * k;
  return 1.0 / (sum2 * sum2);   // the 2D kernel is the product of two 1D ones: its sum of squares is the square of the 1D one
}

void TestHalfSize(bool print) {
  // the 1D model against the two changed cases: 1080p with the AO's plan ultra (the mask goes from the whole traced size to half: ratio 1 -> 2, the iterations 2 -> 1) and 4K (ratio 2 -> 4, 2 -> 1).
  // The penumbra's half width is in PICTURE pixels: a pole's shadow a metre off the ground is 10 pixels, a kerb's contact shadow one.
  if (print) std::printf("HALF SIZE, 1D soft edge (expected values: noise is separate): half width px | 1080p ultra: max |new - part1|, mean, max |new - truth| (part 1: max |part1 - truth|) | 4K: the same\n");
  double worst_a = 0.0, worst_b = 0.0, worst_a_wide = 0.0, worst_b_wide = 0.0;
  for (double w : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
    const Model1D a = CompareRows(w, 2, 1, 1, 2);
    const Model1D b = CompareRows(w, 4, 1, 2, 2);
    if (print) std::printf("  %5.1f | %.3f %.4f %.3f (%.3f) | %.3f %.4f %.3f (%.3f)\n", w, a.max_vs_part1, a.mean_vs_part1, a.max_vs_truth, a.max_part1_vs_truth, b.max_vs_part1, b.mean_vs_part1, b.max_vs_truth, b.max_part1_vs_truth);
    worst_a = std::max(worst_a, a.max_vs_part1);
    worst_b = std::max(worst_b, b.max_vs_part1);
    if (w >= 4.0) {
      worst_a_wide = std::max(worst_a_wide, a.max_vs_part1);
      worst_b_wide = std::max(worst_b_wide, b.max_vs_part1);
    }
  }
  // the bounds are what the model measures with a margin: the changed cases differ from part 1's pipeline by under 0.12 at the worst edge (a hard one pixel step) and by under 0.05 where the penumbra is
  // 8 pixels wide or more (the shadows that are soft by nature); the mean difference is under a per cent for every width
  CHECK(worst_a < 0.12 && worst_a_wide < 0.05);
  CHECK(worst_b < 0.16 && worst_b_wide < 0.06);
  // a constant visibility is read back exactly at every ratio (the kernel is normalised, the bilinear is a partition of one)
  for (int ratio : {1, 2, 4}) {
    for (int iterations : {1, 2}) {
      const std::vector<double> row = RowAtRatio(-1e9, 1.0, 97, ratio, iterations);   // all lit
      bool exact = true;
      for (double v : row) exact = exact && Near(v, 1.0, 1e-12);
      CHECK(exact);
    }
  }
  // the noise the filter leaves: the rays a filter footprint averages (the kernel's effective samples), in picture pixels: whole size with two iterations against half size with one
  const double n2 = EffectiveSamples(2), n1 = EffectiveSamples(1);
  const double rays_whole = n2;                    // a ray a pixel: the footprint holds n2 rays and n2 pixels
  const double rays_half_picture = n1 * 4.0;       // a ray every four pixels: the footprint holds n1 rays and 4 n1 picture pixels
  if (print) std::printf("HALF SIZE, NOISE: the filter averages %.1f rays at the whole size with two iterations and %.1f at half size with one (the footprints cover %.0f and %.0f picture pixels): the standard deviation of one frame's filtered visibility is %.2f times the whole size's; the temporal average (up to 32 frames) divides both alike\n",
                         n2, n1, rays_whole * 1.0, rays_half_picture, std::sqrt(n2 / n1));
  CHECK(n2 > n1 && std::sqrt(n2 / n1) < 3.0);
}

// ---------------------------------------------------------------------------
// the raw frame (RT-LIGHTS-D1 part 2: the Ray Reconstruction hook) and the table's new fields
// ---------------------------------------------------------------------------
void TestRawFrame() {
  CHECK(offsetof(rl::GpuRtHeader, step) == 28 && offsetof(rl::GpuRtHeader, traced_width) == 48 && offsetof(rl::GpuRtHeader, traced_height) == 52 && sizeof(rl::GpuRtHeader) == 64);
  CHECK(rl::kMaskFlagRrRaw == 8u && (rl::kMaskFlagRrRaw & (rl::kMaskFlagValid | rl::kMaskFlagRawView | rl::kMaskFlagView)) == 0u);
  // a filtered frame: the table's slots are RtDenoise's results, the raw slots the signal's; a raw frame: both are the signal's and the flag says so
  rl::MaskInputs in;
  in.images = 2;
  in.width = 960;
  in.height = 540;
  in.step = 2;
  in.traced_width = 1920;
  in.traced_height = 1080;
  in.lin_slot = 40;
  in.signal_slot = {10, 11, 12};
  in.result_slot = {20, 21, 22};
  const rl::MaskFrame filtered = rl::MakeMaskFrame(in);
  CHECK(filtered.valid && filtered.images == 2 && filtered.slot[0] == 20 && filtered.slot[1] == 21 && filtered.slot[2] == lg::kNoLightSlot && filtered.raw_slot[0] == 10 && filtered.raw_slot[1] == 11 && !filtered.rr_raw);
  CHECK(filtered.step == 2 && filtered.width == 960 && filtered.traced_width == 1920 && filtered.traced_height == 1080 && filtered.lin_slot == 40);
  in.raw = true;
  const rl::MaskFrame raw = rl::MakeMaskFrame(in);
  CHECK(raw.valid && raw.slot[0] == 10 && raw.slot[1] == 11 && raw.raw_slot[0] == 10 && raw.raw_slot[1] == 11 && raw.rr_raw && raw.step == 2 && raw.lin_slot == 40);
  const rl::GpuRtMaskTable tf = rl::BuildMaskTable(filtered), tr = rl::BuildMaskTable(raw);
  CHECK(rl::MaskTableValid(tf) && rl::MaskTableValid(tr) && (tf.flags & rl::kMaskFlagRrRaw) == 0u && (tr.flags & rl::kMaskFlagRrRaw) != 0u && tf.step == 2 && tr.step == 2 && tf.traced_width == 1920 && tf.traced_height == 1080);
  CHECK(tf.slot[0] == 20 && tr.slot[0] == 10 && tr.slot[1] == 11 && tr.depth_sigma == rl::kDepthSigma);
  // a table with no traced size takes the mask's size times the step (the tables of part 1); a step that is not 1 or 2 is 1
  rl::MaskFrame legacy = filtered;
  legacy.traced_width = legacy.traced_height = 0;
  const rl::GpuRtMaskTable tl = rl::BuildMaskTable(legacy);
  CHECK(tl.traced_width == 1920 && tl.traced_height == 1080);
  legacy.step = 7;
  CHECK(rl::BuildMaskTable(legacy).step == 1);
  // what a raw frame does and does not
  const rl::RawPlan off = rl::RawPlanFor(false), on = rl::RawPlanFor(true);
  CHECK(off.trace && off.filter && off.history_written && !off.light_pass_reads_signal && !off.hit_distance_for_guides);
  CHECK(on.trace && !on.filter && !on.history_written && on.light_pass_reads_signal && !on.hit_distance_for_guides);
  // the composition does not know the difference: a raw visibility is a number in [0, 1] like any other and a game's lamp is still only taken away from (the identity holds with a 1 spp 0 / 1 signal)
  for (float v : {0.0f, 1.0f, 0.25f, 0.75f}) {
    const float term[3] = {3.0f, 2.0f, 1.0f};
    float game[3], own[3];
    rl::Apply(true, term, v, game);
    rl::Apply(false, term, v, own);
    for (int i = 0; i < 3; ++i) CHECK(game[i] <= 0.0f && game[i] >= -term[i] && own[i] >= 0.0f && own[i] <= term[i]);
  }
}

// ---------------------------------------------------------------------------
// the text of the shader and of the sources
// ---------------------------------------------------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
// "static const uint kName = 12u;" -> 12 (a float too, "1u << 3" too); -1 where the name is not there.
double ShaderNumber(const std::string& text, const std::string& name) {
  const size_t at = text.find(" " + name + " = ");
  if (at == std::string::npos) {
    return -1.0;
  }
  const size_t from = at + name.size() + 4;
  if (text.compare(from, 2, "0x") == 0) {
    return double(std::stoul(text.substr(from + 2), nullptr, 16));
  }
  if (text.compare(from, 4, "1u <") == 0) {
    return double(1u << std::stoul(text.substr(from + 6)));
  }
  return std::stod(text.substr(from));
}

void TestShaderText(const std::string& src) {
  const std::string cs = ReadFile(src + "/gpu/hlsl/rt_lights_trace_cs.hlsl");
  const std::string common = ReadFile(src + "/gpu/hlsl/rt_lights_common.hlsli");
  const std::string read = ReadFile(src + "/gpu/hlsl/rt_mask_read.hlsli");
  CHECK(!cs.empty() && !common.empty() && !read.empty());
  // the numbers of the header, in the shader (the includer's kFlag* and kStat* and the common file's)
  CHECK(ShaderNumber(cs, "kFlagStats") == rl::kFlagStats && ShaderNumber(cs, "kFlagCheckVisible") == rl::kFlagCheckVisible && ShaderNumber(cs, "kFlagCheckBlocked") == rl::kFlagCheckBlocked &&
        ShaderNumber(cs, "kFlagBlue") == rl::kFlagBlue);
  CHECK(Near(ShaderNumber(common, "kRtvMinNdotL"), rl::kMinNdotL) && Near(ShaderNumber(common, "kRtvMinAbove"), rl::kMinAbove) && Near(ShaderNumber(common, "kRtvLiftFloor"), rl::kLiftFloor) &&
        Near(ShaderNumber(common, "kRtvLiftPixels"), rl::kLiftPixels) && Near(ShaderNumber(common, "kRtvLiftCap"), rl::kLiftCap));
  CHECK(ShaderNumber(common, "kRtvMaxLights") == rl::kMaxLights && ShaderNumber(common, "kRtvMaxRays") == rl::kMaxRays && ShaderNumber(common, "kRtvFlagSpot") == rl::kGpuFlagSpot &&
        ShaderNumber(common, "kRtvFlagPair") == rl::kGpuFlagPair);
  CHECK(ShaderNumber(cs, "kSky") == 60000.0);
  CHECK(Has(read, "#define RTM_VALID 1u") && Has(read, "#define RTM_VIEW 4u") && Has(read, "#define RTM_RAW_VIEW 2u"));
  // the layout the host class builds
  CHECK(Has(cs, "ByteAddressBuffer g_rows : register(t0, space1);") && Has(cs, "RWTexture2D<float4> g_signal0 : register(u1, space1);") && Has(cs, "RWTexture2D<float4> g_signal2 : register(u3, space1);") &&
        Has(cs, "RWTexture2D<float> g_depth : register(u4, space1);") && Has(cs, "RWTexture2D<float4> g_normal : register(u5, space1);") && !Has(cs, "g_hit") &&
        Has(cs, "RWByteAddressBuffer g_stats : register(u6, space1);") && Has(cs, "ConstantBuffer<RtvPush> g_push : register(b0, space3);") && Has(cs, "#define CAR_RT_SPACE 2") &&
        Has(cs, "#define RT_WORLD_SPACE 2") && Has(cs, "Texture2D<float4> g_textures[] : register(t0, space0);"));
  // the formats Vulkan takes from the shader: 8 bit signals, R16F depth, the normal and share in an RGBA8 snorm image (rt_lights.cpp EnsureImages makes them these)
  CHECK(Has(cs, "[[vk::image_format(\"rgba8\")]] RWTexture2D<float4> g_signal0") && Has(cs, "[[vk::image_format(\"rgba8\")]] RWTexture2D<float4> g_signal2") && Has(cs, "[[vk::image_format(\"r16f\")]] RWTexture2D<float> g_depth") &&
        Has(cs, "[[vk::image_format(\"rgba8snorm\")]] RWTexture2D<float4> g_normal") && !Has(cs, "rgba16f") && !Has(cs, "r32f") && !Has(cs, "rg16f"));
  // REVIEW RC-3: D3D12's table makes a typed UAV STORE to R8G8B8A8_SNORM required hardware support and a typed UAV LOAD from it optional (Vulkan: STORAGE_IMAGE is mandatory for the format, no extended
  // format feature needed): the trace only ever stores to the normal and share image (the filter reads it as a texture, where the format's load is required), so no load of the UAV is there
  CHECK(cs.find("g_normal[") != std::string::npos && cs.find("g_normal[") == cs.rfind("g_normal[") && Has(cs, "g_normal[id.xy] = float4(normal, signal ? 1.0 : 0.0);") && !Has(cs, "= g_normal[") && !Has(cs, "g_normal.Load"));
  // the rows' header and a light's row as the host writes them (rt_lights_rules.h): the mask's size, the step and the occlusion's traced size
  CHECK(Has(cs, "g_rows.Load4(0)") && Has(cs, "g_rows.Load4(16)") && Has(cs, "g_rows.Load4(32)") && Has(cs, "g_rows.Load4(48)") && Has(cs, "const uint step = clamp(h1.w, 1u, 2u);") &&
        Has(cs, "const int2 p = int2(min(id.x * step, size.x - 1u), min(id.y * step, size.y - 1u));") && Has(cs, "ViewPos(float2(p) + 0.5, float2(size), t, tanHalf)") && Has(cs, "NormalFromDepth(p, int2(size), t, P, tanHalf)"));
  CHECK(Has(cs, "g_rows.Load4(0)") && Has(cs, "g_rows.Load4(16)") && Has(cs, "g_rows.Load4(32)") && Has(cs, "const uint at = 64u + i * 64u;") && Has(cs, "g_rows.Load4(at + 48u)"));
  // no colour target and no write to anything of the game's: the pass writes its own images and counters
  CHECK(!Has(cs, "SV_Target") && !Has(cs, "RWTexture2DArray"));
  // the shared per-frame blue noise, at the light's own sample, with the debug comparison; no per-pixel hash of a lattice
  CHECK(Has(cs, "#include \"blue_noise_frame.hlsli\"") && Has(cs, "BlueNoiseFrame2(int(pixel.x), int(pixel.y), sampleIndex)") && Has(cs, "RtvSampleIndex(frame, i, kRtvSlotPair)") &&
        Has(cs, "RtvSampleIndex(frame, i, kRtvSlotCoin)") && Has(cs, "RtvSampleIndex(frame, i, k)") && Has(cs, "kFlagBlue"));
  // a ray: accept the first hit, the city's mask and the cars' parts, the stop short from the light's row
  CHECK(Has(cs, "RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH") && Has(cs, "RTV_CITY_MASK | CAR_RT_MASK_ALL") && Has(cs, "RtvMakeRay(P, N, lift, lampPoint, r2.w, disc, stopShort)"));
  // the identity checks and the guides
  CHECK(Has(cs, "(flags & kFlagCheckVisible) != 0u") && Has(cs, "(flags & kFlagCheckBlocked) != 0u") && Has(cs, "g_normal[id.xy] = float4(normal, signal ? 1.0 : 0.0);") && Has(cs, "g_depth[id.xy] = world ? t : 0.0;"));
  // the noise is indexed by the mask texel (the blue noise tile at the texel), the signals are written at the texel
  CHECK(Has(cs, "PixelNoise(id.xy, RtvSampleIndex(frame, i, k), flags)") && Has(cs, "g_signal0[id.xy] = float4(vis[0], vis[1], vis[2], 1.0);"));
  // a light the shader cannot place is skipped, a vehicle and the sky are not judged
  CHECK(Has(cs, "if (channel >= 9u)") && Has(cs, "!sky && !vehicle && t < h2.z && worldOk"));
  // the light pass reads the masks: the table, the seams, the flag; the ray's visibility is known after the term and before the lamp branch; a game's lamp is only taken away from
  const std::string ps = ReadFile(src + "/gpu/hlsl/local_light_ps.hlsl");
  CHECK(!ps.empty() && Has(ps, "#include \"rt_mask_read.hlsli\"") && Has(ps, "float4 RtmLoadMask(uint slot, int2 p) {") && Has(ps, "float RtmLoadDepth(uint slot, int2 p) {") &&
        Has(ps, "RtmTable LpLoadRtTable() {") && Has(ps, "g_lights.Load4(LP_RT_OFFSET)") && Has(ps, "g_lights.Load4(LP_RT_OFFSET + 48u)") && Has(ps, "t.step = c.w;") && Has(ps, "t.traced_w = d.y;") &&
        Has(ps, "t.depth_sigma = asfloat(d.x);"));
  CHECK(Has(ps, "const bool rt_ref = (flags & uint(LP_FLAG_RT_MASK)) != 0u && (l.flags & uint(LL_FLAG_RT_VISIBILITY)) != 0u;") &&
        Has(ps, "RtmVisibility(rt_table, l.mask_channel, p, g.size.x, g.size.y, t, (rt_table.flags & uint(RTM_RAW_VIEW)) != 0u)") && Has(ps, "lamp_vis = RtmCombine(lamp_vis, rt_vis);"));
  {
    // The shader is split in LpOneLight (the term, the neon contact term) and LpShadowed (the shadows and the ray's visibility; GB-NEON-CONTACT made the split so that the capped tube path calls it too). The order is
    // the same: the term, then the ray's visibility, then the lamp branch, then the atlas, then the product. The term is now the argument `add` that LpOneLight and LpCappedLights pass in.
    const size_t one_at = ps.find("float4 LpOneLight(");
    const size_t term_at = ps.find("float3 add = NgFinite3(LpPixelLight(l, g, world, normal, albedo, lit));", one_at);
    const size_t call_at = ps.find("return LpShadowed(l, g, flags, p, world, normal, add);", term_at);
    const size_t capped_at = ps.find("LpShadowed(l[j], g, flags, p, world, normal, one);");
    const size_t shadowed_at = ps.find("float4 LpShadowed(LlLight l, LpGlobals g, uint flags, int2 p, float3 world, float3 normal, float3 add) {");
    const size_t rt_at = ps.find("const bool rt_ref =", shadowed_at);
    const size_t lamp_at = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind", shadowed_at);
    const size_t head_at = ps.find("add *= LslLightLit(", shadowed_at);
    const size_t last_at = ps.find("if (rt_ref) {", head_at);
    CHECK(one_at != std::string::npos && term_at != std::string::npos && call_at != std::string::npos && capped_at != std::string::npos && shadowed_at != std::string::npos && rt_at != std::string::npos &&
          lamp_at != std::string::npos && head_at != std::string::npos && last_at != std::string::npos && shadowed_at < rt_at && rt_at < lamp_at && lamp_at < head_at && head_at < last_at && one_at < term_at &&
          term_at < call_at);
  }
  // a game's lamp and the lights the pass owns: the two forms, and no path that adds a game's light on top of the grid's with a channel
  CHECK(Has(ps, "add = game_lit ? -add * (1.0f - lamp_vis) : add * lamp_vis;") && Has(ps, "((l.flags & uint(LL_FLAG_GAME_LIT)) != 0u) ? -add * (1.0f - RtmCombine(1.0f, rt_vis)) : add * RtmCombine(1.0f, rt_vis)"));
  CHECK(Has(ps, "float3(1.0f - rt_vis, rt_vis, 0.0f) * (g.exposure * 0.15f)"));
  // the second line: a lamp the game lights with no source of visibility adds nothing, before the lamp branch (the light returns w = 0, which is what its own draw's `discard;` was before the lights of a draw
  // were summed in one shader; the draw discards the pixel when no light of it adds anything)
  {
    const size_t never = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && (l.flags & uint(LL_FLAG_GAME_LIT)) != 0u && !rt_ref && !(l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u)) {\n    return float4(0.0f, 0.0f, 0.0f, 0.0f);");
    const size_t lamp = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind");
    CHECK(never != std::string::npos && lamp != std::string::npos && never < lamp);
    CHECK(Has(ps, "if (!any_light) {\n    discard;\n  }\n  return float4(total, 0.0f);"));
  }
  // the light pass's reader: the seams
  CHECK(Has(read, "float4 RtmLoadMask(uint slot, int2 p);") && Has(read, "float RtmLoadDepth(uint slot, int2 p);") && Has(read, "float RtmVisibility(RtmTable tb, uint channel, int2 p, float renderW, float renderH, float t, bool raw)"));
  // the weighted mean is held between its taps (check 1's identity on a GPU whose divide is an ulp out): the range is taken over the taps that carry weight and the result clamped to it, once
  const auto times = [&](const std::string& needle) {
    size_t n = 0;
    for (size_t at = read.find(needle); at != std::string::npos; at = read.find(needle, at + needle.size())) ++n;
    return n;
  };
  CHECK(times("return min(max(sum / weights, lo), hi);") == 1 && times("return sum / weights;") == 0 && times("lo = min(lo, v);") == 1 && times("hi = max(hi, v);") == 1 &&
        Has(read, "if (wk > 0.0f) {\n      lo = min(lo, v);"));
}

// ---------------------------------------------------------------------------
// the sources: the host class, the settings, the wiring (text checks: nothing here ran on a device)
// ---------------------------------------------------------------------------
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }

void TestSources(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string host_h = ReadFile(gpu + "rt_lights.h"), host = ReadFile(gpu + "rt_lights.cpp");
  const std::string ll_h = ReadFile(gpu + "local_lights.h"), ll_cpp = ReadFile(gpu + "local_lights.cpp");
  const std::string renderer = ReadFile(gpu + "renderer.cpp"), renderer_h = ReadFile(gpu + "renderer.h");
  const std::string host_shaders = ReadFile(gpu + "host_shaders.h"), cmake = ReadFile(gpu + "CMakeLists.txt"), precache = ReadFile(gpu + "host_precache.h");
  const std::string scene_rules = ReadFile(gpu + "rt_scene_rules.h"), lamp_list = ReadFile(gpu + "vol_lamp_list.h");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h"), meta = ReadFile(src + "/settings/generated/settings_meta.inc"), manifest = ReadFile(src + "/settings/generated/manifest.json");
  CHECK(!host_h.empty() && !host.empty() && !ll_h.empty() && !ll_cpp.empty() && !renderer.empty() && !settings.empty() && !manifest.empty());

  // THE SETTINGS (eight with the review's lamp slot arm), debug only, in the three generated files and the aliases, with the ranges the rules clamp to
  const char* names[] = {"mcla_gpu_rt_lights", "mcla_gpu_rt_lights_k", "mcla_gpu_rt_lights_radius", "mcla_gpu_rt_lights_rays", "mcla_gpu_rt_lights_view", "mcla_gpu_rt_lights_check", "mcla_gpu_rt_lights_scale", "mcla_gpu_rt_lights_lamp_slot"};
  for (const char* name : names) {
    const std::string n = name;
    CHECK(Has(settings, ", " + n + ", ") && Has(settings, "inline auto& " + n + " = g_gpu." + n + ";") && Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},") && Has(manifest, "\"" + n + "\": {"));
    CHECK(Count(settings, "REX_" "SETTING(") > 0 && Count(settings, ", " + n + ", ") == 1);
  }
  CHECK(Has(settings, "REX_" "SETTING(std::string, mcla_gpu_rt_lights, \"off\"") && Has(settings, "rex::kDebugOnly, rex::Allowed(\"auto\", \"off\", \"on\")"));
  CHECK(Has(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_k, 0,") && Has(settings, "rex::Range{0, 9}") && Has(settings, "REX_" "SETTING(f64, mcla_gpu_rt_lights_radius, 0.0,") && Has(settings, "rex::Range{0.0, 2.0}") &&
        Has(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_rays, 1,") && Has(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_view, 0,") && Has(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_check, 0,") && Has(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_scale, 0,"));
  {
    // the scale's range is the rules': 0 auto, 1 whole, 2 half; the A/B switch of the Ray Reconstruction hook: debug only, live, 0 or 1, on by default, in the three files and the alias
    const size_t at = At(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_scale, 0,");
    CHECK(at != std::string::npos && At(settings, "rex::kDebugOnly, rex::Range{0, 2});", at) < At(settings, "REX_" "SETTING(", at + 20));
    const size_t rr = At(settings, "REX_" "SETTING(i32, mcla_gpu_dlss_rr_rt_lights, 1,");
    CHECK(rr != std::string::npos && At(settings, "rex::kDebugOnly, rex::Range{0, 1});", rr) < At(settings, "REX_" "SETTING(", rr + 20) && !Has(settings.substr(rr, 1400), "kRestart"));
    CHECK(Has(settings, "inline auto& mcla_gpu_dlss_rr_rt_lights = g_gpu.mcla_gpu_dlss_rr_rt_lights;") && Has(meta, "{\"mcla_gpu_dlss_rr_rt_lights\", \"MCLA/GPU\"},") && Has(manifest, "\"mcla_gpu_dlss_rr_rt_lights\": {") &&
          Count(settings, ", mcla_gpu_dlss_rr_rt_lights, ") == 1);
  }
  {
    // REVIEW S2: the lamp slot arm is a debug only, live, 0 or 1 setting whose default is today's pick (0), in the three files and the alias; the host reads it every frame and hands it to the pick
    const size_t ls = At(settings, "REX_" "SETTING(i32, mcla_gpu_rt_lights_lamp_slot, 0,");
    CHECK(ls != std::string::npos && At(settings, "rex::kDebugOnly, rex::Range{0, 1});", ls) < At(settings, "REX_" "SETTING(", ls + 20) && !Has(settings.substr(ls, At(settings, "REX_" "SETTING(", ls + 20) - ls), "kRestart"));
    CHECK(Has(host, "lamp_slot_ = st::mcla_gpu_rt_lights_lamp_slot.Get() != 0;") && Has(host, "held.data(), held_count, k_, lamp_slot_);") && Has(host_h, "bool lamp_slot_ = false;"));
    // and the pass's log line says which arm it is (the chain's census)
    CHECK(Has(host, "lamp slot {} (logged when it changes)") && Has(host, "(lamp_slot_ ? 1u << 15 : 0u)"));
  }
  CHECK(rl::kMaxLights == 9 && rl::kMaxRays == 4);   // the ranges above
  // the host reads each of them
  for (const char* name : names) {
    CHECK(Has(host, std::string(name)));
  }
  CHECK(Has(host, "LiveText text(\"mcla_gpu_rt_lights\");") && Has(host, "rt::ParseMode(ModeSetting().Get())") && Has(host, "mcla_gpu_noise_blue.Get()"));

  // THE HOST CLASS: the layout the shader declares, the set's views, the denoisers, the pipeline's owner
  CHECK(Has(host, "builder.addDescriptorSet(heap_->texture_set_builder())") && Has(host, "builder.addDescriptorSet(pass_set_builder_)") && Has(host, "builder.addDescriptorSet(scene.SetDesc())") &&
        Has(host, "builder.addPushConstant(0, rt::kPushSpace, sizeof(rt::Push), plume::RenderShaderStageFlag::COMPUTE);"));
  CHECK(Has(host, "pass_set_builder_.addByteAddressBuffer(rt::kBindRows)") && Has(host, "addReadWriteTexture(rt::kBindSignal0)") && Has(host, "addReadWriteTexture(rt::kBindSignal2)") &&
        Has(host, "addReadWriteTexture(rt::kBindDepth)") && Has(host, "addReadWriteTexture(rt::kBindNormal)") && !Has(host, "kBindHit") && Has(host, "addReadWriteByteAddressBuffer(rt::kBindStats)"));
  // the pass set has seven descriptors (the rows, three signals, two guides, the counters): D3D12 sizes the set by the count the call says
  CHECK(Count(host, "pass_set_builder_.add") == 7 && rl::kSetViews == 7 && Has(host, "CreateDescriptorSet(pass_set_builder_, rt::kSetViews, 0,"));
  CHECK(rl::kBindRows == 0 && rl::kBindSignal0 == 1 && rl::kBindSignal1 == 2 && rl::kBindSignal2 == 3 && rl::kBindDepth == 4 && rl::kBindNormal == 5 && rl::kBindStats == 6 && rl::kTextureSet == 0 &&
        rl::kPassSet == 1 && rl::kSceneSet == 2 && rl::kPushSpace == 3 && rl::kGroup == 8);
  CHECK(Has(host, "RtDenoiseVisShader(false)") && Has(host, "RtDenoiseVisShader(true)") && Has(host, "rt_denoise::Format::kRgba8") && !Has(host, "RtDenoiseDiffuseShader") && Has(host, "rt::kFirstDenoiseInstance + g") && Has(host, "denoise_[g].RecordTemporal(stage)") &&
        Has(host, "denoise_[g].RecordSpatial(stage)") && Has(host, "d.OnPresent();") && Has(host, "d.Shutdown();") && Has(host_h, "std::array<RtDenoise, larecomp::gpu::rt_lights::kMaxImages> denoise_;"));
  {
    // the three denoisers take instances 2 to 4; the others in the tree keep their own (0: the reflections', 1: the per-pixel bounce light's)
    CHECK(rl::kFirstDenoiseInstance == 2 && rl::kFirstDenoiseInstance + rl::kMaxImages <= 16);
    CHECK(Has(ReadFile(gpu + "ssr.cpp"), "/*instance=*/0") && Has(ReadFile(gpu + "gi_pixel.cpp"), "/*instance=*/1"));
    for (const char* other : {"ssr.cpp", "gi_pixel.cpp", "car_rt_refl.cpp", "gi_apply.cpp", "rr_guides.cpp"}) {
      const std::string text = ReadFile(gpu + other);
      CHECK(!Has(text, "/*instance=*/2") && !Has(text, "/*instance=*/3") && !Has(text, "/*instance=*/4"));
    }
  }
  CHECK(Has(precache, "kRtLights = 35,") && Count(precache, "= 35,") == 1 && Has(host, "HostPrecache::Key(HostPrecache::kRtLights, 0)"));
  CHECK(Has(host_shaders, "#include \"rt_lights_trace_cs.hlsl.dxil.h\"") && Has(host_shaders, "#include \"rt_lights_trace_cs.hlsl.spirv.h\"") && Has(host_shaders, "MCLA_GPU_HOST_SHADER_BLOB(rt_lights_trace_cs)") &&
        Has(host, "RtLightsTraceShader()") && Has(cmake, "larecomp_gpu_host_shader_rt(rt_lights_trace_cs cs_6_5)") && Has(cmake, "rt_lights.cpp"));
  CHECK(Has(cmake, "add_executable(larecomp_rt_lights_test EXCLUDE_FROM_ALL rt_lights_test.cpp)"));
  CHECK(Has(scene_rules, "kRtLights,") && Has(scene_rules, "case Consumer::kRtLights: return \"rt lights\";") && Has(host, "rt_scene::Consumer::kRtLights"));
  // the lamp list hook's reader bits are four distinct bits
  CHECK(Has(lamp_list, "kWantedByVolume = 1u << 0;") && Has(lamp_list, "kWantedByWorldFrame = 1u << 1;") && Has(lamp_list, "kWantedByLampShadows = 1u << 2;") && Has(lamp_list, "kWantedByRtLights = 1u << 3;") &&
        Has(host, "vol_lamps::SetWantedBy(vol_lamps::kWantedByRtLights, want);"));
  // the frame: the structures are asked for while it is wanted; a settings read; the lamps by the game's list
  CHECK(Has(host, "service_->Want(rt_scene::Consumer::kRtLights, needs);") && Has(host, "needs.cars = true;") && Has(host, "vol_lamps::Global().Latest(&list_, &list_seq_, &stale, kListMaxAge)") &&
        Has(host, "la::MakePassLight(lamp, none, la::LampMode::kSubtract)") && Has(host, "if (present) {"));

  // THE LIGHT PASS'S USE: the order of the calls is the contract (the lamps are handed over before the selection, the pick after it, the trace before the buffer, the table in the buffer), and nothing runs unless wanted
  const size_t begin = At(ll_cpp, "rt_lights_->BeginFrame(planes_wanted_, tier_ == ll::Tier::kUltra, garage);");
  const size_t append = At(ll_cpp, "rt_lights_->AppendLampLights(rows.position, &extras_);");
  const size_t select = At(ll_cpp, "ll::Selection selection = ll::SelectPassLights(");
  const size_t plan = At(ll_cpp, "rt_ran = rt_lights_->Plan(rin, &selection);");
  const size_t trace = At(ll_cpp, "rt_lights_->RecordTrace(list);");
  const size_t denoise = At(ll_cpp, "rt_lights_->RecordDenoise(list);");
  const size_t build = At(ll_cpp, "ll::BuildBuffer(in, selection, scratch_.data());");
  const size_t mask = At(ll_cpp, "in.rt_mask = (rt_ran && rt_lights_ != nullptr && rt_lights_->Masks().valid) ? &rt_lights_->Masks() : nullptr;");
  const size_t draws = At(ll_cpp, "list->drawInstanced(3, 1, 0, 0);");
  CHECK(begin != std::string::npos && append != std::string::npos && select != std::string::npos && plan != std::string::npos && trace != std::string::npos && denoise != std::string::npos &&
        build != std::string::npos && mask != std::string::npos && draws != std::string::npos);
  CHECK(append < select && select < plan && plan < trace && trace < denoise && denoise < mask && mask < build && build < draws);
  CHECK(Has(ll_cpp, "rin.pause_fx = pause_fx;") && Has(host, "prev_pause_fx_ == in.pause_fx &&") &&
        Has(host, "prev_pause_fx_ = in_.pause_fx;"));
  CHECK(Has(ll_cpp, "if (rt_lights_ != nullptr && rt_lights_->wanted()) {") && Count(ll_cpp, "rt_lights_->wanted()") == 3);   // BeginFrame's two uses and rt_lights_wanted()   // the lamps and the pick only while it is wanted: with it off the pass is what it was
  CHECK(Has(ll_cpp, "pass_timing_->Mark(list, \"rt lights\");") && Has(ll_cpp, "pass_timing_->Mark(list, \"rt lights denoise\");") && Has(ll_cpp, "pass_timing_->Mark(list, \"after rt lights\");"));
  // REVIEW (RTL-D1-REVIEW M1): the trace moves every signal image the pass set binds (EnsureSlot binds image_groups_ of them) to GENERAL, not only this frame's groups: K 6 -> 3 leaves a bound image in the read state
  CHECK(Has(host, "for (uint32_t g = 0; g < std::max(groups_, std::min(image_groups_, rt::kMaxImages)); ++g) {\n    writes[write_count++] = plume::RenderTextureBarrier(signal_[g].texture.get(), Layout::GENERAL);") &&
        Has(host, "const HostImage& image = g < image_groups_ ? signal_[g] : signal_[0];"));
  CHECK(Has(ll_cpp, "void LocalLights::OnPresent() {\n  if (rt_lights_ != nullptr) {\n    rt_lights_->OnPresent();"));   // the denoisers' boundary also while the pass is off
  CHECK(Has(ll_h, "void SetRtLights(RtLights* rt_lights) { rt_lights_ = rt_lights; }") && Has(ll_h, "RtLights* rt_lights_ = nullptr;") && Has(ll_cpp, "#include \"rt_lights.h\""));
  CHECK(Has(renderer_h, "RtLights rt_lights_;") && Has(renderer, "rt_lights_.Initialize(device_, frames_, heap_, passes_, pass_timing_, car_rt_, rt_world_, rt_scene_);") &&
        Has(renderer, "local_lights_.SetRtLights(&rt_lights_);") && Count(renderer, "rt_lights_.Shutdown();") == 2 && Has(renderer, "vram_.AddHolder(\"ray traced light visibility\"") &&
        Has(renderer, "rt_lights_.CensusText()"));
  // the host never touches the light pass's own state and never records outside a frame that planned it
  CHECK(Has(host, "if (why_ != rt::Why::kRuns || !list || light_count_ == 0 || frame_ != in_.frame) {") && Has(host, "if (why_ != rt::Why::kRuns || !list || ran_frame_ != frame_) {"));
  // a frame that does not run the pass trims the lamps it was handed for a ray (a game's lamp with no source would be ADDED on top of the grid's)
  CHECK(Has(host, "ll::TrimSelection(selection);   // a lamp handed over for a ray that will not come"));
  // the 8 bit average's random rounding: the host gives the frame's number, the consumer reads it for the shared blue noise (AGENT-RULE-NOISE: no hash, no lattice)
  {
    const std::string plain = ReadFile(gpu + "hlsl/rt_denoise_plain_consumer.hlsli"), engine = ReadFile(gpu + "hlsl/rt_denoise_engine.hlsli"), u8 = ReadFile(gpu + "hlsl/rt_denoise_unorm8.hlsli");
    CHECK(Count(host, "stage.push[rt_denoise::plain::kFrame] = uint32_t(in_.frame);") == 2 && mcla::gpu::rt_denoise::plain::kFrame == 22 && Has(plain, "#define RTDNP_W_FRAME 22"));
    CHECK(Has(plain, "BlueNoiseFrame2(p.x, p.y, RtDnW(RTDNP_W_FRAME))") && Has(plain, "float3 RtDnDither3(int2 p) {") && Has(plain, "#include \"blue_noise_frame.hlsli\"") && !Has(plain, "Ign("));
    CHECK(Has(engine, "const float3 dither = RtDnDither3(p);") && Has(engine, "RtDnStore8(colour.x, dither.x)") && Has(engine, "RtDnEncodeLength(RtDnNextLength(length, used))") && Has(engine, "[[vk::image_format(\"rgba8\")]]") &&
          !Has(engine, "Nudge") && !Has(u8, "Nudge"));
    CHECK(Has(plain, "float4(h.x, h.y, h.z, RtDnDecodeLength(h.w))") && Has(plain, "const float2 hs = float2(0.0f, nr.w);") && Has(plain, "r.rough = 1.0f;"));
    // the other consumers' compiles are byte-identical to the part 1 tree's (rt_lights_d1_shaders.py --identity-ref): here the text: every change is under the two macros
    CHECK(Count(plain, "#ifdef RT_DN_VIS") == 4 && Count(engine, "#ifdef RT_DN_OUT_UNORM8") == 5);
    // REVIEW (RTL-D1-REVIEW B1): a pixel with no usable guides (the sky, a vehicle, beyond the range, no light reaches it) must come out of the visibility's temporal pass as the signal's own value (1: lit, no history),
    // never black: black is a visibility of 0 and the light pass reads every pixel it lights (cars included: their lamp term would be taken away in full, a headlight would not light them). The other consumers
    // (no RT_DN_OUT_UNORM8) keep the black write: one of each, in that order, inside the guides test, before its return.
    {
      const size_t guides = engine.find("if (!RtDnGuidesOk(c)) {\n#ifdef RT_DN_OUT_UNORM8");
      const size_t own = engine.find("g_dn_out[p] = float4(current.xyz, 0.0f);", guides);
      const size_t other = engine.find("#else\n    g_dn_out[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);\n#endif\n    return;", guides);
      CHECK(guides != std::string::npos && own != std::string::npos && other != std::string::npos && guides < own && own < other);
      CHECK(Count(engine, "g_dn_out[p] = float4(current.xyz, 0.0f);") == 1 && Count(engine, "g_dn_out[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);") == 1);
      // and the trace's side of it: every texel it does not judge is written with the visibility 1 and the guides say "not signal" (depth 0 or share 0), which is what the pass-through above turns into "lit"
      CHECK(Has(ReadFile(gpu + "hlsl/rt_lights_trace_cs.hlsl"), "float vis[9] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};") && Has(ReadFile(gpu + "hlsl/rt_lights_trace_cs.hlsl"), "g_normal[id.xy] = float4(normal, signal ? 1.0 : 0.0);"));
    }
    CHECK(Has(host_shaders, "RtDenoiseVisShader(bool temporal)") && Has(host_shaders, "MCLA_GPU_HOST_SHADER_BLOB(rt_denoise_vis_temporal_cs)") && Has(host_shaders, "#include \"rt_denoise_vis_spatial_cs.hlsl.spirv.h\"") &&
          Has(cmake, "larecomp_gpu_host_shader(rt_denoise_vis_spatial_cs cs_6_0 SOURCE rt_denoise_plain_spatial_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE RT_DN_VIS=1 RT_DN_OUT_UNORM8=1)") &&
          Has(cmake, "larecomp_gpu_host_shader(rt_denoise_vis_temporal_cs cs_6_0 SOURCE rt_denoise_plain_temporal_cs DEFINES RT_DN_PROFILE=RT_DN_PROFILE_DIFFUSE RT_DN_VIS=1 RT_DN_OUT_UNORM8=1)"));
    // the images' formats the host makes and the shader declares
    CHECK(Has(host, "plume::RenderFormat::R8G8B8A8_UNORM, \"rt lights signal\"") && Has(host, "plume::RenderFormat::R16_FLOAT, \"rt lights depth 0\"") && Has(host, "plume::RenderFormat::R16_FLOAT, \"rt lights depth 1\"") &&
          Has(host, "plume::RenderFormat::R8G8B8A8_SNORM, \"rt lights normal and share\"") && !Has(host, "R32_FLOAT") && !Has(host, "R16G16B16A16_FLOAT") && !Has(host, "R16G16_FLOAT"));
    CHECK(Has(host, "rt_denoise::Format::kRgba8") && Has(ReadFile(gpu + "rt_denoise.cpp"), "plume::RenderFormat::R8G8B8A8_UNORM : plume::RenderFormat::R16G16B16A16_FLOAT") && Has(ReadFile(gpu + "rt_denoise.h"), "rt_denoise::Format format = rt_denoise::Format::kRgba16f"));
    // the mask's size and the step: the plan chooses them from the occlusion's traced size and the setting; the trace is one thread a mask texel; the header and the table carry them
    CHECK(Has(host, "step_ = rt::StepFor(scale_setting_, in.traced[0], in.traced[1]);") && Has(host, "EnsureImages(rt::MaskExtent(in.traced[0], step_), rt::MaskExtent(in.traced[1], step_), groups_)") && Has(host, "header.step = step_;") &&
          Has(host, "header.traced_width = in.traced[0];") && Has(host, "stage.iterations = rt::DenoiseIterations(step_);") && Has(host, "scale_setting_ = int(st::mcla_gpu_rt_lights_scale.Get());") &&
          Has(host, "rt::MakeMaskFrame(mi)") && Count(host, "rt::MakeMaskFrame(mi)") == 2);
    // the HeldBytes the budget reads is the formats' (no hit image, 4 byte signals)
    CHECK(Has(host, "rt::kSignalBytes + rt::kGuideBytes") && !Has(host, "28u"));
    // THE RAW MODE: asked once a frame before the plan, taken at the plan, never writing history, the consumer noted
    CHECK(Has(host, "raw_frame_ = raw_requested_;") && Has(host, "if (raw_frame_) {\n    RecordRaw(list);\n    return;\n  }") && Has(host, "denoiser::NoteConsumer(rr::Consumer::kRtLights, wanted_);") && Has(host, "have_prev_ = false;\n  history_frames_ = 0;") &&
          Has(host_h, "void SetRawMode(bool raw) { raw_requested_ = raw; }") && Has(host, "#include \"denoiser_setting.h\""));
    CHECK(Has(ll_cpp, "void LocalLights::SetRtRawMode(bool raw) {") && Has(ll_cpp, "rt_lights_->SetRawMode(raw);") && Has(ll_h, "void SetRtRawMode(bool raw);") && Has(ll_h, "bool rt_lights_wanted() const;"));
    {
      const std::string scene = ReadFile(gpu + "scene.cpp"), temporal = ReadFile(gpu + "temporal.cpp");
      const size_t ask = At(scene, "local_lights_->SetRtRawMode(local_lights_->rt_lights_wanted() && temporal_ && temporal_->RawModes().RtLights());");
      const size_t run = At(scene, "const bool local_lights_ran = local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_);");
      CHECK(ask != std::string::npos && run != std::string::npos && ask < run);
      CHECK(Has(temporal, "rr::RawSwitchOn(int(larecomp::settings::mcla_gpu_dlss_rr_rt_lights.Get()))") && Has(temporal, "window_.rr_raw_frames[size_t(rr::RawEffect::kRtLights)] += modes.RtLights() ? 1 : 0;") &&
            Has(temporal, "modes.RtGi() || modes.RtLights();"));
      // the guide pass gets nothing from it: no hit image, no new guide: the raw mode has no pointer to RR's hit inputs
      const std::string guides = ReadFile(gpu + "rr_guides.cpp");
      CHECK(!Has(host, "NoteRr") && !Has(host, "RawHitImage") && !Has(guides, "rt_lights") && !Has(guides, "RtLights"));
    }
  }
  // the shared noise, at the light's own sample, in the rows' flags
  CHECK(Has(host, "(blue_ ? rt::kFlagBlue : 0u)") && Has(host, "header.rays = rays_;") && Has(host, "header.max_range = rt::kMaxRangeMetres;"));
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows how far it got
  const bool trace = argc > 2 && std::string(argv[2]) == "--trace";
  bool print = false;
  for (int i = 1; i < argc; ++i) print = print || std::string(argv[i]) == "--print";
  const auto section = [&](const char* name, void (*fn)()) {
    if (trace) std::printf("... %s\n", name);
    fn();
  };
  section("channels", TestChannels);
  section("settings", TestSettings);
  section("selection", TestSelection);
  section("assignment", TestAssignment);
  section("rows", TestRows);
  section("composition", TestComposition);
  section("geometry", TestGeometry);
  section("soft shadow", TestSoftShadow);
  section("noise", TestNoise);
  section("mask read", TestMaskRead);
  section("selection and buffer", TestSelectionAndBuffer);
  TestMemory(print);
  TestUnorm8(print);
  TestHalfSize(print);
  TestRawFrame();
  if (argc > 1) {
    TestShaderText(argv[1]);
    TestSources(argv[1]);
  }
  std::printf("rt_lights_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
