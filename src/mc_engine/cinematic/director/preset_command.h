#pragma once

#include <charconv>
#include <cmath>
#include <vector>
#include <optional>
#include <string>

#include "gpu/gfx_preset.h"
#include "json.h"

namespace mc::cinematic::director {

struct PresetEdit {
    std::optional<mcla::gpu::gfx_preset::Preset> preset;
    std::optional<bool> label;
    bool commit = false;
    bool persist = false;
    double t = 0.0;
    std::string error;

    std::string Line(bool is_label) const {
        const std::string value = is_label ? (label.value_or(false) ? "on" : "off") :
                                            std::string(mcla::gpu::gfx_preset::Id(preset.value_or(mcla::gpu::gfx_preset::Preset::kCustom)));
        char seconds[340];
        const auto conversion = std::to_chars(seconds, seconds + sizeof(seconds), t, std::chars_format::fixed, 9);
        return "at " + std::string(seconds, conversion.ptr) + (is_label ? " preset_label " : " preset ") + value;
    }
};

inline PresetEdit ParsePresetEdit(const Json& args, double playhead, bool is_label) {
    namespace gfx = mcla::gpu::gfx_preset;
    PresetEdit edit;
    edit.commit = args.BoolOr("commit", false);
    edit.persist = args.BoolOr("persist", false);
    edit.t = args.NumberOr("t", playhead);
    const char* field = is_label ? "on" : "name";
    if (args.BoolOr("clear", false)) {
        if (args.Has(field) || edit.commit || edit.persist || args.Has("t")) edit.error = "clear cannot be combined with a value, time or save";
    } else if (edit.commit && edit.persist) {
        edit.error = "commit and persist are mutually exclusive";
    } else if ((!edit.commit && args.Has("t")) || !std::isfinite(edit.t) || edit.t < 0.0) {
        edit.error = "'t' requires commit:true and a finite nonnegative scene time";
    } else if (is_label) {
        if (!args.Get(field).is_bool()) edit.error = "give 'on' or clear:true";
        else edit.label = args.BoolOr(field, false);
    } else {
        gfx::Preset preset;
        if (!gfx::Parse(args.StringOr(field, ""), &preset)) edit.error = "name must be vanilla, enhanced, enhanced_rt or custom";
        else edit.preset = preset;
    }
    return edit;
}

struct TakePresets {
    std::vector<mcla::gpu::gfx_preset::Preset> values;
    unsigned mask = 0;
    std::string error;
};

inline TakePresets ParseTakePresets(const Json& list) {
    namespace gfx = mcla::gpu::gfx_preset;
    TakePresets result;
    if (list.is_null()) return result;
    if (!list.is_array() || list.size() == 0 || list.size() > gfx::kPresetCount) {
        result.error = "presets must contain one to four distinct presets";
        return result;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& name = list.at(i);
        gfx::Preset preset;
        if (!name.is_string() || !gfx::Parse(name.AsString(), &preset) || (result.mask & (1u << gfx::Col(preset)))) {
            result.error = "presets must contain distinct preset names";
            return result;
        }
        result.values.push_back(preset);
        result.mask |= 1u << gfx::Col(preset);
    }
    return result;
}

}  // namespace mc::cinematic::director
