#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "host_plan_level.h"
#include "rt_tier.h"

namespace mcla::gpu::rtao_setting {
namespace gc = gpu_class;

enum class Choice : uint8_t { kAuto, kOff, kHigh, kUltra, kRt };

constexpr const char* kValues[5] = {"auto", "off", "high", "ultra", "rt"};
constexpr const char* kNames[5] = {"AUTO", "OFF", "HIGH", "ULTRA", "RAY TRACED"};

inline Choice ParseChoice(std::string_view word) {
  const std::string upper = gc::Upper(word);
  if (upper == "RT") return Choice::kRt;
  if (upper == "ULTRA") return Choice::kUltra;
  if (upper == "HIGH") return Choice::kHigh;
  if (upper == "OFF") return Choice::kOff;
  return Choice::kAuto;
}

enum class Gate : uint8_t { kRuns, kNoRt, kTooSlow, kNotBuilt };

constexpr Gate GateFor(gc::GpuClass cls, gc::RtTier tier, bool pass_built) {
  if (tier == gc::RtTier::kNone) return Gate::kNoRt;
  if (cls == gc::GpuClass::kLow) return Gate::kTooSlow;
  if (!pass_built) return Gate::kNotBuilt;
  return Gate::kRuns;
}

constexpr bool Available(Gate gate) { return gate == Gate::kRuns; }

struct Decision {
  host_plan::Level screen = host_plan::Level::kOff;
  bool rt = false;
  gc::RtTier rung = gc::RtTier::kNone;
  Gate gate = Gate::kNoRt;
  Choice chosen = Choice::kAuto;
};

inline Decision Decide(gc::GpuClass cls, bool class_known, gc::RtTier tier, Choice choice,
                       bool pass_built) {
  if (!class_known) cls = gc::GpuClass::kLow;
  Decision d;
  d.rung = gc::RtRung(cls, tier);
  d.gate = GateFor(cls, tier, pass_built);
  if (choice == Choice::kAuto) {
    const std::string_view rt_cell = gc::RtCell(gc::RtEffect::kRtAo, d.rung);
    choice = ParseChoice(rt_cell.empty() ? gc::Cell(gc::Effect::kAmbientOcclusion, cls) : rt_cell);
  }
  d.chosen = choice;
  d.rt = choice == Choice::kRt && Available(d.gate);
  if (choice == Choice::kUltra || choice == Choice::kRt) d.screen = host_plan::Level::kUltra;
  if (choice == Choice::kHigh) d.screen = host_plan::Level::kHigh;
  return d;
}

// Resolve using the current adapter and the pass readiness published by the renderer.
Decision DecideFor(std::string_view setting_text);
bool PassBuilt();
void NotePassBuilt(bool built);

constexpr std::string_view GateReason(Gate gate) {
  switch (gate) {
    case Gate::kNoRt: return "NO RT GPU";
    case Gate::kTooSlow: return "TOO SLOW";
    case Gate::kNotBuilt: return "NOT READY";
    default: return {};
  }
}

inline std::string AutoLabel(const Decision& d) {
  return gc::AutoLabelOf(d.rt ? "rt" : host_plan::LevelName(d.screen));
}

inline std::string ValueLabel(Choice c, Gate gate) {
  if (c == Choice::kRt && !Available(gate)) {
    return "RT (" + std::string(GateReason(gate)) + ")";
  }
  return kNames[static_cast<uint8_t>(c)];
}

inline std::string AutoDescription() {
  const auto title = [](std::string_view word) {
    if (word == "rt") return std::string("Ray traced");
    std::string text(word);
    if (!text.empty()) text[0] = gc::Upper(word.substr(0, 1))[0];
    return text;
  };
  constexpr auto ao = gc::Effect::kAmbientOcclusion;
  const std::string_view rt = gc::RtCell(gc::RtEffect::kRtAo, gc::RtTier::kStrong);
  if (rt.empty()) {   // no ray traced Auto (RTAO held, 2026-11-24): three classes, no fourth clause
    return "Auto is " + title(gc::Cell(ao, gc::GpuClass::kLow)) + " on integrated graphics, " +
           title(gc::Cell(ao, gc::GpuClass::kMid)) + " on a mid-range card and " +
           title(gc::Cell(ao, gc::GpuClass::kHigh)) + " on a high-end card.";
  }
  return "Auto is " + title(gc::Cell(ao, gc::GpuClass::kLow)) + " on integrated graphics, " +
         title(gc::Cell(ao, gc::GpuClass::kMid)) + " on a mid-range card, " +
         title(gc::Cell(ao, gc::GpuClass::kHigh)) + " on a high-end card and " +
         title(rt) + " on a high-end card that traces rays quickly.";
}

}  // namespace mcla::gpu::rtao_setting
