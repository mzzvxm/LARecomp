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
//   c64-c67  pixel to world: the inverse view-projection with the pixel-to-clip step folded in
//   c69+4k   to c72+4k: the previous frame's view-projection to guest pixels (y flipped, pixel
//            edge convention, no +0.5), with vehicle k's own motion folded in; k = 0 is the world
//
// The slots of vehicles that are not on screen hold garbage (NaN, 2e29, denormals), so a matrix k
// is only read when the main view wrote the stencil ID k in the frame (the draws that write
// "ref = 4k + {0, 1}") and its 16 numbers are sane. IDs of 24 and above are the world's.
//
// All pure functions on the bank, for the render thread. Nothing here touches the GPU.
// ===========================================================================

#include <cstdint>

#include "temporal_math.h"

namespace mcla::gpu::temporal {

inline constexpr uint32_t kVehicleMatrices = 24;
inline constexpr uint32_t kWorldId = 0;

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

  // c64-c67 as read: pixel (x, y, depth, 1) to world. A cross-check, not an input.
  Mat4 pixel_to_world;
  bool pixel_to_world_agrees = false;  // rows 2 and 3 equal those of pixel-to-clip * inverse(VP)

  // The previous frame's matrices (c69 + 4k).
  uint64_t stencil_ids = 0;        // bit n: the main view wrote stencil ID n before the anchor
  uint32_t previous_valid = 0;     // bit k: matrix k is usable (k < kVehicleMatrices)
  Mat4 previous_world;             // k = 0 as the game wrote it, for the history check
  // This frame's clip space to the previous frame's guest pixels, per ID: inverse(VP) * c69+4k,
  // computed in double and rounded; row-major, row vectors (prev = clip * M). Only the entries
  // with a bit in previous_valid are written.
  float clip_to_previous_pixels[kVehicleMatrices][16] = {};
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
// bytes) for a frame whose fetches are `fetch_width` x `fetch_height`, and the matrices of the
// stencil IDs in `stencil_ids`. Anything but kNone leaves `*out` half made.
Reject ExtractCamera(const uint8_t* ps_bank, uint32_t fetch_width, uint32_t fetch_height,
                     uint64_t frame, uint64_t stencil_ids, Camera* out);

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

// How close `now`'s c69-c72 (k = 0) is to the view-projection of the frame before times a pixel
// matrix of the frame's size, the largest difference over x, y and w (the columns the motion
// vectors use), in pixels: `flip` for the layout the census found (y flipped), `noflip` for y
// down. Both tiny when c69 is the previous frame's view-projection; huge when it is not. Needs
// `now.previous_valid & 1` and a valid `before`.
struct History {
  double flip = 0.0;
  double noflip = 0.0;
};
History CompareToHistory(const Camera& now, const Camera& before);
inline constexpr double kHistoryTolerance = 0.05;  // pixels

// The radical inverse of `index` (from 1) in `base`: the Halton sequence, in [0, 1).
double Halton(uint32_t index, uint32_t base);

}  // namespace mcla::gpu::temporal
