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
uint ReflHitScreenId(uint packed) { return (packed >> 16u) & 63u; }
uint ReflHitFallbackId(uint packed) { return (packed >> 24u) & 15u; }

// The six unused scratch bits hold screen confidence. The two exported ID bytes stay unchanged.
uint ReflHitPackConfidence(uint confidence) { return ((confidence & 3u) << 22u) | ((confidence & 60u) << 26u); }
uint ReflHitConfidenceBits(uint packed) { return packed & ReflHitPackConfidence(63u); }
uint ReflHitConfidence(uint packed) { return ((packed >> 22u) & 3u) | ((packed >> 26u) & 60u); }

// All inputs have been checked for finite values by the caller.
float RoadReflectionLobe(float correction, float planar, float strength, float cap) {
  const float radiance = correction + (planar < cap ? planar : cap) * strength;
  return radiance > 0.0f ? radiance : 0.0f;
}

#endif
