// Check of the forward headlight stage against the deferred local light pass (headlight_pass_rules.h; perf/LIGHT-UNIFY.md 6.3 and 11.2, package GB-H2): the switch's words and what each route does, the
// retail census of the forward stage, the picture-identity rule on made-up pictures (equal, a little off, far off, noisy, too few pixels, coverage), the retirement gate, and the PIXEL IDENTITY: on
// random lights, surfaces and shadow maps the pass's light (local_lights_rules.h PixelLightShadowed: the one law and the one lookup) equals the forward stage's (headlight_rig_rules.h LampScale,
// and light_shadow_filter.h ViewLit, the mean of a light's views: the CPU twins of the variant's text; the shadow's receiver is the surface's unit normal in both) to the last bits of a float, for the
// lights the frame holds at all three levels of LIGHT SHADOWS.
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
  Check(hp::ForwardStageOn(R::kForward) && !hp::ForwardStageOn(R::kPass) && hp::ForwardStageOn(R::kBoth), "the forward stage is off only in the route `pass`, and there only with the A/B switch off");
  // GB-H2-FOLLOW: with mcla_gpu_headlight_pass_keep_forward on, `pass` still asks for the variant (the Scene drops it per draw); the other routes do not care
  Check(hp::ForwardStageOn(R::kPass, true) && !hp::ForwardStageOn(R::kPass, false) && hp::ForwardStageOn(R::kForward, true) && hp::ForwardStageOn(R::kForward, false) &&
            hp::ForwardStageOn(R::kBoth, true) && hp::ForwardStageOn(R::kBoth, false),
        "the A/B switch (keep the forward stage in `pass`) changes the route pass only");
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

// ---------------------------------------------------------------------------
// the draws in route `pass`: every pixel is lit once (GB-H2-FOLLOW)
// ---------------------------------------------------------------------------

void DrawChecks() {
  using R = hp::Route;
  using C = hp::Carry;
  const C carries[] = {C::kNone, C::kClear, C::kMask, C::kWrite};
  const char* carry_names[] = {"not carried", "clearing the planes", "masking the planes", "writing the planes"};
  int pass_lit = 0, forward_kept = 0;
  for (int ci = 0; ci < 4; ++ci) {
    for (const bool ready : {false, true}) {
      for (const bool lit : {false, true}) {
        hp::DrawCover cover;
        cover.carry = carries[ci];
        cover.planes_ready = ready;
        cover.shader_lit = lit;
        const std::string what = std::string(carry_names[ci]) + (ready ? ", planes ready" : ", no planes") + (lit ? ", lit shader" : ", emissive shader");
        // the pass lights a draw exactly when it has planes to read, a lit shader, and the draw's pixels carry a surface (it wrote it, or it is a blend over one)
        const bool want = ready && lit && (carries[ci] == C::kWrite || carries[ci] == C::kMask);
        Check(hp::PassLightsDraw(cover) == want, "the pass lights: " + what);
        // forward and both never drop the stage; pass drops it exactly where the pass lights: a pixel is lit by one of the two, never zero and never both
        Check(!hp::DropsForwardStage(R::kForward, cover) && !hp::DropsForwardStage(R::kBoth, cover), "forward and both keep the stage on every draw: " + what);
        Check(hp::DropsForwardStage(R::kPass, cover) == hp::PassLightsDraw(cover), "pass drops the stage exactly where the pass lights: " + what);
        const bool forward_lights = !hp::DropsForwardStage(R::kPass, cover);
        const bool pass_lights = hp::PassLightsDraw(cover);
        Check(forward_lights != pass_lights, "in pass exactly one of the two lights the draw (not zero, not twice): " + what);
        pass_lit += pass_lights ? 1 : 0;
        forward_kept += forward_lights ? 1 : 0;
      }
    }
  }
  Check(pass_lit == 2 && forward_kept == 14, "of the 16 kinds of draw the pass lights 2 (written or masked, with planes ready and a lit shader) and the forward stage the other 14");
  // the named cases
  hp::DrawCover neon{C::kWrite, true, false};       // an emissive shader that writes its planes (the 7 emissive exempt shaders): the pass skips its class
  hp::DrawCover wall{C::kWrite, true, true};        // a lit writer: the pass
  hp::DrawCover decal{C::kMask, true, true};        // a blend over a lit surface (a decal, the tangent decal): the pass lights the surface under it, a forward term would light that share twice
  hp::DrawCover glass{C::kMask, true, false};       // a blended emissive shader (lamp glass, an additive tint): it adds its own light, the surface under it is lit by the pass
  hp::DrawCover cleared{C::kClear, true, true};     // a draw whose shader has no G-buffer word: it clears the planes, the pass skips those pixels
  hp::DrawCover side_view{C::kNone, true, true};    // a draw of another view or a pending pipeline: no planes on it
  hp::DrawCover no_planes{C::kWrite, false, true};  // the frame has no albedo plane: the pass cannot run
  Check(hp::DropsForwardStage(R::kPass, wall), "a lit writer drops the forward headlight in pass");
  Check(!hp::DropsForwardStage(R::kPass, neon), "an emissive shader keeps the forward headlight in pass");
  Check(hp::DropsForwardStage(R::kPass, decal), "a blend over a lit surface drops the forward headlight in pass (the pass lights the surface under it)");
  Check(!hp::DropsForwardStage(R::kPass, glass), "a blended emissive shader keeps the forward headlight in pass (it adds its own light)");
  Check(!hp::DropsForwardStage(R::kPass, cleared) && !hp::DropsForwardStage(R::kPass, side_view) && !hp::DropsForwardStage(R::kPass, no_planes), "a draw with no planes of its own, or a frame with no albedo plane, keeps it");
  // the default is a draw that is lit and not carried: the forward stage (nothing is dark by default)
  Check(!hp::DropsForwardStage(R::kPass, hp::DrawCover{}), "a draw the Scene knows nothing about keeps the forward stage");
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

// GB-H2-REST: the screen-space occlusion's share of the identity (headlight_pass_rules.h ClassifyIdentity). The forward stage's headlight is darkened by the occlusion's apply and the pass's is not, so with
// the occlusion on the pass is brighter by its factor and never darker; with it off the rule stands as it is.
void AoShareChecks() {
  Lcg rng;
  const size_t n = 20000;
  const std::vector<float> forward = Field(rng, n, 0.2f);
  auto scaled = [&](float k) {
    std::vector<float> p = forward;
    for (float& x : p) x *= k;
    return hp::JudgeIdentity(forward.data(), p.data(), n);
  };
  using C = hp::IdentityClass;
  const hp::IdentityVerdict same = scaled(1.0f);
  const hp::IdentityVerdict ao14 = scaled(1.14f);   // the integ3 chase picture: 14 per cent brighter, every pixel 14 per cent (the same share is 0)
  const hp::IdentityVerdict ao08 = scaled(1.08f);
  const hp::IdentityVerdict dark = scaled(0.90f);
  const hp::IdentityVerdict two = scaled(2.2f);
  const hp::IdentityVerdict sixteen = scaled(16.0f);
  // a picture the rule passes is identical whatever the occlusion says
  Check(hp::ClassifyIdentity(same, false, 0.0) == C::kIdentical && hp::ClassifyIdentity(same, true, 0.0) == C::kIdentical && hp::ClassifyIdentity(same, false, 1.0) == C::kIdentical, "a picture the rule passes is identical with the occlusion on, off, or a twin");
  // no judgement under the pixel minimum
  {
    std::vector<float> f(50, 0.3f);
    const hp::IdentityVerdict v = hp::JudgeIdentity(f.data(), f.data(), f.size());
    Check(hp::ClassifyIdentity(v, false, 0.0) == C::kNotEnough && hp::ClassifyIdentity(v, true, 1.0) == C::kNotEnough, "too few pixels lit by both: nothing to judge, with or without the occlusion");
  }
  // the occlusion is OFF: nothing excuses a miss
  Check(hp::ClassifyIdentity(ao14, true, 0.0) == C::kFail && hp::ClassifyIdentity(ao14, true, 1.0) == C::kFail && hp::ClassifyIdentity(ao08, true, 0.0) == C::kFail, "with the occlusion off a pass 8 or 14 per cent brighter is a fail");
  Check(hp::ClassifyIdentity(dark, true, 0.0) == C::kFail, "with the occlusion off a pass 10 per cent darker is a fail");
  // no twin: the excess the occlusion can explain is a CHECK, not a pass; what it cannot is a fail
  Check(hp::ClassifyIdentity(ao14, false, 0.0) == C::kAoExpected && hp::ClassifyIdentity(ao08, false, -1.0) == C::kAoExpected, "no twin: 8 and 14 per cent brighter are the occlusion's share to be confirmed (a CHECK)");
  Check(hp::ClassifyIdentity(dark, false, 0.0) == C::kFail, "no twin: a pass 10 per cent DARKER than forward is a fail (the occlusion only takes light from forward)");
  Check(hp::ClassifyIdentity(two, false, 0.0) == C::kFail && hp::ClassifyIdentity(sixteen, false, 0.0) == C::kFail, "no twin: 2.2 times (the baked vertex occlusion) and 16 times (the exposure) are faults of the pass");
  {
    const hp::IdentityVerdict edge_in = scaled(1.24f), edge_out = scaled(1.26f), edge_low = scaled(0.96f), edge_under = scaled(0.94f);
    Check(hp::ClassifyIdentity(edge_in, false, 0.0) == C::kAoExpected && hp::ClassifyIdentity(edge_out, false, 0.0) == C::kFail, "no twin: the limit is 1.25 (kAoBrighterMax): 1.24 is the occlusion's, 1.26 is not");
    Check(hp::ClassifyIdentity(edge_low, false, 0.0) == C::kIdentical && hp::ClassifyIdentity(edge_under, false, 0.0) == C::kFail, "no twin: the low end is the rule's 0.95: 0.96 is identical, 0.94 is a fail");
  }
  // a twin: the share of the occlusion is this ratio over the twin's
  Check(hp::ClassifyIdentity(ao14, false, 1.02) == C::kAoShare, "a twin at 1.02 and 1.14 with the occlusion on: share 1.12, the occlusion's");
  Check(hp::ClassifyIdentity(ao14, false, 1.14) == C::kAoShare, "a twin as bright as the picture: share 1.0, the occlusion did nothing here and the pass is still the twin's");
  Check(hp::ClassifyIdentity(ao14, false, 0.90) == C::kFail, "a twin at 0.90 and 1.14: share 1.27, more than the occlusion can take");
  Check(hp::ClassifyIdentity(ao08, false, 1.20) == C::kFail, "a picture DARKER than its occlusion-off twin (1.08 against 1.20: share 0.90) is a fail");
  {
    // the limits of the share are in
    const hp::IdentityVerdict v = scaled(1.10f);
    const double r = v.ratio;
    Check(hp::ClassifyIdentity(v, false, r / (hp::kAoShareHigh - 0.01)) == C::kAoShare && hp::ClassifyIdentity(v, false, r / (hp::kAoShareHigh + 0.01)) == C::kFail, "the share's upper limit (1.25): 1.24 is the occlusion's, 1.26 is not");
    Check(hp::ClassifyIdentity(v, false, r / (hp::kAoShareLow + 0.01)) == C::kAoShare && hp::ClassifyIdentity(v, false, r / (hp::kAoShareLow - 0.01)) == C::kFail, "the share's lower limit (0.98): 0.99 is in, 0.97 is a fail");
  }
  Check(hp::kAoShareLow < 1.0 && hp::kAoShareHigh > 1.0 && hp::kAoBrighterMax == hp::kAoShareHigh, "the limits are ordered: the share may be a hair under 1 and up to the no-twin limit");
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
        // a perspective view looking along +z from the origin (w = z, the unit axis in row 3, A times it in row 2: near plane 5, far plane 50, so a point's depth is A (1 - 5 / z)): the plane lookup
        // reads the view the way the replay makes it (HL-SHADOW-QUALITY)
        constexpr float kA = 50.0f / 45.0f;
        v.world_to_shadow[0] = 0.125f / 0.7f; v.world_to_shadow[2] = u0 + 0.125f;
        v.world_to_shadow[5] = 0.25f / 0.7f; v.world_to_shadow[6] = v0 + 0.25f;
        v.world_to_shadow[10] = kA; v.world_to_shadow[11] = -kA * 5.0f;
        v.world_to_shadow[14] = 1.0f;
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

// The forward stage's visibility of light `c` at a point of the surface with the unit normal `n`: the mean of its views' lookups (headlight.cpp HeadlightLight: viewA / viewB, `seen / views`; each view
// is HeadlightShadow, light_shadow_filter.h ViewLit its CPU twin: four gathers at the receiver's plane), 1 with none.
float ForwardVisibility(const Scene& s, uint32_t c, const float world[3], const float n[3]) {
  if (s.count[c] == 0) {
    return 1.0f;
  }
  float seen = 0.0f, views = 0.0f;
  for (uint32_t e = 0; e < s.count[c]; ++e) {
    const LocalShadowView& v = s.local.views[s.first[c] + e];
    const auto gather = [&](int ix, int iy) {
      larecomp::gpu::shadow_filter::Quad q;
      q.x = s.DepthAt(ix, iy + 1);
      q.y = s.DepthAt(ix + 1, iy + 1);
      q.z = s.DepthAt(ix + 1, iy);
      q.w = s.DepthAt(ix, iy);
      return q;
    };
    const float vis = larecomp::gpu::shadow_filter::ViewLit(v, world, n, gather);
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
          const float vis = ForwardVisibility(s, c, world, n);
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
  Check(Has(rig, "return g_selected.load(std::memory_order_relaxed) && larecomp::gpu::headlight_pass::ForwardStageOn(CurrentRoute(), g_keep_in_pass.load(std::memory_order_relaxed));"),
        "the variant is asked for when the rig has a light AND the route keeps the forward stage (in `pass`: with the A/B switch on)");
  Check(Has(rig, "g_keep_in_pass.store(s::mcla_gpu_headlight_pass_keep_forward.Get(), std::memory_order_relaxed);") && Has(rig, "std::atomic<bool> g_keep_in_pass{true};"),
        "the rig reads the A/B switch once a frame, in its BeginFrame, and keeps the stage by default");
  // LIGHT-STACK-REVIEW F3: the route's string setting is read ONCE a frame through LiveText (the debug menu's thread writes the cvar), at the Scene's frame start (headlight_rig::SampleRoute, before the light pass's
  // BeginFrame and the rig's), and both take it from CurrentRoute()
  const std::string scene = ReadFile(gpu + "scene.cpp");
  Check(Has(rig, "larecomp::gpu::headlight_pass::Route SampleRoute() {") &&
            Has(rig, "const larecomp::gpu::headlight_pass::Route route = larecomp::gpu::headlight_pass::ParseRoute(light_text::HeadlightPass().Get());") &&
            Has(rig, "g_route.store(uint8_t(route), std::memory_order_relaxed);") && !Has(rig, "mcla_gpu_headlight_pass.Get()") && !Has(pass_cpp, "mcla_gpu_headlight_pass.Get()") &&
            Has(pass_cpp, "const hp::Route route = headlight_rig::CurrentRoute();"),
        "the route is read once a frame through LiveText (SampleRoute); the rig and the pass take it from CurrentRoute()");
  const size_t sample_at = At(scene, "headlight_rig::SampleRoute();");
  const size_t lights_at = At(scene, "local_lights_->BeginFrame(pause_fx_candidate);");
  const size_t rig_at = At(scene, "headlights_->BeginFrame();");
  Check(sample_at != std::string::npos && lights_at != std::string::npos && rig_at != std::string::npos && sample_at < lights_at && lights_at < rig_at,
        "the Scene samples the route before the light pass's BeginFrame and the rig's");
  // the picker's mode and the record path (also strings another thread may write) are read through LiveText by every reader: the rig, the view replay and the manager's census line
  const std::string shadow = ReadFile(gpu + "local_shadow.cpp");
  const std::string manager = ReadFile(gpu + "light_manager.cpp");
  const std::string text_h = ReadFile(gpu + "light_text.h");
  Check(Has(rig, "tracker_.SetMode(light_pick::ParseMode(light_text::LightPick().Get()));") && Has(shadow, "picker_.SetMode(light_pick::ParseMode(light_text::LightPick().Get()));") &&
            Has(manager, "light_pick::ParseMode(light_text::LightPick().Get())") && Has(rig, "const std::string record_path = light_text::LightPickRecord().Get();") &&
            Has(shadow, "const std::string record_path = light_text::LightPickRecord().Get();") && !Has(rig, "mcla_gpu_light_pick.Get()") && !Has(shadow, "mcla_gpu_light_pick.Get()") &&
            !Has(manager, "mcla_gpu_light_pick.Get()") && !Has(rig, "mcla_gpu_light_pick_record.Get()") && !Has(shadow, "mcla_gpu_light_pick_record.Get()"),
        "mcla_gpu_light_pick and mcla_gpu_light_pick_record are read through LiveText by the rig, the view replay and the manager (review F3)");
  Check(Has(text_h, "#include \"live_text.h\"") && Has(text_h, "static LiveText text(\"mcla_gpu_headlight_pass\");") && Has(text_h, "static LiveText text(\"mcla_gpu_light_pick\");") && Has(text_h, "static LiveText text(\"mcla_gpu_light_pick_record\");"),
        "light_text.h holds one LiveText for each of the three settings");
  // GB-H2-FOLLOW: the Scene drops the stage per draw, in the G-buffer's key, through the rule
  {
    const std::string scene_cpp = ReadFile(gpu + "scene.cpp");
    Check(Has(scene_cpp, "headlight_rig::CurrentRoute() == hp::Route::kPass && chain::HasFeature(d.key.ps_variant, larecomp::shaderpack::kFeatureHeadlight)") && Has(scene_cpp, "hp::DropsForwardStage(hp::Route::kPass, cover)"),
          "Scene::GBufferKey drops the forward stage in route pass, through headlight_pass::DropsForwardStage, for a draw that asked for it");
    Check(Has(scene_cpp, "cover.shader_lit = !d.ps || gbuffer_materials_.PassLights(d.ps->key);") && Has(scene_cpp, "cover.planes_ready = (plan.attached & kPassPlanes) == kPassPlanes;"),
          "... with the shader's class from the material table and the frame's planes");
    Check(Has(scene_cpp, "case gbuffer::PlaneMode::kWrite: cover.carry = hp::Carry::kWrite; break;") && Has(scene_cpp, "case gbuffer::PlaneMode::kMask: cover.carry = hp::Carry::kMask; break;") &&
              Has(scene_cpp, "case gbuffer::PlaneMode::kClear: cover.carry = hp::Carry::kClear; break;") && Has(scene_cpp, "case gbuffer::PlaneMode::kNone: cover.carry = hp::Carry::kNone; break;"),
          "... the planes' mode is the rule's carry, one for one");
    Check(Has(scene_cpp, "key.ps_variant = dropped_word;"), "... a masking blend that drops the stage takes its word without it");
    // the order: the dropped draw's decision replaces the first one, and the key is built from the decision that stands
    const size_t drop_at = At(scene_cpp, "forward_dropped = true;");
    const size_t note_at = At(scene_cpp, "gbuffer_->NoteDraw(decision.why, r.ps_identity, facts.main_samples);");
    Check(drop_at != std::string::npos && note_at != std::string::npos && drop_at < note_at, "the drop is decided before the draw is noted and its key built");
    Check(Has(rig_h, "void NoteDroppedDraw() { ++dropped_draws_; }") && Has(scene_cpp, "headlights_->NoteDroppedDraw();"), "the rig counts the draws that dropped the stage (its census line)");
  }
  // ... and keeps tracking, ranking and publishing in every route: the pass, the replay and the volume read its lights; HEADLIGHTS stays the master (the route is no part of the rig's enable)
  const size_t enabled_at = At(rig, "enabled_ = memory_ != nullptr && headlight_rig::SettingOn();");
  Check(enabled_at != std::string::npos && rig.substr(enabled_at, rig.find('\n', enabled_at) - enabled_at).find("route") == std::string::npos, "HEADLIGHTS stays the master: the route is no part of the rig's own enable");
  Check(Has(rig_h, "bool SettingOn();") && Has(rig_h, "CurrentRoute();") && Has(rig_h, "SampleRoute();") && Has(rig_h, "#include \"headlight_pass_rules.h\""), "the rig's header offers the setting's state and the route");

  // the pass: the route makes it run and lights the frame's headlights
  Check(Has(pass_cpp, "const bool on_for_lights = garage || hp::PassOn(route, setting_on);") && Has(pass_cpp, "hp::SourcesFor(route,"), "the pass runs for the route and adds the frame's source");
  Check(Has(pass_cpp, "planes_wanted_ = enabled_ && (garage || setting_on || (hp::PassLightsHeadlights(route) && headlight_rig::SettingOn()) || neon_active_);"),
        "the planes are wanted for the route only while HEADLIGHTS is on (GB-NEON: and for a lit neon car; the same as before without one)");
  Check(Has(pass_h, "bool wants_draws() const { return enabled_ && planes_wanted_; }"), "the Scene feeds the pass's votes only while its planes are wanted");

  // the setting, in the three generated files, live and debug only, default forward
  Check(Has(settings, "std::string, mcla_gpu_headlight_pass, \"forward\",") && Has(settings, "inline auto& mcla_gpu_headlight_pass = g_gpu.mcla_gpu_headlight_pass;"), "settings_gpu.h defines the route and its alias");
  Check(Has(manifest, "\"mcla_gpu_headlight_pass\": {") && Has(meta, "{\"mcla_gpu_headlight_pass\", \"MCLA/GPU\"},"), "manifest.json and settings_meta.inc list it");
  // the A/B switch of GB-H2-FOLLOW: a bool, on by default, debug only, live, listed in the three files, and a row on the debug menu's rendering page
  Check(Has(settings, "bool, mcla_gpu_headlight_pass_keep_forward, true,") && Has(settings, "inline auto& mcla_gpu_headlight_pass_keep_forward = g_gpu.mcla_gpu_headlight_pass_keep_forward;"), "settings_gpu.h defines the keep-forward switch (default on) and its alias");
  Check(Has(manifest, "\"mcla_gpu_headlight_pass_keep_forward\": {") && Has(meta, "{\"mcla_gpu_headlight_pass_keep_forward\", \"MCLA/GPU\"},"), "manifest.json and settings_meta.inc list the keep-forward switch");
  {
    const size_t at = settings.find(", mcla_gpu_headlight_pass_keep_forward, ");
    const size_t end = settings.find(");", at);
    const std::string text = at == std::string::npos ? std::string() : settings.substr(at, end - at);
    Check(Has(text, "rex::kDebugOnly") && !Has(text, "rex::kRestart"), "the keep-forward switch is debug only and live");
  }
  {
    // the row sits on the rendering page, right after HEADLIGHTS VIA (the FIXES page is at the native page's cap of 24 rows: pause_menu.cpp kMaxRowsPerMenu refuses a page over it whole)
    const size_t via = At(menu, "Str(\"PM_RxDbgHeadlightVia\"");
    const size_t keep = At(menu, "Bool(\"PM_RxDbgHeadlightKeep\", \"mcla_gpu_headlight_pass_keep_forward\", \"PASS FORWARD HEADLIGHT: \")");
    const size_t fixes = At(menu, "const ItemDef kDebugFixesItems[]");
    const size_t rendering = At(menu, "const ItemDef kDebugRenderingItems[]");
    Check(via != std::string::npos && keep != std::string::npos && via < keep && keep - via < 1200 && rendering != std::string::npos && rendering < keep, "the debug rendering page has the keep-forward row right after HEADLIGHTS VIA");
    Check(fixes != std::string::npos && rendering != std::string::npos && fixes < rendering && !(fixes < At(menu, "PM_RxDbgHeadlightKeep") && At(menu, "PM_RxDbgHeadlightKeep") < At(menu, "const ItemDef kDebugPerformanceItems[]")),
          "... and not on the FIXES page, which is already at the 24 row cap");
  }
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
  DrawChecks();
  IdentityChecks();
  AoShareChecks();
  GateChecks();
  PixelIdentityChecks();
  UnitsIdentityChecks();
  LevelChecks();
  SourceChecks(src);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
