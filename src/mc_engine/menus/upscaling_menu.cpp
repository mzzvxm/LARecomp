// The pause menu's upscaling rows (on the DISPLAY page). See upscaling_menu.h.

#include "upscaling_menu.h"
#include "settings/ui_expr_menu.h"

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
using mcla_menu::Bool;
using mcla_menu::ItemDef;
using mcla_menu::NativeOnly;
using mcla_menu::Str;

std::string Cvar(const char* name) { return larecomp::cvar::GetFlagByName(name); }
double CvarNumber(const char* name) { return std::strtod(Cvar(name).c_str(), nullptr); }
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

bool DlssListed(const UpscalerAvailability& a) {
    return a.dlss || a.dlss_needs_setup || a.dlss_after_restart;
}

using Entry = mcla_menu::GeneratedChoice;
using List = mcla_menu::GeneratedChoices;

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
    list.setting = kCvar;
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
    list.setting = kCvar;
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
    list.setting = kCvar;
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

// SHARPNESS: the RCAS pass's strength from 0 to 1. FSR 3.1 and FSR 4: OFF, 0.2 .. 1 in
// mcla_gpu_upscaler_sharpness. FSR 1: 0.2 .. 1 (it has no off: its softest is a quarter of the
// strength) in mcla_gpu_fsr_sharpness, which counts stops of reduction (0 sharpest, 2 softest):
// sharpness s is 2 - 2s stops, the way FSR 2 and 3 turn it into the same pass. A saved number that is not a step (FSR 1's default 0.2 stops is 0.9) is an entry of its own,
// in its place, while it is the setting.
List SharpnessList(bool fsr1) {
    List list;
    list.setting = fsr1 ? "mcla_gpu_fsr_sharpness" : "mcla_gpu_upscaler_sharpness";
    const double saved = fsr1 ? 1.0 - CvarNumber(list.setting) / 2.0 : CvarNumber(list.setting);
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
        list.entries.insert(list.entries.begin() + std::ptrdiff_t(at), Entry{buf, Cvar(list.setting)});
        list.current = int(at);
    }
    return list;
}

// ── The UPSCALING row ──────────────────────────────────────────────────────────────────────────

// ONE on / off row for the graphics card maker's denoiser (owner 2026-11-23; gpu/denoiser_setting.h has every rule and the words): "DLSS RAY RECONSTRUCTION" on a GeForce RTX card (ON is NVIDIA's DLSS
// Ray Reconstruction, which does the upscale too; OFF is ours), "AMD FSR RAY REGENERATION" on a Radeon RX 9000 series card on Windows 11 (ON is AMD's FSR Ray Regeneration, which works with any upscaler; OFF is
// ours). Any other card has no row and uses ours. A saved mcla_gpu_vendor_denoiser takes precedence over the hardware default. The old three-way chooser, mcla_gpu_denoiser, overrides it for debug runs.
// The row is grey, with its reason after its label (NVIDIA's guidelines: disable it where the driver cannot run it, where DLSS Super Resolution is not on and where there is no ray traced content): " (N/A)",
// " (NEEDS DLSS)", " (SETUP)", " (FAILED)", " (NOT NEEDED)" on an RTX card; on a Radeon " (D3D12 ONLY)" on Vulkan (AMD ships no Vulkan provider), " (N/A)", " (SETUP)", " (FAILED)", " (NOT NEEDED)". It says
// " (RESTART)" while the row is ON in a game whose Streamline was started without the plugin (the "Restart now?" prompt reads the word) and " (APPLYING)" while the denoiser's context is being made. It is grey
// (NOT NEEDED) while no effect that has a raw mode is on (RR-INT: the screen-space reflections, the cars' ray traced reflections): nothing is shown as unavailable that a restart or a setting would bring.
// Width: DISPLAY counts its widest label plus its widest value over every row (pause_menu.cpp "Page width"), 41 at most, and its widest value is 17, so no label is over 24: the row says its whole name while
// it says nothing ("DLSS RAY RECONSTRUCTION" 23, "AMD FSR RAY REGENERATION" 24) and its short name in front of a reason ("DLSS RR (NEEDS DLSS)" 20, "RAY REGEN (D3D12 ONLY)" 22).

mcla::gpu::denoiser::VendorRow DenoiserRowKind() {
    namespace gpu = mcla::gpu;
    return gpu::denoiser::RowFor(gpu::rayregen::RowListed(), gpu::denoiser::RtxAdapter());
}
// Whether this PC has the row at all: an RTX card, or a Radeon RX 9000 series card on Windows 11.
bool AmdRow() { return DenoiserRowKind() == mcla::gpu::denoiser::VendorRow::kRayRegen; }

mcla::gpu::denoiser::MenuInputs DenoiserInputs() {
    namespace dn = mcla::gpu::denoiser;
    const UpscalerAvailability a = mcla::gpu::GetUpscalerAvailability();
    const dn::Snapshot snapshot = dn::Current();
    dn::MenuInputs m;
    m.facts = snapshot.facts;
    m.dlss_listed = DenoiserRowKind() == dn::VendorRow::kDlssRr;
    m.dlss_unavailable = (a.dlss_probed && !DlssListed(a)) || (snapshot.valid && !m.facts.build_has_rr);
    m.dlss_chosen = Chosen() == Upscaler::kDlss;
    m.dlss_waits_for_restart = DlssWaitsForRestart();
    m.dlss_needs_setup = a.dlss_needs_setup && !a.dlss;
    m.debug = snapshot.valid ? snapshot.debug : mcla::gpu::rr::DebugSwitch::kRow;
    const auto preference = dn::ReadPreference();
    m.setting = preference.uses_default && !snapshot.default_on ? mcla::gpu::rr::Denoiser::kOurs : preference.setting;
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

// The mode row is listed on both renderers. Its variant and value list
// are selected by ui_rules.toml. The optional denoiser row is last.
constexpr ItemDef kRows[] = {
    NativeOnly(Str("PM_RxGpuUpsc", "mcla_gpu_upscaler", "UPSCALING: ", nullptr, nullptr, 0)),
    Str("PM_RxGpuUpscMode", nullptr, "RENDER SCALE: ", nullptr, nullptr, 0),
    NativeOnly(Str("PM_RxGpuUpscSharp", nullptr, "SHARPNESS: ", nullptr, nullptr, 0)),
    NativeOnly(Bool("PM_RxGpuDenoiser", "mcla_gpu_vendor_denoiser", "DLSS RAY RECONSTRUCTION: ")),
};
constexpr int kUpscalerRowCount = 3;
static_assert(sizeof(kRows) / sizeof(kRows[0]) == size_t(kUpscalerRowCount) + 1);

#endif  // LARECOMP_HAS_GPU

}  // namespace

const mcla_menu::ItemDef* UpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    count = kUpscalerRowCount + int(mcla_menu::PauseRowListed(kRows[kUpscalerRowCount]));
    return kRows;
#else
    count = 0;
    return nullptr;
#endif
}

const mcla_menu::ItemDef* AllUpscalingMenuRows(int& count) {
#if defined(LARECOMP_HAS_GPU)
    count = int(sizeof(kRows) / sizeof(kRows[0]));
    return kRows;
#else
    count = 0;
    return nullptr;
#endif
}

namespace mcla_menu {

GeneratedChoices RenderScaleChoices([[maybe_unused]] std::string_view renderer) {
#if defined(LARECOMP_HAS_GPU)
    return renderer == "xenia" ? XeniaList() : OffList();
#else
    return {};
#endif
}

GeneratedChoices Fsr1ModeChoices() {
#if defined(LARECOMP_HAS_GPU)
    return Fsr1List();
#else
    return {};
#endif
}

GeneratedChoices SharpnessChoices([[maybe_unused]] std::string_view setting) {
#if defined(LARECOMP_HAS_GPU)
    return SharpnessList(setting == "mcla_gpu_fsr_sharpness");
#else
    return {};
#endif
}

std::string_view VendorDenoiserKind() {
#if defined(LARECOMP_HAS_GPU)
    return mcla::gpu::denoiser::VendorName(DenoiserRowKind());
#else
    return "none";
#endif
}

std::string_view VendorDenoiserState() {
#if defined(LARECOMP_HAS_GPU)
    if (AmdRow()) {
        using Gate = mcla::gpu::rayregen::Gate;
        switch (AmdGate()) {
        case Gate::kApplying: return "making";
        case Gate::kHidden: return "hidden";
        case Gate::kNeedsD3D12: return "needs_d3d12";
        case Gate::kNotAvailable: return "unsupported";
        case Gate::kSetup: return "needs_setup";
        case Gate::kFailed: return "failed";
        case Gate::kNotNeeded: return "not_needed";
        default: return "ok";
        }
    }
    using Gate = mcla::gpu::denoiser::Gate;
    switch (DenoiserGate()) {
    case Gate::kApplying: return "making";
    case Gate::kRestart: return "restart";
    case Gate::kHidden: return "hidden";
    case Gate::kNotAvailable: return "unsupported";
    case Gate::kNeedsDlss: return "needs_dlss";
    case Gate::kSetup: return "needs_setup";
    case Gate::kFailed: return "failed";
    case Gate::kNotNeeded: return "not_needed";
    default: break;
    }
#endif
    return "ok";
}

bool VendorDenoiserOn() {
#if defined(LARECOMP_HAS_GPU)
    namespace dn = mcla::gpu::denoiser;
    namespace rg = mcla::gpu::rayregen;
    const auto saved = dn::PlayerPreference();
    switch (DenoiserRowKind()) {
    case dn::VendorRow::kDlssRr:
        return dn::ToggleOn(saved, dn::Current().default_on);
    case dn::VendorRow::kRayRegen:
        return dn::ToggleOn(saved, rg::DefaultOn(rg::MenuFacts(), rg::ReadDebugSwitch()));
    case dn::VendorRow::kNone:
        break;
    }
#endif
    return false;
}

}  // namespace mcla_menu
