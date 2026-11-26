#ifndef MCLA_GPU_AO_RATE_RULES_HLSLI
#define MCLA_GPU_AO_RATE_RULES_HLSLI

// Shared between the trace, filter and standalone C++ tests.
static const uint kAoAgeRadix = 64u;
static const uint kAoScreenMature = 8u;
static const uint kAoRtMature = 4u;
static const float kAoLowVariance = 0.008f;
static const float kAoProbeVariance = 0.025f;

inline uint AoSamplePhase(uint frame) { return frame & 3u; }
inline uint AoSourceAxis(uint texel, uint phase_bit, uint last) {
  const uint source = texel * 2u + phase_bit;
  return source < last ? source : last;
}
inline uint AoGatherIndex(uint x, uint y) { return y == 0u ? 3u - x : x; }
inline uint AoHistoryAge(float state) { return uint(state) % kAoAgeRadix; }
inline uint AoHistoryId(float state) { return uint(state) / kAoAgeRadix; }
inline uint AoHistoryState(uint age, uint id) { return age + id * kAoAgeRadix; }

inline uint AoNextAge(float history_age, uint cap) {
  const uint age = uint(history_age + 1.5f);
  return age < cap ? age : cap;
}

inline bool AoRateSurface(float depth, float old_depth) {
  const float delta = depth - old_depth;
  const float limit = 0.015f + 0.008f * depth;
  return delta >= -limit && delta <= limit;
}

inline uint AoScreenSlices(uint age, float variance, float noise) {
  return age >= kAoScreenMature && variance <= kAoLowVariance && noise >= (1.0f / 6.0f) ? 2u : 3u;
}

inline float AoRayProbability(uint age, float variance) {
  if (age < kAoRtMature || variance > kAoProbeVariance)
    return 1.0f;
  return variance <= kAoLowVariance ? 0.25f : 0.5f;
}

#endif
