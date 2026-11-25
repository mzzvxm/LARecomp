// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections (CITY-RT package R4; src/gpu/ssr_rt_rules.h is the C++ twin and the design, src/gpu/ssr_rt.h the host): the pure functions
// the three parts share, and the words of the compute passes' push block and buffers.
//
// THE HYBRID IN ONE PAGE. The trace (hlsl/ssr_trace_ps.hlsl compiled with SSR_RT 1) writes a second target, its CONFIDENCE: r the class group of a pixel it traced (0: nothing was traced, no ray tracing
// wanted here), g how much of the pixel's reflection is the picture's (0 left the picture, hit nothing, or points back at the camera .. 1 a hit in full; the edge and facing fades are in it). A compute
// pass over the candidates (a pixel whose group the level takes and whose confidence is under the threshold) regenerates the SAME GGX ray (SsrSampleRay: nothing is stored), traces it into the city's top
// level and shades the hit with the city hit library (ResolveHit), and writes the result into the trace image: where the picture's share is c the term becomes
//     trace + (1 - c) * weight * (the ray traced radiance - the road view the trace takes out of a road)
// (the shape of lerp(rt, ssr, c): the trace's own value is already c times the picture's radiance minus its share of the road view). The temporal and the apply passes run unchanged on the result.
//
// The candidates are listed tile by tile (hlsl/ssr_rt_select_cs.hlsl: a group of 8 x 8 pixels reserves a range of the list for its candidates, so a ray's neighbours in a wave are its neighbours in the
// picture: coherent) and capped: past the level's cap a stable thinning keeps a share of them and the survivors weigh by the inverse of the share (an unbiased estimate; the temporal pass averages the
// noise). The list's length of the last frame sets the share, on the GPU (three rotating counters, nothing is read back for it).
//
// The text keeps to a subset both languages read (src/gpu/ssr_rt_rules_test.cpp compiles THIS FILE as C++ through a shim, after ssr_common.hlsli): scalar arithmetic, no out parameters.

#ifndef MCLA_GPU_SSR_RT_COMMON_HLSLI
#define MCLA_GPU_SSR_RT_COMMON_HLSLI

#include "ssr_common.hlsli"
#include "blue_noise_frame.hlsli"

// ---- the compute passes' push block (the SSR push block's 32 words, ssr_common.hlsli SSR_W_*; the words the compute passes do not read for their first meaning) ------------------------------------
#define SSR_W_RT_CONF 4          // the confidence image's bindless slot (low 16 bits) | the class groups the level traces << 16 (4 bits) | rays - 1 << 20 (2 bits) | SSR_RT_FLAG_* << 24
#define SSR_W_RT_CAP 5           // the list's capacity in pixels (the level's ray cap over its rays a pixel, the governor's step in it)
#define SSR_W_RT_REACH 18        // float: the longest ray traced, metres
#define SSR_W_RT_THRESHOLD 19    // float: a pixel whose confidence is under this is a candidate
#define SSR_W_RT_FRAME 30        // the fallback's own frame count (the rotation of the counters, the tile order, the thinning)
#define SSR_RT_CONF_MASK_SHIFT 16u
#define SSR_RT_RAYS_SHIFT 20u
#define SSR_RT_FLAGS_SHIFT 24u
#define SSR_RT_FLAG_STATS 1u     // a stats frame: the calibration's and the census's counters are added
#define SSR_RT_FLAG_RESET 2u     // the first frame: the select pass only clears the control words
#define SSR_RT_FLAG_SCREEN 4u    // the picture's colour may be taken where a hit is seen (CITY_HIT_P_SCREEN)

// ---- the control buffer (u1 of the output set: words) -------------------------------------------------------------------------------------------------------------------------------------------
#define SSR_RT_CTL_TOTAL 0u      // 3 words, a frame each in rotation: the candidates counted BEFORE the thinning
#define SSR_RT_CTL_LISTED 3u     // 3 words: the candidates listed
#define SSR_RT_CTL_WORDS 8u      // (two spare)
#define SSR_RT_STAT_CAL 8u       // 48 words: the library's calibration counters (rt_hit_calibration.h kStatsWords), never cleared (the host takes differences)
#define SSR_RT_STAT_CENSUS 56u   // 16 words: the census's counters (ssr_rt_rules.h Stat*), never cleared
#define SSR_RT_STAT_WORDS 72u
#define SSR_RT_CENSUS_RAYS 0u
#define SSR_RT_CENSUS_CITY 1u
#define SSR_RT_CENSUS_CAR 2u
#define SSR_RT_CENSUS_MISS 3u
#define SSR_RT_CENSUS_NO_DATA 4u
#define SSR_RT_CENSUS_SCREEN 5u
#define SSR_RT_CENSUS_SHADED 6u
#define SSR_RT_CENSUS_FILLED 7u

// ---- numbers ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static const uint kSsrRtGroupNone = 0u;       // nothing was traced, or a class no level takes
static const uint kSsrRtGroupBright = 1u;     // glass, metal, water: what a window, a rail or a puddle shows
static const uint kSsrRtGroupGround = 2u;     // road, wet road, sidewalk and the tiles the table flags (tunnels): the wet street at night
static const float kSsrRtMinRamp = 0.1f;      // a pixel whose smoothness ramp (SsrSmoothRamp) is under this reflects too little to be worth a ray
static const float kSsrRtMinKeep = 0.25f;     // the thinning keeps at least this share of the candidates (so a survivor weighs at most 4)
static const uint kSsrRtTileStride = 7919u;   // the tile order is turned by this many tiles a frame (a prime): the cap is not always spent on the same corner
static const float kSsrRtFadeStart = 0.75f;   // a hit fades out from this share of the reach to nothing at the reach
static const float kSsrRtLiftFloor = 0.006f;  // the ray's origin is lifted off the depth's surface as the city's sun rays are (city_rt_shadow_rules.h Lift)
static const float kSsrRtLiftPixels = 3.0f;
static const float kSsrRtLiftCap = 0.4f;

// ---- the class group and the confidence target -----------------------------------------------------------------------------------------------------------------------------------
uint SsrRtGroupOf(uint cls) {
  if (cls == uint(GB_CLASS_GLASS) || cls == uint(GB_CLASS_METAL) || cls == uint(GB_CLASS_WATER)) {
    return kSsrRtGroupBright;
  }
  if (cls == uint(GB_CLASS_ROAD) || cls == uint(GB_CLASS_WET_ROAD) || cls == uint(GB_CLASS_SIDEWALK) || cls == uint(GB_CLASS_WORLD)) {
    return kSsrRtGroupGround;
  }
  return kSsrRtGroupNone;
}

// The confidence target's two channels (R8G8_UNORM): the group over three in r, the confidence in g. A group of 0 is no candidate whatever g says.
float SsrRtGroupOut(uint group) { return float(group) * (1.0f / 3.0f); }
uint SsrRtGroupIn(float r) { return uint(r * 3.0f + 0.5f); }

// Whether a pixel of the confidence target is a candidate: its group is one the level traces (`group_mask` has bit `group`) and the picture speaks for less than `threshold` of it.
bool SsrRtCandidate(uint group, float confidence, uint group_mask, float threshold) {
  return group != 0u && ((group_mask >> group) & 1u) != 0u && confidence < threshold;
}

// ---- the cap ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The share of the candidates the thinning keeps: the list's capacity over the candidates counted in the last frame (the first frame, with none counted, keeps all), never under kSsrRtMinKeep.
float SsrRtKeep(uint capacity, uint previous_total) {
  const float share = float(capacity) / float(max(previous_total, 1u));
  return clamp(share, kSsrRtMinKeep, 1.0f);
}
// A number in 0..1 per pixel and frame (a pixel is kept when it is under the share): the blue noise tile at the pixel stepped along the R2 sequence by the frame (blue_noise_frame.hlsli), so the pixels
// kept in a frame are a blue noise stipple of the density kept, with no lattice, a pixel is kept in the share of the frames kept (low discrepancy: evenly spread in time), and the pattern is the
// same stipple moved round the circle, so it does not slide. The first version (`legacy`, SSR_FRAME_LEGACY_NOISE of the frame word: mcla_gpu_ssr_blue_noise false) was interleaved gradient noise
// read at a position moved by (7.7, 3.1) pixels a frame: a lattice that slid across the picture, and the pixels traced with it.
float SsrRtThinHash(int px, int py, uint rt_frame, bool legacy) {
  if (legacy) {
    const float f = float(rt_frame & 255u);
    return SsrIgn(float(py) + 3.1f * f + 71.0f, float(px) + 7.7f * f + 13.0f);
  }
  return BlueNoiseFrame(px, py, 0u, rt_frame, kBlueNoiseR2StepA);
}
bool SsrRtKept(float hash, float keep) { return hash < keep; }
// What a survivor of the thinning weighs: the inverse of the share kept.
float SsrRtWeight(float keep) { return 1.0f / max(keep, kSsrRtMinKeep); }
// The tile (in the picture's tile order) a group of the dispatch handles: the order is turned by a frame's stride (the frame reduced to the tile count first, so the product stays under 2^32 for any picture
// up to half a million tiles and the order is a permutation of the tiles in every frame, however long the session).
uint SsrRtTileOf(uint group_x, uint group_y, uint tiles_x, uint tile_count, uint rt_frame) {
  const uint count = max(tile_count, 1u);
  return (group_y * tiles_x + group_x + (rt_frame % count) * kSsrRtTileStride) % count;
}

// ---- the ray -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The ray's origin is lifted off the depth's surface along its normal: a few millimetres and three pixel footprints (what the depth can be wrong by across a pixel), at most kSsrRtLiftCap.
// `footprint_unit` is 2 tan(half fov y) over the depth image's height: a pixel at distance t is t * footprint_unit metres across.
float SsrRtLift(float t, float footprint_unit) {
  return min(kSsrRtLiftFloor + kSsrRtLiftPixels * footprint_unit * t, kSsrRtLiftCap);
}
// A hit's weight by its distance: 1 up to kSsrRtFadeStart of the reach, smoothly to 0 at the reach (a hit at the end of the structures is not a hard edge).
float SsrRtReachFade(float t, float reach) {
  const float start = kSsrRtFadeStart * reach;
  const float s = clamp((t - start) / max(reach - start, 1.0e-6f), 0.0f, 1.0f);
  return 1.0f - s * s * (3.0f - 2.0f * s);
}
// Ground rays fade in only when the hit has strong support from the rasterized view.
float SsrRtHitCoverage(bool ground, float screen) {
  if (!ground) return 1.0f;
  const float t = saturate(2.0f * screen - 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
// The ray's footprint at its hit (the cone's width, metres): the pixel's angular size over the UNFOLDED path (the receiver's distance from the camera plus the ray's length: the virtual image) plus
// the lobe's two half widths.
float SsrRtFootprint(float pixel_angle, float receiver_distance, float t, float cone_tan) {
  return pixel_angle * (receiver_distance + t) + 2.0f * cone_tan * t;
}

// ---- the result --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The term the trace image holds after the fallback: its own value plus the share the picture could not speak for (1 - confidence) of the ray traced term, `weight` the thinning's.
float3 SsrRtBlend(float3 trace, float confidence, float3 term, float weight) {
  const float share = (1.0f - saturate(confidence)) * weight;
  return trace + term * share;
}

#endif  // MCLA_GPU_SSR_RT_COMMON_HLSLI
