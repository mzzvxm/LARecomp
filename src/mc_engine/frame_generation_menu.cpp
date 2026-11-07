// The pause menu's FRAME GENERATION rows. See frame_generation_menu.h.

#include "frame_generation_menu.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "upscaling_menu.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

namespace {

#if defined(LARECOMP_HAS_GPU)

using mcla_menu::Dbl;
using mcla_menu::ItemDef;
using mcla_menu::RowDynamics;
using mcla_menu::Str;
using mcla_menu::WithDynamics;

bool Chosen() {
    return rex::cvar::GetFlagByName("mcla_gpu_frame_generation") == "fsr31";
}

// Chosen and this PC can run it: the rows below it are live.
bool On() {
    return Chosen() && mcla::gpu::GetFrameGenerationAvailability().available;
}

// The FRAME GENERATION row's value 1. "(WAIT)" from the choice until the swap chain has been made
// again for it (a frame boundary away) and while it is being set up; "(FAILED)" once it failed.
std::string FrameGenerationLabel(int i) {
    if (i == 0) return "OFF";
    const mcla::gpu::FrameGenerationAvailability a = mcla::gpu::GetFrameGenerationAvailability();
    if (a.failed) return "FSR 3.1 (FAILED)";
    if (Chosen() && a.available && !a.running) return "FSR 3.1 (WAIT)";
    return "FSR 3.1";
}

// The cap's values, in the frames the game renders: the frames shown are twice as many. -1 is HALF DISPLAY, the cap
// from the display's rate where it is 100 Hz or more; below that the entry says the player's own cap
// (fps_limit) applies.
std::string FpsLimitLabel(double value) {
    char buf[40];
    if (value < 0.0) {
        const uint32_t half = mcla::gpu::GetFrameGenerationAvailability().half_display;
        if (half == 0) return "USE FPS LIMIT";
        std::snprintf(buf, sizeof(buf), "HALF DISPLAY %u", half);
        return buf;
    }
    if (value == 0.0) return "UNCAPPED";
    std::snprintf(buf, sizeof(buf), "%.0f (%.0f SHOWN)", value, 2.0 * value);
    return buf;
}

const char* const kFrameGenerationVals[]  = {"off", "fsr31"};
const char* const kFrameGenerationNames[] = {"OFF", "FSR 3.1"};
constexpr double kFpsLimitVals[] = {-1.0, 30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 0.0};

constexpr RowDynamics kFrameGenerationDynamics = {.str_label = FrameGenerationLabel};
constexpr RowDynamics kFpsLimitValueDynamics = {.value_label = FpsLimitLabel, .enabled = On};

const ItemDef kRows[] = {
    mcla_menu::NativeOnly(WithDynamics(
        Str("PM_RxGpuFrameGen", "mcla_gpu_frame_generation", "FRAME GENERATION: ",
            kFrameGenerationVals, kFrameGenerationNames, 2),
        &kFrameGenerationDynamics)),
    mcla_menu::NativeOnly(WithDynamics(
        Dbl("PM_RxGpuFrameGenFps", "mcla_gpu_frame_generation_fps_limit", "FRAME GEN FPS LIMIT: ",
            kFpsLimitVals, int(sizeof(kFpsLimitVals) / sizeof(kFpsLimitVals[0])), "%g", "",
            "UNCAPPED"),
        &kFpsLimitValueDynamics)),
};
constexpr int kRowCount = int(sizeof(kRows) / sizeof(kRows[0]));

// The rows are listed where frame generation can run here, or failed in this session (the row says
// so); the probe's answer is final a moment after start.
bool Listed() {
    const mcla::gpu::FrameGenerationAvailability a = mcla::gpu::GetFrameGenerationAvailability();
    return a.probed && (a.available || a.failed);
}

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* DisplayMenuRows(int& count) {
    int upscaling_count = 0;
    const mcla_menu::ItemDef* upscaling = UpscalingMenuRows(upscaling_count);
#if defined(LARECOMP_HAS_GPU)
    if (Listed()) {
        // One table per upscaler row set (a pointer the upscaling menu keeps), made on first use and
        // never changed after.
        struct Combined {
            const mcla_menu::ItemDef* upscaling = nullptr;
            int upscaling_count = 0;
            std::vector<mcla_menu::ItemDef> rows;
        };
        static std::mutex mutex;
        static std::vector<std::unique_ptr<Combined>> tables;
        std::lock_guard lock(mutex);
        for (const std::unique_ptr<Combined>& t : tables) {
            if (t->upscaling == upscaling && t->upscaling_count == upscaling_count) {
                count = int(t->rows.size());
                return t->rows.data();
            }
        }
        auto made = std::make_unique<Combined>();
        made->upscaling = upscaling;
        made->upscaling_count = upscaling_count;
        for (int i = 0; i < upscaling_count; ++i) made->rows.push_back(upscaling[i]);
        for (int i = 0; i < kRowCount; ++i) made->rows.push_back(kRows[i]);
        count = int(made->rows.size());
        const mcla_menu::ItemDef* rows = made->rows.data();
        tables.push_back(std::move(made));
        return rows;
    }
#endif
    count = upscaling_count;
    return upscaling;
}

namespace mcla_menu {

#if defined(LARECOMP_HAS_GPU)
namespace {
bool FpsLimitFree() { return !mcla::gpu::FrameGenerationRunning(); }
const char* FpsLimitSuffix() { return mcla::gpu::FrameGenerationRunning() ? " (FRAME GEN)" : ""; }
}  // namespace

extern const RowDynamics kFpsLimitDynamics = {.enabled = FpsLimitFree, .suffix = FpsLimitSuffix};

bool LatencyRowLocked() { return mcla::gpu::FrameGenerationRunning(); }
#else
extern const RowDynamics kFpsLimitDynamics = {};
bool LatencyRowLocked() { return false; }
#endif

}  // namespace mcla_menu
