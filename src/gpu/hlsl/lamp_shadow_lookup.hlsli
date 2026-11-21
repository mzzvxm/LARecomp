// MCLA GPU renderer -- a street lamp's shadow lookup in HLSL: THE TWIN of src/gpu/lamp_shadow_lookup.h (perf/LAMP-SHADOWS-2.md, package LAMP-SHADOWS-4).
//
// The ONE lookup (hlsl/light_shadow_lookup.hlsli) over the street lamps' table, plus the edge fade: the lamp's shadow map ends at its reach (14 to 30 m), so the visibility is faded to 1 over the
// last fifth of the reach by the distance from the lamp (an edge at the far plane would show as a line across the road). A lamp's light names its entries in the pass's COMBINED entry space
// (GpuLight packed1's first view = LSL_LAMP_ENTRY_BASE + the row of the lamps' table): the including shader raises LSL_MAX_VIEWS to cover both tables and defines the seams (LslLoadView, LslLayerOf)
// to read the headlights' table below the base and the lamps' table from it. The reach is the bits of a float in the entry's `light_index` (lamp_shadow_frame.h LampReach).
//
// src/gpu/lamp_shadows_test.cpp compiles THIS FILE as C++ through a shim and compares it with the reference on random lamps, so the text keeps to the subset both languages read: scalars, floor /
// min / max / lerp / sqrt / saturate / asfloat only, no file-scope constants (every number is a #define). THE TEXT must not use the names of the game's registers: every name here starts with Lsl or LSL_.

#ifndef LARECOMP_LAMP_SHADOW_LOOKUP_HLSLI
#define LARECOMP_LAMP_SHADOW_LOOKUP_HLSLI

#include "light_shadow_lookup.hlsli"

#define LSL_LAMP_ENTRY_BASE 8      // = kLampEntryBase (lamp_shadow_frame.h)
#define LSL_LAMP_FADE_FROM 0.8f    // = kLampFadeFrom

// 1 inside LSL_LAMP_FADE_FROM x reach of the lamp, 0 at the reach and beyond, a smoothstep between (a reach of 0, not known, fades nothing).
float LslLampEdge(float distance, float reach) {
  if (!(reach > 0.0f)) {
    return 1.0f;
  }
  float a = LSL_LAMP_FADE_FROM * reach;
  float t = saturate((distance - a) / (reach - a));
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// One lamp's visibility at the world point (px, py, pz): `first_view` is the combined entry number. A light that is not an atlas light, has no views or names an entry of the headlights' table is lit.
float LslLampLit(uint shadow_kind, uint first_view, uint view_count, float weight, float lx, float ly, float lz, float px, float py, float pz, uint slot, uint mode) {
  if (shadow_kind != LSL_SHADOW_ATLAS || view_count == 0u || first_view < uint(LSL_LAMP_ENTRY_BASE)) {
    return 1.0f;
  }
  float mean = LslLightMean(shadow_kind, first_view, view_count, px, py, pz, slot, mode);
  float reach = asfloat(LslLoadView(first_view).light_index);
  float dx = px - lx;
  float dy = py - ly;
  float dz = pz - lz;
  float edge = LslLampEdge(sqrt(dx * dx + dy * dy + dz * dz), reach);
  if (edge < 1.0f) {
    mean = 1.0f + (mean - 1.0f) * edge;
  }
  return LslBlend(mean, weight);
}

#endif  // LARECOMP_LAMP_SHADOW_LOOKUP_HLSLI
