#pragma once
//
// The CAR SELF-REFLECTIONS row of the pause menu (REFLECTIONS page): one row for two settings, and the text
// of its values. The row itself is in pause_menu.cpp, next to its neighbours; this header only holds what has
// no game or renderer code in it, so a standalone program can check it (pause_selfrefl_row_test.cpp).
//
// The settings:
//   mcla_gpu_car_selfrefl      auto | off | on     the player car's paint reflects its own body (car_selfrefl.h)
//   mcla_gpu_car_selfrefl_rt   auto | off          where the graphics card can ray trace the car (Direct3D 12
//                                                  with DXR 1.1 and shader model 6.5, or Vulkan with ray queries: CarRtSupported in
//                                                  car_rt_scene.h), that reflection is traced from the car's own
//                                                  triangles instead of marched through the picture
//                                                  (car_rt_refl.h); it only runs while the first one is on
//
// The row's values and what each writes (the row writes both settings every time, so a value always means the
// same thing; OFF leaves the ray tracing setting where it was, it has no effect while the reflection is off):
//
//   AUTO         mcla_gpu_car_selfrefl = auto, mcla_gpu_car_selfrefl_rt = auto
//                Shown "AUTO (RAY TRACED)", "AUTO (ON)" or "AUTO (OFF)": what runs on this PC.
//   OFF          mcla_gpu_car_selfrefl = off
//   ON           mcla_gpu_car_selfrefl = on, mcla_gpu_car_selfrefl_rt = off    (the screen-space reflection)
//   RAY TRACED   mcla_gpu_car_selfrefl = on, mcla_gpu_car_selfrefl_rt = auto   (only listed where it can run)
//
// What the row shows for the two settings as they are (a larecomp.toml can hold any pair; the row shows what
// the pair makes the game do, so a value that is not one of the four writes above is shown as the one it
// acts as, and keeps its text until the row is stepped):
//
//   selfrefl off                      OFF         whatever the ray tracing setting says
//   selfrefl auto, rt auto            AUTO
//   selfrefl auto, rt off             ON, or OFF where auto comes to off on this graphics card (an integrated
//                                     GPU): auto with no ray tracing is the screen-space reflection or nothing
//   selfrefl on,   rt off             ON
//   selfrefl on,   rt auto            RAY TRACED where it can run, else ON (the screen-space reflection runs)
//   any other text                    read as auto (the settings do the same)
//
// Width (pause_menu.cpp "Page width"): the label is "CAR SELF-REFLECTIONS" (20) and the longest value
// "AUTO (RAY TRACED)" (17): 37 of the 41 a page can take.

#include <cstdint>
#include <string>
#include <string_view>

namespace mcla_menu {
namespace selfrefl {

enum class Choice : uint8_t { kAuto, kOff, kOn, kRayTraced };

// The row lists AUTO, OFF, ON and, where the ray traced path can run, RAY TRACED, in this order.
constexpr int Count(bool ray_traced_available) { return ray_traced_available ? 4 : 3; }
constexpr Choice At(int i) {
    return i <= 0 ? Choice::kAuto : i == 1 ? Choice::kOff : i == 2 ? Choice::kOn : Choice::kRayTraced;
}
constexpr int IndexOf(Choice choice) {
    return choice == Choice::kAuto ? 0 : choice == Choice::kOff ? 1 : choice == Choice::kOn ? 2 : 3;
}

// What a value writes. A null text is left as it is.
struct Writes {
    const char* selfrefl;  // mcla_gpu_car_selfrefl
    const char* rt;        // mcla_gpu_car_selfrefl_rt
};
constexpr Writes WritesOf(Choice choice) {
    switch (choice) {
        case Choice::kOff:       return {"off", nullptr};
        case Choice::kOn:        return {"on", "off"};
        case Choice::kRayTraced: return {"on", "auto"};
        default:                 return {"auto", "auto"};
    }
}

// The value the two settings come to. `auto_is_on`: mcla_gpu_car_selfrefl = auto comes to on on this graphics
// card (gpu_class.h). `ray_traced_available`: the ray traced path can run on this device.
inline Choice ChoiceOf(std::string_view selfrefl, std::string_view rt, bool auto_is_on,
                       bool ray_traced_available) {
    const bool rt_off = rt == "off";
    if (selfrefl == "off") return Choice::kOff;
    if (selfrefl == "on") return rt_off || !ray_traced_available ? Choice::kOn : Choice::kRayTraced;
    if (!rt_off) return Choice::kAuto;  // auto and auto
    return auto_is_on ? Choice::kOn : Choice::kOff;
}

// What AUTO runs: "AUTO (RAY TRACED)", "AUTO (ON)" or "AUTO (OFF)".
inline std::string AutoLabel(bool auto_is_on, bool ray_traced_available) {
    if (!auto_is_on) return "AUTO (OFF)";
    return ray_traced_available ? "AUTO (RAY TRACED)" : "AUTO (ON)";
}

inline std::string LabelOf(Choice choice, bool auto_is_on, bool ray_traced_available) {
    switch (choice) {
        case Choice::kOff:       return "OFF";
        case Choice::kOn:        return "ON";
        case Choice::kRayTraced: return "RAY TRACED";
        default:                 return AutoLabel(auto_is_on, ray_traced_available);
    }
}

}  // namespace selfrefl
}  // namespace mcla_menu
