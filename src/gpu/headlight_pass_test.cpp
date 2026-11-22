// Check of the forward headlight stage against the deferred local light pass (headlight_pass_rules.h; perf/LIGHT-UNIFY.md 6.3 and 11.2, package GB-H2): the switch's words and what each route does, the
// retail census of the forward stage, the picture-identity rule on made-up pictures (equal, a little off, far off, noisy, too few pixels, coverage), the retirement gate, and the PIXEL IDENTITY: on
// random lights, surfaces and shadow maps the pass's light (local_lights_rules.h PixelLightShadowed: the one law and the one lookup) equals the forward stage's (headlight_rig_rules.h LampScale,
// ProjectToShadow, SoftShadow, the mean of a light's views: the CPU twins of the variant's text) to the last bits of a float, for the lights the frame holds at all three levels of LIGHT SHADOWS.
// With the `src` folder it also reads the sources: the wiring of the route in the rig, the pass and the pipelines, the setting, the debug row, the CMake line. A program of its own: no renderer,
// no ReXGlue, no GPU, no game; it exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_headlight_pass_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_headlight_pass_test [<the src folder>]     (found from this file's place without an argument)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "headlight_pass_rules.h"
#include "headlight_rig_rules.h"
#include "local_lights_rules.h"
#include "local_lights_sources.h"

namespace hp = larecomp::gpu::headlight_pass;
namespace lp = larecomp::gpu::local_lights;
namespace hr = mcla::gpu::headlight_rules;
using namespace larecomp::gpu;

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
void CheckRel(double got, double want, double rel, double floor_abs, const std::string& what) {
  ++g_checks;
  const double tol = std::max(floor_abs, rel * std::fabs(want));
  if (!(std::fabs(got - want) <= tol)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.9g, want %.9g (tolerance %.3g)\n", what.c_str(), got, want, tol);
  }
}

struct Lcg {
  uint32_t state = 20261121u;
  float Next() {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
  uint32_t Below(uint32_t n) { return uint32_t(Next() * float(n)) % n; }
  void Direction(float out[3]) {
    const float z = Range(-1.0f, 1.0f);
    const float a = Range(0.0f, 6.2831853f);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    out[0] = r * std::cos(a);
    out[1] = z;
    out[2] = r * std::sin(a);
  }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what) { return text.find(what); }

// ---------------------------------------------------------------------------
// the switch
// ---------------------------------------------------------------------------

void RouteChecks() {
  using R = hp::Route;
  Check(hp::ParseRoute("forward") == R::kForward && hp::ParseRoute("pass") == R::kPass && hp::ParseRoute("both") == R::kBoth, "the three words");
  Check(hp::ParseRoute("") == R::kForward && hp::ParseRoute("Pass") == R::kForward && hp::ParseRoute("deferred") == R::kForward && hp::ParseRoute("passes") == R::kForward,
        "a word that is no value is the forward stage (the game as it is)");
  for (const R r : {R::kForward, R::kPass, R::kBoth}) {
    Check(hp::ParseRoute(hp::RouteName(r)) == r, std::string("the name of ") + hp::RouteName(r) + " parses back to it");
  }
  Check(hp::ForwardStageOn(R::kForward) && !hp::ForwardStageOn(R::kPass) && hp::ForwardStageOn(R::kBoth), "the forward stage is off only in the route `pass`");
  Check(!hp::PassLightsHeadlights(R::kForward) && hp::PassLightsHeadlights(R::kPass) && hp::PassLightsHeadlights(R::kBoth), "the pass lights the frame's headlights in `pass` and `both`");
  // every route lights each headlight at least once, and `both` lights it twice: the three routes are the cases of the A/B
  for (const R r : {R::kForward, R::kPass, R::kBoth}) {
    const int ways = (hp::ForwardStageOn(r) ? 1 : 0) + (hp::PassLightsHeadlights(r) ? 1 : 0);
    Check(r == R::kBoth ? ways == 2 : ways == 1, std::string("route ") + hp::RouteName(r) + ": " + std::to_string(ways) + " way(s) light the headlights");
  }
  // the sources mask: the route adds the frame's source and never removes a bit
  for (uint32_t mask = 0; mask <= lp::kSourceMask; ++mask) {
    Check(hp::SourcesFor(R::kForward, mask) == mask, "forward leaves the sources as they are");
    Check(hp::SourcesFor(R::kPass, mask) == (mask | lp::kSourceFrame) && hp::SourcesFor(R::kBoth, mask) == (mask | lp::kSourceFrame), "pass and both add the frame's headlights to any sources");
  }
  for (const R r : {R::kForward, R::kPass, R::kBoth}) {
    Check(hp::PassOn(r, true), "the setting's own use keeps the pass on in every route");
    Check(hp::PassOn(r, false) == hp::PassLightsHeadlights(r), "without the setting the pass is on exactly when the route gives it the headlights");
  }
  // the retail census of the forward stage
  Check(hp::kForwardEdited == 58 && hp::kForwardLeftOut == 62 && hp::kLeftOutNoGrid == 18 && hp::kLeftOutAmbient == 44 && hp::kLitShaders == 120 && hp::kForwardEdited + hp::kForwardLeftOut == hp::kLitShaders,
        "the forward stage edits 58 of the 120 lit shaders and leaves 62 out (18 with no grid, 44 with an ambient it does not know)");
}

// ---------------------------------------------------------------------------
// the picture-identity rule
// ---------------------------------------------------------------------------

// A picture's light: a smooth field of a given size, in linear luma.
std::vector<float> Field(Lcg& rng, size_t n, float base) {
  std::vector<float> f(n);
  for (size_t i = 0; i < n; ++i) {
    f[i] = base * (0.5f + 1.5f * float(i % 97) / 96.0f) * (0.8f + 0.4f * rng.Next());
  }
  return f;
}

void IdentityChecks() {
  Lcg rng;
  const size_t n = 20000;
  const std::vector<float> forward = Field(rng, n, 0.2f);
  // the same picture: identity
  {
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), forward.data(), n);
    Check(v.ok && v.enough && v.both == n && v.forward_only == 0 && v.pass_only == 0 && std::fabs(v.ratio - 1.0) < 1e-12 && v.same_share == 1.0, "the same picture is the identity");
  }
  // a path 3 per cent brighter everywhere: inside the ratio (0.95 .. 1.05) and every pixel within 10 per cent
  {
    std::vector<float> pass = forward;
    for (float& p : pass) p *= 1.03f;
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(v.ok && std::fabs(v.ratio - 1.03) < 1e-4 && v.same_share == 1.0, "3 per cent brighter passes");
  }
  // 8 per cent brighter: the mean is out of the 5 per cent band
  {
    std::vector<float> pass = forward;
    for (float& p : pass) p *= 1.08f;
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(!v.ok && v.enough && v.ratio > 1.05 && v.same_share == 1.0, "8 per cent brighter fails on the mean alone");
  }
  // the units wrong by 16 (the exposure applied twice or not at all): fails by a mile
  {
    std::vector<float> pass = forward;
    for (float& p : pass) p *= 16.0f;
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(!v.ok && v.ratio > 15.0 && v.same_share == 0.0, "the exposure applied twice fails");
    for (size_t i = 0; i < n; ++i) pass[i] = forward[i] * (1.0f / 16.0f);
    const hp::IdentityVerdict w = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(!w.ok && w.ratio < 0.07, "the exposure not applied fails");
  }
  // a mean that is right and a spread that is not: half the pixels 30 per cent up and half 30 per cent down average to the same and fail the per pixel share
  {
    std::vector<float> pass = forward;
    for (size_t i = 0; i < n; ++i) pass[i] *= i % 2 == 0 ? 1.3f : 0.7f;
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(!v.ok && std::fabs(v.ratio - 1.0) < 0.02 && v.same_share < 0.1, "a mean inside the band does not save a picture whose pixels are 30 per cent off");
  }
  // noise: 5 per cent of random error per pixel passes (the share within 10 per cent is about 100 per cent)
  {
    std::vector<float> pass = forward;
    for (float& p : pass) p *= 1.0f + 0.05f * (rng.Next() * 2.0f - 1.0f);
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(v.ok && v.same_share > 0.99, "5 per cent of noise passes");
  }
  // ... and 15 per cent of noise has most pixels outside 10 per cent
  {
    std::vector<float> pass = forward;
    for (float& p : pass) p *= 1.0f + 0.15f * (rng.Next() * 2.0f - 1.0f);
    const hp::IdentityVerdict v = hp::JudgeIdentity(forward.data(), pass.data(), n);
    Check(!v.ok && v.same_share < hp::kSameShareMin && std::fabs(v.ratio - 1.0) < 0.03, "15 per cent of noise fails the per pixel share");
  }
  // coverage: the pass lights what the forward stage leaves out (pass only), and misses some of what it lights (forward only)
  {
    std::vector<float> fwd = forward, pass = forward;
    for (size_t i = 0; i < 3000; ++i) fwd[i] = 0.0f;                 // 3000 pixels only the pass lights: the left-out shaders, the cars
    for (size_t i = n - 500; i < n; ++i) pass[i] = 0.0f;             // 500 pixels only the forward stage lights: the pass fails to cover them
    const hp::IdentityVerdict v = hp::JudgeIdentity(fwd.data(), pass.data(), n);
    Check(v.pass_only == 3000 && v.forward_only == 500 && v.both == n - 3500 && v.ok, "coverage is counted apart from identity: pass only 3000, forward only 500, identical on the rest");
    Check(v.forward_lit == n - 3000 && v.pass_lit == n - 500, "the lit counts of each path");
  }
  // too few pixels lit by both: nothing to judge (the light is in the wrong place), never a pass
  {
    std::vector<float> fwd(1000, 0.0f), pass(1000, 0.0f);
    for (size_t i = 0; i < hp::kMinBothPixels - 1; ++i) fwd[i] = pass[i] = 0.3f;
    hp::IdentityVerdict v = hp::JudgeIdentity(fwd.data(), pass.data(), fwd.size());
    Check(!v.enough && !v.ok && v.both == hp::kMinBothPixels - 1, "one pixel short of the minimum judges nothing");
    fwd[hp::kMinBothPixels - 1] = pass[hp::kMinBothPixels - 1] = 0.3f;
    v = hp::JudgeIdentity(fwd.data(), pass.data(), fwd.size());
    Check(v.enough && v.ok, "the minimum number of shared pixels judges");
    const std::vector<float> dark(500, 0.0f);
    v = hp::JudgeIdentity(dark.data(), dark.data(), dark.size());
    Check(!v.ok && v.both == 0 && v.ratio == 0.0, "two dark pictures are not an identity: nothing was lit");
  }
  // the floor: a pixel at the lit floor is not lit, and a forward pixel dimmer than 2 codes of mid grey is judged against that floor, not against itself
  {
    std::vector<float> dim(500, 0.01f);
    Check(hp::JudgeIdentity(dim.data(), dim.data(), dim.size()).both == 0, "a pixel exactly at the lit floor is not lit");
    std::vector<float> fwd(500, 0.001f), pass(500, 0.00112f);  // 12 per cent off, but 0.00012 is inside 10 per cent of the floor (0.00157)
    const hp::IdentityVerdict v = hp::JudgeIdentity(fwd.data(), pass.data(), fwd.size(), 0.0005f);
    Check(v.both == 500 && v.same_share == 1.0, "a pixel dimmer than the floor is judged against the floor");
    std::vector<float> bright(500, 0.1f), off(500, 0.112f);
    Check(hp::JudgeIdentity(bright.data(), off.data(), bright.size()).same_share == 0.0, "the same 12 per cent off on a bright pixel is outside 10 per cent");
  }
}

void GateChecks() {
  hp::GateInputs ok;
  ok.scenes = 4;
  ok.scenes_identical = 4;
  ok.forward_only_share = 0.01;
  ok.cost_over_4k_ms = 0.2;
  ok.cost_over_1080_ms = 0.1;
  ok.owner_saw = true;
  Check(hp::MayRetire(ok), "every scene identical, little forward only, the cost in budget and the owner's look: the forward stage may go");
  hp::GateInputs g = ok;
  g.owner_saw = false;
  Check(!hp::MayRetire(g), "NEVER without the owner having looked");
  g = ok;
  g.scenes_identical = 3;
  Check(!hp::MayRetire(g), "one scene that is not identical stops it");
  g = ok;
  g.scenes = 0;
  g.scenes_identical = 0;
  Check(!hp::MayRetire(g), "no scenes judged: no retirement");
  g = ok;
  g.forward_only_share = 0.05;
  Check(!hp::MayRetire(g), "a pass that misses 5 per cent of what the forward stage lights stops it");
  g = ok;
  g.cost_over_4k_ms = 0.31;
  Check(!hp::MayRetire(g), "0.31 ms over the forward stage at 4K stops it");
  g = ok;
  g.cost_over_1080_ms = 0.16;
  Check(!hp::MayRetire(g), "0.16 ms over at 1080p stops it");
  g = ok;
  g.forward_only_share = hp::kForwardOnlyShareMax;
  g.cost_over_4k_ms = hp::kCostOver4kMs;
  g.cost_over_1080_ms = hp::kCostOver1080Ms;
  Check(hp::MayRetire(g), "the limits themselves are in");
}

// ---------------------------------------------------------------------------
// the pixel identity: the pass's light and the forward stage's on the same lights, surfaces and maps
// ---------------------------------------------------------------------------

// The frame's shadow views the way the replay makes them for a level of LIGHT SHADOWS: `cars` cars have views, the first `lamp_cars` of them a view for each headlamp. The map of view e is a plane
// facing +z through the origin of its tile: a point's depth in the map is 0.5 + 0.01 z, uv = (x, y) x 0.025 + the tile's middle, so that a depth image (a wall that tilts) shadows part of the tiles.
struct Scene {
  ShadowFrame frame;
  LocalShadowFrame local;  // the same views as the forward stage reads them (the replay's frame)
  std::array<Light, 4> lights;
  uint32_t light_count = 0;
  std::array<uint32_t, 4> first{}, count{};
  float wall = 0.5f, tilt = 0.0f;
  uint32_t atlas_size = 128;
  float DepthAt(int ix, int iy) const {
    ix = std::clamp(ix, 0, int(atlas_size) - 1);
    iy = std::clamp(iy, 0, int(atlas_size) - 1);
    (void)iy;
    return wall + tilt * (float(ix) / float(atlas_size) - 0.5f);
  }
};

Light Headlight(uint64_t id, const float pos[3], const float dir[3], Lcg& rng) {
  Light l;
  l.law = LightLaw::kHeadlight;
  l.kind = LightKind::kSpot;
  l.source = LightSource::kHeadlight;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = pos[i];
    l.dir[i] = dir[i];
    l.color[i] = rng.Range(0.3f, 1.4f);
  }
  l.cos_outer = hr::CosOfDegrees(hr::kOuterDegrees);
  l.cos_inner = hr::CosOfDegrees(hr::kInnerDegrees);
  l.range = hr::kRange;
  l.law_a = hr::kFalloffDistance;
  l.id = id;
  l.roles = kRoleMaskAll;
  return l;
}

Scene MakeScene(uint32_t cars, uint32_t lamp_cars, Lcg& rng) {
  Scene s;
  s.wall = rng.Range(0.3f, 0.75f);
  s.tilt = rng.Range(-0.5f, 0.5f);
  uint32_t views = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const float pos[3] = {rng.Range(-6.0f, 6.0f), rng.Range(0.5f, 1.0f), rng.Range(-4.0f, 0.0f)};
    float dir[3] = {rng.Range(-0.3f, 0.3f), -0.04f, 1.0f};
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (float& d : dir) d /= len;
    s.lights[c] = Headlight(2000 + c, pos, dir, rng);
    s.first[c] = views;
    s.count[c] = c < cars ? (c < lamp_cars ? 2u : 1u) : 0u;
    if (c < cars) {
      s.lights[c].shadow.kind = ShadowKind::kAtlas;
      s.lights[c].shadow.first_view = uint8_t(views);
      s.lights[c].shadow.view_count = uint8_t(s.count[c]);
      s.lights[c].shadow.weight = 1.0f;
      for (uint32_t e = 0; e < s.count[c]; ++e) {
        LocalShadowView& v = s.frame.views[views];
        std::memset(&v, 0, sizeof(v));
        const float u0 = (views % 4) * 0.25f, v0 = (views / 4) * 0.5f;
        v.world_to_shadow[0] = 0.025f; v.world_to_shadow[3] = u0 + 0.125f;
        v.world_to_shadow[5] = 0.025f; v.world_to_shadow[7] = v0 + 0.25f;
        v.world_to_shadow[10] = 0.01f; v.world_to_shadow[11] = 0.5f;
        v.world_to_shadow[15] = 1.0f;
        v.atlas_rect[0] = u0; v.atlas_rect[1] = v0; v.atlas_rect[2] = u0 + 0.25f; v.atlas_rect[3] = v0 + 0.5f;
        v.depth_bias = rng.Range(0.0004f, 0.003f);
        v.texel_size = 1.0f / 32.0f;
        v.light_index = c;
        v.valid = 1;
        s.frame.view_light[views] = s.lights[c].id;
        s.frame.view_layer[views] = ShadowLayer::kSingle;
        s.local.views[views] = v;
        ++views;
      }
    }
  }
  s.light_count = 4;
  s.frame.count = views;
  s.local.count = views;
  s.frame.atlas.valid = true;
  s.frame.atlas.slot = 9;
  s.frame.atlas.sampler = 3;
  s.frame.atlas.width = s.atlas_size;
  s.frame.atlas.height = s.atlas_size;
  s.frame.atlas.tile_small = 32;
  return s;
}

// The forward stage's visibility of light `c` at a point: the mean of its views' soft box shares (headlight.cpp HeadlightLight: viewA / viewB, `seen / views`), 1 with none.
float ForwardVisibility(const Scene& s, uint32_t c, const float world[3]) {
  if (s.count[c] == 0) {
    return 1.0f;
  }
  float seen = 0.0f, views = 0.0f;
  for (uint32_t e = 0; e < s.count[c]; ++e) {
    const LocalShadowView& v = s.local.views[s.first[c] + e];
    float uv[2], depth = 0.0f;
    float vis = 1.0f;
    if (hr::ProjectToShadow(v, world, uv, &depth)) {
      const float tx = uv[0] / ((v.atlas_rect[2] - v.atlas_rect[0]) * v.texel_size);
      const float ty = uv[1] / ((v.atlas_rect[3] - v.atlas_rect[1]) * v.texel_size);
      vis = hr::SoftShadow(tx, ty, [&](int ix, int iy) { return hr::ShadowTap(s.DepthAt(ix, iy), depth, v.depth_bias); });
    }
    seen += vis;
    views += 1.0f;
  }
  return views > 0.0f ? seen / views : 1.0f;
}

hr::Lamp LampOf(const Light& l) {
  hr::Lamp lamp;
  for (int i = 0; i < 3; ++i) {
    lamp.position[i] = l.pos[i];
    lamp.direction[i] = l.dir[i];
    lamp.color[i] = l.color[i];
  }
  lamp.range = l.range;
  lamp.cos_outer = l.cos_outer;
  lamp.cos_inner = l.cos_inner;
  return lamp;
}

void PixelIdentityChecks() {
  Lcg rng;
  struct Level { const char* name; uint32_t cars; uint32_t lamp_cars; };
  uint64_t compared = 0, lit = 0, in_shadow = 0, edge = 0;
  for (const Level lv : {Level{"medium", 2, 0}, Level{"high", 2, 1}, Level{"ultra", 3, 3}}) {
    for (int round = 0; round < 40; ++round) {
      const Scene s = MakeScene(lv.cars, lv.lamp_cars, rng);
      lp::Units units;  // the units are the pass's own: the forward term is the light BEFORE the lit shader multiplies the sum, so they are 1 here
      units.night = rng.Range(0.0f, 1.0f);  // a headlight's colour holds its own night factor: the law ignores this (PixelLight)
      for (int px = 0; px < 30; ++px) {
        const float world[3] = {rng.Range(-10.0f, 10.0f), rng.Range(-1.0f, 3.0f), rng.Range(1.0f, 40.0f)};
        float n[3];
        rng.Direction(n);
        // the surface faces the viewer (both stages turn it: the forward stage by the camera, the G-buffer by the back face flip): take the side facing the lamps' side
        const float camera[3] = {0.0f, 1.5f, -6.0f};
        const float to_cam[3] = {camera[0] - world[0], camera[1] - world[1], camera[2] - world[2]};
        if (n[0] * to_cam[0] + n[1] * to_cam[1] + n[2] * to_cam[2] < 0.0f) {
          for (float& c : n) c = -c;
        }
        float fwd[3] = {0, 0, 0}, pass[3] = {0, 0, 0};
        for (uint32_t c = 0; c < s.light_count; ++c) {
          hr::Surface surface;
          for (int i = 0; i < 3; ++i) {
            surface.position[i] = world[i];
            surface.normal[i] = n[i];
          }
          const hr::Lamp lamp = LampOf(s.lights[c]);
          const float vis = ForwardVisibility(s, c, world);
          const float scale = hr::LampScale(lamp, surface, vis);
          float p[3];
          const float albedo[3] = {1.0f, 1.0f, 1.0f};
          const bool any = lp::PixelLightShadowed(s.lights[c], units, &s.frame, true, world, n, albedo, 1.0f, [&](int ix, int iy) { return s.DepthAt(ix, iy); }, p);
          // light by light: the forward term of this light and the pass's
          for (int i = 0; i < 3; ++i) {
            const double f = double(lamp.color[i]) * double(scale);
            CheckRel(p[i], f, 2.0e-5, 1.0e-7, "one light of one pixel: the pass equals the forward stage");
            fwd[i] += float(f);
            pass[i] += p[i];
          }
          ++compared;
          if (scale > 0.0f) {
            ++lit;
            if (s.count[c] != 0 && vis < 0.999f) ++in_shadow;
            if (s.count[c] != 0 && vis > 0.001f && vis < 0.999f) ++edge;
            Check(any, "a light the forward stage adds is a light the pass adds");
          }
        }
        for (int i = 0; i < 3; ++i) {
          CheckRel(pass[i], fwd[i], 5.0e-5, 1.0e-7, "the sum of the four lights: the pass equals the forward stage");
        }
      }
    }
  }
  Check(compared > 1000 && lit > 800 && in_shadow > 150 && edge > 20,
        "the random pixels are lit by a good share of the lights, and some are in a shadow and some at its edge (" + std::to_string(lit) + " lit of " + std::to_string(compared) + ", " + std::to_string(in_shadow) +
            " shadowed, " + std::to_string(edge) + " at an edge)");
}

// The units the pass applies on top (exposure, the height factor, the gain) are exactly the lit shader's multiplication of the sum: pass = forward x albedo x lit x exposure x H(y) x gain.
void UnitsIdentityChecks() {
  Lcg rng;
  const Scene s = MakeScene(3, 3, rng);
  for (int round = 0; round < 400; ++round) {
    lp::Units units;
    units.gain = rng.Range(0.5f, 2.0f);
    units.exposure = round % 2 == 0 ? 16.0f : rng.Range(0.5f, 20.0f);
    units.occ[0] = rng.Range(-5.0f, 5.0f);
    units.occ[1] = rng.Range(0.05f, 0.5f);
    units.occ[2] = rng.Range(0.5f, 1.5f);
    units.occ[3] = round % 3 == 0 ? 0.0f : 1.0f;
    const float world[3] = {rng.Range(-10.0f, 10.0f), rng.Range(-1.0f, 3.0f), rng.Range(1.0f, 40.0f)};
    const float n[3] = {0.0f, 1.0f, 0.0f};
    const float albedo[3] = {rng.Next(), rng.Next(), rng.Next()};
    const float lit = rng.Range(0.2f, 1.0f);
    for (uint32_t c = 0; c < 3; ++c) {
      float with_units[3], bare[3];
      lp::PixelLightShadowed(s.lights[c], units, &s.frame, true, world, n, albedo, lit, [&](int ix, int iy) { return s.DepthAt(ix, iy); }, with_units);
      const float ones[3] = {1.0f, 1.0f, 1.0f};
      lp::PixelLightShadowed(s.lights[c], lp::Units{}, &s.frame, true, world, n, ones, 1.0f, [&](int ix, int iy) { return s.DepthAt(ix, iy); }, bare);
      const double factor = double(units.gain) * double(units.exposure) * double(lp::OcclusionFactor(units.occ, world[1])) * double(lit);
      for (int i = 0; i < 3; ++i) {
        CheckRel(with_units[i], double(bare[i]) * double(albedo[i]) * factor, 2.0e-5, 1.0e-9, "the pass's units: the term x albedo x lit weight x exposure x height factor x gain");
      }
    }
  }
}

// What the frame holds for the pass at the three levels: the shadowed cars, their views, and the pass's own budget for them.
void LevelChecks() {
  struct Level { const char* name; uint32_t cars; uint32_t lamp_cars; uint32_t views; };
  Lcg rng;
  for (const Level lv : {Level{"medium", 2, 0, 2}, Level{"high", 2, 1, 3}, Level{"ultra", 3, 3, 6}}) {
    const Scene s = MakeScene(lv.cars, lv.lamp_cars, rng);
    Check(s.frame.count == lv.views, std::string(lv.name) + ": the table's views are the level's (" + std::to_string(lv.views) + ")");
    LightFrame frame;
    for (uint32_t c = 0; c < 4; ++c) frame.lights[c] = s.lights[c];
    frame.count = 4;
    frame.shadow = s.frame;
    const lp::Selection sel = lp::SelectLights(frame, lp::kSourceFrame, nullptr, 0, lp::BudgetOf(lp::Tier::kMedium).frame);
    uint32_t shadowed = 0;
    for (uint32_t i = 0; i < sel.count; ++i) shadowed += sel.lights[i].shadow.kind == ShadowKind::kAtlas ? 1u : 0u;
    Check(sel.count == 4 && shadowed == lv.cars, std::string(lv.name) + ": the pass takes the rig's four lights and " + std::to_string(lv.cars) + " of them are shadowed");
    Check(lp::ShadowTableNeeded(sel, &frame.shadow), std::string(lv.name) + ": the table is written for them");
    // the cost the pass pays for the shadows is the number of shadowed lights: every one is a draw with the lookup
    Check(shadowed <= 4 && lv.views <= kMaxLightShadowViews, std::string(lv.name) + ": at most four lookups' worth of lights, at most eight views");
  }
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

void SourceChecks(const std::string& src) {
  const std::string gpu = src + "/gpu/";
  const std::string rig = ReadFile(gpu + "headlight_rig.cpp");
  const std::string rig_h = ReadFile(gpu + "headlight_rig.h");
  const std::string pipelines = ReadFile(gpu + "pipelines.cpp");
  const std::string pass_cpp = ReadFile(gpu + "local_lights.cpp");
  const std::string pass_h = ReadFile(gpu + "local_lights.h");
  const std::string cmake = ReadFile(gpu + "CMakeLists.txt");
  const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
  const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
  const std::string menu = ReadFile(src + "/mc_engine/menus/debug_menu_pages.cpp");
  const std::string variant_chain = ReadFile(src + "/shaderpack/variant_chain.h");
  Check(!rig.empty() && !pipelines.empty() && !pass_cpp.empty() && !settings.empty() && !menu.empty() && !cmake.empty(), "the sources are readable: " + src);

  // the forward stage is chosen only while the route keeps it (pipelines.cpp asks VariantSelected)
  Check(Has(pipelines, "if (has(sp::kFeatureHeadlight) && headlight_rig::VariantSelected()) {"), "pipelines.cpp still asks the rig whether the variant is chosen");
  Check(Has(rig, "return g_selected.load(std::memory_order_relaxed) && larecomp::gpu::headlight_pass::ForwardStageOn(CurrentRoute());"), "the variant is chosen when the rig has a light AND the route keeps the forward stage");
  Check(Has(rig, "const larecomp::gpu::headlight_pass::Route route = larecomp::gpu::headlight_pass::ParseRoute(s::mcla_gpu_headlight_pass.Get());") &&
            Has(rig, "g_route.store(uint8_t(route), std::memory_order_relaxed);"),
        "the rig reads the route once a frame in its BeginFrame (ReadSettings)");
  // ... and keeps tracking, ranking and publishing in every route: the pass, the replay and the volume read its lights
  const size_t read_at = At(rig, "g_route.store(");
  const size_t enabled_at = At(rig, "enabled_ = memory_ != nullptr && headlight_rig::SettingOn();");
  Check(read_at != std::string::npos && enabled_at != std::string::npos && enabled_at < read_at, "HEADLIGHTS stays the master: the route is read after the rig's own enable");
  Check(Has(rig_h, "bool SettingOn();") && Has(rig_h, "CurrentRoute();") && Has(rig_h, "#include \"headlight_pass_rules.h\""), "the rig's header offers the setting's state and the route");

  // the pass: the route makes it run and lights the frame's headlights
  Check(Has(pass_cpp, "const bool on = hp::PassOn(route, setting_on);") && Has(pass_cpp, "hp::SourcesFor(route,"), "the pass runs for the route and adds the frame's source");
  Check(Has(pass_cpp, "planes_wanted_ = enabled_ && (setting_on || headlight_rig::SettingOn());"), "the planes are wanted for the route only while HEADLIGHTS is on");
  Check(Has(pass_h, "bool wants_draws() const { return enabled_ && planes_wanted_; }"), "the Scene feeds the pass's votes only while its planes are wanted");

  // the setting, in the three generated files, live and debug only, default forward
  Check(Has(settings, "std::string, mcla_gpu_headlight_pass, \"forward\",") && Has(settings, "inline auto& mcla_gpu_headlight_pass = g_gpu.mcla_gpu_headlight_pass;"), "settings_gpu.h defines the route and its alias");
  Check(Has(manifest, "\"mcla_gpu_headlight_pass\": {") && Has(meta, "{\"mcla_gpu_headlight_pass\", \"MCLA/GPU\"},"), "manifest.json and settings_meta.inc list it");
  {
    const size_t at = settings.find(", mcla_gpu_headlight_pass, ");
    const size_t end = settings.find(");", at);
    const std::string text = at == std::string::npos ? std::string() : settings.substr(at, end - at);
    Check(Has(text, "rex::kDebugOnly") && Has(text, "rex::Allowed(\"forward\", \"pass\", \"both\")") && !Has(text, "rex::kRequiresRestart"), "the route is debug only, live, and one of the three words");
  }
  // the hidden debug menu's row (the only place a player-invisible off switch of a fix goes: SETTINGS-REVIEW 7.1) and no row in a player page
  Check(Has(menu, "Str(\"PM_RxDbgHeadlightVia\", \"mcla_gpu_headlight_pass\", \"HEADLIGHTS VIA: \",") && Has(menu, "kHeadlightRouteVals[]  = {\"forward\", \"pass\", \"both\"};"), "the debug rendering page has the HEADLIGHTS VIA row with the three words");
  const std::string pause = ReadFile(src + "/mc_engine/menus/pause_menu.cpp");
  Check(!pause.empty() && !Has(pause, "mcla_gpu_headlight_pass"), "no player page has a row for it");

  // the forward stage is NOT deleted by this package: its variant, its row and its block are all still there
  Check(Has(variant_chain, "{kFeatureHeadlight, \"headlight\", Stage::kPixel, 1072,"), "the forward stage's row is still in the chain (the retirement waits for the owner)");
  Check(!ReadFile(src + "/shaderpack/variants/headlight.cpp").empty() && Has(ReadFile(gpu + "scene.cpp"), "case sp::kFeatureHeadlight:"), "its variant and its block's code are still there");

  // the CMake target of this test
  Check(Has(cmake, "add_executable(larecomp_headlight_pass_test EXCLUDE_FROM_ALL headlight_pass_test.cpp)"), "the CMake has this test");
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    std::string here = __FILE__;
    for (char& c : here) {
      if (c == '\\') c = '/';
    }
    const size_t slash = here.rfind('/');
    src = slash == std::string::npos ? std::string(".") : here.substr(0, slash) + "/..";
  }
  RouteChecks();
  IdentityChecks();
  GateChecks();
  PixelIdentityChecks();
  UnitsIdentityChecks();
  LevelChecks();
  SourceChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
