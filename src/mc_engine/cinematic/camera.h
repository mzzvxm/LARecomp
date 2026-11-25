#pragma once
//
// The camera rig of the trailer toolkit (work package M1b; feature-parity/CINEMATIC-DESIGN.md sections 3.1 and 3.3).
//
// This file is the pure half, like scene.h: the maths (vectors, rotations, easing, a centripetal Catmull-Rom spline, the
// follow lag), the parser of the scene's `cam` and `cut` keys into a camera track, and the rig that turns a track and a scene
// time into a camera. It knows nothing of the game, the SDK or the log, so camera_test.cpp builds it on its own.
// camera_runtime.cpp is the glue that puts the rig's camera into the game's final camera slots (0x822C0868).
//
// THE KEYS (scene time in seconds; the words after the time are in any order):
//
//   cam <T> pos <x> <y> <z>                       the camera stands there (world metres, Y up)
//   cam <T> attach car:<id> [<dx> <dy> <dz>]      the camera sits at that offset in the car's frame: x right, y up, z forward
//           [heading] [follow <s>]                (0 2 -6 = six metres behind, two up); `heading` uses only the car's heading, so
//                                                 the camera does not pitch and roll with the car; `follow <s>` lags the car's
//                                                 position and heading by about <s> seconds (a critically damped spring)
//   cam <T> orbit car:<id> r <m> az <deg> el <deg> [at <dx> <dy> <dz>] [follow <s>]
//                                                 on a sphere around the car (the point `at`, default 0 0 0, in the car's
//                                                 heading frame): az 0 behind, 90 on the right, 180 in front; el up; it looks
//                                                 at that point unless the key says how to aim
//   cam <T> yaw <deg> pitch <deg>                 the aim: yaw 0 looks along +Z, 90 along +X; pitch + looks up
//   cam <T> look car:<id> [<dx> <dy> <dz>]        the aim: at the car (the point at that offset in its heading frame) ...
//   cam <T> look <x> <y> <z>                      ... or at a world point
//           [aimfollow <s>]                       the aim lags a moving target by about <s> seconds
//   cam <T> roll <deg>                            + tilts the camera's right side up
//   cam <T> fov <deg>  near <m>  far <m>          vertical field of view (interpolated in focal length, so a zoom looks even);
//                                                 near and far; a channel no key names stays the game's own
//   cam <T> ... ease linear|in|out|inout          how the segment that STARTS at this key runs (its position, aim, roll, fov ...)
//   cam <T> off                                   the game's own camera again from T
//   cut <T>                                       a cut: nothing is interpolated across T, and the frame at T starts the
//                                                 renderer's histories again (the upscaler, the occlusion, the motion blur)
//
// RIGS (feature-parity/DIRECTOR-DESIGN.md 4.3): `cam <T> rig <kind> ...` is a shot in one line, written in terms of a car or a place, that
// stands for the keys above (a chase camera is an `attach ... heading follow` plus a `look` with the same lag). The words after the kind are in
// any order; every rig also takes fov, roll, near, far and ease with the meaning they have above. Places are world coordinates, or `spot:<name>`
// (spots.h), which the scene parser has turned into numbers by the time this file reads them.
//
//   rig chase car:<id> [dist <m>] [height <m>] [side <m>] [lag <s>] [aim <m>] [ahead <m>]
//        behind the car: `dist` metres back (6), `height` up (1.8), `side` to the right (0), the position and the aim lagging the car by `lag`
//        seconds (0.3); the aim is the point `aim` metres above the car (1) and `ahead` metres in front of it (0: look-ahead on a fast car).
//        It follows the car's heading only, so it does not pitch and roll with the body.
//   rig orbit car:<id> [r <m>] [az <a>[..<b>]] [el <e>[..<f>]] [over <s>] [at <dx> <dy> <dz>] [lag <s>]
//        round the car on a sphere (the `orbit` key: r 8, az 0 behind, el 10). An az or el range a..b needs `over <s>`: the camera sweeps from a
//        to b over those seconds (two orbit keys, an arc round the car) and holds at b.
//   rig tripod at <x> <y> <z> track car:<id> [<dx> <dy> <dz>] [aimlag <s>]
//        a camera standing on a place that follows a car with its aim (the point <dx dy dz> in the car's heading frame, default 0 0.8 0), the aim
//        lagging by `aimlag` seconds. `track <x> <y> <z>` aims at a place instead; yaw <deg> pitch <deg> aim it by hand.
//   rig flyby path <route> [speed <x>] [up <m>] [y <m>] track car:<id> [<dx> <dy> <dz>] [aimlag <s>]
//        the camera flies along a route file (the car routes of path_follow.h: beside the scene or in its routes folder) from this key's time, at
//        the route's own speeds or `speed 12` (metres a second) or `speed x1.5` (a factor); it holds at the last point. Its height is the route's
//        own height (x,z,speed,y files) plus `up` (2), or `y <m>` in the world; a route with no heights needs `y`. The position is a function of
//        the scene time, so a scrub and a take agree. Aimed like a tripod.
//   rig hood|bumper|roof car:<id> [ahead <m>] [lag <s>] [shift <dx> <dy> <dz>]
//        a mount on the car, rigid to its body (it pitches and rolls with it), looking `ahead` metres in front (30). The mount offsets are the
//        same for every car (kMounts in camera.cpp, CHECK: a per-model table needs the car's bounds); `shift` moves the mount in the car's frame.
//        Near defaults to 0.1 m so the bodywork does not clip.
//
// A key names the channels it moves: position (pos, attach, orbit), aim (yaw/pitch, look), roll, fov, near, far. Each channel
// has its own keys; it runs from key to key with the ease of the key it leaves, and holds its last key's value. The first key
// names a position and an aim. Positions run along a centripetal Catmull-Rom spline through the keys (an attach or orbit key is a
// point that moves with its car, so the spline runs through where the keys are NOW); two orbit keys of one car blend azimuth,
// elevation and radius instead, an arc around the car. The aim is a slerp between what each of the two keys would aim at now.
// The camera takes over at the first key and keeps going until the scene ends or a `cam <T> off`; the take-over and the hand-back
// are cuts too. A cut also ends every segment: keys after it never blend with keys before it, and a channel the first keys of the
// new shot do not name carries on from where it was.
//
// Nothing here moves a car or touches the streamer: the world follows the final camera by itself (E3).
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "path_follow.h"
#include "scene.h"

namespace mc::cinematic::camera {

// ---------------------------------------------------------------------------------------------------------------- vectors

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec3 a, Vec3 b);
Vec3 Cross(Vec3 a, Vec3 b);
double Length(Vec3 a);
// a / |a|, or `fallback` when a is too short to have a direction.
Vec3 Normalize(Vec3 a, Vec3 fallback);
Vec3 Lerp(Vec3 a, Vec3 b, double u);
double Lerp(double a, double b, double u);

// ---------------------------------------------------------------------------------------------------------------- rotations

struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};
Quat Normalize(Quat q);
// The shortest way from a to b; u in 0..1. Near-parallel quaternions are blended and renormalised.
Quat Slerp(Quat a, Quat b, double u);
Vec3 Rotate(Quat q, Vec3 v);

// A camera's axes in the game's world, which is right-handed (x east, y up, z south): looking along `forward` with `up` above, the
// right is forward x up (looking along +Z, the right is -X; facing north, -Z, it is east). The game's camera slots hold the rows
// right, up and BACK (minus forward), a proper rotation: right x up = back (menu_camera.cpp writes them that way, and the pictures
// of the first smoke run, which wrote forward into the third row, looked the wrong way). Basis keeps the forward vector and not the
// back, so right x up = -forward here.
struct Basis {
    Vec3 right{-1.0, 0.0, 0.0};
    Vec3 up{0.0, 1.0, 0.0};
    Vec3 forward{0.0, 0.0, 1.0};
};
// The rotation that takes the local axes (1,0,0), (0,1,0), (0,0,1) to right, up, back (the slots' rows).
Quat ToQuat(const Basis& b);
Basis ToBasis(Quat q);

// The three rows the game's camera slots (and a car's matrix: right, up, back, position) hold for a camera. The third row is the
// BACK: the game's own camera looks along minus it (the E5 run's chase camera: third row (0.474, 0.067, -0.878), the car it follows
// at (-0.47, -0.23, 0.86) from the camera), and the renderer's anchor reads the same rows as its c12 to c14.
struct SlotRows {
    Vec3 right{-1.0, 0.0, 0.0};
    Vec3 up{0.0, 1.0, 0.0};
    Vec3 back{0.0, 0.0, -1.0};
};
SlotRows ToSlotRows(const Basis& b);
Basis FromSlotRows(const SlotRows& rows);

// yaw 0 looks along +Z and 90 along +X; pitch + looks up (clamped to +-89 degrees); no roll.
Basis BasisFromYawPitch(double yaw_deg, double pitch_deg);
// Looks from `from` at `to` with the world's Y up. `fallback` is the answer when the two are the same point; looking straight up
// or down keeps the roll of `fallback`.
Basis BasisLookAt(Vec3 from, Vec3 to, const Basis& fallback);
// Turns right and up about forward: + lifts the right side.
Basis WithRoll(const Basis& b, double roll_deg);
// The direction of travel in the ground plane as an angle (radians, atan2(x, z): 0 along +Z, pi/2 along +X); `keep` when the
// vector stands straight up or down.
double HeadingOf(Vec3 forward, double keep);
// Level axes that look along a heading: right, up = Y, forward.
Basis HeadingBasis(double heading_rad);

// ---------------------------------------------------------------------------------------------------------------- interpolation

// The curve of a segment's time (scene.h: linear, in, out, inout); u is clamped to 0..1.
double EaseCurve(Ease ease, double u);

// Field of view is interpolated in focal length (1 / tan(fov / 2)): a zoom looks even.
double FocalFromFov(double fov_deg);
double FovFromFocal(double focal);
double LerpFov(double a_deg, double b_deg, double u);

// The centripetal Catmull-Rom spline through p1 and p2 (u = 0 and 1) with p0 and p3 as the neighbours: no cusps or loops inside a
// segment, whatever the spacing of the keys. Coincident points fall back to a straight line.
Vec3 CatmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double u);

// The point of an orbit: `target` plus `radius` along the direction az (0 behind, 90 to the right of the heading frame `hf`) and
// el (up).
Vec3 OrbitPoint(Vec3 target, const Basis& hf, double radius, double az_deg, double el_deg);

// ---------------------------------------------------------------------------------------------------------------- the follow lag

// A critically damped spring on one number: the exact solution over dt, so the result does not depend on how the time is cut into
// frames. It settles on a still target in a few `smooth` seconds and trails a target that moves at speed v by v * smooth.
// smooth <= 0 follows at once.
struct Damped {
    double x = 0.0;
    double v = 0.0;
    void Snap(double at) {
        x = at;
        v = 0.0;
    }
    double Update(double target, double dt, double smooth);
};
struct Damped3 {
    Damped x, y, z;
    void Snap(Vec3 at);
    Vec3 Update(Vec3 target, double dt, double smooth);
    Vec3 Value() const { return {x.x, y.x, z.x}; }
};
// The same for an angle in radians: the target is taken the short way round.
struct DampedAngle {
    Damped a;
    void Snap(double at) { a.Snap(at); }
    double Update(double target, double dt, double smooth);
    double Value() const { return a.x; }
};

// ---------------------------------------------------------------------------------------------------------------- the track

enum class PosKind : uint8_t { kWorld, kAttach, kOrbit, kPath };  // kPath: `rig flyby`, a place that moves along a route
enum class AimKind : uint8_t { kAngles, kLookPoint, kLookCar };

struct PosKey {
    double t = 0.0;
    int line = 0;
    int shot = 0;
    Ease ease = Ease::kLinear;
    PosKind kind = PosKind::kWorld;
    Vec3 point;                // kWorld: the position; kAttach: the offset in the car's frame; kOrbit: the target's offset
    std::string car;           // kAttach, kOrbit
    bool heading_only = false;
    double follow = 0.0;       // seconds of lag
    double radius = 8.0;       // kOrbit
    double azimuth = 0.0;
    double elevation = 10.0;
    // kPath: the route (an index into CameraTrack::routes), the scene time it starts at (a copy of the key that carries on after a cut keeps it:
    // the route does not start again), the height above the route's own (up) or in the world (y, when has_y)
    int route = -1;
    double path_t0 = 0.0;
    double up = 0.0;
    bool has_y = false;
    double y = 0.0;
};
struct AimKey {
    double t = 0.0;
    int line = 0;
    int shot = 0;
    Ease ease = Ease::kLinear;
    AimKind kind = AimKind::kAngles;
    double yaw = 0.0, pitch = 0.0;  // kAngles
    Vec3 point;                     // kLookPoint: the world point; kLookCar: the offset in the car's heading frame
    std::string car;                // kLookCar
    double follow = 0.0;            // seconds of lag of the target
};
struct ScalarKey {
    double t = 0.0;
    int line = 0;
    int shot = 0;
    Ease ease = Ease::kLinear;
    double value = 0.0;
};
// The camera takes over (on) or gives back (off) at t.
struct Gate {
    double t = 0.0;
    bool on = true;
};

// A route a `rig flyby` flies, ready to be asked where it is at a time since the key.
struct FlyRoute {
    std::string name;
    SpeedSpec speed;
    PathSampler sampler;
    bool has_y = false;  // the route has heights
};

struct CameraTrack {
    std::vector<FlyRoute> routes;  // the routes of the flyby keys, once each
    std::vector<PosKey> pos;
    std::vector<AimKey> aim;
    std::vector<ScalarKey> roll, fov, near_plane, far_plane;
    std::vector<Gate> gates;      // the take-overs and hand-backs, in time order (the camera is off before the first)
    std::vector<double> shots;    // where a shot starts: the take-overs and the `cut` keys, sorted and unique
    std::vector<double> cuts;     // the `cut` keys alone
    std::vector<std::string> cars;  // the car ids the keys use, once each (for the log)

    bool Empty() const { return gates.empty(); }
    // The number of shots that start at or before t: the shot t belongs to (0 before the first).
    int ShotAt(double t) const;
    // Where that shot starts (-infinity for 0).
    double ShotStart(int shot) const;
    // Whether the camera is the scene's at t.
    bool OnAt(double t) const;
};

struct BuildResult {
    CameraTrack track;
    std::vector<Diagnostic> diagnostics;
    int ErrorCount() const;
    bool Ok() const { return ErrorCount() == 0; }
};

// Where the routes of a `rig flyby` come from. The default (null `routes` of BuildTrack) reads route files beside the scene (Scene::source_dir:
// ParseOptions::path) and in its `routes` folder, the way a car's `path` does (a name with no extension also tries .route); a test hands in its own.
class RouteSource {
  public:
    virtual ~RouteSource() = default;
    // The route named in the scene; false with `error` ("no route file 'x' (looked for ...)") when there is none.
    virtual bool Load(const std::string& name, Route* out, std::string* error) = 0;
};

// Reads the camera and cut keys of a scene (scene.h keeps their words). A scene without any gives an empty track and no
// diagnostics. Never throws; with an error the track holds what was understood and must not be run.
BuildResult BuildTrack(const Scene& scene, RouteSource* routes = nullptr);

// "camera: 6 position keys (2 attach, 1 orbit), 5 aim keys, fov 3, roll 1; 1 cut; takes over at 0.00 s, cars: player" for the log.
std::string Summarize(const CameraTrack& track);

// ---------------------------------------------------------------------------------------------------------------- the rig

// A car as the camera needs it: where it is and its own axes (right, up, forward).
struct CarPose {
    Vec3 position;
    Basis frame;
};
class CarSource {
  public:
    virtual ~CarSource() = default;
    // False when the car does not exist (yet): the rig holds the car's last pose, or the world's origin.
    virtual bool Get(const std::string& id, CarPose* out) = 0;
};

// What the camera is at an instant.
struct View {
    bool active = false;      // the scene's camera replaces the game's now
    bool cut = false;         // this frame starts a shot, takes the camera over or gives it back: the histories start again
    bool took_over = false;   // the first frame of a take-over
    bool handed_back = false; // the first frame the game's own camera is back (active is false)
    Vec3 position;
    Basis basis;
    double fov = 0.0;         // vertical degrees; 0: the game's own
    double near_plane = 0.0;  // 0: the game's own
    double far_plane = 0.0;
    int shot = 0;
    int missing_cars = 0;     // keys that named a car nobody has
};

// Evaluates a track at scene times, in order: the follow lag keeps state from one call to the next, and the cuts are the
// shot starts passed since the last call. A time that goes backwards or jumps (a restart, a still) forgets the state and snaps
// the springs to where the camera should be.
class Rig {
  public:
    void Reset();
    View Evaluate(const CameraTrack& track, double t, CarSource& cars);
    // The id of a car some key named that the source did not know, since Reset (for one log line each).
    const std::vector<std::string>& MissingCars() const { return missing_; }

  private:
    struct Followed {
        bool valid = false;
        Vec3 position;
        Basis frame;
        double heading = 0.0;
    };
    struct Spring {
        std::string car;
        double smooth = 0.0;
        uint64_t frame = 0;  // the Evaluate call that updated it last
        bool init = false;
        Damped3 position;
        DampedAngle heading;
    };
    struct CarSlot {
        std::string car;
        bool have = false;     // a pose was ever seen
        bool fresh = false;    // read this call
        CarPose pose;
    };

    Followed Follow(const std::string& car, double smooth, CarSource& cars);
    Vec3 KeyPosition(const PosKey& k, CarSource& cars);
    Basis KeyAim(const AimKey& k, Vec3 camera, CarSource& cars);
    CarSlot& Slot(const std::string& car, CarSource& cars);
    Vec3 EvalPosition(const std::vector<const PosKey*>& k, double t, CarSource& cars);
    Basis EvalAim(const std::vector<const AimKey*>& k, double t, Vec3 camera, CarSource& cars);

    const CameraTrack* track_ = nullptr;  // the track and the time of the Evaluate call in progress (a flyby key reads its route there)
    double cur_t_ = 0.0;
    bool have_last_ = false;
    double last_t_ = 0.0;
    bool was_active_ = false;
    uint64_t frame_ = 0;
    double dt_ = 0.0;
    int missing_this_frame_ = 0;
    std::vector<Spring> springs_;
    std::vector<CarSlot> slots_;
    std::vector<std::string> missing_;
};

}  // namespace mc::cinematic::camera
