#ifndef REXGLUE_HAS_XEO3_TARGET
#include "map_mouse.h"
#include "logging.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(map_mouse, true, "MCLA/Input",
    "Drive the full map's cursor with the mouse. The pointer position is "
    "unprojected onto the map plane, so the cursor lands where you point "
    "instead of drifting like a stick would.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(map_mouse_fov, 45.0, "MCLA/Input",
    "Vertical field of view, in degrees, of the camera the full map is drawn "
    "with. The only number the unprojection cannot read out of the game. Too "
    "small and the cursor lags behind the pointer towards the screen edges; "
    "too large and it runs ahead. The centre of the screen is correct at any "
    "value, so calibrate by pointing at a corner.")
    .range(5.0, 150.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_flip_x, false, "MCLA/Input",
    "Mirror the horizontal axis of the map cursor. Needed only if the camera "
    "basis rows turn out to be left-handed -- the cursor then moves the wrong "
    "way along one axis while the other tracks correctly.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_flip_y, false, "MCLA/Input",
    "Mirror the vertical axis of the map cursor. See map_mouse_flip_x.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_zoom, true, "MCLA/Input",
    "Zoom the full map with the scroll wheel. The camera height is the zoom, "
    "so this walks it through the game's own min/max at +1124/+1128.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(map_mouse_zoom_step, 1.15, "MCLA/Input",
    "Multiplier applied to the map camera's height per wheel detent.")
    .range(1.01, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(map_mouse_zoom_smooth, 0.22, "MCLA/Input",
    "How much of the remaining distance to the wheel's target height the "
    "camera covers each frame. A detent is a step, and stepping the camera "
    "straight to it is what made the zoom read as jerky; this eases into it "
    "instead. 1.0 restores the instant jump.")
    .range(0.02, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_zoom_to_cursor, true, "MCLA/Input",
    "Zoom around the point under the pointer instead of around the middle of "
    "the view. With this off the camera only changes height, which slides the "
    "world under a pointer that has not moved and drags the cursor towards "
    "the centre on every detent.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_magnet, false, "MCLA/Input",
    "Leave the map's cursor magnet alone. The map snaps the cursor onto the "
    "selected icon, both on selecting it and again on every frame it stays "
    "selected, which is how a thumbstick is made to feel sticky over "
    "waypoints and what welds a pointer to the first one it brushes. Off "
    "suppresses both snaps for whichever device currently owns the cursor "
    "being the mouse -- a stick moved out of its deadzone takes ownership "
    "back and gets its magnet with it. The selection and its highlight are "
    "never touched either way, so hovering works regardless.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(map_mouse_max_steps, 96, "MCLA/Input",
    "Ceiling on how many stick-sized steps one frame may walk the cursor "
    "towards the pointer. The map's boundary sweep is written for the small "
    "deltas a stick produces and reads a null neighbour pointer when handed a "
    "long chord, so the move is subdivided rather than teleported. Anything "
    "left over is covered by the next frame, so a ceiling too low shows up as "
    "a cursor that crawls after a fast flick rather than as anything worse. "
    "The walk stops as soon as it arrives or stops making progress, so a high "
    "ceiling costs nothing on the small movements that make up most frames.")
    .range(1, 512)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_hide_cursor, true, "MCLA/Input",
    "Hide the OS pointer while the full map is up. The in-game cursor tracks "
    "it one to one, so the visible cursor is the game's own.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(map_mouse_diag, false, "MCLA/Input",
    "Log the map screen pointer, camera basis and the world point the mouse "
    "unprojects to, once a second.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// ── Guest addresses ────────────────────────────────────────────────────

// sub_826263C0(screen, x, y, z) -- SetCursorPosition. Clamps against the
// cursor bounds, writes screen+448 and publishes cur_cursor_Position /
// _Rotation / _Menuscale / cursor_visibility to the movie.
constexpr uint32_t kSetCursorPosFn = 0x826263C0;
// sub_82625F50(screen, vec4*, 0, 1, 1, 1, useGivenHeight) -- moves the map
// camera to the given target and rebuilds the basis at screen+1056. With the
// last argument zero the height is taken from the camera itself (a pure XZ
// pan, which is how sub_82629BA0 calls it); non-zero is what lets a new height
// through, clamped to screen+1124/+1128. That is the zoom.
constexpr uint32_t kMoveMapCameraFn = 0x82625F50;
// sub_82620158(screen, radius) -- is the cursor still within `radius` of the
// object at screen+600.
constexpr uint32_t kFocusInRangeFn = 0x82620158;
// sub_8261F970(screen, on) -- raises or drops the focus at screen+629 and
// writes `iconisselected` into the movie, which is the hover highlight itself.
// Dropping it also clears screen+600.
constexpr uint32_t kSetFocusFn = 0x8261F970;
constexpr uint32_t kGuestMallocFn = 0x82130528;

// ── Map screen offsets (see map_mouse.h and the RE notes) ──────────────

constexpr uint32_t kNeedsRefresh = 396;    // u8, consumed by sub_8262A9F8
constexpr uint32_t kCursorPos = 448;   // float x3, the cursor's world position
constexpr uint32_t kCursorActive = 496;    // u8
// screen+572/+580 (X) and +576/+584 (Z) look like cursor bounds and are not
// the map's. sub_82623BF8 rewrites all four every frame out of the guest
// viewport's width and height, the camera distance at +1396 and a handful of
// tunables, so they are a box in world space that stands for a region of the
// screen -- what sub_82629910 uses to decide whether a cursor pinned at the
// edge of the view should be dragged along as the camera pans. Clamping a
// pointer to them fences the cursor into a box well inside the visible map,
// and being derived from the guest's viewport rather than the host window they
// do not even agree with where the pointer is. The real limit is the boundary
// polygon that sub_826263C0 sweeps against, which is the map's own edge.
constexpr uint32_t kFocusRadius = 568;     // float, the magnet's hold distance
constexpr uint32_t kFocused = 629;         // u8, an icon is selected
constexpr uint32_t kCursorMoved = 630;     // u8, drives the pick on the next tick
constexpr uint32_t kCameraBasis = 1056;    // 3 rows of 4 floats, 16 apart
constexpr uint32_t kCameraPos = 1104;      // float x3
constexpr uint32_t kCameraHeightMin = 1124;
constexpr uint32_t kCameraHeightMax = 1128;
constexpr uint32_t kCameraDistance = 1132;  // float, camera -> map plane
constexpr uint32_t kCursorSpeed = 1180;    // float, the stick's world units/sec
constexpr uint32_t kInputEnabled = 1427;   // u8
// The two sticks, already normalised to [-1, 1] and curved by sub_8262A688.
// It writes them after this hook runs, so what is read here is last frame's --
// which is exactly the question being asked: was a stick being held.
constexpr uint32_t kViewAxisX = 1456;
constexpr uint32_t kViewAxisY = 1460;
constexpr uint32_t kCursorAxisX = 1464;
constexpr uint32_t kCursorAxisY = 1468;
// sub_8260C6D8(&x, &y, 0.2) is the game's own deadzone on both pairs.
constexpr float kStickDeadzoneSq = 0.2f * 0.2f;

// vfunc92 -- `(*(u32*)(this + 16) & 0x100) != 0`, the screen's own active bit.
// Every one of the game's cursor movers (sub_82629910, sub_82629BA0,
// sub_826290C0) tests it before touching anything, and the objects the sweep
// walks are only valid while it is set.
constexpr uint32_t kIsActiveVFunc = 92;

// ── Guest memory ───────────────────────────────────────────────────────

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadGuestBE32(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base) return 0;
    const uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

void WriteGuestBE32(uint32_t ea, uint32_t v) {
    uint8_t* base = GetMembase();
    if (!base) return;
    uint8_t* p = base + ea;
    p[0] = uint8_t((v >> 24) & 0xFF);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8)  & 0xFF);
    p[3] = uint8_t(v         & 0xFF);
}

uint8_t ReadGuestU8(uint32_t ea) {
    uint8_t* base = GetMembase();
    return base ? base[ea] : 0;
}

void WriteGuestU8(uint32_t ea, uint8_t v) {
    uint8_t* base = GetMembase();
    if (base) base[ea] = v;
}

float ReadGuestFloat(uint32_t ea) {
    uint32_t bits = ReadGuestBE32(ea);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteGuestFloat(uint32_t ea, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    WriteGuestBE32(ea, bits);
}

// ── Guest calls ────────────────────────────────────────────────────────

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

void CallSetCursorPos(uint32_t screen, float x, float y, float z) {
    PPCFunc* fn = GuestFn(kSetCursorPosFn);
    if (!fn) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            MC_WARN("[map-mouse] sub_826263C0 is not in the dispatcher -- the "
                    "cursor cannot be driven");
        return;
    }
    rex::ppc::GuestToHostFunction<uint32_t>(fn, screen, double(x), double(y),
                                           double(z));
}

void CallMoveMapCamera(uint32_t screen, uint32_t vec4, bool use_given_height) {
    PPCFunc* fn = GuestFn(kMoveMapCameraFn);
    if (!fn) return;
    rex::ppc::GuestToHostFunction<uint32_t>(fn, screen, vec4, 0u, 1u, 1u, 1u,
                                           use_given_height ? 1u : 0u);
}

// The map screen's own "am I live" predicate, through its vtable rather than
// the bit it happens to read today, because subclasses override it.
bool ScreenIsActive(uint32_t screen) {
    uint32_t vtable = ReadGuestBE32(screen);
    if (!vtable) return false;
    uint32_t fn_addr = ReadGuestBE32(vtable + kIsActiveVFunc);
    if (!fn_addr) return false;
    PPCFunc* fn = GuestFn(fn_addr);
    if (!fn) return false;
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, screen) != 0;
}

// sub_82625F50 reads its target with lvx, which ignores the low four bits of
// the address, so an unaligned buffer would be read from wherever it rounds
// down to. Over-allocate and align by hand rather than trust the allocator.
uint32_t ScratchVec4() {
    static std::atomic<uint32_t> cached{0};
    uint32_t have = cached.load(std::memory_order_relaxed);
    if (have) return have;

    PPCFunc* malloc_fn = GuestFn(kGuestMallocFn);
    if (!malloc_fn) return 0;
    uint32_t raw = rex::ppc::GuestToHostFunction<uint32_t>(malloc_fn, 32u);
    if (!raw) return 0;

    uint32_t aligned = (raw + 15u) & ~15u;
    uint32_t expected = 0;
    if (!cached.compare_exchange_strong(expected, aligned))
        return expected;  // lost the race; the other buffer is just as good
    return aligned;
}

// ── Mouse state ────────────────────────────────────────────────────────

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Vec3 Normalize(const Vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return {0.0f, 0.0f, 0.0f};
    return v * (1.0f / len);
}

Vec3 ReadGuestVec3(uint32_t ea) {
    return {ReadGuestFloat(ea), ReadGuestFloat(ea + 4), ReadGuestFloat(ea + 8)};
}

// The listener runs on the UI thread and the hook on a guest thread, so
// everything crossing between them is an atomic. Position is kept as one
// packed 64-bit value: a torn read would put the cursor somewhere the pointer
// never was.
std::atomic<uint64_t> g_mouse_xy{0};
std::atomic<uint64_t> g_mouse_seq{0};       // bumped on every motion event
// Raw scroll units, not detents: a high-resolution wheel reports fractions of
// kScrollPerDetent and rounding each event on its own would throw all of them
// away. The hook takes whole detents out and leaves the remainder here.
std::atomic<int32_t> g_wheel_accum{0};
std::atomic<bool> g_listener_attached{false};

// Set by the hook, read by the listener and the tick. Non-zero means the map
// screen ran its input tick recently.
std::atomic<uint64_t> g_map_tick_stamp{0};
std::atomic<uint64_t> g_frame_counter{0};
std::atomic<bool> g_cursor_hidden{false};

// Which device owns the cursor. Last input wins, the way every game arbitrates
// a pointer against a pad, and it is what the magnet guard keys off.
//
// The first attempt at this was a timer -- suppress the snap for so many
// frames after the last mouse movement -- and it was wrong in the way that
// matters: park the pointer for a moment and the magnet came back and yanked
// the cursor onto whatever icon was selected. Ownership does not expire. It
// changes when the other device is used.
std::atomic<bool> g_mouse_owns{false};

// Whether the map is on screen AND ours to drive. Separate from g_mouse_owns
// on purpose: ownership is about which device moved last, this is about whether
// the pointer belongs to the map at all. Anything else that wants to take the
// mouse over -- free look, above all -- has to stand down on this rather than
// on ownership, because a feature that keeps re-centring the pointer stops the
// map from ever seeing the motion that would claim ownership in the first
// place.
std::atomic<bool> g_map_wants_pointer{false};

// Where the wheel wants the camera. Held separately from the camera's own
// height because a detent is a discrete event and the camera has to travel to
// it over several frames -- stepping straight to the new height is what made
// the zoom read as a series of jumps. Guest-thread only.
float g_zoom_target = 0.0f;
bool g_zoom_running = false;

class MapMouseListener final : public rex::ui::WindowInputListener {
 public:
    void OnMouseMove(rex::ui::MouseEvent& e) override { Record(e); }
    void OnMouseDown(rex::ui::MouseEvent& e) override { Record(e); }
    void OnMouseUp(rex::ui::MouseEvent& e) override { Record(e); }

    void OnMouseWheel(rex::ui::MouseEvent& e) override {
        Record(e);
        if (e.scroll_y())
            g_wheel_accum.fetch_add(e.scroll_y(), std::memory_order_relaxed);
    }

 private:
    static void Record(rex::ui::MouseEvent& e) {
        uint64_t packed = (uint64_t(uint32_t(e.x())) << 32) | uint32_t(e.y());
        g_mouse_xy.store(packed, std::memory_order_relaxed);
        g_mouse_seq.fetch_add(1, std::memory_order_relaxed);
    }
};

MapMouseListener g_listener;

rex::ui::Window* GetWindow() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return nullptr;
    auto* isys = static_cast<rex::input::InputSystem*>(rt->input_system());
    return isys ? isys->window() : nullptr;
}

void AttachListener() {
    if (g_listener_attached.load(std::memory_order_relaxed)) return;
    rex::ui::Window* window = GetWindow();
    if (!window) return;
    // Layer 0, the same one the app and the mnk driver use. Listeners in a
    // layer run in reverse order of registration and this one is added last,
    // so it sees the event first -- which costs nothing, because it never
    // marks anything handled. Anything registered above layer 0 (the ImGui
    // overlays) still gets first refusal, so a console that is capturing the
    // mouse stops the map cursor from chasing it.
    window->AddInputListener(&g_listener, 0);
    g_listener_attached.store(true, std::memory_order_relaxed);
    MC_INFO("[map-mouse] mouse listener attached to the window");
}

// Hands the pointer back: the OS cursor becomes visible again and any scroll
// collected meanwhile is dropped, so a wheel turned over a pop-up does not
// zoom the map the moment it closes.
void ReleaseMouse();

// Cursor visibility has to be applied on the UI thread -- SDL video calls are
// not safe from the guest threads the hook runs on.
void SetOsCursorHidden(bool hidden) {
    if (g_cursor_hidden.load(std::memory_order_relaxed) == hidden) return;
    rex::ui::Window* window = GetWindow();
    if (!window) return;
    g_cursor_hidden.store(hidden, std::memory_order_relaxed);
    window->app_context().CallInUIThreadDeferred([window, hidden]() {
        window->SetCursorVisibility(hidden
            ? rex::ui::Window::CursorVisibility::kHidden
            : rex::ui::Window::CursorVisibility::kVisible);
    });
}

// ── The unprojection ───────────────────────────────────────────────────

void ReleaseMouse() {
    SetOsCursorHidden(false);
    g_wheel_accum.store(0, std::memory_order_relaxed);
    // A zoom caught mid-flight belongs to the view the player was looking at.
    // Resuming it after a pop-up closes would move the camera on its own, so
    // it is abandoned and the next detent reseeds from the live camera.
    g_zoom_running = false;
}

// Casts the pointer through the map camera and returns where the ray meets the
// plane the cursor already lives on. False means the ray runs parallel to that
// plane or points away from it, which is what a camera looking at the horizon
// would do -- there is no sensible answer then, and the cursor is left alone.
bool UnprojectToMapPlane(uint32_t screen, int32_t mx, int32_t my,
                         uint32_t width, uint32_t height, Vec3& out) {
    if (!width || !height) return false;

    float ndc_x = (2.0f * float(mx) / float(width)) - 1.0f;
    float ndc_y = 1.0f - (2.0f * float(my) / float(height));
    if (REXCVAR_GET(map_mouse_flip_x)) ndc_x = -ndc_x;
    if (REXCVAR_GET(map_mouse_flip_y)) ndc_y = -ndc_y;

    Vec3 right = Normalize(ReadGuestVec3(screen + kCameraBasis));
    Vec3 up    = Normalize(ReadGuestVec3(screen + kCameraBasis + 16));
    Vec3 fwd   = Normalize(ReadGuestVec3(screen + kCameraBasis + 32));
    Vec3 eye   = ReadGuestVec3(screen + kCameraPos);

    // sub_82202EC0 builds the basis from the vector running from the ground
    // point up to the camera, so whether the third row comes out pointing at
    // the map or away from it depends on a convention this code does not get
    // to see. It does not have to: a camera framing the map always looks
    // downwards, so the row whose Y is positive is the flipped one.
    if (fwd.y > 0.0f) fwd = fwd * -1.0f;
    if (up.y < 0.0f) up = up * -1.0f;

    float tan_half = std::tan(float(REXCVAR_GET(map_mouse_fov)) *
                              3.14159265358979f / 360.0f);
    float aspect = float(width) / float(height);

    Vec3 dir = Normalize(fwd + right * (ndc_x * tan_half * aspect) +
                         up * (ndc_y * tan_half));
    if (std::fabs(dir.y) < 1e-5f) return false;

    float plane_y = ReadGuestFloat(screen + kCursorPos + 4);
    float t = (plane_y - eye.y) / dir.y;
    if (t <= 0.0f) return false;

    out = eye + dir * t;

    // A ray that grazes the plane produces an arbitrarily distant hit, which
    // is not a place the player can be pointing at -- the map camera looks
    // down at somewhere between 45 and 90 degrees, so anything that far out
    // means the basis or the field of view is wrong rather than that the
    // pointer is near a horizon. Measured against the camera's own distance to
    // the map so it scales with the zoom, and loose enough that nothing on
    // screen ever trips it. This is a sanity bound, NOT a limit on where the
    // cursor may go: the map's edge is the boundary polygon the sweep inside
    // sub_826263C0 walks, and nothing here should second-guess it.
    float cam_dist = std::fabs(ReadGuestFloat(screen + kCameraDistance));
    if (cam_dist > 1.0f) {
        float dx = out.x - eye.x;
        float dz = out.z - eye.z;
        float reach = cam_dist * 50.0f;
        if ((dx * dx + dz * dz) > (reach * reach)) return false;
    }

    out.y = plane_y;
    return true;
}

// Writes a camera target into the scratch vector and hands it to the game.
void MoveCamera(uint32_t screen, uint32_t vec, float x, float y, float z) {
    WriteGuestFloat(vec + 0, x);
    WriteGuestFloat(vec + 4, y);
    WriteGuestFloat(vec + 8, z);
    WriteGuestFloat(vec + 12, 1.0f);
    CallMoveMapCamera(screen, vec, /*use_given_height=*/true);
    // sub_82629BA0 raises this at the end of every move it makes and
    // sub_8262A9F8 turns it back into a vfunc184 call on the next tick.
    WriteGuestU8(screen + kNeedsRefresh, 1);
}

// `anchor` is the unclamped world point the pointer is over, and mx/my/w/h are
// the pointer itself, needed again after the camera has moved. Returns whether
// the camera was moved this frame.
bool ApplyWheelZoom(uint32_t screen, const Vec3& anchor, int32_t mx, int32_t my,
                    uint32_t width, uint32_t height) {
    if (!REXCVAR_GET(map_mouse_zoom)) {
        g_wheel_accum.store(0, std::memory_order_relaxed);
        g_zoom_running = false;
        return false;
    }

    float cam_y = ReadGuestFloat(screen + kCameraPos + 4);
    float lo = ReadGuestFloat(screen + kCameraHeightMin);
    float hi = ReadGuestFloat(screen + kCameraHeightMax);

    // Take whole detents and put the remainder back, so a wheel that reports
    // in fractions still adds up to a step instead of being rounded away.
    constexpr int32_t kPerDetent = int32_t(rex::ui::MouseEvent::kScrollPerDetent);
    int32_t units = g_wheel_accum.load(std::memory_order_relaxed);
    int32_t detents = units / kPerDetent;
    if (detents) {
        g_wheel_accum.fetch_sub(detents * kPerDetent, std::memory_order_relaxed);
        // While no zoom is in flight the camera is the truth: the stick, a
        // screen transition or the map recentring itself all move it behind
        // our back, and carrying a stale target across that would snap the
        // view back to wherever the wheel last left it.
        if (!g_zoom_running) g_zoom_target = cam_y;
        // Scrolling up is zooming in, which is the camera coming down.
        g_zoom_target *= std::pow(float(REXCVAR_GET(map_mouse_zoom_step)),
                                  float(-detents));
        if (hi > lo) {
            if (g_zoom_target < lo) g_zoom_target = lo;
            if (g_zoom_target > hi) g_zoom_target = hi;
        }
        g_zoom_running = true;
    }

    if (!g_zoom_running) return false;

    float remaining = g_zoom_target - cam_y;
    // Within a thousandth of the range there is nothing left to see, and the
    // camera has usually been pinned by its own clamp anyway.
    float epsilon = (hi > lo) ? (hi - lo) * 0.001f : 0.01f;
    if (std::fabs(remaining) <= epsilon) {
        g_zoom_running = false;
        return false;
    }

    float new_height = cam_y + remaining * float(REXCVAR_GET(map_mouse_zoom_smooth));

    uint32_t vec = ScratchVec4();
    if (!vec) {
        g_zoom_running = false;
        return false;
    }

    Vec3 cam = ReadGuestVec3(screen + kCameraPos);
    MoveCamera(screen, vec, cam.x, new_height, cam.z);

    // Height alone keeps the middle of the view fixed, so the world slides
    // under a pointer that has not moved and the point being pointed at
    // marches off towards the centre a detent at a time.
    //
    // The first attempt scaled the camera's offset from the anchor by the
    // ratio of the heights, which is what a pinhole looking straight down at a
    // plane does. This camera does not look straight down: it pulls back along
    // Z by up to 1300 units as it descends (see sub_82623350), so the ratio is
    // wrong by whatever that pullback changed, which is why the view kept
    // creeping up the map.
    //
    // Rather than model the pullback, measure it. Ask where the pointer lands
    // now that the camera has actually moved, and shift the camera by whatever
    // is left over. That correction is exact in one step and needs no model at
    // all: translating the camera horizontally leaves the ray direction and
    // the plane alone, so the point under a given pixel moves by exactly the
    // same vector the camera did.
    if (REXCVAR_GET(map_mouse_zoom_to_cursor)) {
        Vec3 after;
        if (UnprojectToMapPlane(screen, mx, my, width, height, after)) {
            float ex = anchor.x - after.x;
            float ez = anchor.z - after.z;
            if (std::fabs(ex) > 0.01f || std::fabs(ez) > 0.01f) {
                Vec3 moved = ReadGuestVec3(screen + kCameraPos);
                MoveCamera(screen, vec, moved.x + ex, moved.y, moved.z + ez);
            }
        }
    }

    // The camera can refuse to travel -- its own clamp, or vfunc512 taking the
    // height out of our hands. Nothing moved means nothing will, so stop
    // asking rather than call it again every frame forever.
    if (ReadGuestFloat(screen + kCameraPos + 4) == cam_y) g_zoom_running = false;
    return true;
}

// Mirrors the tail of sub_82629910: once the cursor has been pulled further
// than half the magnet's radius from the selected icon, the selection is
// dropped. That is the only thing in the game that ever releases it -- the
// pick in sub_826281E8 raises a selection but never lowers one -- and it lives
// on the stick path, which a mouse never walks. Without it the highlight stays
// lit on whatever icon was touched first, forever.
void DropFocusIfCursorLeft(uint32_t screen) {
    if (!ReadGuestU8(screen + kFocused)) return;

    // The stick's copy of this also requires screen+1121, which says the camera
    // has been displaced from its anchor. That gate belongs to the stick's
    // model, where the cursor only ever moves because the camera did; a mouse
    // moves the cursor on its own and can leave an icon with the camera
    // perfectly still. Requiring it there meant a selection that could never
    // be released, so the highlight stayed lit on the first icon touched.
    PPCFunc* in_range = GuestFn(kFocusInRangeFn);
    PPCFunc* set_focus = GuestFn(kSetFocusFn);
    if (!in_range || !set_focus) return;

    float radius = ReadGuestFloat(screen + kFocusRadius) * 0.5f;
    if (rex::ppc::GuestToHostFunction<uint32_t>(in_range, screen, double(radius)))
        return;

    rex::ppc::GuestToHostFunction<uint32_t>(set_focus, screen, 0u);
}

// Walks the cursor to `target` in steps no longer than the stick's own, and
// returns whether anything moved.
//
// The size is taken from the game rather than picked: sub_82629910 moves the
// cursor by screen[1180] * 25 * axis * dt with the axis at most 1, so that
// product is the largest step the boundary sweep is ever asked to resolve.
// Matching it is what keeps sub_82624EC8 walking node to node the way it
// expects instead of being handed a chord across the city.
bool StepCursorTowards(uint32_t screen, const Vec3& target, float dt) {
    float speed = ReadGuestFloat(screen + kCursorSpeed);
    float step = std::fabs(speed * 25.0f * dt);
    if (!(step > 0.0f)) {
        // No usable speed to copy (the field is zero before the screen has
        // settled). A hundredth of the camera height is the same order as the
        // stick's step at a normal zoom and is never a teleport.
        step = std::fabs(ReadGuestFloat(screen + kCameraPos + 4)) * 0.01f;
        if (!(step > 0.0f)) return false;
    }

    int32_t budget = REXCVAR_GET(map_mouse_max_steps);
    bool moved = false;

    for (int32_t i = 0; i < budget; ++i) {
        Vec3 cur = ReadGuestVec3(screen + kCursorPos);
        float dx = target.x - cur.x;
        float dz = target.z - cur.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (dist <= 1e-4f) break;

        Vec3 next = target;
        if (dist > step) {
            float s = step / dist;
            next.x = cur.x + dx * s;
            next.z = cur.z + dz * s;
        }
        next.y = target.y;

        CallSetCursorPos(screen, next.x, next.y, next.z);
        moved = true;

        // The sweep clamps against the map boundary, so a cursor pushed into a
        // wall stops making progress. Continuing to spend the budget on it
        // would burn guest calls to move nothing.
        Vec3 after = ReadGuestVec3(screen + kCursorPos);
        float moved_by = std::sqrt((after.x - cur.x) * (after.x - cur.x) +
                                   (after.z - cur.z) * (after.z - cur.z));
        if (moved_by <= step * 0.01f) break;
    }

    return moved;
}

}  // namespace

// ── Entry points ───────────────────────────────────────────────────────

void InitMapMouse() {
    AttachListener();
}

void TickMapMouse() {
    uint64_t frame = g_frame_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!g_listener_attached.load(std::memory_order_relaxed)) AttachListener();

    // The map screen stops ticking the moment it closes, and nothing tells us
    // that it did. Two frames of silence is the signal.
    uint64_t last = g_map_tick_stamp.load(std::memory_order_relaxed);
    if (last != 0 && frame <= last + 2) return;

    // The map is not on screen. Scroll collected out here belongs to whatever
    // else the wheel does; carrying it in would zoom the map the instant it
    // opens, by however much the player had scrolled since.
    // Ownership goes back to nobody as well, so the next map opens with the
    // game's own magnet intact until a mouse claims it.
    ReleaseMouse();
    g_mouse_owns.store(false, std::memory_order_relaxed);
    g_map_wants_pointer.store(false, std::memory_order_relaxed);
}

bool MapMouseWantsPointer() {
    return g_map_wants_pointer.load(std::memory_order_relaxed);
}

void Hook_MapInputTick(PPCRegister& r31, PPCRegister& f1) {
    const uint32_t screen = r31.u32;
    if (!screen) return;

    g_map_tick_stamp.store(g_frame_counter.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);

    // Whether the map is ours to drive at all.
    //
    // screen+1427 is the map's own "should I react to input", set through the
    // SCXML action handler sub_82622680 (case 4), and screen+496 is the
    // cursor's enable, set through case 8 -> sub_8261FB00. Anything that puts
    // itself in front of the map lowers them -- the Active Missions pop-up,
    // the front-end screens in sub_8266B790 and its neighbours, a transition
    // in flight. screen+16 & 0x100 is the screen's own active bit, which every
    // one of the game's movers tests first and which the boundary sweep's data
    // hangs off.
    bool gated_in = REXCVAR_GET(map_mouse) &&
                    ReadGuestU8(screen + kInputEnabled) != 0 &&
                    ReadGuestU8(screen + kCursorActive) != 0 &&
                    ScreenIsActive(screen);

    g_map_wants_pointer.store(gated_in, std::memory_order_relaxed);

    if (REXCVAR_GET(map_mouse_diag)) {
        static uint8_t last_in = 0xFF, last_cur = 0xFF;
        uint8_t in = ReadGuestU8(screen + kInputEnabled);
        uint8_t cur = ReadGuestU8(screen + kCursorActive);
        if (in != last_in || cur != last_cur) {
            last_in = in;
            last_cur = cur;
            MC_INFO("[map-mouse] gate changed: input_enabled(+1427)={} "
                    "cursor_active(+496)={} active(vfunc92)={}",
                    in, cur, ScreenIsActive(screen));
        }
    }

    rex::ui::Window* window = GetWindow();
    if (!window) {
        ReleaseMouse();
        return;
    }
    uint32_t width = window->GetActualPhysicalWidth();
    uint32_t height = window->GetActualPhysicalHeight();

    uint64_t packed = g_mouse_xy.load(std::memory_order_relaxed);
    int32_t mx = int32_t(uint32_t(packed >> 32));
    int32_t my = int32_t(uint32_t(packed & 0xFFFFFFFFu));

    // Last input wins. sub_8262A688 writes both stick pairs after this hook
    // runs, so these are last frame's values -- which is the right question:
    // was a stick being held. A stick outside its deadzone takes the cursor
    // back and hands the magnet with it; the mouse takes it back by moving.
    float cax = ReadGuestFloat(screen + kCursorAxisX);
    float cay = ReadGuestFloat(screen + kCursorAxisY);
    float vax = ReadGuestFloat(screen + kViewAxisX);
    float vay = ReadGuestFloat(screen + kViewAxisY);
    bool pad_active = (cax * cax + cay * cay) > kStickDeadzoneSq ||
                      (vax * vax + vay * vay) > kStickDeadzoneSq;
    if (pad_active) g_mouse_owns.store(false, std::memory_order_relaxed);

    // Consumed on every tick, driving or not: leaving a stale sequence behind
    // would make the first frame back count as "the pointer moved" using a
    // position from before whatever took the map away.
    static uint64_t last_seq = 0;
    uint64_t seq = g_mouse_seq.load(std::memory_order_relaxed);
    bool mouse_moved = (seq != last_seq);
    last_seq = seq;

    if (gated_in && (mouse_moved || g_wheel_accum.load(std::memory_order_relaxed)))
        g_mouse_owns.store(true, std::memory_order_relaxed);

    // One decision, one place. Every path out of here used to return without
    // putting the pointer back, so opening anything over the map -- Active
    // Missions above all -- left the player with no visible cursor to use it
    // with, which reads exactly as the game refusing to give the mouse back.
    bool driving = gated_in && !pad_active &&
                   g_mouse_owns.load(std::memory_order_relaxed);
    if (!driving) {
        ReleaseMouse();
        return;
    }

    SetOsCursorHidden(REXCVAR_GET(map_mouse_hide_cursor));

    // Unprojected every frame, not only on the frames the pointer moves: the
    // zoom anchors on this point, and a wheel turned while the mouse sits
    // still still needs to know where it is pointing. Unclamped, because the
    // zoom's correction step subtracts two of these and a clamp would poison
    // the difference.
    Vec3 aim;
    if (!UnprojectToMapPlane(screen, mx, my, width, height, aim)) return;

    bool zoomed = ApplyWheelZoom(screen, aim, mx, my, width, height);

    // A pointer that has not moved must not keep re-placing the cursor. A zoom
    // in flight is the one thing that moves the world out from under a parked
    // pointer, so it counts as motion.
    if (!mouse_moved && !zoomed) return;

    // The zoom holds `aim` under the same pixel, so it is still the right
    // target after the camera moved and does not need recomputing. It goes to
    // the cursor unclamped -- see the note on screen+572 above for why the
    // fields that look like cursor bounds are not the map's, and why stopping
    // the cursor at them was what fenced it into a box.
    const Vec3& world = aim;

    float dt = float(f1.f64);
    if (!(dt > 0.0f) || dt > 1.0f) dt = 1.0f / 60.0f;
    if (!StepCursorTowards(screen, world, dt)) return;

    // sub_8262A688 ends with `if (!stickMoved && screen[630]) sub_826281E8()`,
    // and sub_826281E8 is the pick: it asks screen+588 what sits at the cursor
    // (vfunc556) and lights the indicator up. It only ever runs on the frame
    // after something moved the cursor, and screen+630 is the only thing that
    // says something did. Setting it by hand is what makes hovering an icon
    // with the mouse register at all -- our move never went near the stick
    // path that normally raises it. It is cleared inside sub_826281E8, so this
    // fires once per frame the mouse is moving and then stops.
    //
    // Raising it every frame rather than only when the pointer stops is what
    // the stick cannot do (its own `v4` suppresses the pick while it is being
    // held) and is strictly better with a mouse: the highlight tracks the
    // pointer as it sweeps across icons instead of waiting for it to settle.
    // It is only bearable because MCLA_MapCursorMagnetGuard has taken the two
    // snaps out; with them in, a selection every frame is a cursor welded to
    // the first icon it touched.
    WriteGuestU8(screen + kCursorMoved, 1);

    // The pick raises a selection and nothing in the mouse's path ever lowers
    // one, so this is where a selection the cursor has left gets released.
    DropFocusIfCursorLeft(screen);

    if (REXCVAR_GET(map_mouse_diag)) {
        static uint64_t next_log = 0;
        uint64_t frame = g_frame_counter.load(std::memory_order_relaxed);
        if (frame >= next_log) {
            next_log = frame + 60;
            Vec3 cam = ReadGuestVec3(screen + kCameraPos);
            Vec3 cur = ReadGuestVec3(screen + kCursorPos);
            MC_INFO("[map-mouse] screen=0x{:08X} mouse=({}, {}) win={}x{} "
                    "cam=({:.1f}, {:.1f}, {:.1f}) want=({:.1f}, {:.1f}, {:.1f}) "
                    "cursor=({:.1f}, {:.1f}, {:.1f})",
                    screen, mx, my, width, height,
                    cam.x, cam.y, cam.z,
                    world.x, world.y, world.z,
                    cur.x, cur.y, cur.z);
        }
    }
}

bool MCLA_MapCursorMagnetGuard() {
    if (!REXCVAR_GET(map_mouse) || REXCVAR_GET(map_mouse_magnet)) return false;
    return g_mouse_owns.load(std::memory_order_relaxed);
}

bool MCLA_MapCursorSweepNullGuard(PPCRegister& r31, PPCRegister& r30,
                                  PPCRegister& r26) {
    //   0x82625418  bl   sub_82624EC8
    //   0x8262541C  lwz  r25, var_A0(r1)     ; the boundary node, always set
    //   0x82625420  lwz  r31, var_9C(r1)     ; its neighbour, sometimes not
    //   0x82625424  <hook>
    //   ...
    //   0x82625448  lvx128 v53, r0, r31      ; faults on the zero
    //
    // r30 still holds the requested position and r26 the caller's output, the
    // same pair the function's own "nothing to clamp" exit copies between. So
    // the bail is that copy plus a jump to the epilogue at 0x826253F4, which
    // pops the frame and restores v127 without touching a vector register the
    // call could have clobbered.
    if (r31.u32 != 0) return false;

    // Both of sub_82625368's callers pass the same buffer as the requested
    // position and as the output (sub_826263C0 hands it &v46 twice), so the
    // copy is usually already done and self-to-self memcpy is undefined.
    if (r26.u32 != r30.u32) {
        uint8_t* base = GetMembase();
        if (!base) return false;
        std::memmove(base + r26.u32, base + r30.u32, 16);
    }

    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        MC_WARN("[map-mouse] the map boundary sweep resolved no neighbour "
                "(sub_82624EC8 left its out-pointer null) -- taking the "
                "requested position unclamped for this step instead of "
                "reading guest 0. A cursor step got past the subdivision.");
    return true;
}

#else  // REXGLUE_HAS_XEO3_TARGET

void InitMapMouse() {}
void TickMapMouse() {}
bool MapMouseWantsPointer() { return false; }
void Hook_MapInputTick(PPCRegister&, PPCRegister&) {}
bool MCLA_MapCursorMagnetGuard() { return false; }
bool MCLA_MapCursorSweepNullGuard(PPCRegister&, PPCRegister&, PPCRegister&) {
    return false;
}

#endif  // REXGLUE_HAS_XEO3_TARGET
