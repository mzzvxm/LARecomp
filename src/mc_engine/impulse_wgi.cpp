// Xbox One and Xbox Series trigger rumble on Windows, through Windows.Gaming.Input (WGI).
// The worker thread of impulse_triggers.cpp calls this; the file has the reasons for WGI.
//
// What is used: Gamepad.Gamepads (the pads WGI lists), Gamepad.Vibration (two motors and two
// triggers, each 0 to 1, set together), and for the log line the pad's IsWireless and, through
// RawGameController.FromGameController, its USB ids and name. WGI answers for every Xbox 360, Xbox
// One and Xbox Series pad over USB and Bluetooth, next to the XInput calls the game's input makes;
// it ignores the trigger values of a pad that has no trigger motors. Like all WGI vibration it
// reaches the pad only while this process is the foreground application, which is when the
// worker rumbles (impulse_triggers.cpp checks the window is in front).
//
// The WinRT calls are the raw ABI, written out here: the six interfaces used, each a pointer to a
// table of functions (WinRT's vtable layout: IUnknown, IInspectable, then the interface's own
// methods, in the order of the Windows metadata), and the few functions that activate them found
// in combase.dll when first used, as SDL does for its own WGI driver. The SDK's windows.gaming.
// input.h is not used: its C++ form does not compile with MinGW's headers, its C form not as C++,
// and the project builds C++ only. No WinRT library, no C++/WinRT, no link line. CoIncrementMTAUsage
// gives the worker thread (which never initialises COM) the process-wide multithreaded apartment.
// The pads are used on the worker thread only (Shutdown() may release them from another thread
// when the worker did not stop; WGI's gamepad objects are agile).
//
// CHECK: never compiled into the game, never run against a pad (the type names and call order are
// the SDK headers'; the file passed a syntax-only compile with both header sets). What to watch
// in the log, with a Series pad over USB and over Bluetooth: `[Impulse] Windows.Gaming.Input
// lists 1 gamepad`, then the pad's line (`trigger motors yes`), and no repeated found/gone pair
// (a pair every 2 s would mean WGI hands out a new object for the same pad on every call; the
// pads are matched by pointer).

#if defined(_WIN32) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include "impulse_wgi.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winstring.h>  // HSTRING

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "larecomp_log.h"

namespace larecomp::impulse::wgi {
namespace {

// The first six slots of every WinRT interface: IUnknown and IInspectable.
struct UnknownVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void* self, REFIID iid, void** out);
    ULONG(STDMETHODCALLTYPE* AddRef)(void* self);
    ULONG(STDMETHODCALLTYPE* Release)(void* self);
    void* GetIids;
    void* GetRuntimeClassName;
    void* GetTrustLevel;
};
// An interface pointer: a pointer to a pointer to its function table.
template <class Vtbl>
struct Interface {
    const Vtbl* lpVtbl;
};

struct GamepadVibration {  // Windows.Gaming.Input.GamepadVibration: each 0 to 1
    double LeftMotor;
    double RightMotor;
    double LeftTrigger;
    double RightTrigger;
};

struct GamepadListVtbl;  // IVectorView<Gamepad>
struct GamepadVtbl;      // IGamepad
using GamepadList = Interface<GamepadListVtbl>;
using IGamepad = Interface<GamepadVtbl>;

struct GamepadListVtbl {
    UnknownVtbl unknown;
    HRESULT(STDMETHODCALLTYPE* GetAt)(GamepadList* self, UINT32 index, IGamepad** out);
    HRESULT(STDMETHODCALLTYPE* get_Size)(GamepadList* self, UINT32* out);
    void* IndexOf;
    void* GetMany;
};
struct GamepadVtbl {
    UnknownVtbl unknown;
    void* get_Vibration;
    HRESULT(STDMETHODCALLTYPE* put_Vibration)(IGamepad* self, GamepadVibration value);
    void* GetCurrentReading;
};
struct GamepadStaticsVtbl {  // IGamepadStatics
    UnknownVtbl unknown;
    void* add_GamepadAdded;
    void* remove_GamepadAdded;
    void* add_GamepadRemoved;
    void* remove_GamepadRemoved;
    HRESULT(STDMETHODCALLTYPE* get_Gamepads)(void* self, GamepadList** out);
};
struct GameControllerVtbl {  // IGameController, which a Gamepad also is
    UnknownVtbl unknown;
    void* add_HeadsetConnected;
    void* remove_HeadsetConnected;
    void* add_HeadsetDisconnected;
    void* remove_HeadsetDisconnected;
    void* add_UserChanged;
    void* remove_UserChanged;
    void* get_Headset;
    HRESULT(STDMETHODCALLTYPE* get_IsWireless)(void* self, unsigned char* out);
    void* get_User;
};
struct RawGameControllerVtbl {  // IRawGameController
    UnknownVtbl unknown;
    void* get_AxisCount;
    void* get_ButtonCount;
    void* get_ForceFeedbackMotors;
    HRESULT(STDMETHODCALLTYPE* get_HardwareProductId)(void* self, UINT16* out);
    HRESULT(STDMETHODCALLTYPE* get_HardwareVendorId)(void* self, UINT16* out);
    void* get_SwitchCount;
    void* GetButtonLabel;
    void* GetCurrentReading;
    void* GetSwitchKind;
};
struct RawGameController2Vtbl {  // IRawGameController2
    UnknownVtbl unknown;
    void* get_SimpleHapticsControllers;
    void* get_NonRoamableId;
    HRESULT(STDMETHODCALLTYPE* get_DisplayName)(void* self, HSTRING* out);
};
struct RawGameControllerStaticsVtbl {  // IRawGameControllerStatics
    UnknownVtbl unknown;
    void* add_RawGameControllerAdded;
    void* remove_RawGameControllerAdded;
    void* add_RawGameControllerRemoved;
    void* remove_RawGameControllerRemoved;
    void* get_RawGameControllers;
    HRESULT(STDMETHODCALLTYPE* FromGameController)(void* self, void* game_controller, void** out);
};
using IGamepadStatics = Interface<GamepadStaticsVtbl>;
using IGameController = Interface<GameControllerVtbl>;
using IRawGameController = Interface<RawGameControllerVtbl>;
using IRawGameController2 = Interface<RawGameController2Vtbl>;
using IRawGameControllerStatics = Interface<RawGameControllerStaticsVtbl>;

// The slots used, against the Windows SDK's windows.gaming.input.h (its C form lists them in this order).
constexpr size_t kSlot = sizeof(void*);
static_assert(__builtin_offsetof(GamepadListVtbl, GetAt) == 6 * kSlot && __builtin_offsetof(GamepadListVtbl, get_Size) == 7 * kSlot);
static_assert(__builtin_offsetof(GamepadVtbl, put_Vibration) == 7 * kSlot);
static_assert(__builtin_offsetof(GamepadStaticsVtbl, get_Gamepads) == 10 * kSlot);
static_assert(__builtin_offsetof(GameControllerVtbl, get_IsWireless) == 13 * kSlot);
static_assert(__builtin_offsetof(RawGameControllerVtbl, get_HardwareProductId) == 9 * kSlot &&
              __builtin_offsetof(RawGameControllerVtbl, get_HardwareVendorId) == 10 * kSlot);
static_assert(__builtin_offsetof(RawGameController2Vtbl, get_DisplayName) == 8 * kSlot);
static_assert(__builtin_offsetof(RawGameControllerStaticsVtbl, FromGameController) == 11 * kSlot);
static_assert(sizeof(GamepadVibration) == 4 * sizeof(double));

// The interface ids of the WinRT interfaces used .
constexpr GUID kIidGamepadStatics = {0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
constexpr GUID kIidGameController = {0x1baf6522, 0x5f64, 0x42c5, {0x82, 0x67, 0xb9, 0xfe, 0x22, 0x15, 0xbf, 0xbd}};
constexpr GUID kIidRawGameControllerStatics = {0xeb8d0792, 0xe95a, 0x4b19, {0xaf, 0xc7, 0x0a, 0x59, 0xf8, 0xbf, 0x75, 0x9e}};
constexpr GUID kIidRawGameController2 = {0x43c0c035, 0xbb73, 0x4756, {0xa7, 0x87, 0x3e, 0xd6, 0xbe, 0xa6, 0x17, 0xbd}};

// One reference to a WGI gamepad.
class PadRef {
public:
    PadRef() = default;
    explicit PadRef(IGamepad* pad) : pad_(pad) {}
    PadRef(PadRef&& other) noexcept : pad_(other.pad_) { other.pad_ = nullptr; }
    PadRef& operator=(PadRef&& other) noexcept {
        if (this != &other) {
            Reset();
            pad_ = other.pad_;
            other.pad_ = nullptr;
        }
        return *this;
    }
    PadRef(const PadRef&) = delete;
    PadRef& operator=(const PadRef&) = delete;
    ~PadRef() { Reset(); }
    IGamepad* Get() const { return pad_; }

private:
    void Reset() {
        if (pad_) pad_->lpVtbl->unknown.Release(pad_);
        pad_ = nullptr;
    }
    IGamepad* pad_ = nullptr;
};

// What the log line and the 360 check need to know about a pad.
struct Description {
    std::string name;
    bool ids_known = false;
    uint16_t vendor = 0;
    uint16_t product = 0;
    bool link_known = false;
    bool wireless = false;
};

struct Kept {
    PadRef pad;
    std::string name;
    bool shaking = false;       // the triggers were last set to something other than 0
    bool failure_logged = false;
};

struct Api {
    bool tried = false;
    bool ok = false;
    HRESULT(WINAPI* CoIncrementMTAUsage)(void** cookie) = nullptr;
    HRESULT(WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* WindowsCreateStringReference)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*) = nullptr;
    HRESULT(WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
    PCWSTR(WINAPI* WindowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    IGamepadStatics* gamepads = nullptr;                  // never released: the process ends
    IRawGameControllerStatics* raw_controllers = nullptr;  // optional (the ids and the name)
} g_api;

std::vector<Kept> g_kept;
std::vector<PadRef> g_skipped;  // listed pads that are left alone (held so their address cannot be reused)
int g_listed = -1;              // how many pads the last scan saw, for the log

template <class Fn>
bool Find(HMODULE module, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return out != nullptr;
}

uint32_t Code(HRESULT hr) { return static_cast<uint32_t>(hr); }

HRESULT Activate(const wchar_t* class_name, const GUID& iid, void** out) {
    HSTRING_HEADER header;
    HSTRING name = nullptr;
    HRESULT hr = g_api.WindowsCreateStringReference(class_name, static_cast<UINT32>(std::wcslen(class_name)),
                                                    &header, &name);
    if (FAILED(hr)) return hr;
    return g_api.RoGetActivationFactory(name, iid, out);  // a reference string needs no delete
}

// Starts WGI once. False: not available (the reason is in the log, once).
bool Start() {
    if (g_api.tried) return g_api.ok;
    g_api.tried = true;
    const auto fail = [](const char* what, HRESULT hr) {
        LARECOMP_APP_WARN("[Impulse] Windows.Gaming.Input is not available ({}, 0x{:08X}): no Xbox trigger "
                          "rumble",
                          what, Code(hr));
        return false;
    };
    HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!combase) return fail("combase.dll would not load", HRESULT_FROM_WIN32(GetLastError()));
    if (!Find(combase, "CoIncrementMTAUsage", g_api.CoIncrementMTAUsage) ||
        !Find(combase, "RoGetActivationFactory", g_api.RoGetActivationFactory) ||
        !Find(combase, "WindowsCreateStringReference", g_api.WindowsCreateStringReference) ||
        !Find(combase, "WindowsDeleteString", g_api.WindowsDeleteString) ||
        !Find(combase, "WindowsGetStringRawBuffer", g_api.WindowsGetStringRawBuffer)) {
        return fail("combase.dll has no WinRT activation", E_NOTIMPL);
    }
    // This thread never initialised COM: the process-wide multithreaded apartment is what WinRT then uses.
    void* cookie = nullptr;
    const HRESULT mta = g_api.CoIncrementMTAUsage(&cookie);
    if (FAILED(mta)) return fail("CoIncrementMTAUsage failed", mta);
    const HRESULT hr = Activate(L"Windows.Gaming.Input.Gamepad", kIidGamepadStatics,
                                reinterpret_cast<void**>(&g_api.gamepads));
    if (FAILED(hr) || !g_api.gamepads) return fail("Windows.Gaming.Input.Gamepad would not activate", hr);
    // Only for the log: an unreachable one leaves the ids and the name out.
    Activate(L"Windows.Gaming.Input.RawGameController", kIidRawGameControllerStatics,
             reinterpret_cast<void**>(&g_api.raw_controllers));
    g_api.ok = true;
    return true;
}

std::string Utf8(const wchar_t* text, UINT32 length) {
    if (!text || length == 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), bytes, nullptr, nullptr);
    return out;
}

// The pads WGI lists right now, each with a reference of its own.
std::vector<PadRef> List() {
    std::vector<PadRef> out;
    GamepadList* view = nullptr;
    if (FAILED(g_api.gamepads->lpVtbl->get_Gamepads(g_api.gamepads, &view)) || !view) return out;
    unsigned int size = 0;
    if (SUCCEEDED(view->lpVtbl->get_Size(view, &size))) {
        for (unsigned int i = 0; i < size; ++i) {
            IGamepad* pad = nullptr;
            if (SUCCEEDED(view->lpVtbl->GetAt(view, i, &pad)) && pad) out.emplace_back(pad);
        }
    }
    view->lpVtbl->unknown.Release(view);
    return out;
}

Description Describe(IGamepad* pad) {
    Description d;
    IGameController* controller = nullptr;
    if (FAILED(pad->lpVtbl->unknown.QueryInterface(pad, kIidGameController, reinterpret_cast<void**>(&controller))) || !controller) {
        return d;
    }
    unsigned char wireless = 0;
    if (SUCCEEDED(controller->lpVtbl->get_IsWireless(controller, &wireless))) {
        d.link_known = true;
        d.wireless = wireless != 0;
    }
    IRawGameController* raw = nullptr;
    if (g_api.raw_controllers && SUCCEEDED(g_api.raw_controllers->lpVtbl->FromGameController(g_api.raw_controllers, controller,
                                                                                  reinterpret_cast<void**>(&raw))) && raw) {
        UINT16 vendor = 0;
        UINT16 product = 0;
        if (SUCCEEDED(raw->lpVtbl->get_HardwareVendorId(raw, &vendor)) && SUCCEEDED(raw->lpVtbl->get_HardwareProductId(raw, &product))) {
            d.ids_known = true;
            d.vendor = vendor;
            d.product = product;
        }
        IRawGameController2* raw2 = nullptr;
        if (SUCCEEDED(raw->lpVtbl->unknown.QueryInterface(raw, kIidRawGameController2, reinterpret_cast<void**>(&raw2))) && raw2) {
            HSTRING name = nullptr;
            if (SUCCEEDED(raw2->lpVtbl->get_DisplayName(raw2, &name)) && name) {
                UINT32 length = 0;
                const PCWSTR text = g_api.WindowsGetStringRawBuffer(name, &length);
                d.name = Utf8(text, length);
                g_api.WindowsDeleteString(name);
            }
            raw2->lpVtbl->unknown.Release(raw2);
        }
        raw->lpVtbl->unknown.Release(raw);
    }
    controller->lpVtbl->unknown.Release(controller);
    return d;
}

// Microsoft's Xbox 360 type pads (wired, wireless through a receiver, the receivers themselves): no
// trigger motors. Steam Input's virtual pad also reports 045E:028E. Every other pad is tried: a
// third party's Xbox One type pad may have them, and one without ignores the trigger values.
bool KnownWithoutTriggerMotors(const Description& d) {
    if (!d.ids_known || d.vendor != 0x045E) return false;
    switch (d.product) {
        case 0x028E:
        case 0x028F:
        case 0x0291:
        case 0x02A0:
        case 0x02A1:
        case 0x02A9:
        case 0x0719:
            return true;
        default:
            return false;
    }
}

// Sets one pad's whole rumble: the game's two motors and the two triggers.
HRESULT Vibrate(IGamepad* pad, uint32_t game_motors, uint16_t left_trigger, uint16_t right_trigger) {
    GamepadVibration vibration;
    vibration.LeftMotor = static_cast<double>(game_motors >> 16) / 65535.0;
    vibration.RightMotor = static_cast<double>(game_motors & 0xFFFFu) / 65535.0;
    vibration.LeftTrigger = static_cast<double>(left_trigger) / 65535.0;
    vibration.RightTrigger = static_cast<double>(right_trigger) / 65535.0;
    return pad->lpVtbl->put_Vibration(pad, vibration);
}

}  // namespace

void Scan() {
    if (!Start()) return;
    std::vector<PadRef> listed = List();
    const int count = static_cast<int>(listed.size());
    if (count != g_listed) {
        g_listed = count;
        LARECOMP_APP_INFO("[Impulse] Windows.Gaming.Input lists {} gamepad{}{}", count, count == 1 ? "" : "s",
                          count == 0 ? " (no Xbox controller to rumble, or Windows has not listed it yet; a "
                                       "DualSense and a wheel are not gamepads to it)"
                                     : "");
    }
    const auto is_listed = [&](IGamepad* pad) {
        return std::any_of(listed.begin(), listed.end(), [&](const PadRef& l) { return l.Get() == pad; });
    };
    for (size_t i = 0; i < g_kept.size();) {
        if (is_listed(g_kept[i].pad.Get())) {
            ++i;
            continue;
        }
        LARECOMP_APP_INFO("[Impulse] {} is gone", g_kept[i].name);
        g_kept.erase(g_kept.begin() + static_cast<std::ptrdiff_t>(i));
    }
    g_skipped.erase(std::remove_if(g_skipped.begin(), g_skipped.end(),
                                   [&](const PadRef& s) { return !is_listed(s.Get()); }),
                    g_skipped.end());
    for (PadRef& l : listed) {
        const auto same = [&](const PadRef& r) { return r.Get() == l.Get(); };
        if (std::any_of(g_skipped.begin(), g_skipped.end(), same) ||
            std::any_of(g_kept.begin(), g_kept.end(), [&](const Kept& k) { return same(k.pad); })) {
            continue;
        }
        const Description d = Describe(l.Get());
        const std::string name = d.name.empty() ? "Xbox controller" : d.name;
        const char* link = !d.link_known ? "link unknown" : d.wireless ? "wireless" : "wired";
        const bool none = KnownWithoutTriggerMotors(d);
        const char* verdict = none ? "no trigger motors (an Xbox 360 type controller): left alone"
                                   : "trigger motors yes, driven through Windows.Gaming.Input";
        if (d.ids_known) {
            LARECOMP_APP_INFO("[Impulse] {} (vendor 0x{:04X}, product 0x{:04X}, {}): {}", name, d.vendor,
                              d.product, link, verdict);
        } else {
            LARECOMP_APP_INFO("[Impulse] {} (ids unknown, {}): {}", name, link, verdict);
        }
        if (none) {
            g_skipped.push_back(std::move(l));
        } else {
            g_kept.push_back(Kept{std::move(l), name});
        }
    }
}

bool HasPads() { return !g_kept.empty(); }

void Apply(uint16_t left, uint16_t right, uint32_t game_motors) {
    const bool release = left == 0 && right == 0;
    for (Kept& kept : g_kept) {
        if (release && !kept.shaking) continue;
        const HRESULT hr = Vibrate(kept.pad.Get(), game_motors, left, right);
        if (SUCCEEDED(hr)) {
            kept.shaking = !release;
        } else if (!kept.failure_logged) {
            kept.failure_logged = true;
            LARECOMP_APP_WARN("[Impulse] {}: Windows.Gaming.Input would not set the vibration (0x{:08X})",
                              kept.name, Code(hr));
        }
    }
}

void CloseAll(uint32_t game_motors) {
    Apply(0, 0, game_motors);
    g_kept.clear();
    g_skipped.clear();
    g_listed = -1;
}

}  // namespace larecomp::impulse::wgi
#endif  // _WIN32 && !REXGLUE_HAS_XEO3_TARGET
