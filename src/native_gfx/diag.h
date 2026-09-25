#pragma once

#include <cstdarg>
#include <cstdio>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, mcla_native_gfx_diag);

namespace mcla::native_gfx {

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
