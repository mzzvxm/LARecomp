// The pause menu's FRAME GENERATION rows. See frame_generation_menu.h.

#include "frame_generation_menu.h"

#include <algorithm>
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

std::string ChosenText() {
    return rex::cvar::GetFlagByName("mcla_gpu_frame_generation");
}
bool FsrChosen() { return ChosenText() == "fsr31"; }
bool DlssChosen() { return ChosenText() == "dlss"; }

// Chosen and this PC can run it: the rows below it are live.
bool On() {
    if (FsrChosen()) return mcla::gpu::GetFrameGenerationAvailability().available;
    if (DlssChosen()) {
        const mcla::gpu::DlssFrameGenerationAvailability d = mcla::gpu::GetDlssFrameGenerationAvailability();
        return d.supported || d.running;
    }
    return false;
}

// What the FRAME GENERATION row lists: FSR 3.1 where it can run here (or failed in this session: the
// row says so), DLSS where an RTX 40 or 50 series card with the files is (it can run, runs from the
// next start on Vulkan, or failed). The list is made each time the row is drawn.
bool FsrListed() {
    const mcla::gpu::FrameGenerationAvailability a = mcla::gpu::GetFrameGenerationAvailability();
    return a.probed && (a.available || a.failed);
}
bool DlssListed() {
    return mcla::gpu::GetDlssFrameGenerationAvailability().listed;
}
bool MultiplierListed() {
    const mcla::gpu::DlssFrameGenerationAvailability d = mcla::gpu::GetDlssFrameGenerationAvailability();
    return d.listed && d.multiplier_choice;
}

struct Entry {
    const char* text;  // the cvar's value
    int kind;          // 0 off, 1 FSR 3.1, 2 DLSS
};
int Entries(Entry (&out)[3]) {
    int n = 0;
    out[n++] = {"off", 0};
    if (FsrListed() || FsrChosen()) out[n++] = {"fsr31", 1};
    if (DlssListed() || DlssChosen()) out[n++] = {"dlss", 2};
    return n;
}

// The FRAME GENERATION row's value i. "(APPLYING)" from the choice until the swap chain has been made
// again for it (a frame boundary away, or until Streamline has started) and while it is being set up;
// "(FAILED)" once it failed; DLSS says "(N/A)" where Streamline said it cannot run here and it is
// chosen.
std::string FrameGenerationLabel(int i) {
    Entry entries[3];
    const int n = Entries(entries);
    if (i < 0 || i >= n || entries[i].kind == 0) return "OFF";
    if (entries[i].kind == 1) {
        const mcla::gpu::FrameGenerationAvailability a = mcla::gpu::GetFrameGenerationAvailability();
        if (a.failed) return "FSR 3.1 (FAILED)";
        // No free video memory for it: off, and on by itself when there is room (the reason is in the log).
        if (FsrChosen() && a.no_room) return "FSR 3.1 (NO ROOM)";
        if (FsrChosen() && a.available && !a.running) return "FSR 3.1 (APPLYING)";
        return "FSR 3.1";
    }
    const mcla::gpu::DlssFrameGenerationAvailability d = mcla::gpu::GetDlssFrameGenerationAvailability();
    if (d.failed) return "DLSS (FAILED)";
    if (DlssChosen() && d.probed && !d.supported && !d.after_restart) return "DLSS (N/A)";
    if (DlssChosen() && !d.after_restart && !d.running) return "DLSS (APPLYING)";
    return "DLSS";
}
int FrameGenerationCount() {
    Entry entries[3];
    return Entries(entries);
}
int FrameGenerationIndex() {
    Entry entries[3];
    const int n = Entries(entries);
    const std::string chosen = ChosenText();
    for (int i = 0; i < n; ++i) {
        if (chosen == entries[i].text) return i;
    }
    return 0;
}
void FrameGenerationSet(int i, mcla_menu::CvarWriter write) {
    Entry entries[3];
    const int n = Entries(entries);
    if (i >= 0 && i < n) write("mcla_gpu_frame_generation", entries[i].text);
}
// DLSS on Vulkan applies from the next start (the device is made through Streamline only when it is
// saved at the start): the row says RESTART, in a label short enough for the page (pause_menu.cpp
// "Page width": "FRAME GEN (RESTART)" is 19 characters; the label is bare otherwise, "FRAME GENERATION" 16).
bool RestartRow() {
    if (!DlssChosen()) return false;
    const mcla::gpu::DlssFrameGenerationAvailability d = mcla::gpu::GetDlssFrameGenerationAvailability();
    return d.after_restart;
}
const char* FrameGenerationRowLabel() { return RestartRow() ? "FRAME GEN" : "FRAME GENERATION"; }
const char* FrameGenerationRowSuffix() { return RestartRow() ? " (RESTART)" : ""; }

// The multiplier's values: 2X, and up to 4X where the plugin says the card takes them.
std::string MultiplierLabel(int i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%dX", i + 2);
    return buf;
}
int MultiplierCount() {
    const uint32_t most = mcla::gpu::GetDlssFrameGenerationAvailability().max_multiplier;
    return int(most >= 2 ? most - 1 : 1);
}
int MultiplierIndex() {
    const int index = int(mcla::gpu::DlssFrameGenerationMultiplier()) - 2;
    return std::clamp(index, 0, MultiplierCount() - 1);
}
void MultiplierSet(int i, mcla_menu::CvarWriter write) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", i + 2);
    write("mcla_gpu_frame_generation_multiplier", buf);
}
bool MultiplierEnabled() { return DlssChosen(); }

// The cap's values, in the frames the game renders: the frames shown are the multiplier times as many
// (twice for FSR). -1 is HALF DISPLAY, the cap from the display's rate where it is 100 Hz or more; below
// that the entry says the player's own cap (fps_limit) applies.
int ShownPerRendered() {
    return DlssChosen() ? int(mcla::gpu::DlssFrameGenerationMultiplier()) : 2;
}
std::string FpsLimitLabel(double value) {
    char buf[40];
    if (value < 0.0) {
        if (DlssChosen()) {
            const mcla::gpu::DlssFrameGenerationAvailability d = mcla::gpu::GetDlssFrameGenerationAvailability();
            const uint32_t m = mcla::gpu::DlssFrameGenerationMultiplier();
            const uint32_t half = (d.refresh_hz >= 100 && m >= 2)
                                      ? uint32_t(0.97 * double(d.refresh_hz) / double(m))
                                      : 0u;
            if (half == 0) return "USE FPS LIMIT";
            if (m == 2) std::snprintf(buf, sizeof(buf), "HALF DISPLAY %u", half);
            else std::snprintf(buf, sizeof(buf), "1/%u DISPLAY %u", m, half);
            return buf;
        }
        const uint32_t half = mcla::gpu::GetFrameGenerationAvailability().half_display;
        if (half == 0) return "USE FPS LIMIT";
        std::snprintf(buf, sizeof(buf), "HALF DISPLAY %u", half);
        return buf;
    }
    if (value == 0.0) return "UNCAPPED";
    std::snprintf(buf, sizeof(buf), "%.0f (%.0f SHOWN)", value, double(ShownPerRendered()) * value);
    return buf;
}

constexpr double kFpsLimitVals[] = {-1.0, 30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 0.0};

constexpr RowDynamics kFrameGenerationDynamics = {.str_label = FrameGenerationLabel,
                                                  .suffix = FrameGenerationRowSuffix,
                                                  .label = FrameGenerationRowLabel,
                                                  .value_count = FrameGenerationCount,
                                                  .value_index = FrameGenerationIndex,
                                                  .set_value = FrameGenerationSet};
constexpr RowDynamics kMultiplierDynamics = {.str_label = MultiplierLabel,
                                             .enabled = MultiplierEnabled,
                                             .value_count = MultiplierCount,
                                             .value_index = MultiplierIndex,
                                             .set_value = MultiplierSet};
constexpr RowDynamics kFpsLimitValueDynamics = {.value_label = FpsLimitLabel, .enabled = On};

// The list's own values and labels are the dynamics' (value_count and str_label); the static list
// names one value so that the row is a valid kStrCycle.
const char* const kFrameGenerationVals[] = {"off"};
const char* const kFrameGenerationNames[] = {"OFF"};
const char* const kMultiplierVals[] = {"2"};
const char* const kMultiplierNames[] = {"2X"};

const ItemDef kFrameGenerationRow =
    mcla_menu::NativeOnly(WithDynamics(
        Str("PM_RxGpuFrameGen", "mcla_gpu_frame_generation", "FRAME GENERATION: ",
            kFrameGenerationVals, kFrameGenerationNames, 1),
        &kFrameGenerationDynamics));
const ItemDef kMultiplierRow =
    mcla_menu::NativeOnly(WithDynamics(
        Str("PM_RxGpuFrameGenMult", "mcla_gpu_frame_generation_multiplier", "FRAME GEN MULTIPLIER: ",
            kMultiplierVals, kMultiplierNames, 1),
        &kMultiplierDynamics));
const ItemDef kFpsLimitRow =
    mcla_menu::NativeOnly(WithDynamics(
        Dbl("PM_RxGpuFrameGenFps", "mcla_gpu_frame_generation_fps_limit", "FRAME GEN FPS LIMIT: ",
            kFpsLimitVals, int(sizeof(kFpsLimitVals) / sizeof(kFpsLimitVals[0])), "%g", "",
            "UNCAPPED"),
        &kFpsLimitValueDynamics));

// The rows are listed where frame generation can run here, or failed in this session (the row says
// so); the probe's answer is final a moment after start. The multiplier row only where the card has a
// choice (an RTX 50 series).
bool Listed() {
    return FsrListed() || DlssListed();
}

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* DisplayMenuRows(const mcla_menu::ItemDef* picture, int picture_count,
                                          int& count) {
    int upscaling_count = 0;
    const mcla_menu::ItemDef* upscaling = UpscalingMenuRows(upscaling_count);
    bool frame_generation = false;
    bool multiplier = false;
#if defined(LARECOMP_HAS_GPU)
    frame_generation = Listed();
    multiplier = frame_generation && MultiplierListed();
#endif
    if (upscaling_count == 0 && !frame_generation) {
        // No Plume renderer in this build: the page's rows are the picture's alone.
        count = picture_count;
        return picture;
    }
    // One table per combination of the upscaler's row set (a pointer the upscaling menu keeps), the
    // picture rows and the frame generation rows (the multiplier row only for a card that has a
    // choice), made on first use and never changed after: the menu holds pointers into it.
    struct Combined {
        const mcla_menu::ItemDef* upscaling = nullptr;
        int upscaling_count = 0;
        const mcla_menu::ItemDef* picture = nullptr;
        int picture_count = 0;
        bool frame_generation = false;
        bool multiplier = false;
        std::vector<mcla_menu::ItemDef> rows;
    };
    static std::mutex mutex;
    static std::vector<std::unique_ptr<Combined>> tables;
    std::lock_guard lock(mutex);
    for (const std::unique_ptr<Combined>& t : tables) {
        if (t->upscaling == upscaling && t->upscaling_count == upscaling_count &&
            t->picture == picture && t->picture_count == picture_count &&
            t->frame_generation == frame_generation && t->multiplier == multiplier) {
            count = int(t->rows.size());
            return t->rows.data();
        }
    }
    auto made = std::make_unique<Combined>();
    made->upscaling = upscaling;
    made->upscaling_count = upscaling_count;
    made->picture = picture;
    made->picture_count = picture_count;
    made->frame_generation = frame_generation;
    made->multiplier = multiplier;
    for (int i = 0; i < upscaling_count; ++i) made->rows.push_back(upscaling[i]);
    for (int i = 0; i < picture_count; ++i) made->rows.push_back(picture[i]);
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

namespace mcla_menu {

#if defined(LARECOMP_HAS_GPU)
namespace {
bool Running() { return mcla::gpu::FrameGenerationRunning() || mcla::gpu::DlssFrameGenerationRunning(); }
bool FpsLimitFree() { return !Running(); }
const char* FpsLimitSuffix() { return Running() ? " (FRAME GEN)" : ""; }
}  // namespace

extern const RowDynamics kFpsLimitDynamics = {.enabled = FpsLimitFree, .suffix = FpsLimitSuffix};

bool LatencyRowLocked() { return mcla::gpu::FrameGenerationRunning() || mcla::gpu::DlssFrameGenerationRunning(); }
#else
extern const RowDynamics kFpsLimitDynamics = {};
bool LatencyRowLocked() { return false; }
#endif

}  // namespace mcla_menu
