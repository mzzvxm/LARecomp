#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace mcla::gpu::env_hold {

// Ordered with the guest's draws and resolves through CompositeRecord.
inline constexpr uint32_t kDrawn = 0x7F000014u;
inline constexpr uint32_t kHeld = 0x7F000015u;
inline constexpr uint32_t kEnd = 0x7F000016u;
constexpr bool IsMarker(uint32_t marker) {
  return marker == kDrawn || marker == kHeld || marker == kEnd;
}

struct Eye {
  std::array<float, 3> position{};
  bool valid = false;
};

// End carries the freshly rendered probe eye. Held carries the current view
// eye, which may have moved since the probe was rendered.
void SubmitMarker(uint32_t bucket, uint32_t renderer, const Eye& eye);

inline constexpr uint32_t kMapPage = 0x032E2u;
inline constexpr uint32_t kMipPage = 0x033E2u;
inline constexpr uint32_t kSpecPage = 0x032DEu;

// A ready bit applies only to the guest renderer that made these products.
// The consumer withdraws it on a missing/recreated product or at shutdown.
inline std::atomic<uint32_t> g_ready_renderer{0};
inline std::atomic<float> g_brightness{-1.0f};

// Legacy game stays at 1x. Auto now takes the same 2x choice on every GPU.
inline constexpr std::string_view kResolutionSetting = "mcla_gpu_env_resolution";
constexpr std::string_view Resolution(std::string_view value) {
  return value == "game" || value == "off" ? "off" : "on";
}

}  // namespace mcla::gpu::env_hold
