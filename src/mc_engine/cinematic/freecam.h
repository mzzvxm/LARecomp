#pragma once
//
// The free camera and the flight recorder of the trailer toolkit (work package M1d; feature-parity/CINEMATIC-DESIGN.md section 3.5).
//
// This file is the pure half, like camera.h: the pad as the game's input state holds it, the stick response, the flight (a camera
// that a pad moves, with inertia, roll and zoom), the chords that start and stop a recording, the angles of a camera basis, and
// the raw track file the recorder writes. It knows nothing of the game, the SDK or the log, so freecam_test.cpp builds it on its
// own. freecam_runtime.cpp is the glue: it reads the pad in the XamInputGetState wrapper, flies the camera in the final camera hook
// (0x822C0868) and writes the file.
//
// THE CONTROLS (pad 0; while the camera flies the game gets NO pad input except Start, so the car does not drive and the pause menu
// still opens):
//
//   left stick            move: forward and back along where the camera looks (up and down too), left and right level
//   right stick           look: yaw and pitch (stick right turns right, stick up looks up)
//   right / left trigger  up / down (world Y)
//   L3 (left stick click) hold: fast (x4)         R3 (right stick click) hold: slow (x1/4)
//   D-pad left / right    the base speed one step (x1/1.25, x1.25) per press
//   D-pad up / down       hold: zoom in / out (the field of view in focal length: an even zoom)
//   LB / RB               hold: tilt the horizon left / right (roll)
//   Y                     level the horizon and bring the field of view back to where it started
//   Back + A              start or stop the recording        Back + X   home: the game's own camera view again (a cut)
//   Back + Y              mark a cut in the recording (the converter writes a `cut` there)
//   The Director's panel (DIRECTOR-DESIGN.md section 3; src/mc_engine/ui/director_panel_core.h turns each into a command):
//   Back + B              add a camera key here                     Back + RB / LB   go to the next / previous camera key
//   Back + Start          play or pause the scene (Start itself stays the game's pause menu, and never reaches the game while Back is held)
//   Back + D-pad right    one frame on (the fixed step)             Back + D-pad up  render the take
//   With Back held LB / RB do not tilt the horizon, and the D-pad does not change the speed (they are chords).
//
// Every continuous control runs through a one-pole smoothing (the inertia) that is exact in time: the same flight at 60 and 240
// updates a second covers the same distance.
//
// THE RAW TRACK FILE. Text, one sample per game update, written as the flight goes (flushed twice a second, so a crash loses little):
//
//   # larecomp freecam v1                               the first line, always
//   # <anything>                                        notes: start time, the scene that ran, the game's frame at the start
//   t,scene_t,frame,x,y,z,yaw,pitch,roll,fov,flags      the one header row
//   0.000000,-1.000000,48211,819.6120,17.6000,-808.4000,-28.0000,0.0000,0.0000,50.0000,0
//
//   t        seconds since the recording started: the sum of the engine timer's frame deltas BEFORE its time scale (what a scene's clock
//            adds), 0 at the first sample, so a scene whose camera track is this file's keys moves the camera the same way from its t = 0
//   scene_t  the scene's clock when a scene was running, else -1: a flight made against a scene's world (an hour ramp, a time scale)
//            can be keyed to the scene's own time (scene_cam.py --time scene)
//   frame    the game's frame counter
//   x y z    world metres, Y up; yaw pitch roll degrees in the scene file's meaning (cam yaw 0 looks along +Z, 90 along +X; pitch + looks
//            up; roll + lifts the right side); yaw is continuous (not wrapped); fov: vertical degrees. The game's world is right-handed
//            (x east, y up, z south), so a growing yaw turns the camera to the LEFT (south to east) and the camera's right is forward x up
//            (camera.h); the free camera, the scene's camera and scene_cam.py all use that one basis, so a recorded pose replays exactly
//   flags    bit 0: a cut: this sample starts a new shot (the flight jumped to it: home; or the pilot marked one: Back + Y)
//
// ports\scripts\scene_cam.py reads it: smooths it, reduces it to keys within an error bound and writes a scene's `cam` track.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "camera.h"

namespace mc::cinematic::freecam {

namespace cam = mc::cinematic::camera;

// ---------------------------------------------------------------------------------------------------------------- the pad

// XINPUT_GAMEPAD button bits, as the game's X_INPUT_STATE holds them.
constexpr uint16_t kBtnDpadUp = 0x0001, kBtnDpadDown = 0x0002, kBtnDpadLeft = 0x0004, kBtnDpadRight = 0x0008;
constexpr uint16_t kBtnStart = 0x0010, kBtnBack = 0x0020, kBtnL3 = 0x0040, kBtnR3 = 0x0080;
constexpr uint16_t kBtnLb = 0x0100, kBtnRb = 0x0200, kBtnA = 0x1000, kBtnB = 0x2000, kBtnX = 0x4000, kBtnY = 0x8000;

struct Pad {
    uint16_t buttons = 0;
    double lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;  // -1..1, right and up positive
    double lt = 0.0, rt = 0.0;                      // 0..1
    // Only the Director's panel sets these (the mouse and the wheel; a real pad leaves them 0). They are amounts to APPLY ONCE, not states: the look is degrees to turn in this
    // update (to the right and up positive, no smoothing: a mouse is exact), `speed_steps` the notches of the speed (+1 is a D-pad right press: x1.25).
    double look_right = 0.0, look_up = 0.0;
    int speed_steps = 0;
};

// The 16 bytes of the guest's X_INPUT_STATE (big endian): be32 packet, be16 buttons, u8 LT, u8 RT, be16 LX, LY, RX, RY.
constexpr size_t kGuestPadBytes = 16;
Pad PadFromGuest(const uint8_t* state);
// What the camera takes from the game: buttons except Start, both triggers and all four axes become 0. With Back held Start goes too (Back + Start is a chord of the camera:
// play / pause the scene), so the pause menu does not open under it.
void ClearGuestPad(uint8_t* state);

// A stick through a circular dead zone and a response curve: nothing inside `zone`, then 0 to 1 from the zone's edge to the rim, bent
// with a power of kStickExponent so that small deflections give fine control. The direction is kept.
constexpr double kStickExponent = 1.6;
void StickResponse(double x, double y, double zone, double* out_x, double* out_y);
// A trigger: nothing under `zone`, then 0 to 1.
double TriggerResponse(double v, double zone = 0.05);

// ---------------------------------------------------------------------------------------------------------------- the flight

struct Params {
    double speed = 12.0;       // metres a second at a full stick, the speed multiplier 1 and no modifier
    double look = 70.0;        // degrees a second at a full right stick
    double roll_rate = 30.0;   // degrees a second while LB or RB is held
    double zoom_rate = 0.5;    // the focal length grows by e^zoom_rate a second while D-pad up is held (0.5: x1.65)
    double inertia = 0.25;     // seconds the movement takes to settle (the look and the roll use half); 0 = no smoothing
    double fast = 4.0;         // L3
    double slow = 0.25;        // R3
    double deadzone = 0.15;    // radius of the sticks' dead zone
    double min_fov = 10.0;
    double max_fov = 110.0;
};
constexpr double kSpeedStep = 1.25;
constexpr double kMinSpeedMult = 0.04;
constexpr double kMaxSpeedMult = 25.0;
constexpr double kMaxPitch = 89.0;

// A camera as the flight keeps it: the scene file's own words.
struct Pose {
    cam::Vec3 position;
    double yaw = 0.0;    // degrees, continuous
    double pitch = 0.0;  // -89..89
    double roll = 0.0;   // degrees
    double fov = 50.0;   // vertical degrees
};

// The axes the pose looks along: level (yaw, pitch), then rolled.
cam::Basis BasisOf(const Pose& pose);
// The angles of a camera basis. yaw is in -180..180, pitch -90..90; at straight up or down yaw is `keep_yaw`. Round trip with BasisOf.
void AnglesFromBasis(const cam::Basis& basis, double keep_yaw, double* yaw, double* pitch, double* roll);
Pose PoseFromBasis(cam::Vec3 position, const cam::Basis& basis, double fov, double keep_yaw = 0.0);
// The rows the game's camera slots hold for a pose: right, up and BACK (minus forward), as camera.h's ToSlotRows says; and the pose of the
// camera that the slots hold (the game's own, to seed the flight from). The third row is NOT the forward vector: a camera written with it
// looks the opposite way and a cut request built from it is never met by the renderer's anchor (the first smoke run of the scene camera).
cam::SlotRows SlotRowsOf(const Pose& pose);
Pose PoseFromSlotRows(cam::Vec3 position, const cam::SlotRows& rows, double fov, double keep_yaw = 0.0);
// The smallest turn between two directions, degrees (0..180).
double AngleBetween(cam::Vec3 a, cam::Vec3 b);
// The difference of two angles in degrees, wrapped to -180..180.
double WrapDegrees(double d);

struct Events {
    bool toggle_record = false;  // Back + A
    bool home = false;           // Back + X: the caller seeds the flight from the game's own camera
    bool mark_cut = false;       // Back + Y
    bool reset_view = false;     // Y: roll and field of view done by the flight already; for the log
    // The Director panel's chords (the caller turns them into commands: director_panel_core.h CommandsForFlyEvents)
    bool add_key = false;        // Back + B
    bool next_key = false;       // Back + RB
    bool prev_key = false;       // Back + LB
    bool play_pause = false;     // Back + Start
    bool step_frame = false;     // Back + D-pad right
    bool render_take = false;    // Back + D-pad up
    bool Any() const { return toggle_record || home || mark_cut || reset_view || add_key || next_key || prev_key || play_pause || step_frame || render_take; }
};

class Flyer {
  public:
    // Starts from this camera: all motion stops, the field of view of the Y button becomes this one. The speed multiplier stays.
    void Seed(const Pose& pose);
    // One update of dt seconds with the pad as it is now (before the game gets it). dt over 0.25 is cut to it.
    Events Step(const Pad& pad, double dt, const Params& params);

    const Pose& pose() const { return pose_; }
    double speed_multiplier() const { return speed_mult_; }
    // The speed the camera would fly at a full stick now (multiplier and modifier not included in `params.speed` alone).
    double Speed(const Params& params) const { return params.speed * speed_mult_; }

  private:
    Pose pose_;
    double home_fov_ = 50.0;
    double speed_mult_ = 1.0;
    uint16_t prev_buttons_ = 0;
    // the smoothed inputs
    double strafe_ = 0.0, forward_ = 0.0, rise_ = 0.0;
    double yaw_rate_ = 0.0, pitch_rate_ = 0.0, roll_rate_ = 0.0, zoom_ = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------- the raw track

constexpr uint32_t kFlagCut = 1u;

struct Sample {
    double t = 0.0;
    double scene_t = -1.0;
    uint32_t frame = 0;
    Pose pose;
    uint32_t flags = 0;
};

constexpr const char* kRawMagic = "# larecomp freecam v1";
constexpr const char* kRawColumns = "t,scene_t,frame,x,y,z,yaw,pitch,roll,fov,flags";

// The first lines of a file (the magic line, the notes, the header row), each ending in a newline. `notes` are put after "# ".
std::string RawHeader(const std::vector<std::string>& notes);
// One sample, with its newline.
std::string RawRow(const Sample& sample);

struct RawTrack {
    std::vector<std::string> notes;
    std::vector<Sample> samples;
};
// Reads a whole file. False with `error` ("line 7: ...") when it is not one; `out` holds what was read before the error.
bool ParseRawTrack(std::string_view text, RawTrack* out, std::string* error);

// What a recording looked like, for the log line at its end.
struct Stats {
    size_t samples = 0;
    double duration = 0.0;   // seconds, first sample to last
    double distance = 0.0;   // metres flown (the path of the samples, jumps of a cut left out)
    double max_speed = 0.0;  // metres a second between two samples
    size_t cuts = 0;
};
Stats Summarize(const RawTrack& track);

}  // namespace mc::cinematic::freecam
