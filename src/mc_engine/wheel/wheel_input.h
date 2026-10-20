#pragma once
//
// The host side of the steering wheel: finds a wheel through SDL3's joystick API,
// turns its raw axes and buttons into what the Xbox 360 racing wheel would report,
// and decides, per poll, whether the wheel or the gamepad is driving.
// wheel_fake.cpp owns the guest hooks and calls in here. See
// (sections 6, 10 and 14).
//

#include <cstdint>

namespace larecomp::wheel {

// The slice of X_INPUT_GAMEPAD the wheel can change, in host order.
struct PadIn {
    uint16_t buttons = 0;
    uint8_t lt = 0;
    uint8_t rt = 0;
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
};

// Before SDL's joystick layer starts: keeps the wheel out of the gamepad layer
// (the SDK's SDL input driver would otherwise map it as a stick and merge it into
// user 0 next to ours). Called once from main().
void EarlyInit();

// True while a wheel is connected, open and the mode allows it (wheel_mode is not
// "gamepad"). The guest sees a wheel exactly when this is true; it is sampled by
// the capabilities hook every frame, so it is cheap and never blocks.
bool WheelPresent();

// One input poll. `pad` is what the pad / keyboard-and-mouse underneath reported;
// on return it is what the guest should see. Does nothing and returns false when no
// wheel is present. Called from the guest's input thread.
bool ApplyToState(PadIn& pad, bool manual_transmission);

// What the game says about the car being driven, from the HUD update (midasm hook in
// wheel_fake.cpp, once per frame while the HUD is up). gear: -1 reverse, 0 neutral, 1..N.
// speed_ms is the car's speed. The H-shifter emulation steers by it.
void NoteHudGear(int gear, float speed_ms);

// One call per guest input frame (rex_sub_8260C4D0, the game's per-frame input update; the pad is
// polled once inside it). Counts frames so a button press can be made exactly one frame long, and
// doubles as the guest's heartbeat.
void NoteInputFrame();

}  // namespace larecomp::wheel
