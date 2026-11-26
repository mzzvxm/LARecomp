#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's frame interface: THE FROZEN INTERFACE (package G0 of perf/CITY-RT-M3.md, sections 3, 4, 6, 7)
// ===========================================================================
// CITY-RT M3 is global illumination with the real city: a world space field of irradiance probes (cascades round the camera, toroidal addressing, temporal
// accumulation in the probes) that REPLACES the colour chain as the SOURCE of the bounce light on ray tracing cards (the screen-space bounce light, ssgi_light.h, stays for
// cards without ray tracing and as the fallback where the field is not valid). This header is what the packages of M3 and every reader of the field agree on:
//
//   G1  the probe field's owner: makes the three textures below, keeps the cascades' origins (toroidal addressing, dirty marking), counts the memory, publishes the GiFrame
//   G2  the probe update: the ray set, the hit shading in GI mode, the blend with adaptive hysteresis (the integration kernel is IntegrateTexel below), the scheduler
//   G3  the apply: GiLookup, the ground baseline, albedo x E_net, the bounce light's cap / ceiling / vehicle rules (ssgi_light.h), the AO's multiply, the upsample
//   G4  the integration and the settings: SSGI skip / fallback / crossfade (SsgiShare), mcla_gpu_rt_gi, the row, the launcher
//   G5  the per-pixel detail pass (ULTRA, strong ray tracing cards): reads the same field as its second bounce
//   M2  the reflections' hit shading: AmbientAt(P, N) (below; the library's GI mode reads GiLookup)
//
// THIS FILE, hlsl/gi_common.hlsli (the decode, in HLSL) and gi_frame_test.cpp (which compiles the .hlsli as C++ and checks it against this file) are THE INTERFACE. It is
// frozen: a package that needs another field or value says so in its report, it does not edit this file. WHAT MAY CHANGE without a new version: the tuning numbers marked
// `COST PASS` (budgets, estimates, intervals, the blend band) and values that are APPENDED (a consumer, a view mode, a product, a flag bit, a reserved row's field); everything
// else is a new kFormatVersion here and GI_FORMAT_VERSION in the .hlsli. The header includes nothing of Plume or ReXGlue.
//
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// WHAT THE FIELD IS (M3 section 3)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A cascade is a grid of probes on a LATTICE: the probe with integer coordinate c sits at the world point c x spacing (so the lattices of cascades whose spacings double are nested,
// and a probe never moves: only the window of coordinates that is stored moves). A cascade stores the probes of the window [origin, origin + dims) (origin = the integer coordinate
// of the window's minimum corner), at slot = c mod dims on each axis (TOROIDAL addressing): when the camera moves, the origin follows it in whole cells, and only the probes of the
// coordinates that entered the window are new (ExposedBoxes: at most three boxes), every other probe keeps its slot and its data. The world's y is up (as every other pass here).
//
// A probe holds, for each direction in an 8 x 8 octahedral map (OctEncode: the map is the G-buffer's, gbuffer_types.h), with a 1 texel BORDER so that a bilinear read at the map's
// edge never leaves the probe (the 10 x 10 TILE; OctBorderSource says which interior texel a border texel copies):
//   irradiance atlas   RGBA16F  rgb = the irradiance of what SURFACES send into that direction's cosine lobe (rays that miss add NOTHING: the sky is already in the game's ambient),
//                               a = the SKY VISIBILITY (the cosine weighted share of the lobe whose rays escaped), stored LINEAR
//   moments atlas      RG16F    x = the mean distance to the surface along that direction, y = the mean of its square (the Chebyshev visibility test), metres, clamped to the cascade's
//                               MaxDistanceFor(spacing); a ray that escapes counts as that distance
//   weight map         R8_UNORM one texel a probe: how far the probe may be trusted, 0 (inside geometry, never traced, outside the safe range) to 1 (converged). The lookup multiplies every
//                               probe's weight by it and reports the sum as the CONFIDENCE (below), so a field that is still being built fades into the fallback, never into garbage.
// UNITS: "scene colour units", the units the frame's colour constants are in (sun colour c39, ambient c26 / c57) and the deferred light pass's convention, with NO pi: the irradiance
// of a closed environment of uniform radiance L is L, so a surface of albedo A lit by it shows A x L, and `colour += albedo x E`. IntegrateTexel is the definition (E(w) =
// (4 / N) x sum of radiance_i x max(w . dir_i, 0) over N uniformly distributed rays).
//
// THE ATLASES (one texture each, all cascades in it): the irradiance and the moments atlas have the same size, the tile of probe slot (x, y, z) of cascade c is at texel
//   X = (x + dimsX x y) x 10,   Y = atlas_y(c) + z x 10           (TileOrigin), atlas_y(c) = the sum of dimsZ x 10 of the cascades before c
// so a High cascade (32 x 16 x 32) is 5120 x 320 texels, the atlas of three of them 5120 x 960 (13.1 MB of irradiance + 6.6 MB of moments a cascade: 19.7 MB; the weight map one byte a
// probe). The weight map has the probe's texel at (x + dimsX x y, weight_y(c) + z). ONE texture each (not an array, not 3D): one bindless slot, the widest side is 5120 (CHECK C7:
// RGBA16F and RG16F as storage images and as sampled textures on both backends; R11G11B10F for the irradiance if short of memory is a new kFormatVersion).
//
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// WHAT A READER GETS (the GiFrame, published at the trigger -- the mid-scene depth copy where the AO runs -- after the probe update, valid until the frame's end)
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The GiFrame (below) is host plain data: the cascades' table, the three textures' bindless slots (kNoSlot = not there: a product that is not in `valid` must not be read), the
// fade, the safe range, the strength and the frame's colours the baseline needs. MakeGpuFrame turns it into the 20 rows (320 bytes) the shaders read through GI_FRAME_ROW(i); every
// shader reads the field through hlsl/gi_common.hlsli (GiLookup, GiAmbientAt, GiGroundBaseline), never by its own address arithmetic. What each consumer reads (the C++ twin of every
// function is in this file):
//   kBounceApply       (G3, the AO's apply path; replaces the colour chain as the bounce light's SOURCE where the field is valid) GiLookup -> E_surf, sky visibility, confidence; then
//                      GiGroundBaseline and GiNetIrradiance (E_net = max(E_surf - E_base, 0)); gi = albedo x E_net x lit weight x strength x fade x AO visibility; then the bounce light's own
//                      rules (ssgi_light.h: SoftClip to the ceiling, the vehicle share, source rejection), which stay as the owner tuned them. The SSGI's share of a pixel is SsgiShare().
//   kAmbientOcclusion  (ao.cpp) reads ReplacesSsgi(): when true the colour chain's gather and capture are not built (the visibility pass keeps running: they share the samples); its
//                      visibility multiplies the bounce. Nothing else of the field.
//   kLightPass         (GB-H1, the deferred lamp lights) reads nothing of the field in v1: the lamps still light their own pixels (direct) and the field only carries their bounce. The
//                      contract is the ORDER: the field's bounce is added in the AO's apply, BEFORE the light pass, so neither the AO nor the bounce multiplies a lamp's direct light. A
//                      pass that wants the field later (ambient for pixels the lit weight excludes) reads GiLookup like any other.
//   kHitShading        (M2's library, GI mode and the reflections) GiAmbientAt(P, N): the field's irradiance + the sky ambient x the sky visibility where the field is valid, the flat
//                      hemisphere ambient where it is not, blended by confidence x fade. The PROBE UPDATE itself shades its hits with GiLookup (surfaces only: the sky is not in the
//                      field) times GiFrame::bounce_weight, never with AmbientAt.
//   kPixelPass         (G5, ULTRA) the field is its second bounce and its fallback; it adds the product kPixelIrradiance (its own denoised E, slot pixel_slot).
//   kVolumetrics       (G8, later) the froxel inject's ambient in-scatter term.
//   kDebugView         (mcla_gpu_rt_gi_view, View below).
//
// THE RULES THAT KEEP THE FIELD SAFE (M3 section 4.2): the field is ADDITIVE and never darkens; the ground baseline (what an unobstructed plane of albedo 0.2 would send) is
// subtracted so that an open field gains nothing; the owner's bounce light rules (cap, ceiling, vehicle share, source rejection, strength) apply to the result; the AO multiplies it.
//
// THE CLASS / RAY TRACING TIER CELLS (Level, Rung, Layout, Budget, Cost): which budget each (ray tracing tier, level) runs and what it is estimated to cost. They are FIRST GUESSES
// from M3 section 6 (every number marked `COST PASS`); the main session corrects a cell from the measured cost of the first cost run, and the tier thresholds (RT-TIER, feat/rt-tier)
// are set from the same run. The tier, the `auto` word and the gating by graphics class live in rt_tier.h (gpu_class::RtTier, RtRung, kRtEffects' kRtGi cell); this file keeps only
// what the field does at each, with a Rung of its own whose order is RtTier's (the glue casts it: static_assert it when the two branches meet).
//
// THREADING: render thread only (the types are plain data). Everything that decides something is a pure function here (no Plume, no device): gi_frame_test.cpp checks it all.
//
// CHECK: nothing here was run against the game; each of these is a line of the first runs or a picture (M3 section 10), or a number that is a first guess:
//   C1  the pack's triangle winding for the probe validity (a back face is only a signal if the winding is consistent): G2's, not this interface's
//   C5  how far the game's live lamp list reaches (the far cascades need the pack's lamp table): G2b's
//   C6  the open field's excess against the baseline (< 3% of the pixel's colour): the baseline's analytic form is GroundBaseline, its parity with an integrated open ground is tested
//       here with 2,048 rays: the field is within 1.0% of the ground's radiance on average and 3.4% at worst (the 8 x 8 octahedral map's bilinear error on a smooth function is
//       3.7% of its range at worst, 1.0% on average: measured), so E_net on open ground is a ~0.5% glow with texel-sized patches of up to ~3%; the 64 ray field's noise comes on
//       top and is the run's. G3 decides whether a small dead band on E_base (a few % of it) is wanted.
//   C7  RGBA16F and RG16F with storage usage and as sampled textures on both APIs, 5120 x 960 and 5120 x 1280 sizes; the weight map's R8 as a Load
//   C8  the energy parity against the screen-space bounce light at the same strength
//   C9  the frame rows' ambient (c26 up, c57 down) and the sun colour (c39) the baseline reads: GiFrame's colour fields are filled from the car hit's frame rows (rt-hit-shading)
//   C10 hysteresis and convergence at 144 FPS: the intervals are in frames, the publisher's fade in seconds
//   C12 MSAA 2x/4x and the upscaler's jitter against the lookup (the apply reads sample 0 and the traced size depth like the AO)
//   G0a the blend band (2 probes), the normal offset (0.1 of the spacing), the Chebyshev's cube and floors, the moments' lobe power and the range fade's share are first guesses: the leak
//       scene (view 6) and the canyon decide them; they are values (changeable without a version), their meaning is not
//   G0b the y offset of the cascades' centre (8 m under the camera) is M3 section 3.1's, kept for High and Ultra; the lean budget has its own (-2 m, band 1) because its 8 probes
//       tall window could not hold the camera in the full weight part otherwise (NestingChecks); on a hill the street may leave the finest cascade, and what is more than 0 to 2 m
//       above the camera at High is already the 4 m cascade's (the first runs)
//   G0c the weak tier's ULTRA cell (the standard budget without the per-pixel pass) and the Rung-to-RtTier glue are this interface's reading of "weak: probes only"; the owner's
//       first look and the cost run decide whether a weak card gets ULTRA at all
//   G0d the irradiance map is 8 x 8 (the design's): a smooth lobe is read back to 5.8% of its range at worst; a finer map (12 x 12 or 16 x 16 interior) is a value of kProbeInterior
//       and a new kFormatVersion; the memory scales with the tile's square (the High atlas 59 MB: 116 MB at 12 x 12, 191 MB at 16 x 16)
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::gi {

inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;  // a bindless slot that is not there (the same value as gbuffer::kNoSlot and screen::kNoSlot)

// ---------------------------------------------------------------------------
// the numbers
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaxCascades = 4;
inline constexpr uint32_t kProbeInterior = 8;                                 // the octahedral map's side
inline constexpr uint32_t kProbeBorder = 1;                                   // texels of border on every side
inline constexpr uint32_t kProbeTile = kProbeInterior + 2 * kProbeBorder;     // 10
inline constexpr uint32_t kProbeTexels = kProbeTile * kProbeTile;             // 100

enum class TextureFormat : uint8_t { kRgba16F, kRg16F, kR8Unorm };
constexpr uint32_t TexelBytes(TextureFormat format) {
  switch (format) {
    case TextureFormat::kRgba16F: return 8;
    case TextureFormat::kRg16F: return 4;
    case TextureFormat::kR8Unorm: return 1;
  }
  return 0;
}
inline constexpr TextureFormat kIrradianceFormat = TextureFormat::kRgba16F;
inline constexpr TextureFormat kMomentsFormat = TextureFormat::kRg16F;
inline constexpr TextureFormat kWeightFormat = TextureFormat::kR8Unorm;
// What a probe holds in the two atlases (1.2 KB: M3 section 3.1) and in the weight map.
inline constexpr uint32_t kProbeAtlasBytes = kProbeTexels * (TexelBytes(kIrradianceFormat) + TexelBytes(kMomentsFormat));
inline constexpr uint32_t kProbeBytes = kProbeAtlasBytes + TexelBytes(kWeightFormat);

// The units of the integration (IntegrateTexel): 4 / N x sum, for rays spread uniformly over the sphere (a closed environment of radiance L gives L).
inline constexpr float kRayScale = 4.0f;
// The distance moments are weighted by max(cos, 0) ^ this (DDGI's 50 is for a 16 x 16 map; an 8 x 8 map of 64 rays needs a wider lobe). G0a: tuned by the leak scene.
inline constexpr float kMomentsLobePower = 8.0f;
// A ray that escapes, or hits farther than this many spacings, counts as this distance in the moments (the largest corner distance of a cell is 1.73 spacings).
inline constexpr float kMomentsMaxSpacings = 3.0f;

// The lookup's rules (M3 section 4.1). G0a: values, tuned by the leak scene.
inline constexpr float kNormalOffsetSpacings = 0.1f;  // the lookup point is moved this far along the normal (the usual leak guard)
inline constexpr float kBackFaceFloor = 0.0001f;      // max(floor, (dot(dir to probe, N) + 1) / 2) squared
inline constexpr float kChebyshevFloor = 0.0001f;     // the cube of the Chebyshev weight is never under this
inline constexpr float kBlendBandProbes = 2.0f;       // a cascade fades out over its last this many probes (the next, coarser one takes over); a CascadeFrame holds it, this is the default
inline constexpr float kRangeFadeShare = 0.15f;       // the confidence falls to 0 over the last this share of the safe range
inline constexpr float kMinWeightSum = 1.0e-20f;      // under this sum of weights a cascade's lookup holds nothing
inline constexpr float kMinCover = 1.0e-6f;           // ... and the field's, over all cascades
inline constexpr float kRemainingStop = 1.0e-4f;      // the cascades are walked until this little of the pixel is left to the coarser ones

// The ground baseline (M3 section 4.2): the albedo of the unobstructed plane the game's ambient already stands for.
inline constexpr float kGroundAlbedo = 0.2f;

// The cascades' vertical centre: 8 m under the camera (M3 section 3.1: the street, the first floors and the roofs near the car are inside the finest cascade), the High and Ultra
// budgets' value; a budget carries its own (Budget::y_centre_offset_m). With 16 probes of 2 m the finest cascade's full weight part then ends 0 to 2 m above the camera (the
// window's top, less a cell and the 2 probe band): what is higher is blended into the 4 m cascade. G0b.
inline constexpr double kYCentreOffsetM = -8.0;

// The policy numbers that G2 and G4 share (they are in the design; owned by those packages, listed so that they are written once). COST PASS / G0a.
inline constexpr float kHysteresis = 0.97f;               // the probe blend's weight of the stored value (about 33 updates)
inline constexpr float kHysteresisJump = 0.85f;           // ... for a probe whose new result differs a lot (a lamp on, the sun's colour, a sector just built)
inline constexpr float kCutMetres = 200.0f;               // a camera step above this is a cut (the light picker's kCutMetres is the precedent)
inline constexpr uint32_t kCutWarmupFrames = 12;          // frames of the 4x budget after a cut
inline constexpr float kCutWarmupBudgetScale = 4.0f;
inline constexpr float kFadeInSeconds = 0.4f;             // the field's weight rises from 0 over this long after a cut
inline constexpr float kFadeOutSeconds = 0.5f;            // ... and the screen-space bounce light crossfades in over this long where the field goes invalid
inline constexpr float kBounceWeightDefault = 0.5f;       // the probes' own feedback (about two bounces); 0 = strictly one bounce

// ---------------------------------------------------------------------------
// the levels, the tiers, the layouts: THE CELLS OF THE TABLE
// ---------------------------------------------------------------------------

// What mcla_gpu_rt_gi resolves to (`auto` is resolved by rt_tier.h's kRtGi cell before this).
enum class Level : uint8_t { kOff, kHigh, kUltra };

constexpr std::string_view LevelName(Level level) { return level == Level::kOff ? "off" : level == Level::kHigh ? "high" : "ultra"; }

// "off", "high" or "ultra" (exactly); false for anything else, `auto` included.
constexpr bool ParseLevel(std::string_view text, Level* out) {
  if (text == "off") {
    *out = Level::kOff;
    return true;
  }
  if (text == "high") {
    *out = Level::kHigh;
    return true;
  }
  if (text == "ultra") {
    *out = Level::kUltra;
    return true;
  }
  return false;
}

// How well the card traces rays: the order and the meaning of gpu_class::RtTier (rt_tier.h on feat/rt-tier), and the same three words. The glue between the two is a cast.
enum class Rung : uint8_t { kNone, kWeak, kStrong };

constexpr std::string_view RungName(Rung rung) { return rung == Rung::kNone ? "none" : rung == Rung::kWeak ? "weak" : "strong"; }

// What `auto` stands for at each rung (a copy of rt_tier.h's kRtEffects cell of mcla_gpu_rt_gi, so that the two can be compared when the branches meet): the field on a strong card at
// HIGH; a weak card keeps the screen-space bounce light (+2.5 to 4 ms on an RTX 3060 at 1080p: M3 section 6) unless the player turns the field on by hand. COST PASS.
constexpr Level AutoLevel(Rung rung) { return rung == Rung::kStrong ? Level::kHigh : Level::kOff; }

// Which budget a field runs.
enum class Layout : uint8_t {
  kNone,      // no field: the screen-space bounce light is the source
  kLean,      // 2 cascades of 16 x 8 x 16 probes at 3 and 6 m, 32 rays: legacy two-cascade layout
  kStandard,  // 3 cascades of 32 x 16 x 32 probes at 2, 4 and 8 m, 64 rays: the High column
  kExtended,  // 4 cascades (the fourth at 16 m), 64 rays, refreshed twice as often, and the per-pixel detail pass: the Ultra column
};
inline constexpr uint32_t kLayoutCount = 4;

constexpr std::string_view LayoutName(Layout layout) {
  switch (layout) {
    case Layout::kNone: return "none";
    case Layout::kLean: return "lean";
    case Layout::kStandard: return "standard";
    case Layout::kExtended: return "extended";
  }
  return "?";
}

// High uses the standard field. Ultra adds the extended field and per-pixel detail.
constexpr Layout LayoutFor(Rung rung, Level level) {
  if (rung == Rung::kNone || level == Level::kOff) return Layout::kNone;
  return level == Level::kHigh ? Layout::kStandard : Layout::kExtended;
}

// What a layout does where the rung is the one that decides.
constexpr bool FieldOn(Layout layout) { return layout != Layout::kNone; }
constexpr bool PixelPassAllowed(Rung rung, Layout layout) { return rung != Rung::kNone && layout == Layout::kExtended; }
// The screen-space bounce light stops being the source (the field replaces it) exactly where a field runs (and is valid: GiFrame::ReplacesSsgi).
constexpr bool ReplacesSsgi(Layout layout) { return FieldOn(layout); }

// ---------------------------------------------------------------------------
// the cascades' table and the budgets
// ---------------------------------------------------------------------------

struct Dims {
  uint32_t x = 0, y = 0, z = 0;
  constexpr uint32_t Probes() const { return x * y * z; }
  constexpr bool operator==(const Dims& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Coord {
  int32_t x = 0, y = 0, z = 0;
  constexpr bool operator==(const Coord& o) const { return x == o.x && y == o.y && z == o.z; }
};

// One cascade of a budget (M3 sections 3.1, 3.2, 6).
struct CascadeSpec {
  Dims dims;
  float spacing_m = 0.0f;
  uint32_t rays_per_probe = 0;
  uint32_t interval_frames = 1;  // every probe is traced at least once in this many frames (round robin; the nearest ones are never older)
  float blend_band = kBlendBandProbes;  // probes over which the cascade fades into the next one at the edge of its window (CascadeFrame::blend_band); COST PASS
};

constexpr uint32_t ProbesPerFrame(const CascadeSpec& c) { return (c.dims.Probes() + c.interval_frames - 1) / c.interval_frames; }
constexpr uint32_t RaysPerFrame(const CascadeSpec& c) { return ProbesPerFrame(c) * c.rays_per_probe; }
constexpr float MaxDistanceFor(float spacing_m) { return kMomentsMaxSpacings * spacing_m; }
// What a cascade covers across, metres (x, y, z).
constexpr float SpanM(const CascadeSpec& c, uint32_t axis) { return float(axis == 0 ? c.dims.x : axis == 1 ? c.dims.y : c.dims.z) * c.spacing_m; }

// What a layout costs, estimated (M3 section 6): graphics card time on an RTX 5080 at 3840 x 2160 and on an RTX 3060 at 1920 x 1080 (0 = not recommended there), and the video
// memory the field holds (the atlases, the ray buffers, the images; the per-pixel pass's too at kExtended). COST PASS: every number is an estimate x2 uncertain.
struct Cost {
  float ms_5080_4k_lo = 0.0f, ms_5080_4k_hi = 0.0f;
  float ms_3060_1080p_lo = 0.0f, ms_3060_1080p_hi = 0.0f;
  uint32_t vram_mib = 0;
};

struct Budget {
  Layout layout = Layout::kNone;
  uint32_t cascade_count = 0;
  std::array<CascadeSpec, kMaxCascades> cascade = {};
  uint32_t pixel_rays_per_pixel = 0;  // the per-pixel detail pass: 0 off, 1 = one cosine ray a pixel at the traced size (half above 1080 lines)
  // The cascades' vertical centre, metres from the camera's height (negative: under it). The finest cascade's full weight part must reach above the camera whatever the snap
  // (gi_frame_test.cpp NestingChecks): its top is the window's less one cell and the blend band, so a short window needs a centre nearer the camera. G0b / COST PASS.
  double y_centre_offset_m = kYCentreOffsetM;
  Cost cost;
};

inline constexpr Budget kBudgets[kLayoutCount] = {
    // kNone
    {Layout::kNone, 0, {}, 0, kYCentreOffsetM, {}},
    // kLean: 2 x 2,048 probes; 12,288 rays a frame; +0.3 to 0.5 ms (5080, 4K), +0.8 to 1.3 ms (3060, 1080p); about 6 MiB
    // (8 probes tall: a band of 2 would leave 3 for the full weight part, so the band is 1 and the window sits 2 m under the camera)
    {Layout::kLean, 2, {{{{16, 8, 16}, 3.0f, 32, 8, 1.0f}, {{16, 8, 16}, 6.0f, 32, 16, 1.0f}}}, 0, -2.0, {0.3f, 0.5f, 0.8f, 1.3f, 6}},
    // kStandard: 3 x 16,384 probes; 458,752 rays a frame; +0.9 to 1.5 ms, +2.5 to 4 ms; about 75 MiB (59 of atlases)
    {Layout::kStandard, 3, {{{{32, 16, 32}, 2.0f, 64, 4}, {{32, 16, 32}, 4.0f, 64, 8}, {{32, 16, 32}, 8.0f, 64, 16}}}, 0, kYCentreOffsetM, {0.9f, 1.5f, 2.5f, 4.0f, 75}},
    // kExtended: 4 x 16,384 probes; 983,040 rays a frame + 1 ray a pixel; +2.4 to 4.0 ms (field 1.0 to 1.5, per-pixel trace 1.0 to 1.5, denoise 0.3 to 0.6), not recommended on the 3060; about 190 MiB
    {Layout::kExtended,
     4,
     {{{{32, 16, 32}, 2.0f, 64, 2}, {{32, 16, 32}, 4.0f, 64, 4}, {{32, 16, 32}, 8.0f, 64, 8}, {{32, 16, 32}, 16.0f, 64, 16}}},
     1,
     kYCentreOffsetM,
     {2.4f, 4.0f, 0.0f, 0.0f, 190}},
};

constexpr const Budget& BudgetOf(Layout layout) { return kBudgets[static_cast<uint32_t>(layout)]; }

constexpr uint32_t BudgetRaysPerFrame(const Budget& b) {
  uint32_t rays = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) rays += RaysPerFrame(b.cascade[c]);
  return rays;
}
constexpr uint32_t BudgetProbes(const Budget& b) {
  uint32_t probes = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) probes += b.cascade[c].dims.Probes();
  return probes;
}

// The atlases' size for a table of cascades (the layout above): the widest cascade's width, the cascades stacked.
constexpr uint32_t AtlasWidth(const Budget& b) {
  uint32_t width = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) width = std::max(width, b.cascade[c].dims.x * b.cascade[c].dims.y * kProbeTile);
  return width;
}
constexpr uint32_t AtlasHeight(const Budget& b) {
  uint32_t height = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) height += b.cascade[c].dims.z * kProbeTile;
  return height;
}
constexpr uint32_t AtlasRow(const Budget& b, uint32_t cascade) {  // atlas_y of a cascade
  uint32_t row = 0;
  for (uint32_t c = 0; c < cascade && c < b.cascade_count; ++c) row += b.cascade[c].dims.z * kProbeTile;
  return row;
}
constexpr uint32_t WeightWidth(const Budget& b) {
  uint32_t width = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) width = std::max(width, b.cascade[c].dims.x * b.cascade[c].dims.y);
  return width;
}
constexpr uint32_t WeightHeight(const Budget& b) {
  uint32_t height = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) height += b.cascade[c].dims.z;
  return height;
}
constexpr uint32_t WeightRow(const Budget& b, uint32_t cascade) {  // weight_y of a cascade
  uint32_t row = 0;
  for (uint32_t c = 0; c < cascade && c < b.cascade_count; ++c) row += b.cascade[c].dims.z;
  return row;
}
// The bytes the three textures hold for the table (the atlases are full rectangles: a narrower cascade leaves unused texels on its rows).
constexpr uint64_t AtlasBytes(const Budget& b) {
  return uint64_t(AtlasWidth(b)) * AtlasHeight(b) * (TexelBytes(kIrradianceFormat) + TexelBytes(kMomentsFormat)) + uint64_t(WeightWidth(b)) * WeightHeight(b) * TexelBytes(kWeightFormat);
}

// ---------------------------------------------------------------------------
// toroidal addressing
// ---------------------------------------------------------------------------

struct Slot {
  uint32_t x = 0, y = 0, z = 0;
  constexpr bool operator==(const Slot& o) const { return x == o.x && y == o.y && z == o.z; }
};

// The lattice cell of a world coordinate: floor(world / spacing).
inline int32_t CellOf(double world, float spacing_m) { return int32_t(std::floor(world / double(spacing_m))); }

// The window's minimum corner for a camera at `camera` (world metres): the cell under the camera, less half the window, the y centre `y_centre_offset_m` from the camera's height. Moves
// in whole cells, so the data stays where it is until a cell boundary is crossed.
inline Coord OriginFor(const double camera[3], const CascadeSpec& c, double y_centre_offset_m = kYCentreOffsetM) {
  return {CellOf(camera[0], c.spacing_m) - int32_t(c.dims.x / 2), CellOf(camera[1] + y_centre_offset_m, c.spacing_m) - int32_t(c.dims.y / 2),
          CellOf(camera[2], c.spacing_m) - int32_t(c.dims.z / 2)};
}

// c mod dim, never negative.
constexpr uint32_t Wrap(int32_t c, uint32_t dim) {
  const int32_t d = int32_t(dim);
  int32_t r = c % d;
  if (r < 0) r += d;
  return uint32_t(r);
}
constexpr Slot SlotOf(const Coord& c, const Dims& d) { return {Wrap(c.x, d.x), Wrap(c.y, d.y), Wrap(c.z, d.z)}; }
// The probe's number in a cascade (the order the scheduler and the validity buffers use): x fastest, then y, then z.
constexpr uint32_t ProbeIndex(const Slot& s, const Dims& d) { return (s.z * d.y + s.y) * d.x + s.x; }
constexpr bool Contains(const Coord& origin, const Dims& d, const Coord& c) {
  return c.x >= origin.x && c.x < origin.x + int32_t(d.x) && c.y >= origin.y && c.y < origin.y + int32_t(d.y) && c.z >= origin.z && c.z < origin.z + int32_t(d.z);
}
// The probe's world position, metres.
struct Position {
  double x = 0, y = 0, z = 0;
};
inline Position ProbeWorld(const Coord& c, float spacing_m) { return {double(c.x) * spacing_m, double(c.y) * spacing_m, double(c.z) * spacing_m}; }

// The texel of a probe's tile in the atlases (X, and Y without the cascade's atlas_y), and the probe's texel in the weight map (without weight_y).
struct Texel {
  uint32_t x = 0, y = 0;
  constexpr bool operator==(const Texel& o) const { return x == o.x && y == o.y; }
};
constexpr Texel TileOrigin(const Slot& s, const Dims& d) { return {(s.x + d.x * s.y) * kProbeTile, s.z * kProbeTile}; }
constexpr Texel WeightTexel(const Slot& s, const Dims& d) { return {s.x + d.x * s.y, s.z}; }

// A box of lattice coordinates, [lo, hi).
struct Box {
  Coord lo, hi;
  constexpr bool Empty() const { return hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z; }
  constexpr uint64_t Count() const { return Empty() ? 0 : uint64_t(hi.x - lo.x) * uint64_t(hi.y - lo.y) * uint64_t(hi.z - lo.z); }
  constexpr bool Has(const Coord& c) const { return c.x >= lo.x && c.x < hi.x && c.y >= lo.y && c.y < hi.y && c.z >= lo.z && c.z < hi.z; }
};

// The coordinates that are in the window at `new_origin` and were not in the one at `old_origin` (their probes are new: dirty, trace first, no history), as at most three boxes
// (written to out[0..], the number returned): one axis's slab at a time, so that no coordinate is in two. A move of a whole window or more on any axis (a cut) is the whole window.
inline uint32_t ExposedBoxes(const Coord& old_origin, const Coord& new_origin, const Dims& d, Box out[3]) {
  const int32_t dim[3] = {int32_t(d.x), int32_t(d.y), int32_t(d.z)};
  const int32_t o[3] = {old_origin.x, old_origin.y, old_origin.z};
  const int32_t n[3] = {new_origin.x, new_origin.y, new_origin.z};
  bool all = false;
  for (int a = 0; a < 3; ++a) {
    const int32_t delta = n[a] - o[a];
    if (delta >= dim[a] || -delta >= dim[a]) all = true;
  }
  if (all) {
    out[0] = {new_origin, {n[0] + dim[0], n[1] + dim[1], n[2] + dim[2]}};
    return out[0].Empty() ? 0 : 1;
  }
  // The overlap of the two windows on each axis, and the new-only slab of each.
  int32_t lo[3], hi[3], slab_lo[3], slab_hi[3];
  for (int a = 0; a < 3; ++a) {
    lo[a] = std::max(o[a], n[a]);
    hi[a] = std::min(o[a], n[a]) + dim[a];
    if (n[a] > o[a]) {  // moved up: the new coordinates are the top ones
      slab_lo[a] = o[a] + dim[a];
      slab_hi[a] = n[a] + dim[a];
    } else {            // moved down (or not at all: an empty slab)
      slab_lo[a] = n[a];
      slab_hi[a] = std::min(o[a], n[a] + dim[a]);
      if (n[a] == o[a]) slab_hi[a] = slab_lo[a];
    }
  }
  uint32_t count = 0;
  const Box x_box = {{slab_lo[0], n[1], n[2]}, {slab_hi[0], n[1] + dim[1], n[2] + dim[2]}};
  const Box y_box = {{lo[0], slab_lo[1], n[2]}, {hi[0], slab_hi[1], n[2] + dim[2]}};
  const Box z_box = {{lo[0], lo[1], slab_lo[2]}, {hi[0], hi[1], slab_hi[2]}};
  if (!x_box.Empty()) out[count++] = x_box;
  if (!y_box.Empty()) out[count++] = y_box;
  if (!z_box.Empty()) out[count++] = z_box;
  return count;
}

// How many probes a move exposes (what the update must trace first).
inline uint64_t ExposedProbes(const Coord& old_origin, const Coord& new_origin, const Dims& d) {
  Box boxes[3];
  const uint32_t count = ExposedBoxes(old_origin, new_origin, d, boxes);
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) total += boxes[i].Count();
  return total;
}

// ---------------------------------------------------------------------------
// small vectors
// ---------------------------------------------------------------------------

struct Vec2 {
  float x = 0, y = 0;
};
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Rgba {
  float r = 0, g = 0, b = 0, a = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// ---------------------------------------------------------------------------
// the octahedral map (the G-buffer's, gbuffer_types.h / gbuffer_rules.h OctEncode: gi_frame_test.cpp checks they are the same)
// ---------------------------------------------------------------------------

inline float SignNotZero(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

// The map's coordinate of a direction, each 0..1 (the map's whole square is the sphere: the upper hemisphere the inner diamond, the lower one folded out to the corners).
// `n` need not be unit length; it must not be zero.
inline Vec2 OctEncode(Vec3 n) {
  const float inv = 1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
  const float x = n.x * inv;
  const float y = n.y * inv;
  const float z = n.z * inv;
  float ex = x;
  float ey = y;
  if (z < 0.0f) {
    ex = (1.0f - std::fabs(y)) * SignNotZero(x);
    ey = (1.0f - std::fabs(x)) * SignNotZero(y);
  }
  return {ex * 0.5f + 0.5f, ey * 0.5f + 0.5f};
}
// The unit direction of a coordinate in 0..1.
inline Vec3 OctDecode(Vec2 f) {
  float x = f.x * 2.0f - 1.0f;
  float y = f.y * 2.0f - 1.0f;
  const float z = 1.0f - std::fabs(x) - std::fabs(y);
  const float t = Saturate(-z);
  x = x + (x >= 0.0f ? -t : t);
  y = y + (y >= 0.0f ? -t : t);
  const float il = 1.0f / std::sqrt(x * x + y * y + z * z);
  return {x * il, y * il, z * il};
}

// Which texel of a tile a BORDER texel copies (tile coordinates 0..9; the interior is 1..8): the border continues the octahedral map across its seam, so a bilinear read at the
// map's edge blends with what is on the other side of the fold. A corner copies the diagonal corner of the interior, an edge texel the mirrored one across the edge's middle. An
// interior texel copies itself.
constexpr Texel OctBorderSource(uint32_t tx, uint32_t ty) {
  const uint32_t last = kProbeTile - 1;  // 9
  const bool left = tx == 0, right = tx == last, top = ty == 0, bottom = ty == last;
  if (left && top) return {last - 1, last - 1};
  if (right && top) return {1, last - 1};
  if (left && bottom) return {last - 1, 1};
  if (right && bottom) return {1, 1};
  if (left) return {1, last - ty};
  if (right) return {last - 1, last - ty};
  if (top) return {last - tx, 1};
  if (bottom) return {last - tx, last - 1};
  return {tx, ty};
}

// The direction a tile texel (tile coordinates, interior 1..8) is the value of: the centre of the texel in the map.
inline Vec3 TexelDirection(uint32_t tx, uint32_t ty) {
  return OctDecode({(float(tx) - float(kProbeBorder) + 0.5f) / float(kProbeInterior), (float(ty) - float(kProbeBorder) + 0.5f) / float(kProbeInterior)});
}

// ---------------------------------------------------------------------------
// the integration of a probe's rays into a texel (the definition G2's blend kernel implements; the units are the header's)
// ---------------------------------------------------------------------------

struct RayResult {
  Vec3 dir;          // unit, where the ray went
  Vec3 radiance;     // what the hit sends back along the ray (zero where it escaped: the sky is not in the field)
  float distance = 0.0f;  // metres to the hit (ignored where escaped)
  bool escaped = false;
};

struct TexelValue {
  Vec3 irradiance;
  float sky_visibility = 0.0f;
  float mean_distance = 0.0f;
  float mean_distance_sq = 0.0f;
};

// The value of the texel whose direction is `texel_dir` from `count` rays spread uniformly over the sphere: irradiance and sky visibility by the cosine lobe (kRayScale / count), the
// distance moments by max(cos, 0)^kMomentsLobePower, an escaped ray at `max_distance`, a texel no ray reaches (no lobe weight) as open (max_distance).
inline TexelValue IntegrateTexel(Vec3 texel_dir, const RayResult* rays, uint32_t count, float max_distance) {
  TexelValue v;
  float sum_e[3] = {0, 0, 0};
  float sum_sky = 0.0f, sum_w = 0.0f, sum_d = 0.0f, sum_d2 = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    const RayResult& r = rays[i];
    const float cos_t = std::max(Dot(texel_dir, r.dir), 0.0f);
    if (r.escaped) {
      sum_sky += cos_t;
    } else {
      sum_e[0] += r.radiance.x * cos_t;
      sum_e[1] += r.radiance.y * cos_t;
      sum_e[2] += r.radiance.z * cos_t;
    }
    const float w = std::pow(cos_t, kMomentsLobePower);
    const float d = r.escaped ? max_distance : std::min(r.distance, max_distance);
    sum_w += w;
    sum_d += w * d;
    sum_d2 += w * d * d;
  }
  const float scale = count > 0 ? kRayScale / float(count) : 0.0f;
  v.irradiance = {sum_e[0] * scale, sum_e[1] * scale, sum_e[2] * scale};
  v.sky_visibility = sum_sky * scale;
  if (sum_w > 1.0e-12f) {
    v.mean_distance = sum_d / sum_w;
    v.mean_distance_sq = sum_d2 / sum_w;
  } else {
    v.mean_distance = max_distance;
    v.mean_distance_sq = max_distance * max_distance;
  }
  return v;
}

// ---------------------------------------------------------------------------
// the GiFrame: what the field's owner publishes and every reader takes
// ---------------------------------------------------------------------------

// The textures the field holds. A frame's `valid` has a bit of each product it has this frame; a product that is not in it has slot kNoSlot and must not be read.
enum Product : uint32_t {
  kIrradiance = 0,       // the irradiance atlas
  kMoments = 1,          // the distance moments atlas
  kProbeWeight = 2,      // the weight map
  kPixelIrradiance = 3,  // G5's denoised per-pixel irradiance (a product of the per-pixel pass, not of the field)
  kProductCount = 4,
};
constexpr uint32_t ProductBit(Product product) { return 1u << uint32_t(product); }
inline constexpr uint32_t kFieldProducts = ProductBit(kIrradiance) | ProductBit(kMoments) | ProductBit(kProbeWeight);

constexpr const char* ProductName(Product product) {
  switch (product) {
    case kIrradiance: return "irradiance";
    case kMoments: return "moments";
    case kProbeWeight: return "probe_weight";
    case kPixelIrradiance: return "pixel_irradiance";
    case kProductCount: break;
  }
  return "?";
}

// The readers (the interface's registry of who reads what, the header's comment above says what each reads). Appended values are allowed.
enum class Consumer : uint8_t {
  kBounceApply,        // G3: the AO's apply path
  kAmbientOcclusion,   // ao.cpp
  kLightPass,          // GB-H1
  kHitShading,         // M2's library
  kPixelPass,          // G5
  kVolumetrics,        // G8
  kDebugView,          // mcla_gpu_rt_gi_view
  kCount,
};
constexpr const char* ConsumerName(Consumer consumer) {
  switch (consumer) {
    case Consumer::kBounceApply: return "bounce apply";
    case Consumer::kAmbientOcclusion: return "ambient occlusion";
    case Consumer::kLightPass: return "light pass";
    case Consumer::kHitShading: return "hit shading";
    case Consumer::kPixelPass: return "per-pixel pass";
    case Consumer::kVolumetrics: return "volumetric light";
    case Consumer::kDebugView: return "debug view";
    case Consumer::kCount: break;
  }
  return "?";
}

// mcla_gpu_rt_gi_view (debug only): what the debug pass draws over the picture. Appended values are allowed.
enum class View : uint8_t {
  kOff = 0,
  kIrradiance = 1,    // E_surf at the pixel (the field's lookup)
  kValidity = 2,      // the probe weights (red = off, green = converged), the confidence
  kSkyVisibility = 3, // the sky visibility
  kProbes = 4,        // the probes' positions, by validity
  kBaseline = 5,      // the ground baseline E_base
  kLeak = 6,          // the leak test: E_surf split by the probes' sides of the nearest wall
  kCount = 7,
};

// One cascade of the frame (the table the shaders read: GpuGiFrame's cascade rows).
struct CascadeFrame {
  Coord origin;                 // the lattice coordinate of the window's minimum corner
  Dims dims;
  float spacing_m = 0.0f;
  uint32_t atlas_y = 0;         // the cascade's first row in the irradiance and moments atlases (texels)
  uint32_t weight_y = 0;        // ... and in the weight map
  float max_distance_m = 0.0f;  // the moments' clamp (MaxDistanceFor)
  float blend_band = kBlendBandProbes;  // probes over which the cascade fades into the next one at its edge

  // The cascade's window as lattice coordinates.
  constexpr Box Window() const { return {origin, {origin.x + int32_t(dims.x), origin.y + int32_t(dims.y), origin.z + int32_t(dims.z)}}; }
};

// The header flag bits (GpuGiFrame row 0, word 0).
inline constexpr uint32_t kFlagValid = 1u;          // the field's three textures are in the frame and the table is usable
inline constexpr uint32_t kFlagReplacesSsgi = 2u;   // the colour chain's gather may be skipped this frame (GiFrame::ReplacesSsgi)
inline constexpr uint32_t kFlagPixelPass = 4u;      // kPixelIrradiance is in the frame

// G5's per-pixel pass parameters (GpuGiFrame row 7, written only where kPixelIrradiance is in the frame and the image has a size): the pixel image is the occlusion's traced size divided by `divisor` (1 or 2), with its own
// depth guide and view normal guide (R32F / RGBA16F bindless slots, each under 65536) for the apply's depth and normal weighted read of a reduced image. Appended into the reserved row: no version.
struct PixelParams {
  uint32_t depth_slot = kNoSlot;
  uint32_t normal_slot = kNoSlot;
  uint32_t width = 0, height = 0;
  uint32_t divisor = 1;
  bool raw = false;   // G6: the image is the 1 spp signal itself (a frame DLSS Ray Reconstruction takes: no filter ran, no history length in its alpha): the apply weighs it by gi_pixel_rules.h RawWeight
};

struct GiFrame {
  uint32_t generation = 0;     // changes whenever the textures are made again (a new layout, a new size): drop what was cached from them
  uint32_t frame_serial = 0;   // the scene's frame the data belongs to (low 32 bits)
  uint32_t valid = 0;          // ProductBit() of the products in this frame
  Level level = Level::kOff;
  Layout layout = Layout::kNone;
  uint32_t cascade_count = 0;
  std::array<CascadeFrame, kMaxCascades> cascades = {};
  uint32_t atlas_width = 0, atlas_height = 0;  // texels of the irradiance and the moments atlas
  std::array<uint32_t, kProductCount> slot = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};  // bindless texture slots

  // How much of the field a reader may use this frame, 0..1: 0 while it warms up after a cut or is switched off, 1 when it stands. The publisher drives it (kFadeInSeconds,
  // kFadeOutSeconds); the apply multiplies the bounce by it, and the screen-space bounce light takes the rest (SsgiShare), so a fade is a crossfade.
  float fade = 0.0f;
  float safe_range_m = 0.0f;   // M1's SafeRange: from the camera, how far the structures stand (the nearest unbuilt hi sector bounds it); 0 = no field this frame
  float strength = 1.0f;       // the BOUNCE LIGHT level's strength (ssgi_level.h), the apply's
  float bounce_weight = kBounceWeightDefault;  // the probes' own feedback (G2)

  // The camera's place in the world (double: the cascades' origins are made from it; the shaders read it as float).
  double camera[3] = {0, 0, 0};
  // The frame's colours the baseline and AmbientAt need, scene colour units (the car hit's frame rows: the sun colour c39 with its exposure, the ambient c26 up and c57 down).
  float sun_rgb[3] = {0, 0, 0};
  float sun_sin_elevation = 0.0f;      // the sun's direction's y (up) component: sin of its elevation; negative below the horizon
  float ambient_up_rgb[3] = {0, 0, 0};
  float ambient_down_rgb[3] = {0, 0, 0};
  float ground_albedo = kGroundAlbedo;
  PixelParams pixel;   // G5: where the per-pixel pass's image and guides are (only read where Has(kPixelIrradiance))

  bool Has(Product product) const { return (valid & ProductBit(product)) != 0 && slot[uint32_t(product)] != kNoSlot; }
  bool FieldPresent() const { return Has(kIrradiance) && Has(kMoments) && Has(kProbeWeight) && cascade_count > 0 && cascade_count <= kMaxCascades; }
  // Whether the colour chain's gather may be skipped (the AO keeps its visibility pass): a field runs, is in the frame, and its fade is over 0.
  bool ReplacesSsgi() const { return level != Level::kOff && FieldPresent() && fade > 0.0f && safe_range_m > 0.0f; }
};

// The share of a pixel the screen-space bounce light keeps, 0..1: all of it where the field has nothing (confidence 0 or fade 0), none where it stands. The apply scales the SSGI's
// contribution by this and the field's by (1 - this): a crossfade, by pixel.
constexpr float SsgiShare(float confidence, float fade) {
  const float k = confidence * fade;
  return 1.0f - (k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k);
}

// ---------------------------------------------------------------------------
// the rows the shaders read (hlsl/gi_common.hlsli GiLoad...)
// ---------------------------------------------------------------------------
// 20 rows of 16 bytes (320 bytes): 8 rows of header, 3 rows for each of the 4 cascades. Floats are bit patterns, signed integers two's complement.
//   row 0   uint   flags (kFlag*), cascade count, atlas width, atlas height
//   row 1   uint   irradiance slot, moments slot, weight slot, pixel irradiance slot (kNoSlot = none)
//   row 2   float  fade, safe range (m), strength, bounce weight
//   row 3   float  camera x, y, z (world), ground albedo
//   row 4   float  sun colour rgb, sin of the sun's elevation
//   row 5   float  ambient up rgb, 0
//   row 6   float  ambient down rgb, 0
//   row 7   uint   G5's per-pixel pass (PixelParams; zero where the frame has no kPixelIrradiance): depth guide slot (low 16) | view normal guide slot << 16, image width | height << 16, divisor, flags (bit 0 raw: G6)
//   cascade c at row 8 + 3 c:
//     +0    int    origin x, y, z, atlas_y (uint)
//     +1    uint   dims x, y, z, weight_y
//     +2    float  spacing (m), 1 / spacing, max distance (m), blend band (probes)
inline constexpr uint32_t kGpuFrameRows = 20;
inline constexpr uint32_t kRowHeader = 0, kRowSlots = 1, kRowFade = 2, kRowCamera = 3, kRowSun = 4, kRowAmbientUp = 5, kRowAmbientDown = 6, kRowReserved = 7, kRowPixel = 7, kRowCascade0 = 8;
inline constexpr uint32_t kRowsPerCascade = 3;
inline constexpr uint32_t kGpuFrameBytes = kGpuFrameRows * 16;

struct GpuGiFrame {
  std::array<uint32_t, kGpuFrameRows * 4> words = {};
  bool operator==(const GpuGiFrame& o) const { return words == o.words; }
};

namespace detail {
inline uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
inline float FromBits(uint32_t u) { return std::bit_cast<float>(u); }
}  // namespace detail

inline uint32_t FrameFlags(const GiFrame& f) {
  return (f.FieldPresent() ? kFlagValid : 0u) | (f.ReplacesSsgi() ? kFlagReplacesSsgi : 0u) | (f.Has(kPixelIrradiance) ? kFlagPixelPass : 0u);
}

inline GpuGiFrame MakeGpuFrame(const GiFrame& f) {
  GpuGiFrame g;
  auto& w = g.words;
  const auto put = [&w](uint32_t row, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    w[row * 4 + 0] = a;
    w[row * 4 + 1] = b;
    w[row * 4 + 2] = c;
    w[row * 4 + 3] = d;
  };
  using detail::Bits;
  put(kRowHeader, FrameFlags(f), f.cascade_count, f.atlas_width, f.atlas_height);
  put(kRowSlots, f.slot[kIrradiance], f.slot[kMoments], f.slot[kProbeWeight], f.slot[kPixelIrradiance]);
  put(kRowFade, Bits(f.fade), Bits(f.safe_range_m), Bits(f.strength), Bits(f.bounce_weight));
  put(kRowCamera, Bits(float(f.camera[0])), Bits(float(f.camera[1])), Bits(float(f.camera[2])), Bits(f.ground_albedo));
  put(kRowSun, Bits(f.sun_rgb[0]), Bits(f.sun_rgb[1]), Bits(f.sun_rgb[2]), Bits(f.sun_sin_elevation));
  put(kRowAmbientUp, Bits(f.ambient_up_rgb[0]), Bits(f.ambient_up_rgb[1]), Bits(f.ambient_up_rgb[2]), 0);
  put(kRowAmbientDown, Bits(f.ambient_down_rgb[0]), Bits(f.ambient_down_rgb[1]), Bits(f.ambient_down_rgb[2]), 0);
  if (f.Has(kPixelIrradiance) && f.pixel.width != 0) {
    put(kRowPixel, (f.pixel.depth_slot & 0xFFFFu) | (f.pixel.normal_slot << 16), (f.pixel.width & 0xFFFFu) | (f.pixel.height << 16), f.pixel.divisor, f.pixel.raw ? 1u : 0u);
  }
  for (uint32_t c = 0; c < kMaxCascades && c < f.cascade_count; ++c) {
    const CascadeFrame& k = f.cascades[c];
    const uint32_t row = kRowCascade0 + kRowsPerCascade * c;
    put(row + 0, uint32_t(k.origin.x), uint32_t(k.origin.y), uint32_t(k.origin.z), k.atlas_y);
    put(row + 1, k.dims.x, k.dims.y, k.dims.z, k.weight_y);
    put(row + 2, Bits(k.spacing_m), Bits(k.spacing_m > 0.0f ? 1.0f / k.spacing_m : 0.0f), Bits(k.max_distance_m), Bits(k.blend_band));
  }
  return g;
}

// A frame for a budget with the cascades centred on `camera` (the table a publisher starts from: origins, atlas rows, the moments' clamp). The textures' slots, the fade, the safe
// range, the strength and the colours are the publisher's.
inline GiFrame MakeFrame(const Budget& b, const double camera[3]) {
  GiFrame f;
  f.layout = b.layout;
  f.cascade_count = b.cascade_count;
  f.atlas_width = AtlasWidth(b);
  f.atlas_height = AtlasHeight(b);
  for (uint32_t c = 0; c < b.cascade_count; ++c) {
    CascadeFrame& k = f.cascades[c];
    k.origin = OriginFor(camera, b.cascade[c], b.y_centre_offset_m);
    k.dims = b.cascade[c].dims;
    k.spacing_m = b.cascade[c].spacing_m;
    k.atlas_y = AtlasRow(b, c);
    k.weight_y = WeightRow(b, c);
    k.max_distance_m = MaxDistanceFor(b.cascade[c].spacing_m);
    k.blend_band = b.cascade[c].blend_band;
  }
  for (int a = 0; a < 3; ++a) f.camera[a] = camera[a];
  return f;
}

// ---------------------------------------------------------------------------
// the lookup (the C++ twin of hlsl/gi_common.hlsli: the same operations in the same order)
// ---------------------------------------------------------------------------
// A Source is what the textures are: SampleIrradiance(u, v) -> Rgba (bilinear, clamped, the atlas' 0..1 coordinates), SampleMoments(u, v) -> Vec2 (the same), LoadWeight(x, y) ->
// float 0..1 (the weight map's texel, no filtering).

struct Sample {
  Vec3 irradiance;             // E_surf: what surfaces send, scene colour units (no sky)
  float sky_visibility = 0.0f; // the cosine weighted share of the lobe that sees the sky
  float confidence = 0.0f;     // 0..1: how much of the lookup the field holds (the rest is the fallback's)
};

// The atlas coordinates (0..1) of direction `oct` (OctEncode of it) in the tile of slot (sx, sy, sz) of a cascade.
inline Vec2 ProbeUv(const CascadeFrame& k, uint32_t atlas_w, uint32_t atlas_h, uint32_t sx, uint32_t sy, uint32_t sz, Vec2 oct) {
  const float px = float((sx + k.dims.x * sy) * kProbeTile) + float(kProbeBorder) + oct.x * float(kProbeInterior);
  const float py = float(k.atlas_y + sz * kProbeTile) + float(kProbeBorder) + oct.y * float(kProbeInterior);
  return {px * (1.0f / float(atlas_w)), py * (1.0f / float(atlas_h))};
}

// How far a world point is inside a cascade's usable window, 0..1: 1 deeper than blend_band probes from the edge (the edge of the window of probes whose cell is all inside), 0 at it
// and outside. The next, coarser cascade takes over as this falls.
inline float CascadeBlend(const CascadeFrame& k, Vec3 p) {
  const float inv = k.spacing_m > 0.0f ? 1.0f / k.spacing_m : 0.0f;
  const float gx = p.x * inv - float(k.origin.x);
  const float gy = p.y * inv - float(k.origin.y);
  const float gz = p.z * inv - float(k.origin.z);
  const float edge = std::min(std::min(std::min(gx, float(k.dims.x - 1) - gx), std::min(gy, float(k.dims.y - 1) - gy)), std::min(gz, float(k.dims.z - 1) - gz));
  return k.blend_band > 0.0f ? Saturate(edge / k.blend_band) : (edge > 0.0f ? 1.0f : 0.0f);
}

// 1 up to (1 - kRangeFadeShare) of the safe range from the camera, falling to 0 at it; 0 where no safe range is set.
inline float RangeFade(float distance_m, float safe_range_m) {
  if (!(safe_range_m > 0.0f)) return 0.0f;
  const float start = safe_range_m * (1.0f - kRangeFadeShare);
  return Saturate((safe_range_m - distance_m) / (safe_range_m - start));
}

// One cascade's lookup at world point p, normal n (unit): the eight probes round the point, weighted by trilinear x validity x the smooth back face term x the Chebyshev visibility.
template <class Source>
Sample CascadeLookup(const CascadeFrame& k, uint32_t atlas_w, uint32_t atlas_h, const Source& src, Vec3 p, Vec3 n) {
  const float offset = kNormalOffsetSpacings * k.spacing_m;
  const Vec3 q = {p.x + n.x * offset, p.y + n.y * offset, p.z + n.z * offset};
  const float inv = 1.0f / k.spacing_m;
  // The lattice position, kept inside the window so that all eight corners are (a clamp changes the answer only within a tenth of a cell of the edge, where the blend is ~0).
  const float gx = std::min(std::max(q.x * inv, float(k.origin.x)), float(k.origin.x) + float(k.dims.x - 1));
  const float gy = std::min(std::max(q.y * inv, float(k.origin.y)), float(k.origin.y) + float(k.dims.y - 1));
  const float gz = std::min(std::max(q.z * inv, float(k.origin.z)), float(k.origin.z) + float(k.dims.z - 1));
  const int32_t bx = std::min(int32_t(std::floor(gx)), k.origin.x + int32_t(k.dims.x) - 2);
  const int32_t by = std::min(int32_t(std::floor(gy)), k.origin.y + int32_t(k.dims.y) - 2);
  const int32_t bz = std::min(int32_t(std::floor(gz)), k.origin.z + int32_t(k.dims.z) - 2);
  const float fx = gx - float(bx);
  const float fy = gy - float(by);
  const float fz = gz - float(bz);
  const Vec2 oct_n = OctEncode(n);

  float sum_w = 0.0f, sum_t = 0.0f, sky = 0.0f;
  Vec3 acc = {0, 0, 0};
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t ox = i & 1u, oy = (i >> 1) & 1u, oz = (i >> 2) & 1u;
    const int32_t cx = bx + int32_t(ox), cy = by + int32_t(oy), cz = bz + int32_t(oz);
    const float wt = (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy) * (oz ? fz : 1.0f - fz);
    const uint32_t sx = Wrap(cx, k.dims.x), sy = Wrap(cy, k.dims.y), sz = Wrap(cz, k.dims.z);
    const float wv = src.LoadWeight(sx + k.dims.x * sy, k.weight_y + sz);
    if (!(wt * wv > 0.0f)) continue;
    const Vec3 d = {float(cx) * k.spacing_m - q.x, float(cy) * k.spacing_m - q.y, float(cz) * k.spacing_m - q.z};
    const float dist = std::sqrt(std::max(Dot(d, d), 1.0e-8f));
    const Vec3 to_probe = d * (1.0f / dist);
    float bf = std::max((Dot(to_probe, n) + 1.0f) * 0.5f, kBackFaceFloor);
    bf = bf * bf;
    const Vec2 moments_uv = ProbeUv(k, atlas_w, atlas_h, sx, sy, sz, OctEncode({-to_probe.x, -to_probe.y, -to_probe.z}));
    const Vec2 m = src.SampleMoments(moments_uv.x, moments_uv.y);
    float wc = 1.0f;
    if (dist > m.x) {
      const float variance = std::fabs(m.y - m.x * m.x);
      const float diff = dist - m.x;
      const float c = variance / (variance + diff * diff);
      wc = std::max(c * c * c, kChebyshevFloor);
    }
    const float w = wt * wv * bf * wc;
    const Vec2 uv = ProbeUv(k, atlas_w, atlas_h, sx, sy, sz, oct_n);
    const Rgba e = src.SampleIrradiance(uv.x, uv.y);
    acc = acc + Vec3{e.r, e.g, e.b} * w;
    sky += e.a * w;
    sum_w += w;
    sum_t += wt * wv;
  }
  Sample s;
  if (sum_w > kMinWeightSum) {
    const float rcp = 1.0f / sum_w;
    s.irradiance = acc * rcp;
    s.sky_visibility = sky * rcp;
  }
  s.confidence = sum_t;
  return s;
}

// The field's lookup at world point p with unit normal n: the finest cascade that holds the point, blended into the coarser ones over the band at its edge, each cascade counted by how
// much of its probes are valid there (a cascade that is still being built hands the point to the next one), and the whole faded to 0 toward the safe range.
template <class Source>
Sample Lookup(const GiFrame& f, const Source& src, Vec3 p, Vec3 n) {
  float remaining = 1.0f, sky = 0.0f;
  Vec3 acc = {0, 0, 0};
  for (uint32_t c = 0; c < f.cascade_count && c < kMaxCascades; ++c) {
    const CascadeFrame& k = f.cascades[c];
    const float w = CascadeBlend(k, p);
    if (!(w > 0.0f)) continue;
    const Sample s = CascadeLookup(k, f.atlas_width, f.atlas_height, src, p, n);
    const float cw = remaining * (w * s.confidence);
    acc = acc + s.irradiance * cw;
    sky += s.sky_visibility * cw;
    remaining -= cw;
    if (remaining <= kRemainingStop) break;
  }
  const float cover = 1.0f - remaining;
  Sample out;
  if (cover > kMinCover) {
    const float rcp = 1.0f / cover;
    out.irradiance = acc * rcp;
    out.sky_visibility = sky * rcp;
  }
  const Vec3 d = {p.x - float(f.camera[0]), p.y - float(f.camera[1]), p.z - float(f.camera[2])};
  out.confidence = cover * RangeFade(std::sqrt(Dot(d, d)), f.safe_range_m);
  return out;
}

// The game's flat hemisphere ambient for a normal: the down colour at the bottom, the up colour at the top (the fallback of AmbientAt and what the field's ground bounce stands in for).
inline Vec3 AmbientHemisphere(const GiFrame& f, Vec3 n) {
  const float t = 0.5f + 0.5f * n.y;
  const Vec3 up = {f.ambient_up_rgb[0], f.ambient_up_rgb[1], f.ambient_up_rgb[2]};
  const Vec3 down = {f.ambient_down_rgb[0], f.ambient_down_rgb[1], f.ambient_down_rgb[2]};
  return down + (up - down) * t;
}

// The ambient light at a point (M3 section 4.3: M2's hit shading, the library's GI mode in the reflections): where the field stands, its irradiance (surfaces) + the sky's ambient
// x the sky visibility; where it does not, the flat hemisphere; blended by confidence x fade.
template <class Source>
Vec3 AmbientAt(const GiFrame& f, const Source& src, Vec3 p, Vec3 n) {
  const Vec3 hemisphere = AmbientHemisphere(f, n);
  const Sample s = Lookup(f, src, p, n);
  const float k = Saturate(s.confidence * f.fade);
  const Vec3 up = {f.ambient_up_rgb[0], f.ambient_up_rgb[1], f.ambient_up_rgb[2]};
  const Vec3 field = s.irradiance + up * s.sky_visibility;
  return hemisphere + (field - hemisphere) * k;
}

// What an unobstructed plane of albedo ground_albedo sends to a surface with normal n (M3 section 4.2): the cosine weighted share of its hemisphere that is ground, (1 - n.y) / 2,
// times the ground's radiance, albedo x (the sun's colour x sin(elevation) + the up ambient). At night the sun term is 0 and the ambient stays.
inline Vec3 GroundBaseline(const GiFrame& f, Vec3 n) {
  const float share = 0.5f * (1.0f - n.y) * f.ground_albedo;
  const float sun = std::max(f.sun_sin_elevation, 0.0f);
  return {(f.sun_rgb[0] * sun + f.ambient_up_rgb[0]) * share, (f.sun_rgb[1] * sun + f.ambient_up_rgb[1]) * share, (f.sun_rgb[2] * sun + f.ambient_up_rgb[2]) * share};
}

// E_net = max(E_surf - E_base, 0), per channel: what geometry sends over what the open ground the game's ambient stands for would.
inline Vec3 NetIrradiance(Vec3 e_surf, Vec3 e_base) {
  return {std::max(e_surf.x - e_base.x, 0.0f), std::max(e_surf.y - e_base.y, 0.0f), std::max(e_surf.z - e_base.z, 0.0f)};
}

// ---------------------------------------------------------------------------
// the static checks (the layouts and the numbers the design promises)
// ---------------------------------------------------------------------------

static_assert(kProbeTile == 10 && kProbeTexels == 100 && kProbeAtlasBytes == 1200 && kProbeBytes == 1201, "a probe is 1.2 KB of atlas and one byte of weight");
static_assert(TexelBytes(kIrradianceFormat) == 8 && TexelBytes(kMomentsFormat) == 4 && TexelBytes(kWeightFormat) == 1, "RGBA16F, RG16F, R8");
static_assert(sizeof(GpuGiFrame) == kGpuFrameBytes && kGpuFrameBytes == 320, "the frame is 20 rows of 16 bytes");
static_assert(kRowCascade0 + kRowsPerCascade * kMaxCascades == kGpuFrameRows, "8 header rows and 3 rows for each of the 4 cascades");
static_assert(kRowReserved == 7 && kRowAmbientDown == 6, "the header's rows");
static_assert(Wrap(-1, 32) == 31 && Wrap(32, 32) == 0 && Wrap(-33, 32) == 31 && Wrap(0, 16) == 0 && Wrap(17, 16) == 1, "the slot of a coordinate");
static_assert(SlotOf({-1, 17, 33}, {32, 16, 32}) == Slot{31, 1, 1}, "the slot of a probe");
static_assert(ProbeIndex({31, 15, 31}, {32, 16, 32}) == 16383 && ProbeIndex({0, 0, 0}, {32, 16, 32}) == 0 && ProbeIndex({1, 2, 3}, {32, 16, 32}) == (3 * 16 + 2) * 32 + 1, "the probe's number");
static_assert(TileOrigin({31, 15, 31}, {32, 16, 32}) == Texel{(31 + 32 * 15) * 10, 310} && WeightTexel({31, 15, 31}, {32, 16, 32}) == Texel{511, 31}, "the tile and the weight texel");
static_assert(OctBorderSource(0, 0) == Texel{8, 8} && OctBorderSource(9, 0) == Texel{1, 8} && OctBorderSource(0, 9) == Texel{8, 1} && OctBorderSource(9, 9) == Texel{1, 1} &&
                  OctBorderSource(0, 3) == Texel{1, 6} && OctBorderSource(9, 3) == Texel{8, 6} && OctBorderSource(3, 0) == Texel{6, 1} && OctBorderSource(3, 9) == Texel{6, 8} &&
                  OctBorderSource(4, 5) == Texel{4, 5},
              "the border continues the octahedral map across its seam");

// M3 section 6's table: the cascades, the rays, the intervals, and the rays a frame (12 K, 459 K, 983 K).
static_assert(BudgetOf(Layout::kLean).cascade_count == 2 && BudgetOf(Layout::kStandard).cascade_count == 3 && BudgetOf(Layout::kExtended).cascade_count == 4 && BudgetOf(Layout::kNone).cascade_count == 0,
              "cascades: 2 lean, 3 standard, 4 extended (HitBudget::gi_cascades)");
static_assert(BudgetRaysPerFrame(BudgetOf(Layout::kLean)) == 12288 && BudgetRaysPerFrame(BudgetOf(Layout::kStandard)) == 458752 && BudgetRaysPerFrame(BudgetOf(Layout::kExtended)) == 983040 &&
                  BudgetRaysPerFrame(BudgetOf(Layout::kNone)) == 0,
              "rays a frame (HitBudget::gi_rays_per_frame): 12 K, 459 K, 983 K");
static_assert(BudgetProbes(BudgetOf(Layout::kLean)) == 4096 && BudgetProbes(BudgetOf(Layout::kStandard)) == 49152 && BudgetProbes(BudgetOf(Layout::kExtended)) == 65536, "probes");
static_assert(ProbesPerFrame(BudgetOf(Layout::kStandard).cascade[0]) == 4096 && ProbesPerFrame(BudgetOf(Layout::kStandard).cascade[1]) == 2048 && ProbesPerFrame(BudgetOf(Layout::kStandard).cascade[2]) == 1024,
              "High updates 4,096 + 2,048 + 1,024 probes a frame");
static_assert(ProbesPerFrame(BudgetOf(Layout::kLean).cascade[0]) + ProbesPerFrame(BudgetOf(Layout::kLean).cascade[1]) == 384, "Mid updates 384 probes a frame");
static_assert(BudgetOf(Layout::kLean).cascade[0].rays_per_probe == 32 && BudgetOf(Layout::kStandard).cascade[0].rays_per_probe == 64 && BudgetOf(Layout::kExtended).cascade[3].rays_per_probe == 64,
              "rays a probe (HitBudget::gi_rays_per_probe)");
static_assert(BudgetOf(Layout::kExtended).pixel_rays_per_pixel == 1 && BudgetOf(Layout::kStandard).pixel_rays_per_pixel == 0 && BudgetOf(Layout::kLean).pixel_rays_per_pixel == 0,
              "the per-pixel pass only at the extended layout (HitBudget::gi_pixel_pass)");
// A cascade's memory: 16,384 probes x 1.2 KB = 19.66 MB for a High cascade, the atlases of three 58.98 MB, four 78.64 MB, Mid 4.9 MB (the design's 19.7, 59, 79 and 5 MB).
static_assert(BudgetOf(Layout::kStandard).cascade[0].dims.Probes() * kProbeAtlasBytes == 19660800u, "a High cascade is 19.66 MB of atlas");
static_assert(AtlasWidth(BudgetOf(Layout::kStandard)) == 5120 && AtlasHeight(BudgetOf(Layout::kStandard)) == 960 && AtlasHeight(BudgetOf(Layout::kExtended)) == 1280 &&
                  AtlasWidth(BudgetOf(Layout::kLean)) == 1280 && AtlasHeight(BudgetOf(Layout::kLean)) == 320,
              "the atlases' sizes");
static_assert(uint64_t(AtlasWidth(BudgetOf(Layout::kStandard))) * AtlasHeight(BudgetOf(Layout::kStandard)) * 12 == 3ull * 19660800ull, "the atlas of three is three cascades' atlas bytes, no waste");
static_assert(AtlasRow(BudgetOf(Layout::kStandard), 2) == 640 && WeightRow(BudgetOf(Layout::kStandard), 2) == 64 && WeightWidth(BudgetOf(Layout::kStandard)) == 512 && WeightHeight(BudgetOf(Layout::kExtended)) == 128,
              "the cascades' rows");
static_assert(AtlasWidth(BudgetOf(Layout::kExtended)) <= 16384 && AtlasHeight(BudgetOf(Layout::kExtended)) <= 16384, "inside every texture size limit");
static_assert(BudgetOf(Layout::kStandard).cost.vram_mib >= AtlasBytes(BudgetOf(Layout::kStandard)) / (1024 * 1024) && BudgetOf(Layout::kExtended).cost.vram_mib >= AtlasBytes(BudgetOf(Layout::kExtended)) / (1024 * 1024) &&
                  BudgetOf(Layout::kLean).cost.vram_mib >= AtlasBytes(BudgetOf(Layout::kLean)) / (1024 * 1024),
              "the estimated video memory holds the atlases");
// The cascades nest: each is wider than the one before it on every axis, and the y centre keeps the finer window inside the coarser (checked by the test with real cameras).
static_assert(SpanM(BudgetOf(Layout::kStandard).cascade[0], 0) == 64.0f && SpanM(BudgetOf(Layout::kStandard).cascade[2], 1) == 128.0f && SpanM(BudgetOf(Layout::kExtended).cascade[3], 2) == 512.0f &&
                  SpanM(BudgetOf(Layout::kLean).cascade[1], 0) == 96.0f,
              "64 / 128 / 256 (/ 512) m across, 32 / 64 / 128 (/ 256) m up; Mid 48 and 96 m");
static_assert(BudgetOf(Layout::kStandard).cascade[1].spacing_m == 2.0f * BudgetOf(Layout::kStandard).cascade[0].spacing_m && BudgetOf(Layout::kExtended).cascade[3].spacing_m == 16.0f,
              "the spacings double: the lattices are nested");

// The cells of the table (the (tier, level) -> layout rule).
static_assert(LayoutFor(Rung::kNone, Level::kUltra) == Layout::kNone && LayoutFor(Rung::kNone, Level::kHigh) == Layout::kNone && LayoutFor(Rung::kNone, Level::kOff) == Layout::kNone,
              "none: no field, the screen-space bounce light");
static_assert(LayoutFor(Rung::kWeak, Level::kOff) == Layout::kNone && LayoutFor(Rung::kWeak, Level::kHigh) == Layout::kStandard && LayoutFor(Rung::kWeak, Level::kUltra) == Layout::kExtended,
              "weak and strong use the same layouts");
static_assert(LayoutFor(Rung::kStrong, Level::kOff) == Layout::kNone && LayoutFor(Rung::kStrong, Level::kHigh) == Layout::kStandard && LayoutFor(Rung::kStrong, Level::kUltra) == Layout::kExtended,
              "strong: standard at HIGH, extended (with the per-pixel pass) at ULTRA");
static_assert(!PixelPassAllowed(Rung::kWeak, Layout::kStandard) && PixelPassAllowed(Rung::kWeak, Layout::kExtended) && !PixelPassAllowed(Rung::kStrong, Layout::kStandard) &&
                  PixelPassAllowed(Rung::kStrong, Layout::kExtended) && !PixelPassAllowed(Rung::kNone, Layout::kExtended),
              "the per-pixel pass is the ULTRA layer on RT-capable cards");
static_assert(AutoLevel(Rung::kNone) == Level::kOff && AutoLevel(Rung::kWeak) == Level::kOff && AutoLevel(Rung::kStrong) == Level::kHigh, "auto: the field on a strong card at HIGH only (rt_tier.h kRtGi)");
static_assert(!ReplacesSsgi(Layout::kNone) && ReplacesSsgi(Layout::kLean) && ReplacesSsgi(Layout::kStandard) && ReplacesSsgi(Layout::kExtended), "the field replaces the screen-space bounce light wherever it runs");
static_assert(static_cast<uint8_t>(Rung::kNone) == 0 && static_cast<uint8_t>(Rung::kWeak) == 1 && static_cast<uint8_t>(Rung::kStrong) == 2, "the order of gpu_class::RtTier");
static_assert(SsgiShare(0.0f, 1.0f) == 1.0f && SsgiShare(1.0f, 0.0f) == 1.0f && SsgiShare(1.0f, 1.0f) == 0.0f && SsgiShare(0.5f, 1.0f) == 0.5f, "the screen-space share of a pixel");
// The costs are in the design's order: more layout, more cost.
static_assert(BudgetOf(Layout::kLean).cost.ms_5080_4k_hi < BudgetOf(Layout::kStandard).cost.ms_5080_4k_hi && BudgetOf(Layout::kStandard).cost.ms_5080_4k_hi < BudgetOf(Layout::kExtended).cost.ms_5080_4k_hi &&
                  BudgetOf(Layout::kLean).cost.vram_mib < BudgetOf(Layout::kStandard).cost.vram_mib && BudgetOf(Layout::kStandard).cost.vram_mib < BudgetOf(Layout::kExtended).cost.vram_mib,
              "the estimates grow with the layout");

}  // namespace mcla::gpu::gi
