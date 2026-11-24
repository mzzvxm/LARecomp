#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player's CABIN and the probe field's bounce light (INTERIOR-CAM-FIX, perf/BACKLOG-opt.md INTERIOR-CAM-FX)
// ===========================================================================
// WHAT THE OWNER SAW (2026-11-23): orange-brown blotches and streaks on the player's hands in the cockpit camera, in the sunlit road's colour, plus grain. The A/B chain
// (perf/scripts/chain-interior-cam.ps1, perf/out/INTERIOR-CAM.txt) found NO bounce light on the hands in the frozen day frames above the metric's own noise floor (the control arm 'auto2' adds
// as much as 'auto'), and the orange rims on the finger edges and the grain are in the picture with every effect off (the game's own hand shading: the 'floor' arm). The night numbers of the
// 4K runs were different frames (the first arms ran before the HUD and the exposure had settled), not light. So nothing in those frames needed taking away. But the design has a hole the
// pictures did not reach, and it is this file's:
//
//   The probe field TRACES THE CITY ONLY (hlsl/gi_update_trace_cs.hlsl: mask 0x80; the cars are no occluders), so the player's cabin does not exist for it: a probe in or beside the car sees
//   the sky and the street through the roof, and the apply (hlsl/gi_apply_ps.hlsl) lights EVERY pixel the G-buffer has an albedo for, the driver's skin and the dash included, with the open
//   street's irradiance at that place. Where the street gives the cabin something (a sunlit canyon wall beside the car, a street lamp's pool at night, and with the emitters of feat/gi-bounce
//   the lamps, neon and headlights in the probes) the inside of a ROOFED car would be lit through its roof. The screen-space bounce light has no such hole: it gathers what is ON the
//   picture, and a roof is never on the cockpit's picture, so what it carries into the cabin comes in through the glass, which is right.
//
// THE RULE. A closed cabin shields its inside from the street; the glass lets part of it in. So the field's bounce on the CABIN'S OWN surfaces is multiplied by a share:
//
//     cabin(pixel) = 1 - (1 - kOpenShare) x inside(pixel) x interior x closed      (a blend from 1 to kOpenShare)
//
//   inside     the pixel's world position is inside the player's car's box (CarFrame's world box of the body's meshes), with a soft edge of kEdge so that the door's skin is not a line;
//   interior   the CAMERA is in the cabin: the eye inside that box (grown by kBoxGrow, the RTAO contract's interior test, perf/RTAO.md 3.7) with a roof above it (headroom = the box's top less
//              the eye's height, between kHeadMin and kHeadMax: a hood or bumper camera sits low or outside and is NOT an interior camera; no box, no camera in it: no rule);
//   closed     1 for a car with a roof. The host passes 1 today: nothing here knows whether the car is a convertible (CHECK, below); 0 puts the rule out;
//   surface    only a pixel that is a part of a VEHICLE and is not the car's exterior: the G-buffer's class (RT3) is not paint, chrome, glass, emissive or no data (the hood seen through the
//              windscreen, the mirrors' housing and the other cars keep today's light; the driver's skin, the dash, the wheel, the seats and the trim are the cabin).
//
//   OPENINGS. The shell is not a vault: kOpenShare (0.3) of the street's bounce still reaches the inside, the share of the sphere round a driver that is glass (the windscreen, the side and rear
//   windows). It is a floor: nothing here takes the cabin's bounce to 0. An open roof or a convertible takes the rule away (closed = 0), the cabin then gets the field's light as before. The
//   game's own lighting (the sun through the glass, the ambient, the car's sun shadow) is not touched by this rule, and neither is the screen-space bounce light, which sees the windows on the picture.
//
// IDENTITY. With the setting off (mcla_gpu_gi_cabin), no player's car in the frame, no known box, a camera that is not inside the car, or a closed share of 0, the rows are OffRows() (share 1) and the
// shader reads nothing more: the apply's colour is the one it was, to the bit. A pixel outside the box or of an exterior class takes the term 1.
//
// WHERE IT LIVES. Two rows of 16 bytes after the apply's 9 (gi_apply_rules.h kBufferBytes, 464): the apply's own upload buffer is 512 bytes, so they fit; the per-pixel pass's buffer copies only the
// first 464 bytes and puts its own rows there, so it never sees these. Row 0: the box's minimum xyz and the cabin's share (1: no rule); row 1: the box's maximum xyz and the edge's width. The material
// plane's (RT3) bindless slot rides in the push block's last word above the flags and the view (bits 16-31, 0xFFFF none).
//
// THIS FILE IS THE TWIN of hlsl/gi_cabin_common.hlsli: gi_cabin_test.cpp compiles the .hlsli as C++ (through a shim of the HLSL types) and compares each function with the one here on random inputs.
// It includes only gbuffer_types.h (the class numbers). Threading: render thread only (plain data and pure functions).
//
// CHECK (nothing here was run: no build, no game; the rerun is perf/scripts/chain-interior-cam.ps1 on the exe that holds this, with `mcla_gpu_gi_cabin` as the new arm):
//   C1  CarFrame's box is a world AABB of the body's meshes: a car turned 45 degrees has a box larger than the car; the inside test is then generous at the corners. The pixels there that are
//       vehicle parts but not exterior (tyres' rubber, plastic trim) are not seen from the cockpit.
//   C2  the headroom window (kHeadMin 0.05 m, kHeadMax 0.55 m) against the real cars' cockpit eyes and roofs (spoilers and antennas raise the box's top): read the log's first line.
//   C3  convertibles and open roofs: nothing tells the host the roof is not there; the cabin probe of perf/RTAO.md 3.7 (64 rays, CabinClosed) is the verifier to feed `closed` with, when it exists.
//   C4  the material plane (RT3) is in the frame only when some consumer asked for it (the light pass, the reflections, Ray Reconstruction): without it the rule is off for that frame.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "gbuffer_types.h"

namespace mcla::gpu::gi_cabin {

// ---------------------------------------------------------------------------
// the numbers (hlsl/gi_cabin_common.hlsli's #defines are these: gi_cabin_test.cpp compares them)
// ---------------------------------------------------------------------------

inline constexpr float kOpenShare = 0.30f;   // the share of the street's bounce that reaches the inside of a closed cabin (the glass's share of the sphere round a driver)
inline constexpr float kBoxGrow = 0.10f;     // the eye counts as inside the car's box up to this far outside it, metres
inline constexpr float kEdge = 0.06f;        // a pixel this close (metres) to a face of the box is blended back to the outside's share
inline constexpr float kHeadMin0 = 0.0f;     // the headroom (the box's top less the eye's height, metres): 0 at and under this ...
inline constexpr float kHeadMin1 = 0.05f;    // ... 1 from this on (an eye at the roofline is no cockpit's)
inline constexpr float kHeadMax0 = 0.55f;    // ... 1 up to this ...
inline constexpr float kHeadMax1 = 0.75f;    // ... 0 from this on (a hood or bumper camera sits far under the roof)

// The G-buffer's flag bit of a vehicle's part (gbuffer_types.h kMaterialFlagVehicle) and the classes that are the car's EXTERIOR (or nothing): hlsl GIC_FLAG_VEHICLE, GIC_CLASS_*.
inline constexpr uint32_t kFlagVehicle = gbuffer::kMaterialFlagVehicle;

// ---------------------------------------------------------------------------
// where the rows go
// ---------------------------------------------------------------------------

inline constexpr uint32_t kRows = 2;
inline constexpr uint32_t kRowBytes = 16;
inline constexpr uint32_t kBytes = kRows * kRowBytes;           // 32
inline constexpr uint32_t kOffsetBytes = 464;                   // gi_apply_rules.h kBufferBytes: right after the apply's 9 rows (gi_cabin_test.cpp and gi_apply.cpp compare them)
inline constexpr uint32_t kMaterialSlotShift = 16;              // the push block's last word: the material plane's slot in bits 16-31
inline constexpr uint32_t kNoMaterialSlot = 0xFFFFu;

// The rows as the shader reads them: [0..2] the box's minimum, [3] the cabin's share (1: no rule), [4..6] the box's maximum, [7] the edge's width.
using Rows = std::array<float, 8>;

inline float Saturate(float x) { return std::min(std::max(x, 0.0f), 1.0f); }
inline float SmoothStep(float a, float b, float x) {
  const float t = Saturate((x - a) / (b - a));
  return t * t * (3.0f - 2.0f * t);
}

// The rows that say "no cabin": the box empty, the share 1. The shader's first test is `share < 1`, so it reads nothing else.
inline Rows OffRows() { return Rows{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, kEdge}; }

// How much the camera is an INTERIOR camera, 0 to 1: the eye inside the player's box (grown by kBoxGrow, the RTAO contract's rule) and under a roof (the headroom's window). World y is up.
inline float InteriorWeight(const float eye[3], const double lo[3], const double hi[3]) {
  for (int a = 0; a < 3; ++a) {
    if (!(double(eye[a]) >= lo[a] - kBoxGrow && double(eye[a]) <= hi[a] + kBoxGrow)) {
      return 0.0f;
    }
  }
  const float headroom = float(hi[1] - double(eye[1]));
  return SmoothStep(kHeadMin0, kHeadMin1, headroom) * (1.0f - SmoothStep(kHeadMax0, kHeadMax1, headroom));
}

// The cabin's share the rows carry: the blend from 1 to kOpenShare by interior x closed (kOpenShare x k + (1 - k): exactly 1 at k = 0 and exactly kOpenShare at k = 1). `closed` is 0 (no roof: no rule) to 1 (a roof).
inline float CabinShare(float interior, float closed) {
  const float k = Saturate(interior) * Saturate(closed);
  return kOpenShare * k + (1.0f - k);
}

// The rows for a frame: OffRows() unless the rule is on, the car's box is known and sane, the eye is finite, and the share is under 1.
inline Rows MakeRows(bool on, const float eye[3], bool box_valid, const double lo[3], const double hi[3], float closed) {
  if (!on || !box_valid) {
    return OffRows();
  }
  for (int a = 0; a < 3; ++a) {
    if (!std::isfinite(eye[a]) || !std::isfinite(lo[a]) || !std::isfinite(hi[a]) || !(hi[a] > lo[a])) {
      return OffRows();
    }
  }
  const float share = CabinShare(InteriorWeight(eye, lo, hi), closed);
  if (!(share < 1.0f)) {
    return OffRows();
  }
  return Rows{float(lo[0]), float(lo[1]), float(lo[2]), share, float(hi[0]), float(hi[1]), float(hi[2]), kEdge};
}

// ---------------------------------------------------------------------------
// the pixel's maths (the twin of hlsl/gi_cabin_common.hlsli)
// ---------------------------------------------------------------------------

// How far inside the box a point is, 0 to 1: the smooth step of its distance to the nearest face over `edge`, per axis, multiplied; 0 outside the box.
inline float EdgeWeight(const float pos[3], const float lo[3], const float hi[3], float edge) {
  const float e = std::max(edge, 1.0e-4f);
  float w = 1.0f;
  for (int a = 0; a < 3; ++a) {
    const float d = std::min(pos[a] - lo[a], hi[a] - pos[a]);
    const float t = Saturate(d / e);
    w *= t * t * (3.0f - 2.0f * t);
  }
  return w;
}

// A surface of the player's car's inside (as opposed to its exterior, another car, the world): a vehicle's part (the G-buffer's flag) that is not paint, chrome, glass, emissive or no data.
inline bool IsCabinSurface(uint32_t cls, uint32_t flags) {
  if ((flags & kFlagVehicle) == 0) {
    return false;
  }
  switch (static_cast<gbuffer::SurfaceClass>(cls)) {
    case gbuffer::SurfaceClass::kNone:
    case gbuffer::SurfaceClass::kPaint:
    case gbuffer::SurfaceClass::kChrome:
    case gbuffer::SurfaceClass::kGlass:
    case gbuffer::SurfaceClass::kEmissive: return false;
    default: break;
  }
  return true;
}

// The term the field's bounce at `pos` is multiplied by: 1 where there is no rule (share 1), the share where the point is inside the box (blended at its faces), 1 outside.
inline float Term(const Rows& rows, const float pos[3]) {
  const float share = rows[3];
  if (!(share < 1.0f)) {
    return 1.0f;
  }
  return 1.0f + (share - 1.0f) * EdgeWeight(pos, &rows[0], &rows[4], rows[7]);
}

// The term for a pixel of class `cls` with the G-buffer's flags `flags`: Term for the cabin's surfaces, 1 for every other.
inline float ShareFor(const Rows& rows, const float pos[3], uint32_t cls, uint32_t flags) { return IsCabinSurface(cls, flags) ? Term(rows, pos) : 1.0f; }

// The push block's last word with the material plane's slot in its upper half (kNoMaterialSlot, and a slot that does not fit: none).
constexpr uint32_t PackMaterialSlot(uint32_t info_w, uint32_t slot) {
  const uint32_t s = slot < kNoMaterialSlot ? slot : kNoMaterialSlot;
  return (info_w & 0xFFFFu) | (s << kMaterialSlotShift);
}

static_assert(kBytes == 32 && kOffsetBytes + kBytes <= 512, "the cabin's rows fit the apply's 512 byte upload buffer after its 464");
static_assert(kOpenShare > 0.0f && kOpenShare < 1.0f && kHeadMin0 < kHeadMin1 && kHeadMin1 < kHeadMax0 && kHeadMax0 < kHeadMax1, "the share is a floor and the headroom's window is ordered");
static_assert(PackMaterialSlot(0x0309u, 0x1234u) == 0x12340309u && PackMaterialSlot(0x0309u, 0xFFFFu) == 0xFFFF0309u && PackMaterialSlot(0x0309u, 0x20000u) == 0xFFFF0309u, "the slot rides in the upper half");

}  // namespace mcla::gpu::gi_cabin
