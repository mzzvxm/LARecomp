// Checks the Linux crash logger (crash_report_linux.h, crash_handler.cpp) against the REAL SDK's fault handling: the
// guest write watches the GPU caches arm (a guest store into a page the renderer made read-only must be resumed by the
// SDK, not reported) and a fault nothing handles (must be reported once and end the process). Linux only; it needs the
// SDK's install (headers and libraries) and nothing of the game or a GPU. The crash_report_demo.cpp stand-in for the
// runtime's handler cannot show the bug this is for: the order of the callbacks on rex::platform::ExceptionHandler's
// list (see the note at the top of crash_handler.cpp).
//
// Build (the kit's compiler, SDK_INSTALL = the prefix that has include/ and lib/, as the game's own build uses):
//   source ~/larecomp-kit/env.sh
//   clang++ -std=gnu++23 -O1 -g -pthread -DREX_PHYSICAL_OFFSET_IN_CODE=0 -isystem $SDK_INSTALL/include -I src \
//       src/crash_report_sdk_check.cpp -o crash_report_sdk_check -L$SDK_INSTALL/lib -lrexkernel -lrexbase -lrexppc \
//       -Wl,-rpath,$SDK_INSTALL/lib -Wl,-rpath,$HOME/larecomp-kit/llvm/lib/x86_64-unknown-linux-gnu
// Run:   ./crash_report_sdk_check MODE [log file]
//
// MODE:
//   late      the order the game must use: GuestAddressSpace::Initialize (what TitleRuntime::Load does), THEN the logger's
//             second half. Four threads write to watched guest pages of the 0xA0000000 (64 KiB), 0xC0000000 and
//             0xE0000000 heaps; every write is resumed and counted, nothing is reported. Exit 0.
//   early     the order OnInit had until 2026-11-21: the logger's second half, THEN the memory. The first write to a
//             watched page is reported as a crash and the process dies of SIGSEGV (exit 139 in a shell): what the Linux
//             boot check saw. This mode PASSES when the program dies with the report; surviving is the FAIL (3).
//   declined  as late, then a write the SDK does not handle (the guest's null page, protect_zero): one report, killed by
//             SIGSEGV (139).
//
// crash_handler.cpp's OnUnconsumedFault is repeated here (keep the two in step).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <rex/kernel/heap.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform/exception_handler.h>

#include "crash_report_linux.h"

namespace lx = larecomp::crash::lx;

namespace {

bool OnUnconsumedFault(rex::platform::Exception* ex, void*) {
  lx::Fault f;
  switch (ex->GetCode()) {
    case rex::platform::Exception::Code::kAccessViolation:
      f.signo = SIGSEGV;
      f.has_addr = true;
      f.addr = ex->GetFaultAddress();
      switch (ex->GetAccessViolationOperation()) {
        case rex::platform::Exception::AccessViolationOperation::kRead:
          f.op = 1;
          break;
        case rex::platform::Exception::AccessViolationOperation::kWrite:
          f.op = 2;
          break;
        default:
          break;
      }
      break;
    case rex::platform::Exception::Code::kIllegalInstruction:
      f.signo = SIGILL;
      break;
    default:
      return false;
  }
  if (const rex::platform::HostThreadContext* c = ex->GetThreadContext()) {
    f.has_pc = true;
    f.pc = c->rip;
  }
  lx::ReportUnconsumed(f);
  return false;
}

// What InstallCrashLoggerAfterRuntime() does.
void InstallLoggerSecondHalf() {
  rex::platform::ExceptionHandler::Install(&OnUnconsumedFault, nullptr);
  lx::InstallLate();
}

std::atomic<long> g_invalidations{0};

std::pair<rex::u32, rex::u32> CountInvalidation(void*, rex::u32 address, rex::u32 length, bool) {
  g_invalidations.fetch_add(1);
  return {address, length};
}

// Arms a range of one heap, writes to every page of it from `threads` threads, and says how many invalidation
// callbacks ran (one per page when the SDK resumed every write).
bool WatchedWrites(rex::kernel::GuestAddressSpace& memory, rex::u32 heap_page_size, int threads) {
  using namespace rex::kernel;
  BaseHeap* heap = memory.LookupHeapByType(true, heap_page_size);
  constexpr rex::u32 kBytes = 0x40000;  // 64 host pages
  rex::u32 address = 0;
  if (!heap->Alloc(kBytes, heap_page_size,
                   kMemoryAllocationReserve | kMemoryAllocationCommit | kMemoryAllocationTitle,
                   kMemoryProtectRead | kMemoryProtectWrite, false, &address)) {
    std::printf("FAIL: no memory in the heap with %X byte pages\n", heap_page_size);
    return false;
  }
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation, nullptr);
  const rex::u32 physical = memory.GetPhysicalAddress(address);
  auto* host = memory.TranslateVirtual<volatile rex::u32*>(address);
  const rex::u32 pages = kBytes / 4096;
  g_invalidations = 0;
  memory.EnablePhysicalMemoryAccessCallbacks(physical, kBytes, true, false);
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      for (rex::u32 page = static_cast<rex::u32>(t); page < pages; page += static_cast<rex::u32>(threads)) {
        host[page * 1024 + 7] = 0x1000 + page;
      }
    });
  }
  for (auto& w : workers) w.join();
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  heap->Release(address);
  std::printf("heap %08X (%X byte pages): %ld invalidations for %u watched pages\n", heap->GetHeapBase(),
              heap_page_size, g_invalidations.load(), pages);
  return g_invalidations.load() == static_cast<long>(pages);
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "";
  const char* log = argc > 2 ? argv[2] : "crash_report_sdk_check.log";
  const bool early = std::strcmp(mode, "early") == 0;
  if (!early && std::strcmp(mode, "late") != 0 && std::strcmp(mode, "declined") != 0) {
    std::fprintf(stderr, "usage: %s late|early|declined [log file]\n", argv[0]);
    return 2;
  }

  if (FILE* created = std::fopen(log, "a")) std::fclose(created);  // the reports append to it by name: it has to exist
  char log_abs[1024];
  if (!realpath(log, log_abs)) std::snprintf(log_abs, sizeof(log_abs), "%s", log);
  lx::InstallEarly(log_abs, 0, nullptr);  // the app's constructor (InstallCrashLogger)

  if (early) InstallLoggerSecondHalf();  // the old order: before the memory exists

  rex::kernel::GuestAddressSpace memory;  // TitleRuntime::Load: puts the SDK's own callback on the list
  if (!memory.Initialize()) {
    std::puts("FAIL: the guest memory did not initialize");
    return 2;
  }

  if (!early) InstallLoggerSecondHalf();  // OnTitleLaunch

  if (!WatchedWrites(memory, 0x10000, 4) || !WatchedWrites(memory, 0x1000000, 2) || !WatchedWrites(memory, 0x1000, 4)) {
    std::puts(early ? "FAIL: the watched writes ran without a crash in the old order" : "FAIL: a watched write was not resumed");
    return early ? 3 : 4;
  }
  if (early) {
    std::puts("FAIL: the old order should have ended the process at the first watched write");
    return 3;
  }
  std::puts("every watched write was resumed, nothing was reported");

  if (std::strcmp(mode, "declined") == 0) {
    std::puts("now a write the SDK does not handle: the guest's null page");
    *memory.TranslateVirtual<volatile rex::u32*>(0x10) = 1;
    std::puts("FAIL: the null page write did not crash");
    return 3;
  }
  return 0;
}
