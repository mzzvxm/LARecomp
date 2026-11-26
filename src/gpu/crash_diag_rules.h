#pragma once

#include <filesystem>
#include <string_view>

namespace mcla::gpu::device_fault {

constexpr bool CrashDiagnosticsEnabled(std::string_view mode, bool previous_loss) {
  return mode == "on" || (mode == "auto" && previous_loss);
}

// The marker belongs to the next session, irrespective of the selected mode.
inline bool ConsumeLossMarker(const std::filesystem::path& marker, std::error_code& error) {
  return std::filesystem::remove(marker, error);
}

}  // namespace mcla::gpu::device_fault
