#include "guest_screen_size.h"

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

bool Check(bool passed, const char* description) {
  ++checks;
  if (!passed) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
  return passed;
}

void TestScreenSize() {
  using namespace mcla::gpu;
  using namespace rex::kernel;
  uint32_t width = 0, height = 0;
  Check(!ReadGuestScreenSize(nullptr, width, height), "no guest address space");

  GuestAddressSpace memory;
  if (!Check(memory.Initialize(), "initialize the guest address space")) {
    return;
  }
  Check(!ReadGuestScreenSize(&memory, width, height), "guest arena exists before the image");
  Check(width == 0 && height == 0, "unmapped read leaves dimensions untouched");

  auto* heap = memory.LookupHeap(kGuestScreenWidthAddress);
  if (!Check(heap != nullptr, "find the title image heap")) {
    return;
  }
  const uint32_t page_size = heap->GetPageSize();
  const uint32_t page = kGuestScreenWidthAddress - kGuestScreenWidthAddress % page_size;
  if (!Check(heap->AllocFixed(page, page_size, page_size, kMemoryAllocationReserve,
                             kMemoryProtectRead),
             "reserve the screen-size page")) {
    return;
  }
  Check(!ReadGuestScreenSize(&memory, width, height) && width == 0 && height == 0,
        "reserved page with read permission is still unavailable");
  if (!Check(heap->AllocFixed(page, page_size, page_size, kMemoryAllocationCommit,
                             kMemoryProtectRead | kMemoryProtectWrite),
             "commit the screen-size page")) {
    return;
  }
  const MemoryView view(memory);
  view.Store<uint32_t>(kGuestScreenWidthAddress, 1920);
  view.Store<uint32_t>(kGuestScreenHeightAddress, 1080);
  Check(heap->Protect(page, page_size, kMemoryProtectRead), "make the page read-only");
  Check(ReadGuestScreenSize(&memory, width, height) && width == 1920 && height == 1080,
        "retry reads width and height in big-endian order when ready");
  Check(heap->Protect(page, page_size, kMemoryProtectNoAccess), "remove read access");
  Check(!ReadGuestScreenSize(&memory, width, height) && width == 1920 && height == 1080,
        "protected read leaves both dimensions untouched");
}

}  // namespace

int main() {
  TestScreenSize();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
