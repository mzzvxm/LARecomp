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
namespace sl = mcla::gpu::ssgi_lights;

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
const char* ListedName(gb::Listed l) { return l == gb::Listed::kNo ? "not listed" : (l == gb::Listed::kStatic ? "static" : "moving"); }

// The tiers: RT off (no field), HIGH (the field alone), ULTRA (the field and the per-pixel pass), and a frame where ULTRA's pass did not run.
struct Tier {
  const char* name;
  bool field_stands;
  bool pixel_pass;
};
constexpr Tier kTiers[4] = {{"RT off", false, false}, {"HIGH", true, false}, {"ULTRA", true, true}, {"ULTRA without the pass this frame", true, false}};

// ---------------------------------------------------------------------------
// the matrix of the header, written out again (not read from the code under test)
// ---------------------------------------------------------------------------

gb::Carrier Expected(const Tier& t, Kind k, gb::Listed listed) {
  using C = gb::Carrier;
  if (k == Kind::kStreetLamp) return t.field_stands ? C::kField : C::kScreen;
  if (!t.field_stands) return C::kScreen;
  if (listed == gb::Listed::kStatic) return C::kField;
  if (listed == gb::Listed::kMoving) return t.pixel_pass ? C::kPixelPass : C::kScreen;
  return C::kScreen;
}

void CheckMatrix() {
  for (const Tier& t : kTiers) {
    for (int ki = 0; ki < int(Kind::kCount); ++ki) {
      const Kind k = Kind(ki);
      for (gb::Listed listed : kListed) {
        gb::Frame f;
        f.field_stands = t.field_stands;
        f.pixel_pass = t.pixel_pass;
        const lg::Light l = MakeLight(k);
        const gb::Carrier got = gb::CarrierOf(f, l, listed);
        const gb::Carrier want = Expected(t, k, listed);
        Check(got == want, std::string(t.name) + ", " + KindName(k) + ", " + ListedName(listed) + ": " + gb::CarrierName(got) + " (want " + gb::CarrierName(want) + ")");
      }
    }
  }
  // THE GAP, closed: at HIGH a MOVING car's lights (every group) are carried by the screen-space chain; the field has only the static ones.
  for (Kind k : {Kind::kHeadlight, Kind::kNeon, Kind::kRear, Kind::kPolice, Kind::kMod}) {
    gb::Frame high;
    high.field_stands = true;
    const lg::Light l = MakeLight(k);
    Check(gb::CarrierOf(high, l, gb::Listed::kMoving) == gb::Carrier::kScreen && gb::InPair(high, l, gb::Listed::kMoving, 1.0f, 1.0f),
          std::string("HIGH: a moving ") + KindName(k) + " bounces through the screen-space pair (it bounced through neither path before the join)");
    Check(gb::CarrierOf(high, l, gb::Listed::kStatic) == gb::Carrier::kField && !gb::InPair(high, l, gb::Listed::kStatic, 1.0f, 1.0f),
          std::string("HIGH: a parked ") + KindName(k) + " is the field's and NOT in the pair (no double count)");
    gb::Frame ultra = high;
    ultra.pixel_pass = true;
    Check(gb::CarrierOf(ultra, l, gb::Listed::kMoving) == gb::Carrier::kPixelPass && !gb::InPair(ultra, l, gb::Listed::kMoving, 1.0f, 1.0f),
          std::string("ULTRA: a moving ") + KindName(k) + " is the per-pixel pass's and not in the pair");
    gb::Frame off;
    Check(gb::InPair(off, l, gb::Listed::kNo, 1.0f, 1.0f) && gb::InPair(off, l, gb::Listed::kStatic, 1.0f, 1.0f) && gb::InPair(off, l, gb::Listed::kMoving, 1.0f, 1.0f),
          std::string("RT off: every ") + KindName(k) + ", whatever a list says, is the pair's (there is no field)");
  }
  // the world's lamps: the field's where it stands, the screen-space pair's where it does not, never by a group switch
  {
    gb::Frame off, high;
    high.field_stands = true;
    high.groups = 0;
    off.groups = 0;
    const lg::Light lamp = MakeLight(Kind::kStreetLamp);
    Check(gb::CarrierOf(high, lamp, gb::Listed::kNo) == gb::Carrier::kField && gb::CarrierOf(off, lamp, gb::Listed::kNo) == gb::Carrier::kScreen, "a street lamp follows the field, not a group switch");
  }
}

// ---------------------------------------------------------------------------
// exactly once: for every frame, every light, every list state, one path carries a light that bounces
// ---------------------------------------------------------------------------

// The three paths, each by its own definition (not through CarrierOf): the field carries the world's lamps and what its list says is static; the per-pixel pass what the list says is moving, in a frame it ran;
// the screen-space pair what the light pass draws between its captures.
bool FieldCarries(const gb::Frame& f, const lg::Light& l, gb::Listed listed) {
  if (!f.field_stands) return false;
  if (gb::IsWorldLamp(l)) return true;
  return (f.groups & re::GroupOf(l)) != 0 && listed == gb::Listed::kStatic;
}
bool PixelCarries(const gb::Frame& f, const lg::Light& l, gb::Listed listed) {
  return f.field_stands && f.pixel_pass && !gb::IsWorldLamp(l) && (f.groups & re::GroupOf(l)) != 0 && listed == gb::Listed::kMoving;
}

void CheckExactlyOnce() {
  std::mt19937 rng(22);
  int bouncing = 0, none = 0;
  bool once = true, no_gap = true, no_double = true, dark_out = true, switch_off = true;
  for (int i = 0; i < 40000; ++i) {
    gb::Frame f;
    f.groups = rng() & 15u;
    f.field_stands = (rng() & 1) != 0;
    f.pixel_pass = f.field_stands && (rng() & 1) != 0;
    const Kind k = Kind(rng() % uint32_t(Kind::kCount));
    lg::Light l = MakeLight(k, 1 + rng() % 5);
    const bool dark = (rng() % 5) == 0;
    if (dark) {
      l.color[0] = l.color[1] = l.color[2] = 0.0004f;
    }
    const float night = float(rng() & 3) * 0.5f;  // 0, 0.5, 1, 1.5 (clamped)
    const gb::Listed listed = kListed[rng() % 3];
    const bool in_pair = gb::InPair(f, l, listed, night, 1.0f);
    const bool field = FieldCarries(f, l, listed);
    const bool pixel = PixelCarries(f, l, listed);
    const int carriers = (in_pair ? 1 : 0) + (field ? 1 : 0) + (pixel ? 1 : 0);
    const bool group_on = gb::IsWorldLamp(l) || (f.groups & re::GroupOf(l)) != 0;
    const bool bounces = gb::Bounces(l, night, 1.0f);
    once = once && carriers <= 1;
    // A light that is lit (it bounces), whose group is on (a lamp has none): exactly one carrier, unless it is the field's own to carry (a lamp, a listed static one): then the pair holds it NOT.
    if (group_on && (bounces || field || pixel)) {
      // the field and the per-pixel pass carry by their own laws whatever the pass's light does (a dark light is gated by their own filter); a lit light must have a carrier
      if (bounces) {
        no_gap = no_gap && carriers == 1;
        ++bouncing;
      }
    }
    if (!group_on) {
      switch_off = switch_off && !in_pair && !field && !pixel;
      ++none;
    }
    if (!bounces && !field && !pixel) {
      dark_out = dark_out && !in_pair;
    }
    no_double = no_double && !(in_pair && (field || pixel));
  }
  Check(once, "40000 random frames and lights: no light is carried by two paths (no double count)");
  Check(no_gap, "... every light that bounces (lit, its group on) has a carrier: nothing bounces through none (the gap of HIGH is closed)");
  Check(no_double, "... a light in the screen-space pair is never also the field's or the per-pixel pass's");
  Check(switch_off, "... a light of a group whose switch is off is carried by nobody, at every tier");
  Check(dark_out, "... a light too dark to light anything is not in the pair (a day frame has no capture)");
  Check(bouncing > 5000 && none > 3000, "(the sample covers lit lights and switched-off groups: " + std::to_string(bouncing) + " / " + std::to_string(none) + ")");
}

// ---------------------------------------------------------------------------
// the switches: one story
// ---------------------------------------------------------------------------

void CheckSwitches() {
  Check(!gb::AnyGroup(0) && gb::AnyGroup(re::kGroupNeon) && gb::AnyGroup(re::kGroupAll) && !gb::AnyGroup(~re::kGroupAll), "all four group switches off is no lights' share (the one line both halves described)");
  // each group's switch takes exactly its lights out of the pair, at every tier
  for (const Tier& t : kTiers) {
    for (int gi = 0; gi < 4; ++gi) {
      const uint32_t bit = 1u << gi;
      for (int ki = 0; ki < int(Kind::kCount); ++ki) {
        const Kind k = Kind(ki);
        if (k == Kind::kStreetLamp) continue;
        gb::Frame f;
        f.field_stands = t.field_stands;
        f.pixel_pass = t.pixel_pass;
        f.groups = re::kGroupAll & ~bit;
        const lg::Light l = MakeLight(k);
        const bool off = GroupOfKind(k) == bit;
        for (gb::Listed listed : kListed) {
          const bool in_pair = gb::InPair(f, l, listed, 1.0f, 1.0f);
          const gb::Carrier c = gb::CarrierOf(f, l, listed);
          Check(off ? (!in_pair && c == gb::Carrier::kNone) : c == Expected(t, k, listed),
                std::string(t.name) + ", group bit " + std::to_string(gi) + " off, " + KindName(k) + ", " + ListedName(listed) + ": " + (off ? "out of every path" : "as with every switch on"));
        }
      }
    }
  }
  // the groups of the lights are the RT list's (one function): a rear lamp has a car, a bare 'other' light has none
  Check(re::GroupOf(MakeLight(Kind::kRear)) == re::kGroupRear && re::GroupOf(MakeLight(Kind::kPolice)) == re::kGroupOther && re::GroupOf(MakeLight(Kind::kMod)) == re::kGroupOther &&
            re::GroupOf(MakeLight(Kind::kHeadlight)) == re::kGroupHeadlights && re::GroupOf(MakeLight(Kind::kNeon)) == re::kGroupNeon,
        "the screen-space half takes a light's group from the RT list's own function");
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

// ---------------------------------------------------------------------------
// the AO's plan
// ---------------------------------------------------------------------------

void CheckPlan() {
  sl::OnlyHold hold;
  // RT off: the lights' share is the whole pass's, no residual pair, no lights-only chain, and the hold is empty
  gb::JoinPlan p = gb::PlanFor(true, false, true, &hold);
  Check(!p.residual && !p.only && hold.left() == 0, "RT off (or a field that does not stand): the whole pass's pair, the chain as it was");
  // HIGH with no pair yet: the pair is asked for (the light pass makes it where a light is in it), the chain is not built (a frame with no moving light costs nothing)
  p = gb::PlanFor(true, true, false, &hold);
  Check(p.residual && !p.only, "the field stands, no residual pair in the last frame: the residual pair is wanted, the plain occlusion runs");
  // a pair: lights-only
  p = gb::PlanFor(true, true, true, &hold);
  Check(p.residual && p.only, "the field stands and the last frame left a residual pair: the chain runs lights-only on it");
  // the hold
  uint32_t frames = 0;
  while (gb::PlanFor(true, true, false, &hold).only && frames < 1000) ++frames;
  Check(frames == sl::OnlyHold::kHoldFrames, "... and is kept for the hold after the last pair");
  // the lights' share off, or the field fading: no residual pair, no lights-only
  gb::PlanFor(true, true, true, &hold);
  p = gb::PlanFor(false, true, true, &hold);
  Check(!p.residual && !p.only, "the lights' share off (the setting, all four groups off, no extraction): nothing, and the hold is gone");
  gb::PlanFor(true, true, true, &hold);
  p = gb::PlanFor(true, false, true, &hold);
  Check(!p.residual && !p.only && hold.left() == 0, "the field fading (share over 0): the crossfade as it was, the whole pass's pair, the hold gone");
  // the two kinds of pair never meet
  sl::Key key;
  key.valid = true;
  key.frame = 9;
  key.size[0] = 960;
  key.size[1] = 540;
  key.partition = sl::Partition::kResidual;
  Check(sl::ResidualReady(key, 10) && !sl::Usable(key, 10, false, 960, 540, true, sl::Partition::kAll) && sl::Usable(key, 10, false, 960, 540, true, sl::Partition::kResidual),
        "a residual pair is read only by the lights-only chain");
}

// ---------------------------------------------------------------------------
// the text
// ---------------------------------------------------------------------------

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}
size_t At(const std::string& text, const std::string& what) {
  return text.find(what);
}
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

void CheckSources(const std::string& gpu) {
  const std::string ao = Slurp(gpu + "/ao.cpp");
  const std::string ao_h = Slurp(gpu + "/ao.h");
  const std::string ll_cpp = Slurp(gpu + "/local_lights.cpp");
  const std::string ll_h = Slurp(gpu + "/local_lights.h");
  const std::string scene = Slurp(gpu + "/scene.cpp");
  const std::string world = Slurp(gpu + "/rt_world_frame.cpp");
  const std::string world_h = Slurp(gpu + "/rt_world_frame.h");
  const std::string apply = Slurp(gpu + "/gi_apply.cpp");
  const std::string apply_h = Slurp(gpu + "/gi_apply.h");
  const std::string shared_h = Slurp(gpu + "/screen_shared.h");
  const std::string lights_cpp = Slurp(gpu + "/screen_lights.cpp");
  const std::string settings = Slurp(gpu + "/gi_bounce_settings.h");
  const std::string cmake = Slurp(gpu + "/CMakeLists.txt");
  const std::string probe_trace = Slurp(gpu + "/hlsl/gi_update_trace_cs.hlsl");
  const std::string pixel_trace = Slurp(gpu + "/hlsl/gi_pixel_trace_cs.hlsl");
  const std::string pixel_apply = Slurp(gpu + "/hlsl/gi_apply_ps.hlsl");
  Check(!ao.empty() && !ll_cpp.empty() && !scene.empty() && !world.empty() && !apply.empty() && !shared_h.empty() && !lights_cpp.empty() && !settings.empty() && !cmake.empty(), "the sources are readable from " + gpu);
  if (ao.empty() || ll_cpp.empty() || scene.empty() || world.empty() || apply.empty() || shared_h.empty() || lights_cpp.empty() || settings.empty() || cmake.empty()) return;

  // ao.cpp: the plan is the pure function's, the lights-only chain keeps the strength, the residual pair is told to the owner
  Check(Has(ao, "const gi_bounce::JoinPlan join = gi_bounce::PlanFor(g.lights, gi_apply::SsgiSkipped(share), screen_ != nullptr && screen_->LightsResidualReady(), &lights_only_hold_);"),
        "ao.cpp: PlanGi asks the pure plan, with the field's own 'skipped' word and the owner's residual-pair word");
  Check(Has(ao, "plan->lights_residual = join.residual;\n      if (!join.only) {\n        plan->gi = GiPlan{};\n        return;\n      }\n      g.lights_only = true;"),
        "ao.cpp: where the field stands the plain occlusion runs unless the plan says lights-only, which sets the flag and goes on (the strength is not scaled: the field's own share is 0)");
  Check(Has(ao, "} else {\n      g.strength *= share;\n    }"), "ao.cpp: with the field not standing the strength is scaled by the share as it always was (the crossfade)");
  Check(Has(ao, "plan->lights_residual = false;"), "ao.cpp: the residual word is cleared at the start of the plan");
  Check(Has(ao_h, "bool lights_only = false;") && Has(ao_h, "bool lights_residual = false;") && Has(ao_h, "ssgi_lights::OnlyHold lights_only_hold_;"), "ao.h: the plan's two words and the hold");
  // the owner
  Check(Has(shared_h, "bool LightsResidualReady() const { return ssgi_lights::ResidualReady(lights_key_, frame_count_); }") && Has(shared_h, "bool LightsFrameMade() const { return published_ && (pre_made_ || lights_partition_ == ssgi_lights::Partition::kResidual); }"),
        "screen_shared.h: the residual pair's readiness is the header's; a residual pair needs the published screen frame, not the colour chain");
  Check(Has(lights_cpp, "lights_key_.partition = lights_partition_;") && Count(lights_cpp, "LightsFrameMade()") == 3, "screen_lights.cpp: the pair is stamped with its partition and every capture asks the same frame rule");
  // the light pass
  Check(Has(ll_cpp, "in_pair[i] = rects[i].Empty() ? false : gi_bounce::InPair(pair->frame, l, listed, in.units.night, in.units.gain);") &&
            Has(ll_cpp, "pair->frame.field_stands && pair->listed && !gi_bounce::IsWorldLamp(l) ? pair->listed(l.id) : gi_bounce::Listed::kNo;"),
        "local_lights.cpp: a light is in the pair by the pure rule (units' night and gain; the list is asked only where the field stands, never for a street lamp)");
  Check(Has(ll_cpp, "outside = gi_bounce::PartitionOrder(in_pair, selection.count, &order);"), "local_lights.cpp: the selection (and its rectangles) is partitioned, the lights outside the pair first");
  Check(Has(ll_cpp, "draws_outside = ll::PlanDraws(selection, rects.data(), cost_off_, plan.data(), 0, outside);") &&
            Has(ll_cpp, "draws_pair = ll::PlanDraws(selection, rects.data(), cost_off_, plan.data() + draws_outside, outside, selection.count);"),
        "local_lights.cpp: the draws are planned in two ranges that never merge across the border");
  Check(Has(ll_cpp, "const bool bracket = pair != nullptr && draws_pair != 0 && static_cast<bool>(pair->capture_before);"), "local_lights.cpp: a pair with no light in the picture makes no capture");
  const size_t run_out = At(ll_cpp, "run(0, draws_outside);");
  const size_t close_at = At(ll_cpp, "list->setFramebuffer(nullptr);\n    pair->capture_before();");
  const size_t run_pair = At(ll_cpp, "run(draws_outside, draws_outside + draws_pair);");
  Check(run_out != std::string::npos && close_at != std::string::npos && run_pair != std::string::npos && run_out < close_at && close_at < run_pair,
        "local_lights.cpp: the lights outside the pair are drawn, the pass closed, the capture made, the pair's lights drawn (in this order)");
  Check(Has(ll_cpp, "if (bracket && draws_outside == 0) {\n    pair->capture_before();") && Has(ll_cpp, "edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);\n    begin_run();"),
        "local_lights.cpp: with nothing outside the capture is ahead of the first draw; after a capture the second run is set up again from the colour's write layout");
  Check(Has(ll_cpp, "const gi_bounce::Pair* const pair = pair_;  // GI-BOUNCE-JOIN: for this call only\n  pair_ = nullptr;"), "local_lights.cpp: the hand-over lasts one call");
  Check(Has(ll_h, "void SetLightsPair(const gi_bounce::Pair* pair) { pair_ = pair; }"), "local_lights.h: the setter");
  // the scene
  Check(Has(scene, "lights_pair.frame.field_stands = screen_->lights_partition() == ssgi_lights::Partition::kResidual;") && Has(scene, "lights_pair.frame.pixel_pass = gi_apply_ && gi_apply_->pixel_pass_ran();") &&
            Has(scene, "lights_pair.frame.groups = gi_bounce::GroupsNow();"),
        "scene.cpp: the rule's frame is the owner's partition, the apply's word on the per-pixel pass and the four switches");
  Check(Has(scene, "rt_world_->EmitterMotionOf(id)") && Has(scene, "local_lights_->SetLightsPair(want_pair ? &lights_pair : nullptr);"), "scene.cpp: the field's list is asked by id and the pair is handed to the light pass");
  const size_t hand = At(scene, "local_lights_->SetLightsPair(want_pair ? &lights_pair : nullptr);");
  const size_t call = At(scene, "const bool local_lights_ran = local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_);");
  const size_t note_before = At(scene, "order_.Note(frame_order::Step::kLightsBefore, lights_before);");
  const size_t note_lights = At(scene, "order_.Note(frame_order::Step::kLocalLights, local_lights_ran);");
  Check(hand != std::string::npos && hand < call && call < note_before && note_before < note_lights, "scene.cpp: the pair is handed before the pass, and the 'before' capture is noted ahead of the pass in the table's order");
  // the world frame and the apply
  Check(Has(world, "RtWorldFrame::EmitterMotion RtWorldFrame::EmitterMotionOf(uint64_t light_id) const {") && Has(world, "!emitters_->wanted || !Valid() || light_id == larecomp::gpu::kNoLightId") &&
            Has(world, "return e.moving ? EmitterMotion::kMoving : EmitterMotion::kStatic;") && Has(world_h, "enum class EmitterMotion : uint8_t { kNotListed = 0, kStatic = 1, kMoving = 2 };"),
        "rt_world_frame: the list's word about a light is its own motion by id, and 'not listed' in a frame the world frame was not filled in");
  Check(Has(apply, "pixel_ran_frame_ = pixel_ran;") && Has(apply, "pixel_ran_frame_ = false;") && Has(apply_h, "bool pixel_pass_ran() const { return pixel_ran_frame_; }"), "gi_apply: the per-pixel pass's published image is the word, cleared each frame");
  // the switches: the two halves read the same four settings
  for (const char* name : {"mcla_gpu_gi_bounce_headlights", "mcla_gpu_gi_bounce_neon", "mcla_gpu_gi_bounce_rear", "mcla_gpu_gi_bounce_other"}) {
    Check(Has(settings, std::string(name) + ".Get()") && Has(world, std::string(name) + ".Get()"), std::string(name) + ": read by the screen-space half and by the ray traced half");
  }
  Check(Has(cmake, "gi_bounce_carrier_test.cpp"), "CMakeLists.txt: this test is listed");
  Check(Has(probe_trace, "#define CITY_HIT_EMITTER_COUNT (g_world.emitterStatic)"),
        "the probe trace only shades with the static prefix");
  Check(Has(pixel_trace, "#define CITY_HIT_EMITTER_COUNT (g_world.emitterCount)") &&
            Has(pixel_apply, "s.irradiance = GipMix(s.irradiance, float3(pixelTerm.x, pixelTerm.y, pixelTerm.z), pixelTerm.w);"),
        "the pixel trace shades with the full list and its irradiance blends with the field");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) gpu_dir = argv[i];
  CheckMatrix();
  CheckExactlyOnce();
  CheckSwitches();
  CheckBounces();
  CheckPartition();
  CheckPlan();
  if (!gpu_dir.empty()) {
    CheckSources(gpu_dir);
  } else {
    std::printf("(no src/gpu folder given: the source text checks were skipped)\n");
  }
  std::printf("gi_bounce_carrier_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
