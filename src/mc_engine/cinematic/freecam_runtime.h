#pragma once
//
// The free camera and the flight recorder in the game (trailer toolkit, work package M1d): the glue between the pure flight (freecam.h) and
// the game. Two places touch the game, both one flag test when the camera is not armed:
//
//   Hook_MenuCameraFinal (menu_camera.cpp, 0x822C0868, the last point before the renderer) calls FreecamApply first of all: when the cvar
//   mcla_freecam is on and the game is not paused, the camera is the flight's, in every camera mode but the front end's shots, and the
//   recorder (mcla_freecam_record, or Back + A) writes the raw track file one sample a game update.
//
//   The XamInputGetState wrapper (wheel/wheel_fake.cpp) puts a FreecamPadScope on its stack: when the function ends (after the SDK's state,
//   the stick dead zones and the test harness's pad) the scope hands the pad to the flight and clears it for the game, so the car does not
//   drive while the camera flies. Start is left to the game: the pause menu still opens, and a paused game gets its pad back.
//

#include <cstdint>

#include "freecam.h"

namespace mc::cinematic {

// Called by Hook_MenuCameraFinal before anything else. `base` is the guest memory, `sp` the guest stack pointer of sub_822C0320, `mode` the
// camera mode (r24). True when the free camera wrote the final camera (the caller returns).
bool FreecamApply(uint8_t* base, uint32_t sp, uint32_t mode);

// Called when XamInputGetState has filled the state at guest address `state` for `user`. Does nothing unless the camera is flying.
void FreecamObservePad(uint8_t* base, uint32_t user, uint32_t state);

// True while the camera is the flight's.
bool FreecamActive();

// The game is ending: a recording that is still running writes its last rows and closes its file. The fast exit (exit_fast.h) runs no destructor,
// and the file's destructor was what flushed them.
void FreecamStopForExit();

// ---------------------------------------------------------------------------------------------------------------- the Director (src/mc_engine/cinematic/director)
// Game thread, except FreecamFeedPad (any thread).

// Whose camera is final when the Director says so: kNone = as before (the flight when mcla_freecam is on, else the scene's, else the game's), kFree = the flight,
// kGame = the game's own camera whatever else runs (the flight and the scene's camera stand aside; a front-end shot and a paused game keep their own anyway).
enum class CameraOverride : uint8_t { kNone, kFree, kGame };

// The flight starts at this pose (at the next hook run), and mcla_freecam is turned on; a forced game camera is released. The renderer is told of the cut.
void FreecamSetPose(const freecam::Pose& pose);
// The flight's pose; false before it has one (it is seeded from the game's camera when it first flies).
bool FreecamGetPose(freecam::Pose* out);
void FreecamSetOverride(CameraOverride mode);
CameraOverride FreecamOverride();
// A pad that is not the real one flies the camera while it is fed (every drawn frame of the panel; 150 ms of wall time without a feed and the real pad is back): the panel's
// mouse and keys. Pad::look_right / look_up / speed_steps are amounts to apply once: they add up between two updates and the next one takes them. Any thread.
void FreecamFeedPad(const freecam::Pad& pad);
// A function called once for every update of the flight with the events its pad produced (freecam.h Events: the recorder's chords, and the panel's chords
// Back + B add key, Back + RB / LB next / previous key, Back + Start play / pause that package E adds there). The Director's panel turns them into commands.
using FreecamEventHook = void (*)(const freecam::Events& events);
void SetFreecamEventHook(FreecamEventHook hook);

// An object on the stack of the XamInputGetState wrapper: runs FreecamObservePad when the wrapper returns, whatever path it took, unless the
// call failed (`result` is the wrapper's r3, which holds the call's result by then: 0 = a pad answered).
class FreecamPadScope {
  public:
    FreecamPadScope(uint8_t* base, uint32_t user, uint32_t state, const uint64_t& result)
        : base_(base), user_(user), state_(state), result_(result) {}
    ~FreecamPadScope() {
        if ((result_ & 0xFFFFFFFFull) == 0) FreecamObservePad(base_, user_, state_);
    }
    FreecamPadScope(const FreecamPadScope&) = delete;
    FreecamPadScope& operator=(const FreecamPadScope&) = delete;

  private:
    uint8_t* base_;
    uint32_t user_;
    uint32_t state_;
    const uint64_t& result_;
};

}  // namespace mc::cinematic
