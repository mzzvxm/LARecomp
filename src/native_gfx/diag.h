#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, mcla_native_gfx_diag);

namespace mcla::native_gfx {

// TEMP DIAG (inline_direct loading freeze, 28/09): the last place the render
// thread and the submission thread were seen, printed by the nocp freeze line.
// Remove with the fix.
inline std::atomic<const char*> g_hang_render{"-"};
inline std::atomic<const char*> g_hang_worker{"-"};
inline void HangMark(std::atomic<const char*>& slot, const char* where) {
  slot.store(where, std::memory_order_relaxed);
}

inline void DiagLog(const char* fmt, ...) {
  if (!REXCVAR_GET(mcla_native_gfx_diag)) {
    return;
  }
  FILE* f = std::fopen("native_gfx_diag.txt", "ab");
  if (!f) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  std::vfprintf(f, fmt, args);
  va_end(args);
  std::fclose(f);
}

}  // namespace mcla::native_gfx
