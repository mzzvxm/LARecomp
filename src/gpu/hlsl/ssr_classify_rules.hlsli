#ifndef MCLA_GPU_SSR_CLASSIFY_RULES_HLSLI
#define MCLA_GPU_SSR_CLASSIFY_RULES_HLSLI

static const uint kSsrTileSize = 8u;
static const uint kSsrTileYoung = 4u;
static const uint kSsrTileStable = 8u;
static const uint kSsrTileScreen = 0u;
static const uint kSsrTileMixed = 1u;
static const uint kSsrTileWorld = 2u;
// Both values still decode as distant hits for the reflection resolve.
static const float kSsrScreenSkyDistance = 4100.0f;
static const float kSsrSkippedDistance = 4104.0f;

uint SsrTileState(uint age, uint mode) { return (age << 2u) | mode; }
uint SsrTileAge(uint state) { return state >> 2u; }
uint SsrTileClass(uint state) { return state & 3u; }

float SsrTileMiss(float hits, float misses) {
  return hits + misses > 0.0f ? misses / (hits + misses) : 0.5f;
}

bool SsrTileProbe(uint frame, uint phase) { return ((frame + phase) & 3u) == 0u; }

// A safe band on both sides of the mixed class absorbs small changes in
// screen coverage. The unknown class starts mixed and gets full probing.
uint SsrTileMode(uint previous, float miss) {
  if (previous == kSsrTileScreen && miss <= 0.20f) return kSsrTileScreen;
  if (previous == kSsrTileWorld && miss >= 0.60f) return kSsrTileWorld;
  if (miss < 0.10f) return kSsrTileScreen;
  if (miss > 0.75f) return kSsrTileWorld;
  return kSsrTileMixed;
}

float SsrTileFeedback(float current, float previous, uint age) {
  return age == 0u ? current : previous * 0.75f + current * 0.25f;
}

uint SsrTileRays(uint maximum, uint age, float miss, float variance,
                 float smoothness, bool can_skip, bool probe) {
  if (smoothness >= 0.92f) return maximum;
  if (can_skip && age >= kSsrTileStable && miss >= 0.98f &&
      variance < 0.005f && smoothness < 0.66f && !probe) return 0u;
  return age < kSsrTileYoung || variance >= 0.025f ? maximum : 1u;
}

uint SsrTileIterations(uint maximum, float smoothness) {
  if (smoothness >= 0.88f) return maximum;
  return smoothness >= 0.68f ? (maximum * 3u + 3u) / 4u : (maximum + 1u) / 2u;
}

bool SsrTileKeepRt(uint mode, uint age, float variance, float confidence, bool probe) {
  // Preserve the deep holes and reacquire quickly on disocclusions.
  return age < kSsrTileYoung || variance >= 0.025f || mode == kSsrTileWorld ||
         confidence < 0.45f || probe;
}

float SsrTileRtProbeShare(uint mode) { return mode == kSsrTileScreen ? 0.25f : 0.5f; }

#endif
