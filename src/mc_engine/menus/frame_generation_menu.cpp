// The pause menu's FRAME GENERATION rows. See frame_generation_menu.h.

#include "frame_generation_menu.h"
#include "settings/immortal.h"

#include <memory>
#include <mutex>
#include <vector>

#include "upscaling_menu.h"

namespace {

#if defined(LARECOMP_HAS_GPU)

using mcla_menu::Dbl;
using mcla_menu::ItemDef;
using mcla_menu::Str;

constexpr double kFpsLimitVals[] = {-1.0, 30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 0.0};

// ui_rules.toml supplies the choice lists.
const char* const kFrameGenerationVals[] = {"off"};
const char* const kFrameGenerationNames[] = {"OFF"};
const char* const kMultiplierVals[] = {"2"};
const char* const kMultiplierNames[] = {"2X"};

const ItemDef kFrameGenerationRow =
    mcla_menu::NativeOnly(Str("PM_RxGpuFrameGen", "mcla_gpu_frame_generation", "FRAME GENERATION: ",
            kFrameGenerationVals, kFrameGenerationNames, 1));
const ItemDef kMultiplierRow =
    mcla_menu::NativeOnly(Str("PM_RxGpuFrameGenMult", "mcla_gpu_frame_generation_multiplier", "FRAME GEN MULTIPLIER: ",
            kMultiplierVals, kMultiplierNames, 1));
const ItemDef kFpsLimitRow =
    mcla_menu::NativeOnly(Dbl("PM_RxGpuFrameGenFps", "mcla_gpu_frame_generation_fps_limit", "FRAME GEN FPS LIMIT: ",
            kFpsLimitVals, int(sizeof(kFpsLimitVals) / sizeof(kFpsLimitVals[0])), "%g", "",
            "UNCAPPED"));

// The rows are listed where frame generation can run here, or failed in this session (the row says
// so); the probe's answer is final a moment after start. The multiplier row only where the card has a
// choice (an RTX 50 series).
#endif  // LARECOMP_HAS_GPU

}  // namespace

namespace {

// The rows as they are now (one table per combination, below).
const mcla_menu::ItemDef* LiveUpscalingPageRows(int& count) {
    int upscaling_count = 0;
    const mcla_menu::ItemDef* upscaling = UpscalingMenuRows(upscaling_count);
    bool frame_generation = false;
    bool multiplier = false;
#if defined(LARECOMP_HAS_GPU)
    frame_generation = mcla_menu::PauseRowListed(kFrameGenerationRow);
    multiplier = frame_generation && mcla_menu::PauseRowListed(kMultiplierRow);
#endif
    if (upscaling_count == 0 && !frame_generation) {
        // No Plume renderer in this build: the page has no rows.
        count = 0;
        return nullptr;
    }
    // One table per combination of the upscaler's row set (a pointer the upscaling menu keeps) and the
    // frame generation rows (the multiplier row only for a card that has a choice), made on first use
    // and never changed after: the menu holds pointers into it.
    struct Combined {
        const mcla_menu::ItemDef* upscaling = nullptr;
        int upscaling_count = 0;
        bool frame_generation = false;
        bool multiplier = false;
        std::vector<mcla_menu::ItemDef> rows;
    };
    static std::mutex mutex;
    static std::vector<std::unique_ptr<Combined>>& tables = larecomp::Leak<std::vector<std::unique_ptr<Combined>>>();
    std::lock_guard lock(mutex);
    for (const std::unique_ptr<Combined>& t : tables) {
        if (t->upscaling == upscaling && t->upscaling_count == upscaling_count &&
            t->frame_generation == frame_generation && t->multiplier == multiplier) {
            count = int(t->rows.size());
            return t->rows.data();
        }
    }
    auto made = std::make_unique<Combined>();
    made->upscaling = upscaling;
    made->upscaling_count = upscaling_count;
    made->frame_generation = frame_generation;
    made->multiplier = multiplier;
    for (int i = 0; i < upscaling_count; ++i) made->rows.push_back(upscaling[i]);
#if defined(LARECOMP_HAS_GPU)
    if (frame_generation) {
        made->rows.push_back(kFrameGenerationRow);
        if (multiplier) made->rows.push_back(kMultiplierRow);
        made->rows.push_back(kFpsLimitRow);
    }
#endif
    count = int(made->rows.size());
    const mcla_menu::ItemDef* rows = made->rows.data();
    tables.push_back(std::move(made));
    return rows;
}

// The rows the page was built with (MenuDef::extra must keep returning them until the page is built again: the guest rows are indexed by position). Set when the page is built (UpscalingMenuRelayout
// apply=true, before its first build and before each rebuild); until then the live rows.
std::mutex g_built_mutex;
const mcla_menu::ItemDef* g_built = nullptr;
int g_built_count = 0;

}  // namespace

const mcla_menu::ItemDef* UpscalingPageRows(int& count) {
    {
        std::lock_guard lock(g_built_mutex);
        if (g_built) {
            count = g_built_count;
            return g_built;
        }
    }
    return LiveUpscalingPageRows(count);
}

bool UpscalingMenuRelayout(bool apply) {
    int count = 0;
    const mcla_menu::ItemDef* live = LiveUpscalingPageRows(count);
    std::lock_guard lock(g_built_mutex);
    if (apply) {
        g_built = live;
        g_built_count = count;
        return false;
    }
    // The tables are made once per combination and never freed, so the same pointer is the same rows.
    return g_built != nullptr && (live != g_built || count != g_built_count);
}
