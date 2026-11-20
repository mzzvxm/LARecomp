#pragma once
// ===========================================================================
// MCLA GPU renderer -- the occlusion's use of the G-buffer's normal plane: the rules and the C++ twin of hlsl/ao_gbuffer_normal.hlsli (GB-E)
// ===========================================================================
// perf/GBUFFER-SURVEY.md 4 and 7.2 (GB-E): the first consumers of the G-buffer. The ambient occlusion (ao.cpp, hlsl/ao_*), the bounce light and the contact shadows work in
// view space and took the surface's normal from the depth's neighbours (hlsl/ao_common.hlsli AoNormalFromImage) in each of their passes. Where a pixel has a normal in RT1
// (gbuffer_types.h: the WORLD normal, octahedral, 0 = no data) they read that: the edges are the surface's own (a depth edge no longer bends the normal across it), and a pass
// loads one texel in place of four depth samples and the cross product. Everywhere else, and when the G-buffer is off for a frame, the depth's way is exactly what it was.
//
// HOW (ao.cpp, hlsl/ao_normal_ps.hlsl): one pass after the depth chain makes a NORMAL IMAGE, R16G16_UNORM at the traced size, the unit VIEW-space normal of every pixel, octahedral
// (hlsl/gbuffer_common.hlsli GbOctEncode): RT1's texel turned into view space where it has one that faces the camera, the depth's normal where it does not. Every pass that took
// the depth's normal reads the image instead (a bindless slot in a field the pass does not otherwise use; 0 is "no image": the pass reconstructs as before, so a frame without the
// G-buffer, or with mcla_gpu_ao_gbuffer_normals off, is the plain passes to the bit). The contact shadows keep the picture's size beside a half size occlusion (ao_size.h): they
// get a full size image of their own. The bounce light's facing test reads the image at the SAMPLE's pixel in place of the depth one texel further along the slice.
//
// This file: the numbers and the pure decisions (the camera's rows, the choice of the texel at half size, the depth's low bits, when a frame uses the plane, what the frame asks the
// G-buffer for, the image sizes and the slot packing), all in plain C++ so that ao_gbuffer_test.cpp can run them and can compare the shared .hlsli with them.
//
// Threading: pure functions.
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <cstring>

#include "gbuffer_rules.h"
#include "gbuffer_types.h"

namespace mcla::gpu::ao_gbuffer {

namespace gb = mcla::gpu::gbuffer;

// ---------------------------------------------------------------------------
// the numbers (hlsl/ao_gbuffer_normal.hlsli: the test compares them)
// ---------------------------------------------------------------------------

// The least cosine between a G-buffer normal and the direction to the camera that is taken, and the cosine a normal below 0.05 is moved to.
inline constexpr float kRejectFacing = -0.25f;
inline constexpr float kMinFacing = 0.05f;

// The camera's rotation of the frame, to be taken from the vertex constants (this frame's own view inverse) or from the last frame's camera; where the two are not the same and the
// vertex constants are not known to hold the camera (ao.cpp vs_pose_trusted_), the frame cannot tell a turn from a constant that is not the camera, and the frame takes no G-buffer
// normals. The difference of the two rotations, the largest entry of the 3 x 3 (the camera's position, which moves with the car, is not in it): 0.05 is a turn of 3 degrees
// (sin 3 degrees = 0.052).
inline constexpr double kPoseStaleDifference = 0.05;
// The largest difference between an entry of the rotation parts (rows and columns 0 to 2) of two view inverses.
inline double RotationDifference(const double a[4][4], const double b[4][4]) {
  double worst = 0.0;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      worst = std::fmax(worst, std::fabs(a[r][c] - b[r][c]));
    }
  }
  return worst;
}

// The normal image goes back to the heap after this many frames that did not use it (a cutscene, the pause, a switch of the setting: not made again for each).
inline constexpr uint32_t kReleaseAfterFrames = 60;

// The format of the normal image: two 16 bit unorm channels, the octahedral code (4 bytes a pixel).
inline constexpr uint32_t kNormalImageBytesPerPixel = 4;

// ---------------------------------------------------------------------------
// the camera's rows
// ---------------------------------------------------------------------------

// What the normal pass turns a world direction into view space with: the camera's right, up and forward in world space. A row is a float4 for the push block's rows (w 0).
struct Rows {
  float x[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float y[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float z[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

// The game's view inverse (the matrix of constants c12-c15: row vectors, so row 0 is the camera's right in world space, row 1 its up and row 2 its BACK: the game's view space looks
// along -z) as the rows of this renderer's view space, which looks along +z (hlsl/ao_common.hlsli): x right, y up, z forward. Exactly what ao.cpp FillPush does to the sun's
// direction (view[j] = dir . row j, then z negated). False when an entry is not finite or a row is not a unit axis (the matrix is not a camera's).
inline bool RowsFromViewInverse(const double m[4][4], Rows* out) {
  for (int r = 0; r < 3; ++r) {
    double length_squared = 0.0;
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r][c])) {
        return false;
      }
      length_squared += m[r][c] * m[r][c];
    }
    const double length = std::sqrt(length_squared);
    if (!(length > 0.98 && length < 1.02)) {
      return false;
    }
  }
  for (int c = 0; c < 3; ++c) {
    out->x[c] = float(m[0][c]);
    out->y[c] = float(m[1][c]);
    out->z[c] = float(-m[2][c]);
  }
  out->x[3] = out->y[3] = out->z[3] = 0.0f;
  return true;
}

// The rows' choice of the frame: the vertex constants' view inverse when they are trusted (the frame's own camera), else the last frame's camera, which is this frame's
// when the camera does not move; an untrusted bank that differs from the last frame's camera by more than kPoseStaleDifference leaves no answer (kCameraStale).
enum class Camera : uint8_t {
  kNone,           // no camera of this kind of view yet
  kVertexBank,     // the frame's own, from the vertex constants (trusted)
  kPrevious,       // the last frame's camera
  kStale,          // the bank may hold a camera that turned (untrusted and different): no rows
};
struct CameraFacts {
  bool have_previous = false;       // the last frame's camera of this kind of view exists
  bool bank_present = false;        // a main view colour draw carried view inverse constants this frame
  bool bank_trusted = false;        // they have matched the anchor's camera in a frame the camera moved in
  bool bank_sane = false;           // they are a camera's matrix (RowsFromViewInverse accepts them)
  double bank_difference = 0.0;     // RotationDifference(bank, the last frame's view inverse)
};
constexpr Camera ChooseCamera(const CameraFacts& f) {
  if (f.bank_present && f.bank_trusted && f.bank_sane) {
    return Camera::kVertexBank;
  }
  if (!f.have_previous) {
    return Camera::kNone;
  }
  if (f.bank_present && f.bank_sane && !f.bank_trusted && f.bank_difference > kPoseStaleDifference) {
    return Camera::kStale;
  }
  return Camera::kPrevious;
}

// ---------------------------------------------------------------------------
// the normal of a pixel (the twin of AoGbToView / AoGbViewNormal)
// ---------------------------------------------------------------------------

inline gb::Vec3 ToView(gb::Vec3 world, const Rows& rows) {
  const gb::Vec3 x{rows.x[0], rows.x[1], rows.x[2]};
  const gb::Vec3 y{rows.y[0], rows.y[1], rows.y[2]};
  const gb::Vec3 z{rows.z[0], rows.z[1], rows.z[2]};
  return {gb::Dot(world, x), gb::Dot(world, y), gb::Dot(world, z)};
}

struct Pick {
  bool taken = false;
  gb::Vec3 normal;
};
// What a pixel at view position `pc` takes from the RT1 texel `rt1` (as a load of the plane returns it: texel / 65535). Not taken: no data, or a normal that faces away from the
// camera by more than kRejectFacing.
inline Pick PickViewNormal(gb::Vec2 rt1, gb::Vec3 pc, const Rows& rows) {
  Pick pick;
  if (!(rt1.x > 0.0f || rt1.y > 0.0f)) {
    return pick;
  }
  gb::Vec3 n = ToView(gb::OctDecode(rt1), rows);
  const gb::Vec3 to_camera = pc * (-1.0f / std::sqrt(gb::Dot(pc, pc)));
  const float facing = gb::Dot(n, to_camera);
  if (!(facing >= kRejectFacing)) {
    return pick;
  }
  if (facing < kMinFacing) {
    n = n + to_camera * (kMinFacing - facing);
    n = n * (1.0f / std::sqrt(gb::Dot(n, n)));
  }
  pick.taken = true;
  pick.normal = n;
  return pick;
}

// ---------------------------------------------------------------------------
// the depth's low bits (the twin of AoGbSourceOf / AoGbSetSource, and of the vehicle mark of ao_common.hlsli)
// ---------------------------------------------------------------------------

inline uint32_t BitsOf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
inline float FloatOf(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
inline bool IsVehicle(float depth) { return (BitsOf(depth) & 1u) != 0u; }
inline float SetVehicleBit(float depth, bool vehicle) { return FloatOf((BitsOf(depth) & ~1u) | (vehicle ? 1u : 0u)); }
inline uint32_t SourceOf(float depth) { return (BitsOf(depth) >> 1) & 3u; }
inline float SetSource(float depth, uint32_t index) { return FloatOf((BitsOf(depth) & ~6u) | ((index & 3u) << 1)); }

// The pixel of the depth plane (and of RT1, which has the depth's size) that a pixel of the traced image stands for. At full size it is the pixel; at half size the depth pass chose one
// of the block's four samples (index 0 to 3, top left, top right, bottom left, bottom right) and the plane is read at that one, kept inside the plane as the depth pass keeps it.
struct Pixel {
  int32_t x = 0;
  int32_t y = 0;
  bool operator==(const Pixel& o) const { return x == o.x && y == o.y; }
};
constexpr Pixel SourcePixel(Pixel pixel, bool half, uint32_t index, Pixel last) {
  Pixel p = pixel;
  if (half) {
    p.x = pixel.x * 2 + int32_t(index & 1u);
    p.y = pixel.y * 2 + int32_t((index >> 1) & 1u);
  }
  if (p.x > last.x) p.x = last.x;
  if (p.y > last.y) p.y = last.y;
  return p;
}

// ---------------------------------------------------------------------------
// the facing of a sample's surface (the twin of AoGbFacingFromNormal, and the depth's way for the comparison)
// ---------------------------------------------------------------------------

inline float FacingFromNormal(gb::Vec3 normal, gb::Vec3 axis, gb::Vec3 sp, gb::Vec3 dir) {
  gb::Vec3 in_plane = normal - axis * gb::Dot(normal, axis);
  const float length_squared = gb::Dot(in_plane, in_plane);
  if (length_squared < 1.0e-6f) {
    return 1.0f;
  }
  in_plane = in_plane * (1.0f / std::sqrt(length_squared));
  if (gb::Dot(in_plane, sp) > 0.0f) {
    in_plane = in_plane * -1.0f;
  }
  return gb::Saturate(4.0f * gb::Dot(in_plane, dir * -1.0f));
}

// What ao_main_ps.hlsl AoGiSample did before: the surface's tangent in the slice from the sample's own position `sp` and the position one texel on along the slice `sp2`, its normal
// by the cross product with the axis, turned to face the camera, the same weight. 1 where the tangent is not a direction (or the second depth is missing: the caller's rule).
inline float FacingFromTangent(gb::Vec3 sp, gb::Vec3 sp2, gb::Vec3 axis, gb::Vec3 dir) {
  const gb::Vec3 tangent = sp2 - sp;
  if (!(gb::Dot(tangent, tangent) > 1e-10f)) {
    return 1.0f;
  }
  gb::Vec3 normal = gb::Cross(tangent, axis);
  const float length_n = gb::Length(normal);
  if (!(length_n > 1e-6f)) {
    return 1.0f;
  }
  normal = normal * (1.0f / length_n);
  if (gb::Dot(normal, sp) > 0.0f) {
    normal = normal * -1.0f;
  }
  return gb::Saturate(4.0f * gb::Dot(normal, dir * -1.0f));
}

// ---------------------------------------------------------------------------
// when a frame uses the plane, and what the frame asks the G-buffer for
// ---------------------------------------------------------------------------

// The setting mcla_gpu_ao_gbuffer_normals (debug, live, on by default) with the effects that are on: the planes the occlusion asks the G-buffer for. RT1 only (the normal): the
// albedo the bounce light will want is the next step, and asking for it before a stage writes it only makes the sidecar bigger. The G-buffer makes the sidecar only while a consumer
// asks (GBuffer::Want), so with the setting off, or none of the three effects on, no image exists and no draw of the game changes.
constexpr uint32_t PlanesWanted(bool setting, bool occlusion_or_contact_on) {
  return (setting && occlusion_or_contact_on) ? gb::PlaneBit(gb::kPlaneNormal) : 0u;
}
constexpr uint32_t BouncePlanesWanted(bool setting, bool bounce_on) {
  return (setting && bounce_on) ? gb::PlaneBit(gb::kPlaneNormal) : 0u;
}

// Why a frame does or does not use the G-buffer's normals (the log says the reason when it changes).
enum class Why : uint8_t {
  kUsed,
  kSettingOff,        // mcla_gpu_ao_gbuffer_normals is off
  kNoGBuffer,         // the renderer has no G-buffer object (a build without it)
  kPauseView,         // the 4x views have no sidecar (gbuffer_rules.h Decide: kPauseView)
  kNoNormalPlane,     // the frame published no normal plane (nothing asked it in time, a cutscene, the memory cap, no stage writes it)
  kSizeDiffers,       // the plane's size is not the picture's (a render scale change in flight)
  kNoCamera,          // no rotation for the frame (no camera of this kind of view yet)
  kCameraStale,       // the vertex constants may hold a camera that turned, and are not known to
  kPipelinePending,   // the normal pass's pipeline is being made in the background (a frame never waits for a compile)
  kImageFailed,       // the normal image (or the shader of the pass) could not be made
  kCount,
};
constexpr const char* WhyName(Why why) {
  switch (why) {
    case Why::kUsed: return "used";
    case Why::kSettingOff: return "mcla_gpu_ao_gbuffer_normals is off";
    case Why::kNoGBuffer: return "no G-buffer in this renderer";
    case Why::kPauseView: return "the pause menu's / the map's / the garage's world has no sidecar";
    case Why::kNoNormalPlane: return "the frame has no normal plane (the G-buffer was not asked in time, a cutscene, the memory cap)";
    case Why::kSizeDiffers: return "the normal plane's size is not the picture's";
    case Why::kNoCamera: return "no camera of this kind of view yet";
    case Why::kCameraStale: return "the vertex constants' camera may have turned and is not known to be the camera";
    case Why::kPipelinePending: return "the normal pass's pipeline is still being made";
    case Why::kImageFailed: return "the normal image could not be made";
    case Why::kCount: break;
  }
  return "?";
}
struct FrameFacts {
  bool setting = true;
  bool gbuffer_present = true;
  bool play_view = true;
  bool frame_has_normal = false;   // GBufferFrame::Has(kPlaneNormal)
  uint32_t plane_width = 0;        // GBufferFrame::width / height
  uint32_t plane_height = 0;
  uint32_t render_width = 0;       // the picture's size in host pixels (Plan::render)
  uint32_t render_height = 0;
  Camera camera = Camera::kNone;
  bool pipeline_ready = true;
};
constexpr Why Decide(const FrameFacts& f) {
  if (!f.setting) return Why::kSettingOff;
  if (!f.gbuffer_present) return Why::kNoGBuffer;
  if (!f.play_view) return Why::kPauseView;
  if (!f.frame_has_normal) return Why::kNoNormalPlane;
  if (f.plane_width != f.render_width || f.plane_height != f.render_height) return Why::kSizeDiffers;
  if (f.camera == Camera::kNone) return Why::kNoCamera;
  if (f.camera == Camera::kStale) return Why::kCameraStale;
  if (!f.pipeline_ready) return Why::kPipelinePending;
  return Why::kUsed;
}

// ---------------------------------------------------------------------------
// the images, and the slots in the passes' push blocks
// ---------------------------------------------------------------------------

// What a frame holds: the normal image at the traced size, and one of the picture's size where the contact shadows keep that size beside a half size occlusion (ao_size.h; their
// march, their filter and the apply read it). Where the traced size is the picture's the one image serves all.
struct Images {
  bool ao = false;            // the image at the traced size
  bool full = false;          // the image at the picture's size
  uint64_t bytes = 0;
};
constexpr uint64_t NormalBytes(uint32_t width, uint32_t height) {
  return uint64_t(width) * height * kNormalImageBytesPerPixel;
}
constexpr Images ImagesFor(bool used, uint32_t ao_width, uint32_t ao_height, uint32_t render_width, uint32_t render_height, bool split_contact, bool apply) {
  Images images;
  if (!used) return images;
  images.ao = true;
  images.bytes = NormalBytes(ao_width, ao_height);
  if (split_contact && apply) {
    images.full = true;
    images.bytes += NormalBytes(render_width, render_height);
  }
  return images;
}

// Which of the images a consumer reads. The occlusion's passes read the traced one; the contact shadows the full size one when they have it (else the traced one); the apply pass, which
// runs at the picture's size, the full size one, or the traced one where that is the picture's size, and none where it is half size and the contact shadows are not at the picture's
// size: it then takes the normal from the depth as before (a half size image sampled for a full size pixel would take the other side of a depth edge).
enum class Which : uint8_t { kNone, kAo, kFull };
constexpr Which ContactReads(bool split_contact) { return split_contact ? Which::kFull : Which::kAo; }
constexpr Which ApplyReads(bool split_contact, bool traced_at_half) { return split_contact ? Which::kFull : (traced_at_half ? Which::kNone : Which::kAo); }

// The slot of a normal image in the high 16 bits of a word whose low 16 bits are another bindless slot or a small number (kBindlessTextureCount is 32768: a slot is 15 bits). 0 is
// "no image": the slot 0 is the heap's reserved null texture, never an allocated one (descriptors.h).
inline constexpr uint32_t kSlotShift = 16;
constexpr uint32_t PackHigh(uint32_t low, uint32_t slot) { return low | (slot << kSlotShift); }
constexpr uint32_t HighSlot(uint32_t word) { return word >> kSlotShift; }
constexpr uint32_t LowHalf(uint32_t word) { return word & 0xFFFFu; }

}  // namespace mcla::gpu::ao_gbuffer
