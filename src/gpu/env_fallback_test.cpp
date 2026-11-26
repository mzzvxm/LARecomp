#include "env_fallback_rules.h"

#include <cstdio>

namespace {

namespace rules = mcla::gpu::env_fallback;
using float2 = rules::Vec2;
using float3 = rules::Vec3;
using std::abs;
using std::exp2;
using std::sqrt;

float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 operator*(float3 v, float k) { return {v.x * k, v.y * k, v.z * k}; }
float3 operator/(float3 v, float k) { return {v.x / k, v.y / k, v.z / k}; }
float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 normalize(float3 a) { return a / std::sqrt(dot(a, a)); }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float smoothstep(float lo, float hi, float value) {
  const float t = saturate((value - lo) / (hi - lo));
  return 3.0f * t * t - 2.0f * t * t * t;
}
float lerp(float a, float b, float t) { return a * (1.0f - t) + b * t; }

#define ENV_FALLBACK_MATH_ONLY
#include "hlsl/env_fallback.hlsli"
#undef ENV_FALLBACK_MATH_ONLY

int checked = 0;
int failed = 0;

void Check(bool result, const char* description) {
  ++checked;
  if (!result && failed++ < 20) {
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
}

bool Near(float a, float b, float tolerance = 2.0e-6f) { return std::abs(a - b) <= tolerance; }

bool Near(float3 a, float3 b, float tolerance = 2.0e-6f) {
  return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}

void TestMapAndMip() {
  Check(rules::PackMapWord(13, 256) == 0x0100000Du, "the slot and hemisphere size fit one word");
  Check(rules::PackMapWord(32767, 768) == 0x03007FFFu, "a high slot and ultra map fit");
  Check(rules::PackMapWord(13, 0) == 0, "no texture width means unavailable");
  Check(rules::PackMapWord(0xFFFFFFFFu, 256) == 0, "a failed descriptor allocation means unavailable");
  Check(rules::PackMapWord(0x10000u, 256) == 0, "an oversized slot cannot alias a valid one");
  Check(rules::PackMapWord(13, 0x10000u) == 0, "an oversized width cannot alias a valid one");
  Check(EnvFallbackMip(-1.0f) == 0.0f && EnvFallbackMip(0.0f) == 0.0f, "a mirror uses the base level");
  Check(EnvFallbackMip(1.5f) == 3.0f && EnvFallbackMip(1.0f) == 3.0f, "roughness uses the last existing level");
  Check(EnvFallbackMip(0.5f) == 1.5f, "intermediate roughness interpolates between mips");
  for (int i = -64; i <= 128; ++i) {
    const float roughness = float(i) / 64.0f;
    Check(Near(rules::Mip(roughness), EnvFallbackMip(roughness)), "CPU and shader agree on mip");
  }
}

void TestCapture() {
  constexpr mcla::gpu::env_hold::Eye capture{{20.0f, 8.0f, -30.0f}, true};
  constexpr mcla::gpu::env_hold::Eye moved{{22.0f, 7.0f, -26.0f}, true};
  rules::ProbeFrame probe;
  probe.Note(1, 12, false, moved);
  Check(probe.written() == 0, "a hold cannot introduce a capture");
  probe.Note(1, 12, true, capture);
  Check(probe.written() == 12 && probe.has_origin() && probe.CameraDelta() == std::array{0.0f, 0.0f, 0.0f},
        "a complete fresh capture starts at the current eye");
  probe.EndFrame();
  Check(probe.written() == 0 && !probe.has_origin(), "a view without an environment marker cannot use the map");
  probe.Note(1, 12, false, moved);
  Check(probe.written() == 12 && probe.CameraDelta() == std::array{2.0f, -1.0f, 4.0f},
        "a held map keeps its original eye as the view moves");
  probe.Note(1, 12, false, {});
  Check(probe.written() == 12 && !probe.has_origin() && probe.CameraDelta() == std::array{0.0f, 0.0f, 0.0f},
        "a missing view pose still permits an infinity lookup");
  probe.Note(2, 12, false, moved);
  Check(probe.written() == 0, "another renderer cannot use the old probe");
  probe.Note(1, 13, false, moved);
  Check(probe.written() == 0, "a later write cannot use the old probe eye");
  probe.Note(2, 14, true, {});
  probe.Note(2, 14, false, moved);
  Check(probe.written() == 14 && !probe.has_origin(), "a held map with an unknown capture eye stays directional");
  probe.Note(2, 0, true, capture);
  probe.Note(2, 14, false, moved);
  Check(probe.written() == 0, "a failed refresh invalidates older captures");
  probe.Note(0, 15, true, capture);
  Check(probe.written() == 0, "a probe needs an owning renderer");

  Check(rules::LevelMatches(24, 24, true), "a complete mip written with the capture is reusable");
  Check(!rules::LevelMatches(24, 23, true), "a stale mip cannot join a fresh map");
  Check(!rules::LevelMatches(24, 25, true), "a rewritten mip invalidates the old probe");
  Check(!rules::LevelMatches(24, 24, false), "a partial mip cannot join a complete map");
  Check(!rules::LevelMatches(0, 0, true), "an unwritten mip has no capture token");
}

void TestDirection() {
  const float3 origin{0.0f, 0.0f, 0.0f};
  const float3 ray{1.0f, 0.0f, 0.0f};
  const float3 offset{0.0f, -24.0f, 0.0f};
  Check(Near(EnvFallbackDirection(origin, ray, true), ray), "the probe origin has no parallax");
  Check(Near(EnvFallbackDirection(offset, ray, true), {0.8f, -0.6f, 0.0f}), "forward sphere exit gives the expected direction");
  Check(Near(EnvFallbackDirection(offset, ray, false), ray), "non-ground receivers use a direction at infinity");
  const float3 sky = normalize({1.0f, 1.0f, 0.0f});
  Check(Near(EnvFallbackDirection(offset, sky, true), sky), "sky rays keep their direction");
  Check(Near(EnvFallbackDirection({0.0f, -40.0f, 0.0f}, ray, true), ray), "at the proxy boundary the correction is zero");
  Check(Near(EnvFallbackDirection({0.0f, -41.0f, 0.0f}, ray, true), ray), "outside the proxy the direction stays finite");

  for (int step = 1; step <= 59; ++step) {
    const float d = 0.5f * float(step);
    for (int angle = 0; angle < 32; ++angle) {
      const float a = float(angle) * 0.19634954f;
      const float3 receiver{d * std::cos(a), -4.0f, d * std::sin(a)};
      const float3 direction = EnvFallbackDirection(receiver, ray, true);
      // An independent exit calculation for rays along +X. The perpendicular
      // coordinates stay fixed and the exit X must be on the positive side.
      const float3 expected{std::sqrt(1600.0f - receiver.y * receiver.y - receiver.z * receiver.z) / 40.0f,
                           receiver.y / 40.0f, receiver.z / 40.0f};
      Check(Near(direction, expected), "a ray from inside the proxy hits its forward side");
    }
  }
  for (int step = 0; step <= 44; ++step) {
    const float3 receiver{2.0f, -float(step), 4.0f};
    for (int angle = -20; angle <= 20; ++angle) {
      const float3 incoming = normalize({1.0f, float(angle) / 32.0f, 0.5f});
      const float3 direction = EnvFallbackDirection(receiver, incoming, true);
      Check(Near(direction, rules::Direction(receiver, incoming, true)), "CPU and shader agree across sky and radius fades");
      Check(Near(dot(direction, direction), 1.0f), "the faded lookup direction stays unit length");
    }
  }
  for (float boundary : {30.0f, 40.0f}) {
    Check(Near(EnvFallbackDirection({0.0f, 0.0f, boundary - 0.001f}, ray, true),
               EnvFallbackDirection({0.0f, 0.0f, boundary + 0.001f}, ray, true), 7.0e-5f),
          "proxy radius fades continuously");
  }
}

void TestAtlas() {
  for (float side : {256.0f, 512.0f, 768.0f}) {
    for (int level = 0; level <= 12; ++level) {
      const float mip = float(level) * 0.25f;
      const float inset = EnvFallbackRimInset(mip, side);
      Check(Near(inset, rules::RimInset(mip, side)), "CPU and shader agree on the mip footprint");
      const float2 front = EnvFallbackUv({0.0f, 0.0f, -1.0f}, true, inset);
      const float2 back = EnvFallbackUv({0.0f, 0.0f, 1.0f}, false, inset);
      Check(Near(front.x, 0.25f) && Near(front.y, 0.5f) && Near(back.x, 0.75f) && Near(back.y, 0.5f),
            "the poles land in their respective halves");
      Check(Near(EnvFallbackRightWeight(-inset, inset), 0.0f) &&
            Near(EnvFallbackRightWeight(0.0f, inset), 0.5f) && Near(EnvFallbackRightWeight(inset, inset), 1.0f),
            "the equator blends both halves and joins each half continuously");
      for (int angle = 0; angle < 16; ++angle) {
        const float a = float(angle) * 0.39269908f;
        for (int step = -8; step <= 8; ++step) {
          const float z = float(step) * inset / 4.0f;
          const float radial = std::sqrt(1.0f - z * z);
          const float3 r{std::cos(a) * radial, std::sin(a) * radial, z};
          const float2 left = EnvFallbackUv(r, true, inset);
          const float2 right = EnvFallbackUv(r, false, inset);
          const float2 expected = rules::AtlasUv(r, true, inset);
          Check(Near(left.x, expected.x) && Near(left.y, expected.y), "CPU and shader agree on the atlas projection");
          Check(Near(left.x + right.x, 1.0f) && Near(left.y, right.y), "a seam tap uses mirrored rims in both halves");
          const float radius = std::hypot((left.x - 0.25f) * 4.0f, (left.y - 0.5f) * 2.0f);
          Check(radius <= 1.0f - inset + 2.0e-6f, "the seam sample keeps its footprint inside the disc");
          Check(Near(EnvFallbackRightWeight(z, inset), rules::RightWeight(z, inset)), "CPU and shader agree on the seam blend");
        }
      }
    }
  }
  const float2 upper = EnvFallbackUv({0.0f, 0.6f, -0.8f}, true, 0.125f);
  Check(Near(upper.y, 1.0f / 3.0f), "positive world Y faces the top of the map");
  const float2 left = EnvFallbackUv({0.6f, 0.0f, -0.8f}, true, 0.125f);
  const float2 right = EnvFallbackUv({0.6f, 0.0f, 0.8f}, false, 0.125f);
  Check(Near(left.x, 1.0f / 3.0f) && Near(right.x, 2.0f / 3.0f), "the world X axes point in opposite UV directions");
}

}  // namespace

int main() {
  TestMapAndMip();
  TestCapture();
  TestDirection();
  TestAtlas();
  std::printf("environment fallback: %d checks, %d failed\n", checked, failed);
  return failed ? 1 : 0;
}
