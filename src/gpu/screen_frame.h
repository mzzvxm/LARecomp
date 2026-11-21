#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: THE FROZEN INTERFACE (package SS-0a of perf/LIGHT-UNIFY.md section 5 and 11.1)
// ===========================================================================
// One module (screen_shared.h, ScreenShared) makes, once per frame at the mid-scene depth copy (the trigger), every screen-space product that more than one effect
// reads; the consumers read a ScreenFrame. It follows the pattern of the G-buffer (gbuffer_types.h, gbuffer.h): a consumer says what it wants with
// ScreenShared::Want(Consumer, products) (sticky: set it when the effect's setting changes, read at the next frame boundary), the frame is valid from the build to
// the frame's end (BeginFrame empties it), the images are in bindless slots, and a `generation` says when the set of images was replaced. Nothing is made unless
// something wants it.
//
// THIS FILE is the interface the packages SS-0 (adopt what the AO makes, add the Hi-Z), SS-1 (the colour captures, the history, the car history rewired), SS-2 (the AO's
// passes move into the owner), GB-I (SSR) and the consumers are written against. It is frozen: a package that needs another field or value says so in its report, it
// does not edit it. New products, consumers, flags and view modes may be APPENDED (never renumbered); the layouts, the formats and the meanings never change without every
// package agreeing. The header stays free of Plume so that the tests build alone (screen_shared_test.cpp pins every value below).
//
// THE PRODUCTS (LIGHT-UNIFY.md 5.2), host images in bindless slots, read as Texture2D in space 0 (hlsl/host_pass_common.hlsli; none is multisampled: the depth chain
// takes the guest's sample 0, the colour captures average the MSAA samples):
//
//   lin_depth       R32_FLOAT level 0 at the traced size, R16_FLOAT levels 1 and 2 at 1/2 and 1/4 of it: the linear view-space distance along z in the game's world units
//                   (kSky 60000 for the sky and anything the linearisation cannot place). Levels 1 and 2 are WEIGHTED MEANS that favour the farthest of the four texels
//                   (the AO's main pass reads them); only level 0 is a depth you can compare. The lowest THREE bits of a level 0 float are not depth: the vehicle mark (bit 0)
//                   and, when the G-buffer's normals are in use at half size, which of the 2x2 block's samples the pixel took (bits 1 and 2): one to seven units in the last
//                   place, far below anything that reads a depth, kDepthMarkMask is what a reader that needs a clean depth clears (the Hi-Z does). The traced size is the AO's
//                   rule (ao_size.h: half the picture's size above 1080 lines of render height and at the AO's high; the full size at its ultra).
//   lin_depth_full  the same at the picture's size (R32_FLOAT, level 0 only): present only while the contact shadows keep a larger size than the traced one.
//   hiz_min         R32_FLOAT, a chain of kHizMaxLevels at most (HizLevelCount for the size): level 0 is lin_depth's level 0 with the marks cleared, every level above it the MIN
//                   of the 2x2 texels below (nearest, never a mean: a mean over a depth edge hides the surface a ray hits), down to 1/64. Texel (x, y) of level L covers the
//                   level 0 texels [x << L, min((x + 1) << L, width)) x [y << L, ...): an odd edge is clamped, not dropped. One image (one slot) per level.
//   normal          R16G16_UNORM, the octahedral code (hlsl/gbuffer_common.hlsli GbOctDecode) of the unit VIEW-space normal at the traced size (GB-E's `ao normals` pass: the
//                   G-buffer's RT1 where valid, else the depth's); present only in the frames whose passes read it.
//   colour_pre      RGBA16F, the main colour averaged over the MSAA samples at the traced size, 3 levels, CAPPED (the bounce light's rule: a lamp or a neon is not a bounce
//                   source): before the AO's apply, only while the bounce light is on.
//   colour_post     RGBA16F, MSAA-averaged, UNCAPPED, after the AO apply and the deferred lights, with a mip chain for rough reflections: SS-1 makes it (ScreenShared::CapturePost), only
//                   while a consumer outside the car history wants it. Its size is the picture's up to 1080 lines of render height and half of it (rounded up) above (screen_rules.h
//                   ColourPostSize, the AO's height rule): the Image's width and height say, `traced` is the depth's. Each texel is the mean of the host samples (and of the 2x2 pixels at half size)
//                   of the main colour in the target's own units, a NaN counted as black and clamped to what a half float keeps.
//   history         SS-1: the last frame's colour_post (level 0: history_colour_slot) with the camera pair (camera.prev_view_projection); present only while a consumer wants it, and only when the
//                   last frame was the frame before, of the same kind of view and at the same size. history_depth_slot stays kNoSlot until SS-2 (the depth chain's images are the AO's).
//   camera          always filled while the frame is published (ScreenCamera below).
//
// What exists in a given frame is `valid`; a product that is not in it has slot kNoSlot and must not be read. SS-0 PUBLISHES what the AO already makes (lin_depth, lin_depth_full,
// normal, colour_pre: the AO still builds them) and makes hiz_min itself; it is published only in a frame whose depth chain ran (the AO's trigger frame, or a frame run for the
// screen products alone when only a consumer outside the AO wants them).
//
// Threading: render thread only (the types are plain data).
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::screen {

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;  // a bindless slot that is not there (the same value as kInvalidBindlessSlot and gbuffer::kNoSlot)

// What the depth chain stores for the sky and for anything the linearisation could not place (hlsl/ao_common.hlsli kAoSky).
inline constexpr float kSky = 60000.0f;

// The bits of a level 0 linear depth that are marks, not depth (see lin_depth above).
inline constexpr uint32_t kDepthMarkMask = 7u;

// --- the products -------------------------------------------------------------------------------------------------------------------------

enum Product : uint32_t {
  kLinDepth = 0,
  kLinDepthFull = 1,
  kHizMin = 2,
  kNormal = 3,
  kColourPre = 4,
  kColourPost = 5,
  kHistory = 6,
  kProductCount = 7,
};
// The bit of a product in ScreenFrame::valid and in the products masks.
constexpr uint32_t ProductBit(Product product) { return 1u << uint32_t(product); }
inline constexpr uint32_t kAllProducts = (1u << kProductCount) - 1;

constexpr const char* ProductName(Product product) {
  switch (product) {
    case kLinDepth: return "lin_depth";
    case kLinDepthFull: return "lin_depth_full";
    case kHizMin: return "hiz_min";
    case kNormal: return "normal";
    case kColourPre: return "colour_pre";
    case kColourPost: return "colour_post";
    case kHistory: return "history";
    case kProductCount: break;
  }
  return "?";
}

// The depth chain's levels (0: R32_FLOAT, 1 and 2: R16_FLOAT), the bounce light's colour chain, the colour_post mip chain, the Hi-Z chain.
inline constexpr uint32_t kDepthLevels = 3;
inline constexpr uint32_t kColourPreLevels = 3;
inline constexpr uint32_t kColourPostMaxLevels = 8;
inline constexpr uint32_t kHizMaxLevels = 8;
// The Hi-Z goes down to 1/64 of level 0 (six halvings: 7 levels), and no further than the smaller side allows.
inline constexpr uint32_t kHizHalvings = 6;

// The size of Hi-Z level `level` for a level 0 of width x height: halved with the odd edge rounded UP (the same rule as the AO's coarser levels, ao_size.h Half), never below 1.
constexpr uint32_t HizLevelSize(uint32_t extent, uint32_t level) {
  uint32_t size = extent < 1 ? 1 : extent;
  for (uint32_t i = 0; i < level; ++i) {
    size = (size + 1) / 2;
  }
  return size;
}
// How many levels the chain has: level 0 plus up to kHizHalvings halvings, while the smaller side still has a halving left (1920x1080: 7; 640x360: 7; 64x64: 7; 32x32: 6).
constexpr uint32_t HizLevelCount(uint32_t width, uint32_t height) {
  uint32_t smaller = width < height ? width : height;
  uint32_t levels = 1;
  while (levels <= kHizHalvings && smaller > 1) {
    smaller = (smaller + 1) / 2;
    ++levels;
  }
  return levels;
}

// --- the consumers and the registry -------------------------------------------------------------------------------------------------------

// The effects that read the products, each with the products it wants while it is on (ScreenShared::Want; 0 withdraws). Appended values are allowed.
enum class Consumer : uint8_t {
  kAmbientOcclusion,  // ao.cpp: lin_depth (+ normal where the G-buffer's normals are on)
  kContactShadows,    // ao.cpp: lin_depth (+ lin_depth_full beside a half size trace, normal)
  kBounceLight,       // the SSGI: lin_depth, normal, colour_pre
  kCarRtAo,           // car_rt_ao.h: lin_depth level 0
  kCarRtCheck,        // car_rt_scene.h's check pass (debug): lin_depth level 0
  kReflections,       // SSR (GB-I): lin_depth, hiz_min, normal, colour_post, history
  kCarHistory,        // car_selfrefl.h's history pass (SS-1): colour_post, lin_depth
  kCarRtRefl,         // car_rt_refl.h (SS-1): history, camera
  kVolumetrics,       // volumetrics.h: lin_depth (it may keep reading the depth product directly)
  kLocalLights,       // GB-H's light pass: lin_depth
  kDebugView,         // mcla_gpu_screen_view: the product the view shows (made by the setting, never by a caller)
  kCount,
};
constexpr const char* ConsumerName(Consumer consumer) {
  switch (consumer) {
    case Consumer::kAmbientOcclusion: return "ambient occlusion";
    case Consumer::kContactShadows: return "contact shadows";
    case Consumer::kBounceLight: return "bounce light";
    case Consumer::kCarRtAo: return "car ray traced occlusion";
    case Consumer::kCarRtCheck: return "car ray tracing check";
    case Consumer::kReflections: return "reflections";
    case Consumer::kCarHistory: return "car history";
    case Consumer::kCarRtRefl: return "car ray traced reflections";
    case Consumer::kVolumetrics: return "volumetric light";
    case Consumer::kLocalLights: return "local lights";
    case Consumer::kDebugView: return "debug view";
    case Consumer::kCount: break;
  }
  return "?";
}

// The Want registry: one mask of products a consumer, sticky until it is set again. Bits above kAllProducts are dropped.
class Wants {
 public:
  void Set(Consumer consumer, uint32_t products) {
    if (consumer < Consumer::kCount) products_[size_t(consumer)] = products & kAllProducts;
  }
  uint32_t Of(Consumer consumer) const { return consumer < Consumer::kCount ? products_[size_t(consumer)] : 0u; }
  uint32_t Union() const {
    uint32_t all = 0;
    for (uint32_t p : products_) all |= p;
    return all;
  }

 private:
  std::array<uint32_t, size_t(Consumer::kCount)> products_{};
};

// --- the frame ----------------------------------------------------------------------------------------------------------------------------

// The camera the products were made with. The AO's lens is the PREVIOUS frame's anchor camera (the camera exists only at the game's motion blur draw, which comes after the
// trigger); a consumer that needs this frame's own matrices reads the draws' constants as before. Matrices are row-major, row vectors (the game's and temporal::Mat4's
// convention), single precision.
inline constexpr uint32_t kCameraValid = 1u << 0;         // the lens and matrices below hold a camera
inline constexpr uint32_t kCameraHasPrevious = 1u << 1;   // prev_view_projection holds the matrix of the frame before (SS-1; 0 in SS-0)

struct ScreenCamera {
  float tan_half_fov[2] = {0.0f, 0.0f};        // x, y
  // The projection's depth rows P22, P23, P32, P33 of the view space to clip matrix: the linear depth of a stored depth (1 near, 0 far) is
  // t = (lin[2] - ndc * lin[3]) / (lin[0] - ndc * lin[1]), ndc = 1 - stored.
  float lin[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float near_plane = 0.0f;                     // view-space distance at stored depth 1
  float far_plane = 0.0f;                      // ... at stored depth 0
  float exposure_bias = 1.0f;                  // SS-1 (1 until a package fills it). CHECK: SS-0 has no source for it (the AO knows no exposure); where it comes from is SS-1's to find
  uint32_t flags = 0;                          // kCamera*
  float view_projection[16] = {};              // world to clip, without the jitter
  float prev_view_projection[16] = {};         // the frame before's (kCameraHasPrevious)
  float view_inverse[16] = {};                 // view space to world
};

// The frame's flags.
inline constexpr uint32_t kFramePause = 1u << 0;        // the pause menu's, the garage's or the map's world (a 4x view), not the play view
inline constexpr uint32_t kFrameHalfTraced = 1u << 1;   // `traced` is half of `render` (rounded up) on both axes

// What a consumer reads each frame (ScreenShared::Frame()): published at the trigger, valid until the frame's end. A product that is not in `valid` has slot kNoSlot.
struct ScreenFrame {
  uint32_t generation = 0;     // changes whenever the set of images is replaced (a new size or sample count, an image given back and made again): drop what was cached from them
  uint32_t frame_serial = 0;   // the Scene's frame the data belongs to (low 32 bits)
  uint32_t valid = 0;          // ProductBit() of the products with data this frame
  uint32_t flags = 0;          // kFrame*
  std::array<uint32_t, 2> traced = {0, 0};   // host pixels of level 0 of lin_depth, hiz_min, normal, colour_pre, colour_post
  std::array<uint32_t, 2> render = {0, 0};   // host pixels of the main view (lin_depth_full, the car history)
  uint32_t host_samples = 1;   // the main view's samples (1, 2, 4, 8)
  uint32_t sample0 = 0;        // the host sample that is the guest's sample 0
  uint32_t hiz_levels = 0;     // the Hi-Z chain's levels (HizLevelCount(traced)), 0 when hiz_min is not valid
  uint32_t colour_post_levels = 0;
  std::array<uint32_t, kDepthLevels> lin_depth_slot = {kNoSlot, kNoSlot, kNoSlot};
  uint32_t lin_depth_full_slot = kNoSlot;
  std::array<uint32_t, kHizMaxLevels> hiz_min_slot = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint32_t normal_slot = kNoSlot;
  std::array<uint32_t, kColourPreLevels> colour_pre_slot = {kNoSlot, kNoSlot, kNoSlot};
  std::array<uint32_t, kColourPostMaxLevels> colour_post_slot = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint32_t history_colour_slot = kNoSlot;   // the last frame's colour_post level 0
  uint32_t history_depth_slot = kNoSlot;    // the last frame's lin_depth level 0
  ScreenCamera camera;

  bool Has(Product product) const { return (valid & ProductBit(product)) != 0; }
};

// The debug view (mcla_gpu_screen_view, drawn over the main view's colour at the trigger): what it shows. Appended values are allowed.
enum class View : uint8_t {
  kOff = 0,
  kLinDepth = 1,    // lin_depth level 0 as a grey (near bright, a logarithmic scale), black for the sky
  kHizMin = 2,      // the Hi-Z level mcla_gpu_screen_view_level, the same grey, each texel stretched over the pixels it covers
  kHizCheck = 3,    // the Hi-Z level against lin_depth level 0: RED where the texel is deeper than a pixel it covers (never: the chain is a min), else green, brighter the more the min differs
  kNormal = 4,      // the normal image (0.5 + 0.5 n), black where the frame has none
  kVehicle = 5,     // the vehicle mark of level 0: white on a vehicle's pixels
  kColourPost = 6,  // SS-1: colour_post level mcla_gpu_screen_view_level (the same stretch over the pixels it covers), black where the frame has none
  kHistory = 7,     // SS-1: the history's colour (the last frame's colour_post), black where the frame has none
  kCount = 8,
};

}  // namespace mcla::gpu::screen
