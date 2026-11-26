#pragma once

#include "gpu/gpu_class.h"
#include "gpu/rt_tier.h"
#include "ui_expr.h"

namespace larecomp::uiexpr {

using AutoCatalog = std::map<std::string, AutoDescriptor, std::less<>>;

inline AutoCatalog MenuAutoDescriptors() {
    namespace gc = mcla::gpu::gpu_class;
    constexpr gc::GpuClass classes[] = {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh};
    constexpr gc::RtTier rungs[] = {gc::RtTier::kNone, gc::RtTier::kWeak, gc::RtTier::kStrong};
    AutoCatalog result;
    for (int i = 0; i < gc::kEffectCount; ++i) {
        const auto effect = gc::Effect(i);
        if (effect == gc::Effect::kRtShadows) continue;
        auto& descriptor = result[std::string(gc::Info(effect).cvar)];
        descriptor.by = "class";
        for (const auto cls : classes) {
            const std::string key(gc::ClassName(cls));
            descriptor.cells[key] = Value::String(std::string(gc::CellFor(effect, cls, false)));
        }
    }
    for (int i = 0; i < gc::kScaleCount; ++i) {
        const auto scale = gc::Scale(i);
        const auto& info = gc::ScaleOf(scale);
        auto& descriptor = result[std::string(info.cvar)];
        descriptor.by = "class";
        descriptor.has_auto_value = true;
        descriptor.auto_value = info.zero_is_auto ? 0.0 : -1.0;
        for (const auto cls : classes)
            descriptor.cells[std::string(gc::ClassName(cls))] = Value::Number(gc::ScaleCell(scale, cls));
    }
    for (int i = 0; i < gc::kRtEffectCount; ++i) {
        const auto effect = gc::RtEffect(i);
        const std::string cvar(gc::RtInfo(effect).cvar);
        // A cell that is empty at the none rung defers to the class table (mcla_gpu_ao: RTAO adds the `rt` cell at the strong rung only, which
        // the row's AUTO label rule names): the class descriptor stays, or `resolved(s.mcla_gpu_ao)` would be "" on every card without ray tracing.
        if (gc::RtCell(effect, gc::RtTier::kNone).empty() && result.count(cvar)) continue;
        auto& descriptor = result[cvar];
        descriptor.by = "rt_rung";
        for (const auto rung : rungs)
            descriptor.cells[std::string(gc::RtTierName(rung))] = Value::String(std::string(gc::RtCell(effect, rung)));
    }
    return result;
}

}  // namespace larecomp::uiexpr
