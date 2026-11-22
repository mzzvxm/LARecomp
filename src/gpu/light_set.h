#pragma once
// ===========================================================================
// MCLA GPU renderer -- the light set of the GB-SPIKE interface: now a thin include of the one light description (perf/LIGHT-UNIFY.md 2.1)
// ===========================================================================
// light_set.h was the frozen interface of the deferred light pass (package IF of perf/GBUFFER-SURVEY.md 7.2): a `Light` (pos, range, dir, cos_outer, color, cos_inner, kind, source,
// car_key, shadow_view, flags) and a `LightSet` of 32 that nothing produced; only `LightFromHeadlight` had a user (a test). LIGHT-UNIFY replaced it with light_frame.h: the same
// fields and the same units (world space, y up; colour already times the strength, the fade and the night factor; a point light has both cosines -1), grown by the law, the role
// mask, the id, the velocity, the fade state and a ShadowRef in place of `shadow_view`, in a LightFrame of kMaxLights (64) with a 64 byte GpuLight twin.
//
// What a caller of this file gets: the whole of light_frame.h (Light, LightKind, LightSource, kLightFlag*, kMaxLights, LightFrame, GpuLight ...), light_laws.h, light_id.h and the
// conversions of light_convert.h, among them `LightFromHeadlight(h)` (the same call as before, now also setting the law, the id and the role mask). `LightSet` and
// `LightSetFromHeadlights` are gone: a set is a LightFrame, made by the LightManager (LU-2); and `shadow_view` is `Light::shadow` (a ShadowRef into the frame's ShadowFrame).
// ===========================================================================

#include "light_convert.h"
