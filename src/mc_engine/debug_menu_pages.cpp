// The pages of the hidden DEBUG menu. See debug_menu_pages.h.

#include "debug_menu_pages.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "debug_menu.h"

namespace mcla_menu {

namespace {

// ── Value tables ───────────────────────────────────────────────────────
//
// Names stay inside A-Z, 0-9, space, '+', '.' and parentheses like every row of the pause menu, and
// no ':' or '/' (the value's text is also the name of a Flash element, pause_menu.cpp "speed-unit note").

template <typename T, std::size_t N>
constexpr int Len(const T (&)[N]) {
    return int(N);
}

// An INT32 cvar of 0 and 1 (frame_pacing): a Bool row would write "true" and "false".
constexpr const char* kOffOnVals[]  = {"0", "1"};
constexpr const char* kOffOnNames[] = {"OFF", "ON"};

constexpr const char* kFreecamVals[]  = {"off", "free"};
constexpr const char* kFreecamNames[] = {"OFF", "ON"};
constexpr double kCamSpeedVals[]      = {10.0, 20.0, 40.0, 80.0, 150.0, 300.0};
constexpr double kCamSensVals[]       = {0.5, 1.0, 1.5, 2.5, 4.0, 6.0, 10.0};

// The 64 shots menu_cam_slot can pin (0-63).
struct SlotTable {
    double v[64];
};
constexpr SlotTable MakeSlotTable() {
    SlotTable t{};
    for (int i = 0; i < 64; ++i) t.v[i] = double(i);
    return t;
}
constexpr SlotTable kMenuCamSlots = MakeSlotTable();

// Which render phase bits SHADOWS clears (SHADOW PHASES).
//
// STOCK is what the game's own `noshadows` switch clears: the sun cascade phases
// 0x20/0x40/0x80/0x100 plus 0x2000/0x4000. Measured at midnight those are not even in
// renderer+364, so clearing them does nothing: the shadow that is actually drawn comes from phase
// 0x400, which sub_823112C0 routes to the shadowNight / shadowFastBlend technique group and which
// `noshadows` leaves alone. Hence BLEND.
//
// ALL also takes 0x10/0x200/0x800, which are live passes unrelated to shadows; it corrupts the
// frame and is only useful for narrowing a phase down. (These lists were rows of the normal
// PERFORMANCE pages until SX-G5.)
constexpr const char* kShadowBitVals[]  = {"0x61E0", "0x400", "0x65E0", "0x7DF0"};
constexpr const char* kShadowBitNames[] = {"STOCK", "BLEND ONLY", "STOCK+BLEND", "ALL (BREAKS)"};
constexpr double kBreakFpsVals[]        = {0.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0};

#if defined(LARECOMP_HAS_GPU)
constexpr const char* kAoViewVals[]  = {"0", "1", "2", "3", "4", "5", "6"};
constexpr const char* kAoViewNames[] = {"OFF", "FACTOR", "VISIBILITY", "AMBIENT SHARE",
                                        "NORMALS", "DISTANCE", "HISTORY"};
constexpr double kAoHistoryVals[]    = {1.0, 2.0, 4.0, 8.0, 12.0, 16.0, 32.0, 64.0};
constexpr double kAoFadeEndVals[]    = {20.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1000.0};
constexpr const char* kHdrViewVals[]  = {"0", "1", "2", "3", "4", "5"};
constexpr const char* kHdrViewNames[] = {"OFF", "UI ONLY", "SCENE ONLY", "UI COVERAGE",
                                         "OVER WHITE", "DIFFERENCE"};
constexpr const char* kMarkerLevelVals[]  = {"0", "1", "2"};
constexpr const char* kMarkerLevelNames[] = {"FRAME", "PHASES", "DETAIL"};
constexpr double kGhostLogVals[]          = {0.0, 1.0, 10.0, 60.0, 300.0};
constexpr double kMemoryLogVals[]         = {0.0, 60.0, 300.0, 1800.0};
#endif

// The shapes aspect_ratio allows (":" is not allowed in a name).
constexpr const char* kAspectVals[]  = {"auto", "16:9", "16:10", "21:9", "32:9"};
constexpr const char* kAspectNames[] = {"AUTO", "16X9", "16X10", "21X9", "32X9"};

// ── The tables ─────────────────────────────────────────────────────────
//
// Every cvar named here is tagged .debug_only() and live. tools/check_settings_exposure.py reads these
// tables (they start with kDebug) and fails on a cvar that is not, or that is restart-only: a setting
// that is only read at start belongs to a launch argument, not a row here (a debug row is never saved,
// so it could not be kept for the next start).

// The hub, in the order of SETTINGS-REVIEW.md 5.3. The rows are Submenu rows: each opens its page.
const ItemDef kDebugHubItems[] = {
    Submenu("PM_RxDbgSubFixes", "FIXES",       "RxDbgFixesMenu"),
    Submenu("PM_RxDbgSubPerf",  "PERFORMANCE", "RxDbgPerfMenu"),
    Submenu("PM_RxDbgSubRend",  "RENDERING",   "RxDbgRendMenu"),
    Submenu("PM_RxDbgSubCam",   "CAMERA",      "RxDbgCamMenu"),
    Submenu("PM_RxDbgSubTools", "TOOLS",       "RxDbgToolsMenu"),
    Submenu("PM_RxDbgSubLog",   "LOGGING",     "RxDbgLogMenu"),
    Submenu("PM_RxDbgSubInfo",  "INFO",        "RxDbgInfoMenu"),
};

// FIXES: each fix of the recomp with the switch that turns it off, to see whether it caused a bug
// (owner's answer 2026-11-13: kept as triage switches). The rows are the cvars as they are: the page makes
// no new switch.
const ItemDef kDebugFixesItems[] = {
    Bool("PM_RxDbgRealDelta",  "real_frame_delta",          "REAL FRAME DELTA: "),
    // The Plume renderer's frame pacing (frame_pacing.cpp); the cvar is 0 or 1.
    NativeOnly(Str("PM_RxDbgFramePacing", "frame_pacing",   "FRAME PACING: ",
                   kOffOnVals, kOffOnNames, Len(kOffOnVals))),
    Bool("PM_RxDbgChassis",    "smooth_chassis_depth",      "SUSPENSION FIX: "),
    Bool("PM_RxDbgChaseCam",   "smooth_chase_cam",          "SMOOTH CHASE CAM: "),
    Bool("PM_RxDbgVehPhysics", "smooth_vehicle_physics",    "SMOOTH VEHICLE PHYSICS: "),
    Bool("PM_RxDbgCamTrans",   "smooth_camera_transitions", "SMOOTH CAMERA TRANSITIONS: "),
    Bool("PM_RxDbgShakeFix",   "scale_camera_shake",        "CAMERA SHAKE FIX: "),
    Bool("PM_RxDbgCountdown",  "ui_countdown_fix",          "COUNTDOWN FIX: "),
    Bool("PM_RxDbgKeyRepeat",  "ui_input_repeat_30hz",      "MENU KEY REPEAT: "),
    Bool("PM_RxDbgMenuInput",  "ui_advance_on_input",       "MENU INPUT: "),
    Bool("PM_RxDbgMenu3d",     "ui_interpolate_camera",     "SMOOTH MENU 3D: "),
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Bool("PM_RxDbgDither", "mcla_gpu_present_dither", "PRESENT DITHER: ")),
    NativeOnly(Bool("PM_RxDbgA2c",    "mcla_gpu_a2c_dither_all", "ALPHA TO MASK FIX: ")),
#endif
    Action("PM_RxDbgFixReset", kActionDebugReset),
};

// PERFORMANCE: the switches that trade frame pacing, CPU time and picture for speed, and that have no row
// in the normal menus (SX-G5; owner's answer 3: debug for now; a switch comes back after it is
// measured as a safe win on weak hardware). Not here: CHEAP CAR SHADOW, TREE IMPOSTORS and SCREEN BLUR
// (perf_fast_vehicle_shadows, perf_no_impostors, perf_no_fullscreen_blur) are read once at start, so they
// are launch arguments, not rows.
const ItemDef kDebugPerformanceItems[] = {
    Bool("PM_RxDbgPaceSwap",   "pace_at_swap",           "PACE AT PRESENTATION: "),
    // Patch_FenceSpinThrottle, live: the hook reads the cvar on every call, so the next fence poll
    // already sees a change made here. Whether it helps or hurts depends on the host CPU, not on the
    // game. It replaces the guest's fence-poll pause with YieldProcessor plus a SwitchToThread every
    // 16th call, and the guest reaches that poll about 271k times per frame. Measured on an 8C/16T with
    // three threads near 100%: 644k SwitchToThread a second costing 10 ms/s in total, because with cores
    // to spare each one returns without switching -- no gain, no loss. On a machine with fewer cores
    // those same calls switch for real and the poll thread pays microseconds each time, so ON can cost
    // far more than it saves. Leave it ON, turn it OFF if the game stutters.
    Bool("PM_RxDbgFenceSpin",  "fence_spin_throttle",    "FENCE SPIN YIELD: "),
    Bool("PM_RxDbgLimitSpin",  "frame_limit_spin",       "FPS LIMIT SPIN: "),
    Bool("PM_RxDbgPumpWake",   "pump_wake_on_demand",    "FRAME PACING WAKE: "),
    // CHECK: flipped on after the game made its surfaces, are they past the 2048 EDRAM tiles
    // Patch_EdramLimit lets through only while this is on (graphics.cpp)? A live ON may misbehave.
    Bool("PM_RxDbgSingleTile", "single_tile",            "SINGLE TILE: "),
    Str ("PM_RxDbgShadowBits", "perf_shadow_phase_bits", "SHADOW PHASES: ",
         kShadowBitVals, kShadowBitNames, Len(kShadowBitVals)),
    // The cvar is live; the prop parse it skips runs when a district loads.
    // CHECK: whether that parse runs again after a district change or only at start-up (render_phases.cpp).
    Bool("PM_RxDbgNoTrees",    "perf_no_trees",          "FOLIAGE: ", true, " (ON RELOAD)"),
    Dbl ("PM_RxDbgBreakFps",   "breaking_frame_rate_limit", "BREAK FPS FLOOR: ",
         kBreakFpsVals, Len(kBreakFpsVals), "%g", "", "STOCK"),
};

// RENDERING: the Plume renderer's debug views and logs, and the picture's shape.
const ItemDef kDebugRenderingItems[] = {
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Str("PM_RxDbgAoView",     "mcla_gpu_ao_view",       "AO VIEW: ",
                   kAoViewVals, kAoViewNames, Len(kAoViewVals))),
    NativeOnly(Dbl("PM_RxDbgAoHistory",  "mcla_gpu_ao_history",    "AO HISTORY: ",
                   kAoHistoryVals, Len(kAoHistoryVals), "%g")),
    NativeOnly(Dbl("PM_RxDbgAoFade",     "mcla_gpu_ao_fade_end",   "AO FADE END: ",
                   kAoFadeEndVals, Len(kAoFadeEndVals), "%g")),
    NativeOnly(Str("PM_RxDbgHdrView",    "mcla_gpu_hdr_layers_view", "HDR LAYERS VIEW: ",
                   kHdrViewVals, kHdrViewNames, Len(kHdrViewVals))),
    NativeOnly(Bool("PM_RxDbgMarkers",   "mcla_gpu_perf_markers",  "PERF MARKERS: ")),
    NativeOnly(Str("PM_RxDbgMarkerLvl",  "mcla_gpu_perf_markers_level", "PERF MARKERS LEVEL: ",
                   kMarkerLevelVals, kMarkerLevelNames, Len(kMarkerLevelVals))),
    NativeOnly(Dbl("PM_RxDbgGhostLog",   "mcla_gpu_ghost_log_interval", "GHOST LOG EVERY: ",
                   kGhostLogVals, Len(kGhostLogVals), "%g", "", "OFF")),
    NativeOnly(Dbl("PM_RxDbgMemLog",     "mcla_gpu_process_memory_log", "MEMORY LOG EVERY: ",
                   kMemoryLogVals, Len(kMemoryLogVals), "%g", "", "OFF")),
    // Read when frame generation is switched on, so a change shows the next time it is.
    NativeOnly(Bool("PM_RxDbgFgTearing", "mcla_gpu_frame_generation_tearing", "FRAME GEN TEARING: ",
                    false, " (ON NEXT USE)")),
#endif
    Str("PM_RxDbgAspect", "aspect_ratio", "ASPECT RATIO: ", kAspectVals, kAspectNames,
        Len(kAspectVals)),
    // The SDK's vsync on the Xenia renderer: it sets the guest's vblank interval (the game then paces
    // to it, about 30 % slower) but does not stop tearing, and the start-up flags set it again at every
    // start; a test switch, not a player setting.
    XeniaOnly(Bool("PM_RxDbgXeVsync", "vsync", "XENIA VSYNC: ")),
};

// CAMERA: the developer free camera, the free-look collision test and the camera diagnostics.
const ItemDef kDebugCameraItems[] = {
    Str ("PM_RxDbgFreecam",   "debug_cam",              "FREECAM: ",
         kFreecamVals, kFreecamNames, Len(kFreecamVals)),
    Dbl ("PM_RxDbgCamSpeed",  "debug_cam_speed",        "CAM SPEED: ", kCamSpeedVals,
         Len(kCamSpeedVals), "%g"),
    Dbl ("PM_RxDbgCamSens",   "debug_cam_sens",         "FREECAM SENS: ", kCamSensVals,
         Len(kCamSensVals), "%g"),
    Bool("PM_RxDbgCamMouse",  "debug_cam_mouse",        "FREECAM MOUSE: "),
    Bool("PM_RxDbgCamCollide", "cam_freelook_collide",  "CAMERA COLLISION: "),
    Bool("PM_RxDbgCamFlip",   "cam_freelook_cs_invert", "COLLISION FLIP: "),
    Bool("PM_RxDbgCamTrace",  "cam_trace",              "CAM TRACE: "),
    Bool("PM_RxDbgMenuCamLock", "menu_cam_lock",        "MENU CAM LOCK: "),
    Dbl ("PM_RxDbgMenuCamSlot", "menu_cam_slot",        "MENU CAM SLOT: ", kMenuCamSlots.v, 64,
         "%g"),
};

// TOOLS: the modder's texture tools, the panels, a leftover test button.
const ItemDef kDebugToolsItems[] = {
#if defined(LARECOMP_HAS_GPU)
    NativeOnly(Action("PM_RxDbgTexDump",   kActionDumpTextures)),
    NativeOnly(Action("PM_RxDbgTexReload", kActionReloadTextures)),
    NativeOnly(Bool("PM_RxDbgTexDumpAll",  "mcla_gpu_texture_dump", "DUMP ALL TEXTURES: ")),
#endif
    // The game's own TestMovie button on the pause screen (pause_menu.cpp Hook_EnablePMTeste).
    Bool("PM_RxDbgTestMovie", "show_test_movie", "TEST MOVIE BUTTON: "),
    Action("PM_RxDbgOpenF4",     kActionDebugOpenSettings),
    Action("PM_RxDbgTelemetry",  kActionDebugTelemetry),
    Bool("PM_RxDbgSquash",       "squash_stretch", "SQUASH AND STRETCH: "),
};

// LOGGING: the log switches that can be flipped while the game runs. The restart-only diagnostics are
// launch arguments (SETTINGS-REVIEW.md B.2). Each one writes more lines (some a lot) to the log.
const ItemDef kDebugLoggingItems[] = {
    Bool("PM_RxDbgUiColorLog",  "ui_color_log",          "UI COLOR LOG: "),
    Bool("PM_RxDbgMapMouseLog", "map_mouse_diag",        "MAP MOUSE DIAG: "),
    Bool("PM_RxDbgSlipLog",     "trigger_slip_log",      "TRIGGER SLIP LOG: "),
    Bool("PM_RxDbgWheelTrace",  "wheel_trace",           "WHEEL TRACE: "),
    Bool("PM_RxDbgFfbTrace",    "wheel_ffb_trace",       "WHEEL FFB TRACE: "),
    Bool("PM_RxDbgRearDiag",    "rearview_diag",         "REARVIEW DIAG: "),
    Bool("PM_RxDbgMusicLog",    "music_bank_log",        "MUSIC BANK LOG: "),
    Bool("PM_RxDbgLzxStats",    "lzx_stats",             "LZX STATS: "),
    Bool("PM_RxDbgWarpTrace",   "warp_transition_trace", "WARP TRACE: "),
    Bool("PM_RxDbgCutProbe",    "cutscene_probe_log",    "CUTSCENE PROBE LOG: "),
    Bool("PM_RxDbgPhotoTrace",  "photo_ui_trace",        "PHOTO UI TRACE: "),
    Bool("PM_RxDbgOnlineDiag",  "online_diag",           "ONLINE DIAG: "),
    Bool("PM_RxDbgLbDiag",      "leaderboards_diag",     "LEADERBOARDS DIAG: "),
    Bool("PM_RxDbgRmrDiag",     "rmr_diag",              "RMR DIAG: "),
    Bool("PM_RxDbgLowCpuProbe", "lowcpu_probe",          "LOW CPU PROBE: "),
};

// INFO: what differs from the defaults, and the way back.
const ItemDef kDebugInfoItems[] = {
    Action("PM_RxDbgShowChanged", kActionDebugShowChanged),
    Action("PM_RxDbgCopyArgs",    kActionDebugCopyArgs),
    Action("PM_RxDbgInfoReset",   kActionDebugReset),
};

template <std::size_t N>
const ItemDef* Rows(const ItemDef (&table)[N], int& count) {
    count = int(N);
    return table;
}

// ── The actions ────────────────────────────────────────────────────────
//
// All of this runs on the guest thread (the menu's), so the state below needs no lock.

using Clock = std::chrono::steady_clock;

// The short answer an action row shows after a click ("COPIED (3)"), for a moment.
constexpr std::chrono::milliseconds kAnswerTime{2500};
struct RowAnswer {
    ActionId id = ActionId(0);
    std::string text;
    Clock::time_point until{};
};
RowAnswer g_answer;

void Answer(ActionId id, std::string text) {
    g_answer.id = id;
    g_answer.text = std::move(text);
    g_answer.until = Clock::now() + kAnswerTime;
}

// How many debug settings are not at their default. The row asks on every draw, and the count walks
// the whole cvar registry, so it is taken at most twice a second.
// CHECK: the walk is cheap enough (about 600 cvars) that this throttle is all it needs.
constexpr std::chrono::milliseconds kCountEvery{500};
std::size_t g_changed_count = 0;
Clock::time_point g_changed_at{};
bool g_changed_valid = false;

std::size_t ChangedCount() {
    const Clock::time_point now = Clock::now();
    if (!g_changed_valid || now - g_changed_at >= kCountEvery) {
        g_changed_count = larecomp::NonDefaultDebugCvars().size();
        g_changed_at = now;
        g_changed_valid = true;
    }
    return g_changed_count;
}

void ForgetChangedCount() { g_changed_valid = false; }

// The key a bind is on, for the label: "F4" unless it was rebound. Only A-Z, 0-9, space and '+'.
std::string BindKeyLabel(const char* bind_cvar, const char* fallback) {
    const std::string key = rex::cvar::GetFlagByName(bind_cvar);
    std::string out;
    for (const char c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
            out += char(std::toupper(u));
        else if (c == '+' || c == ' ')
            out += c;
    }
    return out.empty() ? std::string(fallback) : out;
}

// The text of one `--name=value` argument: the value is quoted when it has anything but plain
// characters in it.
std::string QuoteArgument(const std::string& value) {
    const bool plain = std::all_of(value.begin(), value.end(), [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || c == '_' || c == '.' || c == '-' || c == '+' || c == ':' ||
               c == '/' || c == '\\';
    });
    if (plain) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// `--debug_menu` then `--name=value` for every debug setting that is not at its default, whichever way
// it got there: the line that brings a run back to this state (a bug report, a shortcut). A setting
// whose value is empty is left out and counted in `skipped`: the game drops its whole command line for
// an empty `--name=` (BACKLOG UW note).
std::string LaunchArguments(int& used, int& skipped) {
    std::string line = "--debug_menu";
    used = 0;
    skipped = 0;
    for (const larecomp::DebugCvarChange& change : larecomp::NonDefaultDebugCvars()) {
        if (change.value.empty()) {
            ++skipped;
            continue;
        }
        line += " --" + change.name + "=" + QuoteArgument(change.value);
        ++used;
    }
    return line;
}

// CHECK: Windows only, and from the guest thread (the clipboard has no owner window here, so it is
// handed the data at once). Elsewhere the line is in the log only.
bool CopyToClipboard(const std::string& text) {
#if defined(_WIN32)
    const int wide_count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wide_count <= 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, SIZE_T(wide_count) * sizeof(wchar_t));
    if (!mem) return false;
    void* locked = GlobalLock(mem);
    if (!locked) {
        GlobalFree(mem);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t*>(locked), wide_count);
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);  // the clipboard owns the memory only once it took it
    return ok;
#else
    (void)text;
    return false;
#endif
}

void ShowChanged() {
    const std::vector<larecomp::DebugCvarChange> changes = larecomp::NonDefaultDebugCvars();
    if (changes.empty()) {
        REXLOG_INFO("[settings] no debug setting is different from its default");
    } else {
        REXLOG_INFO("[settings] {} debug setting(s) are not at their default:", changes.size());
        for (const larecomp::DebugCvarChange& change : changes) {
            REXLOG_INFO("[settings] {} = {} ({})", change.name, change.value,
                        larecomp::CvarSourceName(change.source));
        }
    }
    ForgetChangedCount();
    Answer(kActionDebugShowChanged, "IN THE LOG (" + std::to_string(changes.size()) + ")");
}

void CopyArguments() {
    int used = 0;
    int skipped = 0;
    const std::string line = LaunchArguments(used, skipped);
    REXLOG_INFO("[debug] launch arguments for this state: {}", line);
    if (skipped > 0) {
        REXLOG_WARN("[debug] {} setting(s) with an empty value are left out of the arguments: "
                    "an empty --name= makes the game drop its whole command line",
                    skipped);
    }
    const bool ok = CopyToClipboard(line);
    if (!ok) REXLOG_WARN("[debug] the launch arguments could not be put on the clipboard");
    Answer(kActionDebugCopyArgs,
           ok ? "COPIED (" + std::to_string(used) + ")" : std::string("NOT COPIED, SEE THE LOG"));
}

// Every debug setting that is not at its default goes back to it, whichever way it got there (this
// session, the command line, larecomp.toml): this session only, the file is not touched. Left alone:
// debug_menu itself (NonDefaultDebugCvars does not list it) and any setting that is only read at start,
// which a live write would not change (or would change under the code that read it).
// The value is written through SetFlagByName so the cvar's change callback runs (aspect_ratio, hud_width
// follow their cvar through one; ResetToDefault, which writes the storage directly, would leave them on
// the old value), then ResetToDefault puts the cvar's source back to "default".
void ResetDebugSettings() {
    int reset = 0;
    int skipped = 0;
    for (const larecomp::DebugCvarChange& change : larecomp::NonDefaultDebugCvars()) {
        if (change.name == "debug_menu") continue;
        const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(change.name);
        if (!info) continue;
        if (info->lifecycle != rex::cvar::Lifecycle::kHotReload) {
            ++skipped;
            REXLOG_INFO("[settings] {} is only read at start: left at {} ({})", change.name,
                        change.value, larecomp::CvarSourceName(change.source));
            continue;
        }
        const std::string default_value = info->default_value;  // the entry may move once we write
        REXLOG_INFO("[settings] {} = {} -> {} (reset)", change.name, change.value, default_value);
        rex::cvar::SetFlagByName(change.name, default_value);
        rex::cvar::ResetToDefault(change.name);
        ++reset;
    }
    REXLOG_INFO("[debug] {} debug setting(s) reset to their default, {} left (read at start only)",
                reset, skipped);
    ForgetChangedCount();
    Answer(kActionDebugReset, "RESET (" + std::to_string(reset) + ")");
}

// Presses the key a bind is on, on the UI thread, which is where the keys of the window arrive: the
// panels are ImGui dialogs and are made there. The bind's own callback does the rest, so a key the
// player rebound is followed.
// CHECK: the dialog opens over the pause menu (the menu keeps the pad until it is closed).
void PressBind(const char* bind_cvar, const char* fallback_key) {
    std::string key = rex::cvar::GetFlagByName(bind_cvar);
    if (key.empty()) key = fallback_key;
    const rex::ui::VirtualKey virtual_key = rex::ui::ParseVirtualKey(key);
    if (virtual_key == rex::ui::VirtualKey::kNone) {
        REXLOG_WARN("[debug] {} is on '{}', which is no key the game knows", bind_cvar, key);
        return;
    }
    auto* runtime = rex::Runtime::instance();
    rex::ui::Window* window = runtime ? runtime->display_window() : nullptr;
    if (!window) {
        REXLOG_WARN("[debug] {} cannot be pressed: the game has no window yet", bind_cvar);
        return;
    }
    window->app_context().CallInUIThread([virtual_key] {
        rex::ui::KeyEvent event(nullptr, virtual_key, /*repeat_count=*/1, /*prev_state=*/false,
                                /*shift=*/false, /*ctrl=*/false, /*alt=*/false, /*super=*/false);
        rex::ui::ProcessKeyEvent(event);
    });
}

}  // namespace

const ItemDef* DebugHubRows(int& count)         { return Rows(kDebugHubItems, count); }
const ItemDef* DebugFixesRows(int& count)       { return Rows(kDebugFixesItems, count); }
const ItemDef* DebugPerformanceRows(int& count) { return Rows(kDebugPerformanceItems, count); }
const ItemDef* DebugRenderingRows(int& count)   { return Rows(kDebugRenderingItems, count); }
const ItemDef* DebugCameraRows(int& count)      { return Rows(kDebugCameraItems, count); }
const ItemDef* DebugToolsRows(int& count)       { return Rows(kDebugToolsItems, count); }
const ItemDef* DebugLoggingRows(int& count)     { return Rows(kDebugLoggingItems, count); }
const ItemDef* DebugInfoRows(int& count)        { return Rows(kDebugInfoItems, count); }

std::string DebugActionLabel(ActionId id) {
    if (g_answer.id == id && Clock::now() < g_answer.until) return g_answer.text;
    switch (id) {
    case kActionDebugShowChanged:
        return "SHOW CHANGED SETTINGS (" + std::to_string(ChangedCount()) + ")";
    case kActionDebugCopyArgs:
        return "COPY LAUNCH ARGUMENTS";
    case kActionDebugReset:
        return "RESET DEBUG SETTINGS";
    case kActionDebugOpenSettings:
        return "OPEN SETTINGS PANEL (" + BindKeyLabel("bind_mcla_settings", "F4") + ")";
    case kActionDebugTelemetry:
        return "TELEMETRY CARD (" + BindKeyLabel("bind_mcla_telemetry", "F3") + ")";
    default:
        break;
    }
    return "";
}

void DebugActionRun(ActionId id) {
    switch (id) {
    case kActionDebugShowChanged:    ShowChanged(); break;
    case kActionDebugCopyArgs:       CopyArguments(); break;
    case kActionDebugReset:          ResetDebugSettings(); break;
    case kActionDebugOpenSettings:   PressBind("bind_mcla_settings", "F4"); break;
    case kActionDebugTelemetry:      PressBind("bind_mcla_telemetry", "F3"); break;
    default:                         break;
    }
}

}  // namespace mcla_menu
