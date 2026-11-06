#pragma once
// ===========================================================================
// MCLA GPU renderer -- the camera of a frame, read from the anchor's constants
// ===========================================================================
// The game's motion blur draw (the anchor, temporal_identities.h) is the one place on the GPU side
// where the frame's camera exists (F4-DESIGN.md 2.2). Its pixel shader constants, as the census
// of the real game read them (feature-parity/CENSUS-FINDINGS.md 1.2), row vectors (v' = v M):
//
//   c4-c7    view                          c12-c15  view inverse (c12 right, c13 up, c14 BACKWARD,
//   c8-c11   view-projection, unjittered            c15 position: the camera looks along -c14)
//   c62      (width, height, 1/width, 1/height) of the frame in guest pixels
//   c69-c72  the previous frame's view-projection to guest pixels (y flipped, pixel edge convention,
//            no +0.5): the game's own history. Only checked for being sane (a frame where it is not
//            starts the history again); the motion vectors use this renderer's own history.
//
// All pure functions on the bank, for the render thread. Nothing here touches the GPU.
// ===========================================================================

#include <cstdint>

#include "temporal_math.h"

namespace mcla::gpu::temporal {

// The camera cut rule (F4-DESIGN.md 2.5, the fork's, made velocity-aware): the camera jumped, turned
// more than 30 degrees (the cosine of the angle between the forward vectors below 0.866), or the
// vertical field of view changed by more than 5%.
//
// A jump is a step of more than 25 units that the step before does not predict: the game's opening
// fly-in moves the camera 28 units a frame growing to 100 and more, smoothly (the first form of
// the rule, "moved more than 25 units", fired on every frame of it), while a teleport (the map
// warp, 1,600 units) is one step nothing predicted. The prediction is the previous step; the
// allowed difference is 25 units plus half the previous step.
inline constexpr double kCutDistance = 25.0;
inline constexpr double kCutSpeedTolerance = 0.5;
inline constexpr double kCutCosine = 0.866;
inline constexpr double kCutFov = 0.05;

struct Camera {
  uint64_t frame = 0;      // the guest frame it was read in (TemporalSystem's count)
  uint32_t width = 0;      // c62.xy: the frame in guest pixels
  uint32_t height = 0;

  Mat4 view;                       // c4-c7
  Mat4 view_inverse;               // c12-c15
  Mat4 view_projection;            // c8-c11, without the jitter
  Mat4 view_projection_inverse;    // computed here, in double
  Mat4 projection;                 // view inverse * view-projection (view space to clip)

  double position[3] = {};         // row 3 of the view inverse
  double right[3] = {};            // row 0
  double up[3] = {};               // row 1
  double forward[3] = {};          // minus row 2: the view looks along -z
  double fov_y = 0.0;              // vertical, radians
  double aspect = 0.0;             // width over height of the projection
  double near_plane = 0.0;         // view-space distance at stored depth 1 (depth is 1 near, 0 far)
  double far_plane = 0.0;          // ... at stored depth 0

  // The game's previous-frame matrix of the world (c69) is usable: finite and neither the denormals
  // nor the huge numbers an unused slot holds.
  bool previous_valid = false;
};

// Why a set of constants was not taken as the camera.
enum class Reject : uint8_t {
  kNone,
  kFrameSize,       // c62.xy is not the size of the anchor's fetches
  kConstants,       // c4-c15 are not finite
  kViewProjection,  // the view-projection has no inverse that checks
  kViewInverse,     // view times view inverse is not the identity (relative bound)
  kLens,            // the projection gives no sane near, far or field of view
  kCount,
};
const char* RejectName(Reject reject);

// Reads the camera from the anchor's pixel bank (`ps_bank`: 256 registers of raw big-endian
// bytes) for a frame whose fetches are `fetch_width` x `fetch_height`. Anything but kNone leaves
// `*out` half made.
Reject ExtractCamera(const uint8_t* ps_bank, uint32_t fetch_width, uint32_t fetch_height,
                     uint64_t frame, Camera* out);

// The cut rule between two consecutive frames' cameras. `previous_step` is the position change of
// the step before (three doubles), or null when it is not known (the first step of a run): the
// camera is then predicted to stand still.
struct Cut {
  bool fires = false;
  bool by_distance = false;  // the clauses that fired (a cut may have several)
  bool by_turn = false;
  bool by_fov = false;
  double distance = 0.0;     // units the position moved (this step)
  double deviation = 0.0;    // units this step is away from the predicted one
  double tolerance = 0.0;    // what the deviation may be
  double cosine = 1.0;       // between the forward vectors
  double fov_change = 0.0;   // relative
  double step[3] = {};       // position now minus position before
};
Cut TestCut(const Camera& now, const Camera& before, const double* previous_step);

// Guest clip space to guest pixels as the game's c69 block applies it (the census, CHECK 3): row
// vectors, x_pix = w/2 x + w/2 clip_w, y_pix = h/2 clip_w - h/2 y, depth and w columns kept.
Mat4 ClipToPixels(double width, double height);

// A draw's vertex shader constants read as the game's per-object set the fork's frame capture
// described (launcher/dev/frame-structure-2026-10-16.md, section 4: c0-c3 World, c4-c7 WorldView,
// c8-c11 WorldViewProjection, c12-c15 ViewInverse with the camera position shared): one register
// per row, 16 floats each, row-major as the registers lie. Nothing measured this layout in
// LARecomp; CompareVsMatrices says, per sample, whether it holds against the anchor's camera. That
// decides whether the camera exists before the post chain, where the upscale could then run
// (TemporalSystem::StandInForPostChain says why it does not yet).
struct VsMatrices {
  float world[16] = {};
  float world_view[16] = {};
  float world_view_projection[16] = {};
  float view_inverse[16] = {};
};
void ReadVsMatrices(const uint8_t* vs_bank, VsMatrices* out);

// How far a sample is from the anchor's camera: `view_inverse` is the largest entry difference
// from the anchor's view inverse in units of max(1, its translation); `projection` is the largest
// entry difference between WorldView * projection and WorldViewProjection in units of that
// matrix's largest entry. Infinite when a matrix is not finite.
struct VsProbe {
  double view_inverse = 0.0;
  double projection = 0.0;
  bool ViewInverseMatches() const { return view_inverse < 1e-5; }
  bool ProjectionMatches() const { return projection < 1e-3; }
};
VsProbe CompareVsMatrices(const VsMatrices& vs, const Camera& cam);

// This frame's clip space to the previous frame's guest pixels through the camera alone: this
// frame's view-projection inverse, the previous frame's own view-projection (the history this
// renderer kept, not c69) and ClipToPixels of the frame's size. Row vectors: prev = clip * M, divide
// x and y by w. What the motion vectors use for every pixel.
Mat4 CameraToPreviousPixels(const Camera& now, const Camera& before);

// The radical inverse of `index` (from 1) in `base`: the Halton sequence, in [0, 1).
double Halton(uint32_t index, uint32_t base);

}  // namespace mcla::gpu::temporal
