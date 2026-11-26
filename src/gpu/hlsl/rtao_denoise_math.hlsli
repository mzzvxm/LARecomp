#ifndef MCLA_RTAO_DENOISE_MATH_HLSLI
#define MCLA_RTAO_DENOISE_MATH_HLSLI

bool RtAoAcceptTap(float depth, float expected, bool same_vehicle, float normal_dot) {
  return same_vehicle && normal_dot >= 0.8f && abs(depth - expected) <= 0.04f * expected + 0.02f;
}

float RtAoMotionCap(float speed, float user_cap) {
  return min(user_cap, clamp(32.0f / (1.0f + 1.2f * speed), 4.0f, 32.0f));
}

float RtAoHistoryLength(float weighted_length, float weights, float cap) {
  if (weights < 0.25f) return 0.0f;
  return min(weighted_length / weights * min(1.0f, 2.0f * weights), cap);
}

float RtAoClip(float history, float mean, float sigma, float lo, float hi) {
  const float radius = max(1.25f * sigma, 0.2f);
  const float padding = (hi - lo) * 0.1f;
  return clamp(history, max(mean - radius, lo - padding), min(mean + radius, hi + padding));
}

float RtAoBlend(float history, float current, float length) {
  return lerp(history, current, 1.0f / (length + 1.0f));
}

float RtAoNextLength(float length, float cap) {
  return min(length + 1.0f, cap);
}

float RtAoResolveEdgeWeight(float centre, float tap) {
  return saturate(1.0f - abs(centre - tap) * 4.0f);
}

float RtAoUpsampleWeight(float bilinear, float depth, float receiver, bool same_surface) {
  if (!same_surface) return 0.0f;
  const float dz = (depth - receiver) / max(0.02f * receiver, 0.05f);
  return bilinear * exp(-dz * dz);
}

float RtAoVisibility(float open, float power, float floor_) {
  return max(pow(saturate(open), power), floor_);
}

bool RtAoBlendScreen(uint flags, bool vehicle) {
  return (flags & (1u << 0)) != 0u || (!vehicle && (flags & (1u << 2)) != 0u);
}

float RtAoCoverage(float rt, float coverage, float screen, bool blend_screen) {
  return lerp(screen, blend_screen ? min(rt, screen) : rt, coverage);
}

float4 RtAoWithBounce(float term, float4 bounce) {
  return float4(term, bounce.y, bounce.z, bounce.w);
}

#endif
