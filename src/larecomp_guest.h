#pragma once
//
// How LARecomp's own code reaches the guest on rexglue 0.11. Three things changed shape and kernel_rename.py
// (merge-tools/wpl) points the old call sites here:
//
//   AddressSpace(rt)   what rex::Runtime::memory() returned in 0.10 (a rex::memory::Memory): translate guest addresses, look
//                      up heaps, change page protection, allocate from the system heap. It is rex::kernel::GuestAddressSpace
//                      now, which has the same methods. The SDK gives it out only to guest threads (ThreadState::GetAddressSpace);
//                      TitleRuntime::GetGuestHeap() is the same object seen as an ISystemHeap, which is how a host thread
//                      (the UI, the renderer, the shader build) gets at it. The downcast is exact: GetGuestHeap() is
//                      `*state_->GetAddressSpace()` in src/kernel/title_runtime.cpp. CHECK: ask the SDK for a public
//                      GuestMemory::GetAddressSpace() if the cast ever has to change.
//
//   Membase()          rex::Runtime::instance()->virtual_membase(): the host base of the guest memory, null when no title is
//                      loaded. REX_RUNTIME() is null before Load and from the first step of the shutdown, while guest threads
//                      may still be running hooks; the 0.10 runtime outlived them. Most call sites test the result.
//
//   Call<R>(fn, args)  rex::ppc::GuestToHostFunction<R>(fn, args): run a recompiled function from host code on the calling
//                      guest thread. The 0.10 function did nothing and returned R{} when the thread was not a guest thread;
//                      rex::ppc::HostToGuestCall asserts in Debug and dereferences a null register file in Release, so the
//                      guard stays here. `fn` is a PPCFunc* or the name of a recompiled function (__imp__rex_sub_XXXXXXXX).
//
#include <cstdint>
#include <type_traits>
#include <utility>

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

namespace larecomp::guest {

// The guest address space of a loaded title, null for no runtime or one that is not loaded.
inline rex::kernel::GuestAddressSpace* AddressSpace(rex::kernel::TitleRuntime* runtime) {
  if (!runtime || !runtime->IsLoaded()) {
    return nullptr;
  }
  return static_cast<rex::kernel::GuestAddressSpace*>(&runtime->GetGuestHeap());
}

// The same for the runtime of the process (null before the title is loaded and from the start of the shutdown).
inline rex::kernel::GuestAddressSpace* AddressSpace() {
  return AddressSpace(rex::kernel::GetProcessRuntime());
}

// The host base of the guest memory of the loaded title, null before Load and from the start of the shutdown.
inline uint8_t* Membase() {
  rex::kernel::TitleRuntime* runtime = rex::kernel::GetProcessRuntime();
  return runtime ? runtime->GetMembase().GetBase() : nullptr;
}

// Runs a recompiled function with the given arguments on the calling guest thread. Nothing runs, and R{} comes back, for
// a null function or a thread that is not a guest thread.
template <typename R, typename Function, typename... Args>
R Call(Function&& function, Args&&... args) {
  ::PPCFunc* target = function;
  if (!target || rex::ppc::GetBoundThread().ctx == nullptr) {
    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      return R{};
    }
  }
  return rex::ppc::HostToGuestCall<R>(target, std::forward<Args>(args)...);
}

}  // namespace larecomp::guest
