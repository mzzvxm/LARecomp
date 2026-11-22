#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer: THE FROZEN INTERFACE (package IF of perf/GBUFFER-SURVEY.md 7.2)
// ===========================================================================
// The hybrid path (perf/GBUFFER-SURVEY.md section 0): the game's forward lighting stays, and its lit pixel shaders, drawn as a shader
// variant, also write a thin G-buffer into sidecar render targets of the main view, in the SAME draw as the game's colour (no second draw,
// no render pass switch). Every other draw of the main view has those outputs masked (write mask 0). The consumers (ambient occlusion,
// the bounce light, the deferred lights, reflections, decals) read the sidecar images at the mid-scene depth copy, where the AO runs.
//
// THIS FILE, hlsl/gbuffer_common.hlsli, gbuffer_rules.h (the C++ twin of the .hlsli) and the constants block below are the interface the
// packages GB-A (core), GB-B (variant framework), GB-C and GB-D (the transforms), GB-E (the AO consumer), GB-F (materials), GB-G (cars),
// GB-H (lights) and GB-I (reflections) are written against. They are frozen: a package that needs another field or value says so in its
// report, it does not edit them. New CLASS values and new MODE / FLAG bits may be appended (never renumbered); the layouts, the formats and
// the encodings never change without every package agreeing.
//
// The planes (the survey's section 3.1), all in the camera's scene units, all host images owned by the renderer's GBuffer class (not EDRAM
// identities: no tile map, no aliasing detector), at the size and sample count of the main view's colour target:
//
//   RT1  normal    R16G16_UNORM   the WORLD normal, octahedral (hlsl/gbuffer_common.hlsli GbOctEncode). Cleared to 0 = "no data": a valid
//                                 code is never (0, 0) (GbRt1Encode keeps that value free), so a consumer tests `any(rt1 > 0)`.
//   RT2  albedo    R8G8B8A8_UNORM rgb = sqrt(albedo) (gamma 2: dark albedo keeps its steps), a = the lit weight (255: lit by the forward
//                                 lights, 0: emissive or unlit; the deferred lights add nothing where it is 0). Cleared to 0. The night stage multiplies the draw's weight
//                                 by the shader's own baked occlusion where the game multiplies its lights by it (the vertex alpha iColor1.w: shaderpack/variants/
//                                 gbuffer_night_albedo.h, GB-H2-COVER), so the weight is "the share of the forward lights that reach the pixel": 0 still means no light.
//   RT3  material  R8G8_UNORM     r = smoothness, g = the class (5 bits) and three flags (kMaterialFlag*), see MaterialWord. Cleared to 0.
//
// Phase 1 has RT1 only; RT2 and RT3 are in the interface so the encodings are settled once.
//
// What a consumer gets each frame: a GBufferFrame (below), published at the mid-scene depth copy, valid until the frame's end.
//
// Threading: render thread only (the types are plain data).
// ===========================================================================

#include <array>
#include <cstdint>

namespace mcla::gpu::gbuffer {

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;  // a bindless slot that is not there (the same value as kInvalidBindlessSlot)

// --- the planes -------------------------------------------------------------------------------------------------------------------------

enum Plane : uint32_t { kPlaneNormal = 0, kPlaneAlbedo = 1, kPlaneMaterial = 2, kPlaneCount = 3 };

// The formats, as the renderer's own names (the GBuffer class maps them to plume::RenderFormat; this header stays free of Plume so that the
// rules' tests build alone).
enum class PlaneFormat : uint8_t { kRg16Unorm, kRgba8Unorm, kRg8Unorm };
inline constexpr std::array<PlaneFormat, kPlaneCount> kPlaneFormats = {PlaneFormat::kRg16Unorm, PlaneFormat::kRgba8Unorm,
                                                                       PlaneFormat::kRg8Unorm};
constexpr uint32_t BytesPerSample(PlaneFormat format) {
  switch (format) {
    case PlaneFormat::kRg16Unorm: return 4;
    case PlaneFormat::kRgba8Unorm: return 4;
    case PlaneFormat::kRg8Unorm: return 2;
  }
  return 0;
}
// The bit of a plane in GBufferFrame::valid and in the planes masks.
constexpr uint32_t PlaneBit(Plane plane) { return 1u << uint32_t(plane); }
inline constexpr uint32_t kAllPlanes = (1u << kPlaneCount) - 1;

// What the renderer publishes for the consumers (GBuffer::Frame()). `valid` has a bit per plane that holds this frame's data in every pixel a G-buffer
// draw covered and "no data" (0) everywhere else; a plane that is not in it must not be read (its slot is kNoSlot).
struct GBufferFrame {
  uint32_t generation = 0;    // changes whenever the images are made again (a new size, a new sample count): drop what was cached from them
  uint32_t frame_serial = 0;  // the Scene's frame the data belongs to (low 32 bits)
  uint32_t width = 0;         // host pixels of the images (the main colour target's, render scale included)
  uint32_t height = 0;
  uint32_t host_samples = 1;  // 1, 2, 4 or 8: the images are Texture2DMS above 1 (host_pass_common.hlsli space 1)
  uint32_t sample0 = 0;       // the host sample that is the guest's sample 0 (EdramTargets::HostSampleOf), the one a consumer reads
  uint32_t valid = 0;         // PlaneBit() of the planes with data this frame
  std::array<uint32_t, kPlaneCount> slot = {kNoSlot, kNoSlot, kNoSlot};  // bindless texture slots (SHADER_READ while the frame is published)

  bool Has(Plane plane) const { return (valid & PlaneBit(plane)) != 0 && slot[plane] != kNoSlot; }
};

// --- the class, the flags, the material word ---------------------------------------------------------------------------------------------

// What a pixel is made of, for the consumers' choices (reflections, wetness, the specular lobe, the light's albedo term). 5 bits. Appended
// values are allowed; 0 is "no data" and stays so (a plane that was not written has class 0 in every pixel).
enum class SurfaceClass : uint8_t {
  kNone = 0,       // nothing written here: the consumer falls back (depth normals, estimated albedo)
  kWorld = 1,      // the city's opaque surfaces by default: walls, props
  kRoad = 2,       // asphalt (the puddle mask and the planar reflection are the game's own)
  kWetRoad = 3,    // a road with a reflection sampled: SSR eligible
  kSidewalk = 4,
  kTerrain = 5,
  kLod = 6,        // far buildings and ground: geometry only, a coarse normal
  kFoliage = 7,    // leaves, grass, two-sided
  kPaint = 8,      // a car's clear-coated paint
  kChrome = 9,     // chrome, rims, bright trim: F0 = albedo
  kMetal = 10,     // brushed metal, rotors, props of metal
  kGlass = 11,     // opaque lenses and the dark glass that is drawn opaque (blended glass is never written)
  kSkin = 12,      // pedestrians and characters
  kEmissive = 13,  // neon, signs, lamp glass, traffic signals: the lit weight is 0
  kInterior = 14,  // the store interiors' light model
  kWater = 15,
  kCount = 16,
};
inline constexpr uint32_t kClassBits = 5;
inline constexpr uint32_t kClassMask = (1u << kClassBits) - 1;  // 31: the largest class value there will ever be

// The three flags that ride with the class in RT3.g (bits 5, 6 and 7 of the byte) and in the material word (bits 13, 14 and 15).
inline constexpr uint32_t kMaterialFlagSsr = 1u;           // eligible for screen-space reflections
inline constexpr uint32_t kMaterialFlagAlphaCovered = 2u;  // an alpha tested or alpha to coverage draw: the pixel's edge is the sample coverage
inline constexpr uint32_t kMaterialFlagVehicle = 4u;       // part of a vehicle (the AO and the motion vectors treat vehicles apart)

// The material word of a draw (GBufferConstants::material): everything a plane besides the normal needs that is a property of the DRAW,
// the shader adds the per-pixel parts (the albedo from its diffuse fetch, the smoothness times a specular map):
//
//   bits  0..7    smoothness (unorm8, 0 rough .. 255 mirror): from the game's SpecularExponent through alpha = sqrt(2 / (exponent + 2))
//   bits  8..12   class (SurfaceClass)
//   bits 13..15   flags (kMaterialFlag* << 13)
//   bits 16..23   lit weight (unorm8): 255 lit by the forward lights, 0 emissive; written to RT2.a
//   bits 24..31   reserved, 0 (a metalness byte may take them)
// RT3 = (word & 0xFF, (word >> 8) & 0xFF) as unorm8: the class and the flags are in the one byte.
inline constexpr uint32_t kMaterialSmoothnessShift = 0;
inline constexpr uint32_t kMaterialClassShift = 8;
inline constexpr uint32_t kMaterialFlagsShift = 13;
inline constexpr uint32_t kMaterialLitShift = 16;

// --- the constants block -----------------------------------------------------------------------------------------------------------------

// The mode bits of a draw (GBufferConstants::mode): which planes this draw writes and how its normal is made. A variant writes a plane only
// when its bit is set; the host sets the bits from what the frame has (Phase 1: kModeWriteNormal alone).
inline constexpr uint32_t kModeWriteNormal = 1u << 0;
inline constexpr uint32_t kModeWriteAlbedo = 1u << 1;
inline constexpr uint32_t kModeWriteMaterial = 1u << 2;
inline constexpr uint32_t kModeFlipBackface = 1u << 3;     // a back face's normal is turned to face the viewer (two-sided leaves, fences)
inline constexpr uint32_t kModeDerivativeNormal = 1u << 4;  // the shader has no normal of its own: take it from the derivatives of the fragment's position

// The block a G-buffer variant reads after the 608 bytes of SharedConstants (constants.h kGBufferConstantsOffset): three 16-byte rows, c38 to c40 of the
// block, 48 bytes. It is uploaded by the draws that take a G-buffer variant, SharedConstants followed by this (Scene::FillVariantBlock). When
// another variant's block is in the same shader (the composition of GB-B) the block moves to where the composer puts it: the variants' HLSL reads it
// through GB_CONSTANTS_* names the transform defines, never through a hard coded offset.
//
//   row 0   uint4  mode, material, frame, reserved
//   row 1   float4 the view ray of a fragment, for the derivative normal: x = 2 tan(half fov x) / width, y = -tan(half fov x),
//                  z = -2 tan(half fov y) / height, w = tan(half fov y). For the fragment at SV_Position (px, py) with depth w = SV_Position.w
//                  (the view-space depth: 1 / w_clip), the view-space point is (px * x + y, py * z + w, 1) * (1 / w) (x right, y up, z forward).
//   row 2   float4 the camera's rotation, view space to world space, as a unit quaternion (x, y, z, w)
struct GBufferConstants {
  uint32_t mode = 0;      // kMode*
  uint32_t material = 0;  // the draw's material word
  uint32_t frame = 0;     // the frame's number (low bits), for a pass that wants noise
  uint32_t reserved = 0;
  float view_ray[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float camera_to_world[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  bool operator==(const GBufferConstants& o) const {
    return mode == o.mode && material == o.material && frame == o.frame && reserved == o.reserved &&
           view_ray[0] == o.view_ray[0] && view_ray[1] == o.view_ray[1] && view_ray[2] == o.view_ray[2] &&
           view_ray[3] == o.view_ray[3] && camera_to_world[0] == o.camera_to_world[0] &&
           camera_to_world[1] == o.camera_to_world[1] && camera_to_world[2] == o.camera_to_world[2] &&
           camera_to_world[3] == o.camera_to_world[3];
  }
};
static_assert(sizeof(GBufferConstants) == 48, "GBufferConstants is three rows of the block after SharedConstants");

// The rows' offsets inside the block, for the SPIR-V declarations (RawBufferLoad at base + offset) and the DXIL packoffset rows.
inline constexpr uint32_t kGBufferRowMode = 0;
inline constexpr uint32_t kGBufferRowViewRay = 16;
inline constexpr uint32_t kGBufferRowCamera = 32;

// --- the debug views ---------------------------------------------------------------------------------------------------------------------

// mcla_gpu_gbuffer_view: what the debug pass draws over the main view's colour at the mid-scene depth copy.
enum class View : uint8_t {
  kOff = 0,
  kNormal = 1,       // the decoded normal as a colour (n * 0.5 + 0.5)
  kAlbedo = 2,       // the albedo (RT2.rgb squared)
  kSmoothness = 3,   // RT3.r as grey
  kClass = 4,        // a colour per class and flag (the family picture)
  kCoverage = 5,     // written (white) or not (black): the missing-pixels view
  kLitWeight = 6,    // RT2.a as grey
  kMissing = 7,      // the picture of what no family covers yet: green where RT1 holds a normal, RED where the main depth holds geometry and RT1 does not (a shader with no
                     // variant, a mirror, a masked draw), black where there is neither (the sky); the survey's R2 "missing pixels" view
  kCount = 8,
};

}  // namespace mcla::gpu::gbuffer
