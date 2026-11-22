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
//   - the shader's text (the numbers against the header's, no colour target, the shared noise) and, with the src folder, the host class, the settings and the wiring.

#include <algorithm>
#include <array>
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
#include "noise_resolve_check.h"
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
  // the memory: a group is five RGBA16F-class images, the guides 28 bytes a pixel
  CHECK(rl::HeldBytes(1920, 1080, 1) == uint64_t(1920) * 1080 * 68 && rl::HeldBytes(1920, 1080, 3) == uint64_t(1920) * 1080 * 148);
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
  hs::DepthWidth() = w.tw;
}

hs::RtmTable MakeTable(int images, int tw, int th, bool guided, uint32_t flags = RTM_VALID) {
  hs::RtmTable t;
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
double Reference(const MaskWorld& w, int channel, bool raw, bool guided, int px, int py, int rw, int rh, double t) {
  const std::vector<float>& img = raw ? w.raw[channel] : w.value[channel];
  const double ux = (px + 0.5) * w.tw / rw - 0.5, uy = (py + 0.5) * w.th / rh - 0.5;
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
      const float raw_depth = w.depth[size_t(qy) * w.tw + qx];
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
        Has(cs, "RWTexture2D<float> g_depth : register(u4, space1);") && Has(cs, "RWTexture2D<float4> g_normal : register(u5, space1);") && Has(cs, "RWTexture2D<float2> g_hit : register(u6, space1);") &&
        Has(cs, "RWByteAddressBuffer g_stats : register(u7, space1);") && Has(cs, "ConstantBuffer<RtvPush> g_push : register(b0, space3);") && Has(cs, "#define CAR_RT_SPACE 2") &&
        Has(cs, "#define RT_WORLD_SPACE 2") && Has(cs, "Texture2D<float4> g_textures[] : register(t0, space0);"));
  // the rows' header and a light's row as the host writes them (rt_lights_rules.h)
  CHECK(Has(cs, "g_rows.Load4(0)") && Has(cs, "g_rows.Load4(16)") && Has(cs, "g_rows.Load4(32)") && Has(cs, "const uint at = 64u + i * 64u;") && Has(cs, "g_rows.Load4(at + 48u)"));
  // no colour target and no write to anything of the game's: the pass writes its own images and counters
  CHECK(!Has(cs, "SV_Target") && !Has(cs, "RWTexture2DArray"));
  // the shared per-frame blue noise, at the light's own sample, with the debug comparison; no per-pixel hash of a lattice
  CHECK(Has(cs, "#include \"blue_noise_frame.hlsli\"") && Has(cs, "BlueNoiseFrame2(int(pixel.x), int(pixel.y), sampleIndex)") && Has(cs, "RtvSampleIndex(frame, i, kRtvSlotPair)") &&
        Has(cs, "RtvSampleIndex(frame, i, kRtvSlotCoin)") && Has(cs, "RtvSampleIndex(frame, i, k)") && Has(cs, "kFlagBlue"));
  // a ray: accept the first hit, the city's mask and the cars' parts, the stop short from the light's row
  CHECK(Has(cs, "RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH") && Has(cs, "RTV_CITY_MASK | CAR_RT_MASK_ALL") && Has(cs, "RtvMakeRay(P, N, lift, lampPoint, r2.w, disc, stopShort)"));
  // the identity checks and the guides
  CHECK(Has(cs, "(flags & kFlagCheckVisible) != 0u") && Has(cs, "(flags & kFlagCheckBlocked) != 0u") && Has(cs, "g_hit[p] = float2(0.0, signal ? 1.0 : 0.0);") && Has(cs, "g_depth[p] = world ? t : 0.0;"));
  // a light the shader cannot place is skipped, a vehicle and the sky are not judged
  CHECK(Has(cs, "if (channel >= 9u)") && Has(cs, "!sky && !vehicle && t < h2.z && worldOk"));
  // the light pass reads the masks: the table, the seams, the flag; the ray's visibility is known after the term and before the lamp branch; a game's lamp is only taken away from
  const std::string ps = ReadFile(src + "/gpu/hlsl/local_light_ps.hlsl");
  CHECK(!ps.empty() && Has(ps, "#include \"rt_mask_read.hlsli\"") && Has(ps, "float4 RtmLoadMask(uint slot, int2 p) {") && Has(ps, "float RtmLoadDepth(uint slot, int2 p) {") &&
        Has(ps, "RtmTable LpLoadRtTable() {") && Has(ps, "g_lights.Load4(LP_RT_OFFSET)") && Has(ps, "g_lights.Load(LP_RT_OFFSET + 48u)"));
  CHECK(Has(ps, "const bool rt_ref = (flags & uint(LP_FLAG_RT_MASK)) != 0u && (l.flags & uint(LL_FLAG_RT_VISIBILITY)) != 0u;") &&
        Has(ps, "RtmVisibility(rt_table, l.mask_channel, p, g.size.x, g.size.y, t, (rt_table.flags & uint(RTM_RAW_VIEW)) != 0u)") && Has(ps, "lamp_vis = RtmCombine(lamp_vis, rt_vis);"));
  {
    const size_t term_at = ps.find("float3 add = NgFinite3(LpPixelLight(l, g, world, normal, albedo, rt2.w));");
    const size_t rt_at = ps.find("const bool rt_ref =");
    const size_t lamp_at = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind");
    const size_t head_at = ps.find("add *= LslLightLit(");
    const size_t last_at = ps.find("if (rt_ref) {", head_at);
    CHECK(term_at != std::string::npos && rt_at != std::string::npos && lamp_at != std::string::npos && head_at != std::string::npos && last_at != std::string::npos && term_at < rt_at && rt_at < lamp_at &&
          lamp_at < head_at && head_at < last_at);
  }
  // a game's lamp and the lights the pass owns: the two forms, and no path that adds a game's light on top of the grid's with a channel
  CHECK(Has(ps, "add = game_lit ? -add * (1.0f - lamp_vis) : add * lamp_vis;") && Has(ps, "((l.flags & uint(LL_FLAG_GAME_LIT)) != 0u) ? -add * (1.0f - RtmCombine(1.0f, rt_vis)) : add * RtmCombine(1.0f, rt_vis)"));
  CHECK(Has(ps, "float3(1.0f - rt_vis, rt_vis, 0.0f) * (g.exposure * 0.15f)"));
  // the second line: a lamp the game lights with no source of visibility is discarded, before the lamp branch
  {
    const size_t never = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && (l.flags & uint(LL_FLAG_GAME_LIT)) != 0u && !rt_ref && !(l.shadow_kind == uint(LL_SHADOW_ATLAS) && l.view_count != 0u)) {\n    discard;");
    const size_t lamp = ps.find("if (l.source == uint(LL_SOURCE_LAMP) && ((l.shadow_kind");
    CHECK(never != std::string::npos && lamp != std::string::npos && never < lamp);
  }
  // the light pass's reader: the seams
  CHECK(Has(read, "float4 RtmLoadMask(uint slot, int2 p);") && Has(read, "float RtmLoadDepth(uint slot, int2 p);") && Has(read, "float RtmVisibility(RtmTable tb, uint channel, int2 p, float renderW, float renderH, float t, bool raw)"));
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

  // THE SETTINGS: six, debug only, in the three generated files and the aliases, with the ranges the rules clamp to
  const char* names[] = {"mcla_gpu_rt_lights", "mcla_gpu_rt_lights_k", "mcla_gpu_rt_lights_radius", "mcla_gpu_rt_lights_rays", "mcla_gpu_rt_lights_view", "mcla_gpu_rt_lights_check"};
  for (const char* name : names) {
    const std::string n = name;
    CHECK(Has(settings, ", " + n + ", ") && Has(settings, "inline auto& " + n + " = g_gpu." + n + ";") && Has(meta, "{\"" + n + "\", \"MCLA/GPU\"},") && Has(manifest, "\"" + n + "\": {"));
    CHECK(Count(settings, "REX_SETTING(") > 0 && Count(settings, ", " + n + ", ") == 1);
  }
  CHECK(Has(settings, "REX_SETTING(std::string, mcla_gpu_rt_lights, \"off\"") && Has(settings, "rex::kDebugOnly, rex::Allowed(\"auto\", \"off\", \"on\")"));
  CHECK(Has(settings, "REX_SETTING(i32, mcla_gpu_rt_lights_k, 0,") && Has(settings, "rex::Range{0, 9}") && Has(settings, "REX_SETTING(f64, mcla_gpu_rt_lights_radius, 0.0,") && Has(settings, "rex::Range{0.0, 2.0}") &&
        Has(settings, "REX_SETTING(i32, mcla_gpu_rt_lights_rays, 1,") && Has(settings, "REX_SETTING(i32, mcla_gpu_rt_lights_view, 0,") && Has(settings, "REX_SETTING(i32, mcla_gpu_rt_lights_check, 0,"));
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
        Has(host, "addReadWriteTexture(rt::kBindDepth)") && Has(host, "addReadWriteTexture(rt::kBindNormal)") && Has(host, "addReadWriteTexture(rt::kBindHit)") && Has(host, "addReadWriteByteAddressBuffer(rt::kBindStats)"));
  // the pass set has eight descriptors (the rows, three signals, three guides, the counters): D3D12 sizes the set by the count the call says
  CHECK(Count(host, "pass_set_builder_.add") == 8 && rl::kSetViews == 8 && Has(host, "CreateDescriptorSet(pass_set_builder_, rt::kSetViews, 0,"));
  CHECK(rl::kBindRows == 0 && rl::kBindSignal0 == 1 && rl::kBindSignal1 == 2 && rl::kBindSignal2 == 3 && rl::kBindDepth == 4 && rl::kBindNormal == 5 && rl::kBindHit == 6 && rl::kBindStats == 7 && rl::kTextureSet == 0 &&
        rl::kPassSet == 1 && rl::kSceneSet == 2 && rl::kPushSpace == 3 && rl::kGroup == 8);
  CHECK(Has(host, "RtDenoiseDiffuseShader(false)") && Has(host, "RtDenoiseDiffuseShader(true)") && Has(host, "rt::kFirstDenoiseInstance + g") && Has(host, "denoise_[g].RecordTemporal(stage)") &&
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
  const size_t begin = At(ll_cpp, "rt_lights_->BeginFrame(planes_wanted_, tier_ == ll::Tier::kUltra);");
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
  CHECK(Has(ll_cpp, "if (rt_lights_ != nullptr && rt_lights_->wanted()) {") && Count(ll_cpp, "rt_lights_->wanted()") == 2);   // the lamps and the pick only while it is wanted: with it off the pass is what it was
  CHECK(Has(ll_cpp, "pass_timing_->Mark(list, \"rt lights\");") && Has(ll_cpp, "pass_timing_->Mark(list, \"rt lights denoise\");") && Has(ll_cpp, "pass_timing_->Mark(list, \"after rt lights\");"));
  CHECK(Has(ll_cpp, "void LocalLights::OnPresent() {\n  if (rt_lights_ != nullptr) {\n    rt_lights_->OnPresent();"));   // the denoisers' boundary also while the pass is off
  CHECK(Has(ll_h, "void SetRtLights(RtLights* rt_lights) { rt_lights_ = rt_lights; }") && Has(ll_h, "RtLights* rt_lights_ = nullptr;") && Has(ll_cpp, "#include \"rt_lights.h\""));
  CHECK(Has(renderer_h, "RtLights rt_lights_;") && Has(renderer, "rt_lights_.Initialize(device_, frames_, heap_, passes_, pass_timing_, car_rt_, rt_world_, rt_scene_);") &&
        Has(renderer, "local_lights_.SetRtLights(&rt_lights_);") && Count(renderer, "rt_lights_.Shutdown();") == 2 && Has(renderer, "vram_.AddHolder(\"ray traced light visibility\"") &&
        Has(renderer, "rt_lights_.CensusText()"));
  // the host never touches the light pass's own state and never records outside a frame that planned it
  CHECK(Has(host, "if (why_ != rt::Why::kRuns || !list || light_count_ == 0 || frame_ != in_.frame) {") && Has(host, "if (why_ != rt::Why::kRuns || !list || ran_frame_ != frame_) {"));
  // a frame that does not run the pass trims the lamps it was handed for a ray (a game's lamp with no source would be ADDED on top of the grid's)
  CHECK(Has(host, "ll::TrimSelection(selection);   // a lamp handed over for a ray that will not come"));
  // the shared noise, at the light's own sample, in the rows' flags
  CHECK(Has(host, "(blue_ ? rt::kFlagBlue : 0u)") && Has(host, "header.rays = rays_;") && Has(host, "header.max_range = rt::kMaxRangeMetres;"));
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows how far it got
  const bool trace = argc > 2 && std::string(argv[2]) == "--trace";
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
  if (argc > 1) {
    TestShaderText(argv[1]);
    TestSources(argv[1]);
  }
  std::printf("rt_lights_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
