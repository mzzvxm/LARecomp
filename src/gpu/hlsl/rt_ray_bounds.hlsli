#ifndef MCLA_GPU_RT_RAY_BOUNDS_HLSLI
#define MCLA_GPU_RT_RAY_BOUNDS_HLSLI

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define RT_MIN std::min
#define RT_MAX std::max
#define RT_ABS std::abs
#define RT_INOUT float&
#else
#define RT_MIN min
#define RT_MAX max
#define RT_ABS abs
#define RT_INOUT inout float
#endif

// Allow for different CPU/GPU rounding of the same instance transform.
inline bool RtClipAxis(float lo, float hi, float origin, float direction,
                       RT_INOUT first, RT_INOUT last) {
  const float padding = RT_MAX(0.05f, 2e-6f * RT_MAX(RT_ABS(lo), RT_ABS(hi)));
  lo -= padding;
  hi += padding;
  if (RT_ABS(direction) < 1e-8f) {
    // A component this small may still reach a face. Leave that axis alone.
    return true;
  }
  const float a = (lo - origin) / direction;
  const float b = (hi - origin) / direction;
  first = RT_MAX(first, RT_MIN(a, b));
  last = RT_MIN(last, RT_MAX(a, b));
  return last >= first;
}

#ifndef __cplusplus
// Negative means the ray cannot reach any instance. Keep TMin and the
// consumer's original fade/diagnostic reach so hit distance stays absolute.
float RtClippedLength(float3 lo, float3 hi, float3 origin, float3 direction, float reach) {
  const float3 end = origin + reach * direction;
  if (all(lo <= min(origin, end)) && all(max(origin, end) <= hi)) return reach;
  float first = 0.0f;
  float last = reach;
  if (!RtClipAxis(lo.x, hi.x, origin.x, direction.x, first, last) ||
      !RtClipAxis(lo.y, hi.y, origin.y, direction.y, first, last) ||
      !RtClipAxis(lo.z, hi.z, origin.z, direction.z, first, last)) {
    return -1.0f;
  }
  return last;
}
#endif

#undef RT_MIN
#undef RT_MAX
#undef RT_ABS
#undef RT_INOUT
#endif
