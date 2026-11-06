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
    return Upscaler::kOff;
}

Upscaler Active() {
    const Upscaler chosen = Chosen();
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if ((chosen == Upscaler::kFsr31 && a.fsr31) || (chosen == Upscaler::kFsr4 && a.fsr4))
        return chosen;
    return Upscaler::kOff;
}

bool Fsr31Chosen() { return Active() == Upscaler::kFsr31; }
bool Fsr4Chosen() { return Active() == Upscaler::kFsr4; }
bool AnyChosen() { return Active() != Upscaler::kOff; }

const char* NotInUse() { return Active() == Upscaler::kOff ? " (UPSCALING OFF)" : " (NOT IN USE)"; }
const char* SharpnessSuffix() { return AnyChosen() ? "" : " (UPSCALING OFF)"; }

const char* HandledSuffix() {
    switch (Active()) {
    case Upscaler::kFsr31: return " (HANDLED BY FSR 3.1)";
    case Upscaler::kFsr4:  return " (HANDLED BY FSR 4)";
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
    default:               return " (RESTART)";
    }
}

// What the vendors call the only mode of this step, and its share of the output.
const char* const kModeVals[]  = {"native_aa"};
const char* const kModeNames[] = {"NATIVE AA (100%)"};
// FSR's sharpening (RCAS), 0 to 1.
constexpr double kSharpVals[] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};

const char* const kUpscalerBoth[]       = {"off", "fsr31", "fsr4"};
const char* const kUpscalerBothNames[]  = {"OFF", "FSR 3.1", "FSR 4"};
const char* const kUpscaler31[]         = {"off", "fsr31"};
const char* const kUpscaler31Names[]    = {"OFF", "FSR 3.1"};
const char* const kUpscaler4[]          = {"off", "fsr4"};
const char* const kUpscaler4Names[]     = {"OFF", "FSR 4"};

constexpr RowDynamics kFsr31ModeDynamics = {.enabled = Fsr31Chosen, .suffix = NotInUse};
constexpr RowDynamics kFsr4ModeDynamics  = {.enabled = Fsr4Chosen, .suffix = NotInUse};
constexpr RowDynamics kSharpnessDynamics = {.enabled = AnyChosen, .suffix = SharpnessSuffix};

#define UPSCALING_ROW(values, names, n) \
    Str("PM_RxGpuUpsc", "mcla_gpu_upscaler", "UPSCALING: ", values, names, n)
#define FSR31_MODE_ROW \
    WithDynamics(Str("PM_RxGpuFsr31M", "mcla_gpu_fsr31_mode", "FSR 3.1 MODE: ", kModeVals, \
                     kModeNames, 1), &kFsr31ModeDynamics)
#define FSR4_MODE_ROW \
    WithDynamics(Str("PM_RxGpuFsr4M", "mcla_gpu_fsr4_mode", "FSR 4 MODE: ", kModeVals, \
                     kModeNames, 1), &kFsr4ModeDynamics)
#define SHARPNESS_ROW \
    WithDynamics(Dbl("PM_RxGpuUpscSharp", "mcla_gpu_upscaler_sharpness", "SHARPNESS: ", \
                     kSharpVals, 6, "%g", "", "OFF"), &kSharpnessDynamics)

// NativeOnly: the upscaler is the Plume renderer's.
const ItemDef kBothRows[] = {
    mcla_menu::NativeOnly(UPSCALING_ROW(kUpscalerBoth, kUpscalerBothNames, 3)),
    mcla_menu::NativeOnly(FSR31_MODE_ROW),
    mcla_menu::NativeOnly(FSR4_MODE_ROW),
    mcla_menu::NativeOnly(SHARPNESS_ROW),
};
const ItemDef k31Rows[] = {
    mcla_menu::NativeOnly(UPSCALING_ROW(kUpscaler31, kUpscaler31Names, 2)),
    mcla_menu::NativeOnly(FSR31_MODE_ROW),
    mcla_menu::NativeOnly(SHARPNESS_ROW),
};
const ItemDef k4Rows[] = {
    mcla_menu::NativeOnly(UPSCALING_ROW(kUpscaler4, kUpscaler4Names, 2)),
    mcla_menu::NativeOnly(FSR4_MODE_ROW),
    mcla_menu::NativeOnly(SHARPNESS_ROW),
};

#undef UPSCALING_ROW
#undef FSR31_MODE_ROW
#undef FSR4_MODE_ROW
#undef SHARPNESS_ROW

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* UpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    const mcla::gpu::UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if (a.fsr31 && a.fsr4) {
        count = int(sizeof(kBothRows) / sizeof(kBothRows[0]));
        return kBothRows;
    }
    if (a.fsr31) {
        count = int(sizeof(k31Rows) / sizeof(k31Rows[0]));
        return k31Rows;
    }
    if (a.fsr4) {
        count = int(sizeof(k4Rows) / sizeof(k4Rows[0]));
        return k4Rows;
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
