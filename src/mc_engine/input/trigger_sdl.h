#pragma once
//
// What the trigger effects share off Windows (adaptive_triggers.cpp, impulse_triggers.cpp).
//
// On Windows the executable has an SDL of its own and the SDK's input runs in another one
// (rexruntime.dll). Everywhere else the SDK is linked into the executable, so the process has ONE
// SDL: the SDK's input driver (input_backend = "sdl") and the trigger code talk to the same
// joysticks, and an SDL_Gamepad opened twice is the same device with a reference count.
//

#if !defined(_WIN32)
#include <SDL3/SDL.h>

#include <mutex>

namespace larecomp::trigger_sdl {

// True once the gamepad layer is up and this process holds a reference to it. The trigger code does
// not start the layer itself: SDL only announces the pads that were already connected to a layer
// that someone else started (the SDK's driver listens for exactly those events), so a layer started
// here first would leave the game's input blind to a pad that was plugged in at launch (the wheel
// thread waits the same way, wheel_input.cpp). The reference keeps the layer, and the pads opened
// here, valid until the process ends even if the SDK's driver shuts down first. Any thread.
inline bool GamepadLayerUp() {
    static std::mutex mutex;
    static bool held = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!held) {
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return false;
        held = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    return held;
}

}  // namespace larecomp::trigger_sdl
#endif  // !_WIN32
