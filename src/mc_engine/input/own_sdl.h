#pragma once
//
// The SDL calls that set up THIS executable's own SDL for the code that polls it itself (the wheel layer in
// wheel/wheel_input.cpp, the dialogs' pad reader in ui/gamepad.cpp). The rules are in sdl_instance.h; this
// file is the part that needs SDL.
//
// With one SDL (OneSdl) nothing here changes a thing: the SDK's pad driver shares the SDL and needs HIDAPI,
// XInput and the events. With two (0.10 on Windows) the exe's copy must stay away from every pad the SDK's
// copy owns -- no HIDAPI / RawInput / Windows.Gaming.Input / XInput drivers (a DualSense or an Xbox One pad
// would be opened a second time, by the exe's copy beside the SDK's) -- and, since nothing ever pumps its
// event queue, it posts no joystick or gamepad events (they would pile up to SDL's 65,535-event cap): its
// state is read by polling (SDL_UpdateJoysticks / SDL_UpdateGamepads).

#include <SDL3/SDL.h>

#include <mutex>
#include <string>
#include <string_view>

#include "mc_engine/input/sdl_instance.h"

namespace larecomp::sdlinst {

// Before anything starts this exe's SDL joystick or gamepad layer. Idempotent: the first caller (the wheel
// thread, which starts at launch) does the work, a later caller (the first dialog) finds it done.
inline void ApplyOwnCopyHints(std::string_view input_backend) {
    static std::once_flag once;
    std::call_once(once, [input_backend] {
        if (!OneSdl(input_backend)) {
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
            SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        }
        // Pads (and the wheel) stay readable while the game window is not focused.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    });
}

// After this exe's joystick / gamepad layer is up (the event table is reset when SDL's events subsystem
// quits, so it is asked again after each start).
inline void SilenceOwnCopyEvents(std::string_view input_backend) {
    if (OneSdl(input_backend)) return;
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
}

// One line for the log: which SDL this is and what its pad drivers are set to (the proof that the exe's copy
// opens no pad through HIDAPI beside the SDK's).
inline std::string DescribeOwnCopy(std::string_view input_backend) {
    auto hint = [](const char* name) {
        const char* v = SDL_GetHint(name);
        return std::string(v ? v : "default");
    };
    std::string out = OneSdl(input_backend) ? "the exe's SDL is the SDK's (one SDL)"
                                            : "the exe's SDL is its own copy, not the SDK's (two SDLs)";
    out += "; input_backend=" + std::string(input_backend);
    out += "; HIDAPI=" + hint(SDL_HINT_JOYSTICK_HIDAPI);
    out += " RAWINPUT=" + hint(SDL_HINT_JOYSTICK_RAWINPUT);
    out += " WGI=" + hint(SDL_HINT_JOYSTICK_WGI);
    out += " XINPUT=" + hint(SDL_HINT_XINPUT_ENABLED);
    out += " joystick-events=" + std::string(SDL_JoystickEventsEnabled() ? "on" : "off");
    out += " gamepad-events=" + std::string(SDL_GamepadEventsEnabled() ? "on" : "off");
    out += " ignore-devices=" + hint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES);
    return out;
}

}  // namespace larecomp::sdlinst
