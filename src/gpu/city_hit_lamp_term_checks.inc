// CITY-RT R7: the lamp term at hits and the emissive parts, checked. Not a header: included by city_hit_shade_rules_test.cpp inside its anonymous namespace (so City, Lcg, Check, Near, Num, Close,
// RandomCase, BindFrame, MakeReads ... are the test's own), after city_hit_lamp_term_lib.inc compiled the library a second time as hlsl_shim::grid with the world frame's lamp grid
// (CITY_HIT_LAMP_GRID), the world rows' glow schedule (CITY_HIT_GLOW_FACTOR) and a counter for the census (CITY_HIT_CENSUS_ADD).
//
// What is checked, in order:
//   twin           LampsCapped / LampPickCapped (the cap counts only the lamps that reach the hit, the walk bound, the sum's cap in luminance with the colours' ratios kept, the one lamp's estimate against the capped sum),
//                  the glow's default factor
//   calibration    the glowing class' own range and sample count; the other classes' unchanged
//   the grid       the shader's CityHitShade over the world frame's buffers against the twin's Shade with rw::LampLightAt, on thousands of random hits: the colour, the glow, the rays cast, the census
//   the caps       a dense place (more lamps than the cap), far lamps first in the walk (the near ones still count), the walk bound, a bright cluster (the sum over its cap), day (nothing), a lookup that finds nothing
//   the glow kinds a lens glows by the street lamps' schedule, neon and lit windows by the fill lights', a signal always
//   the reflections' glue (REFL-LAMPS) city_refl_rules.h's LampTermFor / LampFlags / GlowFactorFor in front of the twin's Shade: switch off or world rows not valid = the shading of old (no lamp light, the sun's
//                  schedule for the glow), exactly; on and valid = the grid's lamp light and the world rows' glow by kind

namespace ch = larecomp::citypack;

inline uint32_t GlowKindOfMaterial(const HitMaterial& m) { return m.glow_kind < 3 ? m.glow_kind : 0u; }

template <class P>
P GridParams(const Case& k) {
  P p;
  p.mode = k.p.mode;
  p.flags = k.p.flags;
  p.units = k.p.units;
  p.sunReach = k.p.sun_reach;
  p.sunTan = k.p.sun_tan;
  p.lampFactor = k.p.lamp_factor;
  p.wet = k.p.wet;
  p.aoMix = k.p.ao_mix;
  p.lodBias = k.p.lod_bias;
  p.bounce = k.p.bounce;
  p.lampFirst = 0;
  p.lampCount = 0;
  p.noise = hlsl_shim::float3(k.p.noise[0], k.p.noise[1], k.p.noise[2]);
  return p;
}
template <class R>
R GridRay(const Case& k) {
  R r;
  r.origin = F3(k.ray.origin);
  r.direction = F3(k.ray.direction);
  r.t = k.ray.t;
  r.footprint = k.ray.footprint;
  return r;
}
template <class I>
I GridInstance(const Case& k) {
  I i;
  i.instanceId = kInstanceIdFlag | (k.sector << 1);
  i.geometryIndex = 0;
  i.primitiveIndex = k.primitive;
  i.barycentrics = hlsl_shim::float2(k.b1, k.b2);
  i.row0 = F3(k.inst.row[0]);
  i.row1 = F3(k.inst.row[1]);
  i.row2 = F3(k.inst.row[2]);
  return i;
}

// A world: lamps, the frame's rows and the two buffers' contents, the way the host would write them.
struct GridWorld {
  ch::LampTable table;
  rw::LampFrame lamps;
  rw::WorldInput in;
  uint32_t rows[rw::kTotalRows][4] = {};
  rw::WorldDecoded decoded;
  std::vector<hlsl_shim::uint4> world_storage, lamp_storage;
  void Bind() {
    world_storage.resize(rw::kTotalRows);
    for (uint32_t r = 0; r < rw::kTotalRows; ++r) world_storage[r] = hlsl_shim::uint4(rows[r][0], rows[r][1], rows[r][2], rows[r][3]);
    lamp_storage.resize(lamps.words.size() / 4);
    for (size_t i = 0; i < lamp_storage.size(); ++i) lamp_storage[i] = hlsl_shim::uint4(lamps.words[4 * i], lamps.words[4 * i + 1], lamps.words[4 * i + 2], lamps.words[4 * i + 3]);
    hlsl_shim::gRtWorld.data = world_storage.data();
    hlsl_shim::gRtWorld.count = world_storage.size();
    hlsl_shim::gRtLamps.data = lamp_storage.data();
    hlsl_shim::gRtLamps.count = lamp_storage.size();
  }
};

// `lamps` are in the game's world; the camera and the world's axes are the frame's. `hour` decides the schedule (a night one lights the lamps); `street` and `fill` (non negative) replace the
// schedule's street and fill parts (a test that wants a lens and a neon sign at different strengths).
void MakeGridWorld(GridWorld* w, const rh::FrameInput& frame, std::vector<ch::Lamp> lamps, float hour, float street, float fill, bool stats_frame) {
  w->table = ch::LampTable::Build(std::move(lamps));
  rw::LampInput li;
  li.table = &w->table;
  for (int c = 0; c < 3; ++c) {
    li.camera[c] = frame.camera_world[c];
    for (int k = 0; k < 3; ++k) li.axes[k][c] = frame.world_axes_view[k][c];
  }
  li.hour = hour;
  li.schedule = rw::ScheduleAt(ch::LampHours{}, hour, 0.0f);
  if (street >= 0.0f) li.schedule.street = street;
  if (fill >= 0.0f) li.schedule.fill = fill;
  li.schedule.factor = std::max(li.schedule.street, li.schedule.fill);
  li.budget = {2048, 700.0f};
  rw::BuildLampFrame(li, &w->lamps);
  w->in = rw::WorldInput{};
  w->in.hit = frame;
  w->in.hour = hour;
  w->in.camera_known = true;
  for (int c = 0; c < 3; ++c) w->in.camera[c] = frame.camera_world[c];
  w->in.schedule = li.schedule;
  w->in.lamps = &w->lamps;
  w->in.units = 16.0f;
  w->in.stats_frame = stats_frame;
  rw::MakeWorldRows(w->in, w->rows);
  w->decoded = rw::DecodeWorldRows(w->rows);
  w->Bind();
}

// Lamps around a world point: `fill` omni lights and `street` spot lamps within `spread` metres on the ground, now and then one farther out.
std::vector<ch::Lamp> LampsAround(Lcg& rng, const float centre[3], uint32_t fill, uint32_t street, float spread, float intensity_lo, float intensity_hi) {
  std::vector<ch::Lamp> out;
  for (uint32_t i = 0; i < fill + street; ++i) {
    ch::Lamp l;
    const bool is_street = i >= fill;
    const float reach = i % 9 == 8 ? 3.0f : 1.0f;
    l.pos[0] = centre[0] + rng.Range(-spread, spread) * reach;
    l.pos[1] = is_street ? rng.Range(6.0f, 9.0f) : rng.Range(0.5f, 12.0f);
    l.pos[2] = centre[2] + rng.Range(-spread, spread) * reach;
    l.colour[0] = rng.Range(0.4f, 1.0f);
    l.colour[1] = rng.Range(0.4f, 1.0f);
    l.colour[2] = rng.Range(0.2f, 1.0f);
    l.intensity = rng.Range(intensity_lo, intensity_hi);
    if (is_street) {
      l.spot = 1;
      l.cone_deg = 125.0f;
      l.spread_deg = -50.5f;
      l.dir[0] = 0.0f;
      l.dir[1] = -1.0f;
      l.dir[2] = 0.0f;
      l.source = ch::LampSource::kStreetLamp;
    } else {
      l.spot = rng.Below(5) == 0 ? 1 : 0;
      l.cone_deg = l.spot ? 100.0f : 360.0f;
      l.dir[1] = -1.0f;
      l.source = ch::LampSource::kFile;
      l.file_index = i;
    }
    out.push_back(l);
  }
  return out;
}

struct GridCoverage {
  int cases = 0, lit_cases = 0, cap_count = 0, cap_sum = 0, lamp_rays = 0, glow_kind[3] = {}, calibrate = 0, shadow = 0, empty = 0;
};

// The decode of a case against material words of the test's own (the kinds patched in).
Decoded DecodeCaseWith(const City& city, const std::vector<uint32_t>& words, const Case& k) {
  Decoded d;
  HitView v = city.view;
  v.materials = words.data();
  d.hit = DecodeHit(v, kInstanceIdFlag | (k.sector << 1), 0, k.primitive, k.b1, k.b2);
  if (d.hit.valid) {
    d.material = HitMaterial::Unpack(&words[size_t(d.hit.material) * 8]);
    d.texture = d.material.texture != kNoTexture && d.material.texture < city.texture_rows.size() ? &city.texture_rows[d.material.texture] : nullptr;
  }
  return d;
}

// The twin's reads for a grid case: the fake world of MakeReads plus the grid's lamp light, the glow schedule and the census.
struct TwinCensus {
  std::vector<uint32_t> words = std::vector<uint32_t>(8, 0);
  bool calibrate = false;
};
cs::Reads MakeGridReads(const Case& k, const GridWorld& w, TwinCensus* census, GridCoverage* cov) {
  cs::Reads r = MakeReads(k);
  const rh::FrameInput frame = k.frame;
  const rw::WorldDecoded decoded = w.decoded;
  const std::vector<uint32_t>* lamp_words = &w.lamps.words;
  const float factor = k.p.lamp_factor;
  r.lamps_at = [frame, decoded, lamp_words, factor, census, cov](cs::V3 position, cs::V3 normal, bool shadow, float noise) {
    const cs::LampLight t = rw::LampLightAt(decoded, lamp_words->data(), frame.world_axes_view, factor, position, normal, shadow, noise, [](cs::V3 o, cs::V3 d, float reach) {
      gTwinLamp.push_back({{o.x, o.y, o.z}, {d.x, d.y, d.z}, reach});
      return hlsl_shim::FakeBlocked(o.x, o.y, o.z, d.x, d.y, d.z);
    });
    if (census->calibrate) {
      census->words[cs::kCensusLookups] += 1;
      census->words[cs::kCensusExamined] += t.examined;
      census->words[cs::kCensusLit] += t.lit;
      census->words[cs::kCensusCapCount] += t.cap_count ? 1u : 0u;
      census->words[cs::kCensusCapSum] += t.cap_sum ? 1u : 0u;
    }
    cov->cap_count += t.cap_count ? 1 : 0;
    cov->cap_sum += t.cap_sum ? 1 : 0;
    cov->lit_cases += t.lit != 0 ? 1 : 0;
    return t.light;
  };
  r.glow_factor = [decoded](uint32_t kind) { return decoded.GlowFactor(kind); };
  r.census = [census](uint32_t word, uint32_t value) { census->words[word] += value; };
  return r;
}

void GridShadeChecks(const City& city, const std::vector<uint32_t>& words, Lcg& rng, int rounds, GridCoverage* cov) {
  int wrong_valid = 0, wrong_colour = 0, wrong_glow = 0, wrong_rays = 0, wrong_census = 0;
  double worst = 0.0;
  std::vector<hlsl_shim::uint4> frame_rows;
  for (int round = 0; round < rounds; ++round) {
    Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    k.frame.camera_known = true;
    for (int c = 0; c < 3; ++c) k.frame.camera_world[c] = rng.Range(-300.0f, 300.0f);
    k.p.flags &= ~cs::kPScreen;
    if (rng.Below(4) != 0) k.p.flags |= cs::kPLamps;
    if (rng.Below(4) != 0) k.p.flags |= cs::kPEmissive;
    if (rng.Below(3) == 0) k.p.flags |= cs::kPCalibrate;
    // the world: lamps near the hit (in the game's world), at a random hour; some rounds with more lamps than the cap, some with lamps bright enough for the sum's cap
    const cs::V3 hit = k.ray.origin + k.ray.direction * k.ray.t;
    const float view[3] = {hit.x, hit.y, hit.z};
    float centre[3];
    rh::WorldPoint(k.frame.camera_world, k.frame.world_axes_view, view, centre);
    const uint32_t shape = rng.Below(8);
    const uint32_t fill = shape == 0 ? 0 : (shape == 1 ? 130 + rng.Below(60) : (shape == 5 ? 40 + rng.Below(20) : rng.Below(24)));
    const uint32_t street = shape == 2 ? 0 : rng.Below(14);
    const float spread = shape == 1 ? 12.0f : (shape == 5 ? 5.0f : rng.Range(15.0f, 70.0f));
    const float hour = shape == 3 ? 13.0f : (rng.Below(3) == 0 ? rng.Range(17.0f, 19.0f) : rng.Range(21.0f, 23.5f));
    GridWorld w;
    MakeGridWorld(&w, k.frame, LampsAround(rng, centre, fill, street, spread, 15.0f, shape == 4 || shape == 5 ? 400.0f : 80.0f), hour, -1.0f, -1.0f, (k.p.flags & cs::kPCalibrate) != 0);
    k.p.lamp_factor = w.decoded.lamp_factor;
    BindFrame(k.frame, &frame_rows);
    hlsl_shim::gSunCalls.clear();
    hlsl_shim::gLampCalls.clear();
    gTwinSun.clear();
    gTwinLamp.clear();
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    TwinCensus twin_census;
    twin_census.calibrate = (k.p.flags & cs::kPCalibrate) != 0;
    ++cov->cases;
    cov->calibrate += twin_census.calibrate ? 1 : 0;
    cov->shadow += (k.p.flags & cs::kPLampShadow) != 0 ? 1 : 0;
    cov->empty += w.lamps.lamp_count == 0 ? 1 : 0;

    const hlsl_shim::grid::RtHitFrame f = hlsl_shim::grid::RtHitLoadFrame();
    const hlsl_shim::grid::CityHitShaded s = hlsl_shim::grid::CityHitShade(GridParams<hlsl_shim::grid::CityHitParams>(k), f, GridRay<hlsl_shim::grid::CityHitRay>(k), GridInstance<hlsl_shim::grid::CityHitInstance>(k));
    const Decoded d = DecodeCaseWith(city, words, k);
    const cs::Shaded t = cs::Shade(k.p, k.frame, k.ray, k.inst, d.hit, d.material, d.texture, MakeGridReads(k, w, &twin_census, cov), nullptr);
    if ((s.valid != 0) != t.valid) {
      ++wrong_valid;
      continue;
    }
    if (!CallsAgree(hlsl_shim::gSunCalls, gTwinSun) || !CallsAgree(hlsl_shim::gLampCalls, gTwinLamp)) ++wrong_rays;
    cov->lamp_rays += int(hlsl_shim::gLampCalls.size());
    if (!t.valid) continue;
    if (!Close(s.colour, t.colour, 5e-4)) ++wrong_colour;
    worst = std::max(worst, double(std::fabs(s.colour.x - t.colour.x) / std::max(1.0f, t.colour.x)));
    if (!Near(s.glow, t.glow, 1e-5)) ++wrong_glow;
    if (t.glow > 0.0f) ++cov->glow_kind[GlowKindOfMaterial(d.material)];
    for (uint32_t i = 0; i < 8; ++i) {
      if (hlsl_shim::gCensus[i] != twin_census.words[i]) {
        ++wrong_census;
        break;
      }
    }
  }
  Check(wrong_valid == 0, Num("the grid library has data for the hits the twin decodes, and none for the rest (%.0f differ)", wrong_valid));
  Check(wrong_colour == 0, Num2("the shaded colour with the world frame's lamps is the twin's (%.0f wrong, the worst difference %.1e)", wrong_colour, worst));
  Check(wrong_glow == 0, Num("the glow by kind and schedule is the twin's (%.0f wrong)", wrong_glow));
  Check(wrong_rays == 0, Num("the sun's and the lamps' rays are cast from the same points, in the same directions, as far (%.0f cases differ)", wrong_rays));
  Check(wrong_census == 0, Num("the census' counters (lookups, lamps examined, lit, the caps, the glowing hits by kind) are the twin's (%.0f cases differ)", wrong_census));
}

float float3Luma(const hlsl_shim::float3& v) { return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z; }

// The cases that matter for the caps: built by hand, read through the shader's CityHitLamps.
void GridCapChecks(Lcg& rng) {
  // a frame with identity axes and the camera at the origin: the view space is the world
  rh::FrameInput frame;
  frame.camera_known = true;
  for (int k = 0; k < 3; ++k)
    for (int c = 0; c < 3; ++c) frame.world_axes_view[k][c] = k == c ? 1.0f : 0.0f;
  std::vector<hlsl_shim::uint4> frame_rows;
  BindFrame(frame, &frame_rows);
  const auto params = [&](float lamp_factor, bool shadow) {
    hlsl_shim::grid::CityHitParams p;
    p.mode = CITY_HIT_MODE_REFLECT;
    p.flags = CITY_HIT_P_LAMPS | CITY_HIT_P_CALIBRATE | (shadow ? CITY_HIT_P_LAMP_SHADOW : 0u);
    p.units = 1.0f;
    p.sunReach = 0.0f;
    p.sunTan = 0.0f;
    p.lampFactor = lamp_factor;
    p.wet = 0.0f;
    p.aoMix = 0.5f;
    p.lodBias = 0.0f;
    p.bounce = 0.0f;
    p.lampFirst = 0;
    p.lampCount = 0;
    p.noise = hlsl_shim::float3(0.5f, 0.5f, 0.5f);
    return p;
  };
  const hlsl_shim::float3 position(3.0f, 0.0f, 2.0f), normal(0.0f, 1.0f, 0.0f);
  const float centre[3] = {3.0f, 0.0f, 2.0f};

  // 1. DENSE: 200 lamps within a few metres: the lookup holds more than the cap, the hit examines the first kLampMax of them in the cursor's order and says so
  {
    GridWorld w;
    std::vector<ch::Lamp> lamps = LampsAround(rng, centre, 200, 0, 6.0f, 5.0f, 8.0f);
    for (ch::Lamp& l : lamps) {  // omni lights above the point, all in reach
      l.spot = 0;
      l.cone_deg = 360.0f;
      l.pos[1] = rng.Range(2.0f, 5.0f);
      l.pos[0] = centre[0] + rng.Range(-5.0f, 5.0f);
      l.pos[2] = centre[2] + rng.Range(-5.0f, 5.0f);
    }
    MakeGridWorld(&w, frame, lamps, 22.0f, -1.0f, -1.0f, true);
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 light = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, false), position, normal, 0.1f);
    const cs::LampLight twin = rw::LampLightAt(w.decoded, w.lamps.words.data(), frame.world_axes_view, w.decoded.lamp_factor, cs::V3{3.0f, 0.0f, 2.0f}, cs::V3{0.0f, 1.0f, 0.0f}, false, 0.5f, nullptr);
    Check(w.lamps.lamp_count >= 150, Num("the dense world holds more lamps than the cap (%.0f)", w.lamps.lamp_count));
    Check(hlsl_shim::gCensus[CITY_HIT_CENSUS_LOOKUPS] == 1 && hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED] == cs::kLampMax && hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT] == 1,
          Num2("a hit examines at most kLampMax lamps and counts that it stopped (examined %.0f, cap count %.0f)", hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED], hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT]));
    Check(twin.examined == cs::kLampMax && twin.cap_count && Close(light, twin.light, 5e-4), "the dense hit's light is the twin's over the first kLampMax lamps of the lookup");
    Check(cs::Luma(twin.light) <= cs::kLampSumCap * 1.0001f, "the dense hit's light is under the sum's cap");
  }

  // 1b. FAR FIRST (CITY-RT-REVIEW S4): 150 lamps that cannot reach the point are first in the cursor's order (a row of cells with lower z), eight that do reach come after: the cap counts only lamps
  // that reach, so all eight are in and the light is their plain sum (the count before the reach test took the first 96, all of them far, and lit the hit by none)
  {
    GridWorld w;
    std::vector<ch::Lamp> lamps;
    for (uint32_t i = 0; i < 150; ++i) {  // out of reach: intensity 1 reaches 10 m, these are 40 m away
      ch::Lamp l;
      l.pos[0] = centre[0] + rng.Range(-5.0f, 5.0f);
      l.pos[1] = rng.Range(2.0f, 5.0f);
      l.pos[2] = centre[2] - 40.0f + rng.Range(-3.0f, 3.0f);
      l.intensity = 1.0f;
      lamps.push_back(l);
    }
    for (uint32_t i = 0; i < 8; ++i) {  // in reach, above the point
      ch::Lamp l;
      l.pos[0] = centre[0] + rng.Range(-4.0f, 4.0f);
      l.pos[1] = rng.Range(2.0f, 5.0f);
      l.pos[2] = centre[2] + 20.0f + rng.Range(-3.0f, 3.0f);
      l.colour[0] = rng.Range(0.4f, 1.0f);
      l.colour[1] = rng.Range(0.4f, 1.0f);
      l.colour[2] = rng.Range(0.2f, 1.0f);
      l.intensity = 900.0f;  // reaches 300 m: the lookup's window is the limit
      lamps.push_back(l);
    }
    MakeGridWorld(&w, frame, lamps, 22.0f, -1.0f, -1.0f, true);
    // the walk's order, from the buffers: how many lamps that cannot reach the point come before the first that can
    std::vector<uint32_t> order;
    const float view[3] = {3.0f, 0.0f, 2.0f};
    rw::LampsNear(w.decoded, w.lamps.words.data(), frame.world_axes_view, view, w.decoded.lookup_m > 0.0f ? w.decoded.lookup_m : cs::kLampLookup, &order);
    std::vector<cs::Lamp> in_order;
    for (const uint32_t i : order) {
      uint32_t rows[3][4];
      rw::LampRows(w.lamps.words.data(), i, rows);
      in_order.push_back(cs::UnpackGpuLamp(rows));
    }
    size_t first_reaching = in_order.size();
    uint32_t reaching = 0;
    for (size_t i = 0; i < in_order.size(); ++i) {
      if (cs::LampAt(w.decoded.lamp_factor, in_order[i], cs::V3{3.0f, 0.0f, 2.0f}, cs::V3{0.0f, 1.0f, 0.0f}).reaches) {
        first_reaching = std::min(first_reaching, i);
        ++reaching;
      }
    }
    Check(order.size() >= 158 && first_reaching >= cs::kLampMax && reaching == 8,
          Num2("the setup: the far lamps come first in the cursor's order (%.0f lamps pass before the first that reaches; %.0f reach)", double(first_reaching), double(reaching)));
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 light = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, false), position, normal, 0.1f);
    const cs::V3 plain = cs::LampsSum(w.decoded.lamp_factor, in_order.data(), uint32_t(in_order.size()), cs::V3{3.0f, 0.0f, 2.0f}, cs::V3{0.0f, 1.0f, 0.0f});
    const cs::V3 want = plain * cs::LampKeep(cs::Luma(plain));
    Check(hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED] == 8 && hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT] == 0 && hlsl_shim::gCensus[CITY_HIT_CENSUS_LIT] == 8,
          Num2("far lamps first: the hit examines the eight that reach and none is dropped (examined %.0f, cap count %.0f)", hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED], hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT]));
    Check(cs::Luma(want) > 0.0f && Close(light, want, 5e-4), "far lamps first: the hit's light is the plain sum of every lamp that reaches it (capped in luminance), as if the far ones were not there");
    const cs::LampLight twin = rw::LampLightAt(w.decoded, w.lamps.words.data(), frame.world_axes_view, w.decoded.lamp_factor, cs::V3{3.0f, 0.0f, 2.0f}, cs::V3{0.0f, 1.0f, 0.0f}, false, 0.5f, nullptr);
    Check(twin.examined == 8 && !twin.cap_count && Close(light, twin.light, 5e-4), "far lamps first: the twin agrees");
    // with the shadow ray: the replay walks the same lamps (one lamp is chosen, a ray cast, the estimate is the plain sum or nothing where blocked)
    hlsl_shim::gLampCalls.clear();
    const hlsl_shim::float3 est = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, true), position, normal, 0.1f);
    const bool blocked = hlsl_shim::gLampCalls.size() == 1 && hlsl_shim::FakeBlocked(hlsl_shim::gLampCalls[0].origin[0], hlsl_shim::gLampCalls[0].origin[1], hlsl_shim::gLampCalls[0].origin[2],
                                                                                    hlsl_shim::gLampCalls[0].direction[0], hlsl_shim::gLampCalls[0].direction[1], hlsl_shim::gLampCalls[0].direction[2]);
    Check(hlsl_shim::gLampCalls.size() == 1 && (blocked ? float3Luma(est) == 0.0f : float3Luma(est) > 0.0f), "far lamps first: with the shadow flag one of the lamps that reach is chosen and gets the ray");
  }

  // 1c. THE WALK'S BOUND: 400 lamps that cannot reach the point first, then eight that can: the walk passes kLampWalkMax lamps and stops, so the hit has no lamp light and says it hit the cap
  {
    GridWorld w;
    std::vector<ch::Lamp> lamps;
    for (uint32_t i = 0; i < 400; ++i) {
      ch::Lamp l;
      l.pos[0] = centre[0] + rng.Range(-5.0f, 5.0f);
      l.pos[1] = rng.Range(2.0f, 5.0f);
      l.pos[2] = centre[2] - 40.0f + rng.Range(-3.0f, 3.0f);
      l.intensity = 1.0f;
      lamps.push_back(l);
    }
    for (uint32_t i = 0; i < 8; ++i) {
      ch::Lamp l;
      l.pos[0] = centre[0] + rng.Range(-4.0f, 4.0f);
      l.pos[1] = rng.Range(2.0f, 5.0f);
      l.pos[2] = centre[2] + 20.0f + rng.Range(-3.0f, 3.0f);
      l.intensity = 900.0f;
      lamps.push_back(l);
    }
    MakeGridWorld(&w, frame, lamps, 22.0f, -1.0f, -1.0f, true);
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 light = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, false), position, normal, 0.1f);
    const cs::LampLight twin = rw::LampLightAt(w.decoded, w.lamps.words.data(), frame.world_axes_view, w.decoded.lamp_factor, cs::V3{3.0f, 0.0f, 2.0f}, cs::V3{0.0f, 1.0f, 0.0f}, false, 0.5f, nullptr);
    Check(w.lamps.lamp_count >= 400 && hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED] == 0 && hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT] == 1 && float3Luma(light) == 0.0f,
          Num2("the walk stops after kLampWalkMax lamps (examined %.0f, cap count %.0f)", hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED], hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_COUNT]));
    Check(twin.walked == cs::kLampWalkMax && twin.examined == 0 && twin.cap_count && Close(light, twin.light, 5e-4), "the walk's bound: the twin passes kLampWalkMax lamps and agrees");
  }

  // 2. BRIGHT: twenty lamps of intensity 400 within 3 m of a point whose normal faces them: their sum is far over the cap; the light is the sum scaled to the cap, colours' ratios kept
  {
    GridWorld w;
    std::vector<ch::Lamp> lamps;
    for (uint32_t i = 0; i < 20; ++i) {
      ch::Lamp l;
      l.pos[0] = centre[0] + rng.Range(-1.5f, 1.5f);
      l.pos[1] = 3.0f + rng.Range(0.0f, 1.0f);
      l.pos[2] = centre[2] + rng.Range(-1.5f, 1.5f);
      l.colour[0] = 1.0f;
      l.colour[1] = 0.6f;
      l.colour[2] = 0.3f;
      l.intensity = 400.0f;
      lamps.push_back(l);
    }
    MakeGridWorld(&w, frame, lamps, 22.0f, -1.0f, -1.0f, true);
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 light = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, false), position, normal, 0.1f);
    Check(Near(float3Luma(light), cs::kLampSumCap, 1e-4) && hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_SUM] == 1,
          Num2("twenty bright lamps light a hit to the sum's cap and the census says so (luminance %.4f, cap count %.0f)", float3Luma(light), hlsl_shim::gCensus[CITY_HIT_CENSUS_CAP_SUM]));
    Check(Near(light.y / light.x, 0.6, 1e-4) && Near(light.z / light.x, 0.3, 1e-4), "the capped light keeps the lamps' colour");
    // with the shadow ray's estimate: the lamp is chosen by its brightness, the estimate is the capped sum, a lamp that is blocked gives none
    hlsl_shim::gLampCalls.clear();
    const hlsl_shim::float3 est = hlsl_shim::grid::CityHitLamps(params(w.decoded.lamp_factor, true), position, normal, 0.1f);
    const bool blocked = hlsl_shim::gLampCalls.size() == 1 && hlsl_shim::FakeBlocked(hlsl_shim::gLampCalls[0].origin[0], hlsl_shim::gLampCalls[0].origin[1], hlsl_shim::gLampCalls[0].origin[2],
                                                                                    hlsl_shim::gLampCalls[0].direction[0], hlsl_shim::gLampCalls[0].direction[1], hlsl_shim::gLampCalls[0].direction[2]);
    Check(hlsl_shim::gLampCalls.size() == 1 && (blocked ? float3Luma(est) == 0.0f : Near(float3Luma(est), cs::kLampSumCap, 1e-3)),
          "one shadow ray for the chosen lamp, and its estimate is the capped sum (or nothing where the ray is blocked)");
  }

  // 3. DAY and EMPTY: no schedule, no lamp in the window; a hit far from every lamp has a lookup and nothing examined
  {
    GridWorld w;
    MakeGridWorld(&w, frame, LampsAround(rng, centre, 10, 4, 20.0f, 20.0f, 60.0f), 12.0f, -1.0f, -1.0f, true);  // noon: the schedule is 0, the window holds no lamp
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 light = hlsl_shim::grid::CityHitLamps(params(0.0f, false), position, normal, 0.1f);
    Check(w.lamps.lamp_count == 0 && float3Luma(light) == 0.0f && hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED] == 0, "by day the window holds no lamp and a hit has no lamp light");
    GridWorld far;
    MakeGridWorld(&far, frame, LampsAround(rng, centre, 10, 4, 20.0f, 20.0f, 60.0f), 22.0f, -1.0f, -1.0f, true);
    std::fill(hlsl_shim::gCensus.begin(), hlsl_shim::gCensus.end(), 0u);
    const hlsl_shim::float3 away = hlsl_shim::grid::CityHitLamps(params(far.decoded.lamp_factor, false), hlsl_shim::float3(5000.0f, 0.0f, 5000.0f), normal, 0.1f);
    Check(float3Luma(away) == 0.0f && hlsl_shim::gCensus[CITY_HIT_CENSUS_LOOKUPS] == 1 && hlsl_shim::gCensus[CITY_HIT_CENSUS_EXAMINED] == 0, "a hit far from every lamp has a lookup and no lamp light");
  }
}

// What the glow does: a lens, a neon sign and a signal at street lamps and fill lights of different strengths.
void GlowKindChecks() {
  struct Row {
    uint32_t sector;
    uint32_t kind;
    const char* name;
  };
  // the synthetic city: sector 3 is the lamp lens (material 4), 8 the neon sign (material 8), 9 the lit window (material 9, here patched to be a signal)
  const Row rows[3] = {{3, kGlowLens, "the lamp lens"}, {8, kGlowNight, "the neon sign"}, {9, kGlowAlways, "the signal"}};
  rh::FrameInput frame;
  frame.light.valid = true;
  frame.light.sun[0] = frame.light.sun[1] = frame.light.sun[2] = 0.0f;
  frame.light.ambient[0] = frame.light.ambient[1] = frame.light.ambient[2] = 0.02f;
  frame.sun_known = false;
  frame.camera_known = true;
  for (int k = 0; k < 3; ++k)
    for (int c = 0; c < 3; ++c) frame.world_axes_view[k][c] = k == c ? 1.0f : 0.0f;
  std::vector<hlsl_shim::uint4> frame_rows;
  BindFrame(frame, &frame_rows);
  int wrong = 0, checked = 0;
  for (const float street : {0.0f, 0.3f, 1.0f}) {
    for (const float fill : {0.0f, 0.8f, 1.0f}) {
      GridWorld w;
      MakeGridWorld(&w, frame, {}, 22.0f, street, fill, false);
      for (const Row& r : rows) {
        Case k;
        k.frame = frame;
        k.p.mode = cs::kModeReflect;
        k.p.flags = cs::kPEmissive;
        k.p.units = 1.0f;
        k.p.lamp_factor = w.decoded.lamp_factor;
        k.p.ao_mix = 0.0f;
        k.ray.direction = {0.0f, 0.0f, 1.0f};
        k.ray.origin = {0.0f, 0.0f, 0.0f};
        k.ray.t = 8.0f;
        k.ray.footprint = 0.05f;
        k.sector = r.sector;
        k.primitive = 0;
        k.b1 = 0.3f;
        k.b2 = 0.3f;
        const hlsl_shim::grid::RtHitFrame f = hlsl_shim::grid::RtHitLoadFrame();
        const hlsl_shim::grid::CityHitShaded s = hlsl_shim::grid::CityHitShade(GridParams<hlsl_shim::grid::CityHitParams>(k), f, GridRay<hlsl_shim::grid::CityHitRay>(k), GridInstance<hlsl_shim::grid::CityHitInstance>(k));
        if (!s.valid) continue;
        const float want = r.kind == kGlowAlways ? 1.0f : (r.kind == kGlowLens ? w.decoded.street : w.decoded.fill);
        ++checked;
        if (!Near(s.glow, want, 1e-6)) {
          ++wrong;
          std::printf("glow of %s at street %.1f fill %.1f: %.3f, wanted %.3f\n", r.name, street, fill, s.glow, want);
        }
      }
    }
  }
  Check(checked == 27 && wrong == 0, Num2("a lens glows by the street lamps' schedule, neon by the fill lights', a signal always (%.0f wrong of %.0f)", wrong, checked));
}

// The glowing class has its own calibration range and sample count; the others keep the city's.
void EmissiveCalibrationChecks() {
  // a lamp lens' model is 32 times too dark (five stops): the class' gain follows it up to x32 within a few windows (the city's range would stop at x3), a wall in the same state stops at x3
  hc::ClassGains gains;
  CHECK(Near(gains.Gain(hc::kEmissiveClass), 1.0) && hc::kEmissiveClass == uint32_t(gb::SurfaceClass::kEmissive) && hc::kEmissiveClass == CITY_HIT_CLASS_EMISSIVE);
  CHECK(Near(hc::GainOf(hc::kEmissiveParams, 9.0f), 64.0, 1e-3) && Near(hc::GainOf(hc::kEmissiveParams, -9.0f), 0.25, 1e-4) && hc::kEmissiveParams.min_samples == 12 && hc::kCityParams.min_samples == 48);
  const uint32_t wall = 1, lens = hc::kEmissiveClass;
  for (int window = 0; window < 40; ++window) {
    uint32_t words[hc::kStatsWords] = {};
    for (const uint32_t c : {wall, lens}) {
      const float residual = 5.0f - std::log2(gains.Gain(c));  // the picture is 5 stops over the model before any gain
      const uint32_t samples = c == lens ? 14 : 300;           // a lens is small: 14 samples are enough for it, a wall needs 48
      for (uint32_t s = 0; s < samples; ++s) {
        const uint32_t model_log = hc::FixedLog2(0.5f);
        words[hc::StatsWord(c, hc::kCountWord)] += 1;
        words[hc::StatsWord(c, hc::kSeenWord)] += uint32_t(std::lround(float(model_log) + residual * hc::kLogScale));
        words[hc::StatsWord(c, hc::kModelWord)] += model_log;
      }
    }
    gains.Add(words);
    gains.Close();
  }
  Check(Near(gains.Gain(lens), 32.0, 0.02), Num("the glowing class follows a model that is five stops too dark up to x32 (%.2f)", gains.Gain(lens)));
  Check(Near(gains.Gain(wall), 3.0, 1e-3), Num("a wall in the same state is held at the city's x3 (%.2f)", gains.Gain(wall)));
  // too few samples: 11 a window never move the glowing class, 12 do
  for (const uint32_t samples : {11u, 12u}) {
    hc::ClassGains g;
    uint32_t words[hc::kStatsWords] = {};
    words[hc::StatsWord(lens, hc::kCountWord)] = samples;
    words[hc::StatsWord(lens, hc::kSeenWord)] = samples * (hc::FixedLog2(0.5f) + 64);
    words[hc::StatsWord(lens, hc::kModelWord)] = samples * hc::FixedLog2(0.5f);
    g.Add(words);
    const uint32_t moved = g.Close();
    CHECK((samples == 11u) == (moved == 0 && Near(g.Gain(lens), 1.0)));
  }
  // the table the shader reads carries the wide gain
  float table[hc::kClasses];
  gains.Table(table);
  CHECK(Near(table[lens], gains.Gain(lens)) && table[lens] > 3.0f && table[wall] <= 3.0f + 1e-4f);
}


// REFL-LAMPS: what the car reflection passes' CityReflHit does in front of the library (city_refl_rules.h LampTermFor, LampFlags, GlowFactorFor), run through the twin's Shade on random hits and random worlds.
// With the switch off, or with world rows that are not valid, the shading is the shading of old: the host's flags without the lamp flag, the sun's schedule as the lamp factor, the library's own default glow:
// EXACTLY the same colour and glow, no lamp lookup at all. With the switch on and valid rows it is the grid's lamp light and the world rows' glow by kind.
void ReflectionGlueChecks(const City& city, const std::vector<uint32_t>& words, Lcg& rng, int rounds) {
  namespace cr = mcla::gpu::city_refl;
  int off_cases = 0, on_cases = 0, wrong_off = 0, wrong_on = 0, wrong_glow = 0, lamp_lookups_off = 0, on_lit = 0, on_differs = 0, glow_cases = 0;
  std::vector<hlsl_shim::uint4> frame_rows;
  for (int round = 0; round < rounds; ++round) {
    Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    k.frame.camera_known = true;
    for (int c = 0; c < 3; ++c) k.frame.camera_world[c] = rng.Range(-300.0f, 300.0f);
    const bool lamps_flag = rng.Below(2) == 0;
    const bool valid = rng.Below(4) != 0;
    const float sun_schedule = rng.Below(3) == 0 ? 0.0f : rng.Range(0.0f, 1.0f);
    const bool stats = rng.Below(3) == 0;
    const uint32_t host_flags = cr::ParamFlags(false, stats, lamps_flag);
    // the world: lamps near the hit at a night hour (the schedule lights them)
    const cs::V3 hit = k.ray.origin + k.ray.direction * k.ray.t;
    const float view[3] = {hit.x, hit.y, hit.z};
    float centre[3];
    rh::WorldPoint(k.frame.camera_world, k.frame.world_axes_view, view, centre);
    GridWorld w;
    MakeGridWorld(&w, k.frame, LampsAround(rng, centre, rng.Below(30), rng.Below(12), rng.Range(10.0f, 60.0f), 15.0f, 90.0f), rng.Range(21.0f, 23.5f), rng.Below(2) == 0 ? -1.0f : rng.Range(0.0f, 1.0f),
                  rng.Below(2) == 0 ? -1.0f : rng.Range(0.0f, 1.0f), stats);
    const uint32_t world_flags = valid ? w.decoded.flags : 0u;
    const cr::LampTerm term = cr::LampTermFor(lamps_flag, world_flags, w.decoded.lamp_factor, sun_schedule);
    // the decision itself
    CHECK(term.world == (lamps_flag && valid));
    CHECK(term.lamp_factor == (term.world ? w.decoded.lamp_factor : sun_schedule));
    BindFrame(k.frame, &frame_rows);
    const Decoded d = DecodeCaseWith(city, words, k);
    // what the pass gives the library
    Case now = k;
    now.p.flags = cr::LampFlags(host_flags, term) & ~cs::kPScreen;
    now.p.lamp_factor = term.lamp_factor;
    now.p.lamp_first = 0;
    now.p.lamp_count = 0;
    const auto glow = [&](uint32_t kind) { return cr::GlowFactorFor(term, kind, w.decoded.street, w.decoded.fill); };
    TwinCensus census;
    census.calibrate = (now.p.flags & cs::kPCalibrate) != 0;
    GridCoverage cov;
    cs::Reads reads_now = term.world ? MakeGridReads(now, w, &census, &cov) : MakeReads(now);
    reads_now.glow_factor = glow;
    int lookups = 0;
    if (term.world) {
      const auto inner = reads_now.lamps_at;
      reads_now.lamps_at = [&lookups, inner](cs::V3 position, cs::V3 normal, bool shadow, float noise) {
        ++lookups;
        return inner(position, normal, shadow, noise);
      };
    }
    const cs::Shaded got = cs::Shade(now.p, now.frame, now.ray, now.inst, d.hit, d.material, d.texture, reads_now, nullptr);
    if (!got.valid) continue;
    if (!term.world) {
      // the shading of old: the host's flags as they were (no lamp flag), the sun's schedule as the lamp factor, no glow schedule hook
      Case old = k;
      old.p.flags = (cr::ParamFlags(false, stats, false)) & ~cs::kPScreen;
      old.p.lamp_factor = sun_schedule;
      old.p.lamp_first = 0;
      old.p.lamp_count = 0;
      const cs::Shaded want = cs::Shade(old.p, old.frame, old.ray, old.inst, d.hit, d.material, d.texture, MakeReads(old), nullptr);
      ++off_cases;
      lamp_lookups_off += lookups;
      if (!(want.valid && got.colour.x == want.colour.x && got.colour.y == want.colour.y && got.colour.z == want.colour.z && got.glow == want.glow)) ++wrong_off;
      continue;
    }
    // on: the grid's light and the world rows' glow: the same call with the world's own glow hook (decoded.GlowFactor) and the lamp flag as the host wrote it
    Case ref = k;
    ref.p.flags = host_flags & ~cs::kPScreen;
    ref.p.lamp_factor = w.decoded.lamp_factor;
    ref.p.lamp_first = 0;
    ref.p.lamp_count = 0;
    TwinCensus census_ref;
    census_ref.calibrate = (ref.p.flags & cs::kPCalibrate) != 0;
    GridCoverage cov_ref;
    const cs::Shaded want = cs::Shade(ref.p, ref.frame, ref.ray, ref.inst, d.hit, d.material, d.texture, MakeGridReads(ref, w, &census_ref, &cov_ref), nullptr);
    ++on_cases;
    if (!(want.valid && got.colour.x == want.colour.x && got.colour.y == want.colour.y && got.colour.z == want.colour.z && got.glow == want.glow)) ++wrong_on;
    on_lit += cov.lit_cases;
    // the glow factor of every kind is the world rows' (a signal always, a lens the street lamps', the rest the fill lights')
    for (uint32_t kind = 0; kind < 3; ++kind) {
      ++glow_cases;
      if (glow(kind) != std::clamp(w.decoded.GlowFactor(kind), 0.0f, 1.0f)) ++wrong_glow;
    }
    // a hit the lamps light differs from the same hit without the lamp light (the term does something)
    if (cov.lit_cases != 0) {
      Case dark = ref;
      dark.p.flags &= ~cs::kPLamps;
      const cs::Shaded none = cs::Shade(dark.p, dark.frame, dark.ray, dark.inst, d.hit, d.material, d.texture, MakeGridReads(dark, w, &census_ref, &cov_ref), nullptr);
      on_differs += (none.colour.x != got.colour.x || none.colour.y != got.colour.y || none.colour.z != got.colour.z) ? 1 : 0;
    }
  }
  std::printf("reflection lamp glue: %d cases with the old shading (switch off or no world rows), %d with the world frame's lamps (%d lit by a lamp, %d changed by it)\n", off_cases, on_cases, on_lit, on_differs);
  Check(off_cases > 400 && on_cases > 400 && on_lit > 40, Num2("the glue's random hits covered both sides (old %.0f, with the lamps %.0f)", off_cases, on_cases));
  Check(wrong_off == 0, Num("with the lamp switch off or the world rows not valid the shading is the shading of old, exactly (%.0f differ)", wrong_off));
  Check(lamp_lookups_off == 0, Num("... and no lamp lookup is made (%.0f)", lamp_lookups_off));
  Check(wrong_on == 0, Num("with the lamp switch on and valid rows the shading is the grid's lamp light and the world rows' glow (%.0f differ)", wrong_on));
  Check(wrong_glow == 0, Num("the glow factor of each kind is the world rows' (%.0f wrong)", wrong_glow));
  Check(on_differs > 0 && on_differs * 2 > on_lit, Num2("a lamp-lit hit is lit differently without the lamp term (%.0f of %.0f)", on_differs, on_lit));
}

// The lamp term's library text and the consumers: what the host and the other passes rely on.
void LampTermTextChecks(const std::string& src) {
  const std::string lib = ReadFile(src + "/gpu/hlsl/city_hit_shade.hlsli");
  const std::string lamp_cs = ReadFile(src + "/gpu/hlsl/rt_world_lamp_light_cs.hlsl");
  const std::string check_cs = ReadFile(src + "/gpu/hlsl/city_hit_shade_check_cs.hlsl");
  if (lib.empty() || lamp_cs.empty()) {
    std::printf("the sources were not found under %s: the lamp term's text checks are skipped\n", src.c_str());
    return;
  }
  const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
  CHECK(has(lib, "#if defined(CITY_HIT_LAMP_GRID)") && has(lib, "#elif defined(CITY_HIT_LAMP_LOAD)") && has(lib, "RtLampBegin(w, position, radius)") && has(lib, "kCityHitLampMax") && has(lib, "kCityHitLampSumCap"));
  // the cap counts the lamps that reach (S4 of CITY-RT-REVIEW): the walk is bounded apart from it, and the pick's replay follows the same walk
  CHECK(has(lib, "kCityHitLampWalkMax") && has(lib, "examined += e.reaches;") && has(lib, "walked < kCityHitLampWalkMax") && has(lib, "replayed < walked") && has(lib, "seen += e.reaches;"));
  CHECK(!has(lib, "examined += 1u;"));
  CHECK(has(lib, "CITY_HIT_GLOW_FACTOR(kind)") && has(lib, "CityHitGlowFactor(p, kind)") && has(lib, "CITY_HIT_CENSUS_ADD(word, value)") && has(lib, "CityHitCensus(p, CITY_HIT_CENSUS_GLOW_NIGHT + kind, 1u)"));
  // the library's numbers are the twin's (the grid namespace's copy: the same file)
  CHECK(hlsl_shim::grid::kCityHitLampMax == cs::kLampMax && hlsl_shim::grid::kCityHitLampWalkMax == cs::kLampWalkMax && cs::kLampWalkMax == 3 * cs::kLampMax && Near(hlsl_shim::grid::kCityHitLampSumCap, cs::kLampSumCap) && Near(hlsl_shim::grid::kCityHitLampLookup, cs::kLampLookup));
  CHECK(CITY_HIT_CENSUS_LOOKUPS == cs::kCensusLookups && CITY_HIT_CENSUS_EXAMINED == cs::kCensusExamined && CITY_HIT_CENSUS_LIT == cs::kCensusLit && CITY_HIT_CENSUS_CAP_COUNT == cs::kCensusCapCount &&
        CITY_HIT_CENSUS_CAP_SUM == cs::kCensusCapSum && CITY_HIT_CENSUS_GLOW_NIGHT == cs::kCensusGlowNight && CITY_HIT_CENSUS_WORDS == cs::kCensusWords);
  CHECK(CITY_HIT_GLOW_NIGHT == cs::kGlowNight && CITY_HIT_GLOW_LENS == cs::kGlowLens && CITY_HIT_GLOW_ALWAYS == cs::kGlowAlways);
  // the reference consumer takes the library's grid, the schedule's glow and the census
  CHECK(has(lamp_cs, "#define CITY_HIT_LAMP_GRID") && has(lamp_cs, "#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)") && has(lamp_cs, "CityHitLamps(") && has(lamp_cs, "CITY_HIT_CENSUS_ADD"));
  CHECK(has(check_cs, "#define CITY_HIT_LAMP_LOAD(i)"));  // the check pass keeps the flat range
}

void LampTermChecks(const City& city, Lcg& rng, const std::string& src) {
  // the twin
  {
    std::vector<cs::Lamp> lamps(14);
    const cs::V3 position{1.0f, 0.0f, 2.0f}, n = cs::Normalize({0.2f, 1.0f, 0.1f});
    for (cs::Lamp& l : lamps) {
      l.position = {rng.Range(-4, 4), rng.Range(2, 6), rng.Range(-4, 4)};
      l.intensity = rng.Range(60, 90);
      l.colour = {rng.Range(0.8f, 1.0f), rng.Range(0.8f, 1.0f), rng.Range(0.8f, 1.0f)};
    }
    // four lamps are under the cap (the sum is the plain sum), fourteen over it (the sum's luminance is the cap and the colours' ratios stay)
    const cs::LampLight under = cs::LampsCapped(0.9f, lamps.data(), 4, position, n);
    CHECK(under.examined == 4 && !under.cap_count && !under.cap_sum && under.lit == 4);
    const cs::V3 under_sum = cs::LampsSum(0.9f, lamps.data(), 4, position, n);
    CHECK(Near(under.light.x, under_sum.x, 1e-6) && Near(under.light.y, under_sum.y, 1e-6) && Near(under.light.z, under_sum.z, 1e-6));
    const cs::LampLight few = cs::LampsCapped(0.9f, lamps.data(), 14, position, n);
    const cs::V3 plain = cs::LampsSum(0.9f, lamps.data(), 14, position, n);
    CHECK(few.examined == 14 && !few.cap_count && few.lit == 14);
    CHECK(cs::Luma(plain) > cs::kLampSumCap && few.cap_sum && Near(cs::Luma(few.light), cs::kLampSumCap, 1e-4) && Near(few.light.y / few.light.x, plain.y / plain.x, 1e-4));
    // the cap counts the lamps that REACH the hit (CITY-RT-REVIEW S4): lamps that do not take no slot
    std::vector<cs::Lamp> many(200, lamps[0]);
    for (cs::Lamp& l : many) l.intensity = 1.0f;  // the reach is d^2 < 100: nothing at 40 m reaches: all walked, none examined, none lit
    const cs::LampLight dense = cs::LampsCapped(1.0f, many.data(), 200, {40.0f, 0.0f, 0.0f}, n);
    CHECK(dense.examined == 0 && dense.walked == 200 && !dense.cap_count && dense.lit == 0 && cs::Luma(dense.light) == 0.0f);
    std::vector<cs::Lamp> endless(400, many[0]);
    const cs::LampLight bound = cs::LampsCapped(1.0f, endless.data(), 400, {40.0f, 0.0f, 0.0f}, n);  // the walk's own bound: kLampWalkMax lamps passed, lamps left
    CHECK(bound.examined == 0 && bound.walked == cs::kLampWalkMax && bound.cap_count);
    std::vector<cs::Lamp> far_lamps(400, lamps[0]);  // 80 m from the point at intensity 1 (the reach is 10 m): out of reach
    for (cs::Lamp& l : far_lamps) {
      l.position = {80.0f, 3.0f, 2.0f};
      l.intensity = 1.0f;
    }
    std::vector<cs::Lamp> reach(200, lamps[0]);  // above the point, in reach
    for (size_t i = 0; i < reach.size(); ++i) {
      reach[i].position = {1.0f + rng.Range(-3, 3), rng.Range(2, 4), 2.0f + rng.Range(-3, 3)};
      reach[i].intensity = 400.0f;
      reach[i].cos_outer = -2.0f;
    }
    const cs::LampLight full = cs::LampsCapped(1.0f, reach.data(), 200, position, n);
    CHECK(full.examined == cs::kLampMax && full.walked == cs::kLampMax && full.cap_count && full.lit > 0);
    CHECK(cs::LampsCapped(1.0f, reach.data(), cs::kLampMax, position, n).cap_count == false);  // exactly the cap is not over it
    CHECK(cs::LampsCapped(1.0f, reach.data(), cs::kLampMax + 1, position, n).cap_count == true);
    // 150 lamps out of reach first, then ten in reach: the old count (before the reach test) took the first 96 of them and dropped all ten; now all ten are in, and the light is their plain sum
    {
      std::vector<cs::Lamp> list(far_lamps.begin(), far_lamps.begin() + 150);
      for (size_t i = 0; i < 10; ++i) list.push_back(reach[i]);
      const cs::LampLight t = cs::LampsCapped(1.0f, list.data(), uint32_t(list.size()), position, n);
      const cs::V3 plain = cs::LampsSum(1.0f, list.data(), uint32_t(list.size()), position, n);
      const cs::V3 kept = plain * cs::LampKeep(cs::Luma(plain));
      CHECK(t.examined == 10 && t.walked == 160 && !t.cap_count && t.lit == 10);
      CHECK(Near(t.light.x, kept.x, 1e-5) && Near(t.light.y, kept.y, 1e-5) && Near(t.light.z, kept.z, 1e-5) && cs::Luma(t.light) > 0.0f);
      // a mix: 3 in reach, 100 out, 100 in reach: 93 of the last are taken, the walk passed 196
      std::vector<cs::Lamp> mix(reach.begin(), reach.begin() + 3);
      mix.insert(mix.end(), far_lamps.begin(), far_lamps.begin() + 100);
      mix.insert(mix.end(), reach.begin() + 3, reach.begin() + 103);
      const cs::LampLight m = cs::LampsCapped(1.0f, mix.data(), uint32_t(mix.size()), position, n);
      CHECK(m.examined == cs::kLampMax && m.walked == 3 + 100 + 93 && m.cap_count);
      // the one lamp's estimate (the replay of the same walk, out-of-reach lamps between the others) averages to the capped sum
      std::vector<cs::Lamp> woven;
      for (size_t i = 0; i < 14; ++i) {
        woven.push_back(lamps[i]);
        woven.insert(woven.end(), far_lamps.begin(), far_lamps.begin() + 2);
      }
      const cs::LampLight woven_sum = cs::LampsCapped(0.9f, woven.data(), uint32_t(woven.size()), position, n);
      cs::V3 woven_mean;
      for (int i = 0; i < 6000; ++i) woven_mean = woven_mean + cs::LampPickCapped(0.9f, woven.data(), uint32_t(woven.size()), position, n, (float(i) + 0.5f) / 6000.0f, 0.0f, [](cs::V3, cs::V3, float) { return false; }).light;
      woven_mean = woven_mean * (1.0f / 6000.0f);
      CHECK(woven_sum.examined == 14 && woven_sum.walked == 42 && Near(woven_mean.x, woven_sum.light.x, 4e-3) && Near(woven_mean.y, woven_sum.light.y, 4e-3) && Near(woven_mean.z, woven_sum.light.z, 4e-3));
    }
    // the one lamp's estimate averages to the capped sum
    cs::V3 mean;
    const int samples = 8000;
    for (int i = 0; i < samples; ++i) mean = mean + cs::LampPickCapped(0.9f, lamps.data(), 14, position, n, (float(i) + 0.5f) / float(samples), 0.0f, [](cs::V3, cs::V3, float) { return false; }).light;
    mean = mean * (1.0f / float(samples));
    Check(Near(mean.x, few.light.x, 4e-3) && Near(mean.y, few.light.y, 4e-3) && Near(mean.z, few.light.z, 4e-3), Num2("the one lamp's estimate averages to the capped sum (%.4f against %.4f)", mean.x, few.light.x));
    CHECK(cs::GlowFactorDefault(cs::kGlowAlways, 0.0f) == 1.0f && cs::GlowFactorDefault(cs::kGlowLens, 0.4f) == 0.4f && cs::GlowFactorDefault(cs::kGlowNight, 2.0f) == 1.0f && cs::GlowFactorDefault(3, 0.25f) == 0.25f);
    CHECK(cs::GlowKindOf(3) == cs::kGlowNight && cs::GlowKindOf(1) == cs::kGlowLens && cs::GlowKindOf(2) == cs::kGlowAlways);
  }
  EmissiveCalibrationChecks();

  // the kinds on the synthetic city's materials: 4 the lamp lens, 8 the neon sign, 9 the lit window (here a signal): patched into a copy of the material table (bits 30..31 of the tint word)
  std::vector<uint32_t> words = city.material_words;
  std::vector<hlsl_shim::uint4> rows = city.material_rows;
  const auto set_kind = [&](size_t material, uint32_t kind) {
    words[material * 8 + 2] = (words[material * 8 + 2] & 0x3FFFFFFFu) | (kind << kGlowShift);
    rows[material * 2].z = words[material * 8 + 2];
  };
  set_kind(4, kGlowLens);
  set_kind(8, kGlowNight);
  set_kind(9, kGlowAlways);
  hlsl_shim::gCityMaterials.data = rows.data();
  hlsl_shim::gCityMaterials.count = rows.size();
  hlsl_shim::gCityMaterials.errors = 0;
  GridCoverage cov;
  GridShadeChecks(city, words, rng, 4000, &cov);
  std::printf("lamp term at hits: %d random hits through the grid: %d lit by a lamp, %d at the lamps' cap, %d at the sum's cap, %d lamp rays, %d empty windows, glow by kind night %d lens %d always %d\n", cov.cases,
              cov.lit_cases, cov.cap_count, cov.cap_sum, cov.lamp_rays, cov.empty, cov.glow_kind[0], cov.glow_kind[1], cov.glow_kind[2]);
  Check(cov.cases > 3500 && cov.lit_cases > 300 && cov.cap_count > 40 && cov.cap_sum > 20 && cov.lamp_rays > 100 && cov.calibrate > 500 && cov.shadow > 800 && cov.empty > 100,
        Num2("the grid's random hits covered lit hits, both caps, lamp rays and the census (lit %.0f, count caps %.0f)", cov.lit_cases, cov.cap_count));
  Check(cov.glow_kind[0] > 100 && cov.glow_kind[1] > 20 && cov.glow_kind[2] > 20, Num2("every glow kind was hit (night %.0f, lens %.0f)", cov.glow_kind[0], cov.glow_kind[1]));
  GridCapChecks(rng);
  GlowKindChecks();
  ReflectionGlueChecks(city, words, rng, 3000);
  Check(hlsl_shim::gCityMaterials.errors == 0 && hlsl_shim::gRtWorld.errors == 0 && hlsl_shim::gRtLamps.errors == 0, "no misaligned or out of range load on the world rows or the lamps");
  // the table back as the first part of the test had it
  hlsl_shim::gCityMaterials.data = city.material_rows.data();
  hlsl_shim::gCityMaterials.count = city.material_rows.size();
  LampTermTextChecks(src);
}
