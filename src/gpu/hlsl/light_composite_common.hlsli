#ifndef LARECOMP_LIGHT_COMPOSITE_COMMON
#define LARECOMP_LIGHT_COMPOSITE_COMMON

float LcTapWeight(float depth, float guide, bool vehicle, float bilinear) {
  if (guide == 0.0f || (guide < 0.0f) != vehicle) return 0.0f;
  return bilinear * saturate(1.0f - abs(abs(guide) - depth) / max(0.20f, depth * 0.018f));
}

#endif
