// Gameplay camera: FOV scale, the chase camera smoothing that holds above
// 30 FPS, and the free-fly debug camera.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <cmath>
#include <cstdint>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
#include "imgui.h"

#include "hooks.h"
#include "hooks_internal.h"
#include "../menu_camera.h"

REXCVAR_DEFINE_DOUBLE(fov_1p_scale, 1.0, "MCLA/Camera", "FOV scale — 1st person / cockpit (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(fov_3p_scale, 1.0, "MCLA/Camera", "FOV scale — 3rd person / chase cam (0.5 = narrower, 2.0 = wider)")
    .range(0.5, 5.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Continuous exponential camera boom smoothing at 60 FPS
REXCVAR_DEFINE_BOOL(smooth_chase_cam, true, "MCLA/Camera",
    "Fix: Smooth chase camera boom interpolation at 60 FPS using continuous-time exponential decay.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(chase_cam_smoothing_factor, 1.0, "MCLA/Camera",
    "Chase camera boom smoothing factor multiplier (0.1 - 3.0).")
    .range(0.1, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Smooth camera transition interpolation & wrap fix
REXCVAR_DEFINE_BOOL(smooth_camera_transitions, true, "MCLA/Camera",
    "Fix: Eliminate camera transition jitter and 180-degree wrap flipping when transitioning between orbit, menu, and gameplay cameras.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);


// BadassBaboon's Recomp Adjustments: Continuous-time camera shake scaling
REXCVAR_DEFINE_BOOL(scale_camera_shake, true, "MCLA/Camera",
    "Fix: Scale camera shake intensity across framerates (60+ FPS) to prevent violent noise jitter during drifts, burnouts, and crashes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(camera_shake_scale, 0.6, "MCLA/Camera",
    "Camera shake intensity multiplier (0.0 = disabled, 0.6 = balanced PC rumble, 1.0 = full console shake).")
    .range(0.0, 3.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(debug_cam, "off", "MCLA/Camera",
    "Free-fly camera during live gameplay: left stick moves, right stick looks, triggers change "
    "speed. Gameplay keeps running underneath (drive, traffic, physics).")
    .allowed({"off", "free"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_speed, 40.0, "MCLA/Camera",
    "Free-fly camera move speed (world units/second).")
    .range(1.0, 500.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_sens, 2.5, "MCLA/Camera",
    "Free-fly camera look sensitivity (radians/second at full stick).")
    .range(0.2, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(debug_cam_mouse, true, "MCLA/Camera",
    "Free-fly camera: use the mouse to look (captures the cursor while active).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(debug_cam_mouse_sens, 0.0025, "MCLA/Camera",
    "Free-fly mouse look sensitivity (radians per mouse pixel).")
    .range(0.0002, 0.02)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Free-fly camera host state (see Patch_DebugCam). Angles are seeded from the
// camera on activation, then integrated from the right stick each frame. The
// world position lives in guest memory at +0x50 and is read/written in place.
static bool g_freecam_seeded = false;
static float g_freecam_yaw = 0.0f;
static float g_freecam_pitch = 0.0f;

// Mouse-look for the free-fly camera. Self-contained OS mouse capture (Win32):
// while active we hide + confine the cursor, recenter it every frame, and feed
// the raw pixel delta into yaw/pitch as a displacement (no dt scaling — mouse
// deltas are already per-frame). Independent of mnk_mode, so it does not hijack
// the pad. Sign matches the right-stick path: mouse right = look right (yaw
// down), mouse up = look up (pitch down). Caller clamps pitch afterward.
#if defined(_WIN32)
static bool g_freecam_mouse_captured = false;
static void FreecamMouseUpdate(bool active, float& yaw, float& pitch) {
    auto* win = rex::Runtime::instance()->display_window();
    if (!win) return;
    HWND hwnd = static_cast<HWND>(win->GetNativeWindowHandle());
    if (!hwnd) return;

    // Release the cursor whenever an ImGui overlay (F4 RexGlue Settings, console,
    // etc.) wants the mouse, so the menu is usable; recapture once it is closed.
    // Deterministic from the live overlay state — no key-toggle to drift out of
    // sync with the menu. debug_cam_mouse is the master on/off.
    bool menu_open = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
    bool want = active && REXCVAR_GET(debug_cam_mouse) && !menu_open;

    // Client-area center in screen coordinates (recenter target).
    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT center = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(hwnd, &center);

    if (want && !g_freecam_mouse_captured) {
        g_freecam_mouse_captured = true;
        win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        win->CaptureMouse();
        SetCursorPos(center.x, center.y);  // seed center, skip first-frame spike
        return;
    }
    if (!want) {
        if (g_freecam_mouse_captured) {
            g_freecam_mouse_captured = false;
            win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
            win->ReleaseMouse();
        }
        return;
    }

    POINT cur;
    GetCursorPos(&cur);
    int dx = cur.x - center.x;
    int dy = cur.y - center.y;
    float s = static_cast<float>(REXCVAR_GET(debug_cam_mouse_sens));
    yaw -= dx * s;
    pitch += dy * s;
    SetCursorPos(center.x, center.y);
}
#else
static void FreecamMouseUpdate(bool, float&, float&) {}
#endif

// Normalize an XInput thumbstick axis (int16, -32768..32767, centered at 0) to
// [-1, 1] with a small radial deadzone. XInput convention: up/right = positive.
static float NormalizeStick(int16_t raw) {
    float v = float(raw) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v < 0.15f && v > -0.15f) return 0.0f;
    return v;
}

// Gate bypass so the free-fly camera runs during live gameplay. In sub_822C0320
// the dcam manager block only executes when there is no gameplay camera source
// (r29 == 0, menus) or the photo-mode-active byte is set; in normal gameplay it
// is skipped, so Patch_DebugCam never fires. Hook at the gate compare
// (0x822C0644); returning true jumps straight to the manager block
// (0x822C065C), the same target as the gate-passed path (skipped instructions
// are loads/compares only). Only bypasses when the free camera is requested, so
// photo mode is untouched when debug_cam = off.
bool Patch_DebugCamGate() {
    return REXCVAR_GET(debug_cam) == "free";
}

// Free-fly camera driver. Hook at 0x822C0668: r3 = dcam manager (from
// camsys+0x33C, already null-checked), right before the manager's per-frame
// update (sub_82502E18) which in turn updates the active camera.
//
// The manager's active camera index (mgr+0, big-endian u32) selects slot
// mgr+8+index*4; index 0 (mgr+8) is the free-fly camera — its update
// (sub_82536288 -> sub_82537450) integrates position from velocity and rebuilds
// its orientation matrix every frame from scalar angles, so driving those
// scalars + position is enough to fly it. Camera object layout (offsets from
// the object pointer):
//   +0x3C float  yaw   (added to +0x38, which we keep 0)
//   +0x40 float  pitch
//   +0x50 vec4   velocity (added into position each frame; we zero it and set
//                 position directly)
//   +0x100 vec4  position [x, y, z, w]
// The game rebuilds orientation as a yaw about +Y (sub_82202E38), i.e. Y is up.
// We keep yaw/pitch/position in host state and drive them from the sticks.
void Patch_DebugCam(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t mgr = static_cast<uint32_t>(r3.u64);

    if (REXCVAR_GET(debug_cam) != "free") {
        // Release: cut the blend back to the gameplay camera immediately.
        if (ReadGuestF32(base, mgr + 860) > 0.0f) {
            WriteGuestF32(base, mgr + 860, 0.0f);
        }
        g_freecam_seeded = false;
        FreecamMouseUpdate(false, g_freecam_yaw, g_freecam_pitch);  // release cursor
        return;
    }

    // Force the free-fly camera (index 0) active and fully blended in.
    base[mgr + 0] = 0;
    base[mgr + 1] = 0;
    base[mgr + 2] = 0;
    base[mgr + 3] = 0;
    WriteGuestF32(base, mgr + 860, 1.0f);
    // mgr+856 gates the per-frame camera update in sub_82502E18 (the call that
    // rebuilds the orientation matrix at +0xD0 from our angles). It is 1 in the
    // ctor but cleared outside photo mode, freezing the orientation. Force it on.
    base[mgr + 856] = 1;

    uint32_t cam = ReadGuestU32(base, mgr + 8);
    if (!cam) return;

    // Kill the camera's internal look input so the update (sub_82536288) does
    // not overwrite our yaw at +0x3C from the (unfed) photo-mode input source.
    WriteGuestU32(base, cam + 0x68, 0);

    // Seed our angle state from the camera's current orientation on activation
    // so the view does not jump. Position lives at +0x50 and is read fresh each
    // frame (below), so it needs no host-side seed.
    if (!g_freecam_seeded) {
        g_freecam_yaw = ReadGuestF32(base, cam + 0x3C);
        g_freecam_pitch = ReadGuestF32(base, cam + 0x40);
        g_freecam_seeded = true;
    }

    float dt = ReadGuestF32(base, 0x827D755C);  // clock+0x5C: raw frame dt
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    // Read the pad straight from the host input system (the guest ioPad globals
    // are ambiguous). XInput axes: up/right positive, int16 range.
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
    auto* isys = static_cast<rex::input::InputSystem*>(
        rex::Runtime::instance()->input_system());
    if (isys) {
        rex::input::X_INPUT_STATE state{};
        if (isys->GetState(0, &state) == 0) {
            lx = NormalizeStick(state.gamepad.thumb_lx);
            ly = -NormalizeStick(state.gamepad.thumb_ly);  // stick up = forward
            rx = -NormalizeStick(state.gamepad.thumb_rx);  // stick right = look right
            ry = -NormalizeStick(state.gamepad.thumb_ry);  // stick up = look up
        }
    }

    float sens = static_cast<float>(REXCVAR_GET(debug_cam_sens));
    float speed = static_cast<float>(REXCVAR_GET(debug_cam_speed));

    // Look: integrate the right stick into yaw/pitch and write them back; the
    // game rebuilds the orientation matrix (+0xD0) from these each frame.
    g_freecam_yaw += rx * sens * dt;
    g_freecam_pitch += ry * sens * dt;
    // Add mouse look on top of the stick (displacement, no dt).
    FreecamMouseUpdate(true, g_freecam_yaw, g_freecam_pitch);
    if (g_freecam_pitch > 1.5f) g_freecam_pitch = 1.5f;
    if (g_freecam_pitch < -1.5f) g_freecam_pitch = -1.5f;
    WriteGuestF32(base, cam + 0x38, 0.0f);
    WriteGuestF32(base, cam + 0x3C, g_freecam_yaw);
    WriteGuestF32(base, cam + 0x40, g_freecam_pitch);

    // Move: the world position is the vec3 at +0x50 (x, y=height, z). Read it
    // fresh (so the native button controls LB/RB/LT/RT still add in), advance it
    // along the view direction from the left stick, and write it back. Y is up
    // (orientation is a yaw about Y).
    float cy = std::cos(g_freecam_yaw), sy = std::sin(g_freecam_yaw);
    float cp = std::cos(g_freecam_pitch), sp = std::sin(g_freecam_pitch);
    float fwd_x = cp * sy, fwd_y = sp, fwd_z = cp * cy;  // forward (yaw+pitch)
    float right_x = cy, right_z = -sy;                   // horizontal strafe
    float step = speed * dt;

    float px = ReadGuestF32(base, cam + 0x50);
    float py = ReadGuestF32(base, cam + 0x54);
    float pz = ReadGuestF32(base, cam + 0x58);
    px += (fwd_x * ly + right_x * lx) * step;
    py += (fwd_y * ly) * step;
    pz += (fwd_z * ly + right_z * lx) * step;
    WriteGuestF32(base, cam + 0x50, px);
    WriteGuestF32(base, cam + 0x54, py);
    WriteGuestF32(base, cam + 0x58, pz);

    // Hand the pose to the menu-camera module so menu_cam_dump can emit the spot
    // you are flying at (position + look direction) as a front-end camera line.
    MenuCam_NoteFreecam(px, py, pz, g_freecam_yaw, g_freecam_pitch);
}

void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {
    int cam_idx = static_cast<int>(r24.u64);
    double scale = (cam_idx == 1) ? REXCVAR_GET(fov_1p_scale) : REXCVAR_GET(fov_3p_scale);
    if (scale != 1.0) {
        f1.f64 = f1.f64 * scale;
    }
}

// BadassBaboon's Recomp Adjustments:
// Continuous-time exponential decay for chase camera smoothing factors.
// In sub_82320298 (mcPlayerCamera::Update):
//   0x82320468 - f13 is the camera position chase/lag factor S1
//   0x823204F4 - f0  is the camera look-at / orientation factor S2
//
// On 30 FPS console the engine multiplied the raw profile factor by 0.5 and stepped once per update:
//   S(dt) = 1 - (1 - 0.5 * S_raw) ^ (30 * dt * scale)
// The engine ALREADY halves this factor itself when the frame rate is under
// 60, at 0x82320460:
//
//   82320454  cmpwi cr6, r11, 0x3C     ; r11 = round(1/dt), the frame rate
//   8232045c  lfs   f13, 0xE0(r11)     ; f13 = the raw tune value
//   82320460  bge   cr6, loc_82320468  ; >= 60 fps? leave it alone
//   82320464  fmuls f13, f13, f30      ; else f13 = tune * 0.5
//   82320468  <-- both camera hooks land here
//
// So the value arriving in the hook is the raw tune above 60 fps and half the
// tune below it. Multiplying by 0.5 unconditionally therefore quartered the
// factor whenever the measured rate dipped under 60, and doubled it back the
// moment it recovered. Around the 60 fps boundary that flips every frame,
// which is the camera jitter seen while drifting, sliding and doing donuts:
// exactly the moments where the frame rate wobbles across the threshold.
//
// Halving only when the engine did not reproduces the console reference curve
// continuously across the boundary.
static bool EngineAlreadyHalvedCameraFactor(const uint8_t* base) {
    const float fps = ReadGuestF32(base, kGuestFrameRate);
    // fctiwz after the +/- 0.5 bias is round-half-away-from-zero.
    const int fps_i = static_cast<int>(fps >= 0.0f ? fps + 0.5f : fps - 0.5f);
    return fps_i < 60;
}

static void ApplyCameraSmoothing(PPCRegister& reg) {
    if (!REXCVAR_GET(smooth_chase_cam)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    const double raw_k = reg.f64;
    if (raw_k <= 0.0 || raw_k >= 1.0 || dt <= 0.0f) return;

    const double k30 = EngineAlreadyHalvedCameraFactor(base) ? raw_k : 0.5 * raw_k;
    const double scale = REXCVAR_GET(chase_cam_smoothing_factor);
    reg.f64 = 1.0 - std::pow(1.0 - k30, static_cast<double>(dt) * 30.0 * scale);
}

void MCLACameraPosSmoothing(PPCRegister& f13) {
    ApplyCameraSmoothing(f13);
}

void MCLACameraLookAtSmoothing(PPCRegister& f0) {
    ApplyCameraSmoothing(f0);
}

// BadassBaboon's Recomp Adjustments: Continuous-time camera shake scaling
// In sub_82315000 at 0x82315188:
// The engine evaluates harmonic wave oscillations and discrete white noise from an LCG PRNG.
// Scaling rotational shake displacements (f29 pitch, f30 roll, f31 yaw) eliminates excessive
// high-frequency chatter at high framerates (60, 120, 240 FPS) while maintaining 100% natural
// camera orientation and responsive feel.
void MCLACameraShakeFilter(PPCRegister& f31, PPCRegister& f30, PPCRegister& f29, PPCRegister& f0, PPCRegister& r1) {
    if (!REXCVAR_GET(scale_camera_shake)) return;

    const double user_scale = REXCVAR_GET(camera_shake_scale);
    if (user_scale == 1.0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    f29.f64 *= user_scale;
    f30.f64 *= user_scale;
    f31.f64 *= user_scale;
    f0.f64 = f31.f64 * f31.f64;

    const uint32_t sp = static_cast<uint32_t>(r1.u64);
    if (sp) {
        WriteGuestF32(base, sp + 0x70, static_cast<float>(f29.f64));
        WriteGuestF32(base, sp + 0x74, static_cast<float>(f30.f64));
        WriteGuestF32(base, sp + 0x78, static_cast<float>(f31.f64));
    }
}

// BadassBaboon's Recomp Adjustments: Continuous camera transition interpolation & wrap fix
// In sub_8271F438 at 0x8271F920:
// When transitioning between camera modes (e.g. boot orbit mcTrackCS -> car chase camBoomCS,
// or driving camera -> burnout/cinematic mcCinematicCS), the engine decomposes source and destination
// matrices into Euler angles ("zxy") and interpolates yaw.
// In the original game, when (source_yaw - dest_yaw) crosses +/- pi mid-transition, the engine flips
// dest_yaw by 2*pi, causing a violent 180-degree flip in interpolated yaw (delta = 2*pi * t) and teleporting
// the camera across the car.
// Here we continuously unwrap the live relative angle delta across frames of the active transition.
// This allows both cameras to move, rotate, slew, and spin (e.g. 360-degree donuts/burnouts) dynamically,
// while mathematically guaranteeing a strictly continuous, monotonic transition arc without any
// hemisphere-flipping, stutter, or snapping.
static bool s_cam_trans_active = false;
static uint32_t s_cam_trans_obj = 0;
static uint32_t s_cam_trans_src = 0;
static uint32_t s_cam_trans_dst = 0;
static float s_cam_trans_last_t = 0.0f;
static float s_unwrapped_delta = 0.0f;
static float s_prev_raw_delta = 0.0f;

void MCLACameraTransitionSmoothing(PPCRegister& r27, PPCRegister& r26, PPCRegister& r4, PPCRegister& r1) {
    if (!REXCVAR_GET(smooth_camera_transitions)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t trans_obj = static_cast<uint32_t>(r27.u64);
    const uint32_t p_t = static_cast<uint32_t>(r26.u64);
    const uint32_t p_angles = static_cast<uint32_t>(r4.u64);
    const uint32_t sp = static_cast<uint32_t>(r1.u64);

    if (!trans_obj || !p_t || !p_angles) return;

    const float t = ReadGuestF32(base, p_t);
    const uint32_t src_cam = ReadGuestU32(base, trans_obj + 192);
    const uint32_t dst_cam = ReadGuestU32(base, trans_obj + 196);

    if (!src_cam || !dst_cam) return;

    // Resolve camera matrix: if cam+156 is non-zero, matrix is at cam+80, else cam+16
    const uint32_t src_mat = (base[src_cam + 156] != 0) ? (src_cam + 80) : (src_cam + 16);
    const uint32_t dst_mat = (base[dst_cam + 156] != 0) ? (dst_cam + 80) : (dst_cam + 16);

    // Yaw in "zxy" convention is atan2(M[2][0], M[2][2]) = atan2(offset 32, offset 40)
    const float src_yaw = std::atan2(ReadGuestF32(base, src_mat + 32), ReadGuestF32(base, src_mat + 40));
    const float dst_yaw = std::atan2(ReadGuestF32(base, dst_mat + 32), ReadGuestF32(base, dst_mat + 40));

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;

    auto normalize_angle = [kPi, kTwoPi](float a) {
        while (a > kPi)  a -= kTwoPi;
        while (a < -kPi) a += kTwoPi;
        return a;
    };

    const float raw_delta = normalize_angle(dst_yaw - src_yaw);

    const bool is_same_trans = s_cam_trans_active && (trans_obj == s_cam_trans_obj);
    const bool is_reversed = is_same_trans && (src_cam == s_cam_trans_dst) && (dst_cam == s_cam_trans_src);
    const bool is_restarted = is_same_trans && !is_reversed && (t < s_cam_trans_last_t - 0.2f);

    if (is_reversed) {
        // Native sub_8271F348 reversed this transition in-flight (e.g. user released or tapped 'A').
        // Source and destination cameras swapped, and native engine inverted t = 1.0 - t.
        // Invert our unwrapped delta so the angular path stays 100% continuous without snapping.
        s_unwrapped_delta = -s_unwrapped_delta;
        s_prev_raw_delta = raw_delta;
        s_cam_trans_src = src_cam;
        s_cam_trans_dst = dst_cam;
    } else if (!is_same_trans || is_restarted || src_cam != s_cam_trans_src || dst_cam != s_cam_trans_dst) {
        // Brand new transition starting: latch initial shortest-arc delta
        s_cam_trans_active = true;
        s_cam_trans_obj = trans_obj;
        s_cam_trans_src = src_cam;
        s_cam_trans_dst = dst_cam;
        s_unwrapped_delta = raw_delta;
        s_prev_raw_delta = raw_delta;
    } else {
        // Active transition continuing: unwrap raw_delta changes to guarantee no branch flips mid-arc
        float d_delta = normalize_angle(raw_delta - s_prev_raw_delta);
        s_unwrapped_delta += d_delta;
        s_prev_raw_delta = raw_delta;
    }

    s_cam_trans_last_t = t;

    const float t_clamped = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    const float smooth_yaw = src_yaw + t_clamped * s_unwrapped_delta;

    // Overwrite interpolated yaw at p_angles + 4 (sp + 0x74) before sub_821B9B28 builds matrix
    WriteGuestF32(base, p_angles + 4, smooth_yaw);
    if (sp) {
        WriteGuestF32(base, sp + 0x64, smooth_yaw);
    }
}

// In sub_8271F3B0 at 0x8271F3B0:
// Transition is finished and dest_camera is activated.
// Cleanly reset transition state without hard-snapping destination camera.
void MCLACameraTransitionComplete(PPCRegister& r3) {
    s_cam_trans_active = false;
    s_cam_trans_obj = 0;
    s_cam_trans_src = 0;
    s_cam_trans_dst = 0;
    s_cam_trans_last_t = 0.0f;
    s_unwrapped_delta = 0.0f;
    s_prev_raw_delta = 0.0f;
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_DebugCamGate() { return false; }
void Patch_DebugCam(PPCRegister& r3) {}
void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {}
void MCLACameraPosSmoothing(PPCRegister& f13) {}
void MCLACameraLookAtSmoothing(PPCRegister& f0) {}
void MCLACameraShakeFilter(PPCRegister& f31, PPCRegister& f30, PPCRegister& f29, PPCRegister& f0, PPCRegister& r1) {}
void MCLACameraTransitionSmoothing(PPCRegister& r27, PPCRegister& r26, PPCRegister& r4, PPCRegister& r1) {}
void MCLACameraTransitionComplete(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET

