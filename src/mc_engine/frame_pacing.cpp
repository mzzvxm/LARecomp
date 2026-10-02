// Frame pacing for the unlocked frame rate.
//
// The policy itself, and why the measured frame time is the wrong step to
// simulate, is in frame_pacing_policy.h. This file wires it into the game:
//
// - sub_821BDA90 (the clock update) is wrapped so the game's own clock can be
//   told apart from the other clock objects the same code updates, and so the
//   frame limiter can sleep right before the clock reads the timebase instead
//   of right after it. Sleeping after the read pushed the sleep into the next
//   frame's measurement and made every step carry the frame-to-frame variation
//   of the work.
// - MCLAFrameDelta hands the measured ticks to AdjustFrameTicks, which returns
//   the step to simulate.
// - sub_8231D3A8 is wrapped so the gameplay camera's two temporal filters are
//   scaled by the frame time.
// - frame_pacing_trace writes one line per event to frame_pacing_trace.csv, so
//   the pacing can be measured on a real session and replayed offline.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "frame_pacing.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../native_gfx/nocp/nocp_app.h"
#include "display_clock.h"
#include "frame_pacing_policy.h"
#include "logging.h"
#include "present_overlays.h"

REXCVAR_DEFINE_INT32(frame_pacing, 1, "MCLA/Performance",
    "How the game's time step follows the frame rate. 2: every frame is given a refresh of the "
    "monitor before it is simulated, advances by exactly the time until that refresh, and is "
    "held back until then if it is ready early -- each frame is on screen for exactly the time "
    "it simulated (native renderer only; otherwise behaves like 1). 1: while the game holds its "
    "rate (one vblank, or the FPS LIMIT period) every frame advances by exactly that period; "
    "when it does not, by a smoothed frame time. Simulation time stays on the wall clock "
    "either way. 0: every frame advances by its own measured time, as before -- a frame that "
    "misses a vblank then shows up three frames later as a double step, which reads as the car "
    "lurching back and forth.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(frame_pacing_headroom_ms, 1.0, "MCLA/Performance",
    "frame_pacing 2: extra time planned for every frame on top of how long frames usually take "
    "to be ready (the 97th percentile of the last few seconds). More means fewer frames that "
    "miss the refresh they were simulated for -- each miss shows as a small hitch -- at the "
    "cost of that much more input latency.")
    .range(0.0, 20.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_pacing_direct_present, false, "MCLA/Performance",
    "frame_pacing 2: the presenter shows each frame the moment it is handed over, instead of on "
    "the next of its own repaints. Those repaints (one per monitor refresh) exist for the "
    "overlays -- console, F3, settings, toasts -- so while this is on the overlays are detached "
    "and do not show.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_pacing_vrr, false, "MCLA/Performance",
    "frame_pacing 2 with frame_pacing_direct_present: the monitor runs variable refresh "
    "(G-SYNC / FreeSync), so it shows a frame when it arrives -- frames are planned to the "
    "tenth of a millisecond instead of to the monitor's refreshes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_pacing_trace, false, "MCLA/Diagnostics",
    "Write frame_pacing_trace.csv next to the executable: for every frame the measured and the "
    "simulated time step, every guest vblank and GPU interrupt, and the camera and player car "
    "positions handed to the renderer.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(camera_filters_dt, true, "MCLA/Camera",
    "Scale the gameplay camera's direction and orientation filters by the frame time, so they "
    "behave the way they do at 60 FPS at any frame rate. The game applies them as a fixed "
    "fraction per frame, which makes the camera sway with the frame time when it varies.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, real_frame_delta);
REXCVAR_DECLARE(int32_t, fps_limit);
REXCVAR_DECLARE(bool, mcla_native_gfx_own_swapchain);

// hooks.cpp
void EnforceFrameLimit();

// native_gfx.cpp: where the continuous present can be observed. The tag source
// runs on the render thread at the guest swap; the observer runs where the
// frame reaches the presenter (the submission thread, usually), with that tag.
namespace mcla::native_gfx {
extern uint64_t (*g_present_tag_source)();
extern bool (*g_present_paced)();
extern uint64_t (*g_present_release_time)(uint64_t tag);
extern void (*g_present_observer)(uint64_t tag, bool presented);
}  // namespace mcla::native_gfx

REX_EXTERN(__imp__rex_sub_821BDA90);
REX_EXTERN(__imp__rex_sub_82305B38);
REX_EXTERN(__imp__rex_sub_8231D3A8);
REX_EXTERN(__imp__D3DDevice_BlockOnFence);
REX_EXTERN(__imp__rex_sub_821775D8);

namespace {

// The game's clock object: sub_822C1FA8 updates it through its vtable slot 2.
constexpr uint32_t kGameClock = 0x827D7500;
constexpr uint32_t kClockScaledDelta = 0x827D7508;    // [clock+8]
constexpr uint32_t kClockUnscaledDelta = 0x827D7558;  // [clock+88]

// The D3D device (the r3 sub_8217B7B0 hands D3DDevice_Swap). D3DDevice_BlockOnFence
// waits until the value at [[device+10896]] -- written by the command processor
// as it retires the stream -- reaches the fence it was given; D3DDevice_Swap
// leaves the fence that follows its swap packet in device+14940. So the time
// [[device+10896]] passes a frame's swap fence is when the command processor
// got through that frame's swap: the closest thing to its present the game can
// see.
constexpr uint32_t kD3DDevicePtr = 0x82839254;
constexpr uint32_t kDeviceRetiredFencePtr = 10896;
constexpr uint32_t kDeviceSwapFence = 14940;

// Guest structures read by the trace (all found in sub_822C1FA8 / sub_822C0320).
constexpr uint32_t kPlayerManagerPtr = 0x82874374;  // players at +8, count at +132
constexpr uint32_t kVhsmPtr = 0x8286D804;           // +48 -> camera publisher
constexpr uint32_t kPublishedCamera = 332768;       // sub_8220AA68's final matrix
constexpr uint32_t kCamBoomVtable = 0x82044DB4;

// camBoomCS (sub_82320298) fields.
constexpr uint32_t kCsTune = 240;
constexpr uint32_t kCsUnscaledDtFlag = 184;  // sub_8271F020: byte set -> [clock+88]
constexpr uint32_t kCsFocus = 416;           // smoothed focus / offset, after ApproachRate
constexpr uint32_t kCsOffset = 432;
constexpr uint32_t kCsLongAccel = 568;       // normalised longitudinal acceleration
// Tune fields (sub_8231EF58), the two per-frame filters sub_8231D3A8 applies as
// lerp(state, target, factor): OrientationFilter on the up vector (cs+272) and
// DirectionFilter on the heading (cs+384). OrientationFactor at +212 is a
// static blend between world up and the car's up, not a filter, so it is left
// alone.
constexpr uint32_t kTuneOrientationFilter = 216;
constexpr uint32_t kTuneDirectionFilter = 220;

// Only the frame limiter and the game clock's own update run on this flag.
thread_local bool t_in_game_clock = false;

mc::pacing::Policy g_policy;
uint64_t g_frame = 0;
// The update frame whose state the render thread is drawing: set at the render
// sync, which hands that state over. The render thread finishes the frame
// (sub_821775D8, swap included) before the next sync can run, so it is stable
// for the whole of it.
std::atomic<uint64_t> g_render_frame{0};

uint8_t* Membase() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->virtual_membase() : nullptr;
}

// Guest address to host pointer, the way recompiled code computes it: the
// 0xE0000000 physical heap sits 0x1000 further on Windows.
uint8_t* Host(const uint8_t* base, uint32_t ea) {
    return rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea);
}

uint32_t LoadU32(const uint8_t* base, uint32_t ea) {
    uint32_t v;
    std::memcpy(&v, Host(base, ea), 4);
    return __builtin_bswap32(v);
}

float LoadF32(const uint8_t* base, uint32_t ea) {
    const uint32_t v = LoadU32(base, ea);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void StoreF32(uint8_t* base, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    std::memcpy(Host(base, ea), &v, 4);
}

// The trace chases pointers through live game objects; a stale one must not
// take the process down. No C++ objects in here: __try cannot unwind them.
bool SafeCopy(const uint8_t* base, uint32_t ea, void* dst, uint32_t bytes) {
    if (!base || !ea) return false;
#if defined(_WIN32)
    __try {
        std::memcpy(dst, Host(base, ea), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, Host(base, ea), bytes);
    return true;
#endif
}

bool SafeU32(const uint8_t* base, uint32_t ea, uint32_t& out) {
    uint32_t raw;
    if (!SafeCopy(base, ea, &raw, 4)) return false;
    out = __builtin_bswap32(raw);
    return true;
}

bool SafeVec3(const uint8_t* base, uint32_t ea, float out[3]) {
    uint32_t raw[3];
    if (!SafeCopy(base, ea, raw, 12)) return false;
    for (int i = 0; i < 3; ++i) {
        const uint32_t v = __builtin_bswap32(raw[i]);
        std::memcpy(&out[i], &v, 4);
    }
    return true;
}

// Anything past the null page: the D3D device and its fence block live in
// physical memory (0xA0000000 and up), game objects in the virtual heaps.
// SafeCopy catches what is not mapped.
bool PlausiblePtr(uint32_t v) { return v >= 0x10000u && !(v & 3u); }

int32_t FpsLimit() {
    // Same override EnforceFrameLimit honours.
    static const int32_t env_limit = [] {
        if (const char* e = std::getenv("MCLA_FPS_CAP")) return std::atoi(e);
        return -1;
    }();
    return env_limit >= 0 ? env_limit : REXCVAR_GET(fps_limit);
}

// ---- trace ----------------------------------------------------------------

std::mutex g_trace_mutex;
std::FILE* g_trace = nullptr;
uint32_t g_trace_lines = 0;

bool TraceOn() { return REXCVAR_GET(frame_pacing_trace); }

void TraceLineV(const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    if (!g_trace) {
        g_trace = std::fopen("frame_pacing_trace.csv", "w");
        if (!g_trace) return;
        std::fprintf(g_trace, "#host_freq,%llu,guest_freq,%llu\n",
                     static_cast<unsigned long long>(rex::chrono::Clock::QueryHostTickFrequency()),
                     static_cast<unsigned long long>(rex::chrono::Clock::guest_tick_frequency()));
        std::fprintf(g_trace,
                     "#F,host,frame,measured_us,step_us,ema_us,owed_us,period_us,mode\n"
                     "#V,host (guest vblank)\n"
                     "#I,host,retired_fence (PM4 interrupt)\n"
                     "#Y,host,frame,swap_fence (render sync; fence of the previous frame's swap)\n"
                     "#B,host_enter,host_exit,fence,how (D3DDevice_BlockOnFence; how 3 = the "
                     "wait D3DDevice_Swap ends with)\n"
                     "#D,host,retired_fence (command processor progress)\n"
                     "#P,host (host paint, right before the present)\n"
                     "#R,host_enter,host_exit,frame (render thread end of frame, sub_821775D8: "
                     "EndFrame, swap and, with the native swap chain, the present; frame = the "
                     "update frame whose state it rendered)\n"
                     "#G,host,frame,presented (native continuous present: that frame handed to "
                     "the presenter, or to the native swap chain)\n"
                     "#Q,host_enter,host_exit,present_count,sync_interval,flags,thread,hr (host "
                     "Present on the presenter's swap chain)\n"
                     "#S,host,present_count,present_refresh_count,sync_refresh_count,sync_qpc "
                     "(DXGI frame statistics right after a Present: the last Present that reached "
                     "the screen and the refresh it was shown at)\n"
                     "#M,host,hr,composition_mode,approved_present_duration (0 composed, "
                     "1 overlay, 2 none)\n"
                     "#L,host,frame,refresh,refreshes,used,min_refreshes,latency_us,period_ms "
                     "(frame_pacing 2: the refresh a frame is planned for, and its step in "
                     "refreshes)\n"
                     "#H,host_enter,host_release,frame,planned,target,shown (frame_pacing 2: "
                     "the frame held at the present gate)\n"
                     "#C,host,frame,cam_index,cs,cs_is_boom,cam_x,cam_y,cam_z,fwd_x,fwd_y,fwd_z,"
                     "car_x,car_y,car_z,rel_right,rel_up,rel_fwd,speed,long_accel,offset_z,focus_z\n");
    }
    std::vfprintf(g_trace, fmt, args);
    if (++g_trace_lines % 64 == 0) std::fflush(g_trace);
}

void TraceLine(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    TraceLineV(fmt, args);
    va_end(args);
}

unsigned long long HostNow() {
    return static_cast<unsigned long long>(rex::chrono::Clock::QueryHostTickCount());
}

// Fence of the most recent swap the render thread submitted, 0 if unreadable.
uint32_t LastSwapFence(const uint8_t* base) {
    uint32_t device = 0, fence = 0;
    if (!SafeU32(base, kD3DDevicePtr, device) || !PlausiblePtr(device)) return 0;
    if (!SafeU32(base, device + kDeviceSwapFence, fence)) return 0;
    return fence;
}

// The fence the command processor has retired, 0 if unreadable.
uint32_t RetiredFence(const uint8_t* base) {
    uint32_t device = 0, block = 0, retired = 0;
    if (!SafeU32(base, kD3DDevicePtr, device) || !PlausiblePtr(device)) return 0;
    if (!SafeU32(base, device + kDeviceRetiredFencePtr, block) || !PlausiblePtr(block)) return 0;
    if (!SafeU32(base, block, retired)) return 0;
    return retired;
}

// Watches the fence the command processor retires and logs every change, so
// the trace knows when each frame's swap went through. A thread of its own
// because the moment matters to well under a frame: a high-resolution waitable
// timer paces it at 250 us without touching the process timer resolution.
std::atomic<bool> g_fence_watch_started{false};

void FenceWatch() {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
#endif
    uint32_t last = 0;
    while (TraceOn()) {
        const uint32_t retired = RetiredFence(Membase());
        if (retired && retired != last) {
            last = retired;
            TraceLine("D,%llu,%u\n", HostNow(), retired);
        }
#if defined(_WIN32)
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -2500;  // 250 us, relative
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 5);
                continue;
            }
        }
        Sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(250));
#endif
    }
#if defined(_WIN32)
    if (timer) CloseHandle(timer);
#endif
    g_fence_watch_started.store(false);
}

void EnsureFenceWatch() {
    bool expected = false;
    if (g_fence_watch_started.compare_exchange_strong(expected, true))
        std::thread(FenceWatch).detach();
}

// Stamps every host paint. The presenter runs its UI drawers once per paint,
// after drawing the guest output and right before presenting, so this is when
// the frame the host is holding reaches the swap chain. Registered from the UI
// thread the first time the trace runs, and left in place: it only reads the
// clock.
class PaintProbe final : public rex::ui::UIDrawer {
public:
    void Draw(rex::ui::UIDrawContext&) override {
        if (TraceOn()) TraceLine("P,%llu\n", HostNow());
    }
};

PaintProbe g_paint_probe;
std::atomic<bool> g_paint_probe_registered{false};

void EnsurePaintProbe() {
    if (g_paint_probe_registered.load(std::memory_order_relaxed)) return;
    auto* runtime = rex::Runtime::instance();
    auto* context = runtime ? runtime->app_context() : nullptr;
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
    if (!context || !presenter) return;
    bool expected = false;
    if (!g_paint_probe_registered.compare_exchange_strong(expected, true)) return;
    context->CallInUIThreadDeferred(
        [presenter] { presenter->AddUIDrawerFromUIThread(&g_paint_probe, SIZE_MAX); });
}

// The native runtime's continuous present, tagged with the update frame the
// render thread was drawing when the guest swapped. Installed the first time
// the trace runs and left in place: both only read a counter and the clock.
uint64_t PresentTag() { return g_render_frame.load(std::memory_order_relaxed); }

void OnContinuousPresent(uint64_t frame, bool presented) {
    if (TraceOn())
        TraceLine("G,%llu,%llu,%d\n", HostNow(), static_cast<unsigned long long>(frame),
                  presented ? 1 : 0);
}

void EnsureNativePresentObserver() {
    if (mcla::native_gfx::g_present_observer == &OnContinuousPresent) return;
    mcla::native_gfx::g_present_tag_source = &PresentTag;
    mcla::native_gfx::g_present_observer = &OnContinuousPresent;
}

// ---- display-locked pacing (frame_pacing = 2) ------------------------------
//
// See frame_pacing_policy.h for why. Three places take part:
// - the frame start (the clock wrapper, main thread) picks the refresh the
//   frame is for, and waits first if the frame would only be held later;
// - AdjustFrameTicks turns the distance from the previous frame's refresh into
//   the step;
// - PresentReleaseTime tells native_gfx's present thread when to hand the
//   frame to the presenter so that it is shown at that refresh.

constexpr int kSlotRing = 64;
struct FrameSlot {
    std::atomic<uint64_t> frame{0};
    int64_t refresh = 0;
    uint64_t start_qpc = 0;
};
FrameSlot g_frame_slots[kSlotRing];

std::mutex g_latency_mutex;
mc::pacing::LatencyTracker g_latency;  // frame start -> ready to present, QPC ticks
mc::pacing::StepCap g_step_cap;

// Last time the native runtime asked for a release time: the hold only works
// where frames go through it.
std::atomic<uint64_t> g_last_gate_qpc{0};

// Main thread only.
bool g_have_last_refresh = false;
int64_t g_last_refresh = 0;
bool g_pending = false;
int64_t g_pending_refresh = 0;
uint64_t g_pending_start = 0;
double g_pending_period = 0.0;
int g_pending_min = 1;
double g_pending_latency = 0.0;

// Present thread only.
int64_t g_last_shown_refresh = INT64_MIN;

double QpcFrequency() {
    static const double f = static_cast<double>(rex::chrono::Clock::QueryHostTickFrequency());
    return f;
}

// Sleeps until `target` (QPC): a high-resolution waitable timer for the bulk,
// then a short spin. Used to hold a frame start back; a fraction of a
// millisecond either way changes nothing there.
void WaitUntilQpc(double target) {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    thread_local HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        return h ? h : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();
    const double freq = QpcFrequency();
    // Never wait for more than this, whatever the arithmetic says.
    const double limit = static_cast<double>(mc::display_clock::QpcNow()) + 0.15 * freq;
    if (target > limit) target = limit;
    for (;;) {
        const double remaining = target - static_cast<double>(mc::display_clock::QpcNow());
        if (remaining <= 0.0) return;
        if (timer && remaining > 0.0006 * freq) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((remaining - 0.0004 * freq) * 1e7 / freq);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 200);
                continue;
            }
        }
        YieldProcessor();
    }
#else
    (void)target;
#endif
}

bool VrrGrid() { return REXCVAR_GET(frame_pacing_direct_present) && REXCVAR_GET(frame_pacing_vrr); }

// The fewest grid steps between two frames on screen: one refresh, which on
// the variable refresh grid is the monitor's fastest.
int MonitorGap(const mc::display_clock::Grid& grid) {
    if (!VrrGrid()) return 1;
    const double monitor_hz = mc::display_clock::MonitorRefreshHz();
    const double fastest = monitor_hz > 1.0 ? monitor_hz : 240.0;
    return std::max(1, static_cast<int>(std::ceil(grid.RefreshHz() / fastest - 1e-6)));
}

int MinRefreshes(const mc::display_clock::Grid& grid) {
    const int minimum = MonitorGap(grid);
    const int32_t limit = FpsLimit();
    if (limit <= 0) return minimum;
    return std::max(minimum, static_cast<int>(std::lround(grid.RefreshHz() / limit)));
}

bool DisplayLockWanted() {
    return REXCVAR_GET(frame_pacing) == 2 && REXCVAR_GET(real_frame_delta);
}

bool DirectPresentWanted() {
    return DisplayLockWanted() && REXCVAR_GET(frame_pacing_direct_present);
}

// The refreshes frames are planned on: the monitor's own, or -- with variable
// refresh and direct presents, where the monitor shows a frame whenever it
// arrives -- a fine grid of its own.
mc::display_clock::Grid PacingGrid() {
    if (DisplayLockWanted() && VrrGrid()) {
        mc::display_clock::Grid grid;
        grid.valid = true;
        grid.anchor_index = 0;
        grid.anchor_qpc = 0.0;
        grid.qpc_frequency = QpcFrequency();
        grid.period_qpc = 0.0001 * grid.qpc_frequency;
        return grid;
    }
    mc::display_clock::EnsureRunning();
    return mc::display_clock::Get();
}

// Direct presents: the ImGui overlays come off the presenter while nothing of
// theirs is on screen (present_overlays.cpp). Main thread, once a frame.
void UpdateOverlayAttachment() {
    mc::present_overlays::Update(DirectPresentWanted() && mcla::native_gfx::nocp::PresenterPtr());
}

// At the frame start, before the game's clock reads the timebase. Returns false
// when the frame is not display-locked (the caller then runs the limiter).
bool BeginDisplayLockedFrame() {
    g_pending = false;
    if (!DisplayLockWanted()) {
        g_have_last_refresh = false;
        return false;
    }
    const mc::display_clock::Grid grid = PacingGrid();
    const double freq = QpcFrequency();
    const double now = static_cast<double>(mc::display_clock::QpcNow());
    const uint64_t last_gate = g_last_gate_qpc.load(std::memory_order_relaxed);
    if (!grid.valid || !last_gate || now - static_cast<double>(last_gate) > 0.25 * freq) {
        g_have_last_refresh = false;
        return false;
    }
    double latency;
    {
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        latency = g_latency.Percentile(0.97, 0.045 * freq);
    }
    latency += REXCVAR_GET(frame_pacing_headroom_ms) * 1e-3 * freq;
    const int min_refreshes = MinRefreshes(grid);
    int64_t refresh = grid.IndexAtOrAfter(now + latency);
    if (g_have_last_refresh) refresh = std::max(refresh, g_last_refresh + min_refreshes);
    // A frame that starts much earlier than its refresh needs only waits for it
    // at the present thread, adding input latency -- so a frame that far ahead
    // waits here instead. Up to one refresh early is left alone: waiting that
    // out here as well (measured: 2.7 ms a frame on average) made the main
    // thread the slower of the two and cost frames, and starting early is what
    // gives a frame room to come out a little slow and still make its refresh.
    const double start_by = grid.Time(refresh) - latency - grid.period_qpc;
    if (start_by > now + 0.0002 * freq) WaitUntilQpc(start_by);

    g_pending = true;
    g_pending_refresh = refresh;
    g_pending_start = mc::display_clock::QpcNow();
    g_pending_period = grid.period_qpc;
    g_pending_min = min_refreshes;
    g_pending_latency = latency;
    return true;
}

// Whether native_gfx should send frames through its present thread.
bool PresentPaced() {
    if (!DisplayLockWanted()) return false;
    return PacingGrid().valid;
}

// native_gfx's present thread, once per frame, as soon as the frame's own
// batches are on the queue: the QPC time to hand it to the presenter at (0 =
// right away). `frame` is the update frame it rendered.
uint64_t PresentReleaseTime(uint64_t frame) {
    const uint64_t enter = mc::display_clock::QpcNow();
    g_last_gate_qpc.store(enter, std::memory_order_relaxed);
    FrameSlot& slot = g_frame_slots[frame % kSlotRing];
    if (slot.frame.load(std::memory_order_acquire) != frame) {
        g_last_shown_refresh = INT64_MIN;
        return 0;
    }
    const int64_t planned = slot.refresh;
    {
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        g_latency.Add(static_cast<double>(enter - slot.start_qpc));
    }
    const mc::display_clock::Grid grid = PacingGrid();
    if (!grid.valid) return 0;
    const double freq = QpcFrequency();
    // Two frames on one refresh would show only the second; on the variable
    // refresh grid, one refresh is the monitor's fastest, not one grid step.
    int64_t target = planned;
    if (g_last_shown_refresh != INT64_MIN)
        target = std::max(target, g_last_shown_refresh + MonitorGap(grid));
    // The presenter paints right after each vblank, with whatever it was
    // handed by then: hand the frame over in the middle of the refresh before
    // its own, as far from both edges as possible. Presented on the spot
    // instead -- a swap chain of the runtime's own, or the presenter with no
    // overlays attached -- it goes right after the vblank, or, on a variable
    // refresh grid, right at its time.
    const bool presents_now = REXCVAR_GET(mcla_native_gfx_own_swapchain) ||
                              (DirectPresentWanted() && mc::present_overlays::Detached());
    double release = presents_now ? grid.Time(target) + 0.0001 * freq
                                  : grid.Time(target) - 0.5 * grid.period_qpc;
    const double now = static_cast<double>(enter);
    int64_t shown = target;
    if (release <= now) {
        // Late: it goes now and makes the first refresh it still can.
        release = 0.0;
        shown = presents_now ? grid.IndexAtOrAfter(now - 0.0002 * freq)
                             : grid.IndexAtOrAfter(now + 0.0003 * freq);
    }
    g_last_shown_refresh = shown;
    if (TraceOn())
        TraceLine("H,%llu,%llu,%llu,%lld,%lld,%lld\n", static_cast<unsigned long long>(enter),
                  static_cast<unsigned long long>(release > 0.0 ? release : now),
                  static_cast<unsigned long long>(frame), static_cast<long long>(planned),
                  static_cast<long long>(target), static_cast<long long>(shown));
    return release > 0.0 ? static_cast<uint64_t>(release) : 0;
}

void EnsurePresentGate() {
    if (mcla::native_gfx::g_present_release_time == &PresentReleaseTime) return;
    mcla::native_gfx::g_present_tag_source = &PresentTag;
    mcla::native_gfx::g_present_release_time = &PresentReleaseTime;
    mcla::native_gfx::g_present_paced = &PresentPaced;
}

// The camera and car pair that is about to be handed to the render thread.
void TraceCameraAndCar(const uint8_t* base, uint64_t frame) {
    uint32_t mgr = 0, player = 0, veh = 0, sim = 0;
    if (!SafeU32(base, kPlayerManagerPtr, mgr) || !PlausiblePtr(mgr)) return;
    if (!SafeU32(base, mgr + 8, player) || !PlausiblePtr(player)) return;
    if (!SafeU32(base, player + 48, veh) || !PlausiblePtr(veh)) return;

    float car[3] = {}, speed = 0.0f;
    if (!SafeVec3(base, veh + 144 + 48, car)) return;
    if (SafeU32(base, veh + 8, sim) && PlausiblePtr(sim)) {
        uint32_t raw = 0;
        if (SafeU32(base, sim + 224, raw)) std::memcpy(&speed, &raw, 4);
    }

    uint32_t vhsm = 0, publisher = 0;
    if (!SafeU32(base, kVhsmPtr, vhsm) || !PlausiblePtr(vhsm)) return;
    if (!SafeU32(base, vhsm + 48, publisher) || !PlausiblePtr(publisher)) return;
    const uint32_t m = publisher + kPublishedCamera;
    float right[3], up[3], fwd[3], cam[3];
    if (!SafeVec3(base, m, right) || !SafeVec3(base, m + 16, up) ||
        !SafeVec3(base, m + 32, fwd) || !SafeVec3(base, m + 48, cam))
        return;

    const float rel[3] = {car[0] - cam[0], car[1] - cam[1], car[2] - cam[2]};
    auto dot = [](const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    uint32_t pcam = 0, view = 0, cs = 0, cam_index = 0xFFFFFFFFu, vt = 0;
    float long_accel = 0.0f, offset_z = 0.0f, focus_z = 0.0f;
    if (SafeU32(base, player + 880, pcam) && PlausiblePtr(pcam)) {
        SafeU32(base, pcam + 80, cam_index);
        if (SafeU32(base, pcam + 16, view) && PlausiblePtr(view) && SafeU32(base, view + 48, cs) &&
            PlausiblePtr(cs) && SafeU32(base, cs, vt) && vt == kCamBoomVtable) {
            uint32_t raw = 0;
            if (SafeU32(base, cs + kCsLongAccel, raw)) std::memcpy(&long_accel, &raw, 4);
            if (SafeU32(base, cs + kCsOffset + 8, raw)) std::memcpy(&offset_z, &raw, 4);
            if (SafeU32(base, cs + kCsFocus + 8, raw)) std::memcpy(&focus_z, &raw, 4);
        }
    }

    TraceLine("C,%llu,%llu,%d,0x%08X,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,"
              "%.4f,%.3f,%.4f,%.4f,%.4f\n",
              HostNow(), static_cast<unsigned long long>(frame), static_cast<int>(cam_index), cs,
              vt == kCamBoomVtable ? 1 : 0, cam[0], cam[1], cam[2], fwd[0], fwd[1], fwd[2],
              car[0], car[1], car[2], dot(rel, right), dot(rel, up), dot(rel, fwd), speed,
              long_accel, offset_z, focus_z);
}

}  // namespace

namespace mc::pacing {

bool LimiterRunsBeforeClock() { return REXCVAR_GET(frame_pacing) != 0; }

uint64_t AdjustFrameTicks(uint64_t elapsed_ticks) {
    if (!t_in_game_clock) return elapsed_ticks;
    ++g_frame;

    const double freq = static_cast<double>(rex::chrono::Clock::guest_tick_frequency());
    const int32_t limit = FpsLimit();
    const double limiter_period = (limit > 0 && freq > 0.0) ? freq / limit : 0.0;
    // One guest vblank, but only while the runtime's vsync actually paces the
    // swaps. larecomp_app.h turns vsync off unless MCLA_VSYNC says otherwise,
    // and then the vblank worker ticks every millisecond: no cadence to snap to.
    // Read every frame, since the pause menu can flip it.
    const bool vsync_on = rex::cvar::GetFlagByName("vsync") == "true";
    const double vblank_period = (vsync_on && freq > 0.0) ? freq / 60.0 : 0.0;

    const int mode = REXCVAR_GET(frame_pacing);
    uint64_t step = elapsed_ticks;
    if (g_pending && freq > 0.0) {
        // Display-locked: exactly the time from the previous frame's refresh
        // to this one's.
        g_pending = false;
        // The first frame after locking on has no previous refresh to count
        // from: it keeps its measured time.
        double refreshes = 0.0, used = 0.0;
        if (g_have_last_refresh) {
            refreshes = static_cast<double>(g_pending_refresh - g_last_refresh);
            used = g_step_cap.Apply(refreshes, g_pending_min);
            const double dt = used * g_pending_period * freq / QpcFrequency();
            step = dt >= 1.0 ? static_cast<uint64_t>(dt + 0.5) : 1;
        }
        FrameSlot& slot = g_frame_slots[g_frame % kSlotRing];
        slot.refresh = g_pending_refresh;
        slot.start_qpc = g_pending_start;
        slot.frame.store(g_frame, std::memory_order_release);
        g_last_refresh = g_pending_refresh;
        g_have_last_refresh = true;
        g_policy.Reset();
        if (TraceOn()) {
            TraceLine("L,%llu,%llu,%lld,%.0f,%.2f,%d,%.1f,%.4f\n", HostNow(),
                      static_cast<unsigned long long>(g_frame),
                      static_cast<long long>(g_pending_refresh), refreshes, used, g_pending_min,
                      g_pending_latency * 1e6 / QpcFrequency(),
                      g_pending_period * 1e3 / QpcFrequency());
        }
    } else if (mode != 0 && REXCVAR_GET(real_frame_delta) && freq > 0.0) {
        g_step_cap.Reset();
        const double dt = g_policy.Next(static_cast<double>(elapsed_ticks), limiter_period,
                                        vblank_period, 0.1 * freq);
        step = dt >= 1.0 ? static_cast<uint64_t>(dt + 0.5) : 1;
    } else {
        g_step_cap.Reset();
        g_policy.Reset();
    }

    if (TraceOn() && freq > 0.0) {
        const double to_us = 1e6 / freq;
        TraceLine("F,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n", HostNow(),
                  static_cast<unsigned long long>(g_frame), elapsed_ticks * to_us, step * to_us,
                  g_policy.ema * to_us, g_policy.owed * to_us,
                  (limiter_period > 0.0 ? limiter_period : vblank_period) * to_us, mode);
    }
    return step;
}

void OnGuestInterrupt(uint32_t source) {
    if (!TraceOn()) return;
    if (source == 1)
        TraceLine("I,%llu,%u\n", HostNow(), RetiredFence(Membase()));
    else
        TraceLine("V,%llu\n", HostNow());
}

bool TraceEnabled() { return TraceOn(); }

void TraceWrite(const char* fmt, ...) {
    if (!TraceOn()) return;
    va_list args;
    va_start(args, fmt);
    TraceLineV(fmt, args);
    va_end(args);
}

}  // namespace mc::pacing

// sub_821BDA90: the clock update. r3 is the clock object.
extern "C" REX_FUNC(rex_sub_821BDA90) {
    const bool game_clock = ctx.r3.u32 == kGameClock;
    if (game_clock) {
        // Display-locked frames wait for their own start instead; the limiter's
        // period is folded into the refresh they are given.
        if (!BeginDisplayLockedFrame() && mc::pacing::LimiterRunsBeforeClock())
            EnforceFrameLimit();
        t_in_game_clock = true;
    }
    __imp__rex_sub_821BDA90(ctx, base);
    if (game_clock) t_in_game_clock = false;
}

// sub_82305B38: the render sync. Everything the update produced this frame is
// final here; the snapshot copies it for the render thread.
extern "C" REX_FUNC(rex_sub_82305B38) {
    g_render_frame.store(g_frame, std::memory_order_relaxed);
    EnsurePresentGate();
    UpdateOverlayAttachment();
    if (TraceOn()) {
        EnsureFenceWatch();
        EnsurePaintProbe();
        EnsureNativePresentObserver();
        // The render thread has returned from the swap of the previous frame by
        // now (sub_823057E8 waited for it), so this is that frame's swap fence.
        TraceLine("Y,%llu,%llu,%u\n", HostNow(), static_cast<unsigned long long>(g_frame),
                  LastSwapFence(base));
        TraceCameraAndCar(base, g_frame);
    }
    __imp__rex_sub_82305B38(ctx, base);
}

// sub_821775D8: the render thread's end of frame -- it ends in grcDevice_EndFrame
// and so in D3DDevice_Swap. With the native runtime's own swap chain the
// present happens inside, so the exit time is when the frame was presented.
extern "C" REX_FUNC(rex_sub_821775D8) {
    if (!TraceOn()) {
        __imp__rex_sub_821775D8(ctx, base);
        return;
    }
    const unsigned long long enter = HostNow();
    __imp__rex_sub_821775D8(ctx, base);
    TraceLine("R,%llu,%llu,%llu\n", enter, HostNow(),
              static_cast<unsigned long long>(g_render_frame.load(std::memory_order_relaxed)));
}

// D3DDevice_BlockOnFence (sub_82411E98): r4 the fence, r5 how to wait. The one
// D3DDevice_Swap ends with (r5 = 3) waits for the previous frame's swap, so its
// exit is when the render thread saw that frame go through. Other calls are
// logged only when they actually waited.
extern "C" REX_FUNC(D3DDevice_BlockOnFence) {
    if (!TraceOn()) {
        __imp__D3DDevice_BlockOnFence(ctx, base);
        return;
    }
    const uint32_t fence = ctx.r4.u32;
    const uint32_t how = ctx.r5.u32;
    const unsigned long long enter = HostNow();
    __imp__D3DDevice_BlockOnFence(ctx, base);
    const unsigned long long exit = HostNow();
    if (how == 3 || exit - enter > 1000)
        TraceLine("B,%llu,%llu,%u,%u\n", enter, exit, fence, how);
}

// sub_8231D3A8: builds the boom camera's frame, filtering its up vector and its
// heading by a fixed fraction per frame (OrientationFilter / DirectionFilter).
// The fractions are swapped for their frame-time equivalents for the duration
// of the call: 1 - (1 - k)^(60 dt), which is k itself at exactly 60 FPS.
extern "C" REX_FUNC(rex_sub_8231D3A8) {
    uint8_t* mem = base;
    const uint32_t cs = ctx.r3.u32;
    uint32_t tune = 0;
    float saved[2] = {};
    bool patched = false;
    if (REXCVAR_GET(camera_filters_dt) && PlausiblePtr(cs)) {
        tune = LoadU32(mem, cs + kCsTune);
        if (PlausiblePtr(tune)) {
            const float dt = mem[cs + kCsUnscaledDtFlag] ? LoadF32(mem, kClockUnscaledDelta)
                                                         : LoadF32(mem, kClockScaledDelta);
            if (dt > 0.0f && dt < 0.25f) {
                const uint32_t fields[2] = {kTuneOrientationFilter, kTuneDirectionFilter};
                for (int i = 0; i < 2; ++i) {
                    saved[i] = LoadF32(mem, tune + fields[i]);
                    const double k = saved[i];
                    if (k > 0.0 && k < 1.0)
                        StoreF32(mem, tune + fields[i],
                                 static_cast<float>(1.0 - std::pow(1.0 - k, 60.0 * dt)));
                }
                patched = true;
            }
        }
    }
    __imp__rex_sub_8231D3A8(ctx, base);
    if (patched) {
        StoreF32(mem, tune + kTuneOrientationFilter, saved[0]);
        StoreF32(mem, tune + kTuneDirectionFilter, saved[1]);
    }
}

#endif  // REXGLUE_HAS_XEO3_TARGET
