#ifndef MCLA_GPU_REFLHIT_HLSLI
#define MCLA_GPU_REFLHIT_HLSLI

// The low two bytes of the R32_UINT target go to the VISMET writer.
static const uint kReflHitNone = 0u;
static const uint kReflHitSsrVehicle = 1u;
static const uint kReflHitSsrBuilding = 2u;
static const uint kReflHitSsrRoad = 3u;
static const uint kReflHitSsrSky = 4u;
static const uint kReflHitSsrEmissive = 5u;
static const uint kReflHitSsrOther = 6u;
static const uint kReflHitPlanar = 7u;
static const uint kReflHitEnvironment = 8u;
static const uint kReflHitRtCity = 16u;  // add the city material class (0..15)
static const uint kReflHitRtCar = 32u;
static const uint kReflHitRtMiss = 33u;
static const uint kReflHitRtUnresolved = 34u;
static const uint kReflHitRtCarShaded = 35u;
static const uint kReflHitReuse = 36u;

uint ReflHitScreen(uint cls, uint flags, bool vehicle) {
  if (vehicle || (flags & GB_FLAG_VEHICLE) != 0u || cls == GB_CLASS_PAINT) return kReflHitSsrVehicle;
  if (cls == GB_CLASS_INTERIOR) return kReflHitSsrBuilding;
  if (cls == GB_CLASS_ROAD || cls == GB_CLASS_WET_ROAD) return kReflHitSsrRoad;
  if (cls == GB_CLASS_EMISSIVE) return kReflHitSsrEmissive;
  return kReflHitSsrOther;
}

uint ReflHitFallback(uint cls, uint flags, bool planar) {
  if ((flags & GB_FLAG_VEHICLE) != 0u) return kReflHitNone;
  if (cls == GB_CLASS_ROAD || cls == GB_CLASS_WET_ROAD) return planar ? kReflHitPlanar : kReflHitNone;
  if ((flags & GB_FLAG_SSR) == 0u) return kReflHitNone;
  if (planar && (cls == GB_CLASS_GLASS || cls == GB_CLASS_WATER)) return kReflHitPlanar;
  return kReflHitEnvironment;
}

uint ReflHitCity(uint cls) { return kReflHitRtCity + (cls < 16u ? cls : 0u); }

uint ReflHitDominant(uint screen, float screenWeight, uint rt, float rtWeight, uint fallback) {
  const float fallbackWeight = fallback != kReflHitNone ? 1.0f - screenWeight - rtWeight : 0.0f;
  if (rtWeight > screenWeight && rtWeight >= fallbackWeight) return rt;
  if (screenWeight > 0.0f && screenWeight >= fallbackWeight) return screen;
  return fallback;
}

uint ReflHitPack(uint visible, uint query, uint screen, uint fallback) {
  return visible | (query << 8u) | (screen << 16u) | (fallback << 24u);
}
uint ReflHitScreenId(uint packed) { return (packed >> 16u) & 255u; }
uint ReflHitFallbackId(uint packed) { return (packed >> 24u) & 15u; }
float ReflHitScreenFraction(uint packed) { return float(packed >> 28u) / 15.0f; }
uint ReflHitPackHandoff(uint visible, uint query, uint screen, uint fallback, float screen_fraction) {
  return ReflHitPack(visible, query, screen, fallback) | (uint(round(saturate(screen_fraction) * 15.0f)) << 28u);
}

uint ReflHitHandoffDominant(uint screen, float screen_weight, float reuse_weight, uint rt, float rt_weight, uint fallback) {
  const float fallback_weight = fallback == kReflHitNone ? 0.0f : 1.0f - screen_weight - reuse_weight - rt_weight;
  const float best_screen_weight = max(screen_weight, reuse_weight);
  if (rt_weight > best_screen_weight && rt_weight >= fallback_weight) return rt;
  if (best_screen_weight > 0.0f && best_screen_weight >= fallback_weight) {
    return reuse_weight > screen_weight ? kReflHitReuse : screen;
  }
  return fallback;
}

#endif
