// GI-BOUNCE-RT: the emitters' light at hits, checked. Not a header: included by city_hit_shade_rules_test.cpp inside its anonymous namespace (so City, Lcg, Check, CHECK, Near, Num, Num2, Close, RandomCase,
// BindFrame, MakeReads, CallsAgree, GridParams / GridRay / GridInstance, DecodeCaseWith ... are the test's own), after city_hit_emitter_lib.inc compiled the library a third time as hlsl_shim::emit with the
// emitter hooks over a list of the test's own (hlsl_shim::gEmitters) and CityHitEmitterRay.
//
// What is checked, in order:
//   twin            Emitters / EmittersPick (the sum, its cap with the colours' ratios kept, the count's bound, the one emitter's estimate against the sum over a stratified set of noise, the ray: origin,
//                   direction, length; a hit no emitter reaches casts nothing: the budget is counted AFTER the reach), EmitterNoise (uniform, not the lamps' own number)
//   the library     the shader's CityHitShade with emitters against the twin's Shade, on thousands of random hits, emitter lists (none, a few, more than the bound, some with no colour, some out of range) and
//                   flags: the colour, the rays cast (their origins, directions and lengths), which hooks were called; with the flag clear the hooks are not called and the colour is the library's of old
//   the text        the includers' defines (the probe field reads the static emitters only, the per-pixel pass all), the city mask of the emitters' ray and the lamps' and sun's mask unchanged

hlsl_shim::float3 E3(const cs::V3& v) { return hlsl_shim::float3(v.x, v.y, v.z); }

struct EmitterCoverage {
  int cases = 0, with_emitters = 0, lit = 0, shadow = 0, rays = 0, capped = 0, over_bound = 0, flag_clear = 0, wrong_calls = 0;
};

// A twin lambda over the same list: the law of hlsl_shim::EmitterRadiance in the twin's types.
cs::V3 TwinEmitterRadiance(const std::vector<hlsl_shim::EmitterRec>& list, uint32_t i, cs::V3 p, cs::V3 n) {
  const hlsl_shim::EmitterRec& e = list[i];
  const cs::V3 d = cs::V3{e.pos[0], e.pos[1], e.pos[2]} - p;
  const float d2 = cs::Dot(d, d);
  if (!(d2 < e.range * e.range)) {
    return cs::V3{};
  }
  const float inv = 1.0f / std::sqrt(d2);
  const float ndl = cs::Saturate(cs::Dot(n, d * inv));
  const float term = d2 <= e.plateau ? 1.0f / e.plateau : 1.0f / d2;
  return cs::V3{e.colour[0], e.colour[1], e.colour[2]} * (term * ndl);
}

std::vector<hlsl_shim::RayCall> gTwinEmitter;

// Emitters around a view space point: `n` of them within `spread` metres, bright ones now and then (the sum's cap), a few out of range, a few with no colour. `above` puts them 1.5 to 3 m over the point (a light
// that faces a surface whose normal is up: N.L is not 0).
std::vector<hlsl_shim::EmitterRec> EmittersAround(Lcg& rng, const cs::V3& centre, uint32_t n, float spread, float strength, bool above = false) {
  std::vector<hlsl_shim::EmitterRec> out;
  for (uint32_t i = 0; i < n; ++i) {
    hlsl_shim::EmitterRec e;
    e.pos[0] = centre.x + rng.Range(-spread, spread);
    e.pos[1] = centre.y + (above ? rng.Range(1.5f, 3.0f) : rng.Range(-0.5f, 3.0f));
    e.pos[2] = centre.z + rng.Range(-spread, spread);
    const bool dark = i % 11 == 10;
    for (int c = 0; c < 3; ++c) e.colour[c] = dark ? 0.0f : rng.Range(0.2f, 1.0f) * strength * (i % 7 == 6 ? 12.0f : 1.0f);
    e.range = i % 9 == 8 ? 0.5f : rng.Range(6.0f, 40.0f);
    e.plateau = rng.Range(0.5f, 6.0f);
    out.push_back(e);
  }
  return out;
}

void EmitterTwinChecks(Lcg& rng) {
  const cs::V3 position{1.0f, 0.5f, 6.0f}, n = cs::Normalize({0.1f, 1.0f, -0.2f});
  const auto list_of = [&](uint32_t count, float strength, float spread) { return EmittersAround(rng, position, count, spread, strength, true); };
  const auto radiance_of = [&](const std::vector<hlsl_shim::EmitterRec>& list) {
    return [&list, position, n](uint32_t i) { return TwinEmitterRadiance(list, i, position, n); };
  };
  const auto to_of = [&](const std::vector<hlsl_shim::EmitterRec>& list) {
    return [&list, position](uint32_t i) { return cs::V3{list[i].pos[0], list[i].pos[1], list[i].pos[2]} - position; };
  };
  CHECK(cs::kEmitterMax == 32 && Near(cs::kEmitterSumCap, 8.0) && Near(cs::kEmitterEnd, 0.3) && cs::kEmitterMask == 0x80 && cs::kPEmitters == 128 && cs::kPEmitterShadow == 256);
  CHECK(hlsl_shim::emit::kCityHitEmitterMax == cs::kEmitterMax && Near(hlsl_shim::emit::kCityHitEmitterSumCap, cs::kEmitterSumCap) && Near(hlsl_shim::emit::kCityHitEmitterEnd, cs::kEmitterEnd) &&
        hlsl_shim::emit::kCityHitCityMask == cs::kEmitterMask && CITY_HIT_P_EMITTERS == cs::kPEmitters && CITY_HIT_P_EMITTER_SHADOW == cs::kPEmitterShadow);
  // the sum, under the cap: the plain sum of the emitters in reach; their number that reach is counted
  {
    std::vector<hlsl_shim::EmitterRec> list = list_of(6, 0.05f, 6.0f);
    for (auto& e : list) e.range = 60.0f;
    cs::V3 plain;
    uint32_t reaching = 0;
    for (uint32_t i = 0; i < 6; ++i) {
      const cs::V3 r = TwinEmitterRadiance(list, i, position, n);
      plain = plain + r;
      reaching += cs::Luma(r) > 0.0f ? 1u : 0u;
    }
    const cs::EmitterLight t = cs::Emitters(6, radiance_of(list));
    CHECK(cs::Luma(plain) > 0.0f && cs::Luma(plain) < cs::kEmitterSumCap && !t.cap_sum && t.reaching == reaching);
    CHECK(Near(t.light.x, plain.x, 1e-6) && Near(t.light.y, plain.y, 1e-6) && Near(t.light.z, plain.z, 1e-6));
  }
  // over the cap: the luminance is the cap and the colours' ratios stay
  {
    std::vector<hlsl_shim::EmitterRec> list = list_of(10, 20.0f, 2.0f);
    for (auto& e : list) {
      e.range = 60.0f;
      e.plateau = 4.0f;
    }
    cs::V3 plain;
    for (uint32_t i = 0; i < 10; ++i) plain = plain + TwinEmitterRadiance(list, i, position, n);
    const cs::EmitterLight t = cs::Emitters(10, radiance_of(list));
    CHECK(cs::Luma(plain) > cs::kEmitterSumCap && t.cap_sum && Near(cs::Luma(t.light), cs::kEmitterSumCap, 1e-4) && Near(t.light.y / t.light.x, plain.y / plain.x, 1e-4) && Near(t.light.z / t.light.x, plain.z / plain.x, 1e-4));
  }
  // the count's bound: an emitter past kEmitterMax is never looked at, however bright
  {
    std::vector<hlsl_shim::EmitterRec> list = list_of(40, 0.05f, 4.0f);
    for (size_t i = 0; i < list.size(); ++i) {
      list[i].range = i == 35 ? 90.0f : 0.01f;  // only the 36th reaches
      list[i].colour[0] = list[i].colour[1] = list[i].colour[2] = 1.0f;
      list[i].plateau = 4.0f;
    }
    const cs::EmitterLight t = cs::Emitters(40, radiance_of(list));
    CHECK(t.reaching == 0 && cs::Luma(t.light) == 0.0f);
    list[3].range = 90.0f;  // the fourth does
    const cs::EmitterLight u = cs::Emitters(40, radiance_of(list));
    CHECK(u.reaching == 1 && cs::Luma(u.light) > 0.0f);
  }
  // THE BUDGET IS COUNTED AFTER THE REACH: nothing in reach, no ray; one in reach, one ray toward it, from the lifted point, stopping short of it
  {
    std::vector<hlsl_shim::EmitterRec> list = list_of(12, 0.5f, 5.0f);
    for (auto& e : list) e.range = 0.01f;
    int calls = 0;
    const cs::EmitterLight none = cs::EmittersPick(12, radiance_of(list), to_of(list), position, n, 0.3f, position.z, [&](cs::V3, cs::V3, float) {
      ++calls;
      return false;
    });
    CHECK(calls == 0 && !none.cast && none.reaching == 0 && cs::Luma(none.light) == 0.0f);
    list[5].range = 60.0f;
    list[5].pos[0] = position.x + 1.0f;
    list[5].pos[1] = position.y + 2.0f;
    list[5].pos[2] = position.z + 1.0f;
    list[5].colour[0] = list[5].colour[1] = list[5].colour[2] = 0.5f;
    cs::V3 origin, direction;
    float reach = -1.0f;
    const cs::EmitterLight one = cs::EmittersPick(12, radiance_of(list), to_of(list), position, n, 0.77f, position.z, [&](cs::V3 o, cs::V3 d, float r) {
      ++calls;
      origin = o;
      direction = d;
      reach = r;
      return false;
    });
    const cs::V3 to = to_of(list)(5);
    const float distance = cs::Length(to);
    CHECK(calls == 1 && one.cast && one.chosen == 5 && one.reaching == 1);
    CHECK(Near(reach, distance - cs::kEmitterEnd, 1e-5) && Near(cs::Length(direction), 1.0, 1e-5) && Near(cs::Dot(direction, to * (1.0f / distance)), 1.0, 1e-5));
    CHECK(Near(origin.x, position.x + n.x * cs::Lift(position.z), 1e-6) && Near(origin.y, position.y + n.y * cs::Lift(position.z), 1e-6));
    const cs::V3 own = TwinEmitterRadiance(list, 5, position, n);
    CHECK(Near(one.light.x, own.x, 1e-6) && Near(one.light.y, own.y, 1e-6) && Near(one.light.z, own.z, 1e-6));  // the only emitter: its own light, no cap, no ratio
    const cs::EmitterLight blocked = cs::EmittersPick(12, radiance_of(list), to_of(list), position, n, 0.77f, position.z, [&](cs::V3, cs::V3, float) { return true; });
    CHECK(blocked.cast && cs::Luma(blocked.light) == 0.0f);
    // an emitter closer than the ray's stop: the ray's length is not negative
    list[5].pos[0] = position.x + 0.1f;
    list[5].pos[1] = position.y + 0.1f;
    list[5].pos[2] = position.z;
    float short_reach = -1.0f;
    cs::EmittersPick(12, radiance_of(list), to_of(list), position, n, 0.5f, position.z, [&](cs::V3, cs::V3, float r) {
      short_reach = r;
      return false;
    });
    CHECK(short_reach == 0.0f);
  }
  // the estimate: over a stratified set of noise the one emitter's estimate averages to the sum of the emitters that are not blocked (the same as the lamps' estimator); with the cap, to the capped sum when none is
  // blocked. Emitters in front of and behind a "wall" the blocking test names: x > 3 is blocked.
  {
    std::vector<hlsl_shim::EmitterRec> list = list_of(9, 0.2f, 5.0f);
    for (auto& e : list) e.range = 80.0f;
    const auto wall = [](cs::V3 o, cs::V3 d, float reach) { return o.x + d.x * reach > 3.0f; };
    cs::V3 expected;
    for (uint32_t i = 0; i < 9; ++i) {
      const cs::V3 to = to_of(list)(i);
      const float distance = cs::Length(to);
      const bool blocked = wall(position + n * cs::Lift(position.z), to * (1.0f / distance), distance - cs::kEmitterEnd);
      if (!blocked) expected = expected + TwinEmitterRadiance(list, i, position, n);
    }
    const cs::EmitterLight sum = cs::Emitters(9, radiance_of(list));
    CHECK(!sum.cap_sum && cs::Luma(expected) > 0.0f && cs::Luma(expected) < cs::Luma(sum.light));
    cs::V3 mean;
    const int samples = 9000;
    for (int i = 0; i < samples; ++i) mean = mean + cs::EmittersPick(9, radiance_of(list), to_of(list), position, n, (float(i) + 0.5f) / float(samples), position.z, wall).light;
    mean = mean * (1.0f / float(samples));
    Check(Near(mean.x, expected.x, 6e-3) && Near(mean.y, expected.y, 6e-3) && Near(mean.z, expected.z, 6e-3), Num2("the one emitter's estimate averages to the sum of the emitters in the open (%.4f against %.4f)", mean.x, expected.x));
    // with the cap and nothing blocked: the capped sum
    std::vector<hlsl_shim::EmitterRec> bright = list_of(10, 20.0f, 2.0f);
    for (auto& e : bright) {
      e.range = 80.0f;
      e.plateau = 4.0f;
    }
    const cs::EmitterLight capped = cs::Emitters(10, radiance_of(bright));
    cs::V3 cmean;
    for (int i = 0; i < samples; ++i) cmean = cmean + cs::EmittersPick(10, radiance_of(bright), to_of(bright), position, n, (float(i) + 0.5f) / float(samples), position.z, [](cs::V3, cs::V3, float) { return false; }).light;
    cmean = cmean * (1.0f / float(samples));
    CHECK(capped.cap_sum && Near(cmean.x, capped.light.x, 6e-3) && Near(cmean.y, capped.light.y, 6e-3) && Near(cmean.z, capped.light.z, 6e-3));
  }
  // the noise: uniform in 0..1 over independent y and z, and not the lamps' number (z): over 100000 random pairs it spreads evenly across ten bins and is seldom within 0.001 of z
  {
    int bins[10] = {};
    int equal_z = 0, total = 0;
    const auto mix = [](uint32_t x) {   // an integer hash: the two numbers of a pair are independent (a plain LCG's consecutive outputs are a lattice)
      x ^= x >> 16;
      x *= 0x7feb352du;
      x ^= x >> 15;
      x *= 0x846ca68bu;
      x ^= x >> 16;
      return x;
    };
    for (int i = 0; i < 100000; ++i) {
      const float y = float(mix(uint32_t(2 * i + 1)) >> 8) / float(1u << 24), z = float(mix(uint32_t(2 * i + 2) + 0x9E3779B9u) >> 8) / float(1u << 24);
      const float v = cs::EmitterNoise(y, z);
      CHECK(v >= 0.0f && v < 1.0f);
      ++bins[std::min(int(v * 10.0f), 9)];
      equal_z += std::fabs(v - z) < 1e-3f ? 1 : 0;
      ++total;
    }
    bool even = true;
    for (int i = 0; i < 10; ++i) even = even && bins[i] > 9400 && bins[i] < 10600;
    Check(even && equal_z < 600, Num2("the emitters' choice noise is uniform and is not the lamps' own number (%.0f of 100000 within 0.001 of z, bin 0: %.0f)", equal_z, bins[0]));
  }
}

// The shader's emitters against the twin's: random hits through CityHitShade of the third compile.
void EmitterShadeChecks(const City& city, Lcg& rng, int rounds, EmitterCoverage* cov) {
  int wrong_valid = 0, wrong_colour = 0, wrong_rays = 0, wrong_hooks = 0, wrong_clear = 0;
  double worst = 0.0;
  std::vector<hlsl_shim::uint4> frame_rows;
  for (int round = 0; round < rounds; ++round) {
    Case k = RandomCase(rng, uint32_t(city.sectors.size()));
    k.p.flags &= ~(cs::kPScreen | cs::kPLamps);
    const bool flag_on = rng.Below(8) != 0;
    const bool shadow = rng.Below(3) != 0;
    if (flag_on) k.p.flags |= cs::kPEmitters;
    if (flag_on && shadow) k.p.flags |= cs::kPEmitterShadow;
    const cs::V3 hit = k.ray.origin + k.ray.direction * k.ray.t;
    const uint32_t shape = rng.Below(8);
    const uint32_t count = shape == 0 ? 0 : (shape == 1 ? 33 + rng.Below(10) : (shape == 2 ? 1 : 2 + rng.Below(12)));
    const float strength = shape == 3 ? 60.0f : 0.4f;
    hlsl_shim::gEmitters = EmittersAround(rng, hit, count, shape == 3 ? 2.5f : (shape == 4 ? 3.0f : 12.0f), strength, shape == 3);
    ++cov->cases;
    cov->with_emitters += count != 0 ? 1 : 0;
    cov->shadow += flag_on && shadow ? 1 : 0;
    cov->over_bound += count > cs::kEmitterMax ? 1 : 0;
    cov->flag_clear += flag_on ? 0 : 1;
    BindFrame(k.frame, &frame_rows);
    hlsl_shim::gSunCalls.clear();
    hlsl_shim::gLampCalls.clear();
    hlsl_shim::gEmitterCalls.clear();
    hlsl_shim::gEmitterRadianceCalls = 0;
    hlsl_shim::gEmitterToCalls = 0;
    gTwinSun.clear();
    gTwinLamp.clear();
    gTwinEmitter.clear();

    const hlsl_shim::emit::RtHitFrame f = hlsl_shim::emit::RtHitLoadFrame();
    const hlsl_shim::emit::CityHitShaded s = hlsl_shim::emit::CityHitShade(GridParams<hlsl_shim::emit::CityHitParams>(k), f, GridRay<hlsl_shim::emit::CityHitRay>(k), GridInstance<hlsl_shim::emit::CityHitInstance>(k));
    const Decoded d = DecodeCaseWith(city, city.material_words, k);
    cs::Reads reads = MakeReads(k);
    const std::vector<hlsl_shim::EmitterRec> list = hlsl_shim::gEmitters;
    int* hit_rays = &cov->rays;
    reads.emitters_at = [&list, hit_rays, cov](cs::V3 position, cs::V3 normal, bool want_shadow, float noise_y, float noise_z, float footprint) {
      (void)footprint;
      const uint32_t n = uint32_t(list.size());
      const auto radiance = [&list, position, normal](uint32_t i) { return TwinEmitterRadiance(list, i, position, normal); };
      const auto to = [&list, position](uint32_t i) { return cs::V3{list[i].pos[0], list[i].pos[1], list[i].pos[2]} - position; };
      if (!want_shadow) {
        const cs::EmitterLight t = cs::Emitters(n, radiance);
        cov->capped += t.cap_sum ? 1 : 0;
        cov->lit += t.reaching != 0 ? 1 : 0;
        return t.light;
      }
      const cs::EmitterLight t = cs::EmittersPick(n, radiance, to, position, normal, cs::EmitterNoise(noise_y, noise_z), position.z, [](cs::V3 o, cs::V3 dir, float reach) {
        gTwinEmitter.push_back({{o.x, o.y, o.z}, {dir.x, dir.y, dir.z}, reach});
        return hlsl_shim::FakeBlocked(o.x, o.y, o.z, dir.x, dir.y, dir.z);
      });
      cov->capped += t.cap_sum ? 1 : 0;
      cov->lit += t.reaching != 0 ? 1 : 0;
      *hit_rays += t.cast ? 1 : 0;
      return t.light;
    };
    const cs::Shaded t = cs::Shade(k.p, k.frame, k.ray, k.inst, d.hit, d.material, d.texture, reads, nullptr);
    if ((s.valid != 0) != t.valid) {
      ++wrong_valid;
      continue;
    }
    if (!CallsAgree(hlsl_shim::gSunCalls, gTwinSun) || !CallsAgree(hlsl_shim::gEmitterCalls, gTwinEmitter)) ++wrong_rays;
    if (!t.valid) continue;
    if (!flag_on && (hlsl_shim::gEmitterRadianceCalls != 0 || hlsl_shim::gEmitterToCalls != 0 || !hlsl_shim::gEmitterCalls.empty())) ++wrong_hooks;  // the flag clear: not a hook, not a ray
    if (flag_on && !shadow && (!hlsl_shim::gEmitterCalls.empty() || hlsl_shim::gEmitterToCalls != 0)) ++wrong_hooks;                                    // no shadow flag: no ray, no vector
    if (hlsl_shim::gEmitterCalls.size() > 1) ++wrong_hooks;                                                                                           // at most one ray an emitter term
    if (!Close(s.colour, t.colour, 5e-4)) ++wrong_colour;
    worst = std::max(worst, double(std::fabs(s.colour.x - t.colour.x) / std::max(1.0f, t.colour.x)));
    // with the flag clear the shading is the library's of old: the same call with no emitters in the list and the flag set gives the same colour
    if (!flag_on) {
      hlsl_shim::gEmitters.clear();
      hlsl_shim::gEmitterCalls.clear();
      const hlsl_shim::emit::CityHitShaded plain = hlsl_shim::emit::CityHitShade(GridParams<hlsl_shim::emit::CityHitParams>(k), f, GridRay<hlsl_shim::emit::CityHitRay>(k), GridInstance<hlsl_shim::emit::CityHitInstance>(k));
      if (std::fabs(plain.colour.x - s.colour.x) > 1e-6f || std::fabs(plain.colour.y - s.colour.y) > 1e-6f || std::fabs(plain.colour.z - s.colour.z) > 1e-6f) ++wrong_clear;
    }
  }
  hlsl_shim::gEmitters.clear();
  Check(wrong_valid == 0, Num("the emitter library has data for the hits the twin decodes, and none for the rest (%.0f differ)", wrong_valid));
  Check(wrong_colour == 0, Num2("the shaded colour with the emitters is the twin's (%.0f wrong, the worst difference %.1e)", wrong_colour, worst));
  Check(wrong_rays == 0, Num("the emitters' rays are cast from the same points, in the same directions, as far, and the sun's the same (%.0f cases differ)", wrong_rays));
  Check(wrong_hooks == 0, Num("the hooks are called only with the flag set, the vector only for a ray, and at most one ray is cast (%.0f cases differ)", wrong_hooks));
  Check(wrong_clear == 0, Num("with the flag clear the colour is the library's without emitters (%.0f cases differ)", wrong_clear));
}

void EmitterTextChecks(const std::string& src) {
  const std::string lib = ReadFile(src + "/gpu/hlsl/city_hit_shade.hlsli");
  const std::string update = ReadFile(src + "/gpu/hlsl/gi_update_trace_cs.hlsl");
  const std::string pixel = ReadFile(src + "/gpu/hlsl/gi_pixel_trace_cs.hlsl");
  const std::string glue = ReadFile(src + "/gpu/hlsl/rt_emitters.hlsli");
  const std::string refl = ReadFile(src + "/gpu/hlsl/city_refl.hlsli");
  if (refl.empty() || lib.empty() || update.empty() || pixel.empty() || glue.empty()) {
    std::printf("the sources were not found under %s: the emitters' text checks are skipped\n", src.c_str());
    return;
  }
  const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };
  // the library: the hooks, the flags, the order of the light (lamps, emitters, then the field's feedback), the ray's mask is the city's and the sun's and the lamps' are not
  CHECK(has(lib, "#if defined(CITY_HIT_EMITTER_RADIANCE)") && has(lib, "float3 CityHitEmitters(CityHitParams p, float3 position, float3 normal, float footprint)") && has(lib, "CITY_HIT_EMITTER_TO(chosenAt, position)"));
  CHECK(has(lib, "bool CityHitEmitterRay(float3 origin, float3 direction, float reach, float footprint)") && has(lib, "kCityHitCityMask") && has(lib, "CityHitBlockedBy(origin, direction, reach, footprint, 0.0, kCityHitCityMask)"));
  CHECK(has(lib, "return CityHitBlockedBy(origin, direction, tMax, footprint0, tanHalf, 0xFFu);") && has(lib, "query.TraceRayInline(RT_HIT_TLAS, RAY_FLAG_NONE, mask, ray);"));
  CHECK(has(lib, "if ((p.flags & CITY_HIT_P_EMITTERS) != 0u)") && has(lib, "light += CityHitEmitters(p, hit, normal, ray.footprint);"));
  const size_t lamps_at = lib.find("light += CityHitLamps(p, hit, normal, ray.footprint);"), emit_at = lib.find("light += CityHitEmitters(p, hit, normal, ray.footprint);"), feedback_at = lib.find("light += CITY_HIT_FEEDBACK(hit, normal) * p.bounce;");
  CHECK(lamps_at != std::string::npos && emit_at != std::string::npos && feedback_at != std::string::npos && lamps_at < emit_at && emit_at < feedback_at);
  CHECK(has(lib, "if (!(weightSum > 0.0))") && has(lib, "seen == reaching") && has(lib, "min(uint(CITY_HIT_EMITTER_COUNT), kCityHitEmitterMax)"));
  // the includers: the probe field reads the STATIC emitters, the per-pixel pass all of them; both turn the flags on only where the world frame has emitters; the per-pixel pass's own headlights are gone
  CHECK(has(update, "#define CITY_HIT_EMITTER_COUNT (g_world.emitterStatic)") && has(update, "#define CITY_HIT_EMITTER_RADIANCE(i, P, N) RtEmitterRadiance(g_world, (i), (P), (N))") &&
        has(update, "#define CITY_HIT_EMITTER_TO(i, P) RtEmitterTo(g_world, (i), (P))") && has(update, "if (g_world.emitterStatic != 0u)") && has(update, "p.flags |= CITY_HIT_P_EMITTERS | CITY_HIT_P_EMITTER_SHADOW;"));
  CHECK(has(update, "#include \"light_laws.hlsli\"") && has(update, "#include \"rt_emitters.hlsli\""));
  CHECK(has(pixel, "#define CITY_HIT_EMITTER_COUNT (g_world.emitterCount)") && has(pixel, "if (g_world.emitterCount != 0u)") && has(pixel, "params.flags = params.flags | CITY_HIT_P_EMITTERS | CITY_HIT_P_EMITTER_SHADOW;") &&
        has(pixel, "#include \"rt_emitters.hlsli\""));
  CHECK(!has(pixel, "GipLights") && !has(pixel, "g_lightCount") && !has(pixel, "#define CITY_HIT_AMBIENT"));
  CHECK(has(glue, "LlLightTerm(LlUnpack(RtEmitterGpuLight(w, i)), pw, nw, 1.0)") && has(glue, "if (!(dot(d, d) < range * range))"));
  // the reflections' city hits gain them: all of them, inside the lamps' block (CITY_REFL_NO_LAMPS leaves it out line for line), only where the world frame is valid and row 44 has emitters
  {
    const size_t no_lamps_end = refl.find("#endif\n#include \"city_hit_shade.hlsli\"");
    const size_t macro_at = refl.find("#define CITY_HIT_EMITTER_COUNT (gCityEmitWorld.emitterCount)");
    CHECK(macro_at != std::string::npos && no_lamps_end != std::string::npos && macro_at < no_lamps_end && has(refl, "#include \"rt_emitters.hlsli\"") && has(refl, "static RtWorld gCityEmitWorld;"));
    CHECK(has(refl, "if (lampsWorld && RtEmittersUploaded() != 0u)") && has(glue, "return gRtWorld[RT_WORLD_ROW_EMITTERS].x;") && has(refl, "p.flags = p.flags | CITY_HIT_P_EMITTERS | CITY_HIT_P_EMITTER_SHADOW;") &&
          has(refl, "#define CITY_HIT_EMITTER_RADIANCE(i, P, N) RtEmitterRadiance(gCityEmitWorld, (i), (P), (N))"));
  }
}

void EmitterChecks(const City& city, Lcg& rng, const std::string& src) {
  EmitterTwinChecks(rng);
  EmitterCoverage cov;
  EmitterShadeChecks(city, rng, 5000, &cov);
  std::printf("emitters at hits: %d random hits, %d with emitters, %d lit by one, %d with a shadow flag, %d rays, %d at the sum's cap, %d with more than the bound, %d with the flag clear\n", cov.cases, cov.with_emitters, cov.lit,
              cov.shadow, cov.rays, cov.capped, cov.over_bound, cov.flag_clear);
  Check(cov.cases == 5000 && cov.with_emitters > 3500 && cov.lit > 800 && cov.shadow > 2500 && cov.rays > 300 && cov.capped > 20 && cov.over_bound > 200 && cov.flag_clear > 400,
        Num2("the random hits covered emitters in reach, the shadow ray, the cap, the count's bound and the flag clear (lit %.0f, rays %.0f)", cov.lit, cov.rays));
  EmitterTextChecks(src);
}
