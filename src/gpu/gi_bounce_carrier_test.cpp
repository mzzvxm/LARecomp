// Check of the rule that joins the two halves of the lights that bounce (gi_bounce_carrier.h, GI-BOUNCE-JOIN, perf/GI-BOUNCE.md section JOIN): WHO CARRIES A LIGHT'S BOUNCE at every ray tracing tier, with every
// switch, for every kind of light, so that each light bounces exactly once (no gap, no double count); the partition of the light pass's draws; the plan of the colour chain (lights-only where the probe field
// stands); and, given the src/gpu folder, the text of the places that wire it. A program of its own: no renderer, no ReXGlue, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gi_bounce_carrier_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gi_bounce_carrier_test [<src/gpu dir>]
//
// perf/scripts/gi_bounce_join_mutations.py changes one statement at a time and checks that this test notices.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "gi_bounce_carrier.h"
#include "local_lights_rules.h"

namespace gb = mcla::gpu::gi_bounce;
namespace re = mcla::gpu::rt_emitters;
namespace lg = larecomp::gpu;
namespace ll = larecomp::gpu::local_lights;

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

// ---------------------------------------------------------------------------
// the lights
// ---------------------------------------------------------------------------

enum class Kind { kStreetLamp, kHeadlight, kNeon, kRear, kPolice, kMod, kCount };
constexpr const char* KindName(Kind k) {
  switch (k) {
    case Kind::kStreetLamp: return "street lamp";
    case Kind::kHeadlight: return "headlight";
    case Kind::kNeon: return "neon";
    case Kind::kRear: return "rear lamp";
    case Kind::kPolice: return "police";
    case Kind::kMod: return "mod lamp";
    case Kind::kCount: break;
  }
  return "?";
}
constexpr uint32_t GroupOfKind(Kind k) {
  return k == Kind::kHeadlight ? re::kGroupHeadlights : k == Kind::kNeon ? re::kGroupNeon : k == Kind::kRear ? re::kGroupRear : (k == Kind::kPolice || k == Kind::kMod) ? re::kGroupOther : 0u;
}

lg::Light MakeLight(Kind k, uint64_t id = 7) {
  lg::Light l;
  l.id = id;
  l.range = 12.0f;
  l.color[0] = 1.0f;
  l.color[1] = 0.6f;
  l.color[2] = 0.3f;
  switch (k) {
    case Kind::kStreetLamp: l.source = lg::LightSource::kLamp; l.law = lg::LightLaw::kGameLamp; break;
    case Kind::kHeadlight: l.source = lg::LightSource::kHeadlight; l.law = lg::LightLaw::kHeadlight; l.car_key = 1; break;
    case Kind::kNeon: l.source = lg::LightSource::kNeon; l.law = lg::LightLaw::kOmni; l.car_key = 1; break;
    case Kind::kRear: l.source = lg::LightSource::kOther; l.law = lg::LightLaw::kOmni; l.car_key = 1; break;
    case Kind::kPolice: l.source = lg::LightSource::kPolice; l.law = lg::LightLaw::kOmni; l.car_key = 2; break;
    case Kind::kMod: l.source = lg::LightSource::kMod; l.law = lg::LightLaw::kGameLamp; break;
    case Kind::kCount: break;
  }
  return l;
}

constexpr gb::Listed kListed[3] = {gb::Listed::kNo, gb::Listed::kStatic, gb::Listed::kMoving};

void CheckRoutes() {
  using C = gb::Carrier;
  for (bool field : {false, true}) {
    for (bool enabled : {false, true}) {
      for (uint32_t groups = 0; groups <= re::kGroupAll; ++groups) {
        gb::Frame frame{groups, field, enabled};
        for (int i = 0; i < int(Kind::kCount); ++i) {
          const Kind kind = Kind(i);
          const lg::Light light = MakeLight(kind);
          for (const gb::Listed listed : kListed) {
            const bool parked = listed == gb::Listed::kStatic;
            const bool city = kind == Kind::kStreetLamp;
            const bool on = city || (groups & GroupOfKind(kind)) != 0;
            const bool goes_to_field = on && field && (city || parked);
            const bool goes_to_screen = on && !field && (city || kind == Kind::kMod);
            const bool goes_to_virtual = on && enabled && !goes_to_field && !city && kind != Kind::kMod;
            const C expected = goes_to_field ? C::kField : goes_to_screen ? C::kScreen : goes_to_virtual ? C::kVirtual : C::kNone;
            Check(gb::CarrierOf(frame, light, listed) == expected, KindName(kind));
            Check(gb::InPair(frame, light, listed, 1.0f, 1.0f) == goes_to_screen, "only a screen light enters the capture pair");
          }
        }
      }
    }
  }

  for (int i = 0; i <= 100; ++i) {
    const float field = float(i) / 100.0f;
    Check(gb::VirtualShare(true, field) + field == 1.0f, "a parked light crossfades at constant total share");
    Check(gb::VirtualShare(false, field) == 1.0f, "a moving light keeps its energy during a field fade");
  }
}

void CheckBounces() {
  lg::Light head = MakeLight(Kind::kHeadlight);
  lg::Light neon = MakeLight(Kind::kNeon);
  Check(gb::Bounces(neon, 1.0f, 1.0f) && !gb::Bounces(neon, 0.0f, 1.0f), "a game lamp's law is times the night factor: neon at night bounces, by day it does not");
  Check(gb::Bounces(head, 0.0f, 1.0f), "a headlight's colour holds its own night factor: it is not times the game's");
  head.color[0] = head.color[1] = head.color[2] = 0.0f;
  Check(!gb::Bounces(head, 1.0f, 1.0f), "a headlight with no colour (off) does not");
  neon.range = 0.0f;
  Check(!gb::Bounces(neon, 1.0f, 1.0f), "a light with no range is off");
  neon = MakeLight(Kind::kNeon);
  Check(!gb::Bounces(neon, 1.0f, 0.0f) && gb::Bounces(neon, 1.0f, 2.0f), "the pass's gain scales it (0 is no light)");
  neon.color[0] = std::nanf("");
  Check(gb::Bounces(neon, 1.0f, 1.0f), "a NaN channel is ignored (the other channels still light)");
  neon.color[1] = neon.color[2] = std::nanf("");
  Check(!gb::Bounces(neon, 1.0f, 1.0f) && !gb::Bounces(MakeLight(Kind::kNeon), std::nanf(""), 1.0f), "all channels NaN, or a night factor that is not a number: no light");
  // the same threshold as the field's own filter: what the RT list drops as dark is not carried by the pair
  lg::Light dim = MakeLight(Kind::kNeon);
  dim.color[0] = dim.color[1] = dim.color[2] = re::kMinColour * 0.99f;
  Check(!gb::Bounces(dim, 1.0f, 1.0f), "a colour a hair under the threshold: no light");
  Check(!gb::Bounces(dim, 2.0f, 1.0f), "the night factor cannot amplify the colour above the threshold");
  dim.color[0] = re::kMinColour;
  Check(gb::Bounces(dim, 1.0f, 1.0f), "a light at the RT list's colour threshold is included");
}

// ---------------------------------------------------------------------------
// the partition of the light pass's draws
// ---------------------------------------------------------------------------

void CheckPartition() {
  constexpr size_t N = 32;
  std::mt19937 rng(5);
  bool perm = true, front = true, stable = true;
  for (int round = 0; round < 3000; ++round) {
    std::array<bool, N> in_pair{};
    const uint32_t count = rng() % (N + 1);
    for (uint32_t i = 0; i < N; ++i) in_pair[i] = (rng() & 1) != 0;
    std::array<uint32_t, N> order{};
    const uint32_t outside = gb::PartitionOrder(in_pair, count, &order);
    uint32_t want_outside = 0;
    for (uint32_t i = 0; i < count; ++i) want_outside += in_pair[i] ? 0u : 1u;
    perm = perm && outside == want_outside;
    std::array<int, N> seen{};
    for (uint32_t s = 0; s < count; ++s) {
      perm = perm && order[s] < count;
      if (order[s] < count) ++seen[order[s]];
      front = front && (s < outside ? !in_pair[order[s]] : in_pair[order[s]]);
      if (s > 0 && s != outside) stable = stable && order[s] > order[s - 1];
    }
    for (uint32_t i = 0; i < count; ++i) perm = perm && seen[i] == 1;
  }
  Check(perm, "the partition is a permutation of the lights and counts the ones outside the pair");
  Check(front, "... the lights outside the pair first, the pair's last");
  Check(stable, "... each side keeps the selection's order (a car's neon tubes stay together, so the merge of PlanDraws still sees them)");
  std::array<bool, N> none{};
  std::array<uint32_t, N> order{};
  Check(gb::PartitionOrder(none, 0, &order) == 0 && gb::PartitionOrder(none, 5, &order) == 5 && order[0] == 0 && order[4] == 4, "all outside, or none: the order is as it was");
  std::array<bool, N> all{};
  all.fill(true);
  Check(gb::PartitionOrder(all, 5, &order) == 0 && order[0] == 0 && order[4] == 4, "all in the pair (RT off): nothing moves, the pass is what it was");
  Check(gb::PartitionOrder(all, 99, &order) == 0, "a count over the array is cut to it");
  Check(order.back() == N - 1, "the clamped count still includes the last slot");

  // PlanDraws over a range: a draw never reaches across the border, and the whole range is the old plan
  ll::Selection sel;
  ll::PixelRect rects[ll::kMaxPassLights];
  const uint32_t lights = 6;  // a car's tube of 6 lights: draws of at most kMaxDrawLights (4)
  for (uint32_t i = 0; i < lights; ++i) {
    sel.lights[i] = MakeLight(Kind::kNeon, 100 + i);
    sel.lights[i].car_key = 3;
    rects[i] = ll::PixelRect{10, 10, 50, 50};
  }
  sel.count = lights;
  std::array<ll::PassDraw, ll::kMaxPassLights> whole{}, a{}, b{};
  const uint32_t n_whole = ll::PlanDraws(sel, rects, 0, whole.data());
  const uint32_t n_default = ll::PlanDraws(sel, rects, 0, b.data(), 0, ll::kMaxPassLights);
  Check(n_whole == 2 && n_default == 2 && whole[0].first == 0 && whole[0].count == 4 && whole[1].first == 4 && whole[1].count == 2 && b[0].count == 4 && b[1].count == 2,
        "the whole range is the plan as it was (one car's six tube lights: draws of 4 and 2)");
  const uint32_t na = ll::PlanDraws(sel, rects, 0, a.data(), 0, 3);
  const uint32_t nb = ll::PlanDraws(sel, rects, 0, b.data(), 3, lights);
  Check(na == 1 && a[0].first == 0 && a[0].count == 3 && nb == 1 && b[0].first == 3 && b[0].count == 3, "a range of 3 and a range of 3: two draws, neither reaches across the border though the lights would merge");
  Check(ll::PlanDraws(sel, rects, 0, a.data(), 4, 4) == 0 && ll::PlanDraws(sel, rects, 0, a.data(), 6, 6) == 0, "an empty range plans nothing");
  rects[3] = ll::PixelRect{};
  rects[4] = ll::PixelRect{};
  rects[5] = ll::PixelRect{};
  Check(ll::PlanDraws(sel, rects, 0, b.data(), 3, lights) == 0, "a range whose lights are all off the picture draws nothing (no pair, no capture)");
}

std::string Read(const std::string& file) {
  std::ifstream in(file);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void CheckSources(const std::string& gpu) {
  const auto has = [&](const char* path, const char* word) {
    return Read(gpu + "/" + path).find(word) != std::string::npos;
  };
  Check(has("ao.cpp", "if (gi_apply::SsgiSkipped(share))"), "the field retires the whole SSGI chain when it stands");
  Check(has("ao.cpp", "screen_->WantLights(plan.gi.on && plan.gi.lights);"), "only a live screen chain asks for the capture pair");
  Check(has("vpl.cpp", "EmitterMotionOf(light.id) == RtWorldFrame::EmitterMotion::kStatic"), "parked routing follows the RT emitter tracker");
  Check(has("vpl.cpp", "gi_bounce::VirtualShare(listed_static, field_share)"), "the host uses the shared routing weight");
  Check(has("hlsl/gi_update_trace_cs.hlsl", "CITY_HIT_EMITTER_COUNT (g_world.emitterStatic)"), "probes exclude moving emitters");
  Check(has("hlsl/gi_pixel_trace_cs.hlsl", "CITY_HIT_EMITTER_COUNT (g_world.emitterStatic)"), "pixel GI also excludes moving emitters");
  Check(has("local_lights.cpp", "gi_bounce::InPair"), "direct lights still go on either side of the capture boundary");
  Check(has("scene.cpp", "vpl_->OnTrigger(summary, list, gbuffer_published, slot_, field_share)"), "scene records virtual bounce at the trigger");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) gpu_dir = argv[i];
  CheckRoutes();
  CheckBounces();
  CheckPartition();
  if (!gpu_dir.empty()) {
    CheckSources(gpu_dir);
  } else {
    std::printf("(no src/gpu folder given: the source text checks were skipped)\n");
  }
  std::printf("gi_bounce_carrier_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
