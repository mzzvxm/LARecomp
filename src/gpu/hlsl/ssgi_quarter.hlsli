#ifndef SSGI_QUARTER_HLSLI
#define SSGI_QUARTER_HLSLI
// LT-8 quarter trace. Geometry is centred on the four inner samples of a
// 4x4 block. Keep the picked sample and its vehicle ID in the guide's low bits.
static const uint kSsgiQuarterFlag = 1u << 30;
static const uint kSsgiQuarterApply = 1u << 14;
static const uint kSsgiQuarterVehicleShift = 4u;

int2 SsgiQuarterSource(int2 pixel, uint source, int2 last) {
  return min(pixel * 4 + 1 + int2(source & 1u, source >> 1u), last);
}

uint SsgiQuarterVehicle(float depth) {
  return (asuint(depth) >> kSsgiQuarterVehicleShift) & 31u;
}

float SsgiQuarterMarkVehicle(float depth, uint vehicle) {
  return asfloat((asuint(depth) & ~(31u << kSsgiQuarterVehicleShift)) |
                 (vehicle << kSsgiQuarterVehicleShift));
}

float SsgiQuarterLength(float history_length, float confidence, float cap) {
  return 1.0f + confidence * min(history_length, cap - 1.0f);
}

int SsgiQuarterStep(float history_length) {
  return history_length < 4.0f ? 2 : 1;
}

// Quadratic falloff across a silhouette and away from the receiver plane.
// No positive floor: a tap across an edge must never light an isolated pixel.
float SsgiQuarterWeight(float depth, float centre_depth, float plane_distance) {
  const float depth_delta = abs(depth - centre_depth) / (0.05f * centre_depth + 0.05f);
  const float plane_delta = abs(plane_distance) / (0.01f * centre_depth + 0.035f);
  const float w = saturate(1.0f - max(depth_delta, plane_delta));
  return w * w;
}
#endif
