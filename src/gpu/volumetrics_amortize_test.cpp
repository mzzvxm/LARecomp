#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

#include "hlsl_cpp_shim.h"
#include "volumetrics_amortize.h"
#include "volumetrics_stable.h"

namespace hlsl_shim {
#include "hlsl/volumetrics_amortize.hlsli"
}

namespace {
using namespace hlsl_shim;
namespace vol = mcla::gpu::volumetrics;

int checks = 0;
int failures = 0;

void Check(bool pass, const char* what) {
  ++checks;
  if (!pass && ++failures <= 16) std::cerr << what << '\n';
}

void ParityAndNoise() {
  Check(vol::kLampTileSide == kLampTileSide && vol::kLampTileBytes == kLampTileBytes && vol::kFusedSlices == kFusedSlices,
        "host dispatch and tile stride match the shader");
  for (uint frame : {0u, 1u, 63u, 64u, 65535u, 65536u}) {
    for (uint y : {0u, 1u, 53u, 89u}) {
      for (uint x : {0u, 1u, 95u, 159u}) {
        std::array<bool, 64> seen{};
        for (uint lane = 0; lane < 64; ++lane) {
          const uint z = CheckerSlice(lane, uint2(x, y), frame);
          const uint next = CheckerSlice(lane, uint2(x, y), frame + 1u);
          Check(z < 64 && !seen[z] && next == (z ^ 1u), "each slice maps once and swaps parity on the next frame");
          Check(z == vol::CheckerSlice(lane, x, y, frame) && (((x + y + z + frame) & 1u) == (lane / 32u)),
                "fresh lanes contain exactly the even checkerboard");
          if (z < 64) seen[z] = true;
        }
      }
    }
  }
  const float phases[] = {0.618034f, 0.414214f, 0.732051f};
  const uint bases[] = {2u, 3u, 5u};
  for (uint axis = 0; axis < 3; ++axis) {
    for (uint parity = 0; parity < 2; ++parity) {
      std::array<bool, 8> bins{};
      for (uint frame = parity; frame < 128; frame += 2) {
        const float jitter = vol::AmortizedJitter(frame, bases[axis], phases[axis]);
        Check(jitter >= 0.0f && jitter < 1.0f, "jitter lies inside its cell");
        if (jitter >= 0.0f && jitter < 1.0f) bins[uint(jitter * 8.0f)] = true;
        Check(jitter != vol::AmortizedJitter(frame + 1, bases[axis], phases[axis]), "jitter varies on consecutive frames");
      }
      Check(std::all_of(bins.begin(), bins.end(), [](bool occupied) { return occupied; }),
            "each parity covers the full voxel, including the base-2 direction");
    }
  }
}

void LampMasks(std::mt19937& rng) {
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  Check(VolCullingBasisValid(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, -1)), "an orthonormal camera can cull");
  Check(!VolCullingBasisValid(float3(0.99f, 0, 0), float3(0, 1, 0), float3(0, 0, -1)), "a scaled camera keeps all lamp bounds");
  const float3 sheared_up(0.01f, std::sqrt(1.0f - 0.01f * 0.01f), 0);
  Check(!VolCullingBasisValid(float3(1, 0, 0), sheared_up, float3(0, 0, -1)), "unit-length but sheared axes keep all lamp bounds");
  const float2 test_depth(2.0f, 4.0f);
  const float4 test_slope(-0.2f, 0.2f, -0.2f, 0.2f);
  for (float3 far : {float3(-6, 0, 3), float3(6, 0, 3), float3(0, -6, 3), float3(0, 6, 3), float3(0, 0, 0), float3(0, 0, 6)}) {
    Check(LampSphereOutsideSlab(far, 1.0f, test_slope, test_depth), "a sphere separated by one slab plane is rejected");
  }
  Check(!LampSphereOutsideSlab(float3(0, 0, 1), 1.01f, test_slope, test_depth), "a sphere meeting the near plane is kept");
  Check(LampSphereOutsideCone(float3(0, 0, -4), float3(0, 0, 1), 0.8f, 0.5f), "a narrow cone pointing away is rejected");
  Check(!LampSphereOutsideCone(float3(0, 0, 4), float3(0, 0, 1), 0.8f, 0.5f), "a cone pointing into the tile is kept");
  Check(!LampSphereOutsideCone(float3(0, 0, -4), float3(0, 0, 1), -1.0f, 0.5f), "an omnidirectional lamp is kept");

  uint tested_culled = 0;
  uint tested_lit = 0;
  for (uint i = 0; i < 3000; ++i) {
    const auto g = vol::GridFor(vol::Level(1 + i % 3));
    const uint x = 8 * (rng() % vol::LampTiles(g.nx));
    const uint y = 8 * (rng() % vol::LampTiles(g.ny));
    const uint z = 8 * (rng() % vol::LampTiles(g.nz));
    const float tan_x = 0.5f + unit(rng);
    const float tan_y = 0.3f + unit(rng);
    const auto sx = [&](float cell) { return (2.0f * cell / g.nx - 1.0f) * tan_x; };
    const auto sy = [&](float cell) { return (1.0f - 2.0f * cell / g.ny) * tan_y; };
    const auto depth = [&](uint slice) { return g.near_m * std::pow(g.far_m / g.near_m, float(slice) / g.nz); };
    const uint end_x = std::min(x + 8, g.nx);
    const uint end_y = std::min(y + 8, g.ny);
    const float4 slopes(sx(float(x)), sx(float(end_x)), sy(float(end_y)), sy(float(y)));
    const float2 bounds(depth(z), depth(z + 8));
    const float3 lo(std::min(slopes.x * bounds.x, slopes.x * bounds.y), std::min(slopes.z * bounds.x, slopes.z * bounds.y), bounds.x);
    const float3 hi(std::max(slopes.y * bounds.x, slopes.y * bounds.y), std::max(slopes.w * bounds.x, slopes.w * bounds.y), bounds.y);
    const float3 center = (lo + hi) * 0.5f;
    const float radius = length(hi - lo) * 0.5f;
    const float spread = radius * 3.0f + 5.0f;
    const float3 pos = center + spread * float3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f);
    const float3 axis = normalize(float3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f));
    const float range = spread * unit(rng);
    const float cone = i % 5 == 0 ? -1.0f : unit(rng);
    const bool culled = LampSphereOutsideSlab(pos, range, slopes, bounds) || LampSphereOutsideCone(center - pos, axis, cone, radius);
    vol::VolLamp lamp{{pos.x, pos.y, pos.z}, range, {axis.x, axis.y, axis.z}, cone, {1, 1, 1}, (1 + cone) * 0.5f, -1, {0.64f}, -1, 0};
    const float camera[] = {0, 0, 0};
    for (uint j = 0; j < 48; ++j) {
      const uint cx = x + rng() % (end_x - x);
      const uint cy = y + rng() % (end_y - y);
      const uint cz = z + rng() % 8;
      const float3 ray = float3(sx(float(cx) + 0.5f), sy(float(cy) + 0.5f), 1);
      const float len = length(ray);
      const float dir[] = {ray.x / len, ray.y / len, 1.0f / len};
      const float oracle = vol::LampSliceFactor(lamp, camera, dir, depth(cz) * len, depth(cz + 1) * len, 0.25f);
      tested_lit += oracle > 0.0f;
      tested_culled += culled;
      // With unit white lamps this compares the contribution to the froxel
      // directly, without depending on the number of lamps in its tile.
      Check(!culled || oracle == 0.0f, "culled lamps contribute zero to the brute-force slice");
    }
  }
  Check(tested_culled > 20000 && tested_lit > 1000, "the random test exercises both rejected and contributing lamps");
}

void ChangedLights() {
  vol::VolGpuConstants c{};
  std::array<vol::VolLamp, vol::kMaxLamps> previous{};
  previous[0].pos[0] = 100.0f;
  previous[0].colour[0] = 1.0f;
  previous[1].pos[0] = 200.0f;
  previous[2].pos[0] = 300.0f;
  c.lamps[0] = previous[2];
  c.lamps[1] = previous[0];
  c.lamps[1].colour[0] = 0.8f;
  c.lamps[33] = previous[1];
  c.lamps[33].pos[0] += 1.0f;
  c.counts[0] = 34;
  vol::MarkLampChanges(previous, 3, c);
  Check((c.lamp_changes[0] & 3u) == 0u, "reordering and a gradual fade preserve history");
  Check(c.lamp_changes[1] == 3u && c.lamp_changes[2] == 1u && c.retired_lamps[0].pos[0] == 200.0f,
        "an arriving and departing light mark both sides of the upper-word mask");
  auto bright = previous[0];
  bright.colour[0] *= 2.0f;
  Check(!vol::SameLampForHistory(bright, previous[0]), "an abrupt change in a lamp invalidates its history");

  const float geometry_before[] = {0.1f, 0.1f, 0.1f};
  const float geometry_after[] = {0, 0, 0};
  const float ambient[] = {0.01f, 0.01f, 0.01f};
  const float old[] = {0.07f, 0.07f, 0.07f};  // ambient plus old beam at sigma = 0.01
  float out[3] = {};
  const float retain = VolHistoryWeight(0.9f, true, false);
  const float moving_away = vol::CompensateHeadlightHistory(old, ambient, geometry_after, geometry_before, 0.01f, 0.01f, 60.0f, out);
  Check(retain == 1.0f && moving_away == 1.0f && std::abs(out[0] - ambient[0]) < 1e-7f,
        "a retained slice loses the old unshadowed beam when the headlight leaves");
  const float arriving = vol::CompensateHeadlightHistory(ambient, ambient, geometry_before, geometry_after, 0.01f, 0.01f, 60.0f, out);
  Check(arriving == 0.0f, "a new beam uses fresh lighting even on retained parity");
  Check(std::abs(VolHistoryWeight(0.9f, false, false) - 0.81f) < 1e-6f, "two-frame weight preserves the filter time constant");
  Check(VolHistoryWeight(0.9f, true, true) == 0.9f, "shadowed headlight tiles keep their full-rate filter");
}

void Integrate(std::mt19937& rng) {
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  for (uint trial = 0; trial < 240; ++trial) {
    const uint count = 24 + 8 * (trial % 6);
    std::array<float4, 64> scatter, first, prefix, scratch;
    const float near_z = trial % 2 ? 0.5f : 2.0f;
    const float ln = std::log(160.0f / near_z);
    const auto depth = [&](float slice) { return near_z * std::exp(ln * slice / count); };
    for (uint z = 0; z < 64; ++z) {
      scatter[z] = float4(unit(rng), unit(rng) * 0.2f, unit(rng) * 0.05f, trial % 3 == 0 ? 0.0f : unit(rng) * 0.25f);
      first[z] = z < count ? VolumeSegment(scatter[z], depth(float(z) + 0.5f) - depth(float(z))) : float4(0, 0, 0, 1);
      prefix[z] = z < count ? ComposeVolume(first[z], VolumeSegment(scatter[z], depth(float(z) + 1) - depth(float(z) + 0.5f))) : first[z];
    }
    for (uint stride = 1; stride < 64; stride *= 2) {
      for (uint z = 0; z < 64; ++z) scratch[z] = z < stride ? prefix[z] : ComposeVolume(prefix[z - stride], prefix[z]);
      prefix.swap(scratch);
    }

    float3 s(0, 0, 0);
    float t = 1;
    for (uint z = 0; z < count; ++z) {
      const float4 sv = scatter[z];
      const float sigma = std::max(sv.w, 1e-6f);
      const auto march = [&](float distance) {
        const float tz = std::exp(-sigma * distance);
        s.x += t * (sv.x - sv.x * tz) / sigma;
        s.y += t * (sv.y - sv.y * tz) / sigma;
        s.z += t * (sv.z - sv.z * tz) / sigma;
        t *= tz;
      };
      march(depth(float(z) + 0.5f) - depth(float(z)));
      const float4 got = z == 0 ? first[z] : ComposeVolume(prefix[z - 1], first[z]);
      const auto close = [](float a, float b) { return std::abs(a - b) <= 1e-4f * std::max(1e-6f, std::abs(b)); };
      Check(close(got.x, s.x) && close(got.y, s.y) && close(got.z, s.z) && close(got.w, t),
            "ordered fused prefix agrees with the separate integrate at slice centres, within 1e-4 relative");
      march(depth(float(z) + 1) - depth(float(z) + 0.5f));
    }
  }
}

void ShaderWiring(const std::string& gpu_path) {
  const auto read = [&](const char* name) {
    std::ifstream file(gpu_path + "/hlsl/" + name);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  const auto inject = read("volumetrics_inject_cs.hlsl");
  const auto cull = read("volumetrics_cull_cs.hlsl");
  const auto has = [](std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; };
  Check(has(inject, "tile.w == 0u &&") && has(inject, "if (history_ok && retained_slice && !headlight_tile) return old;"),
        "only valid clean history outside headlight bounds skips fresh shading");
  Check(has(inject, "VolHistoryWeight(history.z, retained_slice, full_rate)") && has(inject, "weight *= CompensateHeadlightHistory"),
        "retained headlight slices still run moving-beam compensation");
  Check(has(inject, "const bool full_rate = (tile.z & kTileHeadlightShadow) != 0u;") &&
        has(cull, "HeadlightTileFlags(previous_head, true)"), "current and previous shadowed beams keep the full-rate filter");
  Check(has(cull, "changes[word] & bit") && has(cull, "kO_retired_lamps + lane * 64u, history_pad"),
        "new and old lamp bounds both invalidate history tiles");
  Check(has(cull, "if (cull_safe == 0u) return true;"), "a nonrigid camera bypasses culling");
  Check(has(inject, "ComposeVolume(prefix_a[z - stride], prefix_a[z])") &&
        has(inject, "ComposeVolume(prefix_b[z - stride * 2u], prefix_b[z])") && has(inject, "stride *= 4u") &&
        has(inject, "ComposeVolume(prefix_a[z - 1u], first)"), "the shader marches the same near-to-far scan to the slice centre");
}
}  // namespace

int main(int argc, char** argv) {
  std::mt19937 rng(0x504F5354u);
  ParityAndNoise();
  LampMasks(rng);
  ChangedLights();
  Integrate(rng);
  ShaderWiring(argc > 1 ? argv[1] : "src/gpu");
  std::cout << "volumetrics amortize: " << checks << " checks, " << failures << " failures\n";
  return failures != 0;
}
