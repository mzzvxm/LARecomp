// MCLA GPU renderer -- the ray tracing tier of this process. See rt_tier.h.

#include "rt_tier.h"
#include "settings/immortal.h"

#include <atomic>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

namespace mcla::gpu::gpu_class {

namespace {

constexpr int kNoOverride = -1;

std::atomic<uint8_t> g_table_tier{static_cast<uint8_t>(RtTier::kNone)};  // what the tables (and the API) said
std::atomic<bool> g_table_known{false};
std::atomic<uint8_t> g_tier{static_cast<uint8_t>(RtTier::kNone)};  // the table's, after the benchmark
std::atomic<bool> g_ready{false};                                  // InitializeRt has run
std::atomic<bool> g_api_rt{false};
std::atomic<int> g_override{kNoOverride};

std::mutex g_text_mutex;  // guards the strings and the key below
std::string& g_table_why = larecomp::Leak<std::string>();
std::string& g_bench_why = larecomp::Leak<std::string>();
std::string& g_adapter = larecomp::Leak<std::string>();
rt_bench::KeyFacts& g_key = larecomp::Leak<rt_bench::KeyFacts>();
bool g_bench_wanted = false;

// Sets the override from the cvar's text: none, weak or strong force a tier; auto (or any other text) leaves it to the card and the class.
void ApplyOverride(std::string_view text) {
  RtTier forced;
  g_override.store(ParseRtTier(text, &forced) ? static_cast<int>(forced) : kNoOverride, std::memory_order_relaxed);
}

// What `auto` comes to for every ray traced effect, in one line.
std::string RtAutoSummary(GpuClass c, RtTier t) {
  std::string out;
  constexpr std::string_view kPrefix = "mcla_gpu_";
  for (int i = 0; i < kRtEffectCount; ++i) {
    const auto effect = static_cast<RtEffect>(i);
    std::string_view name = RtCvarName(effect);
    if (name.substr(0, kPrefix.size()) == kPrefix) {
      name.remove_prefix(kPrefix.size());
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
    out += '=';
    out += RtCell(effect, RtRung(c, t));
  }
  return out;
}

void LogRt() {
  std::string why, bench_why, adapter;
  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    why = g_table_why;
    bench_why = g_bench_why;
    adapter = g_adapter;
  }
  const RtTier decided = static_cast<RtTier>(g_tier.load(std::memory_order_relaxed));
  const RtTier now = CurrentRtTier();
  std::string text = why;
  if (!bench_why.empty()) {
    text += "; benchmark: " + bench_why;
  }
  if (now == decided) {
    REXLOG_INFO("{}", RtTierLogLine(now, text + "; " + adapter));
  } else {
    // A forced tier (mcla_gpu_rt_tier, or a forced class in a test) differs from what the card said.
    REXLOG_INFO("{}", RtTierLogLine(now, "forced (mcla_gpu_rt_tier or mcla_gpu_class); the card alone says " + std::string(RtTierName(decided)) + ": " + text +
                                             "; " + adapter));
  }
  REXLOG_INFO("[gpu] ray tracing tier {}, class {}: auto is {}", RtTierName(now), ClassName(Current()), RtAutoSummary(Current(), now));
}

}  // namespace

void InitializeRt(const plume::RenderDeviceDescription& description, const plume::RenderDeviceCapabilities& caps, const RtProbe& probe) {
  const AdapterFacts adapter = FactsFor(description, caps);
  RtFacts facts;
  facts.api_rt = probe.api_rt;
  facts.api_reason = probe.api_reason;
  facts.software = adapter.software;
  facts.uma = adapter.uma;
  facts.integrated = adapter.integrated;
  facts.vendor_id = adapter.vendor_id;
  facts.device_id = probe.device_id;
  facts.name = adapter.name;
  const RtVerdict verdict = ClassifyRt(facts);

  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    g_table_why = verdict.why;
    g_bench_why.clear();  // a second backend tried, or a second call: the old benchmark is not this device's
    g_adapter = "'" + description.name + "', device id " + (probe.device_id ? HexId(probe.device_id) : std::string("unknown"));
    g_key.vendor_id = adapter.vendor_id;
    g_key.device_id = probe.device_id;
    g_key.driver_version = probe.driver_version;
    g_key.vulkan = probe.vulkan;
    g_key.name = description.name;
    // A benchmark makes sense for a card that can trace and is a real discrete one of a class above Low: nothing is measured for a card that
    // shares memory (nothing heavy runs on it) or one that has no ray tracing at all.
    g_bench_wanted = probe.api_rt && verdict.tier != RtTier::kNone && !adapter.software && !adapter.uma && !adapter.integrated &&
                     ClassDetected() != GpuClass::kLow;
  }
  g_table_tier.store(static_cast<uint8_t>(verdict.tier), std::memory_order_relaxed);
  g_table_known.store(verdict.known, std::memory_order_relaxed);
  g_tier.store(static_cast<uint8_t>(verdict.tier), std::memory_order_relaxed);
  g_api_rt.store(probe.api_rt, std::memory_order_relaxed);

  ApplyOverride(larecomp::settings::mcla_gpu_rt_tier.Get());
  g_ready.store(true, std::memory_order_release);
  // A change made while the game runs (the debug menu's F4 panel, the console) takes effect at once; a forced class changes the tier a test sees.
  static std::once_flag registered;
  std::call_once(registered, [] {
    larecomp::cvar::RegisterChangeCallback("mcla_gpu_rt_tier", [](std::string_view, std::string_view value) {
      ApplyOverride(value);
      LogRt();
    });
    larecomp::cvar::RegisterChangeCallback("mcla_gpu_class", [](std::string_view, std::string_view) { LogRt(); });
  });
  LogRt();
}

void NoteRtBench(const rt_bench::Result& result) {
  if (!g_ready.load(std::memory_order_acquire)) {
    return;
  }
  const RtTier table = static_cast<RtTier>(g_table_tier.load(std::memory_order_relaxed));
  const BenchVerdict verdict = ApplyBench(table, g_table_known.load(std::memory_order_relaxed), result);
  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    g_bench_why = verdict.why;
  }
  g_tier.store(static_cast<uint8_t>(verdict.tier), std::memory_order_relaxed);
  LogRt();
}

rt_bench::KeyFacts RtBenchKey() {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  return g_key;
}

bool RtBenchWanted() {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  return g_bench_wanted;
}

RtTier CurrentRtTier() {
  if (!g_ready.load(std::memory_order_acquire)) {
    return RtTier::kNone;
  }
  const int forced = g_override.load(std::memory_order_relaxed);
  if (forced != kNoOverride) {
    return static_cast<RtTier>(forced);
  }
  if (ClassForced()) {
    return TierForForcedClass(Current(), g_api_rt.load(std::memory_order_relaxed));
  }
  return static_cast<RtTier>(g_tier.load(std::memory_order_relaxed));
}

RtTier CurrentRtRung() {
  if (!Known()) {
    return RtTier::kNone;
  }
  return RtRung(Current(), CurrentRtTier());
}

GpuClass CurrentRtClass() {
  if (!Known()) {
    return GpuClass::kLow;
  }
  return RtClass(Current(), CurrentRtTier());
}

std::string ResolveRt(RtEffect effect, std::string_view user_value) {
  if (user_value != "auto") {
    return std::string(user_value);
  }
  return std::string(RtCell(effect, CurrentRtRung()));
}

std::string AutoLabelRt(RtEffect effect) {
  return AutoLabelOf(RtCell(effect, CurrentRtRung()));
}

}  // namespace mcla::gpu::gpu_class
