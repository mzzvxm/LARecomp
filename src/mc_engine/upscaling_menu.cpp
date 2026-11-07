// The pause menu's UPSCALING rows. See upscaling_menu.h.

#include "upscaling_menu.h"

#include <string>

#include <rex/cvar.h>

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

namespace {

#if defined(LARECOMP_HAS_GPU)

using mcla::gpu::Upscaler;
using mcla_menu::Dbl;
using mcla_menu::ItemDef;
using mcla_menu::RowDynamics;
using mcla_menu::Str;
using mcla_menu::WithDynamics;

// The player's choice as saved, and the choice when this PC can run it (the one that does the work).
Upscaler Chosen() {
    const std::string value = rex::cvar::GetFlagByName("mcla_gpu_upscaler");
    if (value == "fsr31") return Upscaler::kFsr31;
    if (value == "fsr4") return Upscaler::kFsr4;
    if (value == "dlss") return Upscaler::kDlss;
    return Upscaler::kOff;
}

// DLSS is the saved choice but this game can run it only from the next start (a Vulkan game started
// without Streamline). The renderer keeps what it runs until then (gpu.h UpscalerAvailability::kept).
bool DlssWaitsForRestart() {
    if (Chosen() != Upscaler::kDlss) return false;
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    return a.dlss_after_restart && !a.dlss;
}

// The upscaler that does the work: the choice when this PC can run it; while DLSS waits for a
// restart, the one the renderer keeps (FSR 3.1 or 4 as it was running, or off), which is what holds
// FXAA and SMAA off and what the sharpness row and the greyed mode row belong to.
Upscaler Active() {
    const Upscaler chosen = Chosen();
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if ((chosen == Upscaler::kFsr31 && a.fsr31) || (chosen == Upscaler::kFsr4 && a.fsr4) ||
        (chosen == Upscaler::kDlss && a.dlss))
        return chosen;
    if (chosen == Upscaler::kDlss && a.dlss_after_restart) return a.kept;
    return Upscaler::kOff;
}

// The mode rows are live only for an upscaler that is the choice itself: with DLSS waiting for a
// restart, the running upscaler's mode row shows what it runs and is greyed (the renderer keeps
// that mode until the restart, whatever the row would be set to).
bool Fsr31Chosen() { return Active() == Upscaler::kFsr31 && !DlssWaitsForRestart(); }
bool Fsr4Chosen() { return Active() == Upscaler::kFsr4 && !DlssWaitsForRestart(); }
bool DlssChosen() { return Active() == Upscaler::kDlss; }
// FSR's sharpness is not a mode: it applies to the FSR that runs, so it stays live there.
bool FsrChosen() { return Active() == Upscaler::kFsr31 || Active() == Upscaler::kFsr4; }

// The mode rows: grey while their upscaler is not the one in use; while it is, a mode picked now
// is live: the scene is drawn at the new size from a frame boundary, once the upscaler has made
// what it needs for it (the old mode works until then; the UPSCALING row says "(WAIT)"). The one
// exception: DLSS chosen in a game that must restart for it. The upscaler that runs keeps its mode
// (its row shows it, grey, "(UNTIL DLSS)": not "RESTART", which the "Restart now?" prompt reads as
// "this setting needs a start") and the DLSS rows are grey with "(RESTART)".
const char* Fsr31ModeSuffix() {
    switch (Active()) {
    case Upscaler::kOff:   return " (UPSCALING OFF)";
    case Upscaler::kFsr31: return DlssWaitsForRestart() ? " (UNTIL DLSS)" : "";
    default:               return " (NOT IN USE)";
    }
}
const char* Fsr4ModeSuffix() {
    switch (Active()) {
    case Upscaler::kOff:   return " (UPSCALING OFF)";
    case Upscaler::kFsr4:  return DlssWaitsForRestart() ? " (UNTIL DLSS)" : "";
    default:               return " (NOT IN USE)";
    }
}
// DLSS without its files (an RTX card): the row is listed and grey, "(SETUP)" as the UPSCALING
// row's value says; on Vulkan a game started without DLSS cannot switch to it (the device is made
// with Streamline's help only when DLSS is the saved choice at the start): grey, "(RESTART)".
const char* DlssModeSuffix() {
    const Upscaler active = Active();
    if (active == Upscaler::kDlss) return "";
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if (a.dlss_needs_setup && !a.dlss) return " (SETUP)";
    if (a.dlss_after_restart && !a.dlss) return " (RESTART)";
    // A page that was built while DLSS could run and DLSS has been turned off since (it failed): its
    // rows stay as they are until the page is left, and are made again without DLSS the next time it
    // opens (UpscalingMenuRelayout).
    if (a.dlss_probed && !a.dlss) return " (UNAVAILABLE)";
    return active == Upscaler::kOff ? " (UPSCALING OFF)" : " (NOT IN USE)";
}
// FSR's sharpening (RCAS) only; DLSS has none. One suffix for both reasons the row is grey (no
// upscaler, or DLSS): "(UPSCALING OFF)" made it the widest label of DISPLAY (pause_menu.cpp,
// "Page width").
const char* SharpnessSuffix() {
    return FsrChosen() ? "" : " (FSR ONLY)";
}

// NATIVE AA and MSAA while an upscaler runs: grey, and the suffix names the upscaler that does the
// anti-aliasing instead ("MSAA (FSR 3.1 ON)").
const char* HandledSuffix() {
    switch (Active()) {
    case Upscaler::kFsr31: return " (FSR 3.1 ON)";
    case Upscaler::kFsr4:  return " (FSR 4 ON)";
    case Upscaler::kDlss:  return " (DLSS ON)";
    default:               return "";
    }
}
bool AaFree() { return Active() == Upscaler::kOff; }

// The MSAA row. MSAA on against off is a start-up switch of the game (gpu.h GuestMsaa): a game
// started with the upscaler on runs without it (the row names the upscaler), one started with MSAA
// keeps it until the next start, which runs without it (the row says "(RESTART)"). With no upscaler
// the row is the player's own choice: 2X, 4X and 8X among themselves are live in a game that runs
// with MSAA (the renderer changes the host's samples between two frames once the pipelines for them
// are made, msaa_level.h: the row says "(WAIT)" meanwhile); OFF against one of them applies at the
// next start, so the row says "(RESTART)" when the saved choice and the running game disagree about
// on and off (which is what the "Restart now?" prompt reads), and nothing otherwise.
// CHECK: in a game started with MSAA 2X, the values 2X, 4X and 8X carry no suffix (and "(WAIT)" for
// the seconds a change is being prepared) and "Restart now?" is not offered when the menu is left;
// OFF says "(RESTART)" and offers it, and going back to any of the three drops the suffix; in a game
// started with MSAA OFF, 2X, 4X and 8X say "(RESTART)" and OFF says nothing.
const char* MsaaSuffix() {
    const mcla::gpu::GuestMsaa state = mcla::gpu::GetGuestMsaa();
    const bool running = state == mcla::gpu::GuestMsaa::kOn;
    if (Active() != Upscaler::kOff) return running ? " (RESTART)" : HandledSuffix();
    const bool saved_off = rex::cvar::GetFlagByName("mcla_gpu_msaa") == "off";
    const bool running_off = state == mcla::gpu::GuestMsaa::kOffChosen ||
                             state == mcla::gpu::GuestMsaa::kOffForUpscaler;
    if ((running && saved_off) || (running_off && !saved_off)) return " (RESTART)";
    return mcla::gpu::MsaaSwitching() ? " (WAIT)" : "";
}

// The vendors' modes and the share of the output the scene is drawn at, per direction (gpu.h
// ModeInfo): AMD's for FSR 3.1 and FSR 4, NVIDIA's for DLSS. A mode picked while the game runs is
// made live at a frame boundary (temporal.h, "Changing the path").
//
// Width: these rows are on DISPLAY, whose widest value is "PERFORMANCE (50%)", so each of their
// labels, in every state, stays within "RENDER RES (SET BY FSR1)" (pause_menu.cpp, "Page width"):
// the mode rows carry no suffix while they are live, and the UPSCALING row's are short ("(WAIT)"
// while a change is being made; "(RESTART)", like the other rows that wait for a start and what the
// "Restart now?" prompt looks for, while DLSS is chosen in a Vulkan game started without it). The
// mode rows are labelled with the technology alone ("FSR 3.1: ", "FSR 4: ", "DLSS: ": several can be
// listed on one PC, so the label still names it), and ULTRA PERFORMANCE is "ULTRA PERF.". The cvars
// store the full names (ultra_performance).
// CHECK: the "(N/A)" values of the DLSS mode row (kDlssModeNa) keep the page inside its width: each
// has the characters of its "(NN%)" value, "N/A" a little wider than two digits, and the widest,
// "PERFORMANCE (N/A)" and "ULTRA PERF. (N/A)", are 17 characters like "PERFORMANCE (50%)". Open
// DISPLAY at 1280x720 with DLSS on and look at the right edge, with every row on its widest value.
const char* const kModeVals[]  = {"native_aa", "quality", "balanced", "performance",
                                  "ultra_performance"};
const char* const kModeNames[] = {"NATIVE AA (100%)", "QUALITY (67%)", "BALANCED (59%)",
                                  "PERFORMANCE (50%)", "ULTRA PERF. (33%)"};
const char* const kDlssModeVals[]  = {"dlaa", "quality", "balanced", "performance",
                                      "ultra_performance"};
const char* const kDlssModeNames[] = {"DLAA (100%)", "QUALITY (67%)", "BALANCED (58%)",
                                      "PERFORMANCE (50%)", "ULTRA PERF. (33%)"};
// A DLSS mode the card cannot take at the output the renderer works at (gpu.h
// UpscalerAvailability::dlss_modes_refused: Ultra Performance at 1280x720 or 2560x1440) says "(N/A)"
// in place of its share. Picking it saves the choice and changes nothing on screen: the mode that
// runs goes on, as RENDER RES keeps the scale that runs when "TOO BIG (2.0X)" is picked.
const char* const kDlssModeNa[] = {"DLAA (N/A)", "QUALITY (N/A)", "BALANCED (N/A)",
                                   "PERFORMANCE (N/A)", "ULTRA PERF. (N/A)"};
constexpr int kModeCount = 5;
std::string DlssModeLabel(int i) {
    if (i < 0 || i >= kModeCount) return "";
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    return a.DlssModeRefused(mcla::gpu::UpscaleMode(i)) ? kDlssModeNa[i] : kDlssModeNames[i];
}
// FSR's sharpening (RCAS), 0 to 1.
constexpr double kSharpVals[] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};

// The UPSCALING row: " (RESTART)" while DLSS is the saved choice but this game cannot run it until
// it is started again (a Vulkan game started without DLSS: the device is made through Streamline
// only when DLSS is chosen at the start). The choice is saved and applies at the next start, the
// renderer goes on with what it runs meanwhile (the upscaler and its mode as they were, or none),
// and leaving the menu offers "Restart now?" (the prompt reads this suffix). Otherwise " (WAIT)"
// while the settings ask for another upscaler or mode than the one running and its context is
// still being made (DLSS takes several seconds; the old one goes on).
// CHECK: the prompt finds this row through the suffix (pause_menu.cpp RowSaysRestart): in a Vulkan
// game started with FSR or no upscaler on an RTX PC, picking DLSS, then leaving the menu, logs
// "[pause-menu] a setting that needs a restart changed: offered RESTART NOW".
const char* UpscalerRowSuffix() {
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if (DlssWaitsForRestart()) return " (RESTART)";
    return a.switching ? " (WAIT)" : "";
}

constexpr RowDynamics kUpscalerDynamics = {.suffix = UpscalerRowSuffix};
constexpr RowDynamics kFsr31ModeDynamics = {.enabled = Fsr31Chosen, .suffix = Fsr31ModeSuffix};
constexpr RowDynamics kFsr4ModeDynamics  = {.enabled = Fsr4Chosen, .suffix = Fsr4ModeSuffix};
constexpr RowDynamics kDlssModeDynamics  = {
    .str_label = DlssModeLabel, .enabled = DlssChosen, .suffix = DlssModeSuffix};
constexpr RowDynamics kSharpnessDynamics = {.enabled = FsrChosen, .suffix = SharpnessSuffix};

// The rows that can be listed depend on what the probes found (FSR 3.1, FSR 4; DLSS: runs, needs
// setup, or after a restart), which is final a moment after start, and on a backend that fails
// later (DLSS turned off for the session), so one table of rows is built the first time each
// combination is asked for and kept: the menu holds pointers into it.
// NativeOnly: the upscalers are the Plume renderer's.
struct RowSet {
    bool built = false;
    const char* upscaler_vals[4] = {};
    const char* upscaler_names[4] = {};
    int upscaler_count = 0;
    ItemDef rows[5] = {};
    int count = 0;
};
RowSet g_row_sets[16];

const RowSet& RowSetFor(bool fsr31, bool fsr4, int dlss_state) {
    // dlss_state: 0 not listed, 1 runs, 2 needs setup, 3 after restart.
    RowSet& set = g_row_sets[(fsr31 ? 1 : 0) | (fsr4 ? 2 : 0) | (dlss_state << 2)];
    if (set.built) {
        return set;
    }
    set.built = true;
    int n = 0;
    set.upscaler_vals[n] = "off";
    set.upscaler_names[n++] = "OFF";
    if (fsr31) {
        set.upscaler_vals[n] = "fsr31";
        set.upscaler_names[n++] = "FSR 3.1";
    }
    if (fsr4) {
        set.upscaler_vals[n] = "fsr4";
        set.upscaler_names[n++] = "FSR 4";
    }
    if (dlss_state != 0) {
        set.upscaler_vals[n] = "dlss";
        // State 3 (a start is needed) is plain "DLSS": the row's label says (RESTART) while it is the
        // choice (UpscalerRowSuffix), which is where the "Restart now?" prompt reads it.
        set.upscaler_names[n++] = dlss_state == 2 ? "DLSS (SETUP)" : "DLSS";
    }
    set.upscaler_count = n;
    if (n == 1) {
        return set;  // nothing can run: no rows
    }
    int r = 0;
    set.rows[r++] = mcla_menu::NativeOnly(
        WithDynamics(Str("PM_RxGpuUpsc", "mcla_gpu_upscaler", "UPSCALING: ", set.upscaler_vals,
                         set.upscaler_names, set.upscaler_count),
                     &kUpscalerDynamics));
    if (fsr31) {
        set.rows[r++] = mcla_menu::NativeOnly(
            WithDynamics(Str("PM_RxGpuFsr31M", "mcla_gpu_fsr31_mode", "FSR 3.1: ", kModeVals,
                             kModeNames, kModeCount),
                         &kFsr31ModeDynamics));
    }
    if (fsr4) {
        set.rows[r++] = mcla_menu::NativeOnly(
            WithDynamics(Str("PM_RxGpuFsr4M", "mcla_gpu_fsr4_mode", "FSR 4: ", kModeVals,
                             kModeNames, kModeCount),
                         &kFsr4ModeDynamics));
    }
    if (dlss_state != 0) {
        set.rows[r++] = mcla_menu::NativeOnly(
            WithDynamics(Str("PM_RxGpuDlssM", "mcla_gpu_dlss_mode", "DLSS: ", kDlssModeVals,
                             kDlssModeNames, kModeCount),
                         &kDlssModeDynamics));
    }
    if (fsr31 || fsr4) {
        set.rows[r++] = mcla_menu::NativeOnly(
            WithDynamics(Dbl("PM_RxGpuUpscSharp", "mcla_gpu_upscaler_sharpness", "SHARPNESS: ",
                             kSharpVals, 6, "%g", "", "OFF"),
                         &kSharpnessDynamics));
    }
    set.count = r;
    return set;
}

// Which rows availability lists now: FSR 3.1, FSR 4 and DLSS's state, as RowSetFor takes them.
int LiveLayoutKey() {
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const int dlss_state = a.dlss ? 1 : a.dlss_needs_setup ? 2 : a.dlss_after_restart ? 3 : 0;
    return (a.fsr31 ? 1 : 0) | (a.fsr4 ? 2 : 0) | (dlss_state << 2);
}

// The layout the page's rows are built from, kept until the page is built again (UpscalingMenuRelayout).
// The pause menu builds the guest rows of a page once and indexes them by position (pause_menu.cpp
// BuildNativeRows), so the table behind UpscalingMenuRows must not change while a built page holds
// it: an upscaler that fails after the page was built would shift every row below its own. -1: none yet.
// Guest thread only, like the menu.
int g_latched_key = -1;

#endif  // LARECOMP_HAS_GPU

}  // namespace

bool UpscalingMenuRelayout(bool apply) {
#if defined(LARECOMP_HAS_GPU)
    const int key = LiveLayoutKey();
    if (key == g_latched_key) return false;
    if (apply) g_latched_key = key;
    return true;
#else
    (void)apply;
    return false;
#endif
}

const mcla_menu::ItemDef* UpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    if (g_latched_key < 0) UpscalingMenuRelayout(true);  // the first ask: the layout as it is now
    const RowSet& set = RowSetFor((g_latched_key & 1) != 0, (g_latched_key & 2) != 0, g_latched_key >> 2);
    if (set.count > 0) {
        count = set.count;
        return set.rows;
    }
#endif
    count = 0;
    return nullptr;
}

namespace mcla_menu {

#if defined(LARECOMP_HAS_GPU)
extern const RowDynamics kNativeAaDynamics = {.enabled = AaFree, .suffix = HandledSuffix};
extern const RowDynamics kMsaaDynamics = {.enabled = AaFree, .suffix = MsaaSuffix};
#else
extern const RowDynamics kNativeAaDynamics = {};
extern const RowDynamics kMsaaDynamics = {};
#endif

}  // namespace mcla_menu
