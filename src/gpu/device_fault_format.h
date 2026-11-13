#pragma once
// ===========================================================================
// MCLA GPU renderer -- how a lost device is written to the log
// ===========================================================================
// The text half of the device-loss report, with no Vulkan, D3D12 or renderer in it so a
// program of its own can check it (device_fault_format_test.cpp):
//
//   RemovedReasonText   the reason the backend gave: DXGI's HRESULT on D3D12, the VkResult of the
//                       call that came back with the loss on Vulkan (always VK_ERROR_DEVICE_LOST
//                       today), each with its name
//   FaultReport         what VK_EXT_device_fault knows once a device is lost (filled by
//                       backend_vulkan.cpp from vkGetDeviceFaultInfoEXT): the driver's description,
//                       the faulting addresses, the vendor's fault codes, how big its crash dump is
//   FormatFaultReport   that as log lines
//   FormatLabelTrail    the pass labels the renderer recorded for a frame, as one line that keeps
//                       the end (what was recorded last is what the GPU may have been running)
//
// Every function is pure: same input, same text.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::gpu::device_fault {

// VkResult values a lost device's calls can come back with, by their Vulkan names; the number
// otherwise. (Plume's Vulkan queue submit, fence wait, acquire and present all return one.)
inline std::string VkResultName(int32_t result) {
  switch (result) {
    case 0: return "VK_SUCCESS";
    case 1: return "VK_NOT_READY";
    case 2: return "VK_TIMEOUT";
    case 5: return "VK_INCOMPLETE";
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -3: return "VK_ERROR_INITIALIZATION_FAILED";
    case -4: return "VK_ERROR_DEVICE_LOST";
    case -5: return "VK_ERROR_MEMORY_MAP_FAILED";
    case -13: return "VK_ERROR_UNKNOWN";
    case -1000000000: return "VK_ERROR_SURFACE_LOST_KHR";
    case -1000001004: return "VK_ERROR_OUT_OF_DATE_KHR";
    case 1000001003: return "VK_SUBOPTIMAL_KHR";
    case -1000255000: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default: return std::format("VkResult {}", result);
  }
}

// What GetDeviceRemovedReason returns, by its DXGI name; empty for one this table does not know.
inline const char* DxgiReasonName(uint32_t reason) {
  switch (reason) {
    case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006u: return "DXGI_ERROR_DEVICE_HUNG";
    case 0x887A0007u: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020u: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    default: return "";
  }
}

// The reason as the log line prints it. D3D12: the HRESULT in the form the line always had
// ("0x887a0006"), then its name when it has one. Vulkan: the VkResult's name, then its number.
inline std::string RemovedReasonText(bool vulkan, int64_t reason) {
  if (vulkan) {
    return std::format("{} ({})", VkResultName(static_cast<int32_t>(reason)),
                       static_cast<int32_t>(reason));
  }
  const auto hr = static_cast<uint32_t>(reason);
  const char* name = DxgiReasonName(hr);
  return *name ? std::format("{:#010x} ({})", hr, name) : std::format("{:#010x}", hr);
}

// VkDeviceFaultAddressTypeEXT, in words.
inline const char* AddressTypeName(uint32_t type) {
  switch (type) {
    case 0: return "no address";
    case 1: return "read of an invalid address";
    case 2: return "write to an invalid address";
    case 3: return "execution at an invalid address";
    case 4: return "instruction pointer at the fault (the fault's own address is unknown)";
    case 5: return "instruction pointer at an invalid address";
    case 6: return "instruction pointer at the faulting instruction";
    default: return "address of a kind this build does not know";
  }
}

struct FaultAddress {
  uint32_t type = 0;       // VkDeviceFaultAddressTypeEXT
  uint64_t address = 0;    // the reported address, with its low bits cleared by the precision
  uint64_t precision = 0;  // the real address is within [address, address + precision - 1]
};

struct FaultVendor {
  std::string description;
  uint64_t code = 0;
  uint64_t data = 0;
};

struct FaultReport {
  std::string description;                 // the driver's own sentence
  std::vector<FaultAddress> addresses;     // as many as were read
  std::vector<FaultVendor> vendors;
  uint32_t address_count = 0;              // how many the driver had (more than were read, maybe)
  uint32_t vendor_count = 0;
  uint64_t vendor_binary_bytes = 0;        // size of its crash dump (not read: only said)
};

namespace detail {

// A driver's text for a log line: printable ASCII only (an arbitrary byte can break the log's
// encoding), one line, trimmed.
inline std::string Clean(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    out.push_back(u >= 0x20 && u < 0x7F ? c : (u == '\t' || u == '\n' || u == '\r' ? ' ' : '?'));
  }
  const size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = out.find_last_not_of(' ');
  return out.substr(first, last - first + 1);
}

inline bool IsPowerOfTwo(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace detail

// The report as log lines (no prefix; the caller adds "[gpu] "): the description first, then one
// line per address and per vendor entry (at most `max_entries` of each, with a line that counts
// the rest), then the crash dump's size.
inline std::vector<std::string> FormatFaultReport(const FaultReport& report,
                                                  size_t max_entries = 8) {
  std::vector<std::string> lines;
  const std::string description = detail::Clean(report.description);
  lines.push_back(description.empty() ? "device fault: the driver gave no description"
                                      : "device fault: " + description);
  const size_t address_total =
      report.address_count > report.addresses.size() ? report.address_count : report.addresses.size();
  for (size_t i = 0; i < report.addresses.size() && i < max_entries; ++i) {
    const FaultAddress& a = report.addresses[i];
    if (a.type == 0 && a.address == 0) {
      lines.push_back(std::format("  address {}: none", i));
      continue;
    }
    if (detail::IsPowerOfTwo(a.precision) && a.precision > 1) {
      lines.push_back(std::format("  address {}: {}, somewhere in {:#018x} to {:#018x} (precision {} bytes)", i,
                                  AddressTypeName(a.type), a.address, a.address + (a.precision - 1),
                                  a.precision));
    } else {
      lines.push_back(std::format("  address {}: {} at {:#018x}", i, AddressTypeName(a.type), a.address));
    }
  }
  const size_t addresses_listed = report.addresses.size() < max_entries ? report.addresses.size() : max_entries;
  if (address_total > addresses_listed) {
    lines.push_back(std::format("  ... and {} more address entries", address_total - addresses_listed));
  }
  const size_t vendor_total =
      report.vendor_count > report.vendors.size() ? report.vendor_count : report.vendors.size();
  for (size_t i = 0; i < report.vendors.size() && i < max_entries; ++i) {
    const FaultVendor& v = report.vendors[i];
    const std::string text = detail::Clean(v.description);
    lines.push_back(std::format("  vendor {}: {} (code {:#x}, data {:#x})", i,
                                text.empty() ? "no description" : text, v.code, v.data));
  }
  const size_t vendors_listed = report.vendors.size() < max_entries ? report.vendors.size() : max_entries;
  if (vendor_total > vendors_listed) {
    lines.push_back(std::format("  ... and {} more vendor entries", vendor_total - vendors_listed));
  }
  if (address_total == 0 && vendor_total == 0) {
    lines.push_back("  the driver reported no faulting address and no vendor code");
  }
  if (report.vendor_binary_bytes != 0) {
    lines.push_back(std::format("  the driver has a crash dump of {} bytes (not saved)",
                                report.vendor_binary_bytes));
  }
  return lines;
}

// `labels` joined with " > ", the last `keep` of them when there are more, headed by how many were
// left out. Empty list: "no passes recorded".
inline std::string FormatLabelTrail(const std::vector<std::string>& labels, size_t keep) {
  if (labels.empty()) {
    return "no passes recorded";
  }
  if (keep == 0) {
    keep = 1;
  }
  const size_t first = labels.size() > keep ? labels.size() - keep : 0;
  std::string text;
  if (first > 0) {
    text = std::format("({} earlier) ... > ", first);
  }
  for (size_t i = first; i < labels.size(); ++i) {
    if (i > first) {
      text += " > ";
    }
    text += labels[i];
  }
  return text;
}

}  // namespace mcla::gpu::device_fault
