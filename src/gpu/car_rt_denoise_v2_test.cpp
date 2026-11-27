#include "car_rt_denoise_v2_rules.h"

#include <cstdio>
#include <limits>

namespace rules = mcla::gpu::car_rt_denoise_v2;
namespace temporal = mcla::gpu::temporal;

int main() {
  unsigned checks = 0;
  unsigned failures = 0;
  const auto check = [&](bool pass, const char* message) {
    ++checks;
    if (pass) return;
    ++failures;
    std::fprintf(stderr, "%s\n", message);
  };
  const auto close = [](double a, double b) { return std::abs(a - b) < 0.0001; };
  const float current_rows[3][4] = {{0, -2, 0, 5}, {1, 0, 0, 1}, {0, 0, 1, 8}};
  const float previous_rows[3][4] = {{1, 0, 0, 2}, {0, 1, 0, -1}, {0, 0, 1, 10}};
  const auto current = rules::InstanceMatrix(current_rows);
  const auto previous = rules::InstanceMatrix(previous_rows);
  temporal::Mat4 reprojection;
  check(rules::PreviousInstance(current, previous, reprojection), "transformed instance should be invertible");
  check(close(current.m[3][0], 5) && close(current.m[1][0], -2), "instance transform must transpose Plume rows");
  check(temporal::MaxAbsDiff(temporal::Mul(current, reprojection), previous) < 1.0e-8,
        "reprojection moves the current pixel back through the part's local frame");
  check(!rules::PreviousInstance({}, previous, reprojection), "singular motion must reject history");
  check(rules::UniqueMatch(0.2f, 0.6f), "unique nearby part must reproject");
  check(rules::UniqueMatch(1.8f, std::numeric_limits<float>::infinity()), "fast but unique part must reproject");
  check(!rules::UniqueMatch(0.2f, 0.24f), "ambiguous nearby instances must reject history");
  check(!rules::UniqueMatch(2.1f, 3.0f), "distant same-mesh instance must reject history");
  check(!rules::UniqueMatch(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()),
        "missing instances must reject history");
  check(close(rules::HitWeight(0.0f, 0.0f, false), 1.0f), "two misses are compatible");
  check(close(rules::HitWeight(0.0f, 2.0f, false), 0.4f), "a noisy paint miss may retain limited history");
  check(rules::HitWeight(0.0f, 2.0f, true) == 0.0f, "glass must preserve sharp hit/miss edges");
  check(rules::HitWeight(4.0f, 4.2f, true) == 0.0f, "glass must preserve reflected depth edges");
  check(rules::HitWeight(4.0f, 4.2f, false) > 0.8f, "paint cone accepts nearby hit distances");
  check(rules::HitWeight(4.0f, 4.04f, true) > 0.6f, "glass accepts a nearby sample of the same hit");
  check(rules::IsConsecutive(39, 40, false, true), "compatible consecutive frames retain history");
  check(!rules::IsConsecutive(39, 41, false, true), "a skipped trace breaks history");
  check(!rules::IsConsecutive(39, 40, true, true), "camera cuts break history");
  check(!rules::IsConsecutive(39, 40, false, false), "signal changes break history");
  check(!rules::IsConsecutive(UINT64_MAX, 0, false, true), "frame wrap breaks history");
  check(rules::Signature({1, 2, 3}) != rules::Signature({1, 2, 4}), "a changed trace setting changes the signature");
  check(rules::Iterations(false) == 3 && rules::Iterations(true) == 2, "paint uses a wider kernel than glass");
  for (bool glass : {false, true}) {
    const uint32_t count = rules::Iterations(glass);
    for (uint32_t i = 1; i < count; ++i) {
      check(rules::WritesTrace(i, count) != rules::WritesTrace(i - 1u, count), "spatial passes must ping-pong");
    }
    check(rules::WritesTrace(count - 1u, count), "the final spatial result must replace the trace image");
  }
  for (const uint32_t divisor : {1u, 2u}) {
    for (const uint32_t render : {1u, 13u, 32u}) {
      for (uint32_t p = 0; p <= (render - 1) / divisor; ++p) {
        const auto pixel = rules::SamplePixel(float(p), divisor, render);
        check(close(rules::ImageTexel(pixel, divisor, render), p), "projection must round-trip the traced texel");
      }
    }
  }
  check(close(rules::SamplePixel(2, 2, 8), 5.5), "half-size grid reads the bottom-right sample");
  check(close(rules::ImageTexel(4.5, 2, 5), 2), "last odd pixel enters the partial block");
  std::printf("%u checks, %u failed\n", checks, failures);
  return failures ? 1 : 0;
}
