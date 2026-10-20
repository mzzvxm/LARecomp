//
// DirectInput reader for the steering wheel. See wheel_dinput.h.
//

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "wheel_dinput.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "larecomp_log.h"

#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

namespace larecomp::wheel::dinput {

#ifdef _WIN32

namespace {

constexpr int kAxes = 8, kPovs = 4, kButtons = 32;

// The layout asked of DirectInput: DIJOYSTATE2's first fields, cut after 32 buttons.
struct State {
    LONG axis[kAxes];
    DWORD pov[kPovs];
    BYTE button[kButtons];
};
static_assert(sizeof(State) % 4 == 0, "DirectInput wants a data size that is a multiple of 4");

typedef HRESULT(WINAPI* DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

struct Found {
    DIDEVICEINSTANCEW inst;
};
struct FindCtx {
    DWORD product = 0;
    std::vector<Found> found;
};

BOOL CALLBACK FindEach(const DIDEVICEINSTANCEW* inst, void* user) {
    auto* ctx = static_cast<FindCtx*>(user);
    if (inst->guidProduct.Data1 == ctx->product) ctx->found.push_back({*inst});
    return DIENUM_CONTINUE;
}

}  // namespace

struct Pad {
    HMODULE lib = nullptr;
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    HWND helper = nullptr;
    State st{};
    bool ok = false;
};

namespace {

bool Zero(const State& s) {
    for (int i = 0; i < kAxes; ++i) {
        if (s.axis[i] != 0) return false;
    }
    for (int i = 0; i < kButtons; ++i) {
        if (s.button[i]) return false;
    }
    return true;
}

// Opens one instance; true when it can be read. Every HRESULT goes to the log: this is the diagnosis for a wheel
// that reads as nothing.
bool OpenOne(Pad& p, const Found& f, int number, std::string* summary) {
    char name[160] = {};
    WideCharToMultiByte(CP_UTF8, 0, f.inst.tszProductName, -1, name, sizeof name - 1, nullptr, nullptr);
    auto step = [&](const char* what, HRESULT hr) {
        if (FAILED(hr)) {
            LARECOMP_APP_WARN("[wheel] directinput #{} '{}': {} failed, hr=0x{:08x}", number, name, what, unsigned(hr));
            return false;
        }
        return true;
    };
    if (!step("CreateDevice", p.di->CreateDevice(f.inst.guidInstance, &p.dev, nullptr))) return false;

    // Axes in DIJOYSTATE2 order, then four hats, then the buttons. Optional: a wheel without them still opens.
    static const GUID* axis_guid[kAxes] = {&GUID_XAxis, &GUID_YAxis,  &GUID_ZAxis,  &GUID_RxAxis,
                                           &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
    DIOBJECTDATAFORMAT objs[kAxes + kPovs + kButtons];
    int n = 0;
    for (int i = 0; i < kAxes; ++i) {
        objs[n++] = {axis_guid[i], DWORD(offsetof(State, axis) + i * sizeof(LONG)),
                     DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0};
    }
    for (int i = 0; i < kPovs; ++i) {
        objs[n++] = {&GUID_POV, DWORD(offsetof(State, pov) + i * sizeof(DWORD)),
                     DIDFT_POV | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0};
    }
    for (int i = 0; i < kButtons; ++i) {
        objs[n++] = {nullptr, DWORD(offsetof(State, button) + i), DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL,
                     0};
    }
    DIDATAFORMAT fmt = {};
    fmt.dwSize = sizeof(fmt);
    fmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    fmt.dwFlags = DIDF_ABSAXIS;
    fmt.dwDataSize = sizeof(State);
    fmt.dwNumObjs = DWORD(n);
    fmt.rgodf = objs;
    if (!step("SetDataFormat", p.dev->SetDataFormat(&fmt))) return false;

    p.helper = CreateWindowExW(0, L"STATIC", L"larecomp wheel read", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!step("SetCooperativeLevel(non-exclusive, background)",
              p.dev->SetCooperativeLevel(p.helper, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
        return false;
    }

    // Same scale as SDL: the whole travel of an axis is -32768..32767, no dead zone.
    for (int i = 0; i < kAxes; ++i) {
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof(r);
        r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        r.diph.dwHow = DIPH_BYOFFSET;
        r.diph.dwObj = DWORD(offsetof(State, axis) + i * sizeof(LONG));
        r.lMin = -32768;
        r.lMax = 32767;
        p.dev->SetProperty(DIPROP_RANGE, &r.diph);  // an axis the wheel does not have refuses; that is fine
        DIPROPDWORD dz = {};
        dz.diph.dwSize = sizeof(dz);
        dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        dz.diph.dwHow = DIPH_BYOFFSET;
        dz.diph.dwObj = r.diph.dwObj;
        dz.dwData = 0;
        p.dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    }

    const HRESULT acq = p.dev->Acquire();
    if (FAILED(acq)) {
        LARECOMP_APP_WARN("[wheel] directinput #{} '{}': Acquire failed, hr=0x{:08x} (0x80070005 or 0x00000001 mean "
                          "another program or handle has the wheel)", number, name, unsigned(acq));
        return false;
    }
    p.dev->Poll();
    State s{};
    const HRESULT got = p.dev->GetDeviceState(sizeof(s), &s);
    if (FAILED(got)) {
        LARECOMP_APP_WARN("[wheel] directinput #{} '{}': GetDeviceState failed, hr=0x{:08x}", number, name,
                          unsigned(got));
        return false;
    }
    p.st = s;
    char buf[200];
    std::snprintf(buf, sizeof buf, "x=%ld y=%ld z=%ld rx=%ld ry=%ld rz=%ld s0=%ld s1=%ld", long(s.axis[0]),
                  long(s.axis[1]), long(s.axis[2]), long(s.axis[3]), long(s.axis[4]), long(s.axis[5]),
                  long(s.axis[6]), long(s.axis[7]));
    *summary = buf;
    LARECOMP_APP_INFO("[wheel] directinput #{} '{}': acquired, first state: {}", number, name, buf);
    return true;
}

void Release(Pad& p) {
    if (p.dev) {
        p.dev->Unacquire();
        p.dev->Release();
        p.dev = nullptr;
    }
    if (p.helper) {
        DestroyWindow(p.helper);
        p.helper = nullptr;
    }
}

}  // namespace

Pad* Open(uint16_t vid, uint16_t pid, bool* live) {
    *live = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE lib = LoadLibraryW(L"dinput8.dll");
    if (!lib) return nullptr;
    auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(lib, "DirectInput8Create"));
    IDirectInput8W* di = nullptr;
    if (!create ||
        FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                      reinterpret_cast<void**>(&di), nullptr)) ||
        !di) {
        return nullptr;
    }
    FindCtx ctx;
    ctx.product = MAKELONG(vid, pid);
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, FindEach, &ctx, DIEDFL_ATTACHEDONLY);
    LARECOMP_APP_INFO("[wheel] directinput lists {} device(s) for {:04x}:{:04x}", ctx.found.size(), vid, pid);

    Pad* keep = nullptr;
    int number = 0;
    for (const Found& f : ctx.found) {
        auto* p = new Pad();
        p->lib = lib;
        p->di = di;
        std::string summary;
        if (!OpenOne(*p, f, ++number, &summary)) {
            Release(*p);
            delete p;
            continue;
        }
        p->ok = true;
        const bool alive = !Zero(p->st);
        if (!keep || (alive && !*live)) {
            if (keep) {
                Release(*keep);
                delete keep;
            }
            keep = p;
            *live = alive;
        } else {
            Release(*p);
            delete p;
        }
        if (*live) break;
    }
    if (!keep) {
        di->Release();
        FreeLibrary(lib);
    }
    return keep;
}

void Poll(Pad* p) {
    if (!p || !p->dev) return;
    HRESULT hr = p->dev->Poll();
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        p->dev->Acquire();
        p->dev->Poll();
    }
    State s{};
    if (SUCCEEDED(p->dev->GetDeviceState(sizeof(s), &s))) p->st = s;
}

int Axis(const Pad* p, int index) {
    if (!p || index < 0 || index >= kAxes) return 0;
    return int(p->st.axis[index]);
}

bool Button(const Pad* p, int index) {
    if (!p || index < 0 || index >= kButtons) return false;
    return (p->st.button[index] & 0x80) != 0;
}

int Hat(const Pad* p, int index) {
    if (!p || index < 0 || index >= kPovs) return 0;
    const DWORD v = p->st.pov[index];
    if (LOWORD(v) == 0xFFFF) return 0;
    const int deg = int(v) / 100;  // centidegrees clockwise from up
    int bits = 0;
    if (deg > 292 || deg < 68) bits |= 1;
    if (deg > 22 && deg < 158) bits |= 2;
    if (deg > 112 && deg < 248) bits |= 4;
    if (deg > 202 && deg < 338) bits |= 8;
    return bits;
}

void Close(Pad* p) {
    if (!p) return;
    Release(*p);
    delete p;
}

#else  // !_WIN32

Pad* Open(uint16_t, uint16_t, bool* live) {
    *live = false;
    return nullptr;
}
void Poll(Pad*) {}
int Axis(const Pad*, int) { return 0; }
bool Button(const Pad*, int) { return false; }
int Hat(const Pad*, int) { return 0; }
void Close(Pad*) {}

#endif

}  // namespace larecomp::wheel::dinput

#endif  // REXGLUE_HAS_XEO3_TARGET
