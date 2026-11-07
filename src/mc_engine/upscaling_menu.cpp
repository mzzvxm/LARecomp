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

Upscaler Active() {
    const Upscaler chosen = Chosen();
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if ((chosen == Upscaler::kFsr31 && a.fsr31) || (chosen == Upscaler::kFsr4 && a.fsr4) ||
        (chosen == Upscaler::kDlss && a.dlss))
        return chosen;
    return Upscaler::kOff;
}

bool Fsr31Chosen() { return Active() == Upscaler::kFsr31; }
bool Fsr4Chosen() { return Active() == Upscaler::kFsr4; }
bool DlssChosen() { return Active() == Upscaler::kDlss; }
bool FsrChosen() { return Active() == Upscaler::kFsr31 || Active() == Upscaler::kFsr4; }

// The mode rows: grey while their upscaler is not the one in use; while it is, a mode picked now
// is live: the scene is drawn at the new size from a frame boundary, once the upscaler has made
// what it needs for it (the old mode works until then; the UPSCALING row says "(WAIT)").
const char* Fsr31ModeSuffix() {
    switch (Active()) {
    case Upscaler::kOff:   return " (UPSCALING OFF)";
    case Upscaler::kFsr31: return "";
    default:               return " (NOT IN USE)";
    }
}
const char* Fsr4ModeSuffix() {
    switch (Active()) {
    case Upscaler::kOff:   return " (UPSCALING OFF)";
    case Upscaler::kFsr4:  return "";
    default:               return " (NOT IN USE)";
    }
}
// DLSS without its files (an RTX card): the row is listed and grey, "needs setup"; on Vulkan the
// choice only takes effect from the next start (the device is made with Streamline's help).
const char* DlssModeSuffix() {
    const Upscaler active = Active();
    if (active == Upscaler::kDlss) return "";
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if (a.dlss_needs_setup && !a.dlss) return " (NEEDS SETUP)";
    if (a.dlss_after_restart && !a.dlss) return " (AFTER RESTART)";
    return active == Upscaler::kOff ? " (UPSCALING OFF)" : " (NOT IN USE)";
}
// FSR's sharpening (RCAS) only; DLSS has none.
const char* SharpnessSuffix() {
    switch (Active()) {
    case Upscaler::kOff:  return " (UPSCALING OFF)";
    case Upscaler::kDlss: return " (NOT FOR DLSS)";
    default:              return "";
    }
}

const char* HandledSuffix() {
    switch (Active()) {
    case Upscaler::kFsr31: return " (HANDLED BY FSR 3.1)";
    case Upscaler::kFsr4:  return " (HANDLED BY FSR 4)";
    case Upscaler::kDlss:  return " (HANDLED BY DLSS)";
    default:               return "";
    }
}
bool AaFree() { return Active() == Upscaler::kOff; }

// The MSAA row. MSAA is a start-up switch of the game (gpu.h GuestMsaa): a game started with the
// upscaler on runs without it ("handled by"), one started with MSAA keeps it until the next start,
// which runs without it. With no upscaler the row is the player's own choice, which applies at the
// next start (as it always did).
const char* MsaaSuffix() {
    const bool running = mcla::gpu::GetGuestMsaa() == mcla::gpu::GuestMsaa::kOn;
    switch (Active()) {
    case Upscaler::kFsr31: return running ? " (FSR 3.1 AFTER RESTART)" : " (HANDLED BY FSR 3.1)";
    case Upscaler::kFsr4:  return running ? " (FSR 4 AFTER RESTART)"   : " (HANDLED BY FSR 4)";
    case Upscaler::kDlss:  return running ? " (DLSS AFTER RESTART)"    : " (HANDLED BY DLSS)";
    default:               return " (RESTART)";
    }
}

// The vendors' modes and the share of the output the scene is drawn at, per direction (gpu.h
// ModeInfo): AMD's for FSR 3.1 and FSR 4, NVIDIA's for DLSS. A mode picked while the game runs is
// made live at a frame boundary (temporal.h, "Changing the path").
//
// Row width: a row's label and its value share one line of the menu, and the widest line of the
// rest of the pause menu is 26 characters ("RENDER RES: SCREEN (1080P)"). The mode rows' labels
// are the technology alone ("FSR 3.1: ", "FSR 4: ", "DLSS: ": several can be listed on one PC, so
// the label still has to name it) and the longest value is 17 characters ("PERFORMANCE (50%)",
// "ULTRA PERF. (33%)"): 9 + 17 = 26 at most, so those rows carry no suffix while they are live. The
// UPSCALING row's longest value is 14 ("DLSS (RESTART)"; "DLSS (SETUP)" is 12): 11 + 14 = 25; while
// a change is being made it says " (WAIT)" (7 characters), which only the shorter values (the
// longest of them "FSR 3.1", 7) can show: 11 + 7 + 7 = 25. The cvars store the full names
// (ultra_performance).
const char* const kModeVals[]  = {"native_aa", "quality", "balanced", "performance",
                                  "ultra_performance"};
const char* const kModeNames[] = {"NATIVE AA (100%)", "QUALITY (67%)", "BALANCED (59%)",
                                  "PERFORMANCE (50%)", "ULTRA PERF. (33%)"};
const char* const kDlssModeVals[]  = {"dlaa", "quality", "balanced", "performance",
                                      "ultra_performance"};
const char* const kDlssModeNames[] = {"DLAA (100%)", "QUALITY (67%)", "BALANCED (58%)",
                                      "PERFORMANCE (50%)", "ULTRA PERF. (33%)"};
constexpr int kModeCount = 5;
// FSR's sharpening (RCAS), 0 to 1.
constexpr double kSharpVals[] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};

// The UPSCALING row: " (WAIT)" while the settings ask for another upscaler or mode than the one
// running and its context is still being made (DLSS takes several seconds; the old one goes on).
const char* UpscalerRowSuffix() {
    return mcla::gpu::GetUpscalerAvailability().switching ? " (WAIT)" : "";
}

constexpr RowDynamics kUpscalerDynamics = {.suffix = UpscalerRowSuffix};
constexpr RowDynamics kFsr31ModeDynamics = {.enabled = Fsr31Chosen, .suffix = Fsr31ModeSuffix};
constexpr RowDynamics kFsr4ModeDynamics  = {.enabled = Fsr4Chosen, .suffix = Fsr4ModeSuffix};
constexpr RowDynamics kDlssModeDynamics  = {.enabled = DlssChosen, .suffix = DlssModeSuffix};
constexpr RowDynamics kSharpnessDynamics = {.enabled = FsrChosen, .suffix = SharpnessSuffix};

// The rows that can be listed depend on what the probes found (FSR 3.1, FSR 4; DLSS: runs, needs
// setup, or after a restart), which is final a moment after start, so one table of rows is built
// the first time each combination is asked for and kept: the menu holds pointers into it.
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
        set.upscaler_names[n++] = dlss_state == 1 ? "DLSS"
                                  : dlss_state == 2 ? "DLSS (SETUP)"
                                                    : "DLSS (RESTART)";
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

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* UpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const int dlss_state = a.dlss ? 1 : a.dlss_needs_setup ? 2 : a.dlss_after_restart ? 3 : 0;
    const RowSet& set = RowSetFor(a.fsr31, a.fsr4, dlss_state);
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
