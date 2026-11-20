#pragma once
// ===========================================================================
// MCLA GPU renderer -- volumetric light: the FROZEN interface (perf/VOLUMETRICS-SURVEY.md sections 2, 7 and 8)
// ===========================================================================
// Volumetric light = light beams under street lamps and car headlights, fog that light shows in, and sun shafts through the
// cascade shadows. A camera aligned grid of froxels (x, y over the picture, z in log spaced slices) is lit by compute passes and
// composited into the scene colour once. The packages that work in parallel build against THIS file and nothing else:
//
//   VOL-CORE     volumetrics.{h,cpp}: lifecycle, the two points, resources, the compute passes, the composite, gating.
//                It owns everything marked "core" below (VolFrameContext, the GPU constants it fills) and a minimal producer of the
//                sun, the cascades and the weather defaults (VolInputs below) so that it works alone.
//   VOL-SHADERS  hlsl/volumetrics_*: the inject / integrate / apply shaders and volumetrics_rules.h (the C++ twin of their math).
//                They read VolGpuConstants (below) byte for byte, as a ByteAddressBuffer: the offsets are listed in Offsets.
//   VOL-INPUTS   volumetrics_inputs.{h,cpp}: the producers of VolInputs (the lamp list, the weather, the fog colours, the
//                adaptors of HeadlightSet and LocalShadowFrame) behind VolInputSource.
//   VOL-MENU     the setting `mcla_gpu_volumetrics` (auto | off | medium | high | ultra), its row and class cell.
//
// A producer never touches the GPU and the shaders never see a producer: the seam is VolInputs (what exists in the world) and
// VolGpuConstants (what the shaders read). All of it is plain data, so a test can fill it with a mock.
//
// CONVENTIONS (the game's, as the renderer reads them everywhere else)
//   world      the game's: y up, metres; positions and directions of lights are world space.
//   view       the renderer's own view space: x right, y up, z forward (the game's view space looks along -z).
//   camera     pos, right, up and forward are world vectors (right = row 0 of the view inverse, up = row 1, forward = minus row 2,
//              pos = row 3), tan_x / tan_y the tangents of the half field of view of the projection.
//   colours    scene-referred linear light (the HDR colour of the main view, 0 to about 2), already times the light's own strength,
//              fade and night factor: a light that fades is a dimmer one.
//   depth      the froxel grid's slice k covers [near * (far / near)^(k / nz), near * (far / near)^((k + 1) / nz)] metres along the
//              view's z axis; a value stored at texel k stands for the slice's CENTRE depth near * (far / near)^((k + 0.5) / nz).
//   2D atlas   the grid is stored as a 2D image so that the host pass layout needs no 3D descriptor (D3D12 and Vulkan alike):
//              slice k is the tile (k % tiles_x, k / tiles_x) of nx x ny texels; atlas = (tiles_x * nx) x (tiles_y * ny).
//
// The shadow shapes are those of src/gpu/local_shadow_types.h (LocalShadowView, LocalShadowFrame) and the headlights' those of
// src/gpu/headlight_rig.h (Headlight, HeadlightSet) of feat/headlights, field for field; they are not included here (that branch is
// not on main yet): VolShadowView and VolLamp are their GPU side twins and FillHeadlights / FillShadowViews below take any type that
// has those fields (a template: the producer includes the real headers).
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::volumetrics {

// ---------------------------------------------------------------------------
// the level and the grid
// ---------------------------------------------------------------------------

// The quality levels of the setting `mcla_gpu_volumetrics` (auto | off | medium | high | ultra; `auto` resolves per class through
// gpu_class.h: Low off, Mid medium, High high). Nothing is called low.
enum class Level : uint8_t { kOff, kMedium, kHigh, kUltra };

inline constexpr uint32_t kMaxLamps = 48;
inline constexpr uint32_t kMaxHeadlights = 4;
inline constexpr uint32_t kMaxCascades = 4;
inline constexpr uint32_t kMaxShadowViews = 8;  // a view for each lamp of the rig's four cars (local_shadow_types.h kMaxShadowViews)
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

// The grid of a level. The cost is the number of froxels (nx * ny * nz) times the work of one: the survey's table (4K, RTX 5080:
// 0.13 / 0.19 / 0.30 ms for Medium / High / Ultra) is the ceiling.
struct GridDesc {
  uint32_t nx = 0;
  uint32_t ny = 0;
  uint32_t nz = 0;
  uint32_t tiles_x = 0;  // the 2D atlas: tiles_x * tiles_y == nz
  uint32_t tiles_y = 0;
  float near_m = 0.5f;
  float far_m = 160.0f;
  uint32_t lamps = 0;       // how many of VolInputs::lamps are read
  uint32_t headlights = 0;  // how many of VolInputs::headlights are read
  bool temporal = false;    // the scatter is blended with last frame's, reprojected

  constexpr uint32_t atlas_width() const { return nx * tiles_x; }
  constexpr uint32_t atlas_height() const { return ny * tiles_y; }
  constexpr uint32_t voxels() const { return nx * ny * nz; }
  constexpr bool valid() const { return nx != 0 && ny != 0 && nz != 0 && tiles_x * tiles_y == nz; }
};

// Medium: 96 x 54 x 32 (8 x 4 tiles, a 768 x 216 atlas), 120 m, 12 lamps, 2 headlights, no history.
// High:   128 x 72 x 48 (8 x 6, 1024 x 432), 160 m, 32 lamps, 4 headlights, history.
// Ultra:  160 x 90 x 64 (8 x 8, 1280 x 720), 160 m, 48 lamps, 4 headlights, history.
// Off has no grid (valid() is false).
constexpr GridDesc GridFor(Level level) {
  switch (level) {
    case Level::kMedium: return GridDesc{96, 54, 32, 8, 4, 0.5f, 120.0f, 12, 2, false};
    case Level::kHigh: return GridDesc{128, 72, 48, 8, 6, 0.5f, 160.0f, 32, 4, true};
    case Level::kUltra: return GridDesc{160, 90, 64, 8, 8, 0.5f, 160.0f, 48, 4, true};
    case Level::kOff: break;
  }
  return GridDesc{};
}

// The slice maths, shared by the C++ rules, the shaders' twin and the tests (the shaders write the same formulas).
// depth of the slice boundary k (k = 0 .. nz): near * (far / near)^(k / nz).
// The slice coordinate of a view depth, in slices (0 at the near plane, nz at the far plane): nz * ln(z / near) / ln(far / near).
// The atlas tile of slice k: (k % tiles_x, k / tiles_x).

// ---------------------------------------------------------------------------
// what exists in the world: the producers' side (CPU)
// ---------------------------------------------------------------------------

// One spot light: a street lamp of the game's per frame list, or a car's headlight. 64 bytes = four float4, the GPU layout too.
//   pos, range      world position; the reach in metres (the light is zero beyond it)
//   dir, cos_outer  unit axis of the cone; cosine of its outer half angle (-1: a point light, no cone)
//   colour, cos_inner  light colour x intensity x fade; cosine of the inner half angle (full strength inside it)
//   shadow_view     index into VolInputs::shadow_views of this light's shadow map, or -1 (unshadowed)
//   shadow_view_b   a car's light has a view for each of its two headlamps (HL-PER-LAMP): the index of the second, or -1; the light's share is the mean of the two
struct VolLamp {
  float pos[3];
  float range;
  float dir[3];
  float cos_outer;
  float colour[3];
  float cos_inner;
  int32_t shadow_view;
  float pad[1];  // pad[0]: the near radius squared (volumetrics_lamps.h)
  int32_t shadow_view_b;
  float pad2;
};
static_assert(sizeof(VolLamp) == 64);

// One shadow map view of a spot light (LocalShadowView of local_shadow_types.h, field for field). The matrix is the published one
// (local_shadow_rules.h WorldToShadow): column vectors, row major, so o[j] = M[4j] x + M[4j+1] y + M[4j+2] z + M[4j+3] for a world
// point (x, y, z); the ATLAS uv is o.xy / o.w (the tile's offset and size are in it already) and the receiver depth o.z / o.w (a point
// behind the light has o.w <= 0). The map stores z / w of the light's projection, 1 where nothing was drawn; the reader compares
// receiver_depth - depth_bias against the stored value (stored < that: shadowed; a stored 1: nothing there, lit; a receiver depth
// outside 0..1: lit). The atlas rectangle is the tile as (u offset, v offset, u size, v size) of the atlas (to keep a tap inside it),
// `light_index` the index of the headlight it belongs to. 96 bytes, the GPU layout too.
struct VolShadowView {
  float world_to_shadow[16];
  float atlas_rect[4];
  float depth_bias;
  float texel_size;
  uint32_t light_index;
  uint32_t valid;
};
static_assert(sizeof(VolShadowView) == 96);

// The sun's cascades as the game's shadow collector reads them (PS_ShadowBlend, phase 0x1000): the four cascades' light matrices are
// the registers c64 + 4 i ... c67 + 4 i (gLightMatrixArr, "rows" = four float4 as the registers hold them), the box of a cascade the
// planes c85 / c82 (the right and top axes) with the offsets and half extents of c86 / c87 and c83 / c84 at the cascade's index.
//   uv     = ( dot(p, (r0.x, r1.x, r2.x)) + r3.x,  dot(p, (r0.y, r1.y, r2.y)) + r3.y )   in the atlas, 0..1
//   depth  = ( dot(p, (r0.z, r1.z, r2.z)) + r3.z ) * ( dot(p, (r0.w, r1.w, r2.w)) + r3.w )
//   lit    = stored >= depth (the game's own compare; a cascade inside its box: right_offset + dot(p, right) within +-right_half, the same top)
// The cascade is 4 minus the number of boxes that hold the point (nested boxes: the finest that does). CHECK V3.
struct VolCascade {
  float rows[4][4];
  float box[4];  // x right offset (c86[i]), y right half extent (c87[i]), z top offset (c83[i]), w top half extent (c84[i])
};
static_assert(sizeof(VolCascade) == 80);

struct VolSun {
  float right[4] = {};  // xyz: c85 (the right axis the boxes are measured along)
  float top[4] = {};    // xyz: c82 (the top axis)
  std::array<VolCascade, kMaxCascades> cascades{};
  uint32_t atlas_slot = kNoSlot;  // the bindless slot of the cascade atlas (the collector's fetch constant 15), kNoSlot: none
  uint32_t cascade_count = 0;     // 0: no sun shadow (the sun is unshadowed: lit everywhere)
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
  float atlas_size[2] = {1280.0f, 1280.0f};  // the atlas in texels (the game's 1280, or the scaled side view's)
  float depth_bias = 0.0f;                   // added to the receiver depth before the compare (the game's: none)
  float reserved2 = 0.0f;
};

// The world of a frame: the medium and the light that shows in it. All of it is chosen by a producer from the weather, the hour, the
// wetness and the settings; the shaders read it as it is. 7 x float4.
struct VolWorld {
  // x sigma_t: the haze's extinction, 1/m (the clear-air floor times the weather); y ground_y: the ground's height (m, the smoothed
  // player's: a hill above the layer is clear air); z ground_h: the ground layer's scale height (m; 0: none); w ground_k: the layer's
  // extra density at the ground, a multiplier (sigma(y) = sigma_t * (1 + ground_k * exp(-(y - ground_y) / ground_h)))
  float density[4] = {0.0012f, 0.0f, 40.0f, 1.0f};
  // x noise_k: the 3D value noise's relative amplitude (0..1); y noise_scale: 1/m (a feature of 1/scale metres); z, w: the wind's
  // offset of the noise in x and z, metres, integrated by the producer over time
  float noise[4] = {0.35f, 0.02f, 0.0f, 0.0f};
  // xyz: the unit vector TOWARD the sun; w sun_fade: the collector's ShaderGlobals.z (the visibility is lerp(lit, 1, sun_fade))
  float sun_dir[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  // rgb: the sun's colour times its strength (0: no sun term: night, or a sun below the horizon); w g_sun: the phase function's anisotropy
  float sun_colour[4] = {0.0f, 0.0f, 0.0f, 0.6f};
  // rgb: the haze's own in-scatter colour (the game's fog colour); w far_m: the grid's far plane this frame (0: the level's)
  float haze_colour[4] = {0.05f, 0.06f, 0.08f, 0.0f};
  // x night 0..1; y cap: the composite's soft limit on the light a pixel gets from the volume, in scene units (the largest channel; 0: none; the shaders of the
  // inject never read it, the composite gets it in its push; VOL-LOOK-TUNE: it was the wetness, which no shader read); z g_lamp; w g_head (anisotropies)
  float mix[4] = {0.0f, 0.0f, 0.25f, 0.6f};
  // x extinction_scale: how much of the medium's absorption the composite applies (the game's fog already attenuates: 0.25); y lamp_strength;
  // z head_strength; w sun_strength (artist multipliers, 1 = the rules' look)
  float scale[4] = {0.25f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(VolWorld) == 112);

// Everything a frame needs from the producers. `valid` false: the frame is skipped (nothing recorded). The producers fill what they
// know; VOL-CORE fills the defaults first (an empty light list, the sun it read, a clear-air haze), so a producer may fill only its part.
struct VolInputs {
  bool valid = false;
  VolWorld world;
  VolSun sun;
  bool sun_valid = false;  // `sun` holds the cascades of this frame (the sun term is cascade shadowed); false: the sun is unshadowed
  std::array<VolLamp, kMaxLamps> lamps{};
  uint32_t lamp_count = 0;
  std::array<VolLamp, kMaxHeadlights> headlights{};
  uint32_t headlight_count = 0;
  std::array<VolShadowView, kMaxShadowViews> shadow_views{};
  uint32_t shadow_view_count = 0;
  uint32_t shadow_atlas_slot = kNoSlot;  // the bindless slot of the headlights' shadow atlas, kNoSlot: none
  float shadow_atlas_size[2] = {1.0f, 1.0f};
};

// ---------------------------------------------------------------------------
// the camera and the frame: VOL-CORE's side
// ---------------------------------------------------------------------------

struct VolCamera {
  float pos[3] = {};
  float right[3] = {1.0f, 0.0f, 0.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
  float forward[3] = {0.0f, 0.0f, -1.0f};
  float tan_x = 1.0f;  // tan of the half field of view, horizontal and vertical
  float tan_y = 1.0f;
};

// What a producer is told about the frame it fills (VolInputSource::Fill).
struct VolFrameContext {
  uint64_t frame = 0;            // the guest frame number
  Level level = Level::kOff;
  GridDesc grid;
  VolCamera camera;              // this frame's own camera where the vertex constants hold it, else the previous frame's
  bool camera_is_current = false;  // false: the previous frame's (the volume is then one frame behind a moving camera)
  uint32_t render_width = 0;     // the picture in host pixels
  uint32_t render_height = 0;
  bool upscaler = false;         // a temporal upscaler works on this frame
  float elapsed_s = 0.0f;        // seconds since the producer's last Fill (for a wind's integral); 0 on the first call
};

// The producers' plug: VOL-INPUTS implements it (volumetrics_inputs.h) and the core calls Fill once a frame at the first point, after it
// has filled the defaults. Render thread only. Fill may change any field of `out`; it must not keep the pointer.
class VolInputSource {
 public:
  virtual ~VolInputSource() = default;
  virtual void Fill(const VolFrameContext& frame, VolInputs* out) = 0;
};

// ---------------------------------------------------------------------------
// what the shaders read: VOL-SHADERS' side (GPU)
// ---------------------------------------------------------------------------
// One ByteAddressBuffer per frame slot (the inject pass: t0 of set 2), every member a multiple of 16 bytes, float4 aligned. The HLSL
// declares the same members in the same order as plain `float4`s read with Load4 at Offsets (volumetrics_common.hlsli).

struct VolGpuConstants {
  uint32_t grid[4];        // nx, ny, nz, tiles_x
  float depth[4];          // near_m, far_m, ln(far / near), 1 / ln(far / near)
  float history[4];        // x the history's far_m, y its ln(far / near), z the history weight in 0..1 (0: no history, never read), w unused
  float jitter[4];         // x, y the sub-voxel offset in 0..1, z the slice offset in 0..1 (a blue noise of the frame), w the frame number
  float cam_pos[4];        // xyz world position; w tan_x
  float cam_right[4];      // xyz; w tan_y
  float cam_up[4];
  float cam_fwd[4];
  float prev_pos[4];       // the history's camera, as above (w tan_x)
  float prev_right[4];     // (w tan_y)
  float prev_up[4];
  float prev_fwd[4];
  VolWorld world;
  VolSun sun;
  uint32_t counts[4];      // x lamp count, y headlight count, z shadow view count, w shadow atlas slot
  float shadow_atlas[4];   // x, y the headlights' atlas size in texels; zw unused
  std::array<VolLamp, kMaxLamps> lamps;
  std::array<VolLamp, kMaxHeadlights> headlights;
  std::array<VolShadowView, kMaxShadowViews> shadow_views;
};
static_assert(sizeof(VolGpuConstants) % 16 == 0);
static_assert(offsetof(VolGpuConstants, world) == 192);
static_assert(offsetof(VolGpuConstants, sun) == 192 + 112);

// The byte offsets the HLSL reads (the static_asserts above keep them honest; volumetrics_rules_test.cpp checks them all).
struct Offsets {
  static constexpr uint32_t grid = offsetof(VolGpuConstants, grid);
  static constexpr uint32_t depth = offsetof(VolGpuConstants, depth);
  static constexpr uint32_t history = offsetof(VolGpuConstants, history);
  static constexpr uint32_t jitter = offsetof(VolGpuConstants, jitter);
  static constexpr uint32_t cam_pos = offsetof(VolGpuConstants, cam_pos);
  static constexpr uint32_t prev_pos = offsetof(VolGpuConstants, prev_pos);
  static constexpr uint32_t world = offsetof(VolGpuConstants, world);
  static constexpr uint32_t sun = offsetof(VolGpuConstants, sun);
  static constexpr uint32_t counts = offsetof(VolGpuConstants, counts);
  static constexpr uint32_t shadow_atlas = offsetof(VolGpuConstants, shadow_atlas);
  static constexpr uint32_t lamps = offsetof(VolGpuConstants, lamps);
  static constexpr uint32_t headlights = offsetof(VolGpuConstants, headlights);
  static constexpr uint32_t shadow_views = offsetof(VolGpuConstants, shadow_views);
};

// ---------------------------------------------------------------------------
// the adaptors of the headlight rig's and the shadow replay's shapes (templates: no include of those headers here)
// ---------------------------------------------------------------------------

// A `Headlight` of headlight_rig.h (pos[3], dir[3], cos_outer, cos_inner, range, color[3], ...) as a VolLamp, shadow_view -1 (the shadow
// adaptor sets it).
template <class HeadlightT>
constexpr VolLamp LampFromHeadlight(const HeadlightT& h) {
  VolLamp lamp{};
  for (int i = 0; i < 3; ++i) {
    lamp.pos[i] = h.pos[i];
    lamp.dir[i] = h.dir[i];
    lamp.colour[i] = h.color[i];
  }
  lamp.range = h.range;
  lamp.cos_outer = h.cos_outer;
  lamp.cos_inner = h.cos_inner;
  lamp.shadow_view = -1;
  lamp.shadow_view_b = -1;
  return lamp;
}

// The frame's HeadlightSet (`lights`, `count`) into `out->headlights`, at most kMaxHeadlights.
template <class HeadlightSetT>
void FillHeadlights(const HeadlightSetT& set, VolInputs* out) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < set.count && i < kMaxHeadlights; ++i) {
    out->headlights[n++] = LampFromHeadlight(set.lights[i]);
  }
  out->headlight_count = n;
}

// The frame's LocalShadowFrame (`views`, `count`) and the atlas it is in into `out->shadow_views`, and each headlight's `shadow_view` and `shadow_view_b`
// (the first and the second valid view whose light_index is the light's index: a car's two headlamps; -1 when none). `frame` null: no shadows, every headlight lit.
template <class LocalShadowFrameT>
void FillShadowViews(const LocalShadowFrameT* frame, uint32_t atlas_slot, float atlas_width, float atlas_height, VolInputs* out) {
  out->shadow_view_count = 0;
  out->shadow_atlas_slot = kNoSlot;
  for (uint32_t i = 0; i < out->headlight_count; ++i) {
    out->headlights[i].shadow_view = -1;
    out->headlights[i].shadow_view_b = -1;
  }
  if (!frame || atlas_slot == kNoSlot) {
    return;
  }
  uint32_t n = 0;
  for (uint32_t i = 0; i < frame->count && i < kMaxShadowViews; ++i) {
    const auto& v = frame->views[i];
    VolShadowView& d = out->shadow_views[n];
    for (int k = 0; k < 16; ++k) {
      d.world_to_shadow[k] = v.world_to_shadow[k];
    }
    for (int k = 0; k < 4; ++k) {
      d.atlas_rect[k] = v.atlas_rect[k];
    }
    d.depth_bias = v.depth_bias;
    d.texel_size = v.texel_size;
    d.light_index = v.light_index;
    d.valid = v.valid;
    if (v.valid != 0 && v.light_index < out->headlight_count) {
      VolLamp& light = out->headlights[v.light_index];
      if (light.shadow_view < 0) {
        light.shadow_view = int32_t(n);
      } else if (light.shadow_view_b < 0) {
        light.shadow_view_b = int32_t(n);
      }
    }
    ++n;
  }
  out->shadow_view_count = n;
  out->shadow_atlas_slot = n != 0 ? atlas_slot : kNoSlot;
  out->shadow_atlas_size[0] = atlas_width;
  out->shadow_atlas_size[1] = atlas_height;
}

}  // namespace mcla::gpu::volumetrics
