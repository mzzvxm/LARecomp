#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "host_plan_level.h"
#include "rt_tier.h"

namespace mcla::gpu::rtao_setting {
namespace gc = gpu_class;

enum class Choice : uint8_t { kAuto, kOff, kHigh, kUltra, kRt, kOn };

constexpr const char* kValues[] = {"auto", "off", "high", "ultra", "rt", "on"};
constexpr const char* kNames[] = {"AUTO", "OFF", "ON", "ON", "RAY TRACED", "ON"};

inline Choice ParseChoice(std::string_view word) {
  const std::string upper = gc::Upper(word);
  if (upper == "RT") return Choice::kRt;
  if (upper == "ON") return Choice::kOn;
  if (upper == "ULTRA") return Choice::kUltra;
  if (upper == "HIGH") return Choice::kHigh;
  if (upper == "OFF") return Choice::kOff;
  return Choice::kAuto;
}

enum class Gate : uint8_t { kRuns, kNoRt, kNotBuilt };

constexpr Gate GateFor(gc::GpuClass, gc::RtTier tier, bool pass_built) {
  if (tier == gc::RtTier::kNone) return Gate::kNoRt;
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
  if (choice == Choice::kAuto) choice = Choice::kOn;
  d.chosen = choice;
  d.rt = choice == Choice::kRt && Available(d.gate);
  if (choice != Choice::kOff) d.screen = host_plan::Level::kUltra;
  return d;
}

// Resolve using the current adapter and the pass readiness published by the renderer.
Decision DecideFor(std::string_view setting_text);
bool PassBuilt();
void NotePassBuilt(bool built);

constexpr std::string_view GateReason(Gate gate) {
  switch (gate) {
    case Gate::kNoRt: return "NO RT GPU";
    case Gate::kNotBuilt: return "NOT READY";
    default: return {};
  }
}

inline std::string AutoLabel(const Decision& d) {
  return gc::AutoLabelOf(d.rt ? "rt" : d.screen == host_plan::Level::kOff ? "off" : "on");
}

inline std::string ValueLabel(Choice c, Gate gate) {
  if (c == Choice::kRt && !Available(gate)) {
    return "RT (" + std::string(GateReason(gate)) + ")";
  }
  return kNames[static_cast<uint8_t>(c)];
}

inline std::string AutoDescription() {
  return "Auto uses screen-space ambient occlusion.";
}

}  // namespace mcla::gpu::rtao_setting
