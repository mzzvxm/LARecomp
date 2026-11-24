// The pause menu's upscaling rows (on the DISPLAY page). See upscaling_menu.h.

#include "upscaling_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "settings/larecomp_settings.h"

#include "mc_engine/display/display_mode.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/denoiser_setting.h"
#include "gpu/gpu.h"
#include "gpu/ray_regen_plan.h"
#endif

namespace {

#if defined(LARECOMP_HAS_GPU)

using mcla::gpu::FsrPreset;
using mcla::gpu::RenderScaleChoice;
using mcla::gpu::Scale;
using mcla::gpu::Upscaler;
using mcla::gpu::UpscalerAvailability;
using mcla::gpu::UpscaleMode;
using mcla_menu::CvarWriter;
using mcla_menu::ItemDef;
using mcla_menu::NativeOnly;
using mcla_menu::RowDynamics;
using mcla_menu::Str;
using mcla_menu::WithDynamics;

std::string Cvar(const char* name) { return larecomp::cvar::GetFlagByName(name); }
double CvarNumber(const char* name) { return std::strtod(Cvar(name).c_str(), nullptr); }
bool CvarOn(const char* name) { return Cvar(name) == "true"; }

// A setting written only when it differs, through the pause menu's own writer.
void Put(CvarWriter write, const char* name, const char* value) {
    if (Cvar(name) != value) write(name, value);
}

std::string Number(double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}
std::string Percent(int share) { return std::to_string(share) + "%"; }

// ── What the PC runs and what the player picked ────────────────────────────────────────────────

// The temporal upscaler as saved, and the one that does the work (the choice when this PC can run it).
Upscaler Chosen() {
    const std::string value = Cvar("mcla_gpu_upscaler");
    if (value == "fsr31") return Upscaler::kFsr31;
    if (value == "fsr4") return Upscaler::kFsr4;
    if (value == "dlss") return Upscaler::kDlss;
    return Upscaler::kOff;
}

// DLSS is the saved choice but this game can run it only from the next start (a Vulkan game started
// without Streamline). The renderer keeps what it runs until then (gpu.h UpscalerAvailability::kept).
bool DlssWaitsForRestart() {
    if (Chosen() != Upscaler::kDlss) return false;
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    return a.dlss_after_restart && !a.dlss;
}

// The upscaler that does the work: the choice when this PC can run it; while DLSS waits for a
// restart, the one the renderer keeps (FSR 3.1 or 4 as it was running, or off), which is what holds
// FXAA and SMAA off and what the sharpness row belongs to.
Upscaler Active() {
    const Upscaler chosen = Chosen();
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if ((chosen == Upscaler::kFsr31 && a.fsr31) || (chosen == Upscaler::kFsr4 && a.fsr4) ||
        (chosen == Upscaler::kDlss && a.dlss))
        return chosen;
    if (chosen == Upscaler::kDlss && a.dlss_after_restart) return a.kept;
    return Upscaler::kOff;
}

// What UPSCALING lists. FSR 1 is the renderer's own pass on the present and always runs.
enum class Pick : uint8_t { kOff, kFsr1, kFsr31, kFsr4, kDlss };

// DLSS is listed when it can run, when its files are missing (greyed: "(SETUP)") and when it can run
// from the next start.
bool DlssListed(const UpscalerAvailability& a) {
    return a.dlss || a.dlss_needs_setup || a.dlss_after_restart;
}

struct Listed {
    Pick pick[5];
    int count = 0;
};
Listed ListedPicks(const UpscalerAvailability& a) {
    Listed l;
    l.pick[l.count++] = Pick::kOff;
    l.pick[l.count++] = Pick::kFsr1;
    if (a.fsr31) l.pick[l.count++] = Pick::kFsr31;
    if (a.fsr4) l.pick[l.count++] = Pick::kFsr4;
    if (DlssListed(a)) l.pick[l.count++] = Pick::kDlss;
    return l;
}

// What the UPSCALING row is on: the saved upscaler when it is listed, else FSR 1 when that is on,
// else off. A saved choice that cannot run (another GPU, the files gone) shows as off and keeps its
// saved mode (mcla_gpu_*_mode), as the renderer treats it.
Pick Shown(const UpscalerAvailability& a) {
    switch (Chosen()) {
    case Upscaler::kFsr31: if (a.fsr31) return Pick::kFsr31; break;
    case Upscaler::kFsr4:  if (a.fsr4) return Pick::kFsr4; break;
    case Upscaler::kDlss:  if (DlssListed(a)) return Pick::kDlss; break;
    default: break;
    }
    return CvarOn("mcla_gpu_fsr") ? Pick::kFsr1 : Pick::kOff;
}
Pick Shown() { return Shown(mcla::gpu::GetUpscalerAvailability()); }

// ── A row's list ───────────────────────────────────────────────────────────────────────────────

// One value of a mode row or of SHARPNESS: what it says, and the text of the cvar it sets.
struct Entry {
    std::string label;
    std::string text;
};
struct List {
    const char* cvar = nullptr;  // the setting the entries are values of
    std::vector<Entry> entries;
    int current = 0;  // the entry the setting is on
};

// FSR's five modes (FSR 3.1, FSR 4: AMD's names; DLSS: NVIDIA's, DLAA first), each with the share of
// the output the scene is drawn at. The table has the vendors' full names; ULTRA PERFORMANCE is cut
// to ULTRA PERF. to keep the UPSCALING page inside its width (pause_menu.cpp, "Page width").
std::string ModeLabel(Upscaler upscaler, UpscaleMode mode, bool not_available) {
    const mcla::gpu::ModeInfo& info = mcla::gpu::ModeInfoOf(upscaler, mode);
    std::string name = info.name;
    if (name == "ULTRA PERFORMANCE") name = "ULTRA PERF.";
    return name + (not_available ? " (N/A)" : " (" + Percent(int(info.percent)) + ")");
}

// FSR 3.1, FSR 4 or DLSS: the five modes, the saved one current. A mode DLSS cannot take at the output
// size (gpu.h UpscalerAvailability::dlss_modes_refused) says "(N/A)" in place of its share: picking
// it saves the choice and the mode that runs goes on, as the render scale keeps the one that runs
// when it says "LIMITED". A saved text that is no mode of the list (native_aa for DLSS, which the
// renderer takes for DLAA) is the first mode.
List TemporalList(Upscaler upscaler) {
    List list;
    list.cvar = upscaler == Upscaler::kFsr4   ? "mcla_gpu_fsr4_mode"
                : upscaler == Upscaler::kDlss ? "mcla_gpu_dlss_mode"
                                              : "mcla_gpu_fsr31_mode";
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const std::string saved = Cvar(list.cvar);
    for (int i = 0; i < int(UpscaleMode::kCount); ++i) {
        const UpscaleMode mode = UpscaleMode(i);
        const mcla::gpu::ModeInfo& info = mcla::gpu::ModeInfoOf(upscaler, mode);
        list.entries.push_back(
            {ModeLabel(upscaler, mode, upscaler == Upscaler::kDlss && a.DlssModeRefused(mode)),
             info.cvar});
        if (saved == info.cvar) list.current = i;
    }
    return list;
}

// The share of the output a render scale is, in whole percent. The screen's scale (the window's 16:9
// area, mcla_gpu_resolution_scale = 0) is the output, so it is 100% whatever the twentieths it lands
// on make of it (a 1366x768 window: 1.05 against 1.067).
int ShareOf(const RenderScaleChoice& c) {
    if (c.source == RenderScaleChoice::Source::kScreen && !c.limited) return 100;
    return int(std::lround(100.0 * double(c.scale.value()) / c.screen));
}

// OFF: RENDER SCALE, the share of the output the whole picture is drawn at. 100% is the screen's scale
// (the cvar's 0, which follows the window); the others are that many times it, written as the cvar's
// own number for the window as it is now (the cvar counts from the game's 1280x720). The renderer
// makes 3x of 720p at most, so a share it cannot reach is the nearest it can: shares that come to the
// same scale are one entry (1080p: 100%, 150%, 200%; 1440p: 100%, 150%; 4K: 100%). The saved scale
// when it is none of them (0.5X, a number set in larecomp.toml: 1 is 67% in a 1080p window) is an
// entry of its own in its place, as long as it is the setting. A scale the video memory
// could not hold says "LIMITED (NN%)" with the share that does run.
List OffList() {
    constexpr const char* kCvar = "mcla_gpu_resolution_scale";
    List list;
    list.cvar = kCvar;
    const RenderScaleChoice screen =
        mcla::gpu::PreviewRenderScale(0.0, false, FsrPreset::kCustom);
    const mcla::gpu::RenderScaleStatus status = mcla::gpu::GetRenderScaleStatus();
    const double max_scale = double(mcla::gpu::kMaxRenderScale.value());

    // "LIMITED (150%)": only the entry that asks for the scale that could not be made is relabelled.
    const auto label = [&](const RenderScaleChoice& c, int share) -> std::string {
        if (!status.blocked.none() && !status.running.none() && c.scale == status.blocked) {
            const int kept = status.running == screen.scale
                                 ? 100
                                 : int(std::lround(100.0 * double(status.running.value()) / screen.screen));
            return "LIMITED (" + Percent(kept) + ")";
        }
        return Percent(share);
    };

    std::vector<Scale> scales;  // the scale each entry makes, which says which entries are the same
    for (const double times : {1.0, 1.5, 2.0, 3.0}) {
        const double asked =
            times == 1.0 ? 0.0 : std::round(times * screen.screen * 20.0) / 20.0;
        const RenderScaleChoice c = mcla::gpu::PreviewRenderScale(asked, false, FsrPreset::kCustom);
        if (std::find(scales.begin(), scales.end(), c.scale) != scales.end()) continue;
        scales.push_back(c.scale);
        const int share = c.limited ? ShareOf(c) : int(std::lround(times * 100.0));
        list.entries.push_back(
            {label(c, share), asked == 0.0 ? "0" : Number(std::min(asked, max_scale))});
    }

    const RenderScaleChoice saved =
        mcla::gpu::PreviewRenderScale(CvarNumber(kCvar), false, FsrPreset::kCustom);
    const auto same = std::find(scales.begin(), scales.end(), saved.scale);
    if (same != scales.end()) {
        list.current = int(same - scales.begin());
    } else {
        size_t at = 0;
        while (at < scales.size() && scales[at] < saved.scale) ++at;
        list.entries.insert(list.entries.begin() + std::ptrdiff_t(at),
                            Entry{label(saved, ShareOf(saved)), Cvar(kCvar)});
        list.current = int(at);
    }
    return list;
}

// FSR 1: AMD's four modes with the share of the output they render at (the preset's ratio: 1.3, 1.5,
// 1.7, 2 per direction), ULTRA QUALITY cut to ULTRA QUAL. for the page's width. The preset the
// settings hold when it is none of the four is an entry of its own, in its place: ULTRA PERF. (33%),
// the fifth preset the cvar takes, last; "custom" (the render scale decides, the cvar's default) first,
// with the share it gives.
List Fsr1List() {
    constexpr const char* kCvar = "mcla_gpu_fsr_preset";
    struct Mode {
        FsrPreset preset;
        const char* name;
    };
    constexpr Mode kModes[] = {{FsrPreset::kUltraQuality, "ULTRA QUAL."},
                               {FsrPreset::kQuality, "QUALITY"},
                               {FsrPreset::kBalanced, "BALANCED"},
                               {FsrPreset::kPerformance, "PERFORMANCE"}};
    const auto entry = [](FsrPreset preset, const char* name) {
        const int share = int(std::lround(100.0 / mcla::gpu::FsrPresetRatio(preset)));
        return Entry{std::string(name) + " (" + Percent(share) + ")", mcla::gpu::FsrPresetName(preset)};
    };
    List list;
    list.cvar = kCvar;
    for (const Mode& m : kModes) list.entries.push_back(entry(m.preset, m.name));

    const std::string text = Cvar(kCvar);
    FsrPreset saved = FsrPreset::kCustom;
    const bool parsed = mcla::gpu::ParseFsrPreset(text, &saved);
    for (int i = 0; i < int(sizeof(kModes) / sizeof(kModes[0])); ++i) {
        if (kModes[i].preset == saved) {
            list.current = i;
            return list;
        }
    }
    if (saved == FsrPreset::kUltraPerformance) {
        list.entries.push_back(entry(FsrPreset::kUltraPerformance, "ULTRA PERF."));
        list.current = int(list.entries.size()) - 1;
        return list;
    }
    // "custom", or text that is no preset (the renderer takes it for custom and says so in the log).
    const RenderScaleChoice c = mcla::gpu::PreviewRenderScale(
        CvarNumber("mcla_gpu_resolution_scale"), false, FsrPreset::kCustom);
    list.entries.insert(list.entries.begin(),
                        Entry{"CUSTOM (" + Percent(ShareOf(c)) + ")", parsed ? "custom" : text});
    list.current = 0;
    return list;
}

// XENIA: RENDER SCALE is the SDK's draw resolution scale (resolution_scale): whole multiples of the
// game's 1280x720 screen, made when the game starts ("(RESTART)"). It says what a step is as a share
// of the output (the window), as the NATIVE row does: 1080p lists 67%, 133% and 200%; 1440p 50%, 100%
// and 150%; 4K 33%, 67% and 100% (a window the size of the game's screen: 100%, 200%, 300%). The
// scale the settings hold when it is none of the three (4X, which the old row listed, up to 8X) is
// an entry of its own, in its place, while it is the setting.
List XeniaList() {
    constexpr const char* kCvar = "resolution_scale";
    constexpr double kGameScreenWidth = 1280.0;
    constexpr double kGameScreenHeight = 720.0;
    List list;
    list.cvar = kCvar;
    const mcla::display::Size output = mcla::display::OutputSize();
    const double screen =
        output.empty() ? 1.0
                       : std::min(double(output.width) / kGameScreenWidth,
                                  double(output.height) / kGameScreenHeight);
    const int saved = std::clamp(int(std::lround(CvarNumber(kCvar))), 1, 8);
    std::vector<int> steps = {1, 2, 3};
    if (std::find(steps.begin(), steps.end(), saved) == steps.end()) {
        steps.insert(std::upper_bound(steps.begin(), steps.end(), saved), saved);
    }
    for (const int step : steps) {
        if (step == saved) list.current = int(list.entries.size());
        list.entries.push_back(
            {Percent(int(std::lround(100.0 * double(step) / screen))), std::to_string(step)});
    }
    return list;
}

// The mode row's list for what UPSCALING is on.
List ModeList() {
    if (!mcla_menu::NativeRendererRuns()) return XeniaList();
    switch (Shown()) {
    case Pick::kFsr1:  return Fsr1List();
    case Pick::kFsr31: return TemporalList(Upscaler::kFsr31);
    case Pick::kFsr4:  return TemporalList(Upscaler::kFsr4);
    case Pick::kDlss:  return TemporalList(Upscaler::kDlss);
    default:           return OffList();
    }
}

// SHARPNESS: the RCAS pass's strength from 0 to 1. FSR 3.1 and FSR 4: OFF, 0.2 .. 1 in
// mcla_gpu_upscaler_sharpness. FSR 1: 0.2 .. 1 (it has no off: its softest is a quarter of the
// strength) in mcla_gpu_fsr_sharpness, which counts stops of reduction (0 sharpest, 2 softest):
// sharpness s is 2 - 2s stops, the way FSR 2 and 3 turn it into the same pass. A saved number that is not a step (FSR 1's default 0.2 stops is 0.9) is an entry of its own,
// in its place, while it is the setting.
List SharpnessList(bool fsr1) {
    List list;
    list.cvar = fsr1 ? "mcla_gpu_fsr_sharpness" : "mcla_gpu_upscaler_sharpness";
    const double saved = fsr1 ? 1.0 - CvarNumber(list.cvar) / 2.0 : CvarNumber(list.cvar);
    std::vector<double> steps = {0.2, 0.4, 0.6, 0.8, 1.0};
    if (!fsr1) steps.insert(steps.begin(), 0.0);
    bool found = false;
    for (const double s : steps) {
        const std::string text = Number(fsr1 ? 2.0 - 2.0 * s : s);
        list.entries.push_back({s == 0.0 ? "OFF" : Number(s), text});
        if (std::abs(s - saved) < 0.005) {
            list.current = int(list.entries.size()) - 1;
            found = true;
        }
    }
    if (!found) {
        size_t at = 0;
        while (at < steps.size() && steps[at] < saved) ++at;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2g", saved);
        list.entries.insert(list.entries.begin() + std::ptrdiff_t(at), Entry{buf, Cvar(list.cvar)});
        list.current = int(at);
    }
    return list;
}

// ── The UPSCALING row ──────────────────────────────────────────────────────────────────────────

std::string PickLabel(Pick pick, const UpscalerAvailability& a) {
    switch (pick) {
    case Pick::kFsr1:  return "FSR 1";
    case Pick::kFsr31: return "FSR 3.1";
    case Pick::kFsr4:  return "FSR 4";
    // State "needs setup" (the files are missing or fail their signature check) is "DLSS (SETUP)"; a
    // start that is needed (Vulkan, started without Streamline) is plain "DLSS": the row's label says
    // "(RESTART)" while it is the choice (UpscalerRowSuffix), which is where the "Restart now?"
    // prompt reads it.
    case Pick::kDlss:  return a.dlss_needs_setup && !a.dlss ? "DLSS (SETUP)" : "DLSS";
    default:           return "OFF";
    }
}

int UpscalerCount() { return ListedPicks(mcla::gpu::GetUpscalerAvailability()).count; }

std::string UpscalerLabel(int i) {
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const Listed l = ListedPicks(a);
    return i >= 0 && i < l.count ? PickLabel(l.pick[i], a) : std::string();
}

int UpscalerIndex() {
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const Listed l = ListedPicks(a);
    const Pick shown = Shown(a);
    for (int i = 0; i < l.count; ++i)
        if (l.pick[i] == shown) return i;
    return 0;
}

// The choice is written into the settings the renderer reads: mcla_gpu_upscaler for the temporal
// ones, mcla_gpu_fsr (and its preset) for FSR 1, and the other one is turned off so the two do not
// stack. The old one goes off first, so the renderer is never asked for both. Each technology's mode
// is its own cvar and is left as it was; FSR 1 has none saved when its preset is still the default
// "custom" (the render scale decides), and starts at QUALITY.
void UpscalerSet(int i, CvarWriter write) {
    const Listed l = ListedPicks(mcla::gpu::GetUpscalerAvailability());
    if (i < 0 || i >= l.count) return;
    switch (l.pick[i]) {
    case Pick::kFsr1: {
        Put(write, "mcla_gpu_upscaler", "off");
        FsrPreset preset = FsrPreset::kCustom;
        mcla::gpu::ParseFsrPreset(Cvar("mcla_gpu_fsr_preset"), &preset);
        if (preset == FsrPreset::kCustom) Put(write, "mcla_gpu_fsr_preset", "quality");
        Put(write, "mcla_gpu_fsr", "true");
        break;
    }
    case Pick::kFsr31:
        Put(write, "mcla_gpu_fsr", "false");
        Put(write, "mcla_gpu_upscaler", "fsr31");
        break;
    case Pick::kFsr4:
        Put(write, "mcla_gpu_fsr", "false");
        Put(write, "mcla_gpu_upscaler", "fsr4");
        break;
    case Pick::kDlss:
        Put(write, "mcla_gpu_fsr", "false");
        Put(write, "mcla_gpu_upscaler", "dlss");
        break;
    default:
        Put(write, "mcla_gpu_fsr", "false");
        Put(write, "mcla_gpu_upscaler", "off");
        break;
    }
}

// " (RESTART)" while DLSS is the saved choice but this game cannot run it until it is started again
// (a Vulkan game started without DLSS: the device is made through Streamline only when DLSS is chosen
// at the start). The choice is saved and applies at the next start, the renderer goes on with what it
// runs meanwhile (the upscaler and its mode as they were, or none), and leaving the menu offers
// "Restart now?" (the prompt reads this suffix). Otherwise " (APPLYING)" while the settings ask for
// another upscaler or mode than the one running and its context is still being made (DLSS takes
// several seconds; the old one goes on).
// CHECK: the prompt finds this row through the suffix (pause_menu.cpp RowSaysRestart): in a Vulkan
// game started with FSR or no upscaler on an RTX PC, picking DLSS, then leaving the menu, logs
// "[pause-menu] a setting that needs a restart changed: offered RESTART NOW"; picking the old choice
// again before leaving drops it (the setting is back where it started).
const char* UpscalerRowSuffix() {
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    // The row is on screen: a switch to DLSS may come, so the renderer may start Streamline in the
    // background now (gpu.h NoteUpscalerInterest; a cheap atomic read after the first call).
    // CHECK: this is called when the UPSCALING page's rows are drawn, not on every pause (the log line
    // "[gpu] upscalers: the UPSCALING row is on screen" says when it first ran).
    mcla::gpu::NoteUpscalerInterest("the UPSCALING row is on screen");
    if (DlssWaitsForRestart()) return " (RESTART)";
    // The chosen FFX upscaler does not fit the video memory budget now (temporal_ffx.cpp asks it before the
    // context is made); the renderer runs the next one down or none and tries again later.
    if (a.no_room) return " (NO ROOM)";
    return a.switching ? " (APPLYING)" : "";
}

// ── The mode row ───────────────────────────────────────────────────────────────────────────────

// Named for what UPSCALING is on, so a technology that is not the choice has no row of its own.
const char* ModeRowLabel() {
    if (!mcla_menu::NativeRendererRuns()) return "RENDER SCALE";
    switch (Shown()) {
    case Pick::kFsr1:  return "FSR 1 MODE";
    case Pick::kFsr31: return "FSR 3.1 MODE";
    case Pick::kFsr4:  return "FSR 4 MODE";
    case Pick::kDlss:  return "DLSS MODE";
    default:           return "RENDER SCALE";
    }
}

// Live for every choice, with one exception: DLSS shown but not running (its files are missing, or a
// start is needed): the row names DLSS, is grey and says why, and the mode of the upscaler the renderer
// keeps meanwhile is not shown. Width (pause_menu.cpp "Page width", counted for UPSCALING's rows in every
// state): "DLSS MODE (RESTART)" is 19, "DLSS MODE (SETUP)" 17, and every other label of the row stays
// bare.
bool ModeRowEnabled() {
    return !mcla_menu::NativeRendererRuns() || Shown() != Pick::kDlss || Active() == Upscaler::kDlss;
}

// XENIA: the draw scale is read when the game starts, so a step says "(RESTART)" (the prompt reads the
// word) and "RENDER SCALE (RESTART)" is 22 characters.
const char* ModeRowSuffix() {
    if (!mcla_menu::NativeRendererRuns()) return " (RESTART)";
    if (Shown() != Pick::kDlss) return "";
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    if (a.dlss) return "";
    if (a.dlss_needs_setup) return " (SETUP)";
    if (a.dlss_after_restart) return " (RESTART)";
    return "";
}

int ModeCount() { return int(ModeList().entries.size()); }

std::string ModeValueLabel(int i) {
    const List list = ModeList();
    return i >= 0 && i < int(list.entries.size()) ? list.entries[size_t(i)].label : std::string();
}

int ModeIndex() { return ModeList().current; }

void ModeSet(int i, CvarWriter write) {
    const List list = ModeList();
    if (i >= 0 && i < int(list.entries.size()))
        write(list.cvar, list.entries[size_t(i)].text.c_str());
}

// What the "restart now?" prompt looks up: XENIA's draw scale. NATIVE's row writes whichever setting
// the choice above it is about, none of which needs a restart.
const char* ModeRowCvar() { return mcla_menu::NativeRendererRuns() ? nullptr : "resolution_scale"; }

// ── SHARPNESS ──────────────────────────────────────────────────────────────────────────────────

// Live for FSR 1 and for the FSR upscaler that runs (the one the renderer keeps while DLSS waits for a
// restart too); grey for OFF and for DLSS, which has none. One short suffix for both reasons: the row
// stays the same width.
bool SharpnessEnabled() {
    const Upscaler active = Active();
    return Shown() == Pick::kFsr1 || active == Upscaler::kFsr31 || active == Upscaler::kFsr4;
}
const char* SharpnessSuffix() { return SharpnessEnabled() ? "" : " (FSR ONLY)"; }

List CurrentSharpnessList() { return SharpnessList(Shown() == Pick::kFsr1); }
int SharpnessCount() { return int(CurrentSharpnessList().entries.size()); }
std::string SharpnessValueLabel(int i) {
    const List list = CurrentSharpnessList();
    return i >= 0 && i < int(list.entries.size()) ? list.entries[size_t(i)].label : std::string();
}
int SharpnessIndex() { return CurrentSharpnessList().current; }
void SharpnessSet(int i, CvarWriter write) {
    const List list = CurrentSharpnessList();
    if (i >= 0 && i < int(list.entries.size()))
        write(list.cvar, list.entries[size_t(i)].text.c_str());
}

// ── The denoiser row ───────────────────────────────────────────────────────────────────────────
//
// ONE on / off row for the graphics card maker's denoiser (owner 2026-11-23; gpu/denoiser_setting.h has every rule and the words): "DLSS RAY RECONSTRUCTION" on a GeForce RTX card (ON is NVIDIA's DLSS
// Ray Reconstruction, which does the upscale too; OFF is ours), "AMD FSR RAY REGENERATION" on a Radeon RX 9000 series card on Windows 11 (ON is AMD's FSR Ray Regeneration, which works with any upscaler; OFF is
// ours). Any other card has no row (DenoiserListed), and uses ours. It is the player's own setting, mcla_gpu_vendor_denoiser (live, saved, off by default); the old three-way chooser, mcla_gpu_denoiser, is a debug
// setting now and overrides it when it says auto, rr or ours (denoiser_setting.h Effective), so the A/B chains and tours keep working.
// The row is grey, with its reason after its label (NVIDIA's guidelines: disable it where the driver cannot run it, where DLSS Super Resolution is not on and where there is no ray traced content): " (N/A)",
// " (NEEDS DLSS)", " (SETUP)", " (FAILED)", " (NOT NEEDED)" on an RTX card; on a Radeon " (D3D12 ONLY)" on Vulkan (AMD ships no Vulkan provider), " (N/A)", " (SETUP)", " (FAILED)", " (NOT NEEDED)". It says
// " (RESTART)" while the row is ON in a game whose Streamline was started without the plugin (the "Restart now?" prompt reads the word) and " (APPLYING)" while the denoiser's context is being made. It is grey
// (NOT NEEDED) while no effect that has a raw mode is on (RR-INT: the screen-space reflections, the cars' ray traced reflections): nothing is shown as unavailable that a restart or a setting would bring.
// Width: DISPLAY counts its widest label plus its widest value over every row (pause_menu.cpp "Page width"), 41 at most, and its widest value is 17, so no label is over 24: the row says its whole name while
// it says nothing ("DLSS RAY RECONSTRUCTION" 23, "AMD FSR RAY REGENERATION" 24) and its short name in front of a reason ("DLSS RR (NEEDS DLSS)" 20, "RAY REGEN (D3D12 ONLY)" 22).

mcla::gpu::denoiser::VendorRow DenoiserRowKind() {
    return mcla::gpu::denoiser::RowFor(mcla::gpu::rayregen::RowListed(),
                                       DlssListed(mcla::gpu::GetUpscalerAvailability()));
}
// Whether this PC has the row at all: an RTX card, or a Radeon RX 9000 series card on Windows 11.
bool DenoiserListed() { return DenoiserRowKind() != mcla::gpu::denoiser::VendorRow::kNone; }
bool AmdRow() { return DenoiserRowKind() == mcla::gpu::denoiser::VendorRow::kRayRegen; }

mcla::gpu::denoiser::MenuInputs DenoiserInputs() {
    namespace dn = mcla::gpu::denoiser;
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const dn::Snapshot snapshot = dn::Current();
    dn::MenuInputs m;
    m.dlss_listed = DlssListed(a);
    m.dlss_chosen = Chosen() == Upscaler::kDlss;
    m.dlss_waits_for_restart = DlssWaitsForRestart();
    m.dlss_needs_setup = a.dlss_needs_setup && !a.dlss;
    m.facts = snapshot.facts;
    m.debug = snapshot.valid ? snapshot.debug : mcla::gpu::rr::DebugSwitch::kRow;
    m.setting = dn::Effective(Cvar("mcla_gpu_denoiser"), CvarOn("mcla_gpu_vendor_denoiser"));
    m.consumers_on = dn::ConsumersOn();
    m.context_making = snapshot.context_making;
    return m;
}
mcla::gpu::denoiser::Gate DenoiserGate() { return mcla::gpu::denoiser::GateOf(DenoiserInputs()); }

// A Radeon: the gate is the one of AMD's runtime (a Radeon RX 9000 series card on Direct3D 12; no restart: the runtime is opened when the row asks for it). Every other listed card is an RTX card's.
mcla::gpu::rayregen::Gate AmdGate() {
    namespace rg = mcla::gpu::rayregen;
    return rg::GateOf(rg::MenuFacts(), rg::ReadDebugSwitch(), mcla::gpu::denoiser::ConsumersOn(), rg::ContextMaking());
}

const char* DenoiserLabel() {
    return AmdRow() ? mcla::gpu::rayregen::RowLabel(AmdGate()) : mcla::gpu::denoiser::RowLabel(DenoiserGate());
}
bool DenoiserEnabled() {
    return AmdRow() ? mcla::gpu::rayregen::RowEnabled(AmdGate()) : mcla::gpu::denoiser::RowEnabled(DenoiserGate());
}
const char* DenoiserSuffix() {
    return AmdRow() ? mcla::gpu::rayregen::RowSuffix(AmdGate()) : mcla::gpu::denoiser::RowSuffix(DenoiserGate());
}

// ── The rows ───────────────────────────────────────────────────────────────────────────────────
//
// Width: these rows are on DISPLAY, which stays within 41 characters of its widest label plus its
// widest value over ALL its rows (pause_menu.cpp, "Page width"): the page's widest value is 17 and its widest
// label 24. Every state of each row, in characters:
//   UPSCALING   label "UPSCALING" 9, with " (APPLYING)" 20, with " (RESTART)" 19. Values "OFF", "FSR 1",
//               "FSR 3.1", "FSR 4", "DLSS" and "DLSS (SETUP)" 12.
//   mode row    labels "RENDER SCALE" 12, "FSR 1 MODE" 10, "FSR 3.1 MODE" 12, "FSR 4 MODE" 10, "DLSS MODE"
//               9, "DLSS MODE (SETUP)" 17, "DLSS MODE (RESTART)" 19, and on XENIA "RENDER SCALE
//               (RESTART)" 22 with the values "33%" to "267%" (an own entry up to "800%": 4). Values: "NATIVE AA (100%)" 16,
//               "DLAA (100%)" 11, "QUALITY (67%)" 13, "BALANCED (59%)" 14, "PERFORMANCE (50%)" 17,
//               "ULTRA PERF. (33%)" 17, "ULTRA QUAL. (77%)" 17; DLSS's (N/A) forms have the same length
//               as their share ("PERFORMANCE (N/A)" 17, "ULTRA PERF. (N/A)" 17); RENDER SCALE: "100%"
//               to "300%" 4, an entry of the setting's own "CUSTOM (300%)" 13 at the most, "LIMITED
//               (300%)" 14.
//   SHARPNESS   label 9, with " (FSR ONLY)" 20. Values "OFF", "0.2" to "1", an entry of its own such as
//               "0.85" 4.
//   DENOISER    "DLSS RAY RECONSTRUCTION" 23 and "AMD FSR RAY REGENERATION" 24 while the row says nothing; "DLSS RR" with " (NEEDS DLSS)" 20, " (NOT NEEDED)" 20, " (APPLYING)" 18, " (RESTART)" 17,
//               " (SETUP)" 15, " (FAILED)" 16, " (N/A)" 13; "RAY REGEN" with " (D3D12 ONLY)" 22, " (NOT NEEDED)" 22, " (APPLYING)" 20, " (SETUP)" 16, " (FAILED)" 18, " (N/A)" 15.
//               The row is a checkbox (ON / OFF, 3).
// These rows' widest label comes to 24 (AMD FSR RAY REGENERATION) and the widest value to 17. With frame
// generation's rows the page's widest value is still 17 ("FSR 3.1 (NO ROOM)", frame_generation_menu.h): 41 of the
// page's 41. denoiser_setting_test.cpp counts the whole page, every row in every state.
// CHECK: open DISPLAY at 1280x720 with a temporal upscaler on, the mode row on PERFORMANCE, and look at the right
// edge (nothing cut); then the same with FSR 1 on ULTRA QUAL.
// CHECK: in a game, with DISPLAY open, step UPSCALING through every choice: the row under it is
// relabelled at once (RENDER SCALE, FSR 1 MODE, FSR 3.1 MODE, FSR 4 MODE, DLSS MODE) with its own
// saved mode on the middle value and no text of another row or state left in it; left and right wrap
// at both ends of the lists of 4 and 5 entries, and of RENDER SCALE with an entry of its own ("67%" in
// a 1080p window with a saved scale of 1) that goes when the row steps off it; the page is not built
// again ("[pause-menu] 'DISPLAY' native rows built" once); the render scale and the FSR 1 preset
// change the picture at a frame boundary (log "[gpu] temporal: live change").

constexpr RowDynamics kUpscalerDynamics = {.str_label = UpscalerLabel,
                                           .suffix = UpscalerRowSuffix,
                                           .value_count = UpscalerCount,
                                           .value_index = UpscalerIndex,
                                           .set_value = UpscalerSet};
constexpr RowDynamics kModeDynamics = {.str_label = ModeValueLabel,
                                       .enabled = ModeRowEnabled,
                                       .suffix = ModeRowSuffix,
                                       .label = ModeRowLabel,
                                       .value_count = ModeCount,
                                       .value_index = ModeIndex,
                                       .set_value = ModeSet,
                                       .cvar = ModeRowCvar};
constexpr RowDynamics kSharpnessDynamics = {.str_label = SharpnessValueLabel,
                                            .enabled = SharpnessEnabled,
                                            .suffix = SharpnessSuffix,
                                            .value_count = SharpnessCount,
                                            .value_index = SharpnessIndex,
                                            .set_value = SharpnessSet};
constexpr RowDynamics kDenoiserDynamics = {.enabled = DenoiserEnabled, .suffix = DenoiserSuffix, .label = DenoiserLabel};

// The rows' lists and labels follow the PC and the settings every time a row is drawn. The mode row and
// SHARPNESS write several cvars, depending on the choice, so they name none of their own; UPSCALING names
// mcla_gpu_upscaler, which the "Restart now?" prompt looks for, and the denoiser row its own setting
// (mcla_gpu_vendor_denoiser, a checkbox) for the same prompt. Which rows a PC lists is UpscalingMenuRows' count:
// the first three always, the denoiser row (the last one) only where this PC has it (DenoiserListed).
// NativeOnly: the upscalers are the Plume renderer's. The mode row is listed on both renderers (it is
// RENDER SCALE on XENIA, whose own mechanism it writes).
constexpr ItemDef kRows[] = {
    NativeOnly(WithDynamics(
        Str("PM_RxGpuUpsc", "mcla_gpu_upscaler", "UPSCALING: ", nullptr, nullptr, 0),
        &kUpscalerDynamics)),
    WithDynamics(Str("PM_RxGpuUpscMode", nullptr, "RENDER SCALE: ", nullptr, nullptr, 0),
                 &kModeDynamics),
    NativeOnly(WithDynamics(
        Str("PM_RxGpuUpscSharp", nullptr, "SHARPNESS: ", nullptr, nullptr, 0),
        &kSharpnessDynamics)),
    // After the upscaler's rows: RR is the DLSS upscaler's companion (it does the upscale where it takes a frame). LAST, so a PC without it lists the other three (UpscalingMenuRows). One setting, so the
    // "Restart now?" prompt finds it.
    NativeOnly(WithDynamics(
        Bool("PM_RxGpuDenoiser", "mcla_gpu_vendor_denoiser", "DLSS RAY RECONSTRUCTION: "),
        &kDenoiserDynamics)),
};
constexpr int kUpscalerRowCount = 3;  // the rows before the denoiser row
static_assert(sizeof(kRows) / sizeof(kRows[0]) == size_t(kUpscalerRowCount) + 1, "UPSCALING, its mode row, SHARPNESS, and the denoiser row last");

// ── The AA rows (the page width is counted above) ──────────────────────────────────────────────

// POST AA and MSAA while a temporal upscaler runs: grey, and the suffix names the upscaler that
// does the anti-aliasing instead ("MSAA (FSR 3.1 ON)"). Width: both rows are on IMAGE QUALITY, "MSAA (FSR 3.1
// ON)" is 17 and "POST AA (FSR 3.1 ON)" 20 (the page's widest label), with values of at most 4.
const char* HandledSuffix() {
    switch (Active()) {
    case Upscaler::kFsr31: return " (FSR 3.1 ON)";
    case Upscaler::kFsr4:  return " (FSR 4 ON)";
    case Upscaler::kDlss:  return " (DLSS ON)";
    default:               return "";
    }
}
bool AaFree() { return Active() == Upscaler::kOff; }

// The MSAA row. The game always draws its own 2 sample layout and the renderer gives the host's
// targets the samples of the row, so OFF, 2X, 4X and 8X are all live: the renderer changes them
// between two frames once the pipelines for the new sample count are made (msaa_level.h; the row
// says "(APPLYING)" meanwhile) and no value says RESTART. While a temporal upscaler runs the row is grey
// and names the upscaler, which does the anti-aliasing: MSAA is 1 sample then, the choice is kept
// and comes back when the upscaler is off.
// CHECK: OFF, 2X, 4X and 8X carry no suffix (and "(APPLYING)" for the seconds a change is being
// prepared), "Restart now?" is not offered when the menu is left after changing the row, at start
// with MSAA OFF as well as with 2X; with an upscaler on the row says "(FSR 3.1 ON)" (or the
// upscaler's own name) and is grey, and plain again once it is off.
const char* MsaaSuffix() {
    if (Active() != Upscaler::kOff) return HandledSuffix();
    return mcla::gpu::MsaaSwitching() ? " (APPLYING)" : "";
}

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* UpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    // The same table, one row shorter where this PC has no denoiser row (a different count is a different row set: UpscalingPageRows keeps one table per count).
    count = DenoiserListed() ? kUpscalerRowCount + 1 : kUpscalerRowCount;
    return kRows;
#else
    count = 0;
    return nullptr;
#endif
}

namespace mcla_menu {

#if defined(LARECOMP_HAS_GPU)
extern const RowDynamics kPostAaDynamics = {.enabled = AaFree, .suffix = HandledSuffix};
extern const RowDynamics kMsaaDynamics = {.enabled = AaFree, .suffix = MsaaSuffix};
#else
extern const RowDynamics kPostAaDynamics = {};
extern const RowDynamics kMsaaDynamics = {};
#endif

}  // namespace mcla_menu
