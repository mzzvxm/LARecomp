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
#include <string>

#include "temporal_math.h"

namespace mcla::gpu::temporal {

// The camera cut rule (F4-DESIGN.md 2.5, the fork's, made speed- and scale-aware): the camera jumped,
// turned too far, or changed its lens too fast, each time judged against what the camera was doing
// in the steps before and against how much of the picture a step is worth.
//
// WHY A RULE ON THE CAMERA ALONE. The motion vectors are exact for any camera move over a still world
// (they come from this renderer's own camera history), so history only has to start again where the
// pictures stop being the same view: a teleport, a shot change, a camera handed over. The frame has
// no cheap measure of how much the new picture shares with the old one, so the rule estimates it
// from the poses, and each clause names the number it compares:
//
//   JUMP   a step longer than any of the last two and farther than they explain. Explained is "as
//          far as one of the last two steps" (two, because the pause camera and some cutscenes
//          step every second frame: 0, d, 0, d). Allowed: a floor plus the larger of the steps
//          that were not cuts (a step may be twice the recent one: frame times jitter). The floor
//          is the distance that shifts a quarter of the frame's height at a scene distance
//          (kCutShift, kCutDepth): at least kCutDepth, or the lens' near plane when that is
//          larger (the map camera's near plane is about 8,000 and the floor about 900 m, where
//          the drive's 0.2 gives about 9 m at a 25 degree field of view). The lens is the frame
//          before's: the one the vectors are measured against.
//   TURN   the same for the full rotation (axes, so a roll counts). Floor: 0.6 of the horizontal
//          field of view, between 15 and 30 degrees (30 was the whole rule).
//   LENS   the same for the vertical field of view, floor 15% (5% before: the pause camera's zoom
//          starts with 9% and the camera's return after a race with 10% steps).
//
// A jump or turn that follows the camera's own steps is not a cut: the game's opening fly-in moves
// the camera 28 units a frame growing to 100 and more, smoothly (the first form of the rule,
// "moved more than 25 units", fired on every frame of it), while a teleport (the map warp, 1,600
// units) is one step nothing predicted. The step of a cut widens nothing (CutMotion::Accept): a
// second, different cut right after it is judged as if the camera had stood still.
//
// What was measured (ports/run: the drive's camera traces cam-*/cam.csv and cm2-*/camera.csv, the
// lens and cut lines of um-race-dlss-before, um-hint-fsr, t1-on-race): the drive's largest step
// away from the previous one, outside the teleport, is 2.9 m (a frame-time hitch); the camera that
// settles after the teleport takes steps of 4.7 m, 3.0 m, 1.8 m; a cutscene's shot changes move
// 9 to 14 m with 47 to 173 degrees of turn; the pause camera opens with a 9% and 6% lens step and
// the camera leaves a race with 10, 9, 7 and 6% steps; at the end of the opening fly-in a 65 ms
// stall makes the camera's step 3.6 times the one before (129 m after 36), and the steps after it
// follow the frames' lengths (14, 33, 52 m): only the stall's own step is a jump, not the ones
// that come after it.
inline constexpr double kCutShift = 0.25;          // JUMP floor: this fraction of the frame's height
inline constexpr double kCutDepth = 80.0;          // ... at least this scene distance (metres)
inline constexpr double kCutSpeedTolerance = 1.0;  // allowed on top of the floor: this much of the recent step
inline constexpr double kCutTurnFraction = 0.6;    // TURN floor: this fraction of the horizontal field of view
inline constexpr double kCutTurnMin = 15.0 * 3.14159265358979323846 / 180.0;
inline constexpr double kCutTurnMax = 30.0 * 3.14159265358979323846 / 180.0;
inline constexpr double kCutFovFloor = 0.15;       // LENS floor: relative change of the vertical field of view
// The old 5% lens limit, still the one the occlusion (ao.cpp) holds a pause/map view back for until
// its lens is steady; the cut rule's own floor is kCutFovFloor.
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

struct Cut;

// What the camera did in the steps before the one being judged: the last two position steps, turns
// and lens changes, newest first. The caller keeps one between frames (the temporal system's run,
// the occlusion's history) and feeds it TestCut, then Accept with the result.
struct CutMotion {
  int steps = 0;               // how many of the entries below are known: 0, 1 or 2
  double step[2][3] = {};      // position now minus position before
  double turn[2] = {};         // radians, the full rotation between the frames
  double fov[2] = {};          // relative change of the vertical field of view (absolute value)
  bool cut[2] = {};            // the step was itself a cut

  void Clear() { *this = CutMotion{}; }
  // After a frame, with the result of its test. The step is remembered, a cut's too, but a cut's
  // step only explains a repeat of itself (a camera that goes on moving that way, every second
  // frame or not): it does not widen what the next step is allowed, so a different cut right
  // after a cut is still a cut. (A camera that takes the same 1,600 m step twice is a fast pan.)
  void Accept(const Cut& result);
};

// The cut rule between two consecutive frames' cameras. Each clause reports what it measured, the
// floor it needs to pass and what the steps before explain, so the log can say why (CutReason).
struct Cut {
  bool fires = false;
  bool by_distance = false;  // the clauses that fired (a cut may have several)
  bool by_turn = false;
  bool by_fov = false;
  // The jump clause. All in the game's units (metres).
  double distance = 0.0;     // units the position moved (this step)
  double deviation = 0.0;    // units this step is away from the nearest predicted one
  double predicted = 0.0;    // length of that prediction (an earlier step; 0: the camera stood still)
  double floor = 0.0;        // the least a jump has to be, from the lens (kCutShift, kCutDepth)
  double tolerance = 0.0;    // what the deviation may be: floor + the larger recent step
  double step[3] = {};       // position now minus position before
  // The turn clause, radians.
  double turn = 0.0;         // the full rotation (a roll counts); the forward vectors alone when the axes are unknown
  double turn_deviation = 0.0;
  double turn_floor = 0.0;
  double turn_tolerance = 0.0;
  double cosine = 1.0;       // between the forward vectors (the old measure, kept for the logs)
  // The lens clause, relative.
  double fov_change = 0.0;
  double fov_deviation = 0.0;
  double fov_floor = 0.0;
  double fov_tolerance = 0.0;
};
Cut TestCut(const Camera& now, const Camera& before, const CutMotion& motion);
// The earlier form: only the position change of the step before is known (three doubles), or null
// when it is not (the first step of a run: the camera is then predicted to stand still). The
// turn and the lens have no memory, so they are judged from their floors alone.
Cut TestCut(const Camera& now, const Camera& before, const double* previous_step);

// One line of why a cut fired (the clauses that did, with the numbers), for the log.
std::string CutReason(const Cut& cut);

// Guest clip space to guest pixels as the game's c69 block applies it (the census, CHECK 3): row
// vectors, x_pix = w/2 x + w/2 clip_w, y_pix = h/2 clip_w - h/2 y, depth and w columns kept.
Mat4 ClipToPixels(double width, double height);

// This frame's clip space to the previous frame's guest pixels through the camera alone: this
// frame's view-projection inverse, the previous frame's own view-projection (the history this
// renderer kept, not c69) and ClipToPixels of the frame's size. Row vectors: prev = clip * M, divide
// x and y by w. What the motion vectors use for every pixel.
Mat4 CameraToPreviousPixels(const Camera& now, const Camera& before);

// The radical inverse of `index` (from 1) in `base`: the Halton sequence, in [0, 1).
double Halton(uint32_t index, uint32_t base);

}  // namespace mcla::gpu::temporal
