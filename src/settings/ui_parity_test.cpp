// Compare the pause menu overlays with the decisions made before migration.
// The test loads the authored TOML and uses the same rule interpreter as the game.
#include "ui_expr_auto.h"
#include "ui_expr_rules.h"

#include "gpu/city_refl_setting.h"
#include "gpu/city_rt_rules.h"
#include "gpu/denoiser_setting.h"
#include "gpu/gi_setting.h"
#include "gpu/ray_regen_rules.h"
#include "gpu/rtao_setting.h"
#include "gpu/upscale_modes.h"
#include "mc_engine/menus/pause_parity_rows.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using namespace larecomp::uiexpr;
namespace gc = mcla::gpu::gpu_class;
namespace city = mcla::gpu::city_refl_setting;
namespace gi = mcla::gpu::gi_setting;
namespace rtao = mcla::gpu::rtao_setting;
constexpr bool flags[] = {false, true};
constexpr std::string_view levels[] = {"auto", "off", "high", "ultra"};
constexpr std::string_view ao_levels[] = {"auto", "off", "on", "high", "ultra", "rt"};
constexpr gc::GpuClass classes[] = {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh};
constexpr gc::RtTier tiers[] = {gc::RtTier::kNone, gc::RtTier::kWeak, gc::RtTier::kStrong};
int checks = 0;
int failures = 0;

void Check(bool equal, std::string_view row, std::string_view field) {
    ++checks;
    if (!equal) {
        if (failures < 25) std::printf("FAIL %.*s %.*s\n", int(row.size()), row.data(), int(field.size()), field.data());
        ++failures;
    }
}

struct Fixture final : Env {
    const RuleBook& rules;
    const AutoCatalog autos = MenuAutoDescriptors();
    std::map<std::string, Value, std::less<>> settings, facts;
    bool native = true;

    explicit Fixture(const RuleBook& rules) : rules(rules) {}
    Value Setting(std::string_view key) const override { return Get(settings, key); }
    Value Fact(std::string_view key) const override { return Get(facts, key); }
    Value Renderer() const override { return Value::String(native ? "native" : "xenia"); }
    const AutoDescriptor* Auto(std::string_view key) const override {
        auto it = autos.find(key);
        return it == autos.end() ? nullptr : &it->second;
    }
    const Def* DefOf(std::string_view key) const override { return rules.Definition(key); }

    static Value Get(const std::map<std::string, Value, std::less<>>& values, std::string_view key) {
        auto it = values.find(key);
        return it == values.end() ? Value{} : it->second;
    }
    void S(std::string_view key, std::string_view value) {
        settings[std::string(key)] = Value::String(std::string(value));
    }
    void B(const char* key, bool value) { facts[key] = Value::Bool(value); }
    void F(const char* key, std::string_view value) { facts[key] = Value::String(std::string(value)); }
    void N(const char* key, double value) { facts[key] = Value::Number(value); }
    RuleView Row(std::string_view id) const {
        const auto* rule = rules.Row(id);
        if (!rule) throw std::runtime_error("Missing row " + std::string(id));
        return RuleView(*rule, *this, &rules);
    }
    void State(std::string_view id, bool enabled, std::string_view suffix = {},
               std::string_view label = {}, bool restart = false) const {
        const auto row = Row(id);
        const auto* state = row.State();
        Check((!state || (*state)["enabled"].value_or(true)) == enabled, id, "enabled");
        const auto suffix_template = state ? (*state)["suffix"].value_or(std::string_view{}) : std::string_view{};
        Check(ExpandTemplate(suffix_template, *this).value_or("") == suffix, id, "suffix");
        const auto variant = row.Field("label").value_or(std::string_view{});
        Check((state ? (*state)["label"].value_or(variant) : variant) == label, id, "label override");
        Check((state && (*state)["needs_restart"].value_or(false)) == restart, id, "restart");
    }
    void Decodes(std::string_view id, std::string_view expected) const {
        const auto row = Row(id);
        Check(ValueText(Decode(row.Owner("decode").decode, *this)) == expected, id, "decode");
    }
    void Label(std::string_view id, std::string_view value, std::string_view setting, std::string_view expected,
               std::optional<double> number = {}, const char* format = "%g", std::string_view fallback = "AUTO") const {
        const auto row = Row(id);
        const toml::table* entry = nullptr;
        for (const auto* choice : row.Choices())
            if ((*choice)["id"].value_or(std::string_view{}) == value) entry = choice;
        Check(row.Label(value, entry, fallback, setting, number, format).value_or("") == expected, id, "value label");
    }
};

std::vector<std::string> Ids(const RuleView& row) {
    std::vector<std::string> result;
    for (const auto* item : row.Choices()) result.push_back((*item)["id"].value_or(std::string{}));
    return result;
}

void Lighting(Fixture& f) {
    constexpr const char* class_rows[] = {
        "PM_RxGpuShadowFilter", "PM_RxGpuShadowDetail", "PM_RxGpuRoadRefl",
        "PM_RxGpuContact", "PM_RxGpuSunShafts", "PM_RxGpuSsgi", "PM_RxGpuTrafficShadows", nullptr,
        nullptr /* the occlusion: its AUTO is the class cell or `rt` (rtao_setting.h), held below with the other labels */,
        "PM_RxGpuHeadlights", "PM_RxGpuLightShadows", "PM_RxGpuVolumetrics", nullptr,
        "PM_RxGpuSsr", "PM_RxGpuNeonLight", "PM_RxGpuCarShadowMap",
    };
    static_assert(std::size(class_rows) == gc::kEffectCount);
    Check(Ids(f.Row("PM_RxGpuCarRefl")) == (std::vector<std::string>{"off", "on"}),
          "CAR REFLECTIONS", "same On/Off choices on every GPU");
    for (const auto cls : classes) for (const auto tier : tiers) for (bool handheld : flags) {
        f.F("gpu.class", gc::ClassName(cls));
        f.F("gpu.rt_tier", gc::RtTierName(tier));
        f.F("gpu.rt_rung", gc::RtTierName(gc::RtRung(cls, tier)));
        f.B("gpu.handheld", handheld);
        for (int i = 0; i < gc::kEffectCount; ++i) {
            const auto effect = gc::Effect(i);
            const auto cvar = gc::CvarName(effect);
            if (class_rows[i]) {
                f.S(cvar, "auto");
                f.Decodes(class_rows[i], gc::CellFor(effect, cls, handheld));
            }
        }
        f.S("mcla_gpu_rt_reflections", "auto");
        f.Decodes("PM_RxGpuRtCityRefl", "on");
        constexpr std::string_view scale_rows[] = {
            "PM_RxLodCity", "PM_RxLodTraffic", "PM_RxUnspawn", "PM_RxPedDensity", "PM_RxParkedCars",
        };
        for (int i = 0; i < gc::kScaleCount; ++i) {
            const auto scale = gc::Scale(i);
            const auto& info = gc::ScaleOf(scale);
            f.settings[std::string(info.cvar)] = Value::Number(info.zero_is_auto ? 0.0 : -1.0);
            f.Decodes(scale_rows[i], FormatNumber(gc::ScaleCell(scale, cls)));
        }
        for (bool shadows : flags) {
            f.settings["perf_no_shadows"] = Value::Bool(!shadows);
            for (const auto row : {"PM_RxFoliage", "PM_RxGpuShadowDetail", "PM_RxGpuShadowFilter", "PM_RxGpuTrafficShadows"})
                f.State(row, shadows, shadows ? "" : " (SUN OFF)");
            f.Label("PM_RxGpuShadowFilter", "pcss", "", "ULTRA", {}, "%g", "ULTRA");
        }
        for (bool api : flags) for (const auto contact : levels) {
            f.B("api.city_rt_shadows", api);
            f.S("mcla_gpu_contact_shadows", contact);
            const auto resolved = gc::ResolveFor(cls, handheld, gc::Effect::kContactShadows, contact);
            const bool available = api && tier != gc::RtTier::kNone;
            const auto suffix = available ? "" : " (NO RT GPU)";
            f.State("PM_RxGpuRtShadows", available && (resolved == "high" || resolved == "ultra"), suffix);
            f.S("mcla_gpu_rt_shadows", "auto");
            f.Decodes("PM_RxGpuRtShadows", "off");
        }
        for (bool built : flags) for (const auto ao : ao_levels) for (const auto ss : levels)
            for (const auto rt : levels) for (bool replaced : flags) {
                const auto ss_word = gc::ResolveFor(cls, handheld, gc::Effect::kSsgi, ss);
                // RTAO: the occlusion is on when its decision keeps a screen level or runs rays (the pass's own readiness is its own fact, built here as the bounce light's)
                const auto ao_decision = rtao::Decide(cls, true, tier, rtao::ParseChoice(ao), built);
                const bool ao_on = ao_decision.screen != mcla::gpu::host_plan::Level::kOff || ao_decision.rt;
                const auto decision = gi::Decide(cls, tier, rt, built);
                f.B("build.rt_gi_update_built", built);
                f.B("live.rt_gi_replaces_bounce", replaced);
                f.B("live.rtao_pass_built", built);
                f.S("mcla_gpu_ao", ao);
                f.S("mcla_gpu_ssgi", ss);
                f.S("mcla_gpu_rt_gi", rt);
                f.State("PM_RxGpuAoLevel", ao_on);
                if (ao == "auto" || ao == "high" || ao == "ultra") f.Decodes("PM_RxGpuAo", "on");
                for (const auto fixed : {rtao::Choice::kOff, rtao::Choice::kOn, rtao::Choice::kRt})
                    f.Label("PM_RxGpuAo", rtao::kValues[uint8_t(fixed)], "mcla_gpu_ao",
                            fixed == rtao::Choice::kRt && ao_decision.gate == rtao::Gate::kRuns ? "RT" : rtao::ValueLabel(fixed, ao_decision.gate),
                            {}, "%g", fixed == rtao::Choice::kRt ? "RT" : rtao::kNames[uint8_t(fixed)]);
                f.State("PM_RxGpuSsgi", ao_on, replaced ? gi::kReplacedSuffix : "");
                f.State("PM_RxGpuRtGi", gi::RowEnabled(decision.gate), gi::RowSuffix(decision.gate));
                if (rt == "auto") f.Decodes("PM_RxGpuRtGi", "off");
                f.State("PM_RxGpuSsgiLevel", gi::AnyBounceLight(ao_on && (ss_word == "high" || ss_word == "ultra"), decision));
            }
        for (auto word : {"auto", "off", "on"}) {
            f.S("mcla_gpu_headlights", word);
            f.State("PM_RxGpuLightShadows", gc::ResolveFor(cls, handheld, gc::Effect::kHeadlights, word) == "on");
        }
        for (const auto pair : {std::pair{"PM_RxGpuCarAoRt", "mcla_gpu_ao_rt"},
                                std::pair{"PM_RxGpuRtLights", "mcla_gpu_rt_lights"},
                                std::pair{"PM_RxGpuSsrRt", "mcla_gpu_ssr_rt"}}) {
            f.S(pair.second, "auto");
            f.Decodes(pair.first, "off");
        }
        for (bool api : flags) for (auto car_word : {"auto", "on", "off"}) for (auto rt_word : {"auto", "off", "on"}) {
            f.B("api.car_rt", api);
            f.S("mcla_gpu_car_selfrefl", car_word);
            f.S("mcla_gpu_car_selfrefl_rt", rt_word);
            const auto base = gc::ResolveFor(cls, handheld, gc::Effect::kCarSelfRefl, car_word);
            const bool rt = std::string_view(rt_word) == "on";
            f.Decodes("PM_RxGpuCarSelfRefl", base);
            f.Decodes("PM_RxGpuCarSelfReflRt", rt ? "on" : "off");
            const bool car_rt = api && base == "on" && rt;
            for (const auto city_word : {"auto", "off", "on", "high", "ultra"}) for (auto data : {city::Data::kUnknown, city::Data::kReady, city::Data::kBuilding, city::Data::kUnavailable}) {
                f.F("live.city_data", data == city::Data::kBuilding ? "building" :
                    data == city::Data::kUnavailable ? "unavailable" : data == city::Data::kReady ? "ready" : "unknown");
                f.S("mcla_gpu_rt_reflections", city_word);
                const auto decision = city::DecidePaint(cls, tier, city_word, car_rt, data);
                f.State("PM_RxGpuRtCityRefl", city::RowEnabled(decision.gate), city::RowSuffix(decision.gate),
                        decision.gate == city::Gate::kRuns ? "" : city::RowLabel(decision.gate));
            }
        }
    }
}

void Display(Fixture& f) {
    for (bool fullscreen : flags) for (bool exclusive : flags) for (bool supported : flags)
        for (bool fixed : flags) for (bool resize : flags) for (bool size : flags) {
            f.settings["fullscreen"] = Value::Bool(fullscreen);
            f.settings["fullscreen_exclusive"] = Value::Bool(exclusive);
            f.B("os.exclusive_fullscreen", supported);
            f.B("os.window_size_fixed", fixed);
            f.B("os.live_window_resize", resize);
            f.facts["display.primary.size"] = size ? Value::String("3840X2160") : Value{};
            f.facts["live.output_size"] = size ? Value::String("1280X800") : Value{};
            const auto mode = !fullscreen ? "windowed" : exclusive && supported ? "fullscreen" : "borderless";
            const auto row = f.Row("PM_RxDisplayMode");
            Check(ValueText(Decode(row.Owner("decode").decode, f)) == mode, "DISPLAY MODE", "decode");
            Check(row.Choices().size() == (supported ? 3u : 2u), "DISPLAY MODE", "count");
            const auto resolution = f.Row("PM_RxResolution");
            Check(resolution.Field("values_from")["generator"].value_or("") == std::string_view("display.sizes"),
                  "RESOLUTION", "generator");
            Check(resolution.Field("setting").value_or("") ==
                    std::string_view(std::string_view(mode) == "fullscreen" ? "fullscreen_resolution" : ""),
                  "RESOLUTION", "write target");
            const bool borderless = std::string_view(mode) == "borderless";
            const bool restart = !fullscreen && !fixed && !resize;
            const auto suffix = borderless ? (size ? " (3840X2160)" : " (DISPLAY)") :
                !fullscreen && fixed ? (size ? " (1280X800)" : " (FIXED)") : restart ? " (RESTART)" : "";
            f.State("PM_RxResolution", !borderless && !fixed, suffix, {}, restart);
        }
    for (bool hdr : flags) for (bool wide : flags) for (bool portuguese : flags) {
        f.B("live.hdr_active", hdr);
        f.B("live.ultrawide_active", wide);
        f.B("install.portuguese_text", portuguese);
        f.N("features.hdr.auto_paper_white_nits", 240);
        f.N("features.hdr.auto_peak_nits", 1000);
        for (auto row : {"PM_RxGpuHdrWhite", "PM_RxGpuHdrPeak", "PM_RxGpuHdrHighlights", "PM_RxGpuHdrTest"})
            f.State(row, hdr, hdr ? "" : " (HDR OFF)");
        f.settings["mcla_gpu_hdr_paper_white_nits"] = Value::Number(0);
        f.settings["mcla_gpu_hdr_peak_nits"] = Value::Number(0);
        f.Decodes("PM_RxGpuHdrWhite", "240");
        f.Decodes("PM_RxGpuHdrPeak", "1000");
        f.State("PM_RxHudWidth", wide, wide ? "" : " (16X9: NO EFFECT)");
        Check(f.Row("PM_RxLanguage").Choices().size() == (portuguese ? 7u : 6u), "LANGUAGE", "count");
    }
    for (bool forced : flags) for (const auto driver : {"larecomp", "d3d12", "vulkan"}) {
        f.B("live.renderer_forced", forced);
        f.F("live.renderer_choice", driver);
        f.State("PM_RxDbgRenderer", !forced, forced ? " (LAUNCH OPTION)" : " (RESTART)", {}, !forced);
        const auto row = f.Row("PM_RxDbgRenderer");
        Check(ValueText(Decode(row.Owner("decode").decode, f)) == driver, "RENDERER", "decode");
    }
    for (auto row : {"PM_RxTrigThrottle", "PM_RxTrigBrake"}) for (int n = 1; n <= 8; ++n)
        f.Label(row, NumberId(n), "", mcla_menu::parity::TriggerStrengthLabel(n), n, "%.0f", NumberId(n));
    f.Label("PM_RxAiDiff", "-1", "", "STOCK");
    f.Label("PM_RxRbNitroStart", "-1", "", "STOCK");
    f.Label("PM_RxGpuHdrHighlights", "1", "", "NORMAL");
    f.Label("PM_RxGpuHdrHighlights", "1.5", "", "BRIGHT");
    f.Label("PM_RxGpuHdrHighlights", "2", "", "BRIGHTER");
    f.Label("PM_RxGpuHdrHighlights", "3", "", "BRIGHTEST");
    f.Label("PM_RxHudWidth", "0", "", "CENTERED");
    f.Label("PM_RxHudWidth", "50", "", "WIDER");
    f.Label("PM_RxHudWidth", "100", "", "FULL");
}

void Upscaling(Fixture& f) {
    for (bool native : flags) for (auto chosen : {"off", "fsr31", "fsr4", "dlss"})
        for (bool fsr1 : flags) for (bool fsr31 : flags) for (bool fsr4 : flags)
            for (auto dlss : {"no", "ready", "needs_setup", "after_restart"})
                for (auto kept : {"off", "fsr31", "fsr4"}) for (bool switching : flags) {
                    f.native = native;
                    f.S("mcla_gpu_upscaler", chosen);
                    f.settings["mcla_gpu_fsr"] = Value::Bool(fsr1);
                    f.settings["disable_msaa"] = Value::Bool(switching);
                    f.B("features.upscalers.fsr31", fsr31);
                    f.B("features.upscalers.fsr4", fsr4);
                    f.F("features.upscalers.dlss", dlss);
                    f.B("live.dlss_waits_for_restart", std::string_view(dlss) == "after_restart");
                    f.F("live.kept_upscaler", kept);
                    f.B("live.msaa_switching", switching);
                    f.F("live.upscaler_state", switching ? "switching" : "ok");
                    const auto selected = std::string_view(chosen);
                    const auto status = std::string_view(dlss);
                    const bool restart = selected == "dlss" && status == "after_restart";
                    const bool ready = (selected == "dlss" && status == "ready") ||
                        (selected == "fsr31" && fsr31) || (selected == "fsr4" && fsr4);
                    const std::string_view shown = ready || (selected == "dlss" && status != "no")
                        ? selected : fsr1 ? "fsr1" : "off";
                    const std::string_view active = ready ? selected : restart ? kept : "off";
                    const auto aa_suffix = active == "fsr31" ? " (FSR 3.1 ON)" : active == "fsr4"
                        ? " (FSR 4 ON)" : active == "dlss" ? " (DLSS ON)" : "";
                    const auto upsc = f.Row("PM_RxGpuUpsc");
                    Check(ValueText(Decode(upsc.Owner("decode").decode, f)) == shown, "UPSCALING", "decode");
                    Check(upsc.Choices().size() == size_t(2 + fsr31 + fsr4 + (status != "no")), "UPSCALING", "count");
                    f.State("PM_RxGpuUpsc", true, restart ? " (RESTART)" : switching ? " (APPLYING)" : "", {}, restart);
                    f.State("PM_RxGpuAa", active == "off", aa_suffix);
                    f.State("PM_RxMsaa", !native || active == "off", !native ? " (RESTART)" :
                            active != "off" ? aa_suffix : switching ? " (APPLYING)" : "", {}, !native);
                    const auto mode = !native || shown == "off" ? "RENDER SCALE" : shown == "dlss" ? "DLSS MODE" :
                        shown == "fsr1" ? "FSR 1 MODE" : shown == "fsr31" ? "FSR 3.1 MODE" : "FSR 4 MODE";
                    const bool dlss_grey = native && shown == "dlss" && active != "dlss";
                    // A kept upscaler can only be a previously running FSR or off.
                    f.State("PM_RxGpuUpscMode", !dlss_grey, !native || (dlss_grey && restart) ?
                            " (RESTART)" : dlss_grey && status == "needs_setup" ? " (SETUP)" : "",
                            mode, !native || (dlss_grey && restart));
                    const bool sharp = shown == "fsr1" || active == "fsr31" || active == "fsr4";
                    f.State("PM_RxGpuUpscSharp", sharp, sharp ? "" : " (FSR ONLY)");
                }
    f.native = true;
    for (auto mode : {"fsr31", "fsr4"}) {
        f.S("mcla_gpu_upscaler", mode);
        f.B("features.upscalers.fsr31", true);
        f.B("features.upscalers.fsr4", true);
        const auto row = f.Row("PM_RxGpuUpscMode");
        Check(row.Choices().size() == std::size(mcla::gpu::upscale_modes::kFsrModes),
              "FSR MODE", "shared value set");
        for (const auto& value : mcla::gpu::upscale_modes::kFsrModes) {
            const std::string_view name = value.name;
            const std::string base(name == "ULTRA PERFORMANCE" ? "ULTRA PERF." : name);
            f.Label("PM_RxGpuUpscMode", value.cvar, "", base + " (" + std::to_string(value.percent) + "%)");
        }
    }
    f.S("mcla_gpu_upscaler", "off");
    f.settings["mcla_gpu_fsr"] = Value::Bool(true);
    const auto fsr1 = f.Row("PM_RxGpuUpscMode");
    Check(fsr1.Field("values_from")["generator"].value_or("") == std::string_view("render_scale.fsr1_modes"),
          "FSR 1 MODE", "generator");
    for (bool custom : flags) {
        f.B("live.fsr1_preset_custom", custom);
        const auto choices = f.Row("PM_RxGpuUpsc").Choices();
        const auto* writes = (*choices[1])["writes"].as_array();
        Check(writes && writes->size() == (custom ? 3u : 2u), "FSR 1", "write default only for custom");
        if (writes && !writes->empty()) {
            Check((*writes->get(0)->as_table())["setting"].value_or("") == std::string_view("mcla_gpu_upscaler"),
                  "FSR 1", "disable the old upscaler first");
            Check((*writes->get(writes->size() - 1)->as_table())["setting"].value_or("") == std::string_view("mcla_gpu_fsr"),
                  "FSR 1", "enable the new upscaler last");
        }
    }
    f.S("mcla_gpu_upscaler", "dlss");
    f.F("features.upscalers.dlss", "ready");
    f.B("live.dlss_waits_for_restart", false);
    for (bool refused : flags) {
        std::vector<Value> ids;
        if (refused) for (const auto& mode : mcla::gpu::upscale_modes::kDlssModes) ids.push_back(Value::String(mode.cvar));
        f.facts["features.upscalers.dlss_modes_refused"] = Value::List(std::move(ids));
        for (const auto& mode : mcla::gpu::upscale_modes::kDlssModes) {
            const std::string_view name = mode.name;
            const std::string base(name == "ULTRA PERFORMANCE" ? "ULTRA PERF." : name);
            f.Label("PM_RxGpuUpscMode", mode.cvar, "", base + (refused ? " (N/A)" :
                    " (" + std::to_string(mode.percent) + "%)"));
        }
    }
}

void FrameGenerationAndLatency(Fixture& f) {
    for (auto kind : {"none", "reflex", "antilag2"}) for (auto health : {"ok", "paused", "failed"})
        for (bool auto_on : flags) for (bool running : flags) {
            f.F("features.latency.kind", kind);
            f.F("live.latency_health", health);
            f.B("features.latency.auto_on", auto_on);
            f.B("live.frame_generation_running", running);
            f.State("PM_RxFpsCap", !running, running ? " (FRAME GEN)" : "");
            const auto suffix = running ? " (FRAME GEN)" : std::string_view(health) == "paused" ?
                " (PAUSED)" : std::string_view(health) == "failed" ? " (NOT WORKING)" : "";
            for (const auto& [row, expected, label] : {
                    std::tuple{"PM_RxGpuReflex", "reflex", "REFLEX"},
                    std::tuple{"PM_RxGpuAntiLag", "antilag2", "ANTI-LAG 2"}}) {
                Check(f.Row(row).IsListed() == (std::string_view(kind) == expected), row, "listed");
                f.State(row, !running, suffix, suffix[0] ? label : "");
                f.S("mcla_gpu_low_latency", "auto");
                f.Decodes(row, auto_on ? "on" : "off");
            }
        }
    for (auto chosen : {"off", "fsr31", "dlss"})
        for (bool fsr : flags) for (bool dlss : flags) for (bool applying : flags)
            for (bool restart : flags) for (int multiplier = 2; multiplier <= 4; ++multiplier) {
                f.S("mcla_gpu_frame_generation", chosen);
                f.B("live.fsr_frame_generation_listed", fsr);
                f.B("live.dlss_frame_generation_listed", dlss);
                f.B("features.frame_generation.dlss_after_restart", restart);
                f.B("live.frame_generation_applying", applying);
                f.B("live.frame_generation_multiplier_listed", dlss && multiplier > 2);
                f.N("live.frame_generation_multiplier", multiplier);
                f.N("features.frame_generation.dlss_multiplier_max", multiplier);
                const bool picked_dlss = std::string_view(chosen) == "dlss";
                const bool picked_fsr = std::string_view(chosen) == "fsr31";
                const bool needs_restart = picked_dlss && restart;
                const bool cap_enabled = (picked_dlss && dlss && !restart) || (picked_fsr && fsr);
                f.B("live.frame_generation_choice_available", cap_enabled);
                const auto row = f.Row("PM_RxGpuFrameGen");
                Check(row.IsListed() == (fsr || dlss), "FRAME GENERATION", "listed");
                Check(row.Choices().size() == size_t(1 + (fsr || picked_fsr) + (dlss || picked_dlss)),
                      "FRAME GENERATION", "count");
                f.State("PM_RxGpuFrameGen", true, needs_restart ? " (RESTART)" : applying ? " (APPLYING)" : "",
                        needs_restart || applying ? "FRAME GEN" : "", needs_restart);
                f.State("PM_RxGpuFrameGenMult", picked_dlss);
                Check(f.Row("PM_RxGpuFrameGenMult").Choices().size() == size_t(multiplier - 1), "FRAME GEN MULTIPLIER", "count");
                f.State("PM_RxGpuFrameGenFps", cap_enabled);
                const int shown = picked_dlss ? multiplier : 2;
                f.Label("PM_RxGpuFrameGenFps", "30", "", "30 (" + std::to_string(30 * shown) + " SHOWN)", 30);
                f.N("live.frame_generation_half_display", 116);
                f.Label("PM_RxGpuFrameGenFps", "-1", "", shown == 2 ? "HALF DISPLAY 116" :
                        "1/" + std::to_string(shown) + " DISPLAY 116", -1);
                f.N("live.frame_generation_half_display", 0);
                f.Label("PM_RxGpuFrameGenFps", "-1", "", "USE FPS LIMIT", -1);
            }
}

void CityCoverageWarning(Fixture& f) {
    namespace rt = mcla::gpu::city_rt;
    using enum rt::CoveragePressure;
    constexpr auto row = "PM_RxGpuPreset";
    constexpr auto fact = "live.city_rt_coverage";
    f.F("gpu.rt_tier", "strong");
    f.S("mcla_gpu_preset", "custom");
    f.State(row, true);
    f.F(fact, rt::CoveragePressureName(kBlasCap));
    f.State(row, true, "", "PRESET (CITY RT CAP)");
    f.F(fact, rt::CoveragePressureName(kHitPool));
    f.State(row, true, "", "PRESET (CITY RT POOL)");
    f.F("gpu.vram_warning", "SCL/MSAA");
    f.State(row, true, "", "PRESET (VRAM! {gpu.vram_warning})");
    f.F(fact, rt::CoveragePressureName(kAllocation));
    f.State(row, true, "", "PRESET (CITY RT ALLOC)");
    f.facts.erase("gpu.vram_warning");
    f.State(row, true, "", "PRESET (CITY RT ALLOC)");
    f.facts.erase(fact);
    f.State(row, true);
}

void Denoiser(Fixture& f) {
    namespace rr = mcla::gpu::denoiser;
    namespace rg = mcla::gpu::rayregen;
    constexpr const char* nvidia[] = {"ok", "ok", "making", "restart", "hidden", "unsupported",
                                    "needs_dlss", "needs_setup", "failed", "not_needed"};
    constexpr const char* amd[] = {"ok", "ok", "making", "hidden", "needs_d3d12", "unsupported",
                                 "needs_setup", "failed", "not_needed"};
    static_assert(std::size(nvidia) == rr::kGateCount && std::size(amd) == rg::kGateCount);
    f.F("features.denoiser.vendor", "nvidia");
    for (size_t i = 0; i < std::size(nvidia); ++i) {
        const auto gate = rr::Gate(i);
        f.F("live.denoiser_gate", nvidia[i]);
        f.State("PM_RxGpuDenoiser", rr::RowEnabled(gate), rr::RowSuffix(gate),
                rr::RowSuffix(gate)[0] ? rr::RowLabel(gate) : "", gate == rr::Gate::kRestart);
    }
    f.F("features.denoiser.vendor", "amd");
    for (size_t i = 0; i < std::size(amd); ++i) {
        const auto gate = rg::Gate(i);
        f.F("live.denoiser_gate", amd[i]);
        f.State("PM_RxGpuDenoiser", rg::RowEnabled(gate), rg::RowSuffix(gate), rg::RowLabel(gate));
    }
    f.F("features.denoiser.vendor", "none");
    Check(!f.Row("PM_RxGpuDenoiser").IsListed(), "DENOISER", "listed");

    constexpr auto id = "PM_RxGpuDenoiser";
    f.native = false;
    f.F("features.denoiser.vendor", "nvidia");
    Check(!f.Row(id).IsListed(), "DENOISER", "hidden on Xenia");
    f.native = true;
    f.facts.erase("features.denoiser.vendor");
    f.facts.erase("live.denoiser_gate");
    f.F("gpu.vendor", "intel");
    Check(!f.Row(id).IsListed(), "DENOISER", "hidden on Intel");
    f.F("gpu.vendor", "nvidia");
    Check(!f.Row(id).IsListed(), "DENOISER", "unknown NVIDIA generation");
    f.B("gpu.rtx", false);
    Check(!f.Row(id).IsListed(), "DENOISER", "hidden on GTX");
    f.B("gpu.rtx", true);
    Check(f.Row(id).IsListed(), "DENOISER", "RTX listed by hardware");
    f.F("features.denoiser.dlss_rr", "unsupported");
    f.S("mcla_gpu_upscaler", "off");
    f.State(id, false, " (N/A)", "DLSS RR");
    f.F("features.denoiser.dlss_rr", "after_restart");
    f.F("features.upscalers.dlss", "ready");
    f.S("mcla_gpu_upscaler", "dlss");
    f.settings["mcla_gpu_vendor_denoiser"] = Value::Bool(false);
    f.State(id, true);
    f.settings["mcla_gpu_vendor_denoiser"] = Value::Bool(true);
    f.State(id, true, " (RESTART)", "DLSS RR", true);

    // CHECK: Run this fixture in the Linux menu parity binary.
    f.F("os.name", "linux");
    f.F("api.name", "vulkan");
    f.F("features.upscalers.dlss", "no");
    Check(f.Row(id).IsListed(), "DENOISER", "RTX listed on Linux");
    f.State(id, false, " (N/A)", "DLSS RR");

    f.F("gpu.vendor", "amd");
    f.F("os.name", "windows");
    Check(!f.Row(id).IsListed(), "DENOISER", "unknown Radeon generation");
    f.B("gpu.rx9000", false);
    Check(!f.Row(id).IsListed(), "DENOISER", "older Radeon");
    f.F("features.denoiser.vendor", "amd");
    Check(!f.Row(id).IsListed(), "DENOISER", "older machine report tagged all Radeon cards as AMD");
    f.facts.erase("features.denoiser.vendor");
    f.B("gpu.rx9000", true);
    f.N("os.build", 19045);
    Check(!f.Row(id).IsListed(), "DENOISER", "Windows 10");
    f.N("os.build", 22000);
    f.F("api.name", "vulkan");
    Check(f.Row(id).IsListed(), "DENOISER", "RX 9000 listed on Windows 11");
    f.State(id, false, " (D3D12 ONLY)", "RAY REGEN");
    f.F("api.name", "d3d12");
    f.State(id, true, "", "AMD FSR RAY REGENERATION");
    f.F("os.name", "linux");
    Check(!f.Row(id).IsListed(), "DENOISER", "native Linux");
}
}  // namespace

int main(int argc, char** argv) {
    const auto file = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(__FILE__).parent_path() / "ui_rules.toml";
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Cannot open %s\n", file.string().c_str());
        return 1;
    }
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const RuleBook rules(source);
    Fixture fixture(rules);
    Lighting(fixture);
    Display(fixture);
    Upscaling(fixture);
    FrameGenerationAndLatency(fixture);
    Denoiser(fixture);
    CityCoverageWarning(fixture);
    std::printf("pause menu parity: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
