#ifndef MCLA_RT_DENOISE_V2_MATH_HLSLI
#define MCLA_RT_DENOISE_V2_MATH_HLSLI

float3 Dn2YCoCg(float3 rgb) {
  return float3(0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b,
                0.5f * (rgb.r - rgb.b), 0.5f * rgb.g - 0.25f * (rgb.r + rgb.b));
}

float3 Dn2Rgb(float3 yc) { return float3(yc.x + yc.y - yc.z, yc.x + yc.z, yc.x - yc.y - yc.z); }

float Dn2Variance(float history, float current, float differenceSquared, float historyWeight) {
  return lerp(current, history, historyWeight) + historyWeight * (1.0f - historyWeight) * differenceSquared;
}

float Dn2Radius(float base, float samples, float relativeNoise) {
  const float shrink = lerp(1.0f, 0.48f, saturate((samples - 1.0f) / 15.0f));
  return base * max(shrink, saturate(relativeNoise));
}

bool Dn2AcceptGeometry(float depth, float expected, float plane, float normalDot) {
  return abs(depth - expected) <= 0.025f * expected + 0.03f &&
         plane <= 0.012f * expected + 0.02f && normalDot >= 0.83f;
}

float Dn2FireflyCeiling(float mean, float deviation, float maximum) {
  return max(2.0f * maximum, mean + 4.0f * deviation) + max(0.025f, 0.12f * mean);
}

#endif
