// MCLA GPU renderer -- the GPU class and the `auto` value. See gpu_class.h.

#include "gpu_class.h"
#include "settings/immortal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <plume_render_interface.h>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

namespace mcla::gpu::gpu_class {

namespace {

constexpr int kNoOverride = -1;

std::atomic<uint8_t> g_detected{static_cast<uint8_t>(GpuClass::kHigh)};
std::atomic<bool> g_known{false};  // Initialize has run: g_detected is what the adapter said
std::atomic<int> g_override{kNoOverride};

std::mutex g_text_mutex;     // guards the three strings of the log line
std::string& g_detected_why = larecomp::Leak<std::string>();  // why Initialize chose g_detected
std::string& g_adapter = larecomp::Leak<std::string>();  // "name (vendor)" for the log line

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
  // The numeric settings (the LOD scales, the traffic range and the densities): `auto` as the value the hooks apply.
  for (int i = 0; i < kScaleCount; ++i) {
    const auto scale = static_cast<Scale>(i);
    out += ", ";
    out += ScaleCvar(scale);
    out += '=';
    out += ScaleText(scale, ScaleCell(scale, c));
  }
  // Not a setting's `auto`, but chosen by the class too: the sun shafts' ray steps.
  out += ", sun_shafts_steps=";
  out += std::to_string(SunShaftSteps(c));
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

AdapterFacts FactsFor(const plume::RenderDeviceDescription& description,
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
  facts.raytracing = caps.raytracing || caps.rayQuery;  // a Vulkan driver with ray queries and no ray tracing pipelines traces too
  // The speed table reads the name and the PCI vendor id (Plume's RenderDeviceVendor holds the id; it
  // does not carry the device id: the ray tracing tier asks the backend for that, rt_tier.h).
  facts.name = description.name;
  facts.vendor_id = static_cast<uint32_t>(description.vendor);
  return facts;
}

void Initialize(const plume::RenderDeviceDescription& description,
                const plume::RenderDeviceCapabilities& caps) {
  const AdapterFacts facts = FactsFor(description, caps);
  const Verdict verdict = Classify(facts);

  {
    std::lock_guard<std::mutex> lock(g_text_mutex);
    g_detected_why = verdict.why;
    g_adapter = "'" + description.name + "', " + VendorName(description.vendor) +
                (facts.raytracing ? ", ray tracing yes" : ", ray tracing no");
  }
  g_detected.store(static_cast<uint8_t>(verdict.cls), std::memory_order_relaxed);

  ApplyOverride(larecomp::settings::mcla_gpu_class.Get());
  g_known.store(true, std::memory_order_release);
  // A change made while the game runs (the debug menu's F4 panel, the console) takes effect at once:
  // the callback has the new text, so no other thread reads the cvar's string.
  static std::once_flag registered;
  std::call_once(registered, [] {
    larecomp::cvar::RegisterChangeCallback("mcla_gpu_class", [](std::string_view, std::string_view value) {
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

bool ClassForced() {
  return g_override.load(std::memory_order_relaxed) != kNoOverride;
}

GpuClass ClassDetected() {
  return static_cast<GpuClass>(g_detected.load(std::memory_order_relaxed));
}

bool Known() {
  return g_known.load(std::memory_order_acquire);
}

std::string Resolve(Effect effect, std::string_view user_value) {
  return ResolveFor(Current(), effect, user_value);
}

std::string AutoLabel(Effect effect) {
  return AutoLabelFor(Current(), effect);
}

double ScaleInForce(Scale scale, double user_value) {
  return ScaleFor(scale, Known(), Current(), user_value);
}

std::string ScaleLabel(Scale scale, double user_value) {
  return ScaleLabelFor(scale, Known(), Current(), user_value);
}

double CityLodScale(double user_value) {
  return CityLodScaleFor(Known(), Current(), user_value);
}

std::string CityLodLabel(double user_value) {
  return CityLodLabelFor(Known(), Current(), user_value);
}

}  // namespace mcla::gpu::gpu_class
