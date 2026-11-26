#ifndef MCLA_SSGI_SECTOR_HLSLI
#define MCLA_SSGI_SECTOR_HLSLI

// Expand cos(2 * theta - n) from the horizon cosine already computed by
// the march. At a clamped tangent, cos(2 * t - n) is exactly -cos(n).
// CHECK: compare the few samples adjacent to a sector rounding boundary in
// the moving neon and headlight captures when using the approximate acos.
float SsgiFastSectorIntegral(float c, float side, float theta, float n,
                             float sin_n, float cos_n, float half_pi, float t) {
  float cos_arc = -cos_n;
  if (abs(theta - n) < half_pi) {
    c = clamp(c, -1.0f, 1.0f);
    const float s = side * sqrt(max(0.0f, 1.0f - c * c));
    cos_arc = (2.0f * c * c - 1.0f) * cos_n + (2.0f * c * s) * sin_n;
  }
  return (cos_n - cos_arc + 2.0f * t * sin_n) * 0.25f;
}

#endif
