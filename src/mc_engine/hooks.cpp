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
#include <rex/ui/imgui_dialog.h>
#include "imgui.h"
#include "logging.h"
#include "hooks.h"
#include "graphics_button.h"
#include "larecomp_log.h"

// CVAR DEFINITIONS (Will appear in F4 menu)
// The '.lifecycle(kRequiresRestart)' forces the user to restart the game if they change the value.

REXCVAR_DEFINE_BOOL(skip_intro, false, "MCLA/Patches", "Skip the intro videos to prevent graphical issues.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// BadassBaboon's Recomp Adjustments: Enable 60 FPS by default
REXCVAR_DEFINE_BOOL(fps_60, true, "MCLA/Patches", "Increases vsync target to 60 FPS and enables deltatime.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(disable_motion_blur, false, "MCLA/Patches", "Disable Motion Blur completely.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_imposter_shadows, false, "MCLA/Patches", "Performance Mode: Foliage won't cast shadows.")
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
// 0 by default: the presenter's own vsync already paces the frame, and the
// limiter's final wait is a busy spin. Set it only when running with vsync off.
REXCVAR_DEFINE_INT32(fps_limit, 0, "MCLA/Performance",
    "Frame rate cap (0 = uncapped, 60 = 60 FPS, 120 = 120 FPS, 144 = 144 FPS).")
    .range(0, 360)
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
    if (ratio == "21:9") {
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

static bool GetAspectRatio(double& out_val) {
    std::string ratio = REXCVAR_GET(aspect_ratio);
    if (ratio == "21:9") {
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

// 8-bit 60FPS patch. Assumes 'r3' by default, check in IDA.
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

bool OpenRexGraphicsFromGameOptions_826686D4(PPCRegister& r3) {
    mc::ui::RequestOpenRexGraphicsMenu();

    // The original handler would return 1 when consuming the action.
    r3.u64 = 1;

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
