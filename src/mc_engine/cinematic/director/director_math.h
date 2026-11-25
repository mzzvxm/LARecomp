#pragma once
//
// The Director's pure helpers (DIRECTOR-DESIGN.md sections 2.5, 3 and 4.2): the camera's view-projection and a point's projection (the panel's gizmos),
// the aim of a look-at, the world time a scene time stands for, the reduction of a recorded drive to a route, the lexical rule for the paths a command
// may write, which scenes can be scrubbed exactly, and how a scene's keys read as timeline rows. No game, no sockets, no log: larecomp_director_test
// builds it on its own (with scene.cpp, camera.cpp and path_follow.cpp).
//

#include <string>
#include <string_view>
#include <vector>

#include "director.h"
#include "../scene.h"

namespace mc::cinematic::director {

// ---------------------------------------------------------------------------------------------------------------- the camera

// The camera's axes for a pose in the scene file's words (yaw 0 looks along +Z, 90 along +X; pitch + looks up; roll + lifts the right side): the world is
// right-handed (x east, y up, z south), the right is forward x up.
void PoseAxes(const SnapPose& pose, double right[3], double up[3], double forward[3]);
// The angles of a camera that looks along `forward` with `up` above (roll from up). yaw in -180..180, pitch -90..90 (yaw is `keep_yaw` looking straight up or down).
void PoseFromAxes(const double pos[3], const double right[3], const double up[3], const double forward[3], double fov, double keep_yaw, SnapPose* out);

// World -> clip, row-major (clip = M * [x y z 1]^T), Direct3D conventions: x and y in [-w, w], z in [0, w], w = the distance along the camera's forward.
// `fov` is vertical degrees, `aspect` width over height.
void ViewProjection(const SnapPose& pose, double aspect, double near_plane, double far_plane, double out[16]);
// A world point in the picture: ndc x and y in [-1, 1] (y up), depth in [0, 1], w the distance along the camera's forward. False when the point is behind the
// camera (w <= 0): the ndc values are then not meaningful.
bool ProjectPoint(const double view_proj[16], const double point[3], double* ndc_x, double* ndc_y, double* depth, double* w);

// The yaw and pitch (degrees, the scene's meaning) of a camera at `from` that looks at `to`. The same point gives yaw 0, pitch 0.
void LookAtAngles(const double from[3], const double to[3], double* yaw, double* pitch);

// ---------------------------------------------------------------------------------------------------------------- time

// The world time the scene's time t stands for: the integral of the scene's time scale track from 0 to t (a scene with none: t). A car on a route is a closed
// form of THIS time, so a scrub lands where a run would have been. Steps of `step` seconds.
double WorldTimeAt(const Scene& scene, double t, double step = 1.0 / 240.0);

// Whether a seek to the scene's time `t` is exact, and if not why: no brain-mode car, no traffic, no peds in the scene at that time.
struct ScrubInfo {
    bool scrubbable = true;
    std::string hint;
};
ScrubInfo ScrubbableAt(const Scene& scene, double t);

// ---------------------------------------------------------------------------------------------------------------- routes

struct WayPoint {
    double x = 0, y = 0, z = 0;
    double speed = 14.0;  // metres a second
};
// A drive reduced to the points its shape needs (Ramer-Douglas-Peucker on x, y, z within `tolerance_m`; the first and last always stay; a point where the
// speed changes by more than `speed_step` m/s also stays).
std::vector<WayPoint> ReduceRoute(const std::vector<WayPoint>& points, double tolerance_m = 0.25, double speed_step = 2.0);
double RouteLength(const std::vector<WayPoint>& points);  // metres along x and z
// A route file's text (path_follow.h: `x,z,speed,y`), with `header` as comment lines at its top.
std::string FormatRouteFile(const std::vector<WayPoint>& points, const std::string& header = {});

// ---------------------------------------------------------------------------------------------------------------- paths a command may write

// The lexical rule for a path an agent gives (DIRECTOR-PROTOCOL.md "Paths"): relative, no `..` or `.` parts, no drive or colon, no leading separator, no
// device names, at most 200 characters, and at least one part. `normalised` has forward slashes and no repeated ones.
struct PathCheck {
    bool ok = false;
    std::string normalised;
    std::string error;
};
PathCheck CheckRelativePath(std::string_view path);

// ---------------------------------------------------------------------------------------------------------------- the scene as rows

EventKind KindOfTrack(Track track);
// The statement of line `line` (1-based) of `text` without its comment and surrounding blanks; "" when there is no such line.
std::string StatementText(std::string_view text, int line);
// The timeline rows of a scene: every key with its line, time, track, kind and short text.
std::vector<SnapEvent> TimelineEvents(const Scene& scene, std::string_view file_text);
// The ids of the cars a scene names (a `car <id> ...` key), in the order they first appear.
std::vector<std::string> SceneCarIds(const Scene& scene);

}  // namespace mc::cinematic::director
