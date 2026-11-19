#pragma once
//
// Ending the process at once, for the paths that cannot wait for the app's own shutdown: the close watchdog (larecomp_app.cpp) and the
// crash paths (crash_handler.cpp: std::terminate, abort(), a CRT invalid parameter, a pure virtual call).
//
// std::_Exit is not "at once" on Windows. The UCRT's _Exit still ends in ExitProcess, which sends DLL_PROCESS_DETACH to every loaded DLL
// and so runs the static destructors of the SDK's DLLs. rexgpu.dll's graphics module is one of them (the `static ModuleClass instance`
// of REX_DECLARE_MODULE, graphics_module.cpp): when the exit comes before the app's own shutdown ended, it still owns the SDK's graphics
// driver, and its destructor runs the driver's teardown with the other threads gone and the GPU driver's own DLL already detached.
// 2026-11-18, XENIA on Vulkan: the 5 s close watchdog fired while the UI thread sat in the SDK's modal error box, and
// ~VulkanGraphicsDriver called a null function of nvoglv64.dll (ACCESS_VIOLATION executing address 0, from the watchdog thread). A crash
// path that ends in _Exit would do the same to a process that is already failing, and hide the first failure behind a second one.
// TerminateProcess runs none of it.
//
// Linux needs nothing of the kind: _exit / std::_Exit is the exit_group system call (no atexit handlers, no static destructors, no
// library finalizers), and abort() raises SIGABRT whose default action ends the process the same way. So there it is std::_Exit.
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace larecomp {

// Flush the log before calling it: nothing runs after it, not even the log's own destructor.
[[noreturn]] inline void EndProcessNow(int code) {
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
  std::_Exit(code);  // where TerminateProcess does not exist (it does not return on Windows)
}

}  // namespace larecomp
