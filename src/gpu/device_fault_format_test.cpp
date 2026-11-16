// Check of the device-loss report's text (device_fault_format.h). A program of its own: it needs no
// renderer, no ReXGlue, no Vulkan and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_device_fault_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_device_fault_test
//
//   clang++ -std=c++23 -Wall -Wextra -O1 device_fault_format_test.cpp -o device_fault_format_test
//
// The expected strings are written out again here on purpose: a name or a wording changed in the
// header alone fails the check, and changing one on purpose means changing it here too.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "device_fault_format.h"

using namespace mcla::gpu::device_fault;

namespace {

int g_checks = 0;
int g_failed = 0;

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

void CheckNum(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got,
                (unsigned long long)want);
  }
}

void CheckLines(const std::vector<std::string>& got, const std::vector<std::string>& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: %zu lines, want %zu\n", what.c_str(), got.size(), want.size());
    for (const std::string& line : got) {
      std::printf("  got:  '%s'\n", line.c_str());
    }
    for (const std::string& line : want) {
      std::printf("  want: '%s'\n", line.c_str());
    }
  }
}

void TestVkResultNames() {
  CheckEq(VkResultName(-4), "VK_ERROR_DEVICE_LOST", "-4 is the device loss");
  CheckEq(VkResultName(0), "VK_SUCCESS", "0");
  CheckEq(VkResultName(1), "VK_NOT_READY", "1: what an unsignalled fence says while the device is alive");
  CheckEq(VkResultName(-1), "VK_ERROR_OUT_OF_HOST_MEMORY", "-1");
  CheckEq(VkResultName(-2), "VK_ERROR_OUT_OF_DEVICE_MEMORY", "-2");
  CheckEq(VkResultName(-1000000000), "VK_ERROR_SURFACE_LOST_KHR", "the surface");
  CheckEq(VkResultName(-1000001004), "VK_ERROR_OUT_OF_DATE_KHR", "out of date");
  CheckEq(VkResultName(1000001003), "VK_SUBOPTIMAL_KHR", "suboptimal");
  CheckEq(VkResultName(-1000255000), "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT",
          "full screen exclusive");
  CheckEq(VkResultName(-777), "VkResult -777", "an unknown number is printed as it is");
  CheckEq(VkResultName(INT32_MIN), "VkResult -2147483648", "the extreme");
}

void TestDxgiNames() {
  CheckEq(DxgiReasonName(0x887A0005u), "DXGI_ERROR_DEVICE_REMOVED", "removed");
  CheckEq(DxgiReasonName(0x887A0006u), "DXGI_ERROR_DEVICE_HUNG", "hung");
  CheckEq(DxgiReasonName(0x887A0007u), "DXGI_ERROR_DEVICE_RESET", "reset");
  CheckEq(DxgiReasonName(0x887A0020u), "DXGI_ERROR_DRIVER_INTERNAL_ERROR", "driver internal");
  CheckEq(DxgiReasonName(0x80070057u), "", "E_INVALIDARG has no name here");
}

void TestRemovedReasonText() {
  // D3D12 keeps the form the log line always had, and gains the name.
  CheckEq(RemovedReasonText(false, 0x887A0006ll), "0x887a0006 (DXGI_ERROR_DEVICE_HUNG)", "D3D12 hung");
  CheckEq(RemovedReasonText(false, 0x887A0005ll), "0x887a0005 (DXGI_ERROR_DEVICE_REMOVED)", "D3D12 removed");
  CheckEq(RemovedReasonText(false, 0x12345678ll), "0x12345678", "D3D12 unknown: the number alone");
  // The reason as the D3D12 backend returns it: the HRESULT's 32 bits in an int64.
  CheckEq(RemovedReasonText(false, static_cast<int64_t>(static_cast<uint32_t>(0x887A0007))),
          "0x887a0007 (DXGI_ERROR_DEVICE_RESET)", "D3D12 reset");
  CheckEq(RemovedReasonText(true, -4), "VK_ERROR_DEVICE_LOST (-4)", "Vulkan device lost");
  CheckEq(RemovedReasonText(true, -1000000000), "VK_ERROR_SURFACE_LOST_KHR (-1000000000)", "Vulkan surface");
  CheckEq(RemovedReasonText(true, -9999), "VkResult -9999 (-9999)", "Vulkan unknown");
}

void TestAddressTypes() {
  CheckEq(AddressTypeName(0), "no address", "none");
  CheckEq(AddressTypeName(1), "read of an invalid address", "read");
  CheckEq(AddressTypeName(2), "write to an invalid address", "write");
  CheckEq(AddressTypeName(3), "execution at an invalid address", "execute");
  CheckEq(AddressTypeName(6), "instruction pointer at the faulting instruction", "ip fault");
  CheckEq(AddressTypeName(99), "address of a kind this build does not know", "unknown");
}

void TestFaultReport() {
  // Nothing reported: the description and one line that says so.
  {
    FaultReport r;
    r.description = "Device lost";
    CheckLines(FormatFaultReport(r),
               {"device fault: Device lost",
                "  the driver reported no faulting address and no vendor code"},
               "an empty report");
  }
  // No description either.
  {
    FaultReport r;
    CheckLines(FormatFaultReport(r),
               {"device fault: the driver gave no description",
                "  the driver reported no faulting address and no vendor code"},
               "an empty report without a description");
  }
  // One address with a power-of-two precision: a range. One vendor entry. A crash dump.
  {
    FaultReport r;
    r.description = "GPU page fault";
    r.addresses.push_back({1, 0x00007ff612340000ull, 4096});
    r.address_count = 1;
    r.vendors.push_back({"Xid 31 MMU fault", 0x1f, 0x2});
    r.vendor_count = 1;
    r.vendor_binary_bytes = 65536;
    CheckLines(FormatFaultReport(r),
               {"device fault: GPU page fault",
                "  address 0: read of an invalid address, somewhere in 0x00007ff612340000 to "
                "0x00007ff612340fff (precision 4096 bytes)",
                "  vendor 0: Xid 31 MMU fault (code 0x1f, data 0x2)",
                "  the driver has a crash dump of 65536 bytes (not saved)"},
               "a full report");
  }
  // A precision that is not a power of two (or 0, or 1) is no range: the address alone.
  {
    FaultReport r;
    r.description = "x";
    r.addresses.push_back({2, 0x1000, 0});
    r.addresses.push_back({3, 0x2000, 3});
    r.addresses.push_back({4, 0x3000, 1});
    r.address_count = 3;
    CheckLines(FormatFaultReport(r),
               {"device fault: x",
                "  address 0: write to an invalid address at 0x0000000000001000",
                "  address 1: execution at an invalid address at 0x0000000000002000",
                "  address 2: instruction pointer at the fault (the fault's own address is unknown) at 0x0000000000003000"},
               "precision that makes no range");
  }
  // An entry of type none with no address.
  {
    FaultReport r;
    r.description = "x";
    r.addresses.push_back({0, 0, 0});
    r.address_count = 1;
    CheckLines(FormatFaultReport(r), {"device fault: x", "  address 0: none"}, "an empty address entry");
  }
  // More entries than are listed: the first max_entries, then a count of the rest (from the
  // driver's own count when it is bigger than what was read).
  {
    FaultReport r;
    r.description = "many";
    for (uint64_t i = 0; i < 3; ++i) {
      r.addresses.push_back({1, 0x10000 * (i + 1), 0});
    }
    r.address_count = 10;  // the driver has 10, 3 were read
    for (uint64_t i = 0; i < 2; ++i) {
      r.vendors.push_back({"v", i, i});
    }
    r.vendor_count = 2;
    const std::vector<std::string> lines = FormatFaultReport(r, 2);
    CheckNum(lines.size(), 1 + 2 + 1 + 2, "2 listed of each, one count line for the addresses");
    CheckEq(lines[3], "  ... and 8 more address entries", "the rest of the addresses, by the driver's count");
    CheckEq(lines[4], "  vendor 0: v (code 0x0, data 0x0)", "first vendor");
    CheckEq(lines[5], "  vendor 1: v (code 0x1, data 0x1)", "second vendor");
  }
  // Vendor entries past the limit.
  {
    FaultReport r;
    r.description = "many";
    for (uint64_t i = 0; i < 5; ++i) {
      r.vendors.push_back({"v", i, 0});
    }
    r.vendor_count = 5;
    const std::vector<std::string> lines = FormatFaultReport(r, 3);
    CheckNum(lines.size(), 1 + 3 + 1, "3 vendor lines, a count line");
    CheckEq(lines.back(), "  ... and 2 more vendor entries", "the rest of the vendor entries");
  }
  // A driver's text can hold anything: control bytes and non-ASCII become '?', white space one
  // space, the ends are trimmed, an all-blank description counts as none.
  {
    FaultReport r;
    r.description = "  bad\tline\nnext \x01 caf\xC3\xA9  ";
    r.vendors.push_back({"   ", 1, 2});
    r.vendor_count = 1;
    const std::vector<std::string> lines = FormatFaultReport(r);
    CheckEq(lines[0], "device fault: bad line next ? caf??", "cleaned description");
    CheckEq(lines[1], "  vendor 0: no description (code 0x1, data 0x2)", "blank vendor text");
  }
  {
    FaultReport r;
    r.description = " \t\n ";
    CheckEq(FormatFaultReport(r)[0], "device fault: the driver gave no description", "blank description");
  }
  // A vendor count bigger than the entries read, with nothing read.
  {
    FaultReport r;
    r.description = "x";
    r.vendor_count = 4;
    const std::vector<std::string> lines = FormatFaultReport(r, 8);
    CheckNum(lines.size(), 2, "only counted: the description and the rest");
    CheckEq(lines[1], "  ... and 4 more vendor entries", "all four were left out");
  }
}

void TestLabelTrail() {
  CheckEq(FormatLabelTrail({}, 8), "no passes recorded", "no labels");
  CheckEq(FormatLabelTrail({"a"}, 8), "a", "one label");
  CheckEq(FormatLabelTrail({"a", "b", "c"}, 8), "a > b > c", "few labels: all of them");
  CheckEq(FormatLabelTrail({"a", "b", "c"}, 3), "a > b > c", "exactly the limit");
  CheckEq(FormatLabelTrail({"a", "b", "c", "d", "e"}, 2), "(3 earlier) ... > d > e",
          "more than the limit: the end, with the count of the rest");
  CheckEq(FormatLabelTrail({"a", "b"}, 0), "(1 earlier) ... > b", "a limit of 0 keeps the last one");
}

}  // namespace

int main() {
  TestVkResultNames();
  TestDxgiNames();
  TestRemovedReasonText();
  TestAddressTypes();
  TestFaultReport();
  TestLabelTrail();
  if (g_failed != 0) {
    std::printf("%d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("device_fault_format_test: %d checks passed\n", g_checks);
  return 0;
}
