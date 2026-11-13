// MCLA GPU renderer -- the GPU class and the `auto` value. See gpu_class.h.

#include "gpu_class.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(
    mcla_gpu_class, "auto", "MCLA/GPU",
    "Diagnostic: forces the GPU class the graphics upgrades' auto values are chosen from (low: "
    "integrated GPUs and handhelds, mid: discrete with under about 6 GiB of video memory, high: the "
    "rest). "
    "auto: the class the renderer found for this GPU, logged at start as '[gpu] class: ...'. Only "
    "the value auto of the upgrades follows it (shadow filter and detail, car and road reflection "
    "detail, contact shadows, sun shafts, dawn shadows, bounce light); "
    "a choice of the player's own is never changed. Live.")
    .allowed({"auto", "low", "mid", "high"})
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::gpu_class {

namespace {

constexpr int kNoOverride = -1;

std::atomic<uint8_t> g_detected{static_cast<uint8_t>(GpuClass::kHigh)};
std::atomic<int> g_override{kNoOverride};

std::mutex g_text_mutex;     // guards the three strings of the log line
std::string g_detected_why;  // why Initialize chose g_detected
std::string g_adapter;       // "name (vendor)" for the log line

bool ContainsNoCase(std::string_view text, std::string_view part) {
  const auto it = std::search(text.begin(), text.end(), part.begin(), part.end(),
                              [](char a, char b) {
                                const auto low = [](char c) {
                                  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                                };
                                return low(a) == low(b);
                              });
  return it != text.end();
}

// A software rasteriser is a Low class whatever it reports: WARP's "Microsoft Basic Render Driver",
// Mesa's llvmpipe and lavapipe, Google's SwiftShader.
bool IsSoftwareName(std::string_view name) {
  return ContainsNoCase(name, "basic render") || ContainsNoCase(name, "llvmpipe") ||
         ContainsNoCase(name, "lavapipe") || ContainsNoCase(name, "swiftshader");
}

std::string VendorName(plume::RenderDeviceVendor vendor) {
  switch (vendor) {
    case plume::RenderDeviceVendor::NVIDIA:
      return "NVIDIA";
    case plume::RenderDeviceVendor::AMD:
      return "AMD";
    case plume::RenderDeviceVendor::INTEL:
      return "Intel";
    case plume::RenderDeviceVendor::APPLE:
      return "Apple";
    default: {
      char text[32];
      snprintf(text, sizeof(text), "vendor 0x%04X", static_cast<unsigned>(vendor));
      return text;
    }
  }
}

// Sets the override from the cvar's text: low, mid or high force a class; auto (or any other text,
// which the cvar's allowed list does not let through) leaves the class the adapter gave.
void ApplyOverride(std::string_view text) {
  GpuClass forced;
  g_override.store(ParseClass(text, &forced) ? static_cast<int>(forced) : kNoOverride,
                   std::memory_order_relaxed);
}

// What `auto` comes to for every effect, in one line.
std::string AutoSummary(GpuClass c) {
  std::string out;
  constexpr std::string_view kPrefix = "mcla_gpu_";
  for (int i = 0; i < kEffectCount; ++i) {
    const auto effect = static_cast<Effect>(i);
    std::string_view name = CvarName(effect);
    if (name.substr(0, kPrefix.size()) == kPrefix) {
      name.remove_prefix(kPrefix.size());
    }
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
    out += '=';
    out += Cell(effect, c);
  }
  return out;
}

void LogClass() {
  std::string why;
  std::string adapter;
  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    why = g_detected_why;
    adapter = g_adapter;
  }
  const GpuClass detected = static_cast<GpuClass>(g_detected.load(std::memory_order_relaxed));
  const GpuClass now = Current();
  if (now == detected) {
    REXLOG_INFO("[gpu] class: {} ({}; {})", ClassName(now), why, adapter);
  } else {
    REXLOG_INFO("[gpu] class: {} (forced by mcla_gpu_class={}; the adapter alone says {}: {}; {})",
                ClassName(now), ClassName(now), ClassName(detected), why, adapter);
  }
  REXLOG_INFO("[gpu] class {}: auto is {}", ClassName(now), AutoSummary(now));
}

}  // namespace

void Initialize(const plume::RenderDeviceDescription& description,
                const plume::RenderDeviceCapabilities& caps) {
  // CHECK: read from Plume's code, not yet seen on the test PC's adapters: D3D12 never fills
  // description.type, so an integrated GPU is known there only by caps.uma (DXGI's UMA flag); Vulkan
  // sets both (uma needs the integrated type and a host-visible device-local memory type). The log
  // line of a run on --adapter 1 (the 2-CU iGPU) says which one decided.
  AdapterFacts facts;
  facts.uma = caps.uma;
  facts.integrated = description.type == plume::RenderDeviceType::INTEGRATED;
  facts.software = description.type == plume::RenderDeviceType::VIRTUAL ||
                   description.type == plume::RenderDeviceType::CPU ||
                   IsSoftwareName(description.name);
  facts.dedicated_bytes = description.dedicatedVideoMemory;
  const Verdict verdict = Classify(facts);

  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    g_detected_why = verdict.why;
    g_adapter = "'" + description.name + "', " + VendorName(description.vendor);
  }
  g_detected.store(static_cast<uint8_t>(verdict.cls), std::memory_order_relaxed);

  ApplyOverride(REXCVAR_GET(mcla_gpu_class));
  // A change made while the game runs (the debug menu's F4 panel, the console) takes effect at once:
  // the callback has the new text, so no other thread reads the cvar's string.
  static std::once_flag registered;
  std::call_once(registered, [] {
    rex::cvar::RegisterChangeCallback("mcla_gpu_class", [](std::string_view, std::string_view value) {
      ApplyOverride(value);
      LogClass();
    });
  });
  LogClass();
}

GpuClass Current() {
  const int forced = g_override.load(std::memory_order_relaxed);
  if (forced != kNoOverride) {
    return static_cast<GpuClass>(forced);
  }
  return static_cast<GpuClass>(g_detected.load(std::memory_order_relaxed));
}

std::string Resolve(Effect effect, std::string_view user_value) {
  return ResolveFor(Current(), effect, user_value);
}

std::string AutoLabel(Effect effect) {
  return AutoLabelFor(Current(), effect);
}

}  // namespace mcla::gpu::gpu_class
