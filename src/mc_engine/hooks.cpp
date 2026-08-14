#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <rex/chrono/clock.h>
#include <rex/runtime.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
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
#include "logging.h"
#include "hooks.h"
#include "graphics_button.h"
#include "larecomp_log.h"
#include "menu_camera.h"

// CVAR DEFINITIONS (Will appear in F4 menu)
// The '.lifecycle(kRequiresRestart)' forces the user to restart the game if they change the value.

REXCVAR_DEFINE_BOOL(skip_intro, false, "MCLA/Patches", "Skip the intro videos to prevent graphical issues.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
REXCVAR_DEFINE_BOOL(fps_60, true, "MCLA/Patches", "Increases vsync target to 60 FPS and enables deltatime.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "MCLA/Patches", "Disable Motion Blur completely.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_imposter_shadows, true, "MCLA/Patches", "Performance Mode: Foliage won't cast shadows.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_msaa, false, "MCLA/Patches", "Disable Anti-Aliasing (MSAA).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(break_pairwise_collision, false, "MCLA/Patches", "Disables pairwise collision resolution.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_rubberbanding, false, "MCLA/Patches", "Disables the AI RubberBand system, keeping the race fair.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(dbg_print, false, "MCLA/Patches", "Enable DbgPrint console outputs.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(physics_noclip, true, "MCLA/Physics", "Disable CCD/Pairwise Collision (Noclip)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_dof, false, "MCLA/Patches", "Disable Depth of Field (DoF) completely.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

// BadassBaboon's Recomp Adjustments: Ambient traffic & pedestrian density tuning for city performance
REXCVAR_DEFINE_BOOL(enable_ambient_tuning, true, "MCLA/Performance",
    "Enable ambient traffic and pedestrian density tuning.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(traffic_unspawn_dist, 250.0, "MCLA/Performance",
    "Traffic vehicle unspawn radius in meters (default 400.0, lower = higher FPS in city).")
    .range(100.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(ped_density_scale, 0.5, "MCLA/Performance",
    "Pedestrian density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(parked_car_scale, 0.5, "MCLA/Performance",
    "Parked car density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Steering physics and frame rate limiter CVARs
REXCVAR_DEFINE_BOOL(scale_steering_with_fps, true, "MCLA/Controls",
    "Scale vehicle steering delta to maintain consistent handling response at 60 FPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(steering_sensitivity, 1.0, "MCLA/Controls",
    "Vehicle steering sensitivity multiplier (0.2 = tighter, 1.0 = stock, 2.0 = faster).")
    .range(0.2, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// 0 by default: the presenter's own vsync already paces the frame, and the
// limiter's final wait is a busy spin. Set it only when running with vsync off.
REXCVAR_DEFINE_INT32(fps_limit, 0, "MCLA/Performance",
    "Frame rate cap (0 = uncapped, 60 = 60 FPS, 120 = 120 FPS, 144 = 144 FPS).")
    .range(0, 360)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_traffic_scale, 1.0, "MCLA/LOD", "Escala de LOD do Tráfego (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(lod_city_scale, 1.0, "MCLA/LOD", "Escala de LOD da Cidade (0.1 - 10.0)")
    .range(0.1, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "MCLA/Patches", "Screen Aspect Ratio")
    .allowed({"16:9", "16:10", "21:9", "32:9"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rexglue_settings_in_gameoptions, false, "MCLA/Settings",
    "Replace Game Options with ReXGlue Settings overlay (F4)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(single_tile, false, "MCLA/Performance",
    "Render the scene in a single predicated-tiling tile instead of two. Halves draw calls "
    "and state traffic with MSAA on. Requires the enlarged virtual EDRAM (SDK >= this build).")
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

    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "MCLA/Patches", "Screen Aspect Ratio")
    .allowed({"16:9", "21:9", "32:9"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Function to apply/revert the Aspect Ratio patch in GPU memory
static void ApplyAspectRatioPatch(std::string_view ratio) {
    extern uint8_t* g_guest_mem;
    if (!g_guest_mem) return;
    
    uint8_t* patch_ptr = g_guest_mem + 0x8201E7EC;
    
    LARECOMP_APP_INFO("ApplyAspectRatioPatch called! Ratio: {}, Memory Before: {:02X} {:02X} {:02X} {:02X}", 
        ratio, patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);
    
    uint32_t val = 0x3FE38E39; // 16:9 Default (1.777777f)
    if (ratio == "16:10") {
        val = 0x3FCCCCCD; // 16:10 (1.600000f) -> Adicionado aqui
    } else if (ratio == "21:9") {
        val = 0x40155555; // 21:9 (2.333333f)
    } else if (ratio == "32:9") {
        val = 0x40638E39; // 32:9 (3.555555f)
    }
    
    // Write the 4 bytes in Big-Endian at the correct address
    patch_ptr[0] = (val >> 24) & 0xFF; // MSB
    patch_ptr[1] = (val >> 16) & 0xFF;
    patch_ptr[2] = (val >> 8)  & 0xFF;
    patch_ptr[3] = val         & 0xFF; // LSB
    
    LARECOMP_APP_INFO("Memory After: {:02X} {:02X} {:02X} {:02X}", 
        patch_ptr[0], patch_ptr[1], patch_ptr[2], patch_ptr[3]);
}

void InitHooks() {
    ApplyAspectRatioPatch(REXCVAR_GET(aspect_ratio));

    rex::cvar::RegisterChangeCallback("aspect_ratio",
        [](std::string_view name, std::string_view new_value) {
            ApplyAspectRatioPatch(new_value);
        }
    );
}

// HOOK FUNCTIONS (Called in the middle of translated Assembly execution)

bool SkipIntro() {
    return REXCVAR_GET(skip_intro);
}

// 0x821315E4 in sub_82131508, the branch taken when dword_82830B14 (the legals
// screen object) is null:
//
//     if (dword_82830B14) { ...ordered teardown... }
//     else                  sub_822E5B00(dword_8287E064, -1);
//
// sub_822E5B00 ORs its argument into *(mgr + 424), which sub_822E5B60 folds into
// *(mgr + 364) on the next frame. That word is the render pass bitmask that
// sub_822E6408 walks bit by bit (v12 = 1; ...; v12 *= 2; while v12 != 0x40000000),
// dispatching the 13 renderers of every set bit.
//
// Every normal caller ORs 0xFEFFFFFF instead: sub_821F9918 (0x821F99BC) and
// sub_821FCED8 (0x821FCF10). Bit 24 is deliberately excluded and the game never
// sets it anywhere else. This branch's -1 does set it, so skipping the intro
// enables a pass whose stage (mgr[24]) was never prepared, and it draws with
// uninitialised state - corrupt geometry and shading across the frame.
//
// Rewrite the argument to the same mask the rest of the engine uses. Applies to
// any way of reaching a null legals object, so it is independent of which
// SkipIntro hook is active.
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {
    if (REXCVAR_GET(skip_intro)) {
        r4.u32 = 0xFEFFFFFFu;
    }
}

static bool GetAspectRatio(double& out_val) {
    std::string ratio = REXCVAR_GET(aspect_ratio);
    if (ratio == "4:3") {
        out_val = 1.3333333;
        return true;
    } else if (ratio == "16:10") {
        out_val = 1.6000000;
        return true;
    } else if (ratio == "21:9") {
        out_val = 2.3333333;
        return true;
    } else if (ratio == "32:9") {
        out_val = 3.5555556;
        return true;
    }
    return false;
}


bool Patch_AspectRatio_82233EB4(PPCRegister& f0) {
    return GetAspectRatio(f0.f64);
}
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) {
    return GetAspectRatio(f10.f64);
}
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) {
    return GetAspectRatio(f12.f64);
}
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) {
    return GetAspectRatio(f13.f64);
}

bool Patch_60FPS_Jump() {
    return REXCVAR_GET(fps_60);
}

// Single-tile predicated tiling — hook at 0x8217A700 in
// grcDevice::BeginTiledRendering (sub_8217A470), the convergence point right
// after the per-orientation tile size math and before the tile rect loop.
// r7 = tile width, r8 = tile height (both feed the 160/32-aligned dimensions,
// the tile rect array and the PredictedTile RT allocation); r28/r25 = screen
// width/height. Forcing tile size = screen size makes the tile count land on
// 1, so the scene is submitted once instead of once per tile. Tile count is
// also stored at 0x827D42A4 before this point — overwrite it to 1 for the
// resolve/end path that reads the global. Needs the SDK's enlarged virtual
// EDRAM (720p 2xMSAA color+depth = 2880 tiles > the real 2048).
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28) {
    if (!REXCVAR_GET(single_tile)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    r7.u64 = r28.u64;  // tile width  = screen width
    r8.u64 = r25.u64;  // tile height = screen height

    // dword_827D42A4 = tile count (already computed and stored) -> 1
    base[0x827D42A4 + 0] = 0;
    base[0x827D42A4 + 1] = 0;
    base[0x827D42A4 + 2] = 0;
    base[0x827D42A4 + 3] = 1;
}

// EDRAM capacity check bypass — sub_82410D70 (guest D3D CreateSurface,
// auto-allocation path) validates alloc_base + size_in_tiles <= 0x800 (2048,
// the real console EDRAM) at 0x82410E34 and destroys the surface / returns
// NULL past it. Single-tile 720p 2xMSAA needs 2880 tiles, so every render
// target fails and the screen collapses into aliased EDRAM bands. The SDK's
// virtual EDRAM is 4096 tiles and the guest surface header keeps 12-bit base
// fields (max 4095), so allocations up to 4096 are safe. r11 = base + size;
// returning true jumps to the success branch (0x82410E48).
bool Patch_EdramLimit(PPCRegister& r11) {
    if (!REXCVAR_GET(single_tile)) return false;
    return r11.u64 <= 4096;
}

static float ReadGuestF32(const uint8_t* base, uint32_t addr);
static void WriteGuestF32(uint8_t* base, uint32_t addr, float val);
static void ApplyAmbientDensityTuning();

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

static uint32_t ReadGuestU32(const uint8_t* base, uint32_t addr) {
    return (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
           (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
}

static void WriteGuestU32(uint8_t* base, uint32_t addr, uint32_t val) {
    base[addr + 0] = (val >> 24) & 0xFF;
    base[addr + 1] = (val >> 16) & 0xFF;
    base[addr + 2] = (val >> 8) & 0xFF;
    base[addr + 3] = val & 0xFF;
}

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

// Intro/legals pacing at 60 FPS. The intro SWF is advanced by sub_821F9918
// with a HARDCODED 1/60s per call (immune to any dt patch), and that function
// runs twice per frame (present callback sub_821FC008 + main tick
// sub_821FC588). At the console's 30 Hz that totals real time; at 60 Hz it's
// exactly 2x. Skipping every other advance (jump to the 0x821F9A00 epilogue)
// restores the original rate.
bool Hook_IntroHalfRate() {
    if (!REXCVAR_GET(fps_60)) return false;
    static uint32_t call_count = 0;
    return (call_count++ & 1) != 0;  // true = skip this advance
}

// Swap-interval patch at 0x82419AA0 ("li r11, 2"): the game always requests
// D3D interval TWO (30 FPS); replacing with 1 requests 60 Hz. Note the current
// RexGlue command processor ignores the guest swap interval (host vblank is a
// fixed 60 Hz timer), so this is kept only for correctness of the swap packet.
bool Patch_60FPS_Byte(PPCRegister& r11) {
    if (REXCVAR_GET(fps_60)) {
        r11.u64 = 1; // Replaces the original value with 1 (li r11, 1)
        return true; // Skips the original instruction
    }
    return false;
}

// The same hook works for both Motion Blur instructions!
bool Patch_DisableMotionBlur(PPCRegister& r3) {
    if (REXCVAR_GET(disable_motion_blur)) {
        r3.u64 = 0; // li r3, 0
        return true;
    }
    return false;
}

bool Patch_DisableMSAA(PPCRegister& r11) {
    if (REXCVAR_GET(disable_msaa)) {
        r11.u64 = 1; // li r11, 1
        return true;
    }
    return false;
}

// BadassBaboon's Recomp Adjustments: Foliage imposter shadow bypass
bool Patch_DisableImposterShadows(PPCRegister& r11) {
    if (REXCVAR_GET(disable_imposter_shadows)) {
        r11.u64 = 0; // li r11, 0
        return true;
    }
    return false;
}

// Returns 'true' to inject a 'blr' (return from collision function)
bool Patch_PhysicsCollision() {
    return REXCVAR_GET(break_pairwise_collision);
}

bool Patch_DisableRubberBanding() {
    return REXCVAR_GET(disable_rubberbanding);
}

bool Patch_DisableDoF() {
    return REXCVAR_GET(disable_dof);
}

// Disable DoF at the composite. Hooked at sub_8260EBB8 entry where r3 = dofObj
// (dword_829054A0). Zeroing the circle-of-confusion vector at dofObj+0xF0 collapses
// the per-pixel blur to sharp with the scene fully intact — verified in gameplay,
// menu AND freecam. The composite runs every frame DoF is drawn, so this covers
// every state without touching the setters (blocking those left stale DoF in the
// menu). The other composite inputs (+0x158/+0x128/+0x138/+0x1B0) are NOT safe to
// zero — they white-out / desaturate the frame — so only +0xF0 is touched.
void Patch_DofComposite(PPCRegister& r3) {
    if (!REXCVAR_GET(disable_dof)) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t o = static_cast<uint32_t>(r3.u64);  // dofObj guest address
    if (!o) return;
    for (int i = 0; i < 16; ++i) base[o + 0xF0 + i] = 0;  // CoC vector = 0 -> no blur
}

void Patch_ScaleTrafficLOD(PPCRegister& f0) {
    f0.f64 = f0.f64 * REXCVAR_GET(lod_traffic_scale);
}

void UpdateCityLODMemory() {
    extern uint8_t* g_guest_mem;
    if (!g_guest_mem) return;

    float scale = static_cast<float>(REXCVAR_GET(lod_city_scale));
    float final_lod = scale * 300.0f;

    uint32_t int_val;
    std::memcpy(&int_val, &final_lod, sizeof(float));

    uint32_t city_lod_addr = 0x827E0DE0; 
    
    // Injeção Big-Endian segura
    g_guest_mem[city_lod_addr + 0] = (int_val >> 24) & 0xFF;
    g_guest_mem[city_lod_addr + 1] = (int_val >> 16) & 0xFF;
    g_guest_mem[city_lod_addr + 2] = (int_val >> 8)  & 0xFF;
    g_guest_mem[city_lod_addr + 3] = int_val         & 0xFF;
}

void Patch_ScaleCityLOD(PPCRegister& f13) {
    // Multiplicamos o valor que a engine acabou de ler da memória pelo nosso slider
    f13.f64 = f13.f64 * REXCVAR_GET(lod_city_scale);
}

bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) {
    if (!REXCVAR_GET(rexglue_settings_in_gameoptions)) {
        return false;
    }
    mc::ui::RequestOpenRexGraphicsMenu();
    r3.u64 = 1;
    return true;
}

void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {
    int cam_idx = static_cast<int>(r24.u64);
    double scale = (cam_idx == 1) ? REXCVAR_GET(fov_1p_scale) : REXCVAR_GET(fov_3p_scale);
    if (scale != 1.0) {
        f1.f64 = f1.f64 * scale;
    }
}

    // Skip the original Game Options block and fall through to the epilogue.
    return true;
}static float ReadGuestF32(const uint8_t* base, uint32_t addr) {
    uint32_t be = (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
                  (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
    float val;
    std::memcpy(&val, &be, sizeof(float));
    return val;
}

static void WriteGuestF32(uint8_t* base, uint32_t addr, float val) {
    uint32_t be;
    std::memcpy(&be, &val, sizeof(float));
    base[addr + 0] = (be >> 24) & 0xFF;
    base[addr + 1] = (be >> 16) & 0xFF;
    base[addr + 2] = (be >> 8) & 0xFF;
    base[addr + 3] = be & 0xFF;
}

// BadassBaboon's Recomp Adjustments: Precision frame rate limiter
static void EnforceFrameLimit() {
    int32_t limit = REXCVAR_GET(fps_limit);
    if (limit <= 0) return;

    using clock = std::chrono::steady_clock;
    static auto last_frame_time = clock::now();

    const auto target_duration = std::chrono::duration<double, std::micro>(1000000.0 / static_cast<double>(limit));
    auto now = clock::now();
    auto elapsed = now - last_frame_time;

    if (elapsed < target_duration) {
        auto sleep_time = target_duration - elapsed;
        if (sleep_time > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(sleep_time - std::chrono::milliseconds(1)));
        }
        while (clock::now() - last_frame_time < target_duration) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
        }
    }
    last_frame_time = clock::now();
}

// BadassBaboon's Recomp Adjustments: Core 60 FPS Clock Delta Pipeline
// 0x821BDAB0: runs after subf r8,r10,r11 in sub_821BDA90.
// Clamps max ticks and runs precision limiter.
void MCLAFrameDelta(PPCRegister& r8) {
    if (!REXCVAR_GET(fps_60)) return;
    EnforceFrameLimit();
    uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
    if (hz == 0) hz = 50000000;
    uint64_t max_ticks = static_cast<uint64_t>(0.125 * static_cast<double>(hz));
    if (r8.u64 > max_ticks) {
        r8.u64 = max_ticks;
    }
}

// BadassBaboon's Recomp Adjustments: real delta instead of the fixed timestep.
//
// By 0x821BDAF8 sub_821BDA90 has already stored the measured unscaled delta at
// [r3+0x58] and the scaled one at [r3+0x08]. Two separate blocks downstream then
// throw that away and substitute the fixed timestep at [r3+0x20]; both have to
// be handled, and which one runs depends on [r3+0x3A] / [r3+0x3C]:
//
//   loc_821BDB58  reached when both are zero, after the +0x14 / +0x18
//                 accumulator updates. Loads [r3+0x20], and if the real delta is
//                 at least that big writes the FIXED value over +0x58 and +0x08
//                 (0x821BDB84/0x821BDB88). Skipped wholesale by jumping to
//                 loc_821BDC34 -- the accumulators are already updated by then,
//                 so nothing else is lost.
//   loc_821BDB90  reached from 0x821BDB1C / 0x821BDB28 when either flag is set.
//                 Not covered by the jump above, since it sits before it in the
//                 flow. Does the same substitution out of f11, so f11 is
//                 rewritten with the real delta instead.
//
// Returns true to take the jump. Baboon's build jumped unconditionally; gating
// it on fps_60 is the only change, and it makes the cvar actually turn the whole
// thing off instead of leaving half of it live.
bool MCLAUseRealDelta() {
    return REXCVAR_GET(fps_60);
}

// 0x821BDB90, after `lfs f11, 0x20(r3)` has loaded the fixed timestep. f11 feeds
// both `stfs f11, 0x58(r3)` and `fmuls f0, f11, f13` -> `stfs f0, 8(r3)`, so
// replacing it with [r3+0x58] (the measured unscaled delta stored at 0x821BDAF8)
// publishes the real frame time down this path too.
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {
    if (!REXCVAR_GET(fps_60)) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    f11.f64 = static_cast<double>(ReadGuestF32(base, static_cast<uint32_t>(r3.u64) + 0x58));
}

// Fires at 0x822C22EC, right after the clock update (sub_821BDA90). The delta
// time itself is delivered at the clock source (MCLAFrameDelta /
// MCLAUseRealDelta / MCLAFixedStepPath) rather than overwritten late in the
// frame, which is what caused traffic jitter and physics stutter. What is left
// here is the per-frame housekeeping.
void Patch_DeltaTimePre() {
    TickVinylReadbackWindow();  // runs every frame regardless of fps_60
    TickVinylShapeCapture();    // hands-free shape-catalog sweep, if requested
    TickButtonPrompts();        // picks up a live button_prompts change
    TickCustomMusic();          // custom radio: volume + end-of-track advance
    ApplyAmbientDensityTuning();  // no-op unless an ambient cvar moved
}

// Loop-entry anchor. r24 must NOT be modified: the game divides game_dt by it
// on its own (sub_821BD910) and r24 = 0 would clear the sub-tick gate at
// 0x827D754C and freeze physics.
void Patch_DeltaTime(PPCRegister& r24) {
    (void)r24;
}

// BadassBaboon's Recomp Adjustments:
// 0x823203D4, in sub_82320298 (mcPlayerCamera::Update).
// Applies the 60 FPS exponential decay formula to the camera boom interpolation
// constant before it is passed to matrix Lerp.
void MCLACameraBoomSmoothing(PPCRegister& f1) {
    if (!REXCVAR_GET(smooth_chase_cam)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    // Read current frame dt (clock+0x08)
    float dt = ReadGuestF32(base, 0x827D7508);
    if (!(dt > 0.0f)) return;

    double k = f1.f64;
    if (k > 0.0 && k < 1.0) {
        double factor = REXCVAR_GET(chase_cam_smoothing_factor);
        f1.f64 = 1.0 - std::pow(1.0 - k, dt * 30.0 * factor);
    }
}

// BadassBaboon's Recomp Adjustments: ambient traffic / pedestrian density.
//
// mcAmbientDensityTuning is not a separate object: sub_826F5CB0 calls its
// constructor sub_826F5B18 with its OWN `this` (`sub_826F5B18(a1)`), so the
// tuning fields are the base of the ~5936-byte ambient zone. The zone array is
// built in sub_826D8E70 -- 31 zones, `v5 = manager + 56476`, stride 1484 dwords
// -- and each one re-parses $/tune/ambients/density_tuning.xml through
// sub_826F4CB8, whose r31 is that zone. r31 is therefore the tuning object,
// and 31 hook firings per load is expected, one per zone.
//
// (The value passed as the parse's 4th argument is NOT the instance: it is the
// shared class descriptor returned by vtable slot 1, identical across all 31
// zones. Writing tuning fields through it corrupts a live RAGE structure.)
//
// The override has to land AFTER the parse -- hooking the constructor is
// pointless because the parse overwrites every field it set.
//
// Per-zone originals are captured from what the parse left behind, and every
// override is computed from those, so re-parsing a zone never compounds the
// scale the way the original code did.
struct DensityTuningValues {
    float unspawn = 0.0f;
    float ped = 0.0f;
    float parked = 0.0f;
};

static std::mutex g_density_mutex;
static std::map<uint32_t, DensityTuningValues> g_density_orig;  // zone address -> XML values

// Last state pushed into guest memory, so the per-frame tick only writes when a
// cvar actually moved.
static bool g_density_applied_valid = false;
static bool g_density_applied_enabled = false;
static DensityTuningValues g_density_applied;

// Writes one zone. Caller holds g_density_mutex.
static void WriteDensityZone(uint8_t* base, uint32_t a, const DensityTuningValues& orig,
                             bool enabled, const DensityTuningValues& want) {
    if (!enabled) {
        WriteGuestF32(base, a + 16, orig.unspawn);
        WriteGuestF32(base, a + 92, orig.ped);
        WriteGuestF32(base, a + 152, orig.parked);
        return;
    }
    // Absolute metres, applied as given. The original code silently ignored any
    // value above the stock 400, which made the upper half of the cvar's
    // 100..600 range do nothing.
    WriteGuestF32(base, a + 16, want.unspawn > 0.0f ? want.unspawn : orig.unspawn);
    WriteGuestF32(base, a + 92, orig.ped * want.ped);
    WriteGuestF32(base, a + 152, orig.parked * want.parked);
}

static void ReadDensityCvars(bool& enabled, DensityTuningValues& want) {
    enabled = REXCVAR_GET(enable_ambient_tuning);
    want.unspawn = static_cast<float>(REXCVAR_GET(traffic_unspawn_dist));
    want.ped = static_cast<float>(REXCVAR_GET(ped_density_scale));
    want.parked = static_cast<float>(REXCVAR_GET(parked_car_scale));
}

// 0x826F4E3C, the instruction after the density_tuning.xml parse returns.
// r31 is the ambient zone, i.e. the tuning object.
void MCLAAmbientDensityTuning(PPCRegister& r31) {
    const uint32_t a = static_cast<uint32_t>(r31.u64);
    if (a == 0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);

    // The parse just restored this zone to its XML values, so re-reading here
    // is what keeps the scale from compounding across reloads.
    DensityTuningValues orig;
    orig.unspawn = ReadGuestF32(base, a + 16);   // 400.0 stock
    orig.ped = ReadGuestF32(base, a + 92);       // 0.007 stock
    orig.parked = ReadGuestF32(base, a + 152);   // 0.25 stock
    const bool first = g_density_orig.find(a) == g_density_orig.end();
    g_density_orig[a] = orig;

    WriteDensityZone(base, a, orig, enabled, want);

    // One line per zone the first time it is seen; reloads are silent, since 31
    // identical lines every district change is noise.
    if (first) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] zone {} at 0x{:08X}: unspawn {:.1f} -> {:.1f}, "
            "ped {:.4f} -> {:.4f}, parked {:.2f} -> {:.2f}",
            g_density_orig.size(), a, orig.unspawn, ReadGuestF32(base, a + 16), orig.ped,
            ReadGuestF32(base, a + 92), orig.parked, ReadGuestF32(base, a + 152));
    }
}

// Per-frame, so the cvars behave as the kHotReload they are declared to be.
// Costs a compare per frame and touches guest memory only when one moved.
static void ApplyAmbientDensityTuning() {
    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    if (g_density_applied_valid && g_density_applied_enabled == enabled &&
        g_density_applied.unspawn == want.unspawn && g_density_applied.ped == want.ped &&
        g_density_applied.parked == want.parked) {
        return;
    }
    g_density_applied_enabled = enabled;
    g_density_applied = want;
    g_density_applied_valid = true;

    if (g_density_orig.empty()) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    for (const auto& [addr, orig] : g_density_orig) {
        WriteDensityZone(base, addr, orig, enabled, want);
    }
    LARECOMP_APP_INFO("[Ambient Tuning] {} zones updated (enabled={}, unspawn={:.1f}, ped={:.2f}, parked={:.2f})",
                      g_density_orig.size(), enabled, want.unspawn, want.ped, want.parked);
}

// BadassBaboon's Recomp Adjustments:
// 0x822A2ED4 in sub_822A2988: `lfs f0, 0xC(r20)` with r20 = 0x827D7500, so f0
// is the clock's inv_game_dt, and the next lines turn the steering delta into a
// per-second rate with it. Halving it at 60 FPS reproduces the 30 FPS response
// the handling was tuned against.
void Patch_SteeringSensitivity(PPCRegister& f0) {
    double sens = REXCVAR_GET(steering_sensitivity);
    if (REXCVAR_GET(scale_steering_with_fps) && REXCVAR_GET(fps_60)) {
        sens *= 0.5;
    }
    f0.f64 *= sens;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void Hook_PhotoModeCapture(PPCRegister& r3) {}
void ExportVinyl() {}
void DumpVinylShapes() {}
void RequestVinylShapeCapture() {}
void TickVinylShapeCapture() {}
void ApplyDebugOptions() {}
void InitHooks() {}
bool SkipIntro() { return false; }
void MCLA_SkipIntroRenderPassMask(PPCRegister& r4) {}
bool Patch_AspectRatio_82233EB4(PPCRegister& f0) { return false; }
bool Patch_AspectRatio_82214BB8(PPCRegister& f10) { return false; }
bool Patch_AspectRatio_822E5E68(PPCRegister& f12) { return false; }
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13) { return false; }
bool Patch_60FPS_Jump() { return false; }
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28) {}
bool Patch_EdramLimit(PPCRegister& r11) { return false; }
bool Patch_DebugCamGate() { return false; }
void Patch_DebugCam(PPCRegister& r3) {}
bool Hook_IntroHalfRate() { return false; }
bool Patch_60FPS_Byte(PPCRegister& r11) { return false; }
bool Patch_DisableMotionBlur(PPCRegister& r3) { return false; }
bool Patch_DisableMSAA(PPCRegister& r11) { return false; }
bool Patch_PhysicsCollision() { return false; }
bool Patch_DisableRubberBanding() { return false; }
bool Patch_DisableDoF() { return false; }
void Patch_DofComposite(PPCRegister& r3) {}
void Patch_ScaleTrafficLOD(PPCRegister& f0) {}
void Patch_RideHeightFit(PPCRegister& f30) {}
bool Patch_WheelFitBypass() { return false; }
bool Patch_SpeedUnits(PPCRegister& r11) { return false; }
void TickButtonPrompts() {}
void Hook_PlatformVarInit(PPCRegister& r27) {}
bool Patch_PlatformPush(PPCRegister& r5) { return false; }
void Hook_SwfContextEnter(PPCRegister& r3) {}
void UpdateCityLODMemory() {}
void Patch_ScaleCityLOD(PPCRegister& f13) {}
bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) { return false; }
void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24) {}
void Patch_DeltaTimePre() {}
void Patch_DeltaTime(PPCRegister& r24) {}
void Patch_BypassVehicleDLC(PPCRegister& r30) {}
void Hook_CaptureDistrict(PPCRegister& r3) {}
void Hook_LzxDecompressPre(PPCRegister& r1) {}
void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {}
void MCLACameraBoomSmoothing(PPCRegister& f1) {}
void MCLAAmbientDensityTuning(PPCRegister& r31) {}
bool Patch_DisableImposterShadows(PPCRegister& r11) { return false; }
void Patch_SteeringSensitivity(PPCRegister& f0) {}
void MCLAFrameDelta(PPCRegister& r8) {}
bool MCLAUseRealDelta() { return false; }
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {}
#endif // REXGLUE_HAS_XEO3_TARGET
