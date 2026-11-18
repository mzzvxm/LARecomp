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
//                      guest thread, on the thread's own register file with the registers CLEARED, taking from the thread only
//                      r1, r13 and the control state, and restoring the file afterwards (see ThreadFileScope). The 0.10
//                      function cleared them too (a fresh PPCContext newCtx{}), and the callers depend on it: a function reads the registers its caller did not
//                      set (the pause menu's AllocState passes r3 only and the class factory behind it takes r4 as the class
//                      name, null meaning the default menu state). rex::ppc::HostToGuestCall runs the callee on the thread's
//                      OWN register file, so r4..r10 hold whatever the interrupted guest code left there. The 0.10 function
//                      also did nothing and returned R{} when the thread was not a guest thread; HostToGuestCall asserts in
//                      Debug and dereferences a null register file in Release, so that guard stays here. `fn` is a PPCFunc*
//                      or the name of a recompiled function (__imp__rex_sub_XXXXXXXX).
//
//   ThreadFile         for a caller that sets the registers by hand (a vector register, its own frame): the same thread file as
//                      Call, cleared, handed out to fill and call, restored afterwards. It replaces rex::CallFrame of the 0.10
//                      fork SDK, a register file of its own, which cannot work on 0.11 (see ThreadFileScope).
//
#include <cstdint>
#include <optional>
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

namespace detail {

// What Call does to the thread's own register file around a call from host code. The callee must run on THAT file, not on a
// copy: rexglue 0.11 finds a guest thread's registers through it (ThreadState::GetContext(), GetBoundThread()), and the kernel
// runs guest code on it from inside the callee's own kernel calls (APC delivery in alertable waits, traps through the function
// dispatcher), opening their frames below ITS r1. On a copy, the thread's r1 stays where the host call started, so such nested
// guest code writes its frames over the callee's live stack. (2026-11-18: a zeroed copy fixed the pause menu's first AllocState
// and the second one then came back null.)
// The registers are cleared like the `PPCContext newCtx{}` of the 0.10 GuestToHostFunction, so a callee that reads a register
// its caller never set sees 0, not the interrupted guest code's leftover (the pause menu's AllocState passes r3 only; the class
// factory behind it takes r4 as the class name, null meaning the default state). Kept from the thread: r1 (the call opens its
// frame below it), r13, the MSR, the FP and vector control state and the four 0.11 fields (thread state, detour frame, stop
// flag, last indirect target). The whole file comes back at the end, also when the callee throws ThreadStop, with the
// callee's FP control state (it may have changed the rounding mode, as on 0.10).
struct ThreadFileScope {
  ::PPCContext& thread_;
  const ::PPCContext saved_;

  explicit ThreadFileScope(::PPCContext& thread) : thread_(thread), saved_(thread) {
    ::PPCContext fresh{};
    fresh.r1 = thread.r1;
    fresh.r13 = thread.r13;
    fresh.msr = thread.msr;
    fresh.fpscr = thread.fpscr;
    fresh.vscr = thread.vscr;
    fresh.last_indirect_target = thread.last_indirect_target;
    fresh.thread_state = thread.thread_state;
    fresh.detour_frame = thread.detour_frame;
    fresh.stop_requested = thread.stop_requested;
    thread = fresh;
  }

  ~ThreadFileScope() {
    const auto fpscr = thread_.fpscr;
    thread_ = saved_;
    thread_.fpscr = fpscr;
  }

  ThreadFileScope(const ThreadFileScope&) = delete;
  ThreadFileScope& operator=(const ThreadFileScope&) = delete;
};

}  // namespace detail

// Runs a recompiled function with the given arguments on the calling guest thread, on the thread's own register file with the
// registers cleared (see ThreadFileScope). Nothing runs, and R{} comes back, for a null function or a non-guest thread.
template <typename R, typename Function, typename... Args>
R Call(Function&& function, Args&&... args) {
  ::PPCFunc* target = function;
  const rex::ppc::BoundThread bound = rex::ppc::GetBoundThread();
  if (!target || bound.ctx == nullptr) {
    if constexpr (std::is_void_v<R>) {
      return;
    } else {
      return R{};
    }
  }
  // rex::ppc::Call opens the call frame under r1 (back chain, linkage area, stack arguments), sets lr to the host return
  // address and puts the arguments in place; the scope restores the thread's registers afterwards.
  detail::ThreadFileScope scope(*bound.ctx);
  return rex::ppc::Call<R>(*bound.ctx, bound.base, *target, std::forward<Args>(args)...);
}

// The thread's own register file for a call into recompiled code made by a caller that sets the registers by hand (what
// Call cannot place: a vector register, a frame the caller lays out itself). Same rules as Call (see ThreadFileScope): the
// file is cleared except r1, r13, the control state and the 0.11 fields, the callee runs ON it, and the whole file comes
// back at the end with the callee's fpscr, so a hook that makes the call keeps its own registers (r3.. included) as they
// were. It replaces the CallFrame of the 0.10 fork SDK, a file of its own that rexglue 0.11 does not find.
//
//   larecomp::guest::ThreadFile file;   // a hook's own ctx IS this file: read what is needed from it BEFORE this line
//   if (!file) return 0;                // not a guest thread: nothing to call on
//   PPCContext& c = file.ctx();
//   c.r1.u32 -= 0x70;                   // the callee's frames go below r1 (or set r1 to a frame the caller laid out itself)
//   c.r3.u64 = a; c.r4.u64 = b;
//   fn(c, base);
//   return c.r3.u32;                    // read the result before `file` goes out of scope
class ThreadFile {
 public:
  ThreadFile() : bound_(rex::ppc::GetBoundThread()) {
    if (bound_.ctx != nullptr) {
      scope_.emplace(*bound_.ctx);
    }
  }

  // False on a thread that is not a guest thread: ctx() must not be used then.
  explicit operator bool() const { return bound_.ctx != nullptr; }

  ::PPCContext& ctx() { return *bound_.ctx; }

  ThreadFile(const ThreadFile&) = delete;
  ThreadFile& operator=(const ThreadFile&) = delete;

 private:
  rex::ppc::BoundThread bound_;
  std::optional<detail::ThreadFileScope> scope_;
};

}  // namespace larecomp::guest
