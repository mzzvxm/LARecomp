// The device watcher: see device_watch.h for what it is and why it is built this way.

#include "device_watch.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "device_watch_core.h"
#include "larecomp_log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cfgmgr32.h>
#pragma comment(lib, "cfgmgr32.lib")
#else
#include <SDL3/SDL.h>

#include "trigger_sdl.h"
#endif

namespace larecomp::devwatch {
namespace {

// The change listeners, shared by both implementations. Added under a lock, read without one:
// `count` is published after the slot is written.
struct ListenerTable {
    std::mutex mutex;
    struct Slot {
        ChangeListener fn = nullptr;
        void* arg = nullptr;
    };
    std::array<Slot, 8> slots{};
    std::atomic<int> count{0};
};

// Everything here is allocated once and never freed: a pool thread or the startup thread may still
// be inside a callback while the process shuts down, and must not find a destroyed object.
ListenerTable& Listeners() {
    static ListenerTable* table = new ListenerTable();
    return *table;
}

void NotifyListeners() {
    ListenerTable& table = Listeners();
    const int count = table.count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) table.slots[size_t(i)].fn(table.slots[size_t(i)].arg);
}

}  // namespace

bool AddChangeListener(ChangeListener fn, void* arg) {
    if (!fn) return false;
    ListenerTable& table = Listeners();
    std::lock_guard<std::mutex> lock(table.mutex);
    const int count = table.count.load(std::memory_order_relaxed);
    if (count >= int(table.slots.size())) return false;
    table.slots[size_t(count)] = {fn, arg};
    table.count.store(count + 1, std::memory_order_release);
    return true;
}

#if defined(_WIN32)

namespace {

// GUID_DEVINTERFACE_HID: the class every HID interface is registered under (pads, keyboards, mice).
constexpr GUID kHidClass = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
// If the registration is refused (a Windows that lacks it, an environment that stubs it): the list
// is taken again this often, on the startup thread, never on the game's.
constexpr std::chrono::seconds kFallbackRescan{30};

struct State {
    std::mutex mutex;  // guards table
    DeviceTable table;
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> sony{false};
    std::atomic<uint32_t> dualsense{0};
    std::atomic<uint32_t> generation{0};
    std::atomic<HCMNOTIFICATION> notify{nullptr};
    std::atomic<uint32_t> events{0};  // notifications handled, for the log
};

State& S() {
    static State* state = new State();  // see Listeners()
    return *state;
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(c > 0 && c < 128 ? char(c) : '?');
    return out;
}

// Puts the table's answers where the game thread reads them. Called with the mutex held. The
// generation goes last: a reader that sees it moved sees the answers that go with it.
void Publish(const DeviceTable& table) {
    State& state = S();
    state.sony.store(table.summary().sony, std::memory_order_relaxed);
    state.dualsense.store(uint32_t(table.summary().dualsense.size()), std::memory_order_relaxed);
    state.ready.store(table.ready(), std::memory_order_relaxed);
    state.generation.store(table.generation(), std::memory_order_release);
}

// Every present HID interface link. False when the OS would not say.
bool ListHidInterfaces(std::vector<std::wstring>& out) {
    GUID guid = kHidClass;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG chars = 0;
        if (CM_Get_Device_Interface_List_SizeW(&chars, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
            return false;
        }
        std::vector<wchar_t> buffer(size_t(chars) + 2, L'\0');
        const CONFIGRET result = CM_Get_Device_Interface_ListW(&guid, nullptr, buffer.data(), ULONG(chars) + 1,
                                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_BUFFER_SMALL) continue;  // a device arrived between the two calls: ask again
        if (result != CR_SUCCESS) return false;
        for (const wchar_t* p = buffer.data(); *p; p += std::wcslen(p) + 1) out.emplace_back(p);
        return true;
    }
    return false;
}

void LogSummary(const char* what, const std::wstring* link, const DeviceTable& table) {
    LARECOMP_APP_INFO("[devwatch] {}{}{}: Sony pad {}, DualSense interfaces {} ({} HID interfaces present)", what,
                      link ? " " : "", link ? Narrow(*link) : std::string(), table.summary().sony ? "yes" : "no",
                      table.summary().dualsense.size(), table.count());
}

// A pool thread of the OS. Does the least it can: the link is classified from its name, and only
// the events that change what this module reports reach the listeners.
// CHECK: Windows does not say whether two notifications of one registration can run at the same time.
// If they can, the removal and the arrival of one link (a re-plug within microseconds) could be
// applied in the other order; the next change of that device corrects it. No test makes it happen.
// CHECK: never run against a DualSense (the PC this was written on has none): perf/scripts/chain-dshitch.ps1
// says what its run can and cannot show, and tools/device_watch_probe/ is the live test.
DWORD CALLBACK OnNotification(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD size) {
    if (!data || data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
    if (action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL && action != CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        return ERROR_SUCCESS;
    }
    const wchar_t* text = data->u.DeviceInterface.SymbolicLink;
    const size_t offset = size_t(reinterpret_cast<const char*>(text) - reinterpret_cast<const char*>(data));
    if (size <= offset) return ERROR_SUCCESS;
    // The link is a NUL-terminated string inside the event; never read past the size the OS gave.
    const size_t room = (size - offset) / sizeof(wchar_t);
    size_t length = 0;
    while (length < room && text[length] != L'\0') ++length;
    const std::wstring link = LowerLink(std::wstring(text, length));
    const bool arrival = action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL;

    State& state = S();
    state.events.fetch_add(1, std::memory_order_relaxed);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        changed = arrival ? state.table.Arrival(link) : state.table.Removal(link);
        if (changed) {
            Publish(state.table);
            if (ClassifyLink(link).sony) LogSummary(arrival ? "arrival" : "removal", &link, state.table);
        }
    }
    if (changed) NotifyListeners();
    return ERROR_SUCCESS;
}

// Takes a full list and makes it the table's truth (the first one, or a re-list of the fallback).
void TakeList(const char* what) {
    State& state = S();
    std::vector<std::wstring> links;
    const auto t0 = std::chrono::steady_clock::now();
    const bool listed = ListHidInterfaces(links);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!listed) {
        LARECOMP_APP_WARN("[devwatch] {}: Windows would not list the HID interfaces; pads that are already plugged in are not seen",
                          what);
        // Let the table become ready anyway (an empty list), so the answers do not wait for ever.
        links.clear();
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        changed = state.table.ApplySnapshot(links);
        Publish(state.table);
        LARECOMP_APP_INFO("[devwatch] {}: {} HID interfaces listed in {:.2f} ms; Sony pad {}, DualSense interfaces {}", what,
                          links.size(), ms, state.table.summary().sony ? "yes" : "no", state.table.summary().dualsense.size());
        for (const std::wstring& link : state.table.summary().dualsense) {
            LARECOMP_APP_INFO("[devwatch] DualSense interface {}", Narrow(link));
        }
    }
    if (changed) NotifyListeners();
}

// The startup thread: register first, then list, so that no arrival falls between the two (events
// that come before the list lands are held by the table and replayed over it).
void StartupMain() {
    State& state = S();
    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = kHidClass;
    HCMNOTIFICATION handle = nullptr;
    const CONFIGRET registered = CM_Register_Notification(&filter, nullptr, &OnNotification, &handle);
    if (registered == CR_SUCCESS) {
        state.notify.store(handle, std::memory_order_release);
        // Stop() may have run while this was registering: it found no handle to release.
        if (state.stopped.load(std::memory_order_acquire)) {
            if (HCMNOTIFICATION again = state.notify.exchange(nullptr)) CM_Unregister_Notification(again);
            return;
        }
    } else {
        LARECOMP_APP_WARN("[devwatch] CM_Register_Notification failed ({:#x}): pads are looked for again every {} s on a "
                          "thread of the watcher's own", unsigned(registered), kFallbackRescan.count());
    }
    TakeList("initial list");
    if (registered == CR_SUCCESS) return;
    while (!state.stopped.load(std::memory_order_acquire)) {
        // Stepping in short sleeps keeps Stop() from waiting for the full interval.
        for (int i = 0; i < int(kFallbackRescan.count()) && !state.stopped.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!state.stopped.load(std::memory_order_acquire)) TakeList("rescan");
    }
}

}  // namespace

void Start() {
    State& state = S();
    if (state.started.load(std::memory_order_acquire)) return;
    if (state.started.exchange(true, std::memory_order_acq_rel)) return;
    std::thread(StartupMain).detach();
}

void Stop() {
    State& state = S();
    if (state.stopped.exchange(true, std::memory_order_acq_rel)) return;
    if (HCMNOTIFICATION handle = state.notify.exchange(nullptr, std::memory_order_acq_rel)) {
        // Waits for a callback that is running, so nothing is inside one afterwards.
        CM_Unregister_Notification(handle);
        LARECOMP_APP_INFO("[devwatch] stopped after {} device notifications", state.events.load(std::memory_order_relaxed));
    }
}

bool Ready() {
    Start();
    return S().ready.load(std::memory_order_acquire);
}

bool SonyPadPresent() {
    Start();
    return S().sony.load(std::memory_order_acquire);
}

uint32_t DualSenseCount() {
    Start();
    return S().dualsense.load(std::memory_order_acquire);
}

uint32_t Generation() {
    Start();
    return S().generation.load(std::memory_order_acquire);
}

std::vector<std::wstring> DualSensePaths() {
    State& state = S();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.table.summary().dualsense;
}

#else  // !_WIN32

namespace {

// SDL's own pad list answers; the event watch only says that it may have changed. The watch can be
// added only once SDL's event system is up (the SDK's driver starts the gamepad layer), and a layer
// that is restarted loses its watches: neither happens before the process ends.
std::mutex g_mutex;
std::atomic<bool> g_attached{false};
std::atomic<bool> g_sony{false};
std::atomic<uint32_t> g_events{0};              // joystick / gamepad adds and removes seen, +1 when the watch was added
std::atomic<uint32_t> g_applied{0xFFFFFFFFu};   // the value of g_events that g_sony was worked out for

bool SDLCALL OnSdlEvent(void*, SDL_Event* event) {
    switch (event->type) {
        case SDL_EVENT_JOYSTICK_ADDED:
        case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            g_events.fetch_add(1, std::memory_order_acq_rel);
            NotifyListeners();
            break;
        default:
            break;
    }
    return true;  // a watch's result is ignored
}

// Adds the event watch once SDL's gamepad layer is up. A flag check when it is not (no scan of any
// kind), nothing at all afterwards. CHECK: not run on Linux or the Steam Deck.
void EnsureAttached() {
    if (g_attached.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_attached.load(std::memory_order_relaxed)) return;
        if (!larecomp::trigger_sdl::GamepadLayerUp()) return;
        if (!SDL_AddEventWatch(OnSdlEvent, nullptr)) {
            // Pads that come later are then not heard of; what is plugged in now is still read.
            LARECOMP_APP_WARN("[devwatch] SDL would not add the event watch: {}", SDL_GetError());
        }
        g_attached.store(true, std::memory_order_release);
        g_events.fetch_add(1, std::memory_order_acq_rel);  // the pads that were there before the watch
        LARECOMP_APP_INFO("[devwatch] SDL's gamepad layer is up: watching for joysticks coming and going");
    }
    NotifyListeners();
}

// Sony by USB vendor id, or a PlayStation gamepad type (what a pad behind a driver that reports
// other ids looks like). Virtual pads (Steam Input, the wheel code's) are not Sony hardware.
bool AnySonyJoystick() {
    bool found = false;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
        for (int i = 0; i < count && !found; ++i) {
            if (SDL_IsJoystickVirtual(ids[i])) continue;
            const SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[i]);
            found = SDL_GetJoystickVendorForID(ids[i]) == 0x054C || type == SDL_GAMEPAD_TYPE_PS3 ||
                    type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
        }
        SDL_free(ids);
    }
    return found;
}

// Works the answer out again when SDL added or removed a joystick since the last time: a read of
// SDL's list (a few microseconds), once per change.
void RefreshIfChanged() {
    const uint32_t events = g_events.load(std::memory_order_acquire);
    if (events == g_applied.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // another thread is on it; the next call sees the result
    const bool sony = AnySonyJoystick();
    if (sony != g_sony.load(std::memory_order_relaxed)) {
        LARECOMP_APP_INFO("[devwatch] Sony pad {}", sony ? "plugged in" : "gone");
    }
    g_sony.store(sony, std::memory_order_release);
    g_applied.store(events, std::memory_order_release);
}

}  // namespace

void Start() { EnsureAttached(); }

void Stop() {
    // The watch lives as long as SDL's event system; nothing to give back.
}

bool Ready() {
    EnsureAttached();
    return g_attached.load(std::memory_order_acquire);
}

bool SonyPadPresent() {
    EnsureAttached();
    if (!g_attached.load(std::memory_order_acquire)) return false;
    RefreshIfChanged();
    return g_sony.load(std::memory_order_acquire);
}

uint32_t DualSenseCount() { return 0; }

uint32_t Generation() {
    EnsureAttached();
    return g_events.load(std::memory_order_acquire);
}

#endif  // _WIN32

}  // namespace larecomp::devwatch
