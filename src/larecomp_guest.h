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
//                      guest thread, on a register file of its own: ZEROED, taking from the thread only r1, r13, the FP control
//                      state and the two 0.11 pointers (see ZeroedFrame). The 0.10 function did exactly that (a fresh
//                      PPCContext newCtx{}), and the callers depend on it: a function reads the registers its caller did not
//                      set (the pause menu's AllocState passes r3 only and the class factory behind it takes r4 as the class
//                      name, null meaning the default menu state). rex::ppc::HostToGuestCall runs the callee on the thread's
//                      OWN register file, so r4..r10 hold whatever the interrupted guest code left there. The 0.10 function
//                      also did nothing and returned R{} when the thread was not a guest thread; HostToGuestCall asserts in
//                      Debug and dereferences a null register file in Release, so that guard stays here. `fn` is a PPCFunc*
//                      or the name of a recompiled function (__imp__rex_sub_XXXXXXXX).
//
//   CallFrame          rex::CallFrame of the 0.10 fork SDK: a register file of its own for a side call that must not disturb the
//                      registers of the hook it runs in (see the struct below for what it copies from the parent).
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

namespace detail {

// The register file a call from host code runs on (what Call uses): all zero, like the `PPCContext newCtx{}` of the 0.10
// GuestToHostFunction, so a callee that reads a register the caller never set sees 0, not the thread's leftover. From the
// thread it takes the stack pointer r1 (the call opens its frame below it), the thread pointer r13, the FP control state
// (written back to the thread at the end, also when the callee throws ThreadStop: it may have changed the rounding mode) and
// the thread state and stop flag pointers of 0.11, so the callee stops with its thread. detour_frame stays null.
struct ZeroedFrame {
  ::PPCContext ctx{};
  ::PPCContext& thread_;

  explicit ZeroedFrame(::PPCContext& thread) : thread_(thread) {
    ctx.r1 = thread.r1;
    ctx.r13 = thread.r13;
    ctx.fpscr = thread.fpscr;
    ctx.thread_state = thread.thread_state;
    ctx.stop_requested = thread.stop_requested;
  }

  ~ZeroedFrame() { thread_.fpscr = ctx.fpscr; }

  ZeroedFrame(const ZeroedFrame&) = delete;
  ZeroedFrame& operator=(const ZeroedFrame&) = delete;
};

}  // namespace detail

// Runs a recompiled function with the given arguments on the calling guest thread, on a zeroed register file of its own (see
// the note on Call at the top). Nothing runs, and R{} comes back, for a null function or a thread that is not a guest thread.
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
  // address and puts the arguments in place; it saves the file it is given and restores it afterwards, which here is the
  // zeroed one.
  detail::ZeroedFrame frame(*bound.ctx);
  return rex::ppc::Call<R>(frame.ctx, bound.base, *target, std::forward<Args>(args)...);
}

// A register file of its own for a call into recompiled code made from inside a hook, so nothing of the hook's registers is
// touched (the call sets r3..r5 and clobbers the volatile registers). It is a copy of nothing: like the 0.10 rex::CallFrame it
// is NOT zero-initialised and takes only what a callee needs from the parent: the stack pointer r1, the thread pointer r13 and
// the FP control state (written back to the parent at the end, as the callee may have changed the rounding mode). The two
// pointers rexglue 0.11 added to the register file come along: the thread state (kernel calls the callee makes find their
// thread through it) and the stop flag; detour_frame stays null, the callee runs no detour of the parent's.
// CHECK: in a Debug build rex::ppc::detail::CheckHookContext compares a hook's register file with the thread's own, and this
// side file is not that one: a hook (REX_HOOK / a midasm hook) reached by the callee would assert there. Release builds only.
struct CallFrame {
  ::PPCContext ctx;
  ::PPCContext& parent_;

  explicit CallFrame(::PPCContext& parent) : parent_(parent) {
    ctx.r1 = parent.r1;
    ctx.r13 = parent.r13;
    ctx.fpscr = parent.fpscr;
    ctx.thread_state = parent.thread_state;
    ctx.stop_requested = parent.stop_requested;
  }

  ~CallFrame() { parent_.fpscr = ctx.fpscr; }

  operator ::PPCContext&() { return ctx; }

  CallFrame(const CallFrame&) = delete;
  CallFrame& operator=(const CallFrame&) = delete;
};

}  // namespace larecomp::guest
