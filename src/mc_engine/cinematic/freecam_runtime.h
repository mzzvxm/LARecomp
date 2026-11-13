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

namespace mc::cinematic {

// Called by Hook_MenuCameraFinal before anything else. `base` is the guest memory, `sp` the guest stack pointer of sub_822C0320, `mode` the
// camera mode (r24). True when the free camera wrote the final camera (the caller returns).
bool FreecamApply(uint8_t* base, uint32_t sp, uint32_t mode);

// Called when XamInputGetState has filled the state at guest address `state` for `user`. Does nothing unless the camera is flying.
void FreecamObservePad(uint8_t* base, uint32_t user, uint32_t state);

// True while the camera is the flight's.
bool FreecamActive();

// An object on the stack of the XamInputGetState wrapper: runs FreecamObservePad when the wrapper returns, whatever path it took.
class FreecamPadScope {
  public:
    FreecamPadScope(uint8_t* base, uint32_t user, uint32_t state) : base_(base), user_(user), state_(state) {}
    ~FreecamPadScope() { FreecamObservePad(base_, user_, state_); }
    FreecamPadScope(const FreecamPadScope&) = delete;
    FreecamPadScope& operator=(const FreecamPadScope&) = delete;

  private:
    uint8_t* base_;
    uint32_t user_;
    uint32_t state_;
};

}  // namespace mc::cinematic
